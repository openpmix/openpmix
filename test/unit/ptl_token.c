/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * The connection token PMIx_server_setup_fork gives each rank.
 *
 * setup_fork puts PMIX_PTL_TOKEN_LEN random bytes in the child's
 * environment, in lowercase hex after the name of the server that made
 * them and a ';', and the server keeps them for the rank. A process adds
 * them to its connect-ack only for a server of that name: another name,
 * another rank, or a value with no name, adds nothing.
 * The kept token is the only one accepted for it: another, none at all,
 * one of the wrong length or not hex are refused with
 * PMIX_ERR_INVALID_CRED. Two ranks get different tokens, and a rank
 * prepared again gets a new one, which replaces the old. A rank neither
 * registered nor prepared has none, and anything is taken for it.
 *
 * PMIx_server_register_client gives a rank a token nobody is handed:
 * until the rank is prepared, nothing is taken for it, and then only the
 * token it was handed. Registering a rank already prepared keeps the
 * token it was handed; one that connected before it was registered is
 * given none.
 *
 * A process prepared with no rank of its own gets no token, and one it
 * would have inherited is taken out of its environment.
 *
 * Deregistering the rank forgets its token; deregistering the job forgets
 * every rank's.
 */

#include "src/include/pmix_config.h"

#include "include/pmix_server.h"
#include "src/include/pmix_globals.h"
#include "src/mca/ptl/base/base.h"
#include "src/threads/pmix_threads.h"
#include "src/util/pmix_environ.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TK_NSPACE "token.ns"

static int npass = 0;
static int nfail = 0;

static void report(const char *name, int passed)
{
    fprintf(stdout, "  %s: %s\n", passed ? "PASS" : "FAIL", name);
    if (passed) {
        ++npass;
    } else {
        ++nfail;
    }
}

static pmix_server_module_t mymodule = {0};

/* prepare `rank` and return the token it was given, or NULL */
static char *prepare(pmix_rank_t rank, char ***envp)
{
    pmix_proc_t proc;
    char **env = NULL, *tok;

    PMIX_LOAD_PROCID(&proc, TK_NSPACE, rank);
    if (NULL != envp) {
        env = *envp;
    }
    if (PMIX_SUCCESS != PMIx_server_setup_fork(&proc, &env)) {
        return NULL;
    }
    tok = pmix_getenv(PMIX_PTL_TOKEN_ENV, env);
    /* the hex, after the server's name */
    if (NULL != tok && NULL != (tok = strrchr(tok, ';'))) {
        tok = strdup(tok + 1);
    } else {
        tok = NULL;
    }
    if (NULL != envp) {
        *envp = env;
    } else {
        PMIx_Argv_free(env);
    }
    return tok;
}

/* whether add_token adds a token for the server nspace.rank - `want`, if
 * given - and keeps the info it was handed */
static bool added(const char *nspace, pmix_rank_t rank, const char *want)
{
    pmix_info_t *info;
    size_t ninfo = 1, n;
    bool found = false, kept = false;

    PMIX_INFO_CREATE(info, 1);
    PMIX_INFO_LOAD(&info[0], PMIX_PROC_PID, &rank, PMIX_PID);
    if (PMIX_SUCCESS != pmix_ptl_base_add_token(&info, &ninfo, nspace, rank)) {
        PMIX_INFO_FREE(info, ninfo);
        return false;
    }
    for (n = 0; n < ninfo; n++) {
        if (PMIx_Check_key(info[n].key, PMIX_PTL_TOKEN_KEY)) {
            found = (NULL == want || 0 == strcmp(want, info[n].value.data.string));
        } else if (PMIx_Check_key(info[n].key, PMIX_PROC_PID)) {
            kept = true;
        }
    }
    PMIX_INFO_FREE(info, ninfo);
    return found && kept;
}

static bool is_hex(const char *s, size_t len)
{
    size_t n;

    if (NULL == s || len != strlen(s)) {
        return false;
    }
    for (n = 0; n < len; n++) {
        if (!isdigit((unsigned char) s[n]) && !('a' <= s[n] && 'f' >= s[n])) {
            return false;
        }
    }
    return true;
}

/* a blocking registration that worked says PMIX_OPERATION_SUCCEEDED */
static bool registered(pmix_status_t rc)
{
    return PMIX_SUCCESS == rc || PMIX_OPERATION_SUCCEEDED == rc;
}

/* A rank the connection handler made an entry for - one that connected
 * before it was registered - registered by the host afterwards. Runs the
 * entry's setup on the progress thread, where the handler would */
typedef struct {
    pmix_event_t ev;
    pmix_lock_t lock;
} adopt_caddy_t;

