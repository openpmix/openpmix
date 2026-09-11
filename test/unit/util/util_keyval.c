/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit tests for src/util/pmix_keyval_parse.c: the flex-driven parser
 * for MCA parameter files.  Everything in that file is process-global -
 * the key buffer, the accumulated "-x" string, the lock - so the cases
 * that matter most are about what survives an init/finalize cycle, not
 * about any one parse.
 *
 * Exit 0 if all tests pass, 1 otherwise.
 */

#include "src/include/pmix_config.h"
#include "src/include/pmix_globals.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif

#include "pmix_common.h"
#include "src/util/pmix_keyval_parse.h"
#include "src/util/pmix_printf.h"

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

/* What the callback saw, in the order it saw it. */
#define MAX_SEEN 16
static char *seen_name[MAX_SEEN];
static char *seen_value[MAX_SEEN];
static int seen_lineno[MAX_SEEN];
static int nseen = 0;

static void reset_seen(void)
{
    int i;

    for (i = 0; i < nseen; i++) {
        free(seen_name[i]);
        free(seen_value[i]);
    }
    nseen = 0;
}

/* The parser documents that both strings point into buffers it may
 * overwrite the moment we return, so copy them out. */
static void collect(const char *file, int lineno, const char *name,
                    const char *value, void *cbdata)
{
    PMIX_HIDE_UNUSED_PARAMS(file, cbdata);

    if (nseen >= MAX_SEEN) {
        return;
    }
    seen_name[nseen] = (NULL == name) ? NULL : strdup(name);
    seen_value[nseen] = (NULL == value) ? NULL : strdup(value);
    seen_lineno[nseen] = lineno;
    nseen++;
}

static int find_seen(const char *name)
{
    int i;

    for (i = 0; i < nseen; i++) {
        if (NULL != seen_name[i] && 0 == strcmp(name, seen_name[i])) {
            return i;
        }
    }
    return -1;
}

/* Does the pair named \c name carry exactly \c value?  A NULL \c value
 * asks for a pair that was delivered with no value at all. */
static int value_is(const char *name, const char *value)
{
    int idx = find_seen(name);

    if (0 > idx) {
        return 0;
    }
    if (NULL == value) {
        return NULL == seen_value[idx];
    }
    return NULL != seen_value[idx] && 0 == strcmp(value, seen_value[idx]);
}

static char tmpfile_path[512];

static int write_file(const char *contents)
{
    FILE *fp;

    fp = fopen(tmpfile_path, "w");
    if (NULL == fp) {
        return -1;
    }
    fputs(contents, fp);
    fclose(fp);
    return 0;
}

/*
 * The CRLF and byte-order-mark fixtures below have to reach the parser
 * byte for byte, so they are written in binary mode and with an explicit
 * length rather than through fputs().
 */
static int write_file_raw(const char *contents, size_t len)
{
    FILE *fp;

    fp = fopen(tmpfile_path, "wb");
    if (NULL == fp) {
        return -1;
    }
    fwrite(contents, 1, len, fp);
    fclose(fp);
    return 0;
}

/* Parse \c contents and leave the result in seen_*.  Returns the parse's
 * return code, or PMIX_ERROR if the fixture could not even be written. */
static int parse_text(const char *contents)
{
    reset_seen();
    if (0 != write_file(contents)) {
        return PMIX_ERROR;
    }
    return pmix_util_keyval_parse(tmpfile_path, collect, NULL);
}

static void test_basic_pairs(void)
{
    int rc, idx;

    if (0 != write_file("# a comment\n"
                        "alpha = one\n"
                        "\n"
                        "beta = two three\n")) {
        report("could not write the fixture file", 0);
        return;
    }

    reset_seen();
    rc = pmix_util_keyval_parse(tmpfile_path, collect, NULL);
    report("a well-formed file parses", PMIX_SUCCESS == rc);

    idx = find_seen("alpha");
    report("first pair is delivered",
           0 <= idx && NULL != seen_value[idx] && 0 == strcmp("one", seen_value[idx]));
    idx = find_seen("beta");
    report("a value with a space in it is delivered whole",
           0 <= idx && NULL != seen_value[idx] && 0 == strcmp("two three", seen_value[idx]));
    report("the comment and the blank line are not pairs", 2 == nseen);
    reset_seen();
}

