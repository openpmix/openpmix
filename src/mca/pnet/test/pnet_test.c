/*
 * Copyright (c) 2015-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2016      IBM Corporation.  All rights reserved.
 *
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include <src/include/pmix_config.h>

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

#include <pmix_common.h>

#include "src/mca/base/pmix_mca_base_var.h"
#include "src/include/pmix_socket_errno.h"
#include "src/include/pmix_globals.h"
#include "src/class/pmix_list.h"
#include "src/class/pmix_pointer_array.h"
#include "src/util/alfg.h"
#include "src/util/argv.h"
#include "src/util/error.h"
#include "src/util/name_fns.h"
#include "src/util/output.h"
#include "src/util/pmix_environ.h"
#include "src/mca/preg/preg.h"

#include "src/mca/pnet/pnet.h"
#include "src/mca/pnet/base/base.h"
#include "pnet_test.h"

static pmix_status_t test_init(void);
static void test_finalize(void);
static pmix_status_t allocate(pmix_namespace_t *nptr,
                              pmix_info_t info[], size_t ninfo,
                              pmix_list_t *ilist);
static pmix_status_t setup_local_network(pmix_namespace_t *nptr,
                                         pmix_info_t info[],
                                         size_t ninfo);
static pmix_status_t setup_fork(pmix_namespace_t *nptr,
                                const pmix_proc_t *proc,
                                char ***env);
static void child_finalized(pmix_proc_t *peer);
static void local_app_finalized(pmix_namespace_t *nptr);
static void deregister_nspace(pmix_namespace_t *nptr);
static pmix_status_t collect_inventory(pmix_info_t directives[], size_t ndirs,
                                       pmix_inventory_cbfunc_t cbfunc, void *cbdata);
static pmix_status_t deliver_inventory(pmix_info_t info[], size_t ninfo,
                                       pmix_info_t directives[], size_t ndirs,
                                       pmix_op_cbfunc_t cbfunc, void *cbdata);
static pmix_status_t register_fabric(pmix_fabric_t *fabric,
                                     const pmix_info_t directives[],
                                     size_t ndirs);
static pmix_status_t deregister_fabric(pmix_fabric_t *fabric);
static pmix_status_t get_num_verts(pmix_fabric_t *fabric, uint32_t *nverts);
static pmix_status_t get_cost(pmix_fabric_t *fabric,
                              uint32_t src, uint32_t dest,
                              uint16_t *cost);
static pmix_status_t get_vertex(pmix_fabric_t *fabric,
                                uint32_t i,
                                pmix_value_t *identifier,
                                char **nodename);
static pmix_status_t get_index(pmix_fabric_t *fabric,
                               pmix_value_t *identifier,
                               uint32_t *i,
                               char **nodename);
pmix_pnet_module_t pmix_test_module = {
    .name = "test",
    .init = test_init,
    .finalize = test_finalize,
    .allocate = allocate,
    .setup_local_network = setup_local_network,
    .setup_fork = setup_fork,
    .child_finalized = child_finalized,
    .local_app_finalized = local_app_finalized,
    .deregister_nspace = deregister_nspace,
    .collect_inventory = collect_inventory,
    .deliver_inventory = deliver_inventory,
    .register_fabric = register_fabric,
    .deregister_fabric = deregister_fabric,
    .get_num_vertices = get_num_verts,
    .get_cost = get_cost,
    .get_vertex = get_vertex,
    .get_index = get_index
};

/* internal tracking structures */
typedef struct {
    pmix_list_item_t super;
    char *name;
    int index;
    void *node;     // pointer to node hosting this nic
    void *s;        // pointer to switch hosting this port, or
                    // pointer to switch this nic is attached to
    void *plane;    // pointer to plane this NIC is attached to
    void *link;     // nic this nic is connected to
} pnet_nic_t;
static void ncon(pnet_nic_t *p)
{
    p->name = NULL;
    p->index = -1;
    p->node = NULL;
    p->s = NULL;
    p->plane = NULL;
    p->link = NULL;
}
static void ndes(pnet_nic_t *p)
{
    if (NULL != p->name) {
        free(p->name);
    }
}
static PMIX_CLASS_INSTANCE(pnet_nic_t,
                           pmix_list_item_t,
                           ncon, ndes);

typedef struct {
    pmix_list_item_t super;
    char *name;
    int index;
    void *left;  // switch to the left of this one in the ring
    pnet_nic_t leftport;
    void *right; // switch to the right of this one in the ring
    pnet_nic_t rightport;
    pmix_list_t ports;  // NICs included in the switch
} pnet_switch_t;
static void scon(pnet_switch_t *p)
{
    p->name = NULL;
    p->index = -1;
    p->left = NULL;
    p->right = NULL;
    PMIX_CONSTRUCT(&p->leftport, pnet_nic_t);
    PMIX_CONSTRUCT(&p->rightport, pnet_nic_t);
    PMIX_CONSTRUCT(&p->ports, pmix_list_t);
}
static void sdes(pnet_switch_t *p)
{
    if (NULL != p->name) {
        free(p->name);
    }
    PMIX_DESTRUCT(&p->leftport);
    PMIX_DESTRUCT(&p->rightport);
    PMIX_LIST_DESTRUCT(&p->ports);
}
static PMIX_CLASS_INSTANCE(pnet_switch_t,
                           pmix_list_item_t,
                           scon, sdes);

