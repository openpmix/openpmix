/*
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2016      IBM Corporation.  All rights reserved.
 *
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "src/include/pmix_config.h"

#include <string.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif
#include <time.h>
#include <jansson.h>

#include "include/pmix_common.h"

#include "src/mca/base/pmix_mca_base_var.h"
#include "src/include/pmix_socket_errno.h"
#include "src/include/pmix_globals.h"
#include "src/class/pmix_bitmap.h"
#include "src/class/pmix_list.h"
#include "src/class/pmix_pointer_array.h"
#include "src/util/alfg.h"
#include "src/util/argv.h"
#include "src/util/error.h"
#include "src/util/name_fns.h"
#include "src/util/output.h"
#include "src/util/pmix_environ.h"
#include "src/util/printf.h"
#include "src/util/show_help.h"
#include "src/mca/preg/preg.h"

#include "src/mca/pnet/pnet.h"
#include "src/mca/pnet/base/base.h"
#include "pnet_sshot.h"

static pmix_status_t sshot_init(void);
static void sshot_finalize(void);
static pmix_status_t allocate(pmix_namespace_t *nptr,
                              pmix_info_t info[], size_t ninfo,
                              pmix_list_t *ilist);
static pmix_status_t setup_local_network(pmix_namespace_t *nptr,
                                         pmix_info_t info[],
                                         size_t ninfo);
static pmix_status_t setup_fork(pmix_namespace_t *nptr,
                                const pmix_proc_t *proc,
                                char ***env);
pmix_pnet_module_t pmix_sshot_module = {
    .name = "sshot",
    .init = sshot_init,
    .finalize = sshot_finalize,
    .allocate = allocate,
    .setup_local_network = setup_local_network,
    .setup_fork = setup_fork
};

/* internal variables */
static pmix_pointer_array_t mygroups, mynodes, myvertices;
static int mynswitches = 100, mynnics = 80000;
static char **myenvlist = NULL;
static char **myvalues = NULL;
static int schedpipe = -1;

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
static PMIX_CLASS_INSTANCE(pnet_fabricgroup_t,
                           pmix_object_t,
                           ncon, ndes);

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
    for (n=0; n < p->nics.size; n++) {
        if (NULL != (item = pmix_pointer_array_get_item(&p->nics, n))) {
            PMIX_RELEASE(item);
        }
    }
    PMIX_DESTRUCT(&p->nics);
}
static PMIX_CLASS_INSTANCE(pnet_node_t,
                           pmix_object_t,
                           ndcon, nddes);

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
    memset(&p->coord, 0, sizeof(pmix_coord_t));
    p->coord.fabric = strdup("test");
    p->coord.plane = strdup("sshot");
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
    free(p->coord.fabric);
    free(p->coord.plane);
    if (NULL != p->coord.coord) {
        free(p->coord.coord);
    }
}
static PMIX_CLASS_INSTANCE(pnet_vertex_t,
                           pmix_object_t,
                           vtcon, vtdes);


