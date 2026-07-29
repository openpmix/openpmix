/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit tests for reclaiming a stale rendezvous file.
 *
 * A server taking the scheduler role drops a "pmix.sched.<host>" contact
 * file in the system tmpdir so others can find it, and removes it again
 * when it finalizes. A server that is killed or that crashes therefore
 * leaves its file behind - and the listener setup used to treat any
 * pre-existing file as fatal, so the next scheduler could not start until
 * someone deleted the orphan by hand. Worse, the failure surfaced as
 * "the listener thread failed to start", which points at sockets rather
 * than at a leftover file.
 *
 * The file records the pid of the process that wrote it, so a leftover
 * can be told from a file belonging to a running server. These tests pin
 * that boundary: a file whose owner is gone (or that carries no usable
 * pid at all, as happens when its creator died partway thru writing it)
 * must be reclaimed, while a file whose owner is still alive must be left
 * untouched and must still fail the init.
 *
 * Each case runs in a forked child because PMIx_server_init() can only be
 * meaningfully called once per process. The "crashed predecessor" case
 * relies on that too: the child _exit()s without finalizing, which is
 * precisely how a real orphan is created.
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "include/pmix_server.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* child exit codes */
#define CHILD_OK       0 /* init succeeded */
#define CHILD_INITFAIL 1 /* init failed */

#define SCHED_PREFIX "pmix.sched."

static int npass = 0;
static int nfail = 0;
static char tmpdir[PMIX_PATH_MAX];

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

/* return the path of the scheduler rendezvous file in our tmpdir, or
 * NULL if none is there. Caller must free the returned string */
static char *sched_file(void)
{
    DIR *dp;
    struct dirent *entry;
    char *path = NULL;

    dp = opendir(tmpdir);
    if (NULL == dp) {
        return NULL;
    }
    while (NULL != (entry = readdir(dp))) {
        if (0 == strncmp(entry->d_name, SCHED_PREFIX, strlen(SCHED_PREFIX))) {
            if (0 > asprintf(&path, "%s/%s", tmpdir, entry->d_name)) {
                path = NULL;
            }
            break;
        }
    }
    closedir(dp);
    return path;
}

/* drop a rendezvous file that claims to be owned by the given pid. The
 * layout matches what the listener writes: uri, version, pid, uid:gid,
 * and a timestamp, one per line */
static void plant_file(const char *path, pid_t owner, bool truncated)
{
    FILE *fp;

    fp = fopen(path, "w");
    if (NULL == fp) {
        return;
    }
    if (!truncated) {
        fprintf(fp, "testns.0;tcp4://127.0.0.1:1\n%s\n%lu\n%lu:%lu\nsome time\n", PMIX_VERSION,
                (unsigned long) owner, (unsigned long) getuid(), (unsigned long) getgid());
    } else {
        /* the creator died before it got as far as the pid */
        fprintf(fp, "testns.0;tcp4://127.0.0.1:1\n");
    }
    fclose(fp);
}

static bool file_exists(const char *path)
{
    struct stat buf;

    return (0 == stat(path, &buf));
}

/* attempt an init as the scheduler. If "clean" is false, we exit without
 * finalizing so our rendezvous file is left behind - i.e., we stand in
 * for a server that was killed */
static void child(bool clean)
{
    pmix_info_t info;
    pmix_status_t rc;

    PMIX_INFO_LOAD(&info, PMIX_SERVER_SCHEDULER, NULL, PMIX_BOOL);
    rc = PMIx_server_init(&mymodule, &info, 1);
    PMIX_INFO_DESTRUCT(&info);

    if (PMIX_SUCCESS != rc) {
        _exit(CHILD_INITFAIL);
    }
    if (!clean) {
        _exit(CHILD_OK);
    }
    PMIx_server_finalize();
    _exit(CHILD_OK);
}

/* fork a child to attempt the init and return its exit code, or -1 if
 * it did not exit normally */
