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
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
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

#include "pmix_common.h"

#ifdef HAVE_STRING_H
#    include <string.h>
#endif
#ifdef HAVE_FCNTL_H
#    include <fcntl.h>
#endif
#ifdef HAVE_SYS_STAT_H
#    include <sys/stat.h>
#endif

#include "src/class/pmix_list.h"
#include "src/client/pmix_client_ops.h"
#include "src/mca/base/pmix_base.h"
#include "src/mca/base/pmix_mca_base_framework.h"
#include "src/mca/base/pmix_mca_base_var.h"
#include "src/mca/mca.h"
#include "src/server/pmix_server_ops.h"
#include "src/util/pmix_basename.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_os_dirpath.h"
#include "src/util/pmix_os_path.h"
#include "src/util/pmix_environ.h"
#include "src/util/pmix_parse_options.h"
#include "src/util/pmix_show_help.h"

#include "src/mca/ptl/base/base.h"
#include "src/mca/ptl/ptl_types.h"

/*
 * The following file was created by configure.  It contains extern
 * statements and the definition of an array of pointers to each
 * component's public mca_base_component_t struct.
 */

#include "src/mca/ptl/base/static-components.h"

/* Instantiate the global vars */
pmix_ptl_base_t pmix_ptl_base = {
    .initialized = false,
    .selected = false,
    .posted_recvs = PMIX_LIST_STATIC_INIT(pmix_ptl_base.posted_recvs),
    .listener = PMIX_LISTENER_STATIC_INIT,
    .pending_connections = PMIX_LIST_STATIC_INIT(pmix_ptl_base.pending_connections),
    .connection = NULL,
    .max_msg_size = 0,
    .session_tmpdir = NULL,
    .system_tmpdir = NULL,
    .report_uri = NULL,
    .uri = NULL,
    .urifile = NULL,
    .sysctrlr_filename = NULL,
    .scheduler_filename = NULL,
    .system_filename = NULL,
    .session_filename = NULL,
    .nspace_filename = NULL,
    .pid_filename = NULL,
    .rendezvous_filename = NULL,
    .created_rendezvous_dir = false,
    .created_rendezvous_file = false,
    .created_session_tmpdir = false,
    .created_system_tmpdir = false,
    .created_sysctrlr_filename = false,
    .created_scheduler_filename = false,
    .created_system_filename = false,
    .created_session_filename = false,
    .created_nspace_filename = false,
    .created_pid_filename = false,
    .created_urifile = false,
    .remote_connections = false,
    .connections_specified = false,
    .system_tool = false,
    .allow_foreign_tools = true,
    .session_tool = false,
    .tool_support = false,
    .if_include = NULL,
    .if_exclude = NULL,
    .ipv4_ports = NULL,
    .disable_ipv4_family = false,
    .ipv6_ports = NULL,
    .disable_ipv6_family = true,
    .max_retries = 0,
    .wait_to_connect = 0,
    .handshake_wait_time = 60,
    .handshake_max_retries = 0,
    .connect_ack_timeout = 5,
    .max_write = INT_MAX
};
int pmix_ptl_base_output = -1;
pmix_ptl_module_t pmix_ptl = {
    .name = NULL,
    .init = NULL,
    .finalize = NULL,
    .recv = NULL,
    .cancel = NULL,
    .connect_to_peer = NULL,
    .query_servers = NULL,
    .setup_listener = NULL,
    .setup_fork = NULL
};

static size_t max_msg_size = 32;
static char *dyn_port_string;
#if PMIX_ENABLE_IPV6
static char *dyn_port_string6;
#endif

/* Turn a port-list parameter into the array the listener scans. No value,
 * a value the parser cannot read ("-", "abc") and the "-1" wildcard all
 * mean the same thing - let the kernel choose - so each yields {"0"}.
 *
 * Whatever array is already there is given back first. The parser appends
 * to the array it is handed, and this can run a second time in a process:
 * pmix_ptl_close only frees the arrays when the framework was opened, and
 * a framework that was registered but never opened is re-registered by
 * the next open. */
