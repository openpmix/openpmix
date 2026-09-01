/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2014-2015 Artem Y. Polyakov <artpol84@gmail.com>.
 *                         All rights reserved.
 * Copyright (c) 2016-2020 Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2016-2018 IBM Corporation.  All rights reserved.
 * Copyright (c) 2018      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * Copyright (c) 2022-2023 Triad National Security, LLC. All rights reserved.
 * Copyright (c) 2026      Jeff Squyres  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/* Preparation of the information a job needs before its processes are
 * started: resource registration, application and node-local setup, and
 * the helper APIs a host uses to build that information. */

#include "src/include/pmix_config.h"

#include "src/include/pmix_stdint.h"

#include "pmix_common.h"
#include "include/pmix_server.h"

#include "src/include/pmix_globals.h"

#ifdef HAVE_STRING_H
#    include <string.h>
#endif
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif

#include "src/hwloc/pmix_hwloc.h"
#include "src/mca/bfrops/base/base.h"
#include "src/mca/gds/base/base.h"
#include "src/mca/pgpu/base/base.h"
#include "src/mca/pmdl/base/base.h"
#include "src/mca/pnet/base/base.h"
#include "src/mca/preg/preg.h"
#include "src/runtime/pmix_progress_threads.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_output.h"

#include "pmix_server_ops.h"

