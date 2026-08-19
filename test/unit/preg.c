/*
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit tests for the preg framework: PMIx_generate_regex2 /
 * PMIx_parse_regex2, and the deprecated PMIx_generate_regex /
 * PMIx_generate_ppn that the framework base synthesizes from them.
 *
 * Each regex2 test calls PMIx_generate_regex2 on a known input, checks
 * that the returned pmix_regex2_t is non-empty, then round-trips through
 * PMIx_parse_regex2 and verifies the decoded string matches the original.
 * The deprecated-form tests round-trip through the char* serialization in
 * preg_base_legacy.c, including the copy and pack/unpack paths that carry
 * it between peers.
 *
 * A simple PASS/FAIL summary is printed for each case.  The program exits
 * with 0 if all tests pass, 1 otherwise.
 */

#include "src/include/pmix_config.h"
#include "include/pmix_server.h"
#include "src/include/pmix_globals.h"
#include "src/mca/bfrops/bfrops.h"
#include "src/mca/preg/base/base.h"
#include "src/mca/preg/preg.h"
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
static int nskip = 0;

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

#if PMIX_TESTBUILD
/* Note a test case that cannot run in this configuration. Only needed in a
 * --enable-test-build, where the compression-dependent case is skipped. */
static void skipped(const char *name, const char *reason)
{
    fprintf(stdout, "  SKIP: %s (%s)\n", name, reason);
    nskip++;
}
#endif

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

#if PMIX_TESTBUILD
    /* A large, highly-compressible list is encoded via the pcompress
     * "blob" path, but the pcompress components are non-functional shims
     * in a --enable-test-build (see the top-level AGENTS.md): deflate is a
     * no-op, so the round-trip cannot reproduce the input. Skip rather
     * than report a spurious failure. */
    skipped("large list (10000 nodes)", "compression stubbed in --enable-test-build");
#else
    report("large list (10000 nodes)", roundtrip(input));
#endif
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

/* ---- the deprecated char* form -------------------------------------- */

/* Round-trip a list through the deprecated generate/parse pair. The
 * delimiter picks which of the two the caller would use: ',' is
 * PMIx_generate_regex + parse_nodes, ';' is PMIx_generate_ppn +
 * parse_procs. Both are the same encoding underneath, which is the point
 * of the test. */
static int legacy_roundtrip(const char *input, int delimiter)
{
    char *regex = NULL, *rebuilt = NULL;
    char **split = NULL;
    pmix_status_t rc;
    int ok = 0;

    if (',' == delimiter) {
        rc = PMIx_generate_regex(input, &regex);
    } else {
        rc = PMIx_generate_ppn(input, &regex);
    }
    if (PMIX_SUCCESS != rc || NULL == regex) {
        fprintf(stdout, "    generate failed: %s\n", PMIx_Error_string(rc));
        return 0;
    }

    if (',' == delimiter) {
        rc = pmix_preg.parse_nodes(regex, &split);
    } else {
        rc = pmix_preg.parse_procs(regex, &split);
    }
    if (PMIX_SUCCESS != rc || NULL == split) {
        fprintf(stdout, "    parse failed: %s\n", PMIx_Error_string(rc));
        free(regex);
        return 0;
    }

    rebuilt = PMIx_Argv_join(split, delimiter);
    ok = (NULL != rebuilt && 0 == strcmp(rebuilt, input));
    if (!ok) {
        fprintf(stdout, "    expected \"%s\", got \"%s\"\n", input,
                (NULL == rebuilt) ? "(null)" : rebuilt);
    }

    PMIx_Argv_free(split);
    free(rebuilt);
    free(regex);
    return ok;
}

static void test_legacy_nodes(void)
{
    report("deprecated node map round-trip",
           legacy_roundtrip("node01,node02,node03,node17", ','));
}

static void test_legacy_procs(void)
{
    /* the process map: nodes delimited by ';', ranks on each by ',' */
    report("deprecated process map round-trip",
           legacy_roundtrip("0,1,2;3,4,5;6", ';'));
}

/* A plain, unencoded list carries no framing at all - a host is entitled
 * to hand one straight to PMIX_NODE_MAP, and the parse side has to split
 * it rather than reject it. */
static void test_legacy_untagged(void)
{
    char **nodes = NULL;
    pmix_status_t rc;
    int ok = 0;

    rc = pmix_preg.parse_nodes("nodeA,nodeB,nodeC", &nodes);
    if (PMIX_SUCCESS == rc && NULL != nodes) {
        ok = (3 == PMIx_Argv_count(nodes) && 0 == strcmp(nodes[1], "nodeB"));
        PMIx_Argv_free(nodes);
    }
    report("untagged list is split, not rejected", ok);
}

/* copy() has to reproduce the whole serialized form, which for a
 * compressed encoding contains embedded NULs - a strdup would truncate
 * it. Verify by parsing the copy, not by comparing pointers. */
