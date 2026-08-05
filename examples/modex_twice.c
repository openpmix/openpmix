/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Two collecting fences in one job.
 *
 * A fence with PMIX_COLLECT_DATA makes the servers exchange their local
 * procs' data, and gds/shmem3 puts the result in a shared-memory
 * segment that every local client maps. Doing it a SECOND time is the
 * interesting part, because the first segment is by then mapped by
 * those clients and must never be written again - so the second modex
 * has to land somewhere new.
 *
 * The two properties that matter are checked separately, because they
 * fail for opposite reasons:
 *
 *   gen1 keys, put before the first fence and never again, must still
 *   be readable after the second one. If the second modex were treated
 *   as a delta - or if handing the first segment off dropped it - these
 *   are what would go missing. That is not hypothetical: making commit
 *   send only what changed is planned (openpmix#4087), and this
 *   assertion is what will catch the segment rotation not keeping up.
 *
 *   gen2 keys, put only before the second fence, must be readable after
 *   it. These are what goes missing if a client keeps reading the first
 *   segment and never picks up the second.
 *
 * Distinct keys per generation are deliberate: reusing one key would
 * bring the client-side cache into it (see PMIX_GET_REFRESH_CACHE and
 * test_fence.c), and this is about which segment the data is in, not
 * about cache semantics.
 *
 * The second round also publishes far more data than the first, and the
 * volume is not padding. The modex segment is sized from the payload of
 * the modex that created it, with generous slack (the estimate scales
 * the packed size by 5 for possible compression and then by another 5
 * of "fluff"). A second modex written into that same segment therefore
 * only overruns it - which aborts the server, since the allocator
 * backing the segment is a bump allocator with nowhere to grow - once
 * it is substantially larger. GEN2_BYTES is what makes the second
 * round big enough for that to be reachable rather than theoretical.
 */

#include <pmix.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "examples.h"

static pmix_proc_t myproc;
static int nerrs = 0;

/* gen2 publishes this many keys per rank against gen1's one, each
 * carrying this many bytes, against gen1's four. See the note above on
 * why the volume matters. */
#define GEN2_KEYS  64
#define GEN2_BYTES 4096

/* fetch one uint32 key belonging to rank "peer" and compare it */
static void expect_u32(uint32_t peer, const char *key, uint32_t want, const char *what)
{
    pmix_value_t *val = NULL;
    pmix_proc_t proc;
    pmix_status_t rc;

    PMIX_LOAD_PROCID(&proc, myproc.nspace, peer);
    rc = PMIx_Get(&proc, key, NULL, 0, &val);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "modex_twice: rank %u: PMIx_Get(%u, %s) failed: %s [%s]\n",
                (unsigned) myproc.rank, (unsigned) peer, key, PMIx_Error_string(rc), what);
        nerrs++;
        return;
    }
    if (NULL == val || PMIX_UINT32 != val->type || want != val->data.uint32) {
        fprintf(stderr,
                "modex_twice: rank %u: %s from rank %u: wanted %u, got %u (type %d) [%s]\n",
                (unsigned) myproc.rank, key, (unsigned) peer, (unsigned) want,
                (NULL == val) ? 0 : (unsigned) val->data.uint32,
                (NULL == val) ? -1 : (int) val->type, what);
        nerrs++;
    }
    if (NULL != val) {
        PMIX_VALUE_RELEASE(val);
    }
}

static pmix_status_t put_u32(const char *key, uint32_t v)
{
    pmix_value_t value;
    pmix_status_t rc;

    PMIX_VALUE_LOAD(&value, &v, PMIX_UINT32);
    rc = PMIx_Put(PMIX_GLOBAL, key, &value);
    PMIX_VALUE_DESTRUCT(&value);
    return rc;
}

/* a value big enough to matter to the segment estimate, whose contents
 * still identify the rank and key that produced it */
static pmix_status_t put_blob(const char *key, uint32_t seed)
{
    pmix_value_t value;
    pmix_status_t rc;
    char *buf = malloc(GEN2_BYTES);

    if (NULL == buf) {
        return PMIX_ERR_NOMEM;
    }
    memset(buf, (int) (seed & 0xff), GEN2_BYTES);
    value.type = PMIX_BYTE_OBJECT;
    value.data.bo.bytes = buf;
    value.data.bo.size = GEN2_BYTES;
    rc = PMIx_Put(PMIX_GLOBAL, key, &value);
    free(buf);
    return rc;
}

/* the mirror of put_blob: confirm size and contents came back intact */
static void expect_blob(uint32_t peer, const char *key, uint32_t seed, const char *what)
{
    pmix_value_t *val = NULL;
    pmix_proc_t proc;
    pmix_status_t rc;
    size_t i;

    PMIX_LOAD_PROCID(&proc, myproc.nspace, peer);
    rc = PMIx_Get(&proc, key, NULL, 0, &val);
    if (PMIX_SUCCESS != rc || NULL == val) {
        fprintf(stderr, "modex_twice: rank %u: PMIx_Get(%u, %s) failed: %s [%s]\n",
                (unsigned) myproc.rank, (unsigned) peer, key, PMIx_Error_string(rc), what);
        nerrs++;
        return;
    }
    if (PMIX_BYTE_OBJECT != val->type || GEN2_BYTES != val->data.bo.size) {
        fprintf(stderr, "modex_twice: rank %u: %s from rank %u: wrong shape [%s]\n",
                (unsigned) myproc.rank, key, (unsigned) peer, what);
        nerrs++;
        PMIX_VALUE_RELEASE(val);
        return;
    }
    for (i = 0; i < (size_t) GEN2_BYTES; i++) {
        if ((unsigned char) val->data.bo.bytes[i] != (unsigned char) (seed & 0xff)) {
            fprintf(stderr, "modex_twice: rank %u: %s from rank %u: byte %zu corrupt [%s]\n",
                    (unsigned) myproc.rank, key, (unsigned) peer, i, what);
            nerrs++;
            break;
        }
    }
    PMIX_VALUE_RELEASE(val);
}

