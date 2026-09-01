/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * A host revises a running job's data, and the clients see it.
 *
 * PMIx_server_register_nspace() called with a NEGATIVE nlocalprocs is
 * "this is an update to a namespace you already hold", and it is the
 * recommended way for a host to revise job-level data after the job is
 * running. It could not do that:
 *
 *   - the update branch handled PMIX_PROC_INFO_ARRAY,
 *     PMIX_GROUP_CONTEXT_ID and PMIX_JOB_INFO_ARRAY and had no arm for
 *     anything else, so a plain job-level key was silently dropped;
 *   - a PMIX_JOB_INFO_ARRAY was skipped outright once the namespace had
 *     job info, which for a registered namespace is always;
 *   - and the two arms that did store used pmix_globals.mypeer's module
 *     - the server's own hash - so nothing reached the segments a
 *     gds/shmem3 client reads, and no client was told anything.
 *
 * So the API existed, returned success, and did nothing observable.
 *
 * WHAT THIS ASSERTS, and why it needs PMIX_OPTIONAL.
 *
 * The read after the update asks with PMIX_OPTIONAL, which tells the
 * library not to go to the server. That is the whole point: a plain
 * PMIx_Get would miss locally, ask the server, and be answered out of
 * the server's own copy - so it would pass whether or not the update
 * ever reached this client's datastore, which is the thing being
 * tested.
 *
 * It runs against whichever gds module is assigned, and both have to
 * pass it: on Linux that is normally shmem3, which delivers the update
 * by publishing a new segment and telling the client to map it, and on
 * macOS it is hash, which sends the values themselves. The two answering
 * a host differently is the state this is here to prevent.
 *
 * The second phase asserts the other half of the rule: restating a value
 * that has not changed must be harmless. It cannot be observed directly
 * from a client - "nothing was published" leaves no trace by design - so
 * what is checked is that the value is still right afterwards, which is
 * what a caller would notice if a restatement had gone wrong.
 *
 * Later phases cover what the earlier ones did not: a payload too big
 * for an estimate made from the key count, a data array whose elements
 * are copied recursively, and a key whose TYPE changes - all of which
 * a host may legitimately send and none of which a scalar exercises.
 *
 * The third phase covers the OTHER path a host might revise job data by.
 * PMIx_server_register_resources() carries job-level information that
 * governs every namespace on the server, and its fan-out reaches the
 * ones already registered as well as the ones registered later. The man
 * page marks it permitted but not recommended, precisely because it
 * revises every job the server holds rather than the one the host meant
 * - but "not recommended" is not "does not work", and a path a host is
 * told it may take has to arrive. It goes through the same
 * PMIX_GDS_ADD_JOB_DATA the recommended path does, so what this pins is
 * that the fan-out into an already-registered namespace really does
 * reach a client that is already running, in both gds modules.
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

extern char **environ;

#define NSPACE  "job-upd"
#define LATEKEY "sut.late.key"
#define FIRST   11
#define SECOND  22
#define THIRD   33
#define FOURTH  44
/* A payload big enough that an estimate built from the KEY COUNT alone
 * cannot cover it. The segment estimate used to be all per-entry
 * constants while the values are copied into it twice, so a single
 * large value ran the bump allocator off the end - and the allocator
 * aborts rather than returning an error, taking the server and every
 * client on the node with it. Nothing caught that because every value
 * this test used was four bytes. */
#define BIGKEY  "sut.big.key"
#define BIGLEN  65536
#define ARRKEY  "sut.arr.key"
#define ARRN    32

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
/* the client                                                          */
/* ------------------------------------------------------------------ */

/* Read the key from THIS CLIENT'S OWN datastore.
 *
 * PMIX_OPTIONAL keeps the request in this process. Without it the
 * server answers and the test proves nothing. */