pmix_status_t pmix_ptl_base_set_ports(const char *spec, char ***ports)
{
    PMIx_Argv_free(*ports);
    *ports = NULL;
    if (NULL != spec) {
        pmix_util_parse_range_options((char *) spec, ports);
        if (NULL != *ports && NULL != (*ports)[0] && 0 != strcmp((*ports)[0], "-1")) {
            return PMIX_SUCCESS;
        }
        PMIx_Argv_free(*ports);
        *ports = NULL;
    }
    return PMIx_Argv_append_nosize(ports, "0");
}

static int pmix_ptl_register(pmix_mca_base_register_flag_t flags)
{
    int idx;
    pmix_status_t rc;

    (void) flags;
    pmix_mca_base_var_register("pmix", "ptl", "base", "max_msg_size",
                               "Max size (in Mbytes) of a client/server msg",
                               PMIX_MCA_BASE_VAR_TYPE_SIZE_T,
                               &max_msg_size);
    /* a zero value means "no limit" - we still need a ceiling to bound
     * the allocation an inbound header can ask for, so use the taint
     * limit. Note that the working value must always be assigned: leaving
     * it at its static zero would reject every message carrying a payload.
     * A value too large to express in bytes means "no limit" too - the
     * multiplication would otherwise wrap, and a 4096 MB limit on a 32-bit
     * size_t wraps to exactly that zero */
    if (0 == max_msg_size || (PMIX_TAINT_SIZE_LIMIT) / (1024 * 1024) < max_msg_size) {
        pmix_ptl_base.max_msg_size = PMIX_TAINT_SIZE_LIMIT;
    } else {
        pmix_ptl_base.max_msg_size = max_msg_size * 1024 * 1024;
    }

    idx = pmix_mca_base_var_register("pmix", "ptl", "base", "if_include",
                                     "Comma-delimited list of devices and/or CIDR notation of TCP networks "
                                     "(e.g., \"eth0,192.168.0.0/16\").  Mutually exclusive with ptl_base_if_exclude.",
                                     PMIX_MCA_BASE_VAR_TYPE_STRING,
                                     &pmix_ptl_base.if_include);
    (void) pmix_mca_base_var_register_synonym(idx, "pmix", "ptl", "tcp", "if_include",
                                              PMIX_MCA_BASE_VAR_SYN_FLAG_DEPRECATED);

    idx = pmix_mca_base_var_register("pmix", "ptl", "base", "if_exclude",
                                     "Comma-delimited list of devices and/or CIDR notation of TCP networks to NOT use "
                                     "-- all devices not matching these specifications will be used (e.g., "
                                     "\"eth0,192.168.0.0/16\"). "
                                     "If set to a non-default value, it is mutually exclusive with ptl_base_if_include.",
                                     PMIX_MCA_BASE_VAR_TYPE_STRING,
                                     &pmix_ptl_base.if_exclude);
    (void) pmix_mca_base_var_register_synonym(idx, "pmix", "ptl", "tcp", "if_exclude",
                                              PMIX_MCA_BASE_VAR_SYN_FLAG_DEPRECATED);

    /* if_include and if_exclude need to be mutually exclusive */
    if (NULL != pmix_ptl_base.if_include && NULL != pmix_ptl_base.if_exclude) {
        pmix_show_help("help-ptl-base.txt", "include-exclude", true, pmix_ptl_base.if_include,
                       pmix_ptl_base.if_exclude);
        return PMIX_ERR_NOT_AVAILABLE;
    }

    dyn_port_string = NULL;
    idx = pmix_mca_base_var_register("pmix", "ptl", "base", "ipv4_ports",
                                     "IPv4 port(s) to be used",
                                     PMIX_MCA_BASE_VAR_TYPE_STRING,
                                     &dyn_port_string);
    (void) pmix_mca_base_var_register_synonym(idx, "pmix", "ptl", "base", "ipv4_port",
                                              PMIX_MCA_BASE_VAR_SYN_FLAG_DEPRECATED);
    (void) pmix_mca_base_var_register_synonym(idx, "pmix", "ptl", "tcp", "ipv4_port",
                                              PMIX_MCA_BASE_VAR_SYN_FLAG_DEPRECATED);
    rc = pmix_ptl_base_set_ports(dyn_port_string, &pmix_ptl_base.ipv4_ports);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }

