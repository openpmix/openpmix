/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit tests for a pmix_data_array_t whose element type is itself
 * PMIX_DATA_ARRAY. The block holds "size" complete pmix_data_array_t
 * descriptors, which is what pack, unpack, copy, print, construct, and
 * destruct must all agree on. Construct used to have no arm for the
 * type (so unpack failed with PMIX_ERR_NOMEM), copy refused it outright,
 * and destruct treated the block as a single descriptor.
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
/* the fixture: an array of two arrays, one of ints and one of strings  */
/* ------------------------------------------------------------------ */

static int build_fixture(pmix_data_array_t *outer)
{
    pmix_data_array_t *elem;
    int *iptr;
    char **sptr;

    PMIx_Data_array_construct(outer, 2, PMIX_DATA_ARRAY);
    if (NULL == outer->array) {
        fprintf(stdout, "    construct of the outer array returned NULL\n");
        return 0;
    }
    elem = (pmix_data_array_t *) outer->array;

    PMIx_Data_array_construct(&elem[0], 3, PMIX_INT);
    if (NULL == elem[0].array) {
        fprintf(stdout, "    construct of element 0 returned NULL\n");
        return 0;
    }
    iptr = (int *) elem[0].array;
    iptr[0] = 1;
    iptr[1] = 2;
    iptr[2] = 3;

    PMIx_Data_array_construct(&elem[1], 2, PMIX_STRING);
    if (NULL == elem[1].array) {
        fprintf(stdout, "    construct of element 1 returned NULL\n");
        return 0;
    }
    sptr = (char **) elem[1].array;
    sptr[0] = strdup("abc");
    sptr[1] = strdup("def");

    return 1;
}

static int check_fixture(pmix_data_array_t *outer)
{
    pmix_data_array_t *elem;
    int *iptr;
    char **sptr;

    if (PMIX_DATA_ARRAY != outer->type || 2 != outer->size || NULL == outer->array) {
        fprintf(stdout, "    outer array is type %d size %lu array %p\n",
                (int) outer->type, (unsigned long) outer->size, outer->array);
        return 0;
    }
    elem = (pmix_data_array_t *) outer->array;

    if (PMIX_INT != elem[0].type || 3 != elem[0].size || NULL == elem[0].array) {
        fprintf(stdout, "    element 0 is type %d size %lu array %p\n",
                (int) elem[0].type, (unsigned long) elem[0].size, elem[0].array);
        return 0;
    }
    iptr = (int *) elem[0].array;
    if (1 != iptr[0] || 2 != iptr[1] || 3 != iptr[2]) {
        fprintf(stdout, "    element 0 holds %d, %d, %d\n", iptr[0], iptr[1], iptr[2]);
        return 0;
    }

    if (PMIX_STRING != elem[1].type || 2 != elem[1].size || NULL == elem[1].array) {
        fprintf(stdout, "    element 1 is type %d size %lu array %p\n",
                (int) elem[1].type, (unsigned long) elem[1].size, elem[1].array);
        return 0;
    }
    sptr = (char **) elem[1].array;
    if (NULL == sptr[0] || 0 != strcmp(sptr[0], "abc") ||
        NULL == sptr[1] || 0 != strcmp(sptr[1], "def")) {
        fprintf(stdout, "    element 1 holds '%s', '%s'\n",
                NULL == sptr[0] ? "(null)" : sptr[0],
                NULL == sptr[1] ? "(null)" : sptr[1]);
        return 0;
    }

    return 1;
}

/* ------------------------------------------------------------------ */
/* construct / destruct                                                */
/* ------------------------------------------------------------------ */

/* Construct must allocate a block of descriptors, not fall through to
 * the "unknown type" arm that leaves the array NULL. */
