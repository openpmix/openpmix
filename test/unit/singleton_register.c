/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit tests for directives handed to PMIx_server_init() - PMIX_SINGLETON
 * and PMIX_ALLOC_MAU. Both are values that come straight from the host,
 * and the key says only what the directive is, never what is in the value.
 *
 * The value of PMIX_SINGLETON is the string representation of a proc ID -
 * i.e., "nspace.rank" - naming the singleton the server was started to
 * support. The server parses it during init and pre-registers that nspace
 * and rank so the singleton is not rejected when it connects.
 *
 * A malformed value used to walk off the end of the string (there is no
 * guarantee a '.' is present, and the parser dereferenced strrchr's result
 * unconditionally), faulting the server instead of telling the caller what
 * it did wrong. These tests pin the accept/reject boundary: a well-formed
 * value must register the singleton, and every malformed value must come
 * back as PMIX_ERR_BAD_PARAM without a crash.
 *
 * The same "the key says only what the directive is" rule governs the
 * string directives PMIX_SERVER_TMPDIR, PMIX_SYSTEM_TMPDIR and
 * PMIX_SERVER_NSPACE, each of which was read straight out of the value
 * union: a scalar loaded under one of those keys reached strdup as a
 * pointer built from that scalar.
 *
 * PMIX_ALLOC_MAU carries a pmix_data_array_t that belongs to the *caller*.
 * Init stores it in the datastore, which deep-copies it - but it built the
 * kval it stored through by pointing the value straight at the caller's
 * array, and a kval destructor frees a PMIX_DATA_ARRAY payload outright.
 * So releasing that kval handed the host's array back to the heap while
 * the host still held it, and the host's own free of it was a double free.
 * The test asserts both halves: the array still reads correctly after init
 * returns, and the caller can still free it.
 *
 * A rejected init must also leave the library usable. PMIx_server_init
 * latches pmix_globals.init_called before it does any work, and an init
 * that returned an error without giving that latch back wedged the
 * process: every later attempt answered PMIX_ERR_INIT, and
 * PMIx_server_finalize declined as well because we were never marked
 * initialized. Correcting a rejected directive and trying again is the
 * obvious thing for a caller to do, so the last two cases do exactly
 * that - once for a rejection that lands before pmix_rte_init and once
 * for one that lands after it, which is the side that has to unwind.
 *
 * Each case runs in a forked child because one PMIx_server_init per
 * process is all these cases need, and a child's exit status is also how
 * a fault (death by signal) is detected and reported as a failure rather
 * than being mistaken for a rejection.
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "include/pmix_server.h"
#include "src/include/pmix_globals.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* child exit codes */
#define CHILD_OK        0 /* init succeeded and the singleton is registered */
#define CHILD_BADPARAM  1 /* init rejected the value with PMIX_ERR_BAD_PARAM */
#define CHILD_OTHERERR  2 /* init failed with some other status */
#define CHILD_NOTFOUND  3 /* init succeeded but the singleton is not registered */
#define CHILD_ACCEPTED  4 /* init accepted a value it should have rejected */
#define CHILD_RETRY     5 /* the retry after a rejected init did not succeed */

static int npass = 0;
static int nfail = 0;

static pmix_server_module_t mymodule = {0};

static void report(const char *name, int passed, const char *detail)
{
    if (passed) {
        fprintf(stdout, "  PASS: %s\n", name);
        ++npass;
    } else {
        fprintf(stdout, "  FAIL: %s (%s)\n", name, detail);
        ++nfail;
    }
}

/* confirm the singleton's nspace was registered with the expected rank */
static bool singleton_registered(const char *nspace, pmix_rank_t rank)
{
    pmix_namespace_t *nptr;
    pmix_rank_info_t *rinfo;

    PMIX_LIST_FOREACH (nptr, &pmix_globals.nspaces, pmix_namespace_t) {
        if (NULL == nptr->nspace || 0 != strcmp(nptr->nspace, nspace)) {
            continue;
        }
        PMIX_LIST_FOREACH (rinfo, &nptr->ranks, pmix_rank_info_t) {
            if (rank == rinfo->pname.rank) {
                return true;
            }
        }
    }
    return false;
}

/* run one init attempt and exit with the code describing what happened.
 * If "badtype" is set, the directive is loaded as something other than a
 * string so the type check is exercised as well. */
