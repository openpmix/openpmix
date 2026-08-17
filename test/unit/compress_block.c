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
