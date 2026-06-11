/*
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit tests for PMIX_REGEX2 bfrops support: pack/unpack, copy, print,
 * and compare operations exercised through the public PMIx API.
 */

#include "src/include/pmix_config.h"
#include "include/pmix_server.h"
#include "src/include/pmix_globals.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_printf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* Build a pmix_regex2_t from a comma-separated node list. */
static int make_regex(const char *nodes, pmix_regex2_t *rx)
{
    pmix_status_t rc = PMIx_generate_regex2(nodes, NULL, 0, rx);
    return (PMIX_SUCCESS == rc && NULL != rx->type &&
            NULL != rx->bytes && 0 != rx->len);
}

/* ------------------------------------------------------------------ */
/* pack / unpack                                                       */
/* ------------------------------------------------------------------ */

static void test_pack_unpack_roundtrip(void)
{
    pmix_regex2_t src = PMIX_REGEX2_STATIC_INIT;
    pmix_regex2_t dst = PMIX_REGEX2_STATIC_INIT;
    pmix_data_buffer_t buf;
    pmix_status_t rc;
    int32_t count;
    int ok = 0;

    if (!make_regex("node001,node002,node003", &src)) {
        report("pack/unpack round-trip (setup)", 0);
        return;
    }

    PMIx_Data_buffer_construct(&buf);

    rc = PMIx_Data_pack(NULL, &buf, &src, 1, PMIX_REGEX2);
    if (PMIX_SUCCESS != rc) {
        fprintf(stdout, "    pack failed: %s\n", PMIx_Error_string(rc));
        goto cleanup;
    }

    count = 1;
    rc = PMIx_Data_unpack(NULL, &buf, &dst, &count, PMIX_REGEX2);
    if (PMIX_SUCCESS != rc) {
        fprintf(stdout, "    unpack failed: %s\n", PMIx_Error_string(rc));
        goto cleanup;
    }

    if (src.len == dst.len &&
        NULL != dst.type  && 0 == strcmp(src.type, dst.type) &&
        0 == memcmp(src.bytes, dst.bytes, src.len)) {
        ok = 1;
    } else {
        fprintf(stdout, "    content mismatch after unpack\n");
    }

cleanup:
    PMIx_Data_buffer_destruct(&buf);
    PMIx_Regex2_destruct(&src);
    PMIx_Regex2_destruct(&dst);
    report("pack/unpack round-trip", ok);
}

/* Pack multiple regex2 values into one buffer and unpack them all. */
static void test_pack_unpack_multiple(void)
{
    const int N = 3;
    const char *inputs[3] = { "node0001", "gpu001,gpu002", "io001,io002,io003,io004" };
    pmix_regex2_t src[3];
    pmix_regex2_t dst[3];
    pmix_data_buffer_t buf;
    pmix_status_t rc;
    int32_t count;
    int i, ok = 1;

    for (i = 0; i < N; i++) {
        pmix_regex2_t tmp_s = PMIX_REGEX2_STATIC_INIT;
        pmix_regex2_t tmp_d = PMIX_REGEX2_STATIC_INIT;
        src[i] = tmp_s;
        dst[i] = tmp_d;
    }

    PMIx_Data_buffer_construct(&buf);

    for (i = 0; i < N; i++) {
        if (!make_regex(inputs[i], &src[i])) {
            report("pack/unpack multiple (setup)", 0);
            goto cleanup;
        }
        rc = PMIx_Data_pack(NULL, &buf, &src[i], 1, PMIX_REGEX2);
        if (PMIX_SUCCESS != rc) {
            fprintf(stdout, "    pack[%d] failed: %s\n", i, PMIx_Error_string(rc));
            report("pack/unpack multiple", 0);
            goto cleanup;
        }
    }

    for (i = 0; i < N; i++) {
        count = 1;
        rc = PMIx_Data_unpack(NULL, &buf, &dst[i], &count, PMIX_REGEX2);
        if (PMIX_SUCCESS != rc) {
            fprintf(stdout, "    unpack[%d] failed: %s\n", i, PMIx_Error_string(rc));
            ok = 0;
            break;
        }
        if (src[i].len != dst[i].len ||
            NULL == dst[i].type ||
            0 != strcmp(src[i].type, dst[i].type) ||
            0 != memcmp(src[i].bytes, dst[i].bytes, src[i].len)) {
            fprintf(stdout, "    content mismatch at index %d\n", i);
            ok = 0;
            break;
        }
    }

cleanup:
    PMIx_Data_buffer_destruct(&buf);
    for (i = 0; i < N; i++) {
        PMIx_Regex2_destruct(&src[i]);
        PMIx_Regex2_destruct(&dst[i]);
    }
    report("pack/unpack multiple values", ok);
}

