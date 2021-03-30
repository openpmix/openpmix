/*
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2016      IBM Corporation.  All rights reserved.
 *
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "src/include/pmix_config.h"

#include <string.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif
#ifdef HAVE_SYS_TYPES_H
#    include <sys/types.h>
#endif
#ifdef HAVE_SYS_STAT_H
#    include <sys/stat.h>
#endif
#ifdef HAVE_FCNTL_H
#    include <fcntl.h>
#endif
#include <jansson.h>
#include <time.h>

#include "include/pmix_common.h"

#include "src/class/pmix_bitmap.h"
#include "src/class/pmix_list.h"
#include "src/class/pmix_pointer_array.h"
#include "src/include/pmix_globals.h"
#include "src/mca/common/sse/sse.h"
#include "src/mca/preg/preg.h"
#include "src/threads/threads.h"
#include "src/util/argv.h"
#include "src/util/error.h"
#include "src/util/name_fns.h"
#include "src/util/output.h"
#include "src/util/pmix_environ.h"
#include "src/util/printf.h"
#include "src/util/show_help.h"

#include "pnet_sshot.h"
#include "src/mca/pnet/base/base.h"
#include "src/mca/pnet/pnet.h"

/* internal variables */
static pmix_pointer_array_t mygroups, mynodes, myvertices;
static int mynswitches = 100, mynnics = 80000;

/* internal tracking structures */
typedef struct {
    pmix_object_t super;
    int grpID;
    pmix_bitmap_t switches;
    pmix_bitmap_t nics;
    char **members;
} pnet_fabricgroup_t;
static void ncon(pnet_fabricgroup_t *p)
{
    /* initialize the switch bitmap */
    PMIX_CONSTRUCT(&p->switches, pmix_bitmap_t);
    pmix_bitmap_set_max_size(&p->switches, mynswitches);
    pmix_bitmap_init(&p->switches, mynswitches);
    /* initialize the nic bitmap */
    PMIX_CONSTRUCT(&p->nics, pmix_bitmap_t);
    pmix_bitmap_set_max_size(&p->nics, mynnics);
    pmix_bitmap_init(&p->nics, mynnics);
    /* initialize the argv array of members */
    p->members = NULL;
}
static void ndes(pnet_fabricgroup_t *p)
{
    PMIX_DESTRUCT(&p->switches);
    PMIX_DESTRUCT(&p->nics);
    if (NULL != p->members) {
        pmix_argv_free(p->members);
    }
}
static PMIX_CLASS_INSTANCE(pnet_fabricgroup_t, pmix_object_t, ncon, ndes);

typedef struct {
    pmix_object_t super;
    char *name;
    pnet_fabricgroup_t *grp;
    pmix_pointer_array_t nics;
} pnet_node_t;
static void ndcon(pnet_node_t *p)
{
    p->name = NULL;
    p->grp = NULL;
    PMIX_CONSTRUCT(&p->nics, pmix_pointer_array_t);
    pmix_pointer_array_init(&p->nics, 8, INT_MAX, 8);
}
static void nddes(pnet_node_t *p)
{
    pmix_object_t *item;
    int n;

    if (NULL != p->name) {
        free(p->name);
    }
    if (NULL != p->grp) {
        PMIX_RELEASE(p->grp);
    }
    for (n = 0; n < p->nics.size; n++) {
        if (NULL != (item = pmix_pointer_array_get_item(&p->nics, n))) {
            PMIX_RELEASE(item);
        }
    }
    PMIX_DESTRUCT(&p->nics);
}
static PMIX_CLASS_INSTANCE(pnet_node_t, pmix_object_t, ndcon, nddes);

typedef struct {
    pmix_object_t super;
    uint32_t index;
    pnet_node_t *host;
    char *addr;
    pmix_coord_t coord;
} pnet_vertex_t;
static void vtcon(pnet_vertex_t *p)
{
    p->host = NULL;
    p->addr = NULL;
    PMIX_COORD_CONSTRUCT(&p->coord);
    p->coord.view = PMIX_COORD_LOGICAL_VIEW;
}
static void vtdes(pnet_vertex_t *p)
{
    if (NULL != p->host) {
        PMIX_RELEASE(p->host);
    }
    if (NULL != p->addr) {
        free(p->addr);
    }
    PMIX_COORD_DESTRUCT(&p->coord);
}
static PMIX_CLASS_INSTANCE(pnet_vertex_t, pmix_object_t, vtcon, vtdes);

/* internal functions */
static pmix_status_t ask_fabric_controller(pmix_fabric_t *fabric, const pmix_info_t directives[],
                                           size_t ndirs, pmix_op_cbfunc_t cbfunc, void *cbdata);

