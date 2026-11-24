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
 * One type is the exception to "the count survives the round trip": a
 * pointer means nothing outside the process that packed it, so an array
 * of PMIX_POINTER carries no values and unpacks with no elements.
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
 * step with src/mca/bfrops/v70/bfrop_pmix70.c: a type that appears there
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
            } else if (out.type != all_types[i] ||
                       (PMIX_POINTER == all_types[i] ? 0 : NELEMENTS) != out.size) {
                fprintf(stdout, "    %s: unpacked as type %d size %lu\n",
                        PMIx_Data_type_string(all_types[i]), (int) out.type,
                        (unsigned long) out.size);
                ok = 0;
            }
            /* what the unpack built is ours to release */
            PMIX_DATA_ARRAY_DESTRUCT(&out);
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

/* The same question for a single value rather than an array: every
 * type a pmix_value_t can carry must be transferable and releasable as
 * a scalar too. That is answered by two more switches -
 * value_xfer() and value_destruct() in bfrop_base_tma.h - which are not
 * the ones the array sweeps above exercise, and PMIX_NODE_PID was once
 * missing from both: an xfer failed with "UNSUPPORTED TYPE" and a
 * destruct leaked the struct and its hostname.
 *
 * Each value is loaded from element 0 of a constructed array, so the
 * loader, the transfer and the comparison all see the same object. */
static pmix_data_type_t no_scalar_form[] = {
    /* no member of the value union holds one - they are array element
     * types only (see the fifth cluster in bfrops/base/AGENTS.md) */
    PMIX_VALUE, PMIX_INFO, PMIX_PDATA, PMIX_APP, PMIX_KVAL, PMIX_BUFFER,
    /* a pmix_cmd_t is the library's own wire command, not a public
     * type, so the union has no member for it. A query value is
     * packable, but only as the payload of a query request */
    PMIX_COMMAND, PMIX_QUERY,
    /* an empty hwloc object is not a valid one; see test_copy_every_type */
    PMIX_PROC_CPUSET, PMIX_TOPO,
    /* a scalar data array is test_copy_every_type's subject */
    PMIX_DATA_ARRAY,
    /* a pointer is handed over, not copied */
    PMIX_POINTER
};

static int has_scalar_form(pmix_data_type_t t)
{
    size_t n;

    for (n = 0; n < sizeof(no_scalar_form) / sizeof(no_scalar_form[0]); n++) {
        if (t == no_scalar_form[n]) {
            return 0;
        }
    }
    return 1;
}

