/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Expansion of the installation directories.
 *
 * pmix_pinstall_dirs_expand() substitutes each "${dir}" in a string with
 * that installation directory, repeating until nothing changes, since one
 * directory is commonly defined in terms of another ("${prefix}/lib").
 * A directory defined in terms of ITSELF - which the environment can do,
 * through PMIX_INSTALL_PREFIX and its siblings - never stops changing, so
 * the expansion has to give up rather than run for ever. A watchdog turns
 * a hang into a failure.
 */

#include "src/include/pmix_config.h"

#include "src/include/pmix_globals.h"
#include "src/mca/pinstalldirs/base/base.h"
#include "src/mca/pinstalldirs/pinstalldirs.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int npass = 0;
static int nfail = 0;

static void report(const char *name, int passed)
{
    if (passed) {
        fprintf(stdout, "  PASS: %s\n", name);
        ++npass;
    } else {
        fprintf(stdout, "  FAIL: %s\n", name);
        ++nfail;
    }
}

static void watchdog(int sig)
{
    static const char msg[] = "  FAIL: expansion did not return - hung\n";
    PMIX_HIDE_UNUSED_PARAMS(sig);
    (void) !write(STDOUT_FILENO, msg, sizeof(msg) - 1);
    _exit(1);
}

int main(int argc, char **argv)
{
    char *saved_prefix, *saved_libdir, *out;
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    fprintf(stdout, "\n=== pinstalldirs expansion unit tests ===\n\n");
    signal(SIGALRM, watchdog);
    alarm(30);

    saved_prefix = pmix_pinstall_dirs.prefix;
    saved_libdir = pmix_pinstall_dirs.libdir;

    /* one directory defined in terms of another */
    pmix_pinstall_dirs.prefix = (char *) "/opt/pmixut";
    pmix_pinstall_dirs.libdir = (char *) "${prefix}/lib";
    out = pmix_pinstall_dirs_expand("${libdir}/pmix");
    report("a nested directory expands fully",
           NULL != out && 0 == strcmp(out, "/opt/pmixut/lib/pmix"));
    free(out);

    /* one defined in terms of itself */
    pmix_pinstall_dirs.prefix = (char *) "${prefix}/x";
    out = pmix_pinstall_dirs_expand("${prefix}/lib");
    report("a directory defined in terms of itself stops expanding", NULL != out);
    free(out);

    pmix_pinstall_dirs.prefix = saved_prefix;
    pmix_pinstall_dirs.libdir = saved_libdir;
    alarm(0);

    fprintf(stdout, "\n%d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
