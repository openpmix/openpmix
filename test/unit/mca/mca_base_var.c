/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit tests for the MCA variable system in
 * src/mca/base/pmix_mca_base_var.c: name generation, registration and
 * lookup, the value-resolution order (default vs. environment),
 * synonyms, the several dump formats, and pmix_mca_base_var_build_env().
 *
 * The process is brought up with pmix_init_util(), which is the
 * lightest init that establishes the install dirs and the MCA -- a
 * full PMIx_Init would need a server. Reading of MCA parameter files
 * is disabled from main() so the results do not depend on whatever
 * happens to be in the caller's ~/.pmix or $sysconfdir.
 *
 * Exit 0 if all tests pass, 1 otherwise.
 */

#include "src/include/pmix_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pmix_common.h"
#include "src/mca/base/pmix_base.h"
#include "src/mca/base/pmix_mca_base_var.h"
#include "src/mca/base/pmix_mca_base_var_group.h"
#include "src/mca/base/pmix_mca_base_vari.h"
#include "src/runtime/pmix_init_util.h"
#include "src/util/pmix_argv.h"

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

/* ------------------------------------------------------------------ */
/* full-name generation                                                */
/* ------------------------------------------------------------------ */

static void check_full_name(const char *project, const char *framework, const char *component,
                            const char *variable, const char *expected)
{
    char label[256];
    char *out = NULL;
    int rc;

    rc = pmix_mca_base_var_generate_full_name4(project, framework, component, variable, &out);
    snprintf(label, sizeof(label), "full_name4 -> \"%s\"", expected);
    report(label, PMIX_SUCCESS == rc && NULL != out && 0 == strcmp(out, expected));
    free(out);
}

static void test_generate_full_name(void)
{
    check_full_name("pmix", "ptl", "tcp", "if_include", "pmix_ptl_tcp_if_include");
    check_full_name(NULL, "ptl", "tcp", "if_include", "ptl_tcp_if_include");
    check_full_name(NULL, "ptl", NULL, "verbose", "ptl_verbose");
    check_full_name(NULL, NULL, NULL, "solo", "solo");
    check_full_name("pmix", NULL, NULL, NULL, "pmix");
    /* all-NULL yields an empty (but allocated and terminated) string */
    check_full_name(NULL, NULL, NULL, NULL, "");
}

/* ------------------------------------------------------------------ */
/* register / find / get                                               */
/* ------------------------------------------------------------------ */

static int int_var = -1;
static bool bool_var = false;
static char *string_var = NULL;
static size_t size_var = 0;
static double double_var = 0.0;

