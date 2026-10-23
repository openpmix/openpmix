/*
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 *
 * NOTE: THE MUNGE CLIENT LIBRARY (libmunge) IS LICENSED AS LGPL
 *
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "src/include/pmix_config.h"

#include "pmix_common.h"

#include "src/include/pmix_globals.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_output.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef HAVE_SYS_TYPES_H
#    include <sys/types.h>
#endif

// Non-functional shim so this component can be compile-checked without
// libmunge when the library is configured with --enable-test-build.
// The encode/decode stubs deliberately return a failure code (never
// EMUNGE_SUCCESS): munge_init treats a failed encode as "the munge
// daemon is unreachable" and returns PMIX_ERR_SERVER_NOT_AVAIL, so the
// psec framework de-selects this component at runtime. That keeps a
// test-build library from selecting a non-functional MUNGE module and
// crashing when it later tries to use the (never-produced) credential.
#if PMIX_TESTBUILD
typedef int32_t munge_err_t;

/* as libmunge has it: a pointer to a context of its own */
typedef struct munge_ctx *munge_ctx_t;

#define EMUNGE_SUCCESS 0
#define EMUNGE_SNAFU   1

static inline munge_err_t munge_encode (char **cred, munge_ctx_t ctx,
                                        const void *buf, int len)
{
    PMIX_HIDE_UNUSED_PARAMS(cred, ctx, buf, len);
    return EMUNGE_SNAFU;
}

static inline  munge_err_t munge_decode (const char *cred, munge_ctx_t ctx,
                                         void **buf, int *len, uid_t *uid, gid_t *gid)
{
    PMIX_HIDE_UNUSED_PARAMS(cred, ctx, buf, len, uid, gid);
    return EMUNGE_SNAFU;
}

static inline  const char * munge_strerror (munge_err_t e)
{
    PMIX_HIDE_UNUSED_PARAMS(e);
    return "munge testbuild shim - not functional";
}

#define MUNGE_OPT_UID_RESTRICTION 1

static inline munge_ctx_t munge_ctx_create(void)
{
    return NULL;
}

static inline void munge_ctx_destroy(munge_ctx_t ctx)
{
    PMIX_HIDE_UNUSED_PARAMS(ctx);
}

static inline munge_err_t munge_ctx_set(munge_ctx_t ctx, int opt, ...)
{
    PMIX_HIDE_UNUSED_PARAMS(ctx, opt);
    return EMUNGE_SNAFU;
}

#else
#include <munge.h>
#endif

#include "psec_munge.h"
#include "src/mca/psec/base/base.h"
#include "src/threads/pmix_threads.h"

static pmix_status_t munge_init(void);
static void munge_finalize(void);
static pmix_status_t create_cred(struct pmix_peer_t *peer, const pmix_info_t directives[],
                                 size_t ndirs, pmix_info_t **info, size_t *ninfo,
                                 pmix_byte_object_t *cred);
static pmix_status_t validate_cred(struct pmix_peer_t *peer, const pmix_info_t directives[],
                                   size_t ndirs, pmix_info_t **info, size_t *ninfo,
                                   const pmix_byte_object_t *cred);
static pmix_status_t create_bound(const pmix_byte_object_t *bind, bool server, uid_t to,
                                  pmix_byte_object_t *cred);
static pmix_status_t validate_bound(const pmix_byte_object_t *bind, bool server,
                                    const pmix_byte_object_t *cred, uid_t *uid, gid_t *gid);

pmix_psec_module_t pmix_munge_module = {.name = "munge",
                                        .init = munge_init,
                                        .finalize = munge_finalize,
                                        .create_cred = create_cred,
                                        .validate_cred = validate_cred,
                                        .create_bound = create_bound,
                                        .validate_bound = validate_bound};

static char *mycred = NULL;
static bool initialized = false;
static bool refresh = false;
/* PMIx_Get_credential does not thread-shift - it calls create_cred
 * inline on whichever thread invoked it - so two application threads can
 * be inside create_cred at the same time, both refreshing the cached
 * credential. Serialize every access to mycred/refresh. This is not a
 * hot path: it runs only when a credential is requested. */
