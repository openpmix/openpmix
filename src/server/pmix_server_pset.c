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

/* Definition and deletion of process sets by the host environment. */

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

#include "src/runtime/pmix_progress_threads.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_output.h"

#include "pmix_server_ops.h"

typedef struct {
    pmix_info_t *info;
    size_t ninfo;
} mydata_t;

static void release_info(pmix_status_t status, void *cbdata)
{
    mydata_t *cd = (mydata_t *) cbdata;
    PMIX_HIDE_UNUSED_PARAMS(status);

    PMIX_ACQUIRE_OBJECT(cd);

    PMIX_INFO_FREE(cd->info, cd->ninfo);
    free(cd);
}

static void psetdef(int sd, short args, void *cbdata)
{
    pmix_setup_caddy_t *cd = (pmix_setup_caddy_t *) cbdata;
    /* the PMIx server needs to cache the process sets so it can
     * respond to queries for names and memberships */
    mydata_t *mydat;
    pmix_data_array_t *darray;
    pmix_proc_t *ptr;
    pmix_pset_t *ps, *old;
    pmix_status_t rc = PMIX_SUCCESS;

    PMIX_ACQUIRE_OBJECT(cd);
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    /* Record the process set before announcing it. Defining it is what
     * the caller asked for; the event is how local registrants find out.
     * Announcing first meant an allocation failure here left us having
     * advertised a set that does not exist. The order is not otherwise
     * observable: PMIx_Notify_event with a callback thread-shifts, and
     * we are already on the progress thread, so no handler can run until
     * this one returns. */
    ps = PMIX_NEW(pmix_pset_t);
    if (NULL == ps) {
        rc = PMIX_ERR_NOMEM;
        goto done;
    }
    ps->name = strdup(cd->nspace);
    ps->members = (pmix_proc_t *) malloc(cd->nprocs * sizeof(pmix_proc_t));
    if (NULL == ps->name || NULL == ps->members) {
        PMIX_RELEASE(ps);
        rc = PMIX_ERR_NOMEM;
        goto done;
    }
    memcpy(ps->members, cd->procs, cd->nprocs * sizeof(pmix_proc_t));
    ps->nmembers = cd->nprocs;

    /* A name already on the list is a redefinition, not a second set.
     * No API lets a host change a set's membership, so calling this
     * again is the only way to do it - and appending left the old entry
     * in front of the new one, so every member of the old set went on
     * reporting itself a member through PMIX_PSET_NAMES, that key came
     * back naming the set twice for anyone in both, and
     * PMIx_server_delete_process_set removed only the stale entry.
     * Replacing is also what the event raised below says has happened.
     * The old entry goes only once the new one is built, so a failed
     * redefinition leaves the existing set standing rather than
     * destroying it. This is the sole insertion point, so one pass is
     * enough to keep the list free of duplicates. */
    PMIX_LIST_FOREACH (old, &pmix_server_globals.psets, pmix_pset_t) {
        if (NULL != old->name && 0 == strcmp(cd->nspace, old->name)) {
            pmix_list_remove_item(&pmix_server_globals.psets, &old->super);
            PMIX_RELEASE(old);
            break;
        }
    }
    pmix_list_append(&pmix_server_globals.psets, &ps->super);

    /* now tell any local registrants about it */
    mydat = (mydata_t *) malloc(sizeof(mydata_t));
    if (NULL == mydat) {
        goto done;
    }
    mydat->ninfo = 3;
    PMIX_INFO_CREATE(mydat->info, mydat->ninfo);
    /* PMIx_Data_array_create answers NULL for a zero-element array as
     * well as for a failed allocation, so this has to be checked before
     * its "array" member is read - the caller's member count is screened
     * up front for the same reason */
    PMIX_DATA_ARRAY_CREATE(darray, cd->nprocs, PMIX_PROC);
    if (NULL == mydat->info || NULL == darray) {
        if (NULL != mydat->info) {
            PMIX_INFO_FREE(mydat->info, mydat->ninfo);
        }
        if (NULL != darray) {
            PMIX_DATA_ARRAY_FREE(darray);
        }
        free(mydat);
        goto done;
    }
    PMIX_INFO_LOAD(&mydat->info[0], PMIX_EVENT_NON_DEFAULT, NULL, PMIX_BOOL);
    PMIX_INFO_LOAD(&mydat->info[1], PMIX_PSET_NAME, cd->nspace, PMIX_STRING);
    PMIX_LOAD_KEY(mydat->info[2].key, PMIX_PSET_MEMBERS);
    mydat->info[2].value.type = PMIX_DATA_ARRAY;
    mydat->info[2].value.data.darray = darray;
    ptr = (pmix_proc_t *) darray->array;
    memcpy(ptr, cd->procs, cd->nprocs * sizeof(pmix_proc_t));

    /* a notification that is refused will never reach release_info, so
     * hand the payload back ourselves in that case. It does not fail the
     * call: the process set is defined either way, and reporting an
     * undeliverable courtesy event as a failed definition would be a lie
     * the caller cannot act on */
    if (PMIX_SUCCESS != (rc = PMIx_Notify_event(PMIX_PROCESS_SET_DEFINE, &pmix_globals.myid,
                                                PMIX_RANGE_LOCAL, mydat->info, mydat->ninfo,
                                                release_info, (void *) mydat))) {
        PMIX_ERROR_LOG(rc);
        release_info(rc, mydat);
    }
    rc = PMIX_SUCCESS;

done:
    /* report through the waker the public entry point substituted, so
     * the caller hears about a definition that did not happen */
    cd->opcbfunc(rc, cd->cbdata);
}