static void test_register_and_find(void)
{
    const pmix_mca_base_var_t *var = NULL;
    int *ip = NULL;
    pmix_mca_base_var_source_t source = PMIX_MCA_BASE_VAR_SOURCE_MAX;
    int idx, found, rc;

    int_var = 42;
    idx = pmix_mca_base_var_register("pmix", "utest", "vars", "an_int", "an integer variable",
                                     PMIX_MCA_BASE_VAR_TYPE_INT, &int_var);
    report("register int returns an index", 0 <= idx);

    found = pmix_mca_base_var_find("pmix", "utest", "vars", "an_int");
    report("find by components returns the same index", found == idx);

    found = -1;
    rc = pmix_mca_base_var_find_by_name("utest_vars_an_int", &found);
    report("find by full name", PMIX_SUCCESS == rc && found == idx);

    rc = pmix_mca_base_var_find_by_name("utest_vars_no_such_thing", &found);
    report("find by unknown name fails", PMIX_SUCCESS != rc);

    rc = pmix_mca_base_var_get(idx, &var);
    report("get returns the var", PMIX_SUCCESS == rc && NULL != var);
    if (NULL != var) {
        report("var records its full name", 0 == strcmp(var->mbv_full_name, "utest_vars_an_int"));
        report("var records its long name",
               0 == strcmp(var->mbv_long_name, "pmix_utest_vars_an_int"));
        report("var records its type", PMIX_MCA_BASE_VAR_TYPE_INT == var->mbv_type);
        report("var prefix is the upper-cased project",
               0 == strcmp(var->mbv_prefix, "PMIX_MCA_"));
    }

    /* get_value hands back a POINTER TO the backing store, not a copy
     * of the value -- the header used to document a copy-out interface
     * with a value_size parameter that has never existed, so pin the
     * real contract here */
    rc = pmix_mca_base_var_get_value(idx, &ip, &source, NULL);
    report("get_value succeeds", PMIX_SUCCESS == rc);
    report("get_value points at the caller's storage", ip == &int_var);
    report("get_value does not copy: writes through the pointer are seen",
           NULL != ip && 42 == *ip);
    int_var = 43;
    report("get_value aliases, so a later direct write shows through",
           NULL != ip && 43 == *ip);
    int_var = 42;
    report("unset variable reports the default as its source",
           PMIX_MCA_BASE_VAR_SOURCE_DEFAULT == source);

    /* re-registering the same name with a different type must be
     * refused rather than silently reinterpreting the storage */
    rc = pmix_mca_base_var_register("pmix", "utest", "vars", "an_int", "wrong type",
                                    PMIX_MCA_BASE_VAR_TYPE_STRING, &string_var);
    report("re-register with a different type is refused", 0 > rc);

    /* bogus indices */
    rc = pmix_mca_base_var_get_value(-1, &ip, NULL, NULL);
    report("get_value(-1) is refused", PMIX_SUCCESS != rc);
    rc = pmix_mca_base_var_get_value(1000000, &ip, NULL, NULL);
    report("get_value(huge) is refused", PMIX_SUCCESS != rc);
}

static void test_register_types(void)
{
    int idx;

    bool_var = false;
    idx = pmix_mca_base_var_register("pmix", "utest", "vars", "a_bool", "a boolean variable",
                                     PMIX_MCA_BASE_VAR_TYPE_BOOL, &bool_var);
    report("register bool", 0 <= idx);

    size_var = 7;
    idx = pmix_mca_base_var_register("pmix", "utest", "vars", "a_size", "a size_t variable",
                                     PMIX_MCA_BASE_VAR_TYPE_SIZE_T, &size_var);
    report("register size_t", 0 <= idx);

    double_var = 0.5;
    idx = pmix_mca_base_var_register("pmix", "utest", "vars", "a_double", "a double variable",
                                     PMIX_MCA_BASE_VAR_TYPE_DOUBLE, &double_var);
    report("register double", 0 <= idx);

    /* a string default is duplicated into the variable system, so the
     * caller may keep passing a literal */
    char *literal = (char *) "hello";
    string_var = literal;
    idx = pmix_mca_base_var_register("pmix", "utest", "vars", "a_string", "a string variable",
                                     PMIX_MCA_BASE_VAR_TYPE_STRING, &string_var);
    report("register string", 0 <= idx);
    report("string default was duplicated",
           NULL != string_var && 0 == strcmp(string_var, "hello") && string_var != literal);
}

/* ------------------------------------------------------------------ */
/* values sourced from the environment                                 */
/* ------------------------------------------------------------------ */

static int env_int = 0;
static bool env_bool = false;
static char *env_string = NULL;

/* The environment name is the project prefix plus either the full name
 * (framework_component_variable) or the long name
 * (project_framework_component_variable). Both must work. */
