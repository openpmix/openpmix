/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit tests for the rendezvous-file directory walks and the fixed
 * rendezvous names.
 *
 * A tool that is not told where its server is searches for one: it walks
 * the system tmpdir for "pmix.*" contact files, and so does a
 * PMIX_QUERY_AVAIL_SERVERS query. Both walks:
 *
 *  - keep an entry only if it is a regular file, so a FIFO named like a
 *    contact file is skipped rather than opened with fopen();
 *  - never follow a symbolic link to a directory, so a link back up the
 *    tree ("ln -s . a") is not descended into;
 *  - skip a contact file that cannot be read or parsed, or is larger than
 *    any server writes, and carry on, so a valid file readdir() lists
 *    after it is still found;
 *  - pass over the file of a server that is gone - the file says its
 *    server holds the lock beside it, and nothing does - as a server
 *    that is killed leaves one behind next to a running server's. A file
 *    whose server holds that lock, one that does not say it locks, one
 *    naming a lock we do not know, and one with no lock file are kept.
 *
 * The directory built here holds all four next to one valid contact
 * file, and both walks must come back promptly having found that one
 * file. A watchdog turns a hang into a failure rather than a stalled
 * "make check". There are thirty unreadable files so that a readdir()
 * order listing the valid file first is unlikely.
 *
 * A walk descends only ptl_base_search_depth directories below where it
 * starts, so a tree built deeper than any server places its files is not
 * followed to the bottom.
 *
 * A search counts only the servers whose files are owned by a user whose
 * server this process takes: a running server of another user's beside
 * ours does not make the search find too many.
 *
 * A search for one particular server - by pid or by namespace - matches
 * the file's whole name, so asking for "tool.12" does not find "tool.123".
 *
 * The fixed rendezvous names (pmix.sys.<host> and friends) are read with
 * pmix_ptl_base_parse_rndz_file(), which applies the same regular-file
 * rule: a FIFO, or a link to a character device, is refused promptly.
 *
 * A file the caller names (pmix_ptl_base_parse_uri_file(), behind
 * PMIX_TOOL_ATTACHMENT_FILE and "file:<path>") is held to the rule too: a
 * FIFO is refused at once rather than waited on, and a symbolic link is
 * not followed - not even to a valid contact file - while that file named
 * itself is read.
 *
 * Every file these open is close-on-exec: a program a child execs has
 * none of them (pmix_ptl_base_open_regular, pmix_fd_fopen_cloexec,
 * pmix_os_dirpath_open_file).
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "include/pmix_server.h"
#include "src/include/pmix_globals.h"
#include "src/mca/bfrops/base/base.h"
#include "src/mca/psec/base/base.h"
#include "src/mca/ptl/base/base.h"
#include "src/util/pmix_fd.h"
#include "src/util/pmix_os_dirpath.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define NJUNK 30
#define FDCHECK "fdcheck"
#define WATCHDOG_SECS 60

static int npass = 0;
static int nfail = 0;
static char searchdir[PMIX_PATH_MAX];
static char sessdir[PMIX_PATH_MAX];

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
    fflush(stdout);
}

static void watchdog(int sig)
{
    static const char msg[] = "  FAIL: a walk or rendezvous read did not return - hung\n";
    PMIX_HIDE_UNUSED_PARAMS(sig);
    /* async-signal-safe only */
    (void) !write(STDOUT_FILENO, msg, sizeof(msg) - 1);
    _exit(1);
}

static int write_file(const char *name, const char *content)
{
    char path[PMIX_PATH_MAX + 128];
    FILE *fp;

    snprintf(path, sizeof(path), "%s/%s", searchdir, name);
    fp = fopen(path, "w");
    if (NULL == fp) {
        return -1;
    }
    fputs(content, fp);
    fclose(fp);
    return 0;
}

static int build_searchdir(void)
{
    char path[PMIX_PATH_MAX + 128], name[64], *big;
    size_t at;
    int n;

    /* a FIFO nobody will ever write to, named like a contact file */
    snprintf(path, sizeof(path), "%s/pmix.test.fifo", searchdir);
    if (0 != mkfifo(path, 0600)) {
        return -1;
    }
    /* two links back to the directory itself */
    snprintf(path, sizeof(path), "%s/loopA", searchdir);
    if (0 != symlink(".", path)) {
        return -1;
    }
    snprintf(path, sizeof(path), "%s/loopB", searchdir);
    if (0 != symlink(".", path)) {
        return -1;
    }
    /* contact files that cannot be parsed: an empty one, as a server
     * killed before writing leaves, and ones holding no URI */
    for (n = 0; n < NJUNK; n++) {
        snprintf(name, sizeof(name), "pmix.test.junk%02d", n);
        if (0 != write_file(name, (0 == n) ? "" : "not a uri\n")) {
            return -1;
        }
    }
    /* and one larger than any server writes, though it starts as one
     * does - a reader that took it line by line could be kept at it */
    big = malloc(PMIX_PTL_MAX_CONN_FILE + 2);
    if (NULL == big) {
        return -1;
    }
    at = (size_t) snprintf(big, PMIX_PTL_MAX_CONN_FILE, "bigns.0;tcp4://127.0.0.1:4240\n7.0.0\n");
    memset(big + at, 'x', PMIX_PTL_MAX_CONN_FILE + 1 - at);
    big[PMIX_PTL_MAX_CONN_FILE + 1] = '\0';
    n = write_file("pmix.test.huge", big);
    free(big);
    return n;
}

/* a pid no process has: a child that has exited and been reaped */
static pid_t gone_pid(void)
{
    pid_t pid;
    int st;

    pid = fork();
    if (0 == pid) {
        _exit(0);
    }
    if (0 > pid || pid != waitpid(pid, &st, 0)) {
        return -1;
    }
    return pid;
}

