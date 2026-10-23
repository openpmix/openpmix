/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * A struct timeval prints as seconds and a fraction of a second. The
 * microseconds are that fraction, so they keep their leading zeros: the
 * printer wrote them with a bare %lu, and 0.074472 seconds of CPU time
 * read as "0.74472" - ten times as much. Seen in a process monitor's
 * PMIX_PROC_TIME, which is printed through exactly this path.
 */

#include "src/include/pmix_config.h"
#include "include/pmix.h"
#include "include/pmix_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

static pmix_server_module_t mymodule = {
    .client_connected = NULL
};

static int npass = 0;
static int nfail = 0;

static void report(const char *name, int passed, const char *got)
{
    if (passed) {
        fprintf(stdout, "  PASS: %s\n", name);
        npass++;
    } else {
        fprintf(stdout, "  FAIL: %s (got \"%s\")\n", name, (NULL == got) ? "NULL" : got);
        nfail++;
    }
}

/* the printed value must end with want */
static void check(const char *name, long sec, long usec, const char *want)
{
    struct timeval tv;
    pmix_value_t val;
    char *s;
    size_t sl, wl;

    tv.tv_sec = sec;
    tv.tv_usec = usec;
    PMIX_VALUE_CONSTRUCT(&val);
    PMIx_Value_load(&val, &tv, PMIX_TIMEVAL);
    s = PMIx_Value_string(&val);
    sl = (NULL == s) ? 0 : strlen(s);
    wl = strlen(want);
    report(name, NULL != s && sl >= wl && 0 == strcmp(s + sl - wl, want), s);
    free(s);
    PMIX_VALUE_DESTRUCT(&val);
}

int main(int argc, char **argv)
{
    pmix_status_t rc;

    (void) argc;
    (void) argv;

    rc = PMIx_server_init(&mymodule, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    fprintf(stdout, "\n=== bfrops timeval printing ===\n\n");

    check("a fraction under a tenth keeps its leading zero", 0, 74472, "Value: 0.074472");
    check("a few microseconds are a few millionths", 1, 5, "Value: 1.000005");
    check("a whole number of seconds shows six zeros", 3, 0, "Value: 3.000000");
    check("six significant digits print as they are", 12, 345678, "Value: 12.345678");

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);

    PMIx_server_finalize();

    return (nfail > 0) ? 1 : 0;
}
