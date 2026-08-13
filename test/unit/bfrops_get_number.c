/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit tests for PMIx_Value_get_number().
 *
 * That function is roughly two thousand lines of hand-written range and
 * precision checks: one check_<type>() per source type, each with one
 * arm per destination type. It is the shape of code where a single
 * copy-pasted line is invisible on review and wrong at run time, and
 * that is exactly what the August 2026 review of src/mca/bfrops found -
 * sign checks reading the wrong union member, arms reading a narrower
 * member than their function's own type, a range constant with an extra
 * digit in it, and stores with no return after them.
 *
 * So this does not test cases. It states the two properties the
 * function has to satisfy and checks them over every (source,
 * destination) pair at every interesting value:
 *
 *   1. If it reports success, the value it wrote must equal the value
 *      it was given. Never a truncation, never a sign flip.
 *   2. If it refuses, the value must genuinely not have been
 *      representable in the destination type.
 *
 * Property 2 is the one that found the missing returns: every
 * conversion to PMIX_PROC_RANK wrote the right answer and then reported
 * PMIX_ERR_BAD_PARAM, which no test that only looked at successful
 * conversions could have seen.
 *
 * There is one deliberate exception, stated in check_rank() in the
 * source: a PMIx structured value (PMIX_PID, PMIX_STATUS,
 * PMIX_PROC_RANK) is not unloaded into another structured type, even
 * though the underlying C types would allow it. That is policy, not a
 * defect, so this test encodes it rather than reporting it.
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

static pmix_data_type_t types[] = {
    PMIX_SIZE, PMIX_INT, PMIX_INT8, PMIX_INT16, PMIX_INT32, PMIX_INT64,
    PMIX_UINT, PMIX_UINT8, PMIX_UINT16, PMIX_UINT32, PMIX_UINT64,
    PMIX_FLOAT, PMIX_DOUBLE, PMIX_PID, PMIX_PROC_RANK, PMIX_STATUS
};
#define NTYPES (sizeof(types) / sizeof(types[0]))

/* the boundaries of every width the function deals in, from both sides */
static long double samples[] = {
    0, 1, -1, 2, -2,
    127, 128, -128, -129,
    255, 256,
    32767, 32768, -32768, -32769,
    65535, 65536,
    2147483647.0L, 2147483648.0L, -2147483648.0L, -2147483649.0L,
    4294967295.0L, 4294967296.0L,
    9223372036854775807.0L, -9223372036854775807.0L,
    18446744073709551615.0L
};
#define NSAMPLES (sizeof(samples) / sizeof(samples[0]))

static int is_structured(pmix_data_type_t t)
{
    return (PMIX_PID == t || PMIX_STATUS == t || PMIX_PROC_RANK == t);
}

/* the representable range of each integral destination type */
static int int_range(pmix_data_type_t t, long double *lo, long double *hi)
{
    switch (t) {
    case PMIX_SIZE:
    case PMIX_UINT64: *lo = 0; *hi = 18446744073709551615.0L; return 1;
    case PMIX_UINT32:
    case PMIX_UINT:
    case PMIX_PROC_RANK: *lo = 0; *hi = 4294967295.0L; return 1;
    case PMIX_UINT16: *lo = 0; *hi = 65535.0L; return 1;
    case PMIX_UINT8: *lo = 0; *hi = 255.0L; return 1;
    case PMIX_INT64: *lo = -9223372036854775807.0L - 1;
                     *hi = 9223372036854775807.0L; return 1;
    case PMIX_INT32:
    case PMIX_INT:
    case PMIX_PID:
    case PMIX_STATUS: *lo = -2147483648.0L; *hi = 2147483647.0L; return 1;
    case PMIX_INT16: *lo = -32768.0L; *hi = 32767.0L; return 1;
    case PMIX_INT8: *lo = -128.0L; *hi = 127.0L; return 1;
    default: return 0;
    }
}

