/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit tests for pmix_output stream facility:
 *   pmix_output_init, pmix_output_open, pmix_output_close,
 *   pmix_output_switch, pmix_output_set/get_verbosity,
 *   pmix_output_set_output_file_info.
 *
 * PMIx_Init is called first so the output subsystem is already up;
 * PMIX_ERR_UNREACH is treated as a normal no-server situation.
 *
 * Exit 0 if all tests pass, 1 otherwise.
 */

#include "src/include/pmix_config.h"
#include "src/include/pmix_globals.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>

#include "pmix.h"
#include "src/util/pmix_environ.h"
#include "src/util/pmix_output.h"

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
/* pmix_output_init (idempotency after PMIx_Init)                     */
/* ------------------------------------------------------------------ */

static void test_output_init_idempotent(void)
{
    /* PMIx_Init already called pmix_output_init internally.
     * Re-calling it must return true without side effects. */
    report("output_init_idempotent", pmix_output_init());
}

/* ------------------------------------------------------------------ */
/* pmix_output_open / pmix_output_close                               */
/* ------------------------------------------------------------------ */

static void test_output_open_null_lds(void)
{
    int h = pmix_output_open(NULL);
    report("output_open_null_lds: valid handle",
           h >= 0 && h < PMIX_OUTPUT_MAX_STREAMS);
    pmix_output_close(h);
}

static void test_output_open_stderr(void)
{
    pmix_output_stream_t lds;
    int h;

    PMIX_CONSTRUCT(&lds, pmix_output_stream_t);
    lds.lds_want_stderr = true;
    h = pmix_output_open(&lds);
    report("output_open_stderr: valid handle",
           h >= 0 && h < PMIX_OUTPUT_MAX_STREAMS);
    PMIX_DESTRUCT(&lds);
    pmix_output_close(h);
}

static void test_output_open_stdout(void)
{
    pmix_output_stream_t lds;
    int h;

    PMIX_CONSTRUCT(&lds, pmix_output_stream_t);
    lds.lds_want_stdout = true;
    h = pmix_output_open(&lds);
    report("output_open_stdout: valid handle",
           h >= 0 && h < PMIX_OUTPUT_MAX_STREAMS);
    PMIX_DESTRUCT(&lds);
    pmix_output_close(h);
}

static void test_output_close_invalid_no_crash(void)
{
    /* Closing an out-of-range handle must not crash. */
    pmix_output_close(-1);
    pmix_output_close(PMIX_OUTPUT_MAX_STREAMS);
    report("output_close_invalid: no crash", 1);
}

/* ------------------------------------------------------------------ */
/* pmix_output_switch                                                  */
/* ------------------------------------------------------------------ */

static void test_output_switch(void)
{
    pmix_output_stream_t lds;
    bool prev;
    int h;

    PMIX_CONSTRUCT(&lds, pmix_output_stream_t);
    lds.lds_want_stderr = true;
    h = pmix_output_open(&lds);
    PMIX_DESTRUCT(&lds);

    /* Newly opened stream is enabled; disabling returns true (was enabled). */
    prev = pmix_output_switch(h, false);
    report("output_switch_disable: was enabled", prev == true);

    /* Re-enabling returns false (was disabled). */
    prev = pmix_output_switch(h, true);
    report("output_switch_reenable: was disabled", prev == false);

    pmix_output_close(h);
}

/* Disabling a stream does not close it - pmix_output_switch() only
 * discards what is written to it - so a close that follows must still
 * release the descriptor's strdup'ed strings and give the slot back.
 * When the close was gated on the stream being enabled, the slot stayed
 * marked "used" forever and the second open landed somewhere else. */
static void test_output_close_disabled_reclaims_slot(void)
{
    pmix_output_stream_t lds;
    int a, b;

    PMIX_CONSTRUCT(&lds, pmix_output_stream_t);
    lds.lds_want_stderr = true;
    lds.lds_prefix = strdup("[disabled-stream] ");

    a = pmix_output_open(&lds);
    pmix_output_switch(a, false);
    pmix_output_close(a);

    b = pmix_output_open(&lds);

    free(lds.lds_prefix);
    lds.lds_prefix = NULL;
    PMIX_DESTRUCT(&lds);

    report("output_close_disabled: slot reclaimed", 0 <= a && a == b);
    pmix_output_close(b);
}

/* ------------------------------------------------------------------ */
/* file streams sharing one output file                                */
/* ------------------------------------------------------------------ */

