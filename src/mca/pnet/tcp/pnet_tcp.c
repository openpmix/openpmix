/*
 * Copyright (c) 2018-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2018      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 *
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
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
#ifdef HAVE_SYS_SOCKET_H
#    include <sys/socket.h>
#endif
#ifdef HAVE_NETINET_IN_H
#    include <netinet/in.h>
#endif
#ifdef HAVE_ARPA_INET_H
#    include <arpa/inet.h>
#endif
#include <time.h>

#include "pmix_common.h"

#include "src/class/pmix_list.h"
#include "src/include/pmix_globals.h"
#include "src/include/pmix_socket_errno.h"
#include "src/mca/preg/preg.h"
#include "src/util/pmix_alfg.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_printf.h"
#include "src/util/pmix_parse_options.h"
#include "src/util/pmix_if.h"
#include "src/util/pmix_environ.h"

#include "pnet_tcp.h"
#include "src/mca/pnet/base/base.h"

#define PMIX_TCP_SETUP_APP_KEY "pmix.tcp.setup.app.key"
#define PMIX_TCP_INVENTORY_KEY "pmix.tcp.inventory"

static pmix_status_t tcp_init(void);
static void tcp_finalize(void);
static pmix_status_t allocate(pmix_namespace_t *nptr, pmix_info_t info[], size_t ninfo,
                              pmix_list_t *ilist);
static pmix_status_t setup_local_network(pmix_nspace_env_cache_t *nptr, pmix_info_t info[],
                                         size_t ninfo);
static void child_finalized(pmix_proc_t *peer);
static void local_app_finalized(pmix_namespace_t *nptr);
static void deregister_nspace(pmix_namespace_t *nptr);
static pmix_status_t collect_inventory(pmix_info_t directives[], size_t ndirs,
                                       pmix_list_t *inventory);
static pmix_status_t deliver_inventory(pmix_info_t info[], size_t ninfo, pmix_info_t directives[],
                                       size_t ndirs);

pmix_pnet_module_t pmix_pnet_tcp_module = {.name = "tcp",
                                      .init = tcp_init,
                                      .finalize = tcp_finalize,
                                      .allocate = allocate,
                                      .setup_local_network = setup_local_network,
                                      .child_finalized = child_finalized,
                                      .local_app_finalized = local_app_finalized,
                                      .deregister_nspace = deregister_nspace,
                                      .collect_inventory = collect_inventory,
                                      .deliver_inventory = deliver_inventory};

typedef struct {
    pmix_list_item_t super;
    char *device;
    char *address;
} tcp_device_t;

/* local tracker objects */
typedef struct {
    pmix_list_item_t super;
    pmix_list_t devices;
    char *type;
    char *plane;
    char **ports;
    size_t nports;
} tcp_available_ports_t;

typedef struct {
    pmix_list_item_t super;
    char *nspace;
    char **ports;
    tcp_available_ports_t *src; // source of the allocated ports
} tcp_port_tracker_t;

/* the framework used to archive delivered inventory in a global
 * node/resource tree (pmix_pnet_globals.nodes). That store was
 * removed, so this component keeps its own equivalent tree for the
 * inventory it is handed - a node holds a list of per-type resources,
 * each of which holds the reported ports/devices for that type */
typedef struct {
    pmix_list_item_t super;
    char *name;
    pmix_list_t resources;
} tcp_resource_t;

typedef struct {
    pmix_list_item_t super;
    char *name;
    pmix_list_t resources;
} tcp_node_t;

static pmix_list_t allocations, available, nodes;
static pmix_status_t process_request(pmix_namespace_t *nptr, char *idkey, int ports_per_node,
                                     tcp_port_tracker_t *trk, pmix_list_t *ilist);
static tcp_port_tracker_t *new_tracker(pmix_namespace_t *nptr, tcp_available_ports_t *avail);

static void dcon(tcp_device_t *p)
{
    p->device = NULL;
    p->address = NULL;
}
static void ddes(tcp_device_t *p)
{
    if (NULL != p->device) {
        free(p->device);
    }
    if (NULL != p->address) {
        free(p->address);
    }
}
static PMIX_CLASS_INSTANCE(tcp_device_t, pmix_list_item_t, dcon, ddes);

static void tacon(tcp_available_ports_t *p)
{
    PMIX_CONSTRUCT(&p->devices, pmix_list_t);
    p->type = NULL;
    p->plane = NULL;
    p->ports = NULL;
    p->nports = 0;
}
static void tades(tcp_available_ports_t *p)
{
    PMIX_LIST_DESTRUCT(&p->devices);
    if (NULL != p->type) {
        free(p->type);
    }
    if (NULL != p->plane) {
        free(p->plane);
    }
    if (NULL != p->ports) {
        PMIx_Argv_free(p->ports);
    }
}
static PMIX_CLASS_INSTANCE(tcp_available_ports_t, pmix_list_item_t, tacon, tades);

static void ttcon(tcp_port_tracker_t *p)
{
    p->nspace = NULL;
    p->ports = NULL;
    p->src = NULL;
}
static void ttdes(tcp_port_tracker_t *p)
{
    size_t n, m, mstart;

    if (NULL != p->nspace) {
        free(p->nspace);
    }
    if (NULL != p->src) {
        if (NULL != p->ports) {
            mstart = 0;
            for (n = 0; NULL != p->ports[n]; n++) {
                /* find an empty position */
                for (m = mstart; m < p->src->nports; m++) {
                    if (NULL == p->src->ports[m]) {
                        p->src->ports[m] = strdup(p->ports[n]);
                        mstart = m + 1;
                        break;
                    }
                }
            }
            PMIx_Argv_free(p->ports);
        }
        PMIX_RELEASE(p->src); // maintain accounting
    } else if (NULL != p->ports) {
        PMIx_Argv_free(p->ports);
    }
}
static PMIX_CLASS_INSTANCE(tcp_port_tracker_t, pmix_list_item_t, ttcon, ttdes);

static void rescon(tcp_resource_t *p)
{
    p->name = NULL;
    PMIX_CONSTRUCT(&p->resources, pmix_list_t);
}
static void resdes(tcp_resource_t *p)
{
    if (NULL != p->name) {
        free(p->name);
    }
    PMIX_LIST_DESTRUCT(&p->resources);
}
static PMIX_CLASS_INSTANCE(tcp_resource_t, pmix_list_item_t, rescon, resdes);

