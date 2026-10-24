/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2014-2015 Artem Y. Polyakov <artpol84@gmail.com>.
 *                         All rights reserved.
 * Copyright (c) 2016      Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2016      IBM Corporation.  All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "src/include/pmix_config.h"

#include "src/include/pmix_socket_errno.h"
#include "src/include/pmix_stdint.h"

#include "include/pmix_server.h"
#include "src/include/pmix_globals.h"

#ifdef HAVE_STRING_H
#    include <string.h>
#endif
#include <fcntl.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif
#ifdef HAVE_SYS_SOCKET_H
#    include <sys/socket.h>
#endif
#ifdef HAVE_SYS_UN_H
#    include <sys/un.h>
#endif
#ifdef HAVE_SYS_UIO_H
#    include <sys/uio.h>
#endif
#ifdef HAVE_SYS_TYPES_H
#    include <sys/types.h>
#endif
#include <ctype.h>
#include <dirent.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <event.h>
#include <pthread.h>

#include "src/class/pmix_list.h"
#include "src/include/pmix_socket_errno.h"
#include "src/mca/bfrops/base/base.h"
#include "src/mca/psec/base/base.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_basename.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_fd.h"
#include "src/util/pmix_net.h"
#include "src/util/pmix_os_dirpath.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_if.h"
#include "src/util/pmix_environ.h"
#include "src/util/pmix_parse_options.h"
#include "src/util/pmix_printf.h"
#include "src/util/pmix_show_help.h"
#include "src/util/pmix_string_copy.h"

#include "src/mca/ptl/base/base.h"

// local connection handler
static void connection_event_handler(int incoming_sd, short flags, void *cbdata);

static void arm_listener(pmix_listener_t *lt)
{
    if (lt->active || 0 > lt->socket) {
        return;
    }
    pmix_event_set(pmix_globals.evbase, &lt->ev, lt->socket,
                   PMIX_EV_READ|PMIX_EV_PERSIST,
                   connection_event_handler, lt);
    lt->active = true;
    pmix_event_add(&lt->ev, 0);
}

/* stop accepting on lt, whether it is armed or waiting out a pause */
static void disarm_listener(pmix_listener_t *lt)
{
    if (lt->active) {
        pmix_event_del(&lt->ev);
        lt->active = false;
    }
    if (lt->paused) {
        pmix_event_evtimer_del(&lt->retry_ev);
        lt->paused = false;
    }
}

// local value for connection support
static bool setup_complete = false;

/* Bind the socket and publish how to reach it, but do not yet answer.
 *
 * This is deliberately separate from arming the accept event below. The
 * two used to be one call, and that put the moment an outside process
 * could schedule work on our progress thread in the *middle* of the
 * caller's init: pmix_rte_init() ends by starting that thread, so
 * everything an init does afterwards runs on the caller's thread, while
 * a connection handler on the progress thread extends
 * pmix_globals.nspaces, pmix_server_globals.clients and the gds tracker
 * lists. gds/hash takes no lock; it is correct only because everything
 * that touches it is meant to be on one thread.
 *
 * Splitting it here rather than simply moving the whole call to the foot
 * of init keeps the URI and the rendezvous file where they were - a
 * launcher that attaches to a parent partway through its own init has to
 * have a URI of its own to offer it. A peer that connects in the
 * meantime is not refused: the socket is bound and listening as far as
 * the kernel is concerned, so its connection waits in the backlog until
 * we start accepting. */
pmix_status_t pmix_ptl_base_create_listener(pmix_info_t info[], size_t ninfo)
{
    pmix_status_t rc;

    /* setup the listener */
    if (!setup_complete) {
        rc = pmix_ptl.setup_listener(info, ninfo);
        if (PMIX_SUCCESS != rc) {
            return rc;
        }
    }
    setup_complete = true;

    return PMIX_SUCCESS;
}

/* How many connections may be waiting on their connect-ack at once - and,
 * separately, how many tools may be waiting on the host. Each holds a
 * descriptor, and a peer need not have a credential to open one, so by
 * default each gets an eighth of the descriptors we have - a quarter
 * between them; the rest stay for connected clients, IOF and the files we
 * open. Read here rather than at register, so a host that raises its
 * limit during init is counted with the limit it raised */
static int pending_cap(void)
{
    struct rlimit rl;

    if (0 > pmix_ptl_base.max_pending_connections) {
        return 0;
    }
    if (0 < pmix_ptl_base.max_pending_connections) {
        return pmix_ptl_base.max_pending_connections;
    }
    if (0 == getrlimit(RLIMIT_NOFILE, &rl) && RLIM_INFINITY != rl.rlim_cur) {
        if (8 > rl.rlim_cur) {
            return 1;
        }
        if ((rlim_t) INT_MAX < rl.rlim_cur / 8) {
            return INT_MAX;
        }
        return (int) (rl.rlim_cur / 8);
    }
    /* no limit we can see, so none we can run into */
    return 0;
}

/* Begin answering the connections the kernel has been holding for us.
 *
 * Call this at the foot of init, once there is nothing left for the
 * caller's thread to do to shared state. It is a no-op when no listener
 * was ever created - a tool that is not acting as a server never calls
 * pmix_ptl_base_create_listener - so the call site need not be
 * conditional. */
void pmix_ptl_base_start_listening(void)
{
    pmix_listener_t *lt = &pmix_ptl_base.listener;
    pmix_listener_t *alt;

    pmix_ptl_base.pending_cap = pending_cap();
    arm_listener(lt);
    arm_listener(&pmix_ptl_base.local_listener);
    PMIX_LIST_FOREACH (alt, &pmix_ptl_base.alt_listeners, pmix_listener_t) {
        arm_listener(alt);
    }
}

void pmix_ptl_base_stop_listening(void)
{
    pmix_listener_t *lt = &pmix_ptl_base.listener;
    pmix_listener_t *alt;
    pmix_pending_connection_t *pnd, *pnext;
    uint32_t u32;

    pmix_output_verbose(8, pmix_ptl_base_framework.framework_output,
                        "ptl:base:stop_listening");

    /* A connection still sending its connect-ack holds a socket, an armed
     * read event and perhaps a timer, and nothing else refers to it - with
     * the timeout disabled, a peer that connected and went quiet would keep
     * all three for the life of the process. Every caller has stopped the
     * progress thread by now, so neither of its handlers can be running. */
    PMIX_LIST_FOREACH_SAFE (pnd, pnext, &pmix_ptl_base.pending_connections,
                            pmix_pending_connection_t) {
        pmix_ptl_base_drop_pending_connection(pnd, PMIX_ERR_UNREACH);
    }
    /* and the tools still waiting their turn with the host */
    while (NULL != (pnd = (pmix_pending_connection_t *) pmix_list_remove_first(
                        &pmix_ptl_base.tool_queue))) {
        u32 = htonl((uint32_t) PMIX_ERR_UNREACH);
        if (0 <= pnd->sd) {
            (void) pmix_ptl_base_send_blocking(pnd->sd, (char *) &u32, sizeof(uint32_t));
        }
        PMIX_RELEASE(pnd);
    }

    /* The listener has to be built again if the framework is opened
     * again in this process - pmix_ptl_close destructs the listener
     * object, so the socket and URI this flag says we already have are
     * gone with it. Clear it here rather than at the top of setup so a
     * repeated start_listening within one cycle still short-circuits. */
    setup_complete = false;

    /* the alternates go whether or not they were ever armed: their sockets
     * were bound and listening from the moment setup created them */
    while (NULL != (alt = (pmix_listener_t *) pmix_list_remove_first(&pmix_ptl_base.alt_listeners))) {
        disarm_listener(alt);
        PMIX_RELEASE(alt);  // the destructor closes its socket
    }
    free(pmix_ptl_base.alt_uris);
    pmix_ptl_base.alt_uris = NULL;

    /* the local listener's socket too, whether or not it was ever armed */
    disarm_listener(&pmix_ptl_base.local_listener);
    if (0 <= pmix_ptl_base.local_listener.socket) {
        CLOSE_THE_SOCKET(pmix_ptl_base.local_listener.socket);
        pmix_ptl_base.local_listener.socket = -1;
    }

    /* A paused listener is not active, but its socket is still open and
     * its retry timer still armed - so this cannot stop at "not active" */
    if (!lt->active && !lt->paused) {
        /* nothing we need do */
        return;
    }
    disarm_listener(lt);
    /* close the socket to remove the connection points */
    CLOSE_THE_SOCKET(lt->socket);
    lt->socket = -1;
}

