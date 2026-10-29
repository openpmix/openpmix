/*
 * Copyright (c) 2026      Barcelona Supercomputing Center (BSC-CNS). All Rights Reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * A get for a proc of another namespace leaves the requester holding that
 * namespace's current job-level data.
 *
 * The server answers such a get with the target's job-level data unless
 * the requester names the hash of the copy it already holds and that
 * hash is current. So a host revision of the target's job data, which
 * notifies only the target's own clients, still reaches another job's
 * client at its next get - and only an unchanged copy is skipped.
 *
 * One reader (job A) and seven targets (job B) share this server; B's map
 * also places an eighth rank on a node that is not this one. The reader
 * gets a key from B/0 with a PMIX_NODEID qualifier, which narrows the job
 * data it is sent to this node, then from B/1 without one: it must then
 * hold the other node's info too. A get from B/2, with nothing changed,
 * is answered with the hash alone, and the reader must still hold all of
 * it. A narrowed get from B/3 is sent part of the job data, stamped
 * with the current hash, which matches the reader's, so it keeps its
 * hash and the plain get from B/4 is answered with the hash alone. The host
 * then revises a job-level value of B, and after a get from B/5 the
 * reader must hold the revised value. Last, the host registers job C in
 * B's session, restating the session's size as a spawn does, and after a
 * get from B/6 the reader must hold the new size for B. Reads use
 * PMIX_OPTIONAL so the server cannot answer them.
 *
 * The server's get verbose output says when it sent the hash alone: that
 * must be the gets from B/2 and B/4 and no other. A debug build checks each such
 * hash against the data, and must find none stale.
 *
 * Runs with gds/hash: a gds/shmem3 client does not keep job data it is
 * sent this way.
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "include/pmix_server.h"
#include "src/include/pmix_globals.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define NS_A   "jdh-reader"
#define NS_B   "jdh-target"
#define NS_C   "jdh-sibling"
#define JOBKEY "sut.jdh.job"
#define PROCKEY "sut.jdh.k"
#define FIRST  11
#define SECOND 22
#define FAKENODE "jdh-fake"
#define SESSION 7
#define UNIV1  8
#define UNIV2  16
#define NTARGETS 7
#define NPROCS_B 8
#define PPN_B  "0,1,2,3,4,5,6;7"
#define HELD   "JOB DATA FOR " NS_B " HELD"
#define STALE  "WAS STALE"

static int npass = 0, nfail = 0;

static void report(const char *what, bool ok)
{
    fprintf(stdout, "  %s: %s\n", ok ? "PASS" : "FAIL", what);
    if (ok) {
        npass++;
    } else {
        nfail++;
    }
}

/* ------------------------------------------------------------------ */
/* the clients                                                         */
/* ------------------------------------------------------------------ */

static int read_local(const char *key, uint32_t *out)
{
    pmix_proc_t proc;
    pmix_value_t *val = NULL;
    pmix_info_t opt;
    pmix_status_t rc;

    PMIX_LOAD_PROCID(&proc, NS_B, PMIX_RANK_WILDCARD);
    PMIX_INFO_LOAD(&opt, PMIX_OPTIONAL, NULL, PMIX_BOOL);
    rc = PMIx_Get(&proc, key, &opt, 1, &val);
    PMIX_INFO_DESTRUCT(&opt);
    if (PMIX_SUCCESS != rc || NULL == val) {
        return -1;
    }
    rc = PMIx_Value_get_number(val, out, PMIX_UINT32);
    PMIX_VALUE_RELEASE(val);
    return (PMIX_SUCCESS == rc) ? 0 : -1;
}

/* does our own datastore hold job B's info for node 1, the node that is
 * not this one? Its local peers are rank 7 alone. (A get answers the
 * key it is qualified by from the qualifier, so asking for node 1's id
 * or name would prove nothing.) */
