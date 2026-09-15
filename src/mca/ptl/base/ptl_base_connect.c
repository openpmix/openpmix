/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2006 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "src/include/pmix_config.h"
#include "include/pmix_stdint.h"

#ifdef HAVE_FCNTL_H
#    include <fcntl.h>
#endif
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif
#ifdef HAVE_SYS_SOCKET_H
#    include <sys/socket.h>
#endif
#ifdef HAVE_SYS_UIO_H
#    include <sys/uio.h>
#endif
#ifdef HAVE_SYS_TYPES_H
#    include <sys/types.h>
#endif
#ifdef HAVE_SYS_STAT_H
#    include <sys/stat.h>
#endif
#ifdef HAVE_SYS_SELECT_H
#    include <sys/select.h>
#endif
#ifdef HAVE_DIRENT_H
#    include <dirent.h>
#endif
#if OAC_HAVE_APPLE && defined(HAVE_SYS_SYSCTL_H)
#    include <sys/sysctl.h>
#endif

#include "src/client/pmix_client_ops.h"
#include "src/common/pmix_attributes.h"
#include "src/include/pmix_globals.h"
#include "src/include/pmix_socket_errno.h"
#include "src/mca/bfrops/base/base.h"
#include "src/mca/gds/gds.h"
#include "src/server/pmix_server_ops.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_getid.h"
#include "src/util/pmix_os_path.h"
#include "src/util/pmix_printf.h"
#include "src/util/pmix_show_help.h"
#include "src/util/pmix_strnlen.h"

#include "src/mca/ptl/base/base.h"

pmix_status_t pmix_ptl_base_set_nonblocking(int sd)
{
    int flags;
    /* setup the socket as non-blocking */
    flags = fcntl(sd, F_GETFL, 0);
    if (0 > flags) {
        pmix_output(0, "ptl:base:set_nonblocking: fcntl(F_GETFL) failed: %s (%d)\n",
                    strerror(pmix_socket_errno), pmix_socket_errno);
    } else {
        flags |= O_NONBLOCK;
        if (0 > fcntl(sd, F_SETFL, flags)) {
            pmix_output(0, "ptl:base:set_nonblocking: fcntl(F_SETFL) failed: %s (%d)\n",
                        strerror(pmix_socket_errno), pmix_socket_errno);
        }
    }
    return PMIX_SUCCESS;
}

pmix_status_t pmix_ptl_base_set_blocking(int sd)
{
    int flags;
    /* setup the socket as blocking */
    flags = fcntl(sd, F_GETFL, 0);
    if (0 > flags) {
        pmix_output(0, "ptl:base:set_blocking: fcntl(F_GETFL) failed: %s (%d)\n",
                    strerror(pmix_socket_errno), pmix_socket_errno);
    } else {
        flags &= ~(O_NONBLOCK);
        if (0 > fcntl(sd, F_SETFL, flags)) {
            pmix_output(0, "ptl:base:set_blocking: fcntl(F_SETFL) failed: %s (%d)\n",
                        strerror(pmix_socket_errno), pmix_socket_errno);
        }
    }
    return PMIX_SUCCESS;
}

/*
 * A blocking send. Used to send the small amount of connection
 * information that identifies the peers endpoint. Every caller hands this
 * a socket in blocking mode, and none sets SO_SNDTIMEO, so EAGAIN cannot
 * occur on it - the cycle below is only for a socket that is genuinely
 * non-blocking.
 */
pmix_status_t pmix_ptl_base_send_blocking(int sd, char *ptr, size_t size)
{
    size_t cnt = 0;
    ssize_t retval;

    pmix_output_verbose(8, pmix_ptl_base_framework.framework_output,
                        "send blocking of %" PRIsize_t " bytes to socket %d", size, sd);
    while (cnt < size) {
        retval = send(sd, (char *) ptr + cnt, size - cnt, 0);
        if (0 > retval) {
            if (EAGAIN == pmix_socket_errno || EWOULDBLOCK == pmix_socket_errno) {
                /* just cycle and let it try again */
                pmix_output_verbose(8, pmix_ptl_base_framework.framework_output,
                                    "blocking_send received error %d:%s from remote - cycling",
                                    pmix_socket_errno, strerror(pmix_socket_errno));
                continue;
            }
            if (EINTR != pmix_socket_errno) {
                pmix_output_verbose(
                    8, pmix_ptl_base_framework.framework_output,
                    "ptl:base:peer_send_blocking: send() to socket %d failed: %s (%d)\n", sd,
                    strerror(pmix_socket_errno), pmix_socket_errno);
                return PMIX_ERR_UNREACH;
            }
            continue;
        }
        cnt += retval;
    }

    pmix_output_verbose(8, pmix_ptl_base_framework.framework_output,
                        "blocking send complete to socket %d", sd);
    return PMIX_SUCCESS;
}

/*
 * A blocking recv. Used to receive the small amount of connection
 * information that identifies the peers endpoint.
 *
 * Every caller hands this a socket in blocking mode, and on such a socket
 * EAGAIN/EWOULDBLOCK does not mean "no data yet": it is how recv() reports
 * that an SO_RCVTIMEO expired. That is the only way a caller can bound the
 * wait, so it has to come back as PMIX_ERR_TIMEOUT - retrying it silently
 * turned every receive timeout into an unbounded wait. A socket that is
 * genuinely non-blocking still cycles, as it always did.
 */
