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
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#include "pmix_config.h"
#include "include/pmix.h"
#include "include/pmix_common.h"
#include "include/pmix_server.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "include/pmix_tool.h"
#include "src/common/pmix_attributes.h"
#include "src/mca/base/base.h"
#include "src/mca/pinstalldirs/base/base.h"
#include "src/runtime/pmix_rte.h"
#include "src/threads/threads.h"
#include "src/util/cmd_line.h"
#include "src/util/keyval_parse.h"
#include "src/util/show_help.h"

typedef struct {
    pmix_lock_t lock;
    pmix_status_t status;
} mylock_t;

static pmix_proc_t myproc;

/* define a structure for collecting returned
 * info from a query */
typedef struct {
    pmix_lock_t lock;
    pmix_status_t status;
    pmix_info_t *info;
    size_t ninfo;
} myquery_data_t;

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
    PMIX_WAKEUP_THREAD(&mq->lock);
}

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

    if (PMIX_SUCCESS != status) {
        fprintf(stderr, "Client %s:%d EVENT HANDLER REGISTRATION FAILED WITH STATUS %d, ref=%lu\n",
                myproc.nspace, myproc.rank, status, (unsigned long) evhandler_ref);
    }
    lock->status = status;
    PMIX_WAKEUP_THREAD(&lock->lock);
}

/*****************************************
 * Global Vars for Command line Arguments
 *****************************************/
typedef struct {
    bool help;
    bool verbose;
    pid_t pid;
    char *nspace;
    char *uri;
    bool sysfirst;
    bool system;
    char *client;
    char *server;
    char *tool;
    char *host;
    bool clientfns;
    bool serverfns;
    bool toolfns;
    bool hostfns;
} pmix_pquery_globals_t;

pmix_pquery_globals_t pmix_pquery_globals = {
    .help = false,
    .verbose = false,
    .pid = 0,
    .nspace = NULL,
    .uri = NULL,
    .sysfirst = false,
    .system = false,
    .client = NULL,
    .server = NULL,
    .tool = NULL,
    .host = NULL,
    .clientfns = false,
    .serverfns = false,
    .toolfns = false,
    .hostfns = false
};

pmix_cmd_line_init_t cmd_line_opts[]
    = {{NULL, 'h', NULL, "help", 0, &pmix_pquery_globals.help, PMIX_CMD_LINE_TYPE_BOOL,
        "This help message", PMIX_CMD_LINE_OTYPE_GENERAL},

       {NULL, 'v', NULL, "verbose", 0, &pmix_pquery_globals.verbose, PMIX_CMD_LINE_TYPE_BOOL,
        "Be Verbose", PMIX_CMD_LINE_OTYPE_GENERAL},

       {NULL, 'p', NULL, "pid", 1, &pmix_pquery_globals.pid, PMIX_CMD_LINE_TYPE_INT,
        "Specify server pid to connect to", PMIX_CMD_LINE_OTYPE_GENERAL},

       {NULL, 'n', NULL, "nspace", 1, &pmix_pquery_globals.nspace, PMIX_CMD_LINE_TYPE_STRING,
        "Specify server nspace to connect to", PMIX_CMD_LINE_OTYPE_GENERAL},

       {NULL, '\0', NULL, "uri", 1, &pmix_pquery_globals.uri, PMIX_CMD_LINE_TYPE_STRING,
        "Specify URI of server to connect to", PMIX_CMD_LINE_OTYPE_GENERAL},

       {NULL, '\0', NULL, "system-server-first", 0, &pmix_pquery_globals.sysfirst,
        PMIX_CMD_LINE_TYPE_BOOL, "Look for the system server first", PMIX_CMD_LINE_OTYPE_GENERAL},

       {NULL, '\0', NULL, "system-server", 0, &pmix_pquery_globals.system, PMIX_CMD_LINE_TYPE_BOOL,
        "Specifically connect to the system server", PMIX_CMD_LINE_OTYPE_GENERAL},

       /* End of list */
       {NULL, '\0', NULL, NULL, 0, NULL, PMIX_CMD_LINE_TYPE_NULL, NULL, PMIX_CMD_LINE_OTYPE_GENERAL}
    };