static int holds_fake_node(void)
{
    pmix_proc_t proc;
    pmix_value_t *val = NULL;
    pmix_info_t info[2];
    pmix_status_t rc;
    uint32_t nid = 1;
    int ret = -1;

    PMIX_LOAD_PROCID(&proc, NS_B, PMIX_RANK_WILDCARD);
    PMIX_INFO_LOAD(&info[0], PMIX_NODEID, &nid, PMIX_UINT32);
    PMIX_INFO_LOAD(&info[1], PMIX_OPTIONAL, NULL, PMIX_BOOL);
    rc = PMIx_Get(&proc, PMIX_LOCAL_PEERS, info, 2, &val);
    if (PMIX_SUCCESS == rc && NULL != val && PMIX_STRING == val->type &&
        NULL != val->data.string && 0 == strcmp(val->data.string, "7")) {
        ret = 0;
    }
    if (NULL != val) {
        PMIX_VALUE_RELEASE(val);
    }
    return ret;
}

static int get_target(pmix_rank_t r, bool narrowed)
{
    pmix_proc_t proc;
    pmix_value_t *val = NULL;
    pmix_info_t qual;
    pmix_status_t rc;
    uint32_t nid = 0;
    int x = -1;

    PMIX_LOAD_PROCID(&proc, NS_B, r);
    PMIX_INFO_LOAD(&qual, PMIX_NODEID, &nid, PMIX_UINT32);
    rc = PMIx_Get(&proc, PROCKEY, narrowed ? &qual : NULL, narrowed ? 1 : 0, &val);
    if (PMIX_SUCCESS != rc || NULL == val) {
        return -1;
    }
    if (PMIX_INT == val->type) {
        x = val->data.integer;
    }
    PMIX_VALUE_RELEASE(val);
    return ((int) r == x) ? 0 : -1;
}

static int run_reader(int readyfd, int gofd)
{
    pmix_proc_t me;
    uint32_t v = 0;
    char c = 'r';
    int ret = 0;

    if (PMIX_SUCCESS != PMIx_Init(&me, NULL, 0)) {
        return 1;
    }
    if (0 != get_target(0, true)) {
        fprintf(stderr, "  reader: narrowed get from target 0 failed\n");
        ret = 2;
    } else if (0 != get_target(1, false)) {
        fprintf(stderr, "  reader: get from target 1 failed\n");
        ret = 2;
    } else if (0 != holds_fake_node()) {
        fprintf(stderr, "  reader: does not hold the other node's info\n");
        ret = 7;
    } else if (0 != read_local(JOBKEY, &v) || FIRST != v) {
        fprintf(stderr, "  reader: held %u before the revision, expected %u\n",
                (unsigned) v, (unsigned) FIRST);
        ret = 3;
    } else if (0 != read_local(PMIX_UNIV_SIZE, &v) || UNIV1 != v) {
        fprintf(stderr, "  reader: held univ size %u, expected %u\n", (unsigned) v,
                (unsigned) UNIV1);
        ret = 8;
    } else if (0 != get_target(2, false)) {
        /* nothing changed, so this one is answered with the hash alone */
        fprintf(stderr, "  reader: get from target 2 failed\n");
        ret = 2;
    } else if (0 != holds_fake_node() || 0 != read_local(JOBKEY, &v) || FIRST != v) {
        fprintf(stderr, "  reader: lost job B's data on a get that kept it\n");
        ret = 10;
    } else if (0 != get_target(3, true)) {
        /* sent part of the job data, stamped with the hash the reader
         * holds: it keeps it */
        fprintf(stderr, "  reader: narrowed get from target 3 failed\n");
        ret = 2;
    } else if (0 != get_target(4, false)) {
        /* answered with the hash alone */
        fprintf(stderr, "  reader: get from target 4 failed\n");
        ret = 2;
    } else if (0 != holds_fake_node() || 0 != read_local(JOBKEY, &v) || FIRST != v) {
        fprintf(stderr, "  reader: lost job B's data after a narrowed get\n");
        ret = 11;
    }
    if (1 != write(readyfd, &c, 1) || 1 != read(gofd, &c, 1)) {
        ret = 4;
    }
    if (0 == ret && 0 != get_target(5, false)) {
        fprintf(stderr, "  reader: get from target 5 failed\n");
        ret = 5;
    } else if (0 == ret && (0 != read_local(JOBKEY, &v) || SECOND != v)) {
        fprintf(stderr, "  reader: held %u after the revision, expected %u\n",
                (unsigned) v, (unsigned) SECOND);
        ret = 6;
    }
    if (1 != write(readyfd, &c, 1) || 1 != read(gofd, &c, 1)) {
        ret = 4;
    }
    if (0 == ret && 0 != get_target(6, false)) {
        fprintf(stderr, "  reader: get from target 6 failed\n");
        ret = 5;
    } else if (0 == ret && (0 != read_local(PMIX_UNIV_SIZE, &v) || UNIV2 != v)) {
        fprintf(stderr, "  reader: held univ size %u after the session revision, "
                "expected %u\n", (unsigned) v, (unsigned) UNIV2);
        ret = 9;
    }
    PMIx_Finalize(NULL, 0);
    return ret;
}