static void ndcon(tcp_node_t *p)
{
    p->name = NULL;
    PMIX_CONSTRUCT(&p->resources, pmix_list_t);
}
static void nddes(tcp_node_t *p)
{
    if (NULL != p->name) {
        free(p->name);
    }
    PMIX_LIST_DESTRUCT(&p->resources);
}
static PMIX_CLASS_INSTANCE(tcp_node_t, pmix_list_item_t, ndcon, nddes);

static pmix_status_t tcp_init(void)
{
    tcp_available_ports_t *trk = NULL;
    char *p, **grps = NULL;
    size_t n;
    pmix_status_t rc;

    pmix_output_verbose(2, pmix_pnet_base_framework.framework_output, "pnet: tcp init");

    /* the inventory cache is used regardless of role, so set it up
     * before we check whether we are the gateway */
    PMIX_CONSTRUCT(&nodes, pmix_list_t);

    /* if we are not the "gateway", then there is nothing
     * for us to do */
    if (!PMIX_PEER_IS_GATEWAY(pmix_globals.mypeer)) {
        return PMIX_SUCCESS;
    }

    PMIX_CONSTRUCT(&allocations, pmix_list_t);
    PMIX_CONSTRUCT(&available, pmix_list_t);

    /* if we have no static ports, then we don't have
     * anything to manage. However, we cannot just disqualify
     * ourselves as we may still need to provide inventory.
     *
     * NOTE: need to check inventory in addition to MCA param as
     * the inventory may have reported back static ports */
    if (NULL == pmix_mca_pnet_tcp_component.static_ports) {
        return PMIX_SUCCESS;
    }

    /* split on semi-colons */
    grps = PMIx_Argv_split(pmix_mca_pnet_tcp_component.static_ports, ';');
    for (n = 0; NULL != grps[n]; n++) {
        trk = PMIX_NEW(tcp_available_ports_t);
        if (NULL == trk) {
            rc = PMIX_ERR_NOMEM;
            goto error;
        }
        /* there must be at least one colon */
        if (NULL == (p = strrchr(grps[n], ':'))) {
            rc = PMIX_ERR_BAD_PARAM;
            goto error;
        }
        /* extract the ports */
        *p = '\0';
        ++p;
        pmix_util_parse_range_options(p, &trk->ports);
        trk->nports = PMIx_Argv_count(trk->ports);
        /* see if they provided a plane */
        if (NULL != (p = strchr(grps[n], ':'))) {
            /* yep - save the plane */
            *p = '\0';
            ++p;
            trk->plane = strdup(p);
        }
        /* the type is just what is left at the front - every search of
         * the available list compares this string, so an entry carrying
         * a NULL one must never be published */
        trk->type = strdup(grps[n]);
        if (NULL == trk->type) {
            rc = PMIX_ERR_NOMEM;
            goto error;
        }
        pmix_output_verbose(2, pmix_pnet_base_framework.framework_output, "TYPE: %s PLANE %s",
                            trk->type, (NULL == trk->plane) ? "NULL" : trk->plane);
        pmix_list_append(&available, &trk->super);
        trk = NULL; // the list owns it now
    }
    PMIx_Argv_free(grps);

    return PMIX_SUCCESS;

error:
    /* a module whose init fails is dropped by the framework's select
     * without ever being added to the active list, so our finalize will
     * not be called - release everything we constructed here */
    if (NULL != trk) {
        PMIX_RELEASE(trk);
    }
    PMIx_Argv_free(grps);
    PMIX_LIST_DESTRUCT(&allocations);
    PMIX_LIST_DESTRUCT(&available);
    PMIX_LIST_DESTRUCT(&nodes);
    return rc;
}

static void tcp_finalize(void)
{
    pmix_output_verbose(2, pmix_pnet_base_framework.framework_output, "pnet: tcp finalize");
    if (PMIX_PEER_IS_GATEWAY(pmix_globals.mypeer)) {
        PMIX_LIST_DESTRUCT(&allocations);
        PMIX_LIST_DESTRUCT(&available);
    }
    PMIX_LIST_DESTRUCT(&nodes);
}

/* some network users may want to encrypt their communications
 * as a means of securing them, or include a token in their
 * messaging headers for some minimal level of security. This
 * is far from perfect, but is provided to illustrate how it
 * can be done. The resulting info is placed into the
 * app_context's env array so it will automatically be pushed
 * into the environment of every MPI process when launched.
 *
 * In a more perfect world, there would be some privileged place
 * to store the crypto key and the encryption would occur
 * in a non-visible driver - but we don't have a mechanism
 * for doing so.
 */

static inline void generate_key(uint64_t *unique_key)
{
    static pmix_rng_buff_t rng;
    static bool seeded = false;
    uint32_t hi, lo;

    if (!seeded) {
        /* seed once and keep drawing from that stream. Re-seeding from
         * the clock on every call would hand two jobs allocated within
         * the same second the identical "unique" key, which is the one
         * thing this is supposed to avoid.
         *
         * pmix_srand() wants a 32-bit seed, so fold the full width of
         * the time_t into 32 bits rather than silently truncating it -
         * that lets every bit of the clock contribute and remains
         * well-defined whether time_t is 32 or 64 bits wide. Mixing in
         * our pid separates two servers started in the same second */
        uint64_t now = (uint64_t) time(NULL);
        uint32_t seed = (uint32_t) now ^ (uint32_t) (now >> 32) ^ (uint32_t) getpid();
        pmix_srand(&rng, seed);
        seeded = true;
    }
    /* pmix_rand returns 32 bits at a time, so fill each half of the
     * key separately rather than leaving the top half zero */
    hi = pmix_rand(&rng);
    lo = pmix_rand(&rng);
    unique_key[0] = ((uint64_t) hi << 32) | (uint64_t) lo;
    hi = pmix_rand(&rng);
    lo = pmix_rand(&rng);
    unique_key[1] = ((uint64_t) hi << 32) | (uint64_t) lo;
}

/* when allocate is called, we look at our table of available static
 * ports and carve out the number the request asked for, tracking the
 * assignment against the namespace so the ports come back when the job
 * is deregistered. Every job therefore draws from the same pool and
 * cannot collide with a job that is still running.
 *
 * NOTE: this implementation is offered as an example that can
 * undoubtedly be vastly improved/optimized. A real one would want to
 * take the number of processes per node into account rather than
 * requiring the caller to name a count */

