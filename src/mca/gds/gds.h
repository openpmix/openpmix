/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2016-2020 Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2016-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2018      IBM Corporation.  All rights reserved.
 * Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
 * Copyright (c) 2022      Triad National Security, LLC. All rights reserved.
 * Copyright (c) 2026      Jeff Squyres  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PMIX_GDS_H
#define PMIX_GDS_H

#include "src/include/pmix_config.h"

#include "pmix_common.h"
#include "src/mca/base/pmix_mca_base_framework.h"
#include "src/mca/base/pmix_mca_base_var.h"
#include "src/mca/bfrops/bfrops_types.h"
#include "src/mca/mca.h"

/* The client dictates the GDS module that will be used to interact
 * with the server - this module is stored in pmix_globals.mypeer->compat.gds
 * Because that is a long address to keep typing out, convenience macros
 * are provided for when that module is to be used in an operation.
 *
 * However, an application can open any number of GDS modules for
 * purposes other than exchanging info with the server. For example,
 * an application may wish to utilize a DHT module for its own
 * peer-to-peer data sharing. Thus, the public and private interfaces
 * are deliberately designed to be generic. The macros should make
 * things easier for the typical internal operations
 *
 * NOTE: ALTHOUGH SOME GDS COMPONENTS MAY UTILIZE THEIR OWN INTERNAL
 * PROGRESS THREADS, THE GDS IS NOT GUARANTEED TO BE THREAD-SAFE.
 * GDS FUNCTIONS SHOULD THEREFORE ALWAYS BE CALLED IN A THREAD-SAFE
 * CONDITION - E.G., FROM WITHIN AN EVENT
 */

BEGIN_C_DECLS
/* forward declaration */
struct pmix_peer_t;
struct pmix_namespace_t;

/* backdoor to base verbosity */
PMIX_EXPORT extern int pmix_gds_base_output;

/* Which realm answers a fetch.
 *
 * A request is answered out of exactly one of these, and WHICH ONE IS
 * COMPUTED ONCE PER FETCH, by pmix_gds_base_request_realm() in
 * src/mca/gds/base. Nothing else may work it out for itself.
 *
 * It used to be worked out twice - once by process_request() from the
 * directives it had parsed, and again by each gds module from the key
 * and the three realm qualifiers - and the two did not agree. They
 * disagreed about PMIX_JOB_INFO, which the client honored by clearing
 * the realm a key had selected and the modules had never heard of, so a
 * request could be judged a plain job-level lookup by the client and
 * answered from the session realm by the datastore. That is a wrong
 * answer to the caller, and on a module that reports is_tsafe it is
 * also a session read on the application's thread, which is the one
 * thread the session structures are not protected against.
 *
 * PMIX_REALM_UNDEF means "not computed yet" - PMIX_GDS_FETCH_KV fills
 * it in from the request and caches it on the caddy, so a caller that
 * has already worked it out does not pay for it twice and one that has
 * not cannot forget.
 */
typedef enum {
    PMIX_REALM_UNDEF = 0,
    PMIX_REALM_JOB,
    PMIX_REALM_SESSION,
    PMIX_REALM_NODE,
    PMIX_REALM_APP
} pmix_realm_t;

/* Which realm answers this request. THE ONLY place that decides it;
 * implemented in src/mca/gds/base/gds_base_fns.c and declared here
 * because PMIX_GDS_FETCH_KV below is what guarantees it is asked once.
 * Callers cache the answer on the pmix_cb_t they pass to that macro. */
PMIX_EXPORT pmix_realm_t pmix_gds_base_request_realm(const char *key,
                                                     const pmix_info_t info[],
                                                     size_t ninfo);


/**
 * Initialize the module. Returns an error if the module cannot
 * run, success if it can.
 */
typedef pmix_status_t (*pmix_gds_base_module_init_fn_t)(pmix_info_t info[], size_t ninfo);

/**
 * Finalize the module. Tear down any allocated storage, disconnect
 * from any system support.
 */
typedef void (*pmix_gds_base_module_fini_fn_t)(void);

/**
 * Assign a module per the requested directives. Modules should
 * review the provided directives to determine if they can support
 * the request. Modules are "scanned" in component priority order
 * and given an opportunity to respond. If a module offers itself,
 * it will provide a priority (which can be based on the directives
 * and therefore different from the component priority). The highest
 * returned priority received from a responder will be selected
 * and a pointer to its module returned */
typedef pmix_status_t (*pmix_gds_base_assign_module_fn_t)(pmix_info_t *info, size_t ninfo,
                                                          int *priority);

/* Resolve the GDS module to use for a given peer. A peer may carry its
 * own module (peer->gds) that overrides the nspace-level default in
 * nptr->compat.gds - this happens when a client falls back to a
 * different module than the rest of its nspace (e.g. shmem3 attach
 * failed and the client switched to hash). When peer->gds is NULL, the
 * nspace-level module is used. */