pmix_status_t pmix_ptl_base_recv_blocking(int sd, char *data, size_t size)
{
    size_t cnt = 0;
    int flags;

    pmix_output_verbose(8, pmix_ptl_base_framework.framework_output,
                        "waiting for blocking recv of %" PRIsize_t " bytes", size);

    while (cnt < size) {
        ssize_t retval = recv(sd, (char *) data + cnt, size - cnt, MSG_WAITALL);

        /* remote closed connection */
        if (0 == retval) {
            pmix_output_verbose(8, pmix_ptl_base_framework.framework_output,
                                "ptl:base:recv_blocking: remote closed connection");
            return PMIX_ERR_UNREACH;
        }

        /* handle errors */
        if (0 > retval) {
            if (EAGAIN == pmix_socket_errno || EWOULDBLOCK == pmix_socket_errno) {
                flags = fcntl(sd, F_GETFL, 0);
                if (0 <= flags && 0 == (flags & O_NONBLOCK)) {
                    pmix_output_verbose(8, pmix_ptl_base_framework.framework_output,
                                        "blocking_recv timed out after %" PRIsize_t
                                        " of %" PRIsize_t " bytes", cnt, size);
                    return PMIX_ERR_TIMEOUT;
                }
                /* just cycle and let it try again */
                pmix_output_verbose(8, pmix_ptl_base_framework.framework_output,
                                    "blocking_recv received error %d:%s from remote - cycling",
                                    pmix_socket_errno, strerror(pmix_socket_errno));
                continue;
            }
            if (EINTR != pmix_socket_errno) {
                /* If we overflow the listen backlog, it's
                   possible that even though we finished the three
                   way handshake, the remote host was unable to
                   transition the connection from half connected
                   (received the initial SYN) to fully connected
                   (in the listen backlog).  We likely won't see
                   the failure until we try to receive, due to
                   timing and the like.  The first thing we'll get
                   in that case is a RST packet, which receive
                   will turn into a connection reset by peer
                   errno.  In that case, leave the socket in
                   CONNECT_ACK and propagate the error up to
                   recv_connect_ack, who will try to establish the
                   connection again */
                pmix_output_verbose(8, pmix_ptl_base_framework.framework_output,
                                    "blocking_recv received error %d:%s from remote - aborting",
                                    pmix_socket_errno, strerror(pmix_socket_errno));
                return PMIX_ERR_UNREACH;
            }
            continue;
        }
        cnt += retval;
    }

    pmix_output_verbose(8, pmix_ptl_base_framework.framework_output,
                        "blocking receive complete from remote");
    return PMIX_SUCCESS;
}

/* connect() bounded by ptl_base_handshake_wait_time.
 *
 * A blocking connect() to a host that silently drops the attempt waits out
 * the kernel's own connect timeout - often more than a minute - and the
 * loop below makes up to eleven attempts. Every caller of this function
 * blocks its thread for the duration (a tool attaching from its progress
 * thread uses the event-driven path instead - see ptl_base_fns.c), so give
 * each attempt the same bound the handshake replies get. Returns 0 on
 * success, or -1 with errno set - ETIMEDOUT when the bound expired. With
 * the bound disabled, or no select() to wait with, it is a plain
 * connect(). */
static int bounded_connect(int sd, struct sockaddr *addr, pmix_socklen_t addrlen)
{
#ifdef HAVE_SYS_SELECT_H
    int flags, rc, err, n;
    fd_set wfds;
    struct timeval tv;
    pmix_socklen_t errlen = sizeof(err);

    /* FD_SET cannot name a descriptor at or beyond FD_SETSIZE */
    if (0 < pmix_ptl_base.handshake_wait_time && FD_SETSIZE > sd) {
        flags = fcntl(sd, F_GETFL, 0);
        if (0 <= flags && 0 == fcntl(sd, F_SETFL, flags | O_NONBLOCK)) {
            rc = connect(sd, addr, addrlen);
            if (0 > rc && EINPROGRESS == pmix_socket_errno) {
                tv.tv_sec = pmix_ptl_base.handshake_wait_time;
                tv.tv_usec = 0;
                do {
                    FD_ZERO(&wfds);
                    FD_SET(sd, &wfds);
                    n = select(sd + 1, NULL, &wfds, NULL, &tv);
                } while (0 > n && EINTR == pmix_socket_errno);
                if (0 == n) {
                    errno = ETIMEDOUT;
                    rc = -1;
                } else if (0 < n) {
                    /* writable: the attempt finished - find out how */
                    if (0 != getsockopt(sd, SOL_SOCKET, SO_ERROR, (char *) &err, &errlen)) {
                        rc = -1;
                    } else if (0 != err) {
                        errno = err;
                        rc = -1;
                    } else {
                        rc = 0;
                    }
                }
            }
            err = errno;
            /* the caller expects the blocking socket it created */
            (void) fcntl(sd, F_SETFL, flags);
            errno = err;
            return rc;
        }
    }
#endif
    return connect(sd, addr, addrlen);
}

pmix_status_t pmix_ptl_base_connect(struct sockaddr_storage *addr,
                                    pmix_socklen_t addrlen, int *fd)
{
    int sd = -1, sd2;
    int retries = -1;
    bool connected = false;

    pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                        "ptl_base_connect: attempting to connect to server");

    /* Create the new socket */
    sd = socket(addr->ss_family, SOCK_STREAM, 0);

    while (retries < PMIX_MAX_RETRIES) {
        retries++;
        if (sd < 0) {
            pmix_output(0, "pmix:create_socket: socket() failed: %s (%d)\n",
                        strerror(pmix_socket_errno), pmix_socket_errno);
            sd = socket(addr->ss_family, SOCK_STREAM, 0);
            continue;
        }
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "pmix_ptl_base_connect: attempting to connect to server on socket %d",
                            sd);
        /* try to connect */
        if (bounded_connect(sd, (struct sockaddr *) addr, addrlen) < 0) {
            pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                                "Connect failed: %s (%d)", strerror(pmix_socket_errno),
                                pmix_socket_errno);
            if (ETIMEDOUT == pmix_socket_errno) {
                /* nothing answered for the whole bound - a host that did
                 * not answer this attempt will not answer the next ten */
                break;
            }
            /* get a different socket, but do that BEFORE we release the current
             * one so we don't just get the same socket handed back to us */
            sd2 = socket(addr->ss_family, SOCK_STREAM, 0);
            CLOSE_THE_SOCKET(sd);
            sd = sd2;
            continue;
        } else {
            /* otherwise, the connect succeeded - so break out of the loop */
            connected = true;
            break;
        }
    }

    /* record success explicitly rather than inferring it from the retry
     * count: a connect that succeeds on the final attempt leaves "retries"
     * at its limit, and testing that would throw away a live socket */
    if (!connected || sd < 0) {
        /* We were unsuccessful in establishing this connection, and are
         * not likely to suddenly become successful */
        if (0 <= sd) {
            CLOSE_THE_SOCKET(sd);
        }
        return PMIX_ERR_UNREACH;
    }
    *fd = sd;

    return PMIX_SUCCESS;
}