static pmix_status_t allocate(pmix_namespace_t *nptr, pmix_info_t info[], size_t ninfo,
                              pmix_list_t *ilist)
{
    uint64_t unique_key[2];
    size_t n, nreqs = 0;
    int ports_per_node = 0;
    pmix_kval_t *kv;
    pmix_status_t rc;
    pmix_info_t *requests = NULL;
    char **reqs, *cptr;
    bool allocated = false, seckey = false, envars = false;
    tcp_port_tracker_t *trk;
    tcp_available_ports_t *avail, *aptr;
    pmix_list_t mylist;
    pmix_buffer_t buf;
    char *type = NULL, *plane = NULL, *idkey = NULL;

    pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                        "pnet:tcp:allocate for nspace %s", nptr->nspace);

    /* if I am not the gateway, then ignore this call - should never
     * happen, but check to be safe */
    if (!PMIX_PEER_IS_GATEWAY(pmix_globals.mypeer)) {
        return PMIX_SUCCESS;
    }

    if (NULL == info) {
        return PMIX_ERR_TAKE_NEXT_OPTION;
    }

    /* check directives to see if a crypto key and/or
     * network resource allocations requested */
    for (n = 0; n < ninfo; n++) {
        if (PMIX_CHECK_KEY(&info[n], PMIX_SETUP_APP_ENVARS)
            || PMIX_CHECK_KEY(&info[n], PMIX_SETUP_APP_ALL)) {
            envars = PMIX_INFO_TRUE(&info[n]);
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_ALLOC_FABRIC)) {
            /* this info key includes an array of pmix_info_t, each providing
             * a key (that is to be used as the key for the allocated ports) and
             * a number of ports to allocate for that key */
            if (PMIX_DATA_ARRAY != info[n].value.type || NULL == info[n].value.data.darray
                || PMIX_INFO != info[n].value.data.darray->type
                || NULL == info[n].value.data.darray->array) {
                /* they made an error */
                PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
                return PMIX_ERR_BAD_PARAM;
            }
            requests = (pmix_info_t *) info[n].value.data.darray->array;
            nreqs = info[n].value.data.darray->size;
        }
    }

    if (envars) {
        if (NULL != pmix_mca_pnet_tcp_component.include) {
            pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                                "pnet: tcp harvesting envars %s excluding %s",
                                (NULL == pmix_mca_pnet_tcp_component.incparms)
                                    ? "NONE"
                                    : pmix_mca_pnet_tcp_component.incparms,
                                (NULL == pmix_mca_pnet_tcp_component.excparms)
                                    ? "NONE"
                                    : pmix_mca_pnet_tcp_component.excparms);
            rc = pmix_util_harvest_envars(pmix_mca_pnet_tcp_component.include,
                                          pmix_mca_pnet_tcp_component.exclude, ilist);
            if (PMIX_SUCCESS != rc) {
                return rc;
            }
        }
    }

    if (NULL == requests) {
        return PMIX_ERR_TAKE_NEXT_OPTION;
    }

    pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                        "pnet:tcp:allocate alloc_network for nspace %s", nptr->nspace);
    /* cycle thru the provided array and see if this refers to
     * tcp/udp-based resources - there is no required ordering
     * of the keys, so just have to do a search */
    for (n = 0; n < nreqs; n++) {
        if (0 == strncasecmp(requests[n].key, PMIX_ALLOC_FABRIC_TYPE, PMIX_MAX_KEYLEN)) {
            /* check for bozo error */
            if (PMIX_STRING != requests[n].value.type || NULL == requests[n].value.data.string) {
                PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
                return PMIX_ERR_BAD_PARAM;
            }
            type = requests[n].value.data.string;
        } else if (0 == strncasecmp(requests[n].key, PMIX_ALLOC_FABRIC_PLANE, PMIX_MAX_KEYLEN)) {
            /* check for bozo error */
            if (PMIX_STRING != requests[n].value.type || NULL == requests[n].value.data.string) {
                PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
                return PMIX_ERR_BAD_PARAM;
            }
            plane = requests[n].value.data.string;
        } else if (0 == strncasecmp(requests[n].key, PMIX_ALLOC_FABRIC_ENDPTS, PMIX_MAX_KEYLEN)) {
            rc = PMIx_Value_get_number(&requests[n].value, &ports_per_node, PMIX_INT);
            if (PMIX_SUCCESS != rc) {
                return rc;
            }
        } else if (0 == strncmp(requests[n].key, PMIX_ALLOC_FABRIC_ID, PMIX_MAX_KEYLEN)) {
            /* check for bozo error */
            if (PMIX_STRING != requests[n].value.type || NULL == requests[n].value.data.string) {
                PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
                return PMIX_ERR_BAD_PARAM;
            }
            idkey = requests[n].value.data.string;
        } else if (0 == strncasecmp(requests[n].key, PMIX_ALLOC_FABRIC_SEC_KEY, PMIX_MAX_KEYLEN)) {
            seckey = PMIX_INFO_TRUE(&requests[n]);
        }
    }

    /* we at least require an attribute key for the response */
    if (NULL == idkey) {
        return PMIX_ERR_BAD_PARAM;
    }

    PMIX_CONSTRUCT(&mylist, pmix_list_t);
    /* must include the idkey */
    kv = PMIX_NEW(pmix_kval_t);
    if (NULL == kv) {
        return PMIX_ERR_NOMEM;
    }
    kv->key = strdup(PMIX_ALLOC_FABRIC_ID);
    kv->value = (pmix_value_t *) calloc(1, sizeof(pmix_value_t));
    if (NULL == kv->value) {
        PMIX_RELEASE(kv);
        return PMIX_ERR_NOMEM;
    }
    kv->value->type = PMIX_STRING;
    kv->value->data.string = strdup(idkey);
    pmix_list_append(&mylist, &kv->super);

    /* note that they might not provide
     * the network type (letting it fall to a default component
     * based on priority), and they are not required to provide
     * a plane. In addition, they are allowed to simply request
     * a network security key without asking for endpts */

    if (NULL != type && 0 == ports_per_node) {
        /* they named a fabric type but asked for no endpoints. As the
         * note above says, that is allowed - a caller may want nothing
         * but a security key - so there is simply nothing to allocate
         * here. Handing this to process_request instead would have it
         * decline with an error that aborts the whole framework fan-out */
        pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                            "pnet:tcp:allocate no endpoints requested for nspace %s",
                            nptr->nspace);
    } else if (NULL != type) {
        /* if it is tcp or udp, then this is something we should process */
        if (0 == strcasecmp(type, "tcp")) {
            pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                                "pnet:tcp:allocate allocating TCP ports for nspace %s",
                                nptr->nspace);
            /* do we have static tcp ports? */
            avail = NULL;
            PMIX_LIST_FOREACH (aptr, &available, tcp_available_ports_t) {
                if (0 == strcmp(aptr->type, "tcp")) {
                    /* if they specified a plane, then require it */
                    if (NULL != plane && (NULL == aptr->plane || 0 != strcmp(aptr->plane, plane))) {
                        continue;
                    }
                    avail = aptr;
                    break;
                }
            }
            /* nope - they asked for something that we cannot do */
            if (NULL == avail) {
                PMIX_LIST_DESTRUCT(&mylist);
                return PMIX_ERR_NOT_AVAILABLE;
            }
            /* setup to track the assignment */
            trk = new_tracker(nptr, avail);
            if (NULL == trk) {
                PMIX_LIST_DESTRUCT(&mylist);
                return PMIX_ERR_NOMEM;
            }
            rc = process_request(nptr, idkey, ports_per_node, trk, &mylist);
            if (PMIX_SUCCESS != rc) {
                /* return the allocated ports */
                pmix_list_remove_item(&allocations, &trk->super);
                PMIX_RELEASE(trk);
                PMIX_LIST_DESTRUCT(&mylist);
                return rc;
            }
            allocated = true;

        } else if (0 == strcasecmp(type, "udp")) {
            pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                                "pnet:tcp:allocate allocating UDP ports for nspace %s",
                                nptr->nspace);
            /* do we have static udp ports? */
            avail = NULL;
            PMIX_LIST_FOREACH (aptr, &available, tcp_available_ports_t) {
                if (0 == strcmp(aptr->type, "udp")) {
                    /* if they specified a plane, then require it */
                    if (NULL != plane && (NULL == aptr->plane || 0 != strcmp(aptr->plane, plane))) {
                        continue;
                    }
                    avail = aptr;
                    break;
                }
            }
            /* nope - they asked for something that we cannot do */
            if (NULL == avail) {
                PMIX_LIST_DESTRUCT(&mylist);
                return PMIX_ERR_NOT_AVAILABLE;
            }
            /* setup to track the assignment */
            trk = new_tracker(nptr, avail);
            if (NULL == trk) {
                PMIX_LIST_DESTRUCT(&mylist);
                return PMIX_ERR_NOMEM;
            }
            rc = process_request(nptr, idkey, ports_per_node, trk, &mylist);
            if (PMIX_SUCCESS != rc) {
                /* return the allocated ports */
                pmix_list_remove_item(&allocations, &trk->super);
                PMIX_RELEASE(trk);
                PMIX_LIST_DESTRUCT(&mylist);
                return rc;
            }
            allocated = true;
        } else {
            /* unsupported type */
            pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                                "pnet:tcp:allocate unsupported type %s for nspace %s", type,
                                nptr->nspace);
            PMIX_LIST_DESTRUCT(&mylist);
            return PMIX_ERR_TAKE_NEXT_OPTION;
        }

    } else {
        if (NULL != plane) {
            /* if they didn't specify a type, but they did specify a plane, we can
             * see if that is a plane we recognize */
            PMIX_LIST_FOREACH (aptr, &available, tcp_available_ports_t) {
                /* an entry that was configured without a plane cannot
                 * match a requested one */
                if (NULL == aptr->plane || 0 != strcmp(aptr->plane, plane)) {
                    continue;
                }
                /* setup to track the assignment */
                trk = new_tracker(nptr, aptr);
                if (NULL == trk) {
                    PMIX_LIST_DESTRUCT(&mylist);
                    return PMIX_ERR_NOMEM;
                }
                rc = process_request(nptr, idkey, ports_per_node, trk, &mylist);
                if (PMIX_SUCCESS != rc) {
                    /* return the allocated ports */
                    pmix_list_remove_item(&allocations, &trk->super);
                    PMIX_RELEASE(trk);
                    PMIX_LIST_DESTRUCT(&mylist);
                    return rc;
                }
                allocated = true;
                break;
            }
        } else {
            /* if they didn't specify either type or plane, then we got here because
             * nobody of a higher priority could act as a default transport - so try
             * to provide something here, starting by looking at any provided setting */
            if (NULL != pmix_mca_pnet_tcp_component.default_request) {
                pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                                    "pnet:tcp:allocate allocating default ports %s for nspace %s",
                                    pmix_mca_pnet_tcp_component.default_request, nptr->nspace);
                reqs = PMIx_Argv_split(pmix_mca_pnet_tcp_component.default_request, ';');
                for (n = 0; NULL != reqs[n]; n++) {
                    /* if there is no colon, then it is just
                     * a number of ports to use */
                    type = NULL;
                    plane = NULL;
                    if (NULL == (cptr = strrchr(reqs[n], ':'))) {
                        /* pmix_list_get_first hands back the list's own
                         * sentinel - not NULL - when the list is empty, so
                         * the emptiness has to be tested separately or we
                         * would treat that sentinel as a port pool */
                        if (pmix_list_is_empty(&available)) {
                            continue;
                        }
                        avail = (tcp_available_ports_t *) pmix_list_get_first(&available);
                    } else {
                        *cptr = '\0';
                        ++cptr;
                        ports_per_node = strtoul(cptr, NULL, 10);
                        /* the port count is now split off, so anything left
                         * before a second colon is the type and what follows
                         * it is the plane */
                        if (NULL != (cptr = strrchr(reqs[n], ':'))) {
                            *cptr = '\0';
                            ++cptr;
                            plane = cptr;
                        }
                        type = reqs[n];
                        avail = NULL;
                        PMIX_LIST_FOREACH (aptr, &available, tcp_available_ports_t) {
                            if (0 == strcmp(aptr->type, type)) {
                                /* if they specified a plane, then require it */
                                if (NULL != plane
                                    && (NULL == aptr->plane || 0 != strcmp(aptr->plane, plane))) {
                                    continue;
                                }
                                avail = aptr;
                                break;
                            }
                        }
                        /* if we didn't find it, that isn't an error - just ignore */
                        if (NULL == avail) {
                            continue;
                        }
                    }
                    /* setup to track the assignment */
                    trk = new_tracker(nptr, avail);
                    if (NULL == trk) {
                        PMIx_Argv_free(reqs);
                        PMIX_LIST_DESTRUCT(&mylist);
                        return PMIX_ERR_NOMEM;
                    }
                    rc = process_request(nptr, idkey, ports_per_node, trk, &mylist);
                    if (PMIX_SUCCESS != rc) {
                        /* return the allocated ports */
                        pmix_list_remove_item(&allocations, &trk->super);
                        PMIX_RELEASE(trk);
                        PMIx_Argv_free(reqs);
                        PMIX_LIST_DESTRUCT(&mylist);
                        return rc;
                    }
                    allocated = true;
                }
                PMIx_Argv_free(reqs);
            } else {
                pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                                    "pnet:tcp:allocate allocating %d ports/node for nspace %s",
                                    ports_per_node, nptr->nspace);
                if (0 == ports_per_node) {
                    /* nothing to allocate */
                    PMIX_LIST_DESTRUCT(&mylist);
                    return PMIX_ERR_TAKE_NEXT_OPTION;
                }
                /* an empty list yields the sentinel rather than NULL */
                if (!pmix_list_is_empty(&available)) {
                    avail = (tcp_available_ports_t *) pmix_list_get_first(&available);
                    /* setup to track the assignment */
                    trk = new_tracker(nptr, avail);
                    if (NULL == trk) {
                        PMIX_LIST_DESTRUCT(&mylist);
                        return PMIX_ERR_NOMEM;
                    }
                    rc = process_request(nptr, idkey, ports_per_node, trk, &mylist);
                    if (PMIX_SUCCESS != rc) {
                        /* return the allocated ports */
                        pmix_list_remove_item(&allocations, &trk->super);
                        PMIX_RELEASE(trk);
                        PMIX_LIST_DESTRUCT(&mylist);
                        return rc;
                    }
                    allocated = true;
                }
            }
        }
        if (!allocated) {
            /* nope - we cannot help */
            PMIX_LIST_DESTRUCT(&mylist);
            return PMIX_ERR_TAKE_NEXT_OPTION;
        }
    }

    if (seckey) {
        pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                            "pnet:tcp: generate seckey");
        generate_key(unique_key);
        kv = PMIX_NEW(pmix_kval_t);
        if (NULL == kv) {
            PMIX_LIST_DESTRUCT(&mylist);
            return PMIX_ERR_NOMEM;
        }
        kv->key = strdup(PMIX_ALLOC_FABRIC_SEC_KEY);
        kv->value = (pmix_value_t *) calloc(1, sizeof(pmix_value_t));
        if (NULL == kv->value) {
            PMIX_RELEASE(kv);
            PMIX_LIST_DESTRUCT(&mylist);
            return PMIX_ERR_NOMEM;
        }
        kv->value->type = PMIX_BYTE_OBJECT;
        kv->value->data.bo.bytes = (char *) malloc(2 * sizeof(uint64_t));
        if (NULL == kv->value->data.bo.bytes) {
            PMIX_RELEASE(kv);
            PMIX_LIST_DESTRUCT(&mylist);
            return PMIX_ERR_NOMEM;
        }
        memcpy(kv->value->data.bo.bytes, unique_key, 2 * sizeof(uint64_t));
        kv->value->data.bo.size = 2 * sizeof(uint64_t);
        pmix_list_append(&mylist, &kv->super);
    }

    n = pmix_list_get_size(&mylist);
    if (0 < n) {
        PMIX_CONSTRUCT(&buf, pmix_buffer_t);
        /* pack the number of kvals for ease on the remote end - the far
         * end sizes its array from this, so a silent failure here would
         * hand it a blob it cannot read */
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, &buf, &n, 1, PMIX_SIZE);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DESTRUCT(&buf);
            PMIX_LIST_DESTRUCT(&mylist);
            return rc;
        }
        /* cycle across the list and pack the kvals */
        while (NULL != (kv = (pmix_kval_t *) pmix_list_remove_first(&mylist))) {
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
        if (NULL == kv) {
            PMIX_DESTRUCT(&buf);
            return PMIX_ERR_NOMEM;
        }
        kv->key = strdup(PMIX_TCP_SETUP_APP_KEY);
        kv->value = (pmix_value_t *) calloc(1, sizeof(pmix_value_t));
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

    /* if we got here, then we processed this specific request, so
     * indicate that by returning success */
    return PMIX_SUCCESS;
}

