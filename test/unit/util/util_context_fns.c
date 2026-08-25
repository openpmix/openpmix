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
    /* restore - and say so if we cannot, since every later test that
     * touches a relative path would then be running somewhere else */
    if (0 != chdir(saved)) {
        report("check_cwd_chdir: restore cwd", 0);
    }
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

/* The $HOME fallback: a directory the user did NOT ask for, which we
 * cannot reach, is not an error - we go to $HOME instead and say so in
 * *incwd. That worked only while $HOME was set. With it unset,
 * pmix_home_directory() falls back to the passwd entry, and the uid it
 * was asked about was the sentinel (uid_t)-1, which has no passwd entry -
 * so the fallback never fired in the one case it was written for, and
 * *incwd was left naming a directory the process is not in. */
static void test_check_cwd_home_fallback_no_HOME(void)
{
    char *saved = getenv("HOME");
    char *keep = (NULL == saved) ? NULL : strdup(saved);
    char *p = strdup("/nonexistent_path_xyz_abc_123");
    char here[PMIX_PATH_MAX];
    pmix_status_t rc;

    if (NULL == getcwd(here, sizeof(here))) {
        report("check_cwd_home_fallback: getcwd", 0);
        free(p);
        free(keep);
        return;
    }
    unsetenv("HOME");
    rc = pmix_util_check_context_cwd(&p, true, false);
    if (NULL != keep) {
        setenv("HOME", keep, 1);
        free(keep);
    }

    report("check_cwd_home_fallback: returns SUCCESS", PMIX_SUCCESS == rc);
    /* the whole point: we were moved somewhere real, and *incwd names it */
    report("check_cwd_home_fallback: cwd was replaced",
           NULL != p && 0 != strcmp(p, "/nonexistent_path_xyz_abc_123"));
    report("check_cwd_home_fallback: the reported cwd is reachable",
           NULL != p && 0 == access(p, X_OK));
    free(p);
    if (0 != chdir(here)) {
        report("check_cwd_home_fallback: restore cwd", 0);
    }
}

/* ------------------------------------------------------------------ */
/* pmix_util_check_context_app                                        */
/* ------------------------------------------------------------------ */

static void test_check_app_null_input(void)
{
    /* Its sibling screens a NULL out-param and a NULL string; this one
     * did not, and handed the NULL straight to strlen(). A spawn request
     * carrying no cmd at all reaches here. */
    char *cmd = NULL;
    report("check_app_null_incmd: ERR_BAD_PARAM",
           PMIX_ERR_BAD_PARAM == pmix_util_check_context_app(NULL, NULL, NULL));
    report("check_app_null_cmd: ERR_BAD_PARAM",
           PMIX_ERR_BAD_PARAM == pmix_util_check_context_app(&cmd, NULL, NULL));
}

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
    test_check_cwd_home_fallback_no_HOME();
    test_check_app_null_input();
    test_check_app_abs_good();
    test_check_app_abs_bad();
    test_check_app_naked_not_found();

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);

    PMIx_Finalize(NULL, 0);
    return (nfail > 0) ? 1 : 0;
}