static int read_local(uint32_t *out)
{
    pmix_proc_t proc;
    pmix_value_t *val = NULL;
    pmix_info_t opt;
    pmix_status_t rc;

    PMIX_LOAD_PROCID(&proc, NSPACE, PMIX_RANK_WILDCARD);
    PMIX_INFO_LOAD(&opt, PMIX_OPTIONAL, NULL, PMIX_BOOL);
    rc = PMIx_Get(&proc, LATEKEY, &opt, 1, &val);
    PMIX_INFO_DESTRUCT(&opt);
    if (PMIX_SUCCESS != rc || NULL == val) {
        return -1;
    }
    rc = PMIx_Value_get_number(val, out, PMIX_UINT32);
    PMIX_VALUE_RELEASE(val);
    return (PMIX_SUCCESS == rc) ? 0 : -1;
}

/* Poll for the value to become "want".
 *
 * The pipe that releases the client and the socket the notice travels
 * on are different channels, so a single read would race the notice
 * rather than test it. A local read cannot fetch the answer by itself -
 * PMIX_OPTIONAL sees to that - so if the notice never arrives this
 * gives up and the case fails, which is the report it should make. */
static int await_local(uint32_t want, uint32_t *got)
{
    for (int i = 0; i < 200; i++) {
        if (0 == read_local(got) && want == *got) {
            return 0;
        }
        usleep(25000);
    }
    return -1;
}

