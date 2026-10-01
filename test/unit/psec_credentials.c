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
 * 4. That native takes a peer's identity from the kernel, not from the
 *    credential. native looks up the owner of the peer's end of the
 *    connection, so every peer here is given a real loopback TCP
 *    connection. An identity the host registered must match that owner;
 *    one the peer only claimed is replaced by it. A tool's claim of a
 *    group its user does not hold is refused, as is a credential with no
 *    connection behind it.
 *
 * 5. What native does when the kernel cannot name the owner - a peer in
 *    a container with a network of its own, which a closed connection
 *    stands in for here. A credential from a release that predates the
 *    local-socket check is taken at its word, or refused, as
 *    psec_native_legacy_auth says. A current one asks for the
 *    local-socket handshake - unless it names another host's kernel,
 *    which is refused outright. The handshake itself is then run end to
 *    end, its two halves on two threads over a loopback connection.
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
#include "src/mca/base/pmix_mca_base_var.h"
#include "src/class/pmix_list.h"
#include "src/mca/psec/base/base.h"
#include "src/runtime/pmix_init_util.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#ifdef HAVE_SYS_TYPES_H
#    include <sys/types.h>
#endif
#ifdef HAVE_GRP_H
#    include <grp.h>
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
    PMIX_INFO_LOAD(&dir[0], PMIX_CRED_TYPE, "munge,native", PMIX_STRING);
    report("named in list accepts",
           pmix_psec_base_check_directives("native", dir, 1));
    report("absent from list declines",
           !pmix_psec_base_check_directives("ssl", dir, 1));
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

/* A connected loopback TCP pair: what a native peer really arrives on.
 * native asks the kernel who owns the far end of peer->sd, so a peer
 * with no connection can no longer be authenticated at all. */
