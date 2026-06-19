/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit tests for PMIX_ALLOC_INHERIT bfrops support: pack/unpack, copy,
 * print, and compare operations exercised through the public PMIx API.
 */

#include "src/include/pmix_config.h"
#include "include/pmix.h"
#include "include/pmix_server.h"
#include "src/include/pmix_globals.h"

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

/* ------------------------------------------------------------------ */
/* pack / unpack                                                       */
/* ------------------------------------------------------------------ */

static void test_pack_unpack_roundtrip(void)
{
    pmix_alloc_inheritance_t src = PMIX_ALLOC_INHERIT_CHILD;
    pmix_alloc_inheritance_t dst = PMIX_ALLOC_INHERIT_NONE;
    pmix_data_buffer_t buf;
    pmix_status_t rc;
    int32_t count;
    int ok = 0;

    PMIx_Data_buffer_construct(&buf);

    rc = PMIx_Data_pack(NULL, &buf, &src, 1, PMIX_ALLOC_INHERIT);
    if (PMIX_SUCCESS != rc) {
        fprintf(stdout, "    pack failed: %s\n", PMIx_Error_string(rc));
        goto cleanup;
    }

    count = 1;
    rc = PMIx_Data_unpack(NULL, &buf, &dst, &count, PMIX_ALLOC_INHERIT);
    if (PMIX_SUCCESS != rc) {
        fprintf(stdout, "    unpack failed: %s\n", PMIx_Error_string(rc));
        goto cleanup;
    }

    if (src == dst) {
        ok = 1;
    } else {
        fprintf(stdout, "    value mismatch: packed %u, unpacked %u\n",
                (unsigned) src, (unsigned) dst);
    }

cleanup:
    PMIx_Data_buffer_destruct(&buf);
    report("pack/unpack round-trip", ok);
}

/* Pack multiple values into one buffer and unpack them all. */
static void test_pack_unpack_multiple(void)
{
    const int N = 3;
    pmix_alloc_inheritance_t src[3] = {
        PMIX_ALLOC_INHERIT_NONE,
        PMIX_ALLOC_INHERIT_CHILD,
        PMIX_ALLOC_INHERIT_DEFAULT
    };
    pmix_alloc_inheritance_t dst[3] = { 0, 0, 0 };
    pmix_data_buffer_t buf;
    pmix_status_t rc;
    int32_t count;
    int i, ok = 1;

    PMIx_Data_buffer_construct(&buf);

    /* Pack all three in a single packing operation. */
    rc = PMIx_Data_pack(NULL, &buf, src, N, PMIX_ALLOC_INHERIT);
    if (PMIX_SUCCESS != rc) {
        fprintf(stdout, "    pack failed: %s\n", PMIx_Error_string(rc));
        ok = 0;
        goto cleanup;
    }

    count = N;
    rc = PMIx_Data_unpack(NULL, &buf, dst, &count, PMIX_ALLOC_INHERIT);
    if (PMIX_SUCCESS != rc || count != N) {
        fprintf(stdout, "    unpack failed: %s (count=%d)\n",
                PMIx_Error_string(rc), (int) count);
        ok = 0;
        goto cleanup;
    }

    for (i = 0; i < N; i++) {
        if (src[i] != dst[i]) {
            fprintf(stdout, "    mismatch at index %d: %u != %u\n",
                    i, (unsigned) src[i], (unsigned) dst[i]);
            ok = 0;
            break;
        }
    }

cleanup:
    PMIx_Data_buffer_destruct(&buf);
    report("pack/unpack multiple values", ok);
}

/* Pack/unpack through a pmix_value_t to exercise the value path. */
static void test_pack_unpack_via_value(void)
{
    pmix_value_t val;
    pmix_data_buffer_t buf;
    pmix_status_t rc;
    int32_t count;
    int ok = 0;

    PMIX_VALUE_CONSTRUCT(&val);
    PMIx_Data_buffer_construct(&buf);

    val.type = PMIX_ALLOC_INHERIT;
    val.data.inheritance = PMIX_ALLOC_INHERIT_DEFAULT;

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

    if (PMIX_ALLOC_INHERIT == val.type &&
        PMIX_ALLOC_INHERIT_DEFAULT == val.data.inheritance) {
        ok = 1;
    } else {
        fprintf(stdout, "    value mismatch after unpack (type=%d, val=%u)\n",
                (int) val.type, (unsigned) val.data.inheritance);
    }

cleanup:
    PMIx_Data_buffer_destruct(&buf);
    PMIX_VALUE_DESTRUCT(&val);
    report("pack/unpack via pmix_value_t", ok);
}

/* ------------------------------------------------------------------ */
/* copy                                                                */
/* ------------------------------------------------------------------ */

static void test_value_xfer(void)
{
    pmix_value_t vsrc, vdst;
    pmix_status_t rc;
    int ok = 0;

    PMIX_VALUE_CONSTRUCT(&vsrc);
    PMIX_VALUE_CONSTRUCT(&vdst);

    vsrc.type = PMIX_ALLOC_INHERIT;
    vsrc.data.inheritance = PMIX_ALLOC_INHERIT_CHILD;

    rc = PMIx_Value_xfer(&vdst, &vsrc);
    if (PMIX_SUCCESS != rc) {
        fprintf(stdout, "    PMIx_Value_xfer failed: %s\n", PMIx_Error_string(rc));
        goto cleanup;
    }

    if (PMIX_ALLOC_INHERIT == vdst.type &&
        vsrc.data.inheritance == vdst.data.inheritance) {
        ok = 1;
    } else {
        fprintf(stdout, "    copied value does not match source\n");
    }

cleanup:
    PMIX_VALUE_DESTRUCT(&vsrc);
    PMIX_VALUE_DESTRUCT(&vdst);
    report("PMIx_Value_xfer copies PMIX_ALLOC_INHERIT", ok);
}

