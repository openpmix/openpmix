/* -*- Mode: C; c-basic-offset:4 ; -*- */
/*
 * Copyright (c) 2004-2007 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2009 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2012-2013 Los Alamos National Security, Inc.  All rights reserved.
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015-2020 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2021      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
/** @file:
 *
 */
#include "src/include/pmix_config.h"

#include "include/pmix_common.h"

#ifdef HAVE_STRING_H
#include <string.h>
#endif
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif
#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif

#include "src/mca/mca.h"
#include "src/mca/base/base.h"
#include "src/mca/base/pmix_mca_base_var.h"
#include "src/mca/base/pmix_mca_base_framework.h"
#include "src/class/pmix_list.h"
#include "src/client/pmix_client_ops.h"
#include "src/server/pmix_server_ops.h"
#include "src/util/error.h"
#include "src/util/os_dirpath.h"
#include "src/util/pmix_environ.h"
#include "src/util/show_help.h"

#include "src/mca/ptl/ptl_types.h"
#include "src/mca/ptl/base/base.h"

/*
 * The following file was created by configure.  It contains extern
 * statements and the definition of an array of pointers to each
 * component's public mca_base_component_t struct.
 */

#include "src/mca/ptl/base/static-components.h"

#define PMIX_MAX_MSG_SIZE   16

/* Instantiate the global vars */
pmix_ptl_base_t pmix_ptl_base = {0};
int pmix_ptl_base_output = -1;
pmix_ptl_module_t pmix_ptl = {0};

static size_t max_msg_size = PMIX_MAX_MSG_SIZE;

