/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Who owns and reads a job's output files - see docs/hosts/access-control.rst,
 * phase 4.
 *
 * The test process is the server. It registers jobs asking for their
 * output in a directory (PMIX_IOF_OUTPUT_TO_DIRECTORY) or in files
 * (PMIX_IOF_OUTPUT_TO_FILE), delivers a line of output for each with
 * PMIx_server_IOF_deliver, and looks at what was created.
 *
 * Test cases:
 *
 *   a job this server's user owns   -> its files 0600, its directories
 *                                      0700
 *   the same, with a group in the
 *   access list                     -> files 0640 and directories 0750,
 *                                      given to that group
 *   another user's job              -> everything done as the owner:
 *                                      as root, the files and directories
 *                                      are the owner's from creation
 *                                      (0600, 0700); a server that cannot
 *                                      become the owner writes nothing
 *   as root only: another user's
 *   existing file                   -> refused, left as it was
 *   ... a directory only root may
 *   write                           -> refused
 *   ... a directory the owner
 *   reaches through a supplementary
 *   group the host registered       -> works
 *   a file left by an earlier run   -> made owner-only
 *   a file with a second name       -> not written
 *   a job whose files another
 *   process writes
 *   (PMIX_IOF_FILE_WRITER)          -> the server writes none
 *   ... naming the server itself    -> written as usual
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "include/pmix_server.h"

#include "src/include/pmix_globals.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ids no real account should have */
#define OTHER_UID 4242
#define OTHER_GID 4343

static int npass = 0;
static int nfail = 0;
static char base[512];

static void report(const char *name, int passed)
{
    if (passed) {
        fprintf(stdout, "  PASS: %s\n", name);
        npass++;
    } else {
        fprintf(stdout, "  FAIL: %s\n", name);
        nfail++;
    }
}

static void progress_barrier(void)
{
    pmix_value_t v;

    PMIX_VALUE_LOAD(&v, "barrier", PMIX_STRING);
    PMIx_Store_internal(&pmix_globals.myid, "iof-perms-ut.barrier", &v);
    PMIX_VALUE_DESTRUCT(&v);
}

/* register a one-process job owned by uid/gid, its output going to
 * "where" (a directory, or a file prefix), with one group in its access
 * list when listed_gid is not (gid_t)-1 */
static pmix_status_t reg(const char *name, const char *key, const char *where, uint32_t uid,
                         uint32_t gid, gid_t listed_gid)
{
    pmix_info_t info[5], entry;
    pmix_data_array_t *ids, perms;
    pmix_nspace_t ns;
    pmix_status_t rc;
    uint32_t one = 1;
    size_t n, ninfo = 4;

    PMIX_INFO_LOAD(&info[0], PMIX_JOB_SIZE, &one, PMIX_UINT32);
    PMIX_INFO_LOAD(&info[1], key, where, PMIX_STRING);
    PMIX_INFO_LOAD(&info[2], PMIX_USERID, &uid, PMIX_UINT32);
    PMIX_INFO_LOAD(&info[3], PMIX_GRPID, &gid, PMIX_UINT32);
    if ((gid_t) -1 != listed_gid) {
        PMIX_DATA_ARRAY_CREATE(ids, 1, PMIX_UINT32);
        ((uint32_t *) ids->array)[0] = (uint32_t) listed_gid;
        PMIX_INFO_LOAD(&entry, PMIX_ACCESS_GRPIDS, ids, PMIX_DATA_ARRAY);
        PMIX_DATA_ARRAY_FREE(ids);
        perms.type = PMIX_INFO;
        perms.size = 1;
        perms.array = &entry;
        PMIX_INFO_LOAD(&info[4], PMIX_ACCESS_PERMISSIONS, &perms, PMIX_DATA_ARRAY);
        PMIX_INFO_DESTRUCT(&entry);
        ninfo = 5;
    }
    PMIX_LOAD_NSPACE(ns, name);
    rc = PMIx_server_register_nspace(ns, 1, info, ninfo, NULL, NULL);
    if (PMIX_OPERATION_SUCCEEDED == rc) {
        rc = PMIX_SUCCESS;
    }
    progress_barrier();
    for (n = 0; n < ninfo; n++) {
        PMIX_INFO_DESTRUCT(&info[n]);
    }
    return rc;
}