/*
 * The caller (pmix_mca_base_var_cache_files) treats PMIX_ERR_NOT_FOUND
 * as "no such file, carry on", and every other code as fatal, so an
 * absent file has to answer with exactly that one.
 */
static void test_missing_file(void)
{
    char missing[600];
    int rc;

    snprintf(missing, sizeof(missing), "%s.does.not.exist", tmpfile_path);
    reset_seen();
    rc = pmix_util_keyval_parse(missing, collect, NULL);
    report("a missing file answers NOT_FOUND", PMIX_ERR_NOT_FOUND == rc);
    report("a missing file delivers no pairs", 0 == nseen);
    reset_seen();
}

/*
 * "-x FOO=bar" lines are not delivered as they are read.  They pile up
 * in one process-global string that is handed over, and released, by
 * pmix_util_keyval_save_internal_envars().
 */
static void test_env_directives(void)
{
    int rc, idx;

    if (0 != write_file("-x FOO=bar\n"
                        "-x BAZ=qux\n")) {
        report("could not write the fixture file", 0);
        return;
    }

    reset_seen();
    rc = pmix_util_keyval_parse(tmpfile_path, collect, NULL);
    report("a file of -x directives parses", PMIX_SUCCESS == rc);
    report("-x directives are not delivered by the parse", 0 == nseen);

    rc = pmix_util_keyval_save_internal_envars(collect, NULL);
    report("saving the internal envars succeeds", PMIX_SUCCESS == rc);
    idx = find_seen("mca_base_env_list_internal");
    report("both -x directives arrive in one pair",
           0 <= idx && NULL != seen_value[idx]
               && NULL != strstr(seen_value[idx], "FOO=bar")
               && NULL != strstr(seen_value[idx], "BAZ=qux"));

    /* the hand-off releases the string, so a second ask delivers nothing */
    reset_seen();
    rc = pmix_util_keyval_save_internal_envars(collect, NULL);
    report("a second save delivers nothing", PMIX_SUCCESS == rc && 0 == nseen);
    reset_seen();
}

/*
 * The regression this file exists for.  pmix_util_keyval_parse_finalize()
 * released the key buffer but not the accumulated -x string, and
 * pmix_init_util() runs again after pmix_finalize_util() - so a run that
 * ended without reaching the hand-off (pmix_mca_base_var_cache_files
 * returns on the first file that fails to parse, before the store) left
 * its variables standing, and the *next* run was handed them.
 */
static void test_finalize_drops_pending_envars(void)
{
    int rc;

    if (0 != write_file("-x LEAKED=fromfirstrun\n")) {
        report("could not write the fixture file", 0);
        return;
    }

    reset_seen();
    rc = pmix_util_keyval_parse(tmpfile_path, collect, NULL);
    report("the first run parses its -x directive", PMIX_SUCCESS == rc);

    /* end the run without ever asking for the accumulated variables */
    pmix_util_keyval_parse_finalize();
    rc = pmix_util_keyval_parse_init();
    report("the parser re-initializes", PMIX_SUCCESS == rc);

    reset_seen();
    rc = pmix_util_keyval_save_internal_envars(collect, NULL);
    report("a new run is not handed the previous run's -x variables",
           PMIX_SUCCESS == rc && -1 == find_seen("mca_base_env_list_internal"));
    reset_seen();
}

/*
 * Comment forms.  "#" is the documented one; "//" and block comments are
 * undocumented but have always been accepted, and a parameter file in the
 * wild may use them.
 */
static void test_comment_forms(void)
{
    report("a # comment line is not a pair",
           PMIX_SUCCESS == parse_text("# nope\n") && 0 == nseen);
    report("a // comment line is not a pair",
           PMIX_SUCCESS == parse_text("// nope\n") && 0 == nseen);
    report("an indented # comment is not a pair",
           PMIX_SUCCESS == parse_text("    # nope\na = 1\n")
               && 1 == nseen && value_is("a", "1"));
    report("a block comment on one line is not a pair",
           PMIX_SUCCESS == parse_text("/* nope */\na = 1\n")
               && 1 == nseen && value_is("a", "1"));
    report("a block comment spanning lines is not a pair",
           PMIX_SUCCESS == parse_text("/* nope\n still nope */\na = 1\n")
               && 1 == nseen && value_is("a", "1"));
    reset_seen();
}

/*
 * What a value is.  The documented rule is "everything to the right of
 * the =", with no quote processing of any kind - docs/mca.rst promises
 * that param1 = value with multiple words and param2 = "value with
 * multiple words" are *different* values.
 */