static void test_construct_allocates(void)
{
    pmix_data_array_t d;
    pmix_data_array_t *elem;
    size_t n;
    int ok = 0;

    PMIx_Data_array_construct(&d, 3, PMIX_DATA_ARRAY);
    if (NULL == d.array) {
        fprintf(stdout, "    construct left the array NULL\n");
        goto cleanup;
    }
    if (PMIX_DATA_ARRAY != d.type || 3 != d.size) {
        fprintf(stdout, "    construct recorded type %d size %lu\n",
                (int) d.type, (unsigned long) d.size);
        goto cleanup;
    }

    /* every element must be a valid empty array, so that destructing a
     * partially filled block is safe */
    ok = 1;
    elem = (pmix_data_array_t *) d.array;
    for (n = 0; n < 3; n++) {
        if (PMIX_UNDEF != elem[n].type || 0 != elem[n].size || NULL != elem[n].array) {
            fprintf(stdout, "    element %lu is not empty: type %d size %lu array %p\n",
                    (unsigned long) n, (int) elem[n].type,
                    (unsigned long) elem[n].size, elem[n].array);
            ok = 0;
            break;
        }
    }

cleanup:
    PMIx_Data_array_destruct(&d);
    report("construct allocates a block of PMIX_DATA_ARRAY descriptors", ok);
}

/* Destruct must release the whole block and reset the descriptor. */
static void test_destruct_resets(void)
{
    pmix_data_array_t d;
    int ok = 0;

    if (!build_fixture(&d)) {
        goto cleanup;
    }

    PMIx_Data_array_destruct(&d);

    if (NULL == d.array && 0 == d.size && PMIX_UNDEF == d.type) {
        ok = 1;
    } else {
        fprintf(stdout, "    destruct left type %d size %lu array %p\n",
                (int) d.type, (unsigned long) d.size, d.array);
    }

cleanup:
    report("destruct releases the block and resets the descriptor", ok);
}

/* Build and tear the whole structure down repeatedly. Destruct used to
 * walk only the first element and never free the block itself, so this
 * is where a leak or a double free shows up under a memory checker. */
static void test_construct_destruct_cycles(void)
{
    pmix_data_array_t d;
    int i, ok = 1;

    for (i = 0; i < 1000; i++) {
        if (!build_fixture(&d)) {
            ok = 0;
            break;
        }
        if (!check_fixture(&d)) {
            ok = 0;
            PMIx_Data_array_destruct(&d);
            break;
        }
        PMIx_Data_array_destruct(&d);
    }

    report("1000 construct/destruct cycles", ok);
}

/* ------------------------------------------------------------------ */
/* pack / unpack                                                       */
/* ------------------------------------------------------------------ */

static void test_pack_unpack_roundtrip(void)
{
    pmix_data_array_t src, dst;
    pmix_data_buffer_t buf;
    pmix_status_t rc;
    int32_t count;
    int ok = 0;

    memset(&dst, 0, sizeof(dst));
    PMIx_Data_buffer_construct(&buf);

    if (!build_fixture(&src)) {
        goto cleanup;
    }

    rc = PMIx_Data_pack(NULL, &buf, &src, 1, PMIX_DATA_ARRAY);
    if (PMIX_SUCCESS != rc) {
        fprintf(stdout, "    pack failed: %s\n", PMIx_Error_string(rc));
        goto cleanup;
    }

    count = 1;
    rc = PMIx_Data_unpack(NULL, &buf, &dst, &count, PMIX_DATA_ARRAY);
    if (PMIX_SUCCESS != rc) {
        fprintf(stdout, "    unpack failed: %s\n", PMIx_Error_string(rc));
        goto cleanup;
    }

    ok = check_fixture(&dst);

cleanup:
    PMIx_Data_buffer_destruct(&buf);
    PMIx_Data_array_destruct(&src);
    PMIx_Data_array_destruct(&dst);
    report("pack/unpack round-trip", ok);
}

/* The same trip through a pmix_value_t, which is how a nested array
 * actually reaches the wire in practice. */
static void test_pack_unpack_via_value(void)
{
    pmix_value_t val;
    pmix_data_buffer_t buf;
    pmix_status_t rc;
    int32_t count;
    int ok = 0;

    PMIX_VALUE_CONSTRUCT(&val);
    PMIx_Data_buffer_construct(&buf);

    val.type = PMIX_DATA_ARRAY;
    val.data.darray = (pmix_data_array_t *) malloc(sizeof(pmix_data_array_t));
    if (NULL == val.data.darray) {
        goto cleanup;
    }
    if (!build_fixture(val.data.darray)) {
        goto cleanup;
    }

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

    if (PMIX_DATA_ARRAY != val.type || NULL == val.data.darray) {
        fprintf(stdout, "    unpacked value is type %d\n", (int) val.type);
        goto cleanup;
    }
    ok = check_fixture(val.data.darray);

cleanup:
    PMIx_Data_buffer_destruct(&buf);
    PMIX_VALUE_DESTRUCT(&val);
    report("pack/unpack via pmix_value_t", ok);
}