/* Two streams naming the same output file share the open file rather
 * than opening it twice (a second O_TRUNC open would discard what the
 * first had written). They must not share the *descriptor*: closing
 * either one then closed the file out from under the other, which went
 * on writing into whatever descriptor the OS handed out next. */
static void test_output_file_shared_between_streams(void)
{
    pmix_output_stream_t lds;
    char *olddir = NULL, *oldprefix = NULL;
    char prefix[64], path[PMIX_PATH_MAX], buf[4096];
    const char *dir;
    int a, b, fd;
    ssize_t n;

    dir = pmix_tmp_directory();
    snprintf(prefix, sizeof(prefix), "util-output-%d-", (int) getpid());
    pmix_output_set_output_file_info(dir, prefix, &olddir, &oldprefix);
    snprintf(path, sizeof(path), "%s/%sshared.txt", dir, prefix);

    PMIX_CONSTRUCT(&lds, pmix_output_stream_t);
    lds.lds_want_file = true;
    lds.lds_file_suffix = strdup("shared.txt");
    a = pmix_output_open(&lds);
    b = pmix_output_open(&lds);
    PMIX_DESTRUCT(&lds);

    if (0 > a || 0 > b) {
        report("output_file_shared: streams opened", 0);
        goto restore;
    }

    /* the first write is what actually opens the file; the second joins
     * the file the first one opened */
    pmix_output(a, "AAA-opener");
    pmix_output(b, "BBB-joiner");

    pmix_output_close(a);
    pmix_output(b, "CCC-after-peer-close");
    pmix_output_close(b);

    memset(buf, 0, sizeof(buf));
    fd = open(path, O_RDONLY);
    if (0 > fd) {
        report("output_file_shared: file created", 0);
        goto restore;
    }
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    unlink(path);
    if (0 > n) {
        report("output_file_shared: file readable", 0);
        goto restore;
    }

    report("output_file_shared: opener wrote", NULL != strstr(buf, "AAA-opener"));
    report("output_file_shared: joiner wrote", NULL != strstr(buf, "BBB-joiner"));
    report("output_file_shared: joiner survives peer close",
           NULL != strstr(buf, "CCC-after-peer-close"));

restore:
    pmix_output_set_output_file_info(olddir, oldprefix, NULL, NULL);
    free(olddir);
    free(oldprefix);
}

/* ------------------------------------------------------------------ */
/* pmix_output_set/get_verbosity                                       */
/* ------------------------------------------------------------------ */

static void test_output_verbosity(void)
{
    pmix_output_stream_t lds;
    int h;

    PMIX_CONSTRUCT(&lds, pmix_output_stream_t);
    lds.lds_want_stderr = true;
    h = pmix_output_open(&lds);
    PMIX_DESTRUCT(&lds);

    pmix_output_set_verbosity(h, 5);
    report("verbosity_set_get: set 5 → get 5", 5 == pmix_output_get_verbosity(h));

    pmix_output_set_verbosity(h, 0);
    report("verbosity_set_get: set 0 → get 0", 0 == pmix_output_get_verbosity(h));

    pmix_output_close(h);
}

/* ------------------------------------------------------------------ */
/* pmix_output_set_output_file_info                                   */
/* ------------------------------------------------------------------ */

static void test_output_file_info(void)
{
    char *olddir = NULL;
    char *oldprefix = NULL;

    pmix_output_set_output_file_info("/tmp", "unit-test-prefix-", &olddir, &oldprefix);
    report("output_file_info: old dir returned", olddir != NULL);
    report("output_file_info: old prefix returned", oldprefix != NULL);

    /* Restore previous values. */
    pmix_output_set_output_file_info(olddir, oldprefix, NULL, NULL);
    free(olddir);
    free(oldprefix);
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    pmix_proc_t myproc;
    pmix_status_t rc;
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    rc = PMIx_Init(&myproc, NULL, 0);
    if (PMIX_SUCCESS != rc && PMIX_ERR_UNREACH != rc) {
        fprintf(stderr, "PMIx_Init: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    fprintf(stdout, "\n=== pmix_output unit tests ===\n\n");

    test_output_init_idempotent();
    test_output_open_null_lds();
    test_output_open_stderr();
    test_output_open_stdout();
    test_output_close_invalid_no_crash();
    test_output_switch();
    test_output_close_disabled_reclaims_slot();
    test_output_file_shared_between_streams();
    test_output_verbosity();
    test_output_file_info();

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);

    PMIx_Finalize(NULL, 0);
    return (nfail > 0) ? 1 : 0;
}