/* a lock file nothing holds, with the given mode */
static int write_lock_file(const char *name, mode_t mode)
{
    char path[PMIX_PATH_MAX + 128];

    if (0 != write_file(name, "")) {
        return -1;
    }
    snprintf(path, sizeof(path), "%s/%s", searchdir, name);
    return chmod(path, mode);
}

/* the lock beside a file, held as a running server holds its own */
static int heldfd = -1;

static int hold(const char *path)
{
    char *lname = pmix_ptl_base_lock_name(path);
    int fd;

    if (NULL == lname) {
        return -1;
    }
    fd = open(lname, O_RDWR | O_CREAT, 0600);
    free(lname);
    if (0 > fd || 0 != flock(fd, LOCK_EX | LOCK_NB)) {
        if (0 <= fd) {
            close(fd);
        }
        return -1;
    }
    return fd;
}

/* the file of a server that is gone - it says it holds the lock beside
 * it, and nothing does - and the one a search should find, whose lock
 * this process holds */
static int add_lock_files(void)
{
    char path[PMIX_PATH_MAX + 128];

    if (0 != write_file("pmix.test.dead", "deadns.0;tcp4://127.0.0.1:4241\n7.0.0\n1\n0:0\nnow\n"
                                          PMIX_PTL_LOCK_TAG PMIX_PTL_LOCK_FLOCK "\n") ||
        0 != write_lock_file(".pmix.test.dead" PMIX_PTL_LOCK_SUFFIX, 0600) ||
        0 != write_file("pmix.test.good", "goodns.0;tcp4://127.0.0.1:4242\n4.2.0\n1\n0:0\nnow\n"
                                          PMIX_PTL_LOCK_TAG PMIX_PTL_LOCK_FLOCK "\n")) {
        return -1;
    }
    snprintf(path, sizeof(path), "%s/pmix.test.good", searchdir);
    heldfd = hold(path);
    return (0 > heldfd) ? -1 : 0;
}

static void test_df_search(void)
{
    pmix_list_t connections;
    pmix_connection_t *cn;
    pmix_status_t rc;

    PMIX_CONSTRUCT(&connections, pmix_list_t);
    rc = pmix_ptl_base_df_search(searchdir, "pmix.test.", false, NULL, 0, true, &connections);
    report("df_search: returns success past a FIFO, link loops, unreadable files and a "
           "server that is gone",
           PMIX_SUCCESS == rc, PMIx_Error_string(rc));
    report("df_search: finds exactly the one valid contact file",
           1 == pmix_list_get_size(&connections), "wrong number of connections");
    if (1 == pmix_list_get_size(&connections)) {
        cn = (pmix_connection_t *) pmix_list_get_first(&connections);
        report("df_search: the connection found is the valid one",
               NULL != cn->nspace && 0 == strcmp(cn->nspace, "goodns"),
               (NULL == cn->nspace) ? "NULL" : cn->nspace);
        /* the release it names chooses the wire format, whatever is
         * believed of it - and a file found by a search is no caller's
         * word that the server is of an earlier release */
        report("df_search: the file's version is kept as it says, and not vouched for",
               NULL != cn->version && 0 == strcmp(cn->version, "4.2.0") && !cn->vouched,
               (NULL == cn->version) ? "NULL" : cn->version);
    }
    PMIX_LIST_DESTRUCT(&connections);
}

/* A file is kept when its server holds the lock beside it, when it does
 * not say it locks - from an earlier release - and the pid it records is
 * running, when it names a lock we do not know, when it has no lock file,
 * and when its lock file is one others could open. Each but the held one
 * has a lock file nothing holds beside it, but the one with none. A file
 * that does not say it locks and whose pid is no process's here is a
 * server that is gone, and is passed over: one such left behind by an
 * earlier release used to stop every search on its node. */
static bool found_ns(pmix_list_t *connections, const char *nspace)
{
    pmix_connection_t *cn;

    PMIX_LIST_FOREACH (cn, connections, pmix_connection_t) {
        if (NULL != cn->nspace && 0 == strcmp(cn->nspace, nspace)) {
            return true;
        }
    }
    return false;
}

static void test_df_search_kept(void)
{
    char dir[PMIX_PATH_MAX + 16], path[PMIX_PATH_MAX + 160], buf[512];
    const char *names[7] = {"pmix.k.held", "pmix.k.untagged", "pmix.k.otherlock",
                            "pmix.k.nolockfile", "pmix.k.openlock", "pmix.k.linkedlock",
                            "pmix.k.untaggedgone"};
    char other[PMIX_PATH_MAX + 200] = "";
    char *lname;
    pmix_list_t connections;
    pmix_status_t rc;
    pid_t pid;
    FILE *fp;
    int n, fd = -1;

    snprintf(dir, sizeof(dir), "%s.kept", searchdir);
    if (0 != mkdir(dir, 0700) || 0 > (pid = gone_pid())) {
        report("df_search kept: setup", 0, "mkdir or fork failed");
        return;
    }
    for (n = 0; n < 7; n++) {
        /* the untagged file that is kept names a process that runs - our
         * parent; our own pid would be a leftover of an earlier process */
        snprintf(buf, sizeof(buf), "ns%d.0;tcp4://127.0.0.1:%d\n7.0.0\n%lu\n0:0\nnow\n%s\n", n,
                 4250 + n, (1 == n) ? (unsigned long) getppid() : (unsigned long) pid,
                 (1 == n || 6 == n) ? "" : (2 == n) ? PMIX_PTL_LOCK_TAG "fcntl"
                                                    : PMIX_PTL_LOCK_TAG PMIX_PTL_LOCK_FLOCK);
        snprintf(path, sizeof(path), "%s/%s", dir, names[n]);
        if (NULL != (fp = fopen(path, "w"))) {
            fputs(buf, fp);
            fclose(fp);
        }
        if (0 == n) {
            fd = hold(path);
        } else if (3 != n && 6 != n && NULL != (lname = pmix_ptl_base_lock_name(path))) {
            if (NULL != (fp = fopen(lname, "w"))) {
                fclose(fp);
                chmod(lname, (4 == n) ? 0644 : 0600);
            }
            if (5 == n) {
                /* a lock file with a second name */
                snprintf(other, sizeof(other), "%s.other", lname);
                if (0 != link(lname, other)) {
                    other[0] = '\0';
                }
            }
            free(lname);
        }
    }
    PMIX_CONSTRUCT(&connections, pmix_list_t);
    rc = pmix_ptl_base_df_search(dir, "pmix.k.", false, NULL, 0, true, &connections);
    report("df_search kept: a file whose server holds the lock beside it, one that does not "
           "say it locks whose process runs, one naming another lock, one with no lock file, "
           "one whose lock file others could open, and one whose lock file has another name "
           "are all kept",
           0 <= fd && PMIX_SUCCESS == rc && 6 == pmix_list_get_size(&connections),
           "a file was passed over, or the gone one kept");
    report("df_search kept: ...and one that does not say it locks, whose process is gone, "
           "is passed over",
           !found_ns(&connections, "ns6"), "the file of a server that is gone was kept");
    PMIX_LIST_DESTRUCT(&connections);
    if (0 <= fd) {
        close(fd);
    }
    if ('\0' != other[0]) {
        unlink(other);
    }
    for (n = 0; n < 7; n++) {
        snprintf(path, sizeof(path), "%s/%s", dir, names[n]);
        unlink(path);
        snprintf(path, sizeof(path), "%s/.%s" PMIX_PTL_LOCK_SUFFIX, dir, names[n]);
        unlink(path);
    }
    rmdir(dir);
}

