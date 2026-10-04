/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Exercise the pcompress BLOCK API against whichever component the build
 * selected.  The existing compress.c test drives compression only indirectly,
 * through preg's regex encoding; nothing covered PMIx_Data_compress /
 * PMIx_Data_decompress and the shared blob format directly, which is what
 * every collective payload actually rides on.
 *
 * Deliberately component-agnostic: it asserts the CONTRACT the framework
 * documents - decline below the limit, decline when the result would not be
 * smaller, a 4-byte host-order raw-length prefix that get_decompressed_size
 * can read without inflating, and an exact round-trip - so it passes for
 * zlib, zlib-ng or zstd and fails for a component that breaks any of them.
 */

#include "src/include/pmix_config.h"
#include "include/pmix_server.h"
#include "src/include/pmix_globals.h"
#include "src/include/pmix_types.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/mca/pcompress/base/base.h"
#include "src/mca/pcompress/pcompress.h"

static pmix_server_module_t mymodule __pmix_attribute_unused__ = {0};

/* Everything the test uses is compiled only where it runs: in a
 * --enable-test-build main() is a SKIP and nothing below is referenced. */
#if !PMIX_TESTBUILD
static int errors = 0;

#define CHECK(cond, ...)                        \
    do {                                        \
        if (!(cond)) {                          \
            fprintf(stderr, "FAIL: ");          \
            fprintf(stderr, __VA_ARGS__);       \
            fprintf(stderr, "\n");              \
            ++errors;                           \
        }                                       \
    } while (0)

/* Compressible in the way a modex is: repetitive framing wrapped around
 * opaque values that do not compress at all. */
static void fill_modexish(uint8_t *p, size_t len)
{
    uint64_t s = 0x9e3779b97f4a7c15ULL;
    size_t n = 0;

    while (n < len) {
        char rec[160];
        int k = snprintf(rec, sizeof(rec),
                         "pmix.nspace.testjob@1|rank=%u|btl.tcp=10.0.0.%u:%u|",
                         (unsigned) (n / 128), (unsigned) ((n / 128) & 0xff),
                         (unsigned) (30000 + (n & 0x7f)));
        size_t i, tail = 64;
        if (n + (size_t) k + tail > len) {
            break;
        }
        memcpy(p + n, rec, (size_t) k);
        n += (size_t) k;
        for (i = 0; i < tail; i++) {
            s ^= s << 13;
            s ^= s >> 7;
            s ^= s << 17;
            p[n++] = (uint8_t) s;
        }
    }
    while (n < len) {
        p[n++] = 0;
    }
}

/* The decompressors' output buffer: sized from the compressed length, grown
 * as output arrives, and never past one byte beyond the claim. Checked
 * directly because a component test can only see that a forged blob was
 * refused, not how much memory was committed before it was. */
