/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Deleting a published key, and having every peer stop seeing it.
 *
 * A process can remove a key it published by putting it again with a
 * delete scope. Making that stick is the interesting part, because by
 * the time it happens the value is in three kinds of place at once:
 *
 *   the publisher's own store, corrected on the spot;
 *   its server's store, corrected when the commit arrives;
 *   every *other* process that read it, which cached it - and, where the
 *   namespace is served by gds/shmem3, holds it in a shared segment that
 *   is never rewritten, so the module has to record that it has stopped
 *   answering for the key rather than take it out.
 *
 * This drives all three. Every rank publishes a key and a fence
 * circulates them; every rank reads every peer's, so the caches are
 * genuinely populated. Rank 0 then deletes its key, and every other rank
 * must stop being able to read it - which it can only do if the
 * notification reached it and its datastore acted on it.
 *
 * The other ranks' keys must survive, which is what separates "the
 * deletion worked" from "something threw the peer's data away".
 *
 * The second fence collects data, and that is not incidental. A removal
 * reaches another node the same way a re-published value does: carried
 * by a collecting fence. It rides as an entry saying the key is gone,
 * because the modex is additive - a contribution that merely stops
 * naming a key removes nothing at the far end.
 *
 * Finally rank 0 publishes the key again and every rank must see it
 * come back, carrying the new value. A deletion is not permanent, and
 * under gds/shmem3 the record of it is dated with the modex generation
 * it was made at precisely so that a later generation is not shadowed by
 * it.
 *
 * NOTE ON TIMING. Once a server has the removal, passing it to its own
 * clients is a one-way push with no acknowledgement, so it lands
 * promptly rather than synchronously. A reader that races it can
 * legitimately see the old value once more. The check below therefore
 * retries for a bounded time rather than demanding the key be gone on
 * the first look - insisting on that would assert something the library
 * does not promise, and would fail at random.
 */

#include <pmix.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "examples.h"

static pmix_proc_t myproc;
static int nerrs = 0;

#define DELKEY  "delete_key.victim"
#define KEEPKEY "delete_key.keeper"
/* what rank 0 re-publishes DELKEY as, chosen so it cannot be mistaken
 * for the rank number the first round put there */
#define REPUBVAL 4242u

/* how long to allow for the removal to reach us, and how often to look */
#define WAIT_USEC  5000000
#define POLL_USEC  50000
/* the bound on a single lookup - see readable() */
#define GET_TIMEOUT_SEC 2

static pmix_status_t put_u32(const char *key, uint32_t v)
{
    pmix_value_t value;
    pmix_status_t rc;

    PMIX_VALUE_LOAD(&value, &v, PMIX_UINT32);
    rc = PMIx_Put(PMIX_GLOBAL, key, &value);
    PMIX_VALUE_DESTRUCT(&value);
    return rc;
}

/* Can we read this key belonging to that rank right now?
 *
 * The timeout is what makes this a question rather than a demand. An
 * ordinary PMIx_Get for a key the datastore does not have does not fail
 * - it *waits*, on the assumption the owner has yet to publish it. That
 * is the right default, and it is also precisely what a successful
 * deletion looks like from here: a key that will never arrive. Without a
 * bound this would block forever instead of reporting the absence we
 * came to check for.
 *
 * PMIX_OPTIONAL is the wrong instrument for it. That says "answer out of
 * what you already hold and do not ask anyone else", which a reader on
 * another node legitimately cannot satisfy - reaching a peer's data
 * costs a round trip to the server holding it - so every cross-node read
 * would report the key missing whether or not anything was deleted. */
static bool readable(uint32_t peer, const char *key)
{
    pmix_value_t *val = NULL;
    pmix_proc_t proc;
    pmix_info_t tmo;
    pmix_status_t rc;
    int secs = GET_TIMEOUT_SEC;
    bool got;

    PMIX_LOAD_PROCID(&proc, myproc.nspace, peer);
    PMIX_INFO_LOAD(&tmo, PMIX_TIMEOUT, &secs, PMIX_INT);
    rc = PMIx_Get(&proc, key, &tmo, 1, &val);
    PMIX_INFO_DESTRUCT(&tmo);
    got = (PMIX_SUCCESS == rc && NULL != val);
    if (NULL != val) {
        PMIX_VALUE_RELEASE(val);
    }
    return got;
}

