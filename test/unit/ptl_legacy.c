/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Clients and tools of earlier releases - legacy peers.
 *
 * They connect over TCP, which a process of this release on the server's
 * host never uses. A server takes them only when ptl_base_legacy_peers or
 * the PMIX_SERVER_LEGACY_PEERS attribute, which overrides it, says so -
 * and only in a build configured with legacy peer support, the default.
 * In a build without it, asking for them fails the init.
 *
 * By default a server takes none, and has no TCP listener unless it takes
 * remote tools. Where it reports where it is - the PMIX_SERVER_URIxx
 * variables PMIx_server_setup_fork sets, and its rendezvous files - names
 * its local socket. With legacy peers, all of
 * them name its TCP listener, and the local socket's address is given
 * beside it.
 *
 * Each case runs in a child forked before any PMIx call.
 */

#include "src/include/pmix_config.h"

#include "include/pmix_server.h"
#include "src/include/pmix_globals.h"
#include "src/mca/ptl/base/base.h"
#include "src/util/pmix_environ.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHILD_PASS 0
#define CHILD_FAIL 1
#define CHILD_SKIP 77

static int npass = 0;
static int nfail = 0;
static char tmpdir[1024];

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

static pmix_server_module_t mymodule = {0};

static int check(const char *what, bool ok)
{
    fprintf(stdout, "    %s: %s\n", what, ok ? "yes" : "NO");
    return ok ? CHILD_PASS : CHILD_FAIL;
}

/* whether s begins with the URI of the listener lt */
static bool names(const char *s, const pmix_listener_t *lt)
{
    return NULL != s && NULL != lt->uri && 0 == strncmp(s, lt->uri, strlen(lt->uri));
}

/* the first line of the file, without its newline */
static char *first_line(const char *path)
{
    static char line[2048];
    FILE *fp;

    fp = (NULL == path) ? NULL : fopen(path, "r");
    if (NULL == fp) {
        return NULL;
    }
    if (NULL == fgets(line, sizeof(line), fp)) {
        fclose(fp);
        return NULL;
    }
    fclose(fp);
    line[strcspn(line, "\n")] = '\0';
    return line;
}

/* Start a server with tool support - so it writes a rendezvous file - and
 * legacy peers as `legacy` says: -1 not given, 0 false, 1 true. Check
 * that everything it reports names `want` */
static int check_server(int legacy, bool tcp_wanted)
{
    pmix_info_t info[3];
    size_t ninfo = 0;
    pmix_proc_t proc;
    char **env = NULL, *line, *var;
    bool flag = true, lflag = (1 == legacy);
    pmix_listener_t *want;
    int rc = CHILD_PASS;

    PMIX_INFO_LOAD(&info[ninfo], PMIX_SERVER_TOOL_SUPPORT, &flag, PMIX_BOOL);
    ++ninfo;
    PMIX_INFO_LOAD(&info[ninfo], PMIX_SERVER_TMPDIR, tmpdir, PMIX_STRING);
    ++ninfo;
    if (0 <= legacy) {
        PMIX_INFO_LOAD(&info[ninfo], PMIX_SERVER_LEGACY_PEERS, &lflag, PMIX_BOOL);
        ++ninfo;
    }
    if (PMIX_SUCCESS != PMIx_server_init(&mymodule, info, ninfo)) {
        fprintf(stdout, "    PMIx_server_init failed\n");
        return CHILD_FAIL;
    }
    rc |= check("the server takes legacy peers as asked", tcp_wanted == pmix_ptl_base.legacy_peers);
    rc |= check(tcp_wanted ? "it has a TCP listener" : "it has no TCP listener",
                tcp_wanted == (0 <= pmix_ptl_base.listener.socket &&
                               NULL != pmix_ptl_base.listener.uri));
    rc |= check("it has its local socket", 0 <= pmix_ptl_base.local_listener.socket &&
                                               NULL != pmix_ptl_base.local_listener.uri);
    want = tcp_wanted ? &pmix_ptl_base.listener : &pmix_ptl_base.local_listener;

    line = first_line(pmix_ptl_base.pid_filename);
    fprintf(stdout, "    rendezvous file's first line: %s\n", (NULL == line) ? "(none)" : line);
    rc |= check("the rendezvous file names it", names(line, want));

    PMIX_LOAD_PROCID(&proc, "legacy.client", 0);
    if (PMIX_SUCCESS != PMIx_server_setup_fork(&proc, &env)) {
        fprintf(stdout, "    PMIx_server_setup_fork failed\n");
        rc = CHILD_FAIL;
    } else {
        var = pmix_getenv("PMIX_SERVER_URI41", env);
        fprintf(stdout, "    PMIX_SERVER_URI41=%s\n", (NULL == var) ? "(none)" : var);
        rc |= check("PMIX_SERVER_URI41 names it", names(var, want));
        var = pmix_getenv(PMIX_PTL_LOCAL_URI_ENV, env);
        rc |= check("the local socket is given beside it",
                    names(var, &pmix_ptl_base.local_listener));
        PMIx_Argv_free(env);
    }
    PMIx_server_finalize();
    return rc;
}