static void add_entry(int sd, short args, void *cbdata)
{
    adopt_caddy_t *cd = (adopt_caddy_t *) cbdata;
    pmix_namespace_t *nptr;
    pmix_rank_info_t *info;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    nptr = PMIX_NEW(pmix_namespace_t);
    nptr->nspace = strdup("token.adopt");
    info = PMIX_NEW(pmix_rank_info_t);
    info->pname.nspace = strdup(nptr->nspace);
    info->pname.rank = 0;
    pmix_list_append(&nptr->ranks, &info->super);
    pmix_list_append(&pmix_globals.nspaces, &nptr->super);
    PMIX_WAKEUP_THREAD(&cd->lock);
}

static void adopt_case(void)
{
    adopt_caddy_t cd;
    pmix_proc_t proc;
    pmix_status_t rc;

    PMIX_CONSTRUCT_LOCK(&cd.lock);
    pmix_event_assign(&cd.ev, pmix_globals.evbase, -1, EV_WRITE, add_entry, &cd);
    PMIX_POST_OBJECT(&cd);
    pmix_event_active(&cd.ev, EV_WRITE, 1);
    PMIX_WAIT_THREAD(&cd.lock);
    PMIX_DESTRUCT_LOCK(&cd.lock);

    PMIX_LOAD_PROCID(&proc, "token.adopt", 0);
    rc = PMIx_server_register_client(&proc, getuid(), getgid(), NULL, NULL, NULL);
    report("a rank that connected before it was registered is given none",
           registered(rc) && PMIX_SUCCESS == pmix_ptl_base_check_token("token.adopt", 0, NULL));
}

