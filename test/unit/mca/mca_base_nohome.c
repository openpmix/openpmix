/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * The MCA defaults for a process with no home directory.
 *
 * The component search path and the parameter-file list each carry an
 * entry under the user's home directory. When there is none - $HOME unset
 * and no passwd entry for the effective uid - those entries are left out,
 * and the search path is not given a user default at all.
 *
 * Reaching that state needs a uid with no passwd entry, so this runs only
 * as root: a child drops to such a uid, unsets $HOME, brings the variable
 * system up and checks every default entry is absolute. Exit 77 (skip)
 * otherwise.
 */

#include "src/include/pmix_config.h"

#include "pmix_common.h"
#include "src/include/pmix_globals.h"
#include "src/mca/base/pmix_base.h"
#include "src/mca/base/pmix_mca_base_var.h"
#include "src/runtime/pmix_init_util.h"
#include "src/util/pmix_environ.h"

#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static int nfail = 0;

static void report(const char *name, int passed)
{
    fprintf(stdout, "  %s: %s\n", passed ? "PASS" : "FAIL", name);
    fflush(stdout);
    if (!passed) {
        ++nfail;
    }
}

/* every entry of a separated list must be an absolute path */
static bool all_absolute(const char *list, char sep)
{
    char **entries;
    bool ok = true;
    int n;

    if (NULL == list) {
        return true;
    }
    entries = PMIx_Argv_split(list, sep);
    for (n = 0; NULL != entries && NULL != entries[n]; n++) {
        if ('/' != entries[n][0] || NULL != strstr(entries[n], "(null)")) {
            fprintf(stdout, "    not absolute: %s\n", entries[n]);
            ok = false;
        }
    }
    PMIx_Argv_free(entries);
    return ok;
}

static const char *string_var(const char *name)
{
    const char **value = NULL;
    int idx;

    if (PMIX_SUCCESS != pmix_mca_base_var_find_by_name(name, &idx) ||
        PMIX_SUCCESS != pmix_mca_base_var_get_value(idx, &value, NULL, NULL) ||
        NULL == value) {
        return NULL;
    }
    return *value;
}

static int child(uid_t uid)
{
    const char *value;

    if (0 != setgroups(0, NULL) || 0 != setgid((gid_t) uid) || 0 != setuid(uid)) {
        fprintf(stdout, "  child could not drop to uid %lu\n", (unsigned long) uid);
        return 2;
    }
    unsetenv("HOME");
    unsetenv("PMIX_PARAM_FILE_PASSED");
    unsetenv("PMIX_MCA_mca_base_param_files");
    unsetenv("PMIX_MCA_mca_base_component_path");
    if (0 != chdir("/")) {
        return 2;
    }

    report("the process has no home directory", NULL == pmix_home_directory(geteuid()));

    if (PMIX_SUCCESS != pmix_init_util(NULL, 0, NULL)) {
        fprintf(stdout, "  pmix_init_util failed\n");
        return 2;
    }

    value = string_var("mca_base_param_files");
    report("every default parameter file is an absolute path", all_absolute(value, ','));

    /* the search path is "project@dir:dir" entries, ';'-separated */
    {
        char **projects = PMIx_Argv_split(pmix_mca_base_component_path, ';');
        bool ok = (NULL != projects);
        int n;

        for (n = 0; ok && NULL != projects[n]; n++) {
            char *dirs = strchr(projects[n], '@');
            ok = all_absolute((NULL == dirs) ? projects[n] : dirs + 1, ':');
        }
        PMIx_Argv_free(projects);
        report("every default component directory is an absolute path", ok);
    }

    report("no user component directory is defined", NULL == pmix_mca_base_user_default_path);

    return (0 == nfail) ? 0 : 1;
}

int main(int argc, char **argv)
{
    uid_t uid;
    pid_t pid;
    int status;

    (void) argc;
    (void) argv;

    if (0 != geteuid()) {
        fprintf(stdout, "mca_base_nohome: needs root to reach a uid with no passwd entry - skipping\n");
        return 77;
    }

    /* a uid with no passwd entry */
    for (uid = 54321; NULL != getpwuid(uid); uid++) {
        continue;
    }

    fprintf(stdout, "\n=== MCA defaults with no home directory (uid %lu) ===\n",
            (unsigned long) uid);
    fflush(stdout);
    pid = fork();
    if (0 > pid) {
        return 1;
    }
    if (0 == pid) {
        _exit(child(uid));
    }
    if (pid != waitpid(pid, &status, 0) || !WIFEXITED(status)) {
        fprintf(stdout, "  FAIL: the child did not exit normally\n");
        return 1;
    }
    fprintf(stdout, "\nResults: %s\n\n", (0 == WEXITSTATUS(status)) ? "passed" : "failed");
    return (0 == WEXITSTATUS(status)) ? 0 : 1;
}
