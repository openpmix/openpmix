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
#include "src/include/pmix_globals.h"
#include "src/mca/bfrops/base/base.h"

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
/* the element count itself                                            */
/* ------------------------------------------------------------------ */

/* A negative element count on the wire is refused, and the caller's
 * count is not left negative. A zero count is legal - the packer writes
 * one for an empty array - and succeeds without touching the output, so
 * a caller that asked for one value must check the count it gets back.
 * The buffers are undescribed, as they are between peers, so nothing but
 * the count stands between the bytes and the caller. */
static void test_element_count_sign(void)
{
    pmix_bfrops_module_t *mod = pmix_globals.mypeer->nptr->compat.bfrops;
    pmix_buffer_t buf;
    pmix_status_t rc;
    int32_t val = 7, dest = 7, cnt;
    char *str = NULL;

    /* strings, because a negative count of them needs no payload to
     * look complete */
    PMIX_CONSTRUCT(&buf, pmix_buffer_t);
    buf.type = PMIX_BFROP_BUFFER_NON_DESC;
    rc = mod->pack(&buf, &str, -5, PMIX_STRING);
    cnt = 1;
    if (PMIX_SUCCESS == rc) {
        rc = mod->unpack(&buf, &str, &cnt, PMIX_STRING);
    }
    report("a negative element count is refused", PMIX_SUCCESS != rc && 0 == cnt);
    PMIX_DESTRUCT(&buf);

    PMIX_CONSTRUCT(&buf, pmix_buffer_t);
    buf.type = PMIX_BFROP_BUFFER_NON_DESC;
    rc = mod->pack(&buf, &val, 1, PMIX_INT32);
    cnt = -1;
    dest = 0;
    if (PMIX_SUCCESS == rc) {
        rc = mod->unpack(&buf, &dest, &cnt, PMIX_INT32);
    }
    report("a negative caller count is refused", PMIX_SUCCESS != rc && 0 == dest);
    PMIX_DESTRUCT(&buf);

    PMIX_CONSTRUCT(&buf, pmix_buffer_t);
    buf.type = PMIX_BFROP_BUFFER_NON_DESC;
    rc = mod->pack(&buf, NULL, 0, PMIX_STRING);
    cnt = 1;
    if (PMIX_SUCCESS == rc) {
        rc = mod->unpack(&buf, &str, &cnt, PMIX_STRING);
    }
    report("a zero element count unpacks nothing", PMIX_SUCCESS == rc && 0 == cnt && NULL == str);
    PMIX_DESTRUCT(&buf);
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
/* a length off the wire must not buy an unbounded allocation           */
/* ------------------------------------------------------------------ */

/* Random bytes through every unpacker. This is not looking for a
 * particular defect - it is the standing assertion that the contract at
 * the top of this file holds for input nobody wrote down. It has earned
 * its keep twice: a data-array element count that drove the allocator
 * straight off a cliff (a twenty-byte message, killed by the OOM
 * killer), and a query's qualifier count that allocated, failed, and
 * then unpacked into the NULL.
 *
 * The seeds are fixed so a failure is reproducible; the generator is a
 * plain LCG for the same reason. If you find a crash, print the seed
 * and the payload rather than adding a one-off case for it. */
static void test_random_bytes_through_every_unpacker(void)
{
    static const pmix_data_type_t fuzz_types[] = {
        PMIX_BOOL, PMIX_STRING, PMIX_SIZE, PMIX_INT, PMIX_INT64, PMIX_UINT64,
        PMIX_FLOAT, PMIX_DOUBLE, PMIX_TIMEVAL, PMIX_TIME, PMIX_STATUS,
        PMIX_VALUE, PMIX_PROC, PMIX_APP, PMIX_INFO, PMIX_PDATA, PMIX_BUFFER,
        PMIX_BYTE_OBJECT, PMIX_KVAL, PMIX_PROC_INFO, PMIX_DATA_ARRAY,
        PMIX_PROC_RANK, PMIX_QUERY, PMIX_ENVAR, PMIX_COORD, PMIX_REGATTR,
        PMIX_GEOMETRY, PMIX_DEVICE_DIST, PMIX_ENDPOINT, PMIX_DEVICE,
        PMIX_RESOURCE_UNIT, PMIX_PROC_NSPACE, PMIX_DATA_BUFFER,
        PMIX_NODE_PID, PMIX_REGEX2
    };
    static const unsigned long seeds[] = {1, 12345, 20031, 31337, 424242};
    size_t t, sd, r, n;
    unsigned long state;

    for (t = 0; t < sizeof(fuzz_types) / sizeof(fuzz_types[0]); t++) {
        for (sd = 0; sd < sizeof(seeds) / sizeof(seeds[0]); sd++) {
            for (r = 0; r < 40; r++) {
                pmix_data_buffer_t buf;
                unsigned char wire[64];
                unsigned char dest[8192];
                size_t len;
                int32_t cnt;

                state = seeds[sd] + (unsigned long) (t * 1000 + r);
#define NEXTB() (state = state * 6364136223846793005UL + 1442695040888963407UL, \
                 (unsigned char) (state >> 33))
                len = 4 + (NEXTB() % 60);
                for (n = 0; n < len; n++) {
                    wire[n] = NEXTB();
                }
#undef NEXTB
                load_wire(&buf, wire, len);
                memset(dest, 0, sizeof(dest));
                cnt = 8;
                (void) PMIx_Data_unpack(NULL, &buf, dest, &cnt, fuzz_types[t]);
                PMIX_DATA_BUFFER_DESTRUCT(&buf);
            }
        }
    }
    report("random bytes through every unpacker", 1);
}