static void test_legacy_copy(void)
{
    char *regex = NULL, *dup = NULL, *rebuilt = NULL;
    char **nodes = NULL;
    const char *input = "node01,node02,node03";
    size_t len = 0;
    pmix_status_t rc;
    int ok = 0;

    rc = PMIx_generate_regex(input, &regex);
    if (PMIX_SUCCESS != rc) {
        report("deprecated form survives copy", 0);
        return;
    }

    rc = pmix_preg.copy(&dup, &len, regex);
    if (PMIX_SUCCESS == rc && NULL != dup && 0 < len &&
        PMIX_SUCCESS == pmix_preg.parse_nodes(dup, &nodes)) {
        rebuilt = PMIx_Argv_join(nodes, ',');
        ok = (NULL != rebuilt && 0 == strcmp(rebuilt, input));
        PMIx_Argv_free(nodes);
        free(rebuilt);
    }
    report("deprecated form survives copy", ok);

    free(dup);
    free(regex);
}

/* pack/unpack carry the serialized form between peers verbatim - it
 * supplies its own length, so it gets no bfrops framing. */
static void test_legacy_pack_unpack(void)
{
    pmix_buffer_t buf;
    char *regex = NULL, *unpacked = NULL, *rebuilt = NULL;
    char **nodes = NULL;
    const char *input = "node01,node02,node03";
    pmix_status_t rc;
    int ok = 0;

    rc = PMIx_generate_regex(input, &regex);
    if (PMIX_SUCCESS != rc) {
        report("deprecated form survives pack/unpack", 0);
        return;
    }

    PMIX_CONSTRUCT(&buf, pmix_buffer_t);
    rc = pmix_preg.pack(&buf, regex);
    if (PMIX_SUCCESS == rc &&
        PMIX_SUCCESS == pmix_preg.unpack(&buf, &unpacked) &&
        NULL != unpacked &&
        PMIX_SUCCESS == pmix_preg.parse_nodes(unpacked, &nodes)) {
        rebuilt = PMIx_Argv_join(nodes, ',');
        ok = (NULL != rebuilt && 0 == strcmp(rebuilt, input));
        PMIx_Argv_free(nodes);
        free(rebuilt);
    }
    report("deprecated form survives pack/unpack", ok);

    free(unpacked);
    free(regex);
    PMIX_DESTRUCT(&buf);
}

/* Truncated wire data must be declined, not walked off the end of the
 * buffer. Only the decoder's bounded mode can defend against this - a
 * deprecated char* carries no length, so the local-string callers can
 * only trust that what they hold is well-formed. Exercise the mode that
 * does have a bound, which is the one that reads from a peer. */
static void test_legacy_truncated(void)
{
    /* a blob header cut off at every interesting point */
    static const char t1[] = "blob:";
    static const char t2[] = "blob:\0component=zli";
    static const char t3[] = "blob:\0component=zlib:\0size=";
    static const char t4[] = "blob:\0component=zlib:\0size=64:\0abc";
    static const struct {
        const char *bytes;
        size_t len;
    } cases[] = {
        {t1, sizeof(t1)},   // nothing after the tag
        {t2, sizeof(t2)},   // label cut in half
        {t3, sizeof(t3)},   // no digits where the length belongs
        {t4, sizeof(t4)},   // length claims more payload than is present
    };
    pmix_regex2_t r2 = PMIX_REGEX2_STATIC_INIT;
    size_t total, n;
    int ok = 1;

    for (n = 0; n < sizeof(cases) / sizeof(cases[0]); n++) {
        if (PMIX_SUCCESS == pmix_preg_base_legacy_decode(cases[n].bytes, cases[n].len,
                                                         &r2, &total)) {
            fprintf(stdout, "    case %lu was accepted\n", (unsigned long) n);
            ok = 0;
        }
    }
    report("truncated blob is declined, not read past", ok);
}

/* Framing that is not truncated but is wrong. Every one of these arrives
 * as bytes from a peer, so "declined" is the only acceptable answer -
 * an accepted value hands its declared length straight to a decompressor.
 * The SIZE_MAX case is the one that matters most: the declared length is
 * added to the offset of the payload, so a length near SIZE_MAX wraps the
 * sum back to a small number and sails through a bounds check written as
 * an addition. */
