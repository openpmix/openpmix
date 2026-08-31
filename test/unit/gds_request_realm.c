/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * Which realm answers a fetch - and therefore which requests may be
 * answered on the caller's own thread.
 *
 * A datastore that reports is_tsafe lets a fetch run inline, without
 * the thread shift. That is only safe for a request answerable from the
 * job's own realm: resolving a session, node or app realm is further
 * work, and in gds/shmem3 it reaches the session tracker and the
 * process-global sessions list, neither of which is synchronized
 * against the progress thread.
 *
 * pmix_gds_base_request_realm() is the single rule that decides it.
 * It used to be decided TWICE - by process_request() in
 * src/client/pmix_client_get.c from the directives it had parsed, and
 * again by each gds module from the key and the three realm qualifiers
 * - and the two answers differed:
 *
 *   - a NULL key. PMIx_Connect_nb asks for its own namespace at
 *     PMIX_RANK_WILDCARD with no key, through fetch_kv_tsafe, which
 *     consulted nothing at all. That is the whole-job read, and it
 *     calls fetch_sessioninfo() directly.
 *
 *   - PMIX_JOB_INFO on a realm key. The client parse honors the
 *     qualifier by clearing the realm flags the KEY had set, so the
 *     request looked like a plain job-level lookup. gds/shmem3 does not
 *     know the qualifier and re-derives the realm from the key, so it
 *     answered from the session realm anyway - on the caller's thread.
 *
 * Both must be refused. Neither can be caught by a functional test that
 * merely reads the right value back, because the wrong answer here is a
 * race, not a wrong value.
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "include/pmix_common.h"
#include "src/include/pmix_globals.h"
#include "src/mca/gds/gds.h"

#include <stdio.h>
#include <string.h>

static int npass = 0, nfail = 0;

static const char *rname(pmix_realm_t r)
{
    switch (r) {
        case PMIX_REALM_JOB:     return "JOB";
        case PMIX_REALM_SESSION: return "SESSION";
        case PMIX_REALM_NODE:    return "NODE";
        case PMIX_REALM_APP:     return "APP";
        default:                 return "UNDEF";
    }
}

static void check(const char *what, pmix_realm_t got, pmix_realm_t want)
{
    bool ok = (got == want);
    fprintf(stdout, "  %s: %s\n", ok ? "PASS" : "FAIL", what);
    if (ok) {
        npass++;
    } else {
        nfail++;
        fprintf(stdout, "        (wanted %s, got %s)\n", rname(want), rname(got));
    }
}

int main(void)
{
    pmix_info_t q;
    bool flag;

    setvbuf(stdout, NULL, _IOLBF, 0);
    fprintf(stdout, "=== which realm answers a fetch ===\n");

    /* the case the fast path exists for */
    check("an ordinary keyed get is the job realm",
          pmix_gds_base_request_realm("sut.some.key", NULL, 0), PMIX_REALM_JOB);

    /* a NULL key has no realm of its own - the whole-job walk starts in
     * the job realm. It is still not answerable on the caller's thread,
     * but that is the caller's test, not this one's */
    check("a NULL key is the job realm",
          pmix_gds_base_request_realm(NULL, NULL, 0), PMIX_REALM_JOB);

    /* a key that names a realm is answered from it with nothing said */
    check("a session-realm key selects the session realm",
          pmix_gds_base_request_realm(PMIX_UNIV_SIZE, NULL, 0), PMIX_REALM_SESSION);
    check("a node-realm key selects the node realm",
          pmix_gds_base_request_realm(PMIX_HOSTNAME, NULL, 0), PMIX_REALM_NODE);

    /* PMIX_JOB_INFO overrides the key. This is the case the two ends
     * used to answer differently: the client honored it, the datastore
     * had never heard of it and answered from the session realm - on
     * the application's thread, where the session structures are not
     * protected. */
    PMIX_INFO_LOAD(&q, PMIX_JOB_INFO, NULL, PMIX_BOOL);
    check("PMIX_JOB_INFO overrides a session-realm key",
          pmix_gds_base_request_realm(PMIX_UNIV_SIZE, &q, 1), PMIX_REALM_JOB);
    check("PMIX_JOB_INFO on a job-level key is still the job realm",
          pmix_gds_base_request_realm("sut.some.key", &q, 1), PMIX_REALM_JOB);
    PMIX_INFO_DESTRUCT(&q);

    /* a realm qualifier selects its realm, and overrides the key */
    PMIX_INFO_LOAD(&q, PMIX_NODE_INFO, NULL, PMIX_BOOL);
    check("PMIX_NODE_INFO selects the node realm",
          pmix_gds_base_request_realm("sut.some.key", &q, 1), PMIX_REALM_NODE);
    check("PMIX_NODE_INFO overrides a session-realm key",
          pmix_gds_base_request_realm(PMIX_UNIV_SIZE, &q, 1), PMIX_REALM_NODE);
    PMIX_INFO_DESTRUCT(&q);

    /* a realm qualifier set FALSE de-selects that realm rather than
     * handing the decision back to the key */
    flag = false;
    PMIX_INFO_LOAD(&q, PMIX_SESSION_INFO, &flag, PMIX_BOOL);
    check("PMIX_SESSION_INFO=false on a session key is the job realm",
          pmix_gds_base_request_realm(PMIX_UNIV_SIZE, &q, 1), PMIX_REALM_JOB);
    PMIX_INFO_DESTRUCT(&q);

    fprintf(stdout, "\n=== %d passed, %d failed ===\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