static int run_target(int readyfd, int gofd)
{
    pmix_proc_t me;
    pmix_value_t v;
    int x;
    char c = 'r';

    if (PMIX_SUCCESS != PMIx_Init(&me, NULL, 0)) {
        return 1;
    }
    x = (int) me.rank;
    PMIX_VALUE_LOAD(&v, &x, PMIX_INT);
    if (PMIX_SUCCESS != PMIx_Put(PMIX_GLOBAL, PROCKEY, &v) ||
        PMIX_SUCCESS != PMIx_Commit()) {
        PMIx_Finalize(NULL, 0);
        return 2;
    }
    PMIX_VALUE_DESTRUCT(&v);
    if (1 != write(readyfd, &c, 1) || 1 != read(gofd, &c, 1)) {
        PMIx_Finalize(NULL, 0);
        return 3;
    }
    PMIx_Finalize(NULL, 0);
    return 0;
}

/* ------------------------------------------------------------------ */
/* the server                                                          */
/* ------------------------------------------------------------------ */

static volatile int regdone = 0;
static void regcbfunc(pmix_status_t status, void *cbdata)
{
    (void) status;
    (void) cbdata;
    regdone++;
}

static pmix_server_module_t mymodule = {0};

/* in session SESSION with that universe size, or in none for univ 0 */
static pmix_status_t register_job(const char *nsname, uint32_t nprocs, uint32_t nlocal,
                                  const char *nodes, const char *ppn, bool withkey,
                                  uint32_t univ)
{
    pmix_info_t info[7];
    pmix_proc_t proc;
    pmix_nspace_t ns;
    pmix_status_t rc;
    char *noderegex = NULL, *ppnregex = NULL;
    uint32_t first = FIRST, sid = SESSION, r;
    size_t n, ninfo = 4;
    int want, i;

    PMIx_generate_regex(nodes, &noderegex);
    PMIx_generate_ppn(ppn, &ppnregex);
    PMIX_INFO_LOAD(&info[0], PMIX_NODE_MAP, noderegex, PMIX_REGEX);
    PMIX_INFO_LOAD(&info[1], PMIX_PROC_MAP, ppnregex, PMIX_REGEX);
    PMIX_INFO_LOAD(&info[2], PMIX_JOB_SIZE, &nprocs, PMIX_UINT32);
    PMIX_INFO_LOAD(&info[3], PMIX_MAX_PROCS, &nprocs, PMIX_UINT32);
    if (0 < univ) {
        PMIX_INFO_LOAD(&info[ninfo++], PMIX_SESSION_ID, &sid, PMIX_UINT32);
        PMIX_INFO_LOAD(&info[ninfo++], PMIX_UNIV_SIZE, &univ, PMIX_UINT32);
    }
    if (withkey) {
        PMIX_INFO_LOAD(&info[ninfo++], JOBKEY, &first, PMIX_UINT32);
    }
    PMIX_LOAD_NSPACE(ns, nsname);
    rc = PMIx_server_register_nspace(ns, nlocal, info, ninfo, NULL, NULL);
    for (n = 0; n < ninfo; n++) {
        PMIX_INFO_DESTRUCT(&info[n]);
    }
    free(noderegex);
    free(ppnregex);
    if (PMIX_OPERATION_SUCCEEDED == rc) {
        rc = PMIX_SUCCESS;
    }
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    want = regdone + (int) nlocal;
    for (r = 0; r < nlocal; r++) {
        PMIX_LOAD_PROCID(&proc, nsname, r);
        rc = PMIx_server_register_client(&proc, geteuid(), getegid(), NULL, regcbfunc, NULL);
        if (PMIX_OPERATION_SUCCEEDED == rc) {
            regdone++;
        } else if (PMIX_SUCCESS != rc) {
            return rc;
        }
    }
    for (i = 0; i < 400 && regdone < want; i++) {
        usleep(50000);
    }
    if (regdone < want) {
        fprintf(stderr, "registering %s timed out\n", nsname);
        return PMIX_ERR_TIMEOUT;
    }
    return PMIX_SUCCESS;
}

