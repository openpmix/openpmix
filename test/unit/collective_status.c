/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * White-box unit tests for pmix_server_set_collective_status().
 *
 * This is the routine the server's lost-connection accounting uses to record a
 * collective's completion status in the tracker's info array. The status slot
 * (PMIX_LOCAL_COLLECTIVE_STATUS) is seeded by the participant handlers. For the
 * fence and disconnect families it is the last element of the array; but the
 * connect handler appends per-participant endpoint info, and for a
 * cross-namespace connect job-level info, AFTER that slot. A positional write
 * to info[ninfo-1] would therefore clobber the appended info on connect and
 * leave the real status slot stale. The routine locates the slot by key to
 * avoid that.
 *
 * These tests assert the slot's contents directly:
 *
 *  connect layout   : status slot in the middle, endpoint + job-level info
 *                     after it -> the status slot is updated and the appended
 *                     info is left untouched.
 *  fence layout     : status slot last -> updated correctly.
 *  status absent    : no PMIX_LOCAL_COLLECTIVE_STATUS slot -> no-op, no change.
 *  degenerate input : NULL array / zero length -> no crash, no write.
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "src/include/pmix_globals.h"
#include "src/server/pmix_server_ops.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* keys used for the synthetic appended info */
#define ENDPT_KEY "test.endpt.data"
#define JOB_KEY   "test.job.data"
#define USER_KEY  "test.user.dir"

#define ENDPT_VAL 0xE11E11ULL
#define JOB_VAL   0x70B70BULL
#define USER_VAL  0xABCDEFULL

/* Build a connect-style tracker info array:
 *   [0] a user directive
 *   [1] PMIX_SORTED_PROC_ARRAY
 *   [2] PMIX_LOCAL_COLLECTIVE_STATUS (seeded to PMIX_SUCCESS)  <-- NOT last
 *   [3] appended endpoint info
 *   [4] appended job-level info
 */
static pmix_info_t *make_connect_layout(size_t *ninfo)
{
    pmix_info_t *info;
    pmix_status_t seed = PMIX_SUCCESS;
    uint64_t u;

    *ninfo = 5;
    PMIX_INFO_CREATE(info, 5);
    u = USER_VAL;
    PMIX_INFO_LOAD(&info[0], USER_KEY, &u, PMIX_UINT64);
    PMIX_INFO_LOAD(&info[1], PMIX_SORTED_PROC_ARRAY, NULL, PMIX_BOOL);
    PMIX_INFO_LOAD(&info[2], PMIX_LOCAL_COLLECTIVE_STATUS, &seed, PMIX_STATUS);
    u = ENDPT_VAL;
    PMIX_INFO_LOAD(&info[3], ENDPT_KEY, &u, PMIX_UINT64);
    u = JOB_VAL;
    PMIX_INFO_LOAD(&info[4], JOB_KEY, &u, PMIX_UINT64);
    return info;
}

static void test_connect_layout(void)
{
    pmix_info_t *info;
    size_t ninfo;
    int ok;

    info = make_connect_layout(&ninfo);
    pmix_server_set_collective_status(info, ninfo, PMIX_ERR_LOST_CONNECTION);

    /* the status slot (index 2, by key) must now carry the loss status */
    ok = (PMIX_CHECK_KEY(&info[2], PMIX_LOCAL_COLLECTIVE_STATUS)
          && PMIX_ERR_LOST_CONNECTION == info[2].value.data.status);
    report("connect layout: status slot updated by key", ok);

    /* the appended endpoint and job-level info must be untouched */
    ok = (PMIX_CHECK_KEY(&info[3], ENDPT_KEY)
          && PMIX_UINT64 == info[3].value.type
          && ENDPT_VAL == info[3].value.data.uint64);
    report("connect layout: appended endpoint info intact", ok);

    ok = (PMIX_CHECK_KEY(&info[4], JOB_KEY)
          && PMIX_UINT64 == info[4].value.type
          && JOB_VAL == info[4].value.data.uint64);
    report("connect layout: appended job-level info intact", ok);

    /* the untouched leading slots must remain what they were */
    ok = (PMIX_CHECK_KEY(&info[0], USER_KEY)
          && USER_VAL == info[0].value.data.uint64
          && PMIX_CHECK_KEY(&info[1], PMIX_SORTED_PROC_ARRAY));
    report("connect layout: leading slots intact", ok);

    PMIX_INFO_FREE(info, ninfo);
}

