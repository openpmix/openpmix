/*
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2016-2018 IBM Corporation.  All rights reserved.
 * Copyright (c) 2018      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2018-2020 Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2022-2023 Triad National Security, LLC. All rights reserved.
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
#include "src/client/pmix_client_ops.h"
#include "src/include/pmix_globals.h"
#include "src/mca/pcompress/base/base.h"
#include "src/mca/pmdl/pmdl.h"
#include "src/mca/preg/preg.h"
#include "src/mca/ptl/base/base.h"
#include "src/runtime/pmix_rte.h"
#include "src/server/pmix_server_ops.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_hash.h"
#include "src/util/pmix_name_fns.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_environ.h"

#include "gds_hash.h"
#include "src/mca/gds/base/base.h"

static pmix_status_t hash_init(pmix_info_t info[], size_t ninfo);
static void hash_finalize(void);

static pmix_status_t hash_assign_module(pmix_info_t *info, size_t ninfo, int *priority);

static pmix_status_t hash_cache_job_info(struct pmix_namespace_t *ns, pmix_info_t info[],
                                         size_t ninfo);

static pmix_status_t hash_register_job_info(struct pmix_peer_t *pr, pmix_buffer_t *reply);

static pmix_status_t hash_store_job_info(const char *nspace, pmix_buffer_t *buf);

static pmix_status_t hash_store_modex(pmix_buffer_t *buff,
                                      const char *nspace,
                                      void *cbdata);

static pmix_status_t _hash_store_modex(pmix_proc_t *proc,
                                       pmix_buffer_t *pbkt,
                                       uint8_t kind);

static pmix_status_t setup_fork(const pmix_proc_t *peer, char ***env);

static pmix_status_t nspace_add(const char *nspace, uint32_t nlocalprocs, pmix_info_t info[],
                                size_t ninfo);

static pmix_status_t session_add(uint32_t sessionID, pmix_info_t info[], size_t ninfo)
{
    pmix_value_t val;
    pmix_status_t rc;

    /* Establish the tracker even when the host described nothing. A
     * session that exists and carries no attributes is still a session,
     * and a job naming it later has to find this one rather than create
     * a second - which is the whole difference between a session the
     * host declared and one inferred from a job.
     *
     * A registration for a session we already hold is an UPDATE, not a
     * duplicate: a session's resources change under it - see the note in
     * pmix_gds_hash_process_session_array() - so the keys it restates
     * replace the ones we hold. */
    if (NULL == pmix_gds_hash_check_session(NULL, sessionID, true)) {
        return PMIX_ERR_NOMEM;
    }
    if (0 == ninfo || NULL == info) {
        return PMIX_SUCCESS;
    }

    rc = pmix_gds_base_wrap_session_info(sessionID, info, ninfo, &val);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    /* NULL tracker: this session is being described in its own right,
     * not on behalf of some job that mentioned it */
    rc = pmix_gds_hash_process_session_array(&val, NULL);
    pmix_gds_base_release_session_info(&val);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    /* Clients already attached hold what the session said when they
     * attached, so tell them - otherwise a job launched afterwards sees
     * the change and one already running does not. */
    pmix_server_notify_gds_update(NULL);
    return PMIX_SUCCESS;
}

static pmix_status_t session_del(uint32_t sessionID)
{
    pmix_session_t *sptr;

    PMIX_LIST_FOREACH (sptr, &pmix_mca_gds_hash_component.mysessions, pmix_session_t) {
        if (sptr->session == sessionID) {
            /* Drop the list's reference only. Jobs still registered in
             * this session hold their own, so the object survives until
             * they are deregistered - which is what lets a host end a
             * session and tear its jobs down in either order. */
            pmix_list_remove_item(&pmix_mca_gds_hash_component.mysessions, &sptr->super);
            PMIX_RELEASE(sptr);
            return PMIX_SUCCESS;
        }
    }
    /* Not an error. A host deregisters a session across every daemon,
     * and this one may have had no job in it. */
    return PMIX_SUCCESS;
}

/* Pack what this peer needs to see a session whose description has
 * changed.
 *
 * The session's whole description, as the PMIX_SESSION_INFO_ARRAY the
 * client already knows how to store - the same shape register_job_info()
 * hands a client that attaches now. Whole rather than "just what
 * changed", because storing it replaces by key: applying it twice, or
 * to a client that is already current, lands on the same state.
 *
 * Nothing is packed for a job with no session, which is the ordinary
 * case for a host that never described one.
 */
/* File a lone realm key where its realm keeps values - see the note on
 * the definition. Used by every path that takes one: the registration
 * that first stores a job description, the reply to a keyed get, and
 * the job-level values an update carries. */
static pmix_status_t store_lone_realm_key(pmix_job_t *trk, uint32_t sid,
                                          const char *key, pmix_value_t *value);

static pmix_status_t hash_pack_update(struct pmix_peer_t *pr,
                                      pmix_buffer_t *buff)
{
    pmix_peer_t *peer = (pmix_peer_t *) pr;
    pmix_list_t results;
    pmix_kval_t *kvptr;
    pmix_job_t *trk;
    pmix_status_t rc;

    if (NULL == peer || NULL == peer->nptr) {
        return PMIX_SUCCESS;
    }
    trk = pmix_gds_hash_get_tracker(peer->nptr->nspace, false);
    if (NULL == trk) {
        return PMIX_SUCCESS;
    }

    /* Job-level values added since this namespace was registered,
     * wrapped in a PMIX_JOB_INFO_ARRAY.
     *
     * The wrapper is what makes this stream SELF-DESCRIBING, and that is
     * the point of it rather than tidiness. Sent bare, a job-level value
     * is indistinguishable from any other kval, so the receiver had to
     * treat "not a session array" as "a job-level value" - and would
     * therefore misfile, as job data, the first new kind of entry any
     * future release adds here. Named, each kind can be recognized and
     * an unrecognized one skipped, which is what the project's wire
     * rules ask of a reader.
     *
     * These go first and unconditionally - a job need not be in a
     * session at all, and the session walk below returns early when it
     * is not, which would have dropped these with it. */
    if (0 < pmix_list_get_size(&trk->updates)) {
        pmix_data_array_t *darray = NULL;
        pmix_info_t *iptr;
        pmix_kval_t wrap;
        size_t n = 0;

        PMIX_DATA_ARRAY_CREATE(darray, pmix_list_get_size(&trk->updates),
                               PMIX_INFO);
        if (NULL == darray) {
            return PMIX_ERR_NOMEM;
        }
        iptr = (pmix_info_t *) darray->array;
        PMIX_LIST_FOREACH (kvptr, &trk->updates, pmix_kval_t) {
            PMIX_LOAD_KEY(iptr[n].key, kvptr->key);
            rc = PMIx_Value_xfer(&iptr[n].value, kvptr->value);
            if (PMIX_SUCCESS != rc) {
                PMIX_DATA_ARRAY_FREE(darray);
                PMIX_ERROR_LOG(rc);
                return rc;
            }
            n++;
        }
        PMIX_CONSTRUCT(&wrap, pmix_kval_t);
        wrap.key = strdup(PMIX_JOB_INFO_ARRAY);
        wrap.value = (pmix_value_t *) calloc(1, sizeof(pmix_value_t));
        if (NULL == wrap.key || NULL == wrap.value) {
            PMIX_DATA_ARRAY_FREE(darray);
            PMIX_DESTRUCT(&wrap);
            return PMIX_ERR_NOMEM;
        }
        wrap.value->type = PMIX_DATA_ARRAY;
        wrap.value->data.darray = darray;
        PMIX_BFROPS_PACK(rc, peer, buff, &wrap, 1, PMIX_KVAL);
        PMIX_DESTRUCT(&wrap);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            return rc;
        }
    }

    if (NULL == trk->session) {
        return PMIX_SUCCESS;
    }
    PMIX_CONSTRUCT(&results, pmix_list_t);
    rc = pmix_gds_hash_xfer_sessioninfo(peer, trk->session, NULL, &results);
    if (PMIX_SUCCESS != rc) {
        /* nothing to say is not a failure */
        PMIX_LIST_DESTRUCT(&results);
        return PMIX_SUCCESS;
    }
    PMIX_LIST_FOREACH (kvptr, &results, pmix_kval_t) {
        PMIX_BFROPS_PACK(rc, peer, buff, kvptr, 1, PMIX_KVAL);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            break;
        }
    }
    PMIX_LIST_DESTRUCT(&results);
    return rc;
}

/* Take delivery of the above.
 *
 * Straight into the same processing a session array takes when it
 * arrives with job data, which replaces by key - so this brings the
 * client's copy up to date rather than appending a second one beside
 * it. The session belongs to this client's own job, which is the only
 * one it holds. */
static pmix_status_t hash_accept_update(pmix_buffer_t *buff)
{
    pmix_kval_t kv;
    pmix_job_t *trk;
    pmix_status_t rc;
    int32_t cnt;

    trk = pmix_gds_hash_get_tracker(pmix_globals.myid.nspace, false);
    if (NULL == trk) {
        return PMIX_SUCCESS;
    }
    cnt = 1;
    PMIX_CONSTRUCT(&kv, pmix_kval_t);
    PMIX_BFROPS_UNPACK(rc, pmix_client_globals.myserver, buff, &kv, &cnt,
                       PMIX_KVAL);
    while (PMIX_SUCCESS == rc) {
        if (PMIX_CHECK_KEY(&kv, PMIX_SESSION_INFO_ARRAY)) {
            rc = pmix_gds_hash_process_session_array(kv.value, trk);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DESTRUCT(&kv);
                return rc;
            }
        }
        else if (PMIX_CHECK_KEY(&kv, PMIX_JOB_INFO_ARRAY)) {
            /* Job-level values the host added after we attached. Each
             * goes where hash_cache_job_info() would have put it had it
             * been there at the time - which for a key naming a realm is
             * that realm, not the job's own table - and the stores
             * replace by key, so a notice that arrives twice, or one
             * that restates what we already hold, lands on the same
             * state. */
            pmix_info_t *uptr;
            size_t nu, u;

            if (PMIX_DATA_ARRAY != kv.value->type ||
                NULL == kv.value->data.darray ||
                NULL == kv.value->data.darray->array) {
                PMIX_DESTRUCT(&kv);
                return PMIX_ERR_TYPE_MISMATCH;
            }
            uptr = (pmix_info_t *) kv.value->data.darray->array;
            nu = kv.value->data.darray->size;
            for (u = 0; u < nu; u++) {
                pmix_kval_t ukv;

                rc = store_lone_realm_key(trk, UINT32_MAX, uptr[u].key,
                                          &uptr[u].value);
                if (PMIX_ERR_TAKE_NEXT_OPTION == rc) {
                    /* no realm of its own - the job's own table */
                    ukv.key = uptr[u].key;
                    ukv.value = &uptr[u].value;
                    rc = pmix_hash_store(&trk->internal, PMIX_RANK_WILDCARD,
                                         &ukv, NULL, 0, NULL);
                }
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    PMIX_DESTRUCT(&kv);
                    return rc;
                }
            }
        }
        else {
            /* A kind of entry this build has not been taught to read.
             * Skip it: a reader tolerates what it does not recognize, so
             * that a server which has learned to send one more thing can
             * still talk to a client that has not learned to read it.
             * Storing it as job data instead - which is what "not a
             * session array" used to mean here - would misfile it under
             * its own key, where nothing looks for it. */
            ;
        }
        PMIX_DESTRUCT(&kv);
        cnt = 1;
        PMIX_CONSTRUCT(&kv, pmix_kval_t);
        PMIX_BFROPS_UNPACK(rc, pmix_client_globals.myserver, buff, &kv, &cnt,
                           PMIX_KVAL);
    }
    PMIX_DESTRUCT(&kv);
    return PMIX_SUCCESS;
}