/* Stop accepting for good, after an accept() failure we cannot retry.
 *
 * All three pieces go together. The handler used to close only the
 * descriptor it was handed, which left the event registered, the listener
 * marked active, and lt->socket naming a number the kernel was free to
 * hand out again - to a client socket or an IOF pipe. The stale event then
 * fired on that reuse and called accept() on it, and finalize closed it,
 * so the failure took some unrelated descriptor down with it. */
static void abandon_listener(pmix_listener_t *lt)
{
    disarm_listener(lt);
    CLOSE_THE_SOCKET(lt->socket);
}

/* An error accept() reports when it cannot go on: one of these is retried
 * this many times, a second apart, before the listener is abandoned. A
 * minute is longer than anyone waits on a connection */
#define PMIX_PTL_ACCEPT_MAX_RETRIES 60

static void resume_listener(int sd, short args, void *cbdata)
{
    pmix_listener_t *lt = (pmix_listener_t *) cbdata;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    lt->paused = false;
    arm_listener(lt);
}

/* Stop accepting on lt for a second, leaving the socket open. On Linux the
 * connection whose accept() failed stays in the backlog, so the event would
 * fire again at once for as long as the error lasts, and the connection is
 * accepted when we resume. BSD-derived systems, macOS among them, take it
 * off the backlog and close it - the peer sees its connection closed - so
 * there the pause is for the connections that follow */
static void pause_listener(pmix_listener_t *lt)
{
    struct timeval tv = {1, 0};

    if (lt->active) {
        pmix_event_del(&lt->ev);
        lt->active = false;
    }
    if (!lt->paused) {
        pmix_event_evtimer_set(pmix_globals.evbase, &lt->retry_ev, resume_listener, lt);
        lt->paused = true;
    }
    pmix_event_evtimer_add(&lt->retry_ev, &tv);
}

void pmix_ptl_base_accept_failed(pmix_listener_t *lt, int err)
{
    const char *why;

    /* Errors that concern only the connection being accepted. Besides
     * the obvious ones, a connection that was reset or hit a network
     * error while it waited in the backlog is reported by accept() on
     * some systems - Linux documents that these are to be retried like
     * EAGAIN. None of them says anything about the listening socket */
    if (EINTR == err || EAGAIN == err || EWOULDBLOCK == err ||
        ECONNABORTED == err || EPROTO == err ||
        ENETDOWN == err || ENETUNREACH == err ||
        EHOSTUNREACH == err || EHOSTDOWN == err) {
        return;
    }

    /* The descriptor is no longer our socket. Nothing is gained by trying
     * it again, and a number we keep may be handed to something else */
    if (EBADF == err || ENOTSOCK == err) {
        pmix_show_help("help-ptl-base.txt", "accept failed", true,
                       pmix_globals.hostname, err, strerror(err),
                       "The listening socket is gone; no further connections "
                       "will be accepted");
        abandon_listener(lt);
        return;
    }

    /* Out of descriptors or memory. That clears as connections finish or
     * close - an unfinished connect-ack is dropped after
     * ptl_base_connect_ack_timeout - so it is retried for as long as it
     * lasts */
    if (EMFILE == err || ENFILE == err || ENOBUFS == err || ENOMEM == err) {
        if (!lt->warned) {
            PMIX_ERROR_LOG(PMIX_ERR_OUT_OF_RESOURCE);
            why = (ENOBUFS == err || ENOMEM == err) ? "Out of memory" : "Out of file descriptors";
            pmix_show_help("help-ptl-base.txt", "accept failed", true,
                           pmix_globals.hostname, err, strerror(err), why);
            lt->warned = true;
        }
        pause_listener(lt);
        return;
    }

    /* Anything else may or may not clear, so it is retried for a while */
    if (PMIX_PTL_ACCEPT_MAX_RETRIES <= lt->nretries) {
        pmix_show_help("help-ptl-base.txt", "accept failed", true,
                       pmix_globals.hostname, err, strerror(err),
                       "Still failing after a minute of retries; no further "
                       "connections will be accepted");
        abandon_listener(lt);
        return;
    }
    if (!lt->warned) {
        pmix_show_help("help-ptl-base.txt", "accept failed", true,
                       pmix_globals.hostname, err, strerror(err),
                       "Unknown cause; retrying once a second for up to a minute");
        lt->warned = true;
    }
    ++lt->nretries;
    pause_listener(lt);
}

/* Give up on a connection whose connect-ack is still arriving: take it
 * off the pending list, disarm its events, tell the peer `why` (unless
 * that is PMIX_SUCCESS) and release it - so no event is left registered
 * on a descriptor number that has been given back. Runs on the progress
 * thread, or once that has stopped. */
void pmix_ptl_base_drop_pending_connection(pmix_pending_connection_t *pnd, pmix_status_t why)
{
    uint32_t u32;

    pmix_list_remove_item(&pmix_ptl_base.pending_connections, &pnd->super);
    pmix_event_del(&pnd->ev);
    if (pnd->timer_active) {
        pmix_event_evtimer_del(&pnd->timer);
        pnd->timer_active = false;
    }
    if (PMIX_SUCCESS != why && 0 <= pnd->sd) {
        u32 = htonl((uint32_t) why);
        (void) pmix_ptl_base_send_blocking(pnd->sd, (char *) &u32, sizeof(uint32_t));
    }
    PMIX_RELEASE(pnd);
}

/* ptl_base_connect_ack_timeout expired before a connection delivered its
 * whole connect-ack */
static void connect_ack_expired(int sd, short args, void *cbdata)
{
    pmix_pending_connection_t *pnd = (pmix_pending_connection_t *) cbdata;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_ACQUIRE_OBJECT(pnd);
    pnd->timer_active = false;
    pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                        "ptl:base:listener: connection on socket %d sent %" PRIsize_t
                        " bytes of its connect-ack in %d seconds - dropping it",
                        pnd->sd, pnd->hdr_recvd + pnd->msg_recvd,
                        pmix_ptl_base.connect_ack_timeout);
    pmix_ptl_base_drop_pending_connection(pnd, PMIX_ERR_TIMEOUT);
}

/*
 * Handler for accepting connections from the event library
 */
