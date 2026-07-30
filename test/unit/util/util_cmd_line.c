/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit tests for pmix_cmd_line parsing utility:
 *   pmix_cmd_line_parse, pmix_cmd_line_is_taken,
 *   pmix_cmd_line_get_param, pmix_cmd_line_get_ninsts,
 *   pmix_cmd_line_get_nth_instance, pmix_check_cli_option,
 *   pmix_convert_string_to_time.
 *
 * The bulk of this is a table of command lines run through
 * pmix_cmd_line_parse() against one realistic option table. A parser like
 * this one has no interesting internal state to inspect - what matters is
 * the mapping from an argv to (return code, recorded options, tail) - and
 * the failures it has had were all of the shape "one input is read wrongly
 * while its neighbours are fine". A table is the only economical way to
 * hold enough neighbours.
 *
 * PMIx_Init is called first; PMIX_ERR_UNREACH is treated as normal.
 *
 * Exit 0 if all tests pass, 1 otherwise.
 */

#include "src/include/pmix_config.h"
#include "src/include/pmix_globals.h"

#include <fcntl.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "pmix.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_cmd_line.h"
#include "src/util/pmix_printf.h"

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

/* Options used by the hand-written tests below. */
static struct option myopts[] = {
    PMIX_OPTION_DEFINE(PMIX_CLI_VERBOSE, PMIX_ARG_NONE),
    PMIX_OPTION_DEFINE(PMIX_CLI_TIMEOUT, PMIX_ARG_REQD),
    PMIX_OPTION_DEFINE(PMIX_CLI_NAMESPACE, PMIX_ARG_REQD),
    PMIX_OPTION_END
};

/* ------------------------------------------------------------------ */
/* pmix_cmd_line_parse                                                 */
/* ------------------------------------------------------------------ */

static void test_parse_empty(void)
{
    char *argv[] = {"prog", NULL};
    pmix_cli_result_t res;
    int rc;

    PMIX_CONSTRUCT(&res, pmix_cli_result_t);
    rc = pmix_cmd_line_parse(argv, "", myopts, NULL, &res, NULL);
    report("parse_empty: returns SUCCESS", PMIX_SUCCESS == rc);
    report("parse_empty: verbose not taken",
           !pmix_cmd_line_is_taken(&res, PMIX_CLI_VERBOSE));
    PMIX_DESTRUCT(&res);
}

static void test_parse_flag(void)
{
    char *argv[] = {"prog", "--verbose", NULL};
    pmix_cli_result_t res;
    int rc;

    PMIX_CONSTRUCT(&res, pmix_cli_result_t);
    rc = pmix_cmd_line_parse(argv, "", myopts, NULL, &res, NULL);
    report("parse_flag: returns SUCCESS", PMIX_SUCCESS == rc);
    report("parse_flag: verbose taken",
           pmix_cmd_line_is_taken(&res, PMIX_CLI_VERBOSE));
    report("parse_flag: timeout not taken",
           !pmix_cmd_line_is_taken(&res, PMIX_CLI_TIMEOUT));
    PMIX_DESTRUCT(&res);
}

static void test_parse_required_arg(void)
{
    char *argv[] = {"prog", "--timeout", "30", NULL};
    pmix_cli_result_t res;
    pmix_cli_item_t *opt;
    int rc;

    PMIX_CONSTRUCT(&res, pmix_cli_result_t);
    rc = pmix_cmd_line_parse(argv, "", myopts, NULL, &res, NULL);
    report("parse_req_arg: returns SUCCESS", PMIX_SUCCESS == rc);
    report("parse_req_arg: timeout taken",
           pmix_cmd_line_is_taken(&res, PMIX_CLI_TIMEOUT));
    opt = pmix_cmd_line_get_param(&res, PMIX_CLI_TIMEOUT);
    report("parse_req_arg: item non-NULL", opt != NULL);
    report("parse_req_arg: value is \"30\"",
           opt != NULL && opt->values != NULL
               && 0 == strcmp(opt->values[0], "30"));
    PMIX_DESTRUCT(&res);
}

static void test_parse_multiple_options(void)
{
    char *argv[] = {"prog", "--verbose", "--namespace", "myns", NULL};
    pmix_cli_result_t res;
    pmix_cli_item_t *opt;
    int rc;

    PMIX_CONSTRUCT(&res, pmix_cli_result_t);
    rc = pmix_cmd_line_parse(argv, "", myopts, NULL, &res, NULL);
    report("parse_multi: returns SUCCESS", PMIX_SUCCESS == rc);
    report("parse_multi: verbose taken",
           pmix_cmd_line_is_taken(&res, PMIX_CLI_VERBOSE));
    opt = pmix_cmd_line_get_param(&res, PMIX_CLI_NAMESPACE);
    report("parse_multi: namespace item non-NULL", opt != NULL);
    report("parse_multi: namespace value is \"myns\"",
           opt != NULL && opt->values != NULL
               && 0 == strcmp(opt->values[0], "myns"));
    PMIX_DESTRUCT(&res);
}