static void _register_resources(int sd, short args, void *cbdata)
{
    pmix_setup_caddy_t *cd = (pmix_setup_caddy_t *) cbdata;
    pmix_kval_t *kv=NULL, kp;
    size_t n, m, ctxid;
    pmix_status_t rc = PMIX_SUCCESS;
    /* the status handed back to the host. It is kept separate from "rc"
     * because "rc" is the transient status of whichever sub-operation ran
     * last: a later success must not erase an earlier failure, and the
     * job-info unpack loop below ends on
     * PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER by design - reporting that
     * as the result told the host every registration carrying job info
     * had failed */
    pmix_status_t ret = PMIX_SUCCESS;
    bool gotctxid = false;
    pmix_list_t grpinfo, endpts;
    pmix_info_caddy_t *ept=NULL, *g=NULL;
    pmix_info_t *iptr, *pinfo;
    size_t ninfo, npinfo;
    pmix_byte_object_t *pbo=NULL, bo;
    pmix_buffer_t jobinfo, bkt;
    int32_t cnt;
    /* The entries that go into the global cache, and so the entries the
     * fan-out below carries into the namespaces that already exist.
     * Borrowed views of cd->info - the array goes, its entries do not. */
    pmix_info_t *fanout = NULL;
    size_t nfanout = 0;
    char *nspace;
    pmix_proc_t *proc;
    pmix_scope_t scope;

    PMIX_ACQUIRE_OBJECT(cd);
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_CONSTRUCT(&grpinfo, pmix_list_t);
    PMIX_CONSTRUCT(&endpts, pmix_list_t);
    for (n = 0; n < cd->ninfo; n++) {
        if (PMIX_CHECK_KEY(&cd->info[n], PMIX_GROUP_INFO_ARRAY) ||
            PMIX_CHECK_KEY(&cd->info[n], PMIX_GROUP_INFO)) {
            /* both spellings are walked below as an array of pmix_info_t,
             * and the key alone does not make the union an array - see
             * pmix_server_valid_darray */
            if (!pmix_server_valid_darray(&cd->info[n], PMIX_INFO, 1)) {
                PMIX_ERROR_LOG(PMIX_ERR_TYPE_MISMATCH);
                if (PMIX_SUCCESS == ret) {
                    ret = PMIX_ERR_TYPE_MISMATCH;
                }
                continue;
            }
            g = PMIX_NEW(pmix_info_caddy_t);
            if (NULL == g) {
                ret = PMIX_ERR_NOMEM;
                goto release;
            }
            /* the caddy is a borrowed view of one element of the host's
             * array, and its "ninfo" is the count of what that element
             * carries. PMIX_NEW leaves both members uninitialized - the
             * class has no constructor - so neither may be left unset */
            g->info = &cd->info[n];
            g->ninfo = cd->info[n].value.data.darray->size;
            pmix_list_append(&grpinfo, &g->super);

        } else if (PMIX_CHECK_KEY(&cd->info[n], PMIX_GROUP_ENDPT_DATA)) {
            /* the procID and the scope occupy the first two positions of
             * the array, so anything shorter than that describes nothing
             * and indexing it would read past the array the host gave us */
            if (!pmix_server_valid_darray(&cd->info[n], PMIX_INFO, 2)) {
                PMIX_ERROR_LOG(PMIX_ERR_TYPE_MISMATCH);
                if (PMIX_SUCCESS == ret) {
                    ret = PMIX_ERR_TYPE_MISMATCH;
                }
                continue;
            }
            ept = PMIX_NEW(pmix_info_caddy_t);
            if (NULL == ept) {
                ret = PMIX_ERR_NOMEM;
                goto release;
            }
            ept->info = &cd->info[n];
            ept->ninfo = cd->info[n].value.data.darray->size;
            pmix_list_append(&endpts, &ept->super);

        } else if (PMIX_CHECK_KEY(&cd->info[n], PMIX_GROUP_CONTEXT_ID)) {
            rc = PMIx_Value_get_number(&cd->info[n].value, &ctxid, PMIX_SIZE);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                if (PMIX_SUCCESS == ret) {
                    ret = rc;
                }
            } else {
                gotctxid = true;
            }

        } else if (PMIX_CHECK_KEY(&cd->info[n], PMIX_GROUP_JOB_INFO)) {
            if (PMIX_BYTE_OBJECT != cd->info[n].value.type) {
                PMIX_ERROR_LOG(PMIX_ERR_TYPE_MISMATCH);
                if (PMIX_SUCCESS == ret) {
                    ret = PMIX_ERR_TYPE_MISMATCH;
                }
                continue;
            }
            pbo = &cd->info[n].value.data.bo;

        } else {
            /* Remember which entries these are. The fan-out below has to
             * carry the SAME set into the namespaces that already exist
             * as this cache carries into the ones registered later, or
             * the two populations answer differently for one host call
             * - forever. Borrowed views of cd->info, which outlives the
             * call. */
            pmix_info_t *ftmp;

            /* ...but only what a job-level fan-out can actually file.
             * The arms above sort out the group keys; they do not sort
             * out a map, a realm array, or a lone key naming the
             * session, node or app realm, and this else arm sweeps all
             * of those in. PMIX_GDS_ADD_JOB_DATA files job-level
             * VALUES, so handing it one of those stores it under its own
             * key where no reader of its realm looks - and counts it as
             * changed on every call, which in gds/shmem3 publishes a
             * segment that is never reclaimed.
             *
             * The same classification PMIx_server_register_nspace()'s
             * update path applies, asked of the same function, so the
             * two ways a host may revise a running job agree about what
             * a job-level value is. Such entries still reach the cache
             * below, which is copied into a namespace by the
             * registration path - and that one does know how to decode
             * them. */
            if (pmix_server_job_update_is_elsewhere(&cd->info[n])) {
                goto cache_it;
            }
            ftmp = (pmix_info_t *)
                realloc(fanout, (nfanout + 1) * sizeof(pmix_info_t));
            if (NULL == ftmp) {
                ret = PMIX_ERR_NOMEM;
                goto release;
            }
            fanout = ftmp;
            memcpy(&fanout[nfanout], &cd->info[n], sizeof(pmix_info_t));
            nfanout++;

cache_it:
            /* add any provided data to our global cache for all nspaces */
            kv = PMIX_NEW(pmix_kval_t);
            if (NULL == kv) {
                ret = PMIX_ERR_NOMEM;
                goto release;
            }
            kv->key = strdup(cd->info[n].key);
            /* calloc, not malloc: the transfer below sets the value's
             * type before it fills in the union, and every copy helper it
             * dispatches to returns its out-of-memory status *without*
             * writing through the destination pointer. So a failed
             * transfer of, say, a PMIX_PROC_INFO left the type saying
             * "pointer" over whatever the heap happened to hold, and the
             * kval destructor then freed that address. Zeroed, the same
             * failure leaves a NULL the destructor skips. */
            kv->value = (pmix_value_t *) calloc(1, sizeof(pmix_value_t));
            if (NULL == kv->key || NULL == kv->value) {
                PMIX_RELEASE(kv);
                ret = PMIX_ERR_NOMEM;
                goto release;
            }
            PMIX_VALUE_XFER(rc, kv->value, &cd->info[n].value);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_RELEASE(kv);
                /* one element the library cannot copy - an unsupported
                 * type, say - is not a reason to drop every element
                 * behind it on the floor and report a single status for
                 * the lot. Every other arm here records the first failure
                 * and keeps walking; so does this one now. */
                if (PMIX_SUCCESS == ret) {
                    ret = rc;
                }
                continue;
            }
            pmix_list_append(&pmix_server_globals.gdata, &kv->super);
        }
    }

    /* The cache above is copied into a namespace's datastore once, when
     * that namespace is first registered, and nothing re-reads it - so
     * everything just added governs only the namespaces registered after
     * this call, and every job already running keeps the description it
     * was given. That is the additive twin of what
     * retract_from_namespaces() fixes for a removal, and it is what this
     * closes: push the addition into the namespaces that already exist.
     *
     * A fan-out per namespace: a server assigns itself "hash", so its
     * own copy of a namespace's job data lives there, while the segments
     * its clients read are shmem3's. Both have to be told. */
    if (0 < nfanout) {
        pmix_namespace_t *nsptr;
        PMIX_LIST_FOREACH (nsptr, &pmix_globals.nspaces, pmix_namespace_t) {
            pmix_status_t nrc;
            PMIX_GDS_ADD_JOB_DATA(nrc, nsptr->nspace, fanout, nfanout);
            if (PMIX_SUCCESS != nrc) {
                PMIX_ERROR_LOG(nrc);
                if (PMIX_SUCCESS == ret) {
                    ret = nrc;
                }
            }
        }
    }

    /* if endpt data was provided, then we need to
     * store it in our hash table */
    if (0 < pmix_list_get_size(&endpts)) {
        /* Each list member points to a pmix_info_t that contains
         * a data array of info about that proc */
        PMIX_LIST_FOREACH(ept, &endpts, pmix_info_caddy_t) {
            /* the array itself was screened when it was collected above,
             * so it is known to hold at least the two leading elements */
            pinfo = (pmix_info_t*)ept->info->value.data.darray->array;
            npinfo = ept->ninfo;
            /* procID is in the first position and the scope in the second,
             * but the position alone does not make the union a proc
             * pointer - a mistyped first element would be dereferenced by
             * the store below as whatever address the host had there */
            if (PMIX_PROC != pinfo[0].value.type ||
                NULL == pinfo[0].value.data.proc ||
                PMIX_SCOPE != pinfo[1].value.type) {
                PMIX_ERROR_LOG(PMIX_ERR_TYPE_MISMATCH);
                if (PMIX_SUCCESS == ret) {
                    ret = PMIX_ERR_TYPE_MISMATCH;
                }
                continue;
            }
            proc = pinfo[0].value.data.proc;
            scope = pinfo[1].value.data.scope;
            // rest of the array contains endpts
            for (m=2; m < npinfo; m++) {
                kp.key = pinfo[m].key;
                kp.value = &pinfo[m].value;
                /* go through the macro rather than calling the module's
                 * store slot by hand: a gds component is free to leave
                 * that slot NULL and rely on the macro routing the
                 * operation to the local module, which is exactly what
                 * gds/shmem3 does. We are assigned "hash" today, so this
                 * happened to work - but only by that coincidence. */
                PMIX_GDS_STORE_KV(rc, pmix_globals.mypeer, proc, scope, &kp);
                if (PMIX_SUCCESS == rc
                    && (PMIX_REMOTE == scope || PMIX_GLOBAL == scope)) {
                    /* this did not come through pmix_server_commit, so it
                     * is not on that proc's pending list - a delta fence
                     * contribution built from that list alone would omit
                     * it. Make the next one cumulative. */
                    pmix_server_modex_resync(proc);
                }
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    if (PMIX_SUCCESS == ret) {
                        ret = rc;
                    }
                    continue;
                }
            }
        }
    }

    // if group info was provided, then we need to store it
    // in our hash table too
    if (0 < pmix_list_get_size(&grpinfo)) {
        // must have been given a context ID
        if (!gotctxid) {
            PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
            ret = PMIX_ERR_BAD_PARAM;
            goto release;
        }
        /* Each list member points to a pmix_info_t that contains
         * a data array of info from a given proc */
        PMIX_LIST_FOREACH(g, &grpinfo, pmix_info_caddy_t) {
            /* screened when it was collected above */
            iptr = (pmix_info_t*)g->info->value.data.darray->array;
            ninfo = g->ninfo;

            if (PMIX_CHECK_KEY(g->info, PMIX_GROUP_INFO)) {
                // this is just a single array of group info
                rc = pmix_server_process_grpinfo(ctxid, iptr, ninfo);
                if (PMIX_SUCCESS != rc) {
                    ret = rc;
                    goto release;
                }
            } else {
                // contains an array of group info arrays
                for (n=0; n < ninfo; n++) {
                    /* each element is itself an array of group info - the
                     * enclosing array's element type says nothing about
                     * what its members carry */
                    if (!pmix_server_valid_darray(&iptr[n], PMIX_INFO, 1)) {
                        PMIX_ERROR_LOG(PMIX_ERR_TYPE_MISMATCH);
                        ret = PMIX_ERR_TYPE_MISMATCH;
                        goto release;
                    }
                    pinfo = (pmix_info_t*)iptr[n].value.data.darray->array;
                    npinfo = iptr[n].value.data.darray->size;
                    rc = pmix_server_process_grpinfo(ctxid, pinfo, npinfo);
                    if (PMIX_SUCCESS != rc) {
                        ret = rc;
                        goto release;
                    }
                }
            }
        }
    }

    // if job info was provided, process it
    if (NULL != pbo) {
        /* Load it for unpacking, but do NOT take it: the byte object sits
         * in the caller's own info array, which the host owns and keeps
         * valid until our callback fires. PMIX_LOAD_BUFFER does not copy -
         * it points the buffer at the payload and NULLs the source - so
         * using it here emptied the host's info element and then leaked
         * the blob, since this buffer is never destructed (destructing it
         * would free memory the host still owns). Borrow instead. */
        PMIX_CONSTRUCT(&jobinfo, pmix_buffer_t);
        PMIX_LOAD_BUFFER_NON_DESTRUCT(pmix_globals.mypeer, &jobinfo,
                                      pbo->bytes, pbo->size);

        cnt = 1;
        PMIX_BFROPS_UNPACK(rc, pmix_globals.mypeer, &jobinfo, &bo, &cnt, PMIX_BYTE_OBJECT);
        while (PMIX_SUCCESS == rc) {
            /* load it for unpacking */
            PMIX_CONSTRUCT(&bkt, pmix_buffer_t);
            PMIX_LOAD_BUFFER(pmix_globals.mypeer, &bkt, bo.bytes, bo.size);

            /* unpack the nspace for this blob */
            cnt = 1;
            PMIX_BFROPS_UNPACK(rc, pmix_globals.mypeer, &bkt, &nspace, &cnt, PMIX_STRING);
            if (PMIX_SUCCESS != rc) {
                /* this ends the walk, so the check below the loop is what
                 * logs it and records it - doing either here as well
                 * reports the same failure twice */
                PMIX_DESTRUCT(&bkt);
                break;
            }
            /* extract and process any proc-related info for this nspace */
            PMIX_GDS_STORE_JOB_INFO(rc, pmix_globals.mypeer, nspace, &bkt);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                if (PMIX_SUCCESS == ret) {
                    ret = rc;
                }
            }
            free(nspace);
            PMIX_DESTRUCT(&bkt);
            /* get the next one */
            cnt = 1;
            /* the same peer that opened this buffer has to keep reading it -
             * myserver is repointed while a launcher-spawned server attaches
             * to its parent, so the two are not interchangeable here */
            PMIX_BFROPS_UNPACK(rc, pmix_globals.mypeer, &jobinfo, &bo, &cnt, PMIX_BYTE_OBJECT);
        }
        /* running the buffer dry is how this loop ends when every blob was
         * consumed, so it is not a failure to report */
        if (PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER != rc) {
            PMIX_ERROR_LOG(rc);
            if (PMIX_SUCCESS == ret) {
                ret = rc;
            }
        }
    }