static int pmix_ptl_register(pmix_mca_base_register_flag_t flags)
{
    int idx;

    (void)flags;
    pmix_mca_base_var_register("pmix", "ptl", "base", "max_msg_size",
                               "Max size (in Mbytes) of a client/server msg",
                               PMIX_MCA_BASE_VAR_TYPE_SIZE_T, NULL, 0,
                               PMIX_MCA_BASE_VAR_FLAG_NONE,
                               PMIX_INFO_LVL_2,
                               PMIX_MCA_BASE_VAR_SCOPE_READONLY,
                               &max_msg_size);
    pmix_ptl_base.max_msg_size = max_msg_size * 1024 * 1024;

    idx = pmix_mca_base_var_register("pmix", "ptl", "base", "if_include",
                                     "Comma-delimited list of devices and/or CIDR notation of TCP networks "
                                     "(e.g., \"eth0,192.168.0.0/16\").  Mutually exclusive with ptl_tcp_if_exclude.",
                                     PMIX_MCA_BASE_VAR_TYPE_STRING, NULL, 0,
                                     PMIX_MCA_BASE_VAR_FLAG_NONE,
                                     PMIX_INFO_LVL_2,
                                     PMIX_MCA_BASE_VAR_SCOPE_LOCAL,
                                     &pmix_ptl_base.if_include);
    (void)pmix_mca_base_var_register_synonym(idx, "pmix", "ptl", "tcp", "if_include",
                                             PMIX_MCA_BASE_VAR_SYN_FLAG_DEPRECATED);

    idx = pmix_mca_base_var_register("pmix", "ptl", "base", "if_exclude",
                                     "Comma-delimited list of devices and/or CIDR notation of TCP networks to NOT use "
                                     "-- all devices not matching these specifications will be used (e.g., \"eth0,192.168.0.0/16\"). "
                                     "If set to a non-default value, it is mutually exclusive with ptl_tcp_if_include.",
                                     PMIX_MCA_BASE_VAR_TYPE_STRING, NULL, 0,
                                     PMIX_MCA_BASE_VAR_FLAG_NONE,
                                     PMIX_INFO_LVL_2,
                                     PMIX_MCA_BASE_VAR_SCOPE_LOCAL,
                                     &pmix_ptl_base.if_exclude);
    (void)pmix_mca_base_var_register_synonym(idx, "pmix", "ptl", "tcp", "if_exclude",
                                             PMIX_MCA_BASE_VAR_SYN_FLAG_DEPRECATED);

    /* if_include and if_exclude need to be mutually exclusive */
    if (NULL != pmix_ptl_base.if_include &&
        NULL != pmix_ptl_base.if_exclude) {
        pmix_show_help("help-ptl-base.txt", "include-exclude", true,
                       pmix_ptl_base.if_include,
                       pmix_ptl_base.if_exclude);
        return PMIX_ERR_NOT_AVAILABLE;
    }

    idx = pmix_mca_base_var_register("pmix", "ptl", "base", "ipv4_port",
                                     "IPv4 port to be used",
                                     PMIX_MCA_BASE_VAR_TYPE_INT, NULL, 0,
                                     PMIX_MCA_BASE_VAR_FLAG_NONE,
                                     PMIX_INFO_LVL_4,
                                     PMIX_MCA_BASE_VAR_SCOPE_READONLY,
                                     &pmix_ptl_base.ipv4_port);
    (void)pmix_mca_base_var_register_synonym(idx, "pmix", "ptl", "tcp", "ipv4_port",
                                             PMIX_MCA_BASE_VAR_SYN_FLAG_DEPRECATED);

    idx = pmix_mca_base_var_register("pmix", "ptl", "base", "ipv6_port",
                                     "IPv6 port to be used",
                                     PMIX_MCA_BASE_VAR_TYPE_INT, NULL, 0,
                                     PMIX_MCA_BASE_VAR_FLAG_NONE,
                                     PMIX_INFO_LVL_4,
                                     PMIX_MCA_BASE_VAR_SCOPE_READONLY,
                                     &pmix_ptl_base.ipv6_port);
    (void)pmix_mca_base_var_register_synonym(idx, "pmix", "ptl", "tcp", "ipv6_port",
                                             PMIX_MCA_BASE_VAR_SYN_FLAG_DEPRECATED);

    idx = pmix_mca_base_var_register("pmix", "ptl", "base", "disable_ipv4_family",
                                     "Disable the IPv4 interfaces",
                                     PMIX_MCA_BASE_VAR_TYPE_BOOL, NULL, 0,
                                     PMIX_MCA_BASE_VAR_FLAG_NONE,
                                     PMIX_INFO_LVL_4,
                                     PMIX_MCA_BASE_VAR_SCOPE_READONLY,
                                     &pmix_ptl_base.disable_ipv4_family);
    (void)pmix_mca_base_var_register_synonym(idx, "pmix", "ptl", "tcp", "disable_ipv4_family",
                                             PMIX_MCA_BASE_VAR_SYN_FLAG_DEPRECATED);

    pmix_ptl_base.disable_ipv6_family = true;
    idx = pmix_mca_base_var_register("pmix", "ptl", "base", "disable_ipv6_family",
                                     "Disable the IPv6 interfaces (default:disabled)",
                                     PMIX_MCA_BASE_VAR_TYPE_BOOL, NULL, 0,
                                     PMIX_MCA_BASE_VAR_FLAG_NONE,
                                     PMIX_INFO_LVL_4,
                                     PMIX_MCA_BASE_VAR_SCOPE_READONLY,
                                     &pmix_ptl_base.disable_ipv6_family);
    (void)pmix_mca_base_var_register_synonym(idx, "pmix", "ptl", "tcp", "disable_ipv6_family",
                                             PMIX_MCA_BASE_VAR_SYN_FLAG_DEPRECATED);

    idx = pmix_mca_base_var_register("pmix", "ptl", "base", "connection_wait_time",
                                     "Number of seconds to wait for the server connection file to appear",
                                     PMIX_MCA_BASE_VAR_TYPE_INT, NULL, 0,
                                     PMIX_MCA_BASE_VAR_FLAG_NONE,
                                     PMIX_INFO_LVL_4,
                                     PMIX_MCA_BASE_VAR_SCOPE_READONLY,
                                     &pmix_ptl_base.wait_to_connect);
    (void)pmix_mca_base_var_register_synonym(idx, "pmix", "ptl", "tcp", "connection_wait_time",
                                             PMIX_MCA_BASE_VAR_SYN_FLAG_DEPRECATED);

    idx = pmix_mca_base_var_register("pmix", "ptl", "base", "max_retries",
                                     "Number of times to look for the connection file before quitting",
                                     PMIX_MCA_BASE_VAR_TYPE_INT, NULL, 0,
                                     PMIX_MCA_BASE_VAR_FLAG_NONE,
                                     PMIX_INFO_LVL_4,
                                     PMIX_MCA_BASE_VAR_SCOPE_READONLY,
                                     &pmix_ptl_base.max_retries);
    (void)pmix_mca_base_var_register_synonym(idx, "pmix", "ptl", "tcp", "max_retries",
                                             PMIX_MCA_BASE_VAR_SYN_FLAG_DEPRECATED);

    idx = pmix_mca_base_var_register("pmix", "ptl", "base", "handshake_wait_time",
                                     "Number of seconds to wait for the server reply to the handshake request",
                                     PMIX_MCA_BASE_VAR_TYPE_INT, NULL, 0,
                                     PMIX_MCA_BASE_VAR_FLAG_NONE,
                                     PMIX_INFO_LVL_4,
                                     PMIX_MCA_BASE_VAR_SCOPE_READONLY,
                                     &pmix_ptl_base.handshake_wait_time);
    (void)pmix_mca_base_var_register_synonym(idx, "pmix", "ptl", "tcp", "handshake_wait_time",
                                             PMIX_MCA_BASE_VAR_SYN_FLAG_DEPRECATED);

    idx = pmix_mca_base_var_register("pmix", "ptl", "base", "handshake_max_retries",
                                     "Number of times to retry the handshake request before giving up",
                                     PMIX_MCA_BASE_VAR_TYPE_INT, NULL, 0,
                                     PMIX_MCA_BASE_VAR_FLAG_NONE,
                                     PMIX_INFO_LVL_4,
                                     PMIX_MCA_BASE_VAR_SCOPE_READONLY,
                                     &pmix_ptl_base.handshake_max_retries);
    (void)pmix_mca_base_var_register_synonym(idx, "pmix", "ptl", "tcp", "handshake_max_retries",
                                             PMIX_MCA_BASE_VAR_SYN_FLAG_DEPRECATED);

    idx = pmix_mca_base_var_register("pmix", "ptl", "base", "report_uri",
                                     "Output URI [- => stdout, + => stderr, or filename]",
                                     PMIX_MCA_BASE_VAR_TYPE_STRING, NULL, 0,
                                     PMIX_MCA_BASE_VAR_FLAG_NONE,
                                     PMIX_INFO_LVL_2,
                                     PMIX_MCA_BASE_VAR_SCOPE_LOCAL,
                                     &pmix_ptl_base.report_uri);
    (void)pmix_mca_base_var_register_synonym(idx, "pmix", "ptl", "tcp", "report_uri",
                                             PMIX_MCA_BASE_VAR_SYN_FLAG_DEPRECATED);


    return PMIX_SUCCESS;
}