pmix_status_t pmix_pnet_sshot_register_fabric(pmix_fabric_t *fabric, const pmix_info_t directives[],
                                              size_t ndirs, pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    json_t *root, *switches, *ndarray, *sw, *nd, *nic, *nics;
    json_error_t error;
    int nswitches, nnodes, m, n, p, grpID, swcNum, nnics;
    const char *str;
    pnet_fabricgroup_t *grp;
    pnet_node_t *node;
    pnet_vertex_t *vtx;
    char **grps = NULL, *grpmems, *gmem;

    pmix_output_verbose(2, pmix_pnet_base_framework.framework_output, "pnet: sshot init");

    /* if we were given a fabric topology configuration file, then parse
     * it to get the fabric groups and switches */
    if (NULL == mca_pnet_sshot_component.configfile) {
        /* we were not given a file - see if we can get it
         * from the fabric controller via REST interface */
        return ask_fabric_controller(fabric, directives, ndirs, cbfunc, cbdata);
    }

    /* setup to parse the configuration file */
    PMIX_CONSTRUCT(&mygroups, pmix_pointer_array_t);
    pmix_pointer_array_init(&mygroups, 3, INT_MAX, 2);
    PMIX_CONSTRUCT(&mynodes, pmix_pointer_array_t);
    pmix_pointer_array_init(&mynodes, 128, INT_MAX, 64);
    PMIX_CONSTRUCT(&myvertices, pmix_pointer_array_t);
    pmix_pointer_array_init(&myvertices, 128, INT_MAX, 64);

    /* load the file */
    root = json_load_file(mca_pnet_sshot_component.configfile, 0, &error);
    if (NULL == root) {
        /* the error object contains info on the error */
        pmix_show_help("help-pnet-sshot.txt", "json-load", true, error.text, error.source,
                       error.line, error.column, error.position, json_error_code(&error));
        return PMIX_ERROR;
    }

    /* unpack the switch information and assemble them
     * into Dragonfly groups */
    switches = json_object_get(root, "switches");
    if (!json_is_array(switches)) {
        PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
        json_decref(root);
        return PMIX_ERR_BAD_PARAM;
    }
    nswitches = json_array_size(switches);

    for (n = 0; n < nswitches; n++) {
        sw = json_array_get(switches, n);
        json_unpack(sw, "{s:i, s:i}", "grpID", &grpID, "swcNum", &swcNum);
        /* see if we already have this group */
        if (NULL == (grp = (pnet_fabricgroup_t *) pmix_pointer_array_get_item(&mygroups, grpID))) {
            /* nope - better add it */
            grp = PMIX_NEW(pnet_fabricgroup_t);
            grp->grpID = grpID;
            pmix_pointer_array_set_item(&mygroups, grpID, grp);
            /* adjust the bitmap, if necessary */
            if (pmix_bitmap_size(&grp->switches) < nswitches) {
                pmix_bitmap_set_max_size(&grp->switches, nswitches);
            }
            /* indicate that this switch belongs to this group */
            pmix_bitmap_set_bit(&grp->switches, swcNum);
        } else {
            /* indicate that this switch belongs to this group */
            pmix_bitmap_set_bit(&grp->switches, swcNum);
        }
    }

    /* get the total number of NICs in each group */
    ndarray = json_object_get(root, "nodes");
    if (!json_is_array(ndarray)) {
        PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
        json_decref(root);
        return PMIX_ERR_BAD_PARAM;
    }
    nnodes = json_array_size(ndarray);

    for (n = 0; n < nnodes; n++) {
        nd = json_array_get(ndarray, n);
        node = PMIX_NEW(pnet_node_t);
        json_unpack(nd, "{s:s}", "name", &str);
        node->name = strdup(str);
        pmix_pointer_array_add(&mynodes, node);
        nics = json_object_get(nd, "nics");
        nnics = json_array_size(nics);
        /* adjust the bitmaps, if necessary */
        for (p = 0; p < mygroups.size; p++) {
            if (NULL != (grp = (pnet_fabricgroup_t *) pmix_pointer_array_get_item(&mygroups, p))) {
                if (pmix_bitmap_size(&grp->nics) < nnics) {
                    pmix_bitmap_set_max_size(&grp->nics, nnics);
                }
            }
        }
        /* cycle thru the NICs to get the switch
         * to which they are connected */
        for (m = 0; m < nnics; m++) {
            nic = json_array_get(nics, m);
            json_unpack(nic, "{s:s, s:i}", "id", &str, "switch", &swcNum);
            /* add the vertex to our array */
            vtx = PMIX_NEW(pnet_vertex_t);
            PMIX_RETAIN(node);
            vtx->host = node;
            vtx->addr = strdup(str);
            vtx->index = pmix_pointer_array_add(&myvertices, vtx);
            /* add the NIC to the node */
            PMIX_RETAIN(vtx);
            pmix_pointer_array_add(&node->nics, vtx);
            /* look for this switch in our groups */
            for (p = 0; p < mygroups.size; p++) {
                if (NULL
                    != (grp = (pnet_fabricgroup_t *) pmix_pointer_array_get_item(&mygroups, p))) {
                    if (pmix_bitmap_is_set_bit(&grp->switches, swcNum)) {
                        /* mark that this NIC is part of this group */
                        pmix_bitmap_set_bit(&grp->nics, vtx->index);
                        /* record that this node is in this group */
                        if (NULL == node->grp) {
                            PMIX_RETAIN(grp);
                            node->grp = grp;
                            pmix_argv_append_nosize(&grp->members, node->name);
                        }
                        /* record a coordinate for this NIC */
                        /* we are done */
                        break;
                    }
                }
            }
        }
    }

    /* assemble the groups and add them to the fabric object */
    for (p = 0; p < mygroups.size; p++) {
        if (NULL != (grp = (pnet_fabricgroup_t *) pmix_pointer_array_get_item(&mygroups, p))) {
            if (NULL == grp->members) {
                continue;
            }
            grpmems = pmix_argv_join(grp->members, ',');
            pmix_asprintf(&gmem, "%d:%s", grp->grpID, grpmems);
            pmix_argv_append_nosize(&grps, gmem);
            free(gmem);
            free(grpmems);
        }
    }
    if (NULL != grps) {
        gmem = pmix_argv_join(grps, ';');
        PMIX_INFO_CREATE(fabric->info, 1);
        fabric->ninfo = 1;
        PMIX_INFO_LOAD(&fabric->info[0], PMIX_FABRIC_GROUPS, gmem, PMIX_STRING);
        free(gmem);
    }

    return PMIX_OPERATION_SUCCEEDED;
}