/* ------------------------------------------------------------------ */
/* compare                                                             */
/* ------------------------------------------------------------------ */

static void test_compare_equal(void)
{
    pmix_value_t v1, v2;
    pmix_value_cmp_t cmp;
    int ok;

    PMIX_VALUE_CONSTRUCT(&v1);
    PMIX_VALUE_CONSTRUCT(&v2);

    v1.type = PMIX_ALLOC_INHERIT;
    v1.data.inheritance = PMIX_ALLOC_INHERIT_DEFAULT;
    v2.type = PMIX_ALLOC_INHERIT;
    v2.data.inheritance = PMIX_ALLOC_INHERIT_DEFAULT;

    cmp = PMIx_Value_compare(&v1, &v2);
    ok = (PMIX_EQUAL == cmp);
    if (!ok) {
        fprintf(stdout, "    expected PMIX_EQUAL, got %d\n", (int) cmp);
    }

    PMIX_VALUE_DESTRUCT(&v1);
    PMIX_VALUE_DESTRUCT(&v2);
    report("compare: identical PMIX_ALLOC_INHERIT values are PMIX_EQUAL", ok);
}

static void test_compare_different(void)
{
    pmix_value_t v1, v2;
    pmix_value_cmp_t cmp;
    int ok;

    PMIX_VALUE_CONSTRUCT(&v1);
    PMIX_VALUE_CONSTRUCT(&v2);

    v1.type = PMIX_ALLOC_INHERIT;
    v1.data.inheritance = PMIX_ALLOC_INHERIT_NONE;
    v2.type = PMIX_ALLOC_INHERIT;
    v2.data.inheritance = PMIX_ALLOC_INHERIT_CHILD;

    cmp = PMIx_Value_compare(&v1, &v2);
    ok = (PMIX_EQUAL != cmp);
    if (!ok) {
        fprintf(stdout, "    different values incorrectly reported as PMIX_EQUAL\n");
    }

    PMIX_VALUE_DESTRUCT(&v1);
    PMIX_VALUE_DESTRUCT(&v2);
    report("compare: different PMIX_ALLOC_INHERIT values are not PMIX_EQUAL", ok);
}

/* ------------------------------------------------------------------ */
/* print                                                               */
/* ------------------------------------------------------------------ */

static void test_print_value(void)
{
    pmix_value_t val;
    char *output = NULL;
    pmix_status_t rc;
    int ok = 0;

    PMIX_VALUE_CONSTRUCT(&val);

    val.type = PMIX_ALLOC_INHERIT;
    val.data.inheritance = PMIX_ALLOC_INHERIT_CHILD;

    rc = PMIx_Data_print(&output, NULL, &val, PMIX_VALUE);
    if (PMIX_SUCCESS != rc) {
        fprintf(stdout, "    PMIx_Data_print failed: %s\n", PMIx_Error_string(rc));
        goto cleanup;
    }

    /* Output must mention the type name and the converted string value. */
    if (NULL != output && '\0' != output[0] &&
        NULL != strstr(output, "PMIX_ALLOC_INHERIT") &&
        NULL != strstr(output, "CHILD")) {
        ok = 1;
    } else {
        fprintf(stdout, "    print output missing expected content: %s\n",
                output ? output : "(null)");
    }

cleanup:
    free(output);
    PMIX_VALUE_DESTRUCT(&val);
    report("PMIx_Data_print includes 'PMIX_ALLOC_INHERIT' label and value", ok);
}

/* ------------------------------------------------------------------ */
/* string converter                                                    */
/* ------------------------------------------------------------------ */

static void test_string_converter(void)
{
    int ok = 1;

    if (0 != strcmp(PMIx_Alloc_inheritance_string(PMIX_ALLOC_INHERIT_NONE), "NONE")) {
        fprintf(stdout, "    NONE mismatch: got '%s'\n",
                PMIx_Alloc_inheritance_string(PMIX_ALLOC_INHERIT_NONE));
        ok = 0;
    }
    if (0 != strcmp(PMIx_Alloc_inheritance_string(PMIX_ALLOC_INHERIT_CHILD), "CHILD")) {
        fprintf(stdout, "    CHILD mismatch: got '%s'\n",
                PMIx_Alloc_inheritance_string(PMIX_ALLOC_INHERIT_CHILD));
        ok = 0;
    }
    if (0 != strcmp(PMIx_Alloc_inheritance_string(PMIX_ALLOC_INHERIT_DEFAULT), "DEFAULT")) {
        fprintf(stdout, "    DEFAULT mismatch: got '%s'\n",
                PMIx_Alloc_inheritance_string(PMIX_ALLOC_INHERIT_DEFAULT));
        ok = 0;
    }

    report("PMIx_Alloc_inheritance_string returns expected names", ok);
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

    fprintf(stdout, "\n=== PMIX_ALLOC_INHERIT bfrops unit tests ===\n\n");

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

    /* string converter */
    test_string_converter();

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);

    PMIx_server_finalize();

    return (nfail > 0) ? 1 : 0;
}