static void test_parse_not_taken(void)
{
    char *argv[] = {"prog", "--verbose", NULL};
    pmix_cli_result_t res;

    PMIX_CONSTRUCT(&res, pmix_cli_result_t);
    pmix_cmd_line_parse(argv, "", myopts, NULL, &res, NULL);
    report("parse_not_taken: timeout absent",
           !pmix_cmd_line_is_taken(&res, PMIX_CLI_TIMEOUT));
    report("parse_not_taken: get_param returns NULL",
           NULL == pmix_cmd_line_get_param(&res, PMIX_CLI_TIMEOUT));
    report("parse_not_taken: get_ninsts returns 0",
           0 == pmix_cmd_line_get_ninsts(&res, PMIX_CLI_TIMEOUT));
    PMIX_DESTRUCT(&res);
}

/* ================================================================== */
/* The table                                                          */
/* ================================================================== */

/* One option table for every case below, shaped like the ones the tools
 * actually register (see prteshorts/prterunshorts in PRRTE's schizo): a
 * short-only flag or two, an optional-argument short ('h::'), a
 * required-argument short ('n:'), long options with and without arguments,
 * the two-token MCA option, and "--show-version", which takes zero, one, or
 * two trailing tokens. */
static struct option tblopts[] = {
    PMIX_OPTION_SHORT_DEFINE(PMIX_CLI_HELP, PMIX_ARG_OPTIONAL, 'h'),
    PMIX_OPTION_SHORT_DEFINE(PMIX_CLI_VERSION, PMIX_ARG_NONE, 'V'),
    PMIX_OPTION_SHORT_DEFINE(PMIX_CLI_VERBOSE, PMIX_ARG_NONE, 'v'),
    PMIX_OPTION_DEFINE(PMIX_CLI_TIMEOUT, PMIX_ARG_REQD),
    PMIX_OPTION_DEFINE(PMIX_CLI_NAMESPACE, PMIX_ARG_REQD),
    PMIX_OPTION_DEFINE(PMIX_CLI_URI, PMIX_ARG_REQD),
    PMIX_OPTION_DEFINE(PMIX_CLI_PMIXMCA, PMIX_ARG_REQD),
    PMIX_OPTION_DEFINE(PMIX_CLI_INFO_VERSION, PMIX_ARG_OPTIONAL),
    PMIX_OPTION_SHORT_DEFINE("np", PMIX_ARG_REQD, 'n'),
    PMIX_OPTION_END
};
static char *tblshorts = "h::vVn:";

#define MAXARGS 10

typedef struct {
    const char *desc;
    const char *argv[MAXARGS];
    int rc;
    const char *key;      /* option that must be present, or NULL */
    const char *vals;     /* '|'-joined expected values; "" = present with
                           * none; NULL = do not inspect the values */
    const char *key2;     /* a second option that must be present, or NULL */
    const char *vals2;
    const char *absent;   /* option that must NOT be present, or NULL */
    const char *tail;     /* ' '-joined expected tail; NULL = no tail */
} parse_case_t;