static pmix_status_t pmix_ptl_close(void)
{
    if (!pmix_ptl_base.initialized) {
        return PMIX_SUCCESS;
    }
    pmix_ptl_base.initialized = false;
    pmix_ptl_base.selected = false;

    /* ensure the listen thread has been shut down */
    pmix_ptl_base_stop_listening();

    if (NULL != pmix_client_globals.myserver) {
        if (0 <= pmix_client_globals.myserver->sd) {
            CLOSE_THE_SOCKET(pmix_client_globals.myserver->sd);
            pmix_client_globals.myserver->sd = -1;
        }
    }

    /* the component will cleanup when closed */
    PMIX_LIST_DESTRUCT(&pmix_ptl_base.posted_recvs);
    PMIX_LIST_DESTRUCT(&pmix_ptl_base.unexpected_msgs);
    PMIX_DESTRUCT(&pmix_ptl_base.listener);

    if (NULL != pmix_ptl_base.system_filename) {
        if (pmix_ptl_base.created_system_filename) {
            remove(pmix_ptl_base.system_filename);
        }
        free(pmix_ptl_base.system_filename);
    }
    if (NULL != pmix_ptl_base.session_filename) {
        if (pmix_ptl_base.created_session_filename) {
            remove(pmix_ptl_base.session_filename);
        }
        free(pmix_ptl_base.session_filename);
    }
    if (NULL != pmix_ptl_base.nspace_filename) {
        if (pmix_ptl_base.created_nspace_filename) {
            remove(pmix_ptl_base.nspace_filename);
        }
        free(pmix_ptl_base.nspace_filename);
    }
    if (NULL != pmix_ptl_base.pid_filename) {
        if (pmix_ptl_base.created_pid_filename) {
            remove(pmix_ptl_base.pid_filename);
        }
        free(pmix_ptl_base.pid_filename);
    }
    if (NULL != pmix_ptl_base.rendezvous_filename) {
        if (pmix_ptl_base.created_rendezvous_file) {
            remove(pmix_ptl_base.rendezvous_filename);
        }
        free(pmix_ptl_base.rendezvous_filename);
    }
    if (NULL != pmix_ptl_base.uri) {
        free(pmix_ptl_base.uri);
    }
    if (NULL != pmix_ptl_base.urifile) {
        if (pmix_ptl_base.created_urifile) {
            /* remove the file */
            remove(pmix_ptl_base.urifile);
        }
        free(pmix_ptl_base.urifile);
        pmix_ptl_base.urifile = NULL;
    }
    if (NULL != pmix_ptl_base.session_tmpdir) {
        /* if I created the session tmpdir, then remove it if empty */
        if (pmix_ptl_base.created_session_tmpdir) {
            pmix_os_dirpath_destroy(pmix_ptl_base.session_tmpdir,
                                    true, NULL);
        }
        free(pmix_ptl_base.session_tmpdir);
    }
    if (NULL != pmix_ptl_base.system_tmpdir) {
        if (pmix_ptl_base.created_system_tmpdir) {
            pmix_os_dirpath_destroy(pmix_ptl_base.system_tmpdir,
                                    true, NULL);
        }
        free(pmix_ptl_base.system_tmpdir);
    }

    return pmix_mca_base_framework_components_close(&pmix_ptl_base_framework, NULL);
}

