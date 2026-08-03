/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit tests for the MCA variable enumerators in
 * src/mca/base/pmix_mca_base_var_enum.c: the two shipped static
 * enumerators (boolean and verbosity), the generic value enumerator
 * built by pmix_mca_base_var_enum_create(), and the flag enumerator
 * built by pmix_mca_base_var_enum_create_flag().
 *
 * Exit 0 if all tests pass, 1 otherwise.
 */

#include "src/include/pmix_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pmix_common.h"
#include "src/mca/base/pmix_base.h"
#include "src/mca/base/pmix_mca_base_var_enum.h"

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
/* boolean enumerator                                                  */
/* ------------------------------------------------------------------ */

static void check_bool_accepts(const char *str, int expected)
{
    char name[128];
    int value = -1;
    int rc;

    rc = pmix_mca_base_var_enum_bool.value_from_string(&pmix_mca_base_var_enum_bool, str, &value);
    snprintf(name, sizeof(name), "bool: \"%s\" -> %d", str, expected);
    report(name, PMIX_SUCCESS == rc && expected == value);
}

static void check_bool_rejects(const char *str)
{
    char name[128];
    int value = -1;
    int rc;

    rc = pmix_mca_base_var_enum_bool.value_from_string(&pmix_mca_base_var_enum_bool, str, &value);
    snprintf(name, sizeof(name), "bool: \"%s\" rejected", str);
    report(name, PMIX_SUCCESS != rc);
}

static void test_bool_values(void)
{
    check_bool_accepts("0", 0);
    check_bool_accepts("1", 1);
    check_bool_accepts("2", 1);
    check_bool_accepts("true", 1);
    check_bool_accepts("false", 0);
    check_bool_accepts("t", 1);
    check_bool_accepts("f", 0);
    check_bool_accepts("yes", 1);
    check_bool_accepts("no", 0);
    check_bool_accepts("enabled", 1);
    check_bool_accepts("disabled", 0);

    /* case insensitivity: the non-enumerated bool path in
     * pmix_mca_base_var.c accepts these, so the enumerator must too */
    check_bool_accepts("TRUE", 1);
    check_bool_accepts("False", 0);
    check_bool_accepts("Yes", 1);

    /* leading whitespace is explicitly skipped */
    check_bool_accepts("   true", 1);

    /* the empty string parses as the integer 0, i.e. false - it is not
     * an error */
    check_bool_accepts("", 0);

    check_bool_rejects("maybe");
    check_bool_rejects("truthy");
}

/* Everything the bool enumerator's dump() advertises has to actually be
 * accepted by its value_from_string(): the dump is what a user reads out
 * of "pmix_info", so a value listed there and then refused is a bug.
 * This walks the dump text and feeds every non-numeric token back in. */
static void test_bool_dump_is_honest(void)
{
    char *dump = NULL;
    char *copy, *tok, *saveptr = NULL;
    int rc, ok = 1;

    rc = pmix_mca_base_var_enum_bool.dump(&pmix_mca_base_var_enum_bool, &dump,
                                          PMIX_MCA_BASE_VAR_ENUM_DUMP_READABLE);
    report("bool: dump succeeds", PMIX_SUCCESS == rc && NULL != dump);
    if (PMIX_SUCCESS != rc || NULL == dump) {
        return;
    }

    copy = strdup(dump);
    for (tok = strtok_r(copy, "|, ", &saveptr); NULL != tok;
         tok = strtok_r(NULL, "|, ", &saveptr)) {
        int value = -1;
        if (PMIX_SUCCESS
            != pmix_mca_base_var_enum_bool.value_from_string(&pmix_mca_base_var_enum_bool, tok,
                                                             &value)) {
            fprintf(stdout, "        dump advertises \"%s\" but it is rejected\n", tok);
            ok = 0;
        }
    }
    report("bool: every advertised value is accepted", ok);

    free(copy);
    free(dump);
}

static void test_bool_string_from_value(void)
{
    char *str = NULL;
    int rc;

    rc = pmix_mca_base_var_enum_bool.string_from_value(&pmix_mca_base_var_enum_bool, 1, &str);
    report("bool: 1 -> \"true\"", PMIX_SUCCESS == rc && NULL != str && 0 == strcmp(str, "true"));
    free(str);
    str = NULL;

    rc = pmix_mca_base_var_enum_bool.string_from_value(&pmix_mca_base_var_enum_bool, 0, &str);
    report("bool: 0 -> \"false\"", PMIX_SUCCESS == rc && NULL != str && 0 == strcmp(str, "false"));
    free(str);
}

/* ------------------------------------------------------------------ */
/* verbosity enumerator                                                */
/* ------------------------------------------------------------------ */

