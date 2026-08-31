/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2016-2019 Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2018      IBM Corporation.  All rights reserved.
 * Copyright (c) 2018-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 *
 * Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
 * Copyright (c) 2026      Jeff Squyres  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "src/include/pmix_config.h"

#include "pmix_common.h"
#include "src/include/pmix_globals.h"

#include "src/class/pmix_list.h"
#include "src/mca/pcompress/pcompress.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_show_help.h"

#include "src/mca/gds/base/base.h"
#include "src/runtime/pmix_progress_threads.h"
#include "src/server/pmix_server_ops.h"

/* carries a fetch across to the progress thread - see
 * pmix_gds_base_fetch_kv_tsafe() below */
typedef struct {
    pmix_event_t ev;
    pmix_lock_t lock;
    pmix_peer_t *peer;
    pmix_cb_t *cb;
    pmix_status_t status;
} pmix_gds_fetch_caddy_t;

static void fetch_shift(int sd, short args, void *cbdata)
{
    pmix_gds_fetch_caddy_t *fc = (pmix_gds_fetch_caddy_t *) cbdata;
    pmix_status_t rc;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_ACQUIRE_OBJECT(fc);
    PMIX_GDS_FETCH_KV(rc, fc->peer, fc->cb);
    fc->status = rc;
    PMIX_POST_OBJECT(fc);
    PMIX_WAKEUP_THREAD(&fc->lock);
}

/* Which realm answers this request. THE ONLY PLACE THIS IS DECIDED.
 *
 * Two things used to decide it independently: process_request() in
 * src/client/pmix_client_get.c, from the directives it had parsed, and
 * each gds module's fetch, from the key and the three realm
 * qualifiers. They disagreed about PMIX_JOB_INFO - the client honored
 * it by clearing the realm a key had selected, the modules had never
 * heard of it - so a request could look like a plain job-level lookup
 * to the client and be answered from the session realm by the
 * datastore. Two consequences, one of which is a data race: the caller
 * got an answer from a realm it had explicitly steered away from, and
 * on an is_tsafe module the session structures were read from the
 * application's thread, which is the one thread they are not protected
 * against.
 *
 * The precedence, in one place so it cannot be read two ways:
 *
 *   1. A realm qualifier decides it. Later qualifiers override earlier
 *      ones, and a qualifier set FALSE de-selects its realm without
 *      handing the decision back to the key - "not the node realm" is
 *      an answer, not an absence of one.
 *   2. Failing that, the key decides it: a key that names a realm is
 *      answered from that realm even though nothing said so.
 *   3. Failing that, the job's own realm.
 *
 * A NULL key has no realm of its own - it is an aggregate of everything
 * a proc put - and yields PMIX_REALM_JOB, which is where the whole-job
 * walk starts. The callers that care about aggregates test for the NULL
 * key themselves; this answers only "which realm".
 */
PMIX_EXPORT pmix_realm_t pmix_gds_base_request_realm(const char *key,
                                                     const pmix_info_t info[],
                                                     size_t ninfo)
{
    pmix_realm_t realm = PMIX_REALM_UNDEF;
    bool given = false;
    size_t n;

    for (n = 0; n < ninfo; n++) {
        if (PMIX_CHECK_KEY(&info[n], PMIX_JOB_INFO)) {
            realm = PMIX_REALM_JOB;
            given = true;
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_SESSION_INFO)) {
            realm = PMIX_INFO_TRUE(&info[n]) ? PMIX_REALM_SESSION : PMIX_REALM_JOB;
            given = true;
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_NODE_INFO)) {
            realm = PMIX_INFO_TRUE(&info[n]) ? PMIX_REALM_NODE : PMIX_REALM_JOB;
            given = true;
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_APP_INFO)) {
            realm = PMIX_INFO_TRUE(&info[n]) ? PMIX_REALM_APP : PMIX_REALM_JOB;
            given = true;
        }
    }
    if (given) {
        return realm;
    }
    if (NULL != key) {
        if (pmix_check_session_info(key)) {
            return PMIX_REALM_SESSION;
        }
        if (pmix_check_node_info(key)) {
            return PMIX_REALM_NODE;
        }
        if (pmix_check_app_info(key)) {
            return PMIX_REALM_APP;
        }
    }
    return PMIX_REALM_JOB;
}

