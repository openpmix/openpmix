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

    /* the 4-byte prefix must report the inflated size without inflating */
    bo.bytes = (char *) zip;
    bo.size = ziplen;
    CHECK(len == pmix_compress.get_decompressed_size(&bo),
          "get_decompressed_size gave %zu, expected %zu",
          pmix_compress.get_decompressed_size(&bo), len);

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

    if (pmix_compress.compress_string(str, &zip, &ziplen)) {
        bo.bytes = (char *) zip;
        bo.size = ziplen;
        CHECK(len + 1 == pmix_compress.get_decompressed_strlen(&bo),
              "get_decompressed_strlen gave %zu, expected %zu",
              pmix_compress.get_decompressed_strlen(&bo), len + 1);
        CHECK(pmix_compress.decompress_string(&strback, zip, ziplen),
              "decompress_string refused its own output");
        if (NULL != strback) {
            CHECK(0 == strcmp(strback, str), "string round-trip altered the payload");
            free(strback);
        }
        free(zip);
        zip = NULL;
    }
    free(str);
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
