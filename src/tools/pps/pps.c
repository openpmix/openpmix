/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
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
 * Copyright (c) 2006-2013 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2007      Sun Microsystems, Inc.  All rights reserved.
 * Copyright (c) 2007-2016 Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2010      Oracle and/or its affiliates.  All rights reserved.
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/**
 * @file
 * PMIX PS command
 *
 * Report the processes running under the PMIx server(s) we can reach.
 * With no options it connects to the local/system server, queries the
 * set of active namespaces, and prints a process table for each. With
 * "--nodes" it instead reports, per namespace, the set of nodes the
 * job occupies and how many of its processes live on each.
 */

#include "pmix_config.h"
#include "pmix_common.h"

#include <errno.h>
#include <stdio.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif /* HAVE_UNISTD_H */
#include <stdlib.h>
#include <string.h>

#include "src/mca/base/pmix_base.h"
#include "src/mca/pinstalldirs/base/base.h"
#include "src/runtime/pmix_rte.h"
#include "src/threads/pmix_threads.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_cmd_line.h"
#include "src/util/pmix_keyval_parse.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_printf.h"
#include "src/util/pmix_show_help.h"

#include "include/pmix.h"
#include "include/pmix_tool.h"

typedef struct {
    pmix_lock_t lock;
    pmix_status_t status;
} mylock_t;

/* define a structure for collecting returned
 * info from a query */
typedef struct {
    mylock_t lock;
    pmix_status_t status;
    pmix_info_t *info;
    size_t ninfo;
} myquery_data_t;

static pmix_proc_t myproc;

#define PMIX_CLI_NODES  "nodes"

static struct option ppsoptions[] = {
    PMIX_OPTION_SHORT_DEFINE(PMIX_CLI_HELP, PMIX_ARG_OPTIONAL, 'h'),
    PMIX_OPTION_SHORT_DEFINE(PMIX_CLI_VERSION, PMIX_ARG_NONE, 'V'),
    PMIX_OPTION_SHORT_DEFINE(PMIX_CLI_VERBOSE, PMIX_ARG_NONE, 'v'),
    PMIX_OPTION_DEFINE(PMIX_CLI_PMIXMCA, PMIX_ARG_REQD),

    PMIX_OPTION_DEFINE(PMIX_CLI_SYS_SERVER_FIRST, PMIX_ARG_NONE),
    PMIX_OPTION_DEFINE(PMIX_CLI_SYSTEM_SERVER, PMIX_ARG_NONE),
    PMIX_OPTION_DEFINE(PMIX_CLI_WAIT_TO_CONNECT, PMIX_ARG_REQD),
    PMIX_OPTION_DEFINE(PMIX_CLI_NUM_CONNECT_RETRIES, PMIX_ARG_REQD),
    PMIX_OPTION_DEFINE(PMIX_CLI_PID, PMIX_ARG_REQD),
    PMIX_OPTION_DEFINE(PMIX_CLI_NAMESPACE, PMIX_ARG_REQD),
    PMIX_OPTION_DEFINE(PMIX_CLI_URI, PMIX_ARG_REQD),
    PMIX_OPTION_DEFINE(PMIX_CLI_NODES, PMIX_ARG_NONE),
    PMIX_OPTION_DEFINE(PMIX_CLI_TMPDIR, PMIX_ARG_REQD),

    PMIX_OPTION_END
};
static char *ppsshorts = "h::vV";

/* this is a callback function for the PMIx_Query
 * API. The query will callback with a status indicating
 * if the request could be fully satisfied, partially
 * satisfied, or completely failed. The info parameter
 * contains an array of the returned data, with the
 * info->key field being the key that was provided in
 * the query call. Thus, you can correlate the returned
 * data in the info->value field to the requested key.
 *
 * Once we have dealt with the returned data, we must
 * call the release_fn so that the PMIx library can
 * cleanup */
static void querycbfunc(pmix_status_t status, pmix_info_t *info, size_t ninfo, void *cbdata,
                        pmix_release_cbfunc_t release_fn, void *release_cbdata)
{
    myquery_data_t *mq = (myquery_data_t *) cbdata;
    size_t n;

    mq->status = status;
    /* save the returned info - the PMIx library "owns" it
     * and will release it and perform other cleanup actions
     * when release_fn is called */
    if (0 < ninfo) {
        PMIX_INFO_CREATE(mq->info, ninfo);
        mq->ninfo = ninfo;
        for (n = 0; n < ninfo; n++) {
            PMIX_INFO_XFER(&mq->info[n], &info[n]);
        }
    }

    /* let the library release the data and cleanup from
     * the operation */
    if (NULL != release_fn) {
        release_fn(release_cbdata);
    }

    /* release the block */
    PMIX_WAKEUP_THREAD(&mq->lock.lock);
}

