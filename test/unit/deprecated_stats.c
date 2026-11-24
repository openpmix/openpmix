/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * The v5 resource-usage statistics API stays in the library.
 *
 * v7 replaced pmix_proc_stats_t, pmix_disk_stats_t, pmix_net_stats_t and
 * pmix_node_stats_t with arrays of pmix_info_t and removed them, with
 * their sixteen PMIx_*_stats_* functions. A program built against v5
 * that referenced any of those functions then failed to load against
 * the v7 library - "undefined symbol: PMIx_Proc_stats_free" from the
 * Linux loader, "Symbol not found: _PMIx_Proc_stats_create" from dyld -
 * even when it never called one. They are back as stubs that do
 * nothing, and this holds them there:
 *
 *   - linking this program at all needs every one of the sixteen
 *     symbols exported from libpmix;
 *   - the four data type codes keep the values v5 gave them;
 *   - each *_STATIC_INIT macro compiles (v5's PMIX_PROC_STATS_STATIC_INIT
 *     was missing a comma and could not have been used);
 *   - each *_create returns NULL and the FREE / RELEASE macros leave
 *     the caller's pointer NULL.
 */

#include "src/include/pmix_config.h"
#include "include/pmix.h"

#include <stdio.h>
#include <stdlib.h>

static int npass = 0;
static int nfail = 0;

static void check(const char *name, int passed)
{
    if (passed) {
        fprintf(stdout, "  PASS: %s\n", name);
        npass++;
    } else {
        fprintf(stdout, "  FAIL: %s\n", name);
        nfail++;
    }
}

int main(void)
{
    pmix_proc_stats_t pst = PMIX_PROC_STATS_STATIC_INIT;
    pmix_disk_stats_t dst = PMIX_DISK_STATS_STATIC_INIT;
    pmix_net_stats_t nst = PMIX_NET_STATS_STATIC_INIT;
    pmix_node_stats_t ndst = PMIX_NODE_STATS_STATIC_INIT;
    pmix_proc_stats_t *pp = &pst;
    pmix_disk_stats_t *dp = &dst;
    pmix_net_stats_t *np = &nst;
    pmix_node_stats_t *ndp = &ndst;

    fprintf(stdout, "deprecated resource-usage statistics API\n");

    check("PMIX_PROC_STATS is 61", 61 == PMIX_PROC_STATS);
    check("PMIX_DISK_STATS is 62", 62 == PMIX_DISK_STATS);
    check("PMIX_NET_STATS is 63", 63 == PMIX_NET_STATS);
    check("PMIX_NODE_STATS is 64", 64 == PMIX_NODE_STATS);

    check("PMIX_PROC_STATS_STATIC_INIT", NULL == pst.node && PMIX_RANK_UNDEF == pst.proc.rank);
    check("PMIX_DISK_STATS_STATIC_INIT", NULL == dst.disk && 0 == dst.num_reads_completed);
    check("PMIX_NET_STATS_STATIC_INIT", NULL == nst.net_interface && 0 == nst.num_send_errs);
    check("PMIX_NODE_STATS_STATIC_INIT", NULL == ndst.node && NULL == ndst.diskstats &&
          NULL == ndst.netstats);

    PMIX_PROC_STATS_CONSTRUCT(&pst);
    PMIX_PROC_STATS_DESTRUCT(&pst);
    PMIX_DISK_STATS_CONSTRUCT(&dst);
    PMIX_DISK_STATS_DESTRUCT(&dst);
    PMIX_NET_STATS_CONSTRUCT(&nst);
    PMIX_NET_STATS_DESTRUCT(&nst);
    PMIX_NODE_STATS_CONSTRUCT(&ndst);
    PMIX_NODE_STATS_DESTRUCT(&ndst);
    check("construct/destruct return", 1);

    PMIX_PROC_STATS_CREATE(pp, 4);
    check("PMIx_Proc_stats_create returns NULL", NULL == pp);
    PMIX_DISK_STATS_CREATE(dp, 4);
    check("PMIx_Disk_stats_create returns NULL", NULL == dp);
    PMIX_NET_STATS_CREATE(np, 4);
    check("PMIx_Net_stats_create returns NULL", NULL == np);
    PMIX_NODE_STATS_CREATE(ndp, 4);
    check("PMIx_Node_stats_create returns NULL", NULL == ndp);

    /* the FREE and RELEASE macros clear the caller's pointer */
    pp = &pst;
    PMIX_PROC_STATS_FREE(pp, 1);
    check("PMIX_PROC_STATS_FREE clears the pointer", NULL == pp);
    pp = &pst;
    PMIX_PROC_STATS_RELEASE(pp);
    check("PMIX_PROC_STATS_RELEASE clears the pointer", NULL == pp);
    dp = &dst;
    PMIX_DISK_STATS_FREE(dp, 1);
    check("PMIX_DISK_STATS_FREE clears the pointer", NULL == dp);
    dp = &dst;
    PMIX_DISK_STATS_RELEASE(dp);
    check("PMIX_DISK_STATS_RELEASE clears the pointer", NULL == dp);
    np = &nst;
    PMIX_NET_STATS_FREE(np, 1);
    check("PMIX_NET_STATS_FREE clears the pointer", NULL == np);
    np = &nst;
    PMIX_NET_STATS_RELEASE(np);
    check("PMIX_NET_STATS_RELEASE clears the pointer", NULL == np);
    ndp = &ndst;
    PMIX_NODE_STATS_FREE(ndp, 1);
    check("PMIX_NODE_STATS_FREE clears the pointer", NULL == ndp);
    ndp = &ndst;
    PMIX_NODE_STATS_RELEASE(ndp);
    check("PMIX_NODE_STATS_RELEASE clears the pointer", NULL == ndp);

    /* the functions themselves, by name, so that each symbol is
     * referenced directly rather than only through a macro */
    PMIx_Proc_stats_construct(&pst);
    PMIx_Proc_stats_destruct(&pst);
    PMIx_Proc_stats_free(PMIx_Proc_stats_create(1), 1);
    PMIx_Disk_stats_construct(&dst);
    PMIx_Disk_stats_destruct(&dst);
    PMIx_Disk_stats_free(PMIx_Disk_stats_create(1), 1);
    PMIx_Net_stats_construct(&nst);
    PMIx_Net_stats_destruct(&nst);
    PMIx_Net_stats_free(PMIx_Net_stats_create(1), 1);
    PMIx_Node_stats_construct(&ndst);
    PMIx_Node_stats_destruct(&ndst);
    PMIx_Node_stats_free(PMIx_Node_stats_create(1), 1);
    check("each function callable by name", 1);

    fprintf(stdout, "%d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? EXIT_SUCCESS : EXIT_FAILURE;
}
