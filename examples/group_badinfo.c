/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Malformed directives must be rejected, not dereferenced.
 *
 * The group APIs read a number of caller-supplied attributes straight out of
 * the pmix_value_t union without checking the type tag first. PMIX_GROUP_INFO
 * is the worst of them: construct_msg() in src/client/pmix_client_group.c did
 *
 *     iarray = (pmix_info_t *) info[n].value.data.darray->array;
 *     niarray = info[n].value.data.darray->size;
 *     if (PMIX_PROC != iarray[0].value.type) {
 *
 * with nothing establishing that the value is a data array at all. Hand it a
 * PMIX_GROUP_INFO carrying an integer and "darray" is that integer
 * reinterpreted as a pointer, which is then dereferenced twice - once to read
 * ->array, once to read iarray[0].value.type. A zero-length or NULL array had
 * the same effect one step later.
 *
 * These are attributes an application supplies, so getting them wrong is an
 * ordinary programming error and has to produce an error return rather than a
 * SIGSEGV inside the library.
 *
 * This needs a server: PMIx_Group_construct stops at the "am I connected?"
 * check well before it reaches construct_msg, so a singleton
 * (test/unit/client_api.c) cannot get near it. test/unit/run_grpbadinfo.pl
 * drives it under test/simple/simptest.
 *
 * The point of the test is that the process survives to print its verdict.
 * What PMIx_Group_construct *returns* for a malformed directive is
 * deliberately not asserted: the group may legitimately fail to form for
 * other reasons in this harness. Surviving the call is the regression.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "examples.h"
#include <pmix.h>

static pmix_proc_t myproc;

int main(int argc, char **argv)
{
    pmix_status_t rc;
    pmix_proc_t proc;
    pmix_info_t info;
    pmix_info_t *results = NULL;
    size_t nresults = 0;
    uint32_t u32;

    EXAMPLES_HIDE_UNUSED_PARAMS(argc, argv);

    rc = PMIx_Init(&myproc, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_Init failed: %s\n", PMIx_Error_string(rc));
        exit(1);
    }
    fprintf(stderr, "[%s:%u] group_badinfo running\n", myproc.nspace, myproc.rank);

    PMIX_LOAD_PROCID(&proc, myproc.nspace, PMIX_RANK_WILDCARD);

    /* 1. PMIX_GROUP_INFO whose value is a scalar, not a data array. This is
     *    the one that used to segfault. */
    u32 = 42;
    PMIX_INFO_LOAD(&info, PMIX_GROUP_INFO, &u32, PMIX_UINT32);
    rc = PMIx_Group_construct("badinfo-scalar", &proc, 1, &info, 1, &results, &nresults);
    fprintf(stderr, "[%s:%u] scalar PMIX_GROUP_INFO -> %s\n",
            myproc.nspace, myproc.rank, PMIx_Error_string(rc));
    PMIX_INFO_DESTRUCT(&info);
    if (NULL != results) {
        PMIX_INFO_FREE(results, nresults);
        results = NULL;
        nresults = 0;
    }

    /* 2. The right type, but an empty array - iarray[0] is off the end. */
    PMIX_INFO_CONSTRUCT(&info);
    PMIx_Load_key(info.key, PMIX_GROUP_INFO);
    info.value.type = PMIX_DATA_ARRAY;
    info.value.data.darray = NULL;
    rc = PMIx_Group_construct("badinfo-null", &proc, 1, &info, 1, &results, &nresults);
    fprintf(stderr, "[%s:%u] NULL-array PMIX_GROUP_INFO -> %s\n",
            myproc.nspace, myproc.rank, PMIx_Error_string(rc));
    /* the value never owned anything, so do not let DESTRUCT chase it */
    info.value.type = PMIX_UNDEF;
    PMIX_INFO_DESTRUCT(&info);
    if (NULL != results) {
        PMIX_INFO_FREE(results, nresults);
        results = NULL;
        nresults = 0;
    }

    fprintf(stderr, "[%s:%u] group_badinfo: PASS\n", myproc.nspace, myproc.rank);
    fflush(stderr);

    rc = PMIx_Finalize(NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "[%s:%u] PMIx_Finalize failed: %s\n",
                myproc.nspace, myproc.rank, PMIx_Error_string(rc));
        return 1;
    }
    return 0;
}