/* May this fetch run on the CALLER'S thread against an is_tsafe module?
 *
 * Only a keyed lookup that stays in the job's own realm. Resolving any
 * other realm is further work - in gds/shmem3 it reaches the session
 * tracker and the process-global sessions list, neither of which is
 * synchronized against the progress thread - and a NULL key is an
 * aggregate across scopes rather than a lookup.
 *
 * The realm is passed in rather than computed: by the time anyone asks
 * this, it has already been worked out once for the fetch.
 */
static bool request_runs_inline(const char *key, pmix_realm_t realm)
{
    return (NULL != key && PMIX_REALM_JOB == realm);
}

pmix_status_t pmix_gds_base_fetch_kv_tsafe(struct pmix_peer_t *peer, pmix_cb_t *cb)
{
    /* on the stack because we wait for it: PMIX_WAIT_THREAD holds this
     * frame until the handler has woken us, which is the one shape in
     * which a caddy may live there (see PMIx_Load_topology for the
     * other) */
    pmix_gds_fetch_caddy_t fc;
    pmix_status_t rc;

    /* Work out the realm once, here, and leave it on the caddy: the
     * fetch macro will not recompute what is already set, and the
     * module is handed the answer rather than deriving its own. */
    if (PMIX_REALM_UNDEF == cb->realm) {
        cb->realm = pmix_gds_base_request_realm(cb->key, cb->info, cb->ninfo);
    }
    PMIX_GDS_FETCH_IS_TSAFE(rc, peer);
    if (PMIX_SUCCESS == rc && request_runs_inline(cb->key, cb->realm)) {
        /* the module may be read from anywhere, and this request stays
         * inside the job realm, where that is true */
        PMIX_GDS_FETCH_KV(rc, peer, cb);
        return rc;
    }
    if (pmix_progress_thread_is_current()) {
        /* already the thread that does every store */
        PMIX_GDS_FETCH_KV(rc, peer, cb);
        return rc;
    }
    if (NULL == pmix_globals.evbase ||
        pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        /* there is nobody to hand it to, and nobody to race with either -
         * before the thread starts and after it stops, this process is
         * the only one touching the store */
        PMIX_GDS_FETCH_KV(rc, peer, cb);
        return rc;
    }

    PMIX_CONSTRUCT_LOCK(&fc.lock);
    fc.peer = peer;
    fc.cb = cb;
    fc.status = PMIX_ERR_INIT;
    PMIX_THREADSHIFT(&fc, fetch_shift);
    PMIX_WAIT_THREAD(&fc.lock);
    rc = fc.status;
    PMIX_DESTRUCT_LOCK(&fc.lock);
    return rc;
}

char *pmix_gds_base_get_available_modules(void)
{
    if (!pmix_gds_globals.initialized) {
        return NULL;
    }

    return strdup(pmix_gds_globals.all_mods);
}

/* Select a gds module per the given directives */
pmix_gds_base_module_t *pmix_gds_base_assign_module(pmix_info_t *info, size_t ninfo)
{
    pmix_gds_base_active_module_t *active;
    pmix_gds_base_module_t *mod = NULL;
    int pri, priority = -1;

    if (!pmix_gds_globals.initialized) {
        return NULL;
    }

    PMIX_LIST_FOREACH (active, &pmix_gds_globals.actives, pmix_gds_base_active_module_t) {
        if (NULL == active->module->assign_module) {
            continue;
        }
        if (PMIX_SUCCESS == active->module->assign_module(info, ninfo, &pri)) {
            if (pri < 0) {
                /* use the default priority from the component */
                pri = active->pri;
            }
            if (priority < pri) {
                mod = active->module;
                priority = pri;
            }
        }
    }

    return mod;
}

/* Return the highest-priority active module whose name differs from the
 * failing one, selecting purely by the recorded component priority
 * (active->pri). Unlike pmix_gds_base_assign_module(), this does not
 * consult each module's assign_module callback. No module names are
 * hard-coded. With the modules that ship today (shmem3 at higher priority
 * than hash) the failing module is the highest priority, so this returns
 * the next one down; the general contract is "the highest-priority active
 * module other than the failing one." */
pmix_gds_base_module_t *
pmix_gds_base_get_fallback_module(pmix_gds_base_module_t *failing)
{
    pmix_gds_base_active_module_t *active;
    pmix_gds_base_module_t *best = NULL;
    int best_pri = -1;

    if (!pmix_gds_globals.initialized || NULL == failing) {
        return NULL;
    }

    PMIX_LIST_FOREACH (active, &pmix_gds_globals.actives, pmix_gds_base_active_module_t) {
        if (0 == strcmp(active->module->name, failing->name)) {
            continue;
        }
        if (active->pri > best_pri) {
            best = active->module;
            best_pri = active->pri;
        }
    }

    return best; // NULL if no other module is available
}