static void connection_event_handler(int incoming_sd, short flags, void *cbdata)
{
    struct sockaddr_storage addr;
    pmix_socklen_t addrlen = sizeof(addr);
    int sd, fdflags;

    memset(&addr, 0, sizeof(addr));
    pmix_pending_connection_t *pending_connection, *pnd;
    /* the primary listener or one of the alternates - whichever fired */
    pmix_listener_t *lt = (pmix_listener_t *) cbdata;
    PMIX_HIDE_UNUSED_PARAMS(flags);

    /* the address is only printed, but an IPv6 peer's does not fit in a
     * plain struct sockaddr - it was truncated, and then read past the
     * end of the stack variable to print it */
#if defined(HAVE_ACCEPT4) && defined(SOCK_CLOEXEC)
    /* close-on-exec from the moment it exists, where the platform can */
    sd = accept4(incoming_sd, (struct sockaddr *) &addr, &addrlen, SOCK_CLOEXEC);
#else
    sd = accept(incoming_sd, (struct sockaddr *) &addr, &addrlen);
#endif
    pmix_output_verbose(5, pmix_ptl_base_framework.framework_output,
                        "connection_event_handler: working connection "
                        "(%d, %d) %s:%d\n",
                        sd, pmix_socket_errno,
                        pmix_net_get_hostname((struct sockaddr *) &addr),
                        pmix_net_get_port((struct sockaddr *) &addr));
    if (0 <= sd) {
        pmix_ptl_base_own_socket(sd);
    }
    if (sd < 0) {
        pmix_ptl_base_accept_failed(lt, pmix_socket_errno);
        return;
    }
    /* whatever was failing has cleared */
    lt->nretries = 0;
    lt->warned = false;
    pmix_ptl_base_set_nosigpipe(sd);

    /* The connect-ack that follows is read on the progress thread, from a
     * port that anything able to reach it can open, before any credential
     * has been checked - so it is read as its bytes arrive and never waited
     * for. That only works on a socket that says so when it has nothing: an
     * accepted socket inherits the listener's O_NONBLOCK on BSD-derived
     * systems but never does on Linux, and a blocking one parks the progress
     * thread in recv() until the far end sends the rest, which it need never
     * do. So a socket we cannot make non-blocking is not used at all. */
    fdflags = fcntl(sd, F_GETFL, 0);
    if (0 > fdflags || 0 > fcntl(sd, F_SETFL, fdflags | O_NONBLOCK)) {
        pmix_output(0, "connection_event_handler: unable to make socket %d non-blocking: %s (%d)",
                    sd, strerror(pmix_socket_errno), pmix_socket_errno);
        CLOSE_THE_SOCKET(sd);
        return;
    }

    /* this descriptor is ready to be read, which means a connection
     * request has been received - so harvest it. All we want to do
     * here is accept the connection and wait for its connect-ack to
     * arrive - we don't want to actually process the connection here
     * as it takes too long, and so the OS might start rejecting
     * connections due to timeout. The handler runs each time the
     * socket becomes readable until it has the whole connect-ack.
     */
    pending_connection = PMIX_NEW(pmix_pending_connection_t);
    if (NULL == pending_connection) {
        CLOSE_THE_SOCKET(sd);
        PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
        return;
    }
    pending_connection->protocol = lt->protocol;
    pending_connection->transport = lt->transport;
    pending_connection->sd = sd;
    /* where it came from, which decides what may vouch for it */
    memcpy(&pending_connection->addr, &addr, sizeof(addr));

    pmix_output_verbose(8, pmix_ptl_base_framework.framework_output,
                        "connection_event_handler: new connection: (%d, %d)", pending_connection->sd,
                        pmix_socket_errno);

    /* Run the handler only when the peer has sent something. It used to be
     * activated the moment the connection was accepted, and to read the
     * connect-ack with blocking recvs: any process that could reach the
     * listener - or a tool suspended between its connect() and its send -
     * held the progress thread, and with it every client of this server,
     * for as long as it stayed silent. Waiting costs nothing while the peer
     * is idle, and a peer that closes without sending still wakes us, so
     * the handler's error path reclaims the socket. */
    pmix_event_assign(&pending_connection->ev, pmix_globals.evbase,
                      sd, EV_READ,
                      lt->cbfunc, pending_connection);
    /* Track it until its connect-ack is complete, so that one which never
     * finishes is still closed when we finalize - and, unless the timeout
     * is disabled, closed well before that. A peer holding a connection
     * costs nothing but a descriptor, but it costs one for as long as it
     * likes, and anything that can reach the listener can open more. */
    if (0 < pmix_ptl_base.pending_cap &&
        (size_t) pmix_ptl_base.pending_cap <=
            pmix_list_get_size(&pmix_ptl_base.pending_connections)) {
        /* Full. Make room by dropping the connection that has waited
         * longest to send its connect-ack - the list is in accept order.
         * A process that means to connect sends its connect-ack at once,
         * so it is not the one still waiting; refusing the new connection
         * instead would let whoever holds the cap keep everyone else out.
         * Tools waiting on the host are not on this list and are never
         * dropped for it: they have a cap of their own, the same size, at
         * which a further tool is refused */
        pnd = (pmix_pending_connection_t *) pmix_list_get_first(&pmix_ptl_base.pending_connections);
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "ptl:base:listener: %d connections are still sending their "
                            "connect-ack - dropping the oldest, on socket %d",
                            pmix_ptl_base.pending_cap, pnd->sd);
        pmix_ptl_base_drop_pending_connection(pnd, PMIX_ERR_OUT_OF_RESOURCE);
    }
    pmix_ptl_base_await_connect_ack(pending_connection, false);
}

void pmix_ptl_base_await_connect_ack(pmix_pending_connection_t *pnd, bool resume)
{
    pmix_pending_connection_t *item;
    struct timeval tv, now;
    long left = pmix_ptl_base.connect_ack_timeout;

    if (resume) {
        /* the wait goes on from where it was: the timeout runs from the
         * connection's arrival, and it keeps its place by arrival among the
         * connections still waiting - so the oldest is still dropped first */
        if (0 < left) {
            pmix_ptl_base_monotime(&now);
            left -= (long) (now.tv_sec - pnd->waiting.tv_sec);
            if (now.tv_usec < pnd->waiting.tv_usec) {
                ++left;
            }
            if (0 >= left) {
                pmix_list_append(&pmix_ptl_base.pending_connections, &pnd->super);
                pmix_ptl_base_drop_pending_connection(pnd, PMIX_ERR_TIMEOUT);
                return;
            }
        }
        PMIX_LIST_FOREACH (item, &pmix_ptl_base.pending_connections, pmix_pending_connection_t) {
            if (timercmp(&pnd->waiting, &item->waiting, <)) {
                break;
            }
        }
        if (&item->super == pmix_list_get_end(&pmix_ptl_base.pending_connections)) {
            pmix_list_append(&pmix_ptl_base.pending_connections, &pnd->super);
        } else {
            pmix_list_insert_pos(&pmix_ptl_base.pending_connections, &item->super, &pnd->super);
        }
    } else {
        pmix_list_append(&pmix_ptl_base.pending_connections, &pnd->super);
    }
    if (0 < left) {
        pmix_event_evtimer_set(pmix_globals.evbase, &pnd->timer, connect_ack_expired, pnd);
        tv.tv_sec = left;
        tv.tv_usec = 0;
        if (0 == pmix_event_evtimer_add(&pnd->timer, &tv)) {
            pnd->timer_active = true;
        }
    }
    /* post the object */
    PMIX_POST_OBJECT(pnd);
    if (0 != pmix_event_add(&pnd->ev, NULL)) {
        /* nothing will ever service this connection */
        PMIX_ERROR_LOG(PMIX_ERROR);
        pmix_ptl_base_drop_pending_connection(pnd, PMIX_ERROR);
    }
}


/* A rendezvous file is removed by pmix_ptl_close when we finalize, so
 * a server that was killed or that crashed leaves its file behind. As
 * the file records the pid of the process that created it, we can tell
 * a stale file from one belonging to a still-running server - and
 * reclaim the stale one instead of refusing to start.
 *
 * Returns a string describing the live owner of the given file, or
 * NULL if the file has no live owner (i.e., it is stale and can be
 * reclaimed). The caller must free any returned string.
 */
/* in use, though there was no memory to say by whom */
static char inuse_unknown[] = "a running process";