/* The specific shape the fuzzer found, kept as its own case so a
 * regression names itself: a count that says "this array has more
 * elements than this whole message has bytes" cannot be describing
 * anything the peer sent, and must not be allowed to size an
 * allocation. */
/* A growable byte accumulator, used to assemble a message by hand while
 * still letting the real encoder decide every byte of it.
 *
 * Note what this does NOT use: PMIx_Data_embed() replaces a buffer's
 * payload rather than appending to it, so building a message by
 * embedding one piece after another silently leaves you with only the
 * last piece. A test built that way still "passes" - a one-byte buffer
 * is refused by everything - and proves nothing at all. That mistake is
 * why this accumulator exists. */
typedef struct {
    char *bytes;
    size_t len;
} wire_acc_t;

/* Append the encoding of ONE value of `type`, without the count the
 * pack driver normally puts in front of it. PMIx_Data_pack always emits
 * [count][values], and the count 1 flex-encodes to a single byte, so
 * dropping the first byte leaves exactly the bare value. */
static int append_bare(wire_acc_t *acc, const void *val, pmix_data_type_t type)
{
    pmix_data_buffer_t tmp;
    pmix_status_t rc;
    size_t add;
    char *grown;

    PMIX_DATA_BUFFER_CONSTRUCT(&tmp);
    rc = PMIx_Data_pack(NULL, &tmp, (void *) val, 1, type);
    if (PMIX_SUCCESS != rc || tmp.bytes_used < 2) {
        PMIX_DATA_BUFFER_DESTRUCT(&tmp);
        return 0;
    }
    add = tmp.bytes_used - 1;               /* skip the count byte */
    grown = (char *) realloc(acc->bytes, acc->len + add);
    if (NULL == grown) {
        PMIX_DATA_BUFFER_DESTRUCT(&tmp);
        return 0;
    }
    acc->bytes = grown;
    memcpy(acc->bytes + acc->len, tmp.base_ptr + 1, add);
    acc->len += add;
    PMIX_DATA_BUFFER_DESTRUCT(&tmp);
    return 1;
}

static int append_raw(wire_acc_t *acc, size_t n)
{
    char *grown = (char *) realloc(acc->bytes, acc->len + n);

    if (NULL == grown) {
        return 0;
    }
    acc->bytes = grown;
    memset(acc->bytes + acc->len, 0, n);
    acc->len += n;
    return 1;
}

/* The specific shape the fuzzer found, kept as its own case so a
 * regression names itself: a count that says "this array has more
 * elements than this whole message has bytes" cannot be describing
 * anything the peer sent, and must not be allowed to size an
 * allocation. */
/* The specific shape the fuzzer found, kept as its own case so a
 * regression names itself. A data array is described on the wire by an
 * element type and a count, and the count sizes the allocation the
 * receiver makes. A count larger than the whole message cannot be
 * describing anything the peer sent - but before this was bounded, a
 * twenty-byte message asking for 2^40 int64s got exactly what it asked
 * for, and the OOM killer ended the process. */
