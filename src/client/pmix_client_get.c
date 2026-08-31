/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2014-2021 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2014      Artem Y. Polyakov <artpol84@gmail.com>.
 *                         All rights reserved.
 * Copyright (c) 2016-2018 Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2016-2022 IBM Corporation.  All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * Copyright (c) 2023      Triad National Security, LLC. All rights reserved.
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
#include "src/mca/pcompress/base/base.h"
#include "src/mca/ptl/base/base.h"
#include "src/runtime/pmix_progress_threads.h"
#include "src/threads/pmix_threads.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_name_fns.h"
#include "src/util/pmix_output.h"

#include "pmix_client_ops.h"

static pmix_buffer_t *_pack_get(pmix_cb_t *cb,
                                pmix_rank_t rank,
                                pmix_cmd_t cmd);

static void get_data(int sd, short args, void *cbdata);

static void _getnb_cbfunc(struct pmix_peer_t *pr, pmix_ptl_hdr_t *hdr,
                          pmix_buffer_t *buf, void *cbdata);

static void _value_cbfunc(pmix_status_t status, pmix_value_t *kv, void *cbdata);

static pmix_status_t process_values(pmix_cb_t *cb);

static pmix_status_t refresh_cache(const pmix_proc_t *p);

/* Return a caddy's result list to the state a fresh one is in.
 *
 * A gds fetch may append entries and then fail, and a caddy here is
 * fetched into more than once - the retries in get_data(), and
 * _getnb_cbfunc() fetching into a caddy that get_data() already used.
 * process_values() distinguishes "the value" from "an aggregate of
 * everything this proc put" by *counting* that list, so an entry left
 * behind by a fetch that failed turns a scalar get into a data array.
 * Nothing leaks either way - cbdes() destructs the list - so the symptom
 * is a wrong answer, not a leak. */
#define DRAIN_KVS(c)                                \
    do {                                            \
        PMIX_LIST_DESTRUCT(&(c)->kvs);              \
        PMIX_CONSTRUCT(&(c)->kvs, pmix_list_t);     \
    } while (0)

/* Do these two requests name the same data?
 *
 * pmix_client_globals.pending_requests is two tables in one - the
 * coalescing table get_data() consults before it sends, and the delivery
 * table _getnb_cbfunc() walks when a reply arrives - and the two have to
 * ask it the same question. They did not: the coalescing scan used
 * PMIX_CHECK_NAMES, which reports a match whenever *either* rank is
 * PMIX_RANK_WILDCARD, while the delivery walk compares ranks exactly. So
 * a get for a specific rank issued while a get at WILDCARD for the same
 * namespace was outstanding was folded onto that request and never sent,
 * and then never matched when the reply came back. Nothing else drains
 * this list, so a blocking PMIx_Get waited forever and a PMIx_Get_nb was
 * simply never called back (its caddy is released, silently, by the
 * PMIX_LIST_DESTRUCT in PMIx_Finalize).
 *
 * Exact is the right answer for both. A reply carries the data for the
 * rank that was asked about, and job-level data - which is what a
 * WILDCARD request returns - is not any particular proc's data, so
 * coalescing across the two would answer a request the server was never
 * asked. */
static bool same_target(const char *ns1, pmix_rank_t r1,
                        const char *ns2, pmix_rank_t r2)
{
    return (r1 == r2) && PMIX_CHECK_NSPACE(ns1, ns2);
}

/* A PMIX_QUALIFIED_VALUE kval carries the value the caller actually asked for
 * as the first element of a PMIX_INFO data array, with the qualifiers behind
 * it. Three places here unwrap that, and all three used to reach straight
 * through - kv->value->data.darray->array, then iptr[0] - on nothing more
 * than the key having matched.
 *
 * The key is not enough. PMIx_Put screens the shape on the way in (see
 * _putfn), but the datastore is also filled from the server, and nothing
 * screens it on the way out; a malformed entry therefore turned a scalar into
 * a pointer, or indexed an empty array. Returns the embedded info array, or
 * NULL if this kval is not shaped like a qualified value after all. */
static pmix_info_t *qualified_value(const pmix_kval_t *kv)
{
    if (PMIX_UNLIKELY(NULL == kv->value ||
                      PMIX_DATA_ARRAY != kv->value->type ||
                      NULL == kv->value->data.darray ||
                      NULL == kv->value->data.darray->array ||
                      0 == kv->value->data.darray->size ||
                      PMIX_INFO != kv->value->data.darray->type)) {
        return NULL;
    }
    return (pmix_info_t *) kv->value->data.darray->array;
}

