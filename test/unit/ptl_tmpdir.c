/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * The paths a server works in - see src/mca/ptl/base/ptl_base_tmpdir.c.
 *
 *   - pmix_ptl_base_judge_dir, case by case: who owns a directory, who may
 *     write it, the sticky bit, a user private group, the tmpdir itself
 *   - a server whose tmpdir lies below a directory others may write,
 *     without the sticky bit, will not start; with the sticky bit it does;
 *     as root, one below another user's directory will not, and a tmpdir
 *     of another user's that only they can write is used (Slurm's shape)
 *
 * Each server runs in a child of its own.
 */

#include "src/include/pmix_config.h"

#include "include/pmix_server.h"
#include "src/include/pmix_globals.h"
#include "src/mca/ptl/base/base.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define SKIP 77

static int npass = 0;
static int nfail = 0;
static char base[512];
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

static void test_judge(void)
{
    uid_t me = 1000, other = 4242;

#define J(o, m, last, unm, upg) pmix_ptl_base_judge_dir((o), (m), (last), me, (unm), (upg))
    report("judge: a directory above, root's 0755", PMIX_PTL_DIR_OK == J(0, 0755, false, false, false),
           "refused");
    report("judge: ...our own 0700", PMIX_PTL_DIR_OK == J(me, 0700, false, false, false),
           "refused");
    report("judge: ...another user's 0755, refused",
           PMIX_PTL_DIR_OWNER == J(other, 0755, false, false, false), "taken");
    report("judge: ...one from outside our user namespace, as root's",
           PMIX_PTL_DIR_OK == J(65534, 0755, false, true, false), "refused");
    report("judge: ...root's 0777, refused", PMIX_PTL_DIR_WRITABLE == J(0, 0777, false, false, false),
           "taken");
    report("judge: ...root's 1777, as /tmp", PMIX_PTL_DIR_OK == J(0, 01777, false, false, false),
           "refused");
    report("judge: ...our own 0775, refused",
           PMIX_PTL_DIR_WRITABLE == J(me, 0775, false, false, false), "taken");
    report("judge: ...but with a private group", PMIX_PTL_DIR_OK == J(me, 0775, false, false, true),
           "refused");
    report("judge: ...which does not excuse others writing",
           PMIX_PTL_DIR_WRITABLE == J(me, 0777, false, false, true), "taken");
    report("judge: the tmpdir itself, another user's 0700 - Slurm's",
           PMIX_PTL_DIR_OK == J(other, 0700, true, false, false), "refused");
    report("judge: ...another user's 0770, refused",
           PMIX_PTL_DIR_WRITABLE == J(other, 0770, true, false, false), "taken");
    report("judge: ...another user's 1777, refused: the sticky bit does not make it theirs",
           PMIX_PTL_DIR_WRITABLE == J(other, 01777, true, false, false), "taken");
    report("judge: ...root's 1777, as /tmp itself", PMIX_PTL_DIR_OK == J(0, 01777, true, false, false),
           "refused");
    report("judge: ...root's 0777, refused - an emptyDir needs warn or silent",
           PMIX_PTL_DIR_WRITABLE == J(0, 0777, true, false, false), "taken");
#undef J
}

/* a server in a child with this session tmpdir and report file (NULL:
 * none) - PMIX_SUCCESS if its init succeeded */
static pmix_status_t server_with(const char *tmpdir, const char *repfile, mode_t umsk)
{
    int status;
    pid_t pid;

    fflush(stdout);
    pid = fork();
    if (0 == pid) {
        pmix_info_t info[3];
        size_t n = 0;
        pmix_status_t rc;

        umask(umsk);
        PMIX_INFO_LOAD(&info[n++], PMIX_SERVER_TMPDIR, tmpdir, PMIX_STRING);
        PMIX_INFO_LOAD(&info[n++], PMIX_SYSTEM_TMPDIR, tmpdir, PMIX_STRING);
        if (NULL != repfile) {
            PMIX_INFO_LOAD(&info[n++], PMIX_TCP_REPORT_URI, repfile, PMIX_STRING);
        }
        rc = PMIx_server_init(&mymodule, info, n);
        if (PMIX_SUCCESS == rc) {
            /* finalize removes a report file it wrote: keep it to look at */
            if (NULL != repfile) {
                char keep[1100];
                snprintf(keep, sizeof(keep), "%s.kept", repfile);
                if (0 != link(repfile, keep)) {
                    _exit(3);
                }
            }
            PMIx_server_finalize();
            _exit(0);
        }
        _exit(1);
    }
    if (0 > pid || pid != waitpid(pid, &status, 0)) {
        return PMIX_ERROR;
    }
    return (WIFEXITED(status) && 0 == WEXITSTATUS(status)) ? PMIX_SUCCESS : PMIX_ERROR;
}

static void test_paths(void)
{
    char parent[700], tmpdir[800];

    snprintf(parent, sizeof(parent), "%s/open", base);
    snprintf(tmpdir, sizeof(tmpdir), "%s/t", parent);
    if (0 != mkdir(parent, 0700) || 0 != mkdir(tmpdir, 0700)) {
        report("paths: fixture", 0, strerror(errno));
        return;
    }
    report("a server whose tmpdir is ours, below directories of ours, starts",
           PMIX_SUCCESS == server_with(tmpdir, NULL, 022), "refused");
    chmod(parent, 0777);
    report("one below a directory others may write, without the sticky bit, does not",
           PMIX_SUCCESS != server_with(tmpdir, NULL, 022), "started");
    chmod(parent, 01777);
    report("...and does, with the sticky bit", PMIX_SUCCESS == server_with(tmpdir, NULL, 022),
           "refused");
    chmod(parent, 0700);

    if (0 != geteuid()) {
        fprintf(stdout, "  SKIP: a directory above of another user's - not running as root\n");
        fprintf(stdout, "  SKIP: a tmpdir of another user's - not running as root\n");
        return;
    }
    /* root, as Slurm's server is: the parent is root's here */
    chmod(base, 0755);
    chmod(parent, 0755);
    if (0 != chown(parent, 4242, 4242)) {
        report("paths as root: fixture", 0, strerror(errno));
        return;
    }
    report("as root, one below a directory of another user's does not start",
           PMIX_SUCCESS != server_with(tmpdir, NULL, 022), "started");
    if (0 != chown(parent, 0, 0) || 0 != chown(tmpdir, 4242, 4242)) {
        report("paths as root: fixture", 0, strerror(errno));
        return;
    }
    report("...and one whose tmpdir is another user's, that only they can write, does",
           PMIX_SUCCESS == server_with(tmpdir, NULL, 022), "refused");
    if (0 != chown(tmpdir, 0, 0)) {
        fprintf(stderr, "could not restore the tmpdir's owner\n");
    }
}

int main(void)
{
    const char *tmp = getenv("TMPDIR");
    char cmd[600];

    fprintf(stdout, "\n=== a server's paths ===\n\n");
    snprintf(base, sizeof(base), "%s/ptltmp.XXXXXX", (NULL == tmp) ? "/tmp" : tmp);
    if (NULL == mkdtemp(base)) {
        fprintf(stdout, "no scratch directory - skipping\n");
        return SKIP;
    }
    if (60 < strlen(base)) {
        fprintf(stdout, "the scratch directory's path is too long for a socket - skipping\n");
        rmdir(base);
        return SKIP;
    }
    setenv("PMIX_MCA_mca_base_param_files", "none", 1);

    test_judge();
    test_paths();

    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", base);
    if (0 != system(cmd)) {
        fprintf(stderr, "could not remove %s\n", base);
    }
    fprintf(stdout, "\n%d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
