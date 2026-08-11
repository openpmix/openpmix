/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2014      Artem Y. Polyakov <artpol84@gmail.com>.
 *                         All rights reserved.
 * Copyright (c) 2016      Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2016      IBM Corporation.  All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "src/include/pmix_config.h"

#include "src/include/pmix_prefetch.h"

#include "src/include/pmix_stdint.h"

#include "include/pmix.h"

#include "src/include/pmix_globals.h"
#include "src/mca/gds/base/base.h"

#ifdef HAVE_STRING_H
#    include <string.h>
#endif
#include <fcntl.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif
#ifdef HAVE_SYS_SOCKET_H
#    include <sys/socket.h>
#endif
#ifdef HAVE_SYS_UN_H
#    include <sys/un.h>
#endif
#ifdef HAVE_SYS_UIO_H
#    include <sys/uio.h>
#endif
#ifdef HAVE_SYS_TYPES_H
#    include <sys/types.h>
#endif
#include <event.h>

#include "src/class/pmix_list.h"
#include "src/mca/bfrops/bfrops.h"
#include "src/mca/gds/gds.h"
#include "src/mca/ptl/base/base.h"
#include "src/threads/pmix_threads.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_printf.h"

#include "src/client/pmix_client_ops.h"
#include "src/server/pmix_server_ops.h"

/* local object definition */
typedef struct {
    pmix_object_t super;
    pmix_event_t ev;
    pmix_lock_t lock;
    pmix_status_t status;
    char *nodename;
    pmix_nspace_t nspace;
    pmix_proc_t *procs;
    size_t nprocs;
    char *nodelist;
} pmix_resolve_caddy_t;
static void rcon(pmix_resolve_caddy_t *p)
{
    PMIX_CONSTRUCT_LOCK(&p->lock);
    p->nodename = NULL;
    PMIX_LOAD_NSPACE(p->nspace, NULL);
    p->procs = NULL;
    p->nprocs = 0;
    p->nodelist = NULL;
}
static void rdes(pmix_resolve_caddy_t *p)
{
    PMIX_DESTRUCT_LOCK(&p->lock);
    if (NULL != p->procs) {
        PMIX_PROC_FREE(p->procs, p->nprocs);
    }
    if (NULL != p->nodelist) {
        free(p->nodelist);
    }
}
static PMIX_CLASS_INSTANCE(pmix_resolve_caddy_t,
                           pmix_object_t,
                           rcon, rdes);

/* callback for wait completion */
static void wait_peers_cbfunc(struct pmix_peer_t *pr, pmix_ptl_hdr_t *hdr,
                              pmix_buffer_t *buf, void *cbdata);
static void wait_node_cbfunc(struct pmix_peer_t *pr, pmix_ptl_hdr_t *hdr,
                             pmix_buffer_t *buf, void *cbdata);


static pmix_status_t try_fetch(pmix_cb_t *cb)
{
    pmix_status_t rc;

    // first try the client's store
    PMIX_GDS_FETCH_KV(rc, pmix_client_globals.myserver, cb);
    if (PMIX_SUCCESS == rc || PMIX_OPERATION_SUCCEEDED == rc) {
        return PMIX_SUCCESS;
    }

    if (!PMIX_GDS_CHECK_COMPONENT(pmix_client_globals.myserver, "hash")) {
        // try again with our hash store
        PMIX_GDS_FETCH_KV(rc, pmix_globals.mypeer, cb);
        if (PMIX_SUCCESS == rc || PMIX_OPERATION_SUCCEEDED == rc) {
            return PMIX_SUCCESS;
        }
    }

    // if the namespace isn't known, then abort
    if (PMIX_ERR_INVALID_NAMESPACE == rc) {
        return rc;
    }

    if (PMIX_RANK_UNDEF != cb->proc->rank) {
        return PMIX_ERR_NOT_FOUND;
    }

    // try again with wildcard
    cb->proc->rank = PMIX_RANK_WILDCARD;

    PMIX_GDS_FETCH_KV(rc, pmix_client_globals.myserver, cb);
    if (PMIX_SUCCESS == rc || PMIX_OPERATION_SUCCEEDED == rc) {
        return PMIX_SUCCESS;
    }

    if (!PMIX_GDS_CHECK_COMPONENT(pmix_client_globals.myserver, "hash")) {
        // try again with our hash store
        PMIX_GDS_FETCH_KV(rc, pmix_globals.mypeer, cb);
        if (PMIX_SUCCESS == rc || PMIX_OPERATION_SUCCEEDED == rc) {
            return PMIX_SUCCESS;
        }
    }

    // get here if not found
    return PMIX_ERR_NOT_FOUND;
}

