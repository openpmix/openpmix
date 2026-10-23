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
 * untouched and must still fail the init. A file whose writer holds the
 * lock beside it is in use whatever its pid says.
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
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* child exit codes */
#define CHILD_OK       0 /* init succeeded */
#define CHILD_INITFAIL 1 /* init failed */
#define CHILD_NOTMADE  2 /* init succeeded, but not everything was made */

#define SCHED_PREFIX "pmix.sched."

static int npass = 0;
static int nfail = 0;
static char tmpdir[PMIX_PATH_MAX];
static char moveddir[PMIX_PATH_MAX + 8];
/* the name of the rendezvous file within its directory, once known */
static const char *schedname = NULL;
/* the cases in which the server makes its directories itself: 1 moves
 * them all away, 2 puts another directory at the system tmpdir's name */
static int made_case = 0;

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

/* the same layout, with the pid line given verbatim */
static void plant_file_pid(const char *path, const char *pidline)
{
    FILE *fp;

    fp = fopen(path, "w");
    if (NULL == fp) {
        return;
    }
    fprintf(fp, "testns.0;tcp4://127.0.0.1:1\n%s\n%s\n%lu:%lu\nsome time\n", PMIX_VERSION,
            pidline, (unsigned long) getuid(), (unsigned long) getgid());
    fclose(fp);
}

/* a pid no process has: a child that has exited and been reaped */
static pid_t reaped(void)
{
    pid_t pid = fork();
    int st;

    if (0 == pid) {
        _exit(0);
    }
    if (0 < pid) {
        (void) waitpid(pid, &st, 0);
    }
    return pid;
}

static bool file_exists(const char *path)
{
    struct stat buf;

    return (0 == stat(path, &buf));
}

/* the names in a directory, but for those listed in keep, joined by
 * spaces - or NULL if there are none. A directory that is not there says
 * so. Caller must free the result */
static char *leftovers(const char *dir, const char *const keep[])
{
    DIR *dp;
    struct dirent *entry;
    char *list = NULL, *tmp;
    size_t n;
    bool kept;

    dp = opendir(dir);
    if (NULL == dp) {
        return strdup("(the directory is gone)");
    }
    while (NULL != (entry = readdir(dp))) {
        if (0 == strcmp(entry->d_name, ".") || 0 == strcmp(entry->d_name, "..")) {
            continue;
        }
        kept = false;
        for (n = 0; NULL != keep && NULL != keep[n]; n++) {
            if (0 == strcmp(entry->d_name, keep[n])) {
                kept = true;
            }
        }
        if (kept) {
            continue;
        }
        if (0 > asprintf(&tmp, "%s%s%s", (NULL == list) ? "" : list,
                         (NULL == list) ? "" : " ", entry->d_name)) {
            break;
        }
        free(list);
        list = tmp;
    }
    closedir(dp);
    return list;
}

/* whether a directory holds an entry whose name starts with prefix */
static bool has_entry(const char *dir, const char *prefix)
{
    DIR *dp;
    struct dirent *entry;
    bool found = false;

    dp = opendir(dir);
    if (NULL == dp) {
        return false;
    }
    while (!found && NULL != (entry = readdir(dp))) {
        found = (0 == strncmp(entry->d_name, prefix, strlen(prefix)));
    }
    closedir(dp);
    return found;
}

/* write a file holding a line, so it is not empty */
static void plant_line(const char *dir, const char *name)
{
    char path[PMIX_PATH_MAX * 2];
    FILE *fp;

    snprintf(path, sizeof(path), "%s/%s", dir, name);
    fp = fopen(path, "w");
    if (NULL != fp) {
        fprintf(fp, "not the server's\n");
        fclose(fp);
    }
}

/* attempt an init as the scheduler. If "clean" is false, we exit without
 * finalizing so our rendezvous file is left behind - i.e., we stand in
 * for a server that was killed */