static pmix_mutex_t credlock = PMIX_MUTEX_STATIC_INIT;

static pmix_status_t munge_init(void)
{
    int rc;

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "psec: munge init");

    /* attempt to get a credential as a way of checking that
     * the munge server is available - cache the credential
     * for later use */

    pmix_mutex_lock(&credlock);
    if (EMUNGE_SUCCESS != (rc = munge_encode(&mycred, NULL, NULL, 0))) {
        pmix_output_verbose(2, pmix_globals.debug_output,
                            "psec: munge failed to create credential: %s", munge_strerror(rc));
        pmix_mutex_unlock(&credlock);
        return PMIX_ERR_SERVER_NOT_AVAIL;
    }

    initialized = true;
    pmix_mutex_unlock(&credlock);

    return PMIX_SUCCESS;
}

static void munge_finalize(void)
{
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "psec: munge finalize");

    pmix_mutex_lock(&credlock);
    if (NULL != mycred) {
        free(mycred);
        mycred = NULL;
    }
    /* reset our state so that a subsequent re-init of the library
     * starts from a clean slate rather than from a cached credential
     * that no longer exists */
    initialized = false;
    refresh = false;
    pmix_mutex_unlock(&credlock);
}

static pmix_status_t create_cred(struct pmix_peer_t *peer, const pmix_info_t directives[],
                                 size_t ndirs, pmix_info_t **info, size_t *ninfo,
                                 pmix_byte_object_t *cred)
{
    int rc;
    PMIX_HIDE_UNUSED_PARAMS(peer);

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "psec: munge create_cred");

    /* ensure initialization */
    PMIX_BYTE_OBJECT_CONSTRUCT(cred);

    /* if we are responding to a local request to create a credential,
     * then see if they specified a mechanism */
    if (!pmix_psec_base_check_directives("munge", directives, ndirs)) {
        return PMIX_ERR_NOT_SUPPORTED;
    }

    pmix_mutex_lock(&credlock);
    if (!initialized) {
        /* we have no munged daemon to issue a credential, so we cannot
         * hand back an empty one and call it success */
        pmix_mutex_unlock(&credlock);
        return PMIX_ERR_NOT_SUPPORTED;
    }

    if (!refresh) {
        refresh = true;
    } else {
        /* munge does not allow reuse of a credential, so we have to
         * refresh it for every use. Drop our reference before asking
         * for a new one so a failed encode cannot leave us holding a
         * pointer to freed memory */
        if (NULL != mycred) {
            free(mycred);
            mycred = NULL;
        }
        if (EMUNGE_SUCCESS != (rc = munge_encode(&mycred, NULL, NULL, 0))) {
            pmix_output_verbose(2, pmix_globals.debug_output,
                                "psec: munge failed to create credential: %s",
                                munge_strerror(rc));
            pmix_mutex_unlock(&credlock);
            return PMIX_ERR_NOT_SUPPORTED;
        }
    }
    if (NULL == mycred) {
        pmix_mutex_unlock(&credlock);
        return PMIX_ERR_NOT_SUPPORTED;
    }
    cred->bytes = strdup(mycred);
    if (NULL != cred->bytes) {
        cred->size = strlen(mycred) + 1;
    }
    pmix_mutex_unlock(&credlock);
    if (NULL == cred->bytes) {
        return PMIX_ERR_NOMEM;
    }

    if (NULL != info) {
        /* mark that this came from us */
        PMIX_INFO_CREATE(*info, 1);
        if (NULL == *info) {
            /* our caller only reclaims the credential when we report
             * success, so release it here rather than stranding it */
            PMIX_BYTE_OBJECT_DESTRUCT(cred);
            return PMIX_ERR_NOMEM;
        }
        *ninfo = 1;
        PMIX_INFO_LOAD(&(*info)[0], PMIX_CRED_TYPE, "munge", PMIX_STRING);
    }
    return PMIX_SUCCESS;
}

