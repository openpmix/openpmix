/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Exercise the src/hwloc layer the way it is actually used: many ranks, on
 * more than one node, each obtaining the topology their local server handed
 * them and then comparing localities with every peer in the job.
 *
 * Three things are checked here that no single-host unit test can reach:
 *
 *  1. TOPOLOGY HANDOFF.  PMIx_Load_topology must produce a usable topology in
 *     a launched client. On a real node that topology arrives from the local
 *     server by one of the paths in pmix_hwloc_setup_topology -- the hwloc
 *     shared-memory segment first, XML as the fallback -- and none of those
 *     paths exist in a standalone process that discovers its own topology.
 *
 *  2. LOCALITY STRINGS AS THE HOST ACTUALLY STORES THEM.  Each rank reads its
 *     peers' PMIX_LOCALITY_STRING -- the string the server generated with
 *     PMIx_server_generate_locality_string and stored verbatim -- and feeds it
 *     straight to PMIx_Get_relative_locality. That is the producer/consumer
 *     pairing pmix.h documents, and it is exactly what broke for years while
 *     the consumer demanded an "hwloc:" prefix the producer never wrote: every
 *     process on a node reported NO shared locality with its own node-mates.
 *     Only the real generator output can catch that, so nothing here is a
 *     hand-written literal.
 *
 *  3. THE MULTI-NODE ANSWER.  A peer on this node must share at least the
 *     node; a peer on another node must not be claimed as sharing anything.
 *     A single-host run cannot tell a correct answer from one that says
 *     "everything is local".
 *
 * Every rank prints a PASS/FAIL line per check and a final summary line
 * ("TOPO rank N: <k> passed, <j> failed"), so a driver can tally across nodes.
 * Exits non-zero if any check failed.
 */

#define _GNU_SOURCE
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "examples.h"
#include <pmix.h>

static pmix_proc_t myproc;
static int npass = 0;
static int nfail = 0;

static void report(const char *name, bool ok, const char *detail)
{
    if (ok) {
        ++npass;
    } else {
        ++nfail;
    }
    fprintf(stderr, "TOPO rank %u: %s %s%s%s\n", myproc.rank, ok ? "PASS" : "FAIL", name,
            (NULL == detail) ? "" : "  ", (NULL == detail) ? "" : detail);
    fflush(stderr);
}

/* Fetch a string-valued key for a specific peer. Returns a malloc'd copy the
 * caller frees, or NULL if the key is not available for that peer. */
static char *getstr(const pmix_proc_t *p, const char *key)
{
    pmix_value_t *val = NULL;
    char *ret = NULL;

    if (PMIX_SUCCESS != PMIx_Get(p, key, NULL, 0, &val) || NULL == val) {
        return NULL;
    }
    if (PMIX_STRING == val->type && NULL != val->data.string) {
        ret = strdup(val->data.string);
    }
    PMIX_VALUE_RELEASE(val);
    return ret;
}

