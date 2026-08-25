/*
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit tests for pmix_os_dirpath utility functions:
 *   pmix_os_dirpath_create, pmix_os_dirpath_is_empty,
 *   pmix_os_dirpath_access, pmix_os_dirpath_destroy.
 *
 * A temporary directory is created under /tmp for each test and
 * removed by the test itself.
 *
 * Exit 0 if all tests pass, 1 otherwise.
 */

#include "src/include/pmix_config.h"
#include "src/include/pmix_globals.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif
#ifdef HAVE_SYS_STAT_H
#    include <sys/stat.h>
#endif

#include <fcntl.h>

#include "pmix.h"
#include "src/util/pmix_os_dirpath.h"

static int npass = 0;
static int nfail = 0;

/* Root temp directory created once in main(). */
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

static int dir_exists(const char *path)
{
    struct stat st;
    return (0 == stat(path, &st) && S_ISDIR(st.st_mode));
}

/* ------------------------------------------------------------------ */
/* pmix_os_dirpath_create                                              */
/* ------------------------------------------------------------------ */

static void test_create_null(void)
{
    int rc = pmix_os_dirpath_create(NULL, S_IRWXU);
    report("create_null: returns ERR_BAD_PARAM", PMIX_ERR_BAD_PARAM == rc);
}

/* An empty path names nothing, but mkdir("") answers ENOENT - the same
 * errno that means "the parent components are missing, go build the
 * tree". An empty path then splits to no components at all, the build
 * loop never runs, and the function reported that it had created a
 * directory tree while doing nothing at all. */
static void test_create_empty(void)
{
    int rc = pmix_os_dirpath_create("", S_IRWXU);
    report("create_empty: returns ERR_BAD_PARAM", PMIX_ERR_BAD_PARAM == rc);
}

static void test_create_new(void)
{
    char path[512];
    int rc;

    snprintf(path, sizeof(path), "%s/newdir", tmpbase);
    rc = pmix_os_dirpath_create(path, S_IRWXU);
    report("create_new: returns SUCCESS", PMIX_SUCCESS == rc);
    report("create_new: directory exists", dir_exists(path));
    rmdir(path);
}

static void test_create_existing(void)
{
    /* tmpbase already exists */
    int rc = pmix_os_dirpath_create(tmpbase, S_IRWXU);
    report("create_existing: returns ERR_EXISTS", PMIX_ERR_EXISTS == rc);
}

static void test_create_nested(void)
{
    char path[512];
    int rc;

    snprintf(path, sizeof(path), "%s/a/b/c", tmpbase);
    rc = pmix_os_dirpath_create(path, S_IRWXU);
    report("create_nested: returns SUCCESS", PMIX_SUCCESS == rc);
    report("create_nested: leaf directory exists", dir_exists(path));

    /* Cleanup bottom-up. */
    rmdir(path);
    snprintf(path, sizeof(path), "%s/a/b", tmpbase);
    rmdir(path);
    snprintf(path, sizeof(path), "%s/a", tmpbase);
    rmdir(path);
}

/* ------------------------------------------------------------------ */
/* pmix_os_dirpath_is_empty                                            */
/* ------------------------------------------------------------------ */

static void test_is_empty_null(void)
{
    /* NULL path: the implementation falls through to return true */
    report("is_empty_null: returns true", pmix_os_dirpath_is_empty(NULL));
}

static void test_is_empty_true(void)
{
    char path[512];

    snprintf(path, sizeof(path), "%s/emptydir", tmpbase);
    mkdir(path, S_IRWXU);
    report("is_empty_true: empty dir", pmix_os_dirpath_is_empty(path));
    rmdir(path);
}

static void test_is_empty_false(void)
{
    char path[512];
    char file[512];
    FILE *f;

    snprintf(path, sizeof(path), "%s/nonempty", tmpbase);
    snprintf(file, sizeof(file), "%s/nonempty/x", tmpbase);
    mkdir(path, S_IRWXU);
    f = fopen(file, "w");
    if (NULL != f) {
        fclose(f);
    }
    report("is_empty_false: dir with file", !pmix_os_dirpath_is_empty(path));
    unlink(file);
    rmdir(path);
}

/* ------------------------------------------------------------------ */
/* pmix_os_dirpath_access                                              */
/* ------------------------------------------------------------------ */

