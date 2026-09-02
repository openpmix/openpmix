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
 * Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

/*
 * Cross-namespace modex: every proc reads a key every proc of the OTHER
 * namespace posted, including the ones that are not on this node.
 *
 * This is the PMIx-level shape of what an MPI_Comm_spawn followed by
 * MPI_Intercomm_merge and a collective does, and it is a regression test
 * for a get that came back PMIX_ERR_NOT_FOUND for a key that was sitting
 * in the server's own memory (issue #4225). Two things have to be true at
 * once for that to happen, which is why the geometry below matters:
 *
 *   1. A NODE HOSTS BOTH NAMESPACES. That is what gives the daemon a
 *      local client of the target namespace, and so a gds module for it
 *      that is not the daemon's own - the modex of a job whose clients
 *      negotiated gds/shmem3 goes into a shared segment the daemon's
 *      "hash" module cannot see. Run this with the children mapped by
 *      node onto the nodes their parents already occupy, i.e. at least
 *      two slots per node.
 *
 *   2. THE TARGET RANK IS ON ANOTHER NODE. A rank of the other namespace
 *      that is local here is answered out of the daemon's own store,
 *      because a local client's commit is filed in both. So at least two
 *      nodes.
 *
 * Either condition alone passes whether the bug is present or not. The
 * failing geometry is therefore >= 2 nodes with >= 2 slots each:
 *
 *      prterun --host n1:2,n2:2 --map-by node -n 2 ./xnspace_modex
 *
 * Each proc prints one "xnspace_modex: <nspace>:<rank>: PASS|FAIL" line
 * and a non-zero exit status accompanies any FAIL.
 */

#include <stdbool.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libgen.h>

#include "examples.h"
#include <pmix.h>

/* deliberately NOT a "pmix."-prefixed key: a reserved key is fetched by
 * name, and it is the non-reserved path - fetch everything this proc has,
 * then hope the wanted key is in there - that #4225 lived in */
#define XNS_KEY "xnspace.probe"

/* marks a child in its own environment, exactly as examples/dynamic.c does */
#define XNS_CHILD_ENV "PMIX_XNSPACE_CHILD"

static pmix_proc_t myproc;

/* what rank "rank" of namespace "nspace" is expected to have posted */
static void expected_value(char *buf, size_t sz, const char *nspace, pmix_rank_t rank)
{
    snprintf(buf, sz, "%s:%u", nspace, (unsigned) rank);
}

/* returns the number of failures, so a caller can just add them up */
static int check_peer(const char *nspace, pmix_rank_t rank)
{
    pmix_proc_t proc;
    pmix_value_t *val = NULL;
    char expect[2 * PMIX_MAX_NSLEN + 32];
    pmix_status_t rc;

    PMIX_LOAD_PROCID(&proc, nspace, rank);
    rc = PMIx_Get(&proc, XNS_KEY, NULL, 0, &val);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr,
                "xnspace_modex: %s:%u: PMIx_Get(%s:%u, %s) failed: %s\n",
                myproc.nspace, myproc.rank, nspace, (unsigned) rank,
                XNS_KEY, PMIx_Error_string(rc));
        return 1;
    }
    if (NULL == val || PMIX_STRING != val->type || NULL == val->data.string) {
        fprintf(stderr,
                "xnspace_modex: %s:%u: PMIx_Get(%s:%u, %s) returned the wrong type\n",
                myproc.nspace, myproc.rank, nspace, (unsigned) rank, XNS_KEY);
        if (NULL != val) {
            PMIX_VALUE_RELEASE(val);
        }
        return 1;
    }
    expected_value(expect, sizeof(expect), nspace, rank);
    if (0 != strcmp(expect, val->data.string)) {
        fprintf(stderr,
                "xnspace_modex: %s:%u: PMIx_Get(%s:%u, %s) gave \"%s\", expected \"%s\"\n",
                myproc.nspace, myproc.rank, nspace, (unsigned) rank, XNS_KEY,
                val->data.string, expect);
        PMIX_VALUE_RELEASE(val);
        return 1;
    }
    PMIX_VALUE_RELEASE(val);
    return 0;
}