pmix_status_t pmix_gds_base_setup_fork(const pmix_proc_t *proc, char ***env)
{
    pmix_gds_base_active_module_t *active;
    pmix_status_t rc;

    if (!pmix_gds_globals.initialized) {
        return PMIX_ERR_INIT;
    }

    PMIX_LIST_FOREACH (active, &pmix_gds_globals.actives, pmix_gds_base_active_module_t) {
        if (NULL == active->module->setup_fork) {
            continue;
        }
        rc = active->module->setup_fork(proc, env);
        if (PMIX_SUCCESS != rc && PMIX_ERR_NOT_AVAILABLE != rc) {
            return rc;
        }
    }

    return PMIX_SUCCESS;
}

pmix_status_t pmix_gds_base_store_modex(pmix_buffer_t *buff,
                                        const char *nspace,
                                        pmix_gds_base_store_modex_cb_fn_t cb_fn,
                                        void *cbdata)
{
    pmix_status_t rc = PMIX_SUCCESS;
    pmix_buffer_t bkt, bkt3;
    pmix_byte_object_t bo, bo3, bo4, wbo;
    int32_t cnt = 1;
    pmix_server_trkr_t *trk = (pmix_server_trkr_t *) cbdata;
    pmix_proc_t proc;
    pmix_buffer_t pbkt;
    bool compressed, decompressed, found;
    pmix_collect_t last_blob_info_byte, blob_info;
    /* PMIX_BYTE writes exactly one byte through this pointer, and
     * pmix_collect_t is an enum carrying a negative member - so the
     * compiler makes it int-sized and an unpack into it leaves three
     * bytes of the comparisons below reading whatever was on the stack.
     * pmix_server_collect_data packs a uint8_t; read one back. */
    uint8_t blob_info_byte;
    pmix_list_t nspaces;
    pmix_proclist_t *plist;

    PMIX_CONSTRUCT(&nspaces, pmix_list_t);
    last_blob_info_byte = PMIX_COLLECT_INVALID;
    PMIX_CONSTRUCT(&bkt, pmix_buffer_t);

    /* Loop over the enclosed byte object envelopes and
     * store them in our GDS module. There is a byte object
     * from each server involved in the operation. */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, pmix_globals.mypeer, buff, &bo, &cnt, PMIX_BYTE_OBJECT);
    /* If the collect flag is set, we should have some data for unpacking */
    if ((PMIX_COLLECT_YES == trk->collect_type) &&
        (PMIX_SUCCESS != rc)) {
       goto exit;
    }

    while (PMIX_SUCCESS == rc) {
        // setup to unpack the server-level blob
        PMIX_LOAD_BUFFER(pmix_globals.mypeer, &bkt, bo.bytes, bo.size);

        while (PMIX_SUCCESS == rc) {

            // each server contribution contains a compression flag followed by
            // a series of rank-level byte objects

            // first element is a bool indicating if the collection of rank-level
            // objects was compressed
            cnt = 1;
            PMIX_BFROPS_UNPACK(rc, pmix_globals.mypeer, &bkt, &compressed, &cnt, PMIX_BOOL);
            if (PMIX_SUCCESS != rc) {
                if (PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER == rc) {
                    // just indicates we reached the end, so silently break
                    break;
                }
                PMIX_ERROR_LOG(rc);
                goto exit;
            }

            // unpack the byte object containing all the rank-level objects
            // provided by this server
            cnt = 1;
            PMIX_BFROPS_UNPACK(rc, pmix_globals.mypeer, &bkt, &bo3, &cnt, PMIX_BYTE_OBJECT);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                goto exit;
            }

            // decompress it if required
            if (compressed) {
                decompressed = pmix_compress.decompress((uint8_t**)&wbo.bytes, &wbo.size,
                                                        (uint8_t*)bo3.bytes, bo3.size);
                if (decompressed) {
                    PMIX_BYTE_OBJECT_DESTRUCT(&bo3);
                    bo3.bytes = wbo.bytes;
                    bo3.size = wbo.size;
                } else {
                    /* The sender said these bytes are compressed. If we
                     * cannot expand them we have no data, not the
                     * original bytes - carrying on would hand the
                     * compressed stream to the unpacker and report
                     * whatever it made of it. */
                    PMIX_ERROR_LOG(PMIX_ERR_UNPACK_FAILURE);
                    PMIX_BYTE_OBJECT_DESTRUCT(&bo3);
                    rc = PMIX_ERR_UNPACK_FAILURE;
                    goto exit;
                }
            }

            // set it up for unpacking
            PMIX_CONSTRUCT(&bkt3, pmix_buffer_t);
            PMIX_LOAD_BUFFER(pmix_globals.mypeer, &bkt3, bo3.bytes, bo3.size);

            // unpack the flag indicating if data was collected on that node
            cnt = 1;
            PMIX_BFROPS_UNPACK(rc, pmix_globals.mypeer, &bkt3, &blob_info_byte, &cnt, PMIX_BYTE);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DESTRUCT(&bkt3);
                goto exit;
            }
            blob_info = (pmix_collect_t) blob_info_byte;
            /* Screen the value before comparing it. The check below only
             * asks whether the servers agree with each other, so a marker
             * they all agree on and none of us can act on would otherwise
             * pass straight through. Two cases, and they fail differently:
             *
             * PMIX_MODEX_DELTA is honored - the kind is handed to the
             * component's callback, which decides what it means for its
             * own storage (gds/hash accumulates, so nothing; gds/shmem3
             * keeps the generations a delta does not repeat).
             *
             * Anything else is a value no release ever defined, so the
             * sender is not something this code can reason about. */
            if (PMIX_COLLECT_NO != blob_info && PMIX_COLLECT_YES != blob_info
                && PMIX_MODEX_DELTA != blob_info) {
                PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
                rc = PMIX_ERR_BAD_PARAM;
                PMIX_DESTRUCT(&bkt3);
                goto exit;
            }
            if (PMIX_COLLECT_INVALID == last_blob_info_byte) {
                last_blob_info_byte = blob_info;
            } else if (last_blob_info_byte != blob_info) {
                // we have a mismatch - report the error
                pmix_show_help("help-pmix-server.txt", "collection-mismatch", true);
                rc = PMIX_ERR_BAD_PARAM;
                PMIX_DESTRUCT(&bkt3);
                goto exit;
            }

           /* unpack the enclosed blobs from the various peers */
            cnt = 1;
            PMIX_BFROPS_UNPACK(rc, pmix_globals.mypeer, &bkt3, &bo4, &cnt, PMIX_BYTE_OBJECT);
            while (PMIX_SUCCESS == rc) {
                /* unpack all the kval's from this peer and store them in
                 * our GDS. Note that PMIx by design holds all data at
                 * the server level until requested. If our GDS is a
                 * shared memory region, then the data may be available
                 * right away - but the client still has to be notified
                 * of its presence. */

                /* setup the byte object for unpacking */
                PMIX_CONSTRUCT(&pbkt, pmix_buffer_t);
                PMIX_LOAD_BUFFER(pmix_globals.mypeer, &pbkt, bo4.bytes, bo4.size);

                // unpack the proc that provided this data
                cnt = 1;
                PMIX_BFROPS_UNPACK(rc, pmix_globals.mypeer, &pbkt, &proc, &cnt, PMIX_PROC);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    PMIX_DESTRUCT(&pbkt);
                    break;
                }

                /* the caller may have asked for only one nspace's data -
                 * local clients of different nspaces can be on different
                 * gds modules, so the same payload gets walked once for
                 * each of them, each time storing only its own share */
                if (NULL != nspace && !PMIX_CHECK_NSPACE(nspace, proc.nspace)) {
                    PMIX_DESTRUCT(&pbkt);
                    /* get the next peer-level blob */
                    cnt = 1;
                    PMIX_BFROPS_UNPACK(rc, pmix_globals.mypeer, &bkt3, &bo4, &cnt,
                                       PMIX_BYTE_OBJECT);
                    continue;
                }

                // track the nspace involved
                found = false;
                PMIX_LIST_FOREACH(plist, &nspaces, pmix_proclist_t) {
                    if (PMIX_CHECK_NSPACE(plist->proc.nspace, proc.nspace)) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    plist = PMIX_NEW(pmix_proclist_t);
                    PMIX_LOAD_NSPACE(plist->proc.nspace, proc.nspace);
                    pmix_list_append(&nspaces, &plist->super);
                }

                // call the specific GDS component-provided function to store it
                rc = cb_fn(&proc, &pbkt, blob_info_byte);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    PMIX_DESTRUCT(&pbkt);
                    break;
                }
                PMIX_DESTRUCT(&pbkt);
                /* get the next peer-level blob */
                cnt = 1;
                PMIX_BFROPS_UNPACK(rc, pmix_globals.mypeer, &bkt3, &bo4, &cnt, PMIX_BYTE_OBJECT);
            }
            PMIX_DESTRUCT(&bkt3);

            if (PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER == rc) {
                rc = PMIX_SUCCESS;
            } else if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                goto exit;
            }
            // prep for next cycle
            PMIX_DESTRUCT(&bkt);
            PMIX_CONSTRUCT(&bkt, pmix_buffer_t);

            /* unpack and process the next server-level blob */
            cnt = 1;
            PMIX_BFROPS_UNPACK(rc, pmix_globals.mypeer, buff, &bo, &cnt, PMIX_BYTE_OBJECT);
            if (PMIX_SUCCESS == rc) {
                PMIX_LOAD_BUFFER(pmix_globals.mypeer, &bkt, bo.bytes, bo.size);
            }
        }
    }

    if (PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER == rc) {
        rc = PMIX_SUCCESS;
    } else if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto exit;
    }
    // indicate that we are done processing the modex - need to alert
    // for each nspace involved
    PMIX_LIST_FOREACH(plist, &nspaces, pmix_proclist_t) {
        rc = cb_fn(&plist->proc, NULL, (uint8_t) last_blob_info_byte);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            break;
        }
    }

