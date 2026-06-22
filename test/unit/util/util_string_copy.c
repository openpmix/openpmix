/*
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit tests for pmix_string_copy() and pmix_getline().
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

/* dest_len == 0: nothing may be written (no dest[-1] underflow) */
static void test_copy_zero_len_dest(void)
{
    /* surround the target byte with sentinels; a buggy implementation
     * writes to target[-1], i.e. guard[0] */
    char guard[3];
    char *target = &guard[1];
    guard[0] = (char) 0xAB;
    guard[1] = (char) 0xCD;
    guard[2] = (char) 0xEF;
    pmix_string_copy(target, "hello", 0);
    report("copy 0-len dest: no underflow write", (unsigned char) 0xAB == (unsigned char) guard[0]);
    report("copy 0-len dest: target untouched", (unsigned char) 0xCD == (unsigned char) guard[1]);
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
/* pmix_getline                                                        */
/* ------------------------------------------------------------------ */

/* write the given contents to a fresh temp file, rewound for reading */
static FILE *make_tmpfile(const char *contents)
{
    FILE *fp = tmpfile();
    if (NULL == fp) {
        return NULL;
    }
    if (0 < strlen(contents)) {
        fwrite(contents, 1, strlen(contents), fp);
    }
    rewind(fp);
    return fp;
}

/* a normal newline-terminated line has its newline stripped */
static void test_getline_strip_newline(void)
{
    FILE *fp = make_tmpfile("hello\nworld\n");
    char *line;

    line = pmix_getline(fp);
    report("getline: first line stripped", line && 0 == strcmp(line, "hello"));
    free(line);
    line = pmix_getline(fp);
    report("getline: second line stripped", line && 0 == strcmp(line, "world"));
    free(line);
    line = pmix_getline(fp);
    report("getline: EOF returns NULL", NULL == line);
    free(line);
    fclose(fp);
}

/* a final line with no trailing newline must keep all its characters */
static void test_getline_no_trailing_newline(void)
{
    FILE *fp = make_tmpfile("nonewline");
    char *line = pmix_getline(fp);
    report("getline: unterminated last line preserved", line && 0 == strcmp(line, "nonewline"));
    free(line);
    fclose(fp);
}

/* an empty line ("\n") must return an empty string, not underflow */
static void test_getline_empty_line(void)
{
    FILE *fp = make_tmpfile("\nafter\n");
    char *line;

    line = pmix_getline(fp);
    report("getline: empty line returns \"\"", line && 0 == strcmp(line, ""));
    free(line);
    line = pmix_getline(fp);
    report("getline: line after empty preserved", line && 0 == strcmp(line, "after"));
    free(line);
    fclose(fp);
}

/* a line longer than the internal buffer must not lose a character to
 * the newline-stripping logic (the first fgets read has no newline) */
static void test_getline_long_line(void)
{
    char big[2000];
    memset(big, 'a', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';

    FILE *fp = make_tmpfile(big);
    char *line = pmix_getline(fp);
    /* the internal buffer is 1024, so the first read returns 1023
     * 'a' chars with no newline; none of them may be dropped */
    report("getline: long line not truncated by strip",
           line && 1023 == strlen(line) && (size_t) (strspn(line, "a")) == strlen(line));
    free(line);
    fclose(fp);
}

static void test_getline_empty_file(void)
{
    FILE *fp = make_tmpfile("");
    char *line = pmix_getline(fp);
    report("getline: empty file returns NULL", NULL == line);
    free(line);
    fclose(fp);
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
    test_copy_zero_len_dest();
    test_copy_empty_src();
    test_copy_no_overwrite_beyond_null();

    test_getline_strip_newline();
    test_getline_no_trailing_newline();
    test_getline_empty_line();
    test_getline_long_line();
    test_getline_empty_file();

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);
    return (nfail > 0) ? 1 : 0;
}