static int run_default(void)
{
    return check_server(-1, false);
}

static int run_attr_on(void)
{
#if PMIX_ENABLE_LEGACY_PEERS
    return check_server(1, true);
#else
    pmix_info_t info;
    bool flag = true;
    pmix_status_t prc;

    PMIX_INFO_LOAD(&info, PMIX_SERVER_LEGACY_PEERS, &flag, PMIX_BOOL);
    prc = PMIx_server_init(&mymodule, &info, 1);
    PMIX_INFO_DESTRUCT(&info);
    fprintf(stdout, "    PMIx_server_init asking for legacy peers: %s\n", PMIx_Error_string(prc));
    if (PMIX_SUCCESS == prc) {
        PMIx_server_finalize();
        return CHILD_FAIL;
    }
    return CHILD_PASS;
#endif
}

static int run_param_on(void)
{
    setenv("PMIX_MCA_ptl_base_legacy_peers", "1", 1);
#if PMIX_ENABLE_LEGACY_PEERS
    return check_server(-1, true);
#else
    if (PMIX_SUCCESS == PMIx_server_init(&mymodule, NULL, 0)) {
        PMIx_server_finalize();
        return CHILD_FAIL;
    }
    return CHILD_PASS;
#endif
}

static int run_attr_overrides(void)
{
    setenv("PMIX_MCA_ptl_base_legacy_peers", "1", 1);
    return check_server(0, false);
}

static void run_case(const char *name, int (*fn)(void))
{
    pid_t pid;
    int status;
    char detail[64];

    fflush(stdout);
    pid = fork();
    if (0 == pid) {
        exit(fn());
    }
    if (0 > pid || pid != waitpid(pid, &status, 0)) {
        report(name, 0, "fork or wait failed");
        return;
    }
    if (WIFEXITED(status) && CHILD_SKIP == WEXITSTATUS(status)) {
        fprintf(stdout, "  SKIP: %s\n", name);
        return;
    }
    snprintf(detail, sizeof(detail), "child status 0x%x", status);
    report(name, WIFEXITED(status) && CHILD_PASS == WEXITSTATUS(status), detail);
}

int main(void)
{
    const char *base = getenv("TMPDIR");

    snprintf(tmpdir, sizeof(tmpdir), "%s/pmix-lgc-XXXXXX", (NULL == base) ? "/tmp" : base);
    if (NULL == mkdtemp(tmpdir)) {
        fprintf(stderr, "could not make a scratch directory\n");
        return 1;
    }
    unsetenv("PMIX_MCA_ptl_base_legacy_peers");
    fprintf(stdout, "Legacy peers (this build %s them):\n",
            PMIX_ENABLE_LEGACY_PEERS ? "supports" : "does not support");
    run_case("by default a server takes no legacy peers and reports its local socket",
             run_default);
#if PMIX_ENABLE_LEGACY_PEERS
    run_case("PMIX_SERVER_LEGACY_PEERS opens a TCP listener, which the server reports",
             run_attr_on);
    run_case("ptl_base_legacy_peers does the same", run_param_on);
#else
    run_case("asking with PMIX_SERVER_LEGACY_PEERS fails the init", run_attr_on);
    run_case("asking with ptl_base_legacy_peers fails the init", run_param_on);
#endif
    run_case("PMIX_SERVER_LEGACY_PEERS false overrides the parameter", run_attr_overrides);
    rmdir(tmpdir);

    fprintf(stdout, "\nResults: %d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
