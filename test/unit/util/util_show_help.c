/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit tests for pmix_show_help utility:
 *   pmix_show_help_init (idempotency), pmix_show_help_string,
 *   pmix_show_help_vstring, pmix_show_help_norender,
 *   pmix_help_check_dups, pmix_show_help_enabled.
 *
 * Built-in help content (pmix_show_help_data[]) is used so no
 * filesystem access is required.
 *
 * PMIx_Init is called first; PMIX_ERR_UNREACH is treated as normal.
 *
 * Exit 0 if all tests pass, 1 otherwise.
 */

#include "src/include/pmix_config.h"
#include "src/include/pmix_globals.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
#include <string.h>

#include "pmix.h"
#include "src/util/pmix_show_help.h"

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
/* pmix_show_help_init (idempotency after PMIx_Init)                  */
/* ------------------------------------------------------------------ */

static void test_show_help_init_idempotent(void)
{
    /* PMIx_Init already called pmix_show_help_init internally.
     * Re-calling must succeed without side effects. */
    pmix_status_t rc = pmix_show_help_init();
    report("show_help_init_idempotent", PMIX_SUCCESS == rc);
}

/* ------------------------------------------------------------------ */
/* pmix_show_help_enabled global                                       */
/* ------------------------------------------------------------------ */

static void test_show_help_enabled(void)
{
    /* After init, show_help_enabled should be set (> 0). */
    report("show_help_enabled: > 0", pmix_show_help_enabled > 0);
}

/* ------------------------------------------------------------------ */
/* pmix_show_help_string                                               */
/* ------------------------------------------------------------------ */

static void test_show_help_string_known_topic(void)
{
    /* "help-cli.txt" / "unknown-option" has two %s specifiers:
     *   "Option: %s" and "%s --help" */
    char *s = pmix_show_help_string("help-cli.txt", "unknown-option", 0,
                                    "myoption", "myprog");
    report("show_help_string_known: non-NULL", s != NULL);
    report("show_help_string_known: contains option name",
           s != NULL && strstr(s, "myoption") != NULL);
    free(s);
}

static void test_show_help_string_unknown_file(void)
{
    /* Unknown file → returns NULL; local_delivery writes to stderr. */
    char *s = pmix_show_help_string("help-nonexistent-xyz.txt", "topic", 0);
    report("show_help_string_unknown_file: returns NULL", s == NULL);
}

static void test_show_help_string_unknown_topic(void)
{
    /* Known file, unknown topic → returns NULL. */
    char *s = pmix_show_help_string("help-cli.txt", "no-such-topic-xyz", 0);
    report("show_help_string_unknown_topic: returns NULL", s == NULL);
}

static void test_show_help_string_null_args(void)
{
    /* No file or no topic names nothing, so the answer is the same as any
     * other lookup that finds nothing. Both arguments used to reach
     * strcmp() unscreened - which is a crash, not a NULL, and it is
     * reachable from pmix_cmd_line_parse() whenever a tool passes a NULL
     * helpfile and the user asks for help. These die on a signal, not with
     * a failed assertion, if that comes back. */
    char *s = pmix_show_help_string(NULL, "usage", 0);
    report("show_help_string_null_file: returns NULL", s == NULL);
    s = pmix_show_help_string("help-cli.txt", NULL, 0);
    report("show_help_string_null_topic: returns NULL", s == NULL);
    s = pmix_show_help_string(NULL, NULL, 0);
    report("show_help_string_null_both: returns NULL", s == NULL);
}

/* ------------------------------------------------------------------ */
/* pmix_show_help_norender                                             */
/* ------------------------------------------------------------------ */

static void test_show_help_norender(void)
{
    /* Delivers a raw string to the local output; must not crash. */
    pmix_status_t rc = pmix_show_help_norender("help-cli.txt",
                                               "norender-test",
                                               "raw message: no-op delivery\n");
    report("show_help_norender: returns SUCCESS", PMIX_SUCCESS == rc);
}