release:
    /* both scratch lists own the caddies appended to them above and were
     * being dropped on the floor - on the normal path as well as this one */
    PMIX_LIST_DESTRUCT(&grpinfo);
    PMIX_LIST_DESTRUCT(&endpts);
    /* borrowed views of cd->info, so only the array goes */
    if (NULL != fanout) {
        free(fanout);
    }
    cd->opcbfunc(ret, cd->cbdata);
    PMIX_RELEASE(cd);
}

pmix_status_t PMIx_server_register_resources(pmix_info_t info[], size_t ninfo,
                                             pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    pmix_setup_caddy_t *cd;
    pmix_lock_t mylock;
    pmix_status_t rc;

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "pmix:server register resources");

    if (!pmix_atomic_check_bool(&pmix_globals.initialized)) {
        return PMIX_ERR_INIT;
    }

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        return PMIX_ERR_NOT_AVAILABLE;
    }

    /* the handler walks the array "ninfo" times */
    if (0 < ninfo && NULL == info) {
        return PMIX_ERR_BAD_PARAM;
    }

    cd = PMIX_NEW(pmix_setup_caddy_t);
    if (NULL == cd) {
        return PMIX_ERR_NOMEM;
    }
    cd->info = info;
    cd->ninfo = ninfo;
    cd->opcbfunc = cbfunc;
    cd->cbdata = cbdata;

    /* if the provided callback is NULL, then substitute
     * our own internal cbfunc and block here */
    if (NULL == cbfunc) {
        if (pmix_progress_thread_check_blocking("PMIx_server_register_resources")) {
            /* we are ON the progress thread, so waiting for the event we
             * would post is waiting for ourselves - answer rather than
             * hang. The caller wanted the blocking form; the non-blocking
             * one works fine from here */
            PMIX_RELEASE(cd);
            return PMIX_ERR_WOULD_BLOCK;
        }
        PMIX_CONSTRUCT_LOCK(&mylock);
        cd->opcbfunc = pmix_server_lock_opcbfunc;
        cd->cbdata = &mylock;
        PMIX_THREADSHIFT(cd, _register_resources);
        PMIX_WAIT_THREAD(&mylock);
        rc = mylock.status;
        if (PMIX_SUCCESS == rc) {
            rc = PMIX_OPERATION_SUCCEEDED;
        }
        PMIX_DESTRUCT_LOCK(&mylock);
        return rc;
    }

    /* we have to push this into our event library to avoid
     * potential threading issues */
    PMIX_THREADSHIFT(cd, _register_resources);
    return PMIX_SUCCESS;
}

