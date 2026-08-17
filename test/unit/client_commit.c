/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * What PMIx_Commit will send.
 *
 * PMIx_Commit used to fetch and ship this process's entire local and
 * remote store on every call, so n put/commit cycles moved O(n^2) bytes.
 * It now sends only what has been published since the last commit,
 * keeping the cumulative fetch as a fallback for the cases a per-key
 * record cannot express.
 *
 * This runs as a singleton, so it cannot reach _commitfn at all -
 * PMIx_Commit short-circuits with PMIX_SUCCESS when there is no server.
 * What it can reach, and what these cases are about, is the record
 * PMIx_Put builds for the commit to consult: pmix_client_globals is an
 * exported symbol, so the dirty-key lists can be read directly. The
 * assertions are therefore about which keys a commit *would* fetch,
 * which is the half of the change that decides correctness.
 *
 * The other half - that the commit really sends those and the server
 * really stores them - needs a server, and lives in
 * contrib/dockerswarm. examples/modex_twice.c is the end-to-end shape:
 * it publishes one set of keys, commits and fences, publishes a second
 * set, commits and fences again, and asserts the first set is still
 * readable afterwards.
 *
 * Two cases are worth reading before editing:
 *
 *   test_repeat_put_recorded_once() is what keeps the change from
 *   regressing the very thing it optimizes. A key published repeatedly
 *   between two commits is one key to send, not one per put; the
 *   datastore replaces such a value in place, so a record that grew per
 *   put would send the same key many times where the old cumulative
 *   fetch sent it once.
 *
 *   test_qualified_put_forces_resync() covers the one shape that cannot
 *   be recorded by key. Every qualified value is stored under the single
 *   PMIX_QUALIFIED_VALUE key with the real key inside the array, and
 *   pmix_hash's lookup will not match a qualified entry against an
 *   unqualified fetch - so there is no key to ask for it back by, and
 *   the only correct answer is to fall back to the cumulative fetch.
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "src/client/pmix_client_ops.h"
#include "src/include/pmix_globals.h"
#include "src/util/pmix_argv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static pmix_proc_t myproc;
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

/* how many times this key appears in a dirty list */
static int count_in(char **list, const char *key)
{
    int n, found = 0;

    if (NULL == list) {
        return 0;
    }
    for (n = 0; NULL != list[n]; n++) {
        if (0 == strcmp(list[n], key)) {
            ++found;
        }
    }
    return found;
}

/* Put one value and report whether it worked. The value is deliberately
 * small: PMIx_Put compresses a large string into a PMIX_COMPRESSED_STRING
 * before storing it, and that is not what these cases are about. */
static bool put_key(const char *key, pmix_scope_t scope, int val)
{
    pmix_value_t v;
    pmix_status_t rc;

    PMIX_VALUE_CONSTRUCT(&v);
    v.type = PMIX_INT;
    v.data.integer = val;
    rc = PMIx_Put(scope, key, &v);
    PMIX_VALUE_DESTRUCT(&v);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "    PMIx_Put(%s) failed: %s\n", key, PMIx_Error_string(rc));
        return false;
    }
    return true;
}

/* Start each case from a known state. This is what a successful commit
 * does, so the cases read as "commit, then publish, then look". */
static void clear_record(void)
{
    pmix_client_commit_resync();
    pmix_client_globals.commit_resync = false;
}

/* A put is recorded against the scopes it was actually stored at, and
 * that has to mirror the datastore: gds/hash files PMIX_GLOBAL into both
 * the local and the remote table, so a commit owes it to both. */
static void test_scope_routing(void)
{
    fprintf(stdout, "\n-- which scopes a put is recorded against --\n");

    clear_record();
    if (!put_key("cc.local", PMIX_LOCAL, 1)) {
        return;
    }
    report("a local put is recorded local",
           1 == count_in(pmix_client_globals.dirty_local, "cc.local"));
    report("a local put is not recorded remote",
           0 == count_in(pmix_client_globals.dirty_remote, "cc.local"));

    clear_record();
    if (!put_key("cc.remote", PMIX_REMOTE, 2)) {
        return;
    }
    report("a remote put is recorded remote",
           1 == count_in(pmix_client_globals.dirty_remote, "cc.remote"));
    report("a remote put is not recorded local",
           0 == count_in(pmix_client_globals.dirty_local, "cc.remote"));

    clear_record();
    if (!put_key("cc.global", PMIX_GLOBAL, 3)) {
        return;
    }
    report("a global put is recorded local",
           1 == count_in(pmix_client_globals.dirty_local, "cc.global"));
    report("a global put is recorded remote",
           1 == count_in(pmix_client_globals.dirty_remote, "cc.global"));

    /* internal data never leaves the process, so a commit must not carry
     * it - it was not in the old cumulative fetch either, which asked for
     * PMIX_LOCAL and PMIX_REMOTE by name */
    clear_record();
    if (!put_key("cc.internal", PMIX_INTERNAL, 4)) {
        return;
    }
    report("an internal put is recorded at no scope",
           0 == count_in(pmix_client_globals.dirty_local, "cc.internal")
               && 0 == count_in(pmix_client_globals.dirty_remote, "cc.internal"));
}