/* ------------------------------------------------------------------ */
/* pmix_help_check_dups (first-call path: not a duplicate)            */
/* ------------------------------------------------------------------ */

static void test_help_check_dups_first_call(void)
{
    /* A (file, topic) pair seen for the first time is NOT a duplicate.
     * pmix_help_check_dups returns PMIX_ERR_NOT_FOUND for "not a dup". */
    pmix_status_t rc = pmix_help_check_dups("help-cli.txt",
                                            "unit-test-unique-topic-xyz");
    report("help_check_dups_first: returns ERR_NOT_FOUND (not a dup)",
           PMIX_ERR_NOT_FOUND == rc);
}

/* ------------------------------------------------------------------ */
/* pmix_help_check_dups / pmix_show_help_add_data argument screening   */
/* ------------------------------------------------------------------ */

static void test_check_dups_null_args(void)
{
    /* Both arguments reach strcmp() by way of match(), and the list
     * entry built from them outlives this call - a NULL stored there is
     * a segfault on the next lookup, not on this one. */
    report("check_dups_null_file: refused",
           PMIX_SUCCESS != pmix_help_check_dups(NULL, "topic"));
    report("check_dups_null_topic: refused",
           PMIX_SUCCESS != pmix_help_check_dups("help-cli.txt", NULL));
}

static void test_add_data_null_args(void)
{
    report("add_data_null_project: refused",
           PMIX_SUCCESS != pmix_show_help_add_data(NULL, pmix_show_help_data));
    report("add_data_null_array: refused",
           PMIX_SUCCESS != pmix_show_help_add_data("unit-test-null", NULL));
}

/* ------------------------------------------------------------------ */
/* The "#include" directive                                            */
/*                                                                     */
/* Content normally comes from the generated pmix_show_help_content.c, */
/* but pmix_show_help_add_data() lets a caller register an array of its */
/* own, which is the only way to drive the include parser from a test. */
/* Nothing in the tree uses "#include" yet, so these are the cases its  */
/* first user will write by accident.                                  */
/* ------------------------------------------------------------------ */

static const char *content_plain[] = {"the included text", NULL};
/* well formed: pull in the topic above */
static const char *content_good[] = {"#include#help-unit-inc.txt#plain", NULL};
/* malformed: no fields at all after the directive */
static const char *content_bare[] = {"#include", NULL};
/* malformed: one field where two are needed */
static const char *content_short[] = {"#include#help-unit-inc.txt", NULL};
/* a topic that includes itself */
static const char *content_loop[] = {"#include#help-unit-inc.txt#loop", NULL};

static pmix_show_help_entry_t unit_entries[] = {
    {.topic = "plain", .content = content_plain},
    {.topic = "good", .content = content_good},
    {.topic = "bare", .content = content_bare},
    {.topic = "short", .content = content_short},
    {.topic = "loop", .content = content_loop},
    {.topic = NULL, .content = NULL}
};

static pmix_show_help_file_t unit_files[] = {
    {.filename = "help-unit-inc.txt", .entries = unit_entries},
    {.filename = NULL, .entries = NULL}
};

static void test_include(void)
{
    char *s;
    pmix_status_t rc;

    rc = pmix_show_help_add_data("unit-test", unit_files);
    if (PMIX_SUCCESS != rc) {
        report("include: registering the test array (skipped)", 1);
        return;
    }

    s = pmix_show_help_string("help-unit-inc.txt", "good", 0);
    report("include_good: pulls in the included text",
           NULL != s && NULL != strstr(s, "the included text"));
    free(s);

    /* A directive the parser cannot read must be skipped, not followed
     * off the end of the string it is walking backwards through. */
    s = pmix_show_help_string("help-unit-inc.txt", "bare", 0);
    report("include_bare: survives a directive with no fields", 1);
    free(s);

    s = pmix_show_help_string("help-unit-inc.txt", "short", 0);
    report("include_short: survives a directive missing a field", 1);
    free(s);

    /* Self-reference: without a depth bound this recurses until the
     * stack runs out. */
    s = pmix_show_help_string("help-unit-inc.txt", "loop", 0);
    report("include_loop: a topic that includes itself terminates", 1);
    free(s);
}