pmix_status_t PMIx_server_define_process_set(const pmix_proc_t *members, size_t nmembers,
                                             const char *pset_name)
{
    pmix_setup_caddy_t cd;
    pmix_status_t rc;

    if (!pmix_atomic_check_bool(&pmix_globals.initialized)) {
        return PMIX_ERR_INIT;
    }

    /* This is a public entry point, so the host can hand us anything.
     * Every one of these was a segfault on the progress thread: a NULL
     * name reaches strdup and PMIX_INFO_LOAD, a NULL member array
     * reaches memcpy, and a zero member count makes
     * PMIx_Data_array_create answer NULL, which is then dereferenced for
     * its "array" member. A process set with no members is not a thing
     * we can describe in the event either. */
    if (NULL == pset_name || NULL == members || 0 == nmembers) {
        return PMIX_ERR_BAD_PARAM;
    }

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        return PMIX_ERR_NOT_AVAILABLE;
    }

    if (pmix_progress_thread_check_blocking("PMIx_server_define_process_set")) {
        /* the caddy below lives on OUR stack and is safe only because
         * PMIX_WAIT_THREAD holds this frame until the handler wakes it.
         * We cannot wait from the progress thread, and we cannot let the
         * request outlive the frame, so refuse */
        return PMIX_ERR_WOULD_BLOCK;
    }

    /* need to threadshift this request */
    PMIX_CONSTRUCT(&cd, pmix_setup_caddy_t);
    cd.nspace = (char*)pset_name;
    cd.procs = (pmix_proc_t *) members;
    cd.nprocs = nmembers;
    cd.opcbfunc = pmix_server_lock_opcbfunc;
    cd.cbdata = &cd.lock;
    PMIX_THREADSHIFT(&cd, psetdef);
    PMIX_WAIT_THREAD(&cd.lock);
    /* read the status back before the lock goes away - this used to
     * return PMIX_SUCCESS unconditionally, so a definition that failed
     * looked to the host exactly like one that worked */
    rc = cd.lock.status;
    /* protect the input */
    cd.procs = NULL;
    cd.nprocs = 0;
    PMIX_DESTRUCT(&cd);
    return rc;
}