static pmix_status_t revise(uint32_t value)
{
    pmix_info_t upd;
    pmix_nspace_t ns;
    pmix_status_t rc;

    PMIX_LOAD_NSPACE(ns, NS_B);
    PMIX_INFO_LOAD(&upd, JOBKEY, &value, PMIX_UINT32);
    rc = PMIx_server_register_nspace(ns, -1, &upd, 1, NULL, NULL);
    PMIX_INFO_DESTRUCT(&upd);
    if (PMIX_OPERATION_SUCCEEDED == rc) {
        rc = PMIX_SUCCESS;
    }
    return rc;
}

/* fork and exec one client of nsname/rank; its pipes are returned */
static pid_t start(const char *self, const char *role, const char *nsname, pmix_rank_t rank,
                   int *readyfd, int *gofd)
{
    pmix_proc_t proc;
    char **env;
    char *cargv[5], rbuf[16], gbuf[16];
    int ready[2], go[2];
    pid_t pid;

    if (0 != pipe(ready)) {
        return -1;
    }
    if (0 != pipe(go)) {
        close(ready[0]);
        close(ready[1]);
        return -1;
    }
    PMIX_LOAD_PROCID(&proc, nsname, rank);
    /* our own environment first, so the child runs the PMIx under test */
    env = PMIx_Argv_copy(environ);
    if (PMIX_SUCCESS != PMIx_server_setup_fork(&proc, &env)) {
        PMIx_Argv_free(env);
        pid = -1;
        goto fail;
    }
    pid = fork();
    if (0 == pid) {
        close(ready[0]);
        close(go[1]);
        snprintf(rbuf, sizeof(rbuf), "%d", ready[1]);
        snprintf(gbuf, sizeof(gbuf), "%d", go[0]);
        cargv[0] = (char *) self;
        cargv[1] = (char *) role;
        cargv[2] = rbuf;
        cargv[3] = gbuf;
        cargv[4] = NULL;
        execve(self, cargv, env);
        _exit(127);
    }
    PMIx_Argv_free(env);
    if (0 > pid) {
        goto fail;
    }
    close(ready[1]);
    close(go[0]);
    *readyfd = ready[0];
    *gofd = go[1];
    return pid;

fail:
    close(ready[0]);
    close(ready[1]);
    close(go[0]);
    close(go[1]);
    return pid;
}

/* send our stderr, which the server's get verbose output goes to, to a
 * file; the original descriptor is returned */
static int capture_stderr(FILE **log)
{
    int errfd;

    *log = tmpfile();
    if (NULL == *log) {
        return -1;
    }
    errfd = dup(STDERR_FILENO);
    if (0 > errfd || 0 > dup2(fileno(*log), STDERR_FILENO)) {
        fclose(*log);
        *log = NULL;
        return -1;
    }
    setenv("PMIX_MCA_pmix_server_get_verbose", "2", 1);
    return errfd;
}

/* restore stderr and check what the server said; the log is shown on a
 * failure */
static void check_log(FILE *log, int errfd)
{
    char line[1024];
    int nheld = 0, nstale = 0;

    fflush(stderr);
    dup2(errfd, STDERR_FILENO);
    close(errfd);
    rewind(log);
    while (NULL != fgets(line, sizeof(line), log)) {
        if (NULL != strstr(line, HELD)) {
            nheld++;
        }
        if (NULL != strstr(line, STALE)) {
            nstale++;
        }
    }
    /* the narrowed get between them confirmed the reader's hash */
    report("only the gets from targets 2 and 4 are answered with the hash alone", 2 == nheld);
    report("no cached hash was stale", 0 == nstale);
    if (0 < nfail) {
        rewind(log);
        while (NULL != fgets(line, sizeof(line), log)) {
            fputs(line, stderr);
        }
    }
    fclose(log);
}