int main(void)
{
    pmix_nspace_t ns;
    pmix_proc_t proc;
    char *t0, *t1, *t0b, *t2 = NULL, bad[2 * PMIX_PTL_TOKEN_LEN + PMIX_MAX_NSLEN + 32], **env = NULL;
    char value[2 * PMIX_PTL_TOKEN_LEN + PMIX_MAX_NSLEN + 32];
    pmix_status_t rc;

    fprintf(stdout, "\n=== the connection token setup_fork gives a rank ===\n\n");
    if (PMIX_SUCCESS != PMIx_server_init(&mymodule, NULL, 0)) {
        fprintf(stderr, "PMIx_server_init failed\n");
        return 1;
    }
    PMIX_LOAD_NSPACE(ns, TK_NSPACE);
    rc = PMIx_server_register_nspace(ns, 3, NULL, 0, NULL, NULL);
    if (PMIX_SUCCESS != rc && PMIX_OPERATION_SUCCEEDED != rc) {
        fprintf(stderr, "register_nspace failed\n");
        return 1;
    }

    t0 = prepare(0, NULL);
    t1 = prepare(1, NULL);
    report("setup_fork puts a token of the rank's in the environment, in lowercase hex",
           is_hex(t0, 2 * PMIX_PTL_TOKEN_LEN) && is_hex(t1, 2 * PMIX_PTL_TOKEN_LEN));
    PMIX_LOAD_PROCID(&proc, TK_NSPACE, 0);
    env = NULL;
    (void) PMIx_server_setup_fork(&proc, &env);
    snprintf(bad, sizeof(bad), "%s.%u;", pmix_globals.myid.nspace, pmix_globals.myid.rank);
    report("...after the name of the server that made it",
           NULL != pmix_getenv(PMIX_PTL_TOKEN_ENV, env) &&
           0 == strncmp(pmix_getenv(PMIX_PTL_TOKEN_ENV, env), bad, strlen(bad)));
    PMIx_Argv_free(env);
    env = NULL;
    free(t0);
    t0 = prepare(0, NULL);
    if (NULL == t0 || NULL == t1) {
        PMIx_server_finalize();
        return 1;
    }
    report("two ranks get different tokens", 0 != strcmp(t0, t1));
    report("a rank's own token is taken",
           PMIX_SUCCESS == pmix_ptl_base_check_token(TK_NSPACE, 0, t0));
    report("another rank's token is refused",
           PMIX_ERR_INVALID_CRED == pmix_ptl_base_check_token(TK_NSPACE, 0, t1));
    report("no token is refused",
           PMIX_ERR_INVALID_CRED == pmix_ptl_base_check_token(TK_NSPACE, 0, NULL));
    snprintf(bad, sizeof(bad), "%s", t0);
    bad[2 * PMIX_PTL_TOKEN_LEN - 1] = '\0';
    report("a token cut short is refused",
           PMIX_ERR_INVALID_CRED == pmix_ptl_base_check_token(TK_NSPACE, 0, bad));
    snprintf(bad, sizeof(bad), "%s0", t0);
    report("a token run long is refused",
           PMIX_ERR_INVALID_CRED == pmix_ptl_base_check_token(TK_NSPACE, 0, bad));
    snprintf(bad, sizeof(bad), "%s", t0);
    bad[0] = 'g';
    report("a token that is not hex is refused",
           PMIX_ERR_INVALID_CRED == pmix_ptl_base_check_token(TK_NSPACE, 0, bad));
    snprintf(bad, sizeof(bad), "%s", t0);
    bad[2 * PMIX_PTL_TOKEN_LEN - 1] = ('0' == bad[2 * PMIX_PTL_TOKEN_LEN - 1]) ? '1' : '0';
    report("a token differing in its last digit is refused",
           PMIX_ERR_INVALID_CRED == pmix_ptl_base_check_token(TK_NSPACE, 0, bad));
    report("anything is taken for a rank never prepared",
           PMIX_SUCCESS == pmix_ptl_base_check_token(TK_NSPACE, 2, NULL) &&
           PMIX_SUCCESS == pmix_ptl_base_check_token("token.other", 0, t0));

    t0b = prepare(0, NULL);
    report("a rank prepared again gets a new token, which is taken",
           NULL != t0b && 0 != strcmp(t0, t0b) &&
           PMIX_SUCCESS == pmix_ptl_base_check_token(TK_NSPACE, 0, t0b));
    report("...and its old one is refused",
           PMIX_ERR_INVALID_CRED == pmix_ptl_base_check_token(TK_NSPACE, 0, t0));

    /* registration */
    PMIX_LOAD_PROCID(&proc, TK_NSPACE, 2);
    rc = PMIx_server_register_client(&proc, getuid(), getgid(), NULL, NULL, NULL);
    report("a registered rank not yet prepared is taken with no token, nor with another's",
           registered(rc) &&
           PMIX_ERR_INVALID_CRED == pmix_ptl_base_check_token(TK_NSPACE, 2, NULL) &&
           PMIX_ERR_INVALID_CRED == pmix_ptl_base_check_token(TK_NSPACE, 2, t1));
    t2 = prepare(2, NULL);
    report("...and once prepared, with the token it was handed",
           NULL != t2 && PMIX_SUCCESS == pmix_ptl_base_check_token(TK_NSPACE, 2, t2));
    PMIX_LOAD_PROCID(&proc, TK_NSPACE, 1);
    rc = PMIx_server_register_client(&proc, getuid(), getgid(), NULL, NULL, NULL);
    report("registering a rank already prepared keeps the token it was handed",
           registered(rc) && PMIX_SUCCESS == pmix_ptl_base_check_token(TK_NSPACE, 1, t1));
    adopt_case();

    /* a process with no rank of its own, in an environment that has one */
    PMIx_Setenv(PMIX_PTL_TOKEN_ENV, t1, true, &env);
    PMIX_LOAD_PROCID(&proc, TK_NSPACE, PMIX_RANK_WILDCARD);
    rc = PMIx_server_setup_fork(&proc, &env);
    report("a process with no rank gets no token, and loses one it inherited",
           PMIX_SUCCESS == rc && NULL == pmix_getenv(PMIX_PTL_TOKEN_ENV, env));
    PMIx_Argv_free(env);

    /* what a process adds to its connect-ack */
    snprintf(value, sizeof(value), "%s.%u;%s", pmix_globals.myid.nspace, pmix_globals.myid.rank,
             t1);
    setenv(PMIX_PTL_TOKEN_ENV, value, 1);
    report("a process adds its token for the server that made it",
           added(pmix_globals.myid.nspace, pmix_globals.myid.rank, t1));
    report("...and for no server of another name",
           !added("token.other", pmix_globals.myid.rank, NULL) &&
           !added(pmix_globals.myid.nspace, pmix_globals.myid.rank + 1, NULL));
    setenv(PMIX_PTL_TOKEN_ENV, t1, 1);
    report("...nor for any when it names no server",
           !added(pmix_globals.myid.nspace, pmix_globals.myid.rank, NULL));
    unsetenv(PMIX_PTL_TOKEN_ENV);

    PMIX_LOAD_PROCID(&proc, TK_NSPACE, 1);
    PMIx_server_deregister_client(&proc, NULL, NULL);
    report("deregistering a rank forgets its token",
           PMIX_SUCCESS == pmix_ptl_base_check_token(TK_NSPACE, 1, NULL) &&
           PMIX_ERR_INVALID_CRED == pmix_ptl_base_check_token(TK_NSPACE, 0, NULL));
    PMIx_server_deregister_nspace(ns, NULL, NULL);
    report("deregistering the job forgets every rank's",
           PMIX_SUCCESS == pmix_ptl_base_check_token(TK_NSPACE, 0, NULL));

    free(t0);
    free(t1);
    free(t0b);
    free(t2);
    PMIx_server_finalize();
    fprintf(stdout, "\n%d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