#if PMIX_ENABLE_IPV6
    dyn_port_string6 = NULL;
    idx = pmix_mca_base_var_register("pmix", "ptl", "base", "ipv6_ports",
                                     "IPv6 port(s) to be used",
                                     PMIX_MCA_BASE_VAR_TYPE_STRING,
                                     &dyn_port_string6);
    (void) pmix_mca_base_var_register_synonym(idx, "pmix", "ptl", "base", "ipv6_port",
                                              PMIX_MCA_BASE_VAR_SYN_FLAG_DEPRECATED);
    (void) pmix_mca_base_var_register_synonym(idx, "pmix", "ptl", "tcp", "ipv6_port",
                                              PMIX_MCA_BASE_VAR_SYN_FLAG_DEPRECATED);
    rc = pmix_ptl_base_set_ports(dyn_port_string6, &pmix_ptl_base.ipv6_ports);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
#endif

    idx = pmix_mca_base_var_register("pmix", "ptl", "base", "disable_ipv4_family",
                                     "Disable the IPv4 interfaces", PMIX_MCA_BASE_VAR_TYPE_BOOL,
                                     &pmix_ptl_base.disable_ipv4_family);
    (void) pmix_mca_base_var_register_synonym(idx, "pmix", "ptl", "tcp", "disable_ipv4_family",
                                              PMIX_MCA_BASE_VAR_SYN_FLAG_DEPRECATED);

    pmix_ptl_base.disable_ipv6_family = true;
    idx = pmix_mca_base_var_register("pmix", "ptl", "base", "disable_ipv6_family",
                                     "Disable the IPv6 interfaces (default:disabled)",
                                     PMIX_MCA_BASE_VAR_TYPE_BOOL,
                                     &pmix_ptl_base.disable_ipv6_family);
    (void) pmix_mca_base_var_register_synonym(idx, "pmix", "ptl", "tcp", "disable_ipv6_family",
                                              PMIX_MCA_BASE_VAR_SYN_FLAG_DEPRECATED);

    idx = pmix_mca_base_var_register("pmix", "ptl", "base", "connection_wait_time",
                                     "Number of seconds to wait for the server connection file to appear",
                                     PMIX_MCA_BASE_VAR_TYPE_INT,
                                     &pmix_ptl_base.wait_to_connect);
    (void) pmix_mca_base_var_register_synonym(idx, "pmix", "ptl", "tcp", "connection_wait_time",
                                              PMIX_MCA_BASE_VAR_SYN_FLAG_DEPRECATED);

    idx = pmix_mca_base_var_register("pmix", "ptl", "base", "max_retries",
                                     "Number of times to look for the connection file before quitting",
                                     PMIX_MCA_BASE_VAR_TYPE_INT,
                                     &pmix_ptl_base.max_retries);
    (void) pmix_mca_base_var_register_synonym(idx, "pmix", "ptl", "tcp", "max_retries",
                                              PMIX_MCA_BASE_VAR_SYN_FLAG_DEPRECATED);

    idx = pmix_mca_base_var_register("pmix", "ptl", "base", "handshake_wait_time",
                                     "Seconds a client or tool waits on a server while connecting to it - for the connect "
                                     "itself and for each reply to the handshake request (0 = wait indefinitely)",
                                     PMIX_MCA_BASE_VAR_TYPE_INT,
                                     &pmix_ptl_base.handshake_wait_time);
    (void) pmix_mca_base_var_register_synonym(idx, "pmix", "ptl", "tcp", "handshake_wait_time",
                                              PMIX_MCA_BASE_VAR_SYN_FLAG_DEPRECATED);

    idx = pmix_mca_base_var_register("pmix", "ptl", "base", "handshake_max_retries",
                                     "Number of times to retry the handshake request before giving up",
                                     PMIX_MCA_BASE_VAR_TYPE_INT,
                                     &pmix_ptl_base.handshake_max_retries);
    (void) pmix_mca_base_var_register_synonym(idx, "pmix", "ptl", "tcp", "handshake_max_retries",
                                              PMIX_MCA_BASE_VAR_SYN_FLAG_DEPRECATED);

    (void) pmix_mca_base_var_register("pmix", "ptl", "base", "connect_ack_timeout",
                                      "Number of seconds a server gives an incoming connection to "
                                      "deliver its whole connection request, and to answer each "
                                      "step of any security handshake after it, before dropping "
                                      "the connection (0 = no limit)",
                                      PMIX_MCA_BASE_VAR_TYPE_INT,
                                      &pmix_ptl_base.connect_ack_timeout);

    idx = pmix_mca_base_var_register("pmix", "ptl", "base", "report_uri",
                                     "Output URI [- => stdout, + => stderr, or filename]",
                                     PMIX_MCA_BASE_VAR_TYPE_STRING,
                                     &pmix_ptl_base.report_uri);
    (void) pmix_mca_base_var_register_synonym(idx, "pmix", "ptl", "tcp", "report_uri",
                                              PMIX_MCA_BASE_VAR_SYN_FLAG_DEPRECATED);

    return PMIX_SUCCESS;
}

