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
#ifdef HAVE_SYS_STAT_H
#    include <sys/stat.h> /* for struct stat */
#endif

#include "src/class/pmix_pointer_array.h"
#include "src/mca/base/pmix_mca_base_framework.h"
#include "src/mca/mca.h"

#include "src/include/pmix_globals.h"
#include "src/include/pmix_stdatomic.h"
#include "src/mca/psec/psec.h"
#include "src/mca/ptl/base/ptl_base_handshake.h"
#include "src/mca/ptl/ptl.h"
#include "src/util/pmix_fd.h"

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

/* A directory this process made, held so that finalize removes it from
 * wherever it has come to be: a descriptor on the directory holding it,
 * its name there, one on the directory itself, and what that is. parentfd
 * is -1 when there is nothing to remove - we did not make it, or could
 * not hold it */
typedef struct {
    bool created;
    int parentfd;
    int fd;
    char *name;
    char *path;        // where it was made, for messages
    struct stat st;
} pmix_ptl_made_dir_t;
#define PMIX_PTL_MADE_DIR_STATIC_INIT {.created = false, .parentfd = -1, .fd = -1, .name = NULL, .path = NULL}

/* a transport in use, on pmix_ptl_base.actives in priority order */
typedef struct {
    pmix_list_item_t super;
    int priority;
    pmix_ptl_base_component_t *component;
    pmix_ptl_module_t *module;
} pmix_ptl_base_active_t;
PMIX_CLASS_DECLARATION(pmix_ptl_base_active_t);