/* this is the event notification function we pass down below
 * when registering for general events - i.e.,, the default
 * handler. */
static void notification_fn(size_t evhdlr_registration_id, pmix_status_t status,
                            const pmix_proc_t *source, pmix_info_t info[], size_t ninfo,
                            pmix_info_t results[], size_t nresults,
                            pmix_event_notification_cbfunc_fn_t cbfunc, void *cbdata)
{
    PMIX_HIDE_UNUSED_PARAMS(evhdlr_registration_id, status, source, info, ninfo,
                            results, nresults);

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

    if (PMIX_SUCCESS != status) {
        fprintf(stderr, "Client %s:%d EVENT HANDLER REGISTRATION FAILED WITH STATUS %d, ref=%lu\n",
                myproc.nspace, myproc.rank, status, (unsigned long) evhandler_ref);
    }
    lock->status = status;
    PMIX_WAKEUP_THREAD(&lock->lock);
}

/* run a single PMIX_QUERY_PROC_TABLE query for the given namespace,
 * returning the array of pmix_proc_info_t (owned by the caller, who
 * must PMIX_INFO_FREE mq->info) via the mq structure */
static pmix_status_t query_proctable(const char *nspace, myquery_data_t *mq)
{
    pmix_query_t query;
    pmix_status_t rc;

    PMIX_QUERY_CONSTRUCT(&query);
    PMIx_Argv_append_nosize(&query.keys, PMIX_QUERY_PROC_TABLE);
    PMIX_QUERY_QUALIFIERS_CREATE(&query, 1);
    PMIX_INFO_LOAD(&query.qualifiers[0], PMIX_NSPACE, nspace, PMIX_STRING);

    PMIX_CONSTRUCT_LOCK(&mq->lock.lock);
    mq->status = PMIX_SUCCESS;
    mq->info = NULL;
    mq->ninfo = 0;
    rc = PMIx_Query_info_nb(&query, 1, querycbfunc, (void *) mq);
    if (PMIX_SUCCESS == rc) {
        PMIX_WAIT_THREAD(&mq->lock.lock);
    }
    PMIX_DESTRUCT_LOCK(&mq->lock.lock);
    PMIX_QUERY_DESTRUCT(&query);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    return mq->status;
}

/* extract the pmix_proc_info_t array (if any) from a returned
 * proc-table query result */
static pmix_proc_info_t *get_proc_array(myquery_data_t *mq, size_t *np)
{
    *np = 0;
    if (0 == mq->ninfo || NULL == mq->info) {
        return NULL;
    }
    if (PMIX_DATA_ARRAY != mq->info[0].value.type ||
        NULL == mq->info[0].value.data.darray ||
        NULL == mq->info[0].value.data.darray->array) {
        return NULL;
    }
    *np = mq->info[0].value.data.darray->size;
    return (pmix_proc_info_t *) mq->info[0].value.data.darray->array;
}

static void print_proctable(const char *nspace, pmix_proc_info_t *pi, size_t np)
{
    size_t n;

    printf("\nNamespace %s (%lu process%s)\n", nspace,
           (unsigned long) np, (1 == np) ? "" : "es");
    printf("    %8s  %8s  %-20s  %-14s  %s\n",
           "Rank", "PID", "Node", "State", "Executable");
    for (n = 0; n < np; n++) {
        printf("    %8u  %8lu  %-20s  %-14s  %s\n",
               (unsigned) pi[n].proc.rank,
               (unsigned long) pi[n].pid,
               (NULL == pi[n].hostname) ? "unknown" : pi[n].hostname,
               PMIx_Proc_state_string(pi[n].state),
               (NULL == pi[n].executable_name) ? "unknown" : pi[n].executable_name);
    }
}

static void print_nodes(const char *nspace, pmix_proc_info_t *pi, size_t np)
{
    size_t n, m;
    char **nodes = NULL;
    int *counts = NULL;
    int nnodes = 0;
    const char *host;

    /* aggregate the process count per unique node */
    for (n = 0; n < np; n++) {
        host = (NULL == pi[n].hostname) ? "unknown" : pi[n].hostname;
        for (m = 0; (int) m < nnodes; m++) {
            if (0 == strcmp(nodes[m], host)) {
                counts[m]++;
                break;
            }
        }
        if ((int) m == nnodes) {
            PMIx_Argv_append_nosize(&nodes, host);
            counts = (int *) realloc(counts, (nnodes + 1) * sizeof(int));
            counts[nnodes] = 1;
            nnodes++;
        }
    }

    printf("\nNamespace %s (%d node%s)\n", nspace, nnodes, (1 == nnodes) ? "" : "s");
    printf("    %-20s  %s\n", "Node", "Processes");
    for (m = 0; (int) m < nnodes; m++) {
        printf("    %-20s  %d\n", nodes[m], counts[m]);
    }
    PMIx_Argv_free(nodes);
    if (NULL != counts) {
        free(counts);
    }
}