static pmix_status_t sshot_init(void)
{
    json_t *root, *switches, *ndarray, *sw, *nd, *nic, *nics;
    json_error_t error;
    int nswitches, nnodes, m, n, p, grpID, swcNum, nnics;
    const char *str;
    pnet_fabricgroup_t *grp;
    pnet_node_t *node;
    pnet_vertex_t *vtx;
    char **grps = NULL, *grpmems, *gmem;

    pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                        "pnet: sshot init");

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
        pmix_show_help("help-pnet-sshot.txt", "json-load", true,
                       error.text, error.source, error.line,
                       error.column, error.position,
                       json_error_code(&error));
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

    for (n=0; n < nswitches; n++) {
        sw = json_array_get(switches, n);
        json_unpack(sw, "{s:i, s:i}", "grpID", &grpID, "swcNum", &swcNum);
        /* see if we already have this group */
        if (NULL == (grp = (pnet_fabricgroup_t*)pmix_pointer_array_get_item(&mygroups, grpID))) {
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

    for (n=0; n < nnodes; n++) {
        nd = json_array_get(ndarray, n);
        node = PMIX_NEW(pnet_node_t);
        json_unpack(nd, "{s:s}", "name", &str);
        node->name = strdup(str);
        pmix_pointer_array_add(&mynodes, node);
        nics = json_object_get(nd, "nics");
        nnics = json_array_size(nics);
        /* adjust the bitmaps, if necessary */
        for (p=0; p < mygroups.size; p++) {
            if (NULL != (grp = (pnet_fabricgroup_t*)pmix_pointer_array_get_item(&mygroups, p))) {
                if (pmix_bitmap_size(&grp->nics) < nnics) {
                    pmix_bitmap_set_max_size(&grp->nics, nnics);
                }
            }
        }
        /* cycle thru the NICs to get the switch
         * to which they are connected */
        for (m=0; m < nnics; m++) {
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
            for (p=0; p < mygroups.size; p++) {
                if (NULL != (grp = (pnet_fabricgroup_t*)pmix_pointer_array_get_item(&mygroups, p))) {
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

    /* see if we have a FIFO to the scheduler */
    if (NULL != mca_pnet_sshot_component.pipe) {
        schedpipe = open(mca_pnet_sshot_component.pipe, O_WRONLY | O_CLOEXEC);
        if (0 > schedpipe) {
            /* can't open it - scheduler may not have created it */
            return PMIX_SUCCESS;
        }
        for (p=0; p < mygroups.size; p++) {
            if (NULL != (grp = (pnet_fabricgroup_t*)pmix_pointer_array_get_item(&mygroups, p))) {
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
            n = strlen(gmem) + 1;  // include the NULL terminator
            write(schedpipe, &n, sizeof(int));
            write(schedpipe, gmem, n * sizeof(char));
            free(gmem);
            pmix_argv_free(grps);
        }
        /* for test purposes, we are done */
        n = -1;
        write(schedpipe, &n, sizeof(int));
        close(schedpipe);
    }
    return PMIX_SUCCESS;
}

static void sshot_finalize(void)
{
    pnet_fabricgroup_t *ft;
    pnet_node_t *nd;
    pnet_vertex_t *vt;
    int n;

    pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                        "pnet: sshot finalize");

    for (n=0; n < mygroups.size; n++) {
        if (NULL != (ft = (pnet_fabricgroup_t*)pmix_pointer_array_get_item(&mygroups, n))) {
            PMIX_RELEASE(ft);
        }
    }
    PMIX_DESTRUCT(&mygroups);

    for (n=0; n < mynodes.size; n++) {
        if (NULL != (nd = (pnet_node_t*)pmix_pointer_array_get_item(&mynodes, n))) {
            PMIX_RELEASE(nd);
        }
    }
    PMIX_DESTRUCT(&mynodes);

    for (n=0; n < myvertices.size; n++) {
        if (NULL != (vt = (pnet_vertex_t*)pmix_pointer_array_get_item(&myvertices, n))) {
            PMIX_RELEASE(vt);
        }
    }
    PMIX_DESTRUCT(&myvertices);
}


/* NOTE: if there is any binary data to be transferred, then
 * this function MUST pack it for transport as the host will
 * not know how to do so */
static pmix_status_t allocate(pmix_namespace_t *nptr,
                              pmix_info_t info[], size_t ninfo,
                              pmix_list_t *ilist)
{
    pmix_kval_t *kv;
    bool seckey = false, envars = false;
    pmix_list_t mylist;
    size_t n, p, q, nreqs=0;
    int m;
    pmix_info_t *requests = NULL, *iptr, *ip2;
    char *idkey = NULL, **locals = NULL;
    uint64_t unique_key = 12345;
    pmix_buffer_t buf;
    pmix_status_t rc;
    char **nodes = NULL, **procs = NULL;
    pmix_data_array_t *darray, *d2, *d3;
    pmix_rank_t rank;
    pnet_node_t *nd, *nd2;
    uint32_t *u32;
    pmix_coord_t *coords;

    pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                        "pnet:test:allocate for nspace %s",
                        nptr->nspace);

    /* if I am not the scheduler, then ignore this call - should never
     * happen, but check to be safe */
    if (!PMIX_PEER_IS_SCHEDULER(pmix_globals.mypeer)) {
        return PMIX_SUCCESS;
    }

    if (NULL == info) {
        return PMIX_ERR_TAKE_NEXT_OPTION;
    }
    /* check directives to see if a crypto key and/or
     * fabric resource allocations requested */
    for (n=0; n < ninfo; n++) {
        pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                            "pnet:sshot:allocate processing key %s",
                            info[n].key);
        if (PMIX_CHECK_KEY(&info[n], PMIX_ALLOC_FABRIC)) {
            /* this info key includes an array of pmix_info_t, each providing
             * a key (that is to be used as the key for the allocated ports) and
             * a number of ports to allocate for that key */
            if (PMIX_DATA_ARRAY != info[n].value.type ||
                NULL == info[n].value.data.darray ||
                PMIX_INFO != info[n].value.data.darray->type ||
                NULL == info[n].value.data.darray->array) {
                requests = NULL;
                nreqs = 0;
            } else {
                requests = (pmix_info_t*)info[n].value.data.darray->array;
                nreqs = info[n].value.data.darray->size;
            }
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_PROC_MAP)) {
            rc = pmix_preg.parse_procs(info[n].value.data.string, &procs);
            if (PMIX_SUCCESS != rc) {
                return PMIX_ERR_BAD_PARAM;
            }
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_NODE_MAP)) {
            rc = pmix_preg.parse_nodes(info[n].value.data.string, &nodes);
            if (PMIX_SUCCESS != rc) {
                return PMIX_ERR_BAD_PARAM;
            }
        }
    }

    PMIX_CONSTRUCT(&mylist, pmix_list_t);

    if (envars) {
        pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                            "pnet:sshot:allocate adding envars for nspace %s",
                            nptr->nspace);
    }

    if (NULL == requests) {
        pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                            "pnet:sshot:allocate no requests for nspace %s",
                            nptr->nspace);

        rc = PMIX_ERR_TAKE_NEXT_OPTION;
        goto complete;
    }

    pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                        "pnet:sshot:allocate alloc_network for nspace %s",
                        nptr->nspace);

    /* cycle thru the provided array and get the ID key */
    for (n=0; n < nreqs; n++) {
        if (PMIX_CHECK_KEY(&requests[n], PMIX_ALLOC_FABRIC_ID)) {
            /* check for bozo error */
            if (PMIX_STRING != requests[n].value.type ||
                NULL == requests[n].value.data.string) {
                PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
                rc = PMIX_ERR_BAD_PARAM;
                goto cleanup;
            }
            idkey = requests[n].value.data.string;
        } else if (PMIX_CHECK_KEY(&requests[n], PMIX_ALLOC_FABRIC_SEC_KEY)) {
               seckey = PMIX_INFO_TRUE(&requests[n]);
        }
    }

    /* if they didn't give us a key, just create one */
    if (NULL == idkey) {
        idkey = "SSHOTKEY";
    }

    /* must include the idkey */
    kv = PMIX_NEW(pmix_kval_t);
    if (NULL == kv) {
        rc = PMIX_ERR_NOMEM;
        goto cleanup;
    }
    kv->key = strdup(PMIX_ALLOC_FABRIC_ID);
    kv->value = (pmix_value_t*)malloc(sizeof(pmix_value_t));
    if (NULL == kv->value) {
        PMIX_RELEASE(kv);
        rc = PMIX_ERR_NOMEM;
        goto cleanup;
    }
    kv->value->type = PMIX_STRING;
    kv->value->data.string = strdup(idkey);
    pmix_list_append(&mylist, &kv->super);

    if (seckey) {
        pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                            "pnet:test:allocate assigning network security key for nspace %s",
                            nptr->nspace);

        kv = PMIX_NEW(pmix_kval_t);
        if (NULL == kv) {
            rc = PMIX_ERR_NOMEM;
            goto cleanup;
        }
        kv->key = strdup(PMIX_ALLOC_FABRIC_SEC_KEY);
        kv->value = (pmix_value_t*)malloc(sizeof(pmix_value_t));
        if (NULL == kv->value) {
            PMIX_RELEASE(kv);
            rc = PMIX_ERR_NOMEM;
            goto cleanup;
        }
        kv->value->type = PMIX_BYTE_OBJECT;
        kv->value->data.bo.bytes = (char*)malloc(sizeof(uint64_t));
        if (NULL == kv->value->data.bo.bytes) {
            PMIX_RELEASE(kv);
            rc = PMIX_ERR_NOMEM;
            goto cleanup;
        }
        memcpy(kv->value->data.bo.bytes, &unique_key, sizeof(uint64_t));
        kv->value->data.bo.size = sizeof(uint64_t);
        pmix_list_append(&mylist, &kv->super);
    }

    if (NULL == procs || NULL == nodes) {
        pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                            "pnet:test:allocate missing proc/node map for nspace %s",
                            nptr->nspace);
        /* not an error - continue to next active component */
        rc = PMIX_ERR_TAKE_NEXT_OPTION;
        goto complete;
    }

    pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                        "pnet:test:allocate assigning endpoints for nspace %s",
                        nptr->nspace);

    /* cycle across the nodes and add the endpoints
     * for each proc on the node - we assume the same
     * list of static endpoints on each node */
    for (n=0; NULL != nodes[n]; n++) {
        /* split the procs for this node */
        locals = pmix_argv_split(procs[n], ',');
        if (NULL == locals) {
            /* aren't any on this node */
            continue;
        }
        /* find this node in our array */
        nd = NULL;
        for (m=0; m < mynodes.size; m++) {
            if (NULL != (nd2 = (pnet_node_t*)pmix_pointer_array_get_item(&mynodes, m))) {
                if (0 == strcmp(nodes[n], nd2->name)) {
                    nd = nd2;
                    break;
                }
            }
        }
        if (NULL == nd) {
            if (mca_pnet_sshot_component.simulate) {
                /* just take the nth entry */
                nd = (pnet_node_t*)pmix_pointer_array_get_item(&mynodes, n);
                if (NULL == nd) {
                    rc = PMIX_ERR_NOT_FOUND;
                    PMIX_ERROR_LOG(rc);
                    goto cleanup;
                }
            } else {
                /* should be impossible */
                rc = PMIX_ERR_NOT_FOUND;
                PMIX_ERROR_LOG(rc);
                goto cleanup;
            }
        }
        kv = PMIX_NEW(pmix_kval_t);
        if (NULL == kv) {
            rc = PMIX_ERR_NOMEM;
            goto cleanup;
        }
        kv->key = strdup(PMIX_ALLOC_FABRIC_ENDPTS);
        kv->value = (pmix_value_t*)malloc(sizeof(pmix_value_t));
        if (NULL == kv->value) {
            PMIX_RELEASE(kv);
            rc = PMIX_ERR_NOMEM;
            goto cleanup;
        }
        kv->value->type = PMIX_DATA_ARRAY;
        /* for each proc, we will assign an endpt
         * for each NIC on the node */
        q = pmix_argv_count(locals);
        PMIX_DATA_ARRAY_CREATE(darray, q, PMIX_INFO);
        kv->value->data.darray = darray;
        iptr = (pmix_info_t*)darray->array;
        for (m=0; NULL != locals[m]; m++) {
            /* each proc can have multiple endpoints depending
             * on the number of NICs available on the node. So
             * we package the endpoints for each proc as a data
             * array with the first element being the proc ID
             * and the remaining elements being the assigned
             * endpoints for that proc in priority order */
            PMIX_LOAD_KEY(iptr[m].key, PMIX_PROC_DATA);
            PMIX_DATA_ARRAY_CREATE(d2, 3, PMIX_INFO);
            iptr[m].value.type = PMIX_DATA_ARRAY;
            iptr[m].value.data.darray = d2;
            ip2 = (pmix_info_t*)d2->array;
            /* start with the rank */
            rank = strtoul(locals[m], NULL, 10);
            pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                                "pnet:test:allocate assigning %d endpoints for rank %u",
                                (int)q, rank);
            PMIX_INFO_LOAD(&ip2[0], PMIX_RANK, &rank, PMIX_PROC_RANK);
            /* the second element in this array will itself
             * be a data array of endpts */
            PMIX_DATA_ARRAY_CREATE(d3, q, PMIX_UINT32);
            PMIX_LOAD_KEY(ip2[1].key, PMIX_FABRIC_ENDPT);
            ip2[1].value.type = PMIX_DATA_ARRAY;
            ip2[1].value.data.darray = d3;
            u32 = (uint32_t*)d3->array;
            for (p=0; p < q; p++) {
                u32[p] = 3180 + (m * 4) + p;
            }
            /* the third element will also be a data array
             * containing the fabric coordinates of the proc
             * for each NIC - note that the NIC is the true
             * "holder" of the coordinate, but we pass it for
             * each proc for ease of lookup. The coordinate is
             * expressed in LOGICAL view (i.e., as x,y,z) where
             * the z-coordinate is the number of the plane, the
             * y-coordinate is the index of the switch in that plane
             * to which the NIC is connected, and the x-coord is
             * the index of the port on that switch.
             *
             * Thus, two procs that share the same y,z-coords are
             * on the same switch. */
            PMIX_DATA_ARRAY_CREATE(d3, q, PMIX_COORD);
            PMIX_LOAD_KEY(ip2[2].key, PMIX_FABRIC_COORDINATE);
            ip2[2].value.type = PMIX_DATA_ARRAY;
            ip2[2].value.data.darray = d3;
            coords = (pmix_coord_t*)d3->array;
#if 0
            nic = (pnet_nic_t*)pmix_list_get_first(&nd->nics);
            pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                                "pnet:test:allocate assigning %d coordinates for rank %u",
                                (int)q, rank);
            for (p=0; p < q; p++) {
                pln = (pnet_plane_t*)nic->plane;
                sw = (pnet_switch_t*)nic->s;
                coords[p].fabric = strdup("test");
                coords[p].plane = strdup(pln->name);
                coords[p].view = PMIX_COORD_LOGICAL_VIEW;
                coords[p].dims = 3;
                coords[p].coord = (int*)malloc(3 * sizeof(int));
                coords[p].coord[2] = pln->index;
                coords[p].coord[1] = sw->index;
                coords[p].coord[0] = ((pnet_nic_t*)nic->link)->index;
                nic = (pnet_nic_t*)pmix_list_get_next(&nic->super);
            }