/* framework globals */
struct pmix_ptl_base_t {
    bool initialized;
    bool selected;
    pmix_list_t actives;      // pmix_ptl_base_active_t, highest priority first
    pmix_list_t posted_recvs; // list of pmix_ptl_posted_recv_t
    pmix_listener_t listener;
    /* the local transport's listener - the one every process of this
     * release on this host connects to - and the effective uid and gid it
     * was opened as, which the kernel reports to whoever connects */
    pmix_listener_t local_listener;
    uid_t local_uid;
    gid_t local_gid;
    /* Who the server a connection reaches on the local transport must be,
     * as the kernel reports it: the uid and gid the server that launched
     * us named, when it named them - else our own uid, root, or one that
     * ptl_base_server_access allows. See pmix_ptl_base_check_server */
    bool server_expect_given;
    uid_t server_expect_uid;
    gid_t server_expect_gid;
    /* whether a release before this one, if that is what the server's
     * version says, may be believed of it - see
     * pmix_ptl_base_check_server. Set with that version, for one connect */
    bool server_release_vouched;
    char *server_access;
    uid_t *access_uids;
    size_t naccess_uids;
    gid_t *access_gids;
    size_t naccess_gids;
    /* why the last blocking connect failed, for saying so */
    int last_connect_errno;
    /* listeners beyond the primary one, and where they are: today, when
     * remote connections are accepted, tcp's on each other public
     * interface the directives selected - a comma-delimited list of
     * "tcp4://host:port"/"tcp6://host:port" */
    pmix_list_t alt_listeners;
    char *alt_uris;
    pmix_list_t pending_connections; // pmix_pending_connection_t still reading their connect-ack
    /* The host decides on one tool of a name at a time: a tool whose
     * identity is settled waits here (pmix_pending_connection_t), in the
     * order they arrived, while the host decides on a tool of its name -
     * one on tool_asking, which holds every tool the host is deciding on.
     * A tool that needs an identity has no name, and never waits */
    pmix_list_t tool_queue;
    pmix_list_t tool_asking;
    /* tools the server stopped waiting on the host for, whose answer has
     * yet to arrive - their names are held until it does */
    pmix_list_t tool_late;
    /* the connection tokens PMIx_server_setup_fork gave out, by namespace
     * and rank - see ptl_base_token.c */
    pmix_list_t tokens;
    pmix_list_t connecting;          // pmix_ptl_connect_op_t: our own connects still under way
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
    /* the directories we made for the rendezvous file and in place of
     * the system tmpdir, if we made them. The session tmpdir is never
     * ours: the local socket is made in it before any file is written,
     * so it must already exist */
    pmix_ptl_made_dir_t rendezvous_dir;
    pmix_ptl_made_dir_t system_dir;
    bool created_rendezvous_file;
    bool created_sysctrlr_filename;
    bool created_scheduler_filename;
    bool created_system_filename;
    bool created_session_filename;
    bool created_nspace_filename;
    bool created_pid_filename;
    bool created_urifile;
    /* a descriptor on the directory holding each rendezvous file we
     * created, so the file is removed from the directory it was created
     * in rather than wherever its name leads by then. -1 when none */
    int rendezvous_dirfd;
    int sysctrlr_dirfd;
    int scheduler_dirfd;
    int system_dirfd;
    int session_dirfd;
    int nspace_dirfd;
    int pid_dirfd;
    /* the same for the report file, the file it was when we wrote it, and
     * a descriptor on that file, which keeps its identity from passing to
     * another */
    int urifile_dirfd;
    int urifile_fd;
    struct stat urifile_st;
    /* the rendezvous files we wrote - the name, and the file it was when we
     * wrote it, which pmix_ptl_close removes only while it still is - and
     * the lock each holds beside it while we run, if it took one (-1 if
     * not): see PMIX_PTL_LOCK_TAG. One for each of the names above */
#define PMIX_PTL_NLOCKS 8
    int rndz_lockfd[PMIX_PTL_NLOCKS];
    char *rndz_lockname[PMIX_PTL_NLOCKS];
    dev_t rndz_dev[PMIX_PTL_NLOCKS];
    ino_t rndz_ino[PMIX_PTL_NLOCKS];
    size_t nrndz_lockfd;
    bool remote_connections;
    bool connections_specified;
    /* whether clients and tools of earlier releases may connect: the
     * ptl_base_legacy_peers parameter's value, and what this server
     * settled on, which PMIX_SERVER_LEGACY_PEERS can change */
    bool legacy_peers_param;
    bool legacy_peers;
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
    /* how many directories below the system tmpdir a search for a
     * server's rendezvous file descends - see pmix_ptl_base_df_search */
    int search_depth;
    /* seconds a server gives an incoming connection to deliver its whole
     * connect-ack - see pmix_ptl_base_connection_handler */
    int connect_ack_timeout;
    /* the most connections pending_connections may hold, and separately
     * the most tools that may wait on the host (tool_queue and
     * tool_asking), as the max_pending_connections parameter gives it,
     * and as pmix_ptl_base_start_listening works it out from that - 0
     * there means no limit. See connection_event_handler */
    int max_pending_connections;
    int pending_cap;
    /* the most any one writev may carry - see send_msg. Not a tuning
     * parameter: it exists so a test can drive the chunking with a small
     * message instead of a 2 GB one */
    size_t max_write;
};
typedef struct pmix_ptl_base_t pmix_ptl_base_t;

/* The token PMIx_server_setup_fork gives a rank: this many random bytes,
 * in hex in the process's environment under PMIX_PTL_TOKEN_ENV - after the
 * name of the server that made it and a ';' - and sent back in the
 * connect-ack's info under PMIX_PTL_TOKEN_KEY */
#define PMIX_PTL_TOKEN_LEN 16
#define PMIX_PTL_TOKEN_ENV "PMIX_CONNECT_TOKEN"
#define PMIX_PTL_TOKEN_KEY "pmix.ptl.token"

/* The first major release whose clients and tools reach a server on their
 * own host over the local transport. A peer of an earlier one connects
 * over TCP, and is taken only when the server takes legacy peers */
#define PMIX_PTL_LOCAL_MAJOR 7

PMIX_EXPORT extern pmix_ptl_base_t pmix_ptl_base;

/* Tags the line of a rendezvous or report-URI file that carries a server's
 * alternate addresses. The line follows every line a released reader takes
 * by position, and a reader finds it by this tag rather than by where it
 * falls, so more lines may yet follow or precede it. */
