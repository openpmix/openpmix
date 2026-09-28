/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * Whose group-construct outcome a joining client acts on.
 *
 * A client that accepts an invitation waits in PMIx_Group_join for the
 * construct's outcome, which arrives as PMIX_GROUP_CONSTRUCT_COMPLETE or
 * PMIX_GROUP_CONSTRUCT_ABORT. It acts on one only if it names the group it
 * is joining and comes from that group's leader or from its own server;
 * and when the group completes, it stores endpoint data only for the
 * group's members.
 *
 * The test process is the server. It registers a job, forks a real client
 * into it, and the client joins a group led by a process named in the test.
 * The server then raises outcomes itself - a server may raise an event
 * under any source, which is exactly what a relayed forgery would look like
 * to the client:
 *
 *   first, a completion and an abort from a process that is not the leader,
 *   and a completion from the leader that names no group - none of which
 *   may complete the join, record the group or store any data;
 *
 *   then the leader's completion, carrying one contribution from a member
 *   and one from a process outside the group - which completes the join,
 *   records the group, and stores only the member's data.
 *
 * The client reports each check itself and exits with the number that
 * failed.
 */

#include "src/include/pmix_config.h"
#include "include/pmix.h"
#include "include/pmix_server.h"
#include "src/client/pmix_client_ops.h"
#include "src/include/pmix_globals.h"
#include "src/util/pmix_argv.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define GES_NSPACE "gesrc-job"
#define GES_GROUP  "gesrc-grp"
#define GES_KEY    "gesrc.endpt"

extern char **environ;

static pmix_server_module_t mymodule = {0};

static int nfail = 0;

static void check(bool ok, const char *what)
{
    fprintf(stdout, "  %s: %s\n", ok ? "PASS" : "FAIL", what);
    fflush(stdout);
    if (!ok) {
        ++nfail;
    }
}

/* the leader, a process that merely claims to be, and one outside the group */
static void who(pmix_proc_t *leader, pmix_proc_t *forger, pmix_proc_t *outsider)
{
    PMIX_LOAD_PROCID(leader, "gesrc-leader", 0);
    PMIX_LOAD_PROCID(forger, "gesrc-forger", 0);
    PMIX_LOAD_PROCID(outsider, "gesrc-outsider", 0);
}

/* ---------------- the client ---------------- */

static volatile bool joined = false;
static volatile pmix_status_t join_status = PMIX_ERROR;

static void join_cb(pmix_status_t status, pmix_info_t *info, size_t ninfo, void *cbdata,
                    pmix_release_cbfunc_t release_fn, void *release_cbdata)
{
    PMIX_HIDE_UNUSED_PARAMS(info, ninfo, cbdata);
    join_status = status;
    joined = true;
    if (NULL != release_fn) {
        release_fn(release_cbdata);
    }
}

static bool group_recorded(void)
{
    pmix_group_t *gp;
    bool found = false;

    pmix_mutex_lock(&pmix_client_globals.grouplock);
    PMIX_LIST_FOREACH (gp, &pmix_client_globals.groups, pmix_group_t) {
        if (NULL != gp->grpid && 0 == strcmp(gp->grpid, GES_GROUP)) {
            found = true;
        }
    }
    pmix_mutex_unlock(&pmix_client_globals.grouplock);
    return found;
}

/* is there data stored locally for this process under our key? */
static bool data_stored(const pmix_proc_t *proc)
{
    pmix_value_t *val = NULL;
    pmix_info_t opt;
    bool yes = true;
    pmix_status_t rc;

    PMIX_INFO_LOAD(&opt, PMIX_OPTIONAL, &yes, PMIX_BOOL);
    rc = PMIx_Get(proc, GES_KEY, &opt, 1, &val);
    PMIX_INFO_DESTRUCT(&opt);
    if (NULL != val) {
        PMIX_VALUE_RELEASE(val);
    }
    return PMIX_SUCCESS == rc;
}

static void settle(void)
{
    usleep(500000);
}