#endif
        }
        pmix_argv_free(locals);
        locals = NULL;
        pmix_list_append(&mylist, &kv->super);
    }

  complete:
    /* pack all our results into a buffer for xmission to the backend */
    n = pmix_list_get_size(&mylist);
    if (0 < n) {
        PMIX_CONSTRUCT(&buf, pmix_buffer_t);
        /* cycle across the list and pack the kvals */
        while (NULL != (kv = (pmix_kval_t*)pmix_list_remove_first(&mylist))) {
            PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, &buf, kv, 1, PMIX_KVAL);
            PMIX_RELEASE(kv);
            if (PMIX_SUCCESS != rc) {
                PMIX_DESTRUCT(&buf);
                goto cleanup;
            }
        }
        kv = PMIX_NEW(pmix_kval_t);
        kv->key = strdup("pmix-pnet-test-blob");
        kv->value = (pmix_value_t*)malloc(sizeof(pmix_value_t));
        if (NULL == kv->value) {
            PMIX_RELEASE(kv);
            PMIX_DESTRUCT(&buf);
            rc = PMIX_ERR_NOMEM;
            goto cleanup;
        }
        kv->value->type = PMIX_BYTE_OBJECT;
        PMIX_UNLOAD_BUFFER(&buf, kv->value->data.bo.bytes, kv->value->data.bo.size);
        PMIX_DESTRUCT(&buf);
        pmix_list_append(ilist, &kv->super);
    }

  cleanup:
    PMIX_LIST_DESTRUCT(&mylist);
    if (NULL != nodes) {
        pmix_argv_free(nodes);
    }
    if (NULL != procs) {
        pmix_argv_free(procs);
    }
    if (NULL != locals) {
        pmix_argv_free(locals);
    }
    return rc;
}

