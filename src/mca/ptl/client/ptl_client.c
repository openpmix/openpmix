/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2010-2011 Oak Ridge National Labs.  All rights reserved.
 * Copyright (c) 2011-2014 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2011-2013 Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2018      IBM Corporation.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#include "src/include/pmix_config.h"
#include "src/include/pmix_globals.h"

#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif
#ifdef HAVE_SYS_UIO_H
#include <sys/uio.h>
#endif
#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif
#ifdef HAVE_DIRENT_H
#include <dirent.h>
#endif
#ifdef HAVE_SYS_SYSCTL_H
#include <sys/sysctl.h>
#endif

#include "src/include/pmix_socket_errno.h"
#include "src/client/pmix_client_ops.h"
#include "src/server/pmix_server_ops.h"
#include "src/util/argv.h"
#include "src/util/error.h"
#include "src/util/os_path.h"
#include "src/util/show_help.h"
#include "src/mca/bfrops/base/base.h"
#include "src/mca/gds/gds.h"

#include "src/mca/ptl/base/base.h"
#include "ptl_client.h"

static pmix_status_t connect_to_peer(struct pmix_peer_t *peer,
                                     pmix_info_t *info, size_t ninfo);

pmix_ptl_module_t pmix_ptl_client_module = {
    .name = "client",
    .connect_to_peer = connect_to_peer
};

static pmix_status_t connect_to_peer(struct pmix_peer_t *pr,
                                     pmix_info_t *info, size_t ninfo)
{
    char *evar, *vrs, *suri = NULL;
    char *nspace=NULL;
    pmix_rank_t rank = PMIX_RANK_WILDCARD;
    char **server_nspace = NULL, *rendfile = NULL;
    pmix_status_t rc;
    pmix_peer_t *peer = (pmix_peer_t*)pr;

    pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                        "ptl:tcp: connecting to server");

    vrs = getenv("PMIX_VERSION");

    if (NULL != (evar = getenv("PMIX_SERVER_URI4"))) {
        /* we are talking to a v4 server */
        PMIX_SET_PEER_TYPE(peer, PMIX_PROC_SERVER);
        PMIX_SET_PEER_VERSION(peer, vrs, 4, 0);

        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "V4 SERVER DETECTED");

        /* must use the latest bfrops module */
        PMIX_BFROPS_SET_MODULE(rc, pmix_globals.mypeer, peer, NULL);
        if (PMIX_SUCCESS != rc) {
            return rc;
        }

    } else if (NULL != (evar = getenv("PMIX_SERVER_URI3"))) {
        /* we are talking to a v3 server */
        PMIX_SET_PEER_TYPE(peer, PMIX_PROC_SERVER);
        PMIX_SET_PEER_VERSION(peer, vrs, 3, 0);

        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "V3 SERVER DETECTED");

        /* must use the v3 bfrops module */
        PMIX_BFROPS_SET_MODULE(rc, pmix_globals.mypeer, peer, "v3");
        if (PMIX_SUCCESS != rc) {
            return rc;
        }

    } else if (NULL != (evar = getenv("PMIX_SERVER_URI21"))) {
        /* we are talking to a v2.1 server */
        PMIX_SET_PEER_TYPE(peer, PMIX_PROC_SERVER);
        PMIX_SET_PEER_VERSION(peer, vrs, 2, 1);

        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "V21 SERVER DETECTED");

        /* must use the v21 bfrops module */
        PMIX_BFROPS_SET_MODULE(rc, pmix_globals.mypeer, peer, "v21");
        if (PMIX_SUCCESS != rc) {
            return rc;
        }

    } else if (NULL != (evar = getenv("PMIX_SERVER_URI2"))) {
        /* we are talking to a v2.0 server */
        PMIX_SET_PEER_TYPE(peer, PMIX_PROC_SERVER);
        PMIX_SET_PEER_VERSION(peer, vrs, 2, 0);

        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "V20 SERVER DETECTED");

        /* must use the v20 bfrops module */
        PMIX_BFROPS_SET_MODULE(rc, pmix_globals.mypeer, peer, "v20");
        if (PMIX_SUCCESS != rc) {
            return rc;
        }

    } else {
        return PMIX_ERR_UNREACH;
    }

    /* the URI consists of the following elements:
    *    - server nspace.rank
    *    - ptl rendezvous URI
    */
    rc = pmix_ptl_base_parse_uri(evar, &nspace, &rank, &suri);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }

    pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                        "ptl:tcp:client attempt connect to %s:%u at %s", nspace, rank, suri);

    rc = pmix_ptl_base_make_connection(peer, suri, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        free(nspace);
        free(suri);
        return rc;
    }

    pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                        "tcp_peer_try_connect: Connection across to peer %s:%u succeeded",
                        nspace, rank);

    /* mark the connection as made */
    pmix_ptl_base_complete_connection(peer, nspace, rank, suri);

    if (NULL != nspace) {
        free(nspace);
    }
    if (NULL != rendfile) {
        free(rendfile);
    }
    if (NULL != suri) {
        free(suri);
    }
    if (NULL != server_nspace) {
        free(server_nspace);
    }
    return rc;
}