static void resolve_peers(int sd, short args, void *cbdata)
{
    pmix_resolve_caddy_t *cd = (pmix_resolve_caddy_t*)cbdata;
    pmix_cb_t cb;
    pmix_info_t info[3];
    pmix_status_t rc;
    pmix_proc_t proc;
    pmix_value_t *val;
    char **p, **tmp = NULL, *prs;
    pmix_proc_t *pa;
    size_t m, n, np, ninfo;
    int npeers;
    pmix_namespace_t *ns;
    char *key;
    pmix_kval_t *kv;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    // restrict our search to already available info - do
    // not allow the search to call up to the server. This
    // avoids a threadlock situation
    PMIX_INFO_LOAD(&info[0], PMIX_OPTIONAL, NULL, PMIX_BOOL);

    /* if I am a client and my server is earlier than v3.2.x, then
     * I need to look for this data under rank=PMIX_RANK_WILDCARD
     * with a key equal to the nodename */
    if (PMIX_PEER_IS_CLIENT(pmix_globals.mypeer) &&
        PMIX_PEER_IS_EARLIER(pmix_client_globals.myserver, 3, 1, 100)) {
        /* NOTE: pre-v3.2 servers filed this under the wildcard rank keyed by
         * the node name, but the reset below puts the rank back to UNDEF for
         * both branches, so this assignment has no effect. try_fetch() retries
         * an UNDEF rank as WILDCARD, which is why the legacy path still
         * resolves; the dead store is left as-is rather than "fixed" blind,
         * since no pre-v3.2 server is available to test the difference. */
        proc.rank = PMIX_RANK_WILDCARD;
        key = cd->nodename;
        ninfo = 1;
    } else {
        proc.rank = PMIX_RANK_UNDEF;
        key = PMIX_LOCAL_PEERS;
        PMIX_INFO_LOAD(&info[1], PMIX_NODE_INFO, NULL, PMIX_BOOL);
        PMIX_INFO_LOAD(&info[2], PMIX_HOSTNAME, cd->nodename, PMIX_STRING);
        ninfo = 3;
    }

    PMIX_CONSTRUCT(&cb, pmix_cb_t);
    proc.rank = PMIX_RANK_UNDEF;
    cb.proc = &proc;
    cb.key = key;
    cb.scope = PMIX_INTERNAL;
    cb.info = info;
    cb.ninfo = ninfo;

    if (0 == pmix_nslen(cd->nspace)) {
        rc = PMIX_ERR_DATA_VALUE_NOT_FOUND;
        np = 0;
        /* cycle across all known nspaces and aggregate the results */
        PMIX_LIST_FOREACH (ns, &pmix_globals.nspaces, pmix_namespace_t) {
            PMIX_LOAD_NSPACE(proc.nspace, ns->nspace);
            /* restart each namespace from UNDEF: try_fetch() mutates the
             * rank to WILDCARD for its retry and does not put it back, and
             * it declines outright for a rank that is not UNDEF - so
             * leaving it mutated denied every subsequent namespace its own
             * first-choice lookup, making the answer depend on the order
             * the namespaces happen to sit in the list. The server's
             * pmix_server_locally_resolve_peers() resets it for the same
             * reason */
            proc.rank = PMIX_RANK_UNDEF;
            rc = try_fetch(&cb);
            if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
                continue;
            }

            /* sanity check */
            if (0 == pmix_list_get_size(&cb.kvs)) {
                continue;
            }
            kv = (pmix_kval_t*)pmix_list_get_first(&cb.kvs);
            val = kv->value;
            if (PMIX_STRING != val->type) {
                PMIX_ERROR_LOG(PMIX_ERR_INVALID_VAL);
                PMIX_LIST_DESTRUCT(&cb.kvs);
                PMIX_CONSTRUCT(&cb.kvs, pmix_list_t);
                continue;
            }
            if (NULL == val->data.string) {
                /* no local peers on this node */
                PMIX_LIST_DESTRUCT(&cb.kvs);
                PMIX_CONSTRUCT(&cb.kvs, pmix_list_t);
                continue;
            }
            /* count the peers BEFORE recording the entry. An nspace can have
             * this node in its map with no procs of its own on it, and the
             * host reports that as an empty peer list. The transfer pass
             * below splits every recorded entry back apart, and
             * PMIx_Argv_split() of a string with no tokens returns NULL -
             * so recording an empty one dereferences that NULL the moment
             * any other nspace contributes a proc and the pass runs. */
            p = PMIx_Argv_split(val->data.string, ',');
            npeers = PMIx_Argv_count(p);
            PMIx_Argv_free(p);
            if (0 == npeers) {
                /* no local peers on this node for this nspace */
                PMIX_LIST_DESTRUCT(&cb.kvs);
                PMIX_CONSTRUCT(&cb.kvs, pmix_list_t);
                continue;
            }
            /* prepend the nspace */
            if (PMIX_UNLIKELY(0 > pmix_asprintf(&prs, "%s:%s", ns->nspace, val->data.string))) {
                PMIX_LIST_DESTRUCT(&cb.kvs);
                PMIX_CONSTRUCT(&cb.kvs, pmix_list_t);
                continue;
            }
            /* add to our list of results */
            PMIx_Argv_append_nosize(&tmp, prs);
            np += npeers;
            /* done with this entry */
            free(prs);
            // clean up and cycle around to next namespace
            PMIX_LIST_DESTRUCT(&cb.kvs);
            PMIX_CONSTRUCT(&cb.kvs, pmix_list_t);
        }
        if (0 < np) {
            /* allocate the proc array */
            PMIX_PROC_CREATE(pa, np);
            if (PMIX_UNLIKELY(NULL == pa)) {
                rc = PMIX_ERR_NOMEM;
                PMIx_Argv_free(tmp);
                goto done;
            }
            cd->procs = pa;
            /* transfer the results. Note that nprocs is recorded from the
             * transfer below, not from the count above: the two disagree
             * if an append into tmp failed, and the difference would be
             * handed to the caller as procs it may index */
            np = 0;
            for (n = 0; NULL != tmp[n]; n++) {
                /* find the nspace delimiter */
                prs = strchr(tmp[n], ':');
                if (PMIX_UNLIKELY(NULL == prs)) {
                    /* should never happen, but silence a Coverity warning */
                    rc = PMIX_ERR_BAD_PARAM;
                    PMIx_Argv_free(tmp);
                    PMIX_PROC_FREE(pa, np);
                    cd->procs = NULL;
                    cd->nprocs = 0;
                    goto done;
                }
                *prs = '\0';
                ++prs;
                p = PMIx_Argv_split(prs, ',');
                for (m = 0; NULL != p[m]; m++) {
                    PMIX_LOAD_NSPACE(pa[np].nspace, tmp[n]);
                    pa[np].rank = strtoul(p[m], NULL, 10);
                    ++np;
                }
                PMIx_Argv_free(p);
            }
            PMIx_Argv_free(tmp);
            cd->nprocs = np;
            rc = PMIX_SUCCESS;
        } else {
            /* nothing resolved - an entry is recorded only when it carried
             * at least one peer, so tmp is empty here, but free it rather
             * than depend on that */
            PMIx_Argv_free(tmp);
        }
        goto done;
    }

    /* get the list of local peers for this nspace and node */
    PMIX_LOAD_PROCID(&proc, cd->nspace, PMIX_RANK_UNDEF);

    rc = try_fetch(&cb);
    if (PMIX_SUCCESS == rc) {
        goto process;
    }
    if (PMIX_ERR_INVALID_NAMESPACE == rc) {
        // this namespace is unknown
        goto done;
    }
    if (PMIX_ERR_NOT_FOUND == rc) {
        // found the namespace, but the node is
        // not present on that namespace - the
        // default response is correct
        rc = PMIX_SUCCESS;
        goto done;
    }
    if (PMIX_ERR_DATA_VALUE_NOT_FOUND == rc) {
        // found the namespace and node, but the
        // host did not provide the information
        goto done;
    }