/* The two keys that say which node an array describes. Everything else
 * in a qualifier names something to remove from the entry those two
 * select, and everything else in a stored entry is something that entry
 * describes. */
static bool is_node_identifier(const pmix_info_t *info)
{
    return (PMIX_CHECK_KEY(info, PMIX_NODEID) || PMIX_CHECK_KEY(info, PMIX_HOSTNAME));
}

/* Read the node a qualifier array names.
 *
 * A request element carrying a data array is asking for a *narrowed*
 * removal - this key, on that node, and possibly only these parts of it -
 * rather than the removal of every entry carrying the key. The
 * identifiers select the entry; anything else in the array selects
 * elements within it. A qualifier carrying no identifier at all applies
 * to every entry with that key, which is how a device that is unique
 * system-wide is removed without naming its node.
 *
 * The keys do not make the union anything, so both are screened: these
 * arrive from a host, which is free to get them wrong. */
static pmix_status_t parse_node_qualifier(const pmix_info_t *info,
                                          uint32_t *nodeid, bool *have_nodeid,
                                          char **hostname, size_t *ntargets)
{
    pmix_info_t *iptr;
    size_t n, niptr;
    pmix_status_t rc;

    *have_nodeid = false;
    *hostname = NULL;
    *ntargets = 0;

    if (!pmix_server_valid_darray(info, PMIX_INFO, 1)) {
        return PMIX_ERR_TYPE_MISMATCH;
    }
    iptr = (pmix_info_t *) info->value.data.darray->array;
    niptr = info->value.data.darray->size;

    for (n = 0; n < niptr; n++) {
        if (PMIX_CHECK_KEY(&iptr[n], PMIX_NODEID)) {
            rc = PMIx_Value_get_number(&iptr[n].value, nodeid, PMIX_UINT32);
            if (PMIX_SUCCESS != rc) {
                return PMIX_ERR_TYPE_MISMATCH;
            }
            *have_nodeid = true;

        } else if (PMIX_CHECK_KEY(&iptr[n], PMIX_HOSTNAME)) {
            if (PMIX_STRING != iptr[n].value.type ||
                NULL == iptr[n].value.data.string) {
                return PMIX_ERR_TYPE_MISMATCH;
            }
            *hostname = iptr[n].value.data.string;

        } else {
            ++(*ntargets);
        }
    }
    return PMIX_SUCCESS;
}

/* Does the qualifier ask for this stored element to go?
 *
 * A target matches the element directly - same key, same value - or
 * matches something inside it, which is how a fabric device is named: a
 * node array holds a PMIX_FABRIC_DEVICE element whose own array carries
 * the device's name and id, so a qualifier naming the device by either
 * one selects that whole sub-array. */
static bool element_selected(pmix_info_t *qual, size_t nqual, pmix_info_t *stored)
{
    pmix_info_t *sub;
    size_t n, m, nsub;

    for (n = 0; n < nqual; n++) {
        if (is_node_identifier(&qual[n])) {
            continue;
        }
        if (PMIX_CHECK_KEY(stored, qual[n].key) &&
            PMIX_EQUAL == PMIx_Value_compare(&qual[n].value, &stored->value)) {
            return true;
        }
        if (!pmix_server_valid_darray(stored, PMIX_INFO, 1)) {
            continue;
        }
        sub = (pmix_info_t *) stored->value.data.darray->array;
        nsub = stored->value.data.darray->size;
        for (m = 0; m < nsub; m++) {
            if (PMIX_CHECK_KEY(&sub[m], qual[n].key) &&
                PMIX_EQUAL == PMIx_Value_compare(&qual[n].value, &sub[m].value)) {
                return true;
            }
        }
    }
    return false;
}

/* Remove from this entry every element the qualifier selects, rebuilding
 * the stored array around the survivors.
 *
 * Reports through "empty" that nothing the entry described is left, so
 * the caller drops the entry entirely rather than leave a husk naming a
 * node and nothing else - which is what an empty registration would have
 * produced, and would seed every namespace registered later with it. */
static pmix_status_t prune_entry(pmix_kval_t *kv, pmix_info_t *qual, size_t nqual,
                                 bool *empty)
{
    pmix_data_array_t *darray;
    pmix_info_t *stored, *survivors;
    pmix_status_t rc;
    size_t n, nstored, keep = 0, described = 0, k = 0;

    *empty = false;
    stored = (pmix_info_t *) kv->value->data.darray->array;
    nstored = kv->value->data.darray->size;

    /* count first: the replacement has to be sized before anything moves
     * into it, and the answer also decides whether there is a
     * replacement to build at all */
    for (n = 0; n < nstored; n++) {
        if (!element_selected(qual, nqual, &stored[n])) {
            ++keep;
            if (!is_node_identifier(&stored[n])) {
                ++described;
            }
        }
    }
    if (keep == nstored) {
        return PMIX_SUCCESS; /* the qualifier named nothing this entry has */
    }
    if (0 == described) {
        *empty = true;
        return PMIX_SUCCESS;
    }

    darray = PMIx_Data_array_create(keep, PMIX_INFO);
    /* the descriptor and the block it describes are two allocations, and
     * only the first one is reported: PMIx_Data_array_create hands back a
     * non-NULL descriptor whose "size" is the count asked for and whose
     * "array" is NULL when the block could not be had. Testing the
     * descriptor alone therefore left the walk below writing through
     * NULL. */
    if (NULL == darray) {
        return PMIX_ERR_NOMEM;
    }
    if (NULL == darray->array) {
        PMIx_Data_array_free(darray);
        return PMIX_ERR_NOMEM;
    }
    survivors = (pmix_info_t *) darray->array;
    for (n = 0; n < nstored; n++) {
        if (!element_selected(qual, nqual, &stored[n])) {
            /* PMIX_INFO_XFER discards the status, so the copy is made
             * through the underlying call: an element that would not
             * copy leaves a hole in the replacement with nothing to say
             * which one it was, and the replacement is about to become
             * the entry. Better to leave the entry as it stands and tell
             * the host why. */
            rc = PMIx_Info_xfer(&survivors[k], &stored[n]);
            if (PMIX_SUCCESS != rc) {
                PMIx_Data_array_free(darray);
                return rc;
            }
            ++k;
        }
    }
    /* the value owns the array it points at, so the one being displaced
     * goes back through the same free that would have released it with
     * the entry */
    PMIx_Data_array_free(kv->value->data.darray);
    kv->value->data.darray = darray;
    return PMIX_SUCCESS;
}