/* Can a value of `t` hold `x` exactly?
 *
 * This is not a convenience - it is what keeps the test defined. C says
 * that converting a floating value to an integer type is UNDEFINED when
 * the value does not fit, and setval() below builds every sample by
 * converting a long double. An earlier version of this file simply let
 * that happen and expected the usual two's-complement wrap: gcc obliged
 * consistently, so the oracle agreed with the stored value and the test
 * passed. clang is equally within its rights to fold the same
 * expression differently at different sites, and does - it reported the
 * sample as 128 while the value held -128, and the test failed with a
 * long list of "reported success but gave" lines that looked like
 * library defects and were not.
 *
 * So: never build an out-of-range value. Skip the pair instead. Nothing
 * is lost, because a deliberately-wrapped sample was never what any of
 * these properties were about. */
static int fits(pmix_data_type_t t, long double x)
{
    long double lo, hi;

    if (PMIX_FLOAT == t || PMIX_DOUBLE == t) {
        return 1;  /* every sample here is well inside both ranges */
    }
    if (!int_range(t, &lo, &hi)) {
        return 0;
    }
    return (x >= lo && x <= hi);
}

static void setval(pmix_value_t *v, pmix_data_type_t t, long double x)
{
    memset(v, 0, sizeof(*v));
    v->type = t;
    switch (t) {
    case PMIX_SIZE: v->data.size = (size_t) x; break;
    case PMIX_INT: v->data.integer = (int) x; break;
    case PMIX_INT8: v->data.int8 = (int8_t) x; break;
    case PMIX_INT16: v->data.int16 = (int16_t) x; break;
    case PMIX_INT32: v->data.int32 = (int32_t) x; break;
    case PMIX_INT64: v->data.int64 = (int64_t) x; break;
    case PMIX_UINT: v->data.uint = (unsigned int) x; break;
    case PMIX_UINT8: v->data.uint8 = (uint8_t) x; break;
    case PMIX_UINT16: v->data.uint16 = (uint16_t) x; break;
    case PMIX_UINT32: v->data.uint32 = (uint32_t) x; break;
    case PMIX_UINT64: v->data.uint64 = (uint64_t) x; break;
    case PMIX_FLOAT: v->data.fval = (float) x; break;
    case PMIX_DOUBLE: v->data.dval = (double) x; break;
    case PMIX_PID: v->data.pid = (pid_t) x; break;
    case PMIX_PROC_RANK: v->data.rank = (pmix_rank_t) x; break;
    case PMIX_STATUS: v->data.status = (pmix_status_t) x; break;
    default: break;
    }
}

static long double getval(const pmix_value_t *v)
{
    switch (v->type) {
    case PMIX_SIZE: return (long double) v->data.size;
    case PMIX_INT: return (long double) v->data.integer;
    case PMIX_INT8: return (long double) v->data.int8;
    case PMIX_INT16: return (long double) v->data.int16;
    case PMIX_INT32: return (long double) v->data.int32;
    case PMIX_INT64: return (long double) v->data.int64;
    case PMIX_UINT: return (long double) v->data.uint;
    case PMIX_UINT8: return (long double) v->data.uint8;
    case PMIX_UINT16: return (long double) v->data.uint16;
    case PMIX_UINT32: return (long double) v->data.uint32;
    case PMIX_UINT64: return (long double) v->data.uint64;
    case PMIX_FLOAT: return (long double) v->data.fval;
    case PMIX_DOUBLE: return (long double) v->data.dval;
    case PMIX_PID: return (long double) v->data.pid;
    case PMIX_PROC_RANK: return (long double) v->data.rank;
    case PMIX_STATUS: return (long double) v->data.status;
    default: return 0;
    }
}

/* ------------------------------------------------------------------ */