/* A search for a particular server takes only a file of exactly that
 * name: one whose name merely begins with it belongs to another server. */
static void test_df_search_exact(void)
{
    char dir[PMIX_PATH_MAX + 16], path[PMIX_PATH_MAX + 160];
    pmix_list_t connections;
    pmix_connection_t *cn;
    pmix_status_t rc;
    FILE *fp;

    snprintf(dir, sizeof(dir), "%s.exact", searchdir);
    if (0 != mkdir(dir, 0700)) {
        report("df_search exact: setup", 0, "mkdir failed");
        return;
    }
    snprintf(path, sizeof(path), "%s/pmix.x.tool.123", dir);
    if (NULL != (fp = fopen(path, "w"))) {
        fputs("otherns.0;tcp4://127.0.0.1:4243\n4.2.0\n", fp);
        fclose(fp);
    }
    PMIX_CONSTRUCT(&connections, pmix_list_t);
    rc = pmix_ptl_base_df_search(dir, "pmix.x.tool.12", true, NULL, 0, true, &connections);
    report("df_search exact: a longer name is not taken for the one asked for",
           PMIX_SUCCESS != rc && 0 == pmix_list_get_size(&connections),
           "matched by prefix");
    PMIX_LIST_DESTRUCT(&connections);

    snprintf(path, sizeof(path), "%s/pmix.x.tool.12", dir);
    if (NULL != (fp = fopen(path, "w"))) {
        fputs("wantns.0;tcp4://127.0.0.1:4244\n4.2.0\n", fp);
        fclose(fp);
    }
    PMIX_CONSTRUCT(&connections, pmix_list_t);
    rc = pmix_ptl_base_df_search(dir, "pmix.x.tool.12", true, NULL, 0, true, &connections);
    cn = (1 == pmix_list_get_size(&connections))
             ? (pmix_connection_t *) pmix_list_get_first(&connections) : NULL;
    report("df_search exact: the file of exactly that name is found",
           PMIX_SUCCESS == rc && NULL != cn && NULL != cn->nspace &&
               0 == strcmp(cn->nspace, "wantns"),
           "wrong or missing connection");
    PMIX_LIST_DESTRUCT(&connections);

    unlink(path);
    snprintf(path, sizeof(path), "%s/pmix.x.tool.123", dir);
    unlink(path);
    rmdir(dir);
}

/* Make dir/a/b/... levels deep, with a contact file named name at the
 * bottom. The path is built in place: each level appends "/x". */
static int make_chain(const char *dir, int levels, const char *name)
{
    char path[PMIX_PATH_MAX + 64];
    size_t len;
    FILE *fp;
    int n;

    len = strlen(dir);
    if (len + 2 * (size_t) levels + strlen(name) + 2 > sizeof(path)) {
        return -1;
    }
    memcpy(path, dir, len + 1);
    for (n = 0; n < levels; n++) {
        path[len++] = '/';
        path[len++] = (char) ('a' + n);
        path[len] = '\0';
        if (0 != mkdir(path, 0700)) {
            return -1;
        }
    }
    path[len++] = '/';
    memcpy(&path[len], name, strlen(name) + 1);
    if (NULL == (fp = fopen(path, "w"))) {
        return -1;
    }
    fputs("deepns.0;tcp4://127.0.0.1:4245\n4.2.0\n", fp);
    fclose(fp);
    return 0;
}

static void remove_chain(const char *dir, int levels, const char *name)
{
    char path[PMIX_PATH_MAX + 64];
    size_t len;

    len = strlen(dir);
    if (len + 2 * (size_t) levels + strlen(name) + 2 > sizeof(path)) {
        return;
    }
    memcpy(path, dir, len + 1);
    for (int n = 0; n < levels; n++) {
        path[len++] = '/';
        path[len++] = (char) ('a' + n);
    }
    path[len] = '/';
    memcpy(&path[len + 1], name, strlen(name) + 1);
    unlink(path);
    /* then each directory, deepest first */
    while (1) {
        path[len] = '\0';
        rmdir(path);
        if (len == strlen(dir)) {
            break;
        }
        len -= 2;
    }
}