/* upon receipt of the launch message, each daemon adds the
 * static address assignments to the job-level info cache
 * for that job */
static pmix_status_t setup_local_network(pmix_nspace_env_cache_t *nptr, pmix_info_t info[],
                                         size_t ninfo)
{
    size_t n, m, nkvals;
    pmix_buffer_t bkt;
    int32_t cnt;
    pmix_kval_t *kv;
    pmix_status_t rc;
    pmix_info_t *jinfo, stinfo;
    char *idkey;

    pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                        "pnet:tcp:setup_local_network");

    if (NULL == info) {
        return PMIX_SUCCESS;
    }

    for (n = 0; n < ninfo; n++) {
        /* look for my key */
        if (0 != strncmp(info[n].key, PMIX_TCP_SETUP_APP_KEY, PMIX_MAX_KEYLEN)) {
            continue;
        }
        /* the blob belongs to our caller, who hands the same array to
         * every active module in turn - so read it in place instead of
         * taking ownership of the bytes */
        PMIX_LOAD_BUFFER_NON_DESTRUCT(pmix_globals.mypeer, &bkt, info[n].value.data.bo.bytes,
                                      info[n].value.data.bo.size);
        /* unpack the number of kvals */
        cnt = 1;
        PMIX_BFROPS_UNPACK(rc, pmix_globals.mypeer, &bkt, &nkvals, &cnt, PMIX_SIZE);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            return rc;
        }
        if (0 == nkvals) {
            /* nothing in this blob to cache */
            continue;
        }
        /* setup the info array */
        PMIX_INFO_CONSTRUCT(&stinfo);
        stinfo.value.type = PMIX_DATA_ARRAY;
        PMIX_DATA_ARRAY_CREATE(stinfo.value.data.darray, nkvals, PMIX_INFO);
        if (NULL == stinfo.value.data.darray) {
            PMIX_INFO_DESTRUCT(&stinfo);
            return PMIX_ERR_NOMEM;
        }
        jinfo = (pmix_info_t *) stinfo.value.data.darray->array;

        /* cycle thru the blob and extract the kvals - the count we just
         * unpacked is what sized jinfo, so stop there no matter how many
         * kvals the remainder of the blob turns out to hold */
        idkey = NULL;
        m = 0;
        kv = PMIX_NEW(pmix_kval_t);
        cnt = 1;
        PMIX_BFROPS_UNPACK(rc, pmix_globals.mypeer, &bkt, kv, &cnt, PMIX_KVAL);
        while (PMIX_SUCCESS == rc && m < nkvals) {
            pmix_output_verbose(2, pmix_pnet_base_framework.framework_output, "recvd KEY %s %s",
                                kv->key,
                                (PMIX_STRING == kv->value->type) ? kv->value->data.string
                                                                 : "NON-STRING");
            /* xfer the value to the info */
            pmix_strncpy(jinfo[m].key, kv->key, PMIX_MAX_KEYLEN);
            PMIX_BFROPS_VALUE_XFER(rc, pmix_globals.mypeer, &jinfo[m].value, kv->value);
            if (PMIX_SUCCESS != rc) {
                /* the next unpack would overwrite rc and we would cache
                 * a silently empty entry, so stop here */
                PMIX_ERROR_LOG(rc);
                PMIX_RELEASE(kv);
                PMIX_INFO_DESTRUCT(&stinfo);
                if (NULL != idkey) {
                    free(idkey);
                }
                return rc;
            }
            /* if this is the ID key, save it */
            if (NULL == idkey && 0 == strncmp(kv->key, PMIX_ALLOC_FABRIC_ID, PMIX_MAX_KEYLEN)
                && PMIX_STRING == kv->value->type && NULL != kv->value->data.string) {
                idkey = strdup(kv->value->data.string);
            }
            ++m;
            PMIX_RELEASE(kv);
            kv = PMIX_NEW(pmix_kval_t);
            cnt = 1;
            PMIX_BFROPS_UNPACK(rc, pmix_globals.mypeer, &bkt, kv, &cnt, PMIX_KVAL);
        }
        /* the loop ends holding a kval we did not consume - either the
         * unpack that ended it read nothing, or we hit the count and
         * stopped - so it is ours to release either way */
        PMIX_RELEASE(kv);

        /* if they didn't include a network ID, then this is an error */
        if (NULL == idkey) {
            PMIX_INFO_DESTRUCT(&stinfo);
            return PMIX_ERR_BAD_PARAM;
        }
        /* the array is keyed by the fabric ID the allocation was made
         * under - that is what a client will ask for */
        pmix_strncpy(stinfo.key, idkey, PMIX_MAX_KEYLEN);
        free(idkey);

        /* cache the info on the job */
        PMIX_GDS_CACHE_JOB_INFO(rc, pmix_globals.mypeer, nptr->ns, &stinfo, 1);
        PMIX_INFO_DESTRUCT(&stinfo);
    }
    return PMIX_SUCCESS;
}

