/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit tests for the MCA parameter *file* path in src/mca/base:
 * pmix_mca_base_parse_paramfile.c, the read_files()/append_filename_to_list()
 * machinery in pmix_mca_base_var.c, and -- the point of the exercise --
 * the precedence rules between a value in an ordinary parameter file, a
 * value in the override file, and a value in the environment.
 *
 * Everything here is driven through real files that main() writes into a
 * private temporary directory and names through the MCA parameters that
 * select them, because those parameters are read exactly once during
 * pmix_mca_base_var_init() and there is no other way in.
 *
 * Exit 0 if all tests pass, 1 otherwise.
 */

#include "src/include/pmix_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "pmix_common.h"
#include "src/mca/base/pmix_base.h"
#include "src/mca/base/pmix_mca_base_var.h"
#include "src/mca/base/pmix_mca_base_vari.h"
#include "src/runtime/pmix_init_util.h"
#include "src/util/pmix_argv.h"

static int npass = 0;
static int nfail = 0;
static char tmpdir[512];

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
/* a plain value out of an ordinary parameter file                     */
/* ------------------------------------------------------------------ */

static int file_int = 0;
static char *file_string = NULL;

static void test_value_from_file(void)
{
    pmix_mca_base_var_source_t source = PMIX_MCA_BASE_VAR_SOURCE_MAX;
    const char *source_file = NULL;
    int *ip = NULL;
    char **sp = NULL;
    int idx, rc;

    file_int = 1;
    idx = pmix_mca_base_var_register("pmix", "utest", "file", "an_int", "from a parameter file",
                                     PMIX_MCA_BASE_VAR_TYPE_INT, &file_int);
    report("file int registered", 0 <= idx);
    rc = pmix_mca_base_var_get_value(idx, &ip, &source, &source_file);
    report("file int took the file's value", PMIX_SUCCESS == rc && NULL != ip && 77 == *ip);
    report("file int reports FILE as its source", PMIX_MCA_BASE_VAR_SOURCE_FILE == source);
    report("file int names the file it came from",
           NULL != source_file && NULL != strstr(source_file, "params.conf"));

    file_string = NULL;
    idx = pmix_mca_base_var_register("pmix", "utest", "file", "a_string", "from a parameter file",
                                     PMIX_MCA_BASE_VAR_TYPE_STRING, &file_string);
    report("file string registered", 0 <= idx);
    rc = pmix_mca_base_var_get_value(idx, &sp, NULL, NULL);
    report("file string took the file's value",
           PMIX_SUCCESS == rc && NULL != sp && NULL != *sp && 0 == strcmp(*sp, "from-the-file"));
}

/* A variable named in neither file nor environment keeps its default,
 * even though the files were read. */
static int untouched_int = 0;

static void test_default_survives(void)
{
    pmix_mca_base_var_source_t source = PMIX_MCA_BASE_VAR_SOURCE_MAX;
    int *ip = NULL;
    int idx, rc;

    untouched_int = 5;
    idx = pmix_mca_base_var_register("pmix", "utest", "file", "untouched", "never set anywhere",
                                     PMIX_MCA_BASE_VAR_TYPE_INT, &untouched_int);
    rc = pmix_mca_base_var_get_value(idx, &ip, &source, NULL);
    report("unmentioned variable keeps its default",
           PMIX_SUCCESS == rc && NULL != ip && 5 == *ip);
    report("unmentioned variable reports DEFAULT",
           PMIX_MCA_BASE_VAR_SOURCE_DEFAULT == source);
}

/* ------------------------------------------------------------------ */
/* precedence                                                          */
/* ------------------------------------------------------------------ */

/* The environment beats an ordinary parameter file. */
static int env_beats_file = 0;

static void test_env_beats_file(void)
{
    pmix_mca_base_var_source_t source = PMIX_MCA_BASE_VAR_SOURCE_MAX;
    int *ip = NULL;
    int idx, rc;

    env_beats_file = 0;
    idx = pmix_mca_base_var_register("pmix", "utest", "prec", "env_wins",
                                     "in both the file and the environment",
                                     PMIX_MCA_BASE_VAR_TYPE_INT, &env_beats_file);
    rc = pmix_mca_base_var_get_value(idx, &ip, &source, NULL);
    report("environment beats an ordinary parameter file",
           PMIX_SUCCESS == rc && NULL != ip && 22 == *ip);
    report("and reports ENV as the source", PMIX_MCA_BASE_VAR_SOURCE_ENV == source);
}

