/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * White-box unit tests for pmix_server_trk_complete().
 *
 * This is the single predicate the server uses to decide whether a
 * collective's local phase is complete. It gates on two things: the tracker
 * definition must be complete (def_complete - all participating nspaces are
 * registered so nlocal is final), AND every expected local participant must be
 * accounted for, either by contributing (an entry on local_cbs) or by departing
 * before contributing (an entry on departed):
 *
 *     def_complete && (len(local_cbs) + len(departed)) >= nlocal
 *
 * The '>=' is deliberate: it tolerates any over-count from fork/exec'd clones
 * without ever falsely reporting the collective incomplete. These tests pin
 * both the def_complete gate and the counting/tolerance behavior, since the
 * fence, connect, disconnect, and group teardown paths all rest on it.
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "src/include/pmix_globals.h"
#include "src/server/pmix_server_ops.h"

#include <stdio.h>
#include <stdlib.h>

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

/* build a tracker with the given def_complete flag, nlocal count, and the
 * requested number of entries on the local_cbs and departed lists */
static pmix_server_trkr_t *make_trk(bool def_complete, uint32_t nlocal,
                                    size_t ncontributed, size_t ndeparted)
{
    pmix_server_trkr_t *trk;
    pmix_server_caddy_t *cd;
    pmix_proclist_t *p;
    size_t i;

    trk = PMIX_NEW(pmix_server_trkr_t);
    trk->def_complete = def_complete;
    trk->nlocal = nlocal;
    for (i = 0; i < ncontributed; i++) {
        cd = PMIX_NEW(pmix_server_caddy_t);
        pmix_list_append(&trk->local_cbs, &cd->super);
    }
    for (i = 0; i < ndeparted; i++) {
        p = PMIX_NEW(pmix_proclist_t);
        pmix_list_append(&trk->departed, &p->super);
    }
    return trk;
}

static void check(const char *name, bool def_complete, uint32_t nlocal,
                  size_t ncontributed, size_t ndeparted, bool expected)
{
    pmix_server_trkr_t *trk;

    trk = make_trk(def_complete, nlocal, ncontributed, ndeparted);
    report(name, expected == pmix_server_trk_complete(trk));
    PMIX_RELEASE(trk);
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

    fprintf(stdout, "\n=== pmix_server_trk_complete unit tests ===\n\n");

    /* the def_complete gate: even a fully-accounted-for tracker is incomplete
     * while its definition is still open */
    check("incomplete definition is never complete", false, 2, 2, 0, false);

    /* nobody heard from yet */
    check("no participants accounted for", true, 2, 0, 0, false);

    /* partial: one of two contributed */
    check("partial contribution is incomplete", true, 2, 1, 0, false);

    /* exactly complete by contribution */
    check("all contributed is complete", true, 2, 2, 0, true);

    /* completion by a mix of contributed and departed */
    check("contributed + departed reaches nlocal", true, 2, 1, 1, true);

    /* all expected participants departed before contributing */
    check("all departed is complete", true, 2, 0, 2, true);

    /* clone over-count: more contributors than nlocal must still be complete
     * (the '>=' tolerance), never falsely incomplete */
    check("clone over-count stays complete", true, 2, 3, 0, true);

    /* degenerate all-remote collective: nlocal 0, nothing local */
    check("nlocal zero is complete", true, 0, 0, 0, true);

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);

    PMIx_server_finalize();

    return (nfail > 0) ? 1 : 0;
}