/* build the connection directive(s) requested on the command line;
 * returns a freshly-created info array (caller frees) and its size */
static pmix_status_t set_connection(pmix_cli_result_t *results,
                                    pmix_info_t **infoptr, size_t *ninfo)
{
    pmix_cli_item_t *opt;
    pmix_info_t *info;

    PMIX_INFO_CREATE(info, 1);
    *infoptr = info;
    *ninfo = 1;

    if (NULL != (opt = pmix_cmd_line_get_param(results, PMIX_CLI_PID))) {
        /* the value may be an integer pid or a "file:PATH" pointing at a
         * rendezvous file holding the pid */
        char *leftover, *param;
        pid_t pid;
        leftover = NULL;
        pid = strtol(opt->values[0], &leftover, 10);
        if (NULL == leftover || 0 == strlen(leftover)) {
            PMIX_INFO_LOAD(&info[0], PMIX_SERVER_PIDINFO, &pid, PMIX_PID);
        } else if (0 == strncasecmp(opt->values[0], "file", 4)) {
            FILE *fp;
            param = strchr(opt->values[0], ':');
            if (NULL == param) {
                pmix_show_help("help-pquery.txt", "bad-option-input", true, "pps",
                               "--pid", opt->values[0], "file:path");
                PMIX_INFO_FREE(info, 1);
                return PMIX_ERR_BAD_PARAM;
            }
            ++param;
            fp = fopen(param, "r");
            if (NULL == fp) {
                pmix_show_help("help-pquery.txt", "file-open-error", true, "pps",
                               "--pid", opt->values[0], param);
                PMIX_INFO_FREE(info, 1);
                return PMIX_ERR_BAD_PARAM;
            }
            if (1 != fscanf(fp, "%lu", (unsigned long *) &pid)) {
                pmix_show_help("help-pquery.txt", "bad-file", true, "pps",
                               "--pid", opt->values[0], param);
                fclose(fp);
                PMIX_INFO_FREE(info, 1);
                return PMIX_ERR_BAD_PARAM;
            }
            fclose(fp);
            PMIX_INFO_LOAD(&info[0], PMIX_SERVER_PIDINFO, &pid, PMIX_PID);
        } else {
            pmix_show_help("help-pquery.txt", "bad-option-input", true,
                           "pps", "--pid", opt->values[0], "file:path");
            PMIX_INFO_FREE(info, 1);
            return PMIX_ERR_BAD_PARAM;
        }
    } else if (NULL != (opt = pmix_cmd_line_get_param(results, PMIX_CLI_NAMESPACE))) {
        PMIX_INFO_LOAD(&info[0], PMIX_SERVER_NSPACE, opt->values[0], PMIX_STRING);
    } else if (NULL != (opt = pmix_cmd_line_get_param(results, PMIX_CLI_URI))) {
        PMIX_INFO_LOAD(&info[0], PMIX_SERVER_URI, opt->values[0], PMIX_STRING);
    } else if (pmix_cmd_line_is_taken(results, PMIX_CLI_SYSTEM_SERVER)) {
        PMIX_INFO_LOAD(&info[0], PMIX_CONNECT_TO_SYSTEM, NULL, PMIX_BOOL);
    } else {
        /* default: use the system connection first, if available */
        PMIX_INFO_LOAD(&info[0], PMIX_CONNECT_SYSTEM_FIRST, NULL, PMIX_BOOL);
    }
    return PMIX_SUCCESS;
}