static pmix_status_t pmix_ptl_open(pmix_mca_base_open_flag_t flags)
{
    pmix_status_t rc;
    char *tdir;

    /* initialize globals */
    pmix_ptl_base.initialized = true;
    PMIX_CONSTRUCT(&pmix_ptl_base.posted_recvs, pmix_list_t);
    PMIX_CONSTRUCT(&pmix_ptl_base.unexpected_msgs, pmix_list_t);
    pmix_ptl_base.listen_thread_active = false;
    PMIX_CONSTRUCT(&pmix_ptl_base.listener, pmix_listener_t);
    pmix_ptl_base.current_tag = PMIX_PTL_TAG_DYNAMIC;

    /* check for environ-based directives
     * on system tmpdir to use */
    if (PMIX_PEER_IS_SERVER(pmix_globals.mypeer) ||
        PMIX_PEER_IS_LAUNCHER(pmix_globals.mypeer)) {
        pmix_ptl_base.session_tmpdir = strdup(pmix_server_globals.tmpdir);
    } else {
        if (NULL != (tdir = getenv("PMIX_SERVER_TMPDIR"))) {
            pmix_ptl_base.session_tmpdir = strdup(tdir);
        } else {
            pmix_ptl_base.session_tmpdir = strdup(pmix_tmp_directory());
        }
    }

    if (PMIX_PEER_IS_SERVER(pmix_globals.mypeer) ||
        PMIX_PEER_IS_LAUNCHER(pmix_globals.mypeer)) {
        pmix_ptl_base.system_tmpdir = strdup(pmix_server_globals.system_tmpdir);
    } else {
        if (NULL != (tdir = getenv("PMIX_SYSTEM_TMPDIR"))) {
            pmix_ptl_base.system_tmpdir = strdup(tdir);
        } else {
            pmix_ptl_base.system_tmpdir = strdup(pmix_tmp_directory());
        }
    }

    if (NULL != pmix_ptl_base.report_uri &&
        0 != strcmp(pmix_ptl_base.report_uri, "-") &&
        0 != strcmp(pmix_ptl_base.report_uri, "+")) {
        pmix_ptl_base.urifile = strdup(pmix_ptl_base.report_uri);
    }

    if (PMIX_PEER_IS_SERVER(pmix_globals.mypeer) ||
        PMIX_PEER_IS_LAUNCHER(pmix_globals.mypeer)) {
        if (NULL != (tdir = getenv("PMIX_LAUNCHER_RENDEZVOUS_FILE"))) {
            pmix_ptl_base.rendezvous_filename = strdup(tdir);
        }
    }

    /* Open up all available components */
    rc = pmix_mca_base_framework_components_open(&pmix_ptl_base_framework, flags);
    pmix_ptl_base_output = pmix_ptl_base_framework.framework_output;
    return rc;
}

PMIX_MCA_BASE_FRAMEWORK_DECLARE(pmix, ptl, "PMIx Transfer Layer",
                                pmix_ptl_register, pmix_ptl_open, pmix_ptl_close,
                                mca_ptl_base_static_components, PMIX_MCA_BASE_FRAMEWORK_FLAG_DEFAULT);

