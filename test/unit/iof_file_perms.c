/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Who owns and reads a job's output files - see docs/security-plan.rst,
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
 *   another user's job              -> as root, the files are given to
 *                                      the owner, 0600; otherwise
 *                                      readable by the owner's group
 *                                      (0640) when this server can set
 *                                      it, and by everyone (0644) when it
 *                                      cannot - the owner must still be
 *                                      able to read its output
 *   a file left by an earlier run   -> made owner-only
 *   a file with a second name       -> not written
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

static void test_other_owner(void)
{
    char dir[1024], nsdir[1024], rankdir[1024], file[1024];

    /* the owner's group is one we are in, so we can set it */
    snprintf(dir, sizeof(dir), "%s/other", base);
    if (PMIX_SUCCESS != reg("perms-other", PMIX_IOF_OUTPUT_TO_DIRECTORY, dir, OTHER_UID,
                            getegid(), (gid_t) -1)) {
        report("other: fixture", 0);
        return;
    }
    deliver("perms-other");
    paths(dir, "perms-other", nsdir, rankdir, file);
    if (0 == geteuid()) {
        report("another user's job: as root, the file is given to its owner, owner-only",
               mode_is(file, 0600) && owned_by(file, OTHER_UID, (gid_t) -1));
    } else {
        report("another user's job: the file is readable by the owner's group",
               mode_is(file, 0640) && owned_by(file, geteuid(), getegid()));
    }
    report("another user's job: its directories can be entered", mode_is(rankdir, 0755));

    /* the owner's group is one we are not in */
    snprintf(dir, sizeof(dir), "%s/foreign", base);
    if (PMIX_SUCCESS != reg("perms-foreign", PMIX_IOF_OUTPUT_TO_DIRECTORY, dir, OTHER_UID,
                            OTHER_GID, (gid_t) -1)) {
        report("foreign: fixture", 0);
        return;
    }
    deliver("perms-foreign");
    paths(dir, "perms-foreign", nsdir, rankdir, file);
    if (0 == geteuid()) {
        report("a group we cannot set: as root, the file is given to its owner, owner-only",
               mode_is(file, 0600) && owned_by(file, OTHER_UID, OTHER_GID));
    } else {
        report("a group we cannot set: the file is readable by everyone",
               mode_is(file, 0644));
    }
}

static void test_files(void)
{
    char prefix[1024], file[2048], victim[2048], linked[2048];
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
    snprintf(victim, sizeof(victim), "%s/victim", base);
    snprintf(prefix, sizeof(prefix), "%s/lnk", base);
    snprintf(linked, sizeof(linked), "%s.perms-link.0.out", prefix);
    fd = open(victim, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (0 > fd || 4 != write(fd, "keep", 4)) {
        report("link: fixture", 0);
        return;
    }
    close(fd);
    if (0 != link(victim, linked)) {
        report("link: fixture", 0);
        return;
    }
    if (PMIX_SUCCESS != reg("perms-link", PMIX_IOF_OUTPUT_TO_FILE, prefix, geteuid(), getegid(),
                            (gid_t) -1)) {
        report("link: fixture", 0);
        return;
    }
    deliver("perms-link");
    fd = open(victim, O_RDONLY);
    if (0 <= fd) {
        if (0 > read(fd, buf, sizeof(buf) - 1)) {
            buf[0] = '\0';
        }
        close(fd);
    }
    report("a file reached through a second name is not written",
           0 == strcmp(buf, "keep") && mode_is(victim, 0644));
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

    PMIx_server_finalize();
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", base);
    if (0 != system(cmd)) {
        fprintf(stderr, "could not remove %s\n", base);
    }

    fprintf(stdout, "iof_file_perms: %d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