static void test_scalar_xfer_every_type(void)
{
    size_t i;
    int ok = 1;

    for (i = 0; i < sizeof(all_types) / sizeof(all_types[0]); i++) {
        pmix_value_t src, dst;
        pmix_data_array_t *da;
        pmix_status_t rc;

        if (!has_scalar_form(all_types[i])) {
            continue;
        }
        da = PMIx_Data_array_create(1, all_types[i]);
        if (NULL == da) {
            fprintf(stdout, "    %s: could not construct an element\n",
                    PMIx_Data_type_string(all_types[i]));
            ok = 0;
            continue;
        }
        PMIX_VALUE_CONSTRUCT(&src);
        PMIX_VALUE_CONSTRUCT(&dst);
        rc = PMIx_Value_load(&src, da->array, all_types[i]);
        if (PMIX_SUCCESS != rc) {
            fprintf(stdout, "    %s: value load failed: %s\n",
                    PMIx_Data_type_string(all_types[i]), PMIx_Error_string(rc));
            ok = 0;
        } else if (PMIX_SUCCESS != (rc = PMIx_Value_xfer(&dst, &src))) {
            fprintf(stdout, "    %s: value xfer failed: %s\n",
                    PMIx_Data_type_string(all_types[i]), PMIx_Error_string(rc));
            ok = 0;
        } else if (all_types[i] != dst.type) {
            fprintf(stdout, "    %s: value xfer changed the type to %s\n",
                    PMIx_Data_type_string(all_types[i]),
                    PMIx_Data_type_string(dst.type));
            ok = 0;
        } else {
            /* and it prints - as a value, and as an element of an array.
             * Every printer answers a status, and one that answered
             * pmix_asprintf's length instead failed every caller */
            char *txt = PMIx_Value_string(&dst);
            pmix_value_t arr;
            /* "printed" means rendered: the printers' fallback for a
             * type they have no case for also answers a string */
            if (NULL == txt || NULL != strstr(txt, "UNPRINTABLE")) {
                fprintf(stdout, "    %s: value does not print\n",
                        PMIx_Data_type_string(all_types[i]));
                ok = 0;
            }
            free(txt);
            PMIX_VALUE_CONSTRUCT(&arr);
            arr.type = PMIX_DATA_ARRAY;
            arr.data.darray = da;
            txt = PMIx_Value_string(&arr);
            if (NULL == txt || NULL != strstr(txt, "UNPRINTABLE")) {
                fprintf(stdout, "    %s: array does not print\n",
                        PMIx_Data_type_string(all_types[i]));
                ok = 0;
            }
            free(txt);
            /* and it packs and unpacks as a value. pack_val() and
             * unpack_val() have their own switch, and PMIX_DATA_BUFFER
             * was packed from the union itself where unpack reads a
             * pointer to it. (A PMIX_COMPRESSED_STRING is expanded into
             * a PMIX_STRING on the way in - see bfrops/base/AGENTS.md -
             * and an empty element is not compressed data at all.) */
            if (PMIX_COMPRESSED_STRING != all_types[i]) {
                pmix_data_buffer_t buf;
                pmix_value_t back;
                int32_t cnt = 1;

                PMIX_DATA_BUFFER_CONSTRUCT(&buf);
                PMIX_VALUE_CONSTRUCT(&back);
                rc = PMIx_Data_pack(NULL, &buf, &dst, 1, PMIX_VALUE);
                if (PMIX_SUCCESS == rc) {
                    rc = PMIx_Data_unpack(NULL, &buf, &back, &cnt, PMIX_VALUE);
                }
                if (PMIX_SUCCESS != rc || all_types[i] != back.type ||
                    PMIX_EQUAL != PMIx_Value_compare(&dst, &back)) {
                    fprintf(stdout, "    %s: value does not pack and unpack (%s)\n",
                            PMIx_Data_type_string(all_types[i]), PMIx_Error_string(rc));
                    ok = 0;
                }
                PMIX_VALUE_DESTRUCT(&back);
                PMIX_DATA_BUFFER_DESTRUCT(&buf);
            }
        }
        PMIX_VALUE_DESTRUCT(&dst);
        PMIX_VALUE_DESTRUCT(&src);
        PMIx_Data_array_free(da);
    }
    report("every type with a scalar form transfers, prints and packs as a value", ok);
}

/* A node/pid value carries an allocated hostname, so a destruct that
 * does not know the type leaks it (visible under LeakSanitizer), and a
 * transfer that does not know it fails outright. */
static void test_node_pid_value_xfer(void)
{
    pmix_node_pid_t np;
    pmix_value_t src, dst;
    pmix_status_t rc;
    int ok;

    memset(&np, 0, sizeof(np));
    np.hostname = "node-a";
    np.nodeid = 7;
    np.pid = 4242;
    PMIX_VALUE_CONSTRUCT(&src);
    PMIX_VALUE_CONSTRUCT(&dst);
    rc = PMIx_Value_load(&src, &np, PMIX_NODE_PID);
    ok = (PMIX_SUCCESS == rc);
    if (ok) {
        rc = PMIx_Value_xfer(&dst, &src);
        ok = (PMIX_SUCCESS == rc) && (PMIX_NODE_PID == dst.type) &&
             (NULL != dst.data.nodepid) && (NULL != dst.data.nodepid->hostname) &&
             (0 == strcmp("node-a", dst.data.nodepid->hostname)) &&
             (dst.data.nodepid->hostname != src.data.nodepid->hostname) &&
             (7 == dst.data.nodepid->nodeid) && (4242 == dst.data.nodepid->pid);
    }
    PMIX_VALUE_DESTRUCT(&dst);
    PMIX_VALUE_DESTRUCT(&src);
    ok = ok && (PMIX_UNDEF == src.type) && (NULL == src.data.nodepid);
    report("a node/pid value transfers and destructs", ok);
}

