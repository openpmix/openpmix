/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit test: the namespace and rank a server takes for itself.
 *
 *   - PMIX_SERVER_NSPACE and PMIX_SERVER_RANK in its environment - what a
 *     launcher that starts a server itself sets - name it;
 *   - PMIX_SERVER_NAMESPACE, the name read before, still does;
 *   - the attributes of the same names win over the environment;
 *   - with neither, it is pmix-server, and its rank its pid.
 *
 * Each case runs in a child forked before any PMIx call.
 */

#include "src/include/pmix_config.h"

#include "include/pmix_server.h"
#include "src/include/pmix_globals.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static int npass = 0;
static int nfail = 0;

static pmix_server_module_t mymodule = {0};

/* start a server - with the attributes when `attrs` - and say whether it
 * is `nspace`:`rank` (rank PMIX_RANK_UNDEF for its pid) */
static int child(bool attrs, const char *nspace, pmix_rank_t rank)
{
    pmix_info_t info[2];
    pmix_rank_t r = 9;
    pmix_rank_t want = (PMIX_RANK_UNDEF == rank) ? (pmix_rank_t) getpid() : rank;
    int ok;

    if (attrs) {
        PMIX_INFO_LOAD(&info[0], PMIX_SERVER_NSPACE, "attr.ns", PMIX_STRING);
        PMIX_INFO_LOAD(&info[1], PMIX_SERVER_RANK, &r, PMIX_PROC_RANK);
    }
    if (PMIX_SUCCESS != PMIx_server_init(&mymodule, attrs ? info : NULL, attrs ? 2 : 0)) {
        return 2;
    }
    ok = (0 == strcmp(pmix_globals.myid.nspace, nspace) && want == pmix_globals.myid.rank);
    if (!ok) {
        fprintf(stdout, "    the server is %s:%u\n", pmix_globals.myid.nspace,
                pmix_globals.myid.rank);
    }
    PMIx_server_finalize();
    return ok ? 0 : 1;
}

static void run_case(const char *name, const char *nsvar, const char *ns, const char *rk,
                     bool attrs, const char *want_ns, pmix_rank_t want_rank)
{
    pid_t pid;
    int status;

    fflush(stdout);
    pid = fork();
    if (0 == pid) {
        unsetenv("PMIX_SERVER_NSPACE");
        unsetenv("PMIX_SERVER_NAMESPACE");
        unsetenv("PMIX_SERVER_RANK");
        if (NULL != nsvar) {
            setenv(nsvar, ns, 1);
        }
        if (NULL != rk) {
            setenv("PMIX_SERVER_RANK", rk, 1);
        }
        exit(child(attrs, want_ns, want_rank));
    }
    if (0 < pid && pid == waitpid(pid, &status, 0) && WIFEXITED(status) &&
        0 == WEXITSTATUS(status)) {
        fprintf(stdout, "  PASS: %s\n", name);
        ++npass;
    } else {
        fprintf(stdout, "  FAIL: %s\n", name);
        ++nfail;
    }
}

int main(void)
{
    char tmpdir[1024];
    const char *base = getenv("TMPDIR");

    snprintf(tmpdir, sizeof(tmpdir), "%s/pmix-sid-XXXXXX", (NULL == base) ? "/tmp" : base);
    if (NULL == mkdtemp(tmpdir)) {
        return 1;
    }
    setenv("TMPDIR", tmpdir, 1);

    fprintf(stdout, "\n=== a server's own namespace and rank ===\n\n");
    run_case("PMIX_SERVER_NSPACE and PMIX_SERVER_RANK name it", "PMIX_SERVER_NSPACE", "env.ns",
             "3", false, "env.ns", 3);
    run_case("PMIX_SERVER_NAMESPACE names it too", "PMIX_SERVER_NAMESPACE", "old.ns", "4", false,
             "old.ns", 4);
    run_case("the attributes win over the environment", "PMIX_SERVER_NSPACE", "env.ns", "3",
             true, "attr.ns", 9);
    run_case("with neither it is pmix-server, ranked by its pid", NULL, NULL, NULL, false,
             "pmix-server", PMIX_RANK_UNDEF);
    rmdir(tmpdir);

    fprintf(stdout, "\n%d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
