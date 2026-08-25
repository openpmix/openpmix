/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2013 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2007      Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021-2023 Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * Recover the credentials of the process on the far end of a socket.
 */

#include "src/include/pmix_config.h"
#include "pmix_common.h"
#include "src/include/pmix_socket_errno.h"

#include <string.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif
#ifdef HAVE_SYS_TYPES_H
#    include <sys/types.h>
#endif
/* SO_PEERCRED is what the route below is selected on, so the header
 * that spells it has to be in scope here and not merely reachable
 * through some other include's includes: were it to go missing, this
 * file would quietly compile the getpeereid() route on a platform that
 * wanted the other one, rather than say anything about it */
#include <sys/socket.h>

#include "src/include/pmix_globals.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_output.h"

#include "src/util/pmix_getid.h"

/* Which shape of peer credential this platform offers is decided once,
 * here, because SO_PEERCRED on its own does not answer it: the struct
 * getsockopt fills is `struct sockpeercred` on OpenBSD and `struct
 * ucred` elsewhere, and the latter spells its members either uid/gid or
 * cr_uid/cr_gid. Only a build in which configure recognized one of those
 * three shapes can take the getsockopt route.
 *
 * Deciding it in one place is the point. The declarations used to sit
 * behind a bare SO_PEERCRED test while the code reading them sat behind
 * the full condition, so a platform that defines SO_PEERCRED but whose
 * member probe did not fire - a cross-compile, a sysroot, any configure
 * test that failed for reasons of its own - did not fall back to
 * getpeereid() at all. It failed to build: on an incomplete struct
 * ucred, and on two variables nothing went on to use. */
#if defined(SO_PEERCRED) && defined(HAVE_STRUCT_SOCKPEERCRED_UID)
#    define PMIX_HAVE_PEERCRED 1
#    define PMIX_PEERCRED_T    struct sockpeercred
#    define PMIX_PEERCRED_UID  uid
#    define PMIX_PEERCRED_GID  gid
#elif defined(SO_PEERCRED) && defined(HAVE_STRUCT_UCRED_UID)
#    define PMIX_HAVE_PEERCRED 1
#    define PMIX_PEERCRED_T    struct ucred
#    define PMIX_PEERCRED_UID  uid
#    define PMIX_PEERCRED_GID  gid
#elif defined(SO_PEERCRED) && defined(HAVE_STRUCT_UCRED_CR_UID)
#    define PMIX_HAVE_PEERCRED 1
#    define PMIX_PEERCRED_T    struct ucred
#    define PMIX_PEERCRED_UID  cr_uid
#    define PMIX_PEERCRED_GID  cr_gid
#else
#    define PMIX_HAVE_PEERCRED 0
#endif

pmix_status_t pmix_util_getid(int sd, uid_t *uid, gid_t *gid)
{
#if PMIX_HAVE_PEERCRED
    PMIX_PEERCRED_T cred;
    socklen_t crlen = sizeof(cred);

    /* Ignore any credential the peer sent us and ask the kernel about
     * the socket instead. */
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "getid: checking getsockopt for peer credentials");
    if (getsockopt(sd, SOL_SOCKET, SO_PEERCRED, &cred, &crlen) < 0) {
        pmix_output_verbose(2, pmix_globals.debug_output,
                            "getid: getsockopt SO_PEERCRED failed: %s",
                            strerror(pmix_socket_errno));
        return PMIX_ERR_INVALID_CRED;
    }
    /* getsockopt reports back how much it actually wrote. Anything short
     * of the whole struct leaves the rest of it holding whatever was on
     * the stack, and these two fields are what a caller decides an
     * identity on - so refuse the answer rather than hand back a uid
     * that was never written. */
    if (crlen < sizeof(cred)) {
        pmix_output_verbose(2, pmix_globals.debug_output,
                            "getid: getsockopt SO_PEERCRED returned %u bytes, expected %u",
                            (unsigned) crlen, (unsigned) sizeof(cred));
        return PMIX_ERR_INVALID_CRED;
    }
    *uid = cred.PMIX_PEERCRED_UID;
    *gid = cred.PMIX_PEERCRED_GID;

#elif defined(HAVE_GETPEEREID)
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "getid: checking getpeereid for peer credentials");
    if (0 != getpeereid(sd, uid, gid)) {
        pmix_output_verbose(2, pmix_globals.debug_output, "getid: getpeereid failed: %s",
                            strerror(pmix_socket_errno));
        return PMIX_ERR_INVALID_CRED;
    }
#else
    PMIX_HIDE_UNUSED_PARAMS(sd, uid, gid);
    return PMIX_ERR_NOT_SUPPORTED;
#endif

    return PMIX_SUCCESS;
}