static void child(bool clean)
{
    pmix_info_t info[2];
    size_t ninfo = 1;
    pmix_status_t rc;

    /* a case that stalls - e.g., on opening a FIFO - fails rather
     * than hanging the test suite */
    alarm(60);
    PMIX_INFO_LOAD(&info[0], PMIX_SERVER_SCHEDULER, NULL, PMIX_BOOL);
    if (NULL != schedname) {
        /* so the server writes hwloc.sm too, where the platform can */
        PMIX_INFO_LOAD(&info[1], PMIX_SERVER_SHARE_TOPOLOGY, NULL, PMIX_BOOL);
        ninfo = 2;
    }
    rc = PMIx_server_init(&mymodule, info, ninfo);
    PMIX_INFO_DESTRUCT(&info[0]);
    if (2 == ninfo) {
        PMIX_INFO_DESTRUCT(&info[1]);
    }

    if (PMIX_SUCCESS != rc) {
        _exit(CHILD_INITFAIL);
    }
    if (!clean) {
        _exit(CHILD_OK);
    }
    if (2 == made_case) {
        char dir[PMIX_PATH_MAX * 2], old[PMIX_PATH_MAX * 2];

        snprintf(dir, sizeof(dir), "%s/my", tmpdir);
        snprintf(old, sizeof(old), "%s/my.old", tmpdir);
        if (0 != rename(dir, old) || 0 != mkdir(dir, 0700)) {
            _exit(CHILD_INITFAIL);
        }
        plant_line(dir, "decoy");
    } else if (1 == made_case) {
        char dir[PMIX_PATH_MAX * 2];

        /* everything the case is about was made: the socket's
         * directory, the report file, the rendezvous file and the
         * system one */
        snprintf(dir, sizeof(dir), "%s/uri", tmpdir);
        if (!has_entry(tmpdir, "pmix-") || !file_exists(dir)) {
            _exit(CHILD_NOTMADE);
        }
        snprintf(dir, sizeof(dir), "%s/mr/rndz", tmpdir);
        if (!file_exists(dir)) {
            _exit(CHILD_NOTMADE);
        }
        snprintf(dir, sizeof(dir), "%s/my", tmpdir);
        if (!has_entry(dir, SCHED_PREFIX)) {
            _exit(CHILD_NOTMADE);
        }
        /* another server's file in the rendezvous directory this one
         * made. Then the whole tree moves away */
        snprintf(dir, sizeof(dir), "%s/mr", tmpdir);
        plant_line(dir, "other");
        if (0 != rename(tmpdir, moveddir)) {
            _exit(CHILD_INITFAIL);
        }
    }
    if (NULL != schedname) {
        /* move the directory away and put a decoy at the old name, so
         * finalize can only get the right file by removing it from the
         * directory it was created in */
        char decoy[PMIX_PATH_MAX * 2];
        FILE *fp;
        bool had_hwloc;

        /* the server's shmem topology, where this platform makes one, is
         * in the same directory and is owed the same treatment */
        snprintf(decoy, sizeof(decoy), "%s/hwloc.sm", tmpdir);
        had_hwloc = file_exists(decoy);
        if (0 != rename(tmpdir, moveddir) || 0 != mkdir(tmpdir, 0700)) {
            _exit(CHILD_INITFAIL);
        }
        snprintf(decoy, sizeof(decoy), "%s/%s", tmpdir, schedname);
        fp = fopen(decoy, "w");
        if (NULL != fp) {
            fprintf(fp, "decoy\n");
            fclose(fp);
        }
        if (had_hwloc) {
            snprintf(decoy, sizeof(decoy), "%s/hwloc.sm", tmpdir);
            fp = fopen(decoy, "w");
            if (NULL != fp) {
                fprintf(fp, "decoy\n");
                fclose(fp);
            }
            snprintf(decoy, sizeof(decoy), "%s/had-hwloc", moveddir);
            fp = fopen(decoy, "w");
            if (NULL != fp) {
                fclose(fp);
            }
        }
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

/* remove what the servers we killed left in the tmpdir: the directories
 * their sockets are in */
static void clear_killed(void)
{
    char cmd[PMIX_PATH_MAX * 2];

    snprintf(cmd, sizeof(cmd), "rm -rf '%s'/pmix-*", tmpdir);
    if (0 != system(cmd)) {
        fprintf(stderr, "could not clear %s\n", tmpdir);
    }
}

/* remove everything we (or a server we started) left in the tmpdir */
static void cleanup(void)
{
    char cmd[PMIX_PATH_MAX * 2];

    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
    if (0 != system(cmd)) {
        fprintf(stderr, "could not remove %s\n", tmpdir);
    }
}

int main(void)
{
    const char *base;
    char *path = NULL;
    char lname[PMIX_PATH_MAX + 16];
    FILE *fp;
    int rc, held, lockfd;

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

    /* a file whose writer says it holds the lock beside it: one something
     * holds is in use, whatever its pid - which from another pid
     * namespace says nothing; one nothing holds is judged by its pid as
     * before, since a lock file can be removed from under a running
     * server - taken when the pid is gone, kept when it runs */
    for (held = 0; held < 3; held++) {
        fp = fopen(path, "w");
        if (NULL == fp) {
            report("a locked file: setup", 0, "could not write it");
            break;
        }
        fprintf(fp, "testns.0;tcp4://127.0.0.1:1\n%s\n%lu\n%lu:%lu\nsome time\nlock:flock\n",
                PMIX_VERSION, (unsigned long) ((2 == held) ? getpid() : reaped()),
                (unsigned long) getuid(), (unsigned long) getgid());
        fclose(fp);
        snprintf(lname, sizeof(lname), "%.*s.%s.lock", (int) (strrchr(path, '/') + 1 - path),
                 path, strrchr(path, '/') + 1);
        lockfd = open(lname, O_RDWR | O_CREAT, 0600);
        if (0 > lockfd || (0 == held && 0 != flock(lockfd, LOCK_EX | LOCK_NB))) {
            report("a locked file: setup", 0, "could not lock it");
            break;
        }
        if (0 != held) {
            close(lockfd);
        }
        rc = run_child(true);
        if (0 == held) {
            report("a file whose lock is held is not reclaimed, though its pid is gone",
                   CHILD_INITFAIL == rc,
                   "init should have failed");
            report("...and is preserved", file_exists(path), "file was removed");
            close(lockfd);
        } else if (1 == held) {
            report("a file whose lock nothing holds, and whose pid is gone, is reclaimed",
                   CHILD_OK == rc, "init failed");
        } else {
            report("...but not one whose pid runs", CHILD_INITFAIL == rc,
                   "init should have failed");
        }
        unlink(path);
        unlink(lname);
    }

    /* a name whose lock another server holds is that server's, whether
     * its file is there yet or not */
    snprintf(lname, sizeof(lname), "%.*s.%s.lock", (int) (strrchr(path, '/') + 1 - path), path,
             strrchr(path, '/') + 1);
    lockfd = open(lname, O_RDWR | O_CREAT, 0600);
    if (0 > lockfd || 0 != flock(lockfd, LOCK_EX | LOCK_NB)) {
        report("a held name: setup", 0, "could not lock it");
    } else {
        unlink(path);
        rc = run_child(true);
        report("a name whose lock another server holds is refused", CHILD_INITFAIL == rc,
               "init should have failed");
    }
    if (0 <= lockfd) {
        close(lockfd);
    }
    unlink(lname);

    /* and a file whose lock is held is in use though the name's lock
     * cannot be taken for another reason - here, a lock file only its
     * owner may read, which the next server opens to lock and cannot -
     * whatever its pid says */
    fp = fopen(path, "w");
    if (NULL != fp) {
        fprintf(fp, "testns.0;tcp4://127.0.0.1:1\n%s\n%lu\n%lu:%lu\nsome time\nlock:flock\n",
                PMIX_VERSION, (unsigned long) reaped(), (unsigned long) getuid(),
                (unsigned long) getgid());
        fclose(fp);
        lockfd = open(lname, O_RDONLY | O_CREAT, 0400);
        if (0 > lockfd || 0 != flock(lockfd, LOCK_EX | LOCK_NB)) {
            report("a read-only lock: setup", 0, "could not lock it");
        } else {
            rc = run_child(true);
            report("a file whose lock is held is in use, whatever takes the name",
                   CHILD_INITFAIL == rc, "init should have failed");
            report("...and is preserved", file_exists(path), "file was removed");
        }
        if (0 <= lockfd) {
            close(lockfd);
        }
        unlink(path);
        unlink(lname);
    }

    /* a file that was never completely written names no owner at all,
     * so it too must be reclaimed */
    plant_file(path, 0, true);
    rc = run_child(true);
    report("truncated file is reclaimed", CHILD_OK == rc, "init failed");

    /* likewise a file whose pid line is unusable */
    plant_file(path, 0, false);
    rc = run_child(true);
    report("file with no usable pid is reclaimed", CHILD_OK == rc, "init failed");

    /* a pid too wide for a pid_t names no process. 4294967295 narrows to
     * -1, and kill(-1, 0) asks about every process we may signal - so it
     * always answered that the file had a live owner, and a scheduler
     * refused to start over a garbage line */
    plant_file_pid(path, "4294967295");
    rc = run_child(true);
    report("file whose pid does not fit a pid_t is reclaimed", CHILD_OK == rc, "init failed");

    /* something other than a regular file at the name - here a FIFO,
     * whose open would block until a writer came along - is not a file
     * we wrote, and must be reclaimed without stalling the server */
    unlink(path);
    if (0 == mkfifo(path, 0600)) {
        rc = run_child(true);
        report("FIFO at the name is reclaimed", CHILD_OK == rc, "init failed or stalled");
        unlink(path);
    }

    /* the directory's mode is not held against it: a root-owned 0777
     * volume, such as a Kubernetes emptyDir at /tmp, is a normal place
     * for a containerized server to work. Ours at 0777 stands in */
    chmod(tmpdir, 0777);
    rc = run_child(true);
    report("world-writable directory is accepted", CHILD_OK == rc, "init failed");
    chmod(tmpdir, 0700);

    /* The directory is one we were handed, so whoever owns it is not
     * held against it: a host running its server with privilege commonly
     * gives it a per-job directory owned by the job's user. Only root can
     * make one to test with */
    if (0 == geteuid() && 0 == chown(tmpdir, 65534, 65534)) {
        rc = run_child(true);
        report("a given directory owned by another user is used", CHILD_OK == rc,
               "init failed");
        if (0 != chown(tmpdir, 0, 0)) {
            fprintf(stderr, "could not restore the tmpdir's owner\n");
        }
    }

    /* A tmpdir named through a symlink of our own is as much ours to
     * name as the directory itself - TMPDIR=/tmp on macOS is root's
     * link into /private. The tmpdir is the very directory the
     * rendezvous file is created in, and every server given such a
     * TMPDIR used to fail its init. A link of another user's is still
     * refused: anyone able to write where it sits could have put it
     * there. Only root can make one of those to test with */
    {
        char link[PMIX_PATH_MAX + 8];

        snprintf(link, sizeof(link), "%s.link", tmpdir);
        unlink(link);
        if (0 == symlink(tmpdir, link)) {
            setenv("PMIX_SYSTEM_TMPDIR", link, 1);
            setenv("PMIX_SERVER_TMPDIR", link, 1);
            rc = run_child(true);
            report("a tmpdir named through our own symlink is used", CHILD_OK == rc,
                   "init failed");
            report("its file is removed at finalize", !file_exists(path), "file still present");
            if (0 == geteuid() && 0 == lchown(link, 65534, 65534)) {
                rc = run_child(true);
                report("a tmpdir named through another user's symlink is refused",
                       CHILD_INITFAIL == rc, "init should have failed");
            }
            setenv("PMIX_SYSTEM_TMPDIR", tmpdir, 1);
            setenv("PMIX_SERVER_TMPDIR", tmpdir, 1);
            unlink(link);
        }
    }

    /* finalize removes the file from the directory it was created in,
     * not from whatever the name leads to by then */
    {
        char moved[PMIX_PATH_MAX * 2];

        snprintf(moveddir, sizeof(moveddir), "%s.moved", tmpdir);
        schedname = strrchr(path, '/') + 1;
        clear_killed();
        rc = run_child(true);
        schedname = NULL;
        snprintf(moved, sizeof(moved), "%s/%s", moveddir, strrchr(path, '/') + 1);
        report("file is removed from the directory it was created in",
               CHILD_OK == rc && !file_exists(moved), "file left in the moved directory");
        report("a file now at the old name is left alone", file_exists(path),
               "decoy was removed");
        snprintf(moved, sizeof(moved), "%s/had-hwloc", moveddir);
        if (file_exists(moved)) {
            unlink(moved);
            snprintf(moved, sizeof(moved), "%s/hwloc.sm", moveddir);
            report("hwloc.sm is removed from the directory it was created in",
                   !file_exists(moved), "hwloc.sm left in the moved directory");
            unlink(moved);
            snprintf(moved, sizeof(moved), "%s/hwloc.sm", tmpdir);
            report("a hwloc.sm now at the old name is left alone", file_exists(moved),
                   "decoy hwloc.sm was removed");
            unlink(moved);
        } else {
            fprintf(stdout, "  SKIP: no shmem topology on this platform, so no hwloc.sm to check\n");
        }
        snprintf(moved, sizeof(moved), "%s/%s", moveddir, strrchr(path, '/') + 1);
        unlink(moved);
        /* and nothing else the server made is left there either - its
         * socket's directory included */
        {
            char *left = leftovers(moveddir, NULL);

            report("nothing the server made is left in the moved directory", NULL == left,
                   (NULL == left) ? "" : left);
            free(left);
        }
        snprintf(moved, sizeof(moved), "rm -rf '%s'", moveddir);
        if (0 != system(moved)) {
            fprintf(stderr, "could not remove %s\n", moveddir);
        }
        unlink(path);
    }

    /* The directories a server makes for itself - the system tmpdir, the
     * rendezvous file's - and the report file it writes are removed at
     * finalize from wherever they have come to be. The rendezvous
     * directory and a tmpdir it made go only if nothing but the server's
     * own files were in them */
    {
        char moved[PMIX_PATH_MAX * 2], *left;
        const char *keep_top[] = {"mr", NULL};
        const char *keep_rndz[] = {"other", NULL};

        snprintf(moveddir, sizeof(moveddir), "%s.moved", tmpdir);
        clear_killed();
        snprintf(moved, sizeof(moved), "%s/my", tmpdir);
        setenv("PMIX_SYSTEM_TMPDIR", moved, 1);
        snprintf(moved, sizeof(moved), "%s/mr/rndz", tmpdir);
        setenv("PMIX_LAUNCHER_RENDEZVOUS_FILE", moved, 1);
        snprintf(moved, sizeof(moved), "%s/uri", tmpdir);
        setenv("PMIX_MCA_ptl_base_report_uri", moved, 1);
        made_case = 1;
        rc = run_child(true);
        made_case = 0;
        unsetenv("PMIX_MCA_ptl_base_report_uri");
        report("a server whose directories are moved away finalizes", CHILD_OK == rc,
               (CHILD_NOTMADE == rc) ? "not everything was made" : "init failed");

        /* nothing but the rendezvous directory, which holds another
         * server's file, is left where the tmpdir went: not the socket's
         * directory, not the report file, not the empty system tmpdir */
        left = leftovers(moveddir, keep_top);
        report("what it made is removed from where it went, the empty tmpdir it made included",
               CHILD_OK == rc && NULL == left, (NULL == left) ? "" : left);
        free(left);
        snprintf(moved, sizeof(moved), "%s/mr", moveddir);
        left = leftovers(moved, keep_rndz);
        report("its rendezvous file is removed from the directory it made", NULL == left,
               (NULL == left) ? "" : left);
        free(left);
        snprintf(moved, sizeof(moved), "%s/mr/other", moveddir);
        report("...which is kept for another server's file", file_exists(moved),
               "the directory was removed with another server's file in it");


        snprintf(moved, sizeof(moved), "rm -rf '%s'", moveddir);
        if (0 != system(moved)) {
            fprintf(stderr, "could not remove %s\n", moveddir);
        }
        if (0 != mkdir(tmpdir, 0700)) {
            fprintf(stderr, "could not remake %s\n", tmpdir);
        }

        /* A rendezvous directory and a tmpdir it made, with nothing else
         * in them, go. And a directory now at a made directory's name is
         * not the one made: it is left alone, and the one made is emptied
         * of what the server put in it, and removed, under its new name */
        snprintf(moved, sizeof(moved), "%s/mr2/rndz", tmpdir);
        setenv("PMIX_LAUNCHER_RENDEZVOUS_FILE", moved, 1);
        snprintf(moved, sizeof(moved), "%s/my", tmpdir);
        setenv("PMIX_SYSTEM_TMPDIR", moved, 1);
        made_case = 2;
        rc = run_child(true);
        made_case = 0;
        unsetenv("PMIX_LAUNCHER_RENDEZVOUS_FILE");
        setenv("PMIX_SYSTEM_TMPDIR", tmpdir, 1);
        snprintf(moved, sizeof(moved), "%s/mr2", tmpdir);
        report("an empty rendezvous directory it made is removed",
               CHILD_OK == rc && !file_exists(moved), "still present");
        snprintf(moved, sizeof(moved), "%s/my/decoy", tmpdir);
        report("a directory now at a made one's name is left alone",
               CHILD_OK == rc && file_exists(moved), "decoy was removed");
        snprintf(moved, sizeof(moved), "%s/my.old", tmpdir);
        report("...and the one made is removed under its new name",
               CHILD_OK == rc && !file_exists(moved), "still present");
        {
            char cmd[PMIX_PATH_MAX * 3];

            snprintf(cmd, sizeof(cmd), "rm -rf '%s/my' '%s/my.old'", tmpdir, tmpdir);
            if (0 != system(cmd)) {
                fprintf(stderr, "could not remove %s/my\n", tmpdir);
            }
        }
    }

    free(path);
    cleanup();

    fprintf(stdout, "\n%d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