/* Unpack into a pmix_value_t to exercise the PMIX_REGEX2 value path. */
static void test_pack_unpack_via_value(void)
{
    pmix_regex2_t src = PMIX_REGEX2_STATIC_INIT;
    pmix_value_t val;
    pmix_data_buffer_t buf;
    pmix_status_t rc;
    int32_t count;
    int ok = 0;

    PMIX_VALUE_CONSTRUCT(&val);

    if (!make_regex("node100,node101,node102", &src)) {
        report("pack/unpack via pmix_value_t (setup)", 0);
        return;
    }

    PMIx_Data_buffer_construct(&buf);

    /* Load the regex2 into a value and pack the value. */
    val.type = PMIX_REGEX2;
    val.data.regex2 = PMIx_Regex2_create(1);
    if (NULL == val.data.regex2) {
        report("pack/unpack via pmix_value_t (alloc)", 0);
        goto cleanup;
    }
    val.data.regex2->type  = strdup(src.type);
    val.data.regex2->len   = src.len;
    val.data.regex2->bytes = malloc(src.len);
    if (NULL == val.data.regex2->bytes) {
        report("pack/unpack via pmix_value_t (alloc bytes)", 0);
        goto cleanup;
    }
    memcpy(val.data.regex2->bytes, src.bytes, src.len);

    rc = PMIx_Data_pack(NULL, &buf, &val, 1, PMIX_VALUE);
    if (PMIX_SUCCESS != rc) {
        fprintf(stdout, "    pack failed: %s\n", PMIx_Error_string(rc));
        goto cleanup;
    }

    PMIX_VALUE_DESTRUCT(&val);
    PMIX_VALUE_CONSTRUCT(&val);

    count = 1;
    rc = PMIx_Data_unpack(NULL, &buf, &val, &count, PMIX_VALUE);
    if (PMIX_SUCCESS != rc) {
        fprintf(stdout, "    unpack failed: %s\n", PMIx_Error_string(rc));
        goto cleanup;
    }

    if (PMIX_REGEX2 == val.type &&
        NULL != val.data.regex2 &&
        src.len == val.data.regex2->len &&
        0 == memcmp(src.bytes, val.data.regex2->bytes, src.len)) {
        ok = 1;
    } else {
        fprintf(stdout, "    value mismatch after unpack\n");
    }

cleanup:
    PMIx_Data_buffer_destruct(&buf);
    PMIX_VALUE_DESTRUCT(&val);
    PMIx_Regex2_destruct(&src);
    report("pack/unpack via pmix_value_t", ok);
}

/* ------------------------------------------------------------------ */
/* copy                                                                */
/* ------------------------------------------------------------------ */

static void test_value_xfer(void)
{
    pmix_regex2_t src = PMIX_REGEX2_STATIC_INIT;
    pmix_value_t vsrc, vdst;
    int ok = 0;

    PMIX_VALUE_CONSTRUCT(&vsrc);
    PMIX_VALUE_CONSTRUCT(&vdst);

    if (!make_regex("node200,node201,node202,node203", &src)) {
        report("PMIx_Value_xfer PMIX_REGEX2 (setup)", 0);
        return;
    }

    vsrc.type = PMIX_REGEX2;
    vsrc.data.regex2 = PMIx_Regex2_create(1);
    if (NULL == vsrc.data.regex2) {
        report("PMIx_Value_xfer PMIX_REGEX2 (alloc)", 0);
        PMIx_Regex2_destruct(&src);
        return;
    }
    vsrc.data.regex2->type  = strdup(src.type);
    vsrc.data.regex2->len   = src.len;
    vsrc.data.regex2->bytes = malloc(src.len);
    if (NULL != vsrc.data.regex2->bytes)
        memcpy(vsrc.data.regex2->bytes, src.bytes, src.len);

    pmix_status_t rc = PMIx_Value_xfer(&vdst, &vsrc);
    if (PMIX_SUCCESS != rc) {
        fprintf(stdout, "    PMIx_Value_xfer failed: %s\n", PMIx_Error_string(rc));
        goto cleanup;
    }

    if (PMIX_REGEX2 == vdst.type &&
        NULL != vdst.data.regex2 &&
        vsrc.data.regex2->len == vdst.data.regex2->len &&
        NULL != vdst.data.regex2->type &&
        0 == strcmp(vsrc.data.regex2->type, vdst.data.regex2->type) &&
        0 == memcmp(vsrc.data.regex2->bytes, vdst.data.regex2->bytes,
                    vsrc.data.regex2->len)) {
        ok = 1;
    } else {
        fprintf(stdout, "    copied value does not match source\n");
    }

    /* Verify deep copy - mutating src bytes doesn't affect dst. */
    if (ok && vsrc.data.regex2->len > 0) {
        uint8_t *b = (uint8_t *)vsrc.data.regex2->bytes;
        uint8_t saved = b[0];
        b[0] ^= 0xFF;
        if (0 == memcmp(vsrc.data.regex2->bytes, vdst.data.regex2->bytes,
                        vsrc.data.regex2->len)) {
            fprintf(stdout, "    copy shares bytes pointer (shallow copy)\n");
            ok = 0;
        }
        b[0] = saved;
    }

cleanup:
    PMIX_VALUE_DESTRUCT(&vsrc);
    PMIX_VALUE_DESTRUCT(&vdst);
    PMIx_Regex2_destruct(&src);
    report("PMIx_Value_xfer deep-copies PMIX_REGEX2", ok);
}