int main(int argc, char **argv)
{
    pmix_status_t rc;
    pmix_value_t *val = NULL, value;
    pmix_proc_t proc, *parray = NULL;
    uint32_t nprocs;
    size_t nall = 0, n, nother;
    char nsp2[PMIX_MAX_NSLEN + 1];
    char other[PMIX_MAX_NSLEN + 1];
    char mine[2 * PMIX_MAX_NSLEN + 32];
    char hostname[1024];
    pmix_app_t *app;
    pmix_info_t info[2];
    bool ischild;
    int failures = 0;

    if (0 > gethostname(hostname, sizeof(hostname))) {
        exit(1);
    }
    ischild = (NULL != getenv(XNS_CHILD_ENV));

    rc = PMIx_Init(&myproc, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "xnspace_modex: PMIx_Init failed: %s\n", PMIx_Error_string(rc));
        exit(1);
    }
    fprintf(stderr, "xnspace_modex: %s:%u (%s) on host %s\n", myproc.nspace, myproc.rank,
            ischild ? "child" : "parent", hostname);

    /* how big is my own job */
    PMIX_LOAD_PROCID(&proc, myproc.nspace, PMIX_RANK_WILDCARD);
    rc = PMIx_Get(&proc, PMIX_JOB_SIZE, NULL, 0, &val);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "xnspace_modex: %s:%u: PMIx_Get job size failed: %s\n", myproc.nspace,
                myproc.rank, PMIx_Error_string(rc));
        goto done;
    }
    nprocs = val->data.uint32;
    PMIX_VALUE_RELEASE(val);

    /* post the value our peers in the other namespace will come looking
     * for, and exchange it across my own job - this is the fence that
     * puts a job's whole modex into the datastore of every daemon that
     * hosts one of its procs, which is where #4225 could not find it */
    expected_value(mine, sizeof(mine), myproc.nspace, myproc.rank);
    value.type = PMIX_STRING;
    value.data.string = mine;
    rc = PMIx_Put(PMIX_GLOBAL, XNS_KEY, &value);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "xnspace_modex: %s:%u: PMIx_Put failed: %s\n", myproc.nspace, myproc.rank,
                PMIx_Error_string(rc));
        goto done;
    }
    rc = PMIx_Commit();
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "xnspace_modex: %s:%u: PMIx_Commit failed: %s\n", myproc.nspace,
                myproc.rank, PMIx_Error_string(rc));
        goto done;
    }
    PMIX_LOAD_PROCID(&proc, myproc.nspace, PMIX_RANK_WILDCARD);
    PMIX_INFO_LOAD(&info[0], PMIX_COLLECT_DATA, NULL, PMIX_BOOL);
    rc = PMIx_Fence(&proc, 1, info, 1);
    PMIX_INFO_DESTRUCT(&info[0]);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "xnspace_modex: %s:%u: PMIx_Fence failed: %s\n", myproc.nspace,
                myproc.rank, PMIx_Error_string(rc));
        goto done;
    }

    if (ischild) {
        /* the parent told us who it is on our command line */
        if (3 > argc) {
            fprintf(stderr, "xnspace_modex: %s:%u: child started without its parent's identity\n",
                    myproc.nspace, myproc.rank);
            goto done;
        }
        PMIX_LOAD_NSPACE(other, argv[1]);
        nother = (size_t) atoi(argv[2]);
    } else if (0 == myproc.rank) {
        /* map the children by node so they land back on the nodes their
         * parents already occupy - that co-residency is what gives each
         * daemon a local client of both namespaces */
        PMIX_APP_CREATE(app, 1);
        app->cmd = strdup(argv[0]);
        app->maxprocs = nprocs;
        app->argv = (char **) malloc(4 * sizeof(char *));
        app->argv[0] = strdup(basename(argv[0]));
        app->argv[1] = strdup(myproc.nspace);
        if (0 > asprintf(&app->argv[2], "%u", nprocs)) {
            goto done;
        }
        app->argv[3] = NULL;
        app->env = (char **) malloc(2 * sizeof(char *));
        app->env[0] = strdup(XNS_CHILD_ENV "=1");
        app->env[1] = NULL;

        PMIX_INFO_LOAD(&info[0], PMIX_MAPBY, "node", PMIX_STRING);
        rc = PMIx_Spawn(info, 1, app, 1, nsp2);
        PMIX_INFO_DESTRUCT(&info[0]);
        PMIX_APP_FREE(app, 1);
        if (PMIX_SUCCESS != rc) {
            fprintf(stderr, "xnspace_modex: %s:%u: PMIx_Spawn failed: %s\n", myproc.nspace,
                    myproc.rank, PMIx_Error_string(rc));
            goto done;
        }
        /* tell our own peers which namespace the children got */
        value.type = PMIX_STRING;
        value.data.string = nsp2;
        rc = PMIx_Put(PMIX_GLOBAL, "xnspace.child", &value);
        if (PMIX_SUCCESS != rc) {
            fprintf(stderr, "xnspace_modex: %s:%u: PMIx_Put child nspace failed: %s\n",
                    myproc.nspace, myproc.rank, PMIx_Error_string(rc));
            goto done;
        }
        PMIx_Commit();
        PMIX_LOAD_PROCID(&proc, myproc.nspace, PMIX_RANK_WILDCARD);
        rc = PMIx_Fence(&proc, 1, NULL, 0);
        if (PMIX_SUCCESS != rc) {
            fprintf(stderr, "xnspace_modex: %s:%u: PMIx_Fence failed: %s\n", myproc.nspace,
                    myproc.rank, PMIx_Error_string(rc));
            goto done;
        }
        PMIX_LOAD_NSPACE(other, nsp2);
        nother = nprocs;
    } else {
        PMIX_LOAD_PROCID(&proc, myproc.nspace, PMIX_RANK_WILDCARD);
        rc = PMIx_Fence(&proc, 1, NULL, 0);
        if (PMIX_SUCCESS != rc) {
            fprintf(stderr, "xnspace_modex: %s:%u: PMIx_Fence failed: %s\n", myproc.nspace,
                    myproc.rank, PMIx_Error_string(rc));
            goto done;
        }
        proc.rank = 0;
        rc = PMIx_Get(&proc, "xnspace.child", NULL, 0, &val);
        if (PMIX_SUCCESS != rc) {
            fprintf(stderr, "xnspace_modex: %s:%u: PMIx_Get child nspace failed: %s\n",
                    myproc.nspace, myproc.rank, PMIx_Error_string(rc));
            goto done;
        }
        PMIX_LOAD_NSPACE(other, val->data.string);
        PMIX_VALUE_RELEASE(val);
        nother = nprocs;
    }

    /* connect the two namespaces - the parents cannot look at the children
     * until the children exist, and this is what makes both sides wait */
    nall = nprocs + nother;
    PMIX_PROC_CREATE(parray, nall);
    for (n = 0; n < nprocs; n++) {
        PMIX_LOAD_PROCID(&parray[n], myproc.nspace, n);
    }
    for (n = 0; n < nother; n++) {
        PMIX_LOAD_PROCID(&parray[n + nprocs], other, n);
    }
    rc = PMIx_Connect(parray, nall, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "xnspace_modex: %s:%u: PMIx_Connect failed: %s\n", myproc.nspace,
                myproc.rank, PMIx_Error_string(rc));
        goto done;
    }

    /* the point of the exercise: read what every proc of the other
     * namespace posted. No fence spanning the two jobs, deliberately -
     * each namespace exchanged its own data above, and answering this
     * from what the daemons already hold (or fetching it from the daemon
     * that does) is what PMIx is being asked to do */
    for (n = 0; n < nother; n++) {
        failures += check_peer(other, (pmix_rank_t) n);
    }

    fprintf(stderr, "xnspace_modex: %s:%u: %s\n", myproc.nspace, myproc.rank,
            (0 == failures) ? "PASS" : "FAIL");

    rc = PMIx_Disconnect(parray, nall, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "xnspace_modex: %s:%u: PMIx_Disconnect failed: %s\n", myproc.nspace,
                myproc.rank, PMIx_Error_string(rc));
    }

done:
    if (NULL != parray) {
        PMIX_PROC_FREE(parray, nall);
    }
    rc = PMIx_Finalize(NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "xnspace_modex: %s:%u: PMIx_Finalize failed: %s\n", myproc.nspace,
                myproc.rank, PMIx_Error_string(rc));
    }
    fflush(stderr);
    return (0 == failures) ? 0 : 1;
}