static pmix_status_t validate_cred(struct pmix_peer_t *peer, const pmix_info_t directives[],
                                   size_t ndirs, pmix_info_t **info, size_t *ninfo,
                                   const pmix_byte_object_t *cred)
{
    pmix_peer_t *pr = (pmix_peer_t *) peer;
    uid_t euid;
    gid_t egid;
    munge_err_t rc;
    uint32_t u32;
    void *payload = NULL;
    int plen = 0;

    pmix_output_verbose(2, pmix_globals.debug_output, "psec: munge validate_cred %s",
                        (NULL == cred) ? "NULL" : "NON-NULL");

    /* if we are responding to a local request to validate a credential,
     * then see if they specified a mechanism */
    if (!pmix_psec_base_check_directives("munge", directives, ndirs)) {
        return PMIX_ERR_NOT_SUPPORTED;
    }

    /* munge_decode takes a NUL-terminated string. The credential reaches
     * us as a counted byte object that a remote peer supplied, so we
     * cannot assume it is terminated - hand it on only once we have
     * confirmed that it is */
    if (NULL == cred || NULL == cred->bytes || 0 == cred->size
        || '\0' != cred->bytes[cred->size - 1]) {
        pmix_output_verbose(2, pmix_globals.debug_output,
                            "psec: munge given a malformed credential");
        return PMIX_ERR_INVALID_CRED;
    }

    /* parse the inbound string. A credential made here carries nothing
     * else; one with a payload was made for one connection between hosts
     * (create_bound), and is accepted nowhere but there */
    if (EMUNGE_SUCCESS != (rc = munge_decode(cred->bytes, NULL, &payload, &plen, &euid, &egid))) {
        pmix_output_verbose(2, pmix_globals.debug_output,
                            "psec: munge failed to decode credential: %s", munge_strerror(rc));
        free(payload);
        return PMIX_ERR_INVALID_CRED;
    }
    free(payload);
    if (0 != plen) {
        pmix_output_verbose(2, pmix_globals.debug_output,
                            "psec: munge credential carries a payload - not one of ours");
        return PMIX_ERR_INVALID_CRED;
    }

    /* A credential that came with a connection must be from the user and
     * group that connected. One handed to PMIx_Validate_credential has no
     * connection - it is checked by munge alone, and the identity it
     * carries is what we report */
    if (0 <= pr->sd) {
        if (NULL == pr->info) {
            return PMIX_ERR_INVALID_CRED;
        }
        if (euid != pr->info->uid || egid != pr->info->gid) {
            return PMIX_ERR_INVALID_CRED;
        }
    }

    pmix_output_verbose(2, pmix_globals.debug_output, "psec: munge credential valid");
    if (NULL != info) {
        PMIX_INFO_CREATE(*info, 3);
        if (NULL == *info) {
            return PMIX_ERR_NOMEM;
        }
        *ninfo = 3;
        /* mark that this came from us */
        PMIX_INFO_LOAD(&(*info)[0], PMIX_CRED_TYPE, "munge", PMIX_STRING);
        /* provide the uid it contained */
        u32 = euid;
        PMIX_INFO_LOAD(&(*info)[1], PMIX_USERID, &u32, PMIX_UINT32);
        /* provide the gid it contained */
        u32 = egid;
        PMIX_INFO_LOAD(&(*info)[2], PMIX_GRPID, &u32, PMIX_UINT32);
    }
    return PMIX_SUCCESS;
}

/* A credential for one connection between hosts carries the bytes it is
 * bound to as its payload, behind a byte saying whose it is, and munge
 * seals both with the credential. It may be decoded only by a process of
 * the user `to`, when that is given. A credential is encoded afresh for
 * each call: munge accepts each one only once on a host */
#define MUNGE_ROLE_SERVER 'S'
#define MUNGE_ROLE_TOOL   'T'