process:
    /* sanity check */
    if (0 == pmix_list_get_size(&cb.kvs)) {
        rc = PMIX_ERR_INVALID_VAL;
        goto done;
    }
    kv = (pmix_kval_t*)pmix_list_get_first(&cb.kvs);
    val = kv->value;
    if (PMIX_STRING != val->type || NULL == val->data.string) {
        rc = PMIX_ERR_INVALID_VAL;
        goto done;
    }

    /* split the procs to get a list */
    p = PMIx_Argv_split(val->data.string, ',');
    np = PMIx_Argv_count(p);
    if (0 == np) {
        /* the node is in this nspace's map but hosts none of its procs -
         * the documented empty answer, not an error. It has to be caught
         * here because PMIX_PROC_CREATE(0) hands back exactly the NULL an
         * allocation failure does, and the check below would report the
         * empty result as PMIX_ERR_NOMEM */
        PMIx_Argv_free(p);
        goto done;
    }

    /* allocate the proc array */
    PMIX_PROC_CREATE(pa, np);
    if (PMIX_UNLIKELY(NULL == pa)) {
        rc = PMIX_ERR_NOMEM;
        PMIx_Argv_free(p);
        goto done;
    }
    /* transfer the results */
    for (n = 0; n < np; n++) {
        PMIX_LOAD_NSPACE(pa[n].nspace, cd->nspace);
        pa[n].rank = strtoul(p[n], NULL, 10);
    }
    PMIx_Argv_free(p);
    cd->procs = pa;
    cd->nprocs = np;

done:
    for (n=0; n < ninfo; n++) {
        PMIX_INFO_DESTRUCT(&info[n]);
    }
    PMIX_DESTRUCT(&cb);
    cd->status = rc;
    PMIX_POST_OBJECT(cd);
    PMIX_WAKEUP_THREAD(&cd->lock);
}

static void respeer(pmix_status_t status, pmix_info_t info[], size_t ninfo, void *cbdata,
                    pmix_release_cbfunc_t release_fn, void *release_cbdata)
{
    pmix_cb_t *cb = (pmix_cb_t*)cbdata;
    size_t n;

    PMIX_ACQUIRE_OBJECT(cb);

    cb->status = status;
    if (PMIX_SUCCESS == status && 0 < ninfo) {
        PMIX_INFO_CREATE(cb->info, ninfo);
        if (PMIX_UNLIKELY(NULL == cb->info)) {
            cb->status = PMIX_ERR_NOMEM;
        } else {
            cb->ninfo = ninfo;
            /* this is our copy, so mark it as such - otherwise the caddy
             * destructor leaves the whole array behind */
            cb->infocopy = true;
            for (n=0; n < ninfo; n++) {
                PMIX_INFO_XFER(&cb->info[n], &info[n]);
            }
        }
    }
    if (NULL != release_fn) {
        release_fn(release_cbdata);
    }
    PMIX_POST_OBJECT(cb);
    PMIX_WAKEUP_THREAD(&cb->lock);
}