#define PMIX_PTL_ALT_URIS_TAG "alturis:"

/* Tags the line of the same files that carries the server's local
 * transport's URI - the one a process of this release on this host
 * connects to. The first line keeps the address older readers take */
#define PMIX_PTL_LOCAL_URI_TAG "local:"

/* The variable a server puts the same URI in for the processes it starts,
 * followed by ";<uid>:<gid>" - the identity the kernel must report for the
 * server at the far end. No released reader builds or reads this name */
#define PMIX_PTL_LOCAL_URI_ENV "PMIX_SERVER_LOCAL_URI"

/* Build the value of PMIX_SERVER_LOCAL_URI for our local listener, or
 * NULL if there is none */
PMIX_EXPORT char *pmix_ptl_base_local_uri_value(void);

/* Split a PMIX_SERVER_LOCAL_URI value into the URI and the uid and gid the
 * server is to be. PMIX_ERR_BAD_PARAM if it is not that shape */
PMIX_EXPORT pmix_status_t pmix_ptl_base_parse_local_uri(const char *value, char **uri,
                                                        uid_t *uid, gid_t *gid);

/* If the server that started us named its local transport's address, in
 * PMIX_SERVER_LOCAL_URI, give it in *uri (ours to free) and expect the
 * identity it named - PMIX_ERR_NOT_FOUND if it named none. A value that
 * cannot be read is reported, and PMIX_ERR_SILENT returned */
PMIX_EXPORT pmix_status_t pmix_ptl_base_use_local_env(const char *server, char **uri);

/* Whether the server at the far end of the peer's connection is one we
 * may talk to, by what the kernel reports for it - see server_expect_*
 * above. A server that may not be talked to is reported, and
 * PMIX_ERR_INVALID_CRED returned.
 *
 * A tool connected over TCP to a server on this host, on a platform with
 * no table of TCP connections to ask, gets *prove set: the server is to
 * prove who it is, as one on another host does (pmix_ptl_base_auth_server),
 * and PMIX_ERR_NOT_SUPPORTED, reported, if the tool has no mechanism that
 * can. A server whose version is known to be of an earlier release has no
 * such exchange, and passes - when that may be believed (`vouched`: see
 * server_release_vouched). So does a client's server, and a server at an
 * address that is not this host's - see pmix_ptl_base_server_remote */
PMIX_EXPORT pmix_status_t pmix_ptl_base_check_server(pmix_peer_t *peer, bool given, uid_t uid,
                                                     gid_t gid, bool vouched, bool *prove);

/* Resolve the ptl_base_server_access value - "users=a:b,groups=c:d",
 * names or numbers - into pmix_ptl_base's lists. Reports and refuses an
 * unknown name, an empty list, or a directive unknown or given twice */
PMIX_EXPORT pmix_status_t pmix_ptl_base_parse_server_access(const char *spec);

/* Tag the lines of the same files that list the wire formats (bfrops
 * components) and security mechanisms (psec components) the server can
 * accept a connection with, comma-delimited in its priority order. A
 * process that finds a server through a file - one the server did not
 * start, so no environment tells it what to use - takes the highest of
 * its own that the server lists. */
#define PMIX_PTL_BFROPS_TAG "bfrops:"
#define PMIX_PTL_PSEC_TAG   "psec:"

/* Tags the line of a rendezvous file whose server holds a lock (flock)
 * for as long as it runs on a file of its own beside it - the name with a
 * '.' before it and PMIX_PTL_LOCK_SUFFIX after (pmix_ptl_base_lock_name),
 * which no search for "pmix.*" files matches, mode 0600: "lock:flock". A process that can take
 * a lock on that file knows the server is gone - whatever pid namespace,
 * container or jail it ran in, which a pid cannot say. Only the server's
 * user can open it, so no other can hold the lock in its place. A file
 * without the line, from an earlier release, says nothing of the kind */
#define PMIX_PTL_LOCK_TAG    "lock:"
#define PMIX_PTL_LOCK_FLOCK  "flock"
#define PMIX_PTL_LOCK_SUFFIX ".lock"