static int run_client(int readyfd, int gofd)
{
    pmix_proc_t myproc;
    pmix_status_t rc;
    uint32_t v = 0;
    char c;

    rc = PMIx_Init(&myproc, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "  client: PMIx_Init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    if (0 != read_local(&v) || FIRST != v) {
        fprintf(stderr, "  client: expected %u before the update, got %u\n",
                (unsigned) FIRST, (unsigned) v);
        PMIx_Finalize(NULL, 0);
        return 1;
    }

    /* phase 1: the host changes the value */
    c = 'r';
    if (1 != write(readyfd, &c, 1) || 1 != read(gofd, &c, 1)) {
        PMIx_Finalize(NULL, 0);
        return 1;
    }
    if (0 != await_local(SECOND, &v)) {
        fprintf(stderr, "  client: never saw the updated value in its own "
                        "store (last read %u, wanted %u)\n",
                (unsigned) v, (unsigned) SECOND);
        PMIx_Finalize(NULL, 0);
        return 2;
    }
    fprintf(stdout, "  client: sees the revised value %u\n", (unsigned) v);

    /* phase 2: the host restates the SAME value */
    c = 'r';
    if (1 != write(readyfd, &c, 1) || 1 != read(gofd, &c, 1)) {
        PMIx_Finalize(NULL, 0);
        return 1;
    }
    usleep(300000);
    if (0 != read_local(&v) || SECOND != v) {
        fprintf(stderr, "  client: a restatement disturbed the value "
                        "(read %u, wanted %u)\n",
                (unsigned) v, (unsigned) SECOND);
        PMIx_Finalize(NULL, 0);
        return 3;
    }
    fprintf(stdout, "  client: value unchanged after a restatement\n");

    /* phase 3: the host revises it again through register_resources */
    c = 'r';
    if (1 != write(readyfd, &c, 1) || 1 != read(gofd, &c, 1)) {
        PMIx_Finalize(NULL, 0);
        return 1;
    }
    if (0 != await_local(THIRD, &v)) {
        fprintf(stderr, "  client: never saw the value revised by "
                        "register_resources (last read %u, wanted %u)\n",
                (unsigned) v, (unsigned) THIRD);
        PMIx_Finalize(NULL, 0);
        return 4;
    }
    fprintf(stdout, "  client: sees the value revised by register_resources "
                    "%u\n", (unsigned) v);

    /* phase 4: the host restates the WHOLE description, one value
     * changed. The change has to arrive, and the maps that rode along
     * with it must not have become job-level values. */
    c = 'r';
    if (1 != write(readyfd, &c, 1) || 1 != read(gofd, &c, 1)) {
        PMIx_Finalize(NULL, 0);
        return 1;
    }
    if (0 != await_local(FOURTH, &v)) {
        fprintf(stderr, "  client: never saw the value changed by a whole "
                        "description restatement (last read %u, wanted %u)\n",
                (unsigned) v, (unsigned) FOURTH);
        PMIx_Finalize(NULL, 0);
        return 5;
    }
    fprintf(stdout, "  client: sees the value changed by a restatement %u\n",
            (unsigned) v);
    {
        pmix_proc_t p;
        pmix_value_t *mv = NULL;
        pmix_info_t opt;
        pmix_status_t r;

        PMIX_LOAD_PROCID(&p, NSPACE, PMIX_RANK_WILDCARD);
        PMIX_INFO_LOAD(&opt, PMIX_OPTIONAL, NULL, PMIX_BOOL);
        r = PMIx_Get(&p, PMIX_NODE_MAP, &opt, 1, &mv);
        PMIX_INFO_DESTRUCT(&opt);
        if (PMIX_SUCCESS == r && NULL != mv) {
            PMIX_VALUE_RELEASE(mv);
            fprintf(stderr, "  client: the restated node map was filed as a "
                            "job-level value\n");
            PMIx_Finalize(NULL, 0);
            return 6;
        }
        fprintf(stdout, "  client: the restated node map was not filed as a "
                        "job-level value\n");
    }

    /* phase 5: a payload no key-count estimate covers */
    c = 'r';
    if (1 != write(readyfd, &c, 1) || 1 != read(gofd, &c, 1)) {
        PMIx_Finalize(NULL, 0);
        return 1;
    }
    {
        pmix_proc_t p;
        pmix_value_t *bv = NULL;
        pmix_info_t opt;
        int i;

        PMIX_LOAD_PROCID(&p, NSPACE, PMIX_RANK_WILDCARD);
        /* poll: the notice travels on a different channel from the pipe */
        for (i = 0; i < 200; i++) {
            PMIX_INFO_LOAD(&opt, PMIX_OPTIONAL, NULL, PMIX_BOOL);
            if (PMIX_SUCCESS == PMIx_Get(&p, BIGKEY, &opt, 1, &bv) &&
                NULL != bv) {
                PMIX_INFO_DESTRUCT(&opt);
                break;
            }
            PMIX_INFO_DESTRUCT(&opt);
            bv = NULL;
            usleep(25000);
        }
        if (NULL == bv) {
            fprintf(stderr, "  client: never saw the large value\n");
            PMIx_Finalize(NULL, 0);
            return 6;
        }
        if (PMIX_STRING != bv->type || NULL == bv->data.string ||
            BIGLEN != strlen(bv->data.string) ||
            'a' != bv->data.string[0] ||
            (char)('a' + ((BIGLEN - 1) % 26)) != bv->data.string[BIGLEN - 1]) {
            fprintf(stderr, "  client: the large value came back wrong "
                            "(len %zu, wanted %d)\n",
                    (NULL == bv->data.string) ? (size_t)0
                                              : strlen(bv->data.string),
                    BIGLEN);
            PMIX_VALUE_RELEASE(bv);
            PMIx_Finalize(NULL, 0);
            return 7;
        }
        PMIX_VALUE_RELEASE(bv);
        fprintf(stdout, "  client: reads the %d-byte value intact\n", BIGLEN);

        /* and the array that rode with it */
        bv = NULL;
        PMIX_INFO_LOAD(&opt, PMIX_OPTIONAL, NULL, PMIX_BOOL);
        if (PMIX_SUCCESS != PMIx_Get(&p, ARRKEY, &opt, 1, &bv) ||
            NULL == bv || PMIX_DATA_ARRAY != bv->type ||
            NULL == bv->data.darray || ARRN != bv->data.darray->size) {
            fprintf(stderr, "  client: the data array came back wrong\n");
            PMIX_INFO_DESTRUCT(&opt);
            if (NULL != bv) {
                PMIX_VALUE_RELEASE(bv);
            }
            PMIx_Finalize(NULL, 0);
            return 8;
        }
        PMIX_INFO_DESTRUCT(&opt);
        PMIX_VALUE_RELEASE(bv);
        fprintf(stdout, "  client: reads the %d-element array intact\n", ARRN);
    }

    /* phase 6: the same key, a different type */
    c = 'r';
    if (1 != write(readyfd, &c, 1) || 1 != read(gofd, &c, 1)) {
        PMIx_Finalize(NULL, 0);
        return 1;
    }
    {
        pmix_proc_t p;
        pmix_value_t *tv = NULL;
        pmix_info_t opt;
        int i;

        PMIX_LOAD_PROCID(&p, NSPACE, PMIX_RANK_WILDCARD);
        for (i = 0; i < 200; i++) {
            PMIX_INFO_LOAD(&opt, PMIX_OPTIONAL, NULL, PMIX_BOOL);
            if (PMIX_SUCCESS == PMIx_Get(&p, LATEKEY, &opt, 1, &tv) &&
                NULL != tv && PMIX_STRING == tv->type) {
                PMIX_INFO_DESTRUCT(&opt);
                break;
            }
            PMIX_INFO_DESTRUCT(&opt);
            if (NULL != tv) {
                PMIX_VALUE_RELEASE(tv);
                tv = NULL;
            }
            usleep(25000);
        }
        if (NULL == tv || PMIX_STRING != tv->type ||
            NULL == tv->data.string ||
            0 != strcmp(tv->data.string, "now-a-string")) {
            fprintf(stderr, "  client: the key did not change type - a "
                            "changed type has to count as a change\n");
            if (NULL != tv) {
                PMIX_VALUE_RELEASE(tv);
            }
            PMIx_Finalize(NULL, 0);
            return 9;
        }
        PMIX_VALUE_RELEASE(tv);
        fprintf(stdout, "  client: sees the key change type\n");
    }

    PMIx_Finalize(NULL, 0);
    return 0;
}