static void test_value_shapes(void)
{
    report("a value keeps its embedded spaces",
           PMIX_SUCCESS == parse_text("a = one two three\n")
               && value_is("a", "one two three"));
    report("trailing whitespace is trimmed off a value",
           PMIX_SUCCESS == parse_text("a = one two   \n")
               && value_is("a", "one two"));
    report("leading whitespace is trimmed off a value",
           PMIX_SUCCESS == parse_text("a =    one\n") && value_is("a", "one"));
    report("quotes are part of the value, not delimiters",
           PMIX_SUCCESS == parse_text("a = \"one two\"\n")
               && value_is("a", "\"one two\""));
    report("a # inside a value does not start a comment",
           PMIX_SUCCESS == parse_text("a = one # two\n")
               && value_is("a", "one # two"));
    report("a // inside a value does not start a comment",
           PMIX_SUCCESS == parse_text("a = one // two\n")
               && value_is("a", "one // two"));
    report("a second = is part of the value",
           PMIX_SUCCESS == parse_text("a = b=c\n") && value_is("a", "b=c"));
    report("no whitespace around the = is fine",
           PMIX_SUCCESS == parse_text("a=1\n") && value_is("a", "1"));
    report("tabs around the = are fine",
           PMIX_SUCCESS == parse_text("a\t=\t1\n") && value_is("a", "1"));
    report("a key with nothing after the = has no value",
           PMIX_SUCCESS == parse_text("a =\n") && value_is("a", NULL));
    report("a key followed only by whitespace has no value",
           PMIX_SUCCESS == parse_text("a =    \n") && value_is("a", NULL));
    report("dots, dashes and underscores are key characters",
           PMIX_SUCCESS == parse_text("a.b-c_d = 1\n") && value_is("a.b-c_d", "1"));
    report("a non-ASCII value is delivered intact",
           PMIX_SUCCESS == parse_text("a = caf\xc3\xa9" "\n")
               && value_is("a", "caf\xc3\xa9" ""));
    reset_seen();
}

/*
 * A parameter file whose last line has no newline is ordinary - an editor
 * that does not add one, a here-doc, a generated file.  The pair on that
 * line still counts.
 */
static void test_eof_without_newline(void)
{
    report("a pair on an unterminated last line is delivered",
           PMIX_SUCCESS == parse_text("a = 1") && value_is("a", "1"));
    report("a pair before an unterminated last line is delivered",
           PMIX_SUCCESS == parse_text("a = 1\nb = 2") && 2 == nseen
               && value_is("a", "1") && value_is("b", "2"));
    reset_seen();
}

/*
 * An empty file, and a file that is nothing but blank lines, are both
 * ordinary: most of the default parameter files look like this.
 */
static void test_empty_input(void)
{
    report("an empty file parses and yields nothing",
           PMIX_SUCCESS == parse_text("") && 0 == nseen);
    report("a file of blank lines parses and yields nothing",
           PMIX_SUCCESS == parse_text("\n\n   \n\t\n") && 0 == nseen);
    reset_seen();
}

/*
 * The line number handed to the callback is what tells a user which line
 * of which file set a parameter, so it has to survive the lines that
 * produce no pair at all.
 */
static void test_lineno(void)
{
    int idx;

    if (PMIX_SUCCESS != parse_text("# one\n"
                                   "\n"
                                   "a = 1\n"
                                   "/* four\n"
                                   "   five */\n"
                                   "b = 2\n")) {
        report("the line-number fixture parses", 0);
        return;
    }
    idx = find_seen("a");
    report("a pair after a comment and a blank line reports line 3",
           0 <= idx && 3 == seen_lineno[idx]);
    idx = find_seen("b");
    report("a pair after a two-line block comment reports line 6",
           0 <= idx && 6 == seen_lineno[idx]);
    reset_seen();
}

/*
 * A key is [A-Za-z0-9_.-]+.  Anything else on the left of the = is a
 * malformed line, and a malformed line must be *dropped* - the whole
 * line.  The flex scanner resumed in the middle of it instead, so
 * "a:b = 1" set the parameter "b", and "a+b = 1" did too: a typo in a
 * key quietly set some other parameter rather than being reported.
 */