char *pmix_ptl_base_get_cmd_line(void)
{
    char *p = NULL;

#if OAC_HAVE_APPLE
    int mib[3], argmax, nargs, num;
    size_t size;
    char *procargs = NULL, *cp, *cptr;
    char **stack = NULL;

    /* Get the maximum process arguments size. */
    mib[0] = CTL_KERN;
    mib[1] = KERN_ARGMAX;
    size = sizeof(argmax);

    if (-1 == sysctl(mib, 2, &argmax, &size, NULL, 0)) {
        /* the command line is optional information - say why it is
         * missing only to someone who asked, not on every tool's stderr */
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "ptl:base:get_cmd_line: sysctl(KERN_ARGMAX) failed");
        return NULL;
    }

    /* Allocate space for the arguments. */
    procargs = (char *) malloc(argmax);
    if (NULL == procargs) {
        return NULL;
    }

    /* Make a sysctl() call to get the raw argument space of the process. */
    mib[0] = CTL_KERN;
    mib[1] = KERN_PROCARGS2;
    mib[2] = getpid();

    size = (size_t) argmax;

    if (-1 == sysctl(mib, 3, procargs, &size, NULL, 0)) {
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "ptl:base:get_cmd_line: sysctl(KERN_PROCARGS2) failed");
        free(procargs);
        return NULL;
    }

    memcpy(&nargs, procargs, sizeof(nargs));
    /* this points to the executable path, which is not part of argv - skip
     * over it. (An append used to sit here labeled "the first argv", but
     * it appended the path's terminating NUL: an empty first element, so
     * the joined line always started with a blank. argv[0] is picked up
     * by the loop below.) */
    cp = procargs + sizeof(nargs);
    cp += strlen(cp);
    /* skip any embedded NULLs */
    while (cp < &procargs[size] && '\0' == *cp) {
        ++cp;
    }
    if (cp != &procargs[size]) {
        /* from this point, we have the argv separated by NULLs - split them out */
        cptr = cp;
        num = 0;
        while (cp < &procargs[size] && num < nargs) {
            if ('\0' == *cp) {
                PMIx_Argv_append_nosize(&stack, cptr);
                ++cp; // skip over the NULL
                cptr = cp;
                ++num;
            } else {
                ++cp;
            }
        }
    }

    p = PMIx_Argv_join(stack, ' ');
    PMIx_Argv_free(stack);
    free(procargs);
#else
    char tmp[512];
    FILE *fp;
    pid_t mypid;

    /* open the pid's info file */
    mypid = getpid();
    pmix_snprintf(tmp, 512, "/proc/%lu/cmdline", (unsigned long) mypid);
    fp = fopen(tmp, "r");
    if (NULL != fp) {
        /* read the cmd line */
        if (NULL == fgets(tmp, 512, fp)) {
            fclose(fp);
            return NULL;
        }
        fclose(fp);
        p = strdup(tmp);
    }
#endif
    return p;
}

static pmix_status_t check_connections(pmix_list_t *connections)
{
    size_t len;
    pmix_connection_t *cn, *cnbase;

    len = pmix_list_get_size(connections);
    if (0 == len) {
        return PMIX_ERR_NOT_FOUND;
    }
    if (1 == len) {
        return PMIX_SUCCESS;
    }
    /* check to see if all the connections are to the same target */
    cnbase = (pmix_connection_t *) pmix_list_get_first(connections);
    PMIX_LIST_FOREACH (cn, connections, pmix_connection_t) {
        if (cn == cnbase) {
            continue;
        }
        if (0 != strcmp(cn->uri, cnbase->uri)) { // contains nspace and rank plus port
            pmix_show_help("help-ptl-base.txt", "too-many-conns", true);
            return PMIX_ERR_UNREACH;
        }
    }
    /* they are all to the same server */
    return PMIX_SUCCESS;
}

static pmix_status_t tryfile(pmix_peer_t *peer, char **nspace,
                             pmix_rank_t *rank, char **suri,
                             bool optional, char *filename)
{
    pmix_list_t connections;
    pmix_status_t rc;
    pmix_connection_t *cn;

    /* try to read the file */
    PMIX_CONSTRUCT(&connections, pmix_list_t);
    rc = pmix_ptl_base_parse_uri_file(filename, optional, &connections);
    if (PMIX_SUCCESS == rc) {
        rc = check_connections(&connections);
        if (PMIX_SUCCESS != rc) {
            PMIX_LIST_DESTRUCT(&connections);
            return rc;
        }
        cn = (pmix_connection_t *) pmix_list_get_first(&connections);
        *nspace = cn->nspace;
        cn->nspace = NULL;
        *rank = cn->rank;
        *suri = cn->uri;
        cn->uri = NULL;
        peer->protocol = PMIX_PROTOCOL_V2;
        PMIX_SET_PEER_VERSION(peer, cn->version, 2, 0);
    }
    PMIX_LIST_DESTRUCT(&connections);
    return rc;
}

static pmix_status_t trysearch(pmix_peer_t *peer, char **nspace,
                               pmix_rank_t *rank, char **suri,
                               char *filename, pmix_info_t *iptr, size_t niptr,
                               bool optional)
{
    pmix_list_t connections;
    pmix_status_t rc;
    pmix_connection_t *cn;

    PMIX_CONSTRUCT(&connections, pmix_list_t);
    rc = pmix_ptl_base_df_search(pmix_ptl_base.system_tmpdir, filename, iptr, niptr,
                                 optional, &connections);
    if (PMIX_SUCCESS == rc) {
        rc = check_connections(&connections);
        if (PMIX_SUCCESS != rc) {
            PMIX_LIST_DESTRUCT(&connections);
            return rc;
        }
        cn = (pmix_connection_t *) pmix_list_get_first(&connections);
        peer->protocol = PMIX_PROTOCOL_V2;
        PMIX_SET_PEER_VERSION(peer, cn->version, 2, 0);
        *nspace = cn->nspace;
        cn->nspace = NULL;
        *rank = cn->rank;
        *suri = cn->uri;
        cn->uri = NULL;
        PMIX_LIST_DESTRUCT(&connections);
        return rc;
    } else if (1 < pmix_list_get_size(&connections)) {
        pmix_show_help("help-ptl-base.txt", "too-many-conns", true);
    }
    PMIX_LIST_DESTRUCT(&connections);
    return rc;
}