/* a line of stdout from rank 0 of the job, and time for it to be written */
static void deliver(const char *name)
{
    pmix_byte_object_t bo;
    pmix_proc_t src;

    PMIX_LOAD_PROCID(&src, name, 0);
    bo.bytes = (char *) "hello\n";
    bo.size = 6;
    (void) PMIx_server_IOF_deliver(&src, PMIX_FWD_STDOUT_CHANNEL, &bo, NULL, 0, NULL, NULL);
    progress_barrier();
    usleep(200000);
    progress_barrier();
}

static bool mode_is(const char *path, mode_t want)
{
    struct stat buf;

    return 0 == stat(path, &buf) && want == (buf.st_mode & 07777);
}

static bool owned_by(const char *path, uid_t uid, gid_t gid)
{
    struct stat buf;

    return 0 == stat(path, &buf) && buf.st_uid == uid && ((gid_t) -1 == gid || buf.st_gid == gid);
}

static void paths(const char *dir, const char *ns, char *nsdir, char *rankdir, char *file)
{
    snprintf(nsdir, 1024, "%s/%s", dir, ns);
    snprintf(rankdir, 1024, "%s/%s/rank.0", dir, ns);
    snprintf(file, 1024, "%s/%s/rank.0/stdout", dir, ns);
}

static void test_own_job(void)
{
    char dir[1024], nsdir[1024], rankdir[1024], file[1024];

    snprintf(dir, sizeof(dir), "%s/own", base);
    if (PMIX_SUCCESS != reg("perms-own", PMIX_IOF_OUTPUT_TO_DIRECTORY, dir, geteuid(), getegid(),
                            (gid_t) -1)) {
        report("own: fixture", 0);
        return;
    }
    deliver("perms-own");
    paths(dir, "perms-own", nsdir, rankdir, file);
    report("own job: the output file is owner-only", mode_is(file, 0600));
    report("own job: its directories are the owner's alone",
           mode_is(nsdir, 0700) && mode_is(rankdir, 0700));
}

static void test_listed_group(void)
{
    char dir[1024], nsdir[1024], rankdir[1024], file[1024];

    snprintf(dir, sizeof(dir), "%s/listed", base);
    if (PMIX_SUCCESS != reg("perms-listed", PMIX_IOF_OUTPUT_TO_DIRECTORY, dir, geteuid(),
                            getegid(), getegid())) {
        report("listed: fixture", 0);
        return;
    }
    deliver("perms-listed");
    paths(dir, "perms-listed", nsdir, rankdir, file);
    report("listed group: the output file is readable by the group",
           mode_is(file, 0640) && owned_by(file, geteuid(), getegid()));
    report("listed group: its directories let the group in",
           mode_is(nsdir, 0750) && mode_is(rankdir, 0750) &&
               owned_by(rankdir, geteuid(), getegid()));
}

static bool exists(const char *path)
{
    struct stat buf;

    return 0 == lstat(path, &buf);
}

static bool holds(const char *path, const char *want)
{
    char buf[64] = {0};
    FILE *fp = fopen(path, "r");
    size_t n;

    if (NULL == fp) {
        return false;
    }
    n = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    buf[n] = '\0';
    return 0 == strcmp(buf, want);
}