static void test_malformed_key_drops_the_whole_line(void)
{
    report("a key containing ':' delivers no pair at all",
           PMIX_SUCCESS == parse_text("a:b = 1\n") && 0 == nseen);
    report("a key containing '+' delivers no pair at all",
           PMIX_SUCCESS == parse_text("a+b = 1\n") && 0 == nseen);
    report("a non-ASCII key delivers no pair at all",
           PMIX_SUCCESS == parse_text("caf\xc3\xa9" " = 1\n") && 0 == nseen);
    report("a line with no = at all delivers no pair",
           PMIX_SUCCESS == parse_text("a\n") && 0 == nseen);
    report("a line with nothing before the = delivers no pair",
           PMIX_SUCCESS == parse_text("= 1\n") && 0 == nseen);
    report("a malformed line does not stop the ones after it",
           PMIX_SUCCESS == parse_text("a:b = 1\nc = 2\n")
               && 1 == nseen && value_is("c", "2"));
    reset_seen();
}

/*
 * A parameter file edited on Windows, or on a share mounted from one,
 * arrives with CRLF line endings.  The carriage return is part of the
 * line ending, not part of the value: the flex scanner left it on the
 * end, so "ptl_base_verbose = 10" silently became the string "10\r".
 */
static void test_crlf_line_endings(void)
{
    static const char crlf[] = "a = 1\r\nb = two words\r\n";

    reset_seen();
    if (0 != write_file_raw(crlf, sizeof(crlf) - 1)) {
        report("could not write the CRLF fixture", 0);
        return;
    }
    report("a CRLF file parses",
           PMIX_SUCCESS == pmix_util_keyval_parse(tmpfile_path, collect, NULL));
    report("a CRLF line ending is not part of the value", value_is("a", "1"));
    report("a CRLF line ending is not part of a multi-word value",
           value_is("b", "two words"));
    reset_seen();
}

/*
 * An editor that writes a UTF-8 byte-order mark puts three bytes in front
 * of the first key.  They are not part of it.  The flex scanner rejected
 * them one at a time and took the rest of the first line down with them.
 */
static void test_utf8_bom(void)
{
    static const char bom[] = "\xef\xbb\xbf" "a = 1\nb = 2\n";

    reset_seen();
    if (0 != write_file_raw(bom, sizeof(bom) - 1)) {
        report("could not write the BOM fixture", 0);
        return;
    }
    report("a file with a byte-order mark parses",
           PMIX_SUCCESS == pmix_util_keyval_parse(tmpfile_path, collect, NULL));
    report("a byte-order mark does not swallow the first pair",
           value_is("a", "1"));
    report("the pair after a byte-order mark is unaffected", value_is("b", "2"));
    reset_seen();
}

/*
 * "-mca name value" is the command-line spelling of "name = value".  It is
 * undocumented, but it has always been accepted here and a file in the
 * wild may use it, so it keeps working.  Unlike a "name = value" line, a
 * quoted value here *is* unquoted, and more than one directive may share
 * a line.
 */
static void test_mca_directives(void)
{
    report("-mca sets the named parameter",
           PMIX_SUCCESS == parse_text("-mca foo bar\n") && value_is("foo", "bar"));
    report("--mca sets the named parameter",
           PMIX_SUCCESS == parse_text("--mca foo bar\n") && value_is("foo", "bar"));
    report("two -mca directives may share a line",
           PMIX_SUCCESS == parse_text("-mca foo bar -mca baz qux\n")
               && 2 == nseen && value_is("foo", "bar") && value_is("baz", "qux"));
    report("a single-quoted -mca value is unquoted",
           PMIX_SUCCESS == parse_text("-mca foo 'bar baz' \n")
               && value_is("foo", "bar baz"));
    report("a double-quoted -mca value is unquoted",
           PMIX_SUCCESS == parse_text("-mca foo \"bar baz\" \n")
               && value_is("foo", "bar baz"));
    /* the flex rule for a quoted value required whitespace after the
     * closing quote, so a value that ended the line was truncated at the
     * first space inside the quotes */
    report("a quoted -mca value that ends the line is not truncated",
           PMIX_SUCCESS == parse_text("-mca foo 'bar baz'\n")
               && value_is("foo", "bar baz"));
    report("-mca with no value delivers no pair",
           PMIX_SUCCESS == parse_text("-mca foo\n") && 0 == nseen);
    report("an -mca line does not disturb the line after it",
           PMIX_SUCCESS == parse_text("-mca foo bar\nb = 2\n")
               && 2 == nseen && value_is("foo", "bar") && value_is("b", "2"));
    reset_seen();
}

