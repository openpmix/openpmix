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

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifdef HAVE_SYS_TYPES_H
#    include <sys/types.h>
#endif
#include <sys/stat.h>

#include "pmix_common.h"

#include "src/include/pmix_globals.h"
#include "src/include/pmix_socket_errno.h"
#include "src/mca/ptl/base/base.h"
#include "src/server/pmix_server_ops.h"
#include "src/util/pmix_environ.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_fd.h"
#include "src/util/pmix_getid.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_printf.h"
#include "src/util/pmix_show_help.h"

#include "psec_native.h"
#include "src/mca/psec/base/base.h"

static pmix_status_t native_init(void);
static void native_finalize(void);
static pmix_status_t create_cred(struct pmix_peer_t *peer, const pmix_info_t directives[],
                                 size_t ndirs, pmix_info_t **info, size_t *ninfo,
                                 pmix_byte_object_t *cred);
/* whether the far end of a connected socket is at an address of this host */
static bool peer_on_host(int sd)
{
    struct sockaddr_storage sa;
    pmix_socklen_t len = sizeof(sa);

    memset(&sa, 0, sizeof(sa));
    if (0 != getpeername(sd, (struct sockaddr *) &sa, &len)) {
        return false;
    }
    return pmix_ptl_base_addr_on_host(&sa);
}

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

/* The credential is the peer's effective uid and gid - what a server
 * compares with what the kernel says of the connection where it can
 * learn that, and takes on trust from an older peer where it cannot */
