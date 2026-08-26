/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit tests for src/util/pmix_timings.c.
 *
 * The whole subsystem is behind --enable-pmix-timing, so which half of
 * this file runs is a build-time decision. Both halves assert behavior
 * rather than the setting: with timing off, the macros must expand to
 * nothing and do nothing; with it on, a handler must be able to be
 * driven through the documented macros and reported on without taking
 * the process down. Asserting the setting itself - which this file used
 * to do - makes the test fail in the configuration it exists to cover.
 *
 * Exit 0 if all tests pass, 1 otherwise.
 */

#include "src/include/pmix_config.h"
#include "src/include/pmix_globals.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pmix.h"
#include "src/util/pmix_timings.h"

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
/* The macros, in whichever form this build compiled them              */
/* ------------------------------------------------------------------ */

PMIX_TIMING_DECLARE(macro_handle)

static void test_timing_macros(void)
{
    /* Every macro has to compile and be callable in both arms. With
     * timing off they expand to nothing; with it on they drive the real
     * handler declared above. Either way, running them must not crash. */
    PMIX_TIMING_INIT(&macro_handle);
    PMIX_TIMING_EVENT((&macro_handle, "a trace event"));
    PMIX_TIMING_MSTART((&macro_handle, "an interval"));
    PMIX_TIMING_MSTOP(&macro_handle);
    PMIX_TIMING_RELEASE(&macro_handle);
    report("macros: declare/init/event/mstart/mstop/release", 1);
}

/* ------------------------------------------------------------------ */
/* The enabled arm                                                     */
/* ------------------------------------------------------------------ */

#if PMIX_ENABLE_TIMING

/* Read a file back, so the assertions can be about what was written
 * rather than about a return code. */
static char *slurp(const char *path)
{
    long len;
    char *buf;
    FILE *fp = fopen(path, "r");

    if (NULL == fp) {
        return NULL;
    }
    if (0 != fseek(fp, 0, SEEK_END) || 0 > (len = ftell(fp))) {
        fclose(fp);
        return NULL;
    }
    rewind(fp);
    buf = calloc((size_t) len + 1, 1);
    if (NULL != buf && 0 < len) {
        if (1 != fread(buf, (size_t) len, 1, fp)) {
            free(buf);
            buf = NULL;
        }
    }
    fclose(fp);
    return buf;
}

/* An ordinary report has to name the node and the job. Both come out of
 * file-statics, and the node name was one that nothing ever assigned -
 * so every line named the node "(null)", and gcc, seeing the constant
 * NULL, refused to compile the file at all. */
static void test_report_names_the_node(void)
{
    char path[512];
    pmix_timing_t t;
    char *out;

    snprintf(path, sizeof(path), "/tmp/pmix-util-timings-%u-report.txt",
             (unsigned) getpid());
    unlink(path);

    pmix_init_id("myspace", 3);
    pmix_timing_init(&t);
    PMIX_TIMING_EVENT((&t, "a trace event"));
    report("report: writes a file", PMIX_SUCCESS == pmix_timing_report(&t, path));

    out = slurp(path);
    if (NULL == out) {
        report("report: file readable", 0);
    } else {
        report("report: node name is not (null)", NULL == strstr(out, "(null)"));
        report("report: carries the job id", NULL != strstr(out, "myspace:3"));
        report("report: carries the description", NULL != strstr(out, "a trace event"));
        free(out);
    }
    pmix_timing_release(&t);
    unlink(path);
}

/* pmix_init_id() replaces the string it holds, so calling it twice has
 * to release the first rather than leak it - and a second call must be
 * the one that shows up in the output. */
static void test_init_id_replaces(void)
{
    char path[512];
    pmix_timing_t t;
    char *out;

    snprintf(path, sizeof(path), "/tmp/pmix-util-timings-%u-id.txt",
             (unsigned) getpid());
    unlink(path);

    pmix_init_id("first", 1);
    pmix_init_id("second", 2);
    pmix_timing_init(&t);
    PMIX_TIMING_EVENT((&t, "e"));
    (void) pmix_timing_report(&t, path);

    out = slurp(path);
    if (NULL == out) {
        report("init_id: file readable", 0);
    } else {
        report("init_id: latest id is the one reported",
               NULL != strstr(out, "second:2") && NULL == strstr(out, "first:1"));
        free(out);
    }
    pmix_timing_release(&t);
    unlink(path);
}