#define PMIX_GDS_PEER_MODULE(p) \
    (NULL != (p)->gds ? (p)->gds : (p)->nptr->compat.gds)

#define PMIX_GDS_CHECK_COMPONENT(p, s) (0 == strcmp(PMIX_GDS_PEER_MODULE(p)->name, (s)))
#define PMIX_GDS_CHECK_PEER_COMPONENT(p1, p2) \
    (0 == strcmp(PMIX_GDS_PEER_MODULE(p1)->name, PMIX_GDS_PEER_MODULE(p2)->name))

/* SERVER FN: assemble the keys buffer for server answer */
typedef pmix_status_t (*pmix_gds_base_module_assemb_kvs_req_fn_t)(const pmix_proc_t *proc,
                                                                  pmix_list_t *kvs,
                                                                  pmix_buffer_t *buf, void *cbdata);

/* define a macro for server keys answer based on peer */
#define PMIX_GDS_ASSEMB_KVS_REQ(s, p, r, k, b, c)                              \
    do {                                                                       \
        pmix_gds_base_module_t *_g = PMIX_GDS_PEER_MODULE(p);                  \
        (s) = PMIX_SUCCESS;                                                    \
        if (NULL == _g->assemb_kvs_req) {                                      \
            if (0 == strcmp(_g->name, "hash")) {                               \
                (s) = PMIX_ERR_NOT_SUPPORTED;                                  \
            } else {                                                           \
                _g = pmix_globals.mypeer->nptr->compat.gds;                    \
            }                                                                  \
        }                                                                      \
        if (NULL != _g->assemb_kvs_req) {                                      \
            pmix_output_verbose(1, pmix_gds_base_output,                       \
                                "[%s:%d] GDS ASSEMBLE REQ WITH %s",            \
                                __FILE__, __LINE__, _g->name);                 \
            (s) = _g->assemb_kvs_req(r, k, b, (void *) c);                     \
        }                                                                      \
    } while (0)

/* CLIENT FN: unpack buffer and key processing */
typedef pmix_status_t (*pmix_gds_base_module_accept_kvs_resp_fn_t)(pmix_buffer_t *buf);

/* define a macro for client key processing from a server response based on peer */
#define PMIX_GDS_ACCEPT_KVS_RESP(s, p, b)                                      \
    do {                                                                       \
        pmix_gds_base_module_t *_g = PMIX_GDS_PEER_MODULE(p);                  \
        (s) = PMIX_SUCCESS;                                                    \
        if (NULL == _g->accept_kvs_resp) {                                     \
            if (0 == strcmp(_g->name, "hash")) {                               \
                (s) = PMIX_ERR_NOT_SUPPORTED;                                  \
            } else {                                                           \
                _g = pmix_globals.mypeer->nptr->compat.gds;                    \
            }                                                                  \
        }                                                                      \
        if (NULL != _g->accept_kvs_resp) {                                     \
            pmix_output_verbose(1, pmix_gds_base_output,                       \
                                "[%s:%d] GDS ACCEPT RESP WITH %s",             \
                                __FILE__, __LINE__, _g->name);                 \
            (s) = _g->accept_kvs_resp(b);                                      \
        }                                                                      \
    } while (0)

/* SERVER FN: cache job-level info in the server's GDS until client
 * procs connect and we discover which GDS module to use for them.
 * Note that this is essentially the same function as store_job_info,
 * only we don't have packed data on the server side, and don't want
 * to incur the overhead of packing it just to unpack it in the function.
 */
typedef pmix_status_t (*pmix_gds_base_module_cache_job_info_fn_t)(struct pmix_namespace_t *ns,
                                                                  pmix_info_t info[], size_t ninfo);

/* define a convenience macro for caching job info */
#define PMIX_GDS_CACHE_JOB_INFO(s, p, n, i, ni)                                            \
    do {                                                                                   \
        pmix_gds_base_module_t *_g = PMIX_GDS_PEER_MODULE(p);                              \
        pmix_output_verbose(1, pmix_gds_base_output, "[%s:%d] GDS CACHE JOB INFO WITH %s", \
                            __FILE__, __LINE__, _g->name);                                 \
        if (NULL == _g->cache_job_info) {                                                  \
            (s) = PMIX_ERR_NOT_SUPPORTED;                                                  \
        } else {                                                                           \
            (s) = _g->cache_job_info((struct pmix_namespace_t *) (n), (i), (ni));          \
        }                                                                                  \
    } while (0)