static pmix_status_t process_request(const pmix_proc_t *proc, const char key[],
                                     const pmix_info_t info[], size_t ninfo,
                                     pmix_get_logic_t *lg, pmix_value_t **val)
{
    pmix_status_t rc;
    pmix_value_t *ival;
    size_t n, nprocs;
    pmix_proc_t *procs;

    /* if the proc is NULL, then the caller is assuming
     * that the key is universally unique within the caller's
     * own nspace. This most likely indicates that the code
     * was originally written for a legacy version of PMI.
     *
     * If the key is NULL, then the caller wants all
     * data from the specified proc. Again, this likely
     * indicates use of a legacy version of PMI.
     *
     * Either case is supported. However, we don't currently
     * support the case where -both- values are NULL */
    if (PMIX_UNLIKELY(NULL == proc && NULL == key)) {
        pmix_output_verbose(2, pmix_client_globals.get_output,
                            "pmix: get_nb value error - both proc and key are NULL");
        return PMIX_ERR_BAD_PARAM;
    }

    /* if the key is NULL, the rank cannot be WILDCARD as
     * we cannot return all info from every rank */
    if (PMIX_UNLIKELY(NULL != proc && PMIX_RANK_WILDCARD == proc->rank && NULL == key)) {
        pmix_output_verbose(2, pmix_client_globals.get_output,
                            "pmix: get_nb value error - WILDCARD rank and key is NULL");
        return PMIX_ERR_BAD_PARAM;
    }

    if (NULL != key) {
        /* see if they are asking about a specific type of info */
        if (pmix_check_node_info(key)) {
            lg->nodeinfo = true;
        } else if (pmix_check_app_info(key)) {
            lg->appinfo = true;
        } else if (pmix_check_session_info(key)) {
            lg->sessioninfo = true;
        }
    }

    for (n = 0; n < ninfo; n++) {
        if (PMIX_CHECK_KEY(&info[n], PMIX_GET_POINTER_VALUES)) {
            /* they want a pointer to the answer */
            if (PMIX_UNLIKELY(NULL == val)) {
                return PMIX_ERR_BAD_PARAM;
            }
            lg->pntrval = PMIX_INFO_TRUE(&info[n]);
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_GET_STATIC_VALUES)) {
            /* they want a static response (i.e., they provided the storage) */
            if (PMIX_UNLIKELY(NULL == val || NULL == *val)) {
                return PMIX_ERR_BAD_PARAM;
            }
            lg->stval = PMIX_INFO_TRUE(&info[n]);
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_OPTIONAL)) {
            lg->optional = PMIX_INFO_TRUE(&info[n]);
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_IMMEDIATE)) {
            lg->immediate = PMIX_INFO_TRUE(&info[n]);
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_DATA_SCOPE)) {
            /* the key does not make the union a scope, and this qualifier
             * comes straight from the caller. Unlike its neighbors a
             * mistyped one cannot crash - a scope is only ever compared,
             * never used as an index - but it selects which table is
             * searched, so it turns a get into a confidently wrong answer.
             * Rejecting matches every other typed qualifier here */
            if (PMIX_UNLIKELY(PMIX_SCOPE != info[n].value.type)) {
                return PMIX_ERR_BAD_PARAM;
            }
            lg->scope = info[n].value.data.scope;
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_GET_REFRESH_CACHE)) {
            /* immediately query the server */
            lg->refresh_cache = PMIX_INFO_TRUE(&info[n]);
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_JOB_INFO)) {
            /* regardless of the default setting, they want us
             * to get it from the job realm */
            lg->nodeinfo = false;
            lg->appinfo = false;
            lg->sessioninfo = false;
            /* have to let the loop continue in case there are
             * other relevant directives - e.g., refresh_cache */
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_NODE_INFO)) {
            /* regardless of the default setting, they want us
             * to get it from the node realm */
            lg->nodedirective = true;
            lg->nodeinfo = true;
            lg->appinfo = false;
            lg->sessioninfo = false;
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_APP_INFO)) {
            /* regardless of the default setting, they want us
             * to get it from the app realm */
            lg->appdirective = true;
            lg->appinfo = true;
            lg->nodeinfo = false;
            lg->sessioninfo = false;
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_SESSION_INFO)) {
            /* regardless of the default setting, they want us
             * to get it from the session realm */
            lg->sessiondirective = true;
            lg->sessioninfo = true;
            lg->nodeinfo = false;
            lg->appinfo = false;
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_HOSTNAME)) {
            /* the qualifier comes straight from the caller, so verify it
             * really carries a string before handing it to strdup */
            if (PMIX_STRING != info[n].value.type ||
                NULL == info[n].value.data.string) {
                return PMIX_ERR_BAD_PARAM;
            }
            /* must copy - lgdes() frees this field, and the info array
             * belongs to the caller. Every other assignment to
             * lg->hostname strdup's for the same reason.
             *
             * Free any earlier one first: nothing stops a caller naming
             * PMIX_HOSTNAME twice, and the second strdup would otherwise
             * strand the first. */
            if (NULL != lg->hostname) {
                free(lg->hostname);
            }
            lg->hostname = strdup(info[n].value.data.string);
            if (PMIX_UNLIKELY(NULL == lg->hostname)) {
                return PMIX_ERR_NOMEM;
            }
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_NODEID)) {
            rc = PMIx_Value_get_number(&info[n].value, &lg->nodeid, PMIX_UINT32);
            if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
                PMIX_ERROR_LOG(rc);
                return rc;
            }
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_APPNUM)) {
            rc = PMIx_Value_get_number(&info[n].value, &lg->appnum, PMIX_UINT32);
            if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
                PMIX_ERROR_LOG(rc);
                return rc;
            }
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_SESSION_ID)) {
            rc = PMIx_Value_get_number(&info[n].value, &lg->sessionid, PMIX_UINT32);
            if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
                PMIX_ERROR_LOG(rc);
                return rc;
            }
        }
    }

    /* see if they just want their own process ID */
    if (NULL == proc && PMIx_Check_key(key, PMIX_PROCID)) {
        if (lg->stval) {
            ival = *val;
            ival->data.proc = (pmix_proc_t *) malloc(sizeof(pmix_proc_t));
            if (PMIX_UNLIKELY(NULL == ival->data.proc)) {
                return PMIX_ERR_NOMEM;
            }
            /* set the type only once there is something for it to describe -
             * a PMIX_PROC whose pointer is NULL is what the destructor and
             * every reader will trust and follow */
            ival->type = PMIX_PROC;
            PMIX_LOAD_PROCID(ival->data.proc, pmix_globals.myid.nspace, pmix_globals.myid.rank);
        } else if (lg->pntrval) {
            (*val) = &pmix_globals.myidval;
        } else {
            PMIX_VALUE_CREATE(ival, 1);
            if (PMIX_UNLIKELY(NULL == ival)) {
                return PMIX_ERR_NOMEM;
            }
            ival->data.proc = (pmix_proc_t *) malloc(sizeof(pmix_proc_t));
            if (PMIX_UNLIKELY(NULL == ival->data.proc)) {
                PMIX_VALUE_RELEASE(ival);
                return PMIX_ERR_NOMEM;
            }
            ival->type = PMIX_PROC;
            PMIX_LOAD_PROCID(ival->data.proc, pmix_globals.myid.nspace, pmix_globals.myid.rank);
            *val = ival;
        }
        return PMIX_OPERATION_SUCCEEDED;
    }

    /* see if they just want our version */
    if (NULL != key && PMIx_Check_key(key, PMIX_VERSION_NUMERIC)) {
        if (lg->stval) {
            ival = *val;
            ival->type = PMIX_UINT32;
            ival->data.uint32 = PMIX_NUMERIC_VERSION;
        } else {
            PMIX_VALUE_CREATE(ival, 1);
            if (PMIX_UNLIKELY(NULL == ival)) {
                return PMIX_ERR_NOMEM;
            }
            ival->type = PMIX_UINT32;
            ival->data.uint32 = PMIX_NUMERIC_VERSION;
            *val = ival;
        }
        return PMIX_OPERATION_SUCCEEDED;
    }

    /* if the given proc param is NULL, or the nspace is
     * empty, then the caller is referencing our own nspace */
    if (NULL == proc || 0 == strlen(proc->nspace)) {
        PMIX_LOAD_NSPACE(lg->p.nspace, pmix_globals.myid.nspace);
    } else {
        PMIX_LOAD_NSPACE(lg->p.nspace, proc->nspace);
    }
    /* if the proc param is NULL, then we are seeking a key that
     * must be globally unique, so communicate this to the hash
     * functions with the UNDEF rank */
    if (NULL == proc) {
        // if they want node or app info, then use our rank
        if (lg->nodeinfo || lg->appinfo) {
            lg->p.rank = pmix_globals.myid.rank;
        } else {
            lg->p.rank = PMIX_RANK_UNDEF;
        }
    } else {
        lg->p.rank = proc->rank;
    }

    /* if they passed our nspace and an INVALID rank, and are asking
     * for PMIX_RANK, then they are asking for our process rank */
    if (PMIX_RANK_INVALID == lg->p.rank &&
        PMIX_CHECK_NSPACE(lg->p.nspace, pmix_globals.myid.nspace) &&
        NULL != key && PMIx_Check_key(key, PMIX_RANK)) {
        if (lg->stval) {
            ival = *val;
            ival->type = PMIX_PROC_RANK;
            ival->data.rank = pmix_globals.myid.rank;
        } else if (lg->pntrval) {
            (*val) = &pmix_globals.myrankval;
        } else {
            PMIX_VALUE_CREATE(ival, 1);
            if (PMIX_UNLIKELY(NULL == ival)) {
                return PMIX_ERR_NOMEM;
            }
            ival->type = PMIX_PROC_RANK;
            ival->data.rank = pmix_globals.myid.rank;
            *val = ival;
        }
        return PMIX_OPERATION_SUCCEEDED;
    }

    /* if they passed a group in the nspace of proc,
     * replace it with the translated proc. */
    if (!PMIX_PEER_IS_SERVER(pmix_globals.mypeer) &&
        NULL != proc && 0 != strlen(proc->nspace)) {
        rc = pmix_client_convert_group_procs(proc, 1, &procs, &nprocs);
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            return rc;
        }
        if (1 != nprocs) {
            /* we can't support multi-proc gets - and the memcpy below
             * needs exactly one, so a count of zero is equally unusable:
             * PMIx_Proc_create() answers a zero size with NULL */
            PMIX_PROC_FREE(procs, nprocs);
            return PMIX_ERR_BAD_PARAM;
        }
        /* transfer it across in case it was changed */
        memcpy(&lg->p, &procs[0], sizeof(pmix_proc_t));
        PMIX_PROC_FREE(procs, nprocs);
    }

    /* Settle whether this is a plain keyed lookup - see lg->plain.
     *
     * Everything it depends on has been parsed above, so this is the
     * last thing done and it costs one store. It is HERE, rather than
     * where it is consumed, for two reasons. The realm classification
     * is already done - pmix_check_node_info() and its siblings ran at
     * the top of this function - so consulting it again would be free
     * only if it were not, in fact, another pass over the key. And a
     * contributor adding a qualifier is editing the loop above, not
     * try_local_fetch(), which is where this test used to live and why
     * it was easy to add one without ever meeting the question.
     *
     * A NULL key asks for everything the proc put, which is an
     * aggregate across scopes rather than a lookup. The realm flags -
     * whether inferred from the key or stated by a directive - send the
     * request somewhere other than the proc's own data. A hostname or
     * nodeid names a node to resolve first. A cache refresh is a round
     * trip by definition. */
    lg->plain = (NULL != key &&
                 !lg->refresh_cache &&
                 !lg->nodeinfo && !lg->nodedirective &&
                 !lg->appinfo && !lg->appdirective &&
                 !lg->sessioninfo && !lg->sessiondirective &&
                 NULL == lg->hostname &&
                 UINT32_MAX == lg->nodeid);

    /* indicate that everything was okay */
    return PMIX_SUCCESS;
}

/* Answer a get on the caller's own thread, if the datastore holding the
 * answer can be read from one.
 *
 * The thread-shift into the progress thread is the dominant cost of a
 * get that hits locally - measured at roughly 4.9us against ~130ns of
 * actual lookup underneath it - so for the module that can support it,
 * skipping the round trip is most of the operation.
 *
 * "Can support it" is the module's own claim, via is_tsafe. Today only
 * gds/shmem3 makes it, and only because its data lives in shared
 * segments that are immutable once a client can see them, its fetch
 * holds a reference to the tracker that owns those mappings, and a
 * client no longer writes to them at all (the segment is mprotect'd
 * read-only). None of that is true of gds/hash, whose tables the
 * progress thread mutates in place, and which correctly says so.
 *
 * The gating is deliberately narrow. Anything that would make this more
 * than a lookup - a cache refresh, a realm redirection, "give me
 * everything for this proc" - goes the ordinary way. A miss also goes
 * the ordinary way: this is a short-circuit, never a partial one, so
 * every path that could need the server still reaches it unchanged.
 *
 * Returns true only when "cb" now holds the answer.
 */
static bool try_local_fetch(pmix_cb_t *cb, pmix_get_logic_t *lg)
{
    pmix_status_t rc;

    /* Two separate questions, and keeping them apart is the point.
     *
     * Is the REQUEST answerable here? process_request() settled that
     * when it parsed the request, and lg->plain carries the answer -
     * see the note on that field. The list of things that disqualify a
     * request used to be written out here, one function away from the
     * parse that produces them, which is how a qualifier could be added
     * without anyone meeting the question. */
    if (!lg->plain) {
        return false;
    }
    /* And may THIS PROCESS take the short circuit at all? That is about
     * our own state rather than the request, so it is asked here. */
    if (!pmix_client_globals.fast_get) {
        return false;
    }
    if (pmix_client_globals.singleton
        || !pmix_atomic_check_bool(&pmix_globals.connected)) {
        return false;
    }
    /* Resolve the module every time rather than caching the answer:
     * fallback_to_next_gds() re-points this peer at a different one at
     * run time, and a stale answer here would be a read of a store this
     * process has abandoned. */
    PMIX_GDS_FETCH_IS_TSAFE(rc, pmix_client_globals.myserver);
    if (PMIX_SUCCESS != rc) {
        return false;
    }

    cb->proc = &lg->p;
    cb->scope = lg->scope;
    PMIX_GDS_FETCH_KV(rc, pmix_client_globals.myserver, cb);
    if (PMIX_SUCCESS != rc) {
        /* Nothing found, or something we do not handle here. Drop
         * anything collected and let the ordinary path start clean. */
        DRAIN_KVS(cb);
        return false;
    }
    cb->status = process_values(cb);
    if (PMIX_SUCCESS == cb->status && NULL != cb->value) {
        return true;
    }
    /* process_values() can decline - a malformed qualified value in the
     * store, or a failed allocation - and it does not drain what the
     * fetch put on the list. Leaving those entries is not a leak, the
     * caddy destructor takes them, but the ordinary path below is about
     * to fetch into that same list and process_values() decides between
     * "the value" and "an aggregate of everything" by counting it. A
     * stale entry therefore turns a scalar answer into a data array.
     * Hand the slow path a clean caddy, exactly as the failed-fetch
     * return above does - this is a short-circuit, never a partial one. */
    DRAIN_KVS(cb);
    return false;
}