/* A string directive must carry a string. Its value comes from the caller,
 * and one of any other type used to be read out of the union as a pointer
 * - so a PMIX_SERVER_URI, PMIX_TCP_URI, PMIX_TOOL_ATTACHMENT_FILE,
 * PMIX_SERVER_NSPACE or PMIX_CONNECTION_ORDER holding a bool took
 * PMIx_tool_init down with SIGSEGV. Same screen as the tool directives in
 * pmix_tool.c; used by pmix_ptl_base_check_connect_directives. */
static bool is_string_value(const pmix_info_t *info)
{
    if (PMIX_STRING != info->value.type || NULL == info->value.data.string) {
        PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
        return false;
    }
    return true;
}

/* Queue an info for the connect-ack. The caddy borrows it - the caddy has
 * no destructor - so the info must outlive the list. */
static pmix_status_t add_info(pmix_list_t *list, pmix_info_t *info)
{
    pmix_info_caddy_t *kv;

    kv = PMIX_NEW(pmix_info_caddy_t);
    if (NULL == kv) {
        return PMIX_ERR_NOMEM;
    }
    kv->info = info;
    pmix_list_append(list, &kv->super);
    return PMIX_SUCCESS;
}

/* Check the directives pmix_ptl_base_connect_to_peer consumes, acting on
 * none of them. A malformed one is an error in the call rather than a
 * failure to connect - no connection could have been attempted with it -
 * so it must be reported as PMIX_ERR_BAD_PARAM whatever the caller says
 * about the connection being optional. That is why this is separate from
 * connect_to_peer: its status alone cannot say whether anything was
 * attempted, because a server that refuses a tool may answer
 * PMIX_ERR_BAD_PARAM too (the host's tool_connected status is sent to the
 * tool verbatim), and PMIx_tool_init has to honor "optional" for that.
 *
 * Covers every value connect_to_peer would otherwise have to refuse: the
 * type of each directive, a connection order entry that is not one of the
 * connection targets, two different servers named by nspace, and a URI
 * that does not parse - including its address, which is checked the way
 * pmix_ptl_base_setup_connection will read it. No name resolution
 * happens there, so this never blocks. A "file:" URI is not opened: what
 * the file says is the server's word, not the caller's. */
pmix_status_t pmix_ptl_base_check_connect_directives(const pmix_info_t info[], size_t ninfo)
{
    size_t n, m, len;
    char **order;
    const char *str, *server_nspace = NULL;
    char *uri_nspace = NULL, *suri = NULL;
    pmix_rank_t rank;
    pid_t pid;
    int ival;
    struct sockaddr_storage addr;
    pmix_status_t rc;

    if (NULL == info) {
        return PMIX_SUCCESS;
    }
    for (n = 0; n < ninfo; n++) {
        if (PMIX_CHECK_KEY(&info[n], PMIX_CONNECTION_ORDER)) {
            if (!is_string_value(&info[n])) {
                return PMIX_ERR_BAD_PARAM;
            }
            /* "" and "," split to NULL - no preference, which is fine */
            order = PMIx_Argv_split(info[n].value.data.string, ',');
            for (m = 0; NULL != order && NULL != order[m]; m++) {
                /* Each entry names an attribute, which lookup turns into its
                 * string value - and hands back unchanged when it knows no
                 * such name. So the test is whether the result is one of the
                 * connection targets connect_to_peer acts on. This used to be
                 * "did lookup return NULL", which it never does: a misspelled
                 * entry, an unrelated attribute, or one with a space after
                 * its comma was silently skipped, and the requested order
                 * quietly became a different one. */
                str = pmix_attributes_lookup(order[m]);
                if (0 != strcmp(str, PMIX_CONNECT_SYSTEM_FIRST) &&
                    0 != strcmp(str, PMIX_CONNECT_TO_SYSTEM) &&
                    0 != strcmp(str, PMIX_CONNECT_TO_SCHEDULER) &&
                    0 != strcmp(str, PMIX_CONNECT_TO_SYS_CONTROLLER)) {
                    pmix_show_help("help-ptl-base.txt", "unknown-attribute", true,
                                   order[m], PMIX_CONNECTION_ORDER);
                    PMIx_Argv_free(order);
                    return PMIX_ERR_BAD_PARAM;
                }
            }
            PMIx_Argv_free(order);

        } else if (PMIX_CHECK_KEY(&info[n], PMIX_SERVER_PIDINFO)) {
            if (PMIX_SUCCESS != PMIx_Value_get_number(&info[n].value, &pid, PMIX_PID)) {
                PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
                return PMIX_ERR_BAD_PARAM;
            }

        } else if (PMIX_CHECK_KEY(&info[n], PMIX_CONNECT_MAX_RETRIES) ||
                   PMIX_CHECK_KEY(&info[n], PMIX_CONNECT_RETRY_DELAY)) {
            if (PMIX_SUCCESS != PMIx_Value_get_number(&info[n].value, &ival, PMIX_INT)) {
                PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
                return PMIX_ERR_BAD_PARAM;
            }

        } else if (PMIX_CHECK_KEY(&info[n], PMIX_SERVER_NSPACE)) {
            if (!is_string_value(&info[n])) {
                return PMIX_ERR_BAD_PARAM;
            }
            str = info[n].value.data.string;
            /* our own nspace is ignored, exactly as connect_to_peer does */
            if (0 == strcmp(pmix_globals.myid.nspace, str)) {
                continue;
            }
            if (NULL != server_nspace && 0 != strcmp(server_nspace, str)) {
                /* two different servers - we cannot know which one to use */
                PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
                return PMIX_ERR_BAD_PARAM;
            }
            server_nspace = str;

        } else if (PMIX_CHECK_KEY(&info[n], PMIX_TOOL_ATTACHMENT_FILE) ||
                   PMIX_CHECK_KEY(&info[n], PMIX_SERVER_TMPDIR) ||
                   PMIX_CHECK_KEY(&info[n], PMIX_SYSTEM_TMPDIR) ||
                   (PMIX_PEER_IS_LAUNCHER(pmix_globals.mypeer) &&
                    PMIX_CHECK_KEY(&info[n], PMIX_LAUNCHER_RENDEZVOUS_FILE))) {
            if (!is_string_value(&info[n])) {
                return PMIX_ERR_BAD_PARAM;
            }

        } else if (PMIX_CHECK_KEY(&info[n], PMIX_TCP_URI) ||
                   PMIX_CHECK_KEY(&info[n], PMIX_SERVER_URI)) {
            if (!is_string_value(&info[n])) {
                return PMIX_ERR_BAD_PARAM;
            }
            str = info[n].value.data.string;
            if (0 == strncmp(str, "file:", 5)) {
                continue;
            }
            rc = pmix_ptl_base_parse_uri(str, &uri_nspace, &rank, &suri);
            if (PMIX_SUCCESS != rc) {
                /* parse_uri's own copies failed */
                return (PMIX_ERR_NOMEM == rc) ? rc : PMIX_ERR_BAD_PARAM;
            }
            rc = pmix_ptl_base_setup_connection(suri, &addr, &len);
            free(uri_nspace);
            uri_nspace = NULL;
            free(suri);
            suri = NULL;
            if (PMIX_SUCCESS != rc) {
                return (PMIX_ERR_NOMEM == rc) ? rc : PMIX_ERR_BAD_PARAM;
            }
        }
    }
    return PMIX_SUCCESS;
}