/* when a local client finalizes, the server gives us a chance
 * to do any required local cleanup for that peer. We don't
 * have anything we need to do */
static void child_finalized(pmix_proc_t *peer)
{
    PMIX_HIDE_UNUSED_PARAMS(peer);
    pmix_output_verbose(2, pmix_pnet_base_framework.framework_output, "pnet:tcp child finalized");
}

/* when all local clients for a given job finalize, the server
 * provides an opportunity for the local network to cleanup
 * any resources consumed locally by the clients of that job.
 * We don't have anything we need to do */
static void local_app_finalized(pmix_namespace_t *nptr)
{
    PMIX_HIDE_UNUSED_PARAMS(nptr);
    pmix_output_verbose(2, pmix_pnet_base_framework.framework_output, "pnet:tcp app finalized");
}

/* when the job completes, the scheduler calls the "deregister nspace"
 * PMix function, which in turn calls my TCP component to release the
 * assignments for that job. The addresses are marked as "available"
 * for reuse on the next job. */
static void deregister_nspace(pmix_namespace_t *nptr)
{
    tcp_port_tracker_t *trk, *nxt;

    pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                        "pnet:tcp deregister nspace %s", nptr->nspace);

    /* if we are not the "gateway", then there is nothing
     * for us to do */
    if (!PMIX_PEER_IS_GATEWAY(pmix_globals.mypeer)) {
        return;
    }

    /* find the trackers for this job - a single allocation request can
     * produce more than one of them (the default-allocation path makes a
     * tracker per requested group), and every one of them has to be
     * released or its ports never return to the pool */
    PMIX_LIST_FOREACH_SAFE (trk, nxt, &allocations, tcp_port_tracker_t) {
        if (0 == strcmp(nptr->nspace, trk->nspace)) {
            pmix_list_remove_item(&allocations, &trk->super);
            PMIX_RELEASE(trk);
            pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                                "pnet:tcp released tracker for nspace %s", nptr->nspace);
        }
    }
}