static void test_success_means_exact(void)
{
    size_t s, d, k;
    int ok = 1;
    int reported = 0;

    for (s = 0; s < NTYPES; s++) {
        for (k = 0; k < NSAMPLES; k++) {
            pmix_value_t v;
            long double exact;

            if (!fits(types[s], samples[k])) {
                continue;
            }
            setval(&v, types[s], samples[k]);
            exact = getval(&v);

            for (d = 0; d < NTYPES; d++) {
                pmix_value_t out;
                pmix_status_t rc;
                unsigned char buf[32];

                /* a float or double at either end legitimately rounds */
                if (PMIX_FLOAT == types[d] || PMIX_DOUBLE == types[d]
                    || PMIX_FLOAT == types[s] || PMIX_DOUBLE == types[s]) {
                    continue;
                }
                memset(buf, 0xAB, sizeof(buf));
                rc = PMIx_Value_get_number(&v, buf, types[d]);
                if (PMIX_SUCCESS != rc) {
                    continue;
                }
                memset(&out, 0, sizeof(out));
                out.type = types[d];
                memcpy(&out.data, buf, sizeof(out.data) < 16 ? sizeof(out.data) : 16);
                if (getval(&out) != exact) {
                    if (reported < 10) {
                        fprintf(stdout, "    %s(%.0Lf) -> %s reported success but gave %.0Lf\n",
                                PMIx_Data_type_string(types[s]), exact,
                                PMIx_Data_type_string(types[d]), getval(&out));
                        reported++;
                    }
                    ok = 0;
                }
            }
        }
    }
    report("a successful conversion returns the value it was given", ok);
}

static void test_refusal_means_unrepresentable(void)
{
    size_t s, d, k;
    int ok = 1;
    int reported = 0;

    for (s = 0; s < NTYPES; s++) {
        for (k = 0; k < NSAMPLES; k++) {
            pmix_value_t v;
            long double exact;

            if (!fits(types[s], samples[k])) {
                continue;
            }
            setval(&v, types[s], samples[k]);
            exact = getval(&v);

            for (d = 0; d < NTYPES; d++) {
                pmix_status_t rc;
                unsigned char buf[32];
                long double lo, hi;

                if (types[s] == types[d]) {
                    continue;
                }
                /* stated policy in check_rank(): a structured value is
                 * not unloaded into another structured type */
                if (is_structured(types[s]) && is_structured(types[d])) {
                    continue;
                }
                if (PMIX_FLOAT == types[d] || PMIX_DOUBLE == types[d]
                    || PMIX_FLOAT == types[s] || PMIX_DOUBLE == types[s]) {
                    continue;
                }
                if (!int_range(types[d], &lo, &hi)) {
                    continue;
                }
                /* long double is not always wider than 64 bits, so at
                 * the very top of the int64 range it cannot tell
                 * 2^63-1 from 2^63 and the oracle stops being one */
                if (exact >= 9223372036854775807.0L && PMIX_INT64 == types[d]) {
                    continue;
                }
                if (exact < lo || exact > hi) {
                    continue; /* refusing is correct */
                }
                memset(buf, 0xAB, sizeof(buf));
                rc = PMIx_Value_get_number(&v, buf, types[d]);
                if (PMIX_SUCCESS != rc) {
                    if (reported < 10) {
                        fprintf(stdout, "    %s(%.0Lf) -> %s refused (%s) but is representable\n",
                                PMIx_Data_type_string(types[s]), exact,
                                PMIx_Data_type_string(types[d]), PMIx_Error_string(rc));
                        reported++;
                    }
                    ok = 0;
                }
            }
        }
    }
    report("a representable conversion is not refused", ok);
}

/* Every source type must at least be recognized. PMIX_PID was not: it
 * had no check_ function, so every conversion out of a pid returned
 * PMIX_ERR_BAD_PARAM. */