static void test_access_always_ok(void)
{
    /* This is a stale compatibility stub; it always returns PMIX_SUCCESS. */
    report("access_always_ok", PMIX_SUCCESS == pmix_os_dirpath_access("/tmp", S_IRWXU));
}

/* ------------------------------------------------------------------ */
/* pmix_os_dirpath_destroy                                             */
/* ------------------------------------------------------------------ */

static void test_destroy_null(void)
{
    int rc = pmix_os_dirpath_destroy(NULL, false, NULL);
    report("destroy_null: returns PMIX_ERROR", PMIX_ERROR == rc);
}

static void test_destroy_empty(void)
{
    char path[512];
    int rc;

    snprintf(path, sizeof(path), "%s/destroyme", tmpbase);
    mkdir(path, S_IRWXU);
    rc = pmix_os_dirpath_destroy(path, false, NULL);
    report("destroy_empty: returns SUCCESS", PMIX_SUCCESS == rc);
    report("destroy_empty: directory removed", !dir_exists(path));
}

static void test_destroy_with_files(void)
{
    char path[512];
    char file[512];
    FILE *f;
    int rc;

    snprintf(path, sizeof(path), "%s/withfiles", tmpbase);
    snprintf(file, sizeof(file), "%s/withfiles/f1", tmpbase);
    mkdir(path, S_IRWXU);
    f = fopen(file, "w");
    if (NULL != f) {
        fclose(f);
    }
    rc = pmix_os_dirpath_destroy(path, false, NULL);
    report("destroy_with_files: returns SUCCESS", PMIX_SUCCESS == rc);
    report("destroy_with_files: directory removed", !dir_exists(path));
}

static void test_destroy_recursive(void)
{
    char path[512];
    char sub[512];
    char file[512];
    FILE *f;
    int rc;

    snprintf(path, sizeof(path), "%s/recursive", tmpbase);
    snprintf(sub,  sizeof(sub),  "%s/recursive/sub", tmpbase);
    snprintf(file, sizeof(file), "%s/recursive/sub/f", tmpbase);
    mkdir(path, S_IRWXU);
    mkdir(sub,  S_IRWXU);
    f = fopen(file, "w");
    if (NULL != f) {
        fclose(f);
    }
    rc = pmix_os_dirpath_destroy(path, true, NULL);
    report("destroy_recursive: returns SUCCESS", PMIX_SUCCESS == rc);
    report("destroy_recursive: tree removed", !dir_exists(path));
}

/* ------------------------------------------------------------------ */
/* Symlink / non-directory hardening                                   */
/*                                                                     */
/* pmix_os_dirpath_create() must not adopt (or chmod) something that   */
/* is not a directory, and pmix_os_dirpath_destroy() must never follow */
/* a symlink -- either as its base path or as an entry it finds while  */
/* walking the tree.  The session and rendezvous directories have      */
/* fully predictable names under a world-writable root, so a link      */
/* planted at one of those names would otherwise redirect a chmod, or  */
/* an entire recursive removal, at a tree of the planter's choosing.   */
/* ------------------------------------------------------------------ */

static int file_exists(const char *path)
{
    struct stat st;
    return (0 == stat(path, &st) && S_ISREG(st.st_mode));
}

static int is_symlink(const char *path)
{
    struct stat st;
    return (0 == lstat(path, &st) && S_ISLNK(st.st_mode));
}

static void make_file(const char *path)
{
    FILE *f = fopen(path, "w");
    if (NULL != f) {
        fclose(f);
    }
}

static void test_create_on_file(void)
{
    char path[512];
    int rc;

    snprintf(path, sizeof(path), "%s/plainfile", tmpbase);
    make_file(path);

    rc = pmix_os_dirpath_create(path, S_IRWXU);
    report("create_on_file: refused with ERR_SILENT", PMIX_ERR_SILENT == rc);
    report("create_on_file: file left in place", file_exists(path));

    unlink(path);
}

