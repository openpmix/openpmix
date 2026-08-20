/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * Unit tests for the psec framework's credential contract
 * (src/mca/psec/base/psec_base_fns.c, src/mca/psec/native/psec_native.c,
 * and - wherever libmunge and a live munged are present -
 * src/mca/psec/munge/psec_munge.c).
 *
 * Three things are checked here, each of which was broken:
 *
 * 1. pmix_psec_base_check_directives() - the PMIX_CRED_TYPE screening
 *    every module applies to a caller's directives. This used to be five
 *    hand-copied loops, each of which fed the directive's string straight
 *    to PMIx_Argv_split() and then walked the result without checking it.
 *    PMIx_Argv_split() returns NULL for an empty string, for a string of
 *    nothing but separators, and for NULL - so a caller passing
 *    PMIX_CRED_TYPE="" segfaulted the module. The empty-ish cases below
 *    are the regression test for that; they must *decline* (nobody was
 *    named), not crash and not accept.
 *
 * 2. The *info output array a module fills in on success. The modules
 *    wrote it with PMIX_INFO_LOAD(info[n], ...) where info is the
 *    pmix_info_t** out-parameter. info[0] happens to be the same address
 *    as &(*info)[0], so entry 0 looked fine; info[1] and info[2] are
 *    whatever lies past the caller's pointer variable in its own stack
 *    frame, so validate_cred wrote two pmix_info_t structures through a
 *    garbage pointer and left the array entries it was supposed to fill
 *    zeroed. Checking that the returned array actually carries
 *    PMIX_USERID and PMIX_GRPID is what catches that: a re-broken module
 *    returns three empty entries.
 *
 * 3. The credential round trip itself - create_cred on the connecting
 *    side, validate_cred on the accepting side. This runs twice over
 *    every *active* credential-model module, not over a fixed list, so
 *    that a machine which has MUNGE examines `munge` on the same terms
 *    as `native` without needing a second test. The second pass matters
 *    for a module that caches: MUNGE credentials are single-use, so
 *    munge re-encodes on every call after the first, and that refresh
 *    path is where it used to leave the freed credential pointer
 *    dangling for the next call to free again.
 *
 *    `native` then gets the extra examination its own credential format
 *    calls for: the rejections it owes its caller for a truncated,
 *    empty, or NULL credential, and for a peer whose ptl protocol was
 *    never established (PMIX_PROTOCOL_UNDEF), for which there is
 *    neither a socket to interrogate nor a credential format to trust.
 *
 * Like the pstat tests, this needs the MCA up but no server:
 * pmix_init_util() establishes the install dirs, the variable system and
 * the component repository, which is all that opening a framework
 * requires. The modules are then driven directly through the
 * pmix_psec_module_t handed back by pmix_psec_base_assign_module(),
 * rather than through the PMIX_PSEC_* macros, which would require a peer
 * carrying a fully-populated namespace compatibility struct.
 */

#include "src/include/pmix_config.h"

#include "pmix_common.h"

#include "src/include/pmix_globals.h"
#include "src/mca/base/pmix_base.h"
#include "src/class/pmix_list.h"
#include "src/mca/psec/base/base.h"
#include "src/runtime/pmix_init_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef HAVE_SYS_TYPES_H
#    include <sys/types.h>
#endif

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

/* ---------------------------------------------------------------- */
/* PMIX_CRED_TYPE screening                                          */
/* ---------------------------------------------------------------- */