static void test_env_sourced(void)
{
    pmix_mca_base_var_source_t source = PMIX_MCA_BASE_VAR_SOURCE_MAX;
    int *ip = NULL;
    bool *bp = NULL;
    char **sp = NULL;
    int idx, rc;

    env_int = 1;
    idx = pmix_mca_base_var_register("pmix", "utest", "env", "an_int", "from the environment",
                                     PMIX_MCA_BASE_VAR_TYPE_INT, &env_int);
    report("env int registered", 0 <= idx);
    rc = pmix_mca_base_var_get_value(idx, &ip, &source, NULL);
    report("env int picked up the environment value",
           PMIX_SUCCESS == rc && NULL != ip && 99 == *ip);
    report("env int reports ENV as its source", PMIX_MCA_BASE_VAR_SOURCE_ENV == source);

    env_bool = false;
    idx = pmix_mca_base_var_register("pmix", "utest", "env", "a_bool", "from the environment",
                                     PMIX_MCA_BASE_VAR_TYPE_BOOL, &env_bool);
    report("env bool registered", 0 <= idx);
    rc = pmix_mca_base_var_get_value(idx, &bp, NULL, NULL);
    report("env bool parsed \"true\"", PMIX_SUCCESS == rc && NULL != bp && *bp);

    env_string = NULL;
    idx = pmix_mca_base_var_register("pmix", "utest", "env", "a_string", "from the environment",
                                     PMIX_MCA_BASE_VAR_TYPE_STRING, &env_string);
    report("env string registered (long-name form)", 0 <= idx);
    rc = pmix_mca_base_var_get_value(idx, &sp, &source, NULL);
    report("env string picked up the long-name environment value",
           PMIX_SUCCESS == rc && NULL != sp && NULL != *sp && 0 == strcmp(*sp, "from-long-name"));
    report("env string reports ENV as its source", PMIX_MCA_BASE_VAR_SOURCE_ENV == source);
}

/* ------------------------------------------------------------------ */
/* synonyms                                                            */
/* ------------------------------------------------------------------ */

static int syn_var = 0;

static void test_synonyms(void)
{
    int *ip = NULL;
    int idx, syn, found, rc;

    syn_var = 3;
    idx = pmix_mca_base_var_register("pmix", "utest", "syn", "real_name", "the real variable",
                                     PMIX_MCA_BASE_VAR_TYPE_INT, &syn_var);
    report("synonym target registered", 0 <= idx);

    syn = pmix_mca_base_var_register_synonym(idx, "pmix", "utest", "syn", "old_name",
                                             PMIX_MCA_BASE_VAR_SYN_FLAG_DEPRECATED);
    report("synonym registered", 0 <= syn && syn != idx);

    found = pmix_mca_base_var_find("pmix", "utest", "syn", "old_name");
    report("synonym is findable under its own name", found == syn);

    /* looking a synonym up resolves through to the original's storage */
    rc = pmix_mca_base_var_get_value(syn, &ip, NULL, NULL);
    report("synonym resolves to the original storage",
           PMIX_SUCCESS == rc && ip == &syn_var && 3 == *ip);

    /* a synonym of a synonym is not allowed */
    rc = pmix_mca_base_var_register_synonym(syn, "pmix", "utest", "syn", "older_name",
                                            PMIX_MCA_BASE_VAR_SYN_FLAG_DEPRECATED);
    report("synonym of a synonym is refused", 0 > rc);
}

/* ------------------------------------------------------------------ */
/* dump                                                                */
/* ------------------------------------------------------------------ */

static int dump_var = 0;