/* The most a reader takes from a contact file it found rather than was
 * given: one larger is not a server's, and no more lines than these are
 * read past the ones taken by position */
#define PMIX_PTL_MAX_CONN_FILE  (64 * 1024)
#define PMIX_PTL_MAX_TAG_LINES  64

/* A line of a contact file, as pmix_getline reads one - NULL at the end,
 * on a read error or a NUL byte - taken from what is left of *budget
 * bytes: NULL once it is spent, however the file grows while it is read.
 * A last line without a newline that ends exactly as the budget does is
 * dropped; no server writes one. Set *budget to PMIX_PTL_MAX_CONN_FILE
 * before the first. Caller frees */
PMIX_EXPORT char *pmix_ptl_base_getline(FILE *fp, size_t *budget);

/* What the lock beside the rendezvous file `name` (relative to dirfd, or
 * AT_FDCWD), owned by `owner`, says of its server, when its lock line's
 * value (`lock`, NULL if it has none) says it holds one: GONE when
 * nothing holds it, HELD when something does, UNKNOWN when it says
 * nothing - no such line, a lock file this process holds itself (its own
 * server's, which is not opened), one it cannot open or that is not the
 * server's, or a lock it cannot take for another reason */
typedef enum {
    PMIX_PTL_LOCK_UNKNOWN,
    PMIX_PTL_LOCK_GONE,
    PMIX_PTL_LOCK_HELD
} pmix_ptl_lock_state_t;
PMIX_EXPORT pmix_ptl_lock_state_t pmix_ptl_base_lock_state(int dirfd, const char *name,
                                                          uid_t owner, const char *lock);

/* Whether that says the server is gone */
PMIX_EXPORT bool pmix_ptl_base_server_gone(int dirfd, const char *name, uid_t owner,
                                           const char *lock);

/* The lock file's name for a rendezvous file's - a path, or a name alone.
 * Caller frees; NULL if out of memory */
PMIX_EXPORT char *pmix_ptl_base_lock_name(const char *name);

/* Take and hold the lock file for the rendezvous file `name` in dirfd:
 * our own, created 0600 if it is not there. The descriptor;
 * PMIX_PTL_LOCK_BUSY when another server holds it - a reader's moment
 * with it is waited out; or -1 when the name is not ours to use, or the
 * file system does not lock, in which case a lock file this call made,
 * or one of ours there already, is removed */
#define PMIX_PTL_LOCK_BUSY -2
PMIX_EXPORT int pmix_ptl_base_take_lock(int dirfd, const char *name);

typedef struct {
    pmix_list_item_t super;
    int sd;
    char *nspace;
    pmix_rank_t rank;
    char *uri;
    char *version;
    char *alt_uris;  // the server's other addresses, if its file listed any
    char *local_uri; // the server's local transport's address, if its file listed one
    char *bfrops;    // the server's wire formats, if its file listed them
    char *psec;      // the server's security mechanisms, if its file listed them
    bool vouched;    // an earlier release the version names may be believed
} pmix_connection_t;
PMIX_EXPORT PMIX_CLASS_DECLARATION(pmix_connection_t);

/* Choose the wire format and security mechanism `peer` - a server found
 * through `cn` - will be spoken to with: the highest of ours that the
 * server lists (our own mechanism if it is listed). A server whose file
 * lists none (an older release) is matched by its version for the wire
 * format, which assumes it runs every format named for a release up to
 * its own, and gets munge if we have it, else native - never ssl, which
 * no older server has. Returns PMIX_ERR_NOT_SUPPORTED when the two have
 * nothing in common, so the connection is not attempted.
 * For our primary server our own modules change with the server's, as a
 * client's do when its server's environment names them. */
PMIX_EXPORT pmix_status_t pmix_ptl_base_select_compat(struct pmix_peer_t *peer,
                                                     pmix_connection_t *cn);

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
/* A client's connect_to_peer - see ptl_base_client.c */
PMIX_EXPORT pmix_status_t pmix_ptl_base_client_connect_to_peer(struct pmix_peer_t *peer,
                                                               pmix_info_t info[], size_t ninfo,
                                                               char **suri);