/*
 * The "-x" directives accumulate rather than being delivered as they are
 * read; test_env_directives() above covers the ordinary case.  What is
 * here is the edge the flex trailing-context rule got wrong: a bare "-x
 * NAME" as the last line of a file with no closing newline lost its last
 * character, so "-x FOO" stored "FO".
 */
static void test_env_directive_edges(void)
{
    int idx;

    reset_seen();
    report("a bare -x at end of file parses",
           PMIX_SUCCESS == parse_text("-x FOO"));
    pmix_util_keyval_save_internal_envars(collect, NULL);
    idx = find_seen("mca_base_env_list_internal");
    report("a bare -x at end of file keeps its last character",
           0 <= idx && NULL != seen_value[idx]
               && 0 == strcmp("FOO", seen_value[idx]));

    reset_seen();
    report("-x NAME=VALUE at end of file parses",
           PMIX_SUCCESS == parse_text("-x FOO=bar"));
    pmix_util_keyval_save_internal_envars(collect, NULL);
    idx = find_seen("mca_base_env_list_internal");
    report("-x NAME=VALUE at end of file keeps its last character",
           0 <= idx && NULL != seen_value[idx]
               && 0 == strcmp("FOO=bar", seen_value[idx]));

    reset_seen();
    report("--x is accepted as well as -x",
           PMIX_SUCCESS == parse_text("--x FOO=bar\n"));
    pmix_util_keyval_save_internal_envars(collect, NULL);
    report("--x stores the variable",
           value_is("mca_base_env_list_internal", "FOO=bar"));

    reset_seen();
    report("an = inside an -x value is kept",
           PMIX_SUCCESS == parse_text("-x FOO=a=b\n"));
    pmix_util_keyval_save_internal_envars(collect, NULL);
    report("an -x value may contain an =",
           value_is("mca_base_env_list_internal", "FOO=a=b"));
    reset_seen();
}

/*
 * Nothing in a parameter file is length-limited.  A key or a value longer
 * than any buffer the parser starts with has to come through whole.
 */
static void test_long_lines(void)
{
    char *big;
    char *text;
    size_t n = 8192;
    int ok;

    big = malloc(n + 1);
    if (NULL == big) {
        report("could not allocate the long-line fixture", 0);
        return;
    }
    memset(big, 'x', n);
    big[n] = '\0';

    if (0 > pmix_asprintf(&text, "a = %s\n", big)) {
        report("could not build the long-line fixture", 0);
        free(big);
        return;
    }
    ok = (PMIX_SUCCESS == parse_text(text)) && value_is("a", big);
    report("an 8K value comes through whole", ok);
    free(text);

    if (0 > pmix_asprintf(&text, "%s = 1\n", big)) {
        report("could not build the long-key fixture", 0);
        free(big);
        return;
    }
    ok = (PMIX_SUCCESS == parse_text(text)) && value_is(big, "1");
    report("an 8K key comes through whole", ok);
    free(text);
    free(big);
    reset_seen();
}

int main(int argc, char **argv)
{
    const char *tmpdir;
    int rc;

    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    tmpdir = getenv("TMPDIR");
    if (NULL == tmpdir || '\0' == tmpdir[0]) {
        tmpdir = "/tmp";
    }
    snprintf(tmpfile_path, sizeof(tmpfile_path), "%s/pmix_util_keyval_test.%ld", tmpdir,
             (long) getpid());

    rc = pmix_util_keyval_parse_init();
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "pmix_util_keyval_parse_init failed: %d\n", rc);
        return 1;
    }

    fprintf(stdout, "\n=== pmix_util_keyval_parse unit tests ===\n\n");
    test_basic_pairs();
    test_missing_file();
    test_comment_forms();
    test_value_shapes();
    test_eof_without_newline();
    test_empty_input();
    test_lineno();
    test_malformed_key_drops_the_whole_line();
    test_crlf_line_endings();
    test_utf8_bom();
    test_mca_directives();
    test_env_directives();
    test_env_directive_edges();
    test_long_lines();
    test_finalize_drops_pending_envars();

    pmix_util_keyval_parse_finalize();
    reset_seen();
    unlink(tmpfile_path);

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);
    return (nfail > 0) ? 1 : 0;
}