static void child(const char *value, bool badtype, const char *nspace, pmix_rank_t rank)
{
    pmix_info_t info;
    pmix_status_t rc;
    bool found;

    if (badtype) {
        PMIX_INFO_LOAD(&info, PMIX_SINGLETON, &rank, PMIX_UINT32);
    } else {
        PMIX_INFO_LOAD(&info, PMIX_SINGLETON, value, PMIX_STRING);
    }

    rc = PMIx_server_init(&mymodule, &info, 1);
    PMIX_INFO_DESTRUCT(&info);

    if (PMIX_ERR_BAD_PARAM == rc) {
        exit(CHILD_BADPARAM);
    }
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "     init returned %s\n", PMIx_Error_string(rc));
        exit(CHILD_OTHERERR);
    }

    if (NULL == nspace) {
        /* the value should have been rejected - report that it was not
         * instead of probing for an nspace we were never given */
        PMIx_server_finalize();
        exit(CHILD_ACCEPTED);
    }
    found = singleton_registered(nspace, rank);
    PMIx_server_finalize();
    exit(found ? CHILD_OK : CHILD_NOTFOUND);
}

/* fork a child to attempt the init, then check its exit status */
static void check(const char *name, const char *value, bool badtype, const char *nspace,
                  pmix_rank_t rank, int expected)
{
    pid_t pid;
    int status;
    char detail[256];

    fflush(stdout);
    fflush(stderr);

    pid = fork();
    if (0 > pid) {
        report(name, 0, "fork failed");
        return;
    }
    if (0 == pid) {
        child(value, badtype, nspace, rank);
        /* not reached */
        exit(CHILD_OTHERERR);
    }

    if (pid != waitpid(pid, &status, 0)) {
        report(name, 0, "waitpid failed");
        return;
    }
    if (WIFSIGNALED(status)) {
        /* this is the crash we are guarding against */
        snprintf(detail, sizeof(detail), "server died on signal %d", WTERMSIG(status));
        report(name, 0, detail);
        return;
    }
    if (!WIFEXITED(status)) {
        report(name, 0, "child did not exit normally");
        return;
    }
    snprintf(detail, sizeof(detail), "expected exit %d, got %d", expected, WEXITSTATUS(status));
    report(name, expected == WEXITSTATUS(status), detail);
}

/* Attempt an init carrying PMIX_ALLOC_MAU. When "badtype" is set the
 * directive is loaded as a scalar so the type screen is exercised;
 * otherwise it carries a well-formed two-element array that this function
 * owns, reads back after init, and then frees - which is where an init
 * that released the caller's array shows up, as a double free. */
static void mau_child(bool badtype)
{
    pmix_data_array_t *darray;
    pmix_info_t *iptr;
    pmix_info_t info;
    pmix_status_t rc;
    uint32_t scalar = 42;

    PMIX_INFO_CONSTRUCT(&info);
    PMIX_LOAD_KEY(info.key, PMIX_ALLOC_MAU);

    if (badtype) {
        info.value.type = PMIX_UINT32;
        info.value.data.uint32 = scalar;
        rc = PMIx_server_init(&mymodule, &info, 1);
        if (PMIX_ERR_BAD_PARAM == rc) {
            exit(CHILD_BADPARAM);
        }
        if (PMIX_SUCCESS == rc) {
            PMIx_server_finalize();
            exit(CHILD_ACCEPTED);
        }
        fprintf(stderr, "     init returned %s\n", PMIx_Error_string(rc));
        exit(CHILD_OTHERERR);
    }

    PMIX_DATA_ARRAY_CREATE(darray, 2, PMIX_INFO);
    if (NULL == darray) {
        exit(CHILD_OTHERERR);
    }
    iptr = (pmix_info_t *) darray->array;
    PMIX_INFO_LOAD(&iptr[0], "mau.first", "one", PMIX_STRING);
    PMIX_INFO_LOAD(&iptr[1], "mau.second", "two", PMIX_STRING);

    info.value.type = PMIX_DATA_ARRAY;
    info.value.data.darray = darray;

    rc = PMIx_server_init(&mymodule, &info, 1);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "     init returned %s\n", PMIx_Error_string(rc));
        exit(CHILD_OTHERERR);
    }

    /* our array must have survived init untouched */
    if (darray != info.value.data.darray || 2 != darray->size ||
        NULL == darray->array) {
        PMIx_server_finalize();
        exit(CHILD_NOTFOUND);
    }
    iptr = (pmix_info_t *) darray->array;
    if (!PMIX_CHECK_KEY(&iptr[0], "mau.first") ||
        !PMIX_CHECK_KEY(&iptr[1], "mau.second")) {
        PMIx_server_finalize();
        exit(CHILD_NOTFOUND);
    }

    PMIx_server_finalize();
    /* and we must still be able to give it back - against a library that
     * released it for us this is a double free, and the allocator aborts */
    PMIX_DATA_ARRAY_FREE(darray);
    exit(CHILD_OK);
}