/* Locate the server the directives name and connect to it.
 *
 * With cbfunc NULL this is the blocking connect every caller once used,
 * and it returns the outcome. With cbfunc set, everything up to having a
 * URI to connect to is still done here - that is reading files, not
 * waiting on a peer - but the connection itself is handed to the
 * event-driven path, and PMIX_OPERATION_IN_PROGRESS means cbfunc now owns
 * the outcome. */
static pmix_status_t do_connect(struct pmix_peer_t *pr,
                                pmix_info_t *info, size_t ninfo,
                                char **suriout,
                                pmix_ptl_connect_nb_cbfunc_t cbfunc, void *cbdata)
{
    char *suri = NULL, *evar;
    char *filename, *nspace = NULL;
    char **order = NULL;
    const char* tmp;
    pmix_rank_t rank = PMIX_RANK_WILDCARD;
    char *p = NULL, *server_nspace = NULL, *rendfile = NULL;
    int rc;
    size_t n, m;
    pid_t pid = 0;
    pmix_list_t ilist;
    pmix_info_caddy_t *kv;
    pmix_info_t *iptr = NULL, mypidinfo, mycmdlineinfo, launcher;
    pmix_info_t realuid, effectiveuid, realgid, effectivegid;
    size_t niptr = 0;
    pmix_peer_t *peer = (pmix_peer_t *) pr;
    bool optional = false;
    bool cmdline_loaded = false;

    pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                        "ptl:base: connecting to server");

    *suriout = NULL;
    /* Refuse a malformed directive before acting on any of them. Every
     * value the loop below reads has been vetted here, so the loop only
     * has to consume them - and nothing has been changed yet if one is
     * bad, not even the pmix_ptl_base globals the loop overwrites. */
    rc = pmix_ptl_base_check_connect_directives(info, ninfo);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }

    /* check any provided directives
     * to see where they want us to connect to */
    PMIX_CONSTRUCT(&ilist, pmix_list_t);
    if (NULL != info) {
        for (n = 0; n < ninfo; n++) {
            if (PMIX_CHECK_KEY(&info[n], PMIX_CONNECT_TO_SYSTEM)) {
                if (PMIX_INFO_TRUE(&info[n])) {
                    if (NULL != order) {
                        // overrides all prior specs
                        PMIx_Argv_free(order);
                        order = NULL;
                    }
                    PMIx_Argv_append_nosize(&order, PMIX_CONNECT_TO_SYSTEM);
                }

            } else if (PMIX_CHECK_KEY(&info[n], PMIX_CONNECT_SYSTEM_FIRST)) {
                /* try the system-level */
                if (PMIX_INFO_TRUE(&info[n])) {
                    PMIx_Argv_prepend_nosize(&order, PMIX_CONNECT_SYSTEM_FIRST);
                }

            } else if (PMIX_CHECK_KEY(&info[n], PMIX_CONNECT_TO_SCHEDULER)) {
                /* find the scheduler */
                if (PMIX_INFO_TRUE(&info[n])) {
                    PMIx_Argv_append_nosize(&order, PMIX_CONNECT_TO_SCHEDULER);
                }

            } else if (PMIX_CHECK_KEY(&info[n], PMIX_CONNECT_TO_SYS_CONTROLLER)) {
                /* find the system controller */
                if (PMIX_INFO_TRUE(&info[n])) {
                    PMIx_Argv_append_nosize(&order, PMIX_CONNECT_TO_SYS_CONTROLLER);
                }

            } else if (PMIX_CHECK_KEY(&info[n], PMIX_CONNECTION_ORDER)) {
                if (NULL != order) {
                    // overrides all prior specs
                    PMIx_Argv_free(order);
                    order = NULL;
                }
                order = PMIx_Argv_split(info[n].value.data.string, ',');
                /* an empty list - "" or "," - splits to NULL. That names no
                 * preference, so leave the order cleared (this attribute
                 * overrides every prior spec) rather than indexing the NULL
                 * below */
                if (NULL == order) {
                    continue;
                }
                // the strings will just be the name of the attribute, so we
                // must convert them to the attribute values
                for (m=0; NULL != order[m]; m++) {
                    /* every entry was vetted as a connection target by
                     * pmix_ptl_base_check_connect_directives */
                    tmp = pmix_attributes_lookup(order[m]);
                    free(order[m]);
                    order[m] = strdup(tmp);
                    if (NULL == order[m]) {
                        /* a NULL here would end the argv early - silently
                         * dropping the rest of the requested order and
                         * leaking the strings after it */
                        for (++m; NULL != order[m]; m++) {
                            free(order[m]);
                        }
                        free(order);
                        order = NULL;
                        rc = PMIX_ERR_NOMEM;
                        goto badinput;
                    }
                }

            } else if (PMIX_CHECK_KEY(&info[n], PMIX_SERVER_PIDINFO)) {
                /* read as a number, as the validator did */
                rc = PMIx_Value_get_number(&info[n].value, &pid, PMIX_PID);
                if (PMIX_SUCCESS != rc) {
                    goto badinput;
                }

            } else if (PMIX_CHECK_KEY(&info[n], PMIX_SERVER_NSPACE)) {
                // if this is my nspace, then ignore it
                if (0 == strcmp(pmix_globals.myid.nspace, info[n].value.data.string)) {
                    continue;
                }
                if (NULL != server_nspace) {
                    /* they included it more than once */
                    if (0 == strcmp(server_nspace, info[n].value.data.string)) {
                        /* same value, so ignore it */
                        continue;
                    }
                    /* otherwise, we don't know which one to use */
                    rc = PMIX_ERR_BAD_PARAM;
                    goto badinput;
                }
                server_nspace = strdup(info[n].value.data.string);
                if (NULL == server_nspace) {
                    rc = PMIX_ERR_NOMEM;
                    goto badinput;
                }

            } else if (PMIX_CHECK_KEY(&info[n], PMIX_TOOL_ATTACHMENT_FILE)) {
                if (NULL != rendfile) {
                    free(rendfile);
                }
                /* a failed copy here would not fail anything later - it
                 * would drop the file they named and send us off to
                 * discover, and possibly attach to, some other server */
                rendfile = strdup(info[n].value.data.string);
                if (NULL == rendfile) {
                    rc = PMIX_ERR_NOMEM;
                    goto badinput;
                }

            } else if (PMIX_PEER_IS_LAUNCHER(pmix_globals.mypeer)
                       && PMIX_CHECK_KEY(&info[n], PMIX_LAUNCHER_RENDEZVOUS_FILE)) {
                if (NULL != pmix_ptl_base.rendezvous_filename) {
                    free(pmix_ptl_base.rendezvous_filename);
                }
                pmix_ptl_base.rendezvous_filename = strdup(info[n].value.data.string);
                if (NULL == pmix_ptl_base.rendezvous_filename) {
                    rc = PMIX_ERR_NOMEM;
                    goto badinput;
                }

            } else if (PMIX_CHECK_KEY(&info[n], PMIX_TOOL_CONNECT_OPTIONAL)) {
                optional = PMIX_INFO_TRUE(&info[n]);

            } else if (PMIX_CHECK_KEY(&info[n], PMIX_TCP_URI)
                       || PMIX_CHECK_KEY(&info[n], PMIX_SERVER_URI)) {
                if (NULL != pmix_ptl_base.uri) {
                    free(pmix_ptl_base.uri);
                }
                /* as for the attachment file: losing this copy would not
                 * fail, it would discover some other server instead */
                pmix_ptl_base.uri = strdup(info[n].value.data.string);
                if (NULL == pmix_ptl_base.uri) {
                    rc = PMIX_ERR_NOMEM;
                    goto badinput;
                }

            } else if (PMIX_CHECK_KEY(&info[n], PMIX_SERVER_TMPDIR)) {
                if (NULL != pmix_ptl_base.session_tmpdir) {
                    free(pmix_ptl_base.session_tmpdir);
                }
                pmix_ptl_base.session_tmpdir = strdup(info[n].value.data.string);
                if (NULL == pmix_ptl_base.session_tmpdir) {
                    rc = PMIX_ERR_NOMEM;
                    goto badinput;
                }

            } else if (PMIX_CHECK_KEY(&info[n], PMIX_SYSTEM_TMPDIR)) {
                if (NULL != pmix_ptl_base.system_tmpdir) {
                    free(pmix_ptl_base.system_tmpdir);
                }
                pmix_ptl_base.system_tmpdir = strdup(info[n].value.data.string);
                if (NULL == pmix_ptl_base.system_tmpdir) {
                    rc = PMIX_ERR_NOMEM;
                    goto badinput;
                }

            } else if (PMIX_CHECK_KEY(&info[n], PMIX_CONNECT_MAX_RETRIES)) {
                rc = PMIx_Value_get_number(&info[n].value, &pmix_ptl_base.max_retries, PMIX_INT);
                if (PMIX_SUCCESS != rc) {
                    goto badinput;
                }

            } else if (PMIX_CHECK_KEY(&info[n], PMIX_CONNECT_RETRY_DELAY)) {
                rc = PMIx_Value_get_number(&info[n].value, &pmix_ptl_base.wait_to_connect, PMIX_INT);
                if (PMIX_SUCCESS != rc) {
                    goto badinput;
                }

            } else {
                /* need to pass this to server */
                rc = add_info(&ilist, &info[n]);
                if (PMIX_SUCCESS != rc) {
                    goto badinput;
                }
            }
        }
    }

    /* add some info to the array - some of this is duplicative, but we
     * need to add it in the array because some peer cases failed to
     * include it, and we now need it for all cases */

    /* add our pid to the array */
    PMIX_INFO_LOAD(&mypidinfo, PMIX_PROC_PID, &pmix_globals.pid, PMIX_PID);
    rc = add_info(&ilist, &mypidinfo);
    if (PMIX_SUCCESS != rc) {
        goto badinput;
    }

    /* add our real uid */
    PMIX_INFO_LOAD(&realuid, PMIX_REALUID, &pmix_globals.realuid, PMIX_UINT32);
    rc = add_info(&ilist, &realuid);
    if (PMIX_SUCCESS != rc) {
        goto badinput;
    }

    /* add our effective uid */
    PMIX_INFO_LOAD(&effectiveuid, PMIX_USERID, &pmix_globals.uid, PMIX_UINT32);
    rc = add_info(&ilist, &effectiveuid);
    if (PMIX_SUCCESS != rc) {
        goto badinput;
    }

    /* add our real gid */
    PMIX_INFO_LOAD(&realgid, PMIX_REALGID, &pmix_globals.realgid, PMIX_UINT32);
    rc = add_info(&ilist, &realgid);
    if (PMIX_SUCCESS != rc) {
        goto badinput;
    }

    /* add our effective gid */
    PMIX_INFO_LOAD(&effectivegid, PMIX_GRPID, &pmix_globals.gid, PMIX_UINT32);
    rc = add_info(&ilist, &effectivegid);
    if (PMIX_SUCCESS != rc) {
        goto badinput;
    }

    /* if I am a launcher, tell them so */
    if (PMIX_PEER_IS_LAUNCHER(pmix_globals.mypeer)) {
        PMIX_INFO_LOAD(&launcher, PMIX_LAUNCHER, NULL, PMIX_BOOL);
        rc = add_info(&ilist, &launcher);
        if (PMIX_SUCCESS != rc) {
            goto badinput;
        }
    }

    /* if I am a system controller, tell them so */
    if (PMIX_PEER_IS_SYS_CTRLR(pmix_globals.mypeer)) {
        PMIX_INFO_LOAD(&launcher, PMIX_SERVER_SYS_CONTROLLER, NULL, PMIX_BOOL);
        rc = add_info(&ilist, &launcher);
        if (PMIX_SUCCESS != rc) {
            goto badinput;
        }
    }

    /* if I am a scheduler, tell them so */
    if (PMIX_PEER_IS_SCHEDULER(pmix_globals.mypeer)) {
        PMIX_INFO_LOAD(&launcher, PMIX_SERVER_SCHEDULER, NULL, PMIX_BOOL);
        rc = add_info(&ilist, &launcher);
        if (PMIX_SUCCESS != rc) {
            goto badinput;
        }
    }

    /* add our cmd line to the array */
    p = pmix_ptl_base_get_cmd_line();
    if (NULL != p) {
        /* pass it along */
        PMIX_INFO_LOAD(&mycmdlineinfo, PMIX_CMD_LINE, p, PMIX_STRING);
        free(p);
        rc = add_info(&ilist, &mycmdlineinfo);
        if (PMIX_SUCCESS != rc) {
            PMIX_INFO_DESTRUCT(&mycmdlineinfo);
            goto badinput;
        }
        cmdline_loaded = true;
    }

    /* if we need to pass anything, setup an array */
    if (0 < (niptr = pmix_list_get_size(&ilist))) {
        PMIX_INFO_CREATE(iptr, niptr);
        if (NULL == iptr) {
            niptr = 0;
            PMIX_LIST_DESTRUCT(&ilist);
            rc = PMIX_ERR_NOMEM;
            goto cleanup;
        }
        n = 0;
        while (NULL != (kv = (pmix_info_caddy_t *) pmix_list_remove_first(&ilist))) {
            PMIX_INFO_XFER(&iptr[n], kv->info);
            PMIX_RELEASE(kv);
            ++n;
        }
    }
    PMIX_LIST_DESTRUCT(&ilist);

    /* mark that we are using the V2 protocol */
    pmix_globals.mypeer->protocol = PMIX_PROTOCOL_V2;
    /* if we were given a URI, then look no further */
    if (NULL != pmix_ptl_base.uri) {
        /* if the string starts with "file:", then they are pointing
         * us to a file we need to read to get the URI itself */
        if (0 == strncmp(pmix_ptl_base.uri, "file:", 5)) {
            pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                                "ptl:tool:tool getting connection info from %s", pmix_ptl_base.uri);
            rc = tryfile(peer, &nspace, &rank, &suri, optional, &pmix_ptl_base.uri[5]);
            if (PMIX_SUCCESS != rc) {
                goto cleanup;
            }
            goto complete;
        }
        /* Extract the server's nspace and rank with the same parser every
         * other route uses. This used to split "nspace.rank" at the FIRST
         * '.', so an nspace that itself contains one - Slurm's
         * "slurm.pmix.<jobid>.<stepid>", say - connected to the right
         * address but recorded the server as "slurm", rank 0. The rank is
         * whatever follows the LAST '.'. */
        rc = pmix_ptl_base_parse_uri(pmix_ptl_base.uri, &nspace, &rank, &suri);
        if (PMIX_SUCCESS != rc) {
            rc = PMIX_ERR_BAD_PARAM;
            goto cleanup;
        }
        goto complete;
    }

    /* if they gave us a rendezvous file, use it */
    if (NULL != rendfile) {
        rc = tryfile(peer, &nspace, &rank, &suri, optional, rendfile);
        if (PMIX_SUCCESS != rc) {
            /* they gave us a specific rendfile and we couldn't read it,
             * so we have no URI to connect to - whether or not the
             * attempt was optional, there is nothing further to try */
            goto cleanup;
        }
        goto complete;
    }

    if (NULL != order) {
        // cycle thru the requested order
        for (n=0; NULL != order[n]; n++) {
            if (0 == strcmp(order[n], PMIX_CONNECT_SYSTEM_FIRST)) {
                if (0 > pmix_asprintf(&filename, "%s/pmix.sys.%s", pmix_ptl_base.system_tmpdir,
                                 pmix_globals.hostname)) {
                    rc = PMIX_ERR_NOMEM;
                    goto cleanup;
                }
                pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                                    "ptl:tool:tool looking for system server at %s", filename);
                // this is always optional as we are going to fallback to non-system servers
                rc = tryfile(peer, &nspace, &rank, &suri, true, filename);
                free(filename);
                if (PMIX_SUCCESS == rc) {
                    PMIX_SET_PEER_TYPE(peer, PMIX_PROC_SERVER);
                    goto complete;
                }

            } else if (0 == strcmp(order[n], PMIX_CONNECT_TO_SYSTEM)) {
                if (0 > pmix_asprintf(&filename, "%s/pmix.sys.%s", pmix_ptl_base.system_tmpdir,
                                 pmix_globals.hostname)) {
                    rc = PMIX_ERR_NOMEM;
                    goto cleanup;
                }
                pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                                    "ptl:tool:tool looking for system server at %s", filename);
                rc = tryfile(peer, &nspace, &rank, &suri, optional, filename);
                free(filename);
                if (PMIX_SUCCESS == rc) {
                    PMIX_SET_PEER_TYPE(peer, PMIX_PROC_SERVER);
                    goto complete;
                }
                if (!optional) {
                    // not optional, so report error
                    goto cleanup;
                }

            } else if (0 == strcmp(order[n], PMIX_CONNECT_TO_SCHEDULER)) {
                if (0 > pmix_asprintf(&filename, "%s/pmix.sched.%s",
                                 pmix_ptl_base.system_tmpdir,
                                 pmix_globals.hostname)) {
                    rc = PMIX_ERR_NOMEM;
                    goto cleanup;
                }
                pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                                    "ptl:tool:tool looking for scheduler at %s", filename);
                rc = tryfile(peer, &nspace, &rank, &suri, optional, filename);
                free(filename);
                if (PMIX_SUCCESS == rc) {
                    PMIX_SET_PEER_TYPE(peer, PMIX_PROC_SCHEDULER);
                    goto complete;
                }
                if (!optional) {
                    // not optional, so report error
                    goto cleanup;
                }

            } else if (0 == strcmp(order[n], PMIX_CONNECT_TO_SYS_CONTROLLER)) {
                if (0 > pmix_asprintf(&filename, "%s/pmix.sysctrlr.%s",
                                 pmix_ptl_base.system_tmpdir,
                                 pmix_globals.hostname)) {
                    rc = PMIX_ERR_NOMEM;
                    goto cleanup;
                }
                pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                                    "ptl:tool:tool looking for system controller at %s", filename);
                rc = tryfile(peer, &nspace, &rank, &suri, optional, filename);
                free(filename);
                if (PMIX_SUCCESS == rc) {
                    PMIX_SET_PEER_TYPE(peer, PMIX_PROC_SYS_CTRLR);
                    goto complete;
                }
                if (!optional) {
                    // not optional, so report error
                    goto cleanup;
                }
            }
        }
    }

    /* if they gave us a pid, then look for it */
    if (0 != pid) {
        if (0 > pmix_asprintf(&filename, "pmix.%s.tool.%d", pmix_globals.hostname, pid)) {
            rc = PMIX_ERR_NOMEM;
            goto cleanup;
        }
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "ptl:tool:tool searching for given session server %s", filename);
        rc = trysearch(peer, &nspace, &rank, &suri, filename, iptr, niptr, optional);
        free(filename);
        if (PMIX_SUCCESS != rc) {
            /* since they gave us a specific pid and we couldn't
             * connect to it, return an error */
            goto cleanup;
        }
        PMIX_SET_PEER_TYPE(peer, PMIX_PROC_SERVER);
        goto complete;
    }

    /* if they gave us an nspace, then look for it */
    if (NULL != server_nspace) {
        if (0 > pmix_asprintf(&filename, "pmix.%s.tool.%s", pmix_globals.hostname, server_nspace)) {
            rc = PMIX_ERR_NOMEM;
            goto cleanup;
        }
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "ptl:tool:tool searching for given nspace server %s", filename);
        rc = trysearch(peer, &nspace, &rank, &suri, filename, iptr, niptr, optional);
        free(filename);
        if (PMIX_SUCCESS != rc) {
            /* since they gave us a specific nspace and we couldn't
             * connect to it, return an error */
            goto cleanup;
        }
        PMIX_SET_PEER_TYPE(peer, PMIX_PROC_SERVER);
        goto complete;
    }

    /* see if we are a client of some server */
    evar = NULL;
    rc = pmix_ptl_base_set_peer(peer, &evar);
    if (PMIX_SUCCESS == rc) {
        PMIX_SET_PEER_TYPE(pmix_globals.mypeer, PMIX_PROC_CLIENT_TOOL);
        rc = pmix_ptl_base_parse_uri(evar, &nspace, &rank, &suri);
        if (PMIX_SUCCESS != rc) {
            goto cleanup;
        }
        goto complete;
    } else if (!PMIX_PEER_IS_SERVER(pmix_globals.mypeer) ||
               PMIX_PEER_IS_LAUNCHER(pmix_globals.mypeer)) {
        /* we aren't a client, so we will search to see what session-level
         * tools are available to this user. We will take the first connection
         * that succeeds - this is based on the likelihood that there is only
         * one session per user on a node */

        if (0 > pmix_asprintf(&filename, "pmix.%s.tool", pmix_globals.hostname)) {
            rc = PMIX_ERR_NOMEM;
            goto cleanup;
        }
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "ptl:tool:tool searching for session server %s", filename);
        rc = trysearch(peer, &nspace, &rank, &suri, filename, iptr, niptr, optional);
        free(filename);
        if (PMIX_SUCCESS == rc) {
            PMIX_SET_PEER_TYPE(peer, PMIX_PROC_SERVER);
            goto complete;
        }
        if (!optional) {
            goto cleanup;
        }
    }
    rc = PMIX_ERR_UNREACH;
    goto cleanup;