PMIX_EXPORT pmix_status_t PMIx_Resolve_peers(const char *nodename, const pmix_nspace_t nspace,
                                             pmix_proc_t **procs, size_t *nprocs)
{
    pmix_resolve_caddy_t *cd;
    pmix_buffer_t *msg;
    pmix_status_t rc;
    pmix_cmd_t cmd = PMIX_RESOLVE_PEERS_CMD;
    char *nd, *str;
    pmix_info_t *iptr;
    pmix_query_t query;
    pmix_cb_t cb;
    pmix_value_t *val;

    /* both are OUT parameters we write on every path - there is no way to
     * return an answer without them */
    if (PMIX_UNLIKELY(NULL == procs || NULL == nprocs)) {
        return PMIX_ERR_BAD_PARAM;
    }

    /* set default response */
    *procs = NULL;
    *nprocs = 0;

    if (PMIX_UNLIKELY(!pmix_atomic_check_bool(&pmix_globals.initialized))) {
        return PMIX_ERR_INIT;
    }

    if (PMIX_UNLIKELY(pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped))) {
        return PMIX_ERR_NOT_AVAILABLE;
    }

    // if the nodename is NULL, then they are asking for our
    // local host
    if (NULL == nodename) {
        nd = pmix_globals.hostname;
    } else {
        nd = (char*)nodename;
    }

    // if we are a server, see if our host can answer the
    // request since it should have more global knowledge
    if (PMIX_PEER_IS_SERVER(pmix_globals.mypeer)) {
        if (NULL != pmix_host_server.query) {
            PMIX_QUERY_CONSTRUCT(&query);
            PMIX_ARGV_APPEND(rc, query.keys, PMIX_QUERY_RESOLVE_PEERS);
            PMIX_INFO_CREATE(iptr, 2);
            if (PMIX_UNLIKELY(PMIX_SUCCESS != rc || NULL == iptr)) {
                /* we cannot describe the request, so there is no point in
                 * making it - fall through and work it out for ourselves */
                if (NULL != iptr) {
                    PMIX_INFO_FREE(iptr, 2);
                }
                PMIX_QUERY_DESTRUCT(&query);
                goto local;
            }
            str = (char*)nspace;
            PMIX_INFO_LOAD(&iptr[0], PMIX_NSPACE, str, PMIX_STRING);
            PMIX_INFO_SET_QUALIFIER(&iptr[0]);
            PMIX_INFO_LOAD(&iptr[1], PMIX_HOSTNAME, nd, PMIX_STRING);
            PMIX_INFO_SET_QUALIFIER(&iptr[1]);
            query.qualifiers = iptr;
            query.nqual = 2;
            PMIX_CONSTRUCT(&cb, pmix_cb_t);
            rc = pmix_host_server.query(&pmix_globals.myid,
                                        &query, 1,
                                        respeer, (void*)&cb);
            if (PMIX_SUCCESS == rc) {
                // wait for the response
                PMIX_WAIT_THREAD(&cb.lock);
                rc = cb.status;
            }
            // done with the query inputs regardless of outcome - the host
            // is finished with them once the callback fired (success) or
            // the call failed synchronously (no callback)
            PMIX_QUERY_DESTRUCT(&query);
            if (PMIX_SUCCESS == rc) {
                // string return should be in first info
                if (0 < cb.ninfo) {
                    val = &cb.info[0].value;
                    if (PMIX_DATA_ARRAY != val->type ||
                        NULL == val->data.darray ||
                        PMIX_PROC != val->data.darray->type) {
                        PMIX_ERROR_LOG(PMIX_ERR_INVALID_VAL);
                    } else {
                        *procs = (pmix_proc_t*)val->data.darray->array;
                        *nprocs = val->data.darray->size;
                        /* detach just the proc array we are handing back, so
                         * destructing the caddy still reclaims the info array
                         * and the data-array wrapper around it */
                        val->data.darray->array = NULL;
                        val->data.darray->size = 0;
                        PMIX_DESTRUCT(&cb);
                        return rc;
                    }
                }
            }
            PMIX_DESTRUCT(&cb);
        }
    local:
        // if we get here, then the host wasn't able to
        // provide us with what we need, so let's do it ourselves
        cd = PMIX_NEW(pmix_resolve_caddy_t);
        cd->nodename = nd;
        PMIX_LOAD_NSPACE(cd->nspace, nspace);
        PMIX_THREADSHIFT(cd, resolve_peers);
        PMIX_WAIT_THREAD(&cd->lock);
        rc = cd->status;
        if (PMIX_SUCCESS == rc) {
            *procs = cd->procs;
            *nprocs = cd->nprocs;
            // protect the returning values
            cd->procs = NULL;
            cd->nprocs = 0;
        }
        PMIX_RELEASE(cd);
        return rc;
    }

    /* if we aren't connected, don't attempt to send - just
     * provide our local understanding of the situation.
     * Threadshift to compute it as we will be accessing
     * global info */
    if (PMIX_UNLIKELY(!pmix_atomic_check_bool(&pmix_globals.connected))) {
        cd = PMIX_NEW(pmix_resolve_caddy_t);
        cd->nodename = nd;
        PMIX_LOAD_NSPACE(cd->nspace, nspace);
        PMIX_THREADSHIFT(cd, resolve_peers);
        PMIX_WAIT_THREAD(&cd->lock);
        rc = cd->status;
        if (PMIX_SUCCESS == rc) {
            *procs = cd->procs;
            *nprocs = cd->nprocs;
            // protect the returning values
            cd->procs = NULL;
            cd->nprocs = 0;
        }
        PMIX_RELEASE(cd);
        return rc;
    }

    // send the request to our server as they have a more
    // global view of the situation than we might
    msg = PMIX_NEW(pmix_buffer_t);
    /* pack the cmd */
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &cmd, 1, PMIX_COMMAND);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        return rc;
    }
    /* pack the request information */
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &nd, 1, PMIX_STRING);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        return rc;
    }
    str = (char*)nspace;
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &str, 1, PMIX_STRING);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        return rc;
    }

    cd = PMIX_NEW(pmix_resolve_caddy_t);
    PMIX_PTL_SEND_RECV(rc, pmix_client_globals.myserver, msg, wait_peers_cbfunc, (void *) cd);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        /* a refused send never took the message, so it is still ours */
        PMIX_RELEASE(msg);
        PMIX_RELEASE(cd);
        return rc;
    }
    PMIX_WAIT_THREAD(&cd->lock);
    rc = cd->status;
    if (PMIX_SUCCESS == rc) {
        *procs = cd->procs;
        *nprocs = cd->nprocs;
        // protect the returning values
        cd->procs = NULL;
        cd->nprocs = 0;
    } else {
        // if the server couldn't process it, that's likely because it
        // is an older version that doesn't recognize the cmd. So
        // process it ourselves. Note that we reuse the caddy here, so
        // wait_peers_cbfunc() has to leave procs/nprocs agreeing with
        // each other on every failure path - a count left standing over
        // a released array would be handed straight to the caller
        cd->nodename = nd;
        PMIX_LOAD_NSPACE(cd->nspace, nspace);
        // reset the lock
        PMIX_DESTRUCT_LOCK(&cd->lock);
        PMIX_CONSTRUCT_LOCK(&cd->lock);
        PMIX_THREADSHIFT(cd, resolve_peers);
        PMIX_WAIT_THREAD(&cd->lock);
        rc = cd->status;
        if (PMIX_SUCCESS == rc) {
            *procs = cd->procs;
            *nprocs = cd->nprocs;
            // protect the returning values
            cd->procs = NULL;
            cd->nprocs = 0;
        }
    }
    PMIX_RELEASE(cd);
    return rc;
}

