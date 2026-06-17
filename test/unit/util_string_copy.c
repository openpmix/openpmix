/*
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit tests for pmix_string_copy().
 *
 * Exit 0 if all tests pass, 1 otherwise.
 */

#include "src/include/pmix_config.h"
#include "src/include/pmix_globals.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/util/pmix_string_copy.h"

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
/* pmix_string_copy                                                    */
/* ------------------------------------------------------------------ */

/* src fits comfortably in dest */
static void test_copy_short_src(void)
{
    char dest[64];
    memset(dest, 0xff, sizeof(dest));
    pmix_string_copy(dest, "hello", sizeof(dest));
    report("copy short src: content matches", 0 == strcmp(dest, "hello"));
    report("copy short src: null terminated", '\0' == dest[5]);
}

/* src fills dest exactly (dest_len - 1 chars + null) */
static void test_copy_exact_fit(void)
{
    char dest[6];
    memset(dest, 0xff, sizeof(dest));
    pmix_string_copy(dest, "hello", sizeof(dest));
    report("copy exact fit: content matches", 0 == strcmp(dest, "hello"));
    report("copy exact fit: null at dest[5]", '\0' == dest[5]);
}

/* src is longer than dest — must truncate and still null-terminate */
static void test_copy_truncate(void)
{
    char dest[4];
    memset(dest, 0xff, sizeof(dest));
    pmix_string_copy(dest, "hello", sizeof(dest));
    report("copy truncate: null terminated", '\0' == dest[3]);
    report("copy truncate: first chars preserved", 0 == strncmp(dest, "hel", 3));
}

/* dest_len == 1: only the null terminator can fit */
static void test_copy_single_byte_dest(void)
{
    char dest[1];
    dest[0] = 'x';
    pmix_string_copy(dest, "hello", 1);
    report("copy 1-byte dest: null terminated", '\0' == dest[0]);
}

/* empty source string */
static void test_copy_empty_src(void)
{
    char dest[8];
    memset(dest, 0xff, sizeof(dest));
    pmix_string_copy(dest, "", sizeof(dest));
    report("copy empty src: dest is empty string", '\0' == dest[0]);
}

/* verify bytes after the null are not disturbed when src is short */
static void test_copy_no_overwrite_beyond_null(void)
{
    char dest[8];
    memset(dest, 0x55, sizeof(dest));
    pmix_string_copy(dest, "hi", sizeof(dest));
    report("copy no overwrite beyond null: sentinel byte intact", (unsigned char) 0x55 == (unsigned char) dest[3]);
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    fprintf(stdout, "\n=== pmix_string_copy unit tests ===\n\n");

    test_copy_short_src();
    test_copy_exact_fit();
    test_copy_truncate();
    test_copy_single_byte_dest();
    test_copy_empty_src();
    test_copy_no_overwrite_beyond_null();

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);
    return (nfail > 0) ? 1 : 0;
}
