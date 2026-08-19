/*
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2016      IBM Corporation.  All rights reserved.
 *
 * Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
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
#include <time.h>

#include "pmix_common.h"

#include "src/class/pmix_list.h"
#include "src/class/pmix_pointer_array.h"
#include "src/include/pmix_globals.h"
#include "src/include/pmix_socket_errno.h"
#include "src/mca/base/pmix_mca_base_var.h"
#include "src/mca/preg/preg.h"
#include "src/util/pmix_alfg.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_name_fns.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_environ.h"
#include "src/util/pmix_printf.h"
#include "src/util/pmix_show_help.h"

#include "pnet_simptest.h"
#include "src/mca/pnet/base/base.h"
#include "src/mca/pnet/pnet.h"

static pmix_status_t simptest_init(void);
static void simptest_finalize(void);
static pmix_status_t allocate(pmix_namespace_t *nptr, pmix_info_t info[], size_t ninfo,
                              pmix_list_t *ilist);
static pmix_status_t setup_local_network(pmix_nspace_env_cache_t *nptr, pmix_info_t info[],
                                         size_t ninfo);

pmix_pnet_module_t pmix_simptest_module = {
    .name = "simptest",
    .init = simptest_init,
    .finalize = simptest_finalize,
    .allocate = allocate,
    .setup_local_network = setup_local_network};

/* internal tracking structures - each node in the config file is
 * assigned a fabric coordinate (parsed from the file) and a synthetic
 * endpoint, which are then handed out to the procs running on it */
typedef struct {
    pmix_list_item_t super;
    char *name;
    pmix_coord_t coord;
    pmix_byte_object_t endpt;
} pnet_node_t;
static void ndcon(pnet_node_t *p)
{
    p->name = NULL;
    p->coord.view = PMIX_COORD_VIEW_UNDEF;
    p->coord.coord = NULL;
    p->coord.dims = 0;
    PMIX_BYTE_OBJECT_CONSTRUCT(&p->endpt);
}
static void nddes(pnet_node_t *p)
{
    if (NULL != p->name) {
        free(p->name);
    }
    if (NULL != p->coord.coord) {
        free(p->coord.coord);
    }
    PMIX_BYTE_OBJECT_DESTRUCT(&p->endpt);
}
static PMIX_CLASS_INSTANCE(pnet_node_t, pmix_list_item_t, ndcon, nddes);

/* internal variables */
static pmix_list_t mynodes;

#define PMIX_SIMPTEST_MAX_LINE_LENGTH 1024

static char *localgetline(FILE *fp)
{
    char *ret, *buff;
    char input[PMIX_SIMPTEST_MAX_LINE_LENGTH];
    size_t len;
    int i = 0;

    ret = fgets(input, PMIX_SIMPTEST_MAX_LINE_LENGTH, fp);
    if (NULL == ret) {
        return NULL;
    }
    /* remove the newline - but only if there is one. A file whose last
     * line ends without one, or a line longer than our buffer, has no
     * newline to spare and would otherwise lose its last character */
    len = strlen(input);
    if (0 < len && '\n' == input[len - 1]) {
        input[len - 1] = '\0';
    }
    /* strip any leading whitespace */
    while (' ' == input[i] || '\t' == input[i]) {
        i++;
    }
    buff = strdup(&input[i]);
    return buff;
}