static parse_case_t cases[] = {

    /* ---- MCA parameters ---------------------------------------------
     * "--pmixmca <param> <value>" is two tokens, but getopt is told the
     * option takes one. The value is therefore grabbed by hand from
     * argv[optind], which makes "is my value even there?" this parser's
     * own problem - and that decision is where it went wrong. */
    {"mca: plain value",
     {"prog", "--pmixmca", "p", "v", NULL}, PMIX_SUCCESS,
     PMIX_CLI_PMIXMCA, "p=v", NULL, NULL, NULL, NULL},

    /* THE regression: the missing-argument check tested the value's
     * SECOND character for a dash, so every range-valued MCA parameter
     * was rejected as though no value had been given. */
    {"mca: value with an interior dash (a range)",
     {"prog", "--pmixmca", "hwloc_default_cpu_list", "0-1", NULL}, PMIX_SUCCESS,
     PMIX_CLI_PMIXMCA, "hwloc_default_cpu_list=0-1", NULL, NULL, NULL, NULL},

    {"mca: two-digit range value",
     {"prog", "--pmixmca", "p", "10-20", NULL}, PMIX_SUCCESS,
     PMIX_CLI_PMIXMCA, "p=10-20", NULL, NULL, NULL, NULL},

    {"mca: value with several dashes",
     {"prog", "--pmixmca", "p", "a-b-c", NULL}, PMIX_SUCCESS,
     PMIX_CLI_PMIXMCA, "p=a-b-c", NULL, NULL, NULL, NULL},

    /* A value that legitimately STARTS with a dash. Negative MCA values
     * are real - prte_max_vm_size and prte_progress_thread_debug_level
     * both use -1 - so this is the pattern that "just test argv[n][0]
     * for a dash instead" would have broken. */
    {"mca: negative value",
     {"prog", "--pmixmca", "prte_max_vm_size", "-1", NULL}, PMIX_SUCCESS,
     PMIX_CLI_PMIXMCA, "prte_max_vm_size=-1", NULL, NULL, NULL, NULL},

    {"mca: negative multi-digit value",
     {"prog", "--pmixmca", "p", "-100", NULL}, PMIX_SUCCESS,
     PMIX_CLI_PMIXMCA, "p=-100", NULL, NULL, NULL, NULL},

    /* Dash-leading, and 'n' IS a registered short option - but the token
     * as a whole names no option, so it is a value. */
    {"mca: dash value that is not a whole option",
     {"prog", "--pmixmca", "p", "-n4", NULL}, PMIX_SUCCESS,
     PMIX_CLI_PMIXMCA, "p=-n4", NULL, NULL, NULL, NULL},

    {"mca: dash value naming an unregistered letter",
     {"prog", "--pmixmca", "p", "-Z", NULL}, PMIX_SUCCESS,
     PMIX_CLI_PMIXMCA, "p=-Z", NULL, NULL, NULL, NULL},

    {"mca: value with a colon",
     {"prog", "--pmixmca", "p", "1:2", NULL}, PMIX_SUCCESS,
     PMIX_CLI_PMIXMCA, "p=1:2", NULL, NULL, NULL, NULL},

    {"mca: value containing an equals",
     {"prog", "--pmixmca", "p", "a=b", NULL}, PMIX_SUCCESS,
     PMIX_CLI_PMIXMCA, "p=a=b", NULL, NULL, NULL, NULL},

    /* ...and the diagnostics that must still fire: each of these really
     * is a missing value. */
    {"mca: no value at all is refused",
     {"prog", "--pmixmca", "p", NULL}, PMIX_ERR_SILENT,
     NULL, NULL, NULL, NULL, NULL, NULL},

    {"mca: a following long option is refused",
     {"prog", "--pmixmca", "p", "--verbose", NULL}, PMIX_ERR_SILENT,
     NULL, NULL, NULL, NULL, NULL, NULL},

    {"mca: a following long option with an argument is refused",
     {"prog", "--pmixmca", "p", "--timeout", "30", NULL}, PMIX_ERR_SILENT,
     NULL, NULL, NULL, NULL, NULL, NULL},

    {"mca: a following abbreviated long option is refused",
     {"prog", "--pmixmca", "p", "--verb", NULL}, PMIX_ERR_SILENT,
     NULL, NULL, NULL, NULL, NULL, NULL},

    {"mca: a following short option is refused",
     {"prog", "--pmixmca", "p", "-v", NULL}, PMIX_ERR_SILENT,
     NULL, NULL, NULL, NULL, NULL, NULL},

    {"mca: a following short cluster is refused",
     {"prog", "--pmixmca", "p", "-vvv", NULL}, PMIX_ERR_SILENT,
     NULL, NULL, NULL, NULL, NULL, NULL},

    {"mca: a following double-dash is refused",
     {"prog", "--pmixmca", "p", "--", "app", NULL}, PMIX_ERR_SILENT,
     NULL, NULL, NULL, NULL, NULL, NULL},

    /* ...and parsing carries on correctly afterwards */
    {"mca: two parameters, both recorded",
     {"prog", "--pmixmca", "p", "v", "--pmixmca", "q", "0-1", NULL}, PMIX_SUCCESS,
     PMIX_CLI_PMIXMCA, "p=v|q=0-1", NULL, NULL, NULL, NULL},

    {"mca: a range value, then a long option with an argument",
     {"prog", "--pmixmca", "p", "0-1", "--timeout", "30", NULL}, PMIX_SUCCESS,
     PMIX_CLI_PMIXMCA, "p=0-1", PMIX_CLI_TIMEOUT, "30", NULL, NULL},

    {"mca: a range value does not swallow the application",
     {"prog", "--pmixmca", "p", "0-1", "--", "app", "arg", NULL}, PMIX_SUCCESS,
     PMIX_CLI_PMIXMCA, "p=0-1", NULL, NULL, NULL, "app arg"},

    /* ---- long options --------------------------------------------- */
    {"long: flag",
     {"prog", "--verbose", NULL}, PMIX_SUCCESS,
     PMIX_CLI_VERBOSE, NULL, NULL, NULL, PMIX_CLI_TIMEOUT, NULL},

    {"long: required argument as a separate token",
     {"prog", "--timeout", "30", NULL}, PMIX_SUCCESS,
     PMIX_CLI_TIMEOUT, "30", NULL, NULL, NULL, NULL},

    {"long: required argument in the '=' form",
     {"prog", "--timeout=30", NULL}, PMIX_SUCCESS,
     PMIX_CLI_TIMEOUT, "30", NULL, NULL, NULL, NULL},

    {"long: required argument that starts with a dash",
     {"prog", "--timeout=-1", NULL}, PMIX_SUCCESS,
     PMIX_CLI_TIMEOUT, "-1", NULL, NULL, NULL, NULL},

    {"long: required argument containing a dash",
     {"prog", "--uri", "tcp4://1.2.3.4:100-200", NULL}, PMIX_SUCCESS,
     PMIX_CLI_URI, "tcp4://1.2.3.4:100-200", NULL, NULL, NULL, NULL},

    {"long: unambiguous abbreviation resolves",
     {"prog", "--namesp", "myns", NULL}, PMIX_SUCCESS,
     PMIX_CLI_NAMESPACE, "myns", NULL, NULL, NULL, NULL},

    {"long: two different options",
     {"prog", "--timeout", "30", "--namespace", "myns", NULL}, PMIX_SUCCESS,
     PMIX_CLI_TIMEOUT, "30", PMIX_CLI_NAMESPACE, "myns", NULL, NULL},

    {"long: a repeated option records both instances",
     {"prog", "--namespace", "a", "--namespace", "b", NULL}, PMIX_SUCCESS,
     PMIX_CLI_NAMESPACE, "a|b", NULL, NULL, NULL, NULL},

    {"long: an unregistered option is refused",
     {"prog", "--nosuchoption", NULL}, PMIX_ERR_SILENT,
     NULL, NULL, NULL, NULL, NULL, NULL},

    {"long: a registered option missing its argument is refused",
     {"prog", "--timeout", NULL}, PMIX_ERR_SILENT,
     NULL, NULL, NULL, NULL, NULL, NULL},

    /* ---- short options -------------------------------------------- */
    {"short: required argument as a separate token",
     {"prog", "-n", "4", NULL}, PMIX_SUCCESS,
     "np", "4", NULL, NULL, NULL, NULL},

    {"short: required argument attached",
     {"prog", "-n4", NULL}, PMIX_SUCCESS,
     "np", "4", NULL, NULL, NULL, NULL},

    {"short: verbose counts one",
     {"prog", "-v", NULL}, PMIX_SUCCESS,
     PMIX_CLI_VERBOSE, "1", NULL, NULL, NULL, NULL},

    {"short: a verbose cluster counts three",
     {"prog", "-vvv", NULL}, PMIX_SUCCESS,
     PMIX_CLI_VERBOSE, "3", NULL, NULL, NULL, NULL},

    {"short: followed by a long option",
     {"prog", "-n", "4", "--timeout", "30", NULL}, PMIX_SUCCESS,
     "np", "4", PMIX_CLI_TIMEOUT, "30", NULL, NULL},

    /* ---- the tail -------------------------------------------------- */
    {"tail: a bare command with no options",
     {"prog", "app", "arg", NULL}, PMIX_SUCCESS,
     NULL, NULL, NULL, NULL, NULL, "app arg"},

    {"tail: after a flag",
     {"prog", "--verbose", "app", "arg", NULL}, PMIX_SUCCESS,
     PMIX_CLI_VERBOSE, NULL, NULL, NULL, NULL, "app arg"},

    {"tail: after an option with an argument",
     {"prog", "--timeout", "30", "app", NULL}, PMIX_SUCCESS,
     PMIX_CLI_TIMEOUT, "30", NULL, NULL, NULL, "app"},

    {"tail: double-dash separator",
     {"prog", "--verbose", "--", "app", "arg", NULL}, PMIX_SUCCESS,
     PMIX_CLI_VERBOSE, NULL, NULL, NULL, NULL, "app arg"},

    {"tail: the application's own dashed options are not ours",
     {"prog", "--", "app", "--verbose", NULL}, PMIX_SUCCESS,
     NULL, NULL, NULL, NULL, PMIX_CLI_VERBOSE, "app --verbose"},

    {"tail: a lone '&' is dropped",
     {"prog", "--verbose", "&", NULL}, PMIX_SUCCESS,
     PMIX_CLI_VERBOSE, NULL, NULL, NULL, NULL, NULL},

    {"tail: nothing but the program name",
     {"prog", NULL}, PMIX_SUCCESS,
     NULL, NULL, NULL, NULL, PMIX_CLI_VERBOSE, NULL},

    /* ---- --show-version -------------------------------------------
     * Zero, one, or two trailing tokens, so it makes the same "is the
     * next token an option?" decision the MCA path does. */
    {"show-version: no arguments",
     {"prog", "--show-version", NULL}, PMIX_SUCCESS,
     PMIX_CLI_INFO_VERSION, "", NULL, NULL, NULL, NULL},

    {"show-version: one argument",
     {"prog", "--show-version", "pmix", NULL}, PMIX_SUCCESS,
     PMIX_CLI_INFO_VERSION, "pmix", NULL, NULL, NULL, NULL},

    {"show-version: a following long option is not an argument",
     {"prog", "--show-version", "--verbose", NULL}, PMIX_SUCCESS,
     PMIX_CLI_INFO_VERSION, "", PMIX_CLI_VERBOSE, NULL, NULL, NULL},

    /* Same defect, the other direction: a short option used to be taken
     * as --show-version's argument, because only argv[n][1] was looked at. */
    {"show-version: a following short option is not an argument",
     {"prog", "--show-version", "-v", NULL}, PMIX_SUCCESS,
     PMIX_CLI_INFO_VERSION, "", PMIX_CLI_VERBOSE, "1", NULL, NULL},

    {NULL, {NULL}, 0, NULL, NULL, NULL, NULL, NULL, NULL}
};