/* The override file beats the environment -- that is the whole reason
 * the override file exists. A site administrator writes it to say "this
 * value is not negotiable", and a user's environment must not win. */
static int override_wins = 0;

static void test_override_beats_env(void)
{
    pmix_mca_base_var_source_t source = PMIX_MCA_BASE_VAR_SOURCE_MAX;
    int *ip = NULL;
    int idx, rc;

    override_wins = 0;
    idx = pmix_mca_base_var_register("pmix", "utest", "prec", "override_wins",
                                     "in both the override file and the environment",
                                     PMIX_MCA_BASE_VAR_TYPE_INT, &override_wins);
    rc = pmix_mca_base_var_get_value(idx, &ip, &source, NULL);
    report("override file beats the environment",
           PMIX_SUCCESS == rc && NULL != ip && 33 == *ip);
    report("and reports OVERRIDE as the source",
           PMIX_MCA_BASE_VAR_SOURCE_OVERRIDE == source);
}

/* The override file beats an ordinary parameter file too. */
static int override_beats_file = 0;

static void test_override_beats_file(void)
{
    int *ip = NULL;
    int idx, rc;

    override_beats_file = 0;
    idx = pmix_mca_base_var_register("pmix", "utest", "prec", "override_file",
                                     "in both files", PMIX_MCA_BASE_VAR_TYPE_INT,
                                     &override_beats_file);
    rc = pmix_mca_base_var_get_value(idx, &ip, NULL, NULL);
    report("override file beats an ordinary parameter file",
           PMIX_SUCCESS == rc && NULL != ip && 44 == *ip);
}

/* ...and it has to keep beating the environment when the name the
 * override file used was a *synonym*.
 *
 * This is the case that separates "the override is enforced" from "the
 * override is enforced as long as the site administrator happened to
 * write the modern spelling". var_set_from_file() records the source on
 * the variable a synonym stands for, and var_set_initial() has to make
 * that say OVERRIDE -- not just FILE -- or the environment check that
 * follows sees no override to protect and lets the user's value through. */
static int syn_override = 0;

static void test_override_via_synonym_beats_env(void)
{
    pmix_mca_base_var_source_t source = PMIX_MCA_BASE_VAR_SOURCE_MAX;
    int *ip = NULL;
    int idx, syn, rc;

    syn_override = 0;
    idx = pmix_mca_base_var_register("pmix", "utest", "syn", "new_name",
                                     "reachable under two names",
                                     PMIX_MCA_BASE_VAR_TYPE_INT, &syn_override);
    report("synonym target registered", 0 <= idx);

    /* the override file names "utest_old_name", which is this synonym */
    syn = pmix_mca_base_var_register_synonym(idx, "pmix", "utest", NULL, "old_name",
                                             PMIX_MCA_BASE_VAR_SYN_FLAG_DEPRECATED);
    report("synonym registered", 0 <= syn);

    rc = pmix_mca_base_var_get_value(idx, &ip, &source, NULL);
    report("override file naming a synonym still beats the environment",
           PMIX_SUCCESS == rc && NULL != ip && 55 == *ip);
    report("and still reports OVERRIDE as the source",
           PMIX_MCA_BASE_VAR_SOURCE_OVERRIDE == source);
}

/* The sharper form of the same case: the override file names the
 * synonym AND the environment names the synonym too.
 *
 * Above, the environment used the modern name, so the environment
 * lookup for the synonym found nothing and the override survived by
 * luck. Here both sides use the deprecated name, so the environment
 * check actually runs against a variable whose recorded source decides
 * whether it is protected -- and if reading the override file only
 * marked the synonym, not the variable it stands for, the check sees an
 * ordinary file value and lets the user's environment win. A site
 * administrator's override would then be defeated by spelling. */