/* What a job-level value does BESIDES being stored.
 *
 * A few job-level keys are read back out of the namespace object rather
 * than out of the datastore, and storing one without applying it leaves
 * the library answering the new value from the store while every
 * decision derived from it still uses the old one.
 *
 * PMIX_JOB_SIZE is the one that matters: nptr->nprocs is what bounds
 * the per-rank walks in this module, and what pmix_server_fence.c
 * compares against nlocalprocs to decide whether a job is entirely
 * local. The two repair sites elsewhere in this file only fire when
 * nprocs is still zero, so they cannot correct a value that is merely
 * out of date.
 *
 * Shared so the registration that first stores a job description and
 * the update that revises one cannot disagree about what a value means.
 */
static void apply_job_value_effects(pmix_namespace_t *nptr,
                                    const pmix_info_t *info)
{
    if (PMIX_CHECK_KEY(info, PMIX_JOB_SIZE)) {
        uint32_t sz = 0;
        /* asked for rather than read out of the union: the value comes
         * from a host, and one that sends the size as some other integer
         * width would otherwise set nprocs from the wrong bytes */
        if (PMIX_SUCCESS == PMIx_Value_get_number((pmix_value_t *)&info->value,
                                                  &sz, PMIX_UINT32)) {
            nptr->nprocs = sz;
        }
    } else if (PMIX_CHECK_KEY(info, PMIX_NUM_NODES) ||
               PMIX_CHECK_KEY(info, PMIX_MAX_PROCS)) {
        /* recorded by the caller that tracks which keys it was given */
    } else {
        pmix_iof_check_flags((pmix_info_t *)info, &nptr->iof_flags);
    }
}

/* Does this job's internal table already answer "key" with this value? */
static bool job_value_unchanged(pmix_job_t *trk, pmix_info_t *info)
{
    pmix_list_t kvs;
    pmix_kval_t *cur;
    bool same = false;

    PMIX_CONSTRUCT(&kvs, pmix_list_t);
    if (PMIX_SUCCESS == pmix_hash_fetch(&trk->internal, PMIX_RANK_WILDCARD,
                                        info->key, NULL, 0, &kvs, NULL)) {
        cur = (pmix_kval_t *) pmix_list_get_first(&kvs);
        if (NULL != cur && NULL != cur->value) {
            same = (PMIX_EQUAL == PMIx_Value_compare(cur->value,
                                                     &info->value));
        }
    }
    PMIX_LIST_DESTRUCT(&kvs);
    return same;
}

/* Record a changed job-level value for the clients already running.
 *
 * Replaces by key rather than appending, so a host that keeps restating
 * a value it keeps changing does not grow this list without bound - it
 * is bounded by the number of distinct keys added, and each entry says
 * what that key is NOW, which is all a client catching up needs.
 */
static pmix_status_t note_job_update(pmix_job_t *trk, pmix_info_t *info)
{
    pmix_kval_t *kv, *old, *nxt;

    /* Build the replacement BEFORE dropping what it replaces. The other
     * order loses the record entirely if either step below fails - and
     * the caller has already stored the new value by then, so the server
     * would answer with it while the clients were never told. Same rule,
     * and the same reason, as the copy-before-release in
     * pmix_hash_store(). */
    PMIX_KVAL_NEW(kv, info->key);
    if (NULL == kv) {
        return PMIX_ERR_NOMEM;
    }
    if (PMIX_SUCCESS != PMIx_Value_xfer(kv->value, &info->value)) {
        PMIX_RELEASE(kv);
        return PMIX_ERR_NOMEM;
    }
    PMIX_LIST_FOREACH_SAFE (old, nxt, &trk->updates, pmix_kval_t) {
        if (PMIX_CHECK_KEY(old, info->key)) {
            pmix_list_remove_item(&trk->updates, &old->super);
            PMIX_RELEASE(old);
            break;
        }
    }
    pmix_list_append(&trk->updates, &kv->super);
    return PMIX_SUCCESS;
}

/* Add job-level data to a namespace that is already registered.
 *
 * The global cache is copied into a namespace's tables once, at
 * registration; this is how an addition afterwards reaches one that is
 * already running. Job-level values live under PMIX_RANK_WILDCARD on
 * the internal table, which is where hash_cache_job_info() would have
 * put them had they been there at the time.
 *
 * A host may send only what changed or restate the whole description
 * with a few values different in it - see PMIx_server_register_nspace(3)
 * - so unchanged entries are dropped here. That costs a lookup per entry
 * and buys two things: a restatement notifies nobody, and what IS
 * notified is only what a client does not already have. gds/shmem3
 * applies the same rule, and has a harder reason to: a segment it
 * publishes is never reclaimed.
 */
static pmix_status_t hash_add_job_data(const char *nspace,
                                       pmix_info_t info[],
                                       size_t ninfo)
{
    pmix_job_t *trk;
    pmix_kval_t kv;
    pmix_status_t rc = PMIX_SUCCESS;
    size_t n, nchanged = 0;

    if (NULL == nspace || 0 == ninfo || NULL == info) {
        return PMIX_SUCCESS;
    }
    trk = pmix_gds_hash_get_tracker(nspace, false);
    if (NULL == trk) {
        return PMIX_SUCCESS;
    }
    for (n = 0; n < ninfo; n++) {
        if (0 == strlen(info[n].key) ||
            job_value_unchanged(trk, &info[n])) {
            continue;
        }
        /* a borrowed view, as the other job-level stores here use:
         * pmix_hash_store() copies what it keeps */
        kv.key = info[n].key;
        kv.value = &info[n].value;
        rc = pmix_hash_store(&trk->internal, PMIX_RANK_WILDCARD, &kv,
                             NULL, 0, NULL);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            return rc;
        }
        rc = note_job_update(trk, &info[n]);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            return rc;
        }
        /* and whatever this value means beyond being stored - a revised
         * PMIX_JOB_SIZE has to reach nptr->nprocs, or the store answers
         * the new size while every walk bounded by it uses the old one */
        apply_job_value_effects(trk->nptr, &info[n]);
        nchanged++;
    }
    if (0 == nchanged) {
        /* a restatement that changed nothing - nobody needs telling */
        return PMIX_SUCCESS;
    }
    /* and tell the clients already reading it */
    pmix_server_notify_gds_update(nspace);
    return PMIX_SUCCESS;
}

static pmix_status_t nspace_del(const char *nspace);

static pmix_status_t assemb_kvs_req(const pmix_proc_t *proc, pmix_list_t *kvs, pmix_buffer_t *bo,
                                    void *cbdata);

static pmix_status_t accept_kvs_resp(pmix_buffer_t *buf, pmix_list_t *jobvals);


static pmix_status_t mark_modex_complete(struct pmix_peer_t *peer,
                                         pmix_list_t *nslist,
                                         pmix_buffer_t *buff);

static pmix_status_t recv_modex_complete(pmix_buffer_t *buff);

pmix_gds_base_module_t pmix_hash_module = {
    .name = "hash",
    .is_tsafe = false,
    .init = hash_init,
    .finalize = hash_finalize,
    .assign_module = hash_assign_module,
    .cache_job_info = hash_cache_job_info,
    .register_job_info = hash_register_job_info,
    .store_job_info = hash_store_job_info,
    .store = pmix_gds_hash_store,
    .store_modex = hash_store_modex,
    .fetch = pmix_gds_hash_fetch,
    .setup_fork = setup_fork,
    .add_nspace = nspace_add,
    .del_nspace = nspace_del,
    .add_session = session_add,
    .del_session = session_del,
    .pack_update = hash_pack_update,
    .accept_update = hash_accept_update,
    .add_job_data = hash_add_job_data,
    .assemb_kvs_req = assemb_kvs_req,
    .accept_kvs_resp = accept_kvs_resp,
    .fetch_arrays = pmix_gds_hash_fetch_arrays,
    .mark_modex_complete = mark_modex_complete,
    .recv_modex_complete = recv_modex_complete
};

static pmix_status_t hash_init(pmix_info_t info[], size_t ninfo)
{

    PMIX_HIDE_UNUSED_PARAMS(info, ninfo);

    PMIX_CONSTRUCT(&pmix_mca_gds_hash_component.mysessions, pmix_list_t);
    PMIX_CONSTRUCT(&pmix_mca_gds_hash_component.myjobs, pmix_list_t);

    return PMIX_SUCCESS;
}

static void hash_finalize(void)
{
    PMIX_LIST_DESTRUCT(&pmix_mca_gds_hash_component.mysessions);
    PMIX_LIST_DESTRUCT(&pmix_mca_gds_hash_component.myjobs);
    return;
}

static pmix_status_t hash_assign_module(pmix_info_t *info, size_t ninfo, int *priority)
{
    size_t n, m;
    char **options;

    *priority = 10;
    if (NULL != info) {
        for (n = 0; n < ninfo; n++) {
            if (0 == strncmp(info[n].key, PMIX_GDS_MODULE, PMIX_MAX_KEYLEN)) {
                /* the directive comes from the application or the
                 * environment, so it may be malformed - a non-string
                 * value, or a string that splits to nothing. Either way
                 * we simply keep our default bid. */
                if (PMIX_STRING != info[n].value.type ||
                    NULL == info[n].value.data.string) {
                    break;
                }
                options = PMIx_Argv_split(info[n].value.data.string, ',');
                if (NULL == options) {
                    break;
                }
                for (m = 0; NULL != options[m]; m++) {
                    if (0 == strcmp(options[m], "hash")) {
                        /* they specifically asked for us */
                        *priority = 100;
                        break;
                    }
                }
                PMIx_Argv_free(options);
                break;
            }
        }
    }
    return PMIX_SUCCESS;
}