static bool _check_file(const char *root, const char *path)
{
    struct stat st;
    char *fullpath;
    int rc;

    /*
     * Keep:
     *  - non-zero files starting with "output-"
     */
    if (0 == strncmp(path, "output-", strlen("output-"))) {
        memset(&st, 0, sizeof(struct stat));
        fullpath = pmix_os_path(false, root, path, NULL);
        if (NULL == fullpath) {
            /* cannot assemble the name: allow removal */
            return true;
        }
        rc = stat(fullpath, &st);
        if (0 != rc) {
            free(fullpath);
            return true;
        }
        free(fullpath);
        if (0 == st.st_size) {
            return true;
        }
        return false;
    }

    return true;
}

static pmix_status_t pmix_ptl_close(void)
{
    int rc;
    char *tmp;

    if (!pmix_ptl_base.initialized) {
        return PMIX_SUCCESS;
    }
    pmix_ptl_base.initialized = false;
    pmix_ptl_base.selected = false;

    /* Everything freed below is also nulled, and every "did I create
     * this" flag reset with it. The framework may be opened again in
     * this process - pmix_mca_base_framework_close clears the REGISTERED
     * flag, so a later PMIx_server_init runs the whole open path afresh -
     * and pmix_ptl_open only reinstates the fields it sets itself. A
     * rendezvous filename left dangling from the previous cycle would be
     * unlinked and freed a second time here. */

    /* The same goes for the role flags pmix_ptl_base_setup_listener reads
     * out of its caller's directives: it only ever sets the ones it is
     * given, so a flag left over from the previous cycle - remote
     * connections, tool support - would silently apply to a server that
     * never asked for it. Put back the defaults from the initializer. */
    pmix_ptl_base.remote_connections = false;
    pmix_ptl_base.connections_specified = false;
    pmix_ptl_base.system_tool = false;
    pmix_ptl_base.allow_foreign_tools = true;
    pmix_ptl_base.session_tool = false;
    pmix_ptl_base.tool_support = false;
    pmix_ptl_base.max_write = INT_MAX;

    /* ensure the listen thread has been shut down */
    pmix_ptl_base_stop_listening();

    if (NULL != pmix_client_globals.myserver) {
        if (0 <= pmix_client_globals.myserver->sd) {
            CLOSE_THE_SOCKET(pmix_client_globals.myserver->sd);
            pmix_client_globals.myserver->sd = -1;
        }
    }
    if (NULL != pmix_ptl_base.connection) {
        free(pmix_ptl_base.connection);
        pmix_ptl_base.connection = NULL;
    }
    /* the component will cleanup when closed */
    PMIX_LIST_DESTRUCT(&pmix_ptl_base.posted_recvs);
    PMIX_DESTRUCT(&pmix_ptl_base.listener);
    /* stop_listening above has already closed and released anything
     * that was still on it */
    PMIX_DESTRUCT(&pmix_ptl_base.pending_connections);

    if (NULL != pmix_ptl_base.scheduler_filename) {
        if (pmix_ptl_base.created_scheduler_filename) {
            rc = remove(pmix_ptl_base.scheduler_filename);
            if (0 != rc) {
                pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                                    "Remove of %s failed: %s",
                                    pmix_ptl_base.scheduler_filename, strerror(errno));
            }
        }
        free(pmix_ptl_base.scheduler_filename);
        pmix_ptl_base.scheduler_filename = NULL;
        pmix_ptl_base.created_scheduler_filename = false;
    }
    if (NULL != pmix_ptl_base.sysctrlr_filename) {
        if (pmix_ptl_base.created_sysctrlr_filename) {
            rc = remove(pmix_ptl_base.sysctrlr_filename);
            if (0 != rc) {
                pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                                    "Remove of %s failed: %s",
                                    pmix_ptl_base.sysctrlr_filename, strerror(errno));
            }
        }
        free(pmix_ptl_base.sysctrlr_filename);
        pmix_ptl_base.sysctrlr_filename = NULL;
        pmix_ptl_base.created_sysctrlr_filename = false;
    }
    if (NULL != pmix_ptl_base.system_filename) {
        if (pmix_ptl_base.created_system_filename) {
            rc = remove(pmix_ptl_base.system_filename);
            if (0 != rc) {
                pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                                    "Remove of %s failed: %s",
                                    pmix_ptl_base.system_filename, strerror(errno));
            }
        }
        free(pmix_ptl_base.system_filename);
        pmix_ptl_base.system_filename = NULL;
        pmix_ptl_base.created_system_filename = false;
    }
    if (NULL != pmix_ptl_base.session_filename) {
        if (pmix_ptl_base.created_session_filename) {
            rc = remove(pmix_ptl_base.session_filename);
            if (0 != rc) {
                pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                                    "Remove of %s failed: %s",
                                    pmix_ptl_base.session_filename, strerror(errno));
            }
        }
        free(pmix_ptl_base.session_filename);
        pmix_ptl_base.session_filename = NULL;
        pmix_ptl_base.created_session_filename = false;
    }
    if (NULL != pmix_ptl_base.nspace_filename) {
        if (pmix_ptl_base.created_nspace_filename) {
            rc = remove(pmix_ptl_base.nspace_filename);
            if (0 != rc) {
                pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                                    "Remove of %s failed: %s",
                                    pmix_ptl_base.nspace_filename, strerror(errno));
            }
        }
        free(pmix_ptl_base.nspace_filename);
        pmix_ptl_base.nspace_filename = NULL;
        pmix_ptl_base.created_nspace_filename = false;
    }
    if (NULL != pmix_ptl_base.pid_filename) {
        if (pmix_ptl_base.created_pid_filename) {
            rc = remove(pmix_ptl_base.pid_filename);
            if (0 != rc) {
                pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                                    "Remove of %s failed: %s",
                                    pmix_ptl_base.pid_filename, strerror(errno));
            }
        }
        free(pmix_ptl_base.pid_filename);
        pmix_ptl_base.pid_filename = NULL;
        pmix_ptl_base.created_pid_filename = false;
    }
    if (NULL != pmix_ptl_base.rendezvous_filename) {
        /* the file first - destroying the directory takes the file with
         * it, and the remove would then fail on every clean shutdown */
        if (pmix_ptl_base.created_rendezvous_file) {
            rc = remove(pmix_ptl_base.rendezvous_filename);
            if (0 != rc) {
                pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                                    "Remove of %s failed: %s",
                                    pmix_ptl_base.rendezvous_filename, strerror(errno));
            }
        }
        if (pmix_ptl_base.created_rendezvous_dir) {
            tmp = pmix_dirname(pmix_ptl_base.rendezvous_filename);
            pmix_os_dirpath_destroy(tmp, true, _check_file);
            free(tmp);
        }
        free(pmix_ptl_base.rendezvous_filename);
        pmix_ptl_base.rendezvous_filename = NULL;
        pmix_ptl_base.created_rendezvous_dir = false;
        pmix_ptl_base.created_rendezvous_file = false;
    }
    if (NULL != pmix_ptl_base.uri) {
        free(pmix_ptl_base.uri);
        pmix_ptl_base.uri = NULL;
    }
    if (NULL != pmix_ptl_base.urifile) {
        if (pmix_ptl_base.created_urifile) {
            /* remove the file */
            rc = remove(pmix_ptl_base.urifile);
            if (0 != rc) {
                pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                                    "Remove of %s failed: %s",
                                    pmix_ptl_base.urifile, strerror(errno));
            }
        }
        free(pmix_ptl_base.urifile);
        pmix_ptl_base.urifile = NULL;
        pmix_ptl_base.created_urifile = false;
    }
    if (NULL != pmix_ptl_base.session_tmpdir) {
        /* if I created the session tmpdir, then remove it if empty */
        if (pmix_ptl_base.created_session_tmpdir) {
            pmix_os_dirpath_destroy(pmix_ptl_base.session_tmpdir, true, _check_file);
        }
        free(pmix_ptl_base.session_tmpdir);
        pmix_ptl_base.session_tmpdir = NULL;
        pmix_ptl_base.created_session_tmpdir = false;
    }
    if (NULL != pmix_ptl_base.system_tmpdir) {
        if (pmix_ptl_base.created_system_tmpdir) {
            pmix_os_dirpath_destroy(pmix_ptl_base.system_tmpdir, true, _check_file);
        }
        free(pmix_ptl_base.system_tmpdir);
        pmix_ptl_base.system_tmpdir = NULL;
        pmix_ptl_base.created_system_tmpdir = false;
    }

    if (NULL != pmix_ptl_base.ipv4_ports) {
        PMIx_Argv_free(pmix_ptl_base.ipv4_ports);
        pmix_ptl_base.ipv4_ports = NULL;
    }

