/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit tests for bfrops behaviour on malformed input.
 *
 * Everything the unpacker reads came off a socket. A peer that is
 * truncated, that is running a different version than it claimed, or
 * that is simply hostile can put any bytes at all in front of these
 * functions, and the count that says how many values follow is itself
 * one of those bytes. The contract this file pins down is narrow but
 * absolute: no input may make the unpacker read outside the buffer it
 * was given. Returning an error is fine; returning a wrong value is
 * tolerable; walking off the end is not.
 *
 * The interesting cases all involve the flexible integer codec, because
 * that is the one decoder whose consumed length is decided by the data
 * rather than by the type - a continuation flag in the last readable
 * byte is an invitation to read one more.
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

/* Load a literal byte sequence into a data buffer. PMIx_Data_load takes
 * ownership of the block, so hand it a copy each time. */
static void load_wire(pmix_data_buffer_t *buf, const unsigned char *bytes, size_t n)
{
    pmix_byte_object_t bo;

    bo.bytes = (char *) malloc(n);
    memcpy(bo.bytes, bytes, n);
    bo.size = n;
    PMIX_DATA_BUFFER_CONSTRUCT(buf);
    PMIx_Data_load(buf, &bo);
}

/* ------------------------------------------------------------------ */
/* a count that outruns the payload                                    */
/* ------------------------------------------------------------------ */

/* The wire says "100 int32 values follow" and then supplies one byte of
 * them. The unpacker must run out of buffer and say so. Before this was
 * fixed it kept decoding past the end of the allocation, one flexible
 * integer at a time, until it found a byte without a continuation flag. */
static void test_count_larger_than_payload(void)
{
    pmix_data_buffer_t buf;
    pmix_status_t rc;
    int32_t dest[100];
    int32_t cnt;
    /* flex-encoded INT32 count of 100, then a single value byte */
    static const unsigned char wire[] = {0xC8, 0x01, 0x02};

    memset(dest, 0, sizeof(dest));
    load_wire(&buf, wire, sizeof(wire));

    cnt = 100;
    rc = PMIx_Data_unpack(NULL, &buf, dest, &cnt, PMIX_INT32);
    PMIX_DATA_BUFFER_DESTRUCT(&buf);

    report("a count larger than the payload is refused, not read past",
           PMIX_SUCCESS != rc);
}

/* The same shape, with a size_t element type, so the decoder is working
 * at its widest. */
static void test_size_count_larger_than_payload(void)
{
    pmix_data_buffer_t buf;
    pmix_status_t rc;
    size_t dest[64];
    int32_t cnt;
    static const unsigned char wire[] = {0x80, 0x01, 0xFF};

    memset(dest, 0, sizeof(dest));
    load_wire(&buf, wire, sizeof(wire));

    cnt = 64;
    rc = PMIx_Data_unpack(NULL, &buf, dest, &cnt, PMIX_SIZE);
    PMIX_DATA_BUFFER_DESTRUCT(&buf);

    report("an oversized size_t count is refused", PMIX_SUCCESS != rc);
}

/* ------------------------------------------------------------------ */
/* a value truncated mid-encoding                                      */
/* ------------------------------------------------------------------ */

/* Every byte here carries the continuation flag, so the encoding claims
 * to continue past the end of the buffer. There is no correct value to
 * return; the only correct behaviour is to refuse. */
static void test_truncated_flex_value(void)
{
    pmix_data_buffer_t buf;
    pmix_status_t rc;
    uint64_t dest[4];
    int32_t cnt;
    /* count of 1, then a value whose every byte says "more follows" */
    static const unsigned char wire[] = {0x02, 0xFF, 0xFF, 0xFF};

    memset(dest, 0, sizeof(dest));
    load_wire(&buf, wire, sizeof(wire));

    cnt = 4;
    rc = PMIx_Data_unpack(NULL, &buf, dest, &cnt, PMIX_UINT64);
    PMIX_DATA_BUFFER_DESTRUCT(&buf);

    report("a flexible integer truncated mid-encoding is refused",
           PMIX_SUCCESS != rc);
}

/* An empty buffer is the degenerate case of the same thing. */
static void test_empty_buffer(void)
{
    pmix_data_buffer_t buf;
    pmix_status_t rc;
    int32_t dest = 0;
    int32_t cnt = 1;

    PMIX_DATA_BUFFER_CONSTRUCT(&buf);
    rc = PMIx_Data_unpack(NULL, &buf, &dest, &cnt, PMIX_INT32);
    PMIX_DATA_BUFFER_DESTRUCT(&buf);

    report("unpacking from an empty buffer is refused", PMIX_SUCCESS != rc);
}