typedef struct {
    pmix_list_item_t super;
    /* use an atomic lock for this object */
    pmix_atomic_lock_t atomlock;
    char *name;
    int index;
    bool dense;
    int nswitches;
    uint64_t nverts;
    uint16_t **costmatrix;
    pmix_list_t switches;
    uint64_t revision;
} pnet_plane_t;
static void pcon(pnet_plane_t *p)
{
    pmix_atomic_lock_init(&p->atomlock, 0);
    p->name = NULL;
    p->index = -1;
    p->dense = false;
    p->nswitches = 0;
    p->nverts = 0;
    p->costmatrix = NULL;
    PMIX_CONSTRUCT(&p->switches, pmix_list_t);
    p->revision = 0;
}
static void pdes(pnet_plane_t *p)
{
    uint64_t n;

    if (NULL != p->name) {
        free(p->name);
    }
    if (NULL != p->costmatrix) {
        for (n=0; n < p->nverts; n++) {
            free(p->costmatrix[n]);
        }
        free(p->costmatrix);
    }
    PMIX_LIST_DESTRUCT(&p->switches);
}
static PMIX_CLASS_INSTANCE(pnet_plane_t,
                           pmix_list_item_t,
                           pcon, pdes);

typedef struct {
    pmix_list_item_t super;
    char *name;
    pmix_list_t nics;
} pnet_node_t;
static void ndcon(pnet_node_t *p)
{
    p->name = NULL;
    PMIX_CONSTRUCT(&p->nics, pmix_list_t);
}
static void nddes(pnet_node_t *p)
{
    if (NULL != p->name) {
        free(p->name);
    }
    PMIX_LIST_DESTRUCT(&p->nics);
}
static PMIX_CLASS_INSTANCE(pnet_node_t,
                           pmix_list_item_t,
                           ndcon, nddes);

/* internal variables */
static pmix_list_t myplanes;
static pmix_list_t mynodes;
static pmix_pointer_array_t myfabrics;
static pmix_pointer_array_t mynics;