/* A tool's setup_listener - see ptl_base_tool.c */
PMIX_EXPORT pmix_status_t pmix_ptl_base_tool_setup_listener(pmix_info_t info[], size_t ninfo);
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
/* As pmix_ptl_base_parse_uri_file, for a file at one of the fixed
 * rendezvous names (pmix.sys.<host>, pmix.sched.<host>,
 * pmix.sysctrlr.<host>): the file is used only if it is a regular file */
PMIX_EXPORT pmix_status_t pmix_ptl_base_parse_rndz_file(char *filename,
                                                        bool optional,
                                                        pmix_list_t *connections);

/* The address a URI names, from the transport whose scheme it has.
 * PMIX_ERR_NOT_SUPPORTED if no transport in use speaks it */
PMIX_EXPORT pmix_status_t pmix_ptl_base_setup_connection(char *uri,
                                                         struct sockaddr_storage *connection,
                                                         size_t *len);
/* The same, also giving the transport whose scheme the URI has, when
 * transport is not NULL */
PMIX_EXPORT pmix_status_t pmix_ptl_base_uri_to_addr(const char *uri,
                                                    struct sockaddr_storage *connection,
                                                    size_t *len, pmix_ptl_module_t **transport);
/* The tools holding a connection while they wait on the host: queued for
 * it, or being decided on */
PMIX_EXPORT size_t pmix_ptl_base_waiting_tools(void);
/* Give proc a token: make one, keep it, and put it in *env - or, for a
 * process with no rank of its own, take any it inherited out of *env */
PMIX_EXPORT pmix_status_t pmix_ptl_base_make_token(const pmix_proc_t *proc, char ***env);
/* Give a registered rank that has no token one that nobody is handed, so
 * that until it is prepared nothing is taken for it */
PMIX_EXPORT pmix_status_t pmix_ptl_base_reserve_token(const pmix_proc_t *proc);
/* Whether `given` is the token kept for the rank: PMIX_SUCCESS if it is,
 * or if the rank has none - it was neither registered nor prepared;
 * PMIX_ERR_INVALID_CRED if not */
PMIX_EXPORT pmix_status_t pmix_ptl_base_check_token(const char *nspace, pmix_rank_t rank,
                                                    const char *given);
/* Zero a token held as a string, or every token in an info array, before
 * it is freed */
PMIX_EXPORT void pmix_ptl_base_wipe(void *p, size_t len);
PMIX_EXPORT void pmix_ptl_base_wipe_token(char *s);
PMIX_EXPORT void pmix_ptl_base_wipe_token_info(pmix_info_t *info, size_t ninfo);
/* Forget the rank's token - every rank's, for PMIX_RANK_WILDCARD */
PMIX_EXPORT void pmix_ptl_base_drop_tokens(const char *nspace, pmix_rank_t rank);
/* Add the token in our environment to a connect-ack's info - replacing
 * *info and *ninfo - if it is the server nspace.rank's */
PMIX_EXPORT pmix_status_t pmix_ptl_base_add_token(pmix_info_t **info, size_t *ninfo,
                                                  const char *nspace, pmix_rank_t rank);
/* Whether a peer's address is this host's: a loopback address, or the
 * address of one of this host's interfaces */
PMIX_EXPORT bool pmix_ptl_base_addr_on_host(const struct sockaddr_storage *addr);