static pmix_status_t collect_inventory(pmix_info_t directives[], size_t ndirs,
                                       pmix_list_t *inventory)
{
    char *prefix;
    char myconnhost[PMIX_MAXHOSTNAMELEN] = {0};
    char name[32], uri[2048];
    struct sockaddr_storage my_ss;
    char *foo;
    pmix_buffer_t bucket, pbkt;
    int i;
    pmix_status_t rc;
    bool found = false;
    pmix_byte_object_t pbo;
    pmix_kval_t *kv;

    PMIX_HIDE_UNUSED_PARAMS(directives, ndirs);

    pmix_output_verbose(2, pmix_pnet_base_framework.framework_output, "pnet:tcp:collect_inventory");

    /* setup the bucket - we will pass the results as a blob */
    PMIX_CONSTRUCT(&bucket, pmix_buffer_t);
    /* add our hostname */
    PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, &bucket, &pmix_globals.hostname, 1, PMIX_STRING);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DESTRUCT(&bucket);
        return rc;
    }

    /* look at all available interfaces */
    for (i = pmix_ifbegin(); i >= 0; i = pmix_ifnext(i)) {
        if (PMIX_SUCCESS != pmix_ifindextoaddr(i, (struct sockaddr *) &my_ss, sizeof(my_ss))) {
            pmix_output(0, "pnet_tcp: problems getting address for index %i (kernel index %i)\n", i,
                        pmix_ifindextokindex(i));
            continue;
        }
        /* ignore non-ip4/6 interfaces */
        if (AF_INET != my_ss.ss_family && AF_INET6 != my_ss.ss_family) {
            continue;
        }
        /* get the name for diagnostic purposes - without one we cannot
         * report anything useful about this interface, so skip it */
        if (PMIX_SUCCESS != pmix_ifindextoname(i, name, sizeof(name))) {
            continue;
        }

        /* ignore any virtual interfaces */
        if (0 == strncmp(name, "vir", 3)) {
            continue;
        }
        /* ignore the loopback device */
        if (pmix_ifisloopback(i)) {
            continue;
        }
        if (AF_INET == my_ss.ss_family) {
            prefix = "tcp4://";
            inet_ntop(AF_INET, &((struct sockaddr_in *) &my_ss)->sin_addr, myconnhost,
                      sizeof(myconnhost));
        } else if (AF_INET6 == my_ss.ss_family) {
            prefix = "tcp6://";
            inet_ntop(AF_INET6, &((struct sockaddr_in6 *) &my_ss)->sin6_addr, myconnhost,
                      sizeof(myconnhost));
        } else {
            continue;
        }
        (void) pmix_snprintf(uri, sizeof(uri), "%s%s", prefix, myconnhost);
        pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                            "TCP INVENTORY ADDING: %s %s", name, uri);
        found = true;
        /* pack the name of the device */
        PMIX_CONSTRUCT(&pbkt, pmix_buffer_t);
        foo = &name[0];
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, &pbkt, &foo, 1, PMIX_STRING);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DESTRUCT(&pbkt);
            PMIX_DESTRUCT(&bucket);
            return rc;
        }
        /* pack the address */
        foo = &uri[0];
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, &pbkt, &foo, 1, PMIX_STRING);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DESTRUCT(&pbkt);
            PMIX_DESTRUCT(&bucket);
            return rc;
        }
        /* extract the resulting blob - this is a device unit */
        PMIX_UNLOAD_BUFFER(&pbkt, pbo.bytes, pbo.size);
        /* now load that into the blob - the pack copies the bytes, so
         * our own copy has to go back either way */
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, &bucket, &pbo, 1, PMIX_BYTE_OBJECT);
        PMIX_BYTE_OBJECT_DESTRUCT(&pbo);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DESTRUCT(&bucket);
            return rc;
        }
    }
    /* if we have anything to report, then package it up for transfer */
    if (!found) {
        PMIX_DESTRUCT(&bucket);
        return PMIX_ERR_TAKE_NEXT_OPTION;
    }
    /* extract the resulting blob */
    PMIX_UNLOAD_BUFFER(&bucket, pbo.bytes, pbo.size);
    kv = PMIX_NEW(pmix_kval_t);
    if (NULL == kv) {
        PMIX_BYTE_OBJECT_DESTRUCT(&pbo);
        return PMIX_ERR_NOMEM;
    }
    kv->key = strdup(PMIX_TCP_INVENTORY_KEY);
    PMIX_VALUE_CREATE(kv->value, 1);
    if (NULL == kv->value) {
        PMIX_RELEASE(kv);
        PMIX_BYTE_OBJECT_DESTRUCT(&pbo);
        return PMIX_ERR_NOMEM;
    }
    kv->value->type = PMIX_BYTE_OBJECT;
    /* transfer ownership of the unloaded blob into the value */
    kv->value->data.bo.bytes = pbo.bytes;
    kv->value->data.bo.size = pbo.size;
    pmix_list_append(inventory, &kv->super);

    return PMIX_SUCCESS;
}