PMIX_EXPORT pmix_status_t PMIx_Get(const pmix_proc_t *proc, const char key[],
                                   const pmix_info_t info[], size_t ninfo, pmix_value_t **val)
{
    pmix_cb_t *cb;
    pmix_get_logic_t *lg;
    pmix_status_t rc;

    pmix_output_verbose(2, pmix_client_globals.get_output,
                        "pmix:client get for %s key %s",
                        (NULL == proc) ? "NULL" : PMIX_NAME_PRINT(proc),
                        (NULL == key) ? "NULL" : key);

    if (PMIX_UNLIKELY(!pmix_atomic_check_bool(&pmix_globals.initialized))) {
        return PMIX_ERR_INIT;
    }

    /* we have no way to hand back the answer without this */
    if (PMIX_UNLIKELY(NULL == val)) {
        return PMIX_ERR_BAD_PARAM;
    }

    if (PMIX_UNLIKELY(pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped))) {
        return PMIX_ERR_NOT_AVAILABLE;
    }

    if (NULL != key && PMIX_MAX_KEYLEN < pmix_keylen(key)) {
        return PMIX_ERR_BAD_PARAM;
    }

    lg = PMIX_NEW(pmix_get_logic_t);
    if (PMIX_UNLIKELY(NULL == lg)) {
        return PMIX_ERR_NOMEM;
    }
    rc = process_request(proc, key, info, ninfo, lg, val);
    if (PMIX_OPERATION_SUCCEEDED == rc) {
        /* the value has already been prepped */
        PMIX_RELEASE(lg);
        return PMIX_SUCCESS;
    } else if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        *val = NULL;
        PMIX_RELEASE(lg);
        return rc;
    }

    /* Everything from here on either blocks on the progress thread or
     * hands the work to it and waits, so a caller already standing in
     * that thread would be waiting for itself. Say so rather than hang.
     *
     * The check sits below process_request() on purpose: the requests it
     * answers outright - PMIX_PROCID, PMIX_VERSION_NUMERIC, PMIX_RANK -
     * touch nothing shared and never post an event, so they remain
     * perfectly usable from an event handler. */
    if (pmix_progress_thread_check_blocking("PMIx_Get")) {
        PMIX_RELEASE(lg);
        *val = NULL;
        return PMIX_ERR_WOULD_BLOCK;
    }

    /* if we are to refresh the cache, go do that. Use the resolved target in
     * "lg", not the caller's "proc" - the latter is allowed to be NULL (a
     * globally-unique key in our own nspace), and process_request has already
     * filled lg->p with the nspace/rank the request actually refers to */
    if (lg->refresh_cache) {
        rc = refresh_cache(&lg->p);
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            // couldn't refresh for some reason
            PMIX_RELEASE(lg);
            *val = NULL;
            return rc;
        }
    }

    /* the request is good - let's go get the data */
    cb = PMIX_NEW(pmix_cb_t);
    if (PMIX_UNLIKELY(NULL == cb)) {
        PMIX_RELEASE(lg);
        *val = NULL;
        return PMIX_ERR_NOMEM;
    }
    cb->lg = lg;
    cb->key = (char*)key;
    cb->info = (pmix_info_t*)info;
    cb->ninfo = ninfo;
    cb->cbfunc.valuefn = _value_cbfunc;
    cb->cbdata = cb;

    /* If the store can be read from here, do that and skip the round
     * trip entirely. On anything other than a hit this falls through
     * untouched. */
    if (try_local_fetch(cb, lg)) {
        rc = cb->status;
        goto answered;
    }

    /* MUST threadshift here to avoid touching global
     * data while in the user's thread */
    PMIX_THREADSHIFT(cb, get_data);

    /* wait for the data to be obtained */
    PMIX_WAIT_THREAD(&cb->lock);
    rc = cb->status;

answered:
    if (PMIX_OPERATION_SUCCEEDED == rc) {
        rc = PMIX_SUCCESS;
    }
    if (PMIX_SUCCESS == rc && NULL != cb->value) {
        if (lg->stval) {
            /* the caller provided the storage, so copy into it - and then
             * release our own copy, which nothing else owns (the caddy
             * destructor does not touch "value").
             *
             * Report a transfer that fails rather than discarding it: the
             * xfer sets the destination's type before it can fail, so the
             * caller would otherwise be told the get succeeded and handed
             * their own storage naming a type with nothing behind it.
             * *val is not cleared on that path - it is the caller's own
             * object, not something we allocated. */
            rc = PMIx_Value_xfer(*val, cb->value);
            PMIX_VALUE_RELEASE(cb->value);   /* nulls cb->value */
        } else {
            *val = cb->value;
            cb->value = NULL;
        }
    } else {
        /* nothing to hand back - but anything the request did collect is
         * still ours to reclaim */
        if (NULL != cb->value) {
            PMIX_VALUE_RELEASE(cb->value);
        }
        if (PMIX_SUCCESS == rc) {
            /* a success with nothing behind it is the one answer the
             * caller cannot act on: PMIx_Get(3) entitles them to
             * dereference *val when we say SUCCESS. Nothing produces it
             * today - process_values() returns a value or a status - so
             * this is what keeps that true rather than something the
             * caller has to defend against. */
            rc = PMIX_ERR_NOT_FOUND;
        }
        *val = NULL;
    }
    PMIX_RELEASE(lg);
    PMIX_RELEASE(cb);

    pmix_output_verbose(2, pmix_client_globals.get_output,
                        "pmix:client get completed with status %s", PMIx_Error_string(rc));

    return rc;
}

static void gcbfn(int sd, short args, void *cbdata)
{
    pmix_cb_t *cb = (pmix_cb_t *) cbdata;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    cb->cbfunc.valuefn(cb->status, cb->value, cb->cbdata);
    /* the caddy does not always carry a logic object - the path that answers
     * a request entirely from process_request builds a bare caddy purely to
     * deliver the result. PMIX_RELEASE dereferences its argument, so a
     * missing "lg" is a segfault, not a no-op */
    if (NULL != cb->lg) {
        PMIX_RELEASE(cb->lg);
    }
    PMIX_RELEASE(cb);
}

PMIX_EXPORT pmix_status_t PMIx_Get_nb(const pmix_proc_t *proc, const char key[],
                                      const pmix_info_t info[], size_t ninfo,
                                      pmix_value_cbfunc_t cbfunc, void *cbdata)
{
    pmix_cb_t *cb;
    pmix_status_t rc;
    pmix_get_logic_t *lg;
    /* process_request both reads and writes through this pointer - the
     * PMIX_GET_STATIC_VALUES check dereferences it before assigning - so it
     * must start life as a known-NULL "no storage provided" rather than
     * whatever happened to be on the stack */
    pmix_value_t *val = NULL;

    if (PMIX_UNLIKELY(!pmix_atomic_check_bool(&pmix_globals.initialized))) {
        return PMIX_ERR_INIT;
    }

    if (PMIX_UNLIKELY(NULL == cbfunc)) {
        /* no way to return the result! */
        return PMIX_ERR_BAD_PARAM;
    }

    if (PMIX_UNLIKELY(pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped))) {
        return PMIX_ERR_NOT_AVAILABLE;
    }

    if (NULL != key && PMIX_MAX_KEYLEN < pmix_keylen(key)) {
        return PMIX_ERR_BAD_PARAM;
    }

    lg = PMIX_NEW(pmix_get_logic_t);
    if (PMIX_UNLIKELY(NULL == lg)) {
        return PMIX_ERR_NOMEM;
    }
    rc = process_request(proc, key, info, ninfo, lg, &val);
    if (PMIX_OPERATION_SUCCEEDED == rc) {
        /* the value has already been prepped - threadshift to return result */
        cb = PMIX_NEW(pmix_cb_t);
        if (PMIX_UNLIKELY(NULL == cb)) {
            /* the value process_request() prepped is ours until the
             * callback hands it over, so it goes back here - unless it
             * is one of the two process-lifetime globals a
             * PMIX_GET_POINTER_VALUES request is answered with. (The
             * third form, PMIX_GET_STATIC_VALUES, cannot arise on this
             * entry point: it names storage the caller provides through
             * "val", and the non-blocking form has no such argument, so
             * process_request() refuses it.) */
            if (NULL != val && !lg->pntrval) {
                PMIX_VALUE_RELEASE(val);
            }
            PMIX_RELEASE(lg);
            return PMIX_ERR_NOMEM;
        }
        cb->status = PMIX_SUCCESS;
        cb->value = val;
        cb->cbfunc.valuefn = cbfunc;
        cb->cbdata = cbdata;
        PMIX_THREADSHIFT(cb, gcbfn);
        PMIX_RELEASE(lg);
        return PMIX_SUCCESS;
    } else if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        /* it's a true error */
        PMIX_RELEASE(lg);
        return rc;
    }

    /* if we are to refresh the cache, go do that - see the note in PMIx_Get
     * on why this uses the resolved lg->p rather than the caller's proc.
     *
     * refresh_cache() is synchronous even here: it round-trips to the
     * server and PMIX_WAIT_THREADs on the reply, on this thread, before
     * the get itself is ever posted. So the non-blocking entry point does
     * block after all when this directive is given, and it needs the same
     * guard PMIx_Get has. The rest of _nb genuinely does not block, which
     * is why the check is scoped to this branch rather than hoisted. */
    if (lg->refresh_cache) {
        if (pmix_progress_thread_check_blocking("PMIx_Get_nb with PMIX_GET_REFRESH_CACHE")) {
            PMIX_RELEASE(lg);
            return PMIX_ERR_WOULD_BLOCK;
        }
        rc = refresh_cache(&lg->p);
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            // couldn't refresh for some reason
            PMIX_RELEASE(lg);
            return rc;
        }
    }

    /* the request is good - let's go get the data */
    cb = PMIX_NEW(pmix_cb_t);
    if (PMIX_UNLIKELY(NULL == cb)) {
        PMIX_RELEASE(lg);
        return PMIX_ERR_NOMEM;
    }
    cb->lg = lg;
    cb->key = (char*)key;
    cb->info = (pmix_info_t*)info;
    cb->ninfo = ninfo;
    cb->scope = lg->scope;
    cb->cbfunc.valuefn = cbfunc;
    cb->cbdata = cbdata;
    // flag that we need to use an intermediate return point
    cb->checked = true;

    /* MUST threadshift here to avoid touching global
     * data while in the user's thread */
    PMIX_THREADSHIFT(cb, get_data);

    pmix_output_verbose(2, pmix_client_globals.get_output,
                        "pmix:client get_nb in progress");

    return rc;
}