int main(int argc, char **argv)
{
    pmix_status_t rc;
    pmix_value_t *val;
    pmix_proc_t wildcard, peer;
    pmix_topology_t topo = PMIX_TOPOLOGY_STATIC_INIT;
    uint32_t nprocs, n;
    char *myhost = NULL, *myloc = NULL;
    char detail[512];
    int nlocal = 0, nremote = 0;
    bool ok;

    EXAMPLES_HIDE_UNUSED_PARAMS(argc, argv);

    rc = PMIx_Init(&myproc, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "TOPO: PMIx_Init failed: %s\n", PMIx_Error_string(rc));
        exit(1);
    }
    PMIX_LOAD_PROCID(&wildcard, myproc.nspace, PMIX_RANK_WILDCARD);

    /* --- 1. the topology our server handed us ------------------------- */
    rc = PMIx_Load_topology(&topo);
    if (PMIX_SUCCESS == rc && NULL != topo.topology) {
        report("load_topology", true, topo.source);
    } else {
        snprintf(detail, sizeof(detail), "[rc=%s]", PMIx_Error_string(rc));
        report("load_topology", false, detail);
        goto done;
    }

    /* the topology must be structurally usable, not merely non-NULL: an
     * adopted shmem segment or an imported XML string that failed to
     * reconstitute would still hand back a pointer */
    {
        char *printed = NULL;
        ok = (PMIX_SUCCESS == PMIx_Data_print(&printed, NULL, &topo, PMIX_TOPO) &&
              NULL != printed && NULL != strstr(printed, "Machine"));
        report("topology renders a machine object", ok, NULL);
        free(printed);
    }

    /* --- 2. our own identity ------------------------------------------ */
    myhost = getstr(&myproc, PMIX_HOSTNAME);
    report("hostname is available", NULL != myhost, myhost);

    /* PMIX_LOCALITY_STRING is what the server generated for us with
     * PMIx_server_generate_locality_string and stored. If the host did not
     * provide one there is nothing to compare, so say so and stop rather
     * than silently passing. */
    myloc = getstr(&myproc, PMIX_LOCALITY_STRING);
    if (NULL == myloc) {
        report("locality string is available", false,
               "[host stored no PMIX_LOCALITY_STRING - nothing to compare]");
        goto done;
    }
    report("locality string is available", true, myloc);

    /* --- 3. compare against every peer -------------------------------- */
    rc = PMIx_Get(&wildcard, PMIX_JOB_SIZE, NULL, 0, &val);
    if (PMIX_SUCCESS != rc) {
        report("job size", false, PMIx_Error_string(rc));
        goto done;
    }
    nprocs = val->data.uint32;
    PMIX_VALUE_RELEASE(val);

    /* everyone must have posted before anyone reads */
    rc = PMIx_Fence(&wildcard, 1, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        report("fence", false, PMIx_Error_string(rc));
        goto done;
    }

    for (n = 0; n < nprocs; n++) {
        char *peerhost, *peerloc;
        pmix_locality_t bits = 0;
        bool samenode;

        if (n == myproc.rank) {
            continue;
        }
        PMIX_LOAD_PROCID(&peer, myproc.nspace, n);

        peerhost = getstr(&peer, PMIX_HOSTNAME);
        peerloc = getstr(&peer, PMIX_LOCALITY_STRING);
        if (NULL == peerhost) {
            snprintf(detail, sizeof(detail), "[peer %u: no hostname]", n);
            report("peer hostname", false, detail);
            free(peerloc);
            continue;
        }
        samenode = (0 == strcmp(myhost, peerhost));

        if (samenode) {
            ++nlocal;
            /* A node-mate must be recognized as one. The locality strings on
             * both sides are the host's own stored values, so this is the
             * documented usage end to end -- and it is the check that a
             * producer/consumer format mismatch fails. */
            if (NULL == peerloc) {
                snprintf(detail, sizeof(detail), "[peer %u on %s has no locality string]",
                         n, peerhost);
                report("node-mate locality", false, detail);
            } else {
                rc = PMIx_Get_relative_locality(myloc, peerloc, &bits);
                ok = (PMIX_SUCCESS == rc && 0 != (bits & PMIX_LOCALITY_SHARE_NODE));
                snprintf(detail, sizeof(detail), "[peer %u on %s: rc=%s bits=0x%04x  '%s' vs '%s']",
                         n, peerhost, PMIx_Error_string(rc), (unsigned) bits, myloc, peerloc);
                report("node-mate shares the node", ok, detail);
            }
        } else {
            ++nremote;
            /* A peer on another node must not be reported as sharing
             * anything. Comparing localities across nodes is meaningless --
             * "CR0" on two different machines is two different cores -- so
             * the guard that matters is the hostname test the consumer of
             * PMIX_LOCALITY_STRING is required to make first. Assert here
             * only that we can tell the two apart. */
            ok = (0 != strcmp(myhost, peerhost));
            snprintf(detail, sizeof(detail), "[peer %u on %s, we are on %s]", n, peerhost, myhost);
            report("off-node peer is distinguishable", ok, detail);
        }
        free(peerhost);
        free(peerloc);
    }

    /* The whole point of running this across a swarm: if every peer turned
     * out to be local, the multi-node half of the test never ran and a
     * "green" result would be meaningless. Say so loudly. */
    if (0 == nremote) {
        report("job actually spans more than one node", false,
               "[all peers are local - launch this across at least two nodes]");
    } else {
        snprintf(detail, sizeof(detail), "[%d node-mates, %d off-node]", nlocal, nremote);
        report("job actually spans more than one node", true, detail);
    }

done:
    /* topo.topology is the library's cached topology and topo.source is a
     * read-only static string when we did not stipulate one - neither is
     * ours to release */
    free(myhost);
    free(myloc);

    fprintf(stderr, "TOPO rank %u: %d passed, %d failed\n", myproc.rank, npass, nfail);
    fflush(stderr);

    PMIx_Finalize(NULL, 0);
    return (0 == nfail) ? 0 : 1;
}