static char *rndz_file_owner(int dirfd, const char *name, bool held)
{
    FILE *fp;
    struct stat buf;
    char *line, *endptr, *owner = NULL, *lock = NULL;
    unsigned long pid = 0;
    bool havepid = false;
    pmix_ptl_lock_state_t state;
    size_t budget = PMIX_PTL_MAX_CONN_FILE;
    int fd, n;

    /* the file holds the uri, the version, the pid of the process
     * that wrote it, its uid:gid, and a timestamp - one per line - and
     * then tagged lines. We only care about the pid, and the line that
     * says its writer holds it locked while it runs.
     *
     * It is read relative to the directory we checked, and not thru a
     * symlink. O_NONBLOCK keeps a FIFO at the name from stalling us in
     * the open, and anything that is not a regular file is not one we
     * wrote. In each case we cannot identify an owner, so treat it as
     * stale - if it truly is in use, then the removal below will fail
     * and we will report that instead */
    fd = openat(dirfd, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (0 > fd) {
        return NULL;
    }
    if (0 != fstat(fd, &buf) || !S_ISREG(buf.st_mode) || PMIX_PTL_MAX_CONN_FILE < buf.st_size) {
        close(fd);
        return NULL;
    }
    fp = fdopen(fd, "r");
    if (NULL == fp) {
        close(fd);
        return NULL;
    }
    for (n = 0; n < 3; n++) {
        line = pmix_ptl_base_getline(fp, &budget);
        if (NULL == line) {
            /* the file was never completely written - e.g., its
             * creator died partway thru doing so */
            break;
        }
        if (2 == n) {
            endptr = NULL;
            errno = 0;
            pid = strtoul(line, &endptr, 10);
            /* the value has to fit in a pid_t. One that does not - a
             * garbled line, or a pid wider than this system's - narrows
             * to some other number, and 4294967295 narrows to -1, which
             * asks kill() about every process we may signal and so
             * always found a "live owner" */
            if (0 == errno && NULL != endptr && endptr != line && 0 < pid &&
                (unsigned long) INT_MAX >= pid && pid == (unsigned long) (pid_t) pid) {
                havepid = true;
            }
        }
        free(line);
    }
    /* and whether its writer holds it locked, on a tagged line further on */
    for (n = 0; havepid && n < PMIX_PTL_MAX_TAG_LINES && NULL != (line = pmix_ptl_base_getline(fp, &budget));
         n++) {
        if (NULL == lock && 0 == strncmp(line, PMIX_PTL_LOCK_TAG, strlen(PMIX_PTL_LOCK_TAG))) {
            lock = strdup(line + strlen(PMIX_PTL_LOCK_TAG));
        }
        free(line);
    }
    /* A writer that holds the lock beside the file is running, whatever
     * its pid says here - which from another pid namespace, as another
     * container sharing this directory, is another process or none. A
     * lock nothing holds - we hold it ourselves already when no other
     * server of this name runs - is not enough to take the file: a lock
     * file a tmp cleaner removed from under a running server is one
     * nothing holds. That, and a file without the line, from an earlier
     * release, are judged by the pid as before */
    state = PMIX_PTL_LOCK_UNKNOWN;
    if (havepid && NULL != lock) {
        state = held ? PMIX_PTL_LOCK_GONE
                     : pmix_ptl_base_lock_state(dirfd, name, buf.st_uid, lock);
    }
    free(lock);
    fclose(fp);
    if (PMIX_PTL_LOCK_HELD == state) {
        if (0 > pmix_asprintf(&owner, "pid %lu, which holds the lock beside the file", pid)) {
            return inuse_unknown;
        }
        return owner;
    }

    if (!havepid) {
        return NULL;
    }
    /* if the recorded pid is our own, then the file cannot belong to
     * a live owner - we did not write it, so it is a leftover from an
     * earlier process that happened to be given the same pid */
    if ((pid_t)pid == pmix_globals.pid) {
        return NULL;
    }
    /* if that process is still running, then the file is in use and
     * we must not touch it. Note that a "permission denied" response
     * means the process does exist, but belongs to another user */
    if (0 != kill((pid_t)pid, 0) && EPERM != errno) {
        return NULL;
    }
    if (0 > pmix_asprintf(&owner, "pid %lu", pid)) {
        return inuse_unknown;
    }
    return owner;
}

typedef struct {
    const char *role;
    const char *filename;
    int dirfd;
    bool held;      // we hold the lock beside the name
    bool reported;
} rndz_reclaim_t;

/* A rendezvous file is already at the name. If a live process owns it,
 * then another server holds this role and we must not disturb it;
 * otherwise it was left behind by an instance that is no longer running,
 * so reclaim it. Either refusal is explained here, and says so in
 * cbdata so the caller does not add a misleading message on top. */
static bool rndz_reclaim(const char *name, void *cbdata)
{
    rndz_reclaim_t *ctx = (rndz_reclaim_t *) cbdata;
    char *owner;

    owner = rndz_file_owner(ctx->dirfd, name, ctx->held);
    if (NULL != owner) {
        pmix_show_help("help-ptl-base.txt", "rndz-file-in-use", true,
                       ctx->role, ctx->filename, owner);
        if (owner != inuse_unknown) {
            free(owner);
        }
        ctx->reported = true;
        return false;
    }
    /* someone else beating us to the removal is fine - anything else
     * is not */
    if (0 != unlinkat(ctx->dirfd, name, 0) && ENOENT != errno) {
        pmix_show_help("help-ptl-base.txt", "rndz-file-stale", true,
                       ctx->role, ctx->filename, strerror(errno));
        ctx->reported = true;
        return false;
    }
    return true;
}

/* On success, *dirfd is left holding a descriptor on the file's
 * directory, which pmix_ptl_close uses to remove the file */
/* The lines that tell a process finding us through a file which wire
 * formats and security mechanisms we accept a connection with - each
 * tagged, after every line a released reader takes by position. Empty
 * when there is nothing to list. The caller frees it */
static char *compat_lines(void)
{
    char *bfrops, *psec, *lines = NULL;

    bfrops = pmix_bfrops_base_get_available_modules();
    psec = pmix_psec_base_get_accepted_modules();
    if (0 > pmix_asprintf(&lines, "%s%s%s%s%s%s",
                          (NULL == bfrops) ? "" : PMIX_PTL_BFROPS_TAG,
                          (NULL == bfrops) ? "" : bfrops,
                          (NULL == bfrops) ? "" : "\n",
                          (NULL == psec) ? "" : PMIX_PTL_PSEC_TAG,
                          (NULL == psec) ? "" : psec,
                          (NULL == psec) ? "" : "\n")) {
        lines = NULL;
    }
    free(bfrops);
    free(psec);
    return lines;
}

/* Whether the directory fd is open on (O_RDONLY) has nothing in it */
static bool dir_is_empty(int fd)
{
    DIR *dp;
    struct dirent *ep;
    bool empty = true;
    int dfd;

    dfd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    if (0 > dfd) {
        return false;
    }
    dp = fdopendir(dfd);
    if (NULL == dp) {
        close(dfd);
        return false;
    }
    while (NULL != (ep = readdir(dp))) {
        if (0 != strcmp(ep->d_name, ".") && 0 != strcmp(ep->d_name, "..")) {
            empty = false;
            break;
        }
    }
    closedir(dp);
    return empty;
}

/* Hold on to a directory we just made, so that finalize can remove it
 * from wherever it has come to be: a descriptor on the directory that
 * holds it, its name there, and one on the directory itself, which keeps
 * it from being replaced by another with the same identity. Only while
 * that name in that parent leads to the directory dirfd is open on, and
 * it is ours and empty - the directory just made. It is given the mode
 * asked for, whatever the umask took from it. Otherwise nothing is held
 * and the directory is left at finalize */
static void hold_made_dir(pmix_ptl_made_dir_t *made, const char *dirname, int dirfd,
                          mode_t mode)
{
    char *clean = NULL, *parent = NULL;
    const char *base;
    struct stat given, there;
    size_t len;
    int pfd = -1, fd = -1;

    if (0 <= made->parentfd) {
        return;
    }
    /* "dir/" names dir: its last component is before the separators */
    clean = strdup(dirname);
    if (NULL == clean) {
        goto leave;
    }
    len = strlen(clean);
    while (1 < len && PMIX_PATH_SEP[0] == clean[len - 1]) {
        clean[--len] = '\0';
    }
    base = strrchr(clean, PMIX_PATH_SEP[0]);
    base = (NULL == base) ? clean : base + 1;
    if ('\0' == base[0] || 0 == strcmp(base, ".") || 0 == strcmp(base, "..") ||
        0 != fstat(dirfd, &given)) {
        goto leave;
    }
    parent = pmix_dirname(clean);
    if (NULL == parent) {
        goto leave;
    }
    pfd = pmix_os_dirpath_open_dir(parent);
    if (0 > pfd) {
        goto leave;
    }
    fd = openat(pfd, base, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (0 > fd || 0 != fstat(fd, &there) ||
        there.st_dev != given.st_dev || there.st_ino != given.st_ino ||
        geteuid() != there.st_uid || !dir_is_empty(fd) ||
        0 != fchmod(fd, mode | (there.st_mode & S_ISGID)) || 0 != fstat(fd, &made->st) ||
        NULL == (made->name = strdup(base)) || NULL == (made->path = strdup(clean))) {
        goto leave;
    }
    made->parentfd = pfd;
    made->fd = fd;
    free(parent);
    free(clean);
    return;

leave:
    free(made->name);
    made->name = NULL;
    if (0 <= fd) {
        close(fd);
    }
    if (0 <= pfd) {
        close(pfd);
    }
    free(parent);
    free(clean);
    pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                        "ptl:base: the directory %s will be left at finalize", dirname);
}

/* Open a directory a server puts its files in, making it with the given
 * mode if it is not there. One this call makes is held in `made`, if
 * given, which pmix_ptl_close removes from wherever it has come to be. The descriptor,
 * close-on-exec; or -1 with *rc saying why - PMIX_ERR_SILENT once
 * reported */
static int open_dir(const char *dirname, mode_t mode, pmix_ptl_made_dir_t *made,
                    pmix_status_t *rc)
{
    int dirfd;

    *rc = pmix_os_dirpath_create(dirname, mode);
    if (PMIX_ERR_SILENT == *rc) {
        // error has already been reported
        return -1;
    }
    if (PMIX_SUCCESS != *rc && PMIX_ERR_EXISTS != *rc) {
        PMIX_ERROR_LOG(*rc);
        return -1;
    }

    /* The directory is the one we were given, and is trusted as such.
     * Open it once and do everything from here on - including the
     * removal at finalize - relative to that descriptor, so the
     * directory a file is created in is the one it is removed from,
     * whatever the name comes to lead to in the meantime */
    dirfd = pmix_os_dirpath_open_dir(dirname);
    if (0 > dirfd) {
        pmix_show_help("help-pmix-util.txt", "mkdir-failed", true,
                       dirname, strerror(errno));
        *rc = PMIX_ERR_SILENT;
        return -1;
    }
    if (PMIX_SUCCESS == *rc && NULL != made) {
        // do not change the created flag if the directory
        // already exists as we don't know if we previously
        // created it or it already existed. Success is returned
        // only when we created the directory ourselves
        made->created = true;
        hold_made_dir(made, dirname, dirfd, mode);
    }
    *rc = PMIX_SUCCESS;
    return dirfd;
}

/* Hold the directory the report file was written in, provided its name
 * there leads to the file we wrote. Otherwise the file is left at
 * finalize */
static void hold_report_file(const struct stat *wrote)
{
    char *dirname;
    const char *base;
    struct stat there;
    int fd;

    if (0 <= pmix_ptl_base.urifile_dirfd) {
        close(pmix_ptl_base.urifile_dirfd);
        pmix_ptl_base.urifile_dirfd = -1;
    }
    base = strrchr(pmix_ptl_base.urifile, PMIX_PATH_SEP[0]);
    base = (NULL == base) ? pmix_ptl_base.urifile : base + 1;
    dirname = pmix_dirname(pmix_ptl_base.urifile);
    fd = (NULL == dirname) ? -1 : pmix_os_dirpath_open_dir(dirname);
    free(dirname);
    if (0 <= pmix_ptl_base.urifile_fd) {
        close(pmix_ptl_base.urifile_fd);
        pmix_ptl_base.urifile_fd = -1;
    }
    /* and one on the file, so that its identity stays its own */
    if (0 <= fd) {
        pmix_ptl_base.urifile_fd = openat(fd, base, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    }
    if (0 > fd || 0 > pmix_ptl_base.urifile_fd ||
        0 != fstat(pmix_ptl_base.urifile_fd, &there) ||
        there.st_dev != wrote->st_dev || there.st_ino != wrote->st_ino) {
        if (0 <= fd) {
            close(fd);
        }
        if (0 <= pmix_ptl_base.urifile_fd) {
            close(pmix_ptl_base.urifile_fd);
            pmix_ptl_base.urifile_fd = -1;
        }
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "ptl:base: the report file %s will be left at finalize",
                            pmix_ptl_base.urifile);
        return;
    }
    pmix_ptl_base.urifile_st = *wrote;
    pmix_ptl_base.urifile_dirfd = fd;
}

/* Let go of the lock beside a rendezvous file we did not write after all,
 * and remove it */
static void release_lock(int dirfd, const char *base, int lockfd)
{
    char *lname;

    if (0 > lockfd) {
        return;
    }
    if (NULL != (lname = pmix_ptl_base_lock_name(base))) {
        (void) unlinkat(dirfd, lname, 0);
        free(lname);
    }
    close(lockfd);
}

static pmix_status_t write_rndz_file(char *filename, char *uri, const char *role, bool optional,
                                     pmix_ptl_made_dir_t *made, bool *file_created,
                                     int *dirfd_out)
{
    int fd, dirfd;
    char *dirname, *tmp, *compat;
    const char *base;
    time_t mytime;
    bool locked;
    int rc, lockfd = -1;
    size_t n;
    rndz_reclaim_t reclaim = {.role = role, .filename = filename, .dirfd = -1,
                              .held = false, .reported = false};
    mode_t mode;
    struct stat buf;

    base = strrchr(filename, PMIX_PATH_SEP[0]);
    base = (NULL == base) ? filename : base + 1;
    if ('\0' == base[0]) {
        /* the name ends in a separator, so it names no file */
        pmix_output(0, "Rendezvous file %s names a directory, not a file\n", filename);
        return PMIX_ERR_BAD_PARAM;
    }
    dirname = pmix_dirname(filename);
    if (NULL == dirname) {
        return PMIX_ERR_NOMEM;
    }

    /* The mode applies only if we have to create the directory. One
     * that is already there - normally the tmpdir we were given - is
     * used as found: its permissions belong to whoever named it */
    mode = S_IRWXU;
    if (pmix_ptl_base.allow_foreign_tools) {
        mode |= S_IXGRP | S_IRGRP | S_IXOTH | S_IROTH;
    }
    dirfd = open_dir(dirname, mode, made, &rc);
    if (0 > dirfd) {
        free(dirname);
        return rc;
    }
    /* a directory we were given may keep other users out, in which
     * case a foreign tool cannot reach the file however we mark it.
     * That is the directory owner's call, but say so for whoever is
     * wondering why their tool cannot find us */
    if (pmix_ptl_base.allow_foreign_tools &&
        0 == fstat(dirfd, &buf) &&
        (S_IXGRP | S_IXOTH) != (buf.st_mode & (S_IXGRP | S_IXOTH))) {
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "ptl:base: %s rendezvous directory %s has mode %04o - "
                            "tools under other user IDs may be unable to reach it",
                            role, dirname, (unsigned) (buf.st_mode & 07777));
    }
    free(dirname);
    reclaim.dirfd = dirfd;
    /* the lock beside the file, held for as long as we run, so that a
     * process that finds the file can tell whether we still do - whatever
     * pid namespace it is in. The kernel lets go of it however we exit */
    lockfd = -1;
    if (PMIX_PTL_NLOCKS > pmix_ptl_base.nrndz_lockfd) {
        lockfd = pmix_ptl_base_take_lock(dirfd, base);
    }
    if (PMIX_PTL_LOCK_BUSY == lockfd) {
        /* another server of this name runs. A name with our pid in it
         * collides only with one in another pid namespace, and is one
         * of several we write: that one is let go */
        close(dirfd);
        *file_created = false;
        if (optional) {
            pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                                "ptl:base: %s rendezvous file %s is another server's - not "
                                "writing it", role, filename);
            return PMIX_ERR_EXISTS;
        }
        pmix_show_help("help-ptl-base.txt", "rndz-name-held", true, role, filename,
                       "the lock beside the file");
        return PMIX_ERR_SILENT;
    }
    reclaim.held = (0 <= lockfd);

    /* set the file mode */
    mode = S_IRUSR | S_IWUSR ;
    if (pmix_ptl_base.allow_foreign_tools) {
        mode |= S_IRGRP | S_IROTH;
    }
    /* if the file already exists, we get one chance to reclaim it as
     * stale and try the create again */
    fd = pmix_os_dirpath_create_file_at(dirfd, base, O_RDWR, mode, rndz_reclaim, &reclaim);
    if (0 > fd) {
        *file_created = false;
        release_lock(dirfd, base, lockfd);
        close(dirfd);
        if (reclaim.reported) {
            return PMIX_ERR_SILENT;
        }
        if (EEXIST == errno) {
            /* another process recreated the file while we were
             * reclaiming it */
            pmix_show_help("help-ptl-base.txt", "rndz-file-in-use", true,
                           role, filename, "another process");
            return PMIX_ERR_SILENT;
        }
        pmix_output(0, "Impossible to open the file %s in write mode\n", filename);
        PMIX_ERROR_LOG(PMIX_ERR_FILE_OPEN_FAILURE);
        return PMIX_ERR_FILE_OPEN_FAILURE;
    }

    /* output the information */
    mytime = time(NULL);
    /* The five lines every release reads by position come first and are
     * unchanged; our other addresses, wire formats and security
     * mechanisms follow them, tagged, where no older reader looks. The
     * addresses cannot go in the URI itself: every released parser
     * refuses a URI carrying more than one address, and a client refused
     * that way quietly runs as a singleton. */
    compat = compat_lines();
    locked = (0 <= lockfd);
    if (0 > pmix_asprintf(&tmp, "%s\n%s\n%lu\n%lu:%lu\n%s\n%s%s%s%s%s%s%s%s%s%s%s%s",
                          uri, PMIX_VERSION, (unsigned long)pmix_globals.pid,
                          (unsigned long)pmix_globals.uid,
                          (unsigned long)pmix_globals.gid,
                          ctime(&mytime),
                          (NULL == pmix_ptl_base.alt_uris) ? "" : PMIX_PTL_ALT_URIS_TAG,
                          (NULL == pmix_ptl_base.alt_uris) ? "" : pmix_ptl_base.alt_uris,
                          (NULL == pmix_ptl_base.alt_uris) ? "" : "\n",
                          (NULL == pmix_ptl_base.local_listener.uri) ? "" : PMIX_PTL_LOCAL_URI_TAG,
                          (NULL == pmix_ptl_base.local_listener.uri) ? "" : pmix_globals.hostname,
                          (NULL == pmix_ptl_base.local_listener.uri) ? "" : ";",
                          (NULL == pmix_ptl_base.local_listener.uri) ? ""
                                                                     : pmix_ptl_base.local_listener.uri,
                          (NULL == pmix_ptl_base.local_listener.uri) ? "" : "\n",
                          (NULL == compat) ? "" : compat,
                          locked ? PMIX_PTL_LOCK_TAG : "", locked ? PMIX_PTL_LOCK_FLOCK : "",
                          locked ? "\n" : "")) {
        free(compat);
        /* nothing has been written, so do not leave an empty file behind
         * for a peer to read as a server that died partway thru */
        close(fd);
        unlinkat(dirfd, base, 0);
        release_lock(dirfd, base, lockfd);
        close(dirfd);
        *file_created = false;
        return PMIX_ERR_NOMEM;
    }
    free(compat);
    /* a short write leaves a file a peer will read as truncated, which
     * is indistinguishable from the creator having died partway thru
     * writing it - so treat it exactly like a failure */
    rc = write(fd, tmp, strlen(tmp));
    if (0 > rc || (size_t) rc != strlen(tmp)) {
        PMIX_ERROR_LOG(PMIX_ERR_FILE_WRITE_FAILURE);
        *file_created = false;
        pmix_free(tmp);
        close(fd);
        unlinkat(dirfd, base, 0);
        release_lock(dirfd, base, lockfd);
        close(dirfd);
        return PMIX_ERR_FILE_WRITE_FAILURE;
    }
    pmix_free(tmp);
    /* which file we wrote, so that finalize removes this one and no other
     * now at the name - and the lock beside it */
    n = pmix_ptl_base.nrndz_lockfd;
    if (PMIX_PTL_NLOCKS > n && 0 == fstat(fd, &buf) &&
        NULL != (pmix_ptl_base.rndz_lockname[n] = strdup(filename))) {
        pmix_ptl_base.rndz_dev[n] = buf.st_dev;
        pmix_ptl_base.rndz_ino[n] = buf.st_ino;
        pmix_ptl_base.rndz_lockfd[n] = lockfd;
        ++pmix_ptl_base.nrndz_lockfd;
    } else {
        /* with no name to remove it by at finalize, let go of it now:
         * the file is then judged by its pid */
        release_lock(dirfd, base, lockfd);
    }
    close(fd);
    if (0 <= *dirfd_out) {
        close(*dirfd_out);
    }
    *dirfd_out = dirfd;
    *file_created = true;
    return PMIX_SUCCESS;
}