static void _value_cbfunc(pmix_status_t status, pmix_value_t *kv, void *cbdata)
{
    pmix_cb_t *cb;
    pmix_status_t rc;

    cb = (pmix_cb_t *) cbdata;
    PMIX_ACQUIRE_OBJECT(cb);
    cb->status = status;
    /* the local get_data path invokes us with the value it already
     * stashed in cb->value - in that case it is ours to keep, so copying
     * it onto itself would simply orphan (leak) the original.  Only make
     * a copy when we are handed a value we do not already own. */
    if (PMIX_SUCCESS == status && kv != cb->value) {
        PMIX_BFROPS_COPY(rc, pmix_client_globals.myserver, (void **)&cb->value, kv, PMIX_VALUE);
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            PMIX_ERROR_LOG(rc);
            /* the waiter reads cb->status and cb->value as a pair.
             * Logging and leaving the success status standing handed it
             * "the get worked, here is nothing" */
            cb->status = rc;
        }
    }
    PMIX_POST_OBJECT(cb);
    PMIX_WAKEUP_THREAD(&cb->lock);
}

static pmix_buffer_t *_pack_get(pmix_cb_t *cb,
                                pmix_rank_t rank,
                                pmix_cmd_t cmd)
{
    pmix_buffer_t *msg;
    pmix_status_t rc;
    char *nspace = cb->proc->nspace;

    /* nope - see if we can get it */
    msg = PMIX_NEW(pmix_buffer_t);
    if (PMIX_UNLIKELY(NULL == msg)) {
        /* every failure arm below releases the message, and
         * PMIX_RELEASE dereferences what it is given */
        return NULL;
    }
    /* pack the get cmd */
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &cmd, 1, PMIX_COMMAND);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        return NULL;
    }
    /* pack the request information - we'll get the entire blob
     * for this proc, so we don't need to pass the key */
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &nspace, 1, PMIX_STRING);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        return NULL;
    }
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &rank, 1, PMIX_PROC_RANK);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        return NULL;
    }
    /* pack the number of info structs */
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &cb->ninfo, 1, PMIX_SIZE);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        return NULL;
    }
    if (0 < cb->ninfo) {
        PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, cb->info, cb->ninfo, PMIX_INFO);
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(msg);
            return NULL;
        }
    }
    if (NULL != cb->key) {
        /* pack the key */
        PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &cb->key, 1, PMIX_STRING);
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(msg);
            return NULL;
        }
    }

    return msg;
}

/* this callback is coming from the ptl recv, and thus
 * is occurring inside of our progress thread - hence, no
 * need to thread shift */
static void _getnb_cbfunc(struct pmix_peer_t *pr, pmix_ptl_hdr_t *hdr,
                          pmix_buffer_t *buf, void *cbdata)
{
    pmix_cb_t *cb = (pmix_cb_t *) cbdata;
    pmix_cb_t *cb2;
    pmix_status_t rc, ret = PMIX_ERR_NOT_FOUND;
    pmix_value_t *val = NULL;
    int32_t cnt;
    pmix_kval_t *kv;
    pmix_get_logic_t *lg;
    pmix_proc_t rproc;
    PMIX_HIDE_UNUSED_PARAMS(pr, hdr);

    PMIX_ACQUIRE_OBJECT(cb);

    pmix_output_verbose(2, pmix_client_globals.get_output,
                        "pmix: get_nb callback recvd");

    if (PMIX_UNLIKELY(NULL == cb || NULL == cb->lg)) {
        /* nothing we can do */
        PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
        return;
    }
    lg = cb->lg;

    /* a zero-byte buffer indicates that this recv is being
     * completed due to a lost connection. Say that, rather than letting
     * the PMIX_ERR_NOT_FOUND this status was seeded with stand: every
     * waiter below is about to be answered with it, and "the key does not
     * exist" is a different fact from "the server this process was asking
     * is gone" - the first invites the caller to try another key or give
     * up on one value, the second means nothing it asks will ever be
     * answered. refcb() below, the other recv callback in this file,
     * already reports it that way. */
    if (PMIX_BUFFER_IS_EMPTY(buf)) {
        pmix_output_verbose(2, pmix_client_globals.get_output,
                            "pmix: get_nb server lost connection");
        ret = PMIX_ERR_LOST_CONNECTION;
        goto done;
    }

    /* unpack the status */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, pmix_client_globals.myserver, buf, &ret, &cnt, PMIX_STATUS);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        /* fall into the delivery loop below rather than quietly dropping the
         * request: every waiter registered against this proc must be told the
         * request failed. Releasing the caddy here instead both stranded a
         * blocking PMIx_Get on its lock forever and freed the object it was
         * still waiting on. */
        ret = rc;
        goto done;
    }

    if (PMIX_UNLIKELY(PMIX_SUCCESS != ret)) {
        pmix_output_verbose(2, pmix_client_globals.get_output,
                            "pmix: get_nb server returned %s",
                            PMIx_Error_string(ret));
        goto done;
    }
    /* store this into our GDS component associated
     * with the server - if it is the hash component,
     * the buffer will include a copy of the data. If
     * it is the shmem component, it will contain just
     * the memory address info */
    PMIX_GDS_ACCEPT_KVS_RESP(rc, pmix_globals.mypeer, buf);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        /* what the payload could not be stored as is precisely what every
         * waiter below is about to be answered from, so the store's
         * failure is their answer. Dropping it left them with whatever
         * the fetches then produced - a bare "not found" that names
         * nothing the caller could act on, for a reply that did arrive */
        PMIX_ERROR_LOG(rc);
        ret = rc;
    }

done:
    /* now search any pending requests (including the one this was in
     * response to) to see if they can be met. A NULL key is legal here -
     * it means "everything this proc put", and process_values() below
     * shapes that into the data array such a request expects */
    pmix_output_verbose(2, pmix_client_globals.get_output,
                        "pmix: get_nb looking for requested key");
    /* snapshot the reference proc. The cb this recv was in response to is
     * itself in the pending list and its "lg" is the one we hold here;
     * once we start delivering results below, that cb (and its lg) will
     * be released, so we must not dereference lg during the loop */
    PMIX_LOAD_PROCID(&rproc, lg->p.nspace, lg->p.rank);
    PMIX_LIST_FOREACH_SAFE (cb, cb2, &pmix_client_globals.pending_requests, pmix_cb_t) {
        if (same_target(rproc.nspace, rproc.rank,
                        cb->pname.nspace, cb->pname.rank)) {
            /* reset per iteration so a failed fetch for this request can
             * never deliver a value already handed to a previous one */
            val = NULL;
            pmix_list_remove_item(&pmix_client_globals.pending_requests, &cb->super);
            if (PMIX_UNLIKELY(PMIX_SUCCESS != ret)) {
                if (cb->checked) {
                    cb->status = ret;
                    gcbfn(0, 0, cb);
                } else {
                    cb->cbfunc.valuefn(ret, NULL, cb->cbdata);
                }
                continue;
            }
            /* we have the data for this proc - see if we can find the key */
            cb->proc = &rproc;
            cb->scope = PMIX_SCOPE_UNDEF;
            pmix_output_verbose(2, pmix_client_globals.get_output,
                                "pmix: get_nb searching for key %s for rank %s", cb->key,
                                PMIX_RANK_PRINT(cb->proc->rank));
            PMIX_GDS_FETCH_KV(rc, pmix_globals.mypeer, cb);
            if (PMIX_OPERATION_SUCCEEDED == rc) {
                rc = PMIX_SUCCESS;
            } else if (PMIX_SUCCESS != rc && PMIX_RANK_UNDEF == cb->proc->rank) {
                // try again with wildcard rank
                pmix_rank_t saverank;
                saverank = cb->proc->rank;
                cb->proc->rank = PMIX_RANK_WILDCARD;
                PMIX_GDS_FETCH_KV(rc, pmix_globals.mypeer, cb);
                if (PMIX_OPERATION_SUCCEEDED == rc) {
                    rc = PMIX_SUCCESS;
                }
                cb->proc->rank = saverank;
            }
            if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
               /* if we are both using the "hash" component, then the server's peer
                * will simply be pointing at the same hash tables as my peer - no
                * no point in checking there again */
               if (!PMIX_GDS_CHECK_COMPONENT(pmix_client_globals.myserver, "hash")) {
                    pmix_output_verbose(2, pmix_client_globals.get_output,
                                        "pmix: get_nb searching for key %s for proc %s, - %s",
                                        cb->key, PMIX_NAME_PRINT(cb->proc), pmix_client_globals.myserver->nptr->compat.gds->name);
                    PMIX_GDS_FETCH_KV(rc, pmix_client_globals.myserver, cb);
                    if (PMIX_OPERATION_SUCCEEDED == rc) {
                        rc = PMIX_SUCCESS;
                    } else if (PMIX_SUCCESS != rc && PMIX_RANK_UNDEF == cb->proc->rank) {
                        // try again with wildcard
                        pmix_rank_t saverank;
                        saverank = cb->proc->rank;
                        cb->proc->rank = PMIX_RANK_WILDCARD;
                        PMIX_GDS_FETCH_KV(rc, pmix_client_globals.myserver, cb);
                        if (PMIX_OPERATION_SUCCEEDED == rc) {
                            rc = PMIX_SUCCESS;
                        }
                        cb->proc->rank = saverank;
                    }
               }
            }
            if (PMIX_SUCCESS == rc) {
                /* shape the answer exactly as get_data() does when it
                 * satisfies a request locally. This used to demand
                 * exactly one kval and report PMIX_ERR_INVALID_VAL for
                 * anything else, which left a NULL key with no answer at
                 * all: "everything this proc put" is a request the server
                 * honors and _pack_get() does send (it simply omits the
                 * key), so a get of a proc whose data was not already
                 * cached here came back as an error rather than as the
                 * data array the same call returns when it hits locally */
                rc = process_values(cb);
                val = cb->value;
                cb->value = NULL;
            }
            /* release any fetched values we are not going to deliver.
             * process_values() consumes the list only in the single-value
             * case - it copies out of it for an aggregate, and a failed
             * fetch can leave entries behind - and the cb is delivered
             * without that list otherwise being drained */
            while (NULL != (kv = (pmix_kval_t *) pmix_list_remove_first(&cb->kvs))) {
                PMIX_RELEASE(kv);
            }
            if (cb->checked) {
                cb->status = rc;
                cb->value = val;
                gcbfn(0, 0, cb);
            } else {
                /* hand the value to the cb so the blocking caller
                 * collects it directly.  Passing it as cb->value lets
                 * _value_cbfunc() take ownership rather than copying it
                 * onto a separate pointer that would then be leaked */
                cb->value = val;
                cb->cbfunc.valuefn(rc, cb->value, cb->cbdata);
            }
        }
    }
}