static pmix_status_t hash_cache_job_info(struct pmix_namespace_t *ns,
                                         pmix_info_t info[],
                                         size_t ninfo)
{
    pmix_namespace_t *nptr = (pmix_namespace_t *) ns;
    pmix_job_t *trk;
    pmix_hash_table_t *ht;
    pmix_kval_t *kvptr, kv;
    pmix_value_t val;
    pmix_info_t *iptr;
    char **nodes = NULL, **procs = NULL;
    uint32_t sid = UINT32_MAX;
    pmix_rank_t rank;
    pmix_status_t rc = PMIX_SUCCESS;
    size_t n, j, size, idpos;
    uint32_t flags = 0;
    bool found;

    pmix_output_verbose(2, pmix_gds_base_framework.framework_output,
                        "[%s:%d] gds:hash:cache_job_info for nspace %s with %lu info",
                        pmix_globals.myid.nspace, pmix_globals.myid.rank, nptr->nspace, ninfo);

    trk = pmix_gds_hash_get_tracker(nptr->nspace, true);
    if (NULL == trk) {
        return PMIX_ERR_NOMEM;
    }

    /* if there isn't any data, then be content with just
     * creating the tracker */
    if (NULL == info || 0 == ninfo) {
        return PMIX_SUCCESS;
    }

    /* cache the job info on the internal hash table for this nspace */
    ht = &trk->internal;
    for (n = 0; n < ninfo; n++) {
        pmix_output_verbose(12, pmix_gds_base_framework.framework_output,
                            "%s gds:hash:cache_job_info for key %s",
                            PMIX_NAME_PRINT(&pmix_globals.myid), info[n].key);
        if (PMIX_CHECK_KEY(&info[n], PMIX_SESSION_ID)) {
            rc = PMIx_Value_get_number(&info[n].value, &sid, PMIX_UINT32);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                goto release;
            }
            /* called for the binding it performs - this job now belongs
             * to that session. The tracker it returns is not needed
             * here; a lone session key below finds it by the same id. */
            (void) pmix_gds_hash_check_session(trk, sid, true);
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_SESSION_INFO_ARRAY)) {
            if (PMIX_SUCCESS != (rc = pmix_gds_hash_process_session_array(&info[n].value, trk))) {
                PMIX_ERROR_LOG(rc);
                goto release;
            }
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_JOB_INFO_ARRAY)) {
            rc = pmix_gds_hash_process_job_array(&info[n], trk, &flags, &procs, &nodes);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                goto release;
            }
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_APP_INFO_ARRAY)) {
            if (PMIX_SUCCESS != (rc = pmix_gds_hash_process_app_array(&info[n].value, trk))) {
                PMIX_ERROR_LOG(rc);
                goto release;
            }
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_NODE_INFO_ARRAY)) {
            rc = pmix_gds_hash_process_node_array(&info[n].value, &trk->nodeinfo);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                goto release;
            }
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_NODE_MAP)) {
            /* not allowed to get this more than once */
            if (flags & PMIX_HASH_NODE_MAP) {
                PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
                rc = PMIX_ERR_BAD_PARAM;
                goto release;
            }
            /* parse the regex to get the argv array of node names */
            rc = pmix_gds_hash_parse_nodemap(&info[n].value, &nodes);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                goto release;
            }
            /* mark that we got the map */
            flags |= PMIX_HASH_NODE_MAP;
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_PROC_MAP)) {
            /* not allowed to get this more than once */
            if (flags & PMIX_HASH_PROC_MAP) {
                PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
                rc = PMIX_ERR_BAD_PARAM;
                goto release;
            }
            /* parse the regex to get the argv array containing proc ranks on each node */
            rc = pmix_gds_hash_parse_procmap(&info[n].value, &procs);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                goto release;
            }
            /* mark that we got the map */
            flags |= PMIX_HASH_PROC_MAP;
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_PROC_INFO_ARRAY)) {
            found = false;
            /* an array of data pertaining to a specific proc */
            if (PMIX_DATA_ARRAY != info[n].value.type ||
                NULL == info[n].value.data.darray ||
                NULL == info[n].value.data.darray->array) {
                PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
                rc = PMIX_ERR_TYPE_MISMATCH;
                goto release;
            }
            size = info[n].value.data.darray->size;
            iptr = (pmix_info_t *) info[n].value.data.darray->array;
            /* the array has to say which proc it describes - the host
             * may do that with a rank or a procid, in any position */
            rc = pmix_gds_base_proc_array_id(iptr, size, &rank, &idpos);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                goto release;
            }
            /* cycle thru the values for this rank and store them,
             * skipping the entry that identified the array */
            for (j = 0; j < size; j++) {
                if (j == idpos) {
                    continue;
                }
                /* if the key is PMIX_QUALIFIED_VALUE, then the value
                 * consists of a data array that starts with the key-value
                 * itself followed by the qualifiers */
                pmix_output_verbose(2, pmix_gds_base_framework.framework_output,
                                    "[%s:%d] gds:hash:cache_job_info proc data for [%s:%u]: key %s",
                                    pmix_globals.myid.nspace, pmix_globals.myid.rank, trk->ns, rank,
                                    iptr[j].key);
                if (PMIX_CHECK_KEY(&iptr[j], PMIX_QUALIFIED_VALUE)) {
                    rc = pmix_gds_hash_store_qualified(ht, rank, &iptr[j].value);
                    if (PMIX_SUCCESS != rc) {
                        PMIX_ERROR_LOG(rc);
                        goto release;
                    }
                }
                else {
                    kv.key = iptr[j].key;
                    kv.value = &iptr[j].value;
                    /* store it in the hash_table */
                    rc = pmix_hash_store(ht, rank, &kv, NULL, 0, NULL);
                    if (PMIX_SUCCESS != rc) {
                        PMIX_ERROR_LOG(rc);
                        goto release;
                    }
                }
                /* if this is the appnum, pass it to the pmdl framework */
                if (PMIX_CHECK_KEY(&iptr[j], PMIX_APPNUM)) {
                    pmix_pmdl.setup_client(trk->nptr, rank, iptr[j].value.data.uint32);
                    found = true;
                    if (rank == pmix_globals.myid.rank) {
                        pmix_globals.appnum = iptr[j].value.data.uint32;
                    }
                }
            }
            if (!found) {
                /* if they didn't give us an appnum for this proc, we have
                 * to assume it is appnum=0 */
                uint32_t zero = 0;
                kv.key = PMIX_APPNUM;
                kv.value = &val;
                PMIX_VALUE_LOAD(&val, &zero, PMIX_UINT32);
                rc = pmix_hash_store(ht, rank, &kv, NULL, 0, NULL);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    goto release;
                }
                pmix_pmdl.setup_client(trk->nptr, rank, val.data.uint32);
            }
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_MODEL_LIBRARY_NAME) ||
                   PMIX_CHECK_KEY(&info[n], PMIX_PROGRAMMING_MODEL) ||
                   PMIX_CHECK_KEY(&info[n], PMIX_MODEL_LIBRARY_VERSION) ||
                   PMIX_CHECK_KEY(&info[n], PMIX_PERSONALITY)) {
            // pass this info to the pmdl framework
            pmix_pmdl.setup_nspace(trk->nptr, &info[n]);
        } else if (PMIX_ERR_TAKE_NEXT_OPTION !=
                   (rc = store_lone_realm_key(trk, sid, info[n].key,
                                              &info[n].value))) {
            /* A lone key naming a realm. The rule - this job's session,
             * this node, the one app - lives in store_lone_realm_key()
             * so that the other path taking one, the reply to a keyed
             * get in accept_kvs_resp(), cannot file it somewhere else.
             * TAKE_NEXT_OPTION means the key names no realm, and the
             * arms below handle it as job data. */
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                goto release;
            }
        } else {
            if (PMIX_CHECK_KEY(&info[n], PMIX_QUALIFIED_VALUE)) {
                rc = pmix_gds_hash_store_qualified(ht, PMIX_RANK_WILDCARD, &info[n].value);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    goto release;
                }
            } else {
                /* just a value relating to the entire job */
                kv.key = info[n].key;
                kv.value = &info[n].value;
                rc = pmix_hash_store(ht, PMIX_RANK_WILDCARD, &kv, NULL, 0, NULL);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    goto release;
                }
                /* if this is the job size, then store it in
                 * the nptr tracker and flag that we were given it */
                if (PMIX_CHECK_KEY(&info[n], PMIX_JOB_SIZE)) {
                    flags |= PMIX_HASH_JOB_SIZE;
                } else if (PMIX_CHECK_KEY(&info[n], PMIX_NUM_NODES)) {
                    flags |= PMIX_HASH_NUM_NODES;
                } else if (PMIX_CHECK_KEY(&info[n], PMIX_MAX_PROCS)) {
                    flags |= PMIX_HASH_MAX_PROCS;
                }
                apply_job_value_effects(nptr, &info[n]);
            }
        }
    }

    /* now add any global data that was provided */
    if (!trk->gdata_added) {
        PMIX_LIST_FOREACH (kvptr, &pmix_server_globals.gdata, pmix_kval_t) {
            if (PMIX_CHECK_KEY(kvptr, PMIX_QUALIFIED_VALUE)) {
                rc = pmix_gds_hash_store_qualified(ht, PMIX_RANK_WILDCARD, kvptr->value);
            } else {
                rc = pmix_hash_store(ht, PMIX_RANK_WILDCARD, kvptr, NULL, 0, NULL);
            }
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                break;
            }
        }
        trk->gdata_added = true;
    }

    /* we must have the proc AND node maps */
    if (NULL != procs && NULL != nodes) {
        if (PMIX_SUCCESS != (rc = pmix_gds_hash_store_map(trk, nodes, procs, flags))) {
            PMIX_ERROR_LOG(rc);
        }
    }

release:
    if (NULL != nodes) {
        PMIx_Argv_free(nodes);
    }
    if (NULL != procs) {
        PMIx_Argv_free(procs);
    }
    return rc;
}