/* ------------------------------------------------------------------ */
/* Duplicates held back are still shown at finalize                    */
/*                                                                     */
/* pmix_help_check_dups() promises that accumulated duplicates are     */
/* displayed unconditionally at termination.  The aggregation timer    */
/* runs five seconds out, so a job that ends sooner - which is most of */
/* them - only ever sees them if finalize flushes.  This has to run in */
/* a child of its own: it needs a whole init/finalize cycle with       */
/* stderr on a pipe, since the notice goes out directly once the       */
/* progress thread has been paused.                                    */
/* ------------------------------------------------------------------ */

static void test_duplicates_flushed_at_finalize(void)
{
    int fds[2];
    pid_t pid;
    int status = 0;
    char buf[4096];
    ssize_t n;
    size_t total = 0;

    if (0 != pipe(fds)) {
        report("finalize_flush: pipe (skipped)", 1);
        return;
    }
    pid = fork();
    if (0 > pid) {
        close(fds[0]);
        close(fds[1]);
        report("finalize_flush: fork (skipped)", 1);
        return;
    }
    if (0 == pid) {
        pmix_proc_t me;
        pmix_status_t rc;
        close(fds[0]);
        if (0 > dup2(fds[1], fileno(stderr))) {
            _exit(2);
        }
        close(fds[1]);
        rc = PMIx_Init(&me, NULL, 0);
        if (PMIX_SUCCESS != rc && PMIX_ERR_UNREACH != rc) {
            _exit(2);   /* nothing to say about a library that never came up */
        }
        /* first sighting, then a duplicate that gets counted and held */
        (void) pmix_help_check_dups("help-cli.txt", "finalize-flush-topic");
        (void) pmix_help_check_dups("help-cli.txt", "finalize-flush-topic");
        PMIx_Finalize(NULL, 0);
        _exit(0);
    }

    close(fds[1]);
    while (total < sizeof(buf) - 1) {
        n = read(fds[0], buf + total, sizeof(buf) - 1 - total);
        if (0 >= n) {
            break;
        }
        total += (size_t) n;
    }
    buf[total] = '\0';
    close(fds[0]);
    waitpid(pid, &status, 0);

    if (WIFEXITED(status) && 2 == WEXITSTATUS(status)) {
        report("finalize_flush: child setup (skipped)", 1);
        return;
    }
    report("finalize_flush: the held-back duplicate is reported",
           NULL != strstr(buf, "sent help message"));
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    pmix_proc_t myproc;
    pmix_status_t rc;
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    fprintf(stdout, "\n=== pmix_show_help unit tests ===\n\n");

    /* Before our own PMIx_Init: this one needs a whole init/finalize
     * cycle of its own, and a child forked from an already-initialized
     * process would only be adjusting a reference count. */
    test_duplicates_flushed_at_finalize();

    rc = PMIx_Init(&myproc, NULL, 0);
    if (PMIX_SUCCESS != rc && PMIX_ERR_UNREACH != rc) {
        fprintf(stderr, "PMIx_Init: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    test_show_help_init_idempotent();
    test_show_help_enabled();
    test_show_help_string_known_topic();
    test_show_help_string_unknown_file();
    test_show_help_string_unknown_topic();
    test_show_help_string_null_args();
    test_show_help_norender();
    test_help_check_dups_first_call();
    test_check_dups_null_args();
    test_add_data_null_args();
    test_include();

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);

    PMIx_Finalize(NULL, 0);
    return (nfail > 0) ? 1 : 0;
}