static pmix_status_t process_values(pmix_cb_t *cb)
{
    pmix_list_t *kvs = &cb->kvs;
    pmix_kval_t *kv;
    pmix_value_t *val;
    pmix_info_t *info, *iptr;
    pmix_status_t rc;
    size_t ninfo, n;

    if (NULL != cb->key && 1 == pmix_list_get_size(kvs)) {
        kv = (pmix_kval_t *) pmix_list_get_first(kvs);
        if (PMIX_CHECK_KEY(kv, PMIX_QUALIFIED_VALUE)) {
            // the actual value is embedded in the qualified-value array,
            // which the kv still owns.  Hand back a standalone copy so the
            // caller can release it, and leave the kv (with its array) to
            // be released normally when the cb is destructed.
            iptr = qualified_value(kv);
            if (PMIX_UNLIKELY(NULL == iptr)) {
                return PMIX_ERR_INVALID_VAL;
            }
            PMIX_VALUE_CREATE(cb->value, 1);
            if (PMIX_UNLIKELY(NULL == cb->value)) {
                return PMIX_ERR_NOMEM;
            }
            rc = PMIx_Value_xfer(cb->value, &iptr[0].value);
            if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
                PMIX_VALUE_RELEASE(cb->value);
                return rc;
            }
        } else {
            // take ownership of the value away from the kv
            cb->value = kv->value;
            kv->value = NULL;
        }
        return PMIX_SUCCESS;
    }
    /* Anything else is handed back as an array of pmix_info_t in a
     * single pmix_value_t. An empty list has to be caught before that:
     * PMIx_Info_create() answers a zero count with the same NULL an
     * allocation failure gives, so a fetch that reported success with
     * nothing on the list would be reported as PMIX_ERR_NOMEM. Neither
     * in-tree module does that today - both report success only when
     * they appended something - which is exactly why the wrong status
     * would be so hard to place if one ever did. */
    if (0 == pmix_list_get_size(kvs)) {
        return PMIX_ERR_NOT_FOUND;
    }
    PMIX_VALUE_CREATE(val, 1);
    if (PMIX_UNLIKELY(NULL == val)) {
        return PMIX_ERR_NOMEM;
    }
    val->type = PMIX_DATA_ARRAY;
    val->data.darray = (pmix_data_array_t *) malloc(sizeof(pmix_data_array_t));
    if (PMIX_UNLIKELY(NULL == val->data.darray)) {
        PMIX_VALUE_RELEASE(val);
        return PMIX_ERR_NOMEM;
    }
    val->data.darray->type = PMIX_INFO;
    val->data.darray->size = 0;
    val->data.darray->array = NULL;
    ninfo = pmix_list_get_size(kvs);
    PMIX_INFO_CREATE(info, ninfo);
    if (PMIX_UNLIKELY(NULL == info)) {
        PMIX_VALUE_RELEASE(val);
        return PMIX_ERR_NOMEM;
    }
    /* copy the list elements */
    n = 0;
    PMIX_LIST_FOREACH (kv, kvs, pmix_kval_t) {
        /* a qualified value we cannot unwrap is passed through as-is rather
         * than failing the whole aggregate - the other entries are still
         * good, and the caller sees the wrapper instead of a missing key */
        iptr = PMIX_CHECK_KEY(kv, PMIX_QUALIFIED_VALUE) ? qualified_value(kv) : NULL;
        if (NULL != iptr) {
            // extract the actual value
            pmix_strncpy(info[n].key, iptr[0].key, PMIX_MAX_KEYLEN);
            rc = PMIx_Value_xfer(&info[n].value, &iptr[0].value);
        } else {
            pmix_strncpy(info[n].key, kv->key, PMIX_MAX_KEYLEN);
            rc = PMIx_Value_xfer(&info[n].value, kv->value);
        }
        /* the transfer sets the destination's type before it can fail, so
         * an entry it could not carry is not an empty slot the caller can
         * skip - it is one that names a type and has nothing behind it */
        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
            PMIX_INFO_FREE(info, ninfo);
            PMIX_VALUE_RELEASE(val);
            return rc;
        }
        ++n;
    }
    val->data.darray->size = ninfo;
    val->data.darray->array = info;
    cb->value = val;
    return PMIX_SUCCESS;
}

/* Copy the caller's directives into a fresh array with room for "nextra"
 * more behind them, which the realm branches in get_data() fill in.
 *
 * The caller's info[] is never ours to edit, so a request that has to add
 * a directive has to copy first - and the copy's status matters. The
 * transfer sets each destination's type before it can fail, so a
 * directive that would not copy does not leave an empty slot the server
 * can skip: it leaves one naming a type with nothing behind it, and this
 * array is what _pack_get() puts on the wire. PMIX_INFO_XFER discards
 * that status by definition, which is why this uses the function form.
 *
 * Returns NULL with "*status" set, having freed the partial array; on
 * success it sets "*nfo" to the total length. */
static pmix_info_t *copy_directives(pmix_cb_t *cb, size_t nextra,
                                    size_t *nfo, pmix_status_t *status)
{
    pmix_info_t *iptr;
    size_t n, total;

    total = cb->ninfo + nextra;
    PMIX_INFO_CREATE(iptr, total);
    if (PMIX_UNLIKELY(NULL == iptr)) {
        *status = PMIX_ERR_NOMEM;
        return NULL;
    }
    for (n = 0; n < cb->ninfo; n++) {
        *status = PMIx_Info_xfer(&iptr[n], &cb->info[n]);
        if (PMIX_UNLIKELY(PMIX_SUCCESS != *status)) {
            PMIX_INFO_FREE(iptr, total);
            return NULL;
        }
    }
    *nfo = total;
    return iptr;
}