complete:
    if (NULL != cbfunc) {
        rc = pmix_ptl_base_start_connection(peer, nspace, rank, suri, iptr, niptr,
                                            cbfunc, cbdata);
        if (PMIX_SUCCESS == rc) {
            /* all three belong to the connection now */
            nspace = NULL;
            suri = NULL;
            iptr = NULL;
            niptr = 0;
            rc = PMIX_OPERATION_IN_PROGRESS;
        }
        goto cleanup;
    }

    rc = pmix_ptl_base_make_connection(peer, suri, iptr, niptr);
    if (PMIX_SUCCESS != rc) {
        goto cleanup;
    }

    pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                        "tool_peer_try_connect: Connection across to server succeeded");

    rc = pmix_ptl_base_complete_connection(peer, nspace, rank);

cleanup:
    *suriout = suri;
    if (NULL != nspace) {
        free(nspace);
    }
    if (NULL != iptr) {
        PMIX_INFO_FREE(iptr, niptr);
    }
    if (NULL != order) {
        PMIx_Argv_free(order);
    }
    /* the command line was xfer'd into iptr, so release our local copy */
    if (cmdline_loaded) {
        PMIX_INFO_DESTRUCT(&mycmdlineinfo);
    }
    if (NULL != rendfile) {
        free(rendfile);
    }
    if (NULL != server_nspace) {
        free(server_nspace);
    }
    return rc;