static pmix_status_t create_bound(const pmix_byte_object_t *bind, bool server, uid_t to,
                                  pmix_byte_object_t *cred)
{
    char *c = NULL, *payload;
    munge_ctx_t ctx = NULL;
    munge_err_t rc;

    PMIX_BYTE_OBJECT_CONSTRUCT(cred);
    if (NULL == bind || NULL == bind->bytes || 0 == bind->size || INT_MAX < bind->size) {
        return PMIX_ERR_BAD_PARAM;
    }
    pmix_mutex_lock(&credlock);
    if (!initialized) {
        pmix_mutex_unlock(&credlock);
        return PMIX_ERR_NOT_SUPPORTED;
    }
    pmix_mutex_unlock(&credlock);

    payload = (char *) malloc(bind->size + 1);
    if (NULL == payload) {
        return PMIX_ERR_NOMEM;
    }
    payload[0] = server ? MUNGE_ROLE_SERVER : MUNGE_ROLE_TOOL;
    memcpy(payload + 1, bind->bytes, bind->size);
    if ((uid_t) -1 != to) {
        ctx = munge_ctx_create();
        if (NULL == ctx || EMUNGE_SUCCESS != munge_ctx_set(ctx, MUNGE_OPT_UID_RESTRICTION, to)) {
            if (NULL != ctx) {
                munge_ctx_destroy(ctx);
            }
            free(payload);
            return PMIX_ERR_NOT_SUPPORTED;
        }
    }
    rc = munge_encode(&c, ctx, payload, (int) bind->size + 1);
    if (NULL != ctx) {
        munge_ctx_destroy(ctx);
    }
    free(payload);
    if (EMUNGE_SUCCESS != rc || NULL == c) {
        pmix_output_verbose(2, pmix_globals.debug_output,
                            "psec: munge failed to create a bound credential: %s",
                            munge_strerror(rc));
        free(c);
        return PMIX_ERR_NOT_SUPPORTED;
    }
    cred->bytes = c;
    cred->size = strlen(c) + 1;
    return PMIX_SUCCESS;
}

static pmix_status_t validate_bound(const pmix_byte_object_t *bind, bool server,
                                    const pmix_byte_object_t *cred, uid_t *uid, gid_t *gid)
{
    void *payload = NULL;
    int len = 0;
    uid_t euid;
    gid_t egid;
    munge_err_t rc;
    bool match;

    if (NULL == bind || NULL == bind->bytes || 0 == bind->size) {
        return PMIX_ERR_BAD_PARAM;
    }
    /* munge_decode takes a NUL-terminated string - see validate_cred */
    if (NULL == cred || NULL == cred->bytes || 0 == cred->size ||
        '\0' != cred->bytes[cred->size - 1]) {
        pmix_output_verbose(2, pmix_globals.debug_output,
                            "psec: munge given a malformed bound credential");
        return PMIX_ERR_INVALID_CRED;
    }
    rc = munge_decode(cred->bytes, NULL, &payload, &len, &euid, &egid);
    if (EMUNGE_SUCCESS != rc) {
        pmix_output_verbose(2, pmix_globals.debug_output,
                            "psec: munge failed to decode a bound credential: %s",
                            munge_strerror(rc));
        free(payload);
        return PMIX_ERR_INVALID_CRED;
    }
    match = (NULL != payload && 0 <= len && (size_t) len == bind->size + 1 &&
             (server ? MUNGE_ROLE_SERVER : MUNGE_ROLE_TOOL) == ((char *) payload)[0] &&
             0 == memcmp((char *) payload + 1, bind->bytes, bind->size));
    free(payload);
    if (!match) {
        pmix_output_verbose(2, pmix_globals.debug_output,
                            "psec: munge bound credential was made for another connection, "
                            "or as the other role");
        return PMIX_ERR_INVALID_CRED;
    }
    *uid = euid;
    *gid = egid;
    return PMIX_SUCCESS;
}