static void test_df_search_depth(void)
{
    char dir[PMIX_PATH_MAX + 16];
    pmix_list_t connections;
    pmix_status_t rc;
    int saved = pmix_ptl_base.search_depth;

    pmix_ptl_base.search_depth = 2;

    snprintf(dir, sizeof(dir), "%s.deep2", searchdir);
    if (0 != mkdir(dir, 0700) || 0 != make_chain(dir, 2, "pmix.d.file")) {
        report("df_search depth: setup", 0, "could not build the tree");
    } else {
        PMIX_CONSTRUCT(&connections, pmix_list_t);
        rc = pmix_ptl_base_df_search(dir, "pmix.d.", false, NULL, 0, true, &connections);
        report("df_search depth: a file as deep as the limit is found",
               PMIX_SUCCESS == rc && 1 == pmix_list_get_size(&connections),
               PMIx_Error_string(rc));
        PMIX_LIST_DESTRUCT(&connections);
    }
    remove_chain(dir, 2, "pmix.d.file");

    snprintf(dir, sizeof(dir), "%s.deep3", searchdir);
    if (0 != mkdir(dir, 0700) || 0 != make_chain(dir, 3, "pmix.d.file")) {
        report("df_search depth: setup", 0, "could not build the tree");
    } else {
        PMIX_CONSTRUCT(&connections, pmix_list_t);
        rc = pmix_ptl_base_df_search(dir, "pmix.d.", false, NULL, 0, true, &connections);
        report("df_search depth: a file below the limit is not reached",
               PMIX_SUCCESS != rc && 0 == pmix_list_get_size(&connections),
               "found past the limit");
        PMIX_LIST_DESTRUCT(&connections);
    }
    remove_chain(dir, 3, "pmix.d.file");

    pmix_ptl_base.search_depth = saved;
}

static void test_rndz_file(void)
{
    char path[PMIX_PATH_MAX + 128];
    pmix_list_t connections;
    pmix_connection_t *cn;
    pmix_status_t rc;
    FILE *fp;
    size_t n;

    /* a FIFO at a rendezvous name is refused, not waited on */
    snprintf(path, sizeof(path), "%s/pmix.test.fifo", searchdir);
    PMIX_CONSTRUCT(&connections, pmix_list_t);
    rc = pmix_ptl_base_parse_rndz_file(path, true, &connections);
    report("rndz file: a FIFO is refused", PMIX_SUCCESS != rc, "accepted");
    PMIX_LIST_DESTRUCT(&connections);

    /* so is a link to a device that never reaches end of file */
    snprintf(path, sizeof(path), "%s/pmix.test.zero", searchdir);
    if (0 == symlink("/dev/zero", path)) {
        PMIX_CONSTRUCT(&connections, pmix_list_t);
        rc = pmix_ptl_base_parse_rndz_file(path, true, &connections);
        report("rndz file: a link to /dev/zero is refused", PMIX_SUCCESS != rc, "accepted");
        PMIX_LIST_DESTRUCT(&connections);
    }

    /* and a regular file is read as before */
    snprintf(path, sizeof(path), "%s/pmix.test.good", searchdir);
    PMIX_CONSTRUCT(&connections, pmix_list_t);
    rc = pmix_ptl_base_parse_rndz_file(path, true, &connections);
    report("rndz file: a regular contact file is read",
           PMIX_SUCCESS == rc && 1 == pmix_list_get_size(&connections),
           PMIx_Error_string(rc));
    PMIX_LIST_DESTRUCT(&connections);

    /* a file the caller names is not refused for its size - but nothing
     * past as much as any server writes is read: here, the tagged line
     * after a line longer than that */
    snprintf(path, sizeof(path), "%s/named.uri", searchdir);
    if (NULL != (fp = fopen(path, "w"))) {
        fputs("namedns.0;tcp4://127.0.0.1:4260\n7.0.0\n", fp);
        for (n = 0; n < PMIX_PTL_MAX_CONN_FILE + 16; n++) {
            fputc('x', fp);
        }
        fputs("\n" PMIX_PTL_PSEC_TAG "native\n", fp);
        fclose(fp);
        PMIX_CONSTRUCT(&connections, pmix_list_t);
        rc = pmix_ptl_base_parse_uri_file(path, true, &connections);
        cn = (1 == pmix_list_get_size(&connections))
                 ? (pmix_connection_t *) pmix_list_get_first(&connections) : NULL;
        report("named file: read no further than any server writes",
               PMIX_SUCCESS == rc && NULL != cn && NULL == cn->psec,
               (NULL != cn && NULL != cn->psec) ? "read past it" : PMIx_Error_string(rc));
        PMIX_LIST_DESTRUCT(&connections);
        unlink(path);
    }
}

/* A file the caller names is a regular one, at the name */
static void test_named_file(void)
{
    char path[PMIX_PATH_MAX + 128], link[PMIX_PATH_MAX + 128];
    pmix_list_t connections;
    pmix_status_t rc;
    time_t start;

    /* a FIFO, which an open for reading would wait on for a writer */
    snprintf(path, sizeof(path), "%s/pmix.test.fifo", searchdir);
    PMIX_CONSTRUCT(&connections, pmix_list_t);
    start = time(NULL);
    rc = pmix_ptl_base_parse_uri_file(path, true, &connections);
    report("named file: a FIFO is refused, at once",
           PMIX_SUCCESS != rc && 0 == pmix_list_get_size(&connections) &&
               2 >= difftime(time(NULL), start),
           PMIx_Error_string(rc));
    PMIX_LIST_DESTRUCT(&connections);

    /* a link to a valid contact file is not followed */
    snprintf(path, sizeof(path), "%s/pmix.test.good", searchdir);
    snprintf(link, sizeof(link), "%s/named.link", searchdir);
    unlink(link);
    if (0 != symlink(path, link)) {
        report("named file: setup", 0, strerror(errno));
        return;
    }
    PMIX_CONSTRUCT(&connections, pmix_list_t);
    rc = pmix_ptl_base_parse_uri_file(link, true, &connections);
    report("named file: a symbolic link to a contact file is refused",
           PMIX_SUCCESS != rc && 0 == pmix_list_get_size(&connections),
           PMIx_Error_string(rc));
    PMIX_LIST_DESTRUCT(&connections);
    unlink(link);

    /* while the contact file itself is read */
    PMIX_CONSTRUCT(&connections, pmix_list_t);
    rc = pmix_ptl_base_parse_uri_file(path, true, &connections);
    report("named file: a regular contact file is read",
           PMIX_SUCCESS == rc && 1 == pmix_list_get_size(&connections),
           PMIx_Error_string(rc));
    PMIX_LIST_DESTRUCT(&connections);
}