static int syn2_override = 0;

static void test_override_via_synonym_beats_synonym_env(void)
{
    pmix_mca_base_var_source_t source = PMIX_MCA_BASE_VAR_SOURCE_MAX;
    int *ip = NULL;
    int idx, syn, rc;

    syn2_override = 0;
    idx = pmix_mca_base_var_register("pmix", "utest", "syn2", "new_name",
                                     "reachable under two names",
                                     PMIX_MCA_BASE_VAR_TYPE_INT, &syn2_override);
    report("second synonym target registered", 0 <= idx);

    syn = pmix_mca_base_var_register_synonym(idx, "pmix", "utest", NULL, "old2_name",
                                             PMIX_MCA_BASE_VAR_SYN_FLAG_DEPRECATED);
    report("second synonym registered", 0 <= syn);

    rc = pmix_mca_base_var_get_value(idx, &ip, &source, NULL);
    report("override beats the environment even when both name the synonym",
           PMIX_SUCCESS == rc && NULL != ip && 66 == *ip);
    report("and that case reports OVERRIDE as the source",
           PMIX_MCA_BASE_VAR_SOURCE_OVERRIDE == source);
}

/* ------------------------------------------------------------------ */
/* value parsing out of a file                                         */
/* ------------------------------------------------------------------ */

static size_t suffix_k = 0;
static size_t suffix_m = 0;
static size_t suffix_g = 0;
static bool file_bool_true = false;
static bool file_bool_false = true;
static int negative_int = 0;

/* int_from_string() accepts a K/M/G suffix on the integral types. This
 * is the only place that behavior is exercised. */
static void test_value_parsing(void)
{
    size_t *zp = NULL;
    bool *bp = NULL;
    int *ip = NULL;
    int idx, rc;

    idx = pmix_mca_base_var_register("pmix", "utest", "parse", "k", "1k",
                                     PMIX_MCA_BASE_VAR_TYPE_SIZE_T, &suffix_k);
    rc = pmix_mca_base_var_get_value(idx, &zp, NULL, NULL);
    report("\"1k\" parses as 1024", PMIX_SUCCESS == rc && NULL != zp && 1024 == *zp);

    idx = pmix_mca_base_var_register("pmix", "utest", "parse", "m", "2M",
                                     PMIX_MCA_BASE_VAR_TYPE_SIZE_T, &suffix_m);
    rc = pmix_mca_base_var_get_value(idx, &zp, NULL, NULL);
    report("\"2M\" parses as 2097152",
           PMIX_SUCCESS == rc && NULL != zp && (2u << 20) == *zp);

    idx = pmix_mca_base_var_register("pmix", "utest", "parse", "g", "1g",
                                     PMIX_MCA_BASE_VAR_TYPE_SIZE_T, &suffix_g);
    rc = pmix_mca_base_var_get_value(idx, &zp, NULL, NULL);
    report("\"1g\" parses as 1073741824",
           PMIX_SUCCESS == rc && NULL != zp && (1u << 30) == *zp);

    idx = pmix_mca_base_var_register("pmix", "utest", "parse", "yes", "a true bool",
                                     PMIX_MCA_BASE_VAR_TYPE_BOOL, &file_bool_true);
    rc = pmix_mca_base_var_get_value(idx, &bp, NULL, NULL);
    report("\"true\" in a file parses as true", PMIX_SUCCESS == rc && NULL != bp && *bp);

    idx = pmix_mca_base_var_register("pmix", "utest", "parse", "no", "a false bool",
                                     PMIX_MCA_BASE_VAR_TYPE_BOOL, &file_bool_false);
    rc = pmix_mca_base_var_get_value(idx, &bp, NULL, NULL);
    report("\"false\" in a file parses as false", PMIX_SUCCESS == rc && NULL != bp && !*bp);

    idx = pmix_mca_base_var_register("pmix", "utest", "parse", "neg", "a negative int",
                                     PMIX_MCA_BASE_VAR_TYPE_INT, &negative_int);
    rc = pmix_mca_base_var_get_value(idx, &ip, NULL, NULL);
    report("a negative integer round-trips", PMIX_SUCCESS == rc && NULL != ip && -7 == *ip);
}