static pmix_status_t simptest_init(void)
{
    FILE *fp = NULL;
    char *line, **tmp, *endptr;
    pnet_node_t *nd;
    pmix_status_t rc;
    int i, n, cache[1024];

    pmix_output_verbose(2, pmix_pnet_base_framework.framework_output, "pnet: simptest init");

    PMIX_CONSTRUCT(&mynodes, pmix_list_t);

    /* if the configuration was given in a file, then build
     * the topology so we can respond to requests */
    if (NULL == pmix_mca_pnet_simptest_component.configfile) {
        /* we cannot function */
        return PMIX_ERR_INIT;
    }

    fp = fopen(pmix_mca_pnet_simptest_component.configfile, "r");
    if (NULL == fp) {
        pmix_show_help("help-pnet-simptest.txt", "missing-file", true,
                       pmix_mca_pnet_simptest_component.configfile);
        return PMIX_ERR_FATAL;
    }
    while (NULL != (line = localgetline(fp))) {
        /* if the line starts with a '#' or is blank, then
         * it is a comment and we ignore it */
        if (0 == strlen(line) || '#' == line[0]) {
            free(line);
            continue;
        }
        tmp = PMIx_Argv_split(line, ' ');
        if (NULL == tmp) {
            free(line);
            continue;
        }
        nd = PMIX_NEW(pnet_node_t);
        if (NULL == nd) {
            PMIx_Argv_free(tmp);
            free(line);
            rc = PMIX_ERR_NOMEM;
            goto error;
        }
        nd->name = strdup(tmp[0]);
        if (NULL == nd->name) {
            PMIX_RELEASE(nd);
            PMIx_Argv_free(tmp);
            free(line);
            rc = PMIX_ERR_NOMEM;
            goto error;
        }
        pmix_list_append(&mynodes, &nd->super);
        n = 0;
        while (n < 1024 && NULL != tmp[n + 1]) {
            /* a coordinate that is not a number is a mistake in the
             * topology file, and silently reading it as zero would put
             * the node somewhere it is not */
            cache[n] = (int) strtol(tmp[n + 1], &endptr, 10);
            if (endptr == tmp[n + 1] || '\0' != *endptr) {
                pmix_show_help("help-pnet-simptest.txt", "bad-coordinate", true,
                               pmix_mca_pnet_simptest_component.configfile, nd->name,
                               tmp[n + 1]);
                PMIx_Argv_free(tmp);
                free(line);
                rc = PMIX_ERR_BAD_PARAM;
                goto error;
            }
            ++n;
        }

        /* the remainder of the line is this node's fabric coordinate */
        nd->coord.view = PMIX_COORD_LOGICAL_VIEW;
        nd->coord.dims = n;
        if (0 < n) {
            nd->coord.coord = (uint32_t *) malloc(n * sizeof(uint32_t));
            for (i = 0; i < n; i++) {
                nd->coord.coord[i] = (uint32_t) cache[i];
            }
        }
        /* synthesize a fabric endpoint for this node - simptest has no
         * real fabric, so we just fabricate a recognizable string */
        pmix_asprintf((char **) &nd->endpt.bytes, "simptest.endpt.%s", nd->name);
        if (NULL == nd->endpt.bytes) {
            /* allocate hands this to every proc on the node, so a node
             * without one cannot be left on the list */
            PMIx_Argv_free(tmp);
            free(line);
            rc = PMIX_ERR_NOMEM;
            goto error;
        }
        nd->endpt.size = strlen(nd->endpt.bytes) + 1;
        free(line);
        PMIx_Argv_free(tmp);
    }

    fclose(fp);
    return PMIX_SUCCESS;

error:
    /* a module whose init fails is dropped by the framework's select
     * without ever being added to the active list, so our finalize will
     * not be called - give back what we built here */
    fclose(fp);
    PMIX_LIST_DESTRUCT(&mynodes);
    return rc;
}

static void simptest_finalize(void)
{
    pmix_output_verbose(2, pmix_pnet_base_framework.framework_output, "pnet: simptest finalize");

    PMIX_LIST_DESTRUCT(&mynodes);
}

/* A node or proc map arrives in any of three shapes, and which one it
 * is depends on the host: an encoded PMIX_REGEX (payload in the byte
 * object), a PMIX_REGEX2 (which has to be run back through the preg
 * parser first), or a plain PMIX_STRING. Reading value.data.string
 * unconditionally, as this used to, works for the first two only by
 * accident - .string and .bo.bytes alias - and for a REGEX2 it hands
 * the parser a pmix_regex2_t* cast to char*. gds/hash keeps the same
 * dispatch in gds_utils.c's parse_map for the same reason; if a third
 * caller ever needs it, that is the point to hoist it into preg. */