static bool loopback_pair(int *client, int *server)
{
    struct sockaddr_in addr;
    socklen_t alen = sizeof(addr);
    int lsd;

    *client = *server = -1;
    lsd = socket(AF_INET, SOCK_STREAM, 0);
    if (0 > lsd) {
        return false;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (0 != bind(lsd, (struct sockaddr *) &addr, sizeof(addr)) || 0 != listen(lsd, 1) ||
        0 != getsockname(lsd, (struct sockaddr *) &addr, &alen)) {
        close(lsd);
        return false;
    }
    *client = socket(AF_INET, SOCK_STREAM, 0);
    if (0 > *client || 0 != connect(*client, (struct sockaddr *) &addr, sizeof(addr)) ||
        0 > (*server = accept(lsd, NULL, NULL))) {
        if (0 <= *client) {
            close(*client);
        }
        close(lsd);
        return false;
    }
    close(lsd);
    return true;
}

/* A native credential for the given identity, built by hand the way a
 * release that predates the local-socket check builds it: the uid and
 * gid, and nothing after them */
static void make_cred(pmix_byte_object_t *cred, uid_t uid, gid_t gid)
{
    cred->size = sizeof(uid_t) + sizeof(gid_t);
    cred->bytes = (char *) malloc(cred->size);
    memcpy(cred->bytes, &uid, sizeof(uid_t));
    memcpy(cred->bytes + sizeof(uid_t), &gid, sizeof(gid_t));
}

/* The same, with the trailer a current release appends: "PNX1", a length
 * byte, and that many bytes naming the peer's kernel */
static void make_trailer_cred(pmix_byte_object_t *cred, uid_t uid, gid_t gid,
                              const char *magic, const char *kid, size_t claimed_len)
{
    size_t idlen = (NULL == kid) ? 0 : strlen(kid);
    char *ptr;

    cred->size = sizeof(uid_t) + sizeof(gid_t) + 4 + 1 + idlen;
    cred->bytes = (char *) malloc(cred->size);
    memcpy(cred->bytes, &uid, sizeof(uid_t));
    memcpy(cred->bytes + sizeof(uid_t), &gid, sizeof(gid_t));
    ptr = cred->bytes + sizeof(uid_t) + sizeof(gid_t);
    memcpy(ptr, magic, 4);
    ptr[4] = (char) claimed_len;
    if (0 < idlen) {
        memcpy(ptr + 5, kid, idlen);
    }
}

/* the storage behind one of native's MCA variables - it aliases the
 * component's own, so a write through it changes what native reads */
static void *native_param(const char *name)
{
    void *storage = NULL;
    int idx;

    idx = pmix_mca_base_var_find("pmix", "psec", "native", name);
    if (0 > idx || PMIX_SUCCESS != pmix_mca_base_var_get_value(idx, &storage, NULL, NULL)) {
        return NULL;
    }
    return storage;
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
    int client;

    /* a peer that is a V2 (tcp) connection from ourselves. native reads
     * the protocol, the connection and the recorded uid/gid; munge and
     * none ignore the protocol and the connection */
    peer = PMIX_NEW(pmix_peer_t);
    peer->protocol = PMIX_PROTOCOL_V2;
    peer->info = PMIX_NEW(pmix_rank_info_t);
    peer->info->uid = geteuid();
    peer->info->gid = getegid();
    if (!loopback_pair(&client, &peer->sd)) {
        report("active modules: loopback connection", 0);
        PMIX_RELEASE(peer->info);
        peer->info = NULL;
        PMIX_RELEASE(peer);
        return;
    }

    PMIX_LIST_FOREACH (active, &pmix_psec_globals.actives, pmix_psec_base_active_module_t) {
        if (NULL == active->module->create_cred || NULL == active->module->validate_cred) {
            /* a handshake-model module - it has no credential to trip */
            continue;
        }
        module_round_trip(active->module, peer, 1);
        module_round_trip(active->module, peer, 2);
    }

    close(client);
    PMIX_RELEASE(peer->info);
    peer->info = NULL;
    PMIX_RELEASE(peer); /* closes peer->sd */
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
    int client;

    mod = pmix_psec_base_assign_module("native");
    if (NULL == mod) {
        report("native module is available", 0);
        return;
    }
    report("native module is available", 1);
    /* a credential, completed by a handshake when the kernel cannot
     * name the owner of the connection */
    report("native has both a credential and a handshake",
           NULL != mod->create_cred && NULL != mod->validate_cred
               && NULL != mod->client_handshake && NULL != mod->server_handshake);

    /* stand up a peer that is a V2 (tcp) connection from ourselves -
     * which is what native is built to authenticate */
    peer = PMIX_NEW(pmix_peer_t);
    peer->protocol = PMIX_PROTOCOL_V2;
    peer->info = PMIX_NEW(pmix_rank_info_t);
    peer->info->uid = geteuid();
    peer->info->gid = getegid();
    if (!loopback_pair(&client, &peer->sd)) {
        report("native: loopback connection", 0);
        PMIX_RELEASE(peer->info);
        peer->info = NULL;
        PMIX_RELEASE(peer);
        return;
    }

    /* create */
    PMIX_BYTE_OBJECT_CONSTRUCT(&cred);
    rc = mod->create_cred((struct pmix_peer_t *) peer, NULL, 0, &results, &nresults, &cred);
    report("create_cred succeeds", PMIX_SUCCESS == rc);
    report("create_cred returns a credential",
           NULL != cred.bytes && (sizeof(uid_t) + sizeof(gid_t) + 5) <= cred.size);
    report("create_cred marks it as able to run the local-socket check",
           NULL != cred.bytes && (sizeof(uid_t) + sizeof(gid_t) + 5) <= cred.size
               && 0 == memcmp(cred.bytes + sizeof(uid_t) + sizeof(gid_t), "PNX1", 4)
               && sizeof(uid_t) + sizeof(gid_t) + 5
                          + (uint8_t) cred.bytes[sizeof(uid_t) + sizeof(gid_t) + 4]
                      == cred.size);
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
    PMIX_BYTE_OBJECT_DESTRUCT(&cred);

    /* a trailer is ours, and holds what it says it holds - or the
     * credential is refused */
    make_trailer_cred(&cred, geteuid(), getegid(), "XXXX", "abc", 3);
    rc = mod->validate_cred((struct pmix_peer_t *) peer, NULL, 0, &results, &nresults, &cred);
    report("validate_cred rejects a trailer that is not native's", PMIX_ERR_INVALID_CRED == rc);
    PMIX_BYTE_OBJECT_DESTRUCT(&cred);
    make_trailer_cred(&cred, geteuid(), getegid(), "PNX1", "abc", 200);
    rc = mod->validate_cred((struct pmix_peer_t *) peer, NULL, 0, &results, &nresults, &cred);
    report("validate_cred rejects a trailer longer than the credential",
           PMIX_ERR_INVALID_CRED == rc);
    PMIX_BYTE_OBJECT_DESTRUCT(&cred);
    make_cred(&cred, geteuid(), getegid());
    cred.size = sizeof(uid_t) + sizeof(gid_t) + 2;
    cred.bytes = (char *) realloc(cred.bytes, cred.size);
    memcpy(cred.bytes + sizeof(uid_t) + sizeof(gid_t), "PN", 2);
    rc = mod->validate_cred((struct pmix_peer_t *) peer, NULL, 0, &results, &nresults, &cred);
    report("validate_cred rejects a truncated trailer", PMIX_ERR_INVALID_CRED == rc);

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

    close(client);
    PMIX_RELEASE(peer->info);
    peer->info = NULL;
    PMIX_RELEASE(peer); /* closes peer->sd */
}

/* Credentials whose identity does not match the connection. Each is
 * presented over a connection this process owns, so what decides it is
 * the kernel's answer about who owns that connection. */
static void native_identity_checks(void)
{
    pmix_psec_module_t *mod;
    pmix_peer_t *peer;
    pmix_byte_object_t cred;
    pmix_info_t *results = NULL, *ptr;
    size_t nresults = 0;
    pmix_status_t rc;
    uid_t other_uid;
    gid_t other_gid;
    int client;

    mod = pmix_psec_base_assign_module("native");
    if (NULL == mod) {
        report("native module is available for the identity cases", 0);
        return;
    }
    peer = PMIX_NEW(pmix_peer_t);
    peer->protocol = PMIX_PROTOCOL_V2;
    peer->info = PMIX_NEW(pmix_rank_info_t);
    if (!loopback_pair(&client, &peer->sd)) {
        report("identity cases: loopback connection", 0);
        PMIX_RELEASE(peer->info);
        peer->info = NULL;
        PMIX_RELEASE(peer);
        return;
    }

    /* Someone else's uid - root's, unless that is us. A client the host
     * registered as that user, and whose credential claims to be it, is
     * still not it: only the kernel's answer can turn it away */
    other_uid = (0 == geteuid()) ? (uid_t) 4242 : (uid_t) 0;
    peer->info->host_registered = true;
    peer->info->uid = other_uid;
    peer->info->gid = getegid();
    make_cred(&cred, other_uid, getegid());
    rc = mod->validate_cred((struct pmix_peer_t *) peer, NULL, 0, &results, &nresults, &cred);
    report("validate_cred refuses a client registered as another user",
           PMIX_ERR_INVALID_CRED == rc);
    if (NULL != results) {
        PMIX_INFO_FREE(results, nresults);
        results = NULL;
        nresults = 0;
    }
    PMIX_BYTE_OBJECT_DESTRUCT(&cred);

    /* A client registered as us whose credential claims someone else is
     * judged by the connection, not by the claim - inside a user
     * namespace the claim is the uid there, which is not ours */
    peer->info->uid = geteuid();
    make_cred(&cred, other_uid, getegid());
    rc = mod->validate_cred((struct pmix_peer_t *) peer, NULL, 0, &results, &nresults, &cred);
    report("validate_cred judges a registered client by its connection, not its claim",
           PMIX_SUCCESS == rc && geteuid() == peer->info->uid);
    if (NULL != results) {
        PMIX_INFO_FREE(results, nresults);
        results = NULL;
        nresults = 0;
    }
    PMIX_BYTE_OBJECT_DESTRUCT(&cred);

    /* A registered client must hold the group it was registered with */
    if (0 != geteuid()) {
        other_gid = (gid_t) 54321;
#ifdef HAVE_GRP_H
        while (NULL != getgrgid(other_gid) || getegid() == other_gid || getgid() == other_gid) {
            other_gid++;
        }
#endif
        peer->info->gid = other_gid;
        make_cred(&cred, geteuid(), getegid());
        rc = mod->validate_cred((struct pmix_peer_t *) peer, NULL, 0, &results, &nresults, &cred);
        report("validate_cred refuses a client whose group is not the registered one",
               PMIX_ERR_INVALID_CRED == rc);
        PMIX_BYTE_OBJECT_DESTRUCT(&cred);
        /* ...unless it can have its group attested: the claimed group
         * may be the one inside a user namespace */
        make_trailer_cred(&cred, geteuid(), getegid(), "PNX1", NULL, 0);
        rc = mod->validate_cred((struct pmix_peer_t *) peer, NULL, 0, &results, &nresults, &cred);
        report("validate_cred asks a current client with an unconfirmed group for the handshake",
               PMIX_ERR_READY_FOR_HANDSHAKE == rc);
        PMIX_BYTE_OBJECT_DESTRUCT(&cred);
        peer->info->gid = getegid();
    }

    /* An identity nobody registered - a tool's - is only a claim, and
     * the owner of the connection replaces it. Claiming to be root
     * gets a tool nothing but its own uid */
    peer->info->host_registered = false;
    peer->info->uid = other_uid;
    peer->info->gid = getegid();
    make_cred(&cred, other_uid, getegid());
    rc = mod->validate_cred((struct pmix_peer_t *) peer, NULL, 0, &results, &nresults, &cred);
    report("validate_cred replaces a tool's claimed uid with the connection's owner",
           PMIX_SUCCESS == rc && geteuid() == peer->info->uid);
    ptr = (NULL == results) ? NULL : find_key(results, nresults, PMIX_USERID);
    report("validate_cred reports the uid it settled on",
           NULL != ptr && PMIX_UINT32 == ptr->value.type
               && (uint32_t) geteuid() == ptr->value.data.uint32);
    if (NULL != results) {
        PMIX_INFO_FREE(results, nresults);
        results = NULL;
        nresults = 0;
    }
    PMIX_BYTE_OBJECT_DESTRUCT(&cred);

    /* A tool names its own group - nobody registered one for it - and
     * the kernel records none for a TCP socket, so a gid its user does
     * not hold has to be refused. A gid with no group entry at all is
     * held by nobody but root */
    if (0 != geteuid()) {
        other_gid = (gid_t) 54321;
#ifdef HAVE_GRP_H
        while (NULL != getgrgid(other_gid) || getegid() == other_gid || getgid() == other_gid) {
            other_gid++;
        }
#endif
        peer->info->uid = geteuid();
        peer->info->gid = other_gid;
        make_cred(&cred, geteuid(), other_gid);
        rc = mod->validate_cred((struct pmix_peer_t *) peer, NULL, 0, &results, &nresults, &cred);
        report("validate_cred refuses a tool's claim of a group it does not hold",
               PMIX_ERR_INVALID_CRED == rc);
        PMIX_BYTE_OBJECT_DESTRUCT(&cred);
        make_trailer_cred(&cred, geteuid(), other_gid, "PNX1", NULL, 0);
        rc = mod->validate_cred((struct pmix_peer_t *) peer, NULL, 0, &results, &nresults, &cred);
        report("validate_cred asks a current tool with an unconfirmed group for the handshake",
               PMIX_ERR_READY_FOR_HANDSHAKE == rc && other_gid == peer->info->gid);
        PMIX_BYTE_OBJECT_DESTRUCT(&cred);

        /* and the same tool claiming its own group is fine */
        peer->info->gid = getegid();
        make_cred(&cred, geteuid(), getegid());
        rc = mod->validate_cred((struct pmix_peer_t *) peer, NULL, 0, &results, &nresults, &cred);
        report("validate_cred accepts a tool's claim of its own group", PMIX_SUCCESS == rc);
        if (NULL != results) {
            PMIX_INFO_FREE(results, nresults);
            results = NULL;
            nresults = 0;
        }
        PMIX_BYTE_OBJECT_DESTRUCT(&cred);
    }

    close(client);
    PMIX_RELEASE(peer->info);
    peer->info = NULL;
    PMIX_RELEASE(peer); /* closes peer->sd */
}

/* When the kernel cannot name the owner of the connection. A connection
 * whose far end has closed has no owner, which is what a peer in a
 * container with its own network looks like from here. */
static void native_unconfirmed(void)
{
    pmix_psec_module_t *mod;
    pmix_peer_t *peer;
    pmix_byte_object_t cred;
    pmix_info_t *results = NULL;
    size_t nresults = 0;
    pmix_status_t rc;
    bool *legacy;
    int client;

    mod = pmix_psec_base_assign_module("native");
    legacy = (bool *) native_param("legacy_auth");
    report("native's legacy_auth parameter is registered", NULL != legacy);
    if (NULL == mod || NULL == legacy) {
        return;
    }
    report("legacy_auth defaults to true", *legacy);
    peer = PMIX_NEW(pmix_peer_t);
    peer->protocol = PMIX_PROTOCOL_V2;
    peer->info = PMIX_NEW(pmix_rank_info_t);
    peer->info->host_registered = true;
    peer->info->uid = geteuid();
    peer->info->gid = getegid();
    if (!loopback_pair(&client, &peer->sd)) {
        report("unconfirmed cases: loopback connection", 0);
        PMIX_RELEASE(peer->info);
        peer->info = NULL;
        PMIX_RELEASE(peer);
        return;
    }
    close(client);
    usleep(100000);

    /* an older release, which has no other way to say who it is */
    make_cred(&cred, geteuid(), getegid());
    *legacy = true;
    rc = mod->validate_cred((struct pmix_peer_t *) peer, NULL, 0, &results, &nresults, &cred);
    report("an older peer the kernel cannot confirm is accepted on its claim when allowed",
           PMIX_SUCCESS == rc);
    if (NULL != results) {
        PMIX_INFO_FREE(results, nresults);
        results = NULL;
        nresults = 0;
    }
    *legacy = false;
    rc = mod->validate_cred((struct pmix_peer_t *) peer, NULL, 0, &results, &nresults, &cred);
    report("an older peer the kernel cannot confirm is refused when not allowed",
           PMIX_ERR_INVALID_CRED == rc);
    *legacy = true;
    PMIX_BYTE_OBJECT_DESTRUCT(&cred);

    /* even when allowed, an older peer's claim must agree with the
     * registration */
    make_cred(&cred, (0 == geteuid()) ? (uid_t) 4242 : (uid_t) 0, getegid());
    rc = mod->validate_cred((struct pmix_peer_t *) peer, NULL, 0, &results, &nresults, &cred);
    report("an older peer's claim must match its registration", PMIX_ERR_INVALID_CRED == rc);
    PMIX_BYTE_OBJECT_DESTRUCT(&cred);

    /* a current release is asked to prove itself over a local socket -
     * whatever legacy_auth says */
    make_trailer_cred(&cred, geteuid(), getegid(), "PNX1", NULL, 0);
    *legacy = false;
    rc = mod->validate_cred((struct pmix_peer_t *) peer, NULL, 0, &results, &nresults, &cred);
    report("a current peer the kernel cannot confirm is asked for the handshake",
           PMIX_ERR_READY_FOR_HANDSHAKE == rc);
    *legacy = true;
    PMIX_BYTE_OBJECT_DESTRUCT(&cred);

    /* ...unless it says it runs on another host's kernel, where no
     * local socket can reach it */
    make_trailer_cred(&cred, geteuid(), getegid(), "PNX1", "another-hosts-kernel", 20);
    rc = mod->validate_cred((struct pmix_peer_t *) peer, NULL, 0, &results, &nresults, &cred);
#if defined(__linux__) || defined(__APPLE__)
    report("a current peer on another host is refused", PMIX_ERR_INVALID_CRED == rc);
#else
    /* with no way to name our own kernel, every peer might be local */
    report("a current peer on another host is asked for the handshake",
           PMIX_ERR_READY_FOR_HANDSHAKE == rc);
#endif
    PMIX_BYTE_OBJECT_DESTRUCT(&cred);

    PMIX_RELEASE(peer->info);
    peer->info = NULL;
    PMIX_RELEASE(peer); /* closes peer->sd */
}

/* The local-socket handshake, end to end. The server half runs here and
 * the client half on a thread, over a loopback connection - the client
 * half needs nothing but the socket */
typedef struct {
    pmix_psec_module_t *mod;
    int sd;
    bool close_after;
    pmix_status_t rc;
} hs_client_t;

static void *hs_client(void *arg)
{
    hs_client_t *c = (hs_client_t *) arg;

    c->rc = c->mod->client_handshake(NULL, c->sd);
    if (c->close_after) {
        /* as ptl does when a handshake fails */
        close(c->sd);
        c->sd = -1;
    }
    return NULL;
}

/* A peer that does reach the socket, and then sends the nonce one byte a
 * second. Every byte arrives well inside any per-read timeout, so only a
 * deadline over the whole handshake stops the server waiting for it */
static bool read_full(int sd, void *buf, size_t len)
{
    size_t got = 0;
    ssize_t rd;

    while (got < len) {
        rd = recv(sd, (char *) buf + got, len - got, 0);
        if (0 >= rd) {
            return false;
        }
        got += (size_t) rd;
    }
    return true;
}

static char *read_str(int sd)
{
    uint32_t u32;
    char *str;

    if (!read_full(sd, &u32, sizeof(u32)) || 0 == ntohl(u32) || 4096 < ntohl(u32)) {
        return NULL;
    }
    str = (char *) malloc(ntohl(u32));
    if (!read_full(sd, str, ntohl(u32))) {
        free(str);
        return NULL;
    }
    return str;
}

static void *hs_trickler(void *arg)
{
    hs_client_t *c = (hs_client_t *) arg;
    unsigned char nonce[32];
    struct sockaddr_un addr;
    uint32_t u32;
    char *base = NULL, *rel = NULL;
    int usd = -1;
    size_t n;

    c->rc = PMIX_ERROR;
    if (!read_full(c->sd, &u32, sizeof(u32)) || sizeof(nonce) != ntohl(u32) ||
        !read_full(c->sd, nonce, sizeof(nonce)) || NULL == (base = read_str(c->sd)) ||
        NULL == (rel = read_str(c->sd))) {
        goto done;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s/%s", base, rel);
    usd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (0 > usd || 0 != connect(usd, (struct sockaddr *) &addr, sizeof(addr))) {
        goto done;
    }
    for (n = 0; n < sizeof(nonce); n++) {
        /* stop once the server has answered on the TCP connection */
        struct timeval tv = {1, 0};
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(c->sd, &fds);
        if (0 < select(c->sd + 1, &fds, NULL, NULL, &tv)) {
            break;
        }
        if (1 != send(usd, &nonce[n], 1, 0)) {
            break;
        }
    }
    c->rc = PMIX_SUCCESS;
done:
    if (0 <= usd) {
        close(usd);
    }
    free(base);
    free(rel);
    return NULL;
}

static pmix_status_t run_handshake(pmix_psec_module_t *mod, pmix_peer_t *peer, bool close_after,
                                   pmix_status_t *client_rc)
{
    /* a NULL client module means the trickling peer */
    pmix_psec_module_t *server = pmix_psec_base_assign_module("native");
    hs_client_t c;
    pthread_t thr;
    pmix_status_t rc;
    int client;

    *client_rc = PMIX_ERROR;
    if (!loopback_pair(&client, &peer->sd)) {
        return PMIX_ERR_UNREACH;
    }
    c.mod = mod;
    c.sd = client;
    c.close_after = close_after;
    c.rc = PMIX_ERROR;
    if (0 != pthread_create(&thr, NULL, (NULL == mod) ? hs_trickler : hs_client, &c)) {
        close(client);
        close(peer->sd);
        peer->sd = -1;
        return PMIX_ERROR;
    }
    rc = server->server_handshake((struct pmix_peer_t *) peer, peer->sd);
    pthread_join(thr, NULL);
    *client_rc = c.rc;
    if (0 <= c.sd) {
        close(c.sd);
    }
    close(peer->sd);
    peer->sd = -1;
    return rc;
}

static void native_handshake(void)
{
    pmix_psec_module_t *mod;
    pmix_peer_t *peer;
    pmix_status_t rc, crc;
    char **sockdir;
    gid_t other_gid;

    mod = pmix_psec_base_assign_module("native");
    sockdir = (char **) native_param("socket_dir");
    report("native's socket_dir parameter is registered", NULL != sockdir);
    if (NULL == mod || NULL == sockdir) {
        return;
    }
    peer = PMIX_NEW(pmix_peer_t);
    peer->protocol = PMIX_PROTOCOL_V2;
    peer->info = PMIX_NEW(pmix_rank_info_t);

    /* a client the host registered as us */
    peer->info->host_registered = true;
    peer->info->uid = geteuid();
    peer->info->gid = getegid();
    rc = run_handshake(mod, peer, false, &crc);
    report("handshake confirms a client registered as its own user",
           PMIX_SUCCESS == rc && PMIX_SUCCESS == crc);

    /* a client registered as somebody else is refused - and told so */
    peer->info->uid = (0 == geteuid()) ? (uid_t) 4242 : (uid_t) 0;
    rc = run_handshake(mod, peer, false, &crc);
    report("handshake refuses a client registered as another user",
           PMIX_ERR_INVALID_CRED == rc && PMIX_ERR_INVALID_CRED == crc);

    /* a tool's claimed identity is replaced by the one the socket
     * reports - its uid, and its group unless it claimed one it holds */
    peer->info->host_registered = false;
    peer->info->uid = (0 == geteuid()) ? (uid_t) 4242 : (uid_t) 0;
    other_gid = (gid_t) 54321;
#ifdef HAVE_GRP_H
    while (NULL != getgrgid(other_gid) || getegid() == other_gid || getgid() == other_gid) {
        other_gid++;
    }
#endif
    peer->info->gid = (0 == geteuid()) ? getegid() : other_gid;
    rc = run_handshake(mod, peer, false, &crc);
    report("handshake replaces a tool's claimed identity with the socket's",
           PMIX_SUCCESS == rc && PMIX_SUCCESS == crc && geteuid() == peer->info->uid
               && getegid() == peer->info->gid);

    /* the peer sees the server's directory somewhere else */
    *sockdir = strdup("/nonexistent-pmix-native-dir");
    peer->info->host_registered = true;
    peer->info->uid = geteuid();
    peer->info->gid = getegid();
    rc = run_handshake(mod, peer, true, &crc);
    report("handshake fails cleanly when the peer cannot reach the socket",
           PMIX_ERR_INVALID_CRED == rc && PMIX_SUCCESS != crc);
    free(*sockdir);
    *sockdir = NULL;

    /* a peer that sends its nonce a byte a second takes 32 seconds; the
     * server must give up at the handshake deadline (5 seconds by default)
     * whatever the peer does in between */
    {
        struct timeval t0, t1;
        double secs;

        gettimeofday(&t0, NULL);
        rc = run_handshake(NULL, peer, false, &crc);
        gettimeofday(&t1, NULL);
        secs = (double) (t1.tv_sec - t0.tv_sec) + (double) (t1.tv_usec - t0.tv_usec) / 1e6;
        fprintf(stdout, "    trickling peer refused after %.1f seconds\n", secs);
        report("handshake gives up on a trickling peer at one deadline for the whole exchange",
               PMIX_ERR_INVALID_CRED == rc && PMIX_SUCCESS == crc && 8.0 > secs);
    }

    PMIX_RELEASE(peer->info);
    peer->info = NULL;
    PMIX_RELEASE(peer);
}

/* A credential with no connection behind it - which is what
 * PMIx_Validate_credential hands native - is refused. */
static void native_unverified(void)
{
    pmix_psec_module_t *mod;
    pmix_peer_t *peer;
    pmix_byte_object_t cred;
    pmix_info_t *results = NULL;
    size_t nresults = 0;
    pmix_status_t rc;

    mod = pmix_psec_base_assign_module("native");
    if (NULL == mod) {
        report("native module is available for the unverified cases", 0);
        return;
    }
    peer = PMIX_NEW(pmix_peer_t);
    peer->protocol = PMIX_PROTOCOL_V2;
    peer->info = PMIX_NEW(pmix_rank_info_t);
    peer->info->uid = geteuid();
    peer->info->gid = getegid();

    make_cred(&cred, geteuid(), getegid());
    rc = mod->validate_cred((struct pmix_peer_t *) peer, NULL, 0, &results, &nresults, &cred);
    report("a credential on no connection is refused", PMIX_ERR_INVALID_CRED == rc);
    if (NULL != results) {
        PMIX_INFO_FREE(results, nresults);
    }
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

    /* nor can one that authenticates nobody - psec/none accepted every
     * peer and psec/dummy_handshake swapped a fixed string, and neither
     * may come back */
    report("there is no 'none' mechanism", NULL == pmix_psec_base_assign_module("none"));
    report("there is no 'dummy_handshake' mechanism",
           NULL == pmix_psec_base_assign_module("dummy_handshake"));

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
    native_identity_checks();
    native_unconfirmed();
    native_handshake();
    native_unverified();

    (void) pmix_mca_base_framework_close(&pmix_psec_base_framework);

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);

    return (nfail > 0) ? 1 : 0;
}