/* Render an option's recorded values as a '|'-joined string, or NULL if the
 * option is absent. An option present with no values renders as "". */
static char *render(pmix_cli_result_t *res, const char *key)
{
    pmix_cli_item_t *opt;
    char *out;

    opt = pmix_cmd_line_get_param(res, key);
    if (NULL == opt) {
        return NULL;
    }
    if (NULL == opt->values) {
        return strdup("");
    }
    out = PMIx_Argv_join(opt->values, '|');
    return (NULL == out) ? strdup("") : out;
}

static bool check_key(const char *desc, pmix_cli_result_t *res,
                      const char *key, const char *vals)
{
    char *got;
    bool ok;

    if (NULL == key) {
        return true;
    }
    got = render(res, key);
    if (NULL == got) {
        fprintf(stdout, "  FAIL: %s [option \"%s\" absent]\n", desc, key);
        nfail++;
        return false;
    }
    ok = (NULL == vals) || (0 == strcmp(got, vals));
    if (!ok) {
        fprintf(stdout, "  FAIL: %s [option \"%s\": wanted \"%s\", got \"%s\"]\n",
                desc, key, vals, got);
        nfail++;
    }
    free(got);
    return ok;
}

static void run_table(void)
{
    parse_case_t *c;
    pmix_cli_result_t res;
    int rc, saved_err, devnull;
    char *got;
    bool ok;

    /* Each error case prints a help block to stderr. That is the correct
     * behavior, but it buries the results, so park stderr for the duration
     * and put it back afterwards. */
    saved_err = dup(fileno(stderr));
    devnull = open("/dev/null", O_WRONLY);

    for (c = cases; NULL != c->desc; c++) {
        ok = true;
        PMIX_CONSTRUCT(&res, pmix_cli_result_t);

        if (0 <= devnull) {
            fflush(stderr);
            dup2(devnull, fileno(stderr));
        }
        rc = pmix_cmd_line_parse((char **) c->argv, tblshorts, tblopts,
                                 NULL, &res, "help-cli.txt");
        if (0 <= saved_err) {
            fflush(stderr);
            dup2(saved_err, fileno(stderr));
        }

        if (rc != c->rc) {
            fprintf(stdout, "  FAIL: %s [rc: wanted %d (%s), got %d (%s)]\n",
                    c->desc, c->rc, PMIx_Error_string(c->rc),
                    rc, PMIx_Error_string(rc));
            nfail++;
            ok = false;
        }

        /* only inspect the results of a parse that was meant to succeed */
        if (PMIX_SUCCESS == c->rc) {
            ok = check_key(c->desc, &res, c->key, c->vals) && ok;
            ok = check_key(c->desc, &res, c->key2, c->vals2) && ok;

            if (NULL != c->absent &&
                NULL != (got = render(&res, c->absent))) {
                fprintf(stdout, "  FAIL: %s [option \"%s\" should be absent,"
                        " got \"%s\"]\n", c->desc, c->absent, got);
                free(got);
                nfail++;
                ok = false;
            }

            got = (NULL == res.tail) ? NULL : PMIx_Argv_join(res.tail, ' ');
            if (NULL == c->tail) {
                if (NULL != got) {
                    fprintf(stdout, "  FAIL: %s [tail should be empty,"
                            " got \"%s\"]\n", c->desc, got);
                    nfail++;
                    ok = false;
                }
            } else if (NULL == got || 0 != strcmp(got, c->tail)) {
                fprintf(stdout, "  FAIL: %s [tail: wanted \"%s\", got \"%s\"]\n",
                        c->desc, c->tail, (NULL == got) ? "(empty)" : got);
                nfail++;
                ok = false;
            }
            if (NULL != got) {
                free(got);
            }
        }

        if (ok) {
            fprintf(stdout, "  PASS: %s\n", c->desc);
            npass++;
        }
        PMIX_DESTRUCT(&res);
    }

    if (0 <= devnull) {
        close(devnull);
    }
    if (0 <= saved_err) {
        close(saved_err);
    }
}