static void test_verbose_enum(void)
{
    int value = -1;
    char *str = NULL;
    int count = 0;
    int rc;

    rc = pmix_mca_base_var_enum_verbose.get_count(&pmix_mca_base_var_enum_verbose, &count);
    report("verbose: get_count", PMIX_SUCCESS == rc && 8 == count);

    rc = pmix_mca_base_var_enum_verbose.value_from_string(&pmix_mca_base_var_enum_verbose, "debug",
                                                          &value);
    report("verbose: \"debug\" resolves", PMIX_SUCCESS == rc && PMIX_MCA_BASE_VERBOSE_DEBUG == value);

    rc = pmix_mca_base_var_enum_verbose.value_from_string(&pmix_mca_base_var_enum_verbose, "42",
                                                          &value);
    report("verbose: plain integer passes through", PMIX_SUCCESS == rc && 42 == value);

    /* out of range integers clamp rather than fail */
    rc = pmix_mca_base_var_enum_verbose.value_from_string(&pmix_mca_base_var_enum_verbose, "5000",
                                                          &value);
    report("verbose: above max clamps to max",
           PMIX_SUCCESS == rc && PMIX_MCA_BASE_VERBOSE_MAX == value);

    rc = pmix_mca_base_var_enum_verbose.value_from_string(&pmix_mca_base_var_enum_verbose, "bogus",
                                                          &value);
    report("verbose: unknown name rejected", PMIX_SUCCESS != rc);

    rc = pmix_mca_base_var_enum_verbose.string_from_value(&pmix_mca_base_var_enum_verbose,
                                                          PMIX_MCA_BASE_VERBOSE_INFO, &str);
    report("verbose: value -> \"info\"",
           PMIX_SUCCESS == rc && NULL != str && 0 == strcmp(str, "info"));
    free(str);
    str = NULL;

    /* a value with no name is rendered numerically */
    rc = pmix_mca_base_var_enum_verbose.string_from_value(&pmix_mca_base_var_enum_verbose, 42, &str);
    report("verbose: unnamed value -> \"42\"",
           PMIX_SUCCESS == rc && NULL != str && 0 == strcmp(str, "42"));
    free(str);

    /* string_value is documented as optional */
    rc = pmix_mca_base_var_enum_verbose.string_from_value(&pmix_mca_base_var_enum_verbose,
                                                          PMIX_MCA_BASE_VERBOSE_INFO, NULL);
    report("verbose: NULL string_value tolerated", PMIX_SUCCESS == rc);
}

/* ------------------------------------------------------------------ */
/* generic value enumerator                                            */
/* ------------------------------------------------------------------ */

static void test_value_enum(void)
{
    static const pmix_mca_base_var_enum_value_t values[] = {{1, "one"},
                                                            {2, "two"},
                                                            {7, "seven"},
                                                            {0, NULL}};
    pmix_mca_base_var_enum_t *e = NULL;
    const char *sval = NULL;
    char *str = NULL;
    int value = -1, count = 0;
    int rc;

    rc = pmix_mca_base_var_enum_create("test-values", values, &e);
    report("value enum: created", PMIX_SUCCESS == rc && NULL != e);
    if (PMIX_SUCCESS != rc || NULL == e) {
        return;
    }

    rc = e->get_count(e, &count);
    report("value enum: count excludes terminator", PMIX_SUCCESS == rc && 3 == count);

    rc = e->get_value(e, 1, &value, &sval);
    report("value enum: get_value(1)",
           PMIX_SUCCESS == rc && 2 == value && NULL != sval && 0 == strcmp(sval, "two"));

    /* the string handed back by get_value is borrowed - it must point
     * into the enumerator's own copy, not a fresh allocation the caller
     * would have to free (which it has no way to know to do, since the
     * bool enumerator returns a literal here) */
    report("value enum: get_value string is borrowed", sval == e->enum_values[1].string);

    rc = e->get_value(e, count, &value, &sval);
    report("value enum: index == count rejected", PMIX_SUCCESS != rc);

    rc = e->get_value(e, -1, &value, &sval);
    report("value enum: negative index rejected", PMIX_SUCCESS != rc);

    rc = e->value_from_string(e, "seven", &value);
    report("value enum: name resolves", PMIX_SUCCESS == rc && 7 == value);

    rc = e->value_from_string(e, "SEVEN", &value);
    report("value enum: name match is case-insensitive", PMIX_SUCCESS == rc && 7 == value);

    rc = e->value_from_string(e, "7", &value);
    report("value enum: integer resolves", PMIX_SUCCESS == rc && 7 == value);

    rc = e->value_from_string(e, "3", &value);
    report("value enum: unlisted integer rejected", PMIX_SUCCESS != rc);

    rc = e->string_from_value(e, 2, &str);
    report("value enum: value -> name",
           PMIX_SUCCESS == rc && NULL != str && 0 == strcmp(str, "two"));
    free(str);
    str = NULL;

    rc = e->string_from_value(e, 3, &str);
    report("value enum: unlisted value rejected", PMIX_SUCCESS != rc);

    rc = e->dump(e, &str, PMIX_MCA_BASE_VAR_ENUM_DUMP_READABLE);
    report("value enum: dump lists every entry",
           PMIX_SUCCESS == rc && NULL != str && NULL != strstr(str, "one")
               && NULL != strstr(str, "two") && NULL != strstr(str, "seven"));
    free(str);

    PMIX_RELEASE(e);
}