static void regcbfunc(long response_code, const char *effective_url, void *cbdata)
{
    pmix_sse_request_t *req = (pmix_sse_request_t *) cbdata;

    PMIX_ACQUIRE_OBJECT(req); // ensure the object has been updated

    req->status = response_code;
    PMIX_POST_OBJECT(req);          // ensure changes to the object are pushed into memory
    PMIX_WAKEUP_THREAD(&req->lock); // wakeup the waiting thread
}

/* the following function will be called whenever a data stream
 * is received. It will receive a NULL stream when the channel
 * has been closed
 */
static void ondata(const char *stream, char **result, void *userdata)
{
    pmix_cb_t *cb = (pmix_cb_t *) userdata;

    PMIX_ACQUIRE_OBJECT(cb);

    /* if the stream is NULL, then the REST server terminated the
     * connection and there is nothing to return
     */
    if (NULL == stream) {
        cb->status = PMIX_ERR_NOT_AVAILABLE;
    } else {
        /* parse the stream to populate the fabric object - see above
         * parsing of the config file for an example */
        PMIX_INFO_CREATE(cb->fabric->info, 1);
        cb->fabric->ninfo = 1;
        PMIX_INFO_LOAD(&cb->fabric->info[0], PMIX_FABRIC_GROUPS, "0:nodeA,nodeB;1:nodeC,nodeD",
                       PMIX_STRING);
    }

    /* pass back the status - the results are in the fabric
     * object whose pointer we were given */
    cb->cbfunc.opfn(cb->status, cb->cbdata);
    PMIX_RELEASE(cb);
    return;
}

static pmix_status_t ask_fabric_controller(pmix_fabric_t *fabric, const pmix_info_t directives[],
                                           size_t ndirs, pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    pmix_sse_request_t *req;
    pmix_cb_t *cb;
    pmix_status_t rc;

    /* initialize the sse support - it is protected,
     * so it doesn't matter if it was already
     * initialized */
    pmix_sse_common_init();

    /* setup the request */
    req = PMIX_NEW(pmix_sse_request_t);
    req->verb = PMIX_HTTP_GET;
    /* not sure where one gets the actual URL - this is obviously a fake one */
    req->url = "https://10.25.24.156:8080/v1/stream/"
               "cray-logs-containers?batchsize=4&count=2&streamID=stream1";
    req->max_retries = 10;
    req->debug = true;
    req->allow_insecure = true;
    /* not sure we really want a stream - consider this a placeholder */
    req->expected_content_type = "text/event-stream";
    req->ssl_cert = NULL; // replace with appropriate access certificate
    req->ca_info = NULL;  // replace with appropriate CA certificate file

    /* setup the object to return the info */
    cb = PMIX_NEW(pmix_cb_t);
    cb->fabric = fabric;
    cb->cbfunc.opfn = cbfunc;
    cb->cbdata = cbdata;

    /* perform the request */
    rc = pmix_sse_register_request(req, regcbfunc, req, ondata, cb);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(req);
        return rc;
    }
    /* wait for registration to complete */
    PMIX_WAIT_THREAD(&req->lock);
    if (PMIX_SUCCESS != req->status) {
        PMIX_RELEASE(cb);
        return PMIX_ERROR;
    }

    return rc;
}