exit:
    PMIX_DESTRUCT(&bkt);
    PMIX_LIST_DESTRUCT(&nspaces);
    return rc;
}

pmix_status_t pmix_gds_base_proc_array_id(const pmix_info_t *array, size_t size,
                                          pmix_rank_t *rank, size_t *idpos)
{
    size_t n;
    bool haveproc = false;
    size_t procpos = 0;

    if (NULL == array || 0 == size || NULL == rank || NULL == idpos) {
        return PMIX_ERR_BAD_PARAM;
    }

    for (n = 0; n < size; n++) {
        /* an explicit rank is the preferred identifier, so take it
         * as soon as we see one no matter where it sits */
        if (PMIX_CHECK_KEY(&array[n], PMIX_RANK)) {
            if (PMIX_PROC_RANK != array[n].value.type) {
                return PMIX_ERR_TYPE_MISMATCH;
            }
            *rank = array[n].value.data.rank;
            *idpos = n;
            return PMIX_SUCCESS;
        }
        /* remember the first procid in case no rank turns up */
        if (!haveproc && PMIX_CHECK_KEY(&array[n], PMIX_PROCID)) {
            if (PMIX_PROC != array[n].value.type ||
                NULL == array[n].value.data.proc) {
                return PMIX_ERR_TYPE_MISMATCH;
            }
            haveproc = true;
            procpos = n;
        }
    }

    if (haveproc) {
        *rank = array[procpos].value.data.proc->rank;
        *idpos = procpos;
        return PMIX_SUCCESS;
    }

    /* the array does not say who it describes */
    return PMIX_ERR_TYPE_MISMATCH;
}

