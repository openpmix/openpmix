/*
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2016      IBM Corporation.  All rights reserved.
 * Copyright (c) 2017      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "src/include/pmix_config.h"

#include <unistd.h>
#ifdef HAVE_SYS_TYPES_H
#    include <sys/types.h>
#endif

#include "pmix_common.h"

#include "src/include/pmix_globals.h"
#include "src/include/pmix_socket_errno.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_output.h"

#include "psec_native.h"
#include "src/mca/psec/base/base.h"

static pmix_status_t native_init(void);
static void native_finalize(void);
static pmix_status_t create_cred(struct pmix_peer_t *peer, const pmix_info_t directives[],
                                 size_t ndirs, pmix_info_t **info, size_t *ninfo,
                                 pmix_byte_object_t *cred);
static pmix_status_t validate_cred(struct pmix_peer_t *peer, const pmix_info_t directives[],
                                   size_t ndirs, pmix_info_t **info, size_t *ninfo,
                                   const pmix_byte_object_t *cred);

pmix_psec_module_t pmix_native_module = {.name = "native",
                                         .init = native_init,
                                         .finalize = native_finalize,
                                         .create_cred = create_cred,
                                         .validate_cred = validate_cred};

static pmix_status_t native_init(void)
{
    pmix_output_verbose(2, pmix_psec_base_framework.framework_output, "psec: native init");
    return PMIX_SUCCESS;
}

static void native_finalize(void)
{
    pmix_output_verbose(2, pmix_psec_base_framework.framework_output, "psec: native finalize");
}

static pmix_status_t create_cred(struct pmix_peer_t *peer, const pmix_info_t directives[],
                                 size_t ndirs, pmix_info_t **info, size_t *ninfo,
                                 pmix_byte_object_t *cred)
{
    pmix_peer_t *pr = (pmix_peer_t *) peer;
    uid_t euid;
    gid_t egid;
    char *tmp, *ptr;

    /* ensure initialization */
    PMIX_BYTE_OBJECT_CONSTRUCT(cred);

    /* we may be responding to a local request for a credential, so
     * see if they specified a mechanism */
    if (!pmix_psec_base_check_directives("native", directives, ndirs)) {
        PMIX_ERROR_LOG(PMIX_ERR_NOT_SUPPORTED);
        return PMIX_ERR_NOT_SUPPORTED;
    }

    if (PMIX_PROTOCOL_V1 == pr->protocol) {
        /* usock protocol - nothing to do */
        goto complete;
    } else if (PMIX_PROTOCOL_V2 == pr->protocol) {
        /* tcp protocol - need to provide our effective
         * uid and gid for validation on remote end */
        tmp = (char *) malloc(sizeof(uid_t) + sizeof(gid_t));
        if (NULL == tmp) {
            return PMIX_ERR_NOMEM;
        }
        euid = geteuid();
        memcpy(tmp, &euid, sizeof(uid_t));
        ptr = tmp + sizeof(uid_t);
        egid = getegid();
        memcpy(ptr, &egid, sizeof(gid_t));
        cred->bytes = tmp;
        cred->size = sizeof(uid_t) + sizeof(gid_t);
        goto complete;
    } else {
        /* unrecognized protocol */
        PMIX_ERROR_LOG(PMIX_ERR_NOT_SUPPORTED);
        return PMIX_ERR_NOT_SUPPORTED;
    }

complete:
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
        PMIX_INFO_LOAD(&(*info)[0], PMIX_CRED_TYPE, "native", PMIX_STRING);
    }
    return PMIX_SUCCESS;
}