static pmix_status_t register_info(pmix_peer_t *peer,
                                   pmix_namespace_t *ns,
                                   pmix_buffer_t *reply)
{
    pmix_job_t *trk;
    pmix_hash_table_t *ht;
    pmix_value_t blob;
    pmix_list_t values;
    pmix_status_t rc = PMIX_SUCCESS;
    pmix_info_t *info;
    size_t ninfo, n;
    pmix_kval_t kv, *kvptr;
    pmix_buffer_t buf;
    pmix_rank_t rank;
    pmix_list_t results;
    char *hname;
    pmix_session_t *sptr;

    pmix_output_verbose(2, pmix_gds_base_framework.framework_output,
                        "REGISTERING FOR PEER %s type %d.%d.%d",
                        PMIX_PNAME_PRINT(&peer->info->pname), peer->proc_type.major,
                        peer->proc_type.minor, peer->proc_type.release);

    trk = pmix_gds_hash_get_tracker(ns->nspace, true);
    if (NULL == trk) {
        return PMIX_ERR_NOMEM;
    }
    /* the job data is stored on the internal hash table */
    ht = &trk->internal;

    /* fetch all values from the hash table tied to rank=wildcard */
    PMIX_CONSTRUCT(&values, pmix_list_t);
    rc = pmix_hash_fetch(ht, PMIX_RANK_WILDCARD, NULL, NULL, 0, &values, NULL);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_LIST_DESTRUCT(&values);
        return rc;
    }
    /* Note the rc check after every pack below. This reply is cached on
     * the nspace and handed to every remaining local client of the job,
     * so a pack that fails part way through and is not noticed does not
     * cost one client some data - it silently gives all of them a
     * truncated job description, reported as success. */
    PMIX_LIST_FOREACH(kvptr, &values, pmix_kval_t) {
        PMIX_BFROPS_PACK(rc, peer, reply, kvptr, 1, PMIX_KVAL);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_LIST_DESTRUCT(&values);
            return rc;
        }
    }
    PMIX_LIST_DESTRUCT(&values);

    /* get any session-level info for this job */
    PMIX_CONSTRUCT(&results, pmix_list_t);
    rc = pmix_gds_hash_fetch_sessioninfo(peer, NULL, trk, NULL, 0, &results);
    if (PMIX_SUCCESS == rc) {
        PMIX_LIST_FOREACH (kvptr, &results, pmix_kval_t) {
            PMIX_BFROPS_PACK(rc, peer, reply, kvptr, 1, PMIX_KVAL);
            if (PMIX_SUCCESS != rc) {
                break;
            }
        }
    }
    PMIX_LIST_DESTRUCT(&results);
    if (PMIX_SUCCESS != rc && PMIX_ERR_NOT_FOUND != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    /* if the job's tracker points to a non-default session ID,
     * then we add the default session information to it */
    if (NULL != trk->session && UINT32_MAX != trk->session->session) {
        sptr = pmix_gds_hash_check_session(NULL, UINT32_MAX, false);
        if (NULL != sptr) {
            PMIX_CONSTRUCT(&results, pmix_list_t);
            rc = pmix_gds_hash_xfer_sessioninfo(peer, sptr, NULL, &results);
            if (PMIX_SUCCESS == rc) {
                PMIX_LIST_FOREACH (kvptr, &results, pmix_kval_t) {
                    PMIX_BFROPS_PACK(rc, peer, reply, kvptr, 1, PMIX_KVAL);
                    if (PMIX_SUCCESS != rc) {
                        break;
                    }
                }
            }
            PMIX_LIST_DESTRUCT(&results);
            if (PMIX_SUCCESS != rc && PMIX_ERR_NOT_FOUND != rc) {
                PMIX_ERROR_LOG(rc);
                return rc;
            }
        }
    }

    /* get any node-level info for this job */
    PMIX_CONSTRUCT(&results, pmix_list_t);
    rc = pmix_gds_hash_fetch_nodeinfo(peer, NULL, &trk->nodeinfo, NULL, 0, &results);
    if (PMIX_SUCCESS == rc) {
        PMIX_LIST_FOREACH (kvptr, &results, pmix_kval_t) {
            /* if the peer is earlier than v3.2.x, it is expecting
             * node info to be in the form of an array, but with the
             * hostname as the key. Detect and convert that here */
            if (PMIX_PEER_IS_EARLIER(peer, 3, 1, 100)) {
                info = (pmix_info_t *) kvptr->value->data.darray->array;
                ninfo = kvptr->value->data.darray->size;
                hname = NULL;
                /* find the hostname */
                for (n = 0; n < ninfo; n++) {
                    if (PMIX_CHECK_KEY(&info[n], PMIX_HOSTNAME)) {
                        free(kvptr->key);
                        kvptr->key = strdup(info[n].value.data.string);
                        PMIX_BFROPS_PACK(rc, peer, reply, kvptr, 1, PMIX_KVAL);
                        hname = kvptr->key;
                        break;
                    }
                }
                if (NULL != hname && pmix_gds_hash_check_hostname(pmix_globals.hostname, hname)) {
                    /* older versions are looking for node-level keys for
                     * only their own node as standalone keys */
                    for (n = 0; n < ninfo; n++) {
                        if (pmix_check_node_info(info[n].key)) {
                            kv.key = strdup(info[n].key);
                            kv.value = &info[n].value;
                            PMIX_BFROPS_PACK(rc, peer, reply, &kv, 1, PMIX_KVAL);
                            /* kv is a borrowed view of info[n] on the stack -
                             * it was never PMIX_CONSTRUCTed and its value
                             * points into the caller's array. Running the
                             * kval destructor over it would dereference an
                             * uninitialized obj_class and then free a pointer
                             * into the middle of that array. Only the key is
                             * ours to release. */
                            free(kv.key);
                            kv.key = NULL;
                        }
                    }
                }
            } else {
                PMIX_BFROPS_PACK(rc, peer, reply, kvptr, 1, PMIX_KVAL);
            }
            if (PMIX_SUCCESS != rc) {
                break;
            }
        }
    }
    PMIX_LIST_DESTRUCT(&results);
    if (PMIX_SUCCESS != rc && PMIX_ERR_NOT_FOUND != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    /* get any app-level info for this job */
    PMIX_CONSTRUCT(&results, pmix_list_t);
    rc = pmix_gds_hash_fetch_appinfo(peer, NULL, &trk->apps, NULL, 0, &results);
    if (PMIX_SUCCESS == rc) {
        PMIX_LIST_FOREACH (kvptr, &results, pmix_kval_t) {
            PMIX_BFROPS_PACK(rc, peer, reply, kvptr, 1, PMIX_KVAL);
            if (PMIX_SUCCESS != rc) {
                break;
            }
        }
    }
    PMIX_LIST_DESTRUCT(&results);
    if (PMIX_SUCCESS != rc && PMIX_ERR_NOT_FOUND != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    /* get the proc-level data for each proc in the job */
    pmix_output_verbose(2, pmix_gds_base_framework.framework_output,
                        "FETCHING PROC INFO FOR NSPACE %s NPROCS %u", ns->nspace, ns->nprocs);
    for (rank = 0; rank < ns->nprocs; rank++) {
        pmix_output_verbose(2, pmix_gds_base_framework.framework_output,
                            "FETCHING PROC INFO FOR RANK %s", PMIX_RANK_PRINT(rank));
        PMIX_CONSTRUCT(&values, pmix_list_t);
        rc = pmix_hash_fetch(ht, rank, NULL, NULL, 0, &values, NULL);
        if (PMIX_SUCCESS != rc && PMIX_ERR_NOT_FOUND != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_LIST_DESTRUCT(&values);
            return rc;
        }
        if (0 == pmix_list_get_size(&values)) {
            PMIX_LIST_DESTRUCT(&values);
            continue;
        }
        PMIX_CONSTRUCT(&buf, pmix_buffer_t);
        PMIX_BFROPS_PACK(rc, peer, &buf, &rank, 1, PMIX_PROC_RANK);

        PMIX_LIST_FOREACH(kvptr, &values, pmix_kval_t) {
            if (PMIX_SUCCESS != rc) {
                break;
            }
            PMIX_BFROPS_PACK(rc, peer, &buf, kvptr, 1, PMIX_KVAL);
        }
        PMIX_LIST_DESTRUCT(&values);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DESTRUCT(&buf);
            return rc;
        }
        kv.key = PMIX_PROC_BLOB;
        kv.value = &blob;
        blob.type = PMIX_BYTE_OBJECT;
        PMIX_UNLOAD_BUFFER(&buf, blob.data.bo.bytes, blob.data.bo.size);
        PMIX_BFROPS_PACK(rc, peer, reply, &kv, 1, PMIX_KVAL);
        PMIX_VALUE_DESTRUCT(&blob);
        PMIX_DESTRUCT(&buf);
        if (PMIX_SUCCESS != rc) {
            /* this reply is cached on the nspace and handed to every
             * remaining local client, so a pack that fails here has to
             * fail the registration rather than truncate the job
             * description for all of them - see the note above */
            PMIX_ERROR_LOG(rc);
            return rc;
        }
    }

    /* Reaching here means the whole reply packed: every real failure
     * above returns directly.
     *
     * rc, though, may still hold the PMIX_ERR_NOT_FOUND that the LAST
     * rank's fetch reported, and returning that failed the entire
     * registration - the caller logs it and the client is handed no job
     * data at all. A rank with no per-rank data is ordinary, not an
     * error: the loop runs to ns->nprocs, so any job whose size exceeds
     * the ranks the host actually described ends on one. */
    return PMIX_SUCCESS;
}

/* the purpose of this function is to pack the job-level
 * info stored in the pmix_namespace_t into a buffer and send
 * it to the given client */
static pmix_status_t hash_register_job_info(struct pmix_peer_t *pr, pmix_buffer_t *reply)
{
    pmix_peer_t *peer = (pmix_peer_t *) pr;
    pmix_namespace_t *ns = peer->nptr;
    char *msg;
    pmix_status_t rc;
    pmix_job_t *trk;

    if (!PMIX_PEER_IS_SERVER(pmix_globals.mypeer) &&
        !PMIX_PEER_IS_LAUNCHER(pmix_globals.mypeer)) {
        /* this function is only available on servers */
        PMIX_ERROR_LOG(PMIX_ERR_NOT_SUPPORTED);
        return PMIX_ERR_NOT_SUPPORTED;
    }

    pmix_output_verbose(2, pmix_gds_base_framework.framework_output,
                        "%s gds:hash:register_job_info for peer %s",
                        PMIX_NAME_PRINT(&pmix_globals.myid),
                        PMIX_PEER_PRINT(peer));

    /* first see if we already have processed this data
     * for another peer in this nspace so we don't waste
     * time doing it again */
    if (NULL != ns->jobbkt) {
        pmix_output_verbose(2, pmix_gds_base_framework.framework_output,
                            "[%s:%d] gds:hash:register_job_info copying prepacked payload",
                            pmix_globals.myid.nspace, pmix_globals.myid.rank);
        /* we have packed this before - can just deliver it */
        PMIX_BFROPS_COPY_PAYLOAD(rc, peer, reply, ns->jobbkt);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
        }
        /* now see if we have delivered it to all our local clients for
         * this nspace. ndelivered counts the deliveries that have
         * *completed*, and the caller bumps it after we return - so on
         * this call it names the client before this one. Comparing it
         * with nlocalprocs directly therefore never matched during the
         * nlocalprocs deliveries that actually happen, and the cached
         * copy was held until the namespace itself went away. */
        if (!PMIX_PEER_IS_LAUNCHER(pmix_globals.mypeer) &&
            ns->ndelivered + 1 == ns->nlocalprocs) {
            /* we have, so let's get rid of the packed
             * copy of the data */
            PMIX_RELEASE(ns->jobbkt);
            ns->jobbkt = NULL;
        }
        return rc;
    }

    /* setup a tracker for this nspace as we will likely
     * need it again */
    trk = pmix_gds_hash_get_tracker(ns->nspace, true);
    if (NULL == trk) {
        return PMIX_ERR_NOMEM;
    }

    /* the job info for the specified nspace has
     * been given to us in the info array - pack
     * them for delivery */
    /* pack the name of the nspace */
    pmix_output_verbose(2, pmix_gds_base_framework.framework_output,
                        "[%s:%d] gds:hash:register_job_info packing new payload",
                        pmix_globals.myid.nspace, pmix_globals.myid.rank);
    msg = ns->nspace;
    PMIX_BFROPS_PACK(rc, peer, reply, &msg, 1, PMIX_STRING);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    rc = register_info(peer, ns, reply);
    if (PMIX_SUCCESS == rc) {
        /* if we have more than one local client for this nspace,
         * save this packed object so we don't do this again */
        if (PMIX_PEER_IS_LAUNCHER(pmix_globals.mypeer) || 1 < ns->nlocalprocs) {
            PMIX_RETAIN(reply);
            ns->jobbkt = reply;
        }
    } else {
        PMIX_ERROR_LOG(rc);
    }

    return rc;
}