static void test_dump(void)
{
    char **out = NULL;
    int idx, i, rc;

    dump_var = 11;
    idx = pmix_mca_base_var_register("pmix", "utest", "dump", "a_var", "a variable to dump",
                                     PMIX_MCA_BASE_VAR_TYPE_INT, &dump_var);
    report("dump target registered", 0 <= idx);

    rc = pmix_mca_base_var_dump(idx, &out, PMIX_MCA_BASE_VAR_DUMP_SIMPLE);
    report("simple dump succeeds", PMIX_SUCCESS == rc && NULL != out && NULL != out[0]);
    if (PMIX_SUCCESS == rc && NULL != out) {
        report("simple dump shows name and value",
               NULL != strstr(out[0], "utest_dump_a_var") && NULL != strstr(out[0], "11"));
        for (i = 0; NULL != out[i]; ++i) {
            free(out[i]);
        }
        free(out);
        out = NULL;
    }

    rc = pmix_mca_base_var_dump(idx, &out, PMIX_MCA_BASE_VAR_DUMP_READABLE);
    report("readable dump succeeds", PMIX_SUCCESS == rc && NULL != out && NULL != out[0]);
    if (PMIX_SUCCESS == rc && NULL != out) {
        report("readable dump carries the description",
               NULL != out[1] && NULL != strstr(out[1], "a variable to dump"));
        for (i = 0; NULL != out[i]; ++i) {
            free(out[i]);
        }
        free(out);
        out = NULL;
    }

    rc = pmix_mca_base_var_dump(idx, &out, PMIX_MCA_BASE_VAR_DUMP_PARSABLE);
    report("parsable dump succeeds", PMIX_SUCCESS == rc && NULL != out && NULL != out[0]);
    if (PMIX_SUCCESS == rc && NULL != out) {
        bool saw_value = false, saw_type = false;
        for (i = 0; NULL != out[i]; ++i) {
            if (NULL != strstr(out[i], ":value:")) {
                saw_value = true;
            }
            if (NULL != strstr(out[i], ":type:int")) {
                saw_type = true;
            }
        }
        report("parsable dump emits a value line", saw_value);
        report("parsable dump emits a type line", saw_type);
        for (i = 0; NULL != out[i]; ++i) {
            free(out[i]);
        }
        free(out);
    }
}

/* ------------------------------------------------------------------ */
/* build_env                                                           */
/* ------------------------------------------------------------------ */

/* build_env emits only variables whose value did not come from the
 * default -- it is what gets forwarded to a child process.
 *
 * NOTE: the trickiest inputs here are variables that are *not* backed
 * by storage of their own. A synonym has none (it borrows the
 * variable it stands for) and a deregistered variable has had its
 * storage dropped; walking either one as if it had a value dereferences
 * NULL. A deprecated STRING synonym set from the environment -- which
 * is exactly what "PMIX_MCA_mca_base_param_files" produces, and what
 * this test's own main() sets up -- hits both conditions at once. */
static void test_build_env(void)
{
    char **env = NULL;
    int num_env = 0;
    bool saw_env_int = false, saw_default = false, saw_synonym = false;
    bool saw_string = false;
    int i, rc;

    /* a STRING variable set from the environment, with a deprecated
     * synonym, and a deregistered non-default variable: all three shapes
     * must survive the walk */
    static char *syn_string = NULL;
    static int dead_int = 0;
    int sidx, didx;

    /* main() put "PMIX_MCA_utest_benv_a_string" in the environment, so
     * this variable -- and, through var_set_initial(), the synonym
     * registered next -- carries a non-default source. That is what
     * makes the synonym reach the storage dereference below. */
    syn_string = NULL;
    sidx = pmix_mca_base_var_register("pmix", "utest", "benv", "a_string", "string with a synonym",
                                      PMIX_MCA_BASE_VAR_TYPE_STRING, &syn_string);
    report("build_env: string target registered", 0 <= sidx);
    rc = pmix_mca_base_var_register_synonym(sidx, "pmix", "utest", NULL, "old_string",
                                            PMIX_MCA_BASE_VAR_SYN_FLAG_DEPRECATED);
    report("build_env: string synonym registered", 0 <= rc);
    report("build_env: string target took the environment value",
           NULL != syn_string && 0 == strcmp(syn_string, "env-set"));

    dead_int = 0;
    didx = pmix_mca_base_var_register("pmix", "utest", "benv", "dead_int", "deregistered",
                                      PMIX_MCA_BASE_VAR_TYPE_INT, &dead_int);
    report("build_env: deregistered-var target registered", 0 <= didx);
    (void) pmix_mca_base_var_deregister(didx);

    rc = pmix_mca_base_var_build_env(&env, &num_env);
    report("build_env succeeds", PMIX_SUCCESS == rc);
    if (PMIX_SUCCESS != rc || NULL == env) {
        return;
    }

    for (i = 0; i < num_env; ++i) {
        if (0 == strcmp(env[i], "PMIX_MCA_utest_env_an_int=99")) {
            saw_env_int = true;
        }
        /* utest_vars_an_int was never set by anything, so it must not
         * be forwarded */
        if (0 == strncmp(env[i], "PMIX_MCA_utest_vars_an_int=", 27)) {
            saw_default = true;
        }
        if (0 == strcmp(env[i], "PMIX_MCA_utest_benv_a_string=env-set")) {
            saw_string = true;
        }
        /* "utest_old_string" is the deprecated synonym registered
         * above, and "mca_param_files" is the library's own synonym of
         * "mca_base_param_files" (which main() sets). Neither may be
         * forwarded: only the real names are. */
        if (0 == strncmp(env[i], "PMIX_MCA_utest_old_string=", 26)
            || 0 == strncmp(env[i], "PMIX_MCA_mca_param_files=", 25)) {
            saw_synonym = true;
        }
    }
    report("build_env forwards an environment-sourced value", saw_env_int);
    report("build_env forwards an environment-sourced string", saw_string);
    report("build_env omits default-valued variables", !saw_default);
    report("build_env omits deprecated synonyms", !saw_synonym);

    PMIx_Argv_free(env);
}