static pmix_status_t collecting_fence(void)
{
    pmix_info_t info;
    pmix_proc_t proc;
    pmix_status_t rc;

    PMIX_INFO_LOAD(&info, PMIX_COLLECT_DATA, &(bool){true}, PMIX_BOOL);
    PMIX_LOAD_PROCID(&proc, myproc.nspace, PMIX_RANK_WILDCARD);
    rc = PMIx_Fence(&proc, 1, &info, 1);
    PMIX_INFO_DESTRUCT(&info);
    return rc;
}

int main(int argc, char **argv)
{
    pmix_status_t rc;
    pmix_value_t *val;
    pmix_proc_t proc;
    uint32_t nprocs = 0, n;
    char key[PMIX_MAX_KEYLEN + 1];
    int k;

    EXAMPLES_HIDE_UNUSED_PARAMS(argc, argv);

    if (PMIX_SUCCESS != (rc = PMIx_Init(&myproc, NULL, 0))) {
        fprintf(stderr, "modex_twice: PMIx_Init failed: %s\n", PMIx_Error_string(rc));
        exit(1);
    }

    PMIX_LOAD_PROCID(&proc, myproc.nspace, PMIX_RANK_WILDCARD);
    if (PMIX_SUCCESS != (rc = PMIx_Get(&proc, PMIX_JOB_SIZE, NULL, 0, &val))) {
        fprintf(stderr, "modex_twice: could not get job size: %s\n", PMIx_Error_string(rc));
        goto done;
    }
    nprocs = val->data.uint32;
    PMIX_VALUE_RELEASE(val);

    /* ---- first modex: one key per rank ---- */

    snprintf(key, sizeof(key), "gen1-%u", (unsigned) myproc.rank);
    if (PMIX_SUCCESS != (rc = put_u32(key, 1000 + myproc.rank))) {
        fprintf(stderr, "modex_twice: put %s failed: %s\n", key, PMIx_Error_string(rc));
        nerrs++;
        goto done;
    }
    if (PMIX_SUCCESS != (rc = PMIx_Commit())) {
        fprintf(stderr, "modex_twice: first commit failed: %s\n", PMIx_Error_string(rc));
        nerrs++;
        goto done;
    }
    if (PMIX_SUCCESS != (rc = collecting_fence())) {
        fprintf(stderr, "modex_twice: first fence failed: %s\n", PMIx_Error_string(rc));
        nerrs++;
        goto done;
    }

    for (n = 0; n < nprocs; n++) {
        if (n == myproc.rank) {
            continue;
        }
        snprintf(key, sizeof(key), "gen1-%u", (unsigned) n);
        expect_u32(n, key, 1000 + n, "after the first fence");
    }
    if (0 == myproc.rank) {
        fprintf(stdout, "modex_twice: first fence: every rank read every peer\n");
    }

    /* ---- second modex: many more keys per rank ----
     *
     * Nothing re-publishes the gen1 key. It has to survive anyway. */

    for (k = 0; k < GEN2_KEYS; k++) {
        snprintf(key, sizeof(key), "gen2-%u-%d", (unsigned) myproc.rank, k);
        if (PMIX_SUCCESS != (rc = put_blob(key, 2000 + myproc.rank + (uint32_t) k))) {
            fprintf(stderr, "modex_twice: put %s failed: %s\n", key, PMIx_Error_string(rc));
            nerrs++;
            goto done;
        }
    }
    if (PMIX_SUCCESS != (rc = PMIx_Commit())) {
        fprintf(stderr, "modex_twice: second commit failed: %s\n", PMIx_Error_string(rc));
        nerrs++;
        goto done;
    }
    if (PMIX_SUCCESS != (rc = collecting_fence())) {
        fprintf(stderr, "modex_twice: second fence failed: %s\n", PMIx_Error_string(rc));
        nerrs++;
        goto done;
    }

    for (n = 0; n < nprocs; n++) {
        if (n == myproc.rank) {
            continue;
        }
        /* the new generation must be visible ... */
        for (k = 0; k < GEN2_KEYS; k++) {
            snprintf(key, sizeof(key), "gen2-%u-%d", (unsigned) n, k);
            expect_blob(n, key, 2000 + n + (uint32_t) k, "after the second fence");
        }
        /* ... and the old one must not have been lost with it */
        snprintf(key, sizeof(key), "gen1-%u", (unsigned) n);
        expect_u32(n, key, 1000 + n, "carried across the second fence");
    }

    if (0 == nerrs && 0 == myproc.rank) {
        fprintf(stdout, "modex_twice: second fence: new values visible, old values kept\n");
    }

done:
    if (0 != nerrs) {
        fprintf(stderr, "modex_twice: rank %u: %d error(s)\n", (unsigned) myproc.rank, nerrs);
    }
    PMIx_Finalize(NULL, 0);
    return (0 == nerrs) ? 0 : 1;
}