static pmix_status_t test_init(void)
{
    int n, m, r, ns, nplane, nnodes, nports;
    uint64_t n64, m64;
    char **system, **ptr;
    pnet_plane_t *p;
    pnet_switch_t *s, *s2;
    pnet_nic_t *nic, *nic2;
    pnet_node_t *node;

    pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                        "pnet: test init");

    PMIX_CONSTRUCT(&myplanes, pmix_list_t);
    PMIX_CONSTRUCT(&mynodes, pmix_list_t);
    PMIX_CONSTRUCT(&myfabrics, pmix_pointer_array_t);
    pmix_pointer_array_init(&myfabrics, 1, INT_MAX, 1);
    PMIX_CONSTRUCT(&mynics, pmix_pointer_array_t);
    pmix_pointer_array_init(&mynics, 8, INT_MAX, 8);

    /* if we have a config file, read it now */
    if (NULL != mca_pnet_test_component.cfg_file) {

    } else if (NULL != mca_pnet_test_component.nverts) {
        pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                            "pnet: test creating system configuration");
        /* the system description is configured as nodes and fabric planes
         * delineated by semi-colons */
        system = pmix_argv_split(mca_pnet_test_component.nverts, ';');
        /* there can be multiple planes defined, but only one set of
         * nodes. The nodes description contains the #nodes - we assume
         * that each node has a single NIC attached to each fabric plane.
         * Thus, the nodes entry has a single field associated with it that
         * contains the number of nodes in the system.
         *
         * Similarly, we assume that each switch in the plane contains
         * a port to connect to each node in the system. For simplicity,
         * we assume a ring connection topology between the switches and
         * reserve one port on each switch to connect to its "left" peer
         * and another to connect to its "right" peer.
         *
         * Thus, the #NICS in a node equals the number of planes in the
         * overall system. The #ports in a switch equals the #nodes in
         * the system plus two for cross-switch communications.
         */
        for (r=0; NULL != system[r]; r++) {
            if (0 == strncasecmp(system[r], "nodes", 5)) {
                /* the number of nodes must follow the colon after "nodes" */
                nnodes = strtoul(&system[r][6], NULL, 10);
                for (n=0; n < nnodes; n++) {
                    node = PMIX_NEW(pnet_node_t);
                    asprintf(&node->name, "test%03d", n);
                    pmix_list_append(&mynodes, &node->super);
                }
            } else if (0 == strncasecmp(system[r], "plane", 5)) {
                /* create a plane object */
                p = PMIX_NEW(pnet_plane_t);
                /* the plane contains a flag indicating how the nodes
                 * are to be distributed across the plane plus the
                 * number of switches in the plane */
                ptr = pmix_argv_split(&system[r][6], ':');
                if (1 == pmix_argv_count(ptr)) {
                    /* default to dense */
                    p->dense = true;
                    p->nswitches = strtoul(ptr[0], NULL, 10);
                } else {
                    if ('d' == ptr[0][0] || 'D' == ptr[0][0]) {
                        p->dense = true;
                    }
                    p->nswitches = strtoul(ptr[1], NULL, 10);
                }
                pmix_argv_free(ptr);
                pmix_list_append(&myplanes, &p->super);
            } else {
                PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
                pmix_argv_free(system);
                return PMIX_ERR_BAD_PARAM;
            }
        }
        /* setup the ports in each switch for each plane */
        nplane = 0;
        PMIX_LIST_FOREACH(p, &myplanes, pnet_plane_t) {
            /* assign a name to the plane */
            asprintf(&p->name, "plane%03d", nplane);
            /* setup the ports on the switches */
            nports = nnodes / p->nswitches;
            /* if it didn't divide evenly, then we have to add
             * one to each switch to ensure we have enough ports */
            if (0 != nnodes % p->nswitches) {
                ++nports;
            }
            for (n=0; n < p->nswitches; n++) {
                s = PMIX_NEW(pnet_switch_t);
                asprintf(&s->name, "%s:switch%03d", p->name, n);
                s->index = n;
                pmix_list_append(&p->switches, &s->super);
                asprintf(&s->leftport.name, "%s:port000", s->name);
                asprintf(&s->rightport.name, "%s:port%03d", s->name, nports+1);
                for (m=0; m < nports; m++) {
                    nic = PMIX_NEW(pnet_nic_t);
                    asprintf(&nic->name, "%s:port%03d", s->name, m+1);
                    nic->s = s;
                    nic->plane = p;
                    pmix_list_append(&s->ports, &nic->super);
                }
            }

            /* link the switch ring - first nic on each switch connects
             * to the left, last nic on each switch connects to
             * the right */
            s = (pnet_switch_t*)pmix_list_get_first(&p->switches);
            s->left = pmix_list_get_last(&p->switches);
            s->right = pmix_list_get_next(&s->super);
            /* setup his NICs to point to the right place */
            s2 = (pnet_switch_t*)s->left;
            s->leftport.link = &s2->rightport;
            s2->rightport.link = &s->leftport;

            s2 = (pnet_switch_t*)s->right;
            s->rightport.link = &s2->leftport;
            s2->leftport.link = &s->rightport;

            /* progress the search */
            s = (pnet_switch_t*)pmix_list_get_next(&s->super);
            while (s != (pnet_switch_t*)pmix_list_get_last(&p->switches)) {
                s->left = pmix_list_get_prev(&s->super);
                s->right = pmix_list_get_next(&s->super);
                /* setup his NICs to point to the right place */
                s2 = (pnet_switch_t*)s->left;
                s->leftport.link = &s2->rightport;
                s2->rightport.link = &s->leftport;

                s2 = (pnet_switch_t*)s->right;
                s->rightport.link = &s2->leftport;
                s2->leftport.link = &s->rightport;
                s2->left = s;

                /* progress the search */
                s = (pnet_switch_t*)pmix_list_get_next(&s->super);
            }
            /* s now points to the last item on the list */
            s->right = pmix_list_get_first(&p->switches);
            s2 = (pnet_switch_t*)s->left;
            s->leftport.link = &s2->rightport;
            s2->rightport.link = &s->leftport;
            s2 = (pnet_switch_t*)s->right;
            s->rightport.link = &s2->leftport;
            s2->leftport.link = &s->rightport;

            /* now cycle across the nodes and setup their connections
             * to the switches */
            if (p->dense) {
                /* connect each successive node to the same switch
                 * until that switch is full - then move to the next */
                s = (pnet_switch_t*)pmix_list_get_first(&p->switches);
                nic = (pnet_nic_t*)pmix_list_get_first(&s->ports);
                n = 0;
                ns = pmix_list_get_size(&s->ports);
                PMIX_LIST_FOREACH(node, &mynodes, pnet_node_t) {
                    nic2 = PMIX_NEW(pnet_nic_t);
                    asprintf(&nic2->name, "%s:nic%03d", node->name, n);
                    ++n;
                    --ns;
                    nic2->node = node;
                    nic2->s = s;
                    nic2->plane = p;
                    nic2->index = pmix_pointer_array_add(&mynics, nic2);
                    PMIX_RETAIN(nic2);
                    pmix_list_append(&node->nics, &nic2->super);
                    nic2->link = nic;
                    nic->link = nic2;
                    if (0 == ns) {
                        /* move to the next switch */
                        s = (pnet_switch_t*)pmix_list_get_next(&s->super);
                        nic = (pnet_nic_t*)pmix_list_get_first(&s->ports);
                        ns = pmix_list_get_size(&s->ports);
                    }
                }
            }

            /* setup the cost matrix - we assume switch-to-switch hops
             * have a cost of 1, as do all node-to-switch hops */
            p->nverts = nnodes;  // we ignore the switch ports for now
            p->costmatrix = (uint16_t**)malloc(p->nverts * sizeof(uint16_t*));
            for (n64=0; n64 < p->nverts; n64++) {
                p->costmatrix[n64] = malloc(p->nverts * sizeof(uint16_t));
            }
            /* fill the matrix with the #hops between each NIC, keeping it symmetric */
            for (n64=0; n64 < p->nverts; n64++) {
                p->costmatrix[n64][n64] = 0;
                nic = (pnet_nic_t*)pmix_pointer_array_get_item(&mynics, n64);
                for (m64=n64+1; m64 < p->nverts; m64++) {
                    nic2 = (pnet_nic_t*)pmix_pointer_array_get_item(&mynics, m64);
                    /* if they are on the same switch, then cost is 2 */
                    if (nic->s == nic2->s) {
                        p->costmatrix[n64][m64] = 2;
                    } else {
                        /* the cost is increased by the distance
                         * between switches */
                        s = (pnet_switch_t*)nic->s;
                        s2 = (pnet_switch_t*)nic2->s;
                        if (s->index > s2->index) {
                            p->costmatrix[n64][m64] = 2 + s->index - s2->index;
                        } else {
                            p->costmatrix[n64][m64] = 2 + s2->index - s->index;
                        }
                    }
                    p->costmatrix[m64][n64] = p->costmatrix[n64][m64];
                }
            }
            ++nplane;
        }
        pmix_argv_free(system);
    }

    return PMIX_SUCCESS;
}