/* ------------------------------------------------------------------ */
/* deregister                                                          */
/* ------------------------------------------------------------------ */

static int dereg_var = 0;

static void test_deregister(void)
{
    const pmix_mca_base_var_t *var = NULL;
    int idx, rc;

    dereg_var = 5;
    idx = pmix_mca_base_var_register("pmix", "utest", "dereg", "a_var", "to be deregistered",
                                     PMIX_MCA_BASE_VAR_TYPE_INT, &dereg_var);
    report("deregister target registered", 0 <= idx);

    rc = pmix_mca_base_var_deregister(idx);
    report("deregister succeeds", PMIX_SUCCESS == rc);

    rc = pmix_mca_base_var_get(idx, &var);
    report("deregistered var is no longer gettable", PMIX_SUCCESS != rc);

    rc = pmix_mca_base_var_find("pmix", "utest", "dereg", "a_var");
    report("deregistered var is no longer findable", 0 > rc);

    /* deregistering twice is an error, not a crash */
    rc = pmix_mca_base_var_deregister(idx);
    report("double deregister is refused", PMIX_SUCCESS != rc);

    /* re-registering revives the same index */
    dereg_var = 6;
    rc = pmix_mca_base_var_register("pmix", "utest", "dereg", "a_var", "to be deregistered",
                                    PMIX_MCA_BASE_VAR_TYPE_INT, &dereg_var);
    report("re-register reuses the original index", rc == idx);
}

/* ------------------------------------------------------------------ */
/* check_exclusive                                                     */
/* ------------------------------------------------------------------ */

static int excl_a = 0;
static int excl_b = 0;

static void test_check_exclusive(void)
{
    int rc;

    excl_a = 0;
    (void) pmix_mca_base_var_register("pmix", "utest", "excl", "a", "exclusive a",
                                      PMIX_MCA_BASE_VAR_TYPE_INT, &excl_a);
    excl_b = 0;
    (void) pmix_mca_base_var_register("pmix", "utest", "excl", "b", "exclusive b",
                                      PMIX_MCA_BASE_VAR_TYPE_INT, &excl_b);

    /* neither was set, so both are still at their defaults */
    rc = pmix_mca_base_var_check_exclusive("pmix", "utest", "excl", "a", "utest", "excl", "b");
    report("check_exclusive passes when both are defaults", PMIX_SUCCESS == rc);

    rc = pmix_mca_base_var_check_exclusive("pmix", "utest", "excl", "a", "utest", "excl",
                                           "no_such_var");
    report("check_exclusive reports an unknown variable", PMIX_ERR_NOT_FOUND == rc);

    /* utest_env_an_int and utest_env_a_bool were both set from the
     * environment, so they collide */
    rc = pmix_mca_base_var_check_exclusive("pmix", "utest", "env", "an_int", "utest", "env",
                                           "a_bool");
    report("check_exclusive catches two non-default values", PMIX_ERR_BAD_PARAM == rc);
}

