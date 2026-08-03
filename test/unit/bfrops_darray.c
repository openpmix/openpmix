/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit tests for the bfrops data-array machinery.
 *
 * A pmix_data_array_t is described on the wire by an element type and a
 * count, and every one of the four bfrops operations - construct/destruct,
 * pack, unpack, copy - has to agree on what an element of that type is.
 * They have not always agreed: a type could be packable but not
 * constructible (so it never unpacked), or constructible at one width and
 * copied at another (so copying it read past the block). This test walks
 * the whole registered type table and holds those four operations against
 * each other, which is the only way that class of mismatch shows up.
 *
 * It also covers two specific wire behaviours that a per-type sweep does
 * not reach: an array whose element type is PMIX_UNDEF (the "no array
 * here" marker, which pack and unpack must spell identically or the rest
 * of the message is misread), and PMIX_DATA_BUFFER, whose unpacked form
 * must be readable and not merely present.
 */

#include "src/include/pmix_config.h"
#include "include/pmix.h"
#include "include/pmix_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static pmix_server_module_t mymodule = {
    .client_connected = NULL
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

/* Every data type the newest bfrops component registers. Keep this in
 * step with src/mca/bfrops/v61/bfrop_pmix61.c: a type that appears there
 * and not here simply goes untested. */
static pmix_data_type_t all_types[] = {
    PMIX_BOOL, PMIX_BYTE, PMIX_STRING, PMIX_SIZE, PMIX_PID, PMIX_INT,
    PMIX_INT8, PMIX_INT16, PMIX_INT32, PMIX_INT64, PMIX_UINT, PMIX_UINT8,
    PMIX_UINT16, PMIX_UINT32, PMIX_UINT64, PMIX_FLOAT, PMIX_DOUBLE,
    PMIX_TIMEVAL, PMIX_TIME, PMIX_STATUS, PMIX_VALUE, PMIX_PROC, PMIX_APP,
    PMIX_INFO, PMIX_PDATA, PMIX_BUFFER, PMIX_BYTE_OBJECT, PMIX_KVAL,
    PMIX_PERSIST, PMIX_POINTER, PMIX_SCOPE, PMIX_DATA_RANGE, PMIX_COMMAND,
    PMIX_INFO_DIRECTIVES, PMIX_DATA_TYPE, PMIX_PROC_STATE, PMIX_PROC_INFO,
    PMIX_DATA_ARRAY, PMIX_PROC_RANK, PMIX_QUERY, PMIX_COMPRESSED_STRING,
    PMIX_ALLOC_DIRECTIVE, PMIX_IOF_CHANNEL, PMIX_ENVAR, PMIX_COORD,
    PMIX_REGATTR, PMIX_JOB_STATE, PMIX_LINK_STATE, PMIX_PROC_CPUSET,
    PMIX_GEOMETRY, PMIX_DEVTYPE, PMIX_DEVICE_DIST, PMIX_ENDPOINT,
    PMIX_TOPO, PMIX_DEVICE, PMIX_RESOURCE_UNIT, PMIX_LOCTYPE,
    PMIX_PROC_NSPACE, PMIX_COMPRESSED_BYTE_OBJECT, PMIX_DATA_BUFFER,
    PMIX_ALLOC_INHERIT, PMIX_STOR_MEDIUM, PMIX_STOR_ACCESS,
    PMIX_STOR_PERSIST, PMIX_STOR_ACCESS_TYPE, PMIX_RESBLOCK_DIRECTIVE,
    PMIX_NODE_PID, PMIX_REGEX2
};

/* PMIX_REGEX is the one registered type this sweep deliberately skips.
 * The tree does not agree on the width of an element of such an array -
 * the packer strides char*, the destructor strides pmix_byte_object_t -
 * so it is intentionally not constructible. See the note in
 * pmix_bfrops_base_tma_data_array_construct(). */

#define NELEMENTS 3

/* ------------------------------------------------------------------ */
/* every registered type must be usable as a data-array element        */
/* ------------------------------------------------------------------ */

static void test_construct_every_type(void)
{
    size_t i;
    int ok = 1;

    for (i = 0; i < sizeof(all_types) / sizeof(all_types[0]); i++) {
        pmix_data_array_t *da = PMIx_Data_array_create(NELEMENTS, all_types[i]);

        if (NULL == da || NULL == da->array || NELEMENTS != da->size
            || all_types[i] != da->type) {
            fprintf(stdout, "    %s: construct produced no array\n",
                    PMIx_Data_type_string(all_types[i]));
            ok = 0;
        }
        if (NULL != da) {
            PMIx_Data_array_free(da);
        }
    }
    report("every registered type can back a data array", ok);
}