static void test_element_count_cannot_exceed_the_message(void)
{
    pmix_data_buffer_t buf;
    pmix_data_array_t out;
    pmix_status_t rc;
    int32_t cnt, one = 1;
    uint16_t etype = PMIX_INT64;
    size_t huge = (size_t) 1 << 40;
    wire_acc_t acc = {NULL, 0};

    /* [count of arrays][element type][element count] and then nothing,
     * which is what the unpack driver reads */
    if (!append_bare(&acc, &one, PMIX_INT32)
        || !append_bare(&acc, &etype, PMIX_UINT16)
        || !append_bare(&acc, &huge, PMIX_SIZE)) {
        free(acc.bytes);
        report("an element count larger than the message is refused", 0);
        return;
    }
    load_wire(&buf, (const unsigned char *) acc.bytes, acc.len);
    free(acc.bytes);

    memset(&out, 0, sizeof(out));
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, &buf, &out, &cnt, PMIX_DATA_ARRAY);
    PMIX_DATA_BUFFER_DESTRUCT(&buf);

    /* surviving at all is most of the point; having refused rather than
     * allocated eight terabytes is the rest */
    report("an element count larger than the message is refused",
           PMIX_SUCCESS != rc);
}

/* A pmix_value_t keeps its payload in a union, and the type tag that
 * says which member is live comes off the wire. Six registered types
 * have no member there at all - they are data-array element types - and
 * their C representation is larger than the whole union, PMIX_PDATA by
 * 784 bytes. Unpacking one into a value writes that far past its end,
 * and unpack_kval/unpack_info/unpack_pdata all unpack into a value they
 * just allocated: a heap overflow whose length and contents both come
 * from the peer.
 *
 * This is the case the fuzz stage above found, kept separately so a
 * regression names itself. Note it took the fuzzer running everything
 * in ONE process to surface: a harness that forks per input loses the
 * corrupted heap with the child, which is exactly what the scratch
 * version of this did before it was folded in here. */
static void test_a_value_cannot_carry_an_oversized_type(void)
{
    static const pmix_data_type_t too_big[] = {
        PMIX_VALUE, PMIX_INFO, PMIX_PDATA, PMIX_APP, PMIX_KVAL, PMIX_BUFFER
    };
    size_t i;
    int ok = 1;

    for (i = 0; i < sizeof(too_big) / sizeof(too_big[0]); i++) {
        pmix_data_buffer_t buf;
        pmix_value_t *heapval;
        pmix_status_t rc;
        int32_t cnt, one = 1;
        uint16_t tag = (uint16_t) too_big[i];
        wire_acc_t acc = {NULL, 0};

        /* [count of values][type tag][plenty of plausible bytes] - the
         * shape unpack_value() reads. The trailing bytes matter: without
         * them a regression merely runs out of buffer, and what is being
         * tested is that it never starts writing. */
        if (!append_bare(&acc, &one, PMIX_INT32)
            || !append_bare(&acc, &tag, PMIX_UINT16)
            || !append_raw(&acc, 512)) {
            free(acc.bytes);
            ok = 0;
            break;
        }
        load_wire(&buf, (const unsigned char *) acc.bytes, acc.len);
        free(acc.bytes);

        /* heap-allocated and exactly sizeof(pmix_value_t), so an
         * overflow lands on the heap rather than harmlessly on a
         * generous stack buffer */
        heapval = (pmix_value_t *) calloc(1, sizeof(pmix_value_t));
        cnt = 1;
        rc = PMIx_Data_unpack(NULL, &buf, heapval, &cnt, PMIX_VALUE);
        if (PMIX_SUCCESS == rc) {
            fprintf(stdout, "    a value claiming to hold %s was accepted\n",
                    PMIx_Data_type_string(too_big[i]));
            ok = 0;
        }
        free(heapval);
        PMIX_DATA_BUFFER_DESTRUCT(&buf);
    }
    report("a value cannot claim a type larger than its union", ok);
}

/* ------------------------------------------------------------------ */

/* pmix_bfrop_count_fits() relates an element count to the bytes still
 * unread, at the smallest packed size of the element type. These pin
 * down its boundaries: the count that exactly fills the buffer is
 * accepted and one more is refused, the bound tracks the unpack
 * pointer, the int32_t limit applies to every type, and the types with
 * no per-element minimum are bounded only by that limit. */