static int server_up(void)
{
    pmix_info_t sinfo;
    bool flag = true;
    pmix_status_t rc;

    PMIX_INFO_LOAD(&sinfo, PMIX_SERVER_TOOL_SUPPORT, &flag, PMIX_BOOL);
    rc = PMIx_server_init(&mymodule, &sinfo, 1);
    PMIX_INFO_DESTRUCT(&sinfo);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    pid_t reader = -1, targets[NTARGETS];
    int rready = -1, rgo = -1, tready[NTARGETS], tgo[NTARGETS];
    int status = 0, i, errfd = -1;
    char nodes[256];
    FILE *log = NULL;
    bool completed = false;
    char c = 'g';

    setvbuf(stdout, NULL, _IOLBF, 0);
    for (i = 0; i < NTARGETS; i++) {
        targets[i] = -1;
        tready[i] = -1;
        tgo[i] = -1;
    }

    if (3 < argc && 0 == strcmp(argv[1], "reader")) {
        return run_reader(atoi(argv[2]), atoi(argv[3]));
    }
    if (3 < argc && 0 == strcmp(argv[1], "target")) {
        return run_target(atoi(argv[2]), atoi(argv[3]));
    }
    fprintf(stdout, "=== another job's revised data reaches a reader at its next get ===\n");
    /* a child stuck in a get fails the test rather than hanging it; the
     * children see their pipes close and exit */
    alarm(120);

    /* see the header: the property holds for a client that keeps the
     * job data it is sent */
    setenv("PMIX_MCA_gds", "hash", 1);
    errfd = capture_stderr(&log);
    if (0 > errfd) {
        report("the server's output can be captured", false);
        return 1;
    }

    if (0 != server_up()) {
        report("the server comes up", false);
        check_log(log, errfd);
        return 1;
    }
    snprintf(nodes, sizeof(nodes), "%s,%s", pmix_globals.hostname, FAKENODE);
    if (PMIX_SUCCESS != register_job(NS_A, 1, 1, pmix_globals.hostname, "0", false, 0) ||
        PMIX_SUCCESS != register_job(NS_B, NPROCS_B, NTARGETS, nodes, PPN_B, true, UNIV1)) {
        fprintf(stderr, "could not register the jobs\n");
        goto done;
    }

    for (i = 0; i < NTARGETS; i++) {
        targets[i] = start(argv[0], "target", NS_B, i, &tready[i], &tgo[i]);
        if (0 > targets[i] || 1 != read(tready[i], &c, 1)) {
            fprintf(stderr, "target %d did not commit\n", i);
            goto done;
        }
    }
    reader = start(argv[0], "reader", NS_A, 0, &rready, &rgo);
    if (0 > reader || 1 != read(rready, &c, 1)) {
        fprintf(stderr, "the reader never reported its first get\n");
        goto done;
    }
    report("the host revises the target job's data", PMIX_SUCCESS == revise(SECOND));
    c = 'g';
    if (1 != write(rgo, &c, 1) || 1 != read(rready, &c, 1)) {
        goto done;
    }
    /* C has no local clients, so no change notice reaches the reader */
    report("the host registers another job in the session, restating it",
           PMIX_SUCCESS == register_job(NS_C, 1, 0, FAKENODE, "0", false, UNIV2));
    c = 'g';
    if (1 != write(rgo, &c, 1)) {
        goto done;
    }
    waitpid(reader, &status, 0);
    reader = -1;
    report("the reader holds all of job B's data, revised, after its gets",
           WIFEXITED(status) && 0 == WEXITSTATUS(status));
    if (WIFEXITED(status) && 0 != WEXITSTATUS(status)) {
        fprintf(stdout, "        (reader exited %d)\n", WEXITSTATUS(status));
    }
    completed = true;

done:
    if (!completed) {
        report("the exchange ran to completion", false);
    }
    for (i = 0; i < NTARGETS; i++) {
        if (0 < targets[i]) {
            c = 'g';
            (void) !write(tgo[i], &c, 1);
            waitpid(targets[i], &status, 0);
        }
    }
    if (0 < reader) {
        kill(reader, SIGKILL);
        waitpid(reader, &status, 0);
    }
    PMIx_server_finalize();
    check_log(log, errfd);
    fprintf(stdout, "\n=== %d passed, %d failed ===\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
