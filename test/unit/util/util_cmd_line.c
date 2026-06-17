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
 * PMIx_Init is called first; PMIX_ERR_UNREACH is treated as normal.
 *
 * Exit 0 if all tests pass, 1 otherwise.
 */

#include "src/include/pmix_config.h"
#include "src/include/pmix_globals.h"

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pmix.h"
#include "src/util/pmix_cmd_line.h"

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

/* Options used throughout the parse tests. */
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
    test_check_cli_option();
    test_convert_time();

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);

    PMIx_Finalize(NULL, 0);
    return (nfail > 0) ? 1 : 0;
}