/* register job-level info - this is provided as a special function
 * to allow for optimization. Called solely by the server. We cannot
 * prepare the job-level info provided at PMIx_Register_nspace, because
 * we don't know the GDS component to use for that application until
 * a local client contacts us. Thus, the module is required to process
 * the job-level info cached in the pmix_namespace_t for this job and
 * do whatever is necessary to support the client, packing any required
 * return message into the provided buffer.
 *
 * This function will be called once for each local client of
 * a given nspace. PMIx assumes that all peers of a given nspace
 * will use the same GDS module. Thus, the module is free to perform
 * any relevant optimizations (e.g., packing the data only once and
 * then releasing the cached buffer once all local clients have
 * been serviced, or storing it once in shared memory and simply
 * returning the shared memory rendezvous information for subsequent
 * calls).
 *
 * Info provided in the reply buffer will be given to the "store_job_info"
 * API of the GDS module on the client. Since this should match the
 * module used by the server, each module has full knowledge and control
 * over what is in the reply buffer.
 *
 * The pmix_peer_t of the requesting client is provided here so that
 * the module can access the job-level info cached on the corresponding
 * pmix_namespace_t pointed to by the pmix_peer_t
 */
typedef pmix_status_t (*pmix_gds_base_module_register_job_info_fn_t)(struct pmix_peer_t *pr,
                                                                     pmix_buffer_t *reply);

/* define a convenience macro for registering job info for
 * a given peer */
#define PMIX_GDS_REGISTER_JOB_INFO(s, p, b)                                                        \
    do {                                                                                           \
        pmix_gds_base_module_t *_g = PMIX_GDS_PEER_MODULE(p);                                      \
        pmix_output_verbose(1, pmix_gds_base_output, "[%s:%d] GDS REG JOB INFO WITH %s", __FILE__, \
                            __LINE__, _g->name);                                                   \
        (s) = _g->register_job_info((struct pmix_peer_t *) (p), b);                                \
    } while (0)

/* update job-level info - this is provided as a special function
 * to allow for optimization. Called solely by the client. The buffer
 * provided to this API is the same one given to the server by the
 * corresponding "register_job_info" function
 */
typedef pmix_status_t (*pmix_gds_base_module_store_job_info_fn_t)(const char *nspace,
                                                                  pmix_buffer_t *buf);

/* define a convenience macro for storing job info based on peer */
#define PMIX_GDS_STORE_JOB_INFO(s, p, n, b)                                                \
    do {                                                                                   \
        pmix_gds_base_module_t *_g = PMIX_GDS_PEER_MODULE(p);                              \
        pmix_output_verbose(1, pmix_gds_base_output, "[%s:%d] GDS STORE JOB INFO WITH %s", \
                            __FILE__, __LINE__, _g->name);                                 \
        (s) = _g->store_job_info(n, b);                                                    \
    } while (0)

/**
 * store key/value pair - these will either be values committed by the peer
 * and transmitted to the server, or values stored locally by the peer.
 * The format of the data depends on the GDS module. Note that data stored
 * with PMIX_INTERNAL scope should be stored solely within the process and
 * is never shared.
 *
 * @param peer   pointer to pmix_peer_t object of the peer that
 *               provided the data
 *
 * @param proc   the proc that the data describes
 *
 * @param scope  scope of the data
 *
 * @param kv     key/value pair.
 *
 * @return PMIX_SUCCESS on success.
 */
typedef pmix_status_t (*pmix_gds_base_module_store_fn_t)(const pmix_proc_t *proc,
                                                         pmix_scope_t scope, pmix_kval_t *kv);

/* define a convenience macro for storing key-val pairs based on peer */
#define PMIX_GDS_STORE_KV(s, p, pc, sc, k)                                     \
    do {                                                                       \
        pmix_gds_base_module_t *_g = PMIX_GDS_PEER_MODULE(p);                  \
        (s) = PMIX_SUCCESS;                                                    \
        if (NULL == _g->store) {                                               \
            if (0 == strcmp(_g->name, "hash")) {                               \
                (s) = PMIX_ERR_NOT_SUPPORTED;                                  \
            } else {                                                           \
                _g = pmix_globals.mypeer->nptr->compat.gds;                    \
            }                                                                  \
        }                                                                      \
        if (NULL != _g->store) {                                               \
            pmix_output_verbose(1, pmix_gds_base_output,                       \
                                "[%s:%d] GDS STORE KV WITH %s",                \
                                __FILE__, __LINE__, _g->name);                 \
            (s) = _g->store(pc, sc, k);                                        \
        }                                                                      \
    } while (0)

/**
 * unpack and store a data "blob" from a peer so that the individual
 * elements can later be retrieved. This is an optimization path to
 * avoid repeatedly storing pmix_kval_t's for multiple local procs
 * from the same nspace.
 *
 * buff - packed modex data collected across the operation
 *
 * cbdata - pointer to modex callback data
 *
 */
typedef pmix_status_t (*pmix_gds_base_module_store_modex_fn_t)(pmix_buffer_t *buff,
                                                               const char *nspace,
                                                               void *cbdata);