/* the child: whether descriptor `fd` is open in this exec'd program */
static int fdcheck(const char *fd)
{
    return (0 > fcntl(atoi(fd), F_GETFD) && EBADF == errno) ? 0 : 1;
}

/* Whether a program a child execs has none of `fd`: the child execs this
 * program again, which looks for the descriptor */
static bool not_inherited(const char *argv0, int fd)
{
    char num[32];
    pid_t pid;
    int status = 0;

    if (0 > fd) {
        return false;
    }
    snprintf(num, sizeof(num), "%d", fd);
    fflush(stdout);
    pid = fork();
    if (0 > pid) {
        return false;
    }
    if (0 == pid) {
        execl(argv0, argv0, FDCHECK, num, (char *) NULL);
        _exit(2);
    }
    if (pid != waitpid(pid, &status, 0)) {
        return false;
    }
    return WIFEXITED(status) && 0 == WEXITSTATUS(status);
}

static void test_cloexec(const char *argv0)
{
    char path[PMIX_PATH_MAX + 128];
    FILE *fp;
    int fd;

    snprintf(path, sizeof(path), "%s/pmix.test.good", searchdir);
    fp = pmix_ptl_base_open_regular(path, 0);
    report("cloexec: a contact file is not inherited by a program a child execs",
           NULL != fp && not_inherited(argv0, fileno(fp)), "inherited");
    if (NULL != fp) {
        fclose(fp);
    }
    fp = pmix_fd_fopen_cloexec(path, "r");
    report("cloexec: nor is a file pmix_fd_fopen_cloexec opens for reading",
           NULL != fp && not_inherited(argv0, fileno(fp)), "inherited");
    if (NULL != fp) {
        fclose(fp);
    }
    snprintf(path, sizeof(path), "%s/cloexec.out", searchdir);
    fp = pmix_fd_fopen_cloexec(path, "a");
    report("cloexec: ...or for appending",
           NULL != fp && not_inherited(argv0, fileno(fp)), "inherited");
    if (NULL != fp) {
        fclose(fp);
    }
    fd = pmix_os_dirpath_open_file(path, O_WRONLY, 0);
    report("cloexec: nor a file pmix_os_dirpath_open_file opens", not_inherited(argv0, fd),
           "inherited");
    if (0 <= fd) {
        close(fd);
    }
    unlink(path);
}

/* A search counts only the servers whose files say they run as a user
 * this process takes a server of. Two files whose servers both run - each
 * names a process that is running and says nothing of a lock - are found
 * by the walk. Both our own: they name two servers, and the search is
 * refused as finding too many. One of them another user's: it is passed
 * over before the servers are counted, and the search takes ours. Running
 * as root the file is given to another user; otherwise the owner the walk
 * recorded for it is set to another uid, as the walk records it for a
 * file of another user's */
static void test_search_owner(void)
{
    char dir[PMIX_PATH_MAX + 16], path[PMIX_PATH_MAX + 160], buf[256];
    const char *names[2] = {"pmix.o.mine", "pmix.o.other"};
    uid_t other = (0 == geteuid()) ? 4242 : geteuid() + 4242;
    pmix_list_t connections;
    pmix_connection_t *cn;
    pmix_status_t rc;
    FILE *fp;
    int n;

    snprintf(dir, sizeof(dir), "%s.owner", searchdir);
    if (0 != mkdir(dir, 0700)) {
        report("search owner: setup", 0, "mkdir failed");
        return;
    }
    for (n = 0; n < 2; n++) {
        snprintf(buf, sizeof(buf), "ownns%d.0;tcp4://127.0.0.1:%d\n6.1.0\n%lu\n0:0\nnow\n", n,
                 4270 + n, (unsigned long) getppid());
        snprintf(path, sizeof(path), "%s/%s", dir, names[n]);
        if (NULL == (fp = fopen(path, "w"))) {
            report("search owner: setup", 0, "could not write a file");
            rmdir(dir);
            return;
        }
        fputs(buf, fp);
        fclose(fp);
        chmod(path, 0644);
    }

    PMIX_CONSTRUCT(&connections, pmix_list_t);
    rc = pmix_ptl_base_df_search_depth(dir, "pmix.o.", false, NULL, 0, true, &connections, 0);
    if (PMIX_SUCCESS != rc || 2 != pmix_list_get_size(&connections)) {
        report("search owner: both running servers' files are found", 0, PMIx_Error_string(rc));
    } else {
        rc = pmix_ptl_base_check_found(&connections);
        report("search owner: two servers of our own are too many", PMIX_ERR_UNREACH == rc,
               PMIx_Error_string(rc));
    }
    PMIX_LIST_DESTRUCT(&connections);

    snprintf(path, sizeof(path), "%s/%s", dir, names[1]);
    if (0 == geteuid() && 0 != chown(path, other, (gid_t) -1)) {
        report("search owner: owner fixture", 0, "could not give the file to another user");
    } else {
        PMIX_CONSTRUCT(&connections, pmix_list_t);
        rc = pmix_ptl_base_df_search_depth(dir, "pmix.o.", false, NULL, 0, true, &connections, 0);
        if (0 != geteuid()) {
            fprintf(stdout, "  (not running as root: the owner recorded for %s is set to uid "
                            "%lu)\n",
                    names[1], (unsigned long) other);
            PMIX_LIST_FOREACH (cn, &connections, pmix_connection_t) {
                if (NULL != cn->nspace && 0 == strcmp(cn->nspace, "ownns1")) {
                    cn->owner = other;
                }
            }
        }
        if (PMIX_SUCCESS != rc || 2 != pmix_list_get_size(&connections)) {
            report("search owner: the other user's file is found by the walk", 0,
                   PMIx_Error_string(rc));
        } else {
            rc = pmix_ptl_base_check_found(&connections);
            cn = (pmix_connection_t *) pmix_list_get_first(&connections);
            report("search owner: beside another user's running server, ours is taken",
                   PMIX_SUCCESS == rc && 1 == pmix_list_get_size(&connections) &&
                       NULL != cn->nspace && 0 == strcmp(cn->nspace, "ownns0"),
                   PMIX_SUCCESS == rc ? "the other user's server was kept"
                                      : PMIx_Error_string(rc));
        }
        PMIX_LIST_DESTRUCT(&connections);
    }
    for (n = 0; n < 2; n++) {
        snprintf(path, sizeof(path), "%s/%s", dir, names[n]);
        unlink(path);
    }
    rmdir(dir);
}