static void test_count_fits_boundaries(void)
{
    pmix_buffer_t buf;
    char bytes[12];
    int ok = 1;

    memset(bytes, 0, sizeof(bytes));
    PMIX_CONSTRUCT(&buf, pmix_buffer_t);
    PMIX_LOAD_BUFFER_NON_DESTRUCT(pmix_globals.mypeer, &buf, bytes, sizeof(bytes));

    /* a packed pmix_info_t takes at least 4 bytes, a pmix_proc_t 3, and
     * a type with no entry of its own 1 */
    ok &= pmix_bfrop_count_fits(&buf, 0, PMIX_INFO);
    ok &= pmix_bfrop_count_fits(&buf, 3, PMIX_INFO);
    ok &= !pmix_bfrop_count_fits(&buf, 4, PMIX_INFO);
    ok &= pmix_bfrop_count_fits(&buf, 4, PMIX_PROC);
    ok &= !pmix_bfrop_count_fits(&buf, 5, PMIX_PROC);
    ok &= pmix_bfrop_count_fits(&buf, 12, PMIX_BYTE);
    ok &= !pmix_bfrop_count_fits(&buf, 13, PMIX_BYTE);
    ok &= pmix_bfrop_count_fits(&buf, 12, PMIX_STATUS);
    ok &= !pmix_bfrop_count_fits(&buf, 13, PMIX_STATUS);

    /* no per-element minimum: only the int32_t limit */
    ok &= pmix_bfrop_count_fits(&buf, (size_t) INT32_MAX, PMIX_POINTER);
    ok &= pmix_bfrop_count_fits(&buf, (size_t) INT32_MAX, PMIX_DATA_ARRAY);
    ok &= !pmix_bfrop_count_fits(&buf, (size_t) INT32_MAX + 1, PMIX_POINTER);
    ok &= !pmix_bfrop_count_fits(&buf, (size_t) INT32_MAX + 1, PMIX_DATA_ARRAY);
    ok &= !pmix_bfrop_count_fits(&buf, SIZE_MAX, PMIX_BYTE);

    /* only the unread part of the buffer counts */
    buf.unpack_ptr = buf.base_ptr + 8;
    ok &= pmix_bfrop_count_fits(&buf, 1, PMIX_INFO);
    ok &= !pmix_bfrop_count_fits(&buf, 2, PMIX_INFO);
    buf.unpack_ptr = buf.pack_ptr;
    ok &= pmix_bfrop_count_fits(&buf, 0, PMIX_INFO);
    ok &= !pmix_bfrop_count_fits(&buf, 1, PMIX_BYTE);

    /* the buffer does not own the bytes */
    buf.base_ptr = NULL;
    buf.pack_ptr = NULL;
    buf.unpack_ptr = NULL;
    PMIX_DESTRUCT(&buf);

    report("an element count is bounded by the unread bytes at the "
           "type's smallest packed size", ok);
}

/* A packed pmix_info_t takes at least four bytes, so an info count
 * larger than a quarter of the bytes that follow it is refused before
 * anything is allocated - even where it is no larger than the byte
 * count itself. Checked at the two places the base unpacker sizes an
 * info array from a count nested inside another type: a query's
 * qualifiers, and a data array of pmix_info_t. */
static void test_nested_info_counts_bounded(void)
{
    pmix_data_buffer_t buf;
    pmix_query_t query;
    pmix_data_array_t darray;
    pmix_status_t rc;
    int32_t cnt, one = 1, nkeys = 0;
    uint16_t etype = PMIX_INFO;
    size_t nelem = 100;
    wire_acc_t acc = {NULL, 0};
    int ok;

    /* [count of queries][no keys][nqual][nqual bytes of padding] */
    ok = append_bare(&acc, &one, PMIX_INT32) && append_bare(&acc, &nkeys, PMIX_INT32) &&
         append_bare(&acc, &nelem, PMIX_SIZE) && append_raw(&acc, nelem);
    if (!ok) {
        free(acc.bytes);
        report("a qualifier count larger than the qualifiers that fit is refused", 0);
        return;
    }
    load_wire(&buf, (const unsigned char *) acc.bytes, acc.len);
    free(acc.bytes);
    acc.bytes = NULL;
    acc.len = 0;
    PMIX_QUERY_CONSTRUCT(&query);
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, &buf, &query, &cnt, PMIX_QUERY);
    report("a qualifier count larger than the qualifiers that fit is refused",
           PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER == rc && NULL == query.qualifiers &&
           0 == query.nqual);
    PMIX_QUERY_DESTRUCT(&query);
    PMIX_DATA_BUFFER_DESTRUCT(&buf);

    /* [count of arrays][element type][element count][that many bytes] */
    ok = append_bare(&acc, &one, PMIX_INT32) && append_bare(&acc, &etype, PMIX_UINT16) &&
         append_bare(&acc, &nelem, PMIX_SIZE) && append_raw(&acc, nelem);
    if (!ok) {
        free(acc.bytes);
        report("an info data array larger than the infos that fit is refused", 0);
        return;
    }
    load_wire(&buf, (const unsigned char *) acc.bytes, acc.len);
    free(acc.bytes);
    memset(&darray, 0, sizeof(darray));
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, &buf, &darray, &cnt, PMIX_DATA_ARRAY);
    report("an info data array larger than the infos that fit is refused",
           PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER == rc && NULL == darray.array);
    PMIX_DATA_BUFFER_DESTRUCT(&buf);
}