static void test_create_on_symlink(void)
{
    char target[512];
    char link[512];
    struct stat st;
    int rc;

    snprintf(target, sizeof(target), "%s/link_target", tmpbase);
    snprintf(link, sizeof(link), "%s/dirlink", tmpbase);
    mkdir(target, S_IRWXU);
    if (0 != symlink(target, link)) {
        report("create_on_symlink: SKIPPED (symlink unavailable)", 1);
        rmdir(target);
        return;
    }

    /* ask for group bits the target does not have: if the link were
     * followed, the target would come back chmod'ed */
    rc = pmix_os_dirpath_create(link, S_IRWXU | S_IRWXG);
    report("create_on_symlink: refused with ERR_SILENT", PMIX_ERR_SILENT == rc);
    report("create_on_symlink: link not replaced", is_symlink(link));
    report("create_on_symlink: target mode untouched",
           0 == stat(target, &st) && 0 == (st.st_mode & S_IRWXG));

    unlink(link);
    rmdir(target);
}

static void test_destroy_does_not_follow_symlink(void)
{
    char victim[512];
    char victimfile[512];
    char base[512];
    char link[512];
    int rc;

    snprintf(victim, sizeof(victim), "%s/victim", tmpbase);
    snprintf(victimfile, sizeof(victimfile), "%s/victim/precious", tmpbase);
    snprintf(base, sizeof(base), "%s/walkme", tmpbase);
    snprintf(link, sizeof(link), "%s/walkme/escape", tmpbase);
    mkdir(victim, S_IRWXU);
    make_file(victimfile);
    mkdir(base, S_IRWXU);
    if (0 != symlink(victim, link)) {
        report("destroy_does_not_follow_symlink: SKIPPED (symlink unavailable)", 1);
        unlink(victimfile);
        rmdir(victim);
        rmdir(base);
        return;
    }

    rc = pmix_os_dirpath_destroy(base, true, NULL);
    report("destroy_does_not_follow_symlink: returns SUCCESS", PMIX_SUCCESS == rc);
    report("destroy_does_not_follow_symlink: tree removed", !dir_exists(base));
    report("destroy_does_not_follow_symlink: target survives", dir_exists(victim));
    report("destroy_does_not_follow_symlink: target contents survive",
           file_exists(victimfile));

    unlink(victimfile);
    rmdir(victim);
}

static void test_destroy_symlink_base(void)
{
    char victim[512];
    char victimfile[512];
    char link[512];
    int rc;

    snprintf(victim, sizeof(victim), "%s/basevictim", tmpbase);
    snprintf(victimfile, sizeof(victimfile), "%s/basevictim/precious", tmpbase);
    snprintf(link, sizeof(link), "%s/baselink", tmpbase);
    mkdir(victim, S_IRWXU);
    make_file(victimfile);
    if (0 != symlink(victim, link)) {
        report("destroy_symlink_base: SKIPPED (symlink unavailable)", 1);
        unlink(victimfile);
        rmdir(victim);
        return;
    }

    rc = pmix_os_dirpath_destroy(link, true, NULL);
    report("destroy_symlink_base: refused", PMIX_SUCCESS != rc);
    report("destroy_symlink_base: target survives", dir_exists(victim));
    report("destroy_symlink_base: target contents survive", file_exists(victimfile));

    unlink(link);
    unlink(victimfile);
    rmdir(victim);
}

static void test_is_empty_symlink(void)
{
    char target[512];
    char link[512];

    snprintf(target, sizeof(target), "%s/empty_target", tmpbase);
    snprintf(link, sizeof(link), "%s/emptylink", tmpbase);
    mkdir(target, S_IRWXU);
    if (0 != symlink(target, link)) {
        report("is_empty_symlink: SKIPPED (symlink unavailable)", 1);
        rmdir(target);
        return;
    }

    /* the target is empty, but a symlink must not answer on its
     * behalf: callers use this to decide whether to remove the path */
    report("is_empty_symlink: symlink reported not empty",
           !pmix_os_dirpath_is_empty(link));

    unlink(link);
    rmdir(target);
}