/* ------------------------------------------------------------------ */
/* groups                                                              */
/* ------------------------------------------------------------------ */

static int grp_var = 0;

static void test_groups(void)
{
    const pmix_mca_base_var_group_t *group = NULL;
    int gidx, found, rc;

    grp_var = 1;
    (void) pmix_mca_base_var_register("pmix", "utest", "grp", "a_var", "in a group",
                                      PMIX_MCA_BASE_VAR_TYPE_INT, &grp_var);

    /* find returns the group INDEX, not a status -- the header used to
     * say PMIX_SUCCESS, and every caller in the tree relies on the
     * index */
    gidx = pmix_mca_base_var_group_find("pmix", "utest", "grp");
    report("group_find returns an index", 0 <= gidx);

    rc = pmix_mca_base_var_group_get(gidx, &group);
    report("group_get succeeds", PMIX_SUCCESS == rc && NULL != group);
    if (NULL != group) {
        report("group records its own index", group->group_index == gidx);
        report("group records its framework",
               NULL != group->group_framework && 0 == strcmp(group->group_framework, "utest"));
        report("group records its component",
               NULL != group->group_component && 0 == strcmp(group->group_component, "grp"));
        report("group holds the variable", 0 < pmix_value_array_get_size(
                   (pmix_value_array_t *) &group->group_vars));
    }

    /* wildcards force the linear scan rather than the name hash */
    found = pmix_mca_base_var_group_find("*", "utest", "grp");
    report("group_find honors a wildcard project", found == gidx);

    report("group_find on an unknown group fails",
           0 > pmix_mca_base_var_group_find("pmix", "utest", "no_such_group"));

    /* deregistering the group invalidates it and its variables */
    rc = pmix_mca_base_var_group_deregister(gidx);
    report("group_deregister succeeds", PMIX_SUCCESS == rc);
    report("deregistered group is no longer found",
           0 > pmix_mca_base_var_group_find("pmix", "utest", "grp"));
    report("the group's variable went with it",
           0 > pmix_mca_base_var_find("pmix", "utest", "grp", "a_var"));
}

int main(int argc, char **argv)
{
    int rc;

    (void) argc;
    (void) argv;

    /* Do not let the caller's MCA parameter files influence the
     * results, and pre-set the values the environment-sourced tests
     * expect. All of this has to happen before pmix_init_util()
     * initializes the variable system. */
    setenv("PMIX_MCA_mca_base_param_files", "none", 1);
    setenv("PMIX_MCA_utest_env_an_int", "99", 1);
    setenv("PMIX_MCA_utest_env_a_bool", "true", 1);
    setenv("PMIX_MCA_pmix_utest_env_a_string", "from-long-name", 1);
    setenv("PMIX_MCA_utest_benv_a_string", "env-set", 1);

    rc = pmix_init_util(NULL, 0, NULL);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "pmix_init_util failed: %d\n", rc);
        return 1;
    }

    fprintf(stdout, "\n=== pmix_mca_base_var unit tests ===\n\n");

    test_generate_full_name();
    test_register_and_find();
    test_register_types();
    test_env_sourced();
    test_synonyms();
    test_dump();
    test_build_env();
    test_deregister();
    test_check_exclusive();
    test_groups();

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);

    pmix_finalize_util();

    return (nfail > 0) ? 1 : 0;
}