static void test_finalize(void)
{
    pmix_pnet_fabric_t *ft;
    pnet_nic_t *nic;
    int n;

    pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                        "pnet: test finalize");

    for (n=0; n < myfabrics.size; n++) {
        if (NULL != (ft = (pmix_pnet_fabric_t*)pmix_pointer_array_get_item(&myfabrics, n))) {
            PMIX_RELEASE(ft);
        }
    }
    PMIX_DESTRUCT(&myfabrics);
    PMIX_LIST_DESTRUCT(&mynodes);
    for (n=0; n < mynics.size; n++) {
        if (NULL != (nic = (pnet_nic_t*)pmix_pointer_array_get_item(&mynics, n))) {
            PMIX_RELEASE(nic);
        }
    }
    PMIX_DESTRUCT(&mynics);
    PMIX_LIST_DESTRUCT(&myplanes);
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
    size_t n, nreqs=0;
    pmix_info_t *requests = NULL;
    char *idkey = NULL;
    uint64_t unique_key = 12345;
    pmix_buffer_t buf;
    pmix_status_t rc;
    pmix_pnet_job_t *jptr, *job;
    pmix_pnet_node_t *nd;
    pmix_pnet_local_procs_t *lptr, *lp;

    pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                        "pnet:test:allocate for nspace %s key %s",
                        nptr->nspace, info->key);

    /* if I am not the gateway, then ignore this call - should never
     * happen, but check to be safe */
    if (!PMIX_PROC_IS_GATEWAY(pmix_globals.mypeer)) {
        return PMIX_SUCCESS;
    }

    if (NULL == info) {
        return PMIX_ERR_TAKE_NEXT_OPTION;
    }
    /* check directives to see if a crypto key and/or
     * network resource allocations requested */
    for (n=0; n < ninfo; n++) {
        if (PMIX_CHECK_KEY(&info[n], PMIX_SETUP_APP_ENVARS) ||
            PMIX_CHECK_KEY(&info[n], PMIX_SETUP_APP_ALL)) {
            envars = PMIX_INFO_TRUE(&info[n]);
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_ALLOC_NETWORK_ID)) {
            /* this info key includes an array of pmix_info_t, each providing
             * a key (that is to be used as the key for the allocated ports) and
             * a number of ports to allocate for that key */
            if (PMIX_DATA_ARRAY != info->value.type ||
                NULL == info->value.data.darray ||
                PMIX_INFO != info->value.data.darray->type ||
                NULL == info->value.data.darray->array) {
                /* just process something for test */
                goto process;
            }
            requests = (pmix_info_t*)info->value.data.darray->array;
            nreqs = info->value.data.darray->size;
        }
    }

    if (envars) {
        kv = PMIX_NEW(pmix_kval_t);
        if (NULL == kv) {
            return PMIX_ERR_NOMEM;
        }
        kv->key = strdup(PMIX_SET_ENVAR);
        kv->value = (pmix_value_t*)malloc(sizeof(pmix_value_t));
        if (NULL == kv->value) {
            PMIX_RELEASE(kv);
            return PMIX_ERR_NOMEM;
        }
        kv->value->type = PMIX_ENVAR;
        PMIX_ENVAR_LOAD(&kv->value->data.envar, "PMIX_TEST_ENVAR", "1", ':');
        pmix_list_append(ilist, &kv->super);
    }

    if (NULL == requests) {
        return PMIX_ERR_TAKE_NEXT_OPTION;
    }

    pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                        "pnet:test:allocate alloc_network for nspace %s",
                        nptr->nspace);

    /* cycle thru the provided array and get the ID key */
    for (n=0; n < nreqs; n++) {
        if (0 == strncmp(requests[n].key, PMIX_ALLOC_NETWORK_ID, PMIX_MAX_KEYLEN)) {
            /* check for bozo error */
            if (PMIX_STRING != requests[n].value.type ||
                NULL == requests[n].value.data.string) {
                PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
                return PMIX_ERR_BAD_PARAM;
            }
            idkey = requests[n].value.data.string;
        } else if (0 == strncasecmp(requests[n].key, PMIX_ALLOC_NETWORK_SEC_KEY, PMIX_MAX_KEYLEN)) {
               seckey = PMIX_INFO_TRUE(&requests[n]);
           }
       }

  process:
    /* if they didn't give us a test key, just create one */
    if (NULL == idkey) {
        idkey = "TESTKEY";
    }
    PMIX_CONSTRUCT(&mylist, pmix_list_t);

    /* must include the idkey */
    kv = PMIX_NEW(pmix_kval_t);
    if (NULL == kv) {
        return PMIX_ERR_NOMEM;
    }
    kv->key = strdup(PMIX_ALLOC_NETWORK_ID);
    kv->value = (pmix_value_t*)malloc(sizeof(pmix_value_t));
    if (NULL == kv->value) {
        PMIX_RELEASE(kv);
        return PMIX_ERR_NOMEM;
    }
    kv->value->type = PMIX_STRING;
    kv->value->data.string = strdup(idkey);
    pmix_list_append(&mylist, &kv->super);

    if (seckey) {
        kv = PMIX_NEW(pmix_kval_t);
        if (NULL == kv) {
            return PMIX_ERR_NOMEM;
        }
        kv->key = strdup(PMIX_ALLOC_NETWORK_SEC_KEY);
        kv->value = (pmix_value_t*)malloc(sizeof(pmix_value_t));
        if (NULL == kv->value) {
            PMIX_RELEASE(kv);
            return PMIX_ERR_NOMEM;
        }
        kv->value->type = PMIX_BYTE_OBJECT;
        kv->value->data.bo.bytes = (char*)malloc(sizeof(uint64_t));
        if (NULL == kv->value->data.bo.bytes) {
            PMIX_RELEASE(kv);
            return PMIX_ERR_NOMEM;
        }
        memcpy(kv->value->data.bo.bytes, &unique_key, sizeof(uint64_t));
        kv->value->data.bo.size = sizeof(uint64_t);
        pmix_list_append(&mylist, &kv->super);
    }

    /* find the info on this job, if available */
    job = NULL;
    PMIX_LIST_FOREACH(jptr, &pmix_pnet_globals.jobs, pmix_pnet_job_t) {
        if (0 == strcmp(jptr->nspace, nptr->nspace)) {
            job = jptr;
            break;
        }
    }
    if (NULL != job) {
        pmix_output(0, "ALLOCATE RESOURCES FOR JOB %s", job->nspace);
        for (n=0; (int)n < job->nodes.size; n++) {
            if (NULL == (nd = (pmix_pnet_node_t*)pmix_pointer_array_get_item(&job->nodes, n))) {
                continue;
            }
            lp = NULL;
            PMIX_LIST_FOREACH(lptr, &nd->local_jobs, pmix_pnet_local_procs_t) {
                if (0 == strcmp(job->nspace, lptr->nspace)) {
                    lp = lptr;
                    break;
                }
            }
            if (NULL == lp) {
                pmix_output(0, "\t NODE %s 0 RANKS", nd->name);
            } else {
                pmix_output(0, "\tNODE %s %d RANKS", nd->name, (int)lp->np);
            }
        }
    }

    n = pmix_list_get_size(&mylist);
    if (0 < n) {
        PMIX_CONSTRUCT(&buf, pmix_buffer_t);
        /* pack the number of kvals for ease on the remote end */
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, &buf, &n, 1, PMIX_SIZE);
        /* cycle across the list and pack the kvals */
        while (NULL != (kv = (pmix_kval_t*)pmix_list_remove_first(&mylist))) {
            PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, &buf, kv, 1, PMIX_KVAL);
            PMIX_RELEASE(kv);
            if (PMIX_SUCCESS != rc) {
                PMIX_DESTRUCT(&buf);
                PMIX_LIST_DESTRUCT(&mylist);
                return rc;
            }
        }
        PMIX_LIST_DESTRUCT(&mylist);
        kv = PMIX_NEW(pmix_kval_t);
        kv->key = strdup("pmix-pnet-test-blob");
        kv->value = (pmix_value_t*)malloc(sizeof(pmix_value_t));
        if (NULL == kv->value) {
            PMIX_RELEASE(kv);
            PMIX_DESTRUCT(&buf);
            return PMIX_ERR_NOMEM;
        }
        kv->value->type = PMIX_BYTE_OBJECT;
        PMIX_UNLOAD_BUFFER(&buf, kv->value->data.bo.bytes, kv->value->data.bo.size);
        PMIX_DESTRUCT(&buf);
        pmix_list_append(ilist, &kv->super);
    }

    return PMIX_SUCCESS;
}