/****    BETWEEN HOSTS    ****
 *
 * A tool and a server on different hosts authenticate each other, the
 * server first, before the tool sends its connect-ack. The tool opens with
 * a short message in the connect-ack's framing - the security mechanism
 * it means to use, no credential, the connector flag PMIX_REMOTE_AUTH, its
 * version, a nonce of its own, its uid, and a byte that says whether it
 * binds the connection too (below; anything after that is a later
 * release's, and ignored). The server answers with a status and,
 * on success, a nonce of its own and a credential bound to both
 * (create_bound, as a server). The tool checks that credential, and who
 * it says the server is, and only then sends its connect-ack - whose
 * credential is bound to the same two nonces, as a tool. The server takes
 * no peer on another host that has not been through this, whatever its
 * release or mechanism. See ptl_base_auth.c. Each nonce is
 * PMIX_PTL_AUTH_NONCE_LEN (ptl_types.h) bytes.
 *
 * A tool runs the same exchange with a TCP server on its own host when
 * its kernel cannot name the server's user - see
 * pmix_ptl_base_check_server. Its opening says, after its uid, that it
 * binds both credentials to the connection's two ends as well, which each
 * end reads from its own socket: a connection relayed through a third
 * process has other ends on each side. A server that takes tools answers
 * such an opening from a peer at an address of its host whether or not
 * it takes tools from others - where its own kernel cannot name its peers
 * either, as it cannot when the tool's cannot */

/* The bytes a credential made for one such connection is bound to: whose
 * it is, the tool's nonce and the server's - and, when both ends are on
 * one host, the connection's two ends (`conn`, from
 * pmix_ptl_base_auth_conn; NULL otherwise). Caller frees bind->bytes */
PMIX_EXPORT pmix_status_t pmix_ptl_base_auth_bind(bool server, const unsigned char *tool,
                                                  const unsigned char *srv, const char *conn,
                                                  pmix_byte_object_t *bind);

/* The two ends of the TCP connection on sd, the tool's first, as both
 * ends see them when nothing stands between them: "addr,port;addr,port".
 * `at_server` is which end sd is. An IPv4 end seen v4-mapped is written
 * as IPv4 */
#define PMIX_PTL_AUTH_CONN_LEN 128
PMIX_EXPORT pmix_status_t pmix_ptl_base_auth_conn(int sd, bool at_server,
                                                  char conn[PMIX_PTL_AUTH_CONN_LEN]);

/* Whether a peer at this TCP address is on another host */
PMIX_EXPORT bool pmix_ptl_base_tcp_peer_remote(const struct sockaddr_storage *addr);

/* Whether an interface of this host holds the address now, by the kernel's
 * own list (getifaddrs) - which names some the interface list made at init
 * leaves out: an address added since, a secondary address on Linux, a
 * point-to-point or link-local one on BSD. It decides only whether a tool
 * and a server bind their connection (see "Between hosts" below): an
 * address the kernel holds may still lead elsewhere, as a Kubernetes
 * service address does, so it is not "this host" for anything else */
PMIX_EXPORT bool pmix_ptl_base_addr_held(const struct sockaddr_storage *addr);

/* ...that, or one on the interface list, for a TCP peer - and never with
 * pmix_ptl_base_test_remote set */
PMIX_EXPORT bool pmix_ptl_base_tcp_peer_held(const struct sockaddr_storage *addr);

/* Whether the server at the far end of this socket is on another host:
 * a TCP connection whose far end is not one of this host's addresses */
PMIX_EXPORT bool pmix_ptl_base_server_remote(pmix_peer_t *peer);

/* The tool's half, run once the socket to a server on another host is
 * connected and before anything else is sent on it: choose the mechanism,
 * have the server prove who it is, and check that it is a server we may
 * talk to (see pmix_ptl_base_check_ids). On success *bind holds the bytes
 * the tool's own credential is to be bound to - ours to free - and
 * *srvuid the user the server proved it runs as, whom it is for. `local`
 * is a server on this host the kernel cannot identify, which a failure
 * is reported as */
PMIX_EXPORT pmix_status_t pmix_ptl_base_auth_server(pmix_peer_t *peer, bool given, uid_t uid,
                                                    gid_t gid, bool local,
                                                    pmix_byte_object_t *bind, uid_t *srvuid);

/* The opening message for a server on another host, in *msg (ours to
 * free) - see above. `bound`: we bind the connection's two ends into the
 * credentials as well, as a tool on the server's host does */
PMIX_EXPORT pmix_status_t pmix_ptl_base_auth_request(const char *psec, const unsigned char *nonce,
                                                     bool bound, char **msg, size_t *len);