/* ------------------------------------------------------------------ */
/* the server                                                          */
/* ------------------------------------------------------------------ */

static volatile bool regdone = false;
static void regcbfunc(pmix_status_t status, void *cbdata)
{
    (void) status;
    (void) cbdata;
    regdone = true;
}

static pmix_server_module_t mymodule = {0};

static pmix_status_t register_job(void)
{
    pmix_info_t info[5];
    pmix_proc_t proc;
    pmix_nspace_t ns;
    pmix_status_t rc;
    char *noderegex = NULL, *ppnregex = NULL;
    uint32_t nprocs = 1, first = FIRST;
    int i;

    PMIx_generate_regex(pmix_globals.hostname, &noderegex);
    PMIx_generate_ppn("0", &ppnregex);
    PMIX_INFO_LOAD(&info[0], PMIX_NODE_MAP, noderegex, PMIX_REGEX);
    PMIX_INFO_LOAD(&info[1], PMIX_PROC_MAP, ppnregex, PMIX_REGEX);
    PMIX_INFO_LOAD(&info[2], PMIX_JOB_SIZE, &nprocs, PMIX_UINT32);
    PMIX_INFO_LOAD(&info[3], PMIX_MAX_PROCS, &nprocs, PMIX_UINT32);
    PMIX_INFO_LOAD(&info[4], LATEKEY, &first, PMIX_UINT32);

    PMIX_LOAD_NSPACE(ns, NSPACE);
    rc = PMIx_server_register_nspace(ns, 1, info, 5, NULL, NULL);
    for (i = 0; i < 5; i++) {
        PMIX_INFO_DESTRUCT(&info[i]);
    }
    free(noderegex);
    free(ppnregex);
    if (PMIX_OPERATION_SUCCEEDED == rc) {
        rc = PMIX_SUCCESS;
    }
    if (PMIX_SUCCESS != rc) {
        return rc;
    }

    PMIX_LOAD_PROCID(&proc, NSPACE, 0);
    rc = PMIx_server_register_client(&proc, geteuid(), getegid(), NULL,
                                     regcbfunc, NULL);
    if (PMIX_OPERATION_SUCCEEDED == rc) {
        return PMIX_SUCCESS;
    }
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    for (i = 0; i < 400 && !regdone; i++) {
        usleep(50000);
    }
    return PMIX_SUCCESS;
}

/* Restate the WHOLE job description, with one value changed in it.
 *
 * This is the second shape an update may take, and it is the one that
 * carries the entries a job description is mostly made of - the node
 * and proc maps. Those are not job-level values: the registration path
 * parses them into node and proc lists, so they never reach the table
 * the "has this changed?" test consults. A collector that treated them
 * as ordinary values would therefore find them changed on EVERY
 * restatement, store them where no reader of their realm will look,
 * and make gds/shmem3 publish a segment it can never reclaim - which
 * is the opposite of what PMIx_server_register_nspace(3) promises a
 * restatement costs.
 *
 * The client asserts both halves: the value that really changed
 * arrives, and the node map that rode along with it did not become a
 * job-level value.
 */