/**
 * define a convenience macro for storing modex byte objects
 *
 * r - return status code
 *
 * p - pointer to a pmix_peer_t of a local client of the nspace whose
 *     data is being stored. All local clients of one nspace share a
 *     gds module, but clients of *different* nspaces need not, so the
 *     module is resolved from a peer of the nspace being stored - not
 *     from the local server, whose own module may be neither.
 *
 * ns - the nspace whose blobs are to be stored from this payload. The
 *      aggregated result carries every participant's data, so the
 *      caller walks it once per participating nspace.
 *
 * b - pointer to pmix_buffer_t containing the data
 *
 * t - pointer to the modex server tracker
 */
#define PMIX_GDS_STORE_MODEX(r, p, ns, b, t)                                \
    do {                                                                    \
        pmix_gds_base_module_t *_g = PMIX_GDS_PEER_MODULE(p);               \
        pmix_output_verbose(1, pmix_gds_base_output,                        \
                            "[%s:%d] GDS STORE MODEX FOR %s WITH %s",       \
                            __FILE__, __LINE__,                             \
                            (NULL == (ns)) ? "ALL" : (ns), _g->name);       \
        if (NULL == _g->store_modex) {                                      \
            (r) = PMIX_ERR_NOT_SUPPORTED;                                   \
        } else {                                                            \
            (r) = _g->store_modex(b, ns, t);                                \
        }                                                                   \
    } while (0)

typedef pmix_status_t (*pmix_gds_base_module_mark_modex_complete_fn_t)(struct pmix_peer_t *peer,
                                                                       pmix_list_t *nslist,
                                                                       pmix_buffer_t *buff);
/* Resolved from the peer being replied to, not from the local server:
 * this adds whatever that peer's own module needs in order to reach the
 * modex data - for shmem3, the segment info it must map. */
#define PMIX_GDS_MARK_MODEX_COMPLETE(r, p, l, b)                            \
    do {                                                                    \
        pmix_gds_base_module_t *_g = PMIX_GDS_PEER_MODULE(p);               \
        pmix_output_verbose(1, pmix_gds_base_output,                        \
                            "[%s:%d] GDS MARK MODEX COMPLETE WITH %s",      \
                            __FILE__, __LINE__, _g->name);                  \
        if (NULL == _g->mark_modex_complete) {                              \
            (r) = PMIX_SUCCESS;                                             \
        } else {                                                            \
            (r) = _g->mark_modex_complete(p, l, b);                         \
        }                                                                   \
    } while (0)

typedef pmix_status_t (*pmix_gds_base_module_recv_modex_complete_fn_t)(pmix_buffer_t *buff);
/* The client side of the above, resolved from the server peer that sent
 * it - which is the peer carrying this client's assigned module. */
#define PMIX_GDS_RECV_MODEX_COMPLETE(r, p, b)                               \
    do {                                                                    \
        pmix_gds_base_module_t *_g = PMIX_GDS_PEER_MODULE(p);               \
        pmix_output_verbose(1, pmix_gds_base_output,                        \
                            "[%s:%d] GDS RECV MODEX COMPLETE WITH %s",      \
                            __FILE__, __LINE__, _g->name);                  \
        if (NULL == _g->recv_modex_complete) {                              \
            (r) = PMIX_SUCCESS;                                             \
        } else {                                                            \
            (r) = _g->recv_modex_complete(b);                               \
        }                                                                   \
    } while (0)

/**
 * fetch value corresponding to provided key from within the defined
 * scope. A NULL key returns all values committed by the given peer
 * for that scope.
 *
 * @param peer    peer object of the proc requesting the info - needed
 *                because the server can request info on behalf of another
 *                process, and the fetch response has to be formatted
 *                to match the _requesting_ process
 *
 * @param proc    namespace and rank whose info is being requested
 *
 * @param key     key.
 *
 * @param scope   scope of the data to be considered
 *
 * @param copy    true if the caller _requires_ a copy of the data. This
 *                is used when the requestor is off-node. If
 *                set to false, then the GDS component can provide
 *                either a copy of the data, or shmem contact info
 *                to the location of the data
 *
 * @param info    array of pmix_info_t the caller provided as
 *                qualifiers to guide the request
 *
 * @param ninfo   number of elements in the info array
 *
 * @param kvs     pointer to a list that will be populated with the
 *                returned pmix_kval_t data
 *
 * @return       PMIX_SUCCESS on success.
 *
 * Note: all available job-level data for a given nspace can be fetched
 * by passing a proc with rank=PMIX_RANK_WILDCARD and a NULL key. Similarly,
 * passing a NULL key for a non-wildcard rank will return all data "put"
 * by that rank. Scope is ignored for job-level data requests.
 *
 * When a specific rank if provided with a NULL key, then data for only
 * that rank is returned. If the scope is PMIX_LOCAL, then the returned
 * data shall include only data that was specifically "put" to local scope,
 * plus any data that was put to PMIX_GLOBAL scope. Similarly, a scope of
 * PMIX_REMOTE will return data that was "put" to remote scope, plus
 * any data that was put to PMIX_GLOBAL scope. A scope of PMIX_GLOBAL
 * will return LOCAL, REMOTE, and GLOBAL data.
 *
 * Data stored with PMIX_INTERNAL scope can be retrieved with that scope.
 */