static void resolve_nodes(int sd, short args, void *cbdata)
{
    pmix_resolve_caddy_t *cd = (pmix_resolve_caddy_t*)cbdata;
    pmix_cb_t cb;
    pmix_info_t info;
    pmix_status_t rc;
    pmix_proc_t proc;
    pmix_kval_t *kv;
    pmix_value_t *val = NULL;
    char **tmp = NULL, **p;
    size_t n;
    pmix_namespace_t *ns;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_ACQUIRE_OBJECT(cd);

    /* get the list of nodes for this nspace */
    proc.rank = PMIX_RANK_WILDCARD;

    // restrict our search to already available info - do
    // not allow the search to call up to the server. This
    // avoids a threadlock situation
    PMIX_INFO_LOAD(&info, PMIX_OPTIONAL, NULL, PMIX_BOOL);

    PMIX_CONSTRUCT(&cb, pmix_cb_t);
    cb.proc = &proc;
    cb.key = PMIX_NODE_LIST;
    cb.scope = PMIX_INTERNAL;
    cb.info = &info;
    cb.ninfo = 1;

    if (0 == pmix_nslen(cd->nspace)) {
        /* cycle across all known nspaces and aggregate the results */
        PMIX_LIST_FOREACH (ns, &pmix_globals.nspaces, pmix_namespace_t) {
            PMIX_LOAD_NSPACE(proc.nspace, ns->nspace);
            rc = try_fetch(&cb);
            if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
                continue;
            }

            /* sanity check */
            if (0 == pmix_list_get_size(&cb.kvs)) {
                continue;
            }
            kv = (pmix_kval_t*)pmix_list_get_first(&cb.kvs);
            val = kv->value;
            if (PMIX_STRING != val->type) {
                PMIX_ERROR_LOG(PMIX_ERR_INVALID_VAL);
                PMIX_LIST_DESTRUCT(&cb.kvs);
                PMIX_CONSTRUCT(&cb.kvs, pmix_list_t);
                continue;
            }
            if (NULL == val->data.string) {
                /* no nodes found */
                PMIX_LIST_DESTRUCT(&cb.kvs);
                PMIX_CONSTRUCT(&cb.kvs, pmix_list_t);
                continue;
            }
            /* add to our list of results, ensuring uniqueness. An empty
             * node list is the same "no nodes" answer as the NULL one
             * caught above, and it has to be caught too: PMIx_Argv_split()
             * of a string with no tokens returns NULL, not an empty array */
            p = PMIx_Argv_split(val->data.string, ',');
            if (NULL != p) {
                for (n = 0; NULL != p[n]; n++) {
                    PMIx_Argv_append_unique_nosize(&tmp, p[n]);
                }
                PMIx_Argv_free(p);
            }
            PMIX_LIST_DESTRUCT(&cb.kvs);
            PMIX_CONSTRUCT(&cb.kvs, pmix_list_t);
        }
        if (0 < PMIx_Argv_count(tmp)) {
            cd->nodelist = PMIx_Argv_join(tmp, ',');
            PMIx_Argv_free(tmp);
        }
        rc = PMIX_SUCCESS;
        goto done;
    }

    // looking for a specific nspace
    PMIX_LOAD_NSPACE(proc.nspace, cd->nspace);
    rc = try_fetch(&cb);
    if (PMIX_SUCCESS == rc) {
        goto process;
    }
    if (PMIX_ERR_INVALID_NAMESPACE == rc) {
        // this namespace is unknown
        goto done;
    }
    if (PMIX_ERR_NOT_FOUND == rc) {
        // found the namespace, but the node is
        // not present on that namespace - the
        // default response is correct
        rc = PMIX_SUCCESS;
        goto done;
    }
    if (PMIX_ERR_DATA_VALUE_NOT_FOUND == rc) {
        // found the namespace and node, but the
        // host did not provide the information
        goto done;
    }