static void test_destroy_nonrecursive_with_subdir(void)
{
    char base[512];
    char sub[512];
    char subfile[512];
    char topfile[512];
    int rc;

    snprintf(base, sizeof(base), "%s/nonrec", tmpbase);
    snprintf(sub, sizeof(sub), "%s/nonrec/sub", tmpbase);
    snprintf(subfile, sizeof(subfile), "%s/nonrec/sub/f", tmpbase);
    snprintf(topfile, sizeof(topfile), "%s/nonrec/topf", tmpbase);
    mkdir(base, S_IRWXU);
    mkdir(sub, S_IRWXU);
    make_file(subfile);
    make_file(topfile);

    rc = pmix_os_dirpath_destroy(base, false, NULL);
    report("destroy_nonrecursive_with_subdir: returns PMIX_ERROR", PMIX_ERROR == rc);
    report("destroy_nonrecursive_with_subdir: top-level file removed",
           !file_exists(topfile));
    report("destroy_nonrecursive_with_subdir: subdirectory preserved", dir_exists(sub));
    report("destroy_nonrecursive_with_subdir: subdirectory contents preserved",
           file_exists(subfile));

    unlink(subfile);
    rmdir(sub);
    rmdir(base);
}

static bool veto_keepme(const char *root, const char *path)
{
    (void) root;
    /* refuse to remove the file named "keepme" wherever it is found */
    return (0 != strcmp(path, "keepme"));
}

static void test_destroy_callback_veto_in_subdir(void)
{
    char base[512];
    char sub[512];
    char keep[512];
    char drop[512];
    char topfile[512];
    int rc;

    snprintf(base, sizeof(base), "%s/veto", tmpbase);
    snprintf(sub, sizeof(sub), "%s/veto/sub", tmpbase);
    snprintf(keep, sizeof(keep), "%s/veto/sub/keepme", tmpbase);
    snprintf(drop, sizeof(drop), "%s/veto/sub/dropme", tmpbase);
    snprintf(topfile, sizeof(topfile), "%s/veto/topf", tmpbase);
    mkdir(base, S_IRWXU);
    mkdir(sub, S_IRWXU);
    make_file(keep);
    make_file(drop);
    make_file(topfile);

    rc = pmix_os_dirpath_destroy(base, true, veto_keepme);
    report("destroy_callback_veto_in_subdir: returns SUCCESS", PMIX_SUCCESS == rc);
    report("destroy_callback_veto_in_subdir: vetoed file preserved", file_exists(keep));
    report("destroy_callback_veto_in_subdir: parent of vetoed file preserved",
           dir_exists(sub));
    report("destroy_callback_veto_in_subdir: sibling removed", !file_exists(drop));
    report("destroy_callback_veto_in_subdir: top-level file removed",
           !file_exists(topfile));

    unlink(keep);
    rmdir(sub);
    rmdir(base);
}

/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* pmix_os_dirpath_create_under / _open_file_under                     */
/* ------------------------------------------------------------------ */

/* The root is taken as given and must still work when it is reached
 * through a symlink - which is not an exotic case: /tmp and /var are
 * themselves symlinks on macOS, so the default PMIx temporary directory
 * is one. A walk that declined a link at every component would decline
 * the platform. */
static void test_under_trusted_root_may_be_a_symlink(void)
{
    char real[512], link[512], leaf[600];
    int rc;

    snprintf(real, sizeof(real), "%s/realroot", tmpbase);
    snprintf(link, sizeof(link), "%s/linkroot", tmpbase);
    if (0 != mkdir(real, S_IRWXU)) {
        report("under: fixture", 0);
        return;
    }
    unlink(link);
    if (0 != symlink(real, link)) {
        report("under: fixture", 0);
        return;
    }

    rc = pmix_os_dirpath_create_under(link, "ns/rank.0", S_IRWXU);
    report("under: symlinked root is accepted", PMIX_SUCCESS == rc);
    snprintf(leaf, sizeof(leaf), "%s/ns/rank.0", real);
    report("under: tree built through it", dir_exists(leaf));

    rmdir(leaf);
    snprintf(leaf, sizeof(leaf), "%s/ns", real);
    rmdir(leaf);
    unlink(link);
    rmdir(real);
}

/* ...but a component of the tail is a name PMIx composes itself, so a
 * symlink there is not one this code put in place. Building through it
 * would put the tree - and every output file in it - somewhere the
 * caller never named. */
