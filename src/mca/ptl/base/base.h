/* -*- C -*-
 *
 * Copyright (c) 2004-2007 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2006 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2012      Los Alamos National Security, Inc.  All rights reserved.
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015-2020 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * Copyright (c) 2023      Triad National Security, LLC. All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */
#ifndef PMIX_PTL_BASE_H_
#define PMIX_PTL_BASE_H_

#include "src/include/pmix_config.h"

#ifdef HAVE_SYS_TIME_H
#    include <sys/time.h> /* for struct timeval */
#endif
#ifdef HAVE_STRING_H
#    include <string.h>
#endif

#include "src/class/pmix_pointer_array.h"
#include "src/mca/base/pmix_mca_base_framework.h"
#include "src/mca/mca.h"

#include "src/include/pmix_globals.h"
#include "src/include/pmix_stdatomic.h"
#include "src/mca/ptl/base/ptl_base_handshake.h"
#include "src/mca/ptl/ptl.h"

BEGIN_C_DECLS

/*
 * MCA Framework
 */
PMIX_EXPORT extern pmix_mca_base_framework_t pmix_ptl_base_framework;
/**
 * PTL select function
 *
 * Cycle across available components and construct the list
 * of active modules
 */
PMIX_EXPORT pmix_status_t pmix_ptl_base_select(void);

/* how many times a client or tool retries a connect() that fails - both
 * the blocking and the event-driven connect make this many retries after
 * the first attempt */
#define PMIX_MAX_RETRIES 10

/* framework globals */
struct pmix_ptl_base_t {
    bool initialized;
    bool selected;
    pmix_list_t posted_recvs; // list of pmix_ptl_posted_recv_t
    pmix_listener_t listener;
    /* when remote connections are accepted, a listener on each other
     * public interface the directives selected, and where they are:
     * a comma-delimited list of "tcp4://host:port"/"tcp6://host:port" */
    pmix_list_t alt_listeners;
    char *alt_uris;
    pmix_list_t pending_connections; // pmix_pending_connection_t still reading their connect-ack
    pmix_list_t connecting;          // pmix_ptl_connect_op_t: our own connects still under way
    struct sockaddr_storage *connection;
    size_t max_msg_size;
    char *session_tmpdir;
    char *system_tmpdir;
    char *report_uri;
    char *uri;
    char *urifile;
    char *sysctrlr_filename;
    char *scheduler_filename;
    char *system_filename;
    char *session_filename;
    char *nspace_filename;
    char *pid_filename;
    char *rendezvous_filename;
    bool created_rendezvous_dir;
    bool created_rendezvous_file;
    bool created_session_tmpdir;
    bool created_system_tmpdir;
    bool created_sysctrlr_filename;
    bool created_scheduler_filename;
    bool created_system_filename;
    bool created_session_filename;
    bool created_nspace_filename;
    bool created_pid_filename;
    bool created_urifile;
    bool remote_connections;
    bool connections_specified;
    bool system_tool;
    bool allow_foreign_tools;
    bool session_tool;
    bool tool_support;
    char *if_include;
    char *if_exclude;
    char **ipv4_ports;
    bool disable_ipv4_family;
    char **ipv6_ports;
    bool disable_ipv6_family;
    int max_retries;
    int wait_to_connect;
    int handshake_wait_time;
    int handshake_max_retries;
    /* seconds a server gives an incoming connection to deliver its whole
     * connect-ack, and bound on each blocking read of the handshake that
     * follows it - see pmix_ptl_base_connection_handler */
    int connect_ack_timeout;
    /* the most any one writev may carry - see send_msg. Not a tuning
     * parameter: it exists so a test can drive the chunking with a small
     * message instead of a 2 GB one */
    size_t max_write;
};
typedef struct pmix_ptl_base_t pmix_ptl_base_t;

PMIX_EXPORT extern pmix_ptl_base_t pmix_ptl_base;

/* Tags the line of a rendezvous or report-URI file that carries a server's
 * alternate addresses. The line follows every line a released reader takes
 * by position, and a reader finds it by this tag rather than by where it
 * falls, so more lines may yet follow or precede it. */