static void test_every_source_type_is_handled(void)
{
    size_t s;
    int ok = 1;

    for (s = 0; s < NTYPES; s++) {
        pmix_value_t v;
        int64_t out = 0;
        pmix_status_t rc;

        setval(&v, types[s], 42);
        rc = PMIx_Value_get_number(&v, &out, PMIX_INT64);
        if (PMIX_SUCCESS != rc || 42 != out) {
            fprintf(stdout, "    %s(42) -> PMIX_INT64: %s (got %lld)\n",
                    PMIx_Data_type_string(types[s]), PMIx_Error_string(rc),
                    (long long) out);
            ok = 0;
        }
    }
    report("every numeric source type is handled", ok);
}

/* The sign checks are the part that has been wrong most often, and they
 * are wrong in the direction that does not fail loudly, so state them
 * separately. */
static void test_negative_into_unsigned_is_refused(void)
{
    static const pmix_data_type_t signed_src[] = {
        PMIX_INT, PMIX_INT8, PMIX_INT16, PMIX_INT32, PMIX_INT64,
        PMIX_PID, PMIX_STATUS
    };
    static const pmix_data_type_t unsigned_dst[] = {
        PMIX_SIZE, PMIX_UINT, PMIX_UINT8, PMIX_UINT16, PMIX_UINT32,
        PMIX_UINT64
    };
    size_t s, d;
    int ok = 1;

    for (s = 0; s < sizeof(signed_src) / sizeof(signed_src[0]); s++) {
        /* -1 exercises the low bits; the second value has zeroes in the
         * low word, which is what made a sign check that read the wrong
         * union member look like it worked */
        long double vals[2] = {-1, -4294967296.0L};
        size_t n;

        for (n = 0; n < 2; n++) {
            pmix_value_t v;

            if (!fits(signed_src[s], vals[n])) {
                continue;
            }
            setval(&v, signed_src[s], vals[n]);
            for (d = 0; d < sizeof(unsigned_dst) / sizeof(unsigned_dst[0]); d++) {
                uint64_t out = 0;
                pmix_status_t rc = PMIx_Value_get_number(&v, &out, unsigned_dst[d]);

                if (PMIX_SUCCESS == rc) {
                    fprintf(stdout, "    %s(%.0Lf) -> %s accepted, gave %llu\n",
                            PMIx_Data_type_string(signed_src[s]), getval(&v),
                            PMIx_Data_type_string(unsigned_dst[d]),
                            (unsigned long long) out);
                    ok = 0;
                }
            }
        }
    }
    report("a negative value is refused by every unsigned destination", ok);
}

/* Neither argument was screened, and both are dereferenced unconditionally:
 * the value for its type tag, the destination by whichever arm matches. A
 * NULL value is the one that actually arrives - callers pass the "value"
 * member of a pmix_kval_t, which is a pointer that can legitimately be NULL.
 * Against the unfixed library this case segfaults rather than failing. */
static void test_null_arguments_are_refused(void)
{
    pmix_value_t v;
    int64_t out = 0;
    int ok = 1;

    setval(&v, PMIX_INT32, 42);

    if (PMIX_ERR_BAD_PARAM != PMIx_Value_get_number(NULL, &out, PMIX_INT64)) {
        fprintf(stdout, "    a NULL value was not refused\n");
        ok = 0;
    }
    if (PMIX_ERR_BAD_PARAM != PMIx_Value_get_number(&v, NULL, PMIX_INT64)) {
        fprintf(stdout, "    a NULL destination was not refused\n");
        ok = 0;
    }
    if (PMIX_ERR_BAD_PARAM != PMIx_Value_get_number(NULL, NULL, PMIX_INT64)) {
        fprintf(stdout, "    two NULLs were not refused\n");
        ok = 0;
    }
    report("a NULL value or destination is refused", ok);
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

    fprintf(stdout, "\n=== PMIx_Value_get_number unit tests ===\n\n");

    test_null_arguments_are_refused();
    test_every_source_type_is_handled();
    test_success_means_exact();
    test_refusal_means_unrepresentable();
    test_negative_into_unsigned_is_refused();

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);

    PMIx_server_finalize();

    return (nfail > 0) ? 1 : 0;
}