static void check_directives(void)
{
    pmix_info_t dir[2];
    uint32_t u32 = 42;

    /* no directives at all - anyone may serve the request */
    report("no directives accepts",
           pmix_psec_base_check_directives("native", NULL, 0));
    report("zero ndirs accepts",
           pmix_psec_base_check_directives("native", dir, 0));

    /* a directive that is not PMIX_CRED_TYPE is none of our business */
    PMIX_INFO_LOAD(&dir[0], PMIX_USERID, &u32, PMIX_UINT32);
    report("unrelated directive accepts",
           pmix_psec_base_check_directives("native", dir, 1));
    PMIX_INFO_DESTRUCT(&dir[0]);

    /* named outright */
    PMIX_INFO_LOAD(&dir[0], PMIX_CRED_TYPE, "native", PMIX_STRING);
    report("named alone accepts",
           pmix_psec_base_check_directives("native", dir, 1));
    report("named alone declines others",
           !pmix_psec_base_check_directives("munge", dir, 1));
    PMIX_INFO_DESTRUCT(&dir[0]);

    /* named within a list */
    PMIX_INFO_LOAD(&dir[0], PMIX_CRED_TYPE, "munge,native,none", PMIX_STRING);
    report("named in list accepts",
           pmix_psec_base_check_directives("native", dir, 1));
    report("absent from list declines",
           !pmix_psec_base_check_directives("dummy_handshake", dir, 1));
    PMIX_INFO_DESTRUCT(&dir[0]);

    /* a partial name must not match - the comparison is exact */
    PMIX_INFO_LOAD(&dir[0], PMIX_CRED_TYPE, "nativex", PMIX_STRING);
    report("prefix does not match",
           !pmix_psec_base_check_directives("native", dir, 1));
    PMIX_INFO_DESTRUCT(&dir[0]);

    /* every PMIX_CRED_TYPE directive must name us, not just one of them */
    PMIX_INFO_LOAD(&dir[0], PMIX_CRED_TYPE, "native", PMIX_STRING);
    PMIX_INFO_LOAD(&dir[1], PMIX_CRED_TYPE, "munge", PMIX_STRING);
    report("second directive can still decline",
           !pmix_psec_base_check_directives("native", dir, 2));
    PMIX_INFO_DESTRUCT(&dir[0]);
    PMIX_INFO_DESTRUCT(&dir[1]);

    /* the cases that used to dereference NULL: PMIx_Argv_split() hands
     * back NULL for each of these, and the old loops walked it anyway */
    PMIX_INFO_LOAD(&dir[0], PMIX_CRED_TYPE, "", PMIX_STRING);
    report("empty string declines without crashing",
           !pmix_psec_base_check_directives("native", dir, 1));
    PMIX_INFO_DESTRUCT(&dir[0]);

    PMIX_INFO_LOAD(&dir[0], PMIX_CRED_TYPE, ",,,", PMIX_STRING);
    report("separators only decline without crashing",
           !pmix_psec_base_check_directives("native", dir, 1));
    PMIX_INFO_DESTRUCT(&dir[0]);

    /* a PMIX_CRED_TYPE whose value is not a string at all: the union
     * would otherwise be read as a char* and handed to the splitter */
    PMIX_INFO_LOAD(&dir[0], PMIX_CRED_TYPE, &u32, PMIX_UINT32);
    report("non-string value declines without crashing",
           !pmix_psec_base_check_directives("native", dir, 1));
    PMIX_INFO_DESTRUCT(&dir[0]);
}

/* ---------------------------------------------------------------- */
/* credential round trips                                            */
/* ---------------------------------------------------------------- */

/* find the value of a key in a returned info array, or NULL */
static pmix_info_t *find_key(pmix_info_t *info, size_t ninfo, const char *key)
{
    size_t n;

    for (n = 0; n < ninfo; n++) {
        if (PMIx_Check_key(info[n].key, key)) {
            return &info[n];
        }
    }
    return NULL;
}

/* Drive one module's create/validate pair twice, whatever module it is.
 *
 * Running this over every *active* module rather than over a fixed list
 * is what gets `munge` covered: it builds only where libmunge is present
 * and de-selects itself where munged is not answering, so on most
 * development machines the actives list is just `native` and this is a
 * second pass over it. In an environment that has MUNGE - the
 * slurmswarm image in the PRRTE tree is one - `munge` is in the list at
 * priority 80 and gets the identical examination for free.
 *
 * Twice, because the second pass is the interesting one for a module
 * that caches: MUNGE credentials are single-use, so munge's create_cred
 * re-encodes on every call after the first, and that refresh path is
 * where it used to leave the cached credential dangling after a failed
 * encode - freed once here, again on the next call, and a third time at
 * finalize.
 */