/* start tracking an allocation drawn from the given pool, and publish
 * the tracker on the allocations list. Every search of that list - and
 * deregister_nspace in particular - compares the nspace string, so a
 * tracker carrying a NULL one must never be published */
static tcp_port_tracker_t *new_tracker(pmix_namespace_t *nptr, tcp_available_ports_t *avail)
{
    tcp_port_tracker_t *trk;

    trk = PMIX_NEW(tcp_port_tracker_t);
    if (NULL == trk) {
        return NULL;
    }
    trk->nspace = strdup(nptr->nspace);
    if (NULL == trk->nspace) {
        PMIX_RELEASE(trk);
        return NULL;
    }
    PMIX_RETAIN(avail);
    trk->src = avail;
    pmix_list_append(&allocations, &trk->super);
    return trk;
}

static pmix_status_t process_request(pmix_namespace_t *nptr, char *idkey, int ports_per_node,
                                     tcp_port_tracker_t *trk, pmix_list_t *ilist)
{
    char **plist;
    pmix_kval_t *kv;
    size_t m;
    int p, ppn;
    tcp_available_ports_t *avail = trk->src;

    PMIX_HIDE_UNUSED_PARAMS(nptr);

    kv = PMIX_NEW(pmix_kval_t);
    if (NULL == kv) {
        return PMIX_ERR_NOMEM;
    }
    kv->key = strdup(idkey);
    kv->value = (pmix_value_t *) calloc(1, sizeof(pmix_value_t));
    if (NULL == kv->value) {
        PMIX_RELEASE(kv);
        return PMIX_ERR_NOMEM;
    }
    kv->value->type = PMIX_STRING;
    kv->value->data.string = NULL;
    if (0 == ports_per_node) {
        /* find the maxprocs on the nodes in this nspace and
         * allocate that number of resources */
        PMIX_RELEASE(kv);
        return PMIX_ERR_NOT_SUPPORTED;
    } else {
        ppn = ports_per_node;
    }

    /* assemble the list of ports */
    p = 0;
    plist = NULL;
    for (m = 0; p < ppn && m < avail->nports; m++) {
        if (NULL != avail->ports[m]) {
            PMIx_Argv_append_nosize(&trk->ports, avail->ports[m]);
            PMIx_Argv_append_nosize(&plist, avail->ports[m]);
            free(avail->ports[m]);
            avail->ports[m] = NULL;
            ++p;
        }
    }
    /* if we couldn't find enough, then that's an error */
    if (p < ppn) {
        PMIX_RELEASE(kv);
        /* the caller will release trk, and that will return
         * any allocated ports back to the available list */
        return PMIX_ERR_OUT_OF_RESOURCE;
    }
    /* pass the value */
    kv->value->data.string = PMIx_Argv_join(plist, ',');
    PMIx_Argv_free(plist);
    pmix_list_append(ilist, &kv->super);

    /* track where it came from */
    kv = PMIX_NEW(pmix_kval_t);
    if (NULL == kv) {
        return PMIX_ERR_NOMEM;
    }
    kv->key = strdup(idkey);
    kv->value = (pmix_value_t *) calloc(1, sizeof(pmix_value_t));
    if (NULL == kv->value) {
        PMIX_RELEASE(kv);
        return PMIX_ERR_NOMEM;
    }
    kv->value->type = PMIX_STRING;
    kv->value->data.string = strdup(trk->src->type);
    pmix_list_append(ilist, &kv->super);
    if (NULL != trk->src->plane) {
        kv = PMIX_NEW(pmix_kval_t);
        if (NULL == kv) {
            return PMIX_ERR_NOMEM;
        }
        kv->key = strdup(idkey);
        kv->value = (pmix_value_t *) calloc(1, sizeof(pmix_value_t));
        if (NULL == kv->value) {
            PMIX_RELEASE(kv);
            return PMIX_ERR_NOMEM;
        }
        kv->value->type = PMIX_STRING;
        kv->value->data.string = strdup(trk->src->plane);
        pmix_list_append(ilist, &kv->super);
    }
    return PMIX_SUCCESS;
}