static pmix_status_t parse_map(const pmix_value_t *val,
                               pmix_status_t (*parser)(const char *, char ***),
                               char ***result)
{
    pmix_status_t rc;
    char *decoded = NULL;

    switch (val->type) {
    case PMIX_REGEX:
        if (NULL == val->data.bo.bytes) {
            return PMIX_ERR_BAD_PARAM;
        }
        return parser(val->data.bo.bytes, result);
    case PMIX_REGEX2:
        if (NULL == val->data.regex2) {
            return PMIX_ERR_BAD_PARAM;
        }
        rc = pmix_preg.parse_regex(val->data.regex2, NULL, 0, &decoded);
        if (PMIX_SUCCESS != rc) {
            return rc;
        }
        rc = parser(decoded, result);
        free(decoded);
        return rc;
    case PMIX_STRING:
        if (NULL == val->data.string) {
            return PMIX_ERR_BAD_PARAM;
        }
        return parser(val->data.string, result);
    default:
        return PMIX_ERR_TYPE_MISMATCH;
    }
}

/* NOTE: if there is any binary data to be transferred, then
 * this function MUST pack it for transport as the host will
 * not know how to do so */
static pmix_status_t allocate(pmix_namespace_t *nptr, pmix_info_t info[], size_t ninfo,
                              pmix_list_t *ilist)
{
    size_t m, n, q;
    char **procs = NULL;
    char **nodes = NULL;
    pmix_status_t rc;
    pmix_list_t mylist;
    /* the cleanup path frees this, and the early exits above the node
     * loop reach that path before it has ever been assigned */
    char **locals = NULL;
    pnet_node_t *nd, *nd2;
    pmix_kval_t *kv, *kvc;
    pmix_info_t *iptr, *ip2;
    pmix_data_array_t *darray, *d2, *d3, *dcoord, *dnode;
    pmix_rank_t rank;
    pmix_buffer_t buf;
    pmix_byte_object_t *bptr;
    pmix_coord_t *cptr;

    rc = PMIX_SUCCESS;
    pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                        "pnet:simptest:allocate for nspace %s", nptr->nspace);

    /* if I am not the scheduler, then ignore this call - should never
     * happen, but check to be safe. Decline rather than report success
     * so the status says we did nothing */
    if (!PMIX_PEER_IS_SCHEDULER(pmix_globals.mypeer)) {
        return PMIX_ERR_TAKE_NEXT_OPTION;
    }

    if (NULL == info) {
        return PMIX_ERR_TAKE_NEXT_OPTION;
    }

    /* check directives to see if a crypto key and/or
     * fabric resource allocations requested */
    for (n = 0; n < ninfo; n++) {
        pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                            "pnet:simptest:allocate processing key %s", info[n].key);
        if (PMIX_CHECK_KEY(&info[n], PMIX_PROC_MAP)) {
            rc = parse_map(&info[n].value, pmix_preg.parse_procs, &procs);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                goto earlyout;
            }
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_NODE_MAP)) {
            rc = parse_map(&info[n].value, pmix_preg.parse_nodes, &nodes);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                goto earlyout;
            }
        }
    }

    if (NULL == procs || NULL == nodes) {
        pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                            "pnet:simptest:allocate missing proc/node map for nspace %s",
                            nptr->nspace);
        /* not an error - continue to next active component */
        rc = PMIX_ERR_TAKE_NEXT_OPTION;
        goto earlyout;
    }

    /* the two maps are parsed independently, and the walk below reads
     * procs[n] for every entry of nodes[n] - so they have to be the same
     * length or we would hand a node another node's ranks and then read
     * off the end of the shorter array. gds/hash's store_map makes the
     * same check on the same pair for the same reason */
    if (PMIx_Argv_count(procs) != PMIx_Argv_count(nodes)) {
        PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
        rc = PMIX_ERR_BAD_PARAM;
        goto earlyout;
    }

    PMIX_CONSTRUCT(&mylist, pmix_list_t);

    pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                        "pnet:simptest:allocate assigning endpoints for nspace %s", nptr->nspace);

    /* cycle across the nodes and add the endpoints
     * for each proc on the node - we assume the same
     * list of static endpoints on each node */
    for (n = 0; NULL != nodes[n]; n++) {
        /* split the procs for this node */
        locals = PMIx_Argv_split(procs[n], ',');
        if (NULL == locals) {
            /* aren't any on this node */
            continue;
        }
        /* find this node in our list */
        nd = NULL;
        PMIX_LIST_FOREACH (nd2, &mynodes, pnet_node_t) {
            if (0 == strcmp(nd2->name, nodes[n])) {
                nd = nd2;
                break;
            }
        }
        if (NULL == nd) {
            /* far from impossible: it means the topology file does not
             * describe a node the job was placed on. We cannot give that
             * node's procs endpoints, and handing out fabric data for
             * only some of the job is worse than handing out none - so
             * say which node is missing and decline the whole request
             * rather than failing it, which would abort the framework's
             * fan-out for every other component too */
            pmix_show_help("help-pnet-simptest.txt", "node-not-found", true,
                           pmix_mca_pnet_simptest_component.configfile, nodes[n]);
            rc = PMIX_ERR_TAKE_NEXT_OPTION;
            goto cleanup;
        }
        kv = PMIX_NEW(pmix_kval_t);
        if (NULL == kv) {
            rc = PMIX_ERR_NOMEM;
            goto cleanup;
        }
        kv->key = strdup(PMIX_ALLOC_FABRIC_ENDPTS);
        kv->value = (pmix_value_t *) malloc(sizeof(pmix_value_t));
        if (NULL == kv->value) {
            PMIX_RELEASE(kv);
            rc = PMIX_ERR_NOMEM;
            goto cleanup;
        }
        kv->value->type = PMIX_DATA_ARRAY;
        /* for each proc, we will assign an endpt
         * for each NIC on the node */
        q = PMIx_Argv_count(locals);
        PMIX_DATA_ARRAY_CREATE(darray, q, PMIX_INFO);
        kv->value->data.darray = darray;
        if (NULL == darray) {
            PMIX_RELEASE(kv);
            rc = PMIX_ERR_NOMEM;
            goto cleanup;
        }
        pmix_list_append(&mylist, &kv->super);
        iptr = (pmix_info_t *) darray->array;
        for (m = 0; NULL != locals[m]; m++) {
            /* each proc is assigned a fabric endpoint corresponding to
             * the node upon which it is executing. The endpoint is
             * per-process data (PMIX_FABRIC_ENDPT is fetched by rank),
             * so it lives in this proc's PMIX_PROC_INFO_ARRAY array */
            PMIX_LOAD_KEY(iptr[m].key, PMIX_PROC_INFO_ARRAY);
            PMIX_DATA_ARRAY_CREATE(d2, 2, PMIX_INFO);
            iptr[m].value.type = PMIX_DATA_ARRAY;
            iptr[m].value.data.darray = d2;
            if (NULL == d2) {
                rc = PMIX_ERR_NOMEM;
                goto cleanup;
            }
            ip2 = (pmix_info_t *) d2->array;
            /* start with the rank */
            rank = strtoul(locals[m], NULL, 10);
            pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                                "pnet:simptest:allocate assigning %d endpoints for rank %u",
                                (int) q, rank);
            PMIX_INFO_LOAD(&ip2[0], PMIX_RANK, &rank, PMIX_PROC_RANK);
            /* the second element is the endpt - a data array of byte objects */
            PMIX_DATA_ARRAY_CREATE(d3, 1, PMIX_BYTE_OBJECT);
            PMIX_LOAD_KEY(ip2[1].key, PMIX_FABRIC_ENDPT);
            ip2[1].value.type = PMIX_DATA_ARRAY;
            ip2[1].value.data.darray = d3;
            if (NULL == d3) {
                rc = PMIX_ERR_NOMEM;
                goto cleanup;
            }
            bptr = (pmix_byte_object_t *) d3->array;
            bptr[0].bytes = strdup(nd->endpt.bytes);
            if (NULL == bptr[0].bytes) {
                rc = PMIX_ERR_NOMEM;
                goto cleanup;
            }
            bptr[0].size = nd->endpt.size;
        }
        PMIx_Argv_free(locals);
        locals = NULL;

        /* the fabric coordinate is a per-NODE attribute
         * (PMIX_FABRIC_COORDINATES is fetched at the node level, not by
         * rank), so deliver it inside a PMIX_NODE_INFO_ARRAY tagged with
         * this node's name. The backend GDS will match it to the local
         * node and hand it to every proc running there */
        kvc = PMIX_NEW(pmix_kval_t);
        if (NULL == kvc) {
            rc = PMIX_ERR_NOMEM;
            goto cleanup;
        }
        kvc->key = strdup(PMIX_NODE_INFO_ARRAY);
        kvc->value = (pmix_value_t *) malloc(sizeof(pmix_value_t));
        if (NULL == kvc->value) {
            PMIX_RELEASE(kvc);
            rc = PMIX_ERR_NOMEM;
            goto cleanup;
        }
        kvc->value->type = PMIX_DATA_ARRAY;
        PMIX_DATA_ARRAY_CREATE(dnode, 2, PMIX_INFO);
        kvc->value->data.darray = dnode;
        if (NULL == dnode) {
            PMIX_RELEASE(kvc);
            rc = PMIX_ERR_NOMEM;
            goto cleanup;
        }
        pmix_list_append(&mylist, &kvc->super);
        ip2 = (pmix_info_t *) dnode->array;
        PMIX_INFO_LOAD(&ip2[0], PMIX_HOSTNAME, nd->name, PMIX_STRING);
        PMIX_DATA_ARRAY_CREATE(dcoord, 1, PMIX_COORD);
        PMIX_LOAD_KEY(ip2[1].key, PMIX_FABRIC_COORDINATES);
        ip2[1].value.type = PMIX_DATA_ARRAY;
        ip2[1].value.data.darray = dcoord;
        if (NULL == dcoord) {
            rc = PMIX_ERR_NOMEM;
            goto cleanup;
        }
        cptr = (pmix_coord_t *) dcoord->array;
        cptr[0].view = nd->coord.view;
        cptr[0].dims = nd->coord.dims;
        if (0 < nd->coord.dims) {
            cptr[0].coord = (uint32_t *) malloc(nd->coord.dims * sizeof(uint32_t));
            if (NULL == cptr[0].coord) {
                cptr[0].dims = 0;
                rc = PMIX_ERR_NOMEM;
                goto cleanup;
            }
            memcpy(cptr[0].coord, nd->coord.coord, nd->coord.dims * sizeof(uint32_t));
        }
    }

    /* pack all our results into a buffer for xmission to the backend */
    n = pmix_list_get_size(&mylist);
    if (0 < n) {
        PMIX_CONSTRUCT(&buf, pmix_buffer_t);
        /* cycle across the list and pack the kvals */
        while (NULL != (kv = (pmix_kval_t *) pmix_list_remove_first(&mylist))) {
            PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, &buf, kv, 1, PMIX_KVAL);
            PMIX_RELEASE(kv);
            if (PMIX_SUCCESS != rc) {
                PMIX_DESTRUCT(&buf);
                goto cleanup;
            }
        }
        kv = PMIX_NEW(pmix_kval_t);
        if (NULL == kv) {
            PMIX_DESTRUCT(&buf);
            rc = PMIX_ERR_NOMEM;
            goto cleanup;
        }
        kv->key = strdup(PMIX_PNET_SIMPTEST_BLOB);
        kv->value = (pmix_value_t *) calloc(1, sizeof(pmix_value_t));
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
earlyout:
    if (NULL != nodes) {
        PMIx_Argv_free(nodes);
    }
    if (NULL != procs) {
        PMIx_Argv_free(procs);
    }
    if (NULL != locals) {
        PMIx_Argv_free(locals);
    }
    return rc;
}