static pmix_status_t setup_local_network(pmix_namespace_t *nptr,
                                         pmix_info_t info[],
                                         size_t ninfo)
{
    size_t n, nvals;
    pmix_buffer_t bkt;
    int32_t cnt;
    pmix_kval_t *kv;
    pmix_status_t rc;
    char *idkey = NULL;
    uint64_t seckey = 0;
    pmix_info_t *iptr;

    pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                        "pnet:test:setup_local_network with %lu info", (unsigned long)ninfo);

    if (NULL != info) {
        for (n=0; n < ninfo; n++) {
            /* look for my key */
            if (PMIX_CHECK_KEY(&info[n], "pmix-pnet-test-blob")) {
                pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                                    "pnet:test:setup_local_network found my blob");
                /* this macro NULLs and zero's the incoming bo */
                PMIX_LOAD_BUFFER(pmix_globals.mypeer, &bkt,
                                 info[n].value.data.bo.bytes,
                                 info[n].value.data.bo.size);
                /* cycle thru the blob and extract the kvals */
                kv = PMIX_NEW(pmix_kval_t);
                cnt = 1;
                PMIX_BFROPS_UNPACK(rc, pmix_globals.mypeer,
                                   &bkt, kv, &cnt, PMIX_KVAL);
                while (PMIX_SUCCESS == rc) {
                    pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                                        "recvd KEY %s %s", kv->key, PMIx_Data_type_string(kv->value->type));
                    /* check for the fabric ID */
                    if (PMIX_CHECK_KEY(kv, PMIX_ALLOC_FABRIC_ID)) {
                        if (NULL != idkey) {
                            PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
                            free(idkey);
                            return PMIX_ERR_BAD_PARAM;
                        }
                        idkey = strdup(kv->value->data.string);
                        pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                                            "pnet:test:setup_local_network idkey %s", idkey);
                    } else if (PMIX_CHECK_KEY(kv, PMIX_SET_ENVAR)) {
                        /* if this is an envar we are to set, save it on our
                         * list - we will supply it when setup_fork is called */
                        pmix_argv_append_nosize(&myenvlist, kv->value->data.envar.envar);
                        pmix_argv_append_nosize(&myvalues, kv->value->data.envar.value);
                    } else if (PMIX_CHECK_KEY(kv, PMIX_ALLOC_FABRIC_SEC_KEY)) {
                        /* our fabric security key was stored as a byte object but
                         * is really just a uint64_t */
                        memcpy(&seckey, kv->value->data.bo.bytes, sizeof(uint64_t));
                    } else if (PMIX_CHECK_KEY(kv, PMIX_ALLOC_FABRIC_ENDPTS)) {
                        iptr = (pmix_info_t*)kv->value->data.darray->array;
                        nvals = kv->value->data.darray->size;
                        /* each element in this array is itself an array containing
                         * the rank and the endpts and coords assigned to that rank. This is
                         * precisely the data we need to cache for the job, so
                         * just do so) */
                        pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                                            "pnet:test:setup_local_network caching %d endpts", (int)nvals);
                        PMIX_GDS_CACHE_JOB_INFO(rc, pmix_globals.mypeer, nptr, iptr, nvals);
                        if (PMIX_SUCCESS != rc) {
                            PMIX_RELEASE(kv);
                            if (NULL != idkey) {
                                free(idkey);
                            }
                            return rc;
                        }
                    }
                    PMIX_RELEASE(kv);
                    kv = PMIX_NEW(pmix_kval_t);
                    cnt = 1;
                    PMIX_BFROPS_UNPACK(rc, pmix_globals.mypeer,
                                       &bkt, kv, &cnt, PMIX_KVAL);
                }
                PMIX_RELEASE(kv);
                /* restore the incoming data */
                info[n].value.data.bo.bytes = bkt.base_ptr;
                info[n].value.data.bo.size = bkt.bytes_used;
                bkt.base_ptr = NULL;
                bkt.bytes_used = 0;
            }
        }
    }

    if (NULL != idkey) {
        free(idkey);
    }
    return PMIX_SUCCESS;
}

static pmix_status_t setup_fork(pmix_namespace_t *nptr,
                                const pmix_proc_t *proc,
                                char ***env)
{
    int n;

    /* if we have any envars to contribute, do so here */
    if (NULL != myenvlist) {
        for (n=0; NULL != myenvlist[n]; n++) {
            pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                                "pnet:test:setup_fork setenv: %s=%s",
                                myenvlist[n], myvalues[n]);
            pmix_setenv(myenvlist[n], myvalues[n], true, env);
        }
    }
    return PMIX_SUCCESS;
}