static void get_data(int sd, short args, void *cbdata)
{
    pmix_cb_t *cb, cb2;
    pmix_cb_t *cbret;
    pmix_buffer_t *msg;
    pmix_status_t rc;
    pmix_proc_t proc;
    pmix_get_logic_t *lg;
    pmix_info_t optional, *iptr;
    size_t nfo;
    pmix_kval_t *kv;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    cb = (pmix_cb_t*)cbdata;
    PMIX_ACQUIRE_OBJECT(cb);
    lg = cb->lg;
    iptr = cb->info;
    nfo = cb->ninfo;

    pmix_output_verbose(2, pmix_client_globals.get_output,
                        "pmix:client:get_data value for proc %s key %s",
                        PMIX_NAME_PRINT(&lg->p), (NULL == cb->key) ? "NULL" : cb->key);

    /* check the data provided to us by the server first */
    cb->proc = &lg->p;
    cb->scope = lg->scope;
    PMIX_INFO_LOAD(&optional, PMIX_OPTIONAL, NULL, PMIX_BOOL);

    if (lg->nodeinfo) {
        pmix_output_verbose(2, pmix_client_globals.get_output,
                            "pmix:client:get_data value requesting node-level info for proc %s key %s",
                            PMIX_NAME_PRINT(&lg->p), (NULL == cb->key) ? "NULL" : cb->key);
        if (NULL == lg->hostname && UINT32_MAX == lg->nodeid) {
            /* if they didn't specify the target node, then see if they
             * specified a proc */
            if (PMIX_RANK_IS_VALID(cb->proc->rank)) {
                /* if this is us, then see if we know our info */
                if (PMIX_CHECK_PROCID(cb->proc, &pmix_globals.myid)) {
                    if (NULL != pmix_globals.hostname) {
                        lg->hostname = strdup(pmix_globals.hostname);
                    }
                    if (UINT32_MAX != pmix_globals.nodeid) {
                        lg->nodeid = pmix_globals.nodeid;
                    }
                }
                if (NULL == lg->hostname) {
                    PMIX_CONSTRUCT(&cb2, pmix_cb_t);
                    cb2.proc = cb->proc;
                    cb2.key = PMIX_HOSTNAME;
                    cb2.info = &optional;
                    cb2.ninfo = 1;
                    if (PMIX_PEER_IS_CLIENT(pmix_globals.mypeer)) {
                        PMIX_GDS_FETCH_KV(rc, pmix_client_globals.myserver, &cb2);
                    } else {
                        PMIX_GDS_FETCH_KV(rc, pmix_globals.mypeer, &cb2);
                    }
                    if (PMIX_SUCCESS == rc || PMIX_OPERATION_SUCCEEDED == rc) {
                        kv = (pmix_kval_t*)pmix_list_remove_first(&cb2.kvs);
                        /* whatever came back is only useful if it really is a
                         * string - the datastore is fed by the host, so this
                         * is not ours to assume, and strdup(NULL) is a crash */
                        if (NULL != kv && NULL != kv->value &&
                            PMIX_STRING == kv->value->type &&
                            NULL != kv->value->data.string) {
                            lg->hostname = strdup(kv->value->data.string);
                        } else {
                            lg->hostname = strdup("unknown");
                        }
                        if (NULL != kv) {
                            PMIX_RELEASE(kv);
                        }
                    }
                    /* destruct on every outcome - a failed fetch used to leave
                     * the caddy (and anything the GDS had put on its list)
                     * behind */
                    PMIX_DESTRUCT(&cb2);
                }
                if (UINT32_MAX == lg->nodeid) {
                    /* try for the nodeid */
                    PMIX_CONSTRUCT(&cb2, pmix_cb_t);
                    cb2.proc = cb->proc;
                    cb2.key = PMIX_NODEID;
                    cb2.info = &optional;
                    cb2.ninfo = 1;
                    if (PMIX_PEER_IS_CLIENT(pmix_globals.mypeer)) {
                        PMIX_GDS_FETCH_KV(rc, pmix_client_globals.myserver, &cb2);
                    } else {
                        PMIX_GDS_FETCH_KV(rc, pmix_globals.mypeer, &cb2);
                    }
                    if (PMIX_SUCCESS == rc || PMIX_OPERATION_SUCCEEDED == rc) {
                        kv = (pmix_kval_t*)pmix_list_remove_first(&cb2.kvs);
                        PMIX_DESTRUCT(&cb2);
                        if (NULL != kv) { // should never be NULL
                            rc = PMIx_Value_get_number(kv->value, &lg->nodeid, PMIX_UINT32);
                            PMIX_RELEASE(kv);
                        } else {
                            rc = PMIX_ERROR;
                        }
                        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
                            cb->status = rc;
                            goto done;
                        }
                    } else {
                        /* see the hostname fetch above - the caddy must be
                         * torn down on the failure path too */
                        PMIX_DESTRUCT(&cb2);
                    }
                }
                // set the rank to undefined since this request is
                // required to ignore the procID
                cb->proc->rank = PMIX_RANK_UNDEF;
            } else {
                /* it's an invalid rank - assume they are asking about this node.
                 * This is consistent with prior releases */
                cb->proc->rank = PMIX_RANK_UNDEF;
                lg->hostname = strdup(pmix_globals.hostname);
                lg->nodeid = pmix_globals.nodeid;
            }
        }
        /* if they were asking for hostname, then we are done */
        if (PMIx_Check_key(cb->key, PMIX_HOSTNAME)) {
            if (NULL != lg->hostname) {
                PMIX_VALUE_CREATE(cb->value, 1);
                if (PMIX_UNLIKELY(NULL == cb->value)) {
                    cb->status = PMIX_ERR_NOMEM;
                    goto done;
                }
                cb->status = PMIX_SUCCESS;
                PMIX_VALUE_LOAD(cb->value, lg->hostname, PMIX_STRING);
            } else {
                cb->status = PMIX_ERR_NOT_FOUND;
            }
            goto done;
        }
        /* if they were asking for nodeid, then we are done */
        if (PMIx_Check_key(cb->key, PMIX_NODEID)) {
            if (UINT32_MAX != lg->nodeid) {
                PMIX_VALUE_CREATE(cb->value, 1);
                if (PMIX_UNLIKELY(NULL == cb->value)) {
                    cb->status = PMIX_ERR_NOMEM;
                    goto done;
                }
                cb->status = PMIX_SUCCESS;
                PMIX_VALUE_LOAD(cb->value, &lg->nodeid, PMIX_UINT32);
            } else {
                cb->status = PMIX_ERR_NOT_FOUND;
            }
            goto done;
        }
        /* we have to look for the info, so we need to tell the GDS
         * that this is a nodeinfo request and pass the
         * nodename or nodeid */
        if (lg->nodedirective) {
            /* just need to add the hostname/nodeid */
            iptr = copy_directives(cb, 2, &nfo, &cb->status);
            if (PMIX_UNLIKELY(NULL == iptr)) {
                goto done;
            }
            if (NULL != lg->hostname) {
                PMIX_INFO_LOAD(&iptr[cb->ninfo], PMIX_HOSTNAME, lg->hostname, PMIX_STRING);
            } else {
                PMIX_INFO_LOAD(&iptr[cb->ninfo], PMIX_NODEID, &lg->nodeid, PMIX_UINT32);
            }
            PMIX_INFO_LOAD(&iptr[cb->ninfo+1], PMIX_OPTIONAL, NULL, PMIX_BOOL);
            cb->infocopy = true;
        } else {
            /* need to add directive and hostname/nodeid */
            iptr = copy_directives(cb, 3, &nfo, &cb->status);
            if (PMIX_UNLIKELY(NULL == iptr)) {
                goto done;
            }
            PMIX_INFO_LOAD(&iptr[cb->ninfo], PMIX_NODE_INFO, NULL, PMIX_BOOL);
            if (NULL != lg->hostname) {
                PMIX_INFO_LOAD(&iptr[cb->ninfo+1], PMIX_HOSTNAME, lg->hostname, PMIX_STRING);
            } else {
                PMIX_INFO_LOAD(&iptr[cb->ninfo+1], PMIX_NODEID, &lg->nodeid, PMIX_UINT32);
            }
            PMIX_INFO_LOAD(&iptr[cb->ninfo+2], PMIX_OPTIONAL, NULL, PMIX_BOOL);
            cb->infocopy = true;
        }
        goto doget;
    }

    if (lg->appinfo) {
        /* if they didn't provide an appnum, then we have to look it up */
        if (UINT32_MAX == lg->appnum) {
            /* if they didn't specify the target app, then see if they
             * specified a proc */
            if (PMIX_RANK_IS_VALID(cb->proc->rank)) {
                /* if this is us, then we know our appnum */
                if (PMIX_CHECK_PROCID(cb->proc, &pmix_globals.myid)) {
                    lg->appnum = pmix_globals.appnum;
                } else {
                    PMIX_CONSTRUCT(&cb2, pmix_cb_t);
                    cb2.proc = cb->proc;
                    cb2.key = PMIX_APPNUM;
                    cb2.info = &optional;
                    cb2.ninfo = 1;
                    if (PMIX_PEER_IS_CLIENT(pmix_globals.mypeer)) {
                        PMIX_GDS_FETCH_KV(rc, pmix_client_globals.myserver, &cb2);
                    } else {
                        PMIX_GDS_FETCH_KV(rc, pmix_globals.mypeer, &cb2);
                    }
                    if (PMIX_SUCCESS == rc || PMIX_OPERATION_SUCCEEDED == rc) {
                        kv = (pmix_kval_t*)pmix_list_remove_first(&cb2.kvs);
                        PMIX_DESTRUCT(&cb2);
                        if (PMIX_UNLIKELY(NULL == kv)) { // should never happen
                            cb->status = PMIX_ERR_NOT_FOUND;
                            goto done;
                        }
                        rc = PMIx_Value_get_number(kv->value, &lg->appnum, PMIX_UINT32);
                        PMIX_RELEASE(kv);
                        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
                            cb->status = rc;
                            goto done;
                        }
                    } else {
                        /* couldn't find this proc's appnum - nothing we can do.
                         * Tear the caddy down first: see the hostname fetch
                         * above, whose failure path had the same hole */
                        PMIX_DESTRUCT(&cb2);
                        cb->status = PMIX_ERR_NOT_FOUND;
                        goto done;
                    }
                }
                // set the rank to undefined since this request is
                // required to ignore the procID
                cb->proc->rank = PMIX_RANK_UNDEF;
            } else {
                /* rank is invalid - assume they want info about our app.
                 * This is consistent with prior releases */
                cb->proc->rank = PMIX_RANK_UNDEF;
                lg->appnum = pmix_globals.appnum;
            }
        }
        /* we get here with a valid appnum - if that is what they were
         * asking for, then we are done */
        if (PMIx_Check_key(cb->key, PMIX_APPNUM)) {
            PMIX_VALUE_CREATE(cb->value, 1);
            if (PMIX_UNLIKELY(NULL == cb->value)) {
                cb->status = PMIX_ERR_NOMEM;
                goto done;
            }
            cb->status = PMIX_SUCCESS;
            PMIX_VALUE_LOAD(cb->value, &lg->appnum, PMIX_UINT32);
            goto done;
        }
        /* setup the request */
        if (lg->appdirective) {
            /* just need to add the appnum */
            iptr = copy_directives(cb, 2, &nfo, &cb->status);
            if (PMIX_UNLIKELY(NULL == iptr)) {
                goto done;
            }
            PMIX_INFO_LOAD(&iptr[cb->ninfo], PMIX_APPNUM, &lg->appnum, PMIX_UINT32);
            PMIX_INFO_LOAD(&iptr[cb->ninfo+1], PMIX_OPTIONAL, NULL, PMIX_BOOL);
            cb->infocopy = true;
        } else {
            /* need to add directive and appnum */
            iptr = copy_directives(cb, 3, &nfo, &cb->status);
            if (PMIX_UNLIKELY(NULL == iptr)) {
                goto done;
            }
            PMIX_INFO_LOAD(&iptr[cb->ninfo], PMIX_APP_INFO, NULL, PMIX_BOOL);
            PMIX_INFO_LOAD(&iptr[cb->ninfo+1], PMIX_APPNUM, &lg->appnum, PMIX_UINT32);
            PMIX_INFO_LOAD(&iptr[cb->ninfo+2], PMIX_OPTIONAL, NULL, PMIX_BOOL);
            cb->infocopy = true;
        }
        goto doget;
    }

    if (lg->sessioninfo) {
        /* if they didn't provide a sessionid, then we have to look it up */
        if (UINT32_MAX == lg->sessionid) {
            /* if they didn't specify the target session, then see if they
             * specified a proc */
            if (PMIX_RANK_IS_VALID(cb->proc->rank)) {
                /* if this is us, then we know our info */
                if (PMIX_CHECK_PROCID(cb->proc, &pmix_globals.myid)) {
                    lg->sessionid = pmix_globals.sessionid;
                } else {
                    PMIX_CONSTRUCT(&cb2, pmix_cb_t);
                    cb2.proc = cb->proc;
                    cb2.key = PMIX_SESSION_ID;
                    cb2.info = &optional;
                    cb2.ninfo = 1;
                    /* a client's job-level data - which is where another
                     * proc's session id lives - is stored against
                     * myserver's module (see job_data() in
                     * pmix_client.c), not our own. Both sibling lookups
                     * above make the same choice; this one asked only our
                     * own store, which happens to be the same tables when
                     * the server also uses "hash" and is empty of job
                     * data when it does not - so a shmem3 client silently
                     * failed to resolve the session and went on to ask
                     * with a sessionid of UINT32_MAX */
                    if (PMIX_PEER_IS_CLIENT(pmix_globals.mypeer)) {
                        PMIX_GDS_FETCH_KV(rc, pmix_client_globals.myserver, &cb2);
                    } else {
                        PMIX_GDS_FETCH_KV(rc, pmix_globals.mypeer, &cb2);
                    }
                    if (PMIX_SUCCESS == rc || PMIX_OPERATION_SUCCEEDED == rc) {
                        kv = (pmix_kval_t*)pmix_list_remove_first(&cb2.kvs);
                        PMIX_DESTRUCT(&cb2);
                        if (PMIX_UNLIKELY(NULL == kv)) { // should never happen
                            cb->status = PMIX_ERR_NOT_FOUND;
                            goto done;
                        }
                        rc = PMIx_Value_get_number(kv->value, &lg->sessionid, PMIX_UINT32);
                        PMIX_RELEASE(kv);
                        if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
                            cb->status = rc;
                            goto done;
                        }
                    } else {
                        /* the fetch failed, so the caddy still has to come
                         * down - the hostname and nodeid fetches above lost
                         * theirs the same way. Unlike its siblings this one
                         * carries on with sessionid left at UINT32_MAX, which
                         * simply resolves to "no such session" below */
                        PMIX_DESTRUCT(&cb2);
                    }
                }
            } else {
                /* rank is invalid - assume they want info about our session.
                 * This is consistent with prior releases */
                cb->proc->rank = PMIX_RANK_UNDEF;
                lg->sessionid = pmix_globals.sessionid;
            }
        }
        /* if they were asking for sessionid, then we are done */
        if (PMIx_Check_key(cb->key, PMIX_SESSION_ID)) {
            PMIX_VALUE_CREATE(cb->value, 1);
            if (PMIX_UNLIKELY(NULL == cb->value)) {
                cb->status = PMIX_ERR_NOMEM;
                goto done;
            }
            cb->status = PMIX_SUCCESS;
            PMIX_VALUE_LOAD(cb->value, &lg->sessionid, PMIX_UINT32);
            goto done;
        }
        /* setup the request */
        if (lg->sessiondirective) {
            /* just need to add the sessionid */
            iptr = copy_directives(cb, 2, &nfo, &cb->status);
            if (PMIX_UNLIKELY(NULL == iptr)) {
                goto done;
            }
            PMIX_INFO_LOAD(&iptr[cb->ninfo], PMIX_SESSION_ID, &lg->sessionid, PMIX_UINT32);
            PMIX_INFO_LOAD(&iptr[cb->ninfo+1], PMIX_OPTIONAL, NULL, PMIX_BOOL);
            cb->infocopy = true;
        } else {
            /* need to add directive and sessionid */
            iptr = copy_directives(cb, 3, &nfo, &cb->status);
            if (PMIX_UNLIKELY(NULL == iptr)) {
                goto done;
            }
            PMIX_INFO_LOAD(&iptr[cb->ninfo], PMIX_SESSION_INFO, NULL, PMIX_BOOL);
            PMIX_INFO_LOAD(&iptr[cb->ninfo+1], PMIX_SESSION_ID, &lg->sessionid, PMIX_UINT32);
            PMIX_INFO_LOAD(&iptr[cb->ninfo+2], PMIX_OPTIONAL, NULL, PMIX_BOOL);
            cb->infocopy = true;
        }
        goto doget;
    }