/* ================================================================== */
/* Occurrence order                                                   */
/* ================================================================== */

/* The grouped view of a parse result - one item per option, every
 * occurrence of that option on it - cannot express the interleaving of
 * two repeated options. These check the record that can: the position
 * stamped on each stored value, and the ordered view built from it.
 *
 * The environment options are the reason this matters, so they are what
 * is tested with: --set-env replaces a value outright while
 * --prepend-env edits the one already there, so the order they are
 * applied in is the difference between FOO=2 and FOO=x:2. */
static struct option ordopts[] = {
    PMIX_OPTION_DEFINE(PMIX_CLI_SET_ENVAR, PMIX_ARG_REQD),
    PMIX_OPTION_DEFINE(PMIX_CLI_PREPEND_ENVAR, PMIX_ARG_REQD),
    PMIX_OPTION_DEFINE(PMIX_CLI_UNSET_ENVAR, PMIX_ARG_REQD),
    PMIX_OPTION_DEFINE(PMIX_CLI_TIMEOUT, PMIX_ARG_REQD),
    PMIX_OPTION_DEFINE(PMIX_CLI_PARSABLE, PMIX_ARG_NONE),
    PMIX_OPTION_END
};

/* Render the ordered view as "key:value" entries joined by '|'. An
 * option given without a value renders its value as "-". */