/* The sweep above loads empty elements, so it cannot see a defect that
 * shows only in a non-zero field. A registered attribute's type is one:
 * it is packed unsigned, and was unpacked signed, which the size-based
 * integer encoding writes differently - 3 came back as 65534. Zero
 * survives that, so the sweep passed. */
static void test_regattr_type_survives_pack(void)
{
    pmix_regattr_t ra, *ras;
    pmix_data_array_t *da;
    pmix_value_t v, back;
    pmix_data_buffer_t buf;
    pmix_status_t rc;
    int32_t cnt;
    int ok = 1;
    size_t n;
    char *desc[] = {"a description", NULL};

    memset(&ra, 0, sizeof(ra));
    ra.name = "PMIX_EXAMPLE";
    PMIx_Load_key(ra.string, "pmix.example");
    ra.type = PMIX_UINT32;
    ra.description = desc;

    /* as a value */
    PMIX_VALUE_CONSTRUCT(&v);
    PMIX_VALUE_CONSTRUCT(&back);
    PMIX_DATA_BUFFER_CONSTRUCT(&buf);
    rc = PMIx_Value_load(&v, &ra, PMIX_REGATTR);
    if (PMIX_SUCCESS == rc) {
        rc = PMIx_Data_pack(NULL, &buf, &v, 1, PMIX_VALUE);
    }
    cnt = 1;
    if (PMIX_SUCCESS == rc) {
        rc = PMIx_Data_unpack(NULL, &buf, &back, &cnt, PMIX_VALUE);
    }
    if (PMIX_SUCCESS != rc || PMIX_REGATTR != back.type || NULL == back.data.ptr ||
        PMIX_UINT32 != ((pmix_regattr_t *) back.data.ptr)->type) {
        fprintf(stdout, "    regattr value: type did not survive (%s)\n",
                PMIx_Error_string(rc));
        ok = 0;
    }
    PMIX_VALUE_DESTRUCT(&back);
    PMIX_VALUE_DESTRUCT(&v);
    PMIX_DATA_BUFFER_DESTRUCT(&buf);

    /* and as the elements of an array, which is how a query answers */
    da = PMIx_Data_array_create(3, PMIX_REGATTR);
    ras = (pmix_regattr_t *) da->array;
    for (n = 0; n < 3; n++) {
        ras[n].name = strdup("PMIX_EXAMPLE");
        PMIx_Load_key(ras[n].string, "pmix.example");
        ras[n].type = (pmix_data_type_t) (PMIX_BOOL + n * 10);
    }
    PMIX_VALUE_CONSTRUCT(&v);
    PMIX_VALUE_CONSTRUCT(&back);
    PMIX_DATA_BUFFER_CONSTRUCT(&buf);
    v.type = PMIX_DATA_ARRAY;
    v.data.darray = da;
    rc = PMIx_Data_pack(NULL, &buf, &v, 1, PMIX_VALUE);
    cnt = 1;
    if (PMIX_SUCCESS == rc) {
        rc = PMIx_Data_unpack(NULL, &buf, &back, &cnt, PMIX_VALUE);
    }
    if (PMIX_SUCCESS != rc || PMIX_DATA_ARRAY != back.type || NULL == back.data.darray ||
        3 != back.data.darray->size) {
        fprintf(stdout, "    regattr array: did not survive (%s)\n", PMIx_Error_string(rc));
        ok = 0;
    } else {
        ras = (pmix_regattr_t *) back.data.darray->array;
        for (n = 0; n < 3; n++) {
            if ((pmix_data_type_t) (PMIX_BOOL + n * 10) != ras[n].type) {
                fprintf(stdout, "    regattr array: element %d type %u came back as %u\n",
                        (int) n, (unsigned) (PMIX_BOOL + n * 10), (unsigned) ras[n].type);
                ok = 0;
            }
        }
    }
    PMIX_VALUE_DESTRUCT(&back);
    PMIX_VALUE_DESTRUCT(&v);
    PMIX_DATA_BUFFER_DESTRUCT(&buf);
    report("a registered attribute's type survives pack and unpack", ok);
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
    if (PMIX_SUCCESS == rc) {
        PMIX_DATA_ARRAY_DESTRUCT(&out);
    }

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

/* PMIX_INFO_DIRECTIVES, PMIX_DATA_TYPE and PMIX_IOF_CHANNEL were
 * registered types - packable, unpackable, copyable as array elements -
 * with no member of the value union, so a value of one of them crossed
 * the wire intact and then could not be loaded, transferred (hence not
 * stored), unloaded, compared or printed, and PMIx_Value_get_size()
 * left its payload out. The sweep above loads empty elements, so this
 * one uses non-zero contents and checks they survive each step. */
static int check_named_value(pmix_data_type_t t, const void *a, const void *b,
                             size_t width, const char *aname, uint32_t anum)
{
    pmix_value_t va, vb, dst, back, arr, arr2;
    pmix_data_array_t *da, *db;
    pmix_data_buffer_t buf;
    pmix_status_t rc;
    uint64_t store = 0;
    void *out = &store;
    size_t sz = 0;
    uint32_t num = 0;
    int32_t cnt = 1;
    char *txt;
    int ok = 1;
    const char *tn = PMIx_Data_type_string(t);

#define NV_FAIL(what)                                          \
    do {                                                       \
        fprintf(stdout, "    %s: %s\n", tn, what);             \
        ok = 0;                                                \
    } while (0)

    PMIX_VALUE_CONSTRUCT(&va);
    PMIX_VALUE_CONSTRUCT(&vb);
    PMIX_VALUE_CONSTRUCT(&dst);
    PMIX_VALUE_CONSTRUCT(&back);
    if (PMIX_SUCCESS != PMIx_Value_load(&va, a, t) ||
        PMIX_SUCCESS != PMIx_Value_load(&vb, b, t)) {
        NV_FAIL("value load failed");
        goto done;
    }
    if (0 != memcmp(&va.data, a, width)) {
        NV_FAIL("value load did not store the contents");
    }
    if (PMIX_SUCCESS != PMIx_Value_xfer(&dst, &va) || t != dst.type ||
        PMIX_EQUAL != PMIx_Value_compare(&dst, &va)) {
        NV_FAIL("value xfer did not reproduce the value");
    }
    if (PMIX_EQUAL == PMIx_Value_compare(&va, &vb)) {
        NV_FAIL("two different values compare equal");
    }
    rc = PMIx_Value_unload(&va, &out, &sz);
    if (PMIX_SUCCESS != rc || width != sz || 0 != memcmp(out, a, width)) {
        NV_FAIL("value unload did not return the contents");
    }
    if (PMIX_SUCCESS != PMIx_Value_get_size(&va, &sz) ||
        sizeof(pmix_value_t) + width != sz) {
        NV_FAIL("value size leaves out the payload");
    }
    if (PMIX_SUCCESS != PMIx_Value_get_number(&va, &num, PMIX_UINT32) || anum != num) {
        NV_FAIL("value does not read back as a number");
    }
    txt = PMIx_Value_string(&va);
    if (NULL == txt || NULL == strstr(txt, aname)) {
        NV_FAIL("value does not print its contents");
    }
    free(txt);
    PMIX_DATA_BUFFER_CONSTRUCT(&buf);
    rc = PMIx_Data_pack(NULL, &buf, &va, 1, PMIX_VALUE);
    if (PMIX_SUCCESS == rc) {
        rc = PMIx_Data_unpack(NULL, &buf, &back, &cnt, PMIX_VALUE);
    }
    if (PMIX_SUCCESS != rc || t != back.type || 0 != memcmp(&back.data, a, width)) {
        NV_FAIL("value contents do not survive pack and unpack");
    }
    PMIX_DATA_BUFFER_DESTRUCT(&buf);

    /* and as the elements of an array */
    da = PMIx_Data_array_create(2, t);
    db = PMIx_Data_array_create(2, t);
    if (NULL == da || NULL == db) {
        NV_FAIL("could not construct an array");
        PMIx_Data_array_free(da);
        PMIx_Data_array_free(db);
        goto done;
    }
    memcpy(da->array, a, width);
    memcpy((char *) da->array + width, b, width);
    memcpy(db->array, a, width);
    memcpy((char *) db->array + width, a, width);
    PMIX_VALUE_CONSTRUCT(&arr);
    PMIX_VALUE_CONSTRUCT(&arr2);
    arr.type = PMIX_DATA_ARRAY;
    arr.data.darray = da;
    arr2.type = PMIX_DATA_ARRAY;
    arr2.data.darray = db;
    if (PMIX_EQUAL != PMIx_Value_compare(&arr, &arr)) {
        NV_FAIL("array does not compare equal to itself");
    }
    if (PMIX_EQUAL == PMIx_Value_compare(&arr, &arr2)) {
        NV_FAIL("two different arrays compare equal");
    }
    if (PMIX_SUCCESS != PMIx_Value_get_size(&arr, &sz) ||
        sizeof(pmix_value_t) + sizeof(pmix_data_array_t) + 2 * width != sz) {
        NV_FAIL("array size leaves out the elements");
    }
    txt = PMIx_Value_string(&arr);
    if (NULL == txt || NULL == strstr(txt, aname) || NULL != strstr(txt, "UNPRINTABLE")) {
        NV_FAIL("array does not print its elements");
    }
    free(txt);
    PMIx_Data_array_free(da);
    PMIx_Data_array_free(db);

done:
#undef NV_FAIL
    PMIX_VALUE_DESTRUCT(&back);
    PMIX_VALUE_DESTRUCT(&dst);
    PMIX_VALUE_DESTRUCT(&vb);
    PMIX_VALUE_DESTRUCT(&va);
    return ok;
}

static void test_named_values_keep_their_contents(void)
{
    pmix_info_directives_t d1 = PMIX_INFO_REQD, d2 = PMIX_INFO_ARRAY_END;
    pmix_data_type_t t1 = PMIX_STRING, t2 = PMIX_PROC;
    pmix_iof_channel_t c1 = PMIX_FWD_STDOUT_CHANNEL, c2 = PMIX_FWD_STDERR_CHANNEL;
    int ok = 1;

    ok &= check_named_value(PMIX_INFO_DIRECTIVES, &d1, &d2, sizeof(d1),
                            "REQUIRED", (uint32_t) d1);
    ok &= check_named_value(PMIX_DATA_TYPE, &t1, &t2, sizeof(t1),
                            "PMIX_STRING", (uint32_t) t1);
    ok &= check_named_value(PMIX_IOF_CHANNEL, &c1, &c2, sizeof(c1),
                            "STDOUT", (uint32_t) c1);
    report("directive, data-type and channel values keep their contents", ok);
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
    test_scalar_xfer_every_type();
    test_named_values_keep_their_contents();
    test_node_pid_value_xfer();
    test_regattr_type_survives_pack();

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);

    PMIx_server_finalize();

    return (nfail > 0) ? 1 : 0;
}