/* Does this cached entry describe the node the qualifier named? Only an
 * entry that is itself an array of info can, and its element types are
 * no more ours to assume than the request's were - the entry came from a
 * host too. A hostname must match exactly; PMIX_HOSTNAME_ALIASES is not
 * consulted, since the host that registered the entry chose the spelling
 * it wants to be addressed by. */
static bool entry_names_node(pmix_kval_t *kv, uint32_t nodeid,
                             bool have_nodeid, const char *hostname)
{
    pmix_info_t *iptr;
    size_t n, niptr;
    uint32_t nd;

    if (NULL == kv->value || PMIX_DATA_ARRAY != kv->value->type ||
        NULL == kv->value->data.darray ||
        PMIX_INFO != kv->value->data.darray->type ||
        NULL == kv->value->data.darray->array) {
        return false;
    }
    iptr = (pmix_info_t *) kv->value->data.darray->array;
    niptr = kv->value->data.darray->size;

    for (n = 0; n < niptr; n++) {
        if (have_nodeid && PMIX_CHECK_KEY(&iptr[n], PMIX_NODEID)) {
            if (PMIX_SUCCESS == PMIx_Value_get_number(&iptr[n].value, &nd, PMIX_UINT32) &&
                nd == nodeid) {
                return true;
            }

        } else if (NULL != hostname && PMIX_CHECK_KEY(&iptr[n], PMIX_HOSTNAME)) {
            if (PMIX_STRING == iptr[n].value.type &&
                NULL != iptr[n].value.data.string &&
                0 == strcmp(iptr[n].value.data.string, hostname)) {
                return true;
            }
        }
    }
    return false;
}

/* Take a key back from every namespace that already holds it.
 *
 * The global cache is copied into a namespace's datastore once, when
 * that namespace is first registered - hash_cache_job_info(), guarded by
 * the per-namespace gdata_added flag - and nothing re-reads it. So
 * removing an entry from the cache governs the namespaces registered
 * after it and leaves every running job with its copy. That is what
 * docs/todo.rst recorded as an open decision; this is the half of the
 * answer that reaches the data already handed out.
 *
 * Job-level values live under PMIX_RANK_WILDCARD, and mypeer's module is
 * "hash" on a server, whose tracker is found from the namespace in the
 * proc - so one store per namespace reaches each one's tables. The local
 * clients were given their own copy at init and are told separately.
 *
 * A namespace assigned gds/shmem3 keeps the copy in its shared segment:
 * a published segment is never written again, so retracting from one
 * means a new generation carrying a tombstone. See openpmix#4087. */
static pmix_status_t retract_from_namespaces(const char *key)
{
    pmix_namespace_t *nptr;
    pmix_proc_t proc;
    pmix_kval_t *kv;
    pmix_status_t rc, ret = PMIX_SUCCESS;

    if (NULL == key) {
        return PMIX_SUCCESS;
    }
    PMIX_LIST_FOREACH (nptr, &pmix_globals.nspaces, pmix_namespace_t) {
        PMIX_LOAD_PROCID(&proc, nptr->nspace, PMIX_RANK_WILDCARD);
        kv = PMIX_NEW(pmix_kval_t);
        if (PMIX_UNLIKELY(NULL == kv)) {
            /* this walk used to abandon the sweep here and say nothing.
             * The cache entry is already gone, so every namespace behind
             * this one kept a copy the host had asked to be taken back -
             * and the host was told the deregistration had succeeded.
             * Record it, and go on trying the rest: the next namespace
             * may well be reachable, and the second half of the retraction
             * below needs no allocation at all. */
            if (PMIX_SUCCESS == ret) {
                ret = PMIX_ERR_NOMEM;
            }
        } else {
            kv->key = strdup(key);
            if (PMIX_UNLIKELY(NULL == kv->key)) {
                if (PMIX_SUCCESS == ret) {
                    ret = PMIX_ERR_NOMEM;
                }
            } else {
                PMIX_GDS_STORE_KV(rc, pmix_globals.mypeer, &proc, PMIX_DEL_INTERNAL, kv);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    if (PMIX_SUCCESS == ret) {
                        ret = rc;
                    }
                }
            }
            PMIX_RELEASE(kv);
        }
        /* mypeer's module is "hash", which is where the store above
         * landed. A namespace served by another module keeps its own
         * copy - gds/shmem3 in a shared segment it cannot rewrite - so
         * tell that module too. */
        PMIX_GDS_DEL_KEY(rc, nptr, &proc, key);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            if (PMIX_SUCCESS == ret) {
                ret = rc;
            }
        }
        pmix_server_notify_deleted(&proc, PMIX_DEL_INTERNAL, key, NULL);
    }
    return ret;
}

