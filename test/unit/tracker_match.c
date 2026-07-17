/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * White-box unit tests for the collective-tracker identity machinery:
 * pmix_server_new_tracker() and pmix_server_get_tracker().
 *
 * A collective tracker is uniquely identified either by a string id, or by the
 * tuple {set of participating procs, collective type}. get_tracker() performs a
 * brute-force search of pmix_server_globals.collectives and must:
 *   - find a tracker by its id;
 *   - find a tracker by an exact proc-set + type match (order-independent);
 *   - NOT match when the type differs, the proc set differs, or the id is
 *     unknown.
 * This underpins how the fence/connect/disconnect families join contributors to
 * the right in-flight collective, so a matching regression would silently split
 * or merge collectives.
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

static void release_tracker(pmix_server_trkr_t *trk)
{
    /* new_tracker appended it to the collectives list; detach before
     * releasing so we do not leave a dangling entry */
    pmix_list_remove_item(&pmix_server_globals.collectives, &trk->super);
    PMIX_RELEASE(trk);
}

int main(int argc, char **argv)
{
    pmix_status_t rc;
    static pmix_server_module_t mymodule = {0};
    pmix_server_trkr_t *t1, *t2, *found;
    pmix_proc_t procsA[1];
    pmix_proc_t procsB[2];
    pmix_proc_t procsB_rev[2];
    pmix_proc_t procsC[1];
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    rc = PMIx_server_init(&mymodule, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    fprintf(stdout, "\n=== collective tracker matching unit tests ===\n\n");

    PMIX_LOAD_PROCID(&procsA[0], "trk.ns.a", 0);

    PMIX_LOAD_PROCID(&procsB[0], "trk.ns.b", 0);
    PMIX_LOAD_PROCID(&procsB[1], "trk.ns.b", 1);
    /* same set as procsB but in reversed order - matching must be
     * order-independent */
    PMIX_LOAD_PROCID(&procsB_rev[0], "trk.ns.b", 1);
    PMIX_LOAD_PROCID(&procsB_rev[1], "trk.ns.b", 0);

    PMIX_LOAD_PROCID(&procsC[0], "trk.ns.c", 0);

    /* an id-identified tracker (as group collectives use) */
    t1 = pmix_server_new_tracker("trk-collective-A", procsA, 1, PMIX_FENCENB_CMD);
    report("new_tracker(id) returns a tracker", NULL != t1);

    /* a proc-set/type-identified tracker (as fence/connect use) */
    t2 = pmix_server_new_tracker(NULL, procsB, 2, PMIX_CONNECTNB_CMD);
    report("new_tracker(procs) returns a tracker", NULL != t2);

    /* found by id */
    found = pmix_server_get_tracker("trk-collective-A", NULL, 0, PMIX_FENCENB_CMD);
    report("get_tracker finds by id", found == t1);

    /* found by exact proc set + type, regardless of proc order */
    found = pmix_server_get_tracker(NULL, procsB_rev, 2, PMIX_CONNECTNB_CMD);
    report("get_tracker finds by proc set (order-independent)", found == t2);

    /* same proc set but a different collective type must NOT match */
    found = pmix_server_get_tracker(NULL, procsB, 2, PMIX_DISCONNECTNB_CMD);
    report("get_tracker rejects a type mismatch", NULL == found);

    /* a proc set that no tracker holds must NOT match */
    found = pmix_server_get_tracker(NULL, procsC, 1, PMIX_CONNECTNB_CMD);
    report("get_tracker rejects an unknown proc set", NULL == found);

    /* an unknown id must NOT match */
    found = pmix_server_get_tracker("no-such-collective", NULL, 0, PMIX_FENCENB_CMD);
    report("get_tracker rejects an unknown id", NULL == found);

    release_tracker(t1);
    release_tracker(t2);

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);

    PMIx_server_finalize();

    return (nfail > 0) ? 1 : 0;
}