/* As readable(), but hands back what was read. */
static bool read_u32(uint32_t peer, const char *key, uint32_t *out)
{
    pmix_value_t *val = NULL;
    pmix_proc_t proc;
    pmix_info_t tmo;
    pmix_status_t rc;
    int secs = GET_TIMEOUT_SEC;
    bool got = false;

    PMIX_LOAD_PROCID(&proc, myproc.nspace, peer);
    PMIX_INFO_LOAD(&tmo, PMIX_TIMEOUT, &secs, PMIX_INT);
    rc = PMIx_Get(&proc, key, &tmo, 1, &val);
    PMIX_INFO_DESTRUCT(&tmo);
    if (PMIX_SUCCESS == rc && NULL != val && PMIX_UINT32 == val->type) {
        *out = val->data.uint32;
        got = true;
    }
    if (NULL != val) {
        PMIX_VALUE_RELEASE(val);
    }
    return got;
}

/* Wait for the key to become readable again AND carry the given value.
 *
 * Both halves matter. A re-published key arriving is one thing; a
 * re-published key that still reads as the value it had before the
 * delete would mean a stale generation answered. */
static void expect_value(uint32_t peer, const char *key, uint32_t want,
                         const char *what)
{
    int waited = 0;
    uint32_t got = 0;

    for (;;) {
        if (read_u32(peer, key, &got)) {
            if (got == want) {
                return;
            }
            fprintf(stderr,
                    "delete_key: rank %u: %s from rank %u reads %u, wanted "
                    "%u [%s]\n",
                    (unsigned) myproc.rank, key, (unsigned) peer,
                    (unsigned) got, (unsigned) want, what);
            nerrs++;
            return;
        }
        if (waited >= WAIT_USEC) {
            fprintf(stderr,
                    "delete_key: rank %u: %s from rank %u never came back "
                    "after %d ms [%s]\n",
                    (unsigned) myproc.rank, key, (unsigned) peer,
                    WAIT_USEC / 1000, what);
            nerrs++;
            return;
        }
        usleep(POLL_USEC);
        waited += POLL_USEC;
    }
}

static void expect_readable(uint32_t peer, const char *key, const char *what)
{
    if (!readable(peer, key)) {
        fprintf(stderr, "delete_key: rank %u: cannot read %s from rank %u [%s]\n",
                (unsigned) myproc.rank, key, (unsigned) peer, what);
        nerrs++;
    }
}

/* wait for the key to become unreadable - see the timing note above */
static void expect_gone(uint32_t peer, const char *key, const char *what)
{
    int waited = 0;

    while (readable(peer, key)) {
        if (waited >= WAIT_USEC) {
            fprintf(stderr,
                    "delete_key: rank %u: %s from rank %u is STILL readable "
                    "after %d ms [%s]\n",
                    (unsigned) myproc.rank, key, (unsigned) peer,
                    WAIT_USEC / 1000, what);
            nerrs++;
            return;
        }
        usleep(POLL_USEC);
        waited += POLL_USEC;
    }
}