static pmix_status_t hash_store_job_info(const char *nspace, pmix_buffer_t *buf)
{
    pmix_status_t rc = PMIX_SUCCESS;
    pmix_kval_t kptr, kp2, *kp3, *kp4, kv, kv2;
    pmix_value_t val;
    int32_t cnt;
    size_t nnodes, n, sz, idpos;
    uint32_t i, j, sid = UINT32_MAX;
    char **procs = NULL;
    pmix_byte_object_t *bo;
    pmix_buffer_t buf2;
    pmix_rank_t rank;
    pmix_job_t *trk;
    pmix_hash_table_t *ht;
    char **nodelist = NULL;
    pmix_nodeinfo_t *nd;
    pmix_namespace_t *ns, *nptr;
    pmix_info_t *iptr;
    pmix_session_t *s = NULL;
    pmix_apptrkr_t *apptr;
    bool found;
    bool myproc;

    pmix_output_verbose(2, pmix_gds_base_framework.framework_output,
                        "[%s:%u] pmix:gds:hash store job info for nspace %s",
                        pmix_globals.myid.nspace, pmix_globals.myid.rank, nspace);

    /* check buf data */
    if ((NULL == buf) || (0 == buf->bytes_used)) {
        rc = PMIX_ERR_BAD_PARAM;
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    trk = pmix_gds_hash_get_tracker(nspace, true);
    if (NULL == trk) {
        return PMIX_ERR_NOMEM;
    }
    ht = &trk->internal;

    /* retrieve the nspace pointer */
    nptr = NULL;
    PMIX_LIST_FOREACH (ns, &pmix_globals.nspaces, pmix_namespace_t) {
        if (0 == strcmp(ns->nspace, nspace)) {
            nptr = ns;
            break;
        }
    }
    if (NULL == nptr) {
        /* only can happen if we are out of mem */
        return PMIX_ERR_NOMEM;
    }

    cnt = 1;
    PMIX_CONSTRUCT(&kptr, pmix_kval_t);
    PMIX_BFROPS_UNPACK(rc, pmix_client_globals.myserver, buf, &kptr, &cnt, PMIX_KVAL);
    while (PMIX_SUCCESS == rc) {
        pmix_output_verbose(2, pmix_gds_base_framework.framework_output,
                            "[%s:%u] pmix:gds:hash store job info working key %s",
                            pmix_globals.myid.nspace, pmix_globals.myid.rank,
                            PMIx_Get_attribute_name(kptr.key));
        if (PMIX_CHECK_KEY(&kptr, PMIX_PROC_BLOB)) {
            bo = &(kptr.value->data.bo);
            PMIX_CONSTRUCT(&buf2, pmix_buffer_t);
            PMIX_LOAD_BUFFER(pmix_client_globals.myserver, &buf2, bo->bytes, bo->size);
            /* start by unpacking the rank */
            cnt = 1;
            PMIX_BFROPS_UNPACK(rc, pmix_client_globals.myserver, &buf2,
                               &rank, &cnt, PMIX_PROC_RANK);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DESTRUCT(&kptr);
                PMIX_DESTRUCT(&buf2);
                return rc;
            }
            if (PMIX_CHECK_NSPACE(pmix_globals.myid.nspace, nptr->nspace) &&
                rank == pmix_globals.myid.rank) {
                myproc = true;
            } else {
                myproc = false;
            }
            /* unpack the blob and save the values for this rank */
            cnt = 1;
            PMIX_CONSTRUCT(&kp2, pmix_kval_t);
            PMIX_BFROPS_UNPACK(rc, pmix_client_globals.myserver, &buf2, &kp2, &cnt, PMIX_KVAL);
            while (PMIX_SUCCESS == rc) {
                /* this is data provided by a job-level exchange, so store it
                 * in the job-level data hash_table */
                pmix_output_verbose(2, pmix_gds_base_framework.framework_output,
                    "%s pmix:gds:hash store proc info for rank %u working key %s",
                    PMIX_NAME_PRINT(&pmix_globals.myid), rank, kp2.key);
                if (PMIX_CHECK_KEY(&kp2, PMIX_QUALIFIED_VALUE)) {
                    rc = pmix_gds_hash_store_qualified(ht, rank, kp2.value);
                } else {
                    rc = pmix_hash_store(ht, rank, &kp2, NULL, 0, NULL);
                }
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    PMIX_DESTRUCT(&kp2);
                    PMIX_DESTRUCT(&kptr);
                    PMIX_DESTRUCT(&buf2);
                    return rc;
                }
                if (myproc) {
                    if (PMIX_CHECK_KEY(&kp2, PMIX_APPNUM)) {
                        rc = PMIx_Value_get_number(kp2.value, &pmix_globals.appnum, PMIX_UINT32);
                    } else if (PMIX_CHECK_KEY(&kp2, PMIX_NODEID)) {
                        rc = PMIx_Value_get_number(kp2.value, &pmix_globals.nodeid, PMIX_UINT32);
                    } else if (PMIX_CHECK_KEY(&kp2, PMIX_HOSTNAME) &&
                               PMIX_STRING == kp2.value->type &&
                               NULL != kp2.value->data.string) {
                        if (NULL != pmix_globals.hostname) {
                            free(pmix_globals.hostname);
                        }
                        pmix_globals.hostname = strdup(kp2.value->data.string);
                    }
                }
                cnt = 1;
                PMIX_DESTRUCT(&kp2);
                PMIX_CONSTRUCT(&kp2, pmix_kval_t);
                PMIX_BFROPS_UNPACK(rc, pmix_client_globals.myserver, &buf2, &kp2, &cnt, PMIX_KVAL);
            }
            /* cleanup */
            PMIX_DESTRUCT(&buf2); // releases the original kptr data
            PMIX_DESTRUCT(&kp2);
        } else if (PMIX_CHECK_KEY(&kptr, PMIX_MAP_BLOB)) {
            /* transfer the byte object for unpacking */
            bo = &(kptr.value->data.bo);
            PMIX_CONSTRUCT(&buf2, pmix_buffer_t);
            PMIX_LOAD_BUFFER(pmix_client_globals.myserver, &buf2, bo->bytes, bo->size);
            /* start by unpacking the number of nodes */
            cnt = 1;
            PMIX_BFROPS_UNPACK(rc, pmix_client_globals.myserver, &buf2, &nnodes, &cnt, PMIX_SIZE);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DESTRUCT(&kptr);
                PMIX_DESTRUCT(&buf2);
                return rc;
            }
            for (i = 0; i < nnodes; i++) {
                /* unpack the list of procs on each node */
                cnt = 1;
                PMIX_CONSTRUCT(&kv, pmix_kval_t);
                PMIX_BFROPS_UNPACK(rc, pmix_client_globals.myserver, &buf2, &kv, &cnt, PMIX_KVAL);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    PMIX_DESTRUCT(&kptr);
                    PMIX_DESTRUCT(&buf2);
                    PMIX_DESTRUCT(&kv);
                    return rc;
                }
                /* track the nodes in this nspace */
                PMIx_Argv_append_nosize(&nodelist, kv.key);
                /* check and see if we already have this node */
                nd = pmix_gds_hash_check_nodename(&trk->nodeinfo, kv.key);
                if (NULL == nd) {
                    nd = PMIX_NEW(pmix_nodeinfo_t);
                    nd->hostname = strdup(kv.key);
                    pmix_list_append(&trk->nodeinfo, &nd->super);
                }
                /* save the list of peers for this node */
                kp3 = PMIX_NEW(pmix_kval_t);
                if (NULL == kp3) {
                    PMIX_DESTRUCT(&kptr);
                    PMIX_DESTRUCT(&kv);
                    PMIX_DESTRUCT(&buf2);
                    PMIx_Argv_free(nodelist);
                    return PMIX_ERR_NOMEM;
                }
                kp3->key = strdup(PMIX_LOCAL_PEERS);
                kp3->value = (pmix_value_t *) malloc(sizeof(pmix_value_t));
                if (NULL == kp3->value) {
                    PMIX_RELEASE(kp3);
                    PMIX_DESTRUCT(&kptr);
                    PMIX_DESTRUCT(&kv);
                    PMIX_DESTRUCT(&buf2);
                    PMIx_Argv_free(nodelist);
                    return PMIX_ERR_NOMEM;
                }
                kp3->value->type = PMIX_STRING;
                kp3->value->data.string = strdup(kv.value->data.string);
                /* ensure this item only appears once on the list */
                PMIX_LIST_FOREACH (kp4, &nd->info, pmix_kval_t) {
                    if (PMIX_CHECK_KEY(kp4, kp3->key)) {
                        pmix_list_remove_item(&nd->info, &kp4->super);
                        PMIX_RELEASE(kp4);
                        break;
                    }
                }
                pmix_list_append(&nd->info, &kp3->super);
                /* split the list of procs so we can store their
                 * individual location data */
                procs = PMIx_Argv_split(kv.value->data.string, ',');
                kv2.value = &val;
                val.type = PMIX_STRING;
                for (j = 0; NULL != procs[j]; j++) {
                    /* store the hostname for each proc - again, this is
                     * data obtained via a job-level exchange, so store it
                     * in the job-level data hash_table */
                    kv2.key = PMIX_HOSTNAME;
                    val.data.string = kv.key;
                    rank = strtol(procs[j], NULL, 10);
                    pmix_output_verbose(
                        2, pmix_gds_base_framework.framework_output,
                        "[%s:%u] pmix:gds:hash store map info for rank %u working key %s",
                        pmix_globals.myid.nspace, pmix_globals.myid.rank, rank, kv2.key);
                    /* this describes where *this* proc is running, so it
                     * belongs to that rank. Storing it against
                     * PMIX_RANK_WILDCARD would both lose the per-rank
                     * answer and leave the job-level PMIX_HOSTNAME set to
                     * whichever node happened to be processed last. */
                    rc = pmix_hash_store(ht, rank, &kv2, NULL, 0, NULL);
                    if (PMIX_SUCCESS != rc) {
                        PMIX_ERROR_LOG(rc);
                        PMIX_DESTRUCT(&kptr);
                        PMIX_DESTRUCT(&kv);
                        PMIX_DESTRUCT(&buf2);
                        PMIx_Argv_free(procs);
                        PMIx_Argv_free(nodelist);
                        return rc;
                    }
                }
                PMIx_Argv_free(procs);
                PMIX_DESTRUCT(&kv);
            }
            if (NULL != nodelist) {
                /* store the comma-delimited list of nodes hosting
                 * procs in this nspace */
                kv2.key = PMIX_NODE_LIST;
                kv2.value = &val;
                val.type = PMIX_STRING;
                val.data.string = PMIx_Argv_join(nodelist, ',');
                PMIx_Argv_free(nodelist);
                rc = pmix_hash_store(ht, PMIX_RANK_WILDCARD, &kv2, NULL, 0, NULL);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    PMIX_DESTRUCT(&kptr);
                    PMIX_VALUE_DESTRUCT(&val);
                    PMIX_DESTRUCT(&kv);
                    PMIX_DESTRUCT(&buf2);
                    return rc;
                }
                PMIX_VALUE_DESTRUCT(&val);
            }
            /* cleanup */
            PMIX_DESTRUCT(&buf2);
        } else if (PMIX_CHECK_KEY(&kptr, PMIX_SESSION_ID)) {
            rc = PMIx_Value_get_number(kptr.value, &sid, PMIX_UINT32);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DESTRUCT(&kptr);
                return rc;
            }
            s = pmix_gds_hash_check_session(trk, sid, true);
            if (PMIX_CHECK_NSPACE(nspace, pmix_globals.myid.nspace)) {
                pmix_globals.sessionid = sid;
            }
        } else if (PMIX_CHECK_KEY(&kptr, PMIX_SESSION_INFO_ARRAY)) {
            if (PMIX_SUCCESS != (rc = pmix_gds_hash_process_session_array(kptr.value, trk))) {
                PMIX_ERROR_LOG(rc);
                PMIX_DESTRUCT(&kptr);
                return rc;
            }
        } else if (pmix_check_session_info(kptr.key)) {
            /* a lone key must belong to this job's session */
            if (NULL == s) {
                s = pmix_gds_hash_check_session(trk, sid, true);
            }
            if (NULL == s) {
                /* the job is already bound to some other session, so
                 * there is nowhere to put this */
                rc = PMIX_ERR_BAD_PARAM;
                PMIX_ERROR_LOG(rc);
                PMIX_DESTRUCT(&kptr);
                return rc;
            }
            /* ensure the value isn't already on the session info */
            found = false;
            PMIX_LIST_FOREACH (kp3, &s->sessioninfo, pmix_kval_t) {
                if (PMIX_CHECK_KEY(kp3, kptr.key)) {
                    if (PMIX_EQUAL == PMIx_Value_compare(kp3->value, kptr.value)) {
                        found = true;
                    } else {
                        pmix_list_remove_item(&s->sessioninfo, &kp3->super);
                        PMIX_RELEASE(kp3);
                    }
                    break;
                }
            }
            if (!found) {
                /* add the provided value */
                kp3 = PMIX_NEW(pmix_kval_t);
                kp3->key = strdup(kptr.key);
                PMIX_VALUE_XFER(rc, kp3->value, kptr.value);
                pmix_list_append(&s->sessioninfo, &kp3->super);
            }
        } else if (PMIX_CHECK_KEY(&kptr, PMIX_APP_INFO_ARRAY)) {
            if (PMIX_SUCCESS != (rc = pmix_gds_hash_process_app_array(kptr.value, trk))) {
                PMIX_ERROR_LOG(rc);
                PMIX_DESTRUCT(&kptr);
                return rc;
            }
        } else if (pmix_check_app_info(kptr.key)) {
            /* they are passing us app-level info for a default
             * app number - have to assume it is app=0 */
            if (0 == pmix_list_get_size(&trk->apps)) {
                apptr = PMIX_NEW(pmix_apptrkr_t);
                pmix_list_append(&trk->apps, &apptr->super);
            } else if (1 < pmix_list_get_size(&trk->apps)) {
                rc = PMIX_ERR_BAD_PARAM;
                PMIX_ERROR_LOG(rc);
                PMIX_DESTRUCT(&kptr);
                return rc;
            } else {
                apptr = (pmix_apptrkr_t *) pmix_list_get_first(&trk->apps);
            }
            /* ensure the value isn't already on the app info */
            found = false;
            PMIX_LIST_FOREACH (kp3, &apptr->appinfo, pmix_kval_t) {
                if (PMIX_CHECK_KEY(kp3, kptr.key)) {
                    if (PMIX_EQUAL == PMIx_Value_compare(kp3->value, kptr.value)) {
                        found = true;
                    } else {
                        pmix_list_remove_item(&apptr->appinfo, &kp3->super);
                        PMIX_RELEASE(kp3);
                    }
                    break;
                }
            }
            if (!found) {
                /* add the provided value */
                kp3 = PMIX_NEW(pmix_kval_t);
                kp3->key = strdup(kptr.key);
                PMIX_VALUE_XFER(rc, kp3->value, kptr.value);
                pmix_list_append(&apptr->appinfo, &kp3->super);
            }
        } else if (PMIX_CHECK_KEY(&kptr, PMIX_NODE_INFO_ARRAY)) {
            rc = pmix_gds_hash_process_node_array(kptr.value, &trk->nodeinfo);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DESTRUCT(&kptr);
                return rc;
            }
        } else if (pmix_check_node_info(kptr.key)) {
            /* they are passing us the node-level info for just this
             * node - start by seeing if our node is on the list */
            nd = pmix_gds_hash_check_nodename(&trk->nodeinfo, pmix_globals.hostname);
            /* if not, then add it */
            if (NULL == nd) {
                nd = PMIX_NEW(pmix_nodeinfo_t);
                nd->hostname = strdup(pmix_globals.hostname);
                pmix_list_append(&trk->nodeinfo, &nd->super);
            }
            /* ensure the value isn't already on the node info */
            found = false;
            PMIX_LIST_FOREACH (kp3, &nd->info, pmix_kval_t) {
                if (PMIX_CHECK_KEY(kp3, kptr.key)) {
                    if (PMIX_EQUAL == PMIx_Value_compare(kp3->value, kptr.value)) {
                        found = true;
                    } else {
                        pmix_list_remove_item(&nd->info, &kp3->super);
                        PMIX_RELEASE(kp3);
                    }
                    break;
                }
            }
            if (!found) {
                /* add the provided value */
                kp3 = PMIX_NEW(pmix_kval_t);
                kp3->key = strdup(kptr.key);
                PMIX_VALUE_XFER(rc, kp3->value, kptr.value);
                pmix_list_append(&nd->info, &kp3->super);
            }
        } else if (PMIX_CHECK_KEY(&kptr, PMIX_PROC_INFO_ARRAY)) {
            /* the caching path checks this; so must the path that takes
             * the same key from a server's reply */
            if (PMIX_DATA_ARRAY != kptr.value->type ||
                NULL == kptr.value->data.darray ||
                NULL == kptr.value->data.darray->array) {
                PMIX_ERROR_LOG(PMIX_ERR_TYPE_MISMATCH);
                PMIX_DESTRUCT(&kptr);
                return PMIX_ERR_TYPE_MISMATCH;
            }
            iptr = (pmix_info_t*)kptr.value->data.darray->array;
            sz = kptr.value->data.darray->size;
            /* the array has to say which proc it describes - the host
             * may do that with a rank or a procid, in any position */
            rc = pmix_gds_base_proc_array_id(iptr, sz, &rank, &idpos);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DESTRUCT(&kptr);
                return rc;
            }
            for (n=0; n < sz; n++) {
                if (n == idpos) {
                    continue;
                }
                PMIX_CONSTRUCT(&kv, pmix_kval_t);
                kv.key = iptr[n].key;
                kv.value = &iptr[n].value;
                if (PMIX_CHECK_KEY(&kv, PMIX_QUALIFIED_VALUE)) {
                    rc = pmix_gds_hash_store_qualified(ht, rank, kv.value);
                } else {
                    rc = pmix_hash_store(ht, rank, &kv, NULL, 0, NULL);
                }
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    PMIX_DESTRUCT(&kptr);
                    return rc;
                }
            }
        } else {
            pmix_output_verbose(2, pmix_gds_base_framework.framework_output,
                                "[%s:%u] pmix:gds:hash store job info storing key %s for WILDCARD rank",
                                pmix_globals.myid.nspace, pmix_globals.myid.rank, kptr.key);
            if (PMIX_CHECK_KEY(&kptr, PMIX_QUALIFIED_VALUE)) {
                rc = pmix_gds_hash_store_qualified(ht, PMIX_RANK_WILDCARD, kptr.value);
            } else {
                rc = pmix_hash_store(ht, PMIX_RANK_WILDCARD, &kptr, NULL, 0, NULL);
            }
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DESTRUCT(&kptr);
                return rc;
            }
            /* if this is the job size, then store it in
             * the nptr tracker */
            if (0 == nptr->nprocs && PMIX_CHECK_KEY(&kptr, PMIX_JOB_SIZE)) {
                nptr->nprocs = kptr.value->data.uint32;
            }
        }
        PMIX_DESTRUCT(&kptr);
        PMIX_CONSTRUCT(&kptr, pmix_kval_t);
        cnt = 1;
        PMIX_BFROPS_UNPACK(rc, pmix_client_globals.myserver, buf, &kptr, &cnt, PMIX_KVAL);
    }
    /* need to release the leftover kptr */
    PMIX_DESTRUCT(&kptr);

    if (PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER != rc) {
        PMIX_ERROR_LOG(rc);
    } else {
        rc = PMIX_SUCCESS;
    }
    return rc;
}