static pmix_status_t setup_local_network(pmix_namespace_t *nptr,
                                         pmix_info_t info[],
                                         size_t ninfo)
{
    size_t n, m, nkvals;
    char *nodestring, **nodes;
    pmix_proc_t *procs;
    size_t nprocs;
    pmix_buffer_t bkt;
    int32_t cnt;
    pmix_kval_t *kv;
    pmix_status_t rc;
    pmix_info_t *jinfo, stinfo;
    char *idkey = NULL;

    pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                        "pnet:test:setup_local_network");

    /* get the list of nodes in this job - returns a regex */
    pmix_output(0, "pnet:test setup_local_network NSPACE %s", (NULL == nptr) ? "NULL" : nptr->nspace);
    if (NULL == nptr) {
        return PMIX_SUCCESS;
    }
    pmix_preg.resolve_nodes(nptr->nspace, &nodestring);
    if (NULL == nodestring) {
        return PMIX_SUCCESS;
    }
    pmix_preg.parse_nodes(nodestring, &nodes);  // get an argv array of node names
    pmix_output(0, "pnet:test setup_local_network NODES %s", (NULL == nodes) ? "NULL" : "NON-NULL");
    if (NULL == nodes) {
        free(nodestring);
        return PMIX_SUCCESS;
    }
    for (n=0; NULL != nodes[n]; n++) {
        pmix_output(0, "pnet:test setup_local_network NODE: %s", nodes[n]);
    }

    for (n=0; NULL != nodes[n]; n++) {
    /* get an array of pmix_proc_t containing the names of the procs on that node */
      pmix_preg.resolve_peers(nodes[n], nptr->nspace, &procs, &nprocs);
      if (NULL == procs) {
        continue;
    }
    for (m=0; m < nprocs; m++) {
        pmix_output(0, "pnet:test setup_local_network NODE %s: peer %s:%d", nodes[n], procs[m].nspace, procs[m].rank);
    }
        /* do stuff */
        free(procs);
    }

    if (NULL != info) {
       for (n=0; n < ninfo; n++) {
               /* look for my key */
           if (0 == strncmp(info[n].key, "pmix-pnet-test-blob", PMIX_MAX_KEYLEN)) {
                   /* this macro NULLs and zero's the incoming bo */
               PMIX_LOAD_BUFFER(pmix_globals.mypeer, &bkt,
                                info[n].value.data.bo.bytes,
                                info[n].value.data.bo.size);
                   /* unpack the number of kvals */
               cnt = 1;
               PMIX_BFROPS_UNPACK(rc, pmix_globals.mypeer,
                                  &bkt, &nkvals, &cnt, PMIX_SIZE);
                   /* the data gets stored as a pmix_data_array_t on the provided key */
               PMIX_INFO_CONSTRUCT(&stinfo);
               pmix_strncpy(stinfo.key, idkey, PMIX_MAX_KEYLEN);
               stinfo.value.type = PMIX_DATA_ARRAY;
               PMIX_DATA_ARRAY_CREATE(stinfo.value.data.darray, nkvals, PMIX_INFO);
               jinfo = (pmix_info_t*)stinfo.value.data.darray->array;

                   /* cycle thru the blob and extract the kvals */
               kv = PMIX_NEW(pmix_kval_t);
               cnt = 1;
               PMIX_BFROPS_UNPACK(rc, pmix_globals.mypeer,
                                  &bkt, kv, &cnt, PMIX_KVAL);
               m = 0;
               while (PMIX_SUCCESS == rc) {
                   pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                                       "recvd KEY %s %s", kv->key,
                                       (PMIX_STRING == kv->value->type) ? kv->value->data.string : "NON-STRING");
                       /* xfer the value to the info */
                   pmix_strncpy(jinfo[m].key, kv->key, PMIX_MAX_KEYLEN);
                   PMIX_BFROPS_VALUE_XFER(rc, pmix_globals.mypeer,
                                          &jinfo[m].value, kv->value);
                       /* if this is the ID key, save it */
                   if (NULL == idkey &&
                       0 == strncmp(kv->key, PMIX_ALLOC_NETWORK_ID, PMIX_MAX_KEYLEN)) {
                       idkey = strdup(kv->value->data.string);
                   }
                   ++m;
                   PMIX_RELEASE(kv);
                   kv = PMIX_NEW(pmix_kval_t);
                   cnt = 1;
                   PMIX_BFROPS_UNPACK(rc, pmix_globals.mypeer,
                                      &bkt, kv, &cnt, PMIX_KVAL);
               }
                   /* restore the incoming data */
               info[n].value.data.bo.bytes = bkt.base_ptr;
               info[n].value.data.bo.size = bkt.bytes_used;
               bkt.base_ptr = NULL;
               bkt.bytes_used = 0;

                   /* if they didn't include a network ID, then this is an error */
               if (NULL == idkey) {
                   PMIX_INFO_FREE(jinfo, nkvals);
                   return PMIX_ERR_BAD_PARAM;
               }
               /* cache the info on the job */
               PMIX_GDS_CACHE_JOB_INFO(rc, pmix_globals.mypeer, nptr,
                                       &stinfo, 1);
               PMIX_INFO_DESTRUCT(&stinfo);
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
    pmix_cb_t cb;
    pmix_status_t rc;
    pmix_kval_t *kv;
    uint16_t localrank;

    /* we don't have to actually do anything here, so we just test
     * to ensure we can find some required proc-specific data */
    PMIX_CONSTRUCT(&cb, pmix_cb_t);

    cb.key = strdup(PMIX_LOCAL_RANK);
    /* this data isn't going anywhere, so we don't require a copy */
    cb.copy = false;
    /* scope is irrelevant as the info we seek must be local */
    cb.scope = PMIX_SCOPE_UNDEF;
    /* ask for the value for the given proc */
    cb.proc = (pmix_proc_t*)proc;

    PMIX_GDS_FETCH_KV(rc, pmix_globals.mypeer, &cb);
    if (PMIX_SUCCESS != rc) {
        if (PMIX_ERR_INVALID_NAMESPACE != rc) {
            PMIX_ERROR_LOG(rc);
        }
        PMIX_DESTRUCT(&cb);
        return rc;
    }
    /* should just be the one value on the list */
    if (1 != pmix_list_get_size(&cb.kvs)) {
        PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
        PMIX_DESTRUCT(&cb);
        return PMIX_ERR_BAD_PARAM;
    }
    kv = (pmix_kval_t*)pmix_list_get_first(&cb.kvs);
    if (PMIX_UINT16 != kv->value->type) {
        PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
        PMIX_DESTRUCT(&cb);
        return PMIX_ERR_BAD_PARAM;
    }
    localrank = kv->value->data.uint16;

    pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                        "pnet:test LOCAL RANK FOR PROC %s: %d",
                        PMIX_NAME_PRINT(proc), (int)localrank);

    PMIX_DESTRUCT(&cb);
    return PMIX_SUCCESS;
}