static void _deregister_resources(int sd, short args, void *cbdata)
{
    bool retracted = false;
    pmix_setup_caddy_t *cd = (pmix_setup_caddy_t *) cbdata;
    pmix_kval_t *kv, *knext;
    pmix_info_t *qual;
    pmix_status_t rc, ret = PMIX_SUCCESS;
    char *hostname;
    uint32_t nodeid = 0;
    bool have_nodeid, empty;
    size_t n, nqual, ntargets;

    PMIX_ACQUIRE_OBJECT(cd);
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    /* Find any matches in our global cache and remove them - the man page
     * says "each matching entry" is deleted. The cache can legitimately
     * hold more than one entry for a key: _register_resources appends
     * without checking, so a host that re-registers a key to update its
     * value leaves both behind, and the gds walk of this list stores them
     * in order, so the later one is the one in effect. Stopping at the
     * first match therefore removed the entry that was being shadowed and
     * left the one the datastore was actually using: the deregistration
     * silently did nothing. Clear every match, using the SAFE variant since
     * the matching item is released inside the walk.
     *
     * A request element carrying a data array is a qualified request: its
     * node identifiers narrow the sweep to the entries describing that
     * node, and anything else it carries narrows it further to elements
     * within those entries - the fabric device the man page describes.
     * An element carrying anything else selects by key alone, which is
     * what the man page's "only the key fields are used" describes and
     * what every in-tree registration produces. */
    for (n = 0; n < cd->ninfo; n++) {
        if (PMIX_DATA_ARRAY == cd->info[n].value.type) {
            rc = parse_node_qualifier(&cd->info[n], &nodeid, &have_nodeid,
                                      &hostname, &ntargets);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                /* keep the first failure: a later element's success must
                 * not erase it, and the host is owed the reason */
                if (PMIX_SUCCESS == ret) {
                    ret = rc;
                }
                continue;
            }
            qual = (pmix_info_t *) cd->info[n].value.data.darray->array;
            nqual = cd->info[n].value.data.darray->size;

            PMIX_LIST_FOREACH_SAFE (kv, knext, &pmix_server_globals.gdata, pmix_kval_t) {
                if (!PMIX_CHECK_KEY(kv, cd->info[n].key)) {
                    continue;
                }
                if ((have_nodeid || NULL != hostname) &&
                    !entry_names_node(kv, nodeid, have_nodeid, hostname)) {
                    continue;
                }
                if (0 < ntargets) {
                    /* an entry that is not an array of info describes no
                     * elements to take out of it */
                    if (NULL == kv->value || PMIX_DATA_ARRAY != kv->value->type ||
                        NULL == kv->value->data.darray ||
                        PMIX_INFO != kv->value->data.darray->type ||
                        NULL == kv->value->data.darray->array) {
                        continue;
                    }
                    rc = prune_entry(kv, qual, nqual, &empty);
                    if (PMIX_SUCCESS != rc) {
                        PMIX_ERROR_LOG(rc);
                        if (PMIX_SUCCESS == ret) {
                            ret = rc;
                        }
                        continue;
                    }
                    if (!empty) {
                        /* The entry survives, pruned. The namespaces
                         * already holding it have the UNPRUNED value, so
                         * push the pruned one: a store replaces by key,
                         * and gds/shmem3 publishes a segment that
                         * shadows what it replaces. A deletion would be
                         * wrong here - it would take from a namespace
                         * more than the host asked to remove - which is
                         * why this arm did nothing until there was an
                         * update push to use. */
                        pmix_namespace_t *nsp;
                        pmix_info_t pruned;
                        PMIX_INFO_CONSTRUCT(&pruned);
                        PMIX_LOAD_KEY(pruned.key, kv->key);
                        rc = PMIx_Value_xfer(&pruned.value, kv->value);
                        if (PMIX_SUCCESS != rc) {
                            PMIX_ERROR_LOG(rc);
                            if (PMIX_SUCCESS == ret) {
                                ret = rc;
                            }
                            PMIX_INFO_DESTRUCT(&pruned);
                            continue;
                        }
                        PMIX_LIST_FOREACH (nsp, &pmix_globals.nspaces,
                                           pmix_namespace_t) {
                            pmix_status_t prc;
                            PMIX_GDS_ADD_JOB_DATA(prc, nsp->nspace, &pruned, 1);
                            if (PMIX_SUCCESS != prc) {
                                PMIX_ERROR_LOG(prc);
                                if (PMIX_SUCCESS == ret) {
                                    ret = prc;
                                }
                            }
                        }
                        PMIX_INFO_DESTRUCT(&pruned);
                        continue;
                    }
                }
                pmix_list_remove_item(&pmix_server_globals.gdata, &kv->super);
                PMIX_RELEASE(kv);
                retracted = true;
            }
        } else {
            PMIX_LIST_FOREACH_SAFE (kv, knext, &pmix_server_globals.gdata, pmix_kval_t) {
                if (PMIX_CHECK_KEY(kv, cd->info[n].key)) {
                    pmix_list_remove_item(&pmix_server_globals.gdata, &kv->super);
                    PMIX_RELEASE(kv);
                    retracted = true;
                }
            }
        }
        if (retracted) {
            /* the cache no longer has it; take it back from the
             * namespaces that were seeded from the cache, and from the
             * local clients that were given their own copy */
            rc = retract_from_namespaces(cd->info[n].key);
            if (PMIX_SUCCESS != rc && PMIX_SUCCESS == ret) {
                ret = rc;
            }
            retracted = false;
        }
    }

    cd->opcbfunc(ret, cd->cbdata);
    PMIX_RELEASE(cd);
}

pmix_status_t PMIx_server_deregister_resources(pmix_info_t info[], size_t ninfo,
                                               pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    pmix_setup_caddy_t *cd;
    pmix_lock_t mylock;
    pmix_status_t rc;

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "pmix:server deregister resources");

    if (!pmix_atomic_check_bool(&pmix_globals.initialized)) {
        return PMIX_ERR_INIT;
    }

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        return PMIX_ERR_NOT_AVAILABLE;
    }

    /* the handler walks the array "ninfo" times */
    if (0 < ninfo && NULL == info) {
        return PMIX_ERR_BAD_PARAM;
    }

    cd = PMIX_NEW(pmix_setup_caddy_t);
    if (NULL == cd) {
        return PMIX_ERR_NOMEM;
    }
    cd->info = info;
    cd->ninfo = ninfo;
    cd->opcbfunc = cbfunc;
    cd->cbdata = cbdata;

    /* if the provided callback is NULL, then substitute
     * our own internal cbfunc and block here */
    if (NULL == cbfunc) {
        if (pmix_progress_thread_check_blocking("PMIx_server_deregister_resources")) {
            /* we are ON the progress thread, so waiting for the event we
             * would post is waiting for ourselves - answer rather than
             * hang. The caller wanted the blocking form; the non-blocking
             * one works fine from here */
            PMIX_RELEASE(cd);
            return PMIX_ERR_WOULD_BLOCK;
        }
        PMIX_CONSTRUCT_LOCK(&mylock);
        cd->opcbfunc = pmix_server_lock_opcbfunc;
        cd->cbdata = &mylock;
        PMIX_THREADSHIFT(cd, _deregister_resources);
        PMIX_WAIT_THREAD(&mylock);
        rc = mylock.status;
        if (PMIX_SUCCESS == rc) {
            rc = PMIX_OPERATION_SUCCEEDED;
        }
        PMIX_DESTRUCT_LOCK(&mylock);
        return rc;
    }

    /* we have to push this into our event library to avoid
     * potential threading issues */
    PMIX_THREADSHIFT(cd, _deregister_resources);
    return PMIX_SUCCESS;
}