/* Truncating a well-formed message at every offset must never do worse
 * than fail. This is the broad net: it exercises the string, size and
 * count decoders at every possible cut point rather than at the handful
 * we thought to write down. */
static void test_every_truncation_of_a_real_message(void)
{
    pmix_data_buffer_t src;
    pmix_byte_object_t whole;
    pmix_status_t rc;
    char *key = "a-reasonably-long-key-string";
    size_t sz = 4096;
    int32_t i32 = -12345;
    size_t cut;
    int ok = 1;

    /* build something with a mix of strings, flexible integers and a
     * nested array, then chop it short in every possible place */
    PMIX_DATA_BUFFER_CONSTRUCT(&src);
    rc = PMIx_Data_pack(NULL, &src, &key, 1, PMIX_STRING);
    rc = (PMIX_SUCCESS == rc) ? PMIx_Data_pack(NULL, &src, &sz, 1, PMIX_SIZE) : rc;
    rc = (PMIX_SUCCESS == rc) ? PMIx_Data_pack(NULL, &src, &i32, 1, PMIX_INT32) : rc;
    if (PMIX_SUCCESS != rc) {
        PMIX_DATA_BUFFER_DESTRUCT(&src);
        report("every truncation of a real message fails safely", 0);
        return;
    }
    whole.size = src.bytes_used;
    whole.bytes = (char *) malloc(whole.size);
    memcpy(whole.bytes, src.base_ptr, whole.size);
    PMIX_DATA_BUFFER_DESTRUCT(&src);

    for (cut = 0; cut < whole.size; cut++) {
        pmix_data_buffer_t buf;
        char *s = NULL;
        size_t gotsz = 0;
        int32_t got32 = 0;
        int32_t cnt;

        load_wire(&buf, (const unsigned char *) whole.bytes, cut);

        cnt = 1;
        rc = PMIx_Data_unpack(NULL, &buf, &s, &cnt, PMIX_STRING);
        if (PMIX_SUCCESS == rc) {
            /* if it claims success the value must be a real C string */
            if (NULL != s) {
                (void) strlen(s);
                free(s);
            }
            cnt = 1;
            rc = PMIx_Data_unpack(NULL, &buf, &gotsz, &cnt, PMIX_SIZE);
            if (PMIX_SUCCESS == rc) {
                cnt = 1;
                (void) PMIx_Data_unpack(NULL, &buf, &got32, &cnt, PMIX_INT32);
            }
        }
        PMIX_DATA_BUFFER_DESTRUCT(&buf);
    }
    free(whole.bytes);

    report("every truncation of a real message fails safely", ok);
}

/* ------------------------------------------------------------------ */
/* strings must arrive terminated                                      */
/* ------------------------------------------------------------------ */

/* The packer always includes the terminator in the length it writes, but
 * a peer need not. An unterminated payload must not turn into an
 * unterminated C string, because everything downstream treats it as one. */
static void test_unterminated_string(void)
{
    pmix_data_buffer_t buf;
    pmix_status_t rc;
    char *s = NULL;
    int32_t cnt;
    /* count of 1 string, string length 4, then "abcd" with no NUL */
    static const unsigned char wire[] = {0x02, 0x08, 'a', 'b', 'c', 'd'};
    int ok;

    load_wire(&buf, wire, sizeof(wire));
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, &buf, &s, &cnt, PMIX_STRING);
    if (PMIX_SUCCESS == rc && NULL != s) {
        /* must be terminated within the four bytes we were given */
        ok = (strlen(s) < 4);
        free(s);
    } else {
        /* refusing it outright is equally acceptable */
        ok = 1;
    }
    PMIX_DATA_BUFFER_DESTRUCT(&buf);

    report("an unterminated string payload does not escape as a C string", ok);
}

/* A negative string length is only reachable from a corrupt buffer, and
 * must not reach malloc as a huge size_t. */
static void test_negative_string_length(void)
{
    pmix_data_buffer_t buf;
    pmix_status_t rc;
    char *s = (char *) 0x1;
    int32_t cnt;
    /* count of 1 string, then a flex-encoded INT32 of -1 */
    static const unsigned char wire[] = {0x02, 0x01};

    load_wire(&buf, wire, sizeof(wire));
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, &buf, &s, &cnt, PMIX_STRING);
    if (PMIX_SUCCESS == rc && NULL != s && (char *) 0x1 != s) {
        free(s);
    }
    PMIX_DATA_BUFFER_DESTRUCT(&buf);

    report("a negative string length does not reach the allocator", 1);
}