#if PMIX_ENABLE_IPV6
    if (NULL != pmix_ptl_base.ipv6_ports) {
        PMIx_Argv_free(pmix_ptl_base.ipv6_ports);
        pmix_ptl_base.ipv6_ports = NULL;
    }
#endif


    return pmix_mca_base_framework_components_close(&pmix_ptl_base_framework, NULL);
}

/* Give back what pmix_ptl_open built, when the open fails. pmix_ptl_close
 * cannot do this: pmix_mca_base_framework_open() answers a failed open by
 * closing the framework, and a framework that never reached OPEN is closed
 * without calling its close function. */
static void open_cleanup(void)
{
    free(pmix_ptl_base.connection);
    pmix_ptl_base.connection = NULL;
    free(pmix_ptl_base.session_tmpdir);
    pmix_ptl_base.session_tmpdir = NULL;
    free(pmix_ptl_base.system_tmpdir);
    pmix_ptl_base.system_tmpdir = NULL;
    free(pmix_ptl_base.urifile);
    pmix_ptl_base.urifile = NULL;
    free(pmix_ptl_base.rendezvous_filename);
    pmix_ptl_base.rendezvous_filename = NULL;
    PMIX_LIST_DESTRUCT(&pmix_ptl_base.posted_recvs);
    PMIX_DESTRUCT(&pmix_ptl_base.listener);
    PMIX_DESTRUCT(&pmix_ptl_base.pending_connections);
    pmix_ptl_base.initialized = false;
}