/* A server the kernel cannot name is connected to only where its address
 * came from a source that vouches for it. A file the caller named does,
 * when it is the file at the name, only its owner can write it, and it is
 * owned by our own user, root, a user we were told to expect or that
 * ptl_base_server_access names, or an id from outside our user namespace.
 * What release it names, and whether its server holds its lock, are no
 * part of it. A file at a fixed name, or found by a search, never does */
static bool vouched_by(const char *version, const char *tail, mode_t mode, bool named, bool held,
                       uid_t owner)
{
    char path[PMIX_PATH_MAX + 128];
    pmix_list_t connections;
    pmix_connection_t *cn;
    pmix_status_t rc;
    bool vouched = false;
    char *lname;
    int fd = -1;
    FILE *fp;

    snprintf(path, sizeof(path), "%s/vouch.uri", searchdir);
    unlink(path);
    fp = fopen(path, "w");
    if (NULL == fp) {
        return false;
    }
    fprintf(fp, "vns.0;tcp4://127.0.0.1:4260\n%s\n%s", version, tail);
    fclose(fp);
    chmod(path, mode);
    if ((uid_t) -1 != owner && 0 != chown(path, owner, (gid_t) -1)) {
        unlink(path);
        return false;
    }
    if (held) {
        fd = hold(path);
    }
    PMIX_CONSTRUCT(&connections, pmix_list_t);
    if (named) {
        rc = pmix_ptl_base_parse_uri_file(path, true, &connections);
    } else {
        rc = pmix_ptl_base_parse_rndz_file(path, true, &connections);
    }
    if (PMIX_SUCCESS == rc && 1 == pmix_list_get_size(&connections)) {
        cn = (pmix_connection_t *) pmix_list_get_first(&connections);
        vouched = cn->vouched;
    }
    PMIX_LIST_DESTRUCT(&connections);
    if (0 <= fd) {
        close(fd);
    }
    if (NULL != (lname = pmix_ptl_base_lock_name(path))) {
        unlink(lname);
        free(lname);
    }
    unlink(path);
    return vouched;
}

/* a map file of the form /proc/self/uid_map has */
static int write_map(const char *name, const char *body)
{
    char path[PMIX_PATH_MAX + 128];
    FILE *fp;

    snprintf(path, sizeof(path), "%s/%s", searchdir, name);
    fp = fopen(path, "w");
    if (NULL == fp) {
        return -1;
    }
    fputs(body, fp);
    fclose(fp);
    return 0;
}

static void test_vouched(void)
{
    char cur[32], earlier[32], path[PMIX_PATH_MAX + 128], map[PMIX_PATH_MAX + 128];
    const char *locked = PMIX_PTL_LOCK_TAG PMIX_PTL_LOCK_FLOCK "\n";
    struct stat st;
    FILE *fp;

    snprintf(cur, sizeof(cur), "v%d.0.0", PMIX_PTL_LOCAL_MAJOR);
    snprintf(earlier, sizeof(earlier), "v%d.2.0", PMIX_PTL_LOCAL_MAJOR - 3);
    report("vouched: a named file of an earlier release",
           vouched_by(earlier, "", 0644, true, false, (uid_t) -1), "not vouched for");
    report("vouched: a named file of this release whose server holds its lock",
           vouched_by(cur, locked, 0644, true, true, (uid_t) -1), "not vouched for");
    report("vouched: ...and one whose lock nothing holds - the lock is no part of it",
           vouched_by(cur, locked, 0644, true, false, (uid_t) -1), "not vouched for");
    report("vouched: ...and one that names no release at all",
           vouched_by("vfoo", "", 0644, true, false, (uid_t) -1), "not vouched for");
    report("vouched: not a file its group may write",
           !vouched_by(cur, locked, 0664, true, true, (uid_t) -1), "vouched for");
    report("vouched: not a file at a fixed name nobody named",
           !vouched_by(earlier, "", 0644, false, false, (uid_t) -1), "vouched for");

    /* who owns a named file. The owner is decided by a map file, so a
     * fake one stands in for /proc/self/uid_map: OTHER is mapped into our
     * namespace by one, and from outside it by the other */
    snprintf(path, sizeof(path), "%s/owned.uri", searchdir);
    fp = fopen(path, "w");
    if (NULL == fp) {
        report("vouched: owner fixture", 0, "could not write the file");
        return;
    }
    fputs("vns.0;tcp4://127.0.0.1:4260\n", fp);
    fclose(fp);
    chmod(path, 0644);
    if (0 != geteuid()) {
        fprintf(stdout, "  SKIP: a named file of another user's - not running as root\n");
    } else if (0 != chown(path, 4242, (gid_t) -1) || 0 != stat(path, &st) ||
               0 != write_map("map.all", "0 0 4294967295\n") ||
               0 != write_map("map.some", "0 0 1000\n")) {
        report("vouched: owner fixture", 0, "could not set up the owner");
    } else {
        snprintf(map, sizeof(map), "%s/map.all", searchdir);
        report("vouched: not a named file of another user we know",
               !pmix_ptl_base_named_file_vouches(path, &st, map), "vouched for");
        snprintf(map, sizeof(map), "%s/map.some", searchdir);
        report("vouched: a named file whose owner is from outside our user namespace",
               pmix_ptl_base_named_file_vouches(path, &st, map), "not vouched for");
        snprintf(map, sizeof(map), "%s/map.all", searchdir);
        unlink(map);
        snprintf(map, sizeof(map), "%s/map.some", searchdir);
        unlink(map);
    }
    unlink(path);
}