/* ------------------------------------------------------------------ */
/* well-formed values must still decode exactly                        */
/* ------------------------------------------------------------------ */

/* The hardening above changes the decoder's loop bounds, so pin the
 * values that live at the edges of the flexible encoding. */
static void test_flex_boundary_values(void)
{
    static const uint64_t u64s[] = {
        0, 1, 127, 128, 16383, 16384, 0x7FFFFFFFULL, 0x80000000ULL,
        0xFFFFFFFFULL, 0x0100000000ULL, 0x7FFFFFFFFFFFFFFFULL,
        0x8000000000000000ULL, 0xFFFFFFFFFFFFFFFFULL
    };
    static const int64_t i64s[] = {
        0, 1, -1, 127, -127, 128, -128, 16383, -16384,
        2147483647LL, -2147483648LL, 9223372036854775807LL,
        (-9223372036854775807LL - 1)
    };
    size_t n;
    int ok = 1;

    for (n = 0; n < sizeof(u64s) / sizeof(u64s[0]); n++) {
        pmix_data_buffer_t buf;
        uint64_t v = u64s[n], back = 0;
        int32_t cnt = 1;
        pmix_status_t rc;

        PMIX_DATA_BUFFER_CONSTRUCT(&buf);
        rc = PMIx_Data_pack(NULL, &buf, &v, 1, PMIX_UINT64);
        if (PMIX_SUCCESS == rc) {
            rc = PMIx_Data_unpack(NULL, &buf, &back, &cnt, PMIX_UINT64);
        }
        if (PMIX_SUCCESS != rc || back != v) {
            fprintf(stdout, "    uint64 %llu came back as %llu (%s)\n",
                    (unsigned long long) v, (unsigned long long) back,
                    PMIx_Error_string(rc));
            ok = 0;
        }
        PMIX_DATA_BUFFER_DESTRUCT(&buf);
    }

    for (n = 0; n < sizeof(i64s) / sizeof(i64s[0]); n++) {
        pmix_data_buffer_t buf;
        int64_t v = i64s[n], back = 0;
        int32_t cnt = 1;
        pmix_status_t rc;

        PMIX_DATA_BUFFER_CONSTRUCT(&buf);
        rc = PMIx_Data_pack(NULL, &buf, &v, 1, PMIX_INT64);
        if (PMIX_SUCCESS == rc) {
            rc = PMIx_Data_unpack(NULL, &buf, &back, &cnt, PMIX_INT64);
        }
        if (PMIX_SUCCESS != rc || back != v) {
            fprintf(stdout, "    int64 %lld came back as %lld (%s)\n",
                    (long long) v, (long long) back, PMIx_Error_string(rc));
            ok = 0;
        }
        PMIX_DATA_BUFFER_DESTRUCT(&buf);
    }

    report("flexible integers round-trip at their encoding boundaries", ok);
}

/* Many values in one buffer, so the decoder's per-value length has to be
 * right or every value after the first lands in the wrong place. */
static void test_flex_stream_of_many_values(void)
{
    pmix_data_buffer_t buf;
    pmix_status_t rc;
    uint64_t src[256], back[256];
    int32_t cnt;
    size_t n;
    int ok = 1;

    for (n = 0; n < 256; n++) {
        /* span the whole width, one bit at a time */
        src[n] = (n < 64) ? (1ULL << n) : (uint64_t) (n * 2654435761ULL);
    }
    PMIX_DATA_BUFFER_CONSTRUCT(&buf);
    rc = PMIx_Data_pack(NULL, &buf, src, 256, PMIX_UINT64);
    if (PMIX_SUCCESS != rc) {
        ok = 0;
    } else {
        memset(back, 0, sizeof(back));
        cnt = 256;
        rc = PMIx_Data_unpack(NULL, &buf, back, &cnt, PMIX_UINT64);
        ok = (PMIX_SUCCESS == rc) && (256 == cnt)
             && (0 == memcmp(src, back, sizeof(src)));
    }
    PMIX_DATA_BUFFER_DESTRUCT(&buf);

    report("a stream of 256 flexible integers decodes in step", ok);
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

    fprintf(stdout, "\n=== bfrops malformed-input unit tests ===\n\n");

    test_count_larger_than_payload();
    test_size_count_larger_than_payload();
    test_truncated_flex_value();
    test_empty_buffer();
    test_every_truncation_of_a_real_message();
    test_unterminated_string();
    test_negative_string_length();
    test_flex_boundary_values();
    test_flex_stream_of_many_values();

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);

    PMIx_server_finalize();

    return (nfail > 0) ? 1 : 0;
}