static void psetdel(int sd, short args, void *cbdata)
{
    /* the PMIx server needs to delete the process set from its list */
    pmix_setup_caddy_t *cd = (pmix_setup_caddy_t *) cbdata;
    mydata_t *mydat;
    pmix_pset_t *ps;
    pmix_status_t rc = PMIX_SUCCESS;

    PMIX_ACQUIRE_OBJECT(cd);
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    /* drop the process set first, for the reason given in psetdef */
    PMIX_LIST_FOREACH (ps, &pmix_server_globals.psets, pmix_pset_t) {
        if (0 == strcmp(cd->nspace, ps->name)) {
            pmix_list_remove_item(&pmix_server_globals.psets, &ps->super);
            PMIX_RELEASE(ps);
            break;
        }
    }
    /* a name we do not hold is not an error: the host is telling us the
     * set is gone, and it being gone already is the outcome it wanted */

    /* now tell any local registrants about it */
    mydat = (mydata_t *) malloc(sizeof(mydata_t));
    if (NULL == mydat) {
        /* the set is gone either way, so this is not a deletion that
         * failed - reporting it as one would tell the host to treat a
         * set we no longer hold as still defined. Same rule as the
         * refused-notification arm below, and as psetdef's */
        goto done;
    }
    mydat->ninfo = 2;
    PMIX_INFO_CREATE(mydat->info, mydat->ninfo);
    if (NULL == mydat->info) {
        free(mydat);
        goto done;
    }
    PMIX_INFO_LOAD(&mydat->info[0], PMIX_EVENT_NON_DEFAULT, NULL, PMIX_BOOL);
    PMIX_INFO_LOAD(&mydat->info[1], PMIX_PSET_NAME, cd->nspace, PMIX_STRING);

    /* see the matching note in psetdef */
    if (PMIX_SUCCESS != (rc = PMIx_Notify_event(PMIX_PROCESS_SET_DELETE, &pmix_globals.myid,
                                                PMIX_RANGE_LOCAL, mydat->info, mydat->ninfo,
                                                release_info, (void *) mydat))) {
        PMIX_ERROR_LOG(rc);
        release_info(rc, mydat);
    }
    rc = PMIX_SUCCESS;

done:
    cd->opcbfunc(rc, cd->cbdata);
}

pmix_status_t PMIx_server_delete_process_set(char *pset_name)
{
    pmix_setup_caddy_t cd;
    pmix_status_t rc;

    if (!pmix_atomic_check_bool(&pmix_globals.initialized)) {
        return PMIX_ERR_INIT;
    }

    /* a NULL name reaches strcmp and PMIX_INFO_LOAD on the progress
     * thread - see the matching screen in the define entry point */
    if (NULL == pset_name) {
        return PMIX_ERR_BAD_PARAM;
    }

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        return PMIX_ERR_NOT_AVAILABLE;
    }

    if (pmix_progress_thread_check_blocking("PMIx_server_delete_process_set")) {
        /* the caddy below lives on OUR stack and is safe only because
         * PMIX_WAIT_THREAD holds this frame until the handler wakes it.
         * We cannot wait from the progress thread, and we cannot let the
         * request outlive the frame, so refuse */
        return PMIX_ERR_WOULD_BLOCK;
    }

    /* need to threadshift this request */
    PMIX_CONSTRUCT(&cd, pmix_setup_caddy_t);
    cd.nspace = pset_name;
    cd.opcbfunc = pmix_server_lock_opcbfunc;
    cd.cbdata = &cd.lock;
    PMIX_THREADSHIFT(&cd, psetdel);
    PMIX_WAIT_THREAD(&cd.lock);
    /* read the status back before the lock goes away - see the matching
     * note in the define entry point */
    rc = cd.lock.status;
    PMIX_DESTRUCT(&cd);
    return rc;
}