/* A server's file names the wire formats and security mechanisms it
 * accepts; a process that finds it there takes the highest of its own the
 * server lists, refuses to try when there is none, and - for a server too
 * old to list them - goes by the server's version */
static void test_compat(void)
{
    char path[PMIX_PATH_MAX + 128];
    pmix_list_t connections;
    pmix_connection_t *cn, scratch;
    pmix_peer_t *peer;
    pmix_psec_module_t *native;
    pmix_status_t rc;

    snprintf(path, sizeof(path), "%s/pmix.test.compat", searchdir);
    write_file("pmix.test.compat", "pmix-server.1;tcp4://127.0.0.1:1\n7.0.0\n1\n0:0\nnow\n"
                     "bfrops:v41,v21\npsec:native\n");
    PMIX_CONSTRUCT(&connections, pmix_list_t);
    rc = pmix_ptl_base_parse_rndz_file(path, true, &connections);
    cn = (pmix_connection_t *) pmix_list_get_first(&connections);
    report("compat: the server's wire formats and mechanisms are read",
           PMIX_SUCCESS == rc && 1 == pmix_list_get_size(&connections) &&
               NULL != cn->bfrops && 0 == strcmp(cn->bfrops, "v41,v21") &&
               NULL != cn->psec && 0 == strcmp(cn->psec, "native"),
           PMIx_Error_string(rc));
    PMIX_LIST_DESTRUCT(&connections);
    unlink(path);

    snprintf(path, sizeof(path), "%s/pmix.test.old", searchdir);
    write_file("pmix.test.old", "pmix-server.1;tcp4://127.0.0.1:1\n5.0.9\n1\n0:0\nnow\n");
    PMIX_CONSTRUCT(&connections, pmix_list_t);
    rc = pmix_ptl_base_parse_rndz_file(path, true, &connections);
    cn = (pmix_connection_t *) pmix_list_get_first(&connections);
    report("compat: an older server's file has none to read",
           PMIX_SUCCESS == rc && NULL == cn->bfrops && NULL == cn->psec, PMIx_Error_string(rc));
    PMIX_LIST_DESTRUCT(&connections);
    unlink(path);

    /* a server peer that is not our primary - our own modules stay put */
    native = pmix_psec_base_assign_module("native");
    peer = PMIX_NEW(pmix_peer_t);
    peer->nptr = PMIX_NEW(pmix_namespace_t);
    PMIX_CONSTRUCT(&scratch, pmix_connection_t);
    scratch.uri = strdup("tcp4://127.0.0.1:1");

#define RESET()                                                                 \
    do {                                                                        \
        peer->nptr->compat.bfrops = pmix_bfrops_base_assign_module(NULL);      \
        peer->nptr->compat.psec = native;                                      \
        free(scratch.bfrops);                                                   \
        scratch.bfrops = NULL;                                                  \
        free(scratch.psec);                                                     \
        scratch.psec = NULL;                                                    \
        PMIX_SET_PEER_VERSION(peer, "7.0.0", 2, 0);                             \
    } while (0)

    RESET();
    scratch.bfrops = strdup("v99,v41,v21");
    rc = pmix_ptl_base_select_compat((struct pmix_peer_t *) peer, &scratch);
    report("compat: the highest wire format both have is taken",
           PMIX_SUCCESS == rc && 0 == strcmp(peer->nptr->compat.bfrops->version, "v41"),
           PMIx_Error_string(rc));
    report("compat: a server that is not the primary leaves our own format alone",
           pmix_globals.mypeer->nptr->compat.bfrops != peer->nptr->compat.bfrops, "changed");

    RESET();
    scratch.bfrops = strdup("v98,v99");
    rc = pmix_ptl_base_select_compat((struct pmix_peer_t *) peer, &scratch);
    report("compat: no wire format in common - the connection is not attempted",
           PMIX_ERR_NOT_SUPPORTED == rc, PMIx_Error_string(rc));

    RESET();
    PMIX_SET_PEER_VERSION(peer, "5.0.9", 2, 0);
    rc = pmix_ptl_base_select_compat((struct pmix_peer_t *) peer, &scratch);
    report("compat: an older server that lists none is matched by its version",
           PMIX_SUCCESS == rc && 0 == strcmp(peer->nptr->compat.bfrops->version, "v41"),
           PMIx_Error_string(rc));

    RESET();
    rc = pmix_ptl_base_select_compat((struct pmix_peer_t *) peer, &scratch);
    report("compat: a server as new as us that lists none keeps our newest",
           PMIX_SUCCESS == rc &&
               peer->nptr->compat.bfrops == pmix_bfrops_base_assign_module(NULL),
           PMIx_Error_string(rc));

    RESET();
    scratch.psec = strdup("no-such-mechanism,native");
    rc = pmix_ptl_base_select_compat((struct pmix_peer_t *) peer, &scratch);
    report("compat: our security mechanism is kept when the server lists it",
           PMIX_SUCCESS == rc && native == peer->nptr->compat.psec, PMIx_Error_string(rc));

    /* an older server lists no mechanism: munge if we have it, else
     * native - whatever we would have picked for ourselves */
    RESET();
    peer->nptr->compat.psec = NULL;
    rc = pmix_ptl_base_select_compat((struct pmix_peer_t *) peer, &scratch);
    report("compat: an older server that lists no mechanism gets munge or native",
           PMIX_SUCCESS == rc && NULL != peer->nptr->compat.psec &&
               (0 == strcmp(peer->nptr->compat.psec->name, "munge") ||
                0 == strcmp(peer->nptr->compat.psec->name, "native")) &&
               peer->nptr->compat.psec == pmix_psec_base_assign_module("munge,native"),
           PMIx_Error_string(rc));

    RESET();
    scratch.psec = strdup("no-such-mechanism");
    rc = pmix_ptl_base_select_compat((struct pmix_peer_t *) peer, &scratch);
    report("compat: no security mechanism in common - the connection is not attempted",
           PMIX_ERR_NOT_SUPPORTED == rc, PMIx_Error_string(rc));
#undef RESET

    PMIX_DESTRUCT(&scratch);
    peer->nptr->compat.psec = NULL;
    PMIX_RELEASE(peer);
}