static int run_child(bool clean)
{
    pid_t pid;
    int status;

    fflush(stdout);
    fflush(stderr);

    pid = fork();
    if (0 > pid) {
        return -1;
    }
    if (0 == pid) {
        child(clean);
        /* not reached */
        _exit(CHILD_INITFAIL);
    }
    if (pid != waitpid(pid, &status, 0)) {
        return -1;
    }
    if (!WIFEXITED(status)) {
        return -1;
    }
    return WEXITSTATUS(status);
}

/* remove everything we (or a server we started) left in the tmpdir */
static void cleanup(void)
{
    DIR *dp;
    struct dirent *entry;
    char *path;

    dp = opendir(tmpdir);
    if (NULL != dp) {
        while (NULL != (entry = readdir(dp))) {
            if (0 == strcmp(entry->d_name, ".") || 0 == strcmp(entry->d_name, "..")) {
                continue;
            }
            if (0 > asprintf(&path, "%s/%s", tmpdir, entry->d_name)) {
                continue;
            }
            if (0 != unlink(path)) {
                rmdir(path);
            }
            free(path);
        }
        closedir(dp);
    }
    rmdir(tmpdir);
}

int main(void)
{
    const char *base;
    char *path = NULL;
    int rc;

    fprintf(stdout, "Stale rendezvous file handling\n");

    /* work in a directory of our own so we cannot disturb - or be
     * disturbed by - anything else on this node */
    base = getenv("TMPDIR");
    if (NULL == base) {
        base = "/tmp";
    }
    snprintf(tmpdir, sizeof(tmpdir), "%s/pmix-rndz-XXXXXX", base);
    if (NULL == mkdtemp(tmpdir)) {
        fprintf(stderr, "could not create a temporary directory\n");
        return 1;
    }
    setenv("PMIX_SYSTEM_TMPDIR", tmpdir, 1);
    setenv("PMIX_SERVER_TMPDIR", tmpdir, 1);

    /* a scheduler must be able to start in a clean directory, and must
     * leave its rendezvous file behind if it does not finalize */
    rc = run_child(false);
    if (CHILD_OK != rc) {
        report("scheduler starts in a clean tmpdir", 0, "init failed");
        cleanup();
        fprintf(stdout, "\n%d passed, %d failed\n", npass, nfail);
        return 1;
    }
    report("scheduler starts in a clean tmpdir", 1, NULL);

    path = sched_file();
    if (NULL == path) {
        report("killed scheduler leaves its file behind", 0, "no rendezvous file found");
        cleanup();
        fprintf(stdout, "\n%d passed, %d failed\n", npass, nfail);
        return 1;
    }
    report("killed scheduler leaves its file behind", 1, NULL);

    /* the file names a pid that is now gone, so the next scheduler must
     * reclaim it instead of refusing to start */
    rc = run_child(true);
    report("stale file is reclaimed", CHILD_OK == rc, "init failed");
    /* and having created the file itself, that scheduler must have
     * removed it when it finalized */
    report("reclaimed file is removed at finalize", !file_exists(path), "file still present");

    /* a file naming a live process belongs to somebody else - refuse to
     * start, and leave the file alone. Our own pid serves as the live
     * owner: the child that reads it runs under a different pid, so it
     * has no reason to believe the file is its own */
    plant_file(path, getpid(), false);
    rc = run_child(true);
    report("live owner is not disturbed", CHILD_INITFAIL == rc, "init should have failed");
    report("live owner's file is preserved", file_exists(path), "file was removed");
    unlink(path);

    /* a file that was never completely written names no owner at all,
     * so it too must be reclaimed */
    plant_file(path, 0, true);
    rc = run_child(true);
    report("truncated file is reclaimed", CHILD_OK == rc, "init failed");

    /* likewise a file whose pid line is unusable */
    plant_file(path, 0, false);
    rc = run_child(true);
    report("file with no usable pid is reclaimed", CHILD_OK == rc, "init failed");

    free(path);
    cleanup();

    fprintf(stdout, "\n%d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