static void child_finalized(pmix_proc_t *peer)
{
    pmix_output(0, "pnet:test CHILD %s:%d FINALIZED",
                peer->nspace, peer->rank);
}

static void local_app_finalized(pmix_namespace_t *nptr)
{
    pmix_output(0, "pnet:test NSPACE %s LOCALLY FINALIZED", nptr->nspace);
}

static void deregister_nspace(pmix_namespace_t *nptr)
{
    pmix_output(0, "pnet:test DEREGISTER NSPACE %s", nptr->nspace);
}

static pmix_status_t collect_inventory(pmix_info_t directives[], size_t ndirs,
                                       pmix_inventory_cbfunc_t cbfunc, void *cbdata)
{
    pmix_output(0, "pnet:test COLLECT INVENTORY");
    return PMIX_ERR_NOT_SUPPORTED;
}

static pmix_status_t deliver_inventory(pmix_info_t info[], size_t ninfo,
                                       pmix_info_t directives[], size_t ndirs,
                                       pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                        "pnet:test deliver inventory");

    return PMIX_ERR_NOT_SUPPORTED;
}

static pmix_status_t register_fabric(pmix_fabric_t *fabric,
                                     const pmix_info_t directives[],
                                     size_t ndirs)
{
    pmix_pnet_fabric_t *ft;
    pnet_plane_t *p, *p2;
    char *pln = NULL;
    size_t n;

    if (NULL == fabric) {
        return PMIX_ERR_BAD_PARAM;
    }
    /* see what plane they wanted */
    for (n=0; n < ndirs; n++) {
        if (PMIX_CHECK_KEY(&directives[n], PMIX_NETWORK_PLANE)) {
            pln = directives[n].value.data.string;
            break;
        }
    }
    if (NULL == pln) {
        /* just use the first on our list */
        p = (pnet_plane_t*)pmix_list_get_first(&myplanes);
    } else {
        /* find it */
        p = NULL;
        PMIX_LIST_FOREACH(p2, &myplanes, pnet_plane_t) {
            if (0 == strcmp(pln, p2->name)) {
                p = p2;
                break;
            }
        }
    }
    if (NULL == p) {
        return PMIX_ERR_NOT_FOUND;
    }

    ft = PMIX_NEW(pmix_pnet_fabric_t);
    ft->module = &pmix_test_module;
    ft->payload = p;

    /* pass to the user-level object */
    fabric->module = ft;
    fabric->revision = p->revision;

    return PMIX_SUCCESS;
}