/* register the job as reg() does, and the groups its owner belongs to */
static pmix_status_t reg_with_groups(const char *name, const char *where, uint32_t uid,
                                     uint32_t gid, uint32_t extra)
{
    pmix_info_t info[5];
    pmix_data_array_t *ugids;
    pmix_nspace_t ns;
    pmix_status_t rc;
    uint32_t one = 1;
    size_t n;

    PMIX_INFO_LOAD(&info[0], PMIX_JOB_SIZE, &one, PMIX_UINT32);
    PMIX_INFO_LOAD(&info[1], PMIX_IOF_OUTPUT_TO_DIRECTORY, where, PMIX_STRING);
    PMIX_INFO_LOAD(&info[2], PMIX_USERID, &uid, PMIX_UINT32);
    PMIX_INFO_LOAD(&info[3], PMIX_GRPID, &gid, PMIX_UINT32);
    PMIX_DATA_ARRAY_CREATE(ugids, 2, PMIX_UINT32);
    ((uint32_t *) ugids->array)[0] = gid;
    ((uint32_t *) ugids->array)[1] = extra;
    PMIX_INFO_LOAD(&info[4], PMIX_USER_GRPIDS, ugids, PMIX_DATA_ARRAY);
    PMIX_DATA_ARRAY_FREE(ugids);
    PMIX_LOAD_NSPACE(ns, name);
    rc = PMIx_server_register_nspace(ns, 1, info, 5, NULL, NULL);
    if (PMIX_OPERATION_SUCCEEDED == rc) {
        rc = PMIX_SUCCESS;
    }
    progress_barrier();
    for (n = 0; n < 5; n++) {
        PMIX_INFO_DESTRUCT(&info[n]);
    }
    return rc;
}

/* Another user's job. Everything about its output is done as that user
 * - in a child that drops to it - so a server that cannot become the
 * owner writes nothing for it, and a root server writes only what the
 * owner could, owned by the owner from creation */
static void test_other_owner(void)
{
    char dir[1024], nsdir[1024], rankdir[1024], file[1024];

    snprintf(dir, sizeof(dir), "%s/other", base);
    if (0 == geteuid()) {
        /* somewhere the owner may write */
        if (0 != chmod(base, 0755) || 0 != mkdir(dir, 0755) ||
            0 != chown(dir, OTHER_UID, getegid())) {
            report("other: fixture", 0);
            return;
        }
    }
    if (PMIX_SUCCESS != reg("perms-other", PMIX_IOF_OUTPUT_TO_DIRECTORY, dir, OTHER_UID,
                            getegid(), (gid_t) -1)) {
        report("other: fixture", 0);
        return;
    }
    deliver("perms-other");
    paths(dir, "perms-other", nsdir, rankdir, file);
    if (0 == geteuid()) {
        report("another user's job: as root, the file is made by its owner, owner-only",
               mode_is(file, 0600) && owned_by(file, OTHER_UID, (gid_t) -1));
        report("another user's job: as root, its directories are the owner's alone",
               mode_is(rankdir, 0700) && owned_by(rankdir, OTHER_UID, (gid_t) -1));
    } else {
        report("another user's job: a server that cannot become the owner writes nothing",
               !exists(dir));
    }

    /* the owner's group is one we are not in */
    snprintf(dir, sizeof(dir), "%s/foreign", base);
    if (0 == geteuid()) {
        if (0 != mkdir(dir, 0755) || 0 != chown(dir, OTHER_UID, OTHER_GID)) {
            report("foreign: fixture", 0);
            return;
        }
    }
    if (PMIX_SUCCESS != reg("perms-foreign", PMIX_IOF_OUTPUT_TO_DIRECTORY, dir, OTHER_UID,
                            OTHER_GID, (gid_t) -1)) {
        report("foreign: fixture", 0);
        return;
    }
    deliver("perms-foreign");
    paths(dir, "perms-foreign", nsdir, rankdir, file);
    if (0 == geteuid()) {
        report("a group we are not in: as root, the file is the owner's, owner-only",
               mode_is(file, 0600) && owned_by(file, OTHER_UID, OTHER_GID));
    } else {
        report("a group we are not in: a server that cannot become the owner writes nothing",
               !exists(dir));
    }

    if (0 != geteuid()) {
        fprintf(stdout, "  SKIP: the cases a root server must refuse - not running as root\n");
        return;
    }

    /* an existing output file of someone else's, which the owner could
     * write - and a root server once truncated and gave to the owner */
    {
        char held[1024];
        FILE *fp;

        snprintf(dir, sizeof(dir), "%s/takeover", base);
        paths(dir, "perms-held", nsdir, rankdir, held);
        if (0 != mkdir(dir, 0755) || 0 != mkdir(nsdir, 0755) || 0 != mkdir(rankdir, 0755) ||
            0 != chown(dir, OTHER_UID, getegid()) || 0 != chown(nsdir, OTHER_UID, getegid()) ||
            0 != chown(rankdir, OTHER_UID, getegid()) || NULL == (fp = fopen(held, "w"))) {
            report("held: fixture", 0);
            return;
        }
        fputs("keep", fp);
        fclose(fp);
        (void) chmod(held, 0666);
        if (PMIX_SUCCESS != reg("perms-held", PMIX_IOF_OUTPUT_TO_DIRECTORY, dir, OTHER_UID,
                                getegid(), (gid_t) -1)) {
            report("held: fixture", 0);
            return;
        }
        deliver("perms-held");
        report("a file the owner does not own is refused and left as it was",
               holds(held, "keep") && owned_by(held, 0, (gid_t) -1) && mode_is(held, 0666));
    }

    /* a directory only root may write: the owner cannot make its output
     * there, and nor does a root server for it */
    snprintf(dir, sizeof(dir), "%s/rootonly", base);
    if (0 != mkdir(dir, 0755)) {
        report("rootonly: fixture", 0);
        return;
    }
    strncat(dir, "/out", sizeof(dir) - strlen(dir) - 1);
    if (PMIX_SUCCESS != reg("perms-rootonly", PMIX_IOF_OUTPUT_TO_DIRECTORY, dir, OTHER_UID,
                            getegid(), (gid_t) -1)) {
        report("rootonly: fixture", 0);
        return;
    }
    deliver("perms-rootonly");
    report("a directory only root may write is refused", !exists(dir));

    /* a directory the owner reaches only through a supplementary group -
     * one the host registered for it */
    snprintf(dir, sizeof(dir), "%s/grouponly", base);
    if (0 != mkdir(dir, 0770) || 0 != chown(dir, 0, OTHER_GID + 1) || 0 != chmod(dir, 0770)) {
        report("grouponly: fixture", 0);
        return;
    }
    if (PMIX_SUCCESS != reg_with_groups("perms-grouponly", dir, OTHER_UID, OTHER_GID,
                                        OTHER_GID + 1)) {
        report("grouponly: fixture", 0);
        return;
    }
    deliver("perms-grouponly");
    paths(dir, "perms-grouponly", nsdir, rankdir, file);
    report("a directory reached through a group the host registered for the owner works",
           owned_by(file, OTHER_UID, (gid_t) -1) && mode_is(file, 0600));
}

