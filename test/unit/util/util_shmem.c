/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit tests for the backing-file handling in src/util/pmix_shmem.c.
 *
 * A segment's backing path is composed by PMIx out of its own naming
 * scheme, so the name may already be occupied when the segment is
 * created, and what is sitting there decides what has to happen:
 *
 *   - a regular file is left over from an earlier run - the name carries
 *     a pid, and pids get reused - and has to be reclaimed, or the
 *     segment cannot be created at all;
 *   - a symlink is not something this code put there, and must not be
 *     followed: the operation would land on some unrelated file instead
 *     of on the segment, truncating and re-permissioning it.
 *
 * Exit 0 if all tests pass, 1 otherwise.
 */

#include "src/include/pmix_config.h"
#include "src/include/pmix_globals.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif
#include <fcntl.h>
#ifdef HAVE_SYS_STAT_H
#    include <sys/stat.h>
#endif

#include "pmix_common.h"
#include "src/util/pmix_shmem.h"
#include "src/util/pmix_string_copy.h"

static int npass = 0;
static int nfail = 0;
static char tmpbase[256];

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

/* An unrelated file, with contents and permissions we can recognize
 * again afterwards. */
static int make_bystander(const char *path, struct stat *before)
{
    static const char content[] = "UNRELATED CONTENT";
    int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0600);

    if (0 > fd) {
        return -1;
    }
    if ((ssize_t) sizeof(content) != write(fd, content, sizeof(content))) {
        close(fd);
        return -1;
    }
    close(fd);
    return stat(path, before);
}

/* A symlink sitting at the backing path must not carry the segment
 * creation through to whatever it names. open() with O_CREAT | O_TRUNC
 * and no O_NOFOLLOW followed it, and chmod() followed it again - while
 * the lchown() between them does not, so the three calls acted on two
 * different objects. */
static void test_create_does_not_follow_a_symlink(void)
{
    char other[512], seg[512];
    struct stat before, after;
    pmix_shmem_t *shmem;

    snprintf(other, sizeof(other), "%s/other", tmpbase);
    snprintf(seg, sizeof(seg), "%s/linked.seg", tmpbase);

    if (0 != make_bystander(other, &before)) {
        report("symlink: fixture", 0);
        return;
    }
    unlink(seg);
    if (0 != symlink(other, seg)) {
        report("symlink: fixture", 0);
        return;
    }

    shmem = PMIX_NEW(pmix_shmem_t);
    if (NULL == shmem) {
        report("symlink: fixture", 0);
        return;
    }
    /* Whether the create succeeds is deliberately not asserted: it may
     * decline outright, or it may discard the link and build its own
     * file at that name. Both are correct. What may not happen is
     * anything at all to the file the link names. */
    (void) pmix_shmem_segment_create(shmem, 4096, seg, 1);
    (void) pmix_shmem_segment_chmod(shmem, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);

    if (0 != stat(other, &after)) {
        report("symlink: other file still exists", 0);
    } else {
        report("symlink: other file not truncated", before.st_size == after.st_size);
        report("symlink: other file mode unchanged",
               (before.st_mode & 07777) == (after.st_mode & 07777));
    }

    PMIX_RELEASE(shmem);
    unlink(seg);
    unlink(other);
}

/* The other half of the same decision: a leftover file really does have
 * to be reclaimed, or a reused pid leaves a server unable to create its
 * segment. A change that simply declined whatever was already at the
 * name would pass the test above and break this one. */
static void test_create_reclaims_a_stale_file(void)
{
    char seg[512];
    pmix_shmem_t *shmem;
    char buf[8] = {0};
    int fd, rc;

    snprintf(seg, sizeof(seg), "%s/stale.seg", tmpbase);
    unlink(seg);

    fd = open(seg, O_CREAT | O_WRONLY | O_TRUNC, 0600);
    if (0 > fd || 4 != write(fd, "STAL", 4)) {
        report("stale: fixture", 0);
        if (0 <= fd) {
            close(fd);
        }
        return;
    }
    close(fd);

    shmem = PMIX_NEW(pmix_shmem_t);
    if (NULL == shmem) {
        report("stale: fixture", 0);
        return;
    }
    rc = pmix_shmem_segment_create(shmem, 4096, seg, 1);
    report("stale: leftover file reclaimed", PMIX_SUCCESS == rc);

    /* and its bytes are gone rather than left under the new segment -
     * gds/shmem3's allocator hands out storage it never writes, on the
     * strength of a fresh segment reading as zero */
    fd = open(seg, O_RDONLY);
    if (0 > fd || 4 != read(fd, buf, 4)) {
        report("stale: leftover bytes replaced", 0);
    } else {
        report("stale: leftover bytes replaced", 0 != memcmp(buf, "STAL", 4));
    }
    if (0 <= fd) {
        close(fd);
    }

    PMIX_RELEASE(shmem);
    unlink(seg);
}

/* pmix_shmem_segment_chmod() is a public entry point of its own, and
 * gds/shmem3 calls it (through shmem3_segment_fix_perms) separately from
 * the create - so it has to make the same decision on its own account
 * rather than relying on the create having already settled it. chmod(2)
 * follows a symlink at the last component; its neighbour lchown(2) does
 * not, so the two acted on different objects. Drive it directly, since a
 * create beforehand replaces the link and leaves nothing to follow. */
static void test_chmod_does_not_follow_a_symlink(void)
{
    char other[512], seg[512];
    struct stat before, after;
    pmix_shmem_t *shmem;

    snprintf(other, sizeof(other), "%s/chmod-other", tmpbase);
    snprintf(seg, sizeof(seg), "%s/chmod-linked.seg", tmpbase);

    if (0 != make_bystander(other, &before)) {
        report("chmod: fixture", 0);
        return;
    }
    unlink(seg);
    if (0 != symlink(other, seg)) {
        report("chmod: fixture", 0);
        return;
    }

    shmem = PMIX_NEW(pmix_shmem_t);
    if (NULL == shmem) {
        report("chmod: fixture", 0);
        return;
    }
    pmix_string_copy(shmem->backing_path, seg, PMIX_PATH_MAX);
    (void) pmix_shmem_segment_chmod(shmem, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);

    if (0 != stat(other, &after)) {
        report("chmod: other file still exists", 0);
    } else {
        report("chmod: other file mode unchanged",
               (before.st_mode & 07777) == (after.st_mode & 07777));
    }

    PMIX_RELEASE(shmem);
    unlink(seg);
    unlink(other);
}

int main(int argc, char **argv)
{
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    fprintf(stdout, "\n=== pmix_shmem unit tests ===\n\n");

    snprintf(tmpbase, sizeof(tmpbase), "/tmp/pmix-util-shmem-test-%u",
             (unsigned) getpid());
    if (0 != mkdir(tmpbase, S_IRWXU)) {
        fprintf(stderr, "could not create %s: %s\n", tmpbase, strerror(errno));
        return 1;
    }

    test_create_does_not_follow_a_symlink();
    test_create_reclaims_a_stale_file();
    test_chmod_does_not_follow_a_symlink();

    rmdir(tmpbase);

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);
    return (nfail > 0) ? 1 : 0;
}
