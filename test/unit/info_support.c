/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit tests for the pmix_info support-library formatting helpers in
 * src/runtime/pmix_info_support.c.
 *
 * These functions are pure presentation logic and require no PMIx
 * initialization: pmix_info_make_version_str simply builds a string,
 * and pmix_info_out / pmix_info_out_int format a key/value pair to
 * stdout.  The parsable (non-pretty) output path is deterministic, so
 * we drive pmix_info_pretty=false and capture stdout to verify it
 * exactly - this also exercises the internal quote/colon escaping.
 *
 * Test cases:
 *   version-str: full/all include greek; major/minor/release/greek/repo
 *                select the right field.
 *   info-out:    plain "key:value"; values containing ':' are wrapped in
 *                quotes; embedded '"' is backslash-escaped; a NULL/empty
 *                plain message prints the bare value; out_int formats the
 *                integer.
 */

#include "src/include/pmix_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "src/runtime/pmix_info_support.h"

static int npass = 0;
static int nfail = 0;

static void report(const char *name, int passed)
{
    if (passed) {
        fprintf(stdout, "  PASS: %s\n", name);
        ++npass;
    } else {
        fprintf(stdout, "  FAIL: %s\n", name);
        ++nfail;
    }
}

/* ------------------------------------------------------------------ */
/* pmix_info_make_version_str                                          */
/* ------------------------------------------------------------------ */

static void check_version(const char *name, const char *scope, int maj, int min,
                          int rel, const char *greek, const char *repo,
                          const char *expect)
{
    char *got = pmix_info_make_version_str(scope, maj, min, rel, greek, repo);
    int ok = (NULL != got && 0 == strcmp(got, expect));
    if (!ok) {
        fprintf(stdout, "    (scope=%s) expected \"%s\", got \"%s\"\n", scope,
                expect, NULL == got ? "(null)" : got);
    }
    report(name, ok);
    if (NULL != got) {
        free(got);
    }
}

static void test_version_str(void)
{
    /* "full" and "all" render major.minor.release with the greek suffix
     * appended verbatim (an empty greek appends nothing) */
    check_version("version:full", "full", 5, 1, 2, "rc3", "sha", "5.1.2rc3");
    check_version("version:full-nogreek", "full", 5, 1, 2, "", "sha", "5.1.2");
    check_version("version:all", "all", 6, 0, 11, "a1", "sha", "6.0.11a1");

    /* single-field scopes */
    check_version("version:major", "major", 5, 1, 2, "", "", "5");
    check_version("version:minor", "minor", 5, 1, 2, "", "", "1");
    check_version("version:release", "release", 5, 1, 2, "", "", "2");
    check_version("version:greek", "greek", 5, 1, 2, "rc3", "", "rc3");
    check_version("version:repo", "repo", 5, 1, 2, "", "deadbeef", "deadbeef");

    /* misuse hardening: none of these should crash or leak uninitialized
     * memory - they all resolve to an empty string */
    check_version("version:unknown-scope", "bogus", 5, 1, 2, "x", "y", "");
    check_version("version:null-scope", NULL, 5, 1, 2, "x", "y", "");
    check_version("version:greek-null", "greek", 5, 1, 2, NULL, "", "");
    check_version("version:repo-null", "repo", 5, 1, 2, "", NULL, "");
}

/* ------------------------------------------------------------------ */
/* pmix_info_out - capture stdout and compare                          */
/* ------------------------------------------------------------------ */

static char capbuf[8192];

/* Run one pmix_info_out call with stdout redirected to a temp file,
 * then load what it printed into capbuf. */
static void capture_out(const char *pretty, const char *plain, const char *value)
{
    int saved;
    size_t n;
    FILE *tmp;

    capbuf[0] = '\0';
    fflush(stdout);
    saved = dup(fileno(stdout));
    tmp = tmpfile();
    if (NULL == tmp || 0 > saved) {
        return;
    }
    dup2(fileno(tmp), fileno(stdout));

    pmix_info_out(pretty, plain, value);

    fflush(stdout);
    dup2(saved, fileno(stdout));
    close(saved);

    rewind(tmp);
    n = fread(capbuf, 1, sizeof(capbuf) - 1, tmp);
    capbuf[n] = '\0';
    fclose(tmp);
}

static void check_out(const char *name, const char *pretty, const char *plain,
                      const char *value, const char *expect)
{
    capture_out(pretty, plain, value);
    int ok = (0 == strcmp(capbuf, expect));
    if (!ok) {
        fprintf(stdout, "    expected \"%s\", got \"%s\"\n", expect, capbuf);
    }
    report(name, ok);
}

static void test_info_out_parsable(void)
{
    bool saved_pretty = pmix_info_pretty;
    pmix_info_pretty = false;

    /* plain key:value */
    check_out("out:plain", NULL, "key", "value", "key:value\n");

    /* a value containing a colon is wrapped in double quotes so the
     * key/value split stays unambiguous */
    check_out("out:colon", NULL, "key", "a:b", "key:\"a:b\"\n");

    /* embedded double quotes are backslash-escaped (no colon -> no wrap) */
    check_out("out:quote", NULL, "path", "a\"b", "path:a\\\"b\n");

    /* both: escaped quote AND colon-wrapping */
    check_out("out:quote-colon", NULL, "k", "a:\"b", "k:\"a:\\\"b\"\n");

    /* an empty/NULL plain message prints just the bare value */
    check_out("out:no-plain", NULL, NULL, "bare", "bare\n");
    check_out("out:empty-plain", NULL, "", "bare", "bare\n");

    /* NULL value is treated as empty string */
    check_out("out:null-value", NULL, "key", NULL, "key:\n");

    pmix_info_pretty = saved_pretty;
}

static void test_info_out_int(void)
{
    bool saved_pretty = pmix_info_pretty;
    pmix_info_pretty = false;

    int saved;
    size_t n;
    FILE *tmp;

    capbuf[0] = '\0';
    fflush(stdout);
    saved = dup(fileno(stdout));
    tmp = tmpfile();
    if (NULL != tmp && 0 <= saved) {
        dup2(fileno(tmp), fileno(stdout));
        pmix_info_out_int(NULL, "count", 42);
        fflush(stdout);
        dup2(saved, fileno(stdout));
        close(saved);
        rewind(tmp);
        n = fread(capbuf, 1, sizeof(capbuf) - 1, tmp);
        capbuf[n] = '\0';
        fclose(tmp);
    }

    int ok = (0 == strcmp(capbuf, "count:42\n"));
    if (!ok) {
        fprintf(stdout, "    expected \"count:42\\n\", got \"%s\"\n", capbuf);
    }
    report("out_int:value", ok);

    pmix_info_pretty = saved_pretty;
}

int main(void)
{
    fprintf(stdout, "\n=== pmix_info support-library unit tests ===\n\n");

    test_version_str();
    test_info_out_parsable();
    test_info_out_int();

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);

    return (nfail > 0) ? 1 : 0;
}