static pmix_status_t pmix_ptl_open(pmix_mca_base_open_flag_t flags)
{
    pmix_status_t rc;
    const char *tdir;
    bool server;

    /* initialize globals */
    pmix_ptl_base.initialized = true;
    PMIX_CONSTRUCT(&pmix_ptl_base.posted_recvs, pmix_list_t);
    PMIX_CONSTRUCT(&pmix_ptl_base.listener, pmix_listener_t);
    PMIX_CONSTRUCT(&pmix_ptl_base.pending_connections, pmix_list_t);
    pmix_ptl_base.connection = (struct sockaddr_storage *)malloc(sizeof(struct sockaddr_storage));
    if (NULL == pmix_ptl_base.connection) {
        rc = PMIX_ERR_NOMEM;
        goto error;
    }
    memset(pmix_ptl_base.connection, 0, sizeof(struct sockaddr_storage));

    /* check for environ-based directives on the tmpdirs to use. A server
     * or launcher has already resolved both in its init, and they can only
     * be NULL here if that copy failed - which is an allocation failure,
     * not an absent directory */
    server = PMIX_PEER_IS_SERVER(pmix_globals.mypeer) || PMIX_PEER_IS_LAUNCHER(pmix_globals.mypeer);
    if (server) {
        tdir = pmix_server_globals.tmpdir;
    } else if (NULL == (tdir = getenv("PMIX_SERVER_TMPDIR"))) {
        tdir = pmix_tmp_directory();
    }
    if (NULL == tdir || NULL == (pmix_ptl_base.session_tmpdir = strdup(tdir))) {
        rc = PMIX_ERR_NOMEM;
        goto error;
    }

    if (server) {
        tdir = pmix_server_globals.system_tmpdir;
    } else if (NULL == (tdir = getenv("PMIX_SYSTEM_TMPDIR"))) {
        tdir = pmix_tmp_directory();
    }
    if (NULL == tdir || NULL == (pmix_ptl_base.system_tmpdir = strdup(tdir))) {
        rc = PMIX_ERR_NOMEM;
        goto error;
    }

    if (NULL != pmix_ptl_base.report_uri && 0 != strcmp(pmix_ptl_base.report_uri, "-")
        && 0 != strcmp(pmix_ptl_base.report_uri, "+")) {
        pmix_ptl_base.urifile = strdup(pmix_ptl_base.report_uri);
        if (NULL == pmix_ptl_base.urifile) {
            rc = PMIX_ERR_NOMEM;
            goto error;
        }
    }

    if (server) {
        if (NULL != (tdir = getenv("PMIX_LAUNCHER_RENDEZVOUS_FILE"))) {
            pmix_ptl_base.rendezvous_filename = strdup(tdir);
            if (NULL == pmix_ptl_base.rendezvous_filename) {
                rc = PMIX_ERR_NOMEM;
                goto error;
            }
        }
    }

    /* Open up all available components */
    rc = pmix_mca_base_framework_components_open(&pmix_ptl_base_framework, flags);
    pmix_ptl_base_output = pmix_ptl_base_framework.framework_output;
    if (PMIX_SUCCESS == rc) {
        return PMIX_SUCCESS;
    }

error:
    open_cleanup();
    return rc;
}