/* ------------------------------------------------------------------ */
/* compare                                                             */
/* ------------------------------------------------------------------ */

static void test_compare_equal(void)
{
    pmix_regex2_t r1 = PMIX_REGEX2_STATIC_INIT;
    pmix_regex2_t r2 = PMIX_REGEX2_STATIC_INIT;
    pmix_value_t v1, v2;
    pmix_value_cmp_t cmp;
    int ok = 0;

    PMIX_VALUE_CONSTRUCT(&v1);
    PMIX_VALUE_CONSTRUCT(&v2);

    if (!make_regex("node300,node301", &r1) ||
        !make_regex("node300,node301", &r2)) {
        report("compare equal PMIX_REGEX2 (setup)", 0);
        goto cleanup;
    }

    v1.type = PMIX_REGEX2;
    v1.data.regex2 = PMIx_Regex2_create(1);
    if (NULL == v1.data.regex2) goto cleanup;
    v1.data.regex2->type  = strdup(r1.type);
    v1.data.regex2->len   = r1.len;
    v1.data.regex2->bytes = malloc(r1.len);
    if (NULL != v1.data.regex2->bytes)
        memcpy(v1.data.regex2->bytes, r1.bytes, r1.len);

    v2.type = PMIX_REGEX2;
    v2.data.regex2 = PMIx_Regex2_create(1);
    if (NULL == v2.data.regex2) goto cleanup;
    v2.data.regex2->type  = strdup(r2.type);
    v2.data.regex2->len   = r2.len;
    v2.data.regex2->bytes = malloc(r2.len);
    if (NULL != v2.data.regex2->bytes)
        memcpy(v2.data.regex2->bytes, r2.bytes, r2.len);

    cmp = PMIx_Value_compare(&v1, &v2);
    ok = (PMIX_EQUAL == cmp);
    if (!ok)
        fprintf(stdout, "    expected PMIX_EQUAL, got %d\n", (int)cmp);

cleanup:
    PMIX_VALUE_DESTRUCT(&v1);
    PMIX_VALUE_DESTRUCT(&v2);
    PMIx_Regex2_destruct(&r1);
    PMIx_Regex2_destruct(&r2);
    report("compare: identical PMIX_REGEX2 values are PMIX_EQUAL", ok);
}

static void test_compare_different(void)
{
    pmix_regex2_t r1 = PMIX_REGEX2_STATIC_INIT;
    pmix_regex2_t r2 = PMIX_REGEX2_STATIC_INIT;
    pmix_value_t v1, v2;
    pmix_value_cmp_t cmp;
    int ok = 0;

    PMIX_VALUE_CONSTRUCT(&v1);
    PMIX_VALUE_CONSTRUCT(&v2);

    if (!make_regex("node400", &r1) ||
        !make_regex("node400,node401,node402", &r2)) {
        report("compare different PMIX_REGEX2 (setup)", 0);
        goto cleanup;
    }

    v1.type = PMIX_REGEX2;
    v1.data.regex2 = PMIx_Regex2_create(1);
    if (NULL == v1.data.regex2) goto cleanup;
    v1.data.regex2->type  = strdup(r1.type);
    v1.data.regex2->len   = r1.len;
    v1.data.regex2->bytes = malloc(r1.len);
    if (NULL != v1.data.regex2->bytes)
        memcpy(v1.data.regex2->bytes, r1.bytes, r1.len);

    v2.type = PMIX_REGEX2;
    v2.data.regex2 = PMIx_Regex2_create(1);
    if (NULL == v2.data.regex2) goto cleanup;
    v2.data.regex2->type  = strdup(r2.type);
    v2.data.regex2->len   = r2.len;
    v2.data.regex2->bytes = malloc(r2.len);
    if (NULL != v2.data.regex2->bytes)
        memcpy(v2.data.regex2->bytes, r2.bytes, r2.len);

    cmp = PMIx_Value_compare(&v1, &v2);
    ok = (PMIX_EQUAL != cmp);
    if (!ok)
        fprintf(stdout, "    different values incorrectly reported as PMIX_EQUAL\n");

cleanup:
    PMIX_VALUE_DESTRUCT(&v1);
    PMIX_VALUE_DESTRUCT(&v2);
    PMIx_Regex2_destruct(&r1);
    PMIx_Regex2_destruct(&r2);
    report("compare: different PMIX_REGEX2 values are not PMIX_EQUAL", ok);
}