static void _setup_op(pmix_status_t rc, void *cbdata)
{
    pmix_setup_caddy_t *fcd = (pmix_setup_caddy_t *) cbdata;
    PMIX_HIDE_UNUSED_PARAMS(rc);

    if (NULL != fcd->info) {
        PMIX_INFO_FREE(fcd->info, fcd->ninfo);
    }
    PMIX_RELEASE(fcd);
}

static void _setup_app(int sd, short args, void *cbdata)
{
    pmix_setup_caddy_t *cd = (pmix_setup_caddy_t *) cbdata;
    pmix_setup_caddy_t *fcd = NULL;
    pmix_status_t rc;
    pmix_list_t ilist;
    pmix_kval_t *kv;
    size_t n;

    PMIX_ACQUIRE_OBJECT(cd);
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_CONSTRUCT(&ilist, pmix_list_t);

    /* pass to the network libraries */
    if (PMIX_SUCCESS != (rc = pmix_pnet.allocate(cd->nspace, cd->info, cd->ninfo, &ilist))) {
        goto depart;
    }

    /* pass to the GPU libraries */
    if (PMIX_SUCCESS != (rc = pmix_pgpu.allocate(cd->nspace, cd->info, cd->ninfo, &ilist))) {
        goto depart;
    }

    /* pass to the programming model libraries */
    if (PMIX_SUCCESS != (rc = pmix_pmdl.harvest_envars(cd->nspace, cd->info, cd->ninfo, &ilist))) {
        goto depart;
    }

    /* setup the return callback */
    fcd = PMIX_NEW(pmix_setup_caddy_t);
    if (NULL == fcd) {
        rc = PMIX_ERR_NOMEM;
        PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
        goto depart;
    }

    /* if anything came back, construct an info array */
    if (0 < (fcd->ninfo = pmix_list_get_size(&ilist))) {
        PMIX_INFO_CREATE(fcd->info, fcd->ninfo);
        if (NULL == fcd->info) {
            rc = PMIX_ERR_NOMEM;
            PMIX_RELEASE(fcd);
            goto depart;
        }
        n = 0;
        PMIX_LIST_FOREACH (kv, &ilist, pmix_kval_t) {
            pmix_strncpy(fcd->info[n].key, kv->key, PMIX_MAX_KEYLEN);
            rc = PMIx_Value_xfer(&fcd->info[n].value, kv->value);
            if (PMIX_SUCCESS != rc) {
                /* the array would go up to the host with a hole in it and
                 * nothing to say which element was never filled in */
                PMIX_ERROR_LOG(rc);
                _setup_op(rc, fcd);
                fcd = NULL;
                goto depart;
            }
            ++n;
        }
    }

depart:
    /* always execute the callback to avoid hanging */
    if (NULL != cd->setupcbfunc) {
        if (NULL == fcd) {
            cd->setupcbfunc(rc, NULL, 0, cd->cbdata, NULL, NULL);
        } else {
            cd->setupcbfunc(rc, fcd->info, fcd->ninfo, cd->cbdata, _setup_op, fcd);
        }
    } else if (NULL != fcd) {
        /* nobody to hand the results to, so nobody will call _setup_op to
         * give them back either - the caddy and the info array assembled
         * into it are ours to release */
        _setup_op(rc, fcd);
    }

    /* cleanup memory */
    PMIX_LIST_DESTRUCT(&ilist);
    if (NULL != cd->nspace) {
        free(cd->nspace);
    }
    PMIX_RELEASE(cd);
}

pmix_status_t PMIx_server_setup_application(const pmix_nspace_t nspace, pmix_info_t info[],
                                            size_t ninfo, pmix_setup_application_cbfunc_t cbfunc,
                                            void *cbdata)
{
    pmix_setup_caddy_t *cd;

    if (!pmix_atomic_check_bool(&pmix_globals.initialized)) {
        return PMIX_ERR_INIT;
    }

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        return PMIX_ERR_NOT_AVAILABLE;
    }

    /* the array is handed down to the pnet, pgpu and pmdl components,
     * which index it "ninfo" times */
    if (0 < ninfo && NULL == info) {
        return PMIX_ERR_BAD_PARAM;
    }

    /* need to threadshift this request */
    cd = PMIX_NEW(pmix_setup_caddy_t);
    if (NULL == cd) {
        return PMIX_ERR_NOMEM;
    }
    if (NULL != nspace) {
        cd->nspace = strdup(nspace);
        /* an unchecked copy here left the handler passing NULL down to
         * pnet, which answers a missing namespace with PMIX_ERR_BAD_PARAM
         * - so an out-of-memory condition was reported to the host as a
         * malformed request */
        if (NULL == cd->nspace) {
            PMIX_RELEASE(cd);
            return PMIX_ERR_NOMEM;
        }
    }
    cd->info = info;
    cd->ninfo = ninfo;
    cd->setupcbfunc = cbfunc;
    cd->cbdata = cbdata;
    PMIX_THREADSHIFT(cd, _setup_app);

    return PMIX_SUCCESS;
}

static void _setup_local_support(int sd, short args, void *cbdata)
{
    pmix_setup_caddy_t *cd = (pmix_setup_caddy_t *) cbdata;
    pmix_status_t rc;

    PMIX_ACQUIRE_OBJECT(cd);
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    /* pass to the network libraries */
    rc = pmix_pnet.setup_local_network(cd->nspace, cd->info, cd->ninfo);

    /* pass to the GPU libraries */
    if (PMIX_SUCCESS == rc) {
        rc = pmix_pgpu.setup_local(cd->nspace, cd->info, cd->ninfo);
    }

    /* pass the info back */
    if (NULL != cd->opcbfunc) {
        cd->opcbfunc(rc, cd->cbdata);
    }
    /* cleanup memory */
    if (NULL != cd->nspace) {
        free(cd->nspace);
    }
    PMIX_RELEASE(cd);
}

