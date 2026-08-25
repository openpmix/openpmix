/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit tests for src/util/pmix_keyval_parse.c: the flex-driven parser
 * for MCA parameter files.  Everything in that file is process-global -
 * the key buffer, the accumulated "-x" string, the lock - so the cases
 * that matter most are about what survives an init/finalize cycle, not
 * about any one parse.
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

#include "pmix_common.h"
#include "src/util/pmix_keyval_parse.h"

static int npass = 0;
static int nfail = 0;

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

/* What the callback saw, in the order it saw it. */
#define MAX_SEEN 16
static char *seen_name[MAX_SEEN];
static char *seen_value[MAX_SEEN];
static int nseen = 0;

static void reset_seen(void)
{
    int i;

    for (i = 0; i < nseen; i++) {
        free(seen_name[i]);
        free(seen_value[i]);
    }
    nseen = 0;
}

/* The parser documents that both strings point into buffers it may
 * overwrite the moment we return, so copy them out. */
static void collect(const char *file, int lineno, const char *name,
                    const char *value, void *cbdata)
{
    PMIX_HIDE_UNUSED_PARAMS(file, lineno, cbdata);

    if (nseen >= MAX_SEEN) {
        return;
    }
    seen_name[nseen] = (NULL == name) ? NULL : strdup(name);
    seen_value[nseen] = (NULL == value) ? NULL : strdup(value);
    nseen++;
}

static int find_seen(const char *name)
{
    int i;

    for (i = 0; i < nseen; i++) {
        if (NULL != seen_name[i] && 0 == strcmp(name, seen_name[i])) {
            return i;
        }
    }
    return -1;
}

static char tmpfile_path[512];

static int write_file(const char *contents)
{
    FILE *fp;

    fp = fopen(tmpfile_path, "w");
    if (NULL == fp) {
        return -1;
    }
    fputs(contents, fp);
    fclose(fp);
    return 0;
}

static void test_basic_pairs(void)
{
    int rc, idx;

    if (0 != write_file("# a comment\n"
                        "alpha = one\n"
                        "\n"
                        "beta = two three\n")) {
        report("could not write the fixture file", 0);
        return;
    }

    reset_seen();
    rc = pmix_util_keyval_parse(tmpfile_path, collect, NULL);
    report("a well-formed file parses", PMIX_SUCCESS == rc);

    idx = find_seen("alpha");
    report("first pair is delivered",
           0 <= idx && NULL != seen_value[idx] && 0 == strcmp("one", seen_value[idx]));
    idx = find_seen("beta");
    report("a value with a space in it is delivered whole",
           0 <= idx && NULL != seen_value[idx] && 0 == strcmp("two three", seen_value[idx]));
    report("the comment and the blank line are not pairs", 2 == nseen);
    reset_seen();
}

/*
 * The caller (pmix_mca_base_var_cache_files) treats PMIX_ERR_NOT_FOUND
 * as "no such file, carry on", and every other code as fatal, so an
 * absent file has to answer with exactly that one.
 */
static void test_missing_file(void)
{
    char missing[600];
    int rc;

    snprintf(missing, sizeof(missing), "%s.does.not.exist", tmpfile_path);
    reset_seen();
    rc = pmix_util_keyval_parse(missing, collect, NULL);
    report("a missing file answers NOT_FOUND", PMIX_ERR_NOT_FOUND == rc);
    report("a missing file delivers no pairs", 0 == nseen);
    reset_seen();
}

/*
 * "-x FOO=bar" lines are not delivered as they are read.  They pile up
 * in one process-global string that is handed over, and released, by
 * pmix_util_keyval_save_internal_envars().
 */
static void test_env_directives(void)
{
    int rc, idx;

    if (0 != write_file("-x FOO=bar\n"
                        "-x BAZ=qux\n")) {
        report("could not write the fixture file", 0);
        return;
    }

    reset_seen();
    rc = pmix_util_keyval_parse(tmpfile_path, collect, NULL);
    report("a file of -x directives parses", PMIX_SUCCESS == rc);
    report("-x directives are not delivered by the parse", 0 == nseen);

    rc = pmix_util_keyval_save_internal_envars(collect, NULL);
    report("saving the internal envars succeeds", PMIX_SUCCESS == rc);
    idx = find_seen("mca_base_env_list_internal");
    report("both -x directives arrive in one pair",
           0 <= idx && NULL != seen_value[idx]
               && NULL != strstr(seen_value[idx], "FOO=bar")
               && NULL != strstr(seen_value[idx], "BAZ=qux"));

    /* the hand-off releases the string, so a second ask delivers nothing */
    reset_seen();
    rc = pmix_util_keyval_save_internal_envars(collect, NULL);
    report("a second save delivers nothing", PMIX_SUCCESS == rc && 0 == nseen);
    reset_seen();
}

/*
 * The regression this file exists for.  pmix_util_keyval_parse_finalize()
 * released the key buffer but not the accumulated -x string, and
 * pmix_init_util() runs again after pmix_finalize_util() - so a run that
 * ended without reaching the hand-off (pmix_mca_base_var_cache_files
 * returns on the first file that fails to parse, before the store) left
 * its variables standing, and the *next* run was handed them.
 */
static void test_finalize_drops_pending_envars(void)
{
    int rc;

    if (0 != write_file("-x LEAKED=fromfirstrun\n")) {
        report("could not write the fixture file", 0);
        return;
    }

    reset_seen();
    rc = pmix_util_keyval_parse(tmpfile_path, collect, NULL);
    report("the first run parses its -x directive", PMIX_SUCCESS == rc);

    /* end the run without ever asking for the accumulated variables */
    pmix_util_keyval_parse_finalize();
    rc = pmix_util_keyval_parse_init();
    report("the parser re-initializes", PMIX_SUCCESS == rc);

    reset_seen();
    rc = pmix_util_keyval_save_internal_envars(collect, NULL);
    report("a new run is not handed the previous run's -x variables",
           PMIX_SUCCESS == rc && -1 == find_seen("mca_base_env_list_internal"));
    reset_seen();
}

int main(int argc, char **argv)
{
    const char *tmpdir;
    int rc;

    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    tmpdir = getenv("TMPDIR");
    if (NULL == tmpdir || '\0' == tmpdir[0]) {
        tmpdir = "/tmp";
    }
    snprintf(tmpfile_path, sizeof(tmpfile_path), "%s/pmix_util_keyval_test.%ld", tmpdir,
             (long) getpid());

    rc = pmix_util_keyval_parse_init();
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "pmix_util_keyval_parse_init failed: %d\n", rc);
        return 1;
    }

    fprintf(stdout, "\n=== pmix_util_keyval_parse unit tests ===\n\n");
    test_basic_pairs();
    test_missing_file();
    test_env_directives();
    test_finalize_drops_pending_envars();

    pmix_util_keyval_parse_finalize();
    reset_seen();
    unlink(tmpfile_path);

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);
    return (nfail > 0) ? 1 : 0;
}
