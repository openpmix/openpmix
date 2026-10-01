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

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef HAVE_SYS_TYPES_H
#    include <sys/types.h>
#endif

#include "pmix_common.h"

#include "src/include/pmix_globals.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_fd.h"
#include "src/util/pmix_getid.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_show_help.h"

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

    if (PMIX_PROTOCOL_V2 == pr->protocol) {
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

/* A native connection is authenticated by the owner of the peer's end of
 * the connection, which the kernel records for every socket and which we
 * look up for a peer on this host. The uid in the credential must match
 * it. The gid is not recorded by the kernel, so a tool - which names its
 * own group rather than having one registered for it by the host - must
 * also belong to the group it names.
 *
 * native serves peers on this host only. A peer the kernel does not
 * report - one on another host, or in another network namespace - is
 * refused; remote peers use a mechanism that works across hosts (ssl,
 * munge). */
static pmix_status_t check_os_identity(pmix_peer_t *pr, uid_t euid, gid_t egid)
{
    static bool warned = false;
    pmix_status_t rc;
    uid_t owner;

    rc = pmix_util_getid_tcp(pr->sd, &owner);
    if (PMIX_SUCCESS == rc) {
        if (owner != euid) {
            pmix_output_verbose(2, pmix_psec_base_framework.framework_output,
                                "psec: native credential claims uid %lu but the connection "
                                "belongs to uid %lu",
                                (unsigned long) euid, (unsigned long) owner);
            return PMIX_ERR_INVALID_CRED;
        }
        if (PMIX_PEER_IS_TOOL(pr) &&
            !(owner == geteuid() && egid == getegid()) &&
            !pmix_psec_base_gid_held(owner, egid)) {
            pmix_show_help("help-psec-native.txt", "foreign-group", true,
                           (unsigned long) owner, (unsigned long) egid);
            return PMIX_ERR_INVALID_CRED;
        }
        return PMIX_SUCCESS;
    }

    if (0 > pr->sd) {
        /* not a connection - a credential passed to
         * PMIx_Validate_credential, with no connection to check it
         * against */
        pmix_output_verbose(2, pmix_psec_base_framework.framework_output,
                            "psec: native credential for uid %lu has no "
                            "connection to check against",
                            (unsigned long) euid);
        return PMIX_ERR_INVALID_CRED;
    }

    /* say why once; a remote tool that retries would otherwise repeat it */
    if (!warned) {
        pmix_show_help("help-psec-native.txt", "unverified-peer", true,
                       pmix_fd_get_peer_name(pr->sd), (unsigned long) euid,
                       (PMIX_ERR_NOT_FOUND == rc) ? "the peer is not a process on this host"
                       : (PMIX_ERR_NOT_SUPPORTED == rc) ? "this platform offers no way to ask"
                                                        : "the connection is not a TCP connection");
        warned = true;
    }
    return PMIX_ERR_INVALID_CRED;
}

static pmix_status_t validate_cred(struct pmix_peer_t *peer, const pmix_info_t directives[],
                                   size_t ndirs, pmix_info_t **info, size_t *ninfo,
                                   const pmix_byte_object_t *cred)
{
    pmix_peer_t *pr = (pmix_peer_t *) peer;

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

    if (PMIX_PROTOCOL_V2 == pr->protocol) {
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
        /* we have no credential format we can trust, so there is
         * nothing here we can validate. Say so explicitly rather than
         * relying on the (uid_t)-1 initializers above to fail the
         * comparison below - a peer whose recorded uid/gid happened to
         * match those sentinels would otherwise be accepted */
        return PMIX_ERR_INVALID_CRED;
    } else {
        /* don't recognize the protocol */
        return PMIX_ERR_NOT_SUPPORTED;
    }

    /* confirm the identity with the kernel before comparing it with
     * anything */
    if (PMIX_SUCCESS != check_os_identity(pr, euid, egid)) {
        return PMIX_ERR_INVALID_CRED;
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