static void test_fence_layout(void)
{
    pmix_info_t *info;
    size_t ninfo = 3;
    pmix_status_t seed = PMIX_SUCCESS;
    uint64_t u = USER_VAL;
    int ok;

    /* fence/disconnect layout: status slot is the last element */
    PMIX_INFO_CREATE(info, 3);
    PMIX_INFO_LOAD(&info[0], USER_KEY, &u, PMIX_UINT64);
    PMIX_INFO_LOAD(&info[1], PMIX_SORTED_PROC_ARRAY, NULL, PMIX_BOOL);
    PMIX_INFO_LOAD(&info[2], PMIX_LOCAL_COLLECTIVE_STATUS, &seed, PMIX_STATUS);

    pmix_server_set_collective_status(info, ninfo, PMIX_ERR_PARTIAL_SUCCESS);

    ok = (PMIX_CHECK_KEY(&info[2], PMIX_LOCAL_COLLECTIVE_STATUS)
          && PMIX_ERR_PARTIAL_SUCCESS == info[2].value.data.status);
    report("fence layout: trailing status slot updated", ok);

    PMIX_INFO_FREE(info, ninfo);
}

static void test_status_absent(void)
{
    pmix_info_t *info;
    size_t ninfo = 2;
    uint64_t u = USER_VAL;
    int ok;

    /* no PMIX_LOCAL_COLLECTIVE_STATUS slot present */
    PMIX_INFO_CREATE(info, 2);
    PMIX_INFO_LOAD(&info[0], USER_KEY, &u, PMIX_UINT64);
    u = ENDPT_VAL;
    PMIX_INFO_LOAD(&info[1], ENDPT_KEY, &u, PMIX_UINT64);

    /* must not write anywhere and must not crash */
    pmix_server_set_collective_status(info, ninfo, PMIX_ERR_LOST_CONNECTION);

    ok = (PMIX_CHECK_KEY(&info[0], USER_KEY)
          && USER_VAL == info[0].value.data.uint64
          && PMIX_CHECK_KEY(&info[1], ENDPT_KEY)
          && ENDPT_VAL == info[1].value.data.uint64);
    report("status absent: no slot touched", ok);

    PMIX_INFO_FREE(info, ninfo);
}

static void test_degenerate(void)
{
    pmix_info_t *info;
    size_t ninfo = 1;
    pmix_status_t seed = PMIX_SUCCESS;

    /* NULL array and zero length must be safe no-ops */
    pmix_server_set_collective_status(NULL, 0, PMIX_ERR_LOST_CONNECTION);
    pmix_server_set_collective_status(NULL, 5, PMIX_ERR_LOST_CONNECTION);
    report("degenerate: NULL array is a safe no-op", 1);

    PMIX_INFO_CREATE(info, 1);
    PMIX_INFO_LOAD(&info[0], PMIX_LOCAL_COLLECTIVE_STATUS, &seed, PMIX_STATUS);
    /* ninfo of zero over a valid array must not scan or write */
    pmix_server_set_collective_status(info, 0, PMIX_ERR_LOST_CONNECTION);
    report("degenerate: zero length does not scan",
           PMIX_SUCCESS == info[0].value.data.status);
    PMIX_INFO_FREE(info, ninfo);
}

int main(int argc, char **argv)
{
    pmix_status_t rc;
    static pmix_server_module_t mymodule = {0};
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    rc = PMIx_server_init(&mymodule, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    fprintf(stdout, "\n=== pmix_server_set_collective_status unit tests ===\n\n");

    test_connect_layout();
    test_fence_layout();
    test_status_absent();
    test_degenerate();

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);

    PMIx_server_finalize();

    return (nfail > 0) ? 1 : 0;
}
