/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2006-2013 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2009-2012 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2011      Oak Ridge National Labs.  All rights reserved.
 * Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015      Mellanox Technologies, Inc.  All rights reserved.
 * Copyright (c) 2021-2023 Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#include "pmix_config.h"
#include "pmix_common.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "include/pmix_tool.h"
#include "src/mca/base/pmix_base.h"
#include "src/mca/pinstalldirs/base/base.h"
#include "src/runtime/pmix_rte.h"
#include "src/threads/pmix_threads.h"
#include "src/util/pmix_cmd_line.h"
#include "src/util/pmix_keyval_parse.h"
#include "src/util/pmix_show_help.h"

typedef struct {
    pmix_lock_t lock;
    pmix_status_t status;
} mylock_t;

static pmix_proc_t myproc;

/* this is the event notification function we pass down below
 * when registering for general events - i.e.,, the default
 * handler. We don't technically need to register one, but it
 * is usually good practice to catch any events that occur */
static void notification_fn(size_t evhdlr_registration_id, pmix_status_t status,
                            const pmix_proc_t *source, pmix_info_t info[], size_t ninfo,
                            pmix_info_t results[], size_t nresults,
                            pmix_event_notification_cbfunc_fn_t cbfunc, void *cbdata)
{
    PMIX_HIDE_UNUSED_PARAMS(evhdlr_registration_id, status,
                            source, info, ninfo, results, nresults);

    /* this example doesn't do anything with default events */
    if (NULL != cbfunc) {
        cbfunc(PMIX_EVENT_ACTION_COMPLETE, NULL, 0, NULL, NULL, cbdata);
    }
}

/* event handler registration is done asynchronously because it
 * may involve the PMIx server registering with the host RM for
 * external events. So we provide a callback function that returns
 * the status of the request (success or an error), plus a numerical index
 * to the registered event. The index is used later on to deregister
 * an event handler - if we don't explicitly deregister it, then the
 * PMIx server will do so when it see us exit */
static void evhandler_reg_callbk(pmix_status_t status, size_t evhandler_ref, void *cbdata)
{
    mylock_t *lock = (mylock_t *) cbdata;
    PMIX_HIDE_UNUSED_PARAMS(evhandler_ref);

    if (PMIX_SUCCESS != status) {
        fprintf(stderr, "Client %s:%d EVENT HANDLER REGISTRATION FAILED WITH STATUS %d, ref=%lu\n",
                myproc.nspace, myproc.rank, status, (unsigned long) evhandler_ref);
    }
    lock->status = status;
    PMIX_WAKEUP_THREAD(&lock->lock);
}

static struct option peventoptions[] = {
    PMIX_OPTION_SHORT_DEFINE(PMIX_CLI_HELP, PMIX_ARG_OPTIONAL, 'h'),
    PMIX_OPTION_SHORT_DEFINE(PMIX_CLI_VERSION, PMIX_ARG_NONE, 'V'),
    PMIX_OPTION_SHORT_DEFINE(PMIX_CLI_VERBOSE, PMIX_ARG_NONE, 'v'),

    PMIX_OPTION_DEFINE(PMIX_CLI_PID, PMIX_ARG_REQD),
    PMIX_OPTION_DEFINE(PMIX_CLI_TMPDIR, PMIX_ARG_REQD),
    PMIX_OPTION_DEFINE("event", PMIX_ARG_REQD),
    PMIX_OPTION_DEFINE("range", PMIX_ARG_REQD),

    PMIX_OPTION_END
};
static char *peventshorts = "h::vV";

static void opcbfunc(pmix_status_t status, void *cbdata)
{
    mylock_t *lock = (mylock_t *) cbdata;

    lock->status = status;
    PMIX_WAKEUP_THREAD(&lock->lock);
}