process:
    /* sanity check */
    if (0 == pmix_list_get_size(&cb.kvs)) {
        goto done;
    }
    kv = (pmix_kval_t*)pmix_list_get_first(&cb.kvs);
    val = kv->value;
    if (PMIX_STRING != val->type) {
        /* report it as well as log it - rc is PMIX_SUCCESS on the way in
         * here, so falling through left the caller being told the call
         * succeeded with a NULL nodelist it is entitled to parse */
        PMIX_ERROR_LOG(PMIX_ERR_INVALID_VAL);
        rc = PMIX_ERR_INVALID_VAL;
        goto done;
    }
    if (NULL == val->data.string) {
        /* no nodes recorded for this nspace - the default (empty) response
         * is the correct answer, exactly as in the aggregate walk above */
        goto done;
    }
    cd->nodelist = strdup(val->data.string);
    if (PMIX_UNLIKELY(NULL == cd->nodelist)) {
        rc = PMIX_ERR_NOMEM;
    }

done:
    /* pass back the result */
    cd->status = rc;
    // protect the field
    cb.info = NULL;
    cb.ninfo = 0;
    PMIX_DESTRUCT(&cb);
    PMIX_POST_OBJECT(cd);
    PMIX_WAKEUP_THREAD(&cd->lock);
}

static void resnode(pmix_status_t status, pmix_info_t info[], size_t ninfo, void *cbdata,
                    pmix_release_cbfunc_t release_fn, void *release_cbdata)
{
    pmix_cb_t *cb = (pmix_cb_t*)cbdata;
    size_t n;

    PMIX_ACQUIRE_OBJECT(cb);

    cb->status = status;
    if (PMIX_SUCCESS == status && 0 < ninfo) {
        PMIX_INFO_CREATE(cb->info, ninfo);
        if (PMIX_UNLIKELY(NULL == cb->info)) {
            cb->status = PMIX_ERR_NOMEM;
        } else {
            cb->ninfo = ninfo;
            /* our copy - see respeer() above */
            cb->infocopy = true;
            for (n=0; n < ninfo; n++) {
                PMIX_INFO_XFER(&cb->info[n], &info[n]);
            }
        }
    }
    if (NULL != release_fn) {
        release_fn(release_cbdata);
    }
    PMIX_POST_OBJECT(cb);
    PMIX_WAKEUP_THREAD(&cb->lock);
}