static void test_legacy_malformed(void)
{
    static const char m1[] = "blob:\0component=zlib:\0size=18446744073709551615:\0abc";
    static const char m2[] = "blob:\0component=zlib:\0size=3xyz\0abc";
    static const char m3[] = "blob:\0component=zlib:\0size=3\0abc";
    static const struct {
        const char *bytes;
        size_t len;
    } cases[] = {
        {m1, sizeof(m1) - 1},  // length wraps the bounds arithmetic
        {m2, sizeof(m2) - 1},  // digits not followed by the colon
        {m3, sizeof(m3) - 1},  // colon missing entirely
    };
    pmix_regex2_t r2 = PMIX_REGEX2_STATIC_INIT;
    size_t total, n;
    int ok = 1;

    for (n = 0; n < sizeof(cases) / sizeof(cases[0]); n++) {
        r2 = (pmix_regex2_t) PMIX_REGEX2_STATIC_INIT;
        total = 0;
        if (PMIX_SUCCESS == pmix_preg_base_legacy_decode(cases[n].bytes, cases[n].len,
                                                         &r2, &total)) {
            fprintf(stdout, "    case %lu was accepted, len=%lu total=%lu\n",
                    (unsigned long) n, (unsigned long) r2.len, (unsigned long) total);
            ok = 0;
        }
    }
    report("malformed blob framing is declined", ok);
}

/* A plain list whose first node happens to begin with "blob" is not a
 * blob. The tag test has to be exact for that to be true: a prefix test
 * accepts it, and everything after the tag is then read looking for a
 * label that is not there - past the end of a string the caller owns and
 * whose length this entry point has no way to learn. Run this one under
 * valgrind; without it, the test only proves the value was declined. */
static void test_legacy_blob_prefixed_list(void)
{
    char *list = strdup("blobfish,node02");
    pmix_regex2_t r2 = PMIX_REGEX2_STATIC_INIT;
    size_t total = 0;
    char **names = NULL;
    int ok;

    ok = (PMIX_SUCCESS != pmix_preg_base_legacy_decode(list, SIZE_MAX, &r2, &total));

    /* and the whole point of declining it: the list still splits */
    if (PMIX_SUCCESS == pmix_preg.parse_nodes(list, &names)) {
        ok = ok && (NULL != names && NULL != names[0] &&
                    0 == strcmp(names[0], "blobfish"));
        PMIx_Argv_free(names);
    } else {
        ok = 0;
    }
    free(list);
    report("a plain list beginning with \"blob\" is not a blob", ok);
}

/* The bounded decoder must still accept a well-formed value that ends
 * exactly at the end of the buffer - the bounds checks are there to
 * reject short reads, not to demand slack. */
static void test_legacy_decode_exact(void)
{
    static const char blob[] = "blob:\0component=zlib:\0size=3:\0xyz";
    pmix_regex2_t r2 = PMIX_REGEX2_STATIC_INIT;
    size_t total = 0;
    int ok = 0;

    /* sizeof() includes the trailing NUL the compiler adds, which is not
     * part of the encoding - hand over exactly the encoded bytes */
    if (PMIX_SUCCESS == pmix_preg_base_legacy_decode(blob, sizeof(blob) - 1, &r2, &total)) {
        ok = (3 == r2.len && sizeof(blob) - 1 == total &&
              0 == memcmp(r2.bytes, "xyz", 3) && 0 == strcmp(r2.type, "compress"));
    }
    report("blob ending flush with the buffer is accepted", ok);
}

/* A large, highly-compressible list takes the compressed encoding, and so
 * exercises the blob serialization rather than the raw one - which is the
 * half of preg_base_legacy.c with real framing in it. Say out loud which
 * encoding was actually used, so that a build without a compression
 * library reports thin coverage instead of quietly passing. */
static void test_legacy_large(void)
{
    char **nodes = NULL;
    char *input, *regex = NULL;
    int n;

    for (n = 0; n < 5000; n++) {
        char tmp[32];
        snprintf(tmp, sizeof(tmp), "node%05d", n);
        PMIx_Argv_append_nosize(&nodes, tmp);
    }
    input = PMIx_Argv_join(nodes, ',');
    PMIx_Argv_free(nodes);

#if PMIX_TESTBUILD
    /* see test_large_list() - the pcompress shims make this unreachable */
    skipped("deprecated form, large list (5000 nodes)",
            "compression stubbed in --enable-test-build");
#else
    if (PMIX_SUCCESS == PMIx_generate_regex(input, &regex) && NULL != regex) {
        if (0 == strncmp(regex, "blob", 4)) {
            fprintf(stdout, "    (encoded as a compressed blob)\n");
        } else {
            fprintf(stdout, "    NOTE: encoded as \"%.4s\" - no compression"
                            " library, so the blob framing is untested here\n", regex);
        }
        free(regex);
    }
    report("deprecated form, large list (5000 nodes)",
           legacy_roundtrip(input, ','));
#endif
    free(input);
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

    /* The deprecated char* form, which the base synthesizes from the
     * regex2 operations above */
    test_legacy_nodes();
    test_legacy_procs();
    test_legacy_untagged();
    test_legacy_copy();
    test_legacy_pack_unpack();
    test_legacy_truncated();
    test_legacy_malformed();
    test_legacy_blob_prefixed_list();
    test_legacy_decode_exact();
    test_legacy_large();

    fprintf(stdout, "\nResults: %d passed, %d failed, %d skipped\n\n", npass, nfail, nskip);

    PMIx_server_finalize();

    return (nfail > 0) ? 1 : 0;
}
