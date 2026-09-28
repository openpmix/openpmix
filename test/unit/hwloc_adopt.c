/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * What a client will adopt as a shared-memory topology.
 *
 * A server can name a file holding its topology in shared memory
 * (PMIX_HWLOC_SHMEM_FILE, with the address and size to map it at), and a
 * client adopts it during PMIx_Init rather than discovering its own. The
 * client opens it without blocking and without following a symlink at
 * the name, and adopts it only if it is a regular file belonging to the
 * client's user or to root that holds at least the size it was given.
 * Anything else is passed over and the client discovers the topology
 * itself.
 *
 * The server here registers a job whose data names such a file, forks a
 * client into it, and requires the client's PMIx_Init to complete. A
 * client stuck opening a FIFO is killed after a timeout and fails.
 */

#include "src/include/pmix_config.h"
#include "include/pmix.h"
#include "include/pmix_server.h"
#include "src/include/pmix_globals.h"
#include "src/util/pmix_argv.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define ADOPT_NSPACE "hwloc-adopt"

extern char **environ;

static pmix_server_module_t mymodule = {0};

static int npass = 0;
static int nfail = 0;

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

static volatile bool regdone = false;

static void regcbfunc(pmix_status_t status, void *cbdata)
{
    (void) status;
    (void) cbdata;
    regdone = true;
}

/* a job of one process whose data names "file" as the topology segment */
static pmix_status_t register_job(const char *nspace, const char *file)
{
    pmix_info_t info[5];
    pmix_nspace_t ns;
    pmix_proc_t p0;
    pmix_status_t rc;
    char *noderegex = NULL, *ppnregex = NULL;
    size_t addr = 0x10000000, size = 1 << 20;
    int n;

    PMIx_generate_regex(pmix_globals.hostname, &noderegex);
    PMIx_generate_ppn("0", &ppnregex);

    PMIX_INFO_LOAD(&info[0], PMIX_NODE_MAP, noderegex, PMIX_REGEX);
    PMIX_INFO_LOAD(&info[1], PMIX_PROC_MAP, ppnregex, PMIX_REGEX);
    PMIX_INFO_LOAD(&info[2], PMIX_HWLOC_SHMEM_FILE, file, PMIX_STRING);
    PMIX_INFO_LOAD(&info[3], PMIX_HWLOC_SHMEM_ADDR, &addr, PMIX_SIZE);
    PMIX_INFO_LOAD(&info[4], PMIX_HWLOC_SHMEM_SIZE, &size, PMIX_SIZE);

    PMIX_LOAD_NSPACE(ns, nspace);
    rc = PMIx_server_register_nspace(ns, 1, info, 5, NULL, NULL);
    if (PMIX_OPERATION_SUCCEEDED == rc) {
        rc = PMIX_SUCCESS;
    }
    for (n = 0; n < 5; n++) {
        PMIX_INFO_DESTRUCT(&info[n]);
    }
    free(noderegex);
    free(ppnregex);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }

    regdone = false;
    PMIX_LOAD_PROCID(&p0, nspace, 0);
    rc = PMIx_server_register_client(&p0, geteuid(), getegid(), NULL, regcbfunc, NULL);
    if (PMIX_OPERATION_SUCCEEDED == rc) {
        return PMIX_SUCCESS;
    }
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    for (n = 0; n < 400 && !regdone; n++) {
        usleep(50000);
    }
    return PMIX_SUCCESS;
}

static int run_client(void)
{
    pmix_proc_t me;

    if (PMIX_SUCCESS != PMIx_Init(&me, NULL, 0)) {
        return 1;
    }
    return (PMIX_SUCCESS == PMIx_Finalize(NULL, 0)) ? 0 : 1;
}

/* start the client of "nspace" and say how it ended: its exit status,
 * -1 if it could not be started, -2 if it never finished */
static int connect_client(const char *self, const char *nspace)
{
    char **client_env = NULL;
    char *client_argv[3];
    pmix_proc_t p0;
    pid_t child, r;
    int status = 0, i;

    PMIX_LOAD_PROCID(&p0, nspace, 0);
    client_env = PMIx_Argv_copy(environ);
    if (PMIX_SUCCESS != PMIx_server_setup_fork(&p0, &client_env)) {
        PMIx_Argv_free(client_env);
        return -1;
    }
    child = fork();
    if (0 > child) {
        PMIx_Argv_free(client_env);
        return -1;
    }
    if (0 == child) {
        client_argv[0] = (char *) self;
        client_argv[1] = (char *) "client";
        client_argv[2] = NULL;
        execve(self, client_argv, client_env);
        _exit(127);
    }
    PMIx_Argv_free(client_env);
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
        return -2;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static void check(const char *self, const char *name, const char *nspace, const char *file)
{
    pmix_proc_t p0;
    int got;

    if (PMIX_SUCCESS != register_job(nspace, file)) {
        report(name, 0, "could not register the job");
        return;
    }
    got = connect_client(self, nspace);
    if (-2 == got) {
        report(name, 0, "the client's PMIx_Init never finished");
    } else {
        report(name, 0 == got, "the client did not initialize");
    }
    PMIX_LOAD_PROCID(&p0, nspace, 0);
    PMIx_server_deregister_client(&p0, NULL, NULL);
}

int main(int argc, char **argv)
{
    char tmpl[] = "/tmp/pmix-hwloc-adopt-XXXXXX";
    char fifo[1100], link[1100], shortfile[1100], *dir;
    FILE *fp;

    setvbuf(stdout, NULL, _IONBF, 0);

    /* re-executed as our own client */
    if (1 < argc && 0 == strcmp(argv[1], "client")) {
        return run_client();
    }

    fprintf(stdout, "\n=== client adoption of a shared-memory topology ===\n\n");
    dir = mkdtemp(tmpl);
    if (NULL == dir) {
        fprintf(stdout, "  FAIL: could not make a scratch directory\n");
        return 1;
    }
    snprintf(fifo, sizeof(fifo), "%s/fifo", dir);
    snprintf(link, sizeof(link), "%s/link", dir);
    snprintf(shortfile, sizeof(shortfile), "%s/short", dir);
    if (0 != mkfifo(fifo, 0600) || 0 != symlink(fifo, link)) {
        fprintf(stdout, "  FAIL: could not make the scratch files\n");
        return 1;
    }
    fp = fopen(shortfile, "w");
    if (NULL != fp) {
        fprintf(fp, "not a topology\n");
        fclose(fp);
    }

    if (PMIX_SUCCESS != PMIx_server_init(&mymodule, NULL, 0)) {
        fprintf(stdout, "  FAIL: PMIx_server_init\n");
        return 1;
    }

    check(argv[0], "a FIFO named as the segment is passed over", ADOPT_NSPACE "-fifo", fifo);
    check(argv[0], "a symlink named as the segment is passed over", ADOPT_NSPACE "-link", link);
    check(argv[0], "a file shorter than the segment is passed over", ADOPT_NSPACE "-short",
          shortfile);

    PMIx_server_finalize();

    unlink(link);
    unlink(fifo);
    unlink(shortfile);
    rmdir(dir);

    fprintf(stdout, "\n%d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