static void module_round_trip(pmix_psec_module_t *mod, pmix_peer_t *peer, int pass)
{
    pmix_byte_object_t cred;
    pmix_info_t *results = NULL, *ptr;
    size_t nresults = 0;
    pmix_status_t rc;
    char label[256];
    size_t seen_identity = 0;

    snprintf(label, sizeof(label), "%s pass %d: create_cred succeeds", mod->name, pass);
    PMIX_BYTE_OBJECT_CONSTRUCT(&cred);
    rc = mod->create_cred((struct pmix_peer_t *) peer, NULL, 0, &results, &nresults, &cred);
    report(label, PMIX_SUCCESS == rc);
    if (PMIX_SUCCESS != rc) {
        return;
    }

    snprintf(label, sizeof(label), "%s pass %d: create_cred names itself", mod->name, pass);
    ptr = find_key(results, nresults, PMIX_CRED_TYPE);
    report(label,
           NULL != ptr && PMIX_STRING == ptr->value.type && NULL != ptr->value.data.string
               && 0 == strcmp(ptr->value.data.string, mod->name));
    if (NULL != results) {
        PMIX_INFO_FREE(results, nresults);
        results = NULL;
        nresults = 0;
    }

    snprintf(label, sizeof(label), "%s pass %d: validate_cred accepts it", mod->name, pass);
    rc = mod->validate_cred((struct pmix_peer_t *) peer, NULL, 0, &results, &nresults, &cred);
    report(label, PMIX_SUCCESS == rc);
    if (PMIX_SUCCESS != rc) {
        PMIX_BYTE_OBJECT_DESTRUCT(&cred);
        return;
    }

    snprintf(label, sizeof(label), "%s pass %d: validate_cred names itself", mod->name, pass);
    ptr = find_key(results, nresults, PMIX_CRED_TYPE);
    report(label,
           NULL != ptr && PMIX_STRING == ptr->value.type && NULL != ptr->value.data.string
               && 0 == strcmp(ptr->value.data.string, mod->name));

    /* A module that recovered an identity from the credential has to
     * hand it back. This is the &(*info)[n] regression: written as
     * info[n], entries 1 and 2 go through a garbage pointer and the
     * array the caller sees keeps the zeros PMIX_INFO_CREATE left. */
    seen_identity = nresults;
    if (1 < nresults) {
        snprintf(label, sizeof(label), "%s pass %d: validate_cred returns the uid", mod->name,
                 pass);
        ptr = find_key(results, nresults, PMIX_USERID);
        report(label,
               NULL != ptr && PMIX_UINT32 == ptr->value.type
                   && (uint32_t) geteuid() == ptr->value.data.uint32);
        snprintf(label, sizeof(label), "%s pass %d: validate_cred returns the gid", mod->name,
                 pass);
        ptr = find_key(results, nresults, PMIX_GRPID);
        report(label,
               NULL != ptr && PMIX_UINT32 == ptr->value.type
                   && (uint32_t) getegid() == ptr->value.data.uint32);
    }

    if (NULL != results) {
        PMIX_INFO_FREE(results, nresults);
        results = NULL;
        nresults = 0;
    }

    /* A module that recovered an identity is one that actually reads the
     * credential, so it owes its caller a rejection for one it did not
     * issue - and, more to the point, must not read past the end of it.
     *
     * Two details make this case mean something, and getting either
     * wrong turns it into a green line that asserts nothing:
     *
     * - The buffer is an *exact-sized* heap block of bytes that are not
     *   a credential, with no NUL anywhere. That is how a credential
     *   really arrives: ptl mallocs cred.size bytes and memcpys them
     *   off the wire, so nothing follows them, and a peer chooses the
     *   contents. Truncating a *valid* credential instead does not
     *   reach the same code - MUNGE's parser stops at its own delimiter
     *   well before the end, so such an input never runs off the block
     *   and the case passes against a module with no length handling at
     *   all.
     * - What this case asserts is the rejection, and that is all it
     *   asserts. It is deliberately *not* the reproducer for the
     *   related overread, and should not be described as one: MUNGE
     *   refuses a buffer that does not look like a credential before it
     *   ever measures it, so this input is turned away without running
     *   off the end. The overread needs an input MUNGE keeps parsing -
     *   munge_decode() takes a NUL-terminated string and calls strlen()
     *   on it, which valgrind reports as an invalid read directly
     *   beneath munge_decode for an unterminated one. That is why
     *   psec_munge.c checks for the terminator itself rather than
     *   trusting the counted length, and it is a property of the
     *   library rather than of anything reachable from here. Run this
     *   program under valgrind anyway when changing how a module
     *   inspects a credential.
     */
    if (1 < seen_identity) {
        pmix_byte_object_t junk;

        junk.size = cred.size;
        junk.bytes = (char *) malloc(junk.size);
        memset(junk.bytes, 0xAA, junk.size);

        snprintf(label, sizeof(label), "%s pass %d: validate_cred rejects a junk credential",
                 mod->name, pass);
        rc = mod->validate_cred((struct pmix_peer_t *) peer, NULL, 0, &results, &nresults, &junk);
        report(label, PMIX_SUCCESS != rc);
        if (NULL != results) {
            PMIX_INFO_FREE(results, nresults);
            results = NULL;
            nresults = 0;
        }
        free(junk.bytes);
    }

    PMIX_BYTE_OBJECT_DESTRUCT(&cred);
}

