/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * Which user param files the ompi pmdl component reads.
 *
 * When a job is set up for Open MPI, pmdl/ompi reads the user's
 * ~/.openmpi/mca-params.conf and hands every value in it to the job's
 * environment. It reads the file only if it is a regular file belonging
 * to that user - following a symlink to get there - and checks the file
 * it actually parses rather than the name.
 *
 * The component parses the file once per user per process, so each case
 * runs in a child of its own, with $HOME pointed at a scratch directory
 * holding the case's file. A case that blocks - a FIFO at the name - is
 * killed after a timeout and fails.
 */

#include "src/include/pmix_config.h"
#include "include/pmix.h"
#include "include/pmix_server.h"

#include "src/class/pmix_list.h"
#include "src/include/pmix_globals.h"
#include "src/mca/pmdl/base/base.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define MARKER "OMPI_MCA_pmdlut_marker"

/* how a child reports */
#define C_READ    0  /* the file's value reached the environment */
#define C_SETUP   1
#define C_SKIPPED 3  /* it did not */

typedef enum {
    CASE_REGULAR,
    CASE_SYMLINK,
    CASE_FIFO,
    CASE_FOREIGN
} pfcase_t;

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
    fflush(stdout);
}

static bool write_params(const char *path)
{
    FILE *fp = fopen(path, "w");

    if (NULL == fp) {
        return false;
    }
    fprintf(fp, "pmdlut_marker = yes\n");
    fclose(fp);
    return true;
}

/* child: harvest the job environment and look for the file's value */
static int harvest(const char *home)
{
    pmix_info_t info[2];
    pmix_list_t ilist;
    pmix_kval_t *kv;
    pmix_status_t rc;
    bool found = false, yes = true;

    setenv("HOME", home, 1);
    unsetenv("OMPIHOME");
    if (PMIX_SUCCESS != PMIx_server_init(NULL, NULL, 0)) {
        return C_SETUP;
    }
    PMIX_INFO_LOAD(&info[0], PMIX_PROGRAMMING_MODEL, "ompi", PMIX_STRING);
    PMIX_INFO_LOAD(&info[1], PMIX_SETUP_APP_ENVARS, &yes, PMIX_BOOL);
    PMIX_CONSTRUCT(&ilist, pmix_list_t);
    rc = pmix_pmdl.harvest_envars("pmdl-paramfile", info, 2, &ilist);
    PMIX_INFO_DESTRUCT(&info[0]);
    PMIX_INFO_DESTRUCT(&info[1]);
    if (PMIX_SUCCESS != rc && PMIX_ERR_TAKE_NEXT_OPTION != rc) {
        PMIX_LIST_DESTRUCT(&ilist);
        PMIx_server_finalize();
        return C_SETUP;
    }
    PMIX_LIST_FOREACH (kv, &ilist, pmix_kval_t) {
        if (NULL != kv->value && PMIX_ENVAR == kv->value->type &&
            NULL != kv->value->data.envar.envar &&
            0 == strcmp(kv->value->data.envar.envar, MARKER)) {
            found = true;
        }
    }
    PMIX_LIST_DESTRUCT(&ilist);
    PMIx_server_finalize();
    return found ? C_READ : C_SKIPPED;
}

/* run one case in a child and return its report, or -1 */
static int run_case(pfcase_t which, char *home)
{
    char dir[1100], file[1200], real[1200];
    pid_t pid, r;
    int status = 0, i;

    snprintf(dir, sizeof(dir), "%s/.openmpi", home);
    snprintf(file, sizeof(file), "%s/mca-params.conf", dir);
    snprintf(real, sizeof(real), "%s/real.conf", home);
    if (0 != mkdir(dir, 0700) && EEXIST != errno) {
        return -1;
    }
    unlink(file);
    switch (which) {
    case CASE_REGULAR:
        if (!write_params(file)) {
            return -1;
        }
        break;
    case CASE_SYMLINK:
        if (!write_params(real) || 0 != symlink(real, file)) {
            return -1;
        }
        break;
    case CASE_FIFO:
        if (0 != mkfifo(file, 0600)) {
            return -1;
        }
        break;
    case CASE_FOREIGN:
        if (!write_params(file) || 0 != chown(file, 65534, 65534)) {
            return -1;
        }
        break;
    }

    pid = fork();
    if (0 > pid) {
        return -1;
    }
    if (0 == pid) {
        _exit(harvest(home));
    }
    for (i = 0; i < 300; i++) {
        r = waitpid(pid, &status, WNOHANG);
        if (pid == r) {
            break;
        }
        usleep(100000);
    }
    unlink(file);
    unlink(real);
    if (300 == i) {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        return -2;
    }
    if (!WIFEXITED(status)) {
        return -1;
    }
    return WEXITSTATUS(status);
}

static void check(const char *name, pfcase_t which, char *home, int want)
{
    int got = run_case(which, home);
    char detail[64];

    if (-2 == got) {
        report(name, 0, "the harvest never finished");
    } else if (0 > got || C_SETUP == got) {
        report(name, 0, "the case could not be set up");
    } else {
        snprintf(detail, sizeof(detail), "the value was %s",
                 (C_READ == got) ? "read" : "not read");
        report(name, want == got, detail);
    }
}

int main(int argc, char **argv)
{
    char tmpl[] = "/tmp/pmix-pmdl-paramfile-XXXXXX";
    char dir[1100], *home;

    (void) argc;
    (void) argv;

    fprintf(stdout, "\n=== pmdl/ompi user param file ===\n\n");
    home = mkdtemp(tmpl);
    if (NULL == home) {
        fprintf(stdout, "  FAIL: could not make a scratch home\n");
        return 1;
    }

    check("a regular file of the user's is read", CASE_REGULAR, home, C_READ);
    check("a symlink to a file of the user's is read", CASE_SYMLINK, home, C_READ);
    check("a FIFO at the name is skipped without blocking", CASE_FIFO, home, C_SKIPPED);
    if (0 == geteuid()) {
        check("a file belonging to another user is skipped", CASE_FOREIGN, home, C_SKIPPED);
    } else {
        fprintf(stdout, "  SKIP: a file belonging to another user (needs root)\n");
    }

    snprintf(dir, sizeof(dir), "%s/.openmpi", home);
    rmdir(dir);
    rmdir(home);

    fprintf(stdout, "\n%d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
