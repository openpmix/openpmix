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

typedef struct {
    int32_t idx;
} munge_ctx_t;

#define EMUNGE_SUCCESS 0
#define EMUNGE_SNAFU   1

static inline munge_err_t munge_encode (char **cred, munge_ctx_t *ctx,
                                        const void *buf, int len)
{
    PMIX_HIDE_UNUSED_PARAMS(cred, ctx, buf, len);
    return EMUNGE_SNAFU;
}

static inline  munge_err_t munge_decode (const char *cred, munge_ctx_t *ctx,
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

pmix_psec_module_t pmix_munge_module = {.name = "munge",
                                        .init = munge_init,
                                        .finalize = munge_finalize,
                                        .create_cred = create_cred,
                                        .validate_cred = validate_cred};

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

    /* parse the inbound string */
    if (EMUNGE_SUCCESS != (rc = munge_decode(cred->bytes, NULL, NULL, NULL, &euid, &egid))) {
        pmix_output_verbose(2, pmix_globals.debug_output,
                            "psec: munge failed to decode credential: %s", munge_strerror(rc));
        return PMIX_ERR_INVALID_CRED;
    }

    /* check uid */
    if (euid != pr->info->uid) {
        return PMIX_ERR_INVALID_CRED;
    }

    /* check guid */
    if (egid != pr->info->gid) {
        return PMIX_ERR_INVALID_CRED;
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