static pmix_status_t revise_whole_description(uint32_t value)
{
    pmix_info_t info[5];
    pmix_nspace_t ns;
    pmix_status_t rc;
    char *noderegex = NULL, *ppnregex = NULL;
    uint32_t nprocs = 1;
    int i;

    PMIx_generate_regex(pmix_globals.hostname, &noderegex);
    PMIx_generate_ppn("0", &ppnregex);
    PMIX_INFO_LOAD(&info[0], PMIX_NODE_MAP, noderegex, PMIX_REGEX);
    PMIX_INFO_LOAD(&info[1], PMIX_PROC_MAP, ppnregex, PMIX_REGEX);
    PMIX_INFO_LOAD(&info[2], PMIX_JOB_SIZE, &nprocs, PMIX_UINT32);
    PMIX_INFO_LOAD(&info[3], PMIX_MAX_PROCS, &nprocs, PMIX_UINT32);
    PMIX_INFO_LOAD(&info[4], LATEKEY, &value, PMIX_UINT32);

    PMIX_LOAD_NSPACE(ns, NSPACE);
    rc = PMIx_server_register_nspace(ns, -1, info, 5, NULL, NULL);
    for (i = 0; i < 5; i++) {
        PMIX_INFO_DESTRUCT(&info[i]);
    }
    free(noderegex);
    free(ppnregex);
    if (PMIX_OPERATION_SUCCEEDED == rc) {
        rc = PMIX_SUCCESS;
    }
    return rc;
}

/* The update itself: a NEGATIVE nlocalprocs says "revise what you hold
 * for this namespace" rather than "register it". */
static pmix_status_t revise(uint32_t value)
{
    pmix_info_t upd;
    pmix_nspace_t ns;
    pmix_status_t rc;

    PMIX_LOAD_NSPACE(ns, NSPACE);
    PMIX_INFO_LOAD(&upd, LATEKEY, &value, PMIX_UINT32);
    rc = PMIx_server_register_nspace(ns, -1, &upd, 1, NULL, NULL);
    PMIX_INFO_DESTRUCT(&upd);
    if (PMIX_OPERATION_SUCCEEDED == rc) {
        rc = PMIX_SUCCESS;
    }
    return rc;
}

/* The other way in. This one names no namespace: the values are held as
 * a cache that governs every job the server has, so the fan-out has to
 * carry them into the namespaces already registered. That is why it is
 * documented as permitted but not recommended - a host revising one
 * job's data this way revises all of them. */
static pmix_status_t revise_by_resources(uint32_t value)
{
    pmix_info_t upd;
    pmix_status_t rc;

    PMIX_INFO_LOAD(&upd, LATEKEY, &value, PMIX_UINT32);
    rc = PMIx_server_register_resources(&upd, 1, NULL, NULL);
    PMIX_INFO_DESTRUCT(&upd);
    if (PMIX_OPERATION_SUCCEEDED == rc) {
        rc = PMIX_SUCCESS;
    }
    return rc;
}

/* A payload no key-count estimate can cover: a long string and a data
 * array whose elements are copied one at a time. Both in ONE update, so
 * the segment has to be sized for their sum rather than for two keys. */