static void test_outbuf_policy(void)
{
    pmix_compress_base_outbuf_t buf;
    uint8_t *out;
    size_t prev;
    int ngrow;

    /* a tiny blob claiming nearly 4 GiB must not get anything like it */
    CHECK(pmix_compress_base_outbuf_start(&buf, 64, UINT32_MAX - 1),
          "outbuf_start refused a legal claim");
    CHECK(buf.size <= 64 * 1024,
          "outbuf_start committed %zu bytes for a 64-byte blob", buf.size);
    CHECK(buf.ceiling == (size_t) UINT32_MAX,
          "ceiling is %zu, expected the claim plus one", buf.ceiling);
    pmix_compress_base_outbuf_release(&buf);

    /* the error sentinel is not a claim */
    CHECK(!pmix_compress_base_outbuf_start(&buf, 64, UINT32_MAX),
          "outbuf_start accepted the UINT32_MAX sentinel as a length");
    pmix_compress_base_outbuf_release(&buf);

    /* a claim below the first guess is allocated outright */
    CHECK(pmix_compress_base_outbuf_start(&buf, 1000, 100), "outbuf_start failed");
    CHECK(101 == buf.size, "a 100-byte claim got %zu bytes, expected 101", buf.size);
    CHECK(!pmix_compress_base_outbuf_grow(&buf),
          "outbuf_grow went past the claim's ceiling");
    pmix_compress_base_outbuf_release(&buf);

    /* growth doubles, lands exactly on the ceiling, then stops */
    CHECK(pmix_compress_base_outbuf_start(&buf, 16, 1000000), "outbuf_start failed");
    ngrow = 0;
    prev = buf.size;
    while (pmix_compress_base_outbuf_grow(&buf)) {
        CHECK(buf.size > prev && buf.size <= 2 * prev,
              "outbuf_grow went from %zu to %zu", prev, buf.size);
        prev = buf.size;
        ++ngrow;
    }
    CHECK(buf.size == buf.ceiling, "growth stopped at %zu short of the ceiling %zu",
          buf.size, buf.ceiling);
    CHECK(0 < ngrow, "a 1 MB claim from a 16-byte blob never grew");

    /* finish insists on the exact claim */
    out = (uint8_t *) 0x1;
    CHECK(!pmix_compress_base_outbuf_finish(&buf, 999999, 1000000, &out),
          "outbuf_finish accepted a short output");
    CHECK(NULL == out, "a refused finish left an output pointer behind");
    CHECK(!pmix_compress_base_outbuf_finish(&buf, 1000001, 1000000, &out),
          "outbuf_finish accepted a long output");
    pmix_compress_base_outbuf_release(&buf);

    /* a decoder that finished in a buffer exactly the claim's size still
     * gets its terminator */
    CHECK(pmix_compress_base_outbuf_start(&buf, 1, 3), "outbuf_start failed");
    buf.size = 3; // as if the first guess had been exactly the claim
    memcpy(buf.bytes, "abc", 3);
    CHECK(pmix_compress_base_outbuf_finish(&buf, 3, 3, &out),
          "outbuf_finish refused an exact output");
    if (NULL != out) {
        CHECK(0 == strcmp((char *) out, "abc"), "finish did not NUL-terminate");
        free(out);
    }
    pmix_compress_base_outbuf_release(&buf);
}

/* Re-label a blob's length prefix and hand it to both entry points; every
 * claim that is not the payload's true length must be refused. */
static void check_forged_claim(const uint8_t *zip, size_t ziplen, uint32_t claim,
                               const char *what)
{
    uint8_t *forged, *back = NULL;
    char *strback = NULL;
    size_t backlen = 0;

    forged = (uint8_t *) malloc(ziplen);
    if (NULL == forged) {
        fprintf(stderr, "out of memory\n");
        ++errors;
        return;
    }
    memcpy(forged, zip, ziplen);
    memcpy(forged, &claim, sizeof(uint32_t));

    if (pmix_compress.decompress(&back, &backlen, forged, ziplen)) {
        CHECK(false, "decompress accepted a blob whose prefix %s", what);
        free(back);
    } else {
        CHECK(0 == backlen, "refused blob whose prefix %s still reported %zu bytes",
              what, backlen);
    }
    if (NULL != pmix_compress.decompress_string) {
        if (pmix_compress.decompress_string(&strback, forged, ziplen)) {
            CHECK(false, "decompress_string accepted a blob whose prefix %s", what);
            free(strback);
        } else {
            CHECK(NULL == strback, "refused string blob whose prefix %s left a "
                  "string behind", what);
        }
    }
    free(forged);
}

/* A payload that compresses far better than the first guess the output
 * buffer is sized from, so the decoder has to grow it repeatedly, then the
 * same blob with its prefix forged in every direction. */