static int run_client(int readyfd, int gofd)
{
    pmix_proc_t me, leader, forger, outsider;
    pmix_status_t rc;
    char c = 'r';
    int i;

    who(&leader, &forger, &outsider);
    if (PMIX_SUCCESS != PMIx_Init(&me, NULL, 0)) {
        return 100;
    }
    rc = PMIx_Group_join_nb(GES_GROUP, &leader, PMIX_GROUP_ACCEPT, NULL, 0, join_cb, NULL);
    if (PMIX_SUCCESS != rc) {
        fprintf(stdout, "  client: join_nb failed: %s\n", PMIx_Error_string(rc));
        PMIx_Finalize(NULL, 0);
        return 100;
    }
    /* phase one: the outcomes that must not count */
    if (1 != write(readyfd, &c, 1) || 1 != read(gofd, &c, 1)) {
        return 100;
    }
    settle();
    check(!joined, "an outcome from a non-leader, or naming no group, does not end the join");
    check(!group_recorded(), "and does not record the group");
    check(!data_stored(&forger) && !data_stored(&leader),
          "and stores none of the data it carries");

    /* phase two: the leader's outcome */
    if (1 != write(readyfd, &c, 1) || 1 != read(gofd, &c, 1)) {
        return 100;
    }
    for (i = 0; i < 40 && !joined; i++) {
        usleep(50000);
    }
    check(joined && PMIX_SUCCESS == join_status, "the leader's completion ends the join");
    check(group_recorded(), "and records the group");
    check(data_stored(&leader), "and stores a member's contribution");
    check(!data_stored(&outsider), "but not a contribution from outside the group");

    PMIx_Finalize(NULL, 0);
    return nfail;
}

/* ---------------- the server ---------------- */

static volatile bool regdone = false;

static void regcbfunc(pmix_status_t status, void *cbdata)
{
    PMIX_HIDE_UNUSED_PARAMS(status, cbdata);
    regdone = true;
}

static pmix_status_t register_job(void)
{
    pmix_info_t info[2];
    pmix_nspace_t ns;
    pmix_proc_t p0;
    pmix_status_t rc;
    char *noderegex = NULL, *ppnregex = NULL;
    int n;

    PMIx_generate_regex(pmix_globals.hostname, &noderegex);
    PMIx_generate_ppn("0", &ppnregex);
    PMIX_INFO_LOAD(&info[0], PMIX_NODE_MAP, noderegex, PMIX_REGEX);
    PMIX_INFO_LOAD(&info[1], PMIX_PROC_MAP, ppnregex, PMIX_REGEX);
    PMIX_LOAD_NSPACE(ns, GES_NSPACE);
    rc = PMIx_server_register_nspace(ns, 1, info, 2, NULL, NULL);
    if (PMIX_OPERATION_SUCCEEDED == rc) {
        rc = PMIX_SUCCESS;
    }
    PMIX_INFO_DESTRUCT(&info[0]);
    PMIX_INFO_DESTRUCT(&info[1]);
    free(noderegex);
    free(ppnregex);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    PMIX_LOAD_PROCID(&p0, GES_NSPACE, 0);
    rc = PMIx_server_register_client(&p0, geteuid(), getegid(), NULL, regcbfunc, NULL);
    if (PMIX_OPERATION_SUCCEEDED == rc) {
        return PMIX_SUCCESS;
    }
    for (n = 0; PMIX_SUCCESS == rc && n < 400 && !regdone; n++) {
        usleep(50000);
    }
    return rc;
}

/* one contribution: the contributor, the scope, and a value */
static void load_contribution(pmix_info_t *dst, const pmix_proc_t *who)
{
    pmix_data_array_t darray;
    pmix_info_t *iptr;
    pmix_scope_t scope = PMIX_GLOBAL;

    PMIX_DATA_ARRAY_CONSTRUCT(&darray, 3, PMIX_INFO);
    iptr = (pmix_info_t *) darray.array;
    PMIX_INFO_LOAD(&iptr[0], PMIX_PROCID, who, PMIX_PROC);
    PMIX_INFO_LOAD(&iptr[1], PMIX_DATA_SCOPE, &scope, PMIX_SCOPE);
    PMIX_INFO_LOAD(&iptr[2], GES_KEY, "addr", PMIX_STRING);
    PMIX_INFO_LOAD(dst, PMIX_PROC_INFO_ARRAY, &darray, PMIX_DATA_ARRAY);
    PMIX_DATA_ARRAY_DESTRUCT(&darray);
}

/* raise a construct outcome at the client, under "source" */
static void raise_outcome(pmix_status_t code, const pmix_proc_t *source, bool name_group,
                          const pmix_proc_t *contrib1, const pmix_proc_t *contrib2)
{
    pmix_info_t info[6];
    pmix_data_array_t darray;
    pmix_proc_t client, members[2], leader, forger, outsider;
    size_t n = 0;

    who(&leader, &forger, &outsider);
    PMIX_LOAD_PROCID(&client, GES_NSPACE, 0);
    PMIX_LOAD_PROCID(&members[0], leader.nspace, leader.rank);
    PMIX_LOAD_PROCID(&members[1], GES_NSPACE, 0);

    PMIX_INFO_LOAD(&info[n], PMIX_EVENT_DO_NOT_CACHE, NULL, PMIX_BOOL);
    ++n;
    PMIX_INFO_LOAD(&info[n], PMIX_EVENT_CUSTOM_RANGE, &client, PMIX_PROC);
    ++n;
    darray.type = PMIX_PROC;
    darray.array = members;
    darray.size = 2;
    PMIX_INFO_LOAD(&info[n], PMIX_GROUP_MEMBERSHIP, &darray, PMIX_DATA_ARRAY);
    ++n;
    if (name_group) {
        PMIX_INFO_LOAD(&info[n], PMIX_GROUP_ID, GES_GROUP, PMIX_STRING);
        ++n;
    }
    if (NULL != contrib1) {
        load_contribution(&info[n], contrib1);
        ++n;
    }
    if (NULL != contrib2) {
        load_contribution(&info[n], contrib2);
        ++n;
    }
    PMIx_Notify_event(code, source, PMIX_RANGE_CUSTOM, info, n, NULL, NULL);
    while (0 < n) {
        --n;
        PMIX_INFO_DESTRUCT(&info[n]);
    }
}