/* Check what a server on another host sent back for our opening: its
 * credential, bound to our nonce and its - and, `local`, to the
 * connection - and the identity it proves. On success *bind holds the
 * bytes our own credential is to be bound to */
PMIX_EXPORT pmix_status_t pmix_ptl_base_auth_reply(pmix_peer_t *peer, bool given, uid_t uid,
                                                   gid_t gid, bool local,
                                                   const unsigned char *tool,
                                                   const unsigned char *srv,
                                                   const pmix_byte_object_t *cred,
                                                   pmix_byte_object_t *bind, uid_t *srvuid);

/* Report that a server on another host - or, `local`, one on this host
 * the kernel cannot identify - would not prove who it is, as a server of
 * an earlier release, or one that takes no tools from other hosts, will
 * not: with `why`, its answer or our failure to read one */
PMIX_EXPORT void pmix_ptl_base_auth_refused(pmix_peer_t *peer, const char *psec, bool local,
                                            pmix_status_t why);

/* The security mechanism for a server on another host: the one chosen
 * for it, if that can, else the highest of ours that can - set on the
 * server's peer. Reported, and NULL, if none can */
PMIX_EXPORT pmix_psec_module_t *pmix_ptl_base_auth_module(pmix_peer_t *peer);

/* Whether a server that is uid and gid - by what the kernel reports, or,
 * on another host (`remote`), by what its credential proves - is one we
 * may talk to: the identity we were given for it, if we were, else our
 * own user, root, or one ptl_base_server_access allows. Reported, and
 * PMIX_ERR_INVALID_CRED returned, if not */
PMIX_EXPORT pmix_status_t pmix_ptl_base_check_ids(uid_t kuid, gid_t kgid, bool given, uid_t uid,
                                                  gid_t gid, bool remote);

/* A status a server sent in a reply: one a connect uses to say how it is
 * going never comes from a server, and is refused; a server asking for a
 * security handshake is refused as this library runs none */
PMIX_EXPORT pmix_status_t pmix_ptl_base_server_status(uint32_t u32);

/* For the unit tests alone: treat every peer of a TCP connection as on
 * another host. Nothing outside a test's own code sets it */
PMIX_EXPORT extern bool pmix_ptl_base_test_remote;

/* For the unit tests alone: take this platform to have no table of TCP
 * connections, so the kernel names no TCP server's user. Nothing outside
 * a test's own code sets it */
PMIX_EXPORT extern bool pmix_ptl_base_test_no_tcp_table;

/* Every socket PMIx creates or accepts is the library's alone: it is set
 * close-on-exec, so a program the process execs - or a child it forks
 * and execs - does not inherit a connection it cannot use, and keep it
 * open after the process that made it is gone; and it is given
 * pmix_ptl_base_set_nosigpipe() */
PMIX_EXPORT void pmix_ptl_base_own_socket(int sd);

/* Have the local transport open lt's socket and make its URI */
PMIX_EXPORT pmix_status_t pmix_ptl_base_listen_local(pmix_info_t info[], size_t ninfo,
                                                     pmix_listener_t *lt);
/* Have the highest-priority transport that listens open lt's socket and
 * make its URI */
PMIX_EXPORT pmix_status_t pmix_ptl_base_listen(pmix_info_t info[], size_t ninfo,
                                               pmix_listener_t *lt);
/* Replace a string global from a directive, which must be a non-NULL
 * PMIX_STRING. Returns false, with *rc set, if it cannot be used */
PMIX_EXPORT bool pmix_ptl_base_replace_string(const pmix_info_t *info, char **target, int *rc);

PMIX_EXPORT pmix_status_t pmix_ptl_base_create_listener(pmix_info_t info[], size_t ninfo);
PMIX_EXPORT void pmix_ptl_base_start_listening(void);
PMIX_EXPORT void pmix_ptl_base_stop_listening(void);
PMIX_EXPORT void pmix_ptl_base_drop_pending_connection(pmix_pending_connection_t *pnd,
                                                       pmix_status_t why);