static char *render_ordered(pmix_cli_result_t *res)
{
    pmix_cli_occurrence_t *occ = NULL;
    size_t nocc = 0, n;
    char **tmp = NULL;
    char *entry, *out;

    if (PMIX_SUCCESS != pmix_cmd_line_get_ordered(res, &occ, &nocc)) {
        return NULL;
    }
    for (n = 0; n < nocc; n++) {
        pmix_asprintf(&entry, "%s:%s", occ[n].key,
                      (NULL == occ[n].value) ? "-" : occ[n].value);
        PMIx_Argv_append_nosize(&tmp, entry);
        free(entry);
    }
    if (NULL != occ) {
        free(occ);
    }
    if (NULL == tmp) {
        return strdup("");
    }
    out = PMIx_Argv_join(tmp, '|');
    PMIx_Argv_free(tmp);
    return (NULL == out) ? strdup("") : out;
}

static void check_ordered(const char *desc, pmix_cli_result_t *res,
                          const char *expected)
{
    char *got;

    got = render_ordered(res);
    if (NULL == got) {
        fprintf(stdout, "  FAIL: %s [get_ordered failed]\n", desc);
        nfail++;
        return;
    }
    if (0 != strcmp(got, expected)) {
        fprintf(stdout, "  FAIL: %s\n        wanted \"%s\"\n        got    \"%s\"\n",
                desc, expected, got);
        nfail++;
    } else {
        fprintf(stdout, "  PASS: %s\n", desc);
        npass++;
    }
    free(got);
}

/* THE case from the issue: an option repeated after another has
 * intervened. The grouped view files the second --set-env behind the
 * first, so walking it in list order applies both sets and then the
 * prepend - arriving at FOO=x:2 where the user asked for FOO=2. */
