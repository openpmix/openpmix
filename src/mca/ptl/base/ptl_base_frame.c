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
#include "src/util/pmix_printf.h"
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
    .actives = PMIX_LIST_STATIC_INIT(pmix_ptl_base.actives),
    .posted_recvs = PMIX_LIST_STATIC_INIT(pmix_ptl_base.posted_recvs),
    .listener = PMIX_LISTENER_STATIC_INIT,
    .local_listener = PMIX_LISTENER_STATIC_INIT,
    .pending_connections = PMIX_LIST_STATIC_INIT(pmix_ptl_base.pending_connections),
    .tool_queue = PMIX_LIST_STATIC_INIT(pmix_ptl_base.tool_queue),
    .tool_asking = PMIX_LIST_STATIC_INIT(pmix_ptl_base.tool_asking),
    .tool_late = PMIX_LIST_STATIC_INIT(pmix_ptl_base.tool_late),
    .connecting = PMIX_LIST_STATIC_INIT(pmix_ptl_base.connecting),
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
    .rendezvous_dirfd = -1,
    .sysctrlr_dirfd = -1,
    .scheduler_dirfd = -1,
    .system_dirfd = -1,
    .session_dirfd = -1,
    .nspace_dirfd = -1,
    .pid_dirfd = -1,
    .remote_connections = false,
    .connections_specified = false,
    .legacy_peers_param = false,
    .legacy_peers = false,
    .system_tool = false,
    .allow_foreign_tools = false,
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
    .search_depth = 8,
    .connect_ack_timeout = 5,
    .max_pending_connections = 0,
    .pending_cap = 0,
    .max_write = INT_MAX
};
int pmix_ptl_base_output = -1;
pmix_ptl_api_t pmix_ptl = {
    .name = NULL,
    .connect_to_peer = NULL,
    .setup_listener = NULL,
    .setup_fork = NULL,
    .connect_to_peer_nb = NULL
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

    /* Which transports a process uses is not a choice for the ptl
     * selection parameter: every one available is used, whatever it says
     * and wherever it was set - the environment, where client, server and
     * tool init remove it, or a parameter file. The framework registered
     * it just before calling us, and the components are found next */
    free(pmix_ptl_base_framework.framework_selection);
    pmix_ptl_base_framework.framework_selection = NULL;

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

    (void) pmix_mca_base_var_register("pmix", "ptl", "base", "search_depth",
                                      "How many directories below the system tmpdir a tool "
                                      "descends when searching for a server's rendezvous file "
                                      "(default: 8)",
                                      PMIX_MCA_BASE_VAR_TYPE_INT,
                                      &pmix_ptl_base.search_depth);

    (void) pmix_mca_base_var_register("pmix", "ptl", "base", "connect_ack_timeout",
                                      "Number of seconds a server gives an incoming connection to "
                                      "deliver its whole connection request, before dropping "
                                      "the connection (0 = no limit)",
                                      PMIX_MCA_BASE_VAR_TYPE_INT,
                                      &pmix_ptl_base.connect_ack_timeout);

    (void) pmix_mca_base_var_register("pmix", "ptl", "base", "server_access",
                                      "Servers a client or tool will talk to over the local "
                                      "transport besides one running as its own user or root, "
                                      "when no server it was started by names itself: "
                                      "\"users=<list>,groups=<list>\", each list of names or "
                                      "numbers separated by ':' - a server whose uid is in users, "
                                      "or whose effective gid is in groups, is allowed",
                                      PMIX_MCA_BASE_VAR_TYPE_STRING,
                                      &pmix_ptl_base.server_access);

    (void) pmix_mca_base_var_register("pmix", "ptl", "base", "legacy_peers",
                                      "Whether a server takes connections from clients and tools "
                                      "of PMIx releases before v7.0, which connect over TCP. Only "
                                      "a PMIx built with legacy peer support can; the "
                                      "PMIX_SERVER_LEGACY_PEERS attribute overrides it",
                                      PMIX_MCA_BASE_VAR_TYPE_BOOL,
                                      &pmix_ptl_base.legacy_peers_param);

    (void) pmix_mca_base_var_register("pmix", "ptl", "base", "max_pending_connections",
                                      "Most connections a server holds while their connection "
                                      "requests are still arriving, and separately the most tools "
                                      "it holds while the host decides on them; at the first "
                                      "limit, the oldest request still arriving is dropped to "
                                      "make room for a new one, and at the second a further tool "
                                      "is refused (0 = an eighth of the descriptor limit the "
                                      "server starts with, for each; -1 = no limit)",
                                      PMIX_MCA_BASE_VAR_TYPE_INT,
                                      &pmix_ptl_base.max_pending_connections);

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

/* Remove a rendezvous file we created, relative to the descriptor on
 * the directory we created it in, and release everything held for it */
static void remove_rndz_file(char **filename, bool *created, int *dirfd)
{
    const char *base;
    struct stat ours, there;
    char *lname = NULL;
    size_t n, slot = PMIX_PTL_NLOCKS;
    bool remove = true;

    if (NULL != *filename && *created && 0 <= *dirfd) {
        base = strrchr(*filename, PMIX_PATH_SEP[0]);
        base = (NULL == base) ? *filename : base + 1;
        /* the file we wrote at that name, if we know which it was */
        for (n = 0; n < pmix_ptl_base.nrndz_lockfd; n++) {
            if (NULL != pmix_ptl_base.rndz_lockname[n] &&
                0 == strcmp(pmix_ptl_base.rndz_lockname[n], *filename)) {
                slot = n;
                break;
            }
        }
        /* Only while it is still ours: a server can take the name from
         * under us - its file is not ours to remove - and the lock file a
         * tmp cleaner removed is no sign either way */
        if (PMIX_PTL_NLOCKS != slot) {
            remove = (0 == fstatat(*dirfd, base, &there, AT_SYMLINK_NOFOLLOW) &&
                      pmix_ptl_base.rndz_dev[slot] == there.st_dev &&
                      pmix_ptl_base.rndz_ino[slot] == there.st_ino);
        }
        if (remove && 0 != unlinkat(*dirfd, base, 0)) {
            pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                                "Remove of %s failed: %s", *filename, strerror(errno));
        }
        if (PMIX_PTL_NLOCKS != slot) {
            /* and the lock file, if it is the one we hold, before we let
             * go of the lock */
            if (0 <= pmix_ptl_base.rndz_lockfd[slot]) {
                lname = pmix_ptl_base_lock_name(base);
                if (NULL != lname && 0 == fstat(pmix_ptl_base.rndz_lockfd[slot], &ours) &&
                    0 == fstatat(*dirfd, lname, &there, AT_SYMLINK_NOFOLLOW) &&
                    ours.st_dev == there.st_dev && ours.st_ino == there.st_ino) {
                    (void) unlinkat(*dirfd, lname, 0);
                }
                close(pmix_ptl_base.rndz_lockfd[slot]);
                pmix_ptl_base.rndz_lockfd[slot] = -1;
            }
            free(pmix_ptl_base.rndz_lockname[slot]);
            pmix_ptl_base.rndz_lockname[slot] = NULL;
        }
        free(lname);
    }
    if (0 <= *dirfd) {
        close(*dirfd);
        *dirfd = -1;
    }
    free(*filename);
    *filename = NULL;
    *created = false;
}