int main(int argc, char *argv[])
{
    pmix_status_t rc = PMIX_SUCCESS;
    pmix_info_t *info;
    size_t ninfo, n;
    pmix_query_t *query;
    myquery_data_t myquery_data;
    mylock_t mylock;
    pmix_cli_result_t results;
    pmix_cli_item_t *opt;
    bool nodes;
    char **nspaces = NULL;
    pmix_proc_info_t *pi;
    size_t np;
    PMIX_HIDE_UNUSED_PARAMS(argc);

    /* protect against problems if someone passes us thru a pipe
     * and then abnormally terminates the pipe early */
    signal(SIGPIPE, SIG_IGN);

    /* init globals */
    pmix_tool_basename = "pps";

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
    pmix_show_help_init();

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

    /* Parse the command line options */
    PMIX_CONSTRUCT(&results, pmix_cli_result_t);
    rc = pmix_cmd_line_parse(argv, ppsshorts, ppsoptions,
                             NULL, &results, "help-pps.txt");

    if (PMIX_SUCCESS != rc) {
        if (PMIX_ERR_SILENT != rc && PMIX_OPERATION_SUCCEEDED != rc) {
            fprintf(stderr, "%s: command line error (%s)\n", argv[0], PMIx_Error_string(rc));
        }
        if (PMIX_OPERATION_SUCCEEDED == rc) {
            rc = PMIX_SUCCESS;
        }
        exit(rc);
    }

    // handle relevant MCA params
    PMIX_LIST_FOREACH(opt, &results.instances, pmix_cli_item_t) {
        if (0 == strcmp(opt->key, PMIX_CLI_PMIXMCA)) {
            for (n = 0; NULL != opt->values[n]; n++) {
                pmix_expose_param(opt->values[n]);
            }
        }
    }

    /* register params for pmix */
    if (PMIX_SUCCESS != (rc = pmix_register_params())) {
        fprintf(stderr, "pmix_register_params failed with %d\n", rc);
        return PMIX_ERROR;
    }

    nodes = pmix_cmd_line_is_taken(&results, PMIX_CLI_NODES);

    /* build the connection directive from the command line */
    rc = set_connection(&results, &info, &ninfo);
    if (PMIX_SUCCESS != rc) {
        exit(rc);
    }

    /* init as a tool */
    if (PMIX_SUCCESS != (rc = PMIx_tool_init(&myproc, info, ninfo))) {
        fprintf(stderr, "PMIx_tool_init failed: %s\n", PMIx_Error_string(rc));
        PMIX_INFO_FREE(info, ninfo);
        exit(rc);
    }
    PMIX_INFO_FREE(info, ninfo);

    /* register a default event handler */
    PMIX_CONSTRUCT_LOCK(&mylock.lock);
    PMIx_Register_event_handler(NULL, 0, NULL, 0, notification_fn, evhandler_reg_callbk,
                                (void *) &mylock);
    PMIX_WAIT_THREAD(&mylock.lock);
    PMIX_DESTRUCT_LOCK(&mylock.lock);

    /* query the active namespaces */
    PMIX_QUERY_CREATE(query, 1);
    PMIX_ARGV_APPEND(rc, query[0].keys, PMIX_QUERY_NAMESPACES);
    PMIX_CONSTRUCT_LOCK(&myquery_data.lock.lock);
    myquery_data.status = PMIX_SUCCESS;
    myquery_data.info = NULL;
    myquery_data.ninfo = 0;
    rc = PMIx_Query_info_nb(query, 1, querycbfunc, (void *) &myquery_data);
    if (PMIX_SUCCESS == rc) {
        PMIX_WAIT_THREAD(&myquery_data.lock.lock);
    }
    PMIX_DESTRUCT_LOCK(&myquery_data.lock.lock);
    PMIX_QUERY_FREE(query, 1);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_Query_info failed: %s\n", PMIx_Error_string(rc));
        goto done;
    }
    if (PMIX_SUCCESS != myquery_data.status) {
        fprintf(stderr, "PMIx_Query_info returned: %s\n", PMIx_Error_string(myquery_data.status));
        rc = myquery_data.status;
        goto done;
    }

    /* we should have received back one info struct containing
     * a comma-delimited list of nspaces */
    if (1 != myquery_data.ninfo || NULL == myquery_data.info ||
        PMIX_STRING != myquery_data.info[0].value.type ||
        NULL == myquery_data.info[0].value.data.string) {
        fprintf(stderr, "No active namespaces were found\n");
        PMIX_INFO_FREE(myquery_data.info, myquery_data.ninfo);
        goto done;
    }

    nspaces = PMIx_Argv_split(myquery_data.info[0].value.data.string, ',');
    PMIX_INFO_FREE(myquery_data.info, myquery_data.ninfo);

    /* for each active namespace, query and print its process table */
    for (n = 0; NULL != nspaces[n]; n++) {
        myquery_data_t mq;
        rc = query_proctable(nspaces[n], &mq);
        if (PMIX_SUCCESS != rc) {
            fprintf(stderr, "Namespace %s: query failed: %s\n",
                    nspaces[n], PMIx_Error_string(rc));
            /* the callback may still have handed us partial results */
            PMIX_INFO_FREE(mq.info, mq.ninfo);
            continue;
        }
        pi = get_proc_array(&mq, &np);
        if (NULL == pi) {
            printf("\nNamespace %s (no process information available)\n", nspaces[n]);
        } else if (nodes) {
            print_nodes(nspaces[n], pi, np);
        } else {
            print_proctable(nspaces[n], pi, np);
        }
        PMIX_INFO_FREE(mq.info, mq.ninfo);
    }
    PMIx_Argv_free(nspaces);

    /***************
     * Cleanup
     ***************/
done:
    PMIX_DESTRUCT(&results);
    PMIx_tool_finalize();

    return rc;
}