/* A "~/" prefix in a string value is expanded to the user's home
 * directory; so is ":~/" anywhere in a path-style list. */
static char *tilde_simple = NULL;
static char *tilde_in_path = NULL;

static void test_tilde_expansion(void)
{
    char **sp = NULL;
    int idx, rc;

    idx = pmix_mca_base_var_register("pmix", "utest", "tilde", "simple", "a ~/ value",
                                     PMIX_MCA_BASE_VAR_TYPE_STRING, &tilde_simple);
    rc = pmix_mca_base_var_get_value(idx, &sp, NULL, NULL);
    report("leading \"~/\" is expanded",
           PMIX_SUCCESS == rc && NULL != sp && NULL != *sp && '~' != (*sp)[0]
               && NULL != strstr(*sp, "mydir"));

    idx = pmix_mca_base_var_register("pmix", "utest", "tilde", "path", "a :~/ value",
                                     PMIX_MCA_BASE_VAR_TYPE_STRING, &tilde_in_path);
    rc = pmix_mca_base_var_get_value(idx, &sp, NULL, NULL);
    report("\"~/\" after a colon is expanded",
           PMIX_SUCCESS == rc && NULL != sp && NULL != *sp
               && NULL == strstr(*sp, ":~/") && NULL != strstr(*sp, "second"));
}

/* ------------------------------------------------------------------ */
/* the file list itself                                                */
/* ------------------------------------------------------------------ */

/* read_files() takes a comma-delimited list and reads it in reverse so
 * that entries farthest to the LEFT take precedence. */
static int from_first_file = 0;
static int in_both_files = 0;

static void test_file_list_precedence(void)
{
    int *ip = NULL;
    int idx, rc;

    idx = pmix_mca_base_var_register("pmix", "utest", "list", "only_second",
                                     "only in the second file",
                                     PMIX_MCA_BASE_VAR_TYPE_INT, &from_first_file);
    rc = pmix_mca_base_var_get_value(idx, &ip, NULL, NULL);
    report("a value from the second file in the list is read",
           PMIX_SUCCESS == rc && NULL != ip && 88 == *ip);

    idx = pmix_mca_base_var_register("pmix", "utest", "list", "in_both",
                                     "in both files of the list",
                                     PMIX_MCA_BASE_VAR_TYPE_INT, &in_both_files);
    rc = pmix_mca_base_var_get_value(idx, &ip, NULL, NULL);
    report("the leftmost file in the list wins",
           PMIX_SUCCESS == rc && NULL != ip && 99 == *ip);
}

/* ------------------------------------------------------------------ */
/* build_env over a file-sourced variable                              */
/* ------------------------------------------------------------------ */

/* A FILE- or OVERRIDE-sourced variable is the only thing that makes
 * pmix_mca_base_var_build_env() emit a "SOURCE_" entry, so it is the
 * only thing that reaches the second asprintf() in that loop. This is
 * the case mca_base_var's build_env test cannot construct: everything
 * it sets comes from the environment, which takes the branch that
 * emits nothing.
 *
 * Two things are asserted. The call must report PMIX_SUCCESS -- it used
 * to return the *length* of the last string it formatted, so a caller
 * testing PMIX_SUCCESS saw a failure whenever the walk ended on a
 * file-sourced variable -- and the SOURCE_ entry must name the file the
 * value came from. */
static void test_build_env_file_source(void)
{
    char **env = NULL;
    int num_env = 0;
    bool saw_value = false, saw_source = false;
    int i, rc;

    rc = pmix_mca_base_var_build_env(&env, &num_env);
    report("build_env succeeds with a file-sourced variable", PMIX_SUCCESS == rc);
    if (PMIX_SUCCESS != rc || NULL == env) {
        return;
    }

    for (i = 0; i < num_env; ++i) {
        if (0 == strcmp(env[i], "PMIX_MCA_utest_file_an_int=77")) {
            saw_value = true;
        }
        if (0 == strncmp(env[i], "PMIX_MCA_SOURCE_utest_file_an_int=FILE:", 39)
            && NULL != strstr(env[i], "params.conf")) {
            saw_source = true;
        }
    }
    report("build_env forwards a file-sourced value", saw_value);
    report("build_env forwards the file the value came from", saw_source);

    PMIx_Argv_free(env);
}