int main(int argc, char **argv)
{
    pmix_status_t rc;
    pmix_info_t *info;
    mylock_t mylock;
    pmix_status_t status;
    pmix_data_range_t range;
    pmix_cli_result_t results;
    pmix_cli_item_t *opt;
    PMIX_HIDE_UNUSED_PARAMS(argc);

    /* protect against problems if someone passes us thru a pipe
     * and then abnormally terminates the pipe early */
    signal(SIGPIPE, SIG_IGN);

    /* init globals */
    pmix_tool_basename = "pevent";

    /* initialize the output system */
    if (!pmix_output_init()) {
        return PMIX_ERROR;
    }

    /* initialize install dirs code */
    rc = pmix_mca_base_framework_open(&pmix_pinstalldirs_base_framework,
                                      PMIX_MCA_BASE_OPEN_DEFAULT);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr,
                "pmix_pinstalldirs_base_open() failed -- process will likely abort (%s:%d, "
                "returned %d instead of PMIX_SUCCESS)\n",
                __FILE__, __LINE__, rc);
        return rc;
    }
    if (PMIX_SUCCESS != (rc = pmix_pinstall_dirs_base_init(NULL, 0))) {
        fprintf(stderr,
                "pmix_pinstalldirs_base_init() failed -- process will likely abort (%s:%d, "
                "returned %d instead of PMIX_SUCCESS)\n",
                __FILE__, __LINE__, rc);
        return rc;
    }

    /* initialize the help system */
    pmix_show_help_init(NULL);

    /* keyval lex-based parser */
    if (PMIX_SUCCESS != (rc = pmix_util_keyval_parse_init())) {
        fprintf(stderr, "pmix_util_keyval_parse_init failed with %d\n", rc);
        return PMIX_ERROR;
    }

    /* Setup the parameter system */
    if (PMIX_SUCCESS != (rc = pmix_mca_base_var_init())) {
        fprintf(stderr, "pmix_mca_base_var_init failed with %d\n", rc);
        return PMIX_ERROR;
    }

    /* register params for pmix */
    if (PMIX_SUCCESS != (rc = pmix_register_params())) {
        fprintf(stderr, "pmix_register_params failed with %d\n", rc);
        return PMIX_ERROR;
    }

    /* Parse the command line options */
    PMIX_CONSTRUCT(&results, pmix_cli_result_t);
    rc = pmix_cmd_line_parse(argv, peventshorts, peventoptions,
                             NULL, &results, "help-pevent.txt");

    if (PMIX_SUCCESS != rc) {
        if (PMIX_ERR_SILENT != rc && PMIX_OPERATION_SUCCEEDED != rc) {
            fprintf(stderr, "%s: command line error (%s)\n", argv[0], PMIx_Error_string(rc));
        }
        if (PMIX_OPERATION_SUCCEEDED == rc) {
            rc = PMIX_SUCCESS;
        }
        exit(rc);
    }

    opt = pmix_cmd_line_get_param(&results, "event");
    if (NULL == opt) {
        char *str;
        str = pmix_show_help_string("help-pevent.txt", "usage", false,
                                    pmix_tool_basename, "PMIx",
                                    PMIX_PROXY_VERSION,
                                    pmix_tool_basename,
                                    PMIX_PROXY_BUGREPORT);
        if (NULL != str) {
            printf("%s", str);
            free(str);
        }
        exit(1);
    }
    /* if the event is a name, look it up */

    status = (pmix_status_t) strtoul(opt->values[0], NULL, 10);
    opt = pmix_cmd_line_get_param(&results, "range");
    if (NULL == opt) {
        char *str;
        str = pmix_show_help_string("help-pevent.txt", "usage", false,
                                    pmix_tool_basename, "PMIx",
                                    PMIX_PROXY_VERSION,
                                    pmix_tool_basename,
                                    PMIX_PROXY_BUGREPORT);
        if (NULL != str) {
            printf("%s", str);
            free(str);
        }
        exit(1);
    }
    range = (pmix_data_range_t) strtoul(opt->values[0], NULL, 10);

    /* if we were given the pid of a starter, then direct that
     * we connect to it */

    /* otherwise, use the system connection first, if available */
    PMIX_INFO_CREATE(info, 1);
    PMIX_INFO_LOAD(&info[0], PMIX_CONNECT_SYSTEM_FIRST, NULL, PMIX_BOOL);
    /* init as a tool */
    if (PMIX_SUCCESS != (rc = PMIx_tool_init(&myproc, info, 1))) {
        fprintf(stderr, "PMIx_tool_init failed: %d\n", rc);
        exit(rc);
    }
    PMIX_INFO_FREE(info, 1);

    /* register a default event handler */
    PMIX_CONSTRUCT_LOCK(&mylock.lock);
    PMIx_Register_event_handler(NULL, 0, NULL, 0, notification_fn, evhandler_reg_callbk,
                                (void *) &mylock);
    PMIX_WAIT_THREAD(&mylock.lock);
    if (PMIX_SUCCESS != mylock.status) {
        fprintf(stderr, "PMIx_Register_event_handler returned bad status: %d\n", rc);
        PMIX_DESTRUCT_LOCK(&mylock.lock);
        goto done;
    }
    PMIX_DESTRUCT_LOCK(&mylock.lock);

    /* generate the specified event */
    PMIX_CONSTRUCT_LOCK(&mylock.lock);
    rc = PMIx_Notify_event(status, &myproc, range, NULL, 0, opcbfunc, (void *) &mylock);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_Notify_event failed: %d\n", rc);
        goto done;
    }
    PMIX_WAIT_THREAD(&mylock.lock);
    if (PMIX_SUCCESS != mylock.status) {
        fprintf(stderr, "PMIx_Notify_event returned bad status: %d\n", rc);
    }
    PMIX_DESTRUCT_LOCK(&mylock.lock);

done:
    PMIx_tool_finalize();

    return (rc);
}
