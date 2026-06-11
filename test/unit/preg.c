/*
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit tests for PMIx_generate_regex2 and PMIx_parse_regex2.
 *
 * Each test calls PMIx_generate_regex2 on a known input, checks that the
 * returned pmix_regex2_t is non-empty, then round-trips through
 * PMIx_parse_regex2 and verifies the decoded string matches the original.
 *
 * A simple PASS/FAIL summary is printed for each case.  The program exits
 * with 0 if all tests pass, 1 otherwise.
 */

#include "src/include/pmix_config.h"
#include "include/pmix_server.h"
#include "src/include/pmix_globals.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_printf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Minimal server module - only init/finalize are exercised */
static pmix_server_module_t mymodule = {
    .client_connected = NULL,
    .client_finalized = NULL,
    .abort = NULL,
    .fence_nb = NULL,
    .direct_modex = NULL,
    .publish = NULL,
    .lookup = NULL,
    .unpublish = NULL,
    .spawn = NULL,
    .connect = NULL,
    .disconnect = NULL,
    .register_events = NULL,
    .deregister_events = NULL,
    .notify_event = NULL,
    .query = NULL,
    .tool_connected = NULL,
    .log = NULL,
    .allocate = NULL,
    .job_control = NULL,
    .monitor = NULL,
    .group = NULL
};

static int npass = 0;
static int nfail = 0;

/* Report a single test result and update counters */
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

/*
 * Round-trip helper: generate a regex from input, then parse it back and
 * compare the decoded string to input.  Returns 1 on success, 0 on failure.
 */
static int roundtrip(const char *input)
{
    pmix_regex2_t regex = PMIX_REGEX2_STATIC_INIT;
    char *decoded = NULL;
    pmix_status_t rc;
    int ok = 0;

    rc = PMIx_generate_regex2(input, NULL, 0, &regex);
    if (PMIX_SUCCESS != rc) {
        fprintf(stdout, "    generate_regex2 failed: %s\n", PMIx_Error_string(rc));
        goto out;
    }
    if (NULL == regex.type || NULL == regex.bytes || 0 == regex.len) {
        fprintf(stdout, "    generate_regex2 returned empty pmix_regex2_t\n");
        goto out;
    }

    rc = PMIx_parse_regex2(&regex, NULL, 0, &decoded);
    if (PMIX_SUCCESS != rc) {
        fprintf(stdout, "    parse_regex2 failed: %s\n", PMIx_Error_string(rc));
        goto out;
    }
    if (NULL == decoded) {
        fprintf(stdout, "    parse_regex2 returned NULL output\n");
        goto out;
    }

    if (0 == strcmp(decoded, input)) {
        ok = 1;
    } else {
        fprintf(stdout, "    mismatch:\n      expected: %s\n      got:      %s\n",
                input, decoded);
    }

out:
    if (NULL != regex.type)  free(regex.type);
    if (NULL != regex.bytes) free(regex.bytes);
    if (NULL != decoded)     free(decoded);
    return ok;
}

/* ------------------------------------------------------------------ */
/* Individual test cases                                               */
/* ------------------------------------------------------------------ */

/* Single node, no numeric suffix */
static void test_single_node(void)
{
    report("single node", roundtrip("node0"));
}

/* Small homogeneous cluster - nodes share a prefix and contiguous range */
static void test_contiguous_range(void)
{
    char *input = NULL;
    char **nodes = NULL;
    int n;

    for (n = 0; n < 8; n++) {
        char tmp[32];
        snprintf(tmp, sizeof(tmp), "node%04d", n);
        PMIx_Argv_append_nosize(&nodes, tmp);
    }
    input = PMIx_Argv_join(nodes, ',');
    PMIx_Argv_free(nodes);

    report("contiguous range (node0000-node0007)", roundtrip(input));
    free(input);
}

/* Non-contiguous node numbers */
static void test_non_contiguous(void)
{
    report("non-contiguous (node001,node003,node007)",
           roundtrip("node001,node003,node007"));
}

