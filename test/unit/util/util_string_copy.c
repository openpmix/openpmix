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
#include <unistd.h>

#include "src/util/pmix_printf.h"
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
    bool failed;
    FILE *fp = make_tmpfile("hello\nworld\n");
    char *line;

    line = pmix_getline(fp, &failed);
    report("getline: first line stripped", line && 0 == strcmp(line, "hello"));
    free(line);
    line = pmix_getline(fp, &failed);
    report("getline: second line stripped", line && 0 == strcmp(line, "world"));
    free(line);
    line = pmix_getline(fp, &failed);
    report("getline: EOF returns NULL", NULL == line && !failed);
    free(line);
    fclose(fp);
}

/* a final line with no trailing newline must keep all its characters */
static void test_getline_no_trailing_newline(void)
{
    bool failed;
    FILE *fp = make_tmpfile("nonewline");
    char *line = pmix_getline(fp, &failed);
    report("getline: unterminated last line preserved",
           line && 0 == strcmp(line, "nonewline") && !failed);
    free(line);
    fclose(fp);
}

/* an empty line ("\n") must return an empty string, not underflow */
static void test_getline_empty_line(void)
{
    bool failed;
    FILE *fp = make_tmpfile("\nafter\n");
    char *line;

    line = pmix_getline(fp, &failed);
    report("getline: empty line returns \"\"", line && 0 == strcmp(line, ""));
    free(line);
    line = pmix_getline(fp, &failed);
    report("getline: line after empty preserved", line && 0 == strcmp(line, "after"));
    free(line);
    fclose(fp);
}

/* a line far longer than any buffer comes back whole, and the line after
 * it is the next line - not the tail of this one */
static void test_getline_long_line(void)
{
    bool failed;
    char big[5000], *contents = NULL;
    char *line;
    FILE *fp;

    memset(big, 'a', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    if (0 > pmix_asprintf(&contents, "%s\nnext\n%s", big, big)) {
        report("getline: long line test setup", false);
        return;
    }
    fp = make_tmpfile(contents);
    free(contents);

    line = pmix_getline(fp, &failed);
    report("getline: a long line comes back whole",
           line && strlen(big) == strlen(line) && 0 == strcmp(line, big));
    free(line);
    line = pmix_getline(fp, &failed);
    report("getline: the line after a long one is the next line",
           line && 0 == strcmp(line, "next"));
    free(line);
    line = pmix_getline(fp, &failed);
    report("getline: a long final line with no newline comes back whole",
           line && 0 == strcmp(line, big));
    free(line);
    line = pmix_getline(fp, &failed);
    report("getline: then the end of the file", NULL == line && !failed);
    free(line);
    fclose(fp);
}

/* a NUL byte is refused and reported: as a C string the rest of its
 * line would silently vanish */
static void test_getline_nul_byte(void)
{
    bool failed;
    FILE *fp = tmpfile();
    char *line;

    if (NULL == fp) {
        report("getline: NUL test setup", false);
        return;
    }
    fwrite("ok\nab\0cd\nnext\n", 1, 14, fp);
    rewind(fp);
    line = pmix_getline(fp, &failed);
    report("getline: the line before a NUL is read", line && 0 == strcmp(line, "ok") && !failed);
    free(line);
    line = pmix_getline(fp, &failed);
    report("getline: a NUL stops the read", NULL == line);
    report("getline: ...and says so", failed);
    free(line);
    fclose(fp);
}

/* a read error is reported, not taken for the end of the stream */
static void test_getline_read_error(void)
{
    bool failed = false;
    char path[] = "/tmp/pmix_getline_XXXXXX";
    char *line;
    FILE *fp;
    int fd;

    fd = mkstemp(path);
    if (0 > fd) {
        report("getline: read error test setup", false);
        return;
    }
    close(fd);
    /* reading a stream opened only for writing fails */
    fp = fopen(path, "w");
    unlink(path);
    if (NULL == fp) {
        report("getline: read error test setup", false);
        return;
    }
    line = pmix_getline(fp, &failed);
    report("getline: a read error returns NULL", NULL == line);
    report("getline: ...and says so", failed);
    free(line);
    fclose(fp);
}

/* "failed" may be NULL for a caller that does not need it */
static void test_getline_null_flag(void)
{
    FILE *fp = make_tmpfile("one\n");
    char *line;

    line = pmix_getline(fp, NULL);
    report("getline: no flag: the line is read", line && 0 == strcmp(line, "one"));
    free(line);
    line = pmix_getline(fp, NULL);
    report("getline: no flag: then the end of the file", NULL == line);
    free(line);
    fclose(fp);
}

static void test_getline_empty_file(void)
{
    bool failed;
    FILE *fp = make_tmpfile("");
    char *line = pmix_getline(fp, &failed);
    report("getline: empty file returns NULL", NULL == line && !failed);
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
    test_getline_nul_byte();
    test_getline_read_error();
    test_getline_null_flag();
    test_getline_empty_file();

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);
    return (nfail > 0) ? 1 : 0;
}