doget:
    cb->info = iptr;
    cb->ninfo = nfo;
    PMIX_GDS_FETCH_KV(rc, pmix_client_globals.myserver, cb);
    if (PMIX_SUCCESS == rc) {
        pmix_output_verbose(5, pmix_client_globals.get_output,
                            "pmix:client data found in server-provided data");
        cb->status = process_values(cb);
        goto done;
    }
    /* a gds fetch can append to cb->kvs and then fail, and this caddy is
     * fetched into again below - by the retry, and by _getnb_cbfunc() once
     * the server replies. process_values() tells "the value" from "an
     * aggregate of everything this proc put" by counting that list, so one
     * stale entry turns a scalar get into a data array. Hand every later
     * fetch an empty list, exactly as try_local_fetch() does when it
     * declines: nothing is leaked either way (cbdes() destructs the list),
     * which is why this is a wrong-answer bug rather than one valgrind
     * would show. */
    DRAIN_KVS(cb);
    pmix_output_verbose(5, pmix_client_globals.get_output,
                        "pmix:client data NOT found in server-provided data");

    /* if we are both using the "hash" component, then the server's peer
     * will simply be pointing at the same hash tables as my peer - no
     * no point in checking there again */
    if (!PMIX_GDS_CHECK_COMPONENT(pmix_client_globals.myserver, "hash")) {
        /* check the data in my hash module */
        PMIX_GDS_FETCH_KV(rc, pmix_globals.mypeer, cb);
        if (PMIX_SUCCESS == rc) {
            pmix_output_verbose(5, pmix_client_globals.get_output,
                                "pmix:client data found in internal hash data");
            cb->status = process_values(cb);
            goto done;
        }
        DRAIN_KVS(cb);
    }

    /* Note what is deliberately NOT here: a retry of the two fetches
     * above at PMIX_RANK_WILDCARD.
     *
     * PMIX_RANK_UNDEF - what process_request() resolves a NULL proc to,
     * and what the realm branches set - means "anything in this nspace",
     * and job-level data is part of that even though it is filed under no
     * rank. Answering it is the datastore's job. Both in-tree modules
     * used to search only the per-rank tables for it, and a retry here
     * did make PMIx_Get(NULL, <job-level key>) work - which is precisely
     * why the defect survived in both of them for as long as it did, and
     * why it must not come back. A client that compensates for a module's
     * rank convention is a client that cannot tell you the module is
     * wrong. test/unit/get_api.c holds the module behavior directly, from
     * a real client, so a regression fails there instead of being
     * absorbed here. */
    pmix_output_verbose(5, pmix_client_globals.get_output,
                        "pmix:client requested data NOT found");

    /* we may wind up requesting the data using a different rank as an
     * indicator of the breadth of data we want, but we will need to
     * get the specific data someone requested later. So setup a tmp
     * process ID */
    memcpy(&proc, &lg->p, sizeof(pmix_proc_t));
    cb->pname.nspace = strdup(lg->p.nspace);
    if (PMIX_UNLIKELY(NULL == cb->pname.nspace)) {
        /* this is what identifies the request on the pending list, and
         * _getnb_cbfunc() hands it straight to PMIX_CHECK_NSPACE */
        cb->status = PMIX_ERR_NOMEM;
        goto done;
    }
    cb->pname.rank = lg->p.rank;

    /* we didn't find the data in either the server or the internal hash
     * components. If this is a NULL key, then we do NOT go up to the
     * server unless special circumstances require it */
    if (NULL == cb->key) {
        /* if the server is pre-v3.2, or we are asking about the
         * job-level info from another namespace, then we have to
         * request the data */
        if (PMIX_PEER_IS_EARLIER(pmix_client_globals.myserver, 3, 1, 100) ||
            !PMIX_CHECK_NSPACE(lg->p.nspace, pmix_globals.myid.nspace)) {
            /* flag that we want all of the job-level info */
            proc.rank = PMIX_RANK_WILDCARD;
        }
    } else if (PMIX_CHECK_RESERVED_KEY(cb->key)) {
        /* This is a reserved key we should have been given at startup, so
         * something is holding it. The request now goes up to the server
         * like any other. This branch used to *force* PMIX_IMMEDIATE onto
         * it, and that is what has been removed: the flag confines the
         * search to what our own server already holds, and we have just
         * established that it does not hold this - we asked its datastore
         * and our own in the two fetches above. So the added flag could
         * only ever have turned this into a slower way of reaching the
         * answer we already had.
         *
         * Letting the request through matters because the server is not
         * the last word on a reserved key. Its host frequently knows
         * values it chose not to push down - a large machine may hand each
         * daemon only the job-level data its own local clients need,
         * rather than replicate every proc's location keys on every node -
         * and the host is asked only if the request is surfaced to it.
         *
         * A PMIX_IMMEDIATE the caller supplied is untouched by this: it
         * sits in cb->info, is packed into the request below, and the
         * server honors it there. We simply no longer invent one. */
        pmix_output_verbose(5, pmix_client_globals.get_output,
                            "pmix:client reserved key not locally found - "
                            "requesting it from the server");
    }

    /* if we got here, then we don't have the data for this proc. If we
     * are a server, or we are not connected, then there is
     * nothing more we can do */
    if ((PMIX_PEER_IS_SERVER(pmix_globals.mypeer) &&
         !PMIX_PEER_IS_TOOL(pmix_globals.mypeer)) ||
         !pmix_atomic_check_bool(&pmix_globals.connected)) {
        cb->status = PMIX_ERR_NOT_FOUND;
        goto done;
    }

    /* if the data is available in a different scope, then
     * let the user know */
    if (PMIX_ERR_EXISTS_OUTSIDE_SCOPE == rc) {
        cb->status = rc;
        goto done;
    }

    /* we also have to check the user's directives to see if they do not want
     * us to attempt to retrieve it from the server */
    if (lg->optional) {
        /* they don't want us to try and retrieve it */
        pmix_output_verbose(2, pmix_client_globals.get_output,
                            "PMIx_Get key=%s for rank = %u, namespace = %s was not found - request was optional",
                            cb->key, cb->pname.rank, cb->pname.nspace);
        cb->status = rc;
        goto done;
    }

    /* see if we already have a request in place with the server for data from
     * this nspace:rank. If we do, then no need to ask again as the
     * request will return _all_ data from that proc */
    PMIX_LIST_FOREACH (cbret, &pmix_client_globals.pending_requests, pmix_cb_t) {
        /* compare what each request recorded about itself, not the rank
         * this one is about to ask the server with - the rewrite above
         * varies that, and cb->pname is what the reply is matched
         * against. See same_target() for why the two must agree. */
        if (same_target(cbret->pname.nspace, cbret->pname.rank,
                        cb->pname.nspace, cb->pname.rank)) {
            pmix_output_verbose(2, pmix_client_globals.get_output,
                                "%s ADDING REQUEST TO PENDING %s:%s KEY %s",
                                PMIX_NAME_PRINT(&pmix_globals.myid), cb->proc->nspace,
                                PMIX_RANK_PRINT(proc.rank), cb->key);
            /* we do have a pending request, but we still need to track this
             * outstanding request so we can satisfy it once the data is returned */
            pmix_list_append(&pmix_client_globals.pending_requests, &cb->super);
            return;
        }
    }

    /* we don't have a pending request, so let's create one */
    msg = _pack_get(cb, proc.rank, PMIX_GETNB_CMD);
    if (PMIX_UNLIKELY(NULL == msg)) {
        cb->status = PMIX_ERROR;
        PMIX_ERROR_LOG(cb->status);
        goto done;
    }

    pmix_output_verbose(2, pmix_client_globals.get_output,
                        "%s REQUESTING DATA FROM SERVER FOR %s:%s KEY %s",
                        PMIX_NAME_PRINT(&pmix_globals.myid), cb->proc->nspace,
                        PMIX_RANK_PRINT(proc.rank), PMIx_Get_attribute_string(cb->key));

    /* track the callback object */
    pmix_list_append(&pmix_client_globals.pending_requests, &cb->super);
    /* send to the server */
    PMIX_PTL_SEND_RECV(rc, pmix_client_globals.myserver, msg, _getnb_cbfunc, (void *) cb);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        /* the transport only refuses a send it never took ownership of, so
         * the message is still ours to release */
        PMIX_RELEASE(msg);
        pmix_list_remove_item(&pmix_client_globals.pending_requests, &cb->super);
        cb->status = rc;
        goto done;
    }
    return;

