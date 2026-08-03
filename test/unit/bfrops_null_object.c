/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * A pmix_value_t may be tagged with a type whose payload lives behind a
 * pointer and carry no object at all. That value is malformed - but it
 * is the single easiest malformed value in PMIx to produce, because
 * producing it takes nothing more than setting the type before the
 * data:
 *
 *     pmix_value_t v;
 *     PMIX_VALUE_CONSTRUCT(&v);
 *     v.type = PMIX_PROC_INFO;      // and nothing else
 *
 * Every operation in bfrops trusts the type tag - it has to; the tag is
 * the only thing that says which union member is live - and then
 * dereferences what it points at. So each of them has to screen the
 * pointer for itself, and each of them is a separate switch statement
 * with sixty-odd arms, which is why they did not.
 *
 * This walks every pointer-backed type through every value-level
 * operation and asserts one thing: the library survives. What each
 * operation returns is deliberately not checked. "There is no object
 * here" is a state all of them can express - an empty comparison, a
 * size of zero, an unloaded object of no bytes, a printed "NULL
 * pointer" - and which one a given operation chooses is not the
 * subject. The subject is that an ordinary caller mistake does not take
 * the process down.
 *
 * Every case in here segfaulted before the August 2026 review of
 * src/mca/bfrops. If one of them starts failing, the fix is a screen at
 * the operation, not a screen at whatever caller happened to reach it -
 * see pmix_bfrops_base_value_is_null_object().
 *
 * This test is noisy on purpose. A few of the types below - PMIX_APP,
 * PMIX_INFO, PMIX_PDATA, PMIX_QUERY, PMIX_KVAL, PMIX_BUFFER, PMIX_VALUE
 * - have no member in the value union at all: they are element types
 * for a data array, not things a scalar value can hold. Handing one to
 * a value operation is a caller error, and the library says so on
 * stderr. Those lines are the library diagnosing rather than faulting,
 * which is precisely what is being asserted; do not silence them by
 * dropping the types.
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

/* Every type whose value union member is a pointer, plus a handful of
 * neighbours so a regression that widens the blast radius is visible
 * here rather than somewhere else. */
static pmix_data_type_t types[] = {
    PMIX_STRING, PMIX_VALUE, PMIX_PROC, PMIX_APP, PMIX_INFO, PMIX_PDATA,
    PMIX_BUFFER, PMIX_BYTE_OBJECT, PMIX_KVAL, PMIX_PROC_INFO,
    PMIX_DATA_ARRAY, PMIX_QUERY, PMIX_COMPRESSED_STRING, PMIX_ENVAR,
    PMIX_COORD, PMIX_REGATTR, PMIX_REGEX, PMIX_JOB_STATE,
    PMIX_PROC_CPUSET, PMIX_GEOMETRY, PMIX_DEVICE_DIST, PMIX_ENDPOINT,
    PMIX_TOPO, PMIX_DEVICE, PMIX_RESOURCE_UNIT, PMIX_PROC_NSPACE,
    PMIX_COMPRESSED_BYTE_OBJECT, PMIX_DATA_BUFFER, PMIX_NODE_PID,
    PMIX_REGEX2, PMIX_LINK_STATE, PMIX_DEVTYPE, PMIX_LOCTYPE
};
#define NTYPES (sizeof(types) / sizeof(types[0]))

/* type-tagged, payload absent: the malformed value under test */
static void empty_value(pmix_value_t *v, pmix_data_type_t t)
{
    memset(v, 0, sizeof(*v));
    v->type = t;
}

static void test_print(void)
{
    size_t i;

    for (i = 0; i < NTYPES; i++) {
        pmix_value_t v;
        pmix_info_t info;
        char *s;

        empty_value(&v, types[i]);
        s = PMIx_Value_string(&v);
        if (NULL != s) {
            free(s);
        }

        memset(&info, 0, sizeof(info));
        PMIx_Load_key(info.key, "a-key");
        info.value.type = types[i];
        s = PMIx_Info_string(&info);
        if (NULL != s) {
            free(s);
        }
    }
    report("printing a value with no object", 1);
}