pmix_status_t PMIx_server_setup_local_support(const pmix_nspace_t nspace, pmix_info_t info[],
                                              size_t ninfo, pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    pmix_setup_caddy_t *cd;
    pmix_status_t rc;
    pmix_lock_t mylock;

    /* see PMIx_server_setup_application: a launcher tool is a legitimate
     * caller of this pair, so neither screens the server library's flag */
    if (!pmix_atomic_check_bool(&pmix_globals.initialized)) {
        return PMIX_ERR_INIT;
    }

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        return PMIX_ERR_NOT_AVAILABLE;
    }

    /* the array is handed down to the pnet and pgpu components, which
     * index it "ninfo" times */
    if (0 < ninfo && NULL == info) {
        return PMIX_ERR_BAD_PARAM;
    }

    /* need to threadshift this request */
    cd = PMIX_NEW(pmix_setup_caddy_t);
    if (NULL == cd) {
        return PMIX_ERR_NOMEM;
    }
    if (NULL != nspace) {
        cd->nspace = strdup(nspace);
        /* as above: without this, an allocation failure reaches the host
         * as PMIX_ERR_BAD_PARAM out of pnet */
        if (NULL == cd->nspace) {
            PMIX_RELEASE(cd);
            return PMIX_ERR_NOMEM;
        }
    }
    cd->info = info;
    cd->ninfo = ninfo;
    cd->opcbfunc = cbfunc;
    cd->cbdata = cbdata;

    /* if the provided callback is NULL, then substitute
     * our own internal cbfunc and block here */
    if (NULL == cbfunc) {
        if (pmix_progress_thread_check_blocking("PMIx_server_setup_local_support")) {
            /* we are ON the progress thread, so waiting for the event we
             * would post is waiting for ourselves - answer rather than
             * hang. The caller wanted the blocking form; the non-blocking
             * one works fine from here. The nspace copy is ours, and the
             * handler that would have freed it is never going to run */
            if (NULL != cd->nspace) {
                free(cd->nspace);
            }
            PMIX_RELEASE(cd);
            return PMIX_ERR_WOULD_BLOCK;
        }
        PMIX_CONSTRUCT_LOCK(&mylock);
        cd->opcbfunc = pmix_server_lock_opcbfunc;
        cd->cbdata = &mylock;
        PMIX_THREADSHIFT(cd, _setup_local_support);
        PMIX_WAIT_THREAD(&mylock);
        rc = mylock.status;
        PMIX_DESTRUCT_LOCK(&mylock);
        if (PMIX_SUCCESS == rc) {
            rc = PMIX_OPERATION_SUCCEEDED;
        }
        return rc;
    }

    PMIX_THREADSHIFT(cd, _setup_local_support);

    return PMIX_SUCCESS;
}

/* The preg framework takes these arguments at face value - the base
 * hands "input" straight to the active components and writes through
 * "regexp" without looking - so the public entry points screen them
 * here, the same way the regex2 pair below always has. */
PMIX_EXPORT pmix_status_t PMIx_generate_regex(const char *input, char **regexp)
{
    if (!pmix_atomic_check_bool(&pmix_globals.initialized)) {
        return PMIX_ERR_INIT;
    }
    if (NULL == input || NULL == regexp) {
        return PMIX_ERR_BAD_PARAM;
    }

    return pmix_preg.generate_node_regex(input, regexp);
}

PMIX_EXPORT pmix_status_t PMIx_generate_regex2(const char *input,
                                               pmix_info_t info[], size_t ninfo,
                                               pmix_regex2_t *regex)
{
    if (!pmix_atomic_check_bool(&pmix_globals.initialized)) {
        return PMIX_ERR_INIT;
    }
    if (NULL == input || NULL == regex) {
        return PMIX_ERR_BAD_PARAM;
    }

    return pmix_preg.generate_regex(input, info, ninfo, regex);
}


PMIX_EXPORT pmix_status_t PMIx_parse_regex2(const pmix_regex2_t *regex,
                                            pmix_info_t info[], size_t ninfo,
                                            char **output)
{
    if (!pmix_atomic_check_bool(&pmix_globals.initialized)) {
        return PMIX_ERR_INIT;
    }
    if (NULL == regex || NULL == output) {
        return PMIX_ERR_BAD_PARAM;
    }

    return pmix_preg.parse_regex(regex, info, ninfo, output);
}

PMIX_EXPORT pmix_status_t PMIx_generate_ppn(const char *input, char **regexp)
{
    if (!pmix_atomic_check_bool(&pmix_globals.initialized)) {
        return PMIX_ERR_INIT;
    }
    if (NULL == input || NULL == regexp) {
        return PMIX_ERR_BAD_PARAM;
    }

    return pmix_preg.generate_ppn(input, regexp);
}

pmix_status_t PMIx_server_generate_locality_string(const pmix_cpuset_t *cpuset, char **locality)
{
    pmix_status_t rc;

    if (!pmix_atomic_check_bool(&pmix_globals.initialized)) {
        return PMIX_ERR_INIT;
    }
    /* hwloc screens the cpuset, but reports a bad one by writing NULL
     * through the output pointer - so that one has to be screened here */
    if (NULL == locality) {
        return PMIX_ERR_BAD_PARAM;
    }

    /* just pass this down */
    rc = pmix_hwloc_generate_locality_string(cpuset, locality);
    return rc;
}

pmix_status_t PMIx_server_generate_cpuset_string(const pmix_cpuset_t *cpuset, char **cpuset_string)
{
    pmix_status_t rc;

    if (!pmix_atomic_check_bool(&pmix_globals.initialized)) {
        return PMIX_ERR_INIT;
    }
    /* same as above: a bad cpuset is reported by writing NULL through the
     * output pointer, so hwloc cannot be the one to screen it */
    if (NULL == cpuset_string) {
        return PMIX_ERR_BAD_PARAM;
    }

    /* just pass this down */
    rc = pmix_hwloc_generate_cpuset_string(cpuset, cpuset_string);
    return rc;
}

pmix_status_t PMIx_server_generate_cpuset(const char *cpuset_string,
                                          pmix_cpuset_t *cpuset)
{
    pmix_status_t rc;

    if (!pmix_atomic_check_bool(&pmix_globals.initialized)) {
        return PMIX_ERR_INIT;
    }

    /* just pass this down */
    rc = pmix_hwloc_parse_cpuset_string(cpuset_string, cpuset);
    return rc;
}