/* ------------------------------------------------------------------ */
/* copy                                                                */
/* ------------------------------------------------------------------ */

/* PMIx_Value_xfer used to return PMIX_ERR_NOT_SUPPORTED here. */
static void test_value_xfer(void)
{
    pmix_value_t vsrc, vdst;
    pmix_data_array_t *sd, *dd;
    pmix_status_t rc;
    int ok = 0;

    PMIX_VALUE_CONSTRUCT(&vsrc);
    PMIX_VALUE_CONSTRUCT(&vdst);

    vsrc.type = PMIX_DATA_ARRAY;
    vsrc.data.darray = (pmix_data_array_t *) malloc(sizeof(pmix_data_array_t));
    if (NULL == vsrc.data.darray) {
        goto cleanup;
    }
    if (!build_fixture(vsrc.data.darray)) {
        goto cleanup;
    }

    rc = PMIx_Value_xfer(&vdst, &vsrc);
    if (PMIX_SUCCESS != rc) {
        fprintf(stdout, "    PMIx_Value_xfer failed: %s\n", PMIx_Error_string(rc));
        goto cleanup;
    }

    if (PMIX_DATA_ARRAY != vdst.type || NULL == vdst.data.darray) {
        fprintf(stdout, "    copied value is type %d\n", (int) vdst.type);
        goto cleanup;
    }
    if (!check_fixture(vdst.data.darray)) {
        goto cleanup;
    }

    /* it has to be a deep copy: nothing in the copy may alias the
     * source, or destructing both is a double free */
    sd = (pmix_data_array_t *) vsrc.data.darray->array;
    dd = (pmix_data_array_t *) vdst.data.darray->array;
    if (sd == dd || sd[0].array == dd[0].array || sd[1].array == dd[1].array) {
        fprintf(stdout, "    copy aliases the source\n");
        goto cleanup;
    }

    ok = 1;

cleanup:
    PMIX_VALUE_DESTRUCT(&vsrc);
    PMIX_VALUE_DESTRUCT(&vdst);
    report("PMIx_Value_xfer deep-copies a nested array", ok);
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

    val.type = PMIX_DATA_ARRAY;
    val.data.darray = (pmix_data_array_t *) malloc(sizeof(pmix_data_array_t));
    if (NULL == val.data.darray) {
        goto cleanup;
    }
    if (!build_fixture(val.data.darray)) {
        goto cleanup;
    }

    rc = PMIx_Data_print(&output, NULL, &val, PMIX_VALUE);
    if (PMIX_SUCCESS != rc) {
        fprintf(stdout, "    PMIx_Data_print failed: %s\n", PMIx_Error_string(rc));
        goto cleanup;
    }

    /* the outer array and both of its elements must appear */
    if (NULL != output && '\0' != output[0] &&
        NULL != strstr(output, "PMIX_DATA_ARRAY") &&
        NULL != strstr(output, "abc")) {
        ok = 1;
    } else {
        fprintf(stdout, "    print output missing expected content: %s\n",
                NULL == output ? "(null)" : output);
    }

cleanup:
    free(output);
    PMIX_VALUE_DESTRUCT(&val);
    report("PMIx_Data_print renders the nested array", ok);
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

    fprintf(stdout, "\n=== nested PMIX_DATA_ARRAY unit tests ===\n\n");

    /* construct / destruct */
    test_construct_allocates();
    test_destruct_resets();
    test_construct_destruct_cycles();

    /* pack / unpack */
    test_pack_unpack_roundtrip();
    test_pack_unpack_via_value();

    /* copy */
    test_value_xfer();

    /* print */
    test_print_value();

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);

    PMIx_server_finalize();

    return (nfail > 0) ? 1 : 0;
}