badinput:
    /* a directive we could not use, or could not keep, before anything had
     * been built from the array - only the loop's own state to give back */
    if (NULL != server_nspace) {
        free(server_nspace);
    }
    if (NULL != rendfile) {
        free(rendfile);
    }
    PMIx_Argv_free(order);
    PMIX_LIST_DESTRUCT(&ilist);
    return rc;
}

pmix_status_t pmix_ptl_base_connect_to_peer(struct pmix_peer_t *pr,
                                            pmix_info_t *info, size_t ninfo,
                                            char **suriout)
{
    return do_connect(pr, info, ninfo, suriout, NULL, NULL);
}

pmix_status_t pmix_ptl_base_connect_to_peer_nb(struct pmix_peer_t *pr,
                                               pmix_info_t *info, size_t ninfo,
                                               pmix_ptl_connect_nb_cbfunc_t cbfunc,
                                               void *cbdata)
{
    pmix_status_t rc;
    char *suri = NULL;

    if (NULL == cbfunc) {
        return PMIX_ERR_BAD_PARAM;
    }
    rc = do_connect(pr, info, ninfo, &suri, cbfunc, cbdata);
    if (PMIX_OPERATION_IN_PROGRESS == rc) {
        /* the connection owns everything, including the URI - the
         * callback will be handed it */
        return PMIX_SUCCESS;
    }
    /* nothing was started, so nobody else will free what we located */
    free(suri);
    return (PMIX_SUCCESS == rc) ? PMIX_ERROR : rc;
}