#define PMIX_PTL_ALT_URIS_TAG "alturis:"

typedef struct {
    pmix_list_item_t super;
    int sd;
    char *nspace;
    pmix_rank_t rank;
    char *uri;
    char *version;
    char *alt_uris;  // the server's other addresses, if its file listed any
} pmix_connection_t;
PMIX_EXPORT PMIX_CLASS_DECLARATION(pmix_connection_t);

/* API stubs */
PMIX_EXPORT pmix_status_t pmix_ptl_base_set_notification_cbfunc(pmix_ptl_cbfunc_t cbfunc);
/* Check, without acting on any of them, the directives
 * pmix_ptl_base_connect_to_peer consumes. PMIX_ERR_BAD_PARAM means one of
 * them is malformed - no connection could be attempted with it, so the
 * caller's PMIX_TOOL_CONNECT_OPTIONAL does not apply. */
PMIX_EXPORT pmix_status_t pmix_ptl_base_check_connect_directives(const pmix_info_t info[],
                                                                 size_t ninfo);

PMIX_EXPORT pmix_status_t pmix_ptl_base_connect_to_peer(struct pmix_peer_t *peer,
                                                        pmix_info_t info[], size_t ninfo,
                                                        char **suri);
/* The non-blocking form of pmix_ptl_base_connect_to_peer - see
 * pmix_ptl_connect_to_peer_nb_fn_t in ptl.h for the contract. Locating the
 * server is the same synchronous walk; everything from the connect() on is
 * driven by events on the progress thread. */
PMIX_EXPORT pmix_status_t pmix_ptl_base_connect_to_peer_nb(struct pmix_peer_t *peer,
                                                           pmix_info_t info[], size_t ninfo,
                                                           pmix_ptl_connect_nb_cbfunc_t cbfunc,
                                                           void *cbdata);
/* Start the event-driven connect to a located server. Takes ownership of
 * nspace, suri and iptr only when it returns PMIX_SUCCESS - after which
 * cbfunc is called exactly once, and never from inside this call. */
PMIX_EXPORT pmix_status_t pmix_ptl_base_start_connection(pmix_peer_t *peer, char *nspace,
                                                         pmix_rank_t rank, char *suri,
                                                         char *alt_uris,
                                                         pmix_info_t *iptr, size_t niptr,
                                                         pmix_ptl_connect_nb_cbfunc_t cbfunc,
                                                         void *cbdata);
/* Complete every connect still under way with PMIX_ERR_NOT_AVAILABLE.
 * Only for finalize, once the progress thread has stopped. */
PMIX_EXPORT void pmix_ptl_base_abandon_connects(void);
PMIX_EXPORT pmix_status_t pmix_ptl_base_parse_uri_file(char *filename,
                                                       bool optional,
                                                       pmix_list_t *connections);

PMIX_EXPORT pmix_status_t pmix_ptl_base_setup_connection(char *uri,
                                                         struct sockaddr_storage *connection,
                                                         size_t *len);

PMIX_EXPORT pmix_status_t pmix_ptl_base_create_listener(pmix_info_t info[], size_t ninfo);
PMIX_EXPORT void pmix_ptl_base_start_listening(void);
PMIX_EXPORT void pmix_ptl_base_stop_listening(void);
PMIX_EXPORT void pmix_ptl_base_drop_pending_connection(pmix_pending_connection_t *pnd);

/* base support functions */
/* Build the port array a listener scans from a list or range. NULL, an
 * unparseable value and the "-1" wildcard all yield the ephemeral port.
 * Any array already at *ports is freed first. */
PMIX_EXPORT pmix_status_t pmix_ptl_base_set_ports(const char *spec, char ***ports);
PMIX_EXPORT pmix_status_t pmix_ptl_base_setup_fork(const pmix_proc_t *proc, char ***env);
PMIX_EXPORT void pmix_ptl_base_send_handler(int sd, short flags, void *cbdata);

/* Drain everything already queued for this peer onto its socket before that
 * socket is closed in an orderly teardown.  Bounded and best-effort - see
 * the definition. */