static pmix_status_t deliver_inventory(pmix_info_t info[], size_t ninfo, pmix_info_t directives[],
                                       size_t ndirs)
{
    pmix_buffer_t bkt, pbkt;
    size_t n;
    int32_t cnt;
    char *hostname, *device, *address;
    pmix_byte_object_t pbo;
    tcp_node_t *nd, *ndptr;
    tcp_resource_t *lt, *lst;
    tcp_available_ports_t *prts;
    tcp_device_t *res;
    pmix_status_t rc;

    PMIX_HIDE_UNUSED_PARAMS(directives, ndirs);

    pmix_output_verbose(2, pmix_pnet_base_framework.framework_output, "pnet:tcp deliver inventory");

    for (n = 0; n < ninfo; n++) {
        if (0 == strncmp(info[n].key, PMIX_TCP_INVENTORY_KEY, PMIX_MAX_KEYLEN)) {
            /* this is our inventory in the form of a blob. The array is
             * handed to every active module in turn, so load it without
             * taking the bytes away from our caller */
            PMIX_LOAD_BUFFER_NON_DESTRUCT(pmix_globals.mypeer, &bkt, info[n].value.data.bo.bytes,
                                          info[n].value.data.bo.size);
            /* first is the host this came from */
            cnt = 1;
            PMIX_BFROPS_UNPACK(rc, pmix_globals.mypeer, &bkt, &hostname, &cnt, PMIX_STRING);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                /* must _not_ destruct bkt as we don't
                 * own the bytes! */
                return rc;
            }
            /* do we already have this node? */
            nd = NULL;
            PMIX_LIST_FOREACH (ndptr, &nodes, tcp_node_t) {
                if (0 == strcmp(hostname, ndptr->name)) {
                    nd = ndptr;
                    break;
                }
            }
            if (NULL == nd) {
                nd = PMIX_NEW(tcp_node_t);
                if (NULL == nd) {
                    free(hostname);
                    return PMIX_ERR_NOMEM;
                }
                nd->name = strdup(hostname);
                if (NULL == nd->name) {
                    /* every search of this list compares that string */
                    PMIX_RELEASE(nd);
                    free(hostname);
                    return PMIX_ERR_NOMEM;
                }
                pmix_list_append(&nodes, &nd->super);
            }
            /* does this node already have a TCP entry? */
            lst = NULL;
            PMIX_LIST_FOREACH (lt, &nd->resources, tcp_resource_t) {
                if (0 == strcmp(lt->name, "tcp")) {
                    lst = lt;
                    break;
                }
            }
            if (NULL == lst) {
                lst = PMIX_NEW(tcp_resource_t);
                if (NULL == lst) {
                    free(hostname);
                    return PMIX_ERR_NOMEM;
                }
                lst->name = strdup("tcp");
                if (NULL == lst->name) {
                    PMIX_RELEASE(lst);
                    free(hostname);
                    return PMIX_ERR_NOMEM;
                }
                pmix_list_append(&nd->resources, &lst->super);
            }
            /* this is a list of ports and devices */
            prts = PMIX_NEW(tcp_available_ports_t);
            if (NULL == prts) {
                free(hostname);
                return PMIX_ERR_NOMEM;
            }
            pmix_list_append(&lst->resources, &prts->super);
            /* cycle across any provided interfaces */
            cnt = 1;
            PMIX_BFROPS_UNPACK(rc, pmix_globals.mypeer, &bkt, &pbo, &cnt, PMIX_BYTE_OBJECT);
            while (PMIX_SUCCESS == rc) {
                /* load the byte object for unpacking */
                PMIX_CONSTRUCT(&pbkt, pmix_buffer_t);
                PMIX_LOAD_BUFFER(pmix_globals.mypeer, &pbkt, pbo.bytes, pbo.size);
                /* unpack the name of the device */
                cnt = 1;
                PMIX_BFROPS_UNPACK(rc, pmix_globals.mypeer, &pbkt, &device, &cnt, PMIX_STRING);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    PMIX_DESTRUCT(&pbkt);
                    free(hostname);
                    /* must _not_ destruct bkt as we don't
                     * own the bytes! */
                    return rc;
                }
                /* unpack the address */
                cnt = 1;
                PMIX_BFROPS_UNPACK(rc, pmix_globals.mypeer, &pbkt, &address, &cnt, PMIX_STRING);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    PMIX_DESTRUCT(&pbkt);
                    free(device);
                    free(hostname);
                    /* must _not_ destruct bkt as we don't
                     * own the bytes! */
                    return rc;
                }
                /* store this on the node */
                res = PMIX_NEW(tcp_device_t);
                if (NULL == res) {
                    free(device);
                    free(address);
                    free(hostname);
                    PMIX_DESTRUCT(&pbkt);
                    return PMIX_ERR_NOMEM;
                }
                res->device = device;
                res->address = address;
                pmix_list_append(&prts->devices, &res->super);
                PMIX_DESTRUCT(&pbkt);
                cnt = 1;
                PMIX_BFROPS_UNPACK(rc, pmix_globals.mypeer, &bkt, &pbo, &cnt, PMIX_BYTE_OBJECT);
            }
            free(hostname);
            if (5 < pmix_output_get_verbosity(pmix_pnet_base_framework.framework_output)) {
                /* dump the resulting node resources */
                pmix_output(0, "TCP resources for node: %s", nd->name);
                PMIX_LIST_FOREACH (lt, &nd->resources, tcp_resource_t) {
                    if (0 == strcmp(lt->name, "tcp")) {
                        PMIX_LIST_FOREACH (prts, &lt->resources, tcp_available_ports_t) {
                            device = NULL;
                            if (NULL != prts->ports) {
                                device = PMIx_Argv_join(prts->ports, ',');
                            }
                            pmix_output(0, "\tPorts: %s",
                                        (NULL == device) ? "UNSPECIFIED" : device);
                            if (NULL != device) {
                                free(device);
                            }
                            PMIX_LIST_FOREACH (res, &prts->devices, tcp_device_t) {
                                pmix_output(0, "\tDevice: %s", res->device);
                                pmix_output(0, "\tAddress: %s", res->address);
                            }
                        }
                    }
                }
            }
        }
    }

    return PMIX_SUCCESS;
}