/* Replace a string global from a directive. These come straight from the
 * host, and the key says what the value should be, never what it is: a
 * value of any other type was read out of the union as a pointer and
 * handed to strdup. Returns false, with *rc set, if the directive cannot
 * be used. */
bool pmix_ptl_base_replace_string(const pmix_info_t *info, char **target, int *rc)
{
    char *copy;

    if (PMIX_STRING != info->value.type || NULL == info->value.data.string) {
        pmix_output(0, "ptl:base:setup_listener: directive %s must be a non-NULL PMIX_STRING, "
                    "not %s", info->key, PMIx_Data_type_string(info->value.type));
        *rc = PMIX_ERR_BAD_PARAM;
        return false;
    }
    copy = strdup(info->value.data.string);
    if (NULL == copy) {
        *rc = PMIX_ERR_NOMEM;
        return false;
    }
    free(*target);
    *target = copy;
    return true;
}

size_t pmix_ptl_base_waiting_tools(void)
{
    return pmix_list_get_size(&pmix_ptl_base.tool_queue) +
           pmix_list_get_size(&pmix_ptl_base.tool_asking);
}

pmix_status_t pmix_ptl_base_listen(pmix_info_t info[], size_t ninfo, pmix_listener_t *lt)
{
    pmix_ptl_base_active_t *active;

    PMIX_LIST_FOREACH (active, &pmix_ptl_base.actives, pmix_ptl_base_active_t) {
        if (!active->module->local && NULL != active->module->setup_listener) {
            lt->transport = active->module;
            return active->module->setup_listener(info, ninfo, lt);
        }
    }
    return PMIX_ERR_NOT_SUPPORTED;
}