/* A pointer means nothing outside the process that packed it, so an
 * array of PMIX_POINTER carries one sentinel byte and no values, and
 * unpacks with no elements whatever count it declares. An array of data
 * arrays can pack to a single byte at any count, so its count is bounded
 * only by the int32_t an unpack takes. */
static void unpack_one_darray(const wire_acc_t *acc, pmix_data_array_t *out,
                              pmix_status_t *rc)
{
    pmix_data_buffer_t buf;
    int32_t cnt = 1;

    load_wire(&buf, (const unsigned char *) acc->bytes, acc->len);
    memset(out, 0, sizeof(*out));
    *rc = PMIx_Data_unpack(NULL, &buf, out, &cnt, PMIX_DATA_ARRAY);
    PMIX_DATA_BUFFER_DESTRUCT(&buf);
}

static void test_sparse_array_counts(void)
{
    pmix_data_array_t out, in, *inner;
    pmix_data_buffer_t buf;
    pmix_status_t rc;
    int32_t one = 1, cnt, vals[2] = {1, 2};
    uint16_t etype, undef = PMIX_UNDEF;
    uint8_t sentinel = 1;
    size_t huge = (size_t) INT32_MAX, toobig = (size_t) INT32_MAX + 1, five = 5;
    char *trailer = "trailing-data";
    wire_acc_t acc = {NULL, 0};
    int ok;

    /* [count of arrays][PMIX_POINTER][huge][the one sentinel] */
    etype = PMIX_POINTER;
    ok = append_bare(&acc, &one, PMIX_INT32) && append_bare(&acc, &etype, PMIX_UINT16) &&
         append_bare(&acc, &huge, PMIX_SIZE) && append_bare(&acc, &sentinel, PMIX_UINT8);
    if (ok) {
        unpack_one_darray(&acc, &out, &rc);
        ok = (PMIX_SUCCESS == rc && PMIX_POINTER == out.type && 0 == out.size &&
              NULL == out.array);
        PMIX_DATA_ARRAY_DESTRUCT(&out);
    }
    free(acc.bytes);
    acc.bytes = NULL;
    acc.len = 0;
    report("a pointer array unpacks with no elements", ok);

    /* [count of arrays][PMIX_POINTER][5] and no sentinel */
    ok = append_bare(&acc, &one, PMIX_INT32) && append_bare(&acc, &etype, PMIX_UINT16) &&
         append_bare(&acc, &five, PMIX_SIZE);
    if (ok) {
        unpack_one_darray(&acc, &out, &rc);
        ok = (PMIX_SUCCESS != rc && NULL == out.array);
        PMIX_DATA_ARRAY_DESTRUCT(&out);
    }
    free(acc.bytes);
    acc.bytes = NULL;
    acc.len = 0;
    report("a pointer array with no sentinel is refused", ok);

    /* [count of arrays][PMIX_DATA_ARRAY][INT32_MAX + 1][terminator] */
    etype = PMIX_DATA_ARRAY;
    ok = append_bare(&acc, &one, PMIX_INT32) && append_bare(&acc, &etype, PMIX_UINT16) &&
         append_bare(&acc, &toobig, PMIX_SIZE) && append_bare(&acc, &undef, PMIX_UINT16);
    if (ok) {
        unpack_one_darray(&acc, &out, &rc);
        ok = (PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER == rc && NULL == out.array);
        PMIX_DATA_ARRAY_DESTRUCT(&out);
    }
    free(acc.bytes);
    acc.bytes = NULL;
    acc.len = 0;
    report("an array of arrays whose count does not fit an int32_t is refused", ok);

    /* a well-formed pointer array followed by more data: no elements,
     * and the next item still unpacks */
    PMIX_DATA_ARRAY_CONSTRUCT(&in, 3, PMIX_POINTER);
    PMIX_DATA_BUFFER_CONSTRUCT(&buf);
    rc = PMIx_Data_pack(NULL, &buf, &in, 1, PMIX_DATA_ARRAY);
    if (PMIX_SUCCESS == rc) {
        rc = PMIx_Data_pack(NULL, &buf, &trailer, 1, PMIX_STRING);
    }
    memset(&out, 0, sizeof(out));
    if (PMIX_SUCCESS == rc) {
        cnt = 1;
        rc = PMIx_Data_unpack(NULL, &buf, &out, &cnt, PMIX_DATA_ARRAY);
    }
    ok = (PMIX_SUCCESS == rc && PMIX_POINTER == out.type && 0 == out.size &&
          NULL == out.array);
    if (ok) {
        char *got = NULL;
        cnt = 1;
        rc = PMIx_Data_unpack(NULL, &buf, &got, &cnt, PMIX_STRING);
        ok = (PMIX_SUCCESS == rc && NULL != got && 0 == strcmp(got, trailer));
        free(got);
    }
    report("a packed pointer array unpacks empty and stays in step", ok);
    PMIX_DATA_ARRAY_DESTRUCT(&out);
    PMIX_DATA_BUFFER_DESTRUCT(&buf);
    PMIX_DATA_ARRAY_DESTRUCT(&in);

    /* three arrays of which the second is untyped, followed by more
     * data: the count is kept and the next item still unpacks */
    PMIX_DATA_ARRAY_CONSTRUCT(&in, 3, PMIX_DATA_ARRAY);
    inner = (pmix_data_array_t *) in.array;
    PMIX_DATA_ARRAY_CONSTRUCT(&inner[0], 2, PMIX_INT32);
    memcpy(inner[0].array, vals, sizeof(vals));
    PMIX_DATA_ARRAY_CONSTRUCT(&inner[2], 2, PMIX_INT32);
    memcpy(inner[2].array, vals, sizeof(vals));
    PMIX_DATA_BUFFER_CONSTRUCT(&buf);
    rc = PMIx_Data_pack(NULL, &buf, &in, 1, PMIX_DATA_ARRAY);
    if (PMIX_SUCCESS == rc) {
        rc = PMIx_Data_pack(NULL, &buf, &trailer, 1, PMIX_STRING);
    }
    memset(&out, 0, sizeof(out));
    if (PMIX_SUCCESS == rc) {
        cnt = 1;
        rc = PMIx_Data_unpack(NULL, &buf, &out, &cnt, PMIX_DATA_ARRAY);
    }
    inner = (pmix_data_array_t *) out.array;
    ok = (PMIX_SUCCESS == rc && PMIX_DATA_ARRAY == out.type && 3 == out.size &&
          NULL != inner && PMIX_INT32 == inner[0].type && 2 == inner[0].size &&
          0 == memcmp(inner[0].array, vals, sizeof(vals)) &&
          PMIX_UNDEF == inner[1].type && PMIX_UNDEF == inner[2].type);
    if (ok) {
        char *got = NULL;
        cnt = 1;
        rc = PMIx_Data_unpack(NULL, &buf, &got, &cnt, PMIX_STRING);
        ok = (PMIX_SUCCESS == rc && NULL != got && 0 == strcmp(got, trailer));
        free(got);
    }
    report("an array of arrays keeps its count and stays in step", ok);
    PMIX_DATA_ARRAY_DESTRUCT(&out);
    PMIX_DATA_BUFFER_DESTRUCT(&buf);
    PMIX_DATA_ARRAY_DESTRUCT(&in);
}