static void test_under_declines_a_tail_symlink(void)
{
    char root[512], linkname[600], elsewhere[512], leaf[600];
    int rc, fd;

    snprintf(root, sizeof(root), "%s/troot", tmpbase);
    snprintf(elsewhere, sizeof(elsewhere), "%s/elsewhere", tmpbase);
    if (0 != mkdir(root, S_IRWXU) || 0 != mkdir(elsewhere, S_IRWXU)) {
        report("under: fixture", 0);
        return;
    }
    /* something else reached the namespace component first */
    snprintf(linkname, sizeof(linkname), "%s/ns", root);
    if (0 != symlink(elsewhere, linkname)) {
        report("under: fixture", 0);
        return;
    }

    rc = pmix_os_dirpath_create_under(root, "ns/rank.0", S_IRWXU);
    report("under: tail symlink declined", PMIX_SUCCESS != rc);
    snprintf(leaf, sizeof(leaf), "%s/rank.0", elsewhere);
    report("under: nothing built past the link", !dir_exists(leaf));

    /* and the file open makes the same decision on its own account */
    fd = pmix_os_dirpath_open_file_under(root, "ns/rank.0/stdout",
                                         O_CREAT | O_RDWR | O_TRUNC, 0644);
    report("under: file not opened past the link", 0 > fd);
    if (0 <= fd) {
        close(fd);
    }

    rmdir(leaf);
    unlink(linkname);
    rmdir(elsewhere);
    rmdir(root);
}

/* More than one composed level below the root - which is what an IOF
 * pattern like "%h/%n/rank-%R" produces - has to be built a component at
 * a time all the way down, not just at the last one. A symlink at the
 * FIRST composed level is the case a single O_NOFOLLOW never sees: it
 * applies to the end of the name, and by then the resolution has already
 * gone elsewhere. */
static void test_under_declines_a_midway_symlink(void)
{
    char root[512], linkname[600], elsewhere[512], leaf[600];
    int rc;

    snprintf(root, sizeof(root), "%s/mroot", tmpbase);
    snprintf(elsewhere, sizeof(elsewhere), "%s/melsewhere", tmpbase);
    if (0 != mkdir(root, S_IRWXU) || 0 != mkdir(elsewhere, S_IRWXU)) {
        report("midway: fixture", 0);
        return;
    }
    /* the first of three composed levels is already a link */
    snprintf(linkname, sizeof(linkname), "%s/host", root);
    if (0 != symlink(elsewhere, linkname)) {
        report("midway: fixture", 0);
        return;
    }

    rc = pmix_os_dirpath_create_under(root, "host/ns/rank.0", S_IRWXU);
    report("midway: symlink at the first level declined", PMIX_SUCCESS != rc);
    snprintf(leaf, sizeof(leaf), "%s/ns", elsewhere);
    report("midway: nothing built past it", !dir_exists(leaf));

    snprintf(leaf, sizeof(leaf), "%s/ns/rank.0", elsewhere);
    rmdir(leaf);
    snprintf(leaf, sizeof(leaf), "%s/ns", elsewhere);
    rmdir(leaf);
    unlink(linkname);
    rmdir(elsewhere);
    rmdir(root);
}

int main(int argc, char **argv)
{
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    snprintf(tmpbase, sizeof(tmpbase), "/tmp/pmix_unit_XXXXXX");
    if (NULL == mkdtemp(tmpbase)) {
        fprintf(stderr, "FATAL: mkdtemp failed\n");
        return 1;
    }

    fprintf(stdout, "\n=== pmix_os_dirpath unit tests ===\n\n");

    test_create_null();
    test_create_empty();
    test_under_trusted_root_may_be_a_symlink();
    test_under_declines_a_tail_symlink();
    test_under_declines_a_midway_symlink();
    test_create_new();
    test_create_existing();
    test_create_nested();

    test_is_empty_null();
    test_is_empty_true();
    test_is_empty_false();

    test_access_always_ok();

    test_destroy_null();
    test_destroy_empty();
    test_destroy_with_files();
    test_destroy_recursive();

    test_create_on_file();
    test_create_on_symlink();
    test_destroy_does_not_follow_symlink();
    test_destroy_symlink_base();
    test_is_empty_symlink();
    test_destroy_nonrecursive_with_subdir();
    test_destroy_callback_veto_in_subdir();

    /* Remove the test root; all subdirectories were cleaned up above. */
    rmdir(tmpbase);

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);
    return (nfail > 0) ? 1 : 0;
}