PMIX_EXPORT pmix_status_t PMIx_Resolve_nodes(const pmix_nspace_t nspace, char **nodelist)
{
    pmix_resolve_caddy_t *cd;
    pmix_status_t rc;
    pmix_buffer_t *msg = NULL;
    pmix_cmd_t cmd = PMIX_RESOLVE_NODE_CMD;
    char *str;
    pmix_query_t query;
    pmix_info_t info;
    pmix_cb_t cb;
    pmix_value_t *val;

    /* the OUT parameter is how the answer gets back, and we write it on
     * every path - a NULL leaves us nothing to do */
    if (PMIX_UNLIKELY(NULL == nodelist)) {
        return PMIX_ERR_BAD_PARAM;
    }

    /* set default response */
    *nodelist = NULL;

    if (PMIX_UNLIKELY(!pmix_atomic_check_bool(&pmix_globals.initialized))) {
        return PMIX_ERR_INIT;
    }

    if (PMIX_UNLIKELY(pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped))) {
        return PMIX_ERR_NOT_AVAILABLE;
    }

    // if we are a server, see if our host can answer the
    // request since it should have more global knowledge
    if (PMIX_PEER_IS_SERVER(pmix_globals.mypeer)) {
        if (NULL != pmix_host_server.query) {
            PMIX_QUERY_CONSTRUCT(&query);
            PMIX_ARGV_APPEND(rc, query.keys, PMIX_QUERY_RESOLVE_NODE);
            if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
                /* we cannot name what we are asking for, so there is no
                 * point in asking - work it out for ourselves instead */
                PMIX_QUERY_DESTRUCT(&query);
                goto local;
            }
            str = (char*)nspace;
            PMIX_INFO_LOAD(&info, PMIX_NSPACE, str, PMIX_STRING);
            PMIX_INFO_SET_QUALIFIER(&info);
            query.qualifiers = &info;
            query.nqual = 1;
            PMIX_CONSTRUCT(&cb, pmix_cb_t);
            rc = pmix_host_server.query(&pmix_globals.myid,
                                        &query, 1,
                                        resnode, (void*)&cb);
            if (PMIX_SUCCESS == rc) {
                // wait for the response
                PMIX_WAIT_THREAD(&cb.lock);
                rc = cb.status;
            }
            // done with the query inputs regardless of outcome. The
            // qualifier is a stack object, so detach it before destructing
            // the query, then free its contents separately
            query.qualifiers = NULL;
            query.nqual = 0;
            PMIX_QUERY_DESTRUCT(&query);
            PMIX_INFO_DESTRUCT(&info);
            if (PMIX_SUCCESS == rc) {
                // string return should be in first info
                if (0 < cb.ninfo) {
                    val = &cb.info[0].value;
                    if (PMIX_STRING != val->type || NULL == val->data.string) {
                        PMIX_ERROR_LOG(PMIX_ERR_INVALID_VAL);
                    } else {
                        *nodelist = strdup(val->data.string);
                        if (PMIX_UNLIKELY(NULL == *nodelist)) {
                            rc = PMIX_ERR_NOMEM;
                        }
                        PMIX_DESTRUCT(&cb);
                        return rc;
                    }
                }
            }
            PMIX_DESTRUCT(&cb);
        }
    local:
        // if we get here, then the host wasn't able to
        // provide us with what we need, so let's do it ourselves
        cd = PMIX_NEW(pmix_resolve_caddy_t);
        PMIX_LOAD_NSPACE(cd->nspace, nspace);
        PMIX_THREADSHIFT(cd, resolve_nodes);
        PMIX_WAIT_THREAD(&cd->lock);
        rc = cd->status;
        if (PMIX_SUCCESS == rc) {
            *nodelist = cd->nodelist;
            // protect the returned value
            cd->nodelist = NULL;
        }
        PMIX_RELEASE(cd);
        return rc;
    }

    /* if we aren't connected, don't attempt to send - just
     * provide our local understanding of the situation.
     * Threadshift to compute it as we will be accessing
     * global info */
    if (PMIX_UNLIKELY(!pmix_atomic_check_bool(&pmix_globals.connected))) {
        cd = PMIX_NEW(pmix_resolve_caddy_t);
        PMIX_LOAD_NSPACE(cd->nspace, nspace);
        PMIX_THREADSHIFT(cd, resolve_nodes);
        PMIX_WAIT_THREAD(&cd->lock);
        rc = cd->status;
        if (PMIX_SUCCESS == rc) {
            *nodelist = cd->nodelist;
            // protect the returned value
            cd->nodelist = NULL;
        }
        PMIX_RELEASE(cd);
        return rc;
    }

    // send the request to our server as they have a more
    // global view of the situation than we might
    msg = PMIX_NEW(pmix_buffer_t);
    /* pack the cmd */
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &cmd, 1, PMIX_COMMAND);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        return rc;
    }

    /* pack the request information */
    str = (char*)nspace;
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &str, 1, PMIX_STRING);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        return rc;
    }

    cd = PMIX_NEW(pmix_resolve_caddy_t);
    PMIX_PTL_SEND_RECV(rc, pmix_client_globals.myserver, msg, wait_node_cbfunc, (void *) cd);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        /* a refused send never took the message, so it is still ours */
        PMIX_RELEASE(msg);
        PMIX_RELEASE(cd);
        return rc;
    }
    PMIX_WAIT_THREAD(&cd->lock);
    rc = cd->status;
    if (PMIX_SUCCESS == rc) {
        *nodelist = cd->nodelist;
        // protect the returning values
        cd->nodelist = NULL;
    } else {
        // if the server couldn't process it, that's likely because it
        // is an older version that doesn't recognize the cmd. So
        // process it ourselves
        PMIX_LOAD_NSPACE(cd->nspace, nspace);
        // reset the lock
        PMIX_DESTRUCT_LOCK(&cd->lock);
        PMIX_CONSTRUCT_LOCK(&cd->lock);
        PMIX_THREADSHIFT(cd, resolve_nodes);
        PMIX_WAIT_THREAD(&cd->lock);
        rc = cd->status;
        if (PMIX_SUCCESS == rc) {
            *nodelist = cd->nodelist;
            // protect the returned value
            cd->nodelist = NULL;
        }
    }
    PMIX_RELEASE(cd);
    return rc;
}