/* Mixed prefixes in a single list */
static void test_mixed_prefixes(void)
{
    report("mixed prefixes (odin,thor)",
           roundtrip("odin001,odin002,odin003,thor010,thor011"));
}

/* Nodes with no numeric component at all */
static void test_no_digits(void)
{
    report("no-digit names (login,head,storage)",
           roundtrip("login,head,storage"));
}

/* Large list - exercises compression path when zlib is available */
static void test_large_list(void)
{
    char **nodes = NULL;
    char *input = NULL;
    int n;

    for (n = 0; n < 10000; n++) {
        char tmp[32];
        snprintf(tmp, sizeof(tmp), "node%05d", n);
        PMIx_Argv_append_nosize(&nodes, tmp);
    }
    input = PMIx_Argv_join(nodes, ',');
    PMIx_Argv_free(nodes);

    report("large list (10000 nodes)", roundtrip(input));
    free(input);
}

/* Nodes with a numeric suffix after the digits (e.g. HPC blade naming) */
static void test_suffix(void)
{
    report("nodes with suffix (node01n,node02n,node03n)",
           roundtrip("node01n,node02n,node03n"));
}

/* Verify generate_regex2 rejects a NULL regex pointer */
static void test_null_regex_param(void)
{
    pmix_status_t rc = PMIx_generate_regex2("node0", NULL, 0, NULL);
    report("NULL regex param returns PMIX_ERR_BAD_PARAM",
           PMIX_ERR_BAD_PARAM == rc);
}

/* Verify parse_regex2 rejects a NULL regex pointer */
static void test_null_parse_input(void)
{
    char *out = NULL;
    pmix_status_t rc = PMIx_parse_regex2(NULL, NULL, 0, &out);
    report("NULL regex input returns PMIX_ERR_BAD_PARAM",
           PMIX_ERR_BAD_PARAM == rc);
}

/* Verify parse_regex2 rejects a NULL output pointer */
static void test_null_parse_output(void)
{
    pmix_regex2_t regex = PMIX_REGEX2_STATIC_INIT;
    pmix_status_t rc;

    rc = PMIx_generate_regex2("node0", NULL, 0, &regex);
    if (PMIX_SUCCESS != rc) {
        report("NULL parse output (setup failed)", 0);
        return;
    }

    rc = PMIx_parse_regex2(&regex, NULL, 0, NULL);
    report("NULL parse output param returns PMIX_ERR_BAD_PARAM",
           PMIX_ERR_BAD_PARAM == rc);

    if (NULL != regex.type)  free(regex.type);
    if (NULL != regex.bytes) free(regex.bytes);
}

/* Verify that the type field is set to a non-empty string */
static void test_type_field_set(void)
{
    pmix_regex2_t regex = PMIX_REGEX2_STATIC_INIT;
    pmix_status_t rc;
    int ok = 0;

    rc = PMIx_generate_regex2("node0001,node0002,node0003", NULL, 0, &regex);
    if (PMIX_SUCCESS == rc && NULL != regex.type && '\0' != regex.type[0]) {
        ok = 1;
    }
    report("type field is non-empty after generate_regex2", ok);

    if (NULL != regex.type)  free(regex.type);
    if (NULL != regex.bytes) free(regex.bytes);
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    pmix_status_t rc;
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    rc = PMIx_server_init(&mymodule, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    fprintf(stdout, "\n=== PMIx_generate_regex2 / PMIx_parse_regex2 unit tests ===\n\n");

    /* Parameter validation */
    test_null_regex_param();
    test_null_parse_input();
    test_null_parse_output();

    /* Functional round-trip */
    test_single_node();
    test_contiguous_range();
    test_non_contiguous();
    test_mixed_prefixes();
    test_no_digits();
    test_suffix();
    test_large_list();

    /* Structural checks */
    test_type_field_set();

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);

    PMIx_server_finalize();

    return (nfail > 0) ? 1 : 0;
}