static pmix_status_t deregister_fabric(pmix_fabric_t *fabric)
{
    fabric->module = NULL;
    return PMIX_SUCCESS;
}

static pmix_status_t get_num_verts(pmix_fabric_t *fabric, uint32_t *nverts)
{
    pmix_pnet_fabric_t *ft = (pmix_pnet_fabric_t*)fabric->module;
    pnet_plane_t *p = (pnet_plane_t*)ft->payload;
    int rc;

    rc = pmix_atomic_trylock(&p->atomlock);
    if (0 != rc) {
        return PMIX_ERR_RESOURCE_BUSY;
    }
    /* if fabric data has been updated since the last time
     * this was accessed, let them know */
    if (fabric->revision != p->revision) {
        /* update the revision */
        fabric->revision = p->revision;
        pmix_atomic_unlock(&p->atomlock);
        return PMIX_FABRIC_UPDATED;
    }

    /* this component only looks at node-resident NICs as
     * we assume switch-to-switch is done over dedicated
     * ports in a ring topology */
    *nverts = p->nverts;
    pmix_atomic_unlock(&p->atomlock);
    return PMIX_SUCCESS;
}

static pmix_status_t get_cost(pmix_fabric_t *fabric,
                              uint32_t src, uint32_t dest,
                              uint16_t *cost)
{
    pmix_pnet_fabric_t *ft = (pmix_pnet_fabric_t*)fabric->module;
    pnet_plane_t *p = (pnet_plane_t*)ft->payload;
    int rc;

    rc = pmix_atomic_trylock(&p->atomlock);
    if (0 != rc) {
        return PMIX_ERR_RESOURCE_BUSY;
    }
    /* if fabric data has been updated since the last time
     * this was accessed, let them know */
    if (fabric->revision != p->revision) {
        /* update the revision */
        fabric->revision = p->revision;
        pmix_atomic_unlock(&p->atomlock);
        return PMIX_FABRIC_UPDATED;
    }
    if (src >= p->nverts || dest >= p->nverts) {
        pmix_atomic_unlock(&p->atomlock);
        return PMIX_ERR_BAD_PARAM;
    }

    *cost = p->costmatrix[src][dest];
    pmix_atomic_unlock(&p->atomlock);
    return PMIX_SUCCESS;
}