static void test_claims(void)
{
    size_t len = 8 * 1024 * 1024, n, ziplen = 0, backlen = 0;
    uint8_t *raw, *zip = NULL, *back = NULL;
    char *strback = NULL;
    uint32_t truth;

    raw = (uint8_t *) malloc(len + 1);
    if (NULL == raw) {
        fprintf(stderr, "out of memory\n");
        ++errors;
        return;
    }
    for (n = 0; n < len; n++) {
        raw[n] = (uint8_t) ('a' + (n % 23));
    }
    raw[len] = '\0';

    if (!PMIx_Data_compress(raw, len, &zip, &ziplen)) {
        CHECK(false, "declined an 8 MB run of repeated text");
        free(raw);
        return;
    }
    fprintf(stdout, "compressed %zu -> %zu (ratio %.6f)\n", len, ziplen,
            (double) ziplen / (double) len);

    CHECK(PMIx_Data_decompress(zip, ziplen, &back, &backlen),
          "decompress refused a highly compressed blob it had just produced");
    if (NULL != back) {
        CHECK(backlen == len, "inflated to %zu bytes, expected %zu", backlen, len);
        CHECK(0 == memcmp(back, raw, len), "round-trip altered a highly compressed payload");
        free(back);
        back = NULL;
    }
    free(zip);
    zip = NULL;

    if (NULL != pmix_compress.compress_string && NULL != pmix_compress.decompress_string &&
        pmix_compress.compress_string((char *) raw, &zip, &ziplen)) {
        CHECK(pmix_compress.decompress_string(&strback, zip, ziplen),
              "decompress_string refused a highly compressed string");
        if (NULL != strback) {
            CHECK(0 == strcmp(strback, (char *) raw),
                  "string round-trip altered a highly compressed payload");
            free(strback);
            strback = NULL;
        }
        free(zip);
        zip = NULL;
    }

    if (!PMIx_Data_compress(raw, len, &zip, &ziplen)) {
        free(raw);
        return;
    }
    memcpy(&truth, zip, sizeof(uint32_t));

    check_forged_claim(zip, ziplen, truth + 1, "claims one byte more than it holds");
    check_forged_claim(zip, ziplen, truth - 1, "claims one byte less than it holds");
    check_forged_claim(zip, ziplen, truth / 2, "claims half of what it holds");
    check_forged_claim(zip, ziplen, truth * 2, "claims twice what it holds");
    check_forged_claim(zip, ziplen, UINT32_MAX - 1, "claims nearly 4 GiB");
    check_forged_claim(zip, ziplen, 0, "claims nothing");

    /* the true claim over a payload cut short */
    {
        uint8_t *cut = NULL;
        size_t cutlen = ziplen / 2;

        CHECK(!pmix_compress.decompress(&cut, &backlen, zip, cutlen),
              "decompress accepted a blob with half its payload missing");
        if (NULL != cut) {
            free(cut);
        }
        if (NULL != pmix_compress.decompress_string) {
            CHECK(!pmix_compress.decompress_string(&strback, zip, cutlen),
                  "decompress_string accepted a blob with half its payload missing");
            if (NULL != strback) {
                free(strback);
                strback = NULL;
            }
        }
    }

    free(zip);
    free(raw);
}
#endif