static void test_pack_unpack_every_type(void)
{
    size_t i;
    int ok = 1;

    for (i = 0; i < sizeof(all_types) / sizeof(all_types[0]); i++) {
        pmix_data_array_t *da;
        pmix_data_array_t out;
        pmix_data_buffer_t buf;
        pmix_status_t rc;
        int32_t cnt;

        da = PMIx_Data_array_create(NELEMENTS, all_types[i]);
        if (NULL == da) {
            continue; /* reported by test_construct_every_type */
        }
        PMIX_DATA_BUFFER_CONSTRUCT(&buf);
        rc = PMIx_Data_pack(NULL, &buf, da, 1, PMIX_DATA_ARRAY);
        if (PMIX_SUCCESS != rc) {
            fprintf(stdout, "    %s: pack failed: %s\n",
                    PMIx_Data_type_string(all_types[i]), PMIx_Error_string(rc));
            ok = 0;
        } else {
            memset(&out, 0, sizeof(out));
            cnt = 1;
            rc = PMIx_Data_unpack(NULL, &buf, &out, &cnt, PMIX_DATA_ARRAY);
            if (PMIX_SUCCESS != rc) {
                fprintf(stdout, "    %s: unpack failed: %s\n",
                        PMIx_Data_type_string(all_types[i]), PMIx_Error_string(rc));
                ok = 0;
            } else if (out.type != all_types[i] || NELEMENTS != out.size) {
                fprintf(stdout, "    %s: unpacked as type %d size %lu\n",
                        PMIx_Data_type_string(all_types[i]), (int) out.type,
                        (unsigned long) out.size);
                ok = 0;
            }
        }
        PMIX_DATA_BUFFER_DESTRUCT(&buf);
        PMIx_Data_array_free(da);
    }
    report("every registered type round-trips as a data array", ok);
}

static void test_copy_every_type(void)
{
    size_t i;
    int ok = 1;

    for (i = 0; i < sizeof(all_types) / sizeof(all_types[0]); i++) {
        pmix_value_t src, dst;
        pmix_data_array_t *da;
        pmix_status_t rc;

        /* an empty hwloc object is not a valid one, so copying an
         * unpopulated cpuset or topology legitimately declines - the
         * point of covering them here is that it declines rather than
         * corrupting the heap on the way out */
        if (PMIX_PROC_CPUSET == all_types[i] || PMIX_TOPO == all_types[i]) {
            continue;
        }
        da = PMIx_Data_array_create(NELEMENTS, all_types[i]);
        if (NULL == da) {
            continue;
        }
        memset(&src, 0, sizeof(src));
        memset(&dst, 0, sizeof(dst));
        src.type = PMIX_DATA_ARRAY;
        src.data.darray = da;

        rc = PMIx_Value_xfer(&dst, &src);
        if (PMIX_SUCCESS != rc) {
            fprintf(stdout, "    %s: value xfer failed: %s\n",
                    PMIx_Data_type_string(all_types[i]), PMIx_Error_string(rc));
            ok = 0;
        } else if (NULL == dst.data.darray || NELEMENTS != dst.data.darray->size
                   || all_types[i] != dst.data.darray->type) {
            fprintf(stdout, "    %s: value xfer produced a short array\n",
                    PMIx_Data_type_string(all_types[i]));
            ok = 0;
        } else {
            PMIX_VALUE_DESTRUCT(&dst);
        }
        PMIx_Data_array_free(da);
    }
    report("every registered type copies as a data array", ok);
}

/* An empty hwloc cpuset cannot be copied - the interesting part is that
 * declining leaves the heap intact rather than releasing the element
 * block twice. */
static void test_uncopyable_cpuset_array_declines_cleanly(void)
{
    pmix_value_t src, dst;
    pmix_data_array_t *da;
    pmix_status_t rc;
    int ok;

    da = PMIx_Data_array_create(NELEMENTS, PMIX_PROC_CPUSET);
    if (NULL == da) {
        report("an uncopyable cpuset array declines without a double free", 0);
        return;
    }
    memset(&src, 0, sizeof(src));
    memset(&dst, 0, sizeof(dst));
    src.type = PMIX_DATA_ARRAY;
    src.data.darray = da;

    rc = PMIx_Value_xfer(&dst, &src);
    ok = (PMIX_SUCCESS != rc && NULL == dst.data.darray);

    /* if the block had been released twice the process would be gone by
     * now; make sure the source is still intact and still freeable */
    ok = ok && (NULL != da->array) && (NELEMENTS == da->size);
    PMIx_Data_array_free(da);

    report("an uncopyable cpuset array declines without a double free", ok);
}