static void test_files(void)
{
    char prefix[1024], file[2048], other[2048], linked[2048];
    char buf[16] = {0};
    int fd;

    /* a file left by an earlier run, open to everyone */
    snprintf(prefix, sizeof(prefix), "%s/out", base);
    snprintf(file, sizeof(file), "%s.perms-file.0.out", prefix);
    fd = open(file, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (0 <= fd) {
        close(fd);
        (void) chmod(file, 0644);
    }
    if (PMIX_SUCCESS != reg("perms-file", PMIX_IOF_OUTPUT_TO_FILE, prefix, geteuid(), getegid(),
                            (gid_t) -1)) {
        report("files: fixture", 0);
        return;
    }
    deliver("perms-file");
    report("a file left by an earlier run is made owner-only", mode_is(file, 0600));

    /* the output file's name already leads to another file */
    snprintf(other, sizeof(other), "%s/other-file", base);
    snprintf(prefix, sizeof(prefix), "%s/lnk", base);
    snprintf(linked, sizeof(linked), "%s.perms-link.0.out", prefix);
    fd = open(other, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (0 > fd || 4 != write(fd, "keep", 4)) {
        report("link: fixture", 0);
        return;
    }
    close(fd);
    if (0 != link(other, linked)) {
        report("link: fixture", 0);
        return;
    }
    if (PMIX_SUCCESS != reg("perms-link", PMIX_IOF_OUTPUT_TO_FILE, prefix, geteuid(), getegid(),
                            (gid_t) -1)) {
        report("link: fixture", 0);
        return;
    }
    deliver("perms-link");
    fd = open(other, O_RDONLY);
    if (0 <= fd) {
        if (0 > read(fd, buf, sizeof(buf) - 1)) {
            buf[0] = '\0';
        }
        close(fd);
    }
    report("a file reached through a second name is not written",
           0 == strcmp(buf, "keep") && mode_is(other, 0644));
}

/* a job of ours whose output files "writer" writes */
static pmix_status_t reg_with_writer(const char *name, const char *where, const pmix_proc_t *writer)
{
    pmix_info_t info[5];
    pmix_nspace_t ns;
    pmix_status_t rc;
    uint32_t one = 1, uid = (uint32_t) geteuid(), gid = (uint32_t) getegid();
    size_t n;

    PMIX_INFO_LOAD(&info[0], PMIX_JOB_SIZE, &one, PMIX_UINT32);
    PMIX_INFO_LOAD(&info[1], PMIX_IOF_OUTPUT_TO_DIRECTORY, where, PMIX_STRING);
    PMIX_INFO_LOAD(&info[2], PMIX_USERID, &uid, PMIX_UINT32);
    PMIX_INFO_LOAD(&info[3], PMIX_GRPID, &gid, PMIX_UINT32);
    PMIX_INFO_LOAD(&info[4], PMIX_IOF_FILE_WRITER, writer, PMIX_PROC);
    PMIX_LOAD_NSPACE(ns, name);
    rc = PMIx_server_register_nspace(ns, 1, info, 5, NULL, NULL);
    if (PMIX_OPERATION_SUCCEEDED == rc) {
        rc = PMIX_SUCCESS;
    }
    progress_barrier();
    for (n = 0; n < 5; n++) {
        PMIX_INFO_DESTRUCT(&info[n]);
    }
    return rc;
}

static void test_writer(void)
{
    char dir[1024], nsdir[1024], rankdir[1024], file[1024];
    pmix_proc_t tool;

    /* a tool's, which is sent the output and writes the files itself */
    PMIX_LOAD_PROCID(&tool, "perms-launcher", 0);
    snprintf(dir, sizeof(dir), "%s/elsewhere", base);
    if (0 != mkdir(dir, 0700) ||
        PMIX_SUCCESS != reg_with_writer("perms-elsewhere", dir, &tool)) {
        report("writer: fixture", 0);
        return;
    }
    deliver("perms-elsewhere");
    paths(dir, "perms-elsewhere", nsdir, rankdir, file);
    report("a job whose files another process writes gets none from the server",
           !exists(nsdir));

    /* the server's own */
    snprintf(dir, sizeof(dir), "%s/self", base);
    if (0 != mkdir(dir, 0700) ||
        PMIX_SUCCESS != reg_with_writer("perms-self", dir, &pmix_globals.myid)) {
        report("writer self: fixture", 0);
        return;
    }
    deliver("perms-self");
    paths(dir, "perms-self", nsdir, rankdir, file);
    report("a job whose files the server itself writes gets them",
           holds(file, "hello\n") && mode_is(file, 0600));
}

int main(int argc, char **argv)
{
    static pmix_server_module_t mymodule = {0};
    const char *tmp = getenv("TMPDIR");
    char cmd[1100];
    pmix_status_t rc;

    (void) argc;
    (void) argv;

    setvbuf(stdout, NULL, _IONBF, 0);
    fprintf(stdout, "iof_file_perms: who owns and reads a job's output files\n");

    /* what the modes below are measured against */
    umask(022);
    snprintf(base, sizeof(base), "%s/iofperms.XXXXXX", (NULL == tmp) ? "/tmp" : tmp);
    if (NULL == mkdtemp(base)) {
        fprintf(stderr, "mkdtemp failed: %s\n", strerror(errno));
        return 1;
    }
    rc = PMIx_server_init(&mymodule, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    test_own_job();
    test_listed_group();
    test_other_owner();
    test_files();
    test_writer();

    PMIx_server_finalize();
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", base);
    if (0 != system(cmd)) {
        fprintf(stderr, "could not remove %s\n", base);
    }

    fprintf(stdout, "iof_file_perms: %d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