/***   INSTANTIATE INTERNAL CLASSES   ***/
static void scon(pmix_ptl_send_t *p)
{
    memset(&p->hdr, 0, sizeof(pmix_ptl_hdr_t));
    p->hdr.tag = UINT32_MAX;
    p->hdr.nbytes = 0;
    p->data = NULL;
    p->hdr_sent = false;
    p->sdptr = NULL;
    p->sdbytes = 0;
}
static void sdes(pmix_ptl_send_t *p)
{
    if (NULL != p->data) {
        PMIX_RELEASE(p->data);
    }
}
PMIX_EXPORT PMIX_CLASS_INSTANCE(pmix_ptl_send_t,
                                pmix_list_item_t,
                                scon, sdes);

static void rcon(pmix_ptl_recv_t *p)
{
    p->peer = NULL;
    memset(&p->hdr, 0, sizeof(pmix_ptl_hdr_t));
    p->hdr.tag = UINT32_MAX;
    p->hdr.nbytes = 0;
    p->data = NULL;
    p->hdr_recvd = false;
    p->rdptr = NULL;
    p->rdbytes = 0;
}
static void rdes(pmix_ptl_recv_t *p)
{
    if (NULL != p->peer) {
        PMIX_RELEASE(p->peer);
    }
}
PMIX_EXPORT PMIX_CLASS_INSTANCE(pmix_ptl_recv_t,
                                pmix_list_item_t,
                                rcon, rdes);

static void prcon(pmix_ptl_posted_recv_t *p)
{
    p->tag = UINT32_MAX;
    p->cbfunc = NULL;
    p->cbdata = NULL;
}
PMIX_EXPORT PMIX_CLASS_INSTANCE(pmix_ptl_posted_recv_t,
                                pmix_list_item_t,
                                prcon, NULL);


static void srcon(pmix_ptl_sr_t *p)
{
    p->peer = NULL;
    p->bfr = NULL;
    p->cbfunc = NULL;
    p->cbdata = NULL;
}
static void srdes(pmix_ptl_sr_t *p)
{
    if (NULL != p->peer) {
        PMIX_RELEASE(p->peer);
    }
}
PMIX_EXPORT PMIX_CLASS_INSTANCE(pmix_ptl_sr_t,
                                pmix_object_t,
                                srcon, srdes);

static void pccon(pmix_pending_connection_t *p)
{
    p->need_id = false;
    PMIX_LOAD_PROCID(&p->proc, NULL, PMIX_RANK_UNDEF);
    p->info = NULL;
    p->ninfo = 0;
    p->peer = NULL;
    p->version = NULL;
    p->bfrops = NULL;
    p->psec = NULL;
    p->gds = NULL;
    p->cred = NULL;
    p->proc_type.type = PMIX_PROC_UNDEF;
    p->proc_type.major = PMIX_MAJOR_WILDCARD;
    p->proc_type.minor = PMIX_MINOR_WILDCARD;
    p->proc_type.release = PMIX_RELEASE_WILDCARD;
    p->proc_type.flag = 0;
}
static void pcdes(pmix_pending_connection_t *p)
{
    if (NULL != p->info) {
        PMIX_INFO_FREE(p->info, p->ninfo);
    }
    if (NULL != p->version) {
        free(p->version);
    }
    if (NULL != p->bfrops) {
        free(p->bfrops);
    }
    if (NULL != p->psec) {
        free(p->psec);
    }
    if (NULL != p->gds) {
        free(p->gds);
    }
    if (NULL != p->cred) {
        free(p->cred);
    }
}
PMIX_EXPORT PMIX_CLASS_INSTANCE(pmix_pending_connection_t,
                                pmix_object_t,
                                pccon, pcdes);

static void lcon(pmix_listener_t *p)
{
    p->socket = -1;
    p->varname = NULL;
    p->uri = NULL;
    p->owner_given = false;
    p->group_given = false;
    p->mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH;
}
static void ldes(pmix_listener_t *p)
{
    if (0 <= p->socket) {
        CLOSE_THE_SOCKET(p->socket);
    }
    if (NULL != p->varname) {
        free(p->varname);
    }
    if (NULL != p->uri) {
        free(p->uri);
    }
}
PMIX_EXPORT PMIX_CLASS_INSTANCE(pmix_listener_t,
                                pmix_list_item_t,
                                lcon, ldes);

static void qcon(pmix_ptl_queue_t *p)
{
    p->peer = NULL;
    p->buf = NULL;
    p->tag = UINT32_MAX;
}
static void qdes(pmix_ptl_queue_t *p)
{
    if (NULL != p->peer) {
        PMIX_RELEASE(p->peer);
    }
}
PMIX_EXPORT PMIX_CLASS_INSTANCE(pmix_ptl_queue_t,
                                pmix_object_t,
                                qcon, qdes);