pmix_status_t pmix_gds_hash_store(const pmix_proc_t *proc,
                                  pmix_scope_t scope,
                                  pmix_kval_t *kv)
{
    pmix_job_t *trk;
    pmix_status_t rc;
    pmix_kval_t kp;
    pmix_rank_t rank;
    size_t j, size, idpos;
    pmix_info_t *iptr;

    pmix_output_verbose(2, pmix_gds_base_framework.framework_output,
                        "%s gds:hash:hash_store for proc %s key %s type %s scope %s",
                        PMIX_NAME_PRINT(&pmix_globals.myid), PMIX_NAME_PRINT(proc),
                        PMIx_Get_attribute_name(kv->key),
                        /* a delete carries no value */
                        (NULL == kv->value) ? "NONE"
                                            : PMIx_Data_type_string(kv->value->type),
                        PMIx_Scope_string(scope));

    if (NULL == kv->key) {
        return PMIX_ERR_BAD_PARAM;
    }

    /* find the hash table for this nspace */
    trk = pmix_gds_hash_get_tracker(proc->nspace, true);
    if (NULL == trk) {
        return PMIX_ERR_NOMEM;
    }

    /* A delete names the same audiences as its storing counterpart but
     * removes the key instead of adding it. It carries no value, so none
     * of the array expansion below applies and none of it may run.
     *
     * pmix_hash_remove_data() translates the key through the
     * non-registering lookup, so deleting a key that was never stored
     * neither fails nor grows the keyindex with a name nobody stored.
     * That is also why "not found" is not an error here: asking for a
     * key to be gone when it already is has got what it asked for. */
    if (PMIX_DEL_INTERNAL == scope || PMIX_DEL_LOCAL == scope
        || PMIX_DEL_REMOTE == scope || PMIX_DEL_GLOBAL == scope) {
        if (PMIX_DEL_INTERNAL == scope) {
            rc = pmix_hash_remove_data(&trk->internal, proc->rank, kv->key, NULL);
        } else {
            if (PMIX_DEL_LOCAL == scope || PMIX_DEL_GLOBAL == scope) {
                rc = pmix_hash_remove_data(&trk->local, proc->rank, kv->key, NULL);
                if (PMIX_SUCCESS != rc && PMIX_ERR_NOT_FOUND != rc) {
                    return rc;
                }
            }
            if (PMIX_DEL_REMOTE == scope || PMIX_DEL_GLOBAL == scope) {
                rc = pmix_hash_remove_data(&trk->remote, proc->rank, kv->key, NULL);
                if (PMIX_SUCCESS != rc && PMIX_ERR_NOT_FOUND != rc) {
                    return rc;
                }
            }
            /* And the internal table, unconditionally rather than only
             * for our own rank. It holds two things that look alike from
             * here: the copy of our own data kept to simplify retrieval,
             * and - on a client - whatever we have *read* about another
             * process, which accept_kvs_resp() stores at PMIX_INTERNAL
             * under that process's rank. The second is the whole reason a
             * server tells its clients about a removal at all, so
             * skipping it when the rank is not ours left the cached copy
             * of a peer's deleted key in place and the delete looked
             * like it had done nothing. */
            rc = pmix_hash_remove_data(&trk->internal, proc->rank, kv->key, NULL);
        }
        if (PMIX_ERR_NOT_FOUND == rc) {
            rc = PMIX_SUCCESS;
        }
        return rc;
    }

    /* if this is node/app data, then process it accordingly */
    if (PMIX_CHECK_KEY(kv, PMIX_NODE_INFO_ARRAY)) {
        rc = pmix_gds_hash_process_node_array(kv->value, &trk->nodeinfo);
        return rc;
    } else if (PMIX_CHECK_KEY(kv, PMIX_APP_INFO_ARRAY)) {
        rc = pmix_gds_hash_process_app_array(kv->value, trk);
        return rc;
    } else if (PMIX_CHECK_KEY(kv, PMIX_SESSION_INFO_ARRAY)) {
        rc = pmix_gds_hash_process_session_array(kv->value, trk);
        return rc;
    } else if (PMIX_CHECK_KEY(kv, PMIX_JOB_INFO_ARRAY)) {
        return PMIX_ERR_NOT_SUPPORTED;
    }

    /* see if the proc is me - cannot use CHECK_PROCID as
     * we don't want rank=wildcard to match */
    if (proc->rank == pmix_globals.myid.rank &&
        PMIX_CHECK_NSPACE(proc->nspace, pmix_globals.myid.nspace)) {
        if (PMIX_INTERNAL != scope) {
            /* always maintain a copy of my own info here to simplify
             * later retrieval */
            if (PMIX_CHECK_KEY(kv, PMIX_QUALIFIED_VALUE)) {
                rc = pmix_gds_hash_store_qualified(&trk->internal, proc->rank, kv->value);
            } else {
                rc = pmix_hash_store(&trk->internal, proc->rank, kv, NULL, 0, NULL);
            }
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                return rc;
            }
        }
    }

    /* if the number of procs for the nspace object is new, then update it */
    if (0 == trk->nptr->nprocs && PMIX_CHECK_KEY(kv, PMIX_JOB_SIZE)) {
        trk->nptr->nprocs = kv->value->data.uint32;
    }

    /* store it in the corresponding hash table */
    if (PMIX_INTERNAL == scope) {
        /* if this is proc data, then we have to expand it and
         * store the values on that rank */
        if (PMIX_CHECK_KEY(kv, PMIX_PROC_INFO_ARRAY)) {
            /* an array of data pertaining to a specific proc */
            if (PMIX_DATA_ARRAY != kv->value->type ||
                NULL == kv->value->data.darray ||
                NULL == kv->value->data.darray->array) {
                PMIX_ERROR_LOG(PMIX_ERR_TYPE_MISMATCH);
                return PMIX_ERR_TYPE_MISMATCH;
            }
            size = kv->value->data.darray->size;
            iptr = (pmix_info_t *) kv->value->data.darray->array;
            /* the array has to say which proc it describes - the host
             * may do that with a rank or a procid, in any position */
            rc = pmix_gds_base_proc_array_id(iptr, size, &rank, &idpos);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                return rc;
            }
            /* cycle thru the values for this rank and store them,
             * skipping the entry that identified the array */
            for (j = 0; j < size; j++) {
                if (j == idpos) {
                    continue;
                }
                if (PMIX_CHECK_KEY(&iptr[j], PMIX_QUALIFIED_VALUE)) {
                    rc = pmix_gds_hash_store_qualified(&trk->internal, rank, &iptr[j].value);
                    if (PMIX_SUCCESS != rc) {
                        PMIX_ERROR_LOG(rc);
                        return rc;
                    }
                    continue;
                }
                kp.key = iptr[j].key;
                kp.value = &iptr[j].value;
                pmix_output_verbose(2, pmix_gds_base_framework.framework_output,
                                    "%s gds:hash:STORE data for nspace %s rank %u: key %s",
                                    PMIX_NAME_PRINT(&pmix_globals.myid), trk->ns, rank, kp.key);
                /* store it in the hash_table */
                rc = pmix_hash_store(&trk->internal, rank, &kp, NULL, 0, NULL);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    return rc;
                }
            }
            return PMIX_SUCCESS;
        }
        /* if it isn't proc data, then store it */
        if (PMIX_CHECK_KEY(kv, PMIX_QUALIFIED_VALUE)) {
            rc = pmix_gds_hash_store_qualified(&trk->internal, proc->rank, kv->value);
        } else {
            rc = pmix_hash_store(&trk->internal, proc->rank, kv, NULL, 0, NULL);
        }
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            return rc;
        }
    } else if (PMIX_REMOTE == scope) {
        if (PMIX_CHECK_KEY(kv, PMIX_QUALIFIED_VALUE)) {
            rc = pmix_gds_hash_store_qualified(&trk->remote, proc->rank, kv->value);
        } else {
            rc = pmix_hash_store(&trk->remote, proc->rank, kv, NULL, 0, NULL);
        }
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            return rc;
        }
    } else if (PMIX_LOCAL == scope) {
        if (PMIX_CHECK_KEY(kv, PMIX_QUALIFIED_VALUE)) {
            rc = pmix_gds_hash_store_qualified(&trk->local, proc->rank, kv->value);
        } else {
            rc = pmix_hash_store(&trk->local, proc->rank, kv, NULL, 0, NULL);
        }
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            return rc;
        }
    } else if (PMIX_GLOBAL == scope) {
        if (PMIX_CHECK_KEY(kv, PMIX_QUALIFIED_VALUE)) {
            rc = pmix_gds_hash_store_qualified(&trk->remote, proc->rank, kv->value);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                return rc;
            }
            rc = pmix_gds_hash_store_qualified(&trk->local, proc->rank, kv->value);
        } else {
            rc = pmix_hash_store(&trk->remote, proc->rank, kv, NULL, 0, NULL);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                return rc;
            }
            rc = pmix_hash_store(&trk->local, proc->rank, kv, NULL, 0, NULL);
        }
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            return rc;
        }
    } else {
        return PMIX_ERR_BAD_PARAM;
    }

    return PMIX_SUCCESS;
}