typedef pmix_status_t (*pmix_gds_base_module_fetch_fn_t)(struct pmix_peer_t *peer,
                                                         const pmix_proc_t *proc,
                                                         pmix_scope_t scope, bool copy,
                                                         const char *key, pmix_info_t info[],
                                                         size_t ninfo, pmix_realm_t realm,
                                                         pmix_list_t *kvs);

/* define a convenience macro for fetch key-val pairs based on peer,
 * passing a pmix_cb_t containing all the required info */
#define PMIX_GDS_FETCH_KV(s, p, c)                                                             \
    do {                                                                                       \
        pmix_gds_base_module_t *_g = PMIX_GDS_PEER_MODULE(p);                                  \
        pmix_output_verbose(1, pmix_gds_base_output,                                           \
                            "[%s:%d] GDS FETCH KV WITH %s",                                    \
                            __FILE__,  __LINE__, _g->name);                                    \
        /* Which realm answers this is computed ONCE per fetch and cached \
         * here. A caller that already worked it out - the client parse - \
         * has set it and this leaves it alone; one that has not cannot \
         * forget, and the module is handed the answer rather than \
         * deriving a second opinion from the same request. */           \
        if (PMIX_REALM_UNDEF == (c)->realm) {                                                  \
            (c)->realm = pmix_gds_base_request_realm((c)->key, (c)->info, (c)->ninfo);         \
        }                                                                                      \
        (s) = _g->fetch((p), (c)->proc, (c)->scope, (c)->copy, (c)->key,                       \
                        (c)->info, (c)->ninfo, (c)->realm, &(c)->kvs);                         \
    } while (0)

/**
 * Add any envars to a peer's environment that the module needs
 * to communicate. The API stub will rotate across all active modules, giving
 * each a chance to contribute
 *
 * @return PMIX_SUCCESS on success.
 */
typedef pmix_status_t (*pmix_gds_base_module_setup_fork_fn_t)(const pmix_proc_t *proc, char ***env);

/**
 * Define a new nspace in the GDS
 *
 * @param nspace   namespace string
 *
 * @return PMIX_SUCCESS on success.
 */
typedef pmix_status_t (*pmix_gds_base_module_add_nspace_fn_t)(const char *nspace,
                                                              uint32_t nlocalprocs,
                                                              pmix_info_t info[], size_t ninfo);

/* define a convenience macro for add_nspace based on peer */
#define PMIX_GDS_ADD_NSPACE(s, n, ls, i, ni)                                                \
    do {                                                                                    \
        pmix_gds_base_active_module_t *_g;                                                  \
        pmix_status_t _s = PMIX_SUCCESS;                                                    \
        (s) = PMIX_SUCCESS;                                                                 \
        pmix_output_verbose(1, pmix_gds_base_output, "[%s:%d] GDS ADD NSPACE %s", __FILE__, \
                            __LINE__, (n));                                                 \
        PMIX_LIST_FOREACH (_g, &pmix_gds_globals.actives, pmix_gds_base_active_module_t) {  \
            if (NULL == _g->module->add_nspace) {                                           \
                continue;                                                                   \
            }                                                                               \
            _s = _g->module->add_nspace(n, ls, i, ni);                                      \
            if (PMIX_SUCCESS != _s) {                                                       \
                (s) = PMIX_ERROR;                                                           \
            }                                                                               \
        }                                                                                   \
    } while (0)

/**
 * Delete nspace and its associated data
 *
 * @param nspace   namespace string
 *
 * @return PMIX_SUCCESS on success.
 */
typedef pmix_status_t (*pmix_gds_base_module_del_nspace_fn_t)(const char *nspace);

/* define a convenience macro for del_nspace based on peer */
#define PMIX_GDS_DEL_NSPACE(s, n)                                                           \
    do {                                                                                    \
        pmix_gds_base_active_module_t *_g;                                                  \
        pmix_status_t _s = PMIX_SUCCESS;                                                    \
        (s) = PMIX_SUCCESS;                                                                 \
        pmix_output_verbose(1, pmix_gds_base_output, "[%s:%d] GDS DEL NSPACE %s", __FILE__, \
                            __LINE__, (n));                                                 \
        PMIX_LIST_FOREACH (_g, &pmix_gds_globals.actives, pmix_gds_base_active_module_t) {  \
            if (NULL == _g->module->del_nspace) {                                           \
                continue;                                                                   \
            }                                                                               \
            _s = _g->module->del_nspace(n);                                                 \
            if (PMIX_SUCCESS != _s) {                                                       \
                (s) = PMIX_ERROR;                                                           \
            }                                                                               \
        }                                                                                   \
    } while (0)

