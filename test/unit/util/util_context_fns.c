/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit tests for pmix_context_fns utility:
 *   pmix_util_check_context_cwd, pmix_util_check_context_app.
 *
 * PMIx_Init is called first; PMIX_ERR_UNREACH is treated as normal.
 *
 * Exit 0 if all tests pass, 1 otherwise.
 */

#include "src/include/pmix_config.h"
#include "src/include/pmix_globals.h"

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "include/pmix_deprecated.h"
#include "pmix.h"
#include "src/util/pmix_context_fns.h"

static int npass = 0;
static int nfail = 0;

/* Absolute path to a known executable found at startup. */
static char test_exe[512] = {0};

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

/* ------------------------------------------------------------------ */
/* pmix_util_check_context_cwd                                        */
/* ------------------------------------------------------------------ */

static void test_check_cwd_null_incwd(void)
{
    /* NULL pointer-to-pointer → bad parameter. */
    pmix_status_t rc = pmix_util_check_context_cwd(NULL, false, false);
    report("check_cwd_null_incwd: ERR_BAD_PARAM", PMIX_ERR_BAD_PARAM == rc);
}

static void test_check_cwd_null_string(void)
{
    /* Pointer-to-NULL string → SUCCESS (no cwd needed). */
    char *p = NULL;
    pmix_status_t rc = pmix_util_check_context_cwd(&p, false, false);
    report("check_cwd_null_string: SUCCESS", PMIX_SUCCESS == rc);
}

static void test_check_cwd_no_chdir(void)
{
    /* want_chdir=false → always SUCCESS regardless of path. */
    char *p = strdup("/nonexistent_xyz_abc_123");
    pmix_status_t rc = pmix_util_check_context_cwd(&p, false, false);
    report("check_cwd_no_chdir: SUCCESS for any path", PMIX_SUCCESS == rc);
    free(p);
}

static void test_check_cwd_chdir_success(void)
{
    char saved[512];
    char *p;
    pmix_status_t rc;

    if (NULL == getcwd(saved, sizeof(saved))) {
        report("check_cwd_chdir_success: getcwd", 0);
        return;
    }
    p = strdup("/tmp");
    rc = pmix_util_check_context_cwd(&p, true, false);
    report("check_cwd_chdir: /tmp succeeds", PMIX_SUCCESS == rc);
    free(p);
    (void) chdir(saved); /* restore */
}

static void test_check_cwd_chdir_bad_user(void)
{
    /* Nonexistent directory with user_cwd=true → WDIR_NOT_ACCESSIBLE. */
    char *p = strdup("/nonexistent_path_xyz_abc_123");
    pmix_status_t rc = pmix_util_check_context_cwd(&p, true, true);
    report("check_cwd_bad_user: WDIR_NOT_ACCESSIBLE",
           PMIX_ERR_JOB_WDIR_NOT_ACCESSIBLE == rc);
    free(p);
}

/* ------------------------------------------------------------------ */
/* pmix_util_check_context_app                                        */
/* ------------------------------------------------------------------ */

static void test_check_app_abs_good(void)
{
    char *cmd;
    pmix_status_t rc;

    if ('\0' == test_exe[0]) {
        fprintf(stdout, "  SKIP: check_app_abs_good (no known executable found)\n");
        return;
    }
    cmd = strdup(test_exe);
    rc = pmix_util_check_context_app(&cmd, NULL, NULL);
    report("check_app_abs_good: known exe returns SUCCESS", PMIX_SUCCESS == rc);
    free(cmd);
}

static void test_check_app_abs_bad(void)
{
    /* /etc/hosts is not executable. */
    if (0 == access("/etc/hosts", F_OK)) {
        char *cmd = strdup("/etc/hosts");
        pmix_status_t rc = pmix_util_check_context_app(&cmd, NULL, NULL);
        report("check_app_abs_bad: non-executable returns EXE_NOT_ACCESSIBLE",
               PMIX_ERR_EXE_NOT_ACCESSIBLE == rc);
        free(cmd);
    } else {
        fprintf(stdout, "  SKIP: check_app_abs_bad (/etc/hosts not found)\n");
    }
}

static void test_check_app_naked_not_found(void)
{
    /* A nonsensical naked command name that cannot be found on PATH. */
    char *cmd = strdup("__no_such_program_xyz_abc_123__");
    pmix_status_t rc = pmix_util_check_context_app(&cmd, NULL, NULL);
    report("check_app_naked_not_found: EXE_NOT_FOUND",
           PMIX_ERR_JOB_EXE_NOT_FOUND == rc);
    free(cmd);
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    pmix_proc_t myproc;
    pmix_status_t rc;
    static const char *candidates[] = {
        "/usr/bin/env", "/bin/sh", "/bin/ls", "/usr/bin/true", NULL
    };
    int i;
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    /* Find a known executable for the abs-good test. */
    for (i = 0; NULL != candidates[i]; i++) {
        if (0 == access(candidates[i], X_OK)) {
            strncpy(test_exe, candidates[i], sizeof(test_exe) - 1);
            break;
        }
    }

    rc = PMIx_Init(&myproc, NULL, 0);
    if (PMIX_SUCCESS != rc && PMIX_ERR_UNREACH != rc) {
        fprintf(stderr, "PMIx_Init: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    fprintf(stdout, "\n=== pmix_context_fns unit tests ===\n\n");

    test_check_cwd_null_incwd();
    test_check_cwd_null_string();
    test_check_cwd_no_chdir();
    test_check_cwd_chdir_success();
    test_check_cwd_chdir_bad_user();
    test_check_app_abs_good();
    test_check_app_abs_bad();
    test_check_app_naked_not_found();

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);

    PMIx_Finalize(NULL, 0);
    return (nfail > 0) ? 1 : 0;
}