int main(int argc, char **argv)
{
    char **client_env = NULL, *client_argv[5], fdbuf[2][16];
    int readypipe[2], gopipe[2], status = 0, i;
    pmix_proc_t p0, leader, forger, outsider;
    pid_t child, r;
    char c = 'g';

    setvbuf(stdout, NULL, _IONBF, 0);
    if (4 == argc && 0 == strcmp(argv[1], "client")) {
        return run_client(atoi(argv[2]), atoi(argv[3]));
    }

    fprintf(stdout, "\n=== whose construct outcome a joiner acts on ===\n\n");
    who(&leader, &forger, &outsider);
    if (0 != pipe(readypipe) || 0 != pipe(gopipe)) {
        return 1;
    }
    if (PMIX_SUCCESS != PMIx_server_init(&mymodule, NULL, 0)) {
        fprintf(stdout, "  FAIL: PMIx_server_init\n");
        return 1;
    }
    if (PMIX_SUCCESS != register_job()) {
        fprintf(stdout, "  FAIL: could not register the job\n");
        PMIx_server_finalize();
        return 1;
    }
    PMIX_LOAD_PROCID(&p0, GES_NSPACE, 0);
    client_env = PMIx_Argv_copy(environ);
    if (PMIX_SUCCESS != PMIx_server_setup_fork(&p0, &client_env)) {
        fprintf(stdout, "  FAIL: setup_fork\n");
        PMIx_server_finalize();
        return 1;
    }
    child = fork();
    if (0 > child) {
        return 1;
    }
    if (0 == child) {
        close(readypipe[0]);
        close(gopipe[1]);
        snprintf(fdbuf[0], sizeof(fdbuf[0]), "%d", readypipe[1]);
        snprintf(fdbuf[1], sizeof(fdbuf[1]), "%d", gopipe[0]);
        client_argv[0] = argv[0];
        client_argv[1] = (char *) "client";
        client_argv[2] = fdbuf[0];
        client_argv[3] = fdbuf[1];
        client_argv[4] = NULL;
        execve(argv[0], client_argv, client_env);
        _exit(127);
    }
    PMIx_Argv_free(client_env);
    close(readypipe[1]);
    close(gopipe[0]);

    /* phase one, once the client's join is armed */
    if (1 != read(readypipe[0], &c, 1)) {
        fprintf(stdout, "  FAIL: the client never joined\n");
        goto reap;
    }
    usleep(250000);
    raise_outcome(PMIX_GROUP_CONSTRUCT_COMPLETE, &forger, true, &forger, NULL);
    raise_outcome(PMIX_GROUP_CONSTRUCT_ABORT, &forger, true, NULL, NULL);
    raise_outcome(PMIX_GROUP_CONSTRUCT_COMPLETE, &leader, false, &leader, NULL);
    if (1 != write(gopipe[1], &c, 1)) {
        goto reap;
    }

    /* phase two */
    if (1 != read(readypipe[0], &c, 1)) {
        goto reap;
    }
    raise_outcome(PMIX_GROUP_CONSTRUCT_COMPLETE, &leader, true, &leader, &outsider);
    if (1 != write(gopipe[1], &c, 1)) {
        goto reap;
    }

reap:
    close(gopipe[1]);
    for (i = 0; i < 300; i++) {
        r = waitpid(child, &status, WNOHANG);
        if (child == r) {
            break;
        }
        usleep(100000);
    }
    if (300 == i) {
        kill(child, SIGKILL);
        waitpid(child, &status, 0);
        fprintf(stdout, "  FAIL: the client never finished\n");
        status = 1 << 8;
    }
    PMIx_server_finalize();
    if (!WIFEXITED(status) || 0 != WEXITSTATUS(status)) {
        fprintf(stdout, "\nFAILED\n");
        return 1;
    }
    fprintf(stdout, "\nall passed\n");
    return 0;
}
