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
    pmix_pset_t *ps;
    pmix_status_t rc;

    PMIX_ACQUIRE_OBJECT(cd);
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    mydat = (mydata_t *) malloc(sizeof(mydata_t));
    mydat->ninfo = 3;
    PMIX_INFO_CREATE(mydat->info, mydat->ninfo);
    PMIX_INFO_LOAD(&mydat->info[0], PMIX_EVENT_NON_DEFAULT, NULL, PMIX_BOOL);
    PMIX_INFO_LOAD(&mydat->info[1], PMIX_PSET_NAME, cd->nspace, PMIX_STRING);
    PMIX_DATA_ARRAY_CREATE(darray, cd->nprocs, PMIX_PROC);
    PMIX_LOAD_KEY(mydat->info[2].key, PMIX_PSET_MEMBERS);
    mydat->info[2].value.type = PMIX_DATA_ARRAY;
    mydat->info[2].value.data.darray = darray;
    ptr = (pmix_proc_t *) darray->array;
    memcpy(ptr, cd->procs, cd->nprocs * sizeof(pmix_proc_t));

    /* a notification that is refused will never reach release_info, so
     * hand the payload back ourselves in that case */
    rc = PMIx_Notify_event(PMIX_PROCESS_SET_DEFINE, &pmix_globals.myid, PMIX_RANGE_LOCAL,
                           mydat->info, mydat->ninfo, release_info, (void *) mydat);
    if (PMIX_SUCCESS != rc) {
        release_info(rc, mydat);
    }

    /* now record the process set */
    ps = PMIX_NEW(pmix_pset_t);
    ps->name = strdup(cd->nspace);
    ps->members = (pmix_proc_t *) malloc(cd->nprocs * sizeof(pmix_proc_t));
    memcpy(ps->members, cd->procs, cd->nprocs * sizeof(pmix_proc_t));
    ps->nmembers = cd->nprocs;
    pmix_list_append(&pmix_server_globals.psets, &ps->super);

    PMIX_WAKEUP_THREAD(&cd->lock);
}

pmix_status_t PMIx_server_define_process_set(const pmix_proc_t *members, size_t nmembers,
                                             const char *pset_name)
{
    pmix_setup_caddy_t cd;

    if (!pmix_atomic_check_bool(&pmix_globals.initialized)) {
        return PMIX_ERR_INIT;
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
    /* protect the input */
    cd.procs = NULL;
    cd.nprocs = 0;
    PMIX_DESTRUCT(&cd);
    return PMIX_SUCCESS;
}

static void psetdel(int sd, short args, void *cbdata)
{
    /* the PMIx server needs to delete the process set from its list */
    pmix_setup_caddy_t *cd = (pmix_setup_caddy_t *) cbdata;
    mydata_t *mydat;
    pmix_pset_t *ps;
    pmix_status_t rc;

    PMIX_ACQUIRE_OBJECT(cd);
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    mydat = (mydata_t *) malloc(sizeof(mydata_t));
    mydat->ninfo = 2;
    PMIX_INFO_CREATE(mydat->info, mydat->ninfo);
    PMIX_INFO_LOAD(&mydat->info[0], PMIX_EVENT_NON_DEFAULT, NULL, PMIX_BOOL);
    PMIX_INFO_LOAD(&mydat->info[1], PMIX_PSET_NAME, cd->nspace, PMIX_STRING);

    /* see the matching note in psetdef */
    rc = PMIx_Notify_event(PMIX_PROCESS_SET_DELETE, &pmix_globals.myid, PMIX_RANGE_LOCAL,
                           mydat->info, mydat->ninfo, release_info, (void *) mydat);
    if (PMIX_SUCCESS != rc) {
        release_info(rc, mydat);
    }

    /* now find this process set */
    PMIX_LIST_FOREACH (ps, &pmix_server_globals.psets, pmix_pset_t) {
        if (0 == strcmp(cd->nspace, ps->name)) {
            pmix_list_remove_item(&pmix_server_globals.psets, &ps->super);
            PMIX_RELEASE(ps);
            break;
        }
    }
    PMIX_WAKEUP_THREAD(&cd->lock);
}

pmix_status_t PMIx_server_delete_process_set(char *pset_name)
{
    pmix_setup_caddy_t cd;

    if (!pmix_atomic_check_bool(&pmix_globals.initialized)) {
        return PMIX_ERR_INIT;
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
    PMIX_DESTRUCT(&cd);
    return PMIX_SUCCESS;
}