int main(int argc, char **argv)
{
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

#if PMIX_TESTBUILD
    /* The components are non-functional shims in a --enable-test-build, so a
     * round-trip cannot reproduce its input.  Same reasoning as compress.c. */
    fprintf(stdout, "SKIP: compression is stubbed in --enable-test-build\n");
    return 77;
#else
    pmix_status_t rc;
    size_t len = 4 * 1024 * 1024;
    uint8_t *raw, *zip = NULL, *back = NULL;
    size_t ziplen = 0, backlen = 0;
    pmix_byte_object_t bo;
    char *str, *strback = NULL;
    size_t n;

    if (PMIX_SUCCESS != (rc = PMIx_server_init(&mymodule, NULL, 0))) {
        fprintf(stderr, "Init failed with error %s\n", PMIx_Error_string(rc));
        return rc;
    }

    raw = (uint8_t *) malloc(len);
    if (NULL == raw) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }
    fill_modexish(raw, len);

    /* --- a block that should compress --------------------------------- */
    if (!PMIx_Data_compress(raw, len, &zip, &ziplen)) {
        /* Not a failure of the test: a build with no compression library at
         * all keeps the base default stubs, which decline everything. */
        fprintf(stdout, "SKIP: no compression component in this build\n");
        free(raw);
        PMIx_server_finalize();
        return 77;
    }
    fprintf(stdout, "compressed %zu -> %zu (ratio %.4f)\n", len, ziplen,
            (double) ziplen / (double) len);

    CHECK(ziplen < len, "compress returned true but the result is not smaller");

    /* the 4-byte prefix must report the inflated size without inflating.
     *
     * Screen the entry point first. A module fills in only the subset it
     * implements, and components are run-time-loadable, so the plugin
     * that got selected can be older than the libpmix that loaded it -
     * which is what any partial upgrade produces, and every module built
     * before July 2026 left this one NULL. Calling it blind is a jump to
     * address zero, and a test that segfaults reports nothing at all.
     * This is the same screen bfrops applies at its own call sites. */
    if (NULL == pmix_compress.get_decompressed_size) {
        fprintf(stdout, "NOTE: selected component has no get_decompressed_size; "
                        "skipping the size-prefix check\n");
    } else {
        bo.bytes = (char *) zip;
        bo.size = ziplen;
        CHECK(len == pmix_compress.get_decompressed_size(&bo),
              "get_decompressed_size gave %zu, expected %zu",
              pmix_compress.get_decompressed_size(&bo), len);
    }

    /* --- and must round-trip exactly ----------------------------------- */
    CHECK(PMIx_Data_decompress(zip, ziplen, &back, &backlen),
          "decompress refused the blob compress had just produced");
    if (NULL != back) {
        CHECK(backlen == len, "inflated to %zu bytes, expected %zu", backlen, len);
        CHECK(0 == memcmp(back, raw, len), "round-trip altered the payload");
        free(back);
        back = NULL;
    }
    free(zip);
    zip = NULL;

    /* --- output that must grow, and prefixes that lie ------------------ */
    test_outbuf_policy();
    test_claims();

    /* --- below the limit: must decline --------------------------------- */
    if (0 < pmix_compress_base.compress_limit) {
        size_t small = pmix_compress_base.compress_limit - 1;
        CHECK(!PMIx_Data_compress(raw, small, &zip, &ziplen),
              "compressed a %zu-byte input despite a limit of %zu", small,
              pmix_compress_base.compress_limit);
        if (NULL != zip) {
            free(zip);
            zip = NULL;
        }
    }

    /* --- incompressible: must decline rather than grow the payload ----- */
    {
        uint64_t s = 88172645463325252ULL;
        for (n = 0; n < len; n++) {
            s ^= s << 13;
            s ^= s >> 7;
            s ^= s << 17;
            raw[n] = (uint8_t) s;
        }
        if (PMIx_Data_compress(raw, len, &zip, &ziplen)) {
            /* allowed to succeed, but only if it genuinely saved space */
            CHECK(ziplen < len,
                  "claimed success on random data with %zu >= %zu bytes", ziplen, len);
            free(zip);
            zip = NULL;
        } else {
            fprintf(stdout, "declined incompressible input, as it should\n");
        }
    }

    /* --- the string path, which stores length WITHOUT the NUL ---------- */
    str = (char *) malloc(len + 1);
    fill_modexish((uint8_t *) str, len);
    for (n = 0; n < len; n++) {
        /* no embedded NULs, and keep it printable so strlen is the length */
        if (0 == str[n]) {
            str[n] = 'x';
        }
    }
    str[len] = '\0';

    /* every one of these is screened for the same reason as the size
     * entry point above */
    if (NULL != pmix_compress.compress_string &&
        pmix_compress.compress_string(str, &zip, &ziplen)) {
        bo.bytes = (char *) zip;
        bo.size = ziplen;
        if (NULL == pmix_compress.get_decompressed_strlen) {
            fprintf(stdout, "NOTE: selected component has no "
                            "get_decompressed_strlen; skipping that check\n");
        } else {
            CHECK(len + 1 == pmix_compress.get_decompressed_strlen(&bo),
                  "get_decompressed_strlen gave %zu, expected %zu",
                  pmix_compress.get_decompressed_strlen(&bo), len + 1);
        }
        if (NULL == pmix_compress.decompress_string) {
            fprintf(stdout, "NOTE: selected component compresses strings but "
                            "cannot inflate them; skipping the round-trip\n");
        } else {
            CHECK(pmix_compress.decompress_string(&strback, zip, ziplen),
                  "decompress_string refused its own output");
            if (NULL != strback) {
                CHECK(0 == strcmp(strback, str), "string round-trip altered the payload");
                free(strback);
            }
        }
        free(zip);
        zip = NULL;
    }
    free(str);
    str = NULL;

    /* --- a compressed string must not survive deserialization ----------
     *
     * PMIx offers no way for an application to expand a
     * PMIX_COMPRESSED_STRING, so one that reaches a caller through
     * PMIx_Get is bytes they cannot read, and one that reaches the
     * datastore is stored in a form nothing can match against. Peers
     * still send them - every released PMIx compressed large string
     * values on the way out - so unpack has to expand what it is given
     * and hand back a plain PMIX_STRING. Regression for the leak that
     * opened when the two PMIX_VALUE_COMPRESSED_STRING_UNPACK call sites
     * were dropped from the client get path. */
    if (NULL != pmix_compress.compress_string &&
        NULL != pmix_compress.decompress_string) {
        pmix_data_buffer_t dbuf;
        pmix_value_t vsrc, vdst;
        int32_t cnt = 1;

        str = (char *) malloc(8192 + 1);
        if (NULL == str) {
            fprintf(stderr, "out of memory\n");
            return 1;
        }
        fill_modexish((uint8_t *) str, 8192);
        for (n = 0; n < 8192; n++) {
            if (0 == str[n]) {
                str[n] = 'x';
            }
        }
        str[8192] = '\0';

        if (!pmix_compress.compress_string(str, &zip, &ziplen)) {
            fprintf(stdout, "NOTE: component declined an 8k string; "
                            "skipping the compressed-value unpack check\n");
        } else {
            /* hand-build exactly what an older peer puts on the wire */
            memset(&vsrc, 0, sizeof(vsrc));
            vsrc.type = PMIX_COMPRESSED_STRING;
            vsrc.data.bo.bytes = (char *) zip;
            vsrc.data.bo.size = ziplen;

            PMIX_DATA_BUFFER_CONSTRUCT(&dbuf);
            rc = PMIx_Data_pack(NULL, &dbuf, &vsrc, 1, PMIX_VALUE);
            CHECK(PMIX_SUCCESS == rc, "packing a compressed-string value failed: %s",
                  PMIx_Error_string(rc));
            if (PMIX_SUCCESS == rc) {
                memset(&vdst, 0, sizeof(vdst));
                rc = PMIx_Data_unpack(NULL, &dbuf, &vdst, &cnt, PMIX_VALUE);
                CHECK(PMIX_SUCCESS == rc, "unpacking a compressed-string value failed: %s",
                      PMIx_Error_string(rc));
                if (PMIX_SUCCESS == rc) {
                    CHECK(PMIX_STRING == vdst.type,
                          "unpack returned type %s, expected PMIX_STRING - a compressed "
                          "string reached the caller unexpanded",
                          PMIx_Data_type_string(vdst.type));
                    if (PMIX_STRING == vdst.type) {
                        CHECK(NULL != vdst.data.string && 0 == strcmp(vdst.data.string, str),
                              "the expanded string does not match what was compressed");
                    }
                    PMIX_VALUE_DESTRUCT(&vdst);
                }
            }
            PMIX_DATA_BUFFER_DESTRUCT(&dbuf);
            free(zip);
            zip = NULL;
        }
        free(str);
        str = NULL;
    }

    /* --- a blob too short to carry its own length prefix --------------
     *
     * Every entry point that inflates starts by reading the 4-byte raw-length
     * prefix and then sizing the payload as len - 4.  Both steps trust a
     * length that generally came off a peer's wire: a byte object out of a
     * modex, a regex a peer declared the size of, or whatever a caller passed
     * PMIx_Data_decompress - which screens only for a NULL pointer.  Below
     * four bytes the read runs off the end and the subtraction underflows,
     * handing the inflater roughly four billion bytes of "input" to walk.
     *
     * zlib and zlibng screened their string entry point for this and not
     * their block one, which is the asymmetry this covers.  Component
     * agnostic on purpose: the contract is "refuse, do not read", and all
     * four must honor it at both entry points. */
    {
        uint8_t shortblob[8];
        size_t k;

        memset(shortblob, 0, sizeof(shortblob));

        for (k = 0; k < sizeof(uint32_t); k++) {
            back = (uint8_t *) 0x1;
            backlen = 1;
            CHECK(!pmix_compress.decompress(&back, &backlen, shortblob, k),
                  "decompress accepted a %zu-byte blob, which cannot hold the "
                  "4-byte length prefix it is about to read", k);
            CHECK(0 == backlen, "decompress refused a %zu-byte blob but still "
                  "reported %zu bytes of output", k, backlen);

            if (NULL != pmix_compress.decompress_string) {
                strback = (char *) 0x1;
                CHECK(!pmix_compress.decompress_string(&strback, shortblob, k),
                      "decompress_string accepted a %zu-byte blob", k);
                CHECK(NULL == strback,
                      "decompress_string refused a %zu-byte blob but left a "
                      "non-NULL string behind", k);
            }
        }

        /* NULL with a length that would otherwise pass the size screen */
        back = (uint8_t *) 0x1;
        backlen = 1;
        CHECK(!pmix_compress.decompress(&back, &backlen, NULL, 64),
              "decompress accepted a NULL buffer");
        if (NULL != pmix_compress.decompress_string) {
            strback = (char *) 0x1;
            CHECK(!pmix_compress.decompress_string(&strback, NULL, 64),
                  "decompress_string accepted a NULL buffer");
        }

        /* A blob long enough to read, whose prefix claims nothing inflates
         * out of it.  0 is the "I cannot answer" sentinel every one of these
         * entry points shares with the base default and with bfrops'
         * decompressed_strlen() fallback, so the strlen variant must not turn
         * it into 0 + 1 and report a one-byte string. */
        bo.bytes = (char *) shortblob;
        bo.size = sizeof(shortblob);
        if (NULL != pmix_compress.get_decompressed_size) {
            CHECK(0 == pmix_compress.get_decompressed_size(&bo),
                  "get_decompressed_size invented %zu bytes for a zero prefix",
                  pmix_compress.get_decompressed_size(&bo));
        }
        if (NULL != pmix_compress.get_decompressed_strlen) {
            CHECK(0 == pmix_compress.get_decompressed_strlen(&bo),
                  "get_decompressed_strlen gave %zu for a zero prefix; 0 is the "
                  "sentinel the other components and the base default return",
                  pmix_compress.get_decompressed_strlen(&bo));
        }
        back = NULL;
        strback = NULL;
    }

    /* --- a blob some OTHER component produced ------------------------
     *
     * The blob format carries a raw-length prefix and says nothing about
     * what the payload is, so a blob is not self-describing: `zstd` and
     * `lz4` emit frames a DEFLATE reader cannot read, and vice versa.  Every
     * node in a job is therefore required to run the same component (see
     * ../AGENTS.md) - but when that is violated the result must be a clean
     * refusal, not a plausible-looking inflation of the wrong bytes into the
     * modex.
     *
     * `zstd` and `lz4` get this by checking their own frame magic.  `zlib`
     * and `zlibng` get it from the DEFLATE header's own checksum, which
     * rejects both of those magics - which is worth pinning down precisely
     * because it is a property of the format rather than a check anyone
     * wrote.  Each payload below is a valid frame header for one of the
     * three formats followed by bytes that cannot continue it, so the
     * correct answer for every component is false. */
    {
        static const struct {
            const char *what;
            uint8_t magic[4];
            size_t nmagic;
        } foreign[] = {
            {"a zstd frame",   {0x28, 0xB5, 0x2F, 0xFD}, 4},
            {"an lz4 frame",   {0x04, 0x22, 0x4D, 0x18}, 4},
            {"a DEFLATE blob", {0x78, 0x9C, 0x00, 0x00}, 2},
        };
        uint8_t blob[64];
        size_t f;

        for (f = 0; f < sizeof(foreign) / sizeof(foreign[0]); f++) {
            uint32_t claim = 128;

            memset(blob, 0xFF, sizeof(blob));
            memcpy(blob, &claim, sizeof(uint32_t));
            memcpy(blob + sizeof(uint32_t), foreign[f].magic, foreign[f].nmagic);

            back = NULL;
            backlen = 0;
            if (pmix_compress.decompress(&back, &backlen, blob, sizeof(blob))) {
                /* Only tolerable if it genuinely reproduced what the prefix
                 * promised - which these payloads cannot. */
                CHECK(false, "decompress accepted %s that this component did "
                      "not write, yielding %zu bytes", foreign[f].what, backlen);
                free(back);
                back = NULL;
            }
        }
        backlen = 0;
    }

    if (NULL != str) {
        free(str);
    }
    free(raw);

    PMIx_server_finalize();

    if (0 == errors) {
        fprintf(stdout, "COMPRESS BLOCK TEST: PASSED\n");
        return 0;
    }
    fprintf(stderr, "COMPRESS BLOCK TEST: %d FAILURE(S)\n", errors);
    return 1;
#endif
}