int main(int argc, char **argv)
{
    pmix_status_t rc;
    pmix_info_t *info;
    mylock_t mylock;
    pmix_cmd_line_t cmd_line;
    size_t n, m, k, nqueries, ninfo, ndarray;
    myquery_data_t mq = {
        .lock = PMIX_LOCK_STATIC_INIT,
        .status = 0,
        .info = NULL,
        .ninfo = 0
    };
    int count;
    char **qkeys = NULL;
    const char *attr;
    bool server = false;
    pmix_list_t querylist, qlist;
    pmix_querylist_t *qry;
    char **qprs;
    char *strt, *endp, *kptr;
    pmix_infolist_t *iptr;
    char *str, *args = NULL;
    pmix_query_t *queries;
    pmix_info_t *infoptr, *ifptr;
    uint64_t u64;

    /* protect against problems if someone passes us thru a pipe
     * and then abnormally terminates the pipe early */
    signal(SIGPIPE, SIG_IGN);

    /* initialize the output system */
    if (!pmix_output_init()) {
        return PMIX_ERROR;
    }

    /* initialize install dirs code */
    if (PMIX_SUCCESS
        != (rc = pmix_mca_base_framework_open(&pmix_pinstalldirs_base_framework,
                                              PMIX_MCA_BASE_OPEN_DEFAULT))) {
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

    /* register params for pmix */
    if (PMIX_SUCCESS != (rc = pmix_register_params())) {
        fprintf(stderr, "pmix_register_params failed with %d\n", rc);
        return PMIX_ERROR;
    }

    /* Parse the command line options */
    pmix_cmd_line_create(&cmd_line, cmd_line_opts);

    rc = pmix_cmd_line_parse(&cmd_line, true, false, argc, argv);

    if (PMIX_SUCCESS != rc) {
        if (PMIX_ERR_SILENT != rc) {
            fprintf(stderr, "%s: command line error (%s)\n", argv[0], PMIx_Error_string(rc));
        }
        return rc;
    }

    if (pmix_pquery_globals.help) {
        args = pmix_cmd_line_get_usage_msg(&cmd_line);
        str = pmix_show_help_string("help-pquery.txt", "usage", true, args);
        if (NULL != str) {
            printf("%s", str);
            free(str);
        }
        free(args);
        /* If we show the help message, that should be all we do */
        exit(0);
    }

    /* get the argv array of keys they want us to query */
    pmix_cmd_line_get_tail(&cmd_line, &count, &qkeys);

    /* if they didn't give us an option, then we can't do anything */
    if (NULL == qkeys) {
        args = pmix_cmd_line_get_usage_msg(&cmd_line);
        str = pmix_show_help_string("help-pquery.txt", "usage", true, args);
        if (NULL != str) {
            printf("%s", str);
            free(str);
        }
        free(args);
        exit(1);
    }

    /* they might be querying the system about a client, server, or tool
     * attribute, so register those - this will allow us to compare the
     * provided key with the actual name of the attribute so we can
     * identify it */
    pmix_init_registered_attrs();
    pmix_register_client_attrs();
    pmix_register_server_attrs();
    pmix_register_tool_attrs();

    /* if we were given the pid of a starter, then direct that
     * we connect to it */
    n = 1;
    PMIX_INFO_CREATE(info, n);
    if (0 < pmix_pquery_globals.pid) {
        PMIX_INFO_LOAD(&info[0], PMIX_SERVER_PIDINFO, &pmix_pquery_globals.pid, PMIX_PID);
    } else if (NULL != pmix_pquery_globals.nspace) {
        PMIX_INFO_LOAD(&info[0], PMIX_SERVER_NSPACE, pmix_pquery_globals.nspace, PMIX_STRING);
    } else if (NULL != pmix_pquery_globals.uri) {
        PMIX_INFO_LOAD(&info[0], PMIX_SERVER_URI, pmix_pquery_globals.uri, PMIX_STRING);
    } else if (pmix_pquery_globals.sysfirst) {
        /* otherwise, use the system connection first, if available */
        PMIX_INFO_LOAD(&info[0], PMIX_CONNECT_SYSTEM_FIRST, NULL, PMIX_BOOL);
    } else if (pmix_pquery_globals.system) {
        PMIX_INFO_LOAD(&info[0], PMIX_CONNECT_TO_SYSTEM, NULL, PMIX_BOOL);
    } else {
        /* we set ourselves up as a server, but no connections required */
        PMIX_INFO_LOAD(&info[0], PMIX_GDS_MODULE, "hash", PMIX_STRING);
        server = true;
        ;
    }

    if (server) {
        /* initialize as a server so we can process the query ourselves */
        rc = PMIx_server_init(NULL, info, n);
        if (PMIX_SUCCESS != rc) {
            fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
            exit(rc);
        }
    } else {
        /* init as a tool */
        rc = PMIx_tool_init(&myproc, info, n);
        if (PMIX_SUCCESS != rc) {
            fprintf(stderr, "PMIx_tool_init failed: %s\n", PMIx_Error_string(rc));
            exit(rc);
        }
        PMIX_INFO_FREE(info, 1);
    }

    /* register a default event handler */
    PMIX_CONSTRUCT_LOCK(&mylock.lock);
    PMIx_Register_event_handler(NULL, 0, NULL, 0, notification_fn, evhandler_reg_callbk,
                                (void *) &mylock);
    PMIX_WAIT_THREAD(&mylock.lock);
    if (PMIX_SUCCESS != mylock.status) {
        fprintf(stderr, "PMIx_Register_event_handler returned bad status: %d\n", mylock.status);
        PMIX_DESTRUCT_LOCK(&mylock.lock);
        rc = mylock.status;
        goto done;
    }
    PMIX_DESTRUCT_LOCK(&mylock.lock);

    /* generate the queries */
    PMIX_CONSTRUCT(&querylist, pmix_list_t);
    for (n = 0; NULL != qkeys[n]; n++) {
        qry = PMIX_NEW(pmix_querylist_t);
        PMIX_CONSTRUCT(&qlist, pmix_list_t);
        /* check for qualifiers: key[qual=foo;qual=bar] */
        if (NULL != (strt = strchr(qkeys[n], '['))) {
            /* we have qualifiers - find the end */
            *strt = '\0';
            ++strt;
            if (NULL == (endp = strrchr(strt, ']'))) {
                str = pmix_show_help_string("help-pquery.txt", "bad-quals", true, qkeys[n]);
                if (NULL != str) {
                    printf("%s", str);
                    free(str);
                }
                exit(1);
            }
            *endp = '\0';
            /* break into qual=val pairs */
            qprs = pmix_argv_split(strt, ';');
            for (m = 0; NULL != qprs[m]; m++) {
                /* break each pair */
                if (NULL == (kptr = strchr(qprs[m], '='))) {
                    str = pmix_show_help_string("help-pquery.txt", "bad-qual", true, qkeys[n],
                                                qprs[m]);
                    if (NULL != str) {
                        printf("%s", str);
                        free(str);
                    }
                    exit(1);
                }
                *kptr = '\0';
                kptr++;
                iptr = PMIX_NEW(pmix_infolist_t);
                if (NULL == (attr = pmix_attributes_lookup(qprs[m]))) {
                    exit(1);
                }
                PMIX_INFO_LOAD(&iptr->info, attr, kptr, PMIX_STRING);
                pmix_list_append(&qlist, &iptr->super);
            }
        }
        /* convert the key */
        if (NULL == (attr = pmix_attributes_lookup(qkeys[n]))) {
            exit(1);
        }
        pmix_argv_append_nosize(&qry->query.keys, attr);
        /* add in any qualifiers */
        m = pmix_list_get_size(&qlist);
        if (0 < m) {
            PMIX_QUERY_QUALIFIERS_CREATE(&qry->query, m);
            m = 0;
            PMIX_LIST_FOREACH (iptr, &qlist, pmix_infolist_t) {
                PMIX_INFO_XFER(&qry->query.qualifiers[m], &iptr->info);
                ++m;
            }
        }
        pmix_list_append(&querylist, &qry->super);
        PMIX_LIST_DESTRUCT(&qlist);
    }
    /* convert the list of queries into an array */
    nqueries = pmix_list_get_size(&querylist);
    PMIX_QUERY_CREATE(queries, nqueries);
    m = 0;
    PMIX_LIST_FOREACH (qry, &querylist, pmix_querylist_t) {
        /* move the queries across */
        queries[m].keys = qry->query.keys;
        queries[m].nqual = qry->query.nqual;
        queries[m].qualifiers = qry->query.qualifiers;
        ++m;
    }
    PMIX_LIST_DESTRUCT(&querylist);

    PMIX_CONSTRUCT_LOCK(&mq.lock);
    rc = PMIx_Query_info_nb(queries, nqueries, querycbfunc, (void *) &mq);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_Query_info failed: %d\n", rc);
        goto done;
    }
    PMIX_WAIT_THREAD(&mq.lock);
    PMIX_DESTRUCT_LOCK(&mq.lock);
    if (PMIX_SUCCESS != mq.status) {
        fprintf(stderr, "PMIx_Query_info returned: %s\n", PMIx_Error_string(mq.status));
        rc = mq.status;
    } else {
        /* print out the returned value(s) */
        for (n = 0; n < mq.ninfo; n++) {
            if (NULL == (attr = pmix_attributes_reverse_lookup(mq.info[n].key))) {
                fprintf(stdout, "%s: ", mq.info[n].key);
            } else {
                fprintf(stdout, "%s: ", attr);
            }
            if (PMIX_STRING == mq.info[n].value.type) {
                fprintf(stdout, "%s\n", mq.info[n].value.data.string);
            } else if (PMIX_DATA_ARRAY == mq.info[n].value.type) {
                fprintf(stdout, "\n");
                infoptr = (pmix_info_t *) mq.info[n].value.data.darray->array;
                ninfo = mq.info[n].value.data.darray->size;
                for (m = 0; m < ninfo; m++) {
                    if (NULL == (attr = pmix_attributes_reverse_lookup(infoptr[m].key))) {
                        fprintf(stdout, "\t%s:", infoptr[m].key);
                    } else {
                        fprintf(stdout, "\t%s:", attr);
                    }
                    if (PMIX_STRING == infoptr[m].value.type) {
                        fprintf(stdout, "  %s\n", infoptr[m].value.data.string);
                    } else if (PMIX_DATA_ARRAY == infoptr[m].value.type) {
                        fprintf(stdout, "\n");
                        ifptr = (pmix_info_t *) infoptr[m].value.data.darray->array;
                        ndarray = infoptr[m].value.data.darray->size;
                        for (k = 0; k < ndarray; k++) {
                            if (NULL == (attr = pmix_attributes_reverse_lookup(ifptr[k].key))) {
                                fprintf(stdout, "\t\t%s:", ifptr[k].key);
                            } else {
                                fprintf(stdout, "\t\t%s:", attr);
                            }
                            if (PMIX_STRING == ifptr[k].value.type) {
                                fprintf(stdout, "  %s\n", ifptr[k].value.data.string);
                            } else if (PMIX_PROC_RANK == ifptr[k].value.type) {
                                fprintf(stdout, "  %u\n", ifptr[k].value.data.rank);
                            } else {
                                /* see if it is a number */
                                PMIX_VALUE_GET_NUMBER(rc, &ifptr[k].value, u64, uint64_t);
                                if (PMIX_SUCCESS == rc) {
                                    fprintf(stdout, "  %lu\n", (unsigned long) u64);
                                } else {
                                    fprintf(stdout, "  Unimplemented value type: %s\n",
                                            PMIx_Data_type_string(ifptr[k].value.type));
                                }
                            }
                        }
                    } else if (PMIX_PROC_RANK == infoptr[m].value.type) {
                        fprintf(stdout, "  %u\n", infoptr[m].value.data.rank);
                    } else {
                        /* see if it is a number */
                        PMIX_VALUE_GET_NUMBER(rc, &infoptr[m].value, u64, uint64_t);
                        if (PMIX_SUCCESS == rc) {
                            fprintf(stdout, "  %lu\n",
                                    (unsigned long) infoptr[m].value.data.uint64);
                        } else {
                            fprintf(stdout, "  Unimplemented value type: %s\n",
                                    PMIx_Data_type_string(infoptr[m].value.type));
                        }
                    }
                }
            }
        }
    }

done:
    if (server) {
        PMIx_server_finalize();
    } else {
        PMIx_tool_finalize();
    }

    return (rc);
}