PMIX_MCA_BASE_VERSIONED_FRAMEWORK_DECLARE(pmix, ptl, "PMIx Transfer Layer", pmix_ptl_register, pmix_ptl_open,
                                pmix_ptl_close, pmix_mca_ptl_base_static_components,
                                PMIX_MCA_BASE_FRAMEWORK_FLAG_DEFAULT);

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
    p->sd = -1;
    memset(&p->hdr, 0, sizeof(pmix_ptl_hdr_t));
    p->hdr.tag = UINT32_MAX;
    p->hdr.nbytes = 0;
    p->data = NULL;
    p->hdr_recvd = false;
    p->rdptr = NULL;
    p->rdbytes = 0;
    p->loopback = PMIX_PTL_LOOPBACK_NONE;
}
static void rdes(pmix_ptl_recv_t *p)
{
    /* the payload stays ours until pmix_ptl_base_process_msg loads it into
     * a buffer and clears the pointer. Every other way a message ends - no
     * recv posted for its tag, a recv with no callback, or a connection
     * lost part-way through reading it - releases it with the payload
     * still attached */
    if (NULL != p->data) {
        free(p->data);
    }
    if (NULL != p->peer) {
        PMIX_RELEASE(p->peer);
    }
}
PMIX_EXPORT PMIX_CLASS_INSTANCE(pmix_ptl_recv_t,
                                pmix_list_item_t,
                                rcon, rdes);