static void test_query_servers(void)
{
    pmix_query_t query;
    pmix_info_t *results = NULL, *sdata;
    size_t nresults = 0, n, m, nservers = 0, ndata;
    pmix_status_t rc;
    bool found = false;

    PMIX_QUERY_CONSTRUCT(&query);
    PMIx_Argv_append_nosize(&query.keys, PMIX_QUERY_AVAIL_SERVERS);
    rc = PMIx_Query_info(&query, 1, &results, &nresults);
    report("query avail servers: returns success past a FIFO, link loops and a server that is "
           "gone",
           PMIX_SUCCESS == rc, PMIx_Error_string(rc));
    for (n = 0; n < nresults; n++) {
        if (!PMIX_CHECK_KEY(&results[n], PMIX_SERVER_INFO_ARRAY) ||
            PMIX_DATA_ARRAY != results[n].value.type) {
            continue;
        }
        ++nservers;
        sdata = (pmix_info_t *) results[n].value.data.darray->array;
        ndata = results[n].value.data.darray->size;
        for (m = 0; m < ndata; m++) {
            if (PMIX_CHECK_KEY(&sdata[m], PMIX_SERVER_NSPACE) &&
                0 == strcmp(sdata[m].value.data.string, "goodns")) {
                found = true;
            }
        }
    }
    report("query avail servers: reports exactly one server", 1 == nservers,
           "wrong number of servers");
    report("query avail servers: the server reported is the valid one", found, "not found");
    if (NULL != results) {
        PMIX_INFO_FREE(results, nresults);
    }
    PMIX_QUERY_DESTRUCT(&query);
}

static void cleanup(void)
{
    char cmd[3 * PMIX_PATH_MAX];

    /* the tree holds links and a FIFO - let rm deal with them */
    snprintf(cmd, sizeof(cmd), "rm -rf '%s' '%s'", searchdir, sessdir);
    if (0 != system(cmd)) {
        fprintf(stderr, "could not remove %s\n", searchdir);
    }
}

int main(int argc, char **argv)
{
    pmix_info_t info[2];
    pmix_status_t rc;
    const char *tdir;
    if (3 == argc && 0 == strcmp(argv[1], FDCHECK)) {
        return fdcheck(argv[2]);
    }

    tdir = getenv("TMPDIR");
    if (NULL == tdir) {
        tdir = "/tmp";
    }
    snprintf(searchdir, sizeof(searchdir), "%s/ptlsearch.XXXXXX", tdir);
    snprintf(sessdir, sizeof(sessdir), "%s/ptlsess.XXXXXX", tdir);
    if (NULL == mkdtemp(searchdir) || NULL == mkdtemp(sessdir)) {
        fprintf(stderr, "mkdtemp failed\n");
        return 1;
    }
    if (0 != build_searchdir()) {
        fprintf(stderr, "could not build the search directory\n");
        cleanup();
        return 1;
    }

    /* the search directory is the server's system tmpdir, which is what
     * PMIX_QUERY_AVAIL_SERVERS walks; keep the server's own session files
     * out of it */
    PMIX_INFO_LOAD(&info[0], PMIX_SYSTEM_TMPDIR, searchdir, PMIX_STRING);
    PMIX_INFO_LOAD(&info[1], PMIX_SERVER_TMPDIR, sessdir, PMIX_STRING);
    rc = PMIx_server_init(&mymodule, info, 2);
    PMIX_INFO_DESTRUCT(&info[0]);
    PMIX_INFO_DESTRUCT(&info[1]);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        cleanup();
        return 1;
    }

    fprintf(stdout, "\n=== ptl rendezvous directory walk unit tests ===\n\n");
    if (0 != add_lock_files()) {
        fprintf(stderr, "could not write the locked files\n");
        PMIx_server_finalize();
        cleanup();
        return 1;
    }

    signal(SIGALRM, watchdog);
    alarm(WATCHDOG_SECS);
    test_df_search();
    test_df_search_exact();
    test_df_search_kept();
    test_df_search_depth();
    test_search_owner();
    test_query_servers();
    test_rndz_file();
    test_named_file();
    test_cloexec(argv[0]);
    test_compat();
    test_vouched();
    alarm(0);

    fprintf(stdout, "\n%d passed, %d failed\n", npass, nfail);

    PMIx_server_finalize();
    if (0 <= heldfd) {
        close(heldfd);
    }
    cleanup();

    return (0 == nfail) ? 0 : 1;
}