static pmix_status_t publish_big_values(void)
{
    pmix_info_t upd[2];
    pmix_data_array_t *darray = NULL;
    pmix_info_t *iptr;
    pmix_nspace_t ns;
    pmix_status_t rc;
    char *big;
    size_t n;

    big = (char *) malloc(BIGLEN + 1);
    if (NULL == big) {
        return PMIX_ERR_NOMEM;
    }
    /* a pattern, not zeros, so a truncated copy is visible */
    for (n = 0; n < BIGLEN; n++) {
        big[n] = (char) ('a' + (n % 26));
    }
    big[BIGLEN] = '\0';

    PMIX_DATA_ARRAY_CREATE(darray, ARRN, PMIX_INFO);
    if (NULL == darray) {
        free(big);
        return PMIX_ERR_NOMEM;
    }
    iptr = (pmix_info_t *) darray->array;
    for (n = 0; n < ARRN; n++) {
        char key[PMIX_MAX_KEYLEN + 1];
        uint32_t v = (uint32_t) n;
        snprintf(key, sizeof(key), "sut.arr.%zu", n);
        PMIX_INFO_LOAD(&iptr[n], key, &v, PMIX_UINT32);
    }

    PMIX_INFO_LOAD(&upd[0], BIGKEY, big, PMIX_STRING);
    PMIX_LOAD_KEY(upd[1].key, ARRKEY);
    upd[1].flags = 0;
    upd[1].value.type = PMIX_DATA_ARRAY;
    upd[1].value.data.darray = darray;

    PMIX_LOAD_NSPACE(ns, NSPACE);
    rc = PMIx_server_register_nspace(ns, -1, upd, 2, NULL, NULL);
    PMIX_INFO_DESTRUCT(&upd[0]);
    PMIX_INFO_DESTRUCT(&upd[1]);
    free(big);
    if (PMIX_OPERATION_SUCCEEDED == rc) {
        rc = PMIX_SUCCESS;
    }
    return rc;
}

/* The same key, a different TYPE. A host may do this, and "has this
 * changed?" has to say yes - PMIx_Value_compare() reports a type
 * mismatch as not-equal, which is what makes it so. */
static pmix_status_t retype_late_key(void)
{
    pmix_info_t upd;
    pmix_nspace_t ns;
    pmix_status_t rc;

    PMIX_LOAD_NSPACE(ns, NSPACE);
    PMIX_INFO_LOAD(&upd, LATEKEY, "now-a-string", PMIX_STRING);
    rc = PMIx_server_register_nspace(ns, -1, &upd, 1, NULL, NULL);
    PMIX_INFO_DESTRUCT(&upd);
    if (PMIX_OPERATION_SUCCEEDED == rc) {
        rc = PMIX_SUCCESS;
    }
    return rc;
}

