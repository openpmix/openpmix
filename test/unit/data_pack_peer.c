/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit test for a server's PMIx_Data_pack/PMIx_Data_unpack aimed at a
 * namespace that no longer has a connected peer
 * (src/common/pmix_data.c:_findpeer).
 *
 * A server packing for a target first looks for a connected client of the
 * target's namespace and borrows its wire format. When there is none - the
 * ordinary state once a job's local processes have finalized and the host
 * has deregistered them - it falls back on the PMIX_BFROPS_MODULE value
 * the connection handler recorded for that namespace when its first peer
 * connected, and hands it to pmix_bfrops_base_assign_module().
 *
 * That function matches a bfrops *component name* ("v61"). The PTL
 * refactor had the handler record the peer's *library release* ("7.0.0")
 * instead, so nothing ever matched, and every such pack or unpack failed -
 * reported, misleadingly, as PMIX_ERR_NOMEM.
 *
 * The case below builds exactly that state: register a one-process job,
 * exec a real client that connects and finalizes, deregister it, and then
 * round-trip a value through a buffer aimed at a proc of that job.
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

#define DPP_NSPACE "data-pack-peer-job"
#define DPP_VALUE  0x5a5a1234u

static int npass = 0;
static int nfail = 0;

static pmix_server_module_t mymodule = {0};

static void report(const char *name, int passed)
{
    if (passed) {
        fprintf(stdout, "  PASS: %s\n", name);
        ++npass;
    } else {
        fprintf(stdout, "  FAIL: %s\n", name);
        ++nfail;
    }
}

static volatile bool regdone = false;

static void regcbfunc(pmix_status_t status, void *cbdata)
{
    (void) status;
    (void) cbdata;
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

    PMIX_LOAD_NSPACE(ns, DPP_NSPACE);
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

    PMIX_LOAD_PROCID(&p0, DPP_NSPACE, 0);
    rc = PMIx_server_register_client(&p0, geteuid(), getegid(), NULL, regcbfunc, NULL);
    if (PMIX_OPERATION_SUCCEEDED == rc) {
        return PMIX_SUCCESS;
    }
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    /* do not fork the client before it is registered */
    for (n = 0; n < 400 && !regdone; n++) {
        usleep(50000);
    }
    return PMIX_SUCCESS;
}

/* the client only has to connect - that is what records the namespace's
 * wire format - and leave */
static int run_client(void)
{
    pmix_proc_t me;
    pmix_status_t rc;

    rc = PMIx_Init(&me, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "client PMIx_Init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }
    rc = PMIx_Finalize(NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "client PMIx_Finalize failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }
    return 0;
}

static int connect_client(const char *self)
{
    char **client_env = NULL;
    char *client_argv[3];
    pmix_proc_t p0;
    pmix_status_t rc;
    pid_t child;
    int status = 1;

    /* exec rather than run in the fork, and seed the environment with our
     * own first - see the matching comment in get_api.c */
    PMIX_LOAD_PROCID(&p0, DPP_NSPACE, 0);
    client_env = PMIx_Argv_copy(environ);
    rc = PMIx_server_setup_fork(&p0, &client_env);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_setup_fork failed: %s\n", PMIx_Error_string(rc));
        PMIx_Argv_free(client_env);
        return 1;
    }
    child = fork();
    if (0 > child) {
        fprintf(stderr, "fork() failed\n");
    } else if (0 == child) {
        client_argv[0] = (char *) self;
        client_argv[1] = (char *) "client";
        client_argv[2] = NULL;
        execve(self, client_argv, client_env);
        fprintf(stderr, "exec of %s failed\n", self);
        _exit(127);
    } else {
        waitpid(child, &status, 0);
        status = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    }
    PMIx_Argv_free(client_env);
    return status;
}

static void check_round_trip(void)
{
    pmix_data_buffer_t buf;
    pmix_proc_t target;
    pmix_status_t rc;
    uint32_t in = DPP_VALUE, out = 0;
    int32_t cnt = 1;

    PMIX_LOAD_PROCID(&target, DPP_NSPACE, 0);
    PMIx_Data_buffer_construct(&buf);

    rc = PMIx_Data_pack(&target, &buf, &in, 1, PMIX_UINT32);
    if (PMIX_SUCCESS != rc) {
        fprintf(stdout, "    PMIx_Data_pack returned %s\n", PMIx_Error_string(rc));
    }
    report("pack for a namespace with no connected peer", PMIX_SUCCESS == rc);

    if (PMIX_SUCCESS == rc) {
        rc = PMIx_Data_unpack(&target, &buf, &out, &cnt, PMIX_UINT32);
        if (PMIX_SUCCESS != rc) {
            fprintf(stdout, "    PMIx_Data_unpack returned %s\n", PMIx_Error_string(rc));
        }
        report("unpack for the same namespace returns the value",
               PMIX_SUCCESS == rc && 1 == cnt && DPP_VALUE == out);
    }
    PMIx_Data_buffer_destruct(&buf);
}

int main(int argc, char **argv)
{
    pmix_status_t rc;
    pmix_proc_t p0;

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    /* re-executed as our own client */
    if (1 < argc && 0 == strcmp(argv[1], "client")) {
        return run_client();
    }

    rc = PMIx_server_init(&mymodule, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    rc = register_job();
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "register_job failed: %s\n", PMIx_Error_string(rc));
        PMIx_server_finalize();
        return 1;
    }

    fprintf(stdout, "PMIx_Data_pack/unpack for a departed namespace:\n");

    report("client connects and finalizes", 0 == connect_client(argv[0]));

    /* the host retiring the finished process takes it out of the clients
     * array, leaving only the recorded wire format to go on */
    PMIX_LOAD_PROCID(&p0, DPP_NSPACE, 0);
    PMIx_server_deregister_client(&p0, NULL, NULL);

    check_round_trip();

    PMIx_server_finalize();

    fprintf(stdout, "\n%d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