/* Publishing the same key again before committing replaces the value in
 * the datastore rather than adding to it, so the commit still owes the
 * server exactly one key. A record that grew per put would turn a
 * republishing loop into a commit far larger than the cumulative fetch
 * this change replaced. */
static void test_repeat_put_recorded_once(void)
{
    int n;

    fprintf(stdout, "\n-- a key published repeatedly is recorded once --\n");

    clear_record();
    for (n = 0; n < 8; n++) {
        if (!put_key("cc.repeat", PMIX_REMOTE, n)) {
            return;
        }
    }
    report("eight puts of one key leave one entry",
           1 == count_in(pmix_client_globals.dirty_remote, "cc.repeat"));

    /* two distinct keys are two entries - i.e. the case above is
     * deduplication, not a record that stopped working */
    clear_record();
    if (!put_key("cc.two.a", PMIX_REMOTE, 1) || !put_key("cc.two.b", PMIX_REMOTE, 2)) {
        return;
    }
    report("two distinct keys leave two entries",
           1 == count_in(pmix_client_globals.dirty_remote, "cc.two.a")
               && 1 == count_in(pmix_client_globals.dirty_remote, "cc.two.b"));
}

/* A qualified value has no key a later fetch could ask for it back by,
 * so it has to force the cumulative path. */
static void test_qualified_put_forces_resync(void)
{
    pmix_data_array_t darray;
    pmix_info_t *iptr;
    pmix_value_t v;
    pmix_status_t rc;

    fprintf(stdout, "\n-- a qualified value falls back to the full fetch --\n");

    clear_record();

    /* the shape _putfn requires: a data array of pmix_info_t whose first
     * element is the value and whose remainder are its qualifiers */
    PMIX_DATA_ARRAY_CONSTRUCT(&darray, 2, PMIX_INFO);
    iptr = (pmix_info_t *) darray.array;
    PMIX_INFO_LOAD(&iptr[0], "cc.qualified", "value", PMIX_STRING);
    PMIX_INFO_LOAD(&iptr[1], PMIX_HOSTNAME, "somehost", PMIX_STRING);
    PMIX_INFO_SET_QUALIFIER(&iptr[1]);

    PMIX_VALUE_CONSTRUCT(&v);
    v.type = PMIX_DATA_ARRAY;
    v.data.darray = &darray;
    rc = PMIx_Put(PMIX_REMOTE, PMIX_QUALIFIED_VALUE, &v);
    /* do not destruct v - it borrows the stack darray */
    report("a qualified value is accepted", PMIX_SUCCESS == rc);
    if (PMIX_SUCCESS == rc) {
        report("a qualified value forces the next commit to be cumulative",
               pmix_client_globals.commit_resync);
    }
    PMIX_DATA_ARRAY_DESTRUCT(&darray);
}

/* The escape hatch itself: whatever we had recorded is dropped, and the
 * next commit is told to send everything. This is what the tool role
 * calls when it repoints at a server that has seen none of it. */
static void test_resync_drops_the_record(void)
{
    fprintf(stdout, "\n-- pmix_client_commit_resync() --\n");

    clear_record();
    if (!put_key("cc.dropme", PMIX_GLOBAL, 7)) {
        return;
    }
    if (0 == count_in(pmix_client_globals.dirty_local, "cc.dropme")) {
        report("resync precondition: the key was recorded", false);
        return;
    }

    pmix_client_commit_resync();
    report("resync clears the local record", NULL == pmix_client_globals.dirty_local);
    report("resync clears the remote record", NULL == pmix_client_globals.dirty_remote);
    report("resync asks for a cumulative commit", pmix_client_globals.commit_resync);
}


/* Delete a key. Returns true if PMIx_Put accepted the request. */
static bool delete_key(const char *key, pmix_scope_t scope)
{
    pmix_status_t rc;

    rc = PMIx_Put(scope, key, NULL);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "    PMIx_Put(delete %s) failed: %s\n", key,
                PMIx_Error_string(rc));
        return false;
    }
    return true;
}

/* Can this process still read the key back out of its own store? */
static bool can_read(const char *key)
{
    pmix_value_t *val = NULL;
    pmix_status_t rc;
    bool found;

    rc = PMIx_Get(&myproc, key, NULL, 0, &val);
    found = (PMIX_SUCCESS == rc && NULL != val);
    if (NULL != val) {
        PMIX_VALUE_RELEASE(val);
    }
    return found;
}