PMIX_EXPORT void pmix_ptl_base_flush_sends(pmix_peer_t *peer);
PMIX_EXPORT void pmix_ptl_base_recv_handler(int sd, short flags, void *cbdata);
PMIX_EXPORT void pmix_ptl_base_process_msg(int fd, short flags, void *cbdata);
PMIX_EXPORT pmix_status_t pmix_ptl_base_set_nonblocking(int sd);
PMIX_EXPORT pmix_status_t pmix_ptl_base_set_blocking(int sd);
PMIX_EXPORT pmix_status_t pmix_ptl_base_send_blocking(int sd, char *ptr, size_t size);
PMIX_EXPORT pmix_status_t pmix_ptl_base_recv_blocking(int sd, char *data, size_t size);
PMIX_EXPORT pmix_status_t pmix_ptl_base_connect(struct sockaddr_storage *addr, pmix_socklen_t len,
                                                int *fd);
PMIX_EXPORT void pmix_ptl_base_connection_handler(int sd, short args, void *cbdata);
PMIX_EXPORT pmix_status_t pmix_ptl_base_setup_listener(pmix_info_t info[], size_t ninfo);
PMIX_EXPORT pmix_status_t pmix_ptl_base_send_connect_ack(int sd);
PMIX_EXPORT pmix_status_t pmix_ptl_base_recv_connect_ack(int sd);
PMIX_EXPORT bool pmix_ptl_base_peer_is_earlier(pmix_peer_t *peer, uint8_t major, uint8_t minor,
                                               uint8_t release);
PMIX_EXPORT void pmix_ptl_base_query_servers(int sd, short args, void *cbdata);
PMIX_EXPORT pmix_status_t pmix_ptl_base_parse_uri(const char *evar, char **nspace,
                                                  pmix_rank_t *rank, char **suri);
PMIX_EXPORT void pmix_ptl_base_parse_version(const char *vers, uint8_t *major,
                                             uint8_t *minor, uint8_t *release);
PMIX_EXPORT pmix_status_t pmix_ptl_base_df_search(char *dirname, char *prefix, pmix_info_t info[],
                                                  size_t ninfo, bool optional, pmix_list_t *connections);
PMIX_EXPORT pmix_rnd_flag_t pmix_ptl_base_set_flag(size_t *sz);
PMIX_EXPORT pmix_status_t pmix_ptl_base_make_connection(pmix_peer_t *peer, char *suri,
                                                        pmix_info_t *iptr, size_t niptr);
/* As pmix_ptl_base_make_connection, but if the address in *suri cannot be
 * reached, try each of the comma-delimited addresses in alt_uris in turn.
 * *suri is replaced with the address the connection was made to. */
PMIX_EXPORT pmix_status_t pmix_ptl_base_make_connection_alts(pmix_peer_t *peer, char **suri,
                                                             const char *alt_uris,
                                                             pmix_info_t *iptr, size_t niptr);
PMIX_EXPORT pmix_status_t pmix_ptl_base_complete_connection(pmix_peer_t *peer, char *nspace,
                                                            pmix_rank_t rank);
PMIX_EXPORT pmix_status_t pmix_ptl_base_set_timeout(pmix_peer_t *peer, struct timeval *save,
                                                    pmix_socklen_t *sz, bool *sockopt);
PMIX_EXPORT pmix_status_t pmix_ptl_base_client_handshake(pmix_peer_t *peer, pmix_status_t reply);
PMIX_EXPORT pmix_status_t pmix_ptl_base_tool_handshake(pmix_peer_t *peer, pmix_status_t rp);
PMIX_EXPORT char **pmix_ptl_base_split_and_resolve(const char *orig_str,
                                                   const char *name);
PMIX_EXPORT pmix_status_t pmix_ptl_base_set_peer(pmix_peer_t *peer, char **evar);
PMIX_EXPORT char *pmix_ptl_base_get_cmd_line(void);

PMIX_EXPORT char *pmix_ptl_base_peer_type(pmix_peer_t *peer);

END_C_DECLS

#endif