/**
 * Add a session and its associated data.
 *
 * Sessions, like nspaces, are fanned out to *every* active module
 * rather than resolved from a peer: which module will serve the jobs
 * in a session is not known when the session is established, and may
 * differ between them.
 *
 * Optional. A module that keeps no session-level data leaves this NULL
 * and the macro treats that as success.
 *
 * @param sessionID  the session being established
 * @param info       session-level information, or NULL
 * @param ninfo      number of elements in info
 *
 * @return PMIX_SUCCESS on success.
 */
typedef pmix_status_t (*pmix_gds_base_module_add_session_fn_t)(uint32_t sessionID,
                                                               pmix_info_t info[],
                                                               size_t ninfo);

/* define a convenience macro for add_session - a fan-out, like add_nspace */
#define PMIX_GDS_ADD_SESSION(s, sid, i, ni)                                                  \
    do {                                                                                     \
        pmix_gds_base_active_module_t *_g;                                                   \
        pmix_status_t _s = PMIX_SUCCESS;                                                     \
        (s) = PMIX_SUCCESS;                                                                  \
        pmix_output_verbose(1, pmix_gds_base_output, "[%s:%d] GDS ADD SESSION %u", __FILE__, \
                            __LINE__, (unsigned) (sid));                                     \
        PMIX_LIST_FOREACH (_g, &pmix_gds_globals.actives, pmix_gds_base_active_module_t) {   \
            if (NULL == _g->module->add_session) {                                           \
                continue;                                                                    \
            }                                                                                \
            _s = _g->module->add_session(sid, i, ni);                                        \
            if (PMIX_SUCCESS != _s) {                                                        \
                (s) = PMIX_ERROR;                                                            \
            }                                                                                \
        }                                                                                    \
    } while (0)

/**
 * Delete a session and its associated data.
 *
 * This is said by the host, never inferred: a session with no jobs
 * running in it is an ordinary state, not an ended one, so the last
 * job leaving does not end the session. See
 * PMIx_server_deregister_session.
 *
 * Jobs still registered in the session are not deleted here; a module
 * releases the session's own data and lets those jobs go as their own
 * nspaces are deregistered.
 *
 * @param sessionID  the session that has ended
 *
 * @return PMIX_SUCCESS on success.
 */
typedef pmix_status_t (*pmix_gds_base_module_del_session_fn_t)(uint32_t sessionID);

/* define a convenience macro for del_session - a fan-out, like del_nspace */
#define PMIX_GDS_DEL_SESSION(s, sid)                                                         \
    do {                                                                                     \
        pmix_gds_base_active_module_t *_g;                                                   \
        pmix_status_t _s = PMIX_SUCCESS;                                                     \
        (s) = PMIX_SUCCESS;                                                                  \
        pmix_output_verbose(1, pmix_gds_base_output, "[%s:%d] GDS DEL SESSION %u", __FILE__, \
                            __LINE__, (unsigned) (sid));                                     \
        PMIX_LIST_FOREACH (_g, &pmix_gds_globals.actives, pmix_gds_base_active_module_t) {   \
            if (NULL == _g->module->del_session) {                                           \
                continue;                                                                    \
            }                                                                                \
            _s = _g->module->del_session(sid);                                               \
            if (PMIX_SUCCESS != _s) {                                                        \
                (s) = PMIX_ERROR;                                                            \
            }                                                                                \
        }                                                                                    \
    } while (0)

/**
 * Pack whatever this peer needs in order to see data that has been
 * added since it last looked - for gds/shmem3, the segments describing
 * a session whose description has changed.
 *
 * Optional; a module with nothing to say packs nothing and the caller
 * sends nothing. Resolved from the peer being told, because what it
 * needs depends on the module IT is bound to.
 *
 * Packing nothing must be reported as PMIX_SUCCESS - "this peer is up
 * to date" is not a failure.
 *
 * @param peer  the peer to be told
 * @param buff  buffer to pack into
 *
 * @return PMIX_SUCCESS on success.
 */
typedef pmix_status_t (*pmix_gds_base_module_pack_update_fn_t)(struct pmix_peer_t *peer,
                                                               pmix_buffer_t *buff);

#define PMIX_GDS_PACK_UPDATE(s, p, b)                                       \
    do {                                                                    \
        pmix_gds_base_module_t *_g = PMIX_GDS_PEER_MODULE(p);               \
        if (NULL == _g->pack_update) {                                      \
            (s) = PMIX_SUCCESS;                                             \
        } else {                                                            \
            (s) = _g->pack_update(p, b);                                    \
        }                                                                   \
    } while (0)