/* A stop with no measurement running records an interval id of -1 -
 * pmix_timing_end() copies current_id, which is -1 when nothing was
 * started. Both consumers index the descriptor array by that id, and
 * only pmix_timing_deltas() screened for a negative one, so a report
 * read descr[-1] and printed whatever it found there. */
static void test_unmatched_stop_is_survivable(void)
{
    char path[512];
    pmix_timing_t t;

    snprintf(path, sizeof(path), "/tmp/pmix-util-timings-%u-unmatched.txt",
             (unsigned) getpid());
    unlink(path);

    pmix_timing_init(&t);
    /* no MSTART, and no interval was ever described, so next_id_cntr is
     * zero and the descriptor array is not even allocated */
    PMIX_TIMING_MSTOP(&t);
    report("unmatched stop: report survives it",
           PMIX_SUCCESS == pmix_timing_report(&t, path));
    report("unmatched stop: deltas survives it",
           PMIX_SUCCESS == pmix_timing_deltas(&t, path));
    pmix_timing_release(&t);
    unlink(path);
}

/* And the ordinary interval case, which is what deltas exists for. */
static void test_deltas_reports_an_interval(void)
{
    char path[512];
    pmix_timing_t t;
    char *out;

    snprintf(path, sizeof(path), "/tmp/pmix-util-timings-%u-deltas.txt",
             (unsigned) getpid());
    unlink(path);

    pmix_timing_init(&t);
    PMIX_TIMING_MSTART((&t, "measured interval"));
    PMIX_TIMING_MSTOP(&t);
    report("deltas: writes a file", PMIX_SUCCESS == pmix_timing_deltas(&t, path));

    out = slurp(path);
    if (NULL == out) {
        report("deltas: file readable", 0);
    } else {
        report("deltas: carries the interval description",
               NULL != strstr(out, "measured interval"));
        report("deltas: marks it as an overhead line",
               NULL != strstr(out, "[PMIX_OVHD]"));
        report("deltas: node name is not (null)", NULL == strstr(out, "(null)"));
        free(out);
    }
    pmix_timing_release(&t);
    unlink(path);
}

/* PMIX_TIMING_REPORT and PMIX_TIMING_DELTAS expand to a use of
 * pmix_timing_output, and the overhead accounting is switched by
 * pmix_timing_overhead. Both were declared only in
 * src/runtime/pmix_rte.h, which is not installed - so those two macros
 * did not compile for anyone outside this tree.
 *
 * This case is a COMPILE-time assertion wearing a runtime disguise: what
 * it pins is that the two names are reachable from a translation unit
 * that includes nothing but the installed pmix_timings.h. Move the
 * declarations back and this file stops building. (The matching runtime
 * defect - a file-static pmix_timing_overhead inside pmix_timings.c that
 * shadowed the registered one, leaving the parameter inert - is now
 * prevented by the compiler instead: a static definition following the
 * header's extern declaration is an error.) */
static void test_installed_header_declares_its_parameters(void)
{
    const bool saved_overhead = pmix_timing_overhead;
    char *const saved_output = pmix_timing_output;

    report("header: declares the parameters its macros use",
           saved_overhead == pmix_timing_overhead &&
           saved_output == pmix_timing_output);
}
#endif

/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    fprintf(stdout, "\n=== pmix_timings unit tests ===\n\n");

    test_timing_macros();
#if PMIX_ENABLE_TIMING
    test_report_names_the_node();
    test_init_id_replaces();
    test_unmatched_stop_is_survivable();
    test_deltas_reports_an_interval();
    test_installed_header_declares_its_parameters();
#else
    fprintf(stdout, "  (built without --enable-pmix-timing; the timing "
                    "subsystem itself is not compiled)\n");
#endif

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);
    return (nfail > 0) ? 1 : 0;
}