char *pmix_ptl_base_local_uri_value(void)
{
    char *value = NULL;

    if (NULL == pmix_ptl_base.local_listener.uri) {
        return NULL;
    }
    if (0 > pmix_asprintf(&value, "%s;%lu:%lu", pmix_ptl_base.local_listener.uri,
                          (unsigned long) pmix_ptl_base.local_uid,
                          (unsigned long) pmix_ptl_base.local_gid)) {
        return NULL;
    }
    return value;
}

pmix_status_t pmix_ptl_base_listen_local(pmix_info_t info[], size_t ninfo, pmix_listener_t *lt)
{
    pmix_ptl_base_active_t *active;

    PMIX_LIST_FOREACH (active, &pmix_ptl_base.actives, pmix_ptl_base_active_t) {
        if (active->module->local && NULL != active->module->setup_listener) {
            lt->transport = active->module;
            return active->module->setup_listener(info, ninfo, lt);
        }
    }
    return PMIX_ERR_NOT_SUPPORTED;
}

/* store our URI under the given key in our own datastore */
static pmix_status_t store_uri(const char *key, const char *uri)
{
    pmix_kval_t *urikv;
    pmix_status_t rc;

    PMIX_KVAL_NEW(urikv, key);
    if (NULL == urikv || NULL == urikv->value) {
        if (NULL != urikv) {
            PMIX_RELEASE(urikv);
        }
        return PMIX_ERR_NOMEM;
    }
    rc = PMIx_Value_load(urikv->value, uri, PMIX_STRING);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(urikv);
        return rc;
    }
    PMIX_GDS_STORE_KV(rc, pmix_globals.mypeer, &pmix_globals.myid, PMIX_INTERNAL, urikv);
    PMIX_RELEASE(urikv); // maintain accounting
    return rc;
}

/* discover the available
 * interfaces, filter them thru any given directives, and select
 * the one we will listen on for connection requests. This will
 * be a loopback device by default, unless we are asked to support
 * tool connections - in that case, we will take a non-loopback
 * device by default, if one is available after filtering directives
 *
 * If we are a tool and were given a rendezvous file, then we first
 * check to see if it already exists. If it does, then this is the
 * connection info we are to use. If it doesn't, then this is the
 * name of the file we are to use to store our listener info.
 *
 * If we are a server and are given a rendezvous file, then that is
 * is the name of the file we are to use to store our listener info.
 */