static void test_order_interleaved(void)
{
    char *argv[] = {"prog", "--set-env", "FOO=1", "--prepend-env", "FOO[:]",
                    "x", "--set-env", "FOO=2", NULL};
    pmix_cli_result_t res;
    pmix_cli_item_t *opt;
    int rc;

    PMIX_CONSTRUCT(&res, pmix_cli_result_t);
    rc = pmix_cmd_line_parse(argv, "", ordopts, NULL, &res, NULL);
    report("order_interleaved: returns SUCCESS", PMIX_SUCCESS == rc);

    /* the grouped view is exactly what it always was - this record is
     * additional, not a replacement */
    opt = pmix_cmd_line_get_param(&res, PMIX_CLI_SET_ENVAR);
    report("order_interleaved: set-env still groups both values",
           NULL != opt && 2 == PMIx_Argv_count(opt->values)
               && 0 == strcmp(opt->values[0], "FOO=1")
               && 0 == strcmp(opt->values[1], "FOO=2"));
    opt = pmix_cmd_line_get_param(&res, PMIX_CLI_PREPEND_ENVAR);
    report("order_interleaved: prepend-env still holds its two tokens",
           NULL != opt && 2 == PMIx_Argv_count(opt->values)
               && 0 == strcmp(opt->values[0], "FOO[:]")
               && 0 == strcmp(opt->values[1], "x"));

    /* ...and the order is now recoverable */
    report("order_interleaved: first set-env precedes the prepend",
           pmix_cmd_line_get_nth_seq(&res, PMIX_CLI_SET_ENVAR, 0)
               < pmix_cmd_line_get_nth_seq(&res, PMIX_CLI_PREPEND_ENVAR, 0));
    report("order_interleaved: second set-env follows the prepend",
           pmix_cmd_line_get_nth_seq(&res, PMIX_CLI_SET_ENVAR, 1)
               > pmix_cmd_line_get_nth_seq(&res, PMIX_CLI_PREPEND_ENVAR, 1));

    check_ordered("order_interleaved: ordered view is command-line order",
                  &res,
                  "set-env:FOO=1|prepend-env:FOO[:]|prepend-env:x|set-env:FOO=2");
    PMIX_DESTRUCT(&res);
}

/* The mirror image: the same options the other way round must not come
 * back looking the same. */
static void test_order_reversed(void)
{
    char *argv[] = {"prog", "--prepend-env", "FOO[:]", "x",
                    "--set-env", "FOO=1", NULL};
    pmix_cli_result_t res;

    PMIX_CONSTRUCT(&res, pmix_cli_result_t);
    pmix_cmd_line_parse(argv, "", ordopts, NULL, &res, NULL);
    check_ordered("order_reversed: prepend first, then set", &res,
                  "prepend-env:FOO[:]|prepend-env:x|set-env:FOO=1");
    report("order_reversed: set-env follows the prepend",
           pmix_cmd_line_get_nth_seq(&res, PMIX_CLI_SET_ENVAR, 0)
               > pmix_cmd_line_get_nth_seq(&res, PMIX_CLI_PREPEND_ENVAR, 0));
    PMIX_DESTRUCT(&res);
}

/* An option that takes no value stores nothing, so it has no value to
 * stamp - its position is recorded on the option itself. */
static void test_order_valueless(void)
{
    char *argv[] = {"prog", "--timeout", "30", "--parsable",
                    "--timeout", "60", NULL};
    pmix_cli_result_t res;

    PMIX_CONSTRUCT(&res, pmix_cli_result_t);
    pmix_cmd_line_parse(argv, "", ordopts, NULL, &res, NULL);
    check_ordered("order_valueless: flag sits between the two timeouts",
                  &res, "timeout:30|parsable:-|timeout:60");
    report("order_valueless: flag follows the first timeout",
           pmix_cmd_line_get_first_seq(&res, PMIX_CLI_PARSABLE)
               > pmix_cmd_line_get_nth_seq(&res, PMIX_CLI_TIMEOUT, 0));
    report("order_valueless: flag precedes the second timeout",
           pmix_cmd_line_get_first_seq(&res, PMIX_CLI_PARSABLE)
               < pmix_cmd_line_get_nth_seq(&res, PMIX_CLI_TIMEOUT, 1));
    report("order_valueless: an option not given has no position",
           -1 == pmix_cmd_line_get_first_seq(&res, PMIX_CLI_SET_ENVAR));
    PMIX_DESTRUCT(&res);
}

/* A value a caller adds to the results itself was never on the command
 * line, so it has no position there. It must not be given one - it is
 * reported last, and says so. */