static pmix_status_t get_vertex(pmix_fabric_t *fabric,
                                uint32_t i,
                                pmix_value_t *identifier,
                                char **nodename)
{
    pmix_pnet_fabric_t *ft = (pmix_pnet_fabric_t*)fabric->module;
    pnet_plane_t *p = (pnet_plane_t*)ft->payload;
    pnet_nic_t *nic;
    pnet_plane_t *pln;
    pnet_switch_t *sw;
    pnet_node_t *node;
    pmix_info_t *info;
    size_t n;
    int rc;

    if (NULL == p) {
        return PMIX_ERR_NOT_SUPPORTED;
    }

    rc = pmix_atomic_trylock(&p->atomlock);
    if (0 != rc) {
        return PMIX_ERR_RESOURCE_BUSY;
    }
    /* if fabric data has been updated since the last time
     * this was accessed, let them know */
    if (fabric->revision != p->revision) {
        /* update the revision */
        fabric->revision = p->revision;
        pmix_atomic_unlock(&p->atomlock);
        return PMIX_FABRIC_UPDATED;
    }
    if (i >= p->nverts) {
        pmix_atomic_unlock(&p->atomlock);
        return PMIX_ERR_BAD_PARAM;
    }

    /* find NIC that corresponds to this index */
    nic = (pnet_nic_t*)pmix_pointer_array_get_item(&mynics, i);
    node = (pnet_node_t*)nic->node;
    *nodename = strdup(node->name);
    /* the value we pass back will be a data array containing
     * info on the switch this NIC is connected to and the
     * plane it is on */
    identifier->type = PMIX_DATA_ARRAY;
    PMIX_DATA_ARRAY_CREATE(identifier->data.darray, 3, PMIX_INFO);
    info = (pmix_info_t*)identifier->data.darray->array;
    n = 0;
    pln = (pnet_plane_t*)nic->plane;
    PMIX_INFO_LOAD(&info[n], PMIX_NETWORK_PLANE, pln->name, PMIX_STRING);
    ++n;
    sw = (pnet_switch_t*)nic->s;
    PMIX_INFO_LOAD(&info[n], PMIX_NETWORK_SWITCH, sw->name, PMIX_STRING);
    ++n;
    PMIX_INFO_LOAD(&info[n], PMIX_NETWORK_NIC, nic->name, PMIX_STRING);

    pmix_atomic_unlock(&p->atomlock);
    return PMIX_SUCCESS;
}

static pmix_status_t get_index(pmix_fabric_t *fabric,
                               pmix_value_t *identifier,
                               uint32_t *i,
                               char **nodename)
{
    pmix_pnet_fabric_t *ft = (pmix_pnet_fabric_t*)fabric->module;
    pnet_plane_t *p = (pnet_plane_t*)ft->payload;
    pnet_node_t *node;
    pnet_nic_t *nic;
    int rc, m;
    pmix_status_t ret;
    pmix_info_t *info;
    char *nc=NULL;
    size_t n;

    if (NULL == p) {
        return PMIX_ERR_NOT_SUPPORTED;
    }

    rc = pmix_atomic_trylock(&p->atomlock);
    if (0 != rc) {
        return PMIX_ERR_RESOURCE_BUSY;
    }
    /* if fabric data has been updated since the last time
     * this was accessed, let them know */
    if (fabric->revision != p->revision) {
        /* update the revision */
        fabric->revision = p->revision;
        pmix_atomic_unlock(&p->atomlock);
        return PMIX_FABRIC_UPDATED;
    }

    /* see what they gave us */
    if (PMIX_DATA_ARRAY == identifier->type) {
        if (PMIX_INFO != identifier->data.darray->type) {
            ret = PMIX_ERR_BAD_PARAM;
            goto cleanup;
        }
        info = (pmix_info_t*)identifier->data.darray->array;
        for (n=0; n < identifier->data.darray->size; n++) {
            if (PMIX_CHECK_KEY(&info[n], PMIX_NETWORK_NIC)) {
                nc = info[n].value.data.string;
            }
        }
        if (NULL == nc) {
            ret = PMIX_ERR_BAD_PARAM;
            goto cleanup;
        }
        /* find the NIC */
        for (m=0; m < mynics.size; m++) {
            nic = (pnet_nic_t*)pmix_pointer_array_get_item(&mynics, m);
            if (NULL == nic) {
                continue;
            }
            if (0 == strcmp(nc, nic->name)) {
                *i = m;
                node = (pnet_node_t*)nic->node;
                *nodename = strdup(node->name);
                ret = PMIX_SUCCESS;
                goto cleanup;
            }
        }
        ret = PMIX_ERR_NOT_FOUND;
    } else if (PMIX_UINT32 == identifier->type) {
        /* they gave us the vertex number - in our case,
         * that is the NIC id */
        *i = identifier->data.uint32;
        nic = (pnet_nic_t*)pmix_pointer_array_get_item(&mynics, *i);
        node = (pnet_node_t*)nic->node;
        *nodename = strdup(node->name);
        ret = PMIX_SUCCESS;
    } else {
        ret = PMIX_ERR_BAD_PARAM;
    }

  cleanup:
    pmix_atomic_unlock(&p->atomlock);
    return ret;
}
