/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * A client that cannot map the modex segment must degrade, not break.
 *
 * gds/shmem3 hands a client the address of the segment holding a
 * collecting fence's result, and the client has to map it at exactly
 * that address. That can fail - the address is chosen by the server,
 * and the client is a different process whose address space has moved
 * on since it started. openpmix#4156 is what used to happen when it
 * did, and it is worth being precise about, because two separate things
 * went wrong and only one of them is what the title said:
 *
 *   PMIx_Fence returned PMIX_ERR_TAKE_NEXT_OPTION to the application.
 *   There is no "next option" to take at that point - the fence path
 *   resolves one GDS module and the modex cannot be re-delivered in
 *   another's format - so this was an internal signal escaping into a
 *   public return code. Open MPI does not check that return, so the
 *   job carried on with a modex it had not stored.
 *
 *   The failed attach ALSO tore down the client's job tracker, which
 *   owned the job and session segments it had been reading happily
 *   since PMIx_Init. So every job-level lookup for the client's OWN
 *   namespace then failed with PMIX_ERR_INVALID_NAMESPACE - which is
 *   how it was noticed, as MPI_Comm_split_type(MPI_COMM_TYPE_SHARED)
 *   putting five processes in five communicators of one.
 *
 * What should happen instead is that the client loses the fast path and
 * nothing else: the fence succeeds, job-level data is still there, and
 * remote procs' values are fetched from the server on demand. That is
 * what this program asserts.
 *
 * RUN IT WITH the failure forced, or it proves nothing:
 *
 *   PMIX_MCA_gds_shmem3_force_modex_attach_failure=1
 *
 * and across AT LEAST TWO NODES. A single-node job has no remote data
 * to exchange, so no modex segment is sent to the client, so there is
 * no attach to fail. (The neighbouring force_client_attach_failure
 * cannot be used here: it fails every attach, so the client falls back
 * to gds/hash during PMIx_Init and never reaches a fence on shmem3.)
 *
 * Prints "MODEX-ATTACH-FAIL OK" per rank and exits 0 when correct.
 */

#include <pmix.h>

#include <stdio.h>

int main(int argc, char **argv)
{
    pmix_proc_t myproc, wildcard, peer;
    pmix_value_t put;
    pmix_value_t *val = NULL;
    pmix_info_t info;
    bool collect = true;
    uint32_t nprocs = 1;
    pmix_status_t rc;
    int failures = 0;

    (void) argc;
    (void) argv;

    rc = PMIx_Init(&myproc, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_Init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    PMIX_LOAD_PROCID(&wildcard, myproc.nspace, PMIX_RANK_WILDCARD);

    rc = PMIx_Get(&wildcard, PMIX_JOB_SIZE, NULL, 0, &val);
    if (PMIX_SUCCESS == rc && NULL != val) {
        nprocs = val->data.uint32;
        PMIX_VALUE_RELEASE(val);
        val = NULL;
    }
    if (2 > nprocs) {
        fprintf(stderr, "[rank %u] this test needs at least 2 procs on 2 nodes\n",
                myproc.rank);
        PMIx_Finalize(NULL, 0);
        return 1;
    }

    /* Publish something every other rank will come looking for. */
    PMIX_VALUE_CONSTRUCT(&put);
    put.type = PMIX_UINT32;
    put.data.uint32 = myproc.rank;
    rc = PMIx_Put(PMIX_GLOBAL, "modex-attach-key", &put);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "[rank %u] PMIx_Put failed: %s\n",
                myproc.rank, PMIx_Error_string(rc));
        PMIx_Finalize(NULL, 0);
        return 1;
    }
    rc = PMIx_Commit();
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "[rank %u] PMIx_Commit failed: %s\n",
                myproc.rank, PMIx_Error_string(rc));
        PMIx_Finalize(NULL, 0);
        return 1;
    }

    /* (1) The fence itself must succeed. This is the assertion that
     * fails with PMIX_ERR_TAKE_NEXT_OPTION on an unfixed library. */
    PMIX_INFO_LOAD(&info, PMIX_COLLECT_DATA, &collect, PMIX_BOOL);
    rc = PMIx_Fence(NULL, 0, &info, 1);
    printf("[rank %u] PMIx_Fence rc=%s\n", myproc.rank, PMIx_Error_string(rc));
    if (PMIX_SUCCESS != rc) {
        printf("[rank %u] FAILED: fence did not succeed\n", myproc.rank);
        ++failures;
    }
    PMIX_INFO_DESTRUCT(&info);

    /* (2) Job-level data for our own namespace must still be reachable.
     * This is the one that broke MPI locality: the tracker holding the
     * job segment was released along with the modex attach. */
    rc = PMIx_Get(&wildcard, PMIX_LOCAL_PEERS, NULL, 0, &val);
    printf("[rank %u] PMIx_Get(PMIX_LOCAL_PEERS) rc=%s value=%s\n",
           myproc.rank, PMIx_Error_string(rc),
           (PMIX_SUCCESS == rc && NULL != val && PMIX_STRING == val->type
            && NULL != val->data.string) ? val->data.string : "(none)");
    if (PMIX_SUCCESS != rc) {
        printf("[rank %u] FAILED: lost job-level data for my own namespace\n",
               myproc.rank);
        ++failures;
    }
    if (NULL != val) {
        PMIX_VALUE_RELEASE(val);
        val = NULL;
    }

    /* (3) A remote peer's value must still arrive - by whatever route.
     * Without the segment this is answered by the server rather than
     * out of shared memory, which is slower and entirely correct. */
    PMIX_LOAD_PROCID(&peer, myproc.nspace, (myproc.rank + 1) % nprocs);

    rc = PMIx_Get(&peer, "modex-attach-key", NULL, 0, &val);
    printf("[rank %u] PMIx_Get(rank %u, modex-attach-key) rc=%s\n",
           myproc.rank, peer.rank, PMIx_Error_string(rc));
    if (PMIX_SUCCESS != rc || NULL == val) {
        printf("[rank %u] FAILED: could not read peer %u's value\n",
               myproc.rank, peer.rank);
        ++failures;
    } else if (PMIX_UINT32 != val->type || val->data.uint32 != peer.rank) {
        printf("[rank %u] FAILED: peer %u's value is wrong\n",
               myproc.rank, peer.rank);
        ++failures;
    }
    if (NULL != val) {
        PMIX_VALUE_RELEASE(val);
        val = NULL;
    }

    printf("[rank %u] MODEX-ATTACH-FAIL %s\n",
           myproc.rank, (0 == failures) ? "OK" : "FAILED");
    fflush(stdout);

    PMIx_Finalize(NULL, 0);
    return (0 == failures) ? 0 : 1;
}