int main(int argc, char **argv)
{
    pmix_info_t sinfo;
    pmix_proc_t proc;
    pmix_status_t rc;
    char **client_env = NULL;
    char *client_argv[5];
    char fdbuf[32];
    int readypipe[2], gopipe[2], status = 0;
    pid_t child;
    bool flag = true;
    char c;
    bool completed = false;

    setvbuf(stdout, NULL, _IOLBF, 0);

    if (1 < argc && 0 == strcmp(argv[1], "client")) {
        return run_client(atoi(argv[2]), atoi(argv[3]));
    }

    fprintf(stdout, "=== a job-data update reaches a running client ===\n");

    if (0 != pipe(readypipe) || 0 != pipe(gopipe)) {
        fprintf(stderr, "pipe() failed\n");
        return 1;
    }

    PMIX_INFO_LOAD(&sinfo, PMIX_SERVER_TOOL_SUPPORT, &flag, PMIX_BOOL);
    rc = PMIx_server_init(&mymodule, &sinfo, 1);
    PMIX_INFO_DESTRUCT(&sinfo);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    rc = register_job();
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "could not register the job: %s\n", PMIx_Error_string(rc));
        goto done;
    }

    PMIX_LOAD_PROCID(&proc, NSPACE, 0);
    /* Seed the child's environment with our own first - see the note in
     * event_forward.c: without it the child runs against the INSTALLED
     * PMIx rather than the one under test. */
    client_env = PMIx_Argv_copy(environ);
    rc = PMIx_server_setup_fork(&proc, &client_env);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_setup_fork failed: %s\n",
                PMIx_Error_string(rc));
        goto done;
    }

    child = fork();
    if (0 > child) {
        fprintf(stderr, "fork() failed\n");
        goto done;
    }
    if (0 == child) {
        /* exec rather than run in the fork: this process has an
         * initialized PMIx server in it */
        close(readypipe[0]);
        close(gopipe[1]);
        client_argv[0] = argv[0];
        client_argv[1] = (char *) "client";
        snprintf(fdbuf, sizeof(fdbuf), "%d", readypipe[1]);
        client_argv[2] = strdup(fdbuf);
        snprintf(fdbuf, sizeof(fdbuf), "%d", gopipe[0]);
        client_argv[3] = strdup(fdbuf);
        client_argv[4] = NULL;
        execve(argv[0], client_argv, client_env);
        fprintf(stderr, "exec of %s failed\n", argv[0]);
        _exit(127);
    }
    close(readypipe[1]);
    close(gopipe[0]);

    /* phase 1 */
    if (1 != read(readypipe[0], &c, 1)) {
        fprintf(stderr, "the client never reported its first read\n");
        goto done;
    }
    report("a running client reads the original job value", true);

    rc = revise(SECOND);
    report("the host revises it with register_nspace(nlocalprocs<0)",
           PMIX_SUCCESS == rc);

    c = 'g';
    if (1 != write(gopipe[1], &c, 1)) {
        goto done;
    }

    /* phase 2: restate the same value */
    if (1 != read(readypipe[0], &c, 1)) {
        fprintf(stderr, "the client never reported its second read\n");
        goto done;
    }
    rc = revise(SECOND);
    report("the host restates the same value", PMIX_SUCCESS == rc);
    c = 'g';
    if (1 != write(gopipe[1], &c, 1)) {
        goto done;
    }

    /* phase 3: revise it through register_resources instead */
    if (1 != read(readypipe[0], &c, 1)) {
        fprintf(stderr, "the client never reported its third read\n");
        goto done;
    }
    rc = revise_by_resources(THIRD);
    report("the host revises it with register_resources", PMIX_SUCCESS == rc);
    c = 'g';
    if (1 != write(gopipe[1], &c, 1)) {
        goto done;
    }

    /* phase 4: restate the whole description with one value changed */
    if (1 != read(readypipe[0], &c, 1)) {
        fprintf(stderr, "the client never reported its fourth read\n");
        goto done;
    }
    rc = revise_whole_description(FOURTH);
    report("the host restates the whole description with one value changed",
           PMIX_SUCCESS == rc);
    c = 'g';
    if (1 != write(gopipe[1], &c, 1)) {
        goto done;
    }

    /* phase 5: a payload no key-count estimate covers */
    if (1 != read(readypipe[0], &c, 1)) {
        fprintf(stderr, "the client never reported its fifth read\n");
        goto done;
    }
    rc = publish_big_values();
    report("the host publishes a large value and a data array",
           PMIX_SUCCESS == rc);
    c = 'g';
    if (1 != write(gopipe[1], &c, 1)) {
        goto done;
    }

    /* phase 6: the same key with a different type */
    if (1 != read(readypipe[0], &c, 1)) {
        fprintf(stderr, "the client never reported its sixth read\n");
        goto done;
    }
    rc = retype_late_key();
    report("the host changes an existing key's type", PMIX_SUCCESS == rc);
    c = 'g';
    if (1 != write(gopipe[1], &c, 1)) {
        goto done;
    }

    waitpid(child, &status, 0);
    if (WIFEXITED(status)) {
        report("the client's own datastore carries the revision",
               0 == WEXITSTATUS(status));
        if (0 != WEXITSTATUS(status)) {
            fprintf(stdout, "        (client exited %d)\n", WEXITSTATUS(status));
        }
    } else {
        report("the client's own datastore carries the revision", false);
        fprintf(stdout, "        (client died on a signal)\n");
    }

    completed = true;

done:
    if (!completed) {
        /* Every early exit above lands here, and most of them happen
         * BEFORE the case has reported anything - so without this the
         * program returns 0 having asserted nothing. That is not a
         * hypothetical: the client exiting at its first failed check
         * makes the parent's next read() fail, which jumps here, and
         * the run then printed the mismatch and still exited zero. A
         * test that cannot fail is worse than no test. */
        report("the exchange ran to completion", false);
    }
    if (NULL != client_env) {
        PMIx_Argv_free(client_env);
    }
    PMIx_server_finalize();
    fprintf(stdout, "\n=== %d passed, %d failed ===\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