/* fork a child to attempt a PMIX_ALLOC_MAU init, then check its status */
static void check_mau(const char *name, bool badtype, int expected)
{
    pid_t pid;
    int status;
    char detail[256];

    fflush(stdout);
    fflush(stderr);

    pid = fork();
    if (0 > pid) {
        report(name, 0, "fork failed");
        return;
    }
    if (0 == pid) {
        mau_child(badtype);
        /* not reached */
        exit(CHILD_OTHERERR);
    }

    if (pid != waitpid(pid, &status, 0)) {
        report(name, 0, "waitpid failed");
        return;
    }
    if (WIFSIGNALED(status)) {
        snprintf(detail, sizeof(detail), "server died on signal %d", WTERMSIG(status));
        report(name, 0, detail);
        return;
    }
    if (!WIFEXITED(status)) {
        report(name, 0, "child did not exit normally");
        return;
    }
    snprintf(detail, sizeof(detail), "expected exit %d, got %d", expected, WEXITSTATUS(status));
    report(name, expected == WEXITSTATUS(status), detail);
}

/* Attempt an init carrying a directive whose value is not of the type the
 * key implies. A key states only what a directive *is*, never what its
 * value holds, and the string directives here were read straight out of
 * the union - so a scalar loaded under one of these keys reached strdup
 * as a pointer built from that scalar. */
static void badtype_child(const char *key)
{
    pmix_info_t info;
    pmix_status_t rc;
    uint32_t scalar = 42;

    PMIX_INFO_LOAD(&info, key, &scalar, PMIX_UINT32);
    rc = PMIx_server_init(&mymodule, &info, 1);
    PMIX_INFO_DESTRUCT(&info);

    if (PMIX_ERR_BAD_PARAM == rc) {
        exit(CHILD_BADPARAM);
    }
    if (PMIX_SUCCESS == rc) {
        PMIx_server_finalize();
        exit(CHILD_ACCEPTED);
    }
    fprintf(stderr, "     init returned %s\n", PMIx_Error_string(rc));
    exit(CHILD_OTHERERR);
}

/* fork a child to attempt a mistyped-directive init, then check its status */
static void check_badtype(const char *name, const char *key, int expected)
{
    pid_t pid;
    int status;
    char detail[256];

    fflush(stdout);
    fflush(stderr);

    pid = fork();
    if (0 > pid) {
        report(name, 0, "fork failed");
        return;
    }
    if (0 == pid) {
        badtype_child(key);
        /* not reached */
        exit(CHILD_OTHERERR);
    }

    if (pid != waitpid(pid, &status, 0)) {
        report(name, 0, "waitpid failed");
        return;
    }
    if (WIFSIGNALED(status)) {
        /* against an unfixed library the strdup of a scalar-as-pointer
         * takes the process down here rather than reporting anything */
        snprintf(detail, sizeof(detail), "server died on signal %d", WTERMSIG(status));
        report(name, 0, detail);
        return;
    }
    if (!WIFEXITED(status)) {
        report(name, 0, "child did not exit normally");
        return;
    }
    snprintf(detail, sizeof(detail), "expected exit %d, got %d", expected, WEXITSTATUS(status));
    report(name, expected == WEXITSTATUS(status), detail);
}

/* Reject a directive, then correct it and init again.
 *
 * "early" chooses which side of pmix_rte_init the first rejection lands
 * on. A mistyped PMIX_SINGLETON is caught while reading the directives,
 * before the runtime exists; a malformed singleton *string* is caught by
 * register_singleton, well after it - and only the second exercises the
 * unwind that has to give the runtime, the server globals and the latch
 * back. */
static void retry_child(bool early)
{
    pmix_info_t info;
    pmix_status_t rc;
    uint32_t scalar = 42;
    bool found;

    if (early) {
        PMIX_INFO_LOAD(&info, PMIX_SINGLETON, &scalar, PMIX_UINT32);
    } else {
        PMIX_INFO_LOAD(&info, PMIX_SINGLETON, "no-separator", PMIX_STRING);
    }
    rc = PMIx_server_init(&mymodule, &info, 1);
    PMIX_INFO_DESTRUCT(&info);
    if (PMIX_ERR_BAD_PARAM != rc) {
        fprintf(stderr, "     first init returned %s\n", PMIx_Error_string(rc));
        if (PMIX_SUCCESS == rc) {
            PMIx_server_finalize();
            exit(CHILD_ACCEPTED);
        }
        exit(CHILD_OTHERERR);
    }

    /* the caller corrects the directive and tries again */
    PMIX_INFO_LOAD(&info, PMIX_SINGLETON, "retry.2", PMIX_STRING);
    rc = PMIx_server_init(&mymodule, &info, 1);
    PMIX_INFO_DESTRUCT(&info);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "     retry returned %s\n", PMIx_Error_string(rc));
        exit(CHILD_RETRY);
    }
    found = singleton_registered("retry", 2);
    PMIx_server_finalize();
    exit(found ? CHILD_OK : CHILD_NOTFOUND);
}