/* ------------------------------------------------------------------ */
/* print                                                               */
/* ------------------------------------------------------------------ */

static void test_print_value(void)
{
    pmix_regex2_t src = PMIX_REGEX2_STATIC_INIT;
    pmix_value_t val;
    char *output = NULL;
    pmix_status_t rc;
    int ok = 0;

    PMIX_VALUE_CONSTRUCT(&val);

    if (!make_regex("node500,node501", &src)) {
        report("print PMIX_REGEX2 value (setup)", 0);
        return;
    }

    val.type = PMIX_REGEX2;
    val.data.regex2 = PMIx_Regex2_create(1);
    if (NULL == val.data.regex2) {
        report("print PMIX_REGEX2 value (alloc)", 0);
        PMIx_Regex2_destruct(&src);
        return;
    }
    val.data.regex2->type  = strdup(src.type);
    val.data.regex2->len   = src.len;
    val.data.regex2->bytes = malloc(src.len);
    if (NULL != val.data.regex2->bytes)
        memcpy(val.data.regex2->bytes, src.bytes, src.len);

    rc = PMIx_Data_print(&output, NULL, &val, PMIX_VALUE);
    if (PMIX_SUCCESS != rc) {
        fprintf(stdout, "    PMIx_Data_print failed: %s\n", PMIx_Error_string(rc));
        goto cleanup;
    }

    /* Output must be non-empty and mention the type name. */
    if (NULL != output && '\0' != output[0] &&
        NULL != strstr(output, "PMIX_REGEX2")) {
        ok = 1;
    } else {
        fprintf(stdout, "    print output missing expected content: %s\n",
                output ? output : "(null)");
    }

cleanup:
    free(output);
    PMIX_VALUE_DESTRUCT(&val);
    PMIx_Regex2_destruct(&src);
    report("PMIx_Data_print includes 'PMIX_REGEX2' label", ok);
}

/* ------------------------------------------------------------------ */
/* pack/unpack preserves decodability                                  */
/* ------------------------------------------------------------------ */

static void test_unpack_then_decode(void)
{
    const char *nodes = "node600,node601,node602,node603,node604";
    pmix_regex2_t src = PMIX_REGEX2_STATIC_INIT;
    pmix_regex2_t dst = PMIX_REGEX2_STATIC_INIT;
    pmix_data_buffer_t buf;
    pmix_status_t rc;
    int32_t count;
    char *decoded = NULL;
    int ok = 0;

    if (!make_regex(nodes, &src)) {
        report("unpack then decode (setup)", 0);
        return;
    }

    PMIx_Data_buffer_construct(&buf);

    rc = PMIx_Data_pack(NULL, &buf, &src, 1, PMIX_REGEX2);
    if (PMIX_SUCCESS != rc) goto cleanup;

    count = 1;
    rc = PMIx_Data_unpack(NULL, &buf, &dst, &count, PMIX_REGEX2);
    if (PMIX_SUCCESS != rc) {
        fprintf(stdout, "    unpack failed: %s\n", PMIx_Error_string(rc));
        goto cleanup;
    }

    rc = PMIx_parse_regex2(&dst, NULL, 0, &decoded);
    if (PMIX_SUCCESS != rc) {
        fprintf(stdout, "    parse_regex2 after unpack failed: %s\n", PMIx_Error_string(rc));
        goto cleanup;
    }

    if (NULL != decoded && 0 == strcmp(decoded, nodes)) {
        ok = 1;
    } else {
        fprintf(stdout, "    decoded mismatch: expected '%s', got '%s'\n",
                nodes, decoded ? decoded : "(null)");
    }

cleanup:
    PMIx_Data_buffer_destruct(&buf);
    PMIx_Regex2_destruct(&src);
    PMIx_Regex2_destruct(&dst);
    free(decoded);
    report("unpacked PMIX_REGEX2 still decodable via parse_regex2", ok);
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

    fprintf(stdout, "\n=== PMIX_REGEX2 bfrops unit tests ===\n\n");

    /* pack / unpack */
    test_pack_unpack_roundtrip();
    test_pack_unpack_multiple();
    test_pack_unpack_via_value();

    /* copy */
    test_value_xfer();

    /* compare */
    test_compare_equal();
    test_compare_different();

    /* print */
    test_print_value();

    /* integration: pack → unpack → decode */
    test_unpack_then_decode();

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);

    PMIx_server_finalize();

    return (nfail > 0) ? 1 : 0;
}