pmix_status_t pmix_gds_base_wrap_session_info(uint32_t sessionID,
                                              pmix_info_t info[], size_t ninfo,
                                              pmix_value_t *val)
{
    pmix_data_array_t *array = NULL;
    pmix_info_t *iptr;

    val->type = PMIX_DATA_ARRAY;
    val->data.darray = NULL;

    PMIX_DATA_ARRAY_CREATE(array, ninfo + 1, PMIX_INFO);
    if (NULL == array || NULL == array->array) {
        if (NULL != array) {
            free(array);
        }
        return PMIX_ERR_NOMEM;
    }
    iptr = (pmix_info_t *) array->array;
    /* the session id has to lead: it is what every consumer of this
     * shape reads element zero for */
    PMIX_INFO_LOAD(&iptr[0], PMIX_SESSION_ID, &sessionID, PMIX_UINT32);
    if (0 < ninfo && NULL != info) {
        memcpy(&iptr[1], info, ninfo * sizeof(pmix_info_t));
    }
    val->data.darray = array;
    return PMIX_SUCCESS;
}

void pmix_gds_base_release_session_info(pmix_value_t *val)
{
    pmix_data_array_t *array = val->data.darray;
    pmix_info_t *iptr;

    if (NULL == array) {
        return;
    }
    iptr = (pmix_info_t *) array->array;
    if (NULL != iptr) {
        /* Element zero is the only one we built. Everything past it is a
         * shallow copy of the caller's array, so destructing it here
         * would free values the caller still owns. */
        PMIX_INFO_DESTRUCT(&iptr[0]);
        free(iptr);
    }
    free(array);
    val->data.darray = NULL;
}