static void test_order_added_after_parse(void)
{
    char *argv[] = {"prog", "--set-env", "FOO=1", "--timeout", "30", NULL};
    pmix_cli_result_t res;
    pmix_cli_item_t *opt;

    PMIX_CONSTRUCT(&res, pmix_cli_result_t);
    pmix_cmd_line_parse(argv, "", ordopts, NULL, &res, NULL);

    /* PRRTE's schizo does exactly this when it converts a deprecated
     * option into a current one */
    opt = pmix_cmd_line_get_param(&res, PMIX_CLI_SET_ENVAR);
    PMIx_Argv_append_nosize(&opt->values, "BAR=2");

    report("order_added: added value has no recorded position",
           -1 == pmix_cmd_line_get_nth_seq(&res, PMIX_CLI_SET_ENVAR, 1));
    check_ordered("order_added: added value is reported last", &res,
                  "set-env:FOO=1|timeout:30|set-env:BAR=2");
    PMIX_DESTRUCT(&res);
}

static void test_order_empty(void)
{
    char *argv[] = {"prog", NULL};
    pmix_cli_occurrence_t *occ = (pmix_cli_occurrence_t *) 1;
    pmix_cli_result_t res;
    size_t nocc = 99;
    int rc;

    PMIX_CONSTRUCT(&res, pmix_cli_result_t);
    pmix_cmd_line_parse(argv, "", ordopts, NULL, &res, NULL);
    rc = pmix_cmd_line_get_ordered(&res, &occ, &nocc);
    report("order_empty: returns SUCCESS", PMIX_SUCCESS == rc);
    report("order_empty: reports nothing", 0 == nocc && NULL == occ);
    report("order_empty: rejects a NULL argument",
           PMIX_ERR_BAD_PARAM == pmix_cmd_line_get_ordered(&res, NULL, &nocc));
    PMIX_DESTRUCT(&res);
}

/* ------------------------------------------------------------------ */
/* pmix_check_cli_option (inline abbreviation matching)               */
/* ------------------------------------------------------------------ */

static void test_check_cli_option(void)
{
    /* Exact match */
    report("check_cli: exact match",
           pmix_check_cli_option("verbose", "verbose"));
    /* Prefix match (single word) */
    report("check_cli: prefix single-word",
           pmix_check_cli_option("verb", "verbose"));
    /* No match */
    report("check_cli: no match",
           !pmix_check_cli_option("xyz", "verbose"));
    /* Multi-word: all given segments match */
    report("check_cli: multi-word prefix",
           pmix_check_cli_option("sys-server", "system-server"));
    /* Multi-word: too many segments from input */
    report("check_cli: too many segments is false",
           !pmix_check_cli_option("sys-server-only-extra", "system-server-only"));
    /* Case-insensitive */
    report("check_cli: case-insensitive",
           pmix_check_cli_option("VERB", "verbose"));
}

/* ------------------------------------------------------------------ */
/* pmix_convert_string_to_time                                        */
/* ------------------------------------------------------------------ */

static void test_convert_time(void)
{
    /* Seconds only */
    report("convert_time: \"30\" → 30",
           30 == pmix_convert_string_to_time("30"));
    /* mm:ss */
    report("convert_time: \"1:30\" → 90",
           90 == pmix_convert_string_to_time("1:30"));
    /* hh:mm:ss */
    report("convert_time: \"1:2:30\" → 3750",
           3750 == pmix_convert_string_to_time("1:2:30"));
    /* dd:hh:mm:ss */
    report("convert_time: \"0:1:0:0\" → 3600",
           3600 == pmix_convert_string_to_time("0:1:0:0"));
    /* zero */
    report("convert_time: \"0\" → 0",
           0 == pmix_convert_string_to_time("0"));
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    pmix_proc_t myproc;
    pmix_status_t rc;
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    rc = PMIx_Init(&myproc, NULL, 0);
    if (PMIX_SUCCESS != rc && PMIX_ERR_UNREACH != rc) {
        fprintf(stderr, "PMIx_Init: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    fprintf(stdout, "\n=== pmix_cmd_line unit tests ===\n\n");

    test_parse_empty();
    test_parse_flag();
    test_parse_required_arg();
    test_parse_multiple_options();
    test_parse_not_taken();

    fprintf(stdout, "\n--- command-line table ---\n");
    run_table();
    fprintf(stdout, "\n");

    test_order_interleaved();
    test_order_reversed();
    test_order_valueless();
    test_order_added_after_parse();
    test_order_empty();

    test_check_cli_option();
    test_convert_time();

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);

    PMIx_Finalize(NULL, 0);
    return (nfail > 0) ? 1 : 0;
}