done:
    /* we made a lot of changes to cb, so ensure they get
     * written out before we return */
    PMIX_POST_OBJECT(cb);
    if (cb->checked) {
        gcbfn(0, 0, cb);
    } else {
        cb->cbfunc.valuefn(cb->status, cb->value, cb->cbdata);
    }
    return;
}

static void refcb(struct pmix_peer_t *pr, pmix_ptl_hdr_t *hdr,
                  pmix_buffer_t *buf, void *cbdata)
{
    pmix_cb_t *cb = (pmix_cb_t *) cbdata;
    int32_t cnt;
    pmix_status_t rc, ret, strc, store_rc = PMIX_SUCCESS;
    pmix_kval_t kv;
    PMIX_HIDE_UNUSED_PARAMS(pr, hdr);

    PMIX_ACQUIRE_OBJECT(cb);

    if (PMIX_UNLIKELY(NULL == cb)) {
        /* nothing we can do */
        PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
        return;
    }
    /* a zero-byte buffer indicates that this recv is being
     * completed due to a lost connection */
    if (PMIX_BUFFER_IS_EMPTY(buf)) {
        pmix_output_verbose(2, pmix_client_globals.get_output,
                            "pmix: refcb server lost connection");
        ret = PMIX_ERR_LOST_CONNECTION;
        goto done;
    }

    /* unpack the status */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, pmix_client_globals.myserver, buf, &ret, &cnt, PMIX_STATUS);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        ret = rc;
        goto done;
    }

    /* unpack and store any returned data. The unpack is what terminates the
     * loop, so the store cannot be allowed to overwrite its status - but a
     * store failure must not be dropped either. The caller asked explicitly
     * for fresh data, and the alternative to reporting it is to hand them the
     * stale copy without a word. Record the first one and report it below.
     * Note this deliberately does not resurrect the status the *server* sent:
     * refresh_cache() is called for its side effect and its return aborts the
     * enclosing PMIx_Get, so a "nothing to refresh" answer must not fail gets
     * that succeed today - see the note in src/server/AGENTS.md */
    PMIX_CONSTRUCT(&kv, pmix_kval_t);
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, pmix_client_globals.myserver, buf, &kv, &cnt, PMIX_KVAL);
    while (PMIX_SUCCESS == rc) {
        PMIX_GDS_STORE_KV(strc, pmix_globals.mypeer, cb->proc, PMIX_INTERNAL, &kv);
        if (PMIX_UNLIKELY(PMIX_SUCCESS != strc && PMIX_SUCCESS == store_rc)) {
            PMIX_ERROR_LOG(strc);
            store_rc = strc;
        }
        PMIX_DESTRUCT(&kv);
        PMIX_CONSTRUCT(&kv, pmix_kval_t);
        cnt = 1;
        PMIX_BFROPS_UNPACK(rc, pmix_client_globals.myserver, buf, &kv, &cnt, PMIX_KVAL);
    }
    PMIX_DESTRUCT(&kv);
    if (PMIX_SUCCESS != store_rc) {
        ret = store_rc;
    } else if (PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER == rc) {
        ret = PMIX_SUCCESS;
    } else {
        ret = rc;
    }

done:
    cb->status = ret;
    /* release the lock */
    PMIX_POST_OBJECT(cb);
    PMIX_WAKEUP_THREAD(&cb->lock);
    return;
}

static pmix_status_t refresh_cache(const pmix_proc_t *p)
{
    pmix_cb_t *cb;
    pmix_buffer_t *msg;
    pmix_status_t rc;
    pmix_cmd_t cmd = PMIX_REFRESH_CACHE;
    char *nspace = (char*)p->nspace;

    pmix_output_verbose(2, pmix_client_globals.get_output,
                        "%s REQUESTING CACHE REFRESH BY SERVER FOR PROC %s",
                        PMIX_NAME_PRINT(&pmix_globals.myid),
                        PMIX_NAME_PRINT(p));

    /* only the server can refresh our cache, so there is nothing to do (and
     * nothing to pack a message against) if we have no server. Every other
     * server round-trip in this file gates on this; without it a singleton
     * asking for PMIX_GET_REFRESH_CACHE packed a message for a peer that was
     * never given a transport. */
    if (pmix_client_globals.singleton ||
        !pmix_atomic_check_bool(&pmix_globals.connected)) {
        return PMIX_SUCCESS;
    }

    /* pack a quick message to the server asking it
     * to refresh our cache */
    msg = PMIX_NEW(pmix_buffer_t);
    if (PMIX_UNLIKELY(NULL == msg)) {
        return PMIX_ERR_NOMEM;
    }
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &cmd, 1, PMIX_COMMAND);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        return rc;
    }
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &nspace, 1, PMIX_STRING);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        return rc;
    }
    PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &p->rank, 1, PMIX_PROC_RANK);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        return rc;
    }

    cb = PMIX_NEW(pmix_cb_t);
    if (PMIX_UNLIKELY(NULL == cb)) {
        PMIX_RELEASE(msg);
        return PMIX_ERR_NOMEM;
    }
    cb->proc = (pmix_proc_t*)p;

    /* send to the server */
    PMIX_PTL_SEND_RECV(rc, pmix_client_globals.myserver, msg, refcb, (void *)cb);
    if (PMIX_UNLIKELY(PMIX_SUCCESS != rc)) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        PMIX_RELEASE(cb);
        return rc;
    }
    PMIX_WAIT_THREAD(&cb->lock);
    rc = cb->status;
    PMIX_RELEASE(cb);
    return rc;
}