pmix_status_t pmix_ptl_base_setup_listener(pmix_info_t info[], size_t ninfo)
{
    pmix_listener_t *lt, *llt = NULL;
    int rc = 0;
    pid_t mypid;
    int outpipe;
    char *leftover, *uri;
    size_t n;
    FILE *fptst;
    const char *legacy_from = "ptl_base_legacy_peers";

    pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                        "ptl:base:setup_listener");

    pmix_ptl_base.legacy_peers = pmix_ptl_base.legacy_peers_param;
    for (n = 0; n < ninfo; n++) {
        if (0 == strcmp(info[n].key, PMIX_SERVER_SESSION_SUPPORT)) {
            pmix_ptl_base.session_tool = PMIX_INFO_TRUE(&info[n]);

        } else if (PMIX_CHECK_KEY(&info[n], PMIX_SERVER_SYSTEM_SUPPORT)) {
            pmix_ptl_base.system_tool = PMIX_INFO_TRUE(&info[n]);

        } else if (PMIX_CHECK_KEY(&info[n], PMIX_SERVER_ALLOW_FOREIGN_TOOLS)) {
            pmix_ptl_base.allow_foreign_tools = PMIX_INFO_TRUE(&info[n]);

        } else if (0 == strcmp(info[n].key, PMIX_SERVER_TOOL_SUPPORT)) {
            pmix_ptl_base.tool_support = PMIX_INFO_TRUE(&info[n]);

        } else if (PMIX_CHECK_KEY(&info[n], PMIX_SERVER_REMOTE_CONNECTIONS)) {
            pmix_ptl_base.remote_connections = PMIX_INFO_TRUE(&info[n]);
            pmix_ptl_base.connections_specified = true;
#if !PMIX_ENABLE_REMOTE_TOOLS
            if (pmix_ptl_base.remote_connections) {
                pmix_show_help("help-ptl-base.txt", "remote-tools-not-built", true,
                               pmix_globals.hostname);
                return PMIX_ERR_SILENT;
            }
#endif

        } else if (PMIX_CHECK_KEY(&info[n], PMIX_SERVER_LEGACY_PEERS)) {
            pmix_ptl_base.legacy_peers = PMIX_INFO_TRUE(&info[n]);
            legacy_from = PMIX_SERVER_LEGACY_PEERS;

        } else if (PMIX_CHECK_KEY(&info[n], PMIX_TCP_REPORT_URI)) {
            if (!pmix_ptl_base_replace_string(&info[n], &pmix_ptl_base.report_uri, &rc)) {
                return rc;
            }

        } else if (PMIX_CHECK_KEY(&info[n], PMIX_SERVER_TMPDIR)) {
            if (!pmix_ptl_base_replace_string(&info[n], &pmix_ptl_base.session_tmpdir, &rc)) {
                return rc;
            }

        } else if (PMIX_CHECK_KEY(&info[n], PMIX_SYSTEM_TMPDIR)) {
            if (!pmix_ptl_base_replace_string(&info[n], &pmix_ptl_base.system_tmpdir, &rc)) {
                return rc;
            }
        }
    }

#if !PMIX_ENABLE_LEGACY_PEERS
    if (pmix_ptl_base.legacy_peers) {
        pmix_show_help("help-ptl-base.txt", "legacy-peers-not-built", true,
                       pmix_globals.hostname, legacy_from);
        return PMIX_ERR_SILENT;
    }
#else
    PMIX_HIDE_UNUSED_PARAMS(legacy_from);
#endif

    /* a tool on another host proves who it is with a credential made
     * there, which only some mechanisms validate */
    if (pmix_ptl_base.remote_connections && !pmix_psec_base_remote_available()) {
        leftover = pmix_psec_base_get_accepted_modules();
        pmix_show_help("help-ptl-base.txt", "remote-no-psec", true, pmix_globals.hostname,
                       (NULL == leftover) ? "none" : leftover);
        free(leftover);
        return PMIX_ERR_SILENT;
    }

    lt = &pmix_ptl_base.listener;
    lt->varname = pmix_bfrops_base_get_components();
    lt->protocol = PMIX_PROTOCOL_V2;
    lt->cbfunc = pmix_ptl_base_connection_handler;

    /* The TCP transport opens its socket and says where it is - only for
     * those who use it: peers of earlier releases, and tools on other
     * hosts. A process of this release on this host uses the local one */
    if (pmix_ptl_base.legacy_peers || pmix_ptl_base.remote_connections) {
        rc = pmix_ptl_base_listen(info, ninfo, lt);
        if (PMIX_SUCCESS != rc) {
            goto sockerror;
        }
    }

    /* and the local transport its own, which every process of this
     * release on this host connects to - there is no running without it */
    llt = &pmix_ptl_base.local_listener;
    llt->varname = strdup(lt->varname);
    llt->protocol = PMIX_PROTOCOL_V2;
    llt->cbfunc = pmix_ptl_base_connection_handler;
    rc = pmix_ptl_base_listen_local(info, ninfo, llt);
    if (PMIX_SUCCESS != rc) {
        goto sockerror;
    }
    pmix_ptl_base.local_uid = geteuid();
    pmix_ptl_base.local_gid = getegid();

    /* Where we are, as we report it: the TCP address when we listen on
     * one - a reader of an earlier release, or on another host, can use
     * nothing else, and one of this release on this host finds the local
     * socket beside it - and the local socket's when we do not */
    uri = (NULL != lt->uri) ? lt->uri : llt->uri;

    /* save the URI internally so we can report it - and a legacy copy
     * for older tools */
    rc = store_uri(PMIX_MYSERVER_URI, uri);
    if (PMIX_SUCCESS == rc) {
        rc = store_uri(PMIX_SERVER_URI, uri);
    }
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto sockerror;
    }
    /* and where else a remote tool may reach us */
    if (NULL != pmix_ptl_base.alt_uris) {
        rc = store_uri(PMIX_MYSERVER_ALT_URIS, pmix_ptl_base.alt_uris);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto sockerror;
        }
    }

    /* an empty value names nowhere to report to */
    if (NULL != pmix_ptl_base.report_uri && '\0' != pmix_ptl_base.report_uri[0]) {
        /* if the string is a "-", then output to stdout */
        if (0 == strcmp(pmix_ptl_base.report_uri, "-")) {
            fprintf(stdout, "%s\n", uri);
        } else if (0 == strcmp(pmix_ptl_base.report_uri, "+")) {
            /* output to stderr */
            fprintf(stderr, "%s\n", uri);
        } else {
            /* see if it is an integer pipe. It is one only if there were
             * digits and nothing after them: strtol consumes nothing of
             * an empty value and leaves no leftover, so "" used to be
             * read as descriptor 0 - and stdin was written and closed */
            leftover = NULL;
            outpipe = strtol(pmix_ptl_base.report_uri, &leftover, 10);
            if (NULL != leftover && leftover != pmix_ptl_base.report_uri &&
                '\0' == *leftover) {
                /* stitch together the var names and URI */
                if (0 > pmix_asprintf(&leftover, "%s;%s", lt->varname, uri)) {
                    rc = PMIX_ERR_NOMEM;
                    goto sockerror;
                }
                /* output to the pipe */
                rc = pmix_fd_write_nosigpipe(outpipe, strlen(leftover) + 1, leftover);
                free(leftover);
                close(outpipe);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    goto sockerror;
                }
            } else {
                /* must be a file - a regular one with a single name. A
                 * symlink at the name is not followed, and the file is
                 * opened without O_TRUNC so that nothing is emptied until
                 * it has passed that check */
                FILE *fp = NULL;
                struct stat sbuf;
                int fd;
                /* anyone may read it; only we write it */
                fd = pmix_os_dirpath_open_file(pmix_ptl_base.report_uri,
                                               O_WRONLY | O_CREAT,
                                               S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
                if (0 <= fd) {
                    if (0 != fstat(fd, &sbuf) || !S_ISREG(sbuf.st_mode) ||
                        1 != sbuf.st_nlink || 0 != ftruncate(fd, 0) ||
                        NULL == (fp = fdopen(fd, "w"))) {
                        close(fd);
                        fp = NULL;
                    }
                }
                if (NULL == fp) {
                    pmix_output(0, "Impossible to open the file %s in write mode\n",
                                pmix_ptl_base.report_uri);
                    PMIX_ERROR_LOG(PMIX_ERR_FILE_OPEN_FAILURE);
                    rc = PMIX_ERR_FILE_OPEN_FAILURE;
                    goto sockerror;
                }
                /* output my nspace and rank plus the URI */
                fprintf(fp, "%s\n", uri);
                /* record our version - a connecting peer uses it to decide
                 * whether it can talk to us */
                fprintf(fp, "v%s\n", PMIX_VERSION);
                /* and, after everything an older reader looks at, the
                 * other addresses we listen on */
                if (NULL != pmix_ptl_base.alt_uris) {
                    fprintf(fp, "%s%s\n", PMIX_PTL_ALT_URIS_TAG, pmix_ptl_base.alt_uris);
                }
                /* the local socket, for a tool on this host - a report
                 * file may be read on another */
                if (NULL != pmix_ptl_base.local_listener.uri) {
                    fprintf(fp, "%s%s;%s\n", PMIX_PTL_LOCAL_URI_TAG, pmix_globals.hostname,
                            pmix_ptl_base.local_listener.uri);
                }
                /* and the wire formats and security mechanisms we accept */
                leftover = compat_lines();
                if (NULL != leftover) {
                    fprintf(fp, "%s", leftover);
                    free(leftover);
                }
                fclose(fp);
                /* record the name of the file we actually wrote, which is
                 * what pmix_ptl_close removes. The open took its copy from
                 * the MCA parameter, before a PMIX_TCP_REPORT_URI directive
                 * could replace it - so a file named by the directive was
                 * left behind, and one named by the parameter removed
                 * though this run never wrote it */
                leftover = strdup(pmix_ptl_base.report_uri);
                if (NULL == leftover) {
                    rc = PMIX_ERR_NOMEM;
                    goto sockerror;
                }
                free(pmix_ptl_base.urifile);
                pmix_ptl_base.urifile = leftover;
                pmix_ptl_base.created_urifile = true;
                /* and hold the directory it is in, with the file it is,
                 * so that finalize removes this file from there */
                hold_report_file(&sbuf);
            }
        }
    }

    /* if we were given a rendezvous file, then drop it */
    if (NULL != pmix_ptl_base.rendezvous_filename) {
        /* if we are a tool and the file already exists, then we
         * just use it as providing the rendezvous info for our
         * server */
        if (PMIX_PEER_IS_TOOL(pmix_globals.mypeer)) {
            fptst = fopen(pmix_ptl_base.rendezvous_filename, "r");
            if (NULL != fptst) {
                fclose(fptst);
                goto nextstep;
            }
        }
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "WRITING RENDEZVOUS FILE %s", pmix_ptl_base.rendezvous_filename);
        rc = write_rndz_file(pmix_ptl_base.rendezvous_filename, uri,
                             "server", false,
                             &pmix_ptl_base.rendezvous_dir,
                             &pmix_ptl_base.created_rendezvous_file,
                             &pmix_ptl_base.rendezvous_dirfd);
        if (PMIX_SUCCESS != rc) {
            goto sockerror;
        }
    }