/**
 * Take delivery of what pack_update() produced.
 *
 * The client half of the above, resolved from the server peer that sent
 * it - which is the peer carrying this client's assigned module.
 *
 * @param buff  the buffer that was sent
 *
 * @return PMIX_SUCCESS on success.
 */
typedef pmix_status_t (*pmix_gds_base_module_accept_update_fn_t)(pmix_buffer_t *buff);

#define PMIX_GDS_ACCEPT_UPDATE(s, p, b)                                     \
    do {                                                                    \
        pmix_gds_base_module_t *_g = PMIX_GDS_PEER_MODULE(p);               \
        if (NULL == _g->accept_update) {                                    \
            (s) = PMIX_SUCCESS;                                             \
        } else {                                                            \
            (s) = _g->accept_update(b);                                     \
        }                                                                   \
    } while (0)

/**
 * Add job-level data to a namespace that is already registered.
 *
 * The global resource cache (pmix_server_globals.gdata) is copied into a
 * namespace's datastore once, when that namespace is first registered.
 * A host adding to it afterwards - PMIx_server_register_resources on a
 * running system - therefore governs only the namespaces registered
 * after it, and every job already running keeps the description it was
 * given. This is how that addition reaches them.
 *
 * A fan-out across every active module, like add_nspace and for the
 * same reason: more than one module can be holding a namespace's job
 * data at once. A server assigns ITSELF "hash", so its own copy lives
 * there, while the segments its clients read are gds/shmem3's - both
 * have to learn of the addition, and neither is "the" module for the
 * namespace. Each module decides for itself whether it holds the
 * namespace named.
 *
 * Optional. A module that holds no job data leaves it NULL and the
 * macro treats that as success.
 *
 * @param nspace  the namespace being added to
 * @param info    the job-level information to add
 * @param ninfo   number of elements in info
 *
 * @return PMIX_SUCCESS on success.
 */
typedef pmix_status_t (*pmix_gds_base_module_add_job_data_fn_t)(const char *nspace,
                                                                pmix_info_t info[],
                                                                size_t ninfo);

#define PMIX_GDS_ADD_JOB_DATA(s, n, i, ni)                                  \
    do {                                                                    \
        pmix_gds_base_active_module_t *_g;                                  \
        pmix_status_t _s = PMIX_SUCCESS;                                    \
        (s) = PMIX_SUCCESS;                                                 \
        pmix_output_verbose(1, pmix_gds_base_output,                        \
                            "[%s:%d] GDS ADD JOB DATA for %s",              \
                            __FILE__, __LINE__, (n));                       \
        PMIX_LIST_FOREACH (_g, &pmix_gds_globals.actives,                   \
                           pmix_gds_base_active_module_t) {                 \
            if (NULL == _g->module->add_job_data) {                         \
                continue;                                                   \
            }                                                               \
            _s = _g->module->add_job_data((n), (i), (ni));                  \
            if (PMIX_SUCCESS != _s) {                                       \
                (s) = PMIX_ERROR;                                           \
            }                                                               \
        }                                                                   \
    } while (0)

/**
 * Note that a key has been removed, so this module stops answering for
 * it. Optional: a module whose own store already handled the removal -
 * gds/hash, which takes a delete through its store slot like any other
 * scope - leaves this NULL and the macro treats that as success.
 *
 * It exists for a module that keeps data somewhere its store cannot
 * reach. gds/shmem3 publishes into shared segments that are never
 * written again once a client can see them, so it cannot take the key
 * out; it records the removal instead and stops answering for it.
 *
 * @param proc  the process whose data is affected. PMIX_RANK_WILDCARD
 *              names the job-level data.
 * @param key   the key to stop answering for
 *
 * @return PMIX_SUCCESS on success.
 */
typedef pmix_status_t (*pmix_gds_base_module_del_key_fn_t)(const pmix_proc_t *proc,
                                                           const char *key);

/* Convenience macro for del_key, resolved against the module serving a
 * given namespace rather than a peer - the caller is correcting what
 * that namespace holds, and may have no peer in hand. A NULL slot is
 * success: the module has nothing of its own to correct. */
#define PMIX_GDS_DEL_KEY(s, ns, pc, k)                                                      \
    do {                                                                                    \
        pmix_gds_base_module_t *_g;                                                         \
        (s) = PMIX_SUCCESS;                                                                 \
        if (NULL != (ns) && NULL != (ns)->compat.gds) {                                     \
            _g = (ns)->compat.gds;                                                          \
            pmix_output_verbose(1, pmix_gds_base_output,                                    \
                                "[%s:%d] GDS DEL KEY %s for %s", __FILE__, __LINE__,        \
                                (NULL == (k)) ? "NULL" : (k), _g->name);                    \
            if (NULL != _g->del_key) {                                                      \
                (s) = _g->del_key((pc), (k));                                               \
            }                                                                               \
        }                                                                                   \
    } while (0)