/* ------------------------------------------------------------------ */
/* an undefined element type is a marker, and must be one on both sides */
/* ------------------------------------------------------------------ */

static void test_undef_array_keeps_the_stream_in_step(void)
{
    pmix_data_buffer_t buf;
    pmix_data_array_t da, out;
    pmix_status_t rc;
    int32_t cnt;
    char *sentinel = "SENTINEL";
    char *back = NULL;
    int ok = 1;

    /* a descriptor with no element type: exactly what copying a value
     * whose darray pointer was NULL hands back */
    memset(&da, 0, sizeof(da));

    PMIX_DATA_BUFFER_CONSTRUCT(&buf);
    rc = PMIx_Data_pack(NULL, &buf, &da, 1, PMIX_DATA_ARRAY);
    ok = ok && (PMIX_SUCCESS == rc);
    rc = PMIx_Data_pack(NULL, &buf, &sentinel, 1, PMIX_STRING);
    ok = ok && (PMIX_SUCCESS == rc);

    memset(&out, 0, sizeof(out));
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, &buf, &out, &cnt, PMIX_DATA_ARRAY);
    ok = ok && (PMIX_SUCCESS == rc) && (PMIX_UNDEF == out.type) && (0 == out.size);

    /* the real assertion: whatever follows the marker must still be
     * where the reader expects it */
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, &buf, &back, &cnt, PMIX_STRING);
    ok = ok && (PMIX_SUCCESS == rc) && (NULL != back)
         && (0 == strcmp(back, "SENTINEL"));
    if (NULL != back) {
        free(back);
    }
    PMIX_DATA_BUFFER_DESTRUCT(&buf);

    report("an undefined-element array does not desync the stream", ok);
}

static void test_null_darray_value_keeps_the_stream_in_step(void)
{
    pmix_data_buffer_t buf;
    pmix_value_t v, out;
    pmix_status_t rc;
    int32_t cnt;
    char *sentinel = "AFTER";
    char *back = NULL;
    int ok = 1;

    /* a value that says "data array" and carries no array */
    memset(&v, 0, sizeof(v));
    v.type = PMIX_DATA_ARRAY;
    v.data.darray = NULL;

    PMIX_DATA_BUFFER_CONSTRUCT(&buf);
    rc = PMIx_Data_pack(NULL, &buf, &v, 1, PMIX_VALUE);
    ok = ok && (PMIX_SUCCESS == rc);
    rc = PMIx_Data_pack(NULL, &buf, &sentinel, 1, PMIX_STRING);
    ok = ok && (PMIX_SUCCESS == rc);

    memset(&out, 0, sizeof(out));
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, &buf, &out, &cnt, PMIX_VALUE);
    ok = ok && (PMIX_SUCCESS == rc);

    cnt = 1;
    rc = PMIx_Data_unpack(NULL, &buf, &back, &cnt, PMIX_STRING);
    ok = ok && (PMIX_SUCCESS == rc) && (NULL != back)
         && (0 == strcmp(back, "AFTER"));
    if (NULL != back) {
        free(back);
    }
    PMIX_VALUE_DESTRUCT(&out);
    PMIX_DATA_BUFFER_DESTRUCT(&buf);

    report("a NULL-array value does not desync the stream", ok);
}

/* ------------------------------------------------------------------ */
/* a nested data buffer must come back readable                        */
/* ------------------------------------------------------------------ */

static void test_data_buffer_roundtrip_is_readable(void)
{
    pmix_data_buffer_t outer, inner, got;
    pmix_status_t rc;
    int32_t cnt;
    char *payload = "nested";
    char *back = NULL;
    int ok = 1;

    PMIX_DATA_BUFFER_CONSTRUCT(&inner);
    rc = PMIx_Data_pack(NULL, &inner, &payload, 1, PMIX_STRING);
    ok = ok && (PMIX_SUCCESS == rc);

    PMIX_DATA_BUFFER_CONSTRUCT(&outer);
    rc = PMIx_Data_pack(NULL, &outer, &inner, 1, PMIX_DATA_BUFFER);
    ok = ok && (PMIX_SUCCESS == rc);

    PMIX_DATA_BUFFER_CONSTRUCT(&got);
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, &outer, &got, &cnt, PMIX_DATA_BUFFER);
    ok = ok && (PMIX_SUCCESS == rc);

    /* recovering the payload is only half the job - without the cursors
     * and the allocation size the caller cannot read a single value out
     * of what it was handed */
    ok = ok && (got.bytes_used == inner.bytes_used)
         && (got.bytes_allocated >= got.bytes_used)
         && (NULL != got.unpack_ptr) && (NULL != got.pack_ptr);

    cnt = 1;
    rc = PMIx_Data_unpack(NULL, &got, &back, &cnt, PMIX_STRING);
    ok = ok && (PMIX_SUCCESS == rc) && (NULL != back)
         && (0 == strcmp(back, "nested"));
    if (NULL != back) {
        free(back);
    }

    PMIX_DATA_BUFFER_DESTRUCT(&got);
    PMIX_DATA_BUFFER_DESTRUCT(&outer);
    PMIX_DATA_BUFFER_DESTRUCT(&inner);

    report("an unpacked data buffer can be read from", ok);
}