nextstep:
    /* if we are the scheduler, then drop an appropriately named
     * contact file so the system's resource manager can find us */
    if (PMIX_PEER_IS_SCHEDULER(pmix_globals.mypeer)) {
        if (0 > pmix_asprintf(&pmix_ptl_base.scheduler_filename, "%s/pmix.sched.%s",
                         pmix_ptl_base.system_tmpdir, pmix_globals.hostname)) {
            rc = PMIX_ERR_NOMEM;
            goto sockerror;
        }
        rc = write_rndz_file(pmix_ptl_base.scheduler_filename, uri,
                             "scheduler", false,
                             &pmix_ptl_base.system_dir,
                             &pmix_ptl_base.created_scheduler_filename,
                             &pmix_ptl_base.scheduler_dirfd);
        if (PMIX_SUCCESS != rc) {
            goto sockerror;
        }

    } else if (PMIX_PEER_IS_SYS_CTRLR(pmix_globals.mypeer)) {
        /* if we are the system controller, then drop an appropriately named
         * contact file so others can find us */
            if (0 > pmix_asprintf(&pmix_ptl_base.sysctrlr_filename, "%s/pmix.sysctrlr.%s",
                         pmix_ptl_base.system_tmpdir, pmix_globals.hostname)) {
            rc = PMIX_ERR_NOMEM;
            goto sockerror;
        }
        rc = write_rndz_file(pmix_ptl_base.sysctrlr_filename, uri,
                             "system controller", false,
                             &pmix_ptl_base.system_dir,
                             &pmix_ptl_base.created_sysctrlr_filename,
                             &pmix_ptl_base.sysctrlr_dirfd);
        if (PMIX_SUCCESS != rc) {
            goto sockerror;
        }
    } else if (pmix_ptl_base.tool_support) {
        /* if we are going to support tools, then drop contact file(s) */
        if (pmix_ptl_base.system_tool) {
            if (0 > pmix_asprintf(&pmix_ptl_base.system_filename, "%s/pmix.sys.%s",
                             pmix_ptl_base.system_tmpdir, pmix_globals.hostname)) {
                rc = PMIX_ERR_NOMEM;
                goto sockerror;
            }
            rc = write_rndz_file(pmix_ptl_base.system_filename, uri,
                                 "system tool", false,
                                 &pmix_ptl_base.system_dir,
                                 &pmix_ptl_base.created_system_filename,
                                 &pmix_ptl_base.system_dirfd);
            if (PMIX_SUCCESS != rc) {
                goto sockerror;
            }
        }

        if (pmix_ptl_base.session_tool) {
            /* first output to a std file */
            if (0 > pmix_asprintf(&pmix_ptl_base.session_filename, "%s/pmix.%s.tool",
                             pmix_ptl_base.session_tmpdir, pmix_globals.hostname)) {
                rc = PMIX_ERR_NOMEM;
                goto sockerror;
            }
            pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                                "WRITING SESSION TOOL FILE %s", pmix_ptl_base.session_filename);
            rc = write_rndz_file(pmix_ptl_base.session_filename, uri,
                                 "session tool", false,
                                 NULL,
                                 &pmix_ptl_base.created_session_filename,
                                 &pmix_ptl_base.session_dirfd);
            if (PMIX_SUCCESS != rc) {
                goto sockerror;
            }
        }

        /* now output to a file based on pid */
        mypid = getpid();
        if (0 > pmix_asprintf(&pmix_ptl_base.pid_filename, "%s/pmix.%s.tool.%d",
                         pmix_ptl_base.session_tmpdir, pmix_globals.hostname, mypid)) {
            rc = PMIX_ERR_NOMEM;
            goto sockerror;
        }
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output, "WRITING PID TOOL FILE %s",
                            pmix_ptl_base.pid_filename);
        rc = write_rndz_file(pmix_ptl_base.pid_filename, uri,
                             "tool (by pid)", true,
                             NULL,
                             &pmix_ptl_base.created_pid_filename,
                             &pmix_ptl_base.pid_dirfd);
        if (PMIX_ERR_EXISTS == rc) {
            rc = PMIX_SUCCESS;
        } else if (PMIX_SUCCESS != rc) {
            goto sockerror;
        }

        /* now output it into a file based on my nspace */
        if (0 > pmix_asprintf(&pmix_ptl_base.nspace_filename, "%s/pmix.%s.tool.%s",
                         pmix_ptl_base.session_tmpdir, pmix_globals.hostname,
                         pmix_globals.myid.nspace)) {
            rc = PMIX_ERR_NOMEM;
            goto sockerror;
        }
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "WRITING NSPACE TOOL FILE %s", pmix_ptl_base.nspace_filename);
        rc = write_rndz_file(pmix_ptl_base.nspace_filename, uri,
                             "tool (by namespace)", false,
                             NULL,
                             &pmix_ptl_base.created_nspace_filename,
                             &pmix_ptl_base.nspace_dirfd);
        if (PMIX_SUCCESS != rc) {
            goto sockerror;
        }
    }

    return PMIX_SUCCESS;

sockerror:
    CLOSE_THE_SOCKET(lt->socket);
    if (NULL != llt && 0 <= llt->socket) {
        CLOSE_THE_SOCKET(llt->socket);
        llt->socket = -1;
    }
    return rc;
}