static void wait_peers_cbfunc(struct pmix_peer_t *pr, pmix_ptl_hdr_t *hdr,
                              pmix_buffer_t *buf, void *cbdata)
{
    pmix_resolve_caddy_t *cd = (pmix_resolve_caddy_t*)cbdata;
    pmix_status_t rc;
    pmix_status_t ret;
    int32_t cnt;

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix:client resolve_peers callback activated with %d bytes",
                        (NULL == buf) ? -1 : (int) buf->bytes_used);
    PMIX_HIDE_UNUSED_PARAMS(pr, hdr);

    if (PMIX_UNLIKELY(NULL == buf)) {
        ret = PMIX_ERR_BAD_PARAM;
        goto done;
    }

    /* a zero-byte buffer indicates that this recv is being
     * completed due to a lost connection */
    if (PMIX_BUFFER_IS_EMPTY(buf)) {
        ret = PMIX_ERR_UNREACH;
        goto done;
    }

    /* unpack the returned status */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, pmix_client_globals.myserver, buf, &ret, &cnt, PMIX_STATUS);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        ret = rc;
    }

    /* if the status is success, then unpack the answer */
    if (PMIX_SUCCESS == ret) {
        cnt = 1;
        PMIX_BFROPS_UNPACK(rc, pmix_client_globals.myserver, buf, &cd->nprocs, &cnt, PMIX_SIZE);
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            PMIX_ERROR_LOG(rc);
            cd->nprocs = 0;
            ret = rc;
            goto done;
        }
        if (0 < cd->nprocs) {
            /* the count came off the wire, so this allocation can genuinely
             * fail */
            PMIX_PROC_CREATE(cd->procs, cd->nprocs);
            if (PMIX_UNLIKELY(NULL == cd->procs)) {
                cd->nprocs = 0;
                ret = PMIX_ERR_NOMEM;
                goto done;
            }
            cnt = cd->nprocs;
            PMIX_BFROPS_UNPACK(rc, pmix_client_globals.myserver, buf, cd->procs, &cnt, PMIX_PROC);
            if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
                PMIX_ERROR_LOG(rc);
                PMIX_PROC_FREE(cd->procs, cd->nprocs);
                cd->nprocs = 0;
                ret = rc;
                goto done;
            }
            if (0 == cnt) {
                /* the peer's count and its payload disagreed and nothing was
                 * filled in. PMIX_PROC_FREE is a no-op for a zero count, so
                 * release the block before recording that count */
                PMIX_PROC_FREE(cd->procs, cd->nprocs);
                cd->nprocs = 0;
            } else {
                /* report what actually arrived, not what the server said it
                 * was sending: the unpack succeeds having filled fewer
                 * entries when the two disagree */
                cd->nprocs = cnt;
            }
        }
    }

done:
    cd->status = ret;
    PMIX_WAKEUP_THREAD(&cd->lock);
    return;
}

static void wait_node_cbfunc(struct pmix_peer_t *pr, pmix_ptl_hdr_t *hdr,
                             pmix_buffer_t *buf, void *cbdata)
{
    pmix_resolve_caddy_t *cd = (pmix_resolve_caddy_t*)cbdata;
    pmix_status_t rc;
    pmix_status_t ret;
    int32_t cnt;

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix:client resolve_peers callback activated with %d bytes",
                        (NULL == buf) ? -1 : (int) buf->bytes_used);
    PMIX_HIDE_UNUSED_PARAMS(pr, hdr);

    if (PMIX_UNLIKELY(NULL == buf)) {
        ret = PMIX_ERR_BAD_PARAM;
        goto done;
    }

    /* a zero-byte buffer indicates that this recv is being
     * completed due to a lost connection */
    if (PMIX_BUFFER_IS_EMPTY(buf)) {
        ret = PMIX_ERR_UNREACH;
        goto done;
    }

    /* unpack the returned status */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, pmix_client_globals.myserver, buf, &ret, &cnt, PMIX_STATUS);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        ret = rc;
    }

    /* if the status is success, then unpack the answer */
    if (PMIX_SUCCESS == ret) {
        cnt = 1;
        PMIX_BFROPS_UNPACK(rc, pmix_client_globals.myserver, buf, &cd->nodelist, &cnt, PMIX_STRING);
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            PMIX_ERROR_LOG(rc);
            ret = rc;
            goto done;
        }
    }

done:
    cd->status = ret;
    PMIX_WAKEUP_THREAD(&cd->lock);
    return;
}
