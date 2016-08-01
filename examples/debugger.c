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
 * Copyright (c) 2013-2016 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015      Mellanox Technologies, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>

#include <pmix_tool.h>

static int attach_to_running_job(char *nspace);

int main(int argc, char **argv)
{
    pmix_status_t rc;
    pmix_proc_t myproc;
    pmix_info_t *info;
    pmix_app_t *app;
    size_t ninfo, napps;
    
    /* check for user directives - this would include:
     * - a flag indicating we want to attach to a specified application
     * - application info if we are launching a new app
     */
    
    /* init us - if a PMIx server pid was provided, then pass it along */
    if (0 < server_pid) {
        ninfo = 1;
        PMIX_INFO_CREATE(info, ninfo);
        PMIX_INFO_LOAD(&info[0], PMIX_SERVER_PIDINFO, server_pid, PMIX_UINT32);
    } else {
        info = NULL;
        ninfo = 0;
    }
    if (PMIX_SUCCESS != (rc = PMIx_tool_init(&myproc, info, ninfo))) {
        fprintf(stderr, "PMIx_tool_init failed: %d\n", rc);
        exit(rc);
    }
    if (0 < ninfo) {
        PMIX_INFO_FREE(info, ninfo);
    }
    fprintf(stderr, "Tool ns %s rank %d: Running\n", myproc.nspace, myproc.rank);

    /* if we are attaching to a running job, then attach to it */
    if (attach) {
        ret = attach_to_running_job(argv[1]);
    } else {
        /* this is an initial launch - we need to launch the application
         * plus the debugger daemons, letting the RM know we are debugging
         * so that it will "pause" the app procs until we are ready */
        napps = 2;
        PMIX_APP_CREATE(app, napps);
        /* setup the executable */
        app[0].cmd = strdup("app");
        app[0].argc = 1;
        app[0].argv = (char**)malloc(2*sizeof(char*));
        app[0].argv[0] = strdup("app");
        app[0].argv[1] = NULL;
        /* provide directives so the apps do what the user requested */
        ninfo = 2;
        PMIX_INFO_CREATE(app[0].info, ninfo);
        PMIX_INFO_LOAD(&app[0].info[0], PMIX_NP, 128, PMIX_UINT64);
        PMIX_INFO_LOAD(&app[0].info[0], PMIX_MAPBY, "slot", PMIX_STRING);

        /* setup the name of the daemon executable to launch */
        app[1].cmd = strdup("debuggerdaemon");
        app[1].argc = 1;
        app[1].argv = (char**)malloc(2*sizeof(char*));
        app[1].argv[0] = strdup("debuggerdaemon");
        app[1].argv[1] = NULL;
        /* provide directives so the daemons go where we want, and
         * let the RM know these are debugger daemons */
        ninfo = 2;
        PMIX_INFO_CREATE(app[1].info, ninfo);
        PMIX_INFO_LOAD(&app[1].info[0], PMIX_MAPBY, "ppr:1:node", PMIX_STRING);  // instruct the RM to launch one copy of the executable on each node
        PMIX_INFO_LOAD(&app[1].info[1], PMIX_DEBUGGER_DAEMONS, true, PMIX_BOOL); // these are debugger daemons
        /* spawn the daemons */
        PMIx_Spawn(NULL, 0, app, napps, dspace);
        /* cleanup */
        PMIX_APP_FREE(app, napps);
        
        /* this is where a debugger tool would wait until the debug operation is complete */
    }

    
 done:
    PMIx_tool_finalize(NULL, 0);

    return(ret);
}

static int attach_to_running_job(char *nspace)
{
    pmix_status_t rc;
    pmix_proc_t myproc;
    pmix_info_t *info;
    pmix_app_t *app;
    size_t ninfo, napps;

    /* query the active nspaces so we can verify that the
     * specified one exists */
    ninfo = 1;
    PMIX_INFO_CREATE(info, ninfo);
    (void)strncpy(info[0].key, PMIX_QUERY_NAMESPACES, PMIX_MAX_KEYLEN);
    if (PMIX_SUCCESS != (rc = PMIx_Query_info(info, ninfo))) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Query_info failed: %d\n", myproc.nspace, myproc.rank, rc);
        return -1;
    }
    /* the query should have returned a comma-delimited list of nspaces */
    if (PMIX_STRING != info[0].type) {
        fprintf(stderr, "Query returned incorrect data type: %d\n", info[0].type);
        return -1;
    }
    if (NULL == info[0].data.string) {
        fprintf(stderr, "Query returned no active nspaces\n");
        return -1;
    }
    /* split the returned string and look for the given nspace */

    /* if not found, then we have an error */
    PMIX_INFO_FREE(info, ninfo);
    
    /* get the proctable for this nspace */
    ninfo = 1;
    PMIX_INFO_CREATE(info, ninfo);
    (void)strncpy(info[0].key, PMIX_QUERY_PROC_TABLE, PMIX_MAX_KEYLEN);
    (void)strncpy(info[0].qualifier, nspace, PMIX_MAX_KEYLEN);
    if (PMIX_SUCCESS != (rc = PMIx_Query_info(info, ninfo))) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Query_info failed: %d\n", myproc.nspace, myproc.rank, rc);
        return -1;
    }
    /* the query should have returned a data_array */
    if (PMIX_DATA_ARRAY != info[0].type) {
        fprintf(stderr, "Query returned incorrect data type: %d\n", info[0].type);
        return -1;
    }
    if (NULL == info[0].data.darray.array) {
        fprintf(stderr, "Query returned no proctable info\n");
        return -1;
    }
    /* the data array consists of a struct:
     *     size_t size;
     *     void* array;
     *
     * In this case, the array is composed of pmix_proc_info_t structs:
     *     pmix_proc_t proc;   // contains the nspace,rank of this proc
     *     char* hostname;
     *     char* executable_name;
     *     pid_t pid;
     *     int exit_code;
     *     pmix_proc_state_t state;
     */
    
    /* this is where a debugger tool would process the proctable to
     * create whatever blob it needs to provide to its daemons */
    PMIX_INFO_FREE(info, ninfo);

    /* setup the debugger daemon spawn request */
    napps = 1;
    PMIX_APP_CREATE(app, napps);
    /* setup the name of the daemon executable to launch */
    app[0].cmd = strdup("debuggerdaemon");
    app[0].argc = 1;
    app[0].argv = (char**)malloc(2*sizeof(char*));
    app[0].argv[0] = strdup("debuggerdaemon");
    app[0].argv[1] = NULL;
    /* provide directives so the daemons go where we want, and
     * let the RM know these are debugger daemons */
    ninfo = 3;
    PMIX_INFO_CREATE(app[0].info, ninfo);
    PMIX_INFO_LOAD(&app[0].info[0], PMIX_MAPBY, "ppr:1:node", PMIX_STRING);  // instruct the RM to launch one copy of the executable on each node
    PMIX_INFO_LOAD(&app[0].info[1], PMIX_DEBUGGER_DAEMONS, true, PMIX_BOOL); // these are debugger daemons
    PMIX_INFO_LOAD(&app[0].info[2], PMIX_DEBUG_TARGET, nspace, PMIX_STRING); // the "jobid" of the application to be debugged

    /* spawn the daemons */
    PMIx_Spawn(NULL, 0, app, napps, dspace);
    /* cleanup */
    PMIX_APP_FREE(app, napps);

    /* this is where a debugger tool would wait until the debug operation is complete */

    return 0;
}