static void free_server_access(void);

static pmix_status_t pmix_ptl_close(void)
{
    int rc;
    char *tmp;
    pmix_ptl_base_active_t *active;

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
    pmix_ptl_base.legacy_peers = false;
    pmix_ptl_base.system_tool = false;
    pmix_ptl_base.allow_foreign_tools = false;
    pmix_ptl_base.session_tool = false;
    pmix_ptl_base.tool_support = false;
    pmix_ptl_base.max_write = INT_MAX;
    pmix_ptl_base.pending_cap = 0;

    /* ensure the listen thread has been shut down */
    pmix_ptl_base_stop_listening();

    if (NULL != pmix_client_globals.myserver) {
        if (0 <= pmix_client_globals.myserver->sd) {
            CLOSE_THE_SOCKET(pmix_client_globals.myserver->sd);
            pmix_client_globals.myserver->sd = -1;
        }
    }
    /* each transport releases what it holds */
    PMIX_LIST_FOREACH (active, &pmix_ptl_base.actives, pmix_ptl_base_active_t) {
        if (NULL != active->module->close) {
            active->module->close();
        }
    }
    PMIX_LIST_DESTRUCT(&pmix_ptl_base.actives);
    free_server_access();
    /* the component will cleanup when closed */
    PMIX_LIST_DESTRUCT(&pmix_ptl_base.posted_recvs);
    PMIX_DESTRUCT(&pmix_ptl_base.listener);
    PMIX_DESTRUCT(&pmix_ptl_base.local_listener);
    /* stop_listening above released every alternate and their list */
    PMIX_LIST_DESTRUCT(&pmix_ptl_base.alt_listeners);
    /* stop_listening above has already closed and released anything
     * that was still on it, and on the tool queue */
    PMIX_DESTRUCT(&pmix_ptl_base.pending_connections);
    PMIX_DESTRUCT(&pmix_ptl_base.tool_queue);
    /* the tools the host is deciding on are the host's: it holds them */
    while (NULL != pmix_list_remove_first(&pmix_ptl_base.tool_asking)) {
    }
    PMIX_DESTRUCT(&pmix_ptl_base.tool_asking);
    /* a tool still on this list is one the host has yet to answer for,
     * which the host holds, or one whose not-connected notice is still on
     * its way, which that notice holds: neither is ours to release */
    while (NULL != pmix_list_remove_first(&pmix_ptl_base.tool_late)) {
    }
    PMIX_DESTRUCT(&pmix_ptl_base.tool_late);
    /* a connect still under way will never finish now - the progress
     * thread is stopped - so tell whoever is waiting on it */
    pmix_ptl_base_abandon_connects();
    PMIX_DESTRUCT(&pmix_ptl_base.connecting);
    PMIX_LIST_DESTRUCT(&pmix_ptl_base.tokens);

    remove_rndz_file(&pmix_ptl_base.scheduler_filename,
                     &pmix_ptl_base.created_scheduler_filename,
                     &pmix_ptl_base.scheduler_dirfd);
    remove_rndz_file(&pmix_ptl_base.sysctrlr_filename,
                     &pmix_ptl_base.created_sysctrlr_filename,
                     &pmix_ptl_base.sysctrlr_dirfd);
    remove_rndz_file(&pmix_ptl_base.system_filename,
                     &pmix_ptl_base.created_system_filename,
                     &pmix_ptl_base.system_dirfd);
    remove_rndz_file(&pmix_ptl_base.session_filename,
                     &pmix_ptl_base.created_session_filename,
                     &pmix_ptl_base.session_dirfd);
    remove_rndz_file(&pmix_ptl_base.nspace_filename,
                     &pmix_ptl_base.created_nspace_filename,
                     &pmix_ptl_base.nspace_dirfd);
    remove_rndz_file(&pmix_ptl_base.pid_filename,
                     &pmix_ptl_base.created_pid_filename,
                     &pmix_ptl_base.pid_dirfd);
    if (NULL != pmix_ptl_base.rendezvous_filename) {
        /* the file first - destroying the directory takes the file with
         * it, and the remove would then fail on every clean shutdown */
        tmp = NULL;
        if (pmix_ptl_base.created_rendezvous_dir) {
            tmp = pmix_dirname(pmix_ptl_base.rendezvous_filename);
        }
        remove_rndz_file(&pmix_ptl_base.rendezvous_filename,
                         &pmix_ptl_base.created_rendezvous_file,
                         &pmix_ptl_base.rendezvous_dirfd);
        if (NULL != tmp) {
            pmix_os_dirpath_destroy(tmp, true, _check_file);
            free(tmp);
        }
        pmix_ptl_base.created_rendezvous_dir = false;
    }
    /* any lock whose file was not ours to remove is let go of too */
    while (0 < pmix_ptl_base.nrndz_lockfd) {
        --pmix_ptl_base.nrndz_lockfd;
        if (0 <= pmix_ptl_base.rndz_lockfd[pmix_ptl_base.nrndz_lockfd]) {
            close(pmix_ptl_base.rndz_lockfd[pmix_ptl_base.nrndz_lockfd]);
        }
        free(pmix_ptl_base.rndz_lockname[pmix_ptl_base.nrndz_lockfd]);
        pmix_ptl_base.rndz_lockname[pmix_ptl_base.nrndz_lockfd] = NULL;
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
static void free_server_access(void)
{
    free(pmix_ptl_base.access_uids);
    pmix_ptl_base.access_uids = NULL;
    pmix_ptl_base.naccess_uids = 0;
    free(pmix_ptl_base.access_gids);
    pmix_ptl_base.access_gids = NULL;
    pmix_ptl_base.naccess_gids = 0;
    pmix_ptl_base.server_expect_given = false;
    pmix_ptl_base.server_release_vouched = false;
}

static void open_cleanup(void)
{
    PMIX_LIST_DESTRUCT(&pmix_ptl_base.actives);
    free_server_access();
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
    PMIX_DESTRUCT(&pmix_ptl_base.local_listener);
    PMIX_LIST_DESTRUCT(&pmix_ptl_base.alt_listeners);
    PMIX_DESTRUCT(&pmix_ptl_base.pending_connections);
    PMIX_DESTRUCT(&pmix_ptl_base.tool_queue);
    PMIX_DESTRUCT(&pmix_ptl_base.tool_asking);
    PMIX_DESTRUCT(&pmix_ptl_base.tool_late);
    PMIX_DESTRUCT(&pmix_ptl_base.connecting);
    PMIX_LIST_DESTRUCT(&pmix_ptl_base.tokens);
    pmix_ptl_base.initialized = false;
}

static pmix_status_t pmix_ptl_open(pmix_mca_base_open_flag_t flags)
{
    pmix_status_t rc;
    const char *tdir;
    bool server;

    /* initialize globals */
    pmix_ptl_base.initialized = true;
    PMIX_CONSTRUCT(&pmix_ptl_base.actives, pmix_list_t);
    PMIX_CONSTRUCT(&pmix_ptl_base.posted_recvs, pmix_list_t);
    PMIX_CONSTRUCT(&pmix_ptl_base.listener, pmix_listener_t);
    PMIX_CONSTRUCT(&pmix_ptl_base.local_listener, pmix_listener_t);
    PMIX_CONSTRUCT(&pmix_ptl_base.alt_listeners, pmix_list_t);
    pmix_ptl_base.alt_uris = NULL;
    PMIX_CONSTRUCT(&pmix_ptl_base.pending_connections, pmix_list_t);
    PMIX_CONSTRUCT(&pmix_ptl_base.tool_queue, pmix_list_t);
    PMIX_CONSTRUCT(&pmix_ptl_base.tool_asking, pmix_list_t);
    PMIX_CONSTRUCT(&pmix_ptl_base.tool_late, pmix_list_t);
    PMIX_CONSTRUCT(&pmix_ptl_base.connecting, pmix_list_t);
    PMIX_CONSTRUCT(&pmix_ptl_base.tokens, pmix_list_t);

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

    /* the servers we may talk to besides our own user's and root's */
    rc = pmix_ptl_base_parse_server_access(pmix_ptl_base.server_access);
    if (PMIX_SUCCESS != rc) {
        goto error;
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

void pmix_ptl_base_monotime(struct timeval *tv)
{
#if PMIX_HAVE_CLOCK_GETTIME && defined(CLOCK_MONOTONIC)
    struct timespec ts;

    if (0 == clock_gettime(CLOCK_MONOTONIC, &ts)) {
        tv->tv_sec = ts.tv_sec;
        tv->tv_usec = ts.tv_nsec / 1000;
        return;
    }
#endif
    (void) gettimeofday(tv, NULL);
}

static void pccon(pmix_pending_connection_t *p)
{
    /* every field is set here - including the ones the listener fills in
     * immediately, since an error path may look at them before it gets
     * that far, and several of these want something other than the zero
     * PMIX_NEW would leave */
    p->protocol = PMIX_PROTOCOL_UNDEF;
    p->transport = NULL;
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
    p->id_unsent = false;
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
    p->psecmod = NULL;
    p->proved = false;
    p->auth_bound = false;
    p->authmod = NULL;
    p->auth_uid = (uid_t) -1;
    memset(p->auth_tool, 0, sizeof(p->auth_tool));
    memset(p->auth_srv, 0, sizeof(p->auth_srv));
    p->nptr = NULL;
    p->rinfo = NULL;
    p->bfropsmod = NULL;
    p->gdsmod = NULL;
    p->blob = NULL;
    p->bloblen = 0;
    p->pid = 0;
    p->realuid = 0;
    p->realgid = 0;
    p->realuid_given = false;
    p->realgid_given = false;
    PMIX_LOAD_PROCID(&p->assigned, NULL, PMIX_RANK_UNDEF);
    p->connecting = false;
    pmix_atomic_unset_bool(&p->answered);
    p->upcall_rc = PMIX_SUCCESS;
    p->abandoned = false;
    p->late_listed = false;
    p->asking = false;
    p->token = NULL;
    pmix_ptl_base_monotime(&p->waiting);
    p->host_tmo = 0;
}
static void pcdes(pmix_pending_connection_t *p)
{
    pmix_peer_t *peer = (pmix_peer_t *) p->peer;
    pmix_namespace_t *nptr = (pmix_namespace_t *) p->nptr;
    pmix_rank_info_t *rinfo = (pmix_rank_info_t *) p->rinfo;

    if (p->timer_active) {
        pmix_event_evtimer_del(&p->timer);
        p->timer_active = false;
    }
    /* a connection that was waiting on the host no longer is */
    if (p->asking) {
        pmix_list_remove_item(&pmix_ptl_base.tool_asking, &p->super);
        p->asking = false;
    }
    if (p->connecting && NULL != rinfo && 0 < rinfo->connecting) {
        --rinfo->connecting;
    }
    /* a connection that was never accepted: what it built was never
     * listed, so releasing it is all the undoing there is */
    if (NULL != peer) {
        /* the socket is ours, below - not the peer's */
        peer->sd = -1;
        PMIX_RELEASE(peer);
    }
    if (NULL != rinfo) {
        PMIX_RELEASE(rinfo);
    }
    if (NULL != nptr) {
        PMIX_RELEASE(nptr);
    }
    if (0 <= p->sd) {
        CLOSE_THE_SOCKET(p->sd);
    }
    if (NULL != p->blob) {
        /* the info it holds may carry a connection token */
        pmix_ptl_base_wipe(p->blob, p->bloblen);
        free(p->blob);
    }
    if (NULL != p->msg) {
        /* as much of the connect-ack as arrived, token and all */
        pmix_ptl_base_wipe(p->msg, p->msg_recvd);
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
    pmix_ptl_base_wipe_token(p->token);
    free(p->token);
}
PMIX_EXPORT PMIX_CLASS_INSTANCE(pmix_pending_connection_t,
                                pmix_list_item_t,
                                pccon, pcdes);

static void lcon(pmix_listener_t *p)
{
    memset(&p->ev, 0, sizeof(pmix_event_t));
    p->active = false;
    p->protocol = PMIX_PROTOCOL_UNDEF;
    p->transport = NULL;
    p->socket = -1;
    p->varname = NULL;
    p->uri = NULL;
    p->owner = 0;
    p->owner_given = false;
    p->group = 0;
    p->group_given = false;
    p->cbfunc = NULL;
    p->mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH;
    memset(&p->retry_ev, 0, sizeof(pmix_event_t));
    p->paused = false;
    p->nretries = 0;
    p->warned = false;
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
PMIX_EXPORT PMIX_CLASS_INSTANCE(pmix_ptl_base_active_t,
                                pmix_list_item_t,
                                NULL, NULL);

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
    p->alt_uris = NULL;
    p->local_uri = NULL;
    p->bfrops = NULL;
    p->psec = NULL;
    p->vouched = false;
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
    free(p->alt_uris);
    free(p->local_uri);
    free(p->bfrops);
    free(p->psec);
}
PMIX_EXPORT PMIX_CLASS_INSTANCE(pmix_connection_t,
                                pmix_list_item_t,
                                ccon, dcon);