static void active_modules_round_trip(void)
{
    pmix_psec_base_active_module_t *active;
    pmix_peer_t *peer;

    /* a peer that looks like a V2 (tcp) connection from ourselves.
     * native reads the protocol and the recorded uid/gid; munge and none
     * ignore the protocol and read only the uid/gid */
    peer = PMIX_NEW(pmix_peer_t);
    peer->protocol = PMIX_PROTOCOL_V2;
    peer->info = PMIX_NEW(pmix_rank_info_t);
    peer->info->uid = geteuid();
    peer->info->gid = getegid();

    PMIX_LIST_FOREACH (active, &pmix_psec_globals.actives, pmix_psec_base_active_module_t) {
        if (NULL == active->module->create_cred || NULL == active->module->validate_cred) {
            /* a handshake-model module - it has no credential to trip */
            continue;
        }
        module_round_trip(active->module, peer, 1);
        module_round_trip(active->module, peer, 2);
    }

    PMIX_RELEASE(peer->info);
    peer->info = NULL;
    PMIX_RELEASE(peer);
}

static void native_round_trip(void)
{
    pmix_psec_module_t *mod;
    pmix_peer_t *peer;
    pmix_byte_object_t cred;
    pmix_info_t *results = NULL, *ptr;
    size_t nresults = 0;
    pmix_status_t rc;
    pmix_info_t dir;

    mod = pmix_psec_base_assign_module("native");
    if (NULL == mod) {
        report("native module is available", 0);
        return;
    }
    report("native module is available", 1);
    report("native uses the credential model",
           NULL != mod->create_cred && NULL != mod->validate_cred
               && NULL == mod->client_handshake && NULL == mod->server_handshake);

    /* stand up a peer that looks like a V2 (tcp) connection from
     * ourselves - which is what native is built to authenticate */
    peer = PMIX_NEW(pmix_peer_t);
    peer->protocol = PMIX_PROTOCOL_V2;
    peer->info = PMIX_NEW(pmix_rank_info_t);
    peer->info->uid = geteuid();
    peer->info->gid = getegid();

    /* create */
    PMIX_BYTE_OBJECT_CONSTRUCT(&cred);
    rc = mod->create_cred((struct pmix_peer_t *) peer, NULL, 0, &results, &nresults, &cred);
    report("create_cred succeeds", PMIX_SUCCESS == rc);
    report("create_cred returns a credential",
           NULL != cred.bytes && (sizeof(uid_t) + sizeof(gid_t)) == cred.size);
    report("create_cred names itself",
           1 == nresults && NULL != find_key(results, nresults, PMIX_CRED_TYPE));
    if (NULL != results) {
        PMIX_INFO_FREE(results, nresults);
        results = NULL;
        nresults = 0;
    }

    /* validate what we just created */
    rc = mod->validate_cred((struct pmix_peer_t *) peer, NULL, 0, &results, &nresults, &cred);
    report("validate_cred accepts our own credential", PMIX_SUCCESS == rc);

    /* the returned array has to actually carry the identity the
     * credential contained - this is the info[n] indexing regression */
    report("validate_cred returns three results", 3 == nresults && NULL != results);
    if (3 == nresults && NULL != results) {
        ptr = find_key(results, nresults, PMIX_CRED_TYPE);
        report("validate_cred names itself",
               NULL != ptr && PMIX_STRING == ptr->value.type
                   && NULL != ptr->value.data.string
                   && 0 == strcmp(ptr->value.data.string, "native"));
        ptr = find_key(results, nresults, PMIX_USERID);
        report("validate_cred returns the uid",
               NULL != ptr && PMIX_UINT32 == ptr->value.type
                   && (uint32_t) geteuid() == ptr->value.data.uint32);
        ptr = find_key(results, nresults, PMIX_GRPID);
        report("validate_cred returns the gid",
               NULL != ptr && PMIX_UINT32 == ptr->value.type
                   && (uint32_t) getegid() == ptr->value.data.uint32);
    } else {
        report("validate_cred names itself", 0);
        report("validate_cred returns the uid", 0);
        report("validate_cred returns the gid", 0);
    }
    if (NULL != results) {
        PMIX_INFO_FREE(results, nresults);
        results = NULL;
        nresults = 0;
    }

    /* a credential too short to hold a uid and a gid must be refused,
     * not read past */
    cred.size = sizeof(uid_t);
    rc = mod->validate_cred((struct pmix_peer_t *) peer, NULL, 0, &results, &nresults, &cred);
    report("validate_cred rejects a truncated credential", PMIX_ERR_INVALID_CRED == rc);
    cred.size = 0;
    rc = mod->validate_cred((struct pmix_peer_t *) peer, NULL, 0, &results, &nresults, &cred);
    report("validate_cred rejects an empty credential", PMIX_ERR_INVALID_CRED == rc);
    cred.size = sizeof(uid_t) + sizeof(gid_t);

    /* a NULL credential on a V2 peer is equally inadmissible */
    rc = mod->validate_cred((struct pmix_peer_t *) peer, NULL, 0, &results, &nresults, NULL);
    report("validate_cred rejects a NULL credential", PMIX_ERR_INVALID_CRED == rc);

    PMIX_BYTE_OBJECT_DESTRUCT(&cred);

    /* Asking for somebody else must be declined by both halves - and
     * declined on the grounds of the mechanism, not of the credential,
     * so validate_cred has to screen the directives before it starts
     * interpreting bytes that are not its own. Note that create_cred
     * constructs (and so empties) the credential it is handed before it
     * decides anything, which is why each case below gets a fresh one. */
    PMIX_INFO_LOAD(&dir, PMIX_CRED_TYPE, "munge", PMIX_STRING);
    PMIX_BYTE_OBJECT_CONSTRUCT(&cred);
    rc = mod->create_cred((struct pmix_peer_t *) peer, &dir, 1, &results, &nresults, &cred);
    report("create_cred declines another mechanism", PMIX_ERR_NOT_SUPPORTED == rc);
    rc = mod->create_cred((struct pmix_peer_t *) peer, NULL, 0, &results, &nresults, &cred);
    if (PMIX_SUCCESS == rc && NULL != results) {
        PMIX_INFO_FREE(results, nresults);
        results = NULL;
        nresults = 0;
    }
    rc = mod->validate_cred((struct pmix_peer_t *) peer, &dir, 1, &results, &nresults, &cred);
    report("validate_cred declines another mechanism", PMIX_ERR_NOT_SUPPORTED == rc);
    PMIX_INFO_DESTRUCT(&dir);
    PMIX_BYTE_OBJECT_DESTRUCT(&cred);

    /* an empty PMIX_CRED_TYPE names nobody - it must decline, and in
     * particular must not walk a NULL argv */
    PMIX_INFO_LOAD(&dir, PMIX_CRED_TYPE, "", PMIX_STRING);
    PMIX_BYTE_OBJECT_CONSTRUCT(&cred);
    rc = mod->create_cred((struct pmix_peer_t *) peer, &dir, 1, &results, &nresults, &cred);
    report("create_cred survives an empty PMIX_CRED_TYPE", PMIX_ERR_NOT_SUPPORTED == rc);
    rc = mod->create_cred((struct pmix_peer_t *) peer, NULL, 0, &results, &nresults, &cred);
    if (PMIX_SUCCESS == rc && NULL != results) {
        PMIX_INFO_FREE(results, nresults);
        results = NULL;
        nresults = 0;
    }
    rc = mod->validate_cred((struct pmix_peer_t *) peer, &dir, 1, &results, &nresults, &cred);
    report("validate_cred survives an empty PMIX_CRED_TYPE", PMIX_ERR_NOT_SUPPORTED == rc);
    PMIX_INFO_DESTRUCT(&dir);
    PMIX_BYTE_OBJECT_DESTRUCT(&cred);

    /* a peer whose transport was never established carries neither a
     * socket we can interrogate nor a credential format we trust */
    peer->protocol = PMIX_PROTOCOL_UNDEF;
    PMIX_BYTE_OBJECT_CONSTRUCT(&cred);
    rc = mod->create_cred((struct pmix_peer_t *) peer, NULL, 0, &results, &nresults, &cred);
    report("create_cred refuses an undefined protocol", PMIX_ERR_NOT_SUPPORTED == rc);
    rc = mod->validate_cred((struct pmix_peer_t *) peer, NULL, 0, &results, &nresults, &cred);
    report("validate_cred refuses an undefined protocol", PMIX_ERR_INVALID_CRED == rc);
    PMIX_BYTE_OBJECT_DESTRUCT(&cred);

    PMIX_RELEASE(peer->info);
    peer->info = NULL;
    PMIX_RELEASE(peer);
}

