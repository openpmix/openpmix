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

    /* Remove the test root; all subdirectories were cleaned up above. */
    rmdir(tmpbase);

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);
    return (nfail > 0) ? 1 : 0;
}