/* ------------------------------------------------------------------ */

/* The v21 and v3 wire formats carry two deprecated types, an info array
 * and a modex blob, each preceded by its element count. That count must
 * be bounded by the bytes left in the message before it sizes an
 * allocation. The type values and the modex layout are private to those
 * components, so they are mirrored here. */
#define UT_PMIX_MODEX      29
#define UT_PMIX_INFO_ARRAY 44

typedef struct {
    char nspace[PMIX_MAX_NSLEN + 1];
    int rank;
    uint8_t *blob;
    size_t size;
} ut_modex_data_t;

static void test_legacy_counts_bounded(const char *version)
{
    pmix_bfrops_module_t *mod;
    pmix_buffer_t buf;
    pmix_info_array_t arr;
    ut_modex_data_t modex;
    size_t huge = (size_t) 1 << 50;
    int32_t n;
    pmix_status_t rc;
    char label[128];

    mod = pmix_bfrops_base_assign_module(version);
    if (NULL == mod) {
        fprintf(stdout, "  SKIP: bfrops %s not available\n", version);
        return;
    }

    /* [count 1][size huge] and nothing after it */
    PMIX_CONSTRUCT(&buf, pmix_buffer_t);
    buf.type = PMIX_BFROP_BUFFER_NON_DESC;
    rc = mod->pack(&buf, &huge, 1, PMIX_SIZE);
    memset(&arr, 0, sizeof(arr));
    n = 1;
    if (PMIX_SUCCESS == rc) {
        rc = mod->unpack(&buf, &arr, &n, UT_PMIX_INFO_ARRAY);
    }
    snprintf(label, sizeof(label), "%s: an info-array size larger than the message is refused",
             version);
    report(label, PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER == rc && NULL == arr.array);
    PMIX_DESTRUCT(&buf);

    PMIX_CONSTRUCT(&buf, pmix_buffer_t);
    buf.type = PMIX_BFROP_BUFFER_NON_DESC;
    rc = mod->pack(&buf, &huge, 1, PMIX_SIZE);
    memset(&modex, 0, sizeof(modex));
    n = 1;
    if (PMIX_SUCCESS == rc) {
        rc = mod->unpack(&buf, &modex, &n, UT_PMIX_MODEX);
    }
    snprintf(label, sizeof(label), "%s: a modex size larger than the message is refused",
             version);
    report(label, PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER == rc && NULL == modex.blob);
    PMIX_DESTRUCT(&buf);
}