/* ---------------------------------------------------------------- */

static void available_modules(void)
{
    char *avail;

    /* the string a server advertises to its clients. native has no
     * configure gate and no runtime probe, so it is always in it */
    avail = pmix_psec_base_get_available_modules();
    report("available modules includes native",
           NULL != avail && NULL != strstr(avail, "native"));
    if (NULL != avail) {
        free(avail);
    }

    /* an unknown mechanism cannot be assigned */
    report("unknown mechanism is not assigned",
           NULL == pmix_psec_base_assign_module("no-such-mechanism"));

    /* with no preference expressed, the highest-priority module wins */
    report("no preference still yields a module",
           NULL != pmix_psec_base_assign_module(NULL));
}

int main(int argc, char **argv)
{
    pmix_status_t rc;

    (void) argc;
    (void) argv;

    /* keep the caller's own MCA parameter files out of the results, as
     * the mca/ suite does */
    setenv("PMIX_MCA_mca_base_param_files", "none", 1);

    rc = pmix_init_util(NULL, 0, NULL);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "pmix_init_util failed: %d\n", rc);
        return 1;
    }

    rc = pmix_mca_base_framework_open(&pmix_psec_base_framework, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "psec framework open failed: %d\n", rc);
        return 1;
    }
    rc = pmix_psec_base_select();
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "psec select failed: %d\n", rc);
        return 1;
    }

    check_directives();
    available_modules();
    active_modules_round_trip();
    native_round_trip();

    (void) pmix_mca_base_framework_close(&pmix_psec_base_framework);

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);

    return (nfail > 0) ? 1 : 0;
}