/* ------------------------------------------------------------------ */

static void write_file(const char *name, const char *contents)
{
    char path[1024];
    FILE *fp;

    snprintf(path, sizeof(path), "%s/%s", tmpdir, name);
    fp = fopen(path, "w");
    if (NULL == fp) {
        fprintf(stderr, "cannot write %s\n", path);
        exit(1);
    }
    fputs(contents, fp);
    fclose(fp);
}

int main(int argc, char **argv)
{
    char params[1024], params2[1024], override[1024], list[2048];
    char tmpl[] = "/tmp/pmix-mca-utest-XXXXXX";
    int rc;

    (void) argc;
    (void) argv;

    if (NULL == mkdtemp(tmpl)) {
        fprintf(stderr, "cannot create a temporary directory\n");
        return 1;
    }
    snprintf(tmpdir, sizeof(tmpdir), "%s", tmpl);

    /* The first file in the list wins over the second, so put the
     * "in_both" value here. */
    write_file("params.conf",
               "# an MCA parameter file\n"
               "utest_file_an_int = 77\n"
               "utest_file_a_string = from-the-file\n"
               "utest_prec_env_wins = 11\n"
               "utest_prec_override_file = 11\n"
               "utest_parse_k = 1k\n"
               "utest_parse_m = 2M\n"
               "utest_parse_g = 1g\n"
               "utest_parse_yes = true\n"
               "utest_parse_no = false\n"
               "utest_parse_neg = -7\n"
               "utest_tilde_simple = ~/mydir\n"
               "utest_tilde_path = /first:~/second\n"
               "utest_list_in_both = 99\n");

    write_file("params2.conf",
               "utest_list_only_second = 88\n"
               "utest_list_in_both = 11\n");

    /* The override file names utest_prec_override_wins directly, and
     * names the synonym utest_old_name for utest_syn_new_name. */
    write_file("override.conf",
               "utest_prec_override_wins = 33\n"
               "utest_prec_override_file = 44\n"
               "utest_old_name = 55\n"
               "utest_old2_name = 66\n");

    snprintf(params, sizeof(params), "%s/params.conf", tmpdir);
    snprintf(params2, sizeof(params2), "%s/params2.conf", tmpdir);
    snprintf(override, sizeof(override), "%s/override.conf", tmpdir);
    snprintf(list, sizeof(list), "%s,%s", params, params2);

    /* All of this has to be in place before pmix_init_util() runs: the
     * variable system reads these files exactly once, during
     * pmix_mca_base_var_init(). */
    setenv("PMIX_MCA_mca_base_param_files", list, 1);
    setenv("PMIX_MCA_mca_base_override_param_file", override, 1);

    /* the environment side of the precedence cases */
    setenv("PMIX_MCA_utest_prec_env_wins", "22", 1);
    setenv("PMIX_MCA_utest_prec_override_wins", "2222", 1);
    setenv("PMIX_MCA_utest_syn_new_name", "5555", 1);
    setenv("PMIX_MCA_utest_old2_name", "6666", 1);

    rc = pmix_init_util(NULL, 0, NULL);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "pmix_init_util failed: %d\n", rc);
        return 1;
    }

    fprintf(stdout, "\n=== mca_base parameter file unit tests ===\n\n");

    test_value_from_file();
    test_default_survives();
    test_env_beats_file();
    test_override_beats_env();
    test_override_beats_file();
    test_override_via_synonym_beats_env();
    test_override_via_synonym_beats_synonym_env();
    test_value_parsing();
    test_tilde_expansion();
    test_file_list_precedence();
    test_build_env_file_source();

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);

    pmix_finalize_util();

    /* leave nothing behind in /tmp */
    unlink(params);
    unlink(params2);
    unlink(override);
    rmdir(tmpdir);

    return (nfail > 0) ? 1 : 0;
}