/* fork a child to attempt the reject-then-retry, then check its status */
static void check_retry(const char *name, bool early, int expected)
{
    pid_t pid;
    int status;
    char detail[256];

    fflush(stdout);
    fflush(stderr);

    pid = fork();
    if (0 > pid) {
        report(name, 0, "fork failed");
        return;
    }
    if (0 == pid) {
        retry_child(early);
        /* not reached */
        exit(CHILD_OTHERERR);
    }

    if (pid != waitpid(pid, &status, 0)) {
        report(name, 0, "waitpid failed");
        return;
    }
    if (WIFSIGNALED(status)) {
        snprintf(detail, sizeof(detail), "server died on signal %d", WTERMSIG(status));
        report(name, 0, detail);
        return;
    }
    if (!WIFEXITED(status)) {
        report(name, 0, "child did not exit normally");
        return;
    }
    snprintf(detail, sizeof(detail), "expected exit %d, got %d", expected, WEXITSTATUS(status));
    report(name, expected == WEXITSTATUS(status), detail);
}

/* a nspace longer than PMIX_MAX_NSLEN cannot name a real nspace */
static char *overlong(void)
{
    char *p;
    size_t len = PMIX_MAX_NSLEN + 8;

    p = malloc(len + 4);
    memset(p, 'a', len);
    p[len] = '.';
    p[len + 1] = '0';
    p[len + 2] = '\0';
    return p;
}

int main(int argc, char **argv)
{
    char *big;

    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    fprintf(stdout, "PMIX_SINGLETON parsing\n");

    /* well-formed values are accepted and register the singleton */
    check("simple nspace.rank", "singleton.0", false, "singleton", 0, CHILD_OK);
    check("non-zero rank", "singleton.7", false, "singleton", 7, CHILD_OK);
    /* the rank is delimited by the LAST '.', so dots are legal in an nspace */
    check("dotted nspace", "my.dotted.ns.3", false, "my.dotted.ns", 3, CHILD_OK);

    /* malformed values are rejected, not faulted */
    check("no separator", "singleton", false, NULL, 0, CHILD_BADPARAM);
    check("empty string", "", false, NULL, 0, CHILD_BADPARAM);
    check("separator only", ".", false, NULL, 0, CHILD_BADPARAM);
    check("no nspace", ".0", false, NULL, 0, CHILD_BADPARAM);
    check("no rank", "singleton.", false, NULL, 0, CHILD_BADPARAM);
    check("non-numeric rank", "singleton.abc", false, NULL, 0, CHILD_BADPARAM);
    check("trailing garbage", "singleton.0abc", false, NULL, 0, CHILD_BADPARAM);
    check("trailing space", "singleton.0 ", false, NULL, 0, CHILD_BADPARAM);
    check("hex rank", "singleton.0x10", false, NULL, 0, CHILD_BADPARAM);
    check("negative rank", "singleton.-1", false, NULL, 0, CHILD_BADPARAM);
    check("overflowed rank", "singleton.99999999999999999999", false, NULL, 0, CHILD_BADPARAM);
    check("reserved rank", "singleton.4294967295", false, NULL, 0, CHILD_BADPARAM);

    big = overlong();
    check("overlong nspace", big, false, NULL, 0, CHILD_BADPARAM);
    free(big);

    /* the directive must carry a string */
    check("wrong data type", NULL, true, NULL, 0, CHILD_BADPARAM);

    fprintf(stdout, "\nPMIX_ALLOC_MAU handling\n");

    /* the caller's array must survive init and still be freeable */
    check_mau("array is left to its owner", false, CHILD_OK);
    /* the directive must carry a data array */
    check_mau("wrong data type", true, CHILD_BADPARAM);

    fprintf(stdout, "\nString directives must carry strings\n");

    check_badtype("mistyped PMIX_SERVER_TMPDIR", PMIX_SERVER_TMPDIR, CHILD_BADPARAM);
    check_badtype("mistyped PMIX_SYSTEM_TMPDIR", PMIX_SYSTEM_TMPDIR, CHILD_BADPARAM);
    check_badtype("mistyped PMIX_SERVER_NSPACE", PMIX_SERVER_NSPACE, CHILD_BADPARAM);

    fprintf(stdout, "\nA rejected init leaves the library usable\n");

    /* rejected before the runtime is built */
    check_retry("retry after an early rejection", true, CHILD_OK);
    /* rejected after it is built, so the unwind has real work to do */
    check_retry("retry after a late rejection", false, CHILD_OK);

    fprintf(stdout, "\n%d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