static pmix_status_t create_cred(struct pmix_peer_t *peer, const pmix_info_t directives[],
                                 size_t ndirs, pmix_info_t **info, size_t *ninfo,
                                 pmix_byte_object_t *cred)
{
    pmix_peer_t *pr = (pmix_peer_t *) peer;
    uid_t euid;
    gid_t egid;
    char *tmp;
    size_t len;

    /* ensure initialization */
    PMIX_BYTE_OBJECT_CONSTRUCT(cred);

    /* we may be responding to a local request for a credential, so
     * see if they specified a mechanism */
    if (!pmix_psec_base_check_directives("native", directives, ndirs)) {
        PMIX_ERROR_LOG(PMIX_ERR_NOT_SUPPORTED);
        return PMIX_ERR_NOT_SUPPORTED;
    }

    if (PMIX_PROTOCOL_V2 != pr->protocol) {
        /* unrecognized protocol */
        PMIX_ERROR_LOG(PMIX_ERR_NOT_SUPPORTED);
        return PMIX_ERR_NOT_SUPPORTED;
    }
    len = sizeof(uid_t) + sizeof(gid_t);
    tmp = (char *) malloc(len);
    if (NULL == tmp) {
        return PMIX_ERR_NOMEM;
    }
    euid = geteuid();
    memcpy(tmp, &euid, sizeof(uid_t));
    egid = getegid();
    memcpy(tmp + sizeof(uid_t), &egid, sizeof(gid_t));
    cred->bytes = tmp;
    cred->size = len;

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

/* Decide the peer's identity, given the uid the kernel attests for it
 * and its group - attested too when `gid_attested`, else the group it
 * claimed.
 *
 * - An identity the host registered (a client) is what the peer must
 *   be: the attested uid, and the group, must match it.
 * - Any other identity (a tool, or a process that connected before the
 *   host registered it) is only what the peer claimed. The attested
 *   uid replaces it - the claim may be the uid inside a user namespace,
 *   which is not the uid outside it. The claimed group stands if it is
 *   the attested one or a group the user holds; otherwise an attested
 *   group replaces it, and a group nobody attested is refused.
 *
 * On success the peer's info carries the identity decided on. A refusal
 * for the group alone - the uid was right - is reported in *group_only,
 * since an attested group may yet settle it. */
static pmix_status_t settle_identity(pmix_peer_t *pr, uid_t uid, gid_t gid, bool gid_attested,
                                     bool *group_only)
{
    gid_t claimed = pr->info->gid;

    *group_only = false;

    if (pr->info->host_registered) {
        if (uid != pr->info->uid) {
            pmix_output_verbose(2, pmix_psec_base_framework.framework_output,
                                "psec: native peer is uid %lu - required uid %lu",
                                (unsigned long) uid, (unsigned long) pr->info->uid);
            return PMIX_ERR_INVALID_CRED;
        }
        if (gid != pr->info->gid) {
            pmix_output_verbose(2, pmix_psec_base_framework.framework_output,
                                "psec: native peer is gid %lu - required gid %lu",
                                (unsigned long) gid, (unsigned long) pr->info->gid);
            *group_only = true;
            return PMIX_ERR_INVALID_CRED;
        }
        return PMIX_SUCCESS;
    }

    if (gid_attested) {
        if (claimed != gid && !pmix_psec_base_gid_held(uid, claimed)) {
            claimed = gid;
        }
    } else if (!(uid == geteuid() && claimed == getegid()) &&
               !pmix_psec_base_gid_held(uid, claimed)) {
        *group_only = true;
        return PMIX_ERR_INVALID_CRED;
    }
    pr->info->uid = uid;
    pr->info->gid = claimed;
    return PMIX_SUCCESS;
}

static pmix_status_t validate_cred(struct pmix_peer_t *peer, const pmix_info_t directives[],
                                   size_t ndirs, pmix_info_t **info, size_t *ninfo,
                                   const pmix_byte_object_t *cred)
{
    static bool warned_unowned = false;
    pmix_peer_t *pr = (pmix_peer_t *) peer;
    uid_t euid = (uid_t) -1;
    gid_t egid = (gid_t) -1;
    uid_t owner = (uid_t) -1;
    pmix_status_t rc;
    uint32_t u32;
    bool group_only;

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
        /* the uid and gid; anything after them is ignored, as every
         * release has ignored it */
        if (sizeof(uid_t) + sizeof(gid_t) > cred->size) {
            return PMIX_ERR_INVALID_CRED;
        }
        memcpy(&euid, cred->bytes, sizeof(uid_t));
        memcpy(&egid, cred->bytes + sizeof(uid_t), sizeof(gid_t));
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

    /* On a local transport the kernel says who is at the far end - its
     * uid, and the gid it runs as - whatever the peer claimed */
    if (NULL != pr->transport && NULL != pr->transport->peer_identity) {
        uid_t kuid;
        gid_t kgid;
        pid_t kpid;

        if (PMIX_SUCCESS != pr->transport->peer_identity(pr->sd, &kuid, &kgid, &kpid)) {
            pmix_output_verbose(2, pmix_psec_base_framework.framework_output,
                                "psec: native cannot learn who is at the far end of a "
                                "local connection");
            return PMIX_ERR_INVALID_CRED;
        }
        rc = settle_identity(pr, kuid, kgid, true, &group_only);
        if (PMIX_SUCCESS != rc) {
            return rc;
        }
        goto validated;
    }

    /* ask the kernel who owns the peer's end of the connection */
    rc = pmix_util_getid_tcp(pr->sd, &owner);
    if (PMIX_SUCCESS == rc) {
        /* the kernel records no group for a TCP socket, so the group is
         * the one the peer claims. Inside a user namespace the claim is
         * the gid there */
        rc = settle_identity(pr, owner, egid, false, &group_only);
        if (PMIX_SUCCESS != rc) {
            if (group_only && !pr->info->host_registered) {
                pmix_show_help("help-psec-native.txt", "foreign-group", true,
                               (unsigned long) owner, (unsigned long) pr->info->gid);
            }
            return rc;
        }

    } else if (PMIX_ERR_NOT_FOUND == rc && peer_on_host(pr->sd)) {
        /* A peer at an address of this host has its end in that table, as
         * an established connection, once it has sent its request - so
         * the kernel found no owner because there is none to find: the
         * claim stands for nothing */
        if (!warned_unowned) {
            pmix_show_help("help-psec-native.txt", "unowned-peer", true,
                           pmix_fd_get_peer_name(pr->sd), (unsigned long) euid);
            warned_unowned = true;
        }
        return PMIX_ERR_INVALID_CRED;

    } else if (PMIX_ERR_NOT_SUPPORTED != rc) {
        /* the table could not be read - this process is out of
         * descriptors or memory, say. That is no answer, and no reason to
         * take the peer at its word */
        pmix_output_verbose(2, pmix_psec_base_framework.framework_output,
                            "psec: native could not ask the kernel who the peer at %s is: %s",
                            pmix_fd_get_peer_name(pr->sd), PMIx_Error_string(rc));
        return PMIX_ERR_INVALID_CRED;

    } else if (pmix_ptl_base.legacy_peers) {
        /* nothing the kernel can tell us - take the peer at its word, as
         * every earlier release did; a peer of an earlier release has no
         * other way to say who it is. A server takes a TCP connection only
         * when it takes legacy peers */
        if (euid != pr->info->uid) {
            pmix_output_verbose(2, pmix_psec_base_framework.framework_output,
                                "psec: socket cred contains invalid uid %u - required uid %u",
                                euid, pr->info->uid);
            return PMIX_ERR_INVALID_CRED;
        }
        if (egid != pr->info->gid) {
            pmix_output_verbose(2, pmix_psec_base_framework.framework_output,
                                "psec: socket cred contains invalid gid %u - required gid %u",
                                egid, pr->info->gid);
            return PMIX_ERR_INVALID_CRED;
        }
        pmix_output_verbose(2, pmix_psec_base_framework.framework_output,
                            "psec: native accepting the claimed identity of an older peer "
                            "at %s", pmix_fd_get_peer_name(pr->sd));

    } else {
        pmix_output_verbose(2, pmix_psec_base_framework.framework_output,
                            "psec: native refusing a TCP peer at %s - this server "
                            "takes no legacy peers", pmix_fd_get_peer_name(pr->sd));
        return PMIX_ERR_INVALID_CRED;
    }

validated:
    /* validated - mark that we did it */
    if (NULL != info) {
        PMIX_INFO_CREATE(*info, 3);
        if (NULL == *info) {
            return PMIX_ERR_NOMEM;
        }
        *ninfo = 3;
        /* mark that this came from us */
        PMIX_INFO_LOAD(&(*info)[0], PMIX_CRED_TYPE, "native", PMIX_STRING);
        /* provide the identity it was validated as */
        u32 = pr->info->uid;
        PMIX_INFO_LOAD(&(*info)[1], PMIX_USERID, &u32, PMIX_UINT32);
        u32 = pr->info->gid;
        PMIX_INFO_LOAD(&(*info)[2], PMIX_GRPID, &u32, PMIX_UINT32);
    }
    return PMIX_SUCCESS;
}