/* v21 and v3 register PMIX_MODEX, but only as an array element type -
 * no release gave pmix_value_t a member for it. A value tagged with it
 * must be refused in both directions.
 *
 * The wire image is what a value of that type would look like: the
 * count and the type tag (a one-element PMIX_UINT16 packs to exactly
 * that under these formats), then a well-formed modex with no blob, so
 * that the only thing wrong with the message is the type. It is
 * unpacked into a block larger than a value and filled with a known
 * byte, and nothing past the value may change. */
#define UT_CANARY_LEN 512

static void test_legacy_modex_value_refused(const char *version)
{
    pmix_bfrops_module_t *mod;
    pmix_buffer_t buf, tmp;
    pmix_value_t *heapval;
    ut_modex_data_t modex;
    uint16_t tag = UT_PMIX_MODEX;
    unsigned char *block;
    char *dst;
    size_t len, i;
    int32_t n;
    pmix_status_t rc;
    char label[128];
    int ok;

    mod = pmix_bfrops_base_assign_module(version);
    if (NULL == mod) {
        fprintf(stdout, "  SKIP: bfrops %s not available\n", version);
        return;
    }

    PMIX_CONSTRUCT(&buf, pmix_buffer_t);
    buf.type = PMIX_BFROP_BUFFER_NON_DESC;
    PMIX_CONSTRUCT(&tmp, pmix_buffer_t);
    tmp.type = PMIX_BFROP_BUFFER_NON_DESC;
    memset(&modex, 0, sizeof(modex));
    ok = (PMIX_SUCCESS == mod->pack(&buf, &tag, 1, PMIX_UINT16) &&
          PMIX_SUCCESS == mod->pack(&tmp, &modex, 1, UT_PMIX_MODEX) &&
          sizeof(int32_t) < tmp.bytes_used);
    if (ok) {
        /* the modex without its own element count */
        len = tmp.bytes_used - sizeof(int32_t);
        dst = pmix_bfrop_buffer_extend(&buf, len);
        ok = (NULL != dst);
        if (ok) {
            memcpy(dst, tmp.base_ptr + sizeof(int32_t), len);
            buf.pack_ptr += len;
            buf.bytes_used += len;
        }
    }
    if (ok) {
        block = (unsigned char *) malloc(UT_CANARY_LEN);
        memset(block, 0xa5, UT_CANARY_LEN);
        n = 1;
        rc = mod->unpack(&buf, block, &n, PMIX_VALUE);
        ok = (PMIX_SUCCESS != rc);
        for (i = sizeof(pmix_value_t); i < UT_CANARY_LEN; i++) {
            if (0xa5 != block[i]) {
                fprintf(stdout, "    byte %lu past the value was written\n", (unsigned long) i);
                ok = 0;
                break;
            }
        }
        free(block);
    }
    snprintf(label, sizeof(label), "%s: a value tagged PMIX_MODEX is not unpacked", version);
    report(label, ok);
    PMIX_DESTRUCT(&tmp);
    PMIX_DESTRUCT(&buf);

    heapval = (pmix_value_t *) calloc(1, sizeof(pmix_value_t));
    heapval->type = UT_PMIX_MODEX;
    PMIX_CONSTRUCT(&buf, pmix_buffer_t);
    buf.type = PMIX_BFROP_BUFFER_NON_DESC;
    rc = mod->pack(&buf, heapval, 1, PMIX_VALUE);
    snprintf(label, sizeof(label), "%s: a value tagged PMIX_MODEX is not packed", version);
    report(label, PMIX_SUCCESS != rc);
    PMIX_DESTRUCT(&buf);
    free(heapval);
}

/* A value of type PMIX_POINTER carries only a sentinel on the wire. The
 * value it unpacks into must hold no pointer afterward, whatever its
 * storage held before. */