/* Wait, without blocking, for a connection's connect-ack: put it on the
 * pending list, arm the connect-ack timer and the read event. For one just
 * accepted; and, with `resume`, for a peer on another host whose server
 * has just proved who it is - which gets only what is left of the timeout
 * that began when it arrived, and its place among the others by arrival */
PMIX_EXPORT void pmix_ptl_base_await_connect_ack(pmix_pending_connection_t *pnd, bool resume);
/* What the listener does when accept() on lt fails with err: nothing for an
 * error that concerns only the connection, a pause and a retry a second
 * later for anything else, and a stop once the retries of an error that
 * does not clear on its own run out. Runs on the progress thread */
PMIX_EXPORT void pmix_ptl_base_accept_failed(pmix_listener_t *lt, int err);

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
/* If a peer's socket has closed, or been reset, before its receive handler
 * has run to see it, handle the loss now, as the handler would, and return
 * true - the peer may have been released. A socket with unread data on it,
 * or one that reports any other error, is live: returns false. Progress
 * thread only */
PMIX_EXPORT bool pmix_ptl_base_reap_if_closed(pmix_peer_t *peer);
/* Whether the other end of a socket has closed it, or reset it - looked at
 * without taking anything from it. Unread data, and any other error, is a
 * socket still open. A blocking socket is not looked at where the look
 * could wait: false */
PMIX_EXPORT bool pmix_ptl_base_sd_closed(int sd);
/* The time now, on a clock that only goes forward where there is one */
PMIX_EXPORT void pmix_ptl_base_monotime(struct timeval *tv);
/* Tell the host that `proc`, which it accepted, is not connected: the
 * PMIX_ERR_LOST_CONNECTION it is given for any process that leaves
 * without finalizing. `cbfunc`, if given, is called on the progress
 * thread once the host's handlers have been given the notice - or at once
 * if it could not be raised. Progress thread only */
PMIX_EXPORT void pmix_ptl_base_report_not_connected(const pmix_proc_t *proc,
                                                    pmix_op_cbfunc_t cbfunc, void *cbdata);
PMIX_EXPORT void pmix_ptl_base_process_msg(int fd, short flags, void *cbdata);
PMIX_EXPORT pmix_status_t pmix_ptl_base_set_nonblocking(int sd);
PMIX_EXPORT pmix_status_t pmix_ptl_base_set_blocking(int sd);

/* A write to a socket whose peer has reset raises SIGPIPE, and its
 * default action ends the process - the host's or the application's, not
 * a library's to decide. So nothing PMIx writes may raise it: every send
 * passes PMIX_PTL_NOSIGNAL, and every socket PMIx creates or accepts is
 * given pmix_ptl_base_set_nosigpipe(), for a platform whose sends take no
 * such flag. */
#ifdef MSG_NOSIGNAL
#    define PMIX_PTL_NOSIGNAL MSG_NOSIGNAL
#else
#    define PMIX_PTL_NOSIGNAL 0
#endif
PMIX_EXPORT void pmix_ptl_base_set_nosigpipe(int sd);
/* writev() that cannot raise SIGPIPE */
PMIX_EXPORT ssize_t pmix_ptl_base_writev(int sd, struct iovec *iov, int iovcnt);
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
/* Search dirname and the directories below it for rendezvous files. With
 * exact, a file's name must be the whole of prefix; otherwise it need
 * only begin with it. */
PMIX_EXPORT pmix_status_t pmix_ptl_base_df_search(char *dirname, char *prefix, bool exact,
                                                  pmix_info_t info[], size_t ninfo, bool optional,
                                                  pmix_list_t *connections);
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
PMIX_EXPORT pmix_status_t pmix_ptl_base_set_peer(pmix_peer_t *peer, char **evar);
PMIX_EXPORT char *pmix_ptl_base_get_cmd_line(void);

PMIX_EXPORT char *pmix_ptl_base_peer_type(pmix_peer_t *peer);

END_C_DECLS

#endif