int main(int argc, char **argv)
{
    pmix_status_t rc;
    pmix_proc_t wildcard;
    pmix_value_t *val = NULL;
    pmix_info_t info;
    uint32_t nprocs, n;
    bool flag;

    EXAMPLES_HIDE_UNUSED_PARAMS(argc, argv);

    if (PMIX_SUCCESS != (rc = PMIx_Init(&myproc, NULL, 0))) {
        fprintf(stderr, "delete_key: PMIx_Init failed: %s\n", PMIx_Error_string(rc));
        exit(1);
    }

    PMIX_LOAD_PROCID(&wildcard, myproc.nspace, PMIX_RANK_WILDCARD);
    if (PMIX_SUCCESS != (rc = PMIx_Get(&wildcard, PMIX_JOB_SIZE, NULL, 0, &val))) {
        fprintf(stderr, "delete_key: rank %u: job size failed: %s\n",
                (unsigned) myproc.rank, PMIx_Error_string(rc));
        exit(1);
    }
    nprocs = val->data.uint32;
    PMIX_VALUE_RELEASE(val);

    if (2 > nprocs) {
        fprintf(stderr, "delete_key: needs at least 2 processes\n");
        exit(1);
    }

    /* publish, commit, and circulate */
    if (PMIX_SUCCESS != (rc = put_u32(DELKEY, myproc.rank))
        || PMIX_SUCCESS != (rc = put_u32(KEEPKEY, myproc.rank))) {
        fprintf(stderr, "delete_key: rank %u: put failed: %s\n",
                (unsigned) myproc.rank, PMIx_Error_string(rc));
        exit(1);
    }
    if (PMIX_SUCCESS != (rc = PMIx_Commit())) {
        fprintf(stderr, "delete_key: rank %u: commit failed: %s\n",
                (unsigned) myproc.rank, PMIx_Error_string(rc));
        exit(1);
    }
    flag = true;
    PMIX_INFO_LOAD(&info, PMIX_COLLECT_DATA, &flag, PMIX_BOOL);
    rc = PMIx_Fence(&wildcard, 1, &info, 1);
    PMIX_INFO_DESTRUCT(&info);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "delete_key: rank %u: fence failed: %s\n",
                (unsigned) myproc.rank, PMIx_Error_string(rc));
        exit(1);
    }

    /* read everybody, so the caches really do hold the key */
    for (n = 0; n < nprocs; n++) {
        expect_readable(n, DELKEY, "before the delete");
        expect_readable(n, KEEPKEY, "before the delete");
    }

    /* rank 0 takes its key back */
    if (0 == myproc.rank) {
        rc = PMIx_Put(PMIX_DEL_GLOBAL, DELKEY, NULL);
        if (PMIX_SUCCESS != rc) {
            fprintf(stderr, "delete_key: rank 0: delete failed: %s\n",
                    PMIx_Error_string(rc));
            exit(1);
        }
        if (PMIX_SUCCESS != (rc = PMIx_Commit())) {
            fprintf(stderr, "delete_key: rank 0: commit failed: %s\n",
                    PMIx_Error_string(rc));
            exit(1);
        }
        /* the publisher's own store is corrected on the spot */
        if (readable(0, DELKEY)) {
            fprintf(stderr, "delete_key: rank 0: still reads its own deleted key\n");
            nerrs++;
        }
    }

    /* A *collecting* fence, which is how any change to published data
     * reaches another node - a re-published value would need one too.
     * The removal rides it as an entry saying the key is gone, because
     * the modex is additive and a contribution that merely stops naming
     * a key removes nothing at the far end. */
    flag = true;
    PMIX_INFO_LOAD(&info, PMIX_COLLECT_DATA, &flag, PMIX_BOOL);
    rc = PMIx_Fence(&wildcard, 1, &info, 1);
    PMIX_INFO_DESTRUCT(&info);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "delete_key: rank %u: second fence failed: %s\n",
                (unsigned) myproc.rank, PMIx_Error_string(rc));
        exit(1);
    }

    /* the deleted key must go everywhere... */
    expect_gone(0, DELKEY, "after the delete");
    /* ...and nothing else may go with it */
    for (n = 0; n < nprocs; n++) {
        expect_readable(n, KEEPKEY, "after the delete");
        if (0 != n) {
            expect_readable(n, DELKEY, "after the delete");
        }
    }

    /* And now put it back.
     *
     * A deletion is not permanent - nothing in the Standard says a key
     * may only be published once - so a rank that publishes the same key
     * again must be readable again. Under gds/shmem3 that is the one
     * thing a removal record must NOT outlast: the data still sits in a
     * segment that is never rewritten, so the module answers by
     * remembering that it stopped answering, and a record that never
     * expired would bury the new value along with the old one. Each
     * record is therefore dated with the modex generation it was made
     * at, and only shadows generations up to that one.
     *
     * The value differs from the original so that "it came back" and
     * "the old copy was never really gone" are distinguishable. */
    if (0 == myproc.rank) {
        if (PMIX_SUCCESS != (rc = put_u32(DELKEY, REPUBVAL))) {
            fprintf(stderr, "delete_key: rank 0: re-publish failed: %s\n",
                    PMIx_Error_string(rc));
            exit(1);
        }
        if (PMIX_SUCCESS != (rc = PMIx_Commit())) {
            fprintf(stderr, "delete_key: rank 0: third commit failed: %s\n",
                    PMIx_Error_string(rc));
            exit(1);
        }
    }

    flag = true;
    PMIX_INFO_LOAD(&info, PMIX_COLLECT_DATA, &flag, PMIX_BOOL);
    rc = PMIx_Fence(&wildcard, 1, &info, 1);
    PMIX_INFO_DESTRUCT(&info);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "delete_key: rank %u: third fence failed: %s\n",
                (unsigned) myproc.rank, PMIx_Error_string(rc));
        exit(1);
    }

    expect_value(0, DELKEY, REPUBVAL, "after the re-publish");
    /* and nothing that survived the delete may be lost to the return */
    for (n = 0; n < nprocs; n++) {
        expect_readable(n, KEEPKEY, "after the re-publish");
    }

    if (0 == nerrs) {
        fprintf(stderr, "delete_key: rank %u: deleted key gone, others kept\n",
                (unsigned) myproc.rank);
        fprintf(stderr, "delete_key: rank %u: re-published key came back\n",
                (unsigned) myproc.rank);
    }

    PMIx_Finalize(NULL, 0);
    return (0 == nerrs) ? 0 : 1;
}