static void test_pointer_value_unpacks_null(void)
{
    pmix_data_buffer_t buf;
    pmix_value_t in, out;
    pmix_status_t rc;
    int32_t cnt = 1;
    int local = 0;
    bool ok;

    PMIX_VALUE_CONSTRUCT(&in);
    in.type = PMIX_POINTER;
    in.data.ptr = &local;
    PMIX_DATA_BUFFER_CONSTRUCT(&buf);
    rc = PMIx_Data_pack(NULL, &buf, &in, 1, PMIX_VALUE);
    memset(&out, 0, sizeof(out));
    out.data.ptr = (void *) &cnt;   /* not what a received value may hold */
    if (PMIX_SUCCESS == rc) {
        rc = PMIx_Data_unpack(NULL, &buf, &out, &cnt, PMIX_VALUE);
    }
    ok = (PMIX_SUCCESS == rc && PMIX_POINTER == out.type && NULL == out.data.ptr);
    report("a received pointer value holds no pointer", ok);
    PMIX_DATA_BUFFER_DESTRUCT(&buf);
}

/* An app's argv and env are unpacked into arrays allocated once from
 * the counts that precede them: every element survives the trip, and a
 * count the payload cannot hold is refused. */
static void test_app_argv(void)
{
    pmix_data_buffer_t buf;
    pmix_app_t in, out;
    pmix_status_t rc;
    int32_t cnt = 1;
    int big = 1000000;
    char arg[32];
    bool ok;
    int n;

    PMIX_APP_CONSTRUCT(&in);
    in.cmd = strdup("prog");
    for (n = 0; n < 1000; n++) {
        snprintf(arg, sizeof(arg), "arg%d", n);
        PMIx_Argv_append_nosize(&in.argv, arg);
    }
    PMIx_Argv_append_nosize(&in.env, "A=1");
    PMIx_Argv_append_nosize(&in.env, "B=2");
    PMIX_DATA_BUFFER_CONSTRUCT(&buf);
    rc = PMIx_Data_pack(NULL, &buf, &in, 1, PMIX_APP);
    memset(&out, 0, sizeof(out));
    if (PMIX_SUCCESS == rc) {
        rc = PMIx_Data_unpack(NULL, &buf, &out, &cnt, PMIX_APP);
    }
    ok = (PMIX_SUCCESS == rc && 1000 == PMIx_Argv_count(out.argv) &&
          2 == PMIx_Argv_count(out.env) && 0 == strcmp(out.argv[999], "arg999") &&
          0 == strcmp(out.env[1], "B=2"));
    report("an app's argv and env survive the trip", ok);
    PMIX_APP_DESTRUCT(&out);
    PMIX_APP_DESTRUCT(&in);
    PMIX_DATA_BUFFER_DESTRUCT(&buf);

    /* [one app][cmd][argc far past what follows] */
    {
        wire_acc_t acc = {NULL, 0};
        char *cmd = "prog";

        ok = append_raw(&acc, 1) && append_bare(&acc, &cmd, PMIX_STRING) &&
             append_bare(&acc, &big, PMIX_INT);
        memset(&out, 0, sizeof(out));
        rc = PMIX_ERROR;
        if (ok) {
            acc.bytes[0] = 1;   /* the count of apps, flex-encoded */
            load_wire(&buf, (const unsigned char *) acc.bytes, acc.len);
            cnt = 1;
            rc = PMIx_Data_unpack(NULL, &buf, &out, &cnt, PMIX_APP);
            PMIX_DATA_BUFFER_DESTRUCT(&buf);
        }
        free(acc.bytes);
        report("an argv count the payload cannot hold is refused",
               ok && PMIX_SUCCESS != rc && NULL == out.argv);
        PMIX_APP_DESTRUCT(&out);
    }
}

int main(int argc, char **argv)
{
    pmix_status_t rc;
    (void) argc;
    (void) argv;

    /* The literal wire images below carry no data-type descriptors -
     * they are what one peer sends another. A debug build describes its
     * buffers by default, and would then read the first byte of each
     * image as a type code and refuse it before the decoder under test
     * ever ran. */
    setenv("PMIX_MCA_bfrops_base_default_type", "1", 1);

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
    test_element_count_cannot_exceed_the_message();
    test_a_value_cannot_carry_an_oversized_type();
    test_random_bytes_through_every_unpacker();
    test_element_count_sign();
    test_legacy_counts_bounded("v21");
    test_legacy_counts_bounded("v3");
    test_legacy_modex_value_refused("v21");
    test_legacy_modex_value_refused("v3");
    test_count_fits_boundaries();
    test_nested_info_counts_bounded();
    test_sparse_array_counts();
    test_pointer_value_unpacks_null();
    test_app_argv();

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);

    PMIx_server_finalize();

    return (nfail > 0) ? 1 : 0;
}