/* The delete scopes remove the key rather than storing it, and take
 * effect on this process's own store immediately - exactly as a put
 * does. This is the half a singleton can see end to end. */
static void test_delete_removes_the_key(void)
{
    fprintf(stdout, "\n-- a delete scope removes the key --\n");

    clear_record();
    if (!put_key("cc.del.global", PMIX_GLOBAL, 11)) {
        return;
    }
    report("the key is readable once published", can_read("cc.del.global"));
    if (!delete_key("cc.del.global", PMIX_DEL_GLOBAL)) {
        return;
    }
    report("the key is gone after PMIX_DEL_GLOBAL", !can_read("cc.del.global"));

    /* internal data never leaves the process, so its delete is purely
     * local and must work without any server in sight */
    clear_record();
    if (!put_key("cc.del.internal", PMIX_INTERNAL, 12)) {
        return;
    }
    report("an internal key is readable once published",
           can_read("cc.del.internal"));
    if (!delete_key("cc.del.internal", PMIX_DEL_INTERNAL)) {
        return;
    }
    report("the key is gone after PMIX_DEL_INTERNAL",
           !can_read("cc.del.internal"));

    /* deleting something that was never there is not an error - the
     * caller asked for it to be absent, and it is */
    clear_record();
    report("deleting an absent key succeeds",
           delete_key("cc.del.never.stored", PMIX_DEL_GLOBAL));

    /* a delete carries no value, and a put still requires one */
    report("a put with no value is still refused",
           PMIX_ERR_BAD_PARAM == PMIx_Put(PMIX_GLOBAL, "cc.del.noval", NULL));
}

/* A deletion cannot ride the dirty-key record - that names keys for the
 * commit to fetch back, and a deleted key is the one it will not find -
 * so it is recorded separately, and forces the commit to be cumulative
 * so a delete and a re-publish in one interval need no ordering. */
static void test_delete_is_recorded_for_the_commit(void)
{
    fprintf(stdout, "\n-- what a delete tells the next commit --\n");

    clear_record();
    if (!put_key("cc.rec.global", PMIX_GLOBAL, 1)
        || !delete_key("cc.rec.global", PMIX_DEL_GLOBAL)) {
        return;
    }
    report("a global delete is recorded at local scope",
           1 == count_in(pmix_client_globals.del_local, "cc.rec.global"));
    report("a global delete is recorded at remote scope",
           1 == count_in(pmix_client_globals.del_remote, "cc.rec.global"));
    report("a delete forces the next commit to be cumulative",
           pmix_client_globals.commit_resync);

    clear_record();
    if (!delete_key("cc.rec.remote", PMIX_DEL_REMOTE)) {
        return;
    }
    report("a remote delete is recorded remote only",
           1 == count_in(pmix_client_globals.del_remote, "cc.rec.remote")
               && 0 == count_in(pmix_client_globals.del_local, "cc.rec.remote"));

    /* an internal delete never leaves the process, so there is nothing
     * to tell a server about */
    clear_record();
    if (!delete_key("cc.rec.internal", PMIX_DEL_INTERNAL)) {
        return;
    }
    report("an internal delete is recorded at no scope",
           0 == count_in(pmix_client_globals.del_local, "cc.rec.internal")
               && 0 == count_in(pmix_client_globals.del_remote, "cc.rec.internal"));
}

int main(int argc, char **argv)
{
    pmix_status_t rc;

    (void) argc;
    (void) argv;

    /* force the singleton path */
    unsetenv("PMIX_NAMESPACE");
    unsetenv("PMIX_RANK");
    unsetenv("PMIX_SERVER_URI");
    unsetenv("PMIX_SERVER_URI2");
    unsetenv("PMIX_SERVER_URI3");
    unsetenv("PMIX_SERVER_URI21");
    unsetenv("PMIX_SERVER_URI41");
    unsetenv("PMIX_SERVER_URI51");

    fprintf(stdout, "\n=== PMIx_Commit delta-record test ===\n");

    /* a singleton reports PMIX_ERR_UNREACH from init - it is fully
     * initialized, it just has no server */
    rc = PMIx_Init(&myproc, NULL, 0);
    if (PMIX_SUCCESS != rc && PMIX_ERR_UNREACH != rc) {
        fprintf(stderr, "PMIx_Init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    test_scope_routing();
    test_repeat_put_recorded_once();
    test_qualified_put_forces_resync();
    test_resync_drops_the_record();
    test_delete_removes_the_key();
    test_delete_is_recorded_for_the_commit();

    PMIx_Finalize(NULL, 0);

    fprintf(stdout, "\n=== %d passed, %d failed ===\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