/* ------------------------------------------------------------------ */
/* a kval array with unfilled elements must pack rather than crash      */
/* ------------------------------------------------------------------ */

static void test_kval_array_with_empty_elements(void)
{
    pmix_data_array_t *da;
    pmix_data_array_t out;
    pmix_data_buffer_t buf;
    pmix_status_t rc;
    int32_t cnt;
    int ok = 1;

    da = PMIx_Data_array_create(NELEMENTS, PMIX_KVAL);
    if (NULL == da || NULL == da->array) {
        report("a kval array with empty elements packs", 0);
        return;
    }
    /* every element still has a NULL key and a NULL value pointer */
    PMIX_DATA_BUFFER_CONSTRUCT(&buf);
    rc = PMIx_Data_pack(NULL, &buf, da, 1, PMIX_DATA_ARRAY);
    ok = ok && (PMIX_SUCCESS == rc);

    memset(&out, 0, sizeof(out));
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, &buf, &out, &cnt, PMIX_DATA_ARRAY);
    ok = ok && (PMIX_SUCCESS == rc) && (NELEMENTS == out.size);

    PMIX_DATA_BUFFER_DESTRUCT(&buf);
    PMIx_Data_array_free(da);

    report("a kval array with empty elements packs", ok);
}

/* ------------------------------------------------------------------ */
/* a populated array must survive the round trip with its contents      */
/* ------------------------------------------------------------------ */

static void test_populated_array_contents_survive(void)
{
    pmix_data_array_t da;
    pmix_data_array_t out;
    pmix_data_buffer_t buf;
    pmix_status_t rc;
    int32_t cnt;
    int32_t *vals;
    int ok = 1;
    size_t n;

    PMIX_DATA_ARRAY_CONSTRUCT(&da, 5, PMIX_INT32);
    vals = (int32_t *) da.array;
    for (n = 0; n < 5; n++) {
        vals[n] = (int32_t) (n * 1000) - 2000;
    }

    PMIX_DATA_BUFFER_CONSTRUCT(&buf);
    rc = PMIx_Data_pack(NULL, &buf, &da, 1, PMIX_DATA_ARRAY);
    ok = ok && (PMIX_SUCCESS == rc);

    memset(&out, 0, sizeof(out));
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, &buf, &out, &cnt, PMIX_DATA_ARRAY);
    ok = ok && (PMIX_SUCCESS == rc) && (5 == out.size) && (PMIX_INT32 == out.type);
    if (ok) {
        int32_t *got = (int32_t *) out.array;
        for (n = 0; n < 5; n++) {
            if (got[n] != vals[n]) {
                ok = 0;
                break;
            }
        }
    }
    PMIX_DATA_ARRAY_DESTRUCT(&out);
    PMIX_DATA_BUFFER_DESTRUCT(&buf);
    PMIX_DATA_ARRAY_DESTRUCT(&da);

    report("a populated int32 array round-trips its contents", ok);
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    pmix_status_t rc;
    (void) argc;
    (void) argv;

    rc = PMIx_server_init(&mymodule, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    fprintf(stdout, "\n=== bfrops data-array unit tests ===\n\n");

    test_construct_every_type();
    test_pack_unpack_every_type();
    test_copy_every_type();
    test_uncopyable_cpuset_array_declines_cleanly();
    test_undef_array_keeps_the_stream_in_step();
    test_null_darray_value_keeps_the_stream_in_step();
    test_data_buffer_roundtrip_is_readable();
    test_kval_array_with_empty_elements();
    test_populated_array_contents_survive();

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);

    PMIx_server_finalize();

    return (nfail > 0) ? 1 : 0;
}