/* ------------------------------------------------------------------ */
/* flag enumerator                                                     */
/* ------------------------------------------------------------------ */

/* The flag enumerator's value_from_string() takes a comma-delimited
 * list. Its table lookup is nested inside the loop over the caller's
 * tokens, and the two index spaces are unrelated -- a version of this
 * code that confused them both matched the wrong flag and read past the
 * end of the table whenever more tokens were passed than the enumerator
 * had flags. Hence the deliberately-longer-than-the-table cases below. */
static void test_flag_enum(void)
{
    static const pmix_mca_base_var_enum_value_flag_t flags[] = {{0x1, "alpha", 0x2},
                                                                {0x2, "beta", 0x1},
                                                                {0x4, "gamma", 0},
                                                                {0, NULL, 0}};
    pmix_mca_base_var_enum_flag_t *fe = NULL;
    pmix_mca_base_var_enum_t *e;
    const char *sval = NULL;
    char *str = NULL;
    int value = -1, count = 0;
    int rc;

    rc = pmix_mca_base_var_enum_create_flag("test-flags", flags, &fe);
    report("flag enum: created", PMIX_SUCCESS == rc && NULL != fe);
    if (PMIX_SUCCESS != rc || NULL == fe) {
        return;
    }
    e = &fe->super;

    rc = e->get_count(e, &count);
    report("flag enum: count excludes terminator", PMIX_SUCCESS == rc && 3 == count);

    rc = e->get_value(e, 2, &value, &sval);
    report("flag enum: get_value(2)",
           PMIX_SUCCESS == rc && 0x4 == value && NULL != sval && 0 == strcmp(sval, "gamma"));
    report("flag enum: get_value string is borrowed", sval == fe->enum_flags[2].string);

    /* single token, last entry in the table: only resolves if the table
     * is searched by its own index */
    rc = e->value_from_string(e, "gamma", &value);
    report("flag enum: last table entry resolves", PMIX_SUCCESS == rc && 0x4 == value);

    rc = e->value_from_string(e, "alpha", &value);
    report("flag enum: first table entry resolves", PMIX_SUCCESS == rc && 0x1 == value);

    /* two tokens in the reverse of table order */
    rc = e->value_from_string(e, "gamma,alpha", &value);
    report("flag enum: out-of-order pair ORs together",
           PMIX_SUCCESS == rc && (0x4 | 0x1) == value);

    rc = e->value_from_string(e, "0x4", &value);
    report("flag enum: numeric token resolves", PMIX_SUCCESS == rc && 0x4 == value);

    /* conflicting flags must be refused, not silently merged */
    rc = e->value_from_string(e, "alpha,beta", &value);
    report("flag enum: conflicting flags rejected", PMIX_ERR_BAD_PARAM == rc);

    rc = e->value_from_string(e, "delta", &value);
    report("flag enum: unknown token rejected", PMIX_ERR_VALUE_OUT_OF_BOUNDS == rc);

    /* more tokens than the table has entries: an implementation that
     * indexed the table with the token index would run off the end here */
    rc = e->value_from_string(e, "gamma,gamma,gamma,gamma,gamma,gamma", &value);
    report("flag enum: more tokens than table entries stays in bounds",
           PMIX_SUCCESS == rc && 0x4 == value);

    rc = e->value_from_string(e, "gamma,gamma,gamma,nope", &value);
    report("flag enum: bad token past table length still rejected",
           PMIX_ERR_VALUE_OUT_OF_BOUNDS == rc);

    rc = e->string_from_value(e, 0x4 | 0x1, &str);
    report("flag enum: value -> comma list",
           PMIX_SUCCESS == rc && NULL != str && NULL != strstr(str, "alpha")
               && NULL != strstr(str, "gamma"));
    free(str);
    str = NULL;

    rc = e->string_from_value(e, 0x8, &str);
    report("flag enum: unknown bit rejected", PMIX_ERR_VALUE_OUT_OF_BOUNDS == rc);

    rc = e->dump(e, &str, PMIX_MCA_BASE_VAR_ENUM_DUMP_READABLE);
    report("flag enum: dump lists every entry",
           PMIX_SUCCESS == rc && NULL != str && NULL != strstr(str, "alpha")
               && NULL != strstr(str, "beta") && NULL != strstr(str, "gamma"));
    free(str);

    PMIX_RELEASE(fe);
}

int main(int argc, char **argv)
{
    (void) argc;
    (void) argv;

    fprintf(stdout, "\n=== pmix_mca_base_var_enum unit tests ===\n\n");

    test_bool_values();
    test_bool_dump_is_honest();
    test_bool_string_from_value();
    test_verbose_enum();
    test_value_enum();
    test_flag_enum();

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);
    return (nfail > 0) ? 1 : 0;
}