static pmix_status_t setup_local_network(pmix_nspace_env_cache_t *nptr, pmix_info_t info[],
                                         size_t ninfo)
{
    size_t n, nvals;
    pmix_buffer_t bkt;
    int32_t cnt;
    pmix_kval_t *kv;
    pmix_status_t rc;
    pmix_info_t *iptr;
    pmix_info_t nodeinfo;

    pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                        "pnet:simptest:setup_local_network with %lu info", (unsigned long) ninfo);

    /* the value field is filled in per node-info kval below; the GDS
     * copies the data, so a borrowed shallow value is sufficient */
    PMIX_INFO_CONSTRUCT(&nodeinfo);
    PMIX_LOAD_KEY(nodeinfo.key, PMIX_NODE_INFO_ARRAY);

    if (NULL != info) {
        for (n = 0; n < ninfo; n++) {
            /* look for my key */
            if (PMIX_CHECK_KEY(&info[n], PMIX_PNET_SIMPTEST_BLOB)) {
                pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                                    "pnet:simptest:setup_local_network found my blob");
                if (PMIX_BYTE_OBJECT != info[n].value.type
                    || NULL == info[n].value.data.bo.bytes) {
                    PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
                    return PMIX_ERR_BAD_PARAM;
                }
                /* the array belongs to our caller, who hands the same one
                 * to every active module in turn - so read it in place
                 * rather than taking the bytes away from it. The plain
                 * PMIX_LOAD_BUFFER NULLs the source, and we must not
                 * destruct the buffer either way since the bytes are not
                 * ours, so it would orphan the blob outright */
                PMIX_LOAD_BUFFER_NON_DESTRUCT(pmix_globals.mypeer, &bkt,
                                              info[n].value.data.bo.bytes,
                                              info[n].value.data.bo.size);
                /* cycle thru the blob and extract the kvals */
                kv = PMIX_NEW(pmix_kval_t);
                cnt = 1;
                PMIX_BFROPS_UNPACK(rc, pmix_globals.mypeer, &bkt, kv, &cnt, PMIX_KVAL);
                while (PMIX_SUCCESS == rc) {
                    pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                                        "recvd KEY %s %s", kv->key,
                                        PMIx_Data_type_string(kv->value->type));
                    /* check for the fabric ID */
                    if (PMIX_CHECK_KEY(kv, PMIX_ALLOC_FABRIC_ENDPTS)) {
                        /* a key does not make the union an array - this
                         * blob crossed the wire, so check before reading
                         * the value as one */
                        if (PMIX_DATA_ARRAY != kv->value->type
                            || NULL == kv->value->data.darray
                            || NULL == kv->value->data.darray->array) {
                            PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
                            PMIX_RELEASE(kv);
                            return PMIX_ERR_BAD_PARAM;
                        }
                        iptr = (pmix_info_t *) kv->value->data.darray->array;
                        nvals = kv->value->data.darray->size;
                        /* each element in this array is itself an array containing
                         * the rank and the endpts and coords assigned to that rank. This is
                         * precisely the data we need to cache for the job, so
                         * just do so) */
                        pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                                            "pnet:simptest:setup_local_network caching %d endpts",
                                            (int) nvals);
                        PMIX_GDS_CACHE_JOB_INFO(rc, pmix_globals.mypeer, nptr->ns, iptr, nvals);
                        if (PMIX_SUCCESS != rc) {
                            PMIX_RELEASE(kv);
                            return rc;
                        }
                    } else if (PMIX_CHECK_KEY(kv, PMIX_NODE_INFO_ARRAY)) {
                        if (PMIX_DATA_ARRAY != kv->value->type
                            || NULL == kv->value->data.darray
                            || NULL == kv->value->data.darray->array) {
                            PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
                            PMIX_RELEASE(kv);
                            return PMIX_ERR_BAD_PARAM;
                        }
                        /* per-node fabric coordinate - hand the entire
                         * node-info array to the GDS, which will store it
                         * as node-level info and match it to the local
                         * node so PMIX_FABRIC_COORDINATES resolves there */
                        pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                                            "pnet:simptest:setup_local_network caching node info");
                        nodeinfo.value = *kv->value;
                        PMIX_GDS_CACHE_JOB_INFO(rc, pmix_globals.mypeer, nptr->ns, &nodeinfo, 1);
                        if (PMIX_SUCCESS != rc) {
                            PMIX_RELEASE(kv);
                            return rc;
                        }
                    }
                    PMIX_RELEASE(kv);
                    kv = PMIX_NEW(pmix_kval_t);
                    cnt = 1;
                    PMIX_BFROPS_UNPACK(rc, pmix_globals.mypeer, &bkt, kv, &cnt, PMIX_KVAL);
                }
                PMIX_RELEASE(kv);
            }
        }
    }

    return PMIX_SUCCESS;
}