/* define a convenience macro for is_tsafe for fetch operation */
#define PMIX_GDS_FETCH_IS_TSAFE(s, p)                         \
    do {                                                      \
        pmix_gds_base_module_t *_g = PMIX_GDS_PEER_MODULE(p); \
        pmix_output_verbose(1, pmix_gds_base_output,          \
                            "[%s:%d] GDS FETCH IS THREAD "    \
                            "SAFE WITH %s",                   \
                            __FILE__, __LINE__, _g->name);    \
        if (true == _g->is_tsafe) {                           \
            (s) = PMIX_SUCCESS;                               \
        } else {                                              \
            (s) = PMIX_ERR_NOT_SUPPORTED;                     \
        }                                                     \
    } while(0)

typedef pmix_status_t (*pmix_gds_base_module_fetch_array_fn_t)(struct pmix_peer_t *pr,
                                                               pmix_buffer_t *reply);
/* define a convenience macro for fetching array info for
 * a given peer */
#define PMIX_GDS_FETCH_INFO_ARRAYS(s, p, b)                                 \
    do {                                                                    \
        pmix_gds_base_module_t *_g = pmix_globals.mypeer->nptr->compat.gds; \
        pmix_output_verbose(1, pmix_gds_base_output,                        \
                            "[%s:%d] GDS FETCH ARRAYS WITH %s",             \
                            __FILE__, __LINE__, _g->name);                  \
        if (NULL == _g->fetch_arrays) {                                     \
            /* gds/shmem3 really does leave this slot empty */              \
            (s) = PMIX_ERR_NOT_SUPPORTED;                                   \
        } else {                                                            \
            (s) = _g->fetch_arrays((struct pmix_peer_t*)(p), b);            \
        }                                                                   \
    } while(0)


/* structure for gds modules */
typedef struct {
    const char *name;
    const bool is_tsafe;
    pmix_gds_base_module_init_fn_t                  init;
    pmix_gds_base_module_fini_fn_t                  finalize;
    pmix_gds_base_assign_module_fn_t                assign_module;
    pmix_gds_base_module_cache_job_info_fn_t        cache_job_info;
    pmix_gds_base_module_register_job_info_fn_t     register_job_info;
    pmix_gds_base_module_store_job_info_fn_t        store_job_info;
    pmix_gds_base_module_store_fn_t                 store;
    pmix_gds_base_module_store_modex_fn_t           store_modex;
    pmix_gds_base_module_fetch_fn_t                 fetch;
    pmix_gds_base_module_setup_fork_fn_t            setup_fork;
    pmix_gds_base_module_add_nspace_fn_t            add_nspace;
    pmix_gds_base_module_del_nspace_fn_t            del_nspace;
    pmix_gds_base_module_del_key_fn_t               del_key;
    pmix_gds_base_module_assemb_kvs_req_fn_t        assemb_kvs_req;
    pmix_gds_base_module_accept_kvs_resp_fn_t       accept_kvs_resp;
    pmix_gds_base_module_fetch_array_fn_t           fetch_arrays;
    pmix_gds_base_module_mark_modex_complete_fn_t   mark_modex_complete;
    pmix_gds_base_module_recv_modex_complete_fn_t   recv_modex_complete;
    /* Appended, never inserted: this struct is installed under
     * $(pmixincludedir)/src/mca/gds and an out-of-tree component may
     * initialize it positionally. New slots go here, and the framework
     * version above is bumped so a component built against an older
     * layout is refused rather than misread. */
    pmix_gds_base_module_add_session_fn_t           add_session;
    pmix_gds_base_module_del_session_fn_t           del_session;
    pmix_gds_base_module_pack_update_fn_t           pack_update;
    pmix_gds_base_module_accept_update_fn_t         accept_update;
    pmix_gds_base_module_add_job_data_fn_t          add_job_data;
} pmix_gds_base_module_t;

/* NOTE: there is no public GDS interface structure - all access is
 * done directly to/from an assigned module */

/* define the component structure */
typedef pmix_mca_base_component_t pmix_gds_base_component_t;

/* The gds framework interface version. It is stated here and nowhere
 * else: components stamp it into their struct with
 * PMIX_MCA_BASE_VERSION(gds), and the framework's declaration reaches
 * the same three by pasting its name, so the two cannot drift apart.
 * Bump it on any change to the module interface that a component built
 * against the previous one would not survive. */
#define PMIX_MCA_gds_MAJOR_VERSION   2
#define PMIX_MCA_gds_MINOR_VERSION   0
#define PMIX_MCA_gds_RELEASE_VERSION 0

END_C_DECLS

#endif