static void test_compare(void)
{
    size_t i;

    for (i = 0; i < NTYPES; i++) {
        pmix_value_t a, b;

        empty_value(&a, types[i]);
        empty_value(&b, types[i]);
        (void) PMIx_Value_compare(&a, &b);
    }
    report("comparing two values with no object", 1);
}

static void test_get_size(void)
{
    size_t i;

    for (i = 0; i < NTYPES; i++) {
        pmix_value_t v;
        pmix_info_t info;
        size_t sz = 0;

        empty_value(&v, types[i]);
        (void) PMIx_Value_get_size(&v, &sz);

        memset(&info, 0, sizeof(info));
        PMIx_Load_key(info.key, "a-key");
        info.value.type = types[i];
        sz = 0;
        (void) PMIx_Info_get_size(&info, &sz);
    }
    report("sizing a value with no object", 1);
}

static void test_xfer(void)
{
    size_t i;

    for (i = 0; i < NTYPES; i++) {
        pmix_value_t src, dst;
        pmix_info_t isrc, idst;

        empty_value(&src, types[i]);
        memset(&dst, 0, sizeof(dst));
        (void) PMIx_Value_xfer(&dst, &src);
        PMIX_VALUE_DESTRUCT(&dst);

        memset(&isrc, 0, sizeof(isrc));
        memset(&idst, 0, sizeof(idst));
        PMIx_Load_key(isrc.key, "a-key");
        isrc.value.type = types[i];
        (void) PMIx_Info_xfer(&idst, &isrc);
        PMIX_INFO_DESTRUCT(&idst);
    }
    report("transferring a value with no object", 1);
}

static void test_unload(void)
{
    size_t i;

    for (i = 0; i < NTYPES; i++) {
        pmix_value_t v;
        void *data = NULL;
        size_t sz = 0;

        empty_value(&v, types[i]);
        (void) PMIx_Value_unload(&v, &data, &sz);
        if (NULL != data) {
            free(data);
        }
    }
    report("unloading a value with no object", 1);
}

static void test_info_list(void)
{
    size_t i;

    for (i = 0; i < NTYPES; i++) {
        pmix_info_t info;
        void *lst;

        memset(&info, 0, sizeof(info));
        PMIx_Load_key(info.key, "a-key");
        info.value.type = types[i];

        lst = PMIx_Info_list_start();
        if (NULL == lst) {
            continue;
        }
        (void) PMIx_Info_list_xfer(lst, &info);
        PMIx_Info_list_release(lst);
    }
    report("adding a value with no object to an info list", 1);
}

/* The one case where "absent" has a specific required spelling: a value
 * that says PMIX_DATA_ARRAY and carries no array copies to an *empty
 * array*, not to another NULL. Callers - the group code among them -
 * dereference the result. */
static void test_null_darray_copies_to_an_empty_array(void)
{
    pmix_value_t src, dst;
    pmix_status_t rc;
    int ok;

    empty_value(&src, PMIX_DATA_ARRAY);
    memset(&dst, 0, sizeof(dst));

    rc = PMIx_Value_xfer(&dst, &src);
    ok = (PMIX_SUCCESS == rc) && (PMIX_DATA_ARRAY == dst.type)
         && (NULL != dst.data.darray) && (0 == dst.data.darray->size);
    PMIX_VALUE_DESTRUCT(&dst);

    report("a NULL data array copies to an empty array, not to NULL", ok);
}

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

    fprintf(stdout, "\n=== bfrops absent-object unit tests ===\n\n");

    test_print();
    test_compare();
    test_get_size();
    test_xfer();
    test_unload();
    test_info_list();
    test_null_darray_copies_to_an_empty_array();

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);

    PMIx_server_finalize();

    return (nfail > 0) ? 1 : 0;
}