static void prcon(pmix_ptl_posted_recv_t *p)
{
    p->peer = NULL;
    p->tag = UINT32_MAX;
    p->cbfunc = NULL;
    p->cbdata = NULL;
}
PMIX_EXPORT PMIX_CLASS_INSTANCE(pmix_ptl_posted_recv_t,
                                pmix_list_item_t,
                                prcon, NULL);

static void srcon(pmix_ptl_sr_t *p)
{
    p->active = false;
    p->peer = NULL;
    p->status = PMIX_SUCCESS;
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
    /* every field is set here - including the ones the listener fills in
     * immediately, since an error path may look at them before it gets
     * that far, and several of these want something other than the zero
     * PMIX_NEW would leave */
    p->protocol = PMIX_PROTOCOL_UNDEF;
    /* assigned only when the connect-ack timeout is enabled, and deleted
     * only when timer_active says it was added */
    memset(&p->timer, 0, sizeof(p->timer));
    p->timer_active = false;
    p->sd = -1;
    memset(&p->hdr, 0, sizeof(p->hdr));
    p->hdr_recvd = 0;
    p->msg = NULL;
    p->msg_recvd = 0;
    p->status = PMIX_SUCCESS;
    p->flag = PMIX_SIMPLE_CLIENT;
    p->buffer_type = PMIX_BFROP_BUFFER_UNDEF;
    p->len = 0;
    p->uid = 0;
    p->gid = 0;
    memset(&p->addr, 0, sizeof(p->addr));
    p->need_id = false;
    p->nspace_created = false;
    p->rinfo_created = false;
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
    if (p->timer_active) {
        pmix_event_evtimer_del(&p->timer);
        p->timer_active = false;
    }
    if (NULL != p->msg) {
        free(p->msg);
    }
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
                                pmix_list_item_t,
                                pccon, pcdes);

static void lcon(pmix_listener_t *p)
{
    memset(&p->ev, 0, sizeof(pmix_event_t));
    p->active = false;
    p->protocol = PMIX_PROTOCOL_UNDEF;
    p->socket = -1;
    p->varname = NULL;
    p->uri = NULL;
    p->owner = 0;
    p->owner_given = false;
    p->group = 0;
    p->group_given = false;
    p->cbfunc = NULL;
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
    p->active = false;
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

static void ccon(pmix_connection_t *p)
{
    p->sd = -1;
    p->nspace = NULL;
    p->rank = PMIX_RANK_INVALID;
    p->uri = NULL;
    p->version = NULL;
}
static void dcon(pmix_connection_t *p)
{
    if (NULL != p->nspace) {
        free(p->nspace);
    }
    if (NULL != p->uri) {
        free(p->uri);
    }
    if (NULL != p->version) {
        free(p->version);
    }
}
PMIX_EXPORT PMIX_CLASS_INSTANCE(pmix_connection_t,
                                pmix_list_item_t,
                                ccon, dcon);