/* this function is only called by the PMIx server when its
 * host has received data from some other peer. It therefore
 * always contains data solely from remote procs, and we
 * shall store it accordingly */
static pmix_status_t hash_store_modex(pmix_buffer_t *buf,
                                      const char *nspace,
                                      void *cbdata)
{
    return pmix_gds_base_store_modex(buf, nspace, _hash_store_modex, cbdata);
}

static pmix_status_t _hash_store_modex(pmix_proc_t *proc,
                                       pmix_buffer_t *pbkt,
                                       uint8_t kind)
{
    pmix_job_t *trk;
    pmix_status_t rc = PMIX_SUCCESS;
    pmix_kval_t kv;
    int32_t cnt;

    /* This store accumulates - a value replaces the one it matches and
     * everything else stays - so a delta contribution needs no different
     * handling here. It matters to a datastore that retires what an
     * earlier modex left behind; see gds/shmem3. */
    PMIX_HIDE_UNUSED_PARAMS(kind);

    if (NULL == pbkt) {
        return PMIX_SUCCESS;
    }

    pmix_output_verbose(2, pmix_gds_base_framework.framework_output,
                        "[%s:%d] gds:hash:store_modex for nspace %s",
                        pmix_globals.myid.nspace,
                        pmix_globals.myid.rank, proc->nspace);

    /* find the hash table for this nspace */
    trk = pmix_gds_hash_get_tracker(proc->nspace, true);
    if (NULL == trk) {
        return PMIX_ERR_NOMEM;
    }

    /* this is data returned via the PMIx_Fence call when
     * data collection was requested, so it only contains
     * REMOTE/GLOBAL data. The byte object contains
     * the pmix_kval_t's published by the given proc
     */

    /* unpack the values until we hit the end of the buffer */
    PMIX_CONSTRUCT(&kv, pmix_kval_t);
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, pmix_globals.mypeer, pbkt, &kv, &cnt, PMIX_KVAL);

    while (PMIX_SUCCESS == rc) {
        /* An entry whose value is PMIX_UNDEF is not data - it is the
         * contributing process saying the key is gone. The modex is
         * additive, so a contribution that merely stops carrying a key
         * removes nothing here; the removal has to be stated, and this
         * is where it is acted on. We can take the key straight out,
         * unlike a datastore whose copy is in a segment it cannot
         * rewrite. */
        if (NULL != kv.value && PMIX_UNDEF == kv.value->type) {
            pmix_rank_t drank = (PMIX_RANK_UNDEF == proc->rank) ? 0 : proc->rank;
            rc = pmix_hash_remove_data(&trk->remote, drank, kv.key, NULL);
            if (PMIX_SUCCESS != rc && PMIX_ERR_NOT_FOUND != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DESTRUCT(&kv);
                return rc;
            }
            /* Our own store is corrected, but our local clients cached
             * this key when they read it - the modex answers a get from
             * the reader's own copy once it has one. Pass the removal on
             * to them, which is what finally closes the loop from the
             * process that deleted the key to a reader on another node. */
            if (PMIX_PEER_IS_SERVER(pmix_globals.mypeer)) {
                pmix_proc_t dproc;
                PMIX_LOAD_PROCID(&dproc, proc->nspace, drank);
                pmix_server_notify_deleted(&dproc, PMIX_DEL_REMOTE, kv.key, NULL);
            }
            PMIX_DESTRUCT(&kv);
            PMIX_CONSTRUCT(&kv, pmix_kval_t);
            cnt = 1;
            PMIX_BFROPS_UNPACK(rc, pmix_globals.mypeer, pbkt, &kv, &cnt, PMIX_KVAL);
            continue;
        }
        if (PMIX_RANK_UNDEF == proc->rank) {
            /* if the rank is undefined, then we store it on the
             * remote table of rank=0 as we know that rank must
             * always exist */
            if (PMIX_CHECK_KEY(&kv, PMIX_QUALIFIED_VALUE)) {
                rc = pmix_gds_hash_store_qualified(&trk->remote, 0, kv.value);
            } else {
                rc = pmix_hash_store(&trk->remote, 0, &kv, NULL, 0, NULL);
            }
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DESTRUCT(&kv);
                return rc;
            }
        } else {
            /* store this in the hash table */
            if (PMIX_CHECK_KEY(&kv, PMIX_QUALIFIED_VALUE)) {
                rc = pmix_gds_hash_store_qualified(&trk->remote, proc->rank, kv.value);
            } else {
                rc = pmix_hash_store(&trk->remote, proc->rank, &kv, NULL, 0, NULL);
            }
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DESTRUCT(&kv);
                return rc;
            }
        }
        PMIX_DESTRUCT(&kv);
        /* continue along */
        PMIX_CONSTRUCT(&kv, pmix_kval_t);
        PMIX_BFROPS_UNPACK(rc, pmix_globals.mypeer, pbkt, &kv, &cnt, PMIX_KVAL);
    }
    PMIX_DESTRUCT(&kv);
    if (PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER != rc) {
        PMIX_ERROR_LOG(rc);
    } else {
        rc = PMIX_SUCCESS;
    }
    return rc;
}

static pmix_status_t setup_fork(const pmix_proc_t *proc, char ***env)
{

    PMIX_HIDE_UNUSED_PARAMS(proc, env);

    /* we don't need to add anything */
    return PMIX_SUCCESS;
}

static pmix_status_t nspace_add(const char *nspace, uint32_t nlocalprocs, pmix_info_t info[],
                                size_t ninfo)
{

    PMIX_HIDE_UNUSED_PARAMS(nspace, nlocalprocs, info, ninfo);

    /* we don't need to do anything here */
    return PMIX_SUCCESS;
}

static pmix_status_t nspace_del(const char *nspace)
{
    pmix_job_t *t;

    /* find the hash table for this nspace */
    PMIX_LIST_FOREACH (t, &pmix_mca_gds_hash_component.myjobs, pmix_job_t) {
        if (0 == strcmp(nspace, t->ns)) {
            /* release it */
            pmix_list_remove_item(&pmix_mca_gds_hash_component.myjobs, &t->super);
            PMIX_RELEASE(t);
            break;
        }
    }
    return PMIX_SUCCESS;
}

static pmix_status_t assemb_kvs_req(const pmix_proc_t *proc,
                                    pmix_list_t *kvs, pmix_buffer_t *buf,
                                    void *cbdata)
{
    pmix_status_t rc = PMIX_SUCCESS;
    pmix_server_caddy_t *cd = (pmix_server_caddy_t *) cbdata;
    pmix_kval_t *kv;

    if (!PMIX_PEER_IS_V1(cd->peer)) {
        PMIX_BFROPS_PACK(rc, cd->peer, buf, proc, 1, PMIX_PROC);
        if (PMIX_SUCCESS != rc) {
            return rc;
        }
    }
    PMIX_LIST_FOREACH (kv, kvs, pmix_kval_t) {
        PMIX_BFROPS_PACK(rc, cd->peer, buf, kv, 1, PMIX_KVAL);
        if (PMIX_SUCCESS != rc) {
            return rc;
        }
    }
    return rc;
}

/* Replace-by-key on one of the realm lists.
 *
 * Every realm keeps its values as a list of kvals, and a value arriving
 * for a key already on it replaces it - an equal one is left alone so
 * the list does not churn. Shared so the three realms cannot drift.
 */