static pmix_status_t validate_cred(struct pmix_peer_t *peer, const pmix_info_t directives[],
                                   size_t ndirs, pmix_info_t **info, size_t *ninfo,
                                   const pmix_byte_object_t *cred)
{
    pmix_peer_t *pr = (pmix_peer_t *) peer;

/* Declare the peer-credential scratch only on the platforms whose
 * getsockopt path below is actually compiled in. The use site is guarded
 * on SO_PEERCRED *and* one of the ucred field macros; declaring on
 * SO_PEERCRED alone leaves these two variables unused - and therefore a
 * -Werror build failure - on a platform that has SO_PEERCRED but neither
 * field. HAVE_STRUCT_SOCKPEERCRED_UID has to be named here rather than
 * HAVE_STRUCT_UCRED_UID, because it is this block that defines the
 * latter for that platform. */
#if defined(SO_PEERCRED)                    \
    && (defined(HAVE_STRUCT_SOCKPEERCRED_UID) || defined(HAVE_STRUCT_UCRED_UID) \
        || defined(HAVE_STRUCT_UCRED_CR_UID))
#    ifdef HAVE_STRUCT_SOCKPEERCRED_UID
#        define HAVE_STRUCT_UCRED_UID
    struct sockpeercred ucred;
#    else
    struct ucred ucred;
#    endif
    socklen_t crlen = sizeof(ucred);
#endif
    uid_t euid = (uid_t) -1;
    gid_t egid = (gid_t) -1;
    char *ptr;
    size_t ln;
    uint32_t u32;

    pmix_output_verbose(2, pmix_psec_base_framework.framework_output,
                        "psec: native validate_cred %s", (NULL == cred) ? "NULL" : "NON-NULL");

    /* if we are responding to a local request to validate a credential,
     * then see if they specified a mechanism. Settle "is this even our
     * job" before we start interpreting bytes that may not be ours -
     * the other modules screen in this order too */
    if (!pmix_psec_base_check_directives("native", directives, ndirs)) {
        return PMIX_ERR_NOT_SUPPORTED;
    }

    if (PMIX_PROTOCOL_V1 == pr->protocol) {
        /* usock protocol - get the remote side's uid/gid */
#if defined(SO_PEERCRED) && (defined(HAVE_STRUCT_UCRED_UID) || defined(HAVE_STRUCT_UCRED_CR_UID))
        /* Ignore received 'cred' and validate ucred for socket instead. */
        pmix_output_verbose(2, pmix_psec_base_framework.framework_output,
                            "psec:native checking getsockopt on socket %d for peer credentials",
                            pr->sd);
        if (getsockopt(pr->sd, SOL_SOCKET, SO_PEERCRED, &ucred, &crlen) < 0) {
            pmix_output_verbose(2, pmix_psec_base_framework.framework_output,
                                "psec: getsockopt SO_PEERCRED failed: %s",
                                strerror(pmix_socket_errno));
            return PMIX_ERR_INVALID_CRED;
        }
#    if defined(HAVE_STRUCT_UCRED_UID)
        euid = ucred.uid;
        egid = ucred.gid;
#    else
        euid = ucred.cr_uid;
        egid = ucred.cr_gid;
#    endif

#elif defined(HAVE_GETPEEREID)
        pmix_output_verbose(2, pmix_psec_base_framework.framework_output,
                            "psec:native checking getpeereid on socket %d for peer credentials",
                            pr->sd);
        if (0 != getpeereid(pr->sd, &euid, &egid)) {
            pmix_output_verbose(2, pmix_psec_base_framework.framework_output,
                                "psec: getsockopt getpeereid failed: %s",
                                strerror(pmix_socket_errno));
            return PMIX_ERR_INVALID_CRED;
        }
#else
        return PMIX_ERR_NOT_SUPPORTED;
#endif
    } else if (PMIX_PROTOCOL_V2 == pr->protocol) {
        /* this is a tcp protocol, so the cred is actually the uid/gid
         * passed upwards from the client */
        if (NULL == cred) {
            /* not allowed */
            return PMIX_ERR_INVALID_CRED;
        }
        ln = cred->size;
        euid = 0;
        egid = 0;
        if (sizeof(uid_t) <= ln) {
            memcpy(&euid, cred->bytes, sizeof(uid_t));
            ln -= sizeof(uid_t);
            ptr = cred->bytes + sizeof(uid_t);
        } else {
            return PMIX_ERR_INVALID_CRED;
        }
        if (sizeof(gid_t) <= ln) {
            memcpy(&egid, ptr, sizeof(gid_t));
        } else {
            return PMIX_ERR_INVALID_CRED;
        }
    } else if (PMIX_PROTOCOL_UNDEF == pr->protocol) {
        /* we have neither a socket to interrogate nor a credential
         * format we can trust, so there is nothing here we can
         * validate. Say so explicitly rather than relying on the
         * (uid_t)-1 initializers above to fail the comparison below -
         * a peer whose recorded uid/gid happened to match those
         * sentinels would otherwise be accepted */
        return PMIX_ERR_INVALID_CRED;
    } else {
        /* don't recognize the protocol */
        return PMIX_ERR_NOT_SUPPORTED;
    }

    /* check uid */
    if (euid != pr->info->uid) {
        pmix_output_verbose(2, pmix_psec_base_framework.framework_output,
                            "psec: socket cred contains invalid uid %u - required uid %u",
                            euid, pr->info->uid);
        return PMIX_ERR_INVALID_CRED;
    }

    /* check gid */
    if (egid != pr->info->gid) {
        pmix_output_verbose(2, pmix_psec_base_framework.framework_output,
                            "psec: socket cred contains invalid gid %u - required gid %u",
                            egid, pr->info->gid);
        return PMIX_ERR_INVALID_CRED;
    }

    /* validated - mark that we did it */
    if (NULL != info) {
        PMIX_INFO_CREATE(*info, 3);
        if (NULL == *info) {
            return PMIX_ERR_NOMEM;
        }
        *ninfo = 3;
        /* mark that this came from us */
        PMIX_INFO_LOAD(&(*info)[0], PMIX_CRED_TYPE, "native", PMIX_STRING);
        /* provide the uid it contained */
        u32 = euid;
        PMIX_INFO_LOAD(&(*info)[1], PMIX_USERID, &u32, PMIX_UINT32);
        /* provide the gid it contained */
        u32 = egid;
        PMIX_INFO_LOAD(&(*info)[2], PMIX_GRPID, &u32, PMIX_UINT32);
    }
    return PMIX_SUCCESS;
}
