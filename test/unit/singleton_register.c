/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit tests for the PMIX_SINGLETON directive to PMIx_server_init().
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
 * Each case runs in a forked child because PMIx_server_init() can only be
 * meaningfully called once per process - a failed init leaves init_called
 * set, so a second attempt returns PMIX_ERR_INIT. The parent inspects the
 * child's exit status, which is also how a fault (death by signal) is
 * detected and reported as a failure rather than being mistaken for a
 * rejection.
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

    fprintf(stdout, "\n%d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