static pmix_status_t realm_list_set(pmix_list_t *target, const char *key,
                                    pmix_value_t *value)
{
    pmix_kval_t *kp2;
    pmix_status_t rc;

    PMIX_LIST_FOREACH (kp2, target, pmix_kval_t) {
        if (PMIX_CHECK_KEY(kp2, key)) {
            if (PMIX_EQUAL == PMIx_Value_compare(kp2->value, value)) {
                return PMIX_SUCCESS;
            }
            pmix_list_remove_item(target, &kp2->super);
            PMIX_RELEASE(kp2);
            break;
        }
    }
    kp2 = PMIX_NEW(pmix_kval_t);
    if (NULL == kp2) {
        return PMIX_ERR_NOMEM;
    }
    kp2->key = strdup(key);
    if (NULL == kp2->key) {
        PMIX_RELEASE(kp2);
        return PMIX_ERR_NOMEM;
    }
    PMIX_VALUE_XFER(rc, kp2->value, value);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(kp2);
        return rc;
    }
    pmix_list_append(target, &kp2->super);
    return PMIX_SUCCESS;
}

/* File a LONE key - one that arrived on its own rather than inside a
 * session, node or app array - in the realm its name says it belongs to.
 *
 * A key that names a realm is answered from that realm, whether or not
 * anything said so (see pmix_gds_base_request_realm()). So it has to be
 * STORED there too, or it is written where no reader of it will look.
 *
 * That was the gap behind a whole class of missing answers. The
 * registration path applied this rule; accept_kvs_resp() did not, and
 * routed by the WRAPPER the server happened to use instead - session,
 * node and app arrays to their realms, and everything else to the job.
 * A keyed PMIx_Get answered by the server returns its value BARE, with
 * no wrapper, so a session-realm value came back, was filed at job
 * level, and the fetch that immediately followed looked for it in the
 * session realm and missed. The value was on the wire and in the
 * process and still unreachable. Both paths now ask the same question
 * of the same function and put the answer in the same place.
 *
 * Returns PMIX_ERR_TAKE_NEXT_OPTION for a key with no realm of its own,
 * which the caller stores as job-level data.
 */
static pmix_status_t store_lone_realm_key(pmix_job_t *trk, uint32_t sid,
                                          const char *key, pmix_value_t *value)
{
    pmix_session_t *s;
    pmix_nodeinfo_t *nd;
    pmix_apptrkr_t *apptr;

    switch (pmix_gds_base_request_realm(key, NULL, 0)) {
        case PMIX_REALM_SESSION:
            /* a lone key must belong to this job's session */
            s = pmix_gds_hash_check_session(trk, sid, true);
            if (NULL == s) {
                /* the job is already bound to some other session, so
                 * there is nowhere to put this */
                return PMIX_ERR_BAD_PARAM;
            }
            return realm_list_set(&s->sessioninfo, key, value);

        case PMIX_REALM_NODE:
            /* node-level info for just this node */
            nd = pmix_gds_hash_check_nodename(&trk->nodeinfo, pmix_globals.hostname);
            if (NULL == nd) {
                nd = PMIX_NEW(pmix_nodeinfo_t);
                if (NULL == nd) {
                    return PMIX_ERR_NOMEM;
                }
                nd->hostname = strdup(pmix_globals.hostname);
                pmix_set_aliases(&nd->aliases, nd->hostname);
                pmix_list_append(&trk->nodeinfo, &nd->super);
            }
            return realm_list_set(&nd->info, key, value);

        case PMIX_REALM_APP:
            /* app-level info for a default app number - assume app=0 */
            if (0 == pmix_list_get_size(&trk->apps)) {
                apptr = PMIX_NEW(pmix_apptrkr_t);
                if (NULL == apptr) {
                    return PMIX_ERR_NOMEM;
                }
                pmix_list_append(&trk->apps, &apptr->super);
            } else if (1 < pmix_list_get_size(&trk->apps)) {
                return PMIX_ERR_BAD_PARAM;
            } else {
                apptr = (pmix_apptrkr_t *) pmix_list_get_first(&trk->apps);
            }
            return realm_list_set(&apptr->appinfo, key, value);

        default:
            /* no realm of its own - the caller files it as job data */
            return PMIX_ERR_TAKE_NEXT_OPTION;
    }
}

static pmix_status_t store_session_info(pmix_nspace_t nspace, pmix_kval_t *kv)
{
    pmix_job_t *trk;
    pmix_status_t rc;

    /* find the hash table for this nspace */
    trk = pmix_gds_hash_get_tracker(nspace, true);
    if (NULL == trk) {
        return PMIX_ERR_NOMEM;
    }
    rc = pmix_gds_hash_process_session_array(kv->value, trk);
    return rc;
}

static pmix_status_t store_node_info(pmix_nspace_t nspace, pmix_kval_t *kv)
{
    pmix_job_t *trk;
    pmix_status_t rc;

    /* find the hash table for this nspace */
    trk = pmix_gds_hash_get_tracker(nspace, true);
    if (NULL == trk) {
        return PMIX_ERR_NOMEM;
    }
    rc = pmix_gds_hash_process_node_array(kv->value, &trk->nodeinfo);
    return rc;
}

static pmix_status_t store_app_info(pmix_nspace_t nspace, pmix_kval_t *kv)
{
    pmix_job_t *trk;
    pmix_status_t rc;

    /* find the hash table for this nspace */
    trk = pmix_gds_hash_get_tracker(nspace, true);
    if (NULL == trk) {
        return PMIX_ERR_NOMEM;
    }
    rc = pmix_gds_hash_process_app_array(kv->value, trk);
    return rc;
}

static pmix_status_t accept_kvs_resp(pmix_buffer_t *buf, pmix_list_t *jobvals)
{
    pmix_status_t rc = PMIX_SUCCESS;
    int32_t cnt;
    pmix_byte_object_t bo;
    pmix_buffer_t pbkt;
    pmix_kval_t kv;
    pmix_proc_t proct;
    pmix_rank_t wildrank;
    bool keep_jobdata;

    /* the incoming payload is provided as a set of packed
     * byte objects, one for each rank. A pmix_proc_t is the first
     * entry in the byte object. If the rank=PMIX_RANK_WILDCARD,
     * then that byte object contains job level info
     * for the provided nspace. Otherwise, the byte
     * object contains the pmix_kval_t's that were "put" by the
     * referenced process */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, pmix_client_globals.myserver, buf, &bo, &cnt, PMIX_BYTE_OBJECT);
    while (PMIX_SUCCESS == rc) {
        /* setup the byte object for unpacking */
        PMIX_CONSTRUCT(&pbkt, pmix_buffer_t);
        PMIX_LOAD_BUFFER(pmix_client_globals.myserver, &pbkt, bo.bytes, bo.size);
        /* unpack the id of the providing process */
        cnt = 1;
        PMIX_BFROPS_UNPACK(rc, pmix_client_globals.myserver, &pbkt, &proct, &cnt, PMIX_PROC);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DESTRUCT(&pbkt);
            return rc;
        }
        /* remembered before the normalization below, which turns UNDEF
         * into our own rank and would hide what this object is */
        wildrank = proct.rank;
        /* if the rank is UNDEF, then we store this on our own
         * rank tables */
        if (PMIX_RANK_UNDEF == proct.rank) {
            proct.rank = pmix_globals.myid.rank;
        }

        /* Is this byte object JOB-LEVEL data, and does this client read
         * its job data somewhere other than here?
         *
         * If so it must not be stored. gds/shmem3 keeps a client's job
         * data in shared segments that the server republishes whenever
         * it changes, and the notice that carries a republication goes
         * to THAT module - so a copy kept here is never refreshed by it.
         * It is not even a fast path: it is only ever consulted when the
         * client's real store has nothing to say, which is exactly when
         * a stale copy answers in place of a miss. One local fetch that
         * missed would then be answered from this copy for the rest of
         * the run, however many times the host revised the value.
         *
         * A client whose server module IS "hash" reads job data from
         * these very tables, so for it this is not a second copy and the
         * store below is right. */
        keep_jobdata = (PMIX_RANK_WILDCARD == wildrank) &&
                       !PMIX_GDS_CHECK_COMPONENT(pmix_client_globals.myserver, "hash");

        cnt = 1;
        PMIX_CONSTRUCT(&kv, pmix_kval_t);
        PMIX_BFROPS_UNPACK(rc, pmix_client_globals.myserver, &pbkt, &kv, &cnt, PMIX_KVAL);
        while (PMIX_SUCCESS == rc) {
            /* if this is an info array, then store it here as dstore
             * doesn't know how to handle it */
            if (PMIX_CHECK_KEY(&kv, PMIX_SESSION_INFO_ARRAY)) {
                rc = store_session_info(proct.nspace, &kv);
            } else if (PMIX_CHECK_KEY(&kv, PMIX_NODE_INFO_ARRAY)) {
                rc = store_node_info(proct.nspace, &kv);
            } else if (PMIX_CHECK_KEY(&kv, PMIX_APP_INFO_ARRAY)) {
                rc = store_app_info(proct.nspace, &kv);
            } else if (keep_jobdata) {
                /* job-level, and this client reads its job data
                 * elsewhere - hand it back for the request in flight
                 * rather than keeping a copy nothing will refresh */
                pmix_kval_t *jk = PMIX_NEW(pmix_kval_t);
                if (NULL == jk) {
                    rc = PMIX_ERR_NOMEM;
                } else {
                    jk->key = strdup(kv.key);
                    PMIX_VALUE_XFER(rc, jk->value, kv.value);
                    if (PMIX_SUCCESS == rc && NULL != jobvals) {
                        pmix_list_append(jobvals, &jk->super);
                    } else {
                        PMIX_RELEASE(jk);
                    }
                }
            } else {
                /* A LONE key, which is what a keyed PMIx_Get is answered
                 * with - no wrapper to route on. File it by what its name
                 * says it is, exactly as the registration path does, or a
                 * session-, node- or app-realm value lands at job level
                 * and the fetch that follows looks for it in its realm
                 * and misses. See store_lone_realm_key(). */
                pmix_job_t *ltrk = pmix_gds_hash_get_tracker(proct.nspace, true);
                if (NULL == ltrk) {
                    rc = PMIX_ERR_NOMEM;
                } else {
                    rc = store_lone_realm_key(ltrk, UINT32_MAX, kv.key, kv.value);
                    if (PMIX_ERR_TAKE_NEXT_OPTION == rc) {
                        /* no realm of its own - job data */
                        rc = pmix_gds_hash_store(&proct, PMIX_INTERNAL, &kv);
                    }
                }
            }
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DESTRUCT(&kv);
                PMIX_DESTRUCT(&pbkt);
                return rc;
            }
            /* get the next one */
            PMIX_DESTRUCT(&kv);
            PMIX_CONSTRUCT(&kv, pmix_kval_t);
            cnt = 1;
            PMIX_BFROPS_UNPACK(rc, pmix_client_globals.myserver, &pbkt, &kv, &cnt, PMIX_KVAL);
        }
        PMIX_DESTRUCT(&kv);
        if (PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DESTRUCT(&pbkt);
            return rc;
        }
        PMIX_DESTRUCT(&pbkt);
        /* get the next one */
        cnt = 1;
        PMIX_BFROPS_UNPACK(rc, pmix_client_globals.myserver, buf, &bo, &cnt, PMIX_BYTE_OBJECT);
    }
    if (PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    /* running off the end of the payload is how we know we are done -
     * report that to the caller as success, not as the unpack error */
    return PMIX_SUCCESS;
}

static pmix_status_t mark_modex_complete(struct pmix_peer_t *peer,
                                         pmix_list_t *nslist,
                                         pmix_buffer_t *buff)
{
    PMIX_HIDE_UNUSED_PARAMS(peer, nslist, buff);
    /* nothing to do. */
    return PMIX_SUCCESS;
}

static pmix_status_t recv_modex_complete(pmix_buffer_t *buff)
{
    PMIX_HIDE_UNUSED_PARAMS(buff);
    /* nothing to do. */
    return PMIX_SUCCESS;
}
