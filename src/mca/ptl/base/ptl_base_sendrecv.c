/*
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014      Artem Y. Polyakov <artpol84@gmail.com>.
 *                         All rights reserved.
 * Copyright (c) 2015-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
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

#include "src/class/pmix_pointer_array.h"
#include "src/client/pmix_client_ops.h"
#include "src/include/pmix_globals.h"
#include "src/mca/psensor/psensor.h"
#include "src/server/pmix_server_ops.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_name_fns.h"
#include "src/util/pmix_show_help.h"

#include "src/mca/ptl/base/base.h"

/* How long pmix_ptl_base_flush_sends() is willing to wait on a peer that is
 * not draining its socket: at most MAX_RETRIES waits of RETRY_USEC each.
 * Small on purpose - the messages this path exists for are a few tens of
 * bytes on a socket the peer is about to read to EOF, so a full kernel
 * buffer here means the peer is wedged, not busy. */
#define PMIX_PTL_FLUSH_MAX_RETRIES 10
#define PMIX_PTL_FLUSH_RETRY_USEC  1000

/* The aggregation window to use for a report that cannot aggregate: deliver
 * on the next pass of the event loop rather than a second from now. */
static struct timeval report_now = {0, 0};

static void _notify_complete(pmix_status_t status, void *cbdata)
{
    (void) status;
    pmix_event_chain_t *chain = (pmix_event_chain_t *) cbdata;
    PMIX_RELEASE(chain);
}

/* Hand a sendrecv that will never be answered the empty buffer its
 * callback treats as "no reply", so a caller blocked on it unwinds. */
static void deliver_empty(pmix_peer_t *peer, pmix_ptl_tag_t tag,
                          pmix_ptl_cbfunc_t cbfunc, void *cbdata)
{
    pmix_buffer_t buf;
    pmix_ptl_hdr_t hdr;

    PMIX_CONSTRUCT(&buf, pmix_buffer_t);
    /* must set the buffer type so it doesn't fail in unpack */
    if (NULL != peer && NULL != peer->nptr) {
        buf.type = peer->nptr->compat.type;
    } else {
        buf.type = pmix_globals.mypeer->nptr->compat.type;
    }
    memset(&hdr, 0, sizeof(hdr));
    hdr.tag = tag;
    cbfunc(pmix_globals.mypeer, &hdr, &buf, cbdata);
    PMIX_DESTRUCT(&buf);
}

/* A connection has dropped, so any sendrecv still waiting on a reply from
 * that peer will never get one. Complete each with an empty buffer, and
 * take it off the list - a dynamic-tag recv is one-shot, and its callback
 * has now fired.
 *
 * Only this peer's recvs. A tool can be connected to more than one server
 * at once and switch which one is its primary, so the recvs on the list
 * can be waiting on several peers. Completing the others as well answers
 * requests whose connection is still up - and when their real reply then
 * arrives it matches the recv again and runs the callback a second time,
 * on a caddy the first run released. Leaving this peer's recvs posted is
 * the same fault in another form: nothing else ever removes them, and
 * each holds a pointer to a peer that may be freed and its address reused.
 *
 * The reserved tags below PMIX_PTL_TAG_DYNAMIC hold persistent recvs - the
 * notification, IOF and IOF flow control handlers - and nobody is waiting
 * on those. Handing one of them this empty buffer would only make it fail
 * to unpack a message that was never sent. The recvs are moved to a list
 * of our own before any callback runs, so a callback cannot disturb the
 * walk. */
static void complete_recvs(pmix_peer_t *peer)
{
    pmix_ptl_posted_recv_t *rcv, *rnext;
    pmix_list_t done;

    PMIX_CONSTRUCT(&done, pmix_list_t);
    PMIX_LIST_FOREACH_SAFE (rcv, rnext, &pmix_ptl_base.posted_recvs, pmix_ptl_posted_recv_t) {
        if (PMIX_PTL_TAG_DYNAMIC <= rcv->tag && UINT_MAX != rcv->tag &&
            peer == rcv->peer) {
            pmix_list_remove_item(&pmix_ptl_base.posted_recvs, &rcv->super);
            pmix_list_append(&done, &rcv->super);
        }
    }
    while (NULL != (rcv = (pmix_ptl_posted_recv_t *) pmix_list_remove_first(&done))) {
        if (NULL != rcv->cbfunc) {
            deliver_empty(peer, rcv->tag, rcv->cbfunc, rcv->cbdata);
        }
        PMIX_RELEASE(rcv);
    }
    PMIX_DESTRUCT(&done);
}

static void lost_connection(pmix_peer_t *peer)
{
    pmix_ptl_recv_t *msg;

    /* stop all events */
    if (peer->recv_ev_active) {
        pmix_event_del(&peer->recv_event);
        peer->recv_ev_active = false;
    }
    if (peer->send_ev_active) {
        pmix_event_del(&peer->send_event);
        peer->send_ev_active = false;
    }
    /* Detach the message this peer was part-way through reading, but do
     * NOT release it here.
     *
     * That message holds a reference on this peer, taken in
     * pmix_ptl_base_recv_handler when the message was created, and it can
     * be the last one - a peer whose only remaining holder is the partial
     * message it was reading is exactly the peer that turns up in a
     * teardown.  Releasing it at this point would run rdes, drop that
     * reference, and free the peer; the assignment that used to follow was
     * then a write into freed memory, and so was every use of peer in the
     * rest of this function.
     *
     * Holding a reference of our own instead does not work, because the
     * tail of this function hands the peer to pmix_server_peer_finalized(),
     * which is an ownership transfer and not a balanced release - the
     * comment there says the call must be the last use of the object.  So
     * let the message's reference do exactly what a reference is for: keep
     * the peer alive until we are finished with it, and release it at the
     * very end. */
    msg = peer->recv_msg;
    peer->recv_msg = NULL;
    CLOSE_THE_SOCKET(peer->sd);
    if (PMIX_PEER_IS_SERVER(pmix_globals.mypeer)) {

        if (!PMIX_PEER_IS_TOOL(pmix_globals.mypeer)) {
            /* Account for the loss of this client in any local collective it
             * was participating in - the fence/connect/disconnect family and
             * the group family are tracked on separate lists, so each has its
             * own entry point. Note that the proc would not have been added
             * to any collective tracker until after it successfully
             * connected.
             *
             * Both routines account only for an ABNORMAL termination. A peer
             * that dropped its connection by calling PMIx_Finalize has not
             * left the collectives' accounting - the rank remains expected,
             * exactly as the untouched nlocalprocs below says it is - and
             * both routines return without doing anything for it. See
             * pmix_server_trk_peer_lost() and issue #4113. */
            pmix_server_trk_peer_lost(peer);
            pmix_server_grp_peer_lost(peer);

            /* if the peer simply died without finalizing,
             * then reduce the number of local procs, and reduce the
             * rank's live-process count so a later clone recycle decision
             * remains correct (the finalize path decrements proc_cnt in
             * pmix_server_peer_finalized; the abnormal path must too) */
            if (!peer->finalized) {
                if (0 < peer->nptr->nlocalprocs) {
                    --peer->nptr->nlocalprocs;
                }
                if (0 < peer->info->proc_cnt) {
                    --peer->info->proc_cnt;
                }
            }
        }

        /* purge any notifications cached for this client.  Here the
         * connection really did drop, so that is what anyone parked on
         * this peer's data is told. */
        pmix_server_purge_events(peer, NULL, PMIX_ERR_LOST_CONNECTION);

        /* Ask which peer went away, not what I am. A launcher - prun, and
         * anything else that passes PMIX_LAUNCHER to PMIx_tool_init - is a
         * tool with an upstream server AND a server with downstream peers of
         * its own, because PMIX_PROC_LAUNCHER carries PMIX_PROC_SERVER. So
         * "am I a server" cannot distinguish the two directions, and testing
         * it here got both of them wrong: losing a tool of my own would
         * declare me disconnected from my server, while losing my server
         * would leave every sendrecv I had outstanding on it unanswered -
         * the finalize sync among them, so a launcher paid its full
         * finalize guard timer on every departure whose reply went astray.
         * That recovery lives in the client arm below, which a launcher
         * never reaches. */
        if (peer == pmix_client_globals.myserver) {
            pmix_atomic_unset_bool(&pmix_globals.connected);
        } else if (!PMIX_PEER_IS_TOOL(peer)) {
            /* cleanup any sensors that are monitoring them */
            pmix_psensor.stop(peer, NULL);
        }
        complete_recvs(peer);
        if (!pmix_globals.mypeer->finalized && !peer->finalized) {
            /* if this peer already called finalize, then
             * we are just seeing their connection go away
             * when they terminate - so do not generate
             * an event. If an abnormal termination, then we do.
             *
             * Which peer it was decides whether the aggregation window is
             * worth waiting out.  Losing a client is precisely the cascade
             * the window exists for: a job that dies drops all of its
             * peers at once, and one event naming them all is the point.
             * Losing my own server is not - I have exactly one, so that
             * chain can never gain a second source and the window is a
             * second of delay in exchange for nothing. */
            PMIX_REPORT_EVENT_WINDOW(PMIX_ERR_LOST_CONNECTION, peer,
                                     PMIX_RANGE_PROC_LOCAL, _notify_complete,
                                     (peer == pmix_client_globals.myserver)
                                         ? &report_now
                                         : &pmix_globals.event_window);
        }

        /* if a local peer finalized cleanly and its socket has now dropped,
         * retire its peer object so it does not leak until the nspace is
         * deregistered. This MUST be the last use of peer here: the object
         * may be freed. The callers only issue a write barrier afterward
         * (they never dereference peer), so doing it inline is safe. */
        if (peer->finalized && PMIX_PEER_IS_CLIENT(peer) && !PMIX_PEER_IS_TOOL(peer)) {
            /* a pure client is tombstoned - reclaimed on reconnect or
             * namespace deregistration */
            pmix_server_peer_finalized(peer);
        } else if (peer->finalized && PMIX_PEER_IS_TOOL(peer) && !PMIX_PEER_IS_CLIENT(peer)) {
            /* a pure tool is freed outright: nothing resolves a tool through
             * info->peerid, so it needs no tombstone, and it never reconnects
             * onto the same rank (each tool init is assigned a fresh nspace),
             * so leaving it would leak one peer per init/finalize cycle. A
             * tool the host registered as a client keeps the client tombstone
             * path above and is reclaimed by its tool reconnect (or namespace
             * deregistration) instead. */
            pmix_server_peer_finalized(peer);
        }

    } else {
        /* if this was the server to which I am connected,
         * then we need to exit */
        if (peer == pmix_client_globals.myserver) {
            pmix_atomic_unset_bool(&pmix_globals.connected);
        }
        /* and whichever server it was, nothing it owed us is coming. A
         * tool attached to several servers loses one that is not its
         * primary through here too, and used to leave every request
         * outstanding on it waiting forever */
        complete_recvs(peer);
        /* if I called finalize, then don't generate an event.
         *
         * No aggregation window here: this report names my one and only
         * server, so nothing can ever join it.  Waiting the window out is
         * what made a tool whose whole job is to watch for this - pterm,
         * which orders a DVM down and then waits for the connection to
         * drop as proof it went - spend a full second of its ~1.02 s
         * runtime waiting for an event that was ready immediately. */
        if (peer == pmix_client_globals.myserver && !pmix_globals.mypeer->finalized) {
            PMIX_REPORT_EVENT_WINDOW(PMIX_ERR_LOST_CONNECTION,
                                     pmix_client_globals.myserver,
                                     PMIX_RANGE_PROC_LOCAL, _notify_complete,
                                     &report_now);
        }
    }

    /* Now let the in-flight message go.  This is the last statement that can
     * touch the peer: the release runs rdes, which drops the message's
     * reference, and where that was the last one the peer is freed right
     * here - which is why nothing below may use it. */
    if (NULL != msg) {
        PMIX_RELEASE(msg);
    }
}

static pmix_status_t send_msg(int sd, pmix_ptl_send_t *msg)
{
    struct iovec iov[2];
    int iov_count;
    size_t nbytes = ntohl(msg->hdr.nbytes), want, cap, done;
    ssize_t rc;

    /* No single writev may carry more than max_write bytes. A message can
     * be up to 4 GB, and macOS refuses a writev totalling more than
     * INT_MAX with EINVAL rather than writing part of it - which dropped
     * the connection. A capped write that goes out whole is not the kernel
     * buffer filling, so keep going; only a short write waits for the
     * socket to become writable again. */
    cap = (0 < pmix_ptl_base.max_write) ? pmix_ptl_base.max_write : (size_t) INT_MAX;

    while (1) {
        /* what is left of the current region, plus the payload if the
         * header is still going out */
        iov[0].iov_base = msg->sdptr;
        iov[0].iov_len = (msg->sdbytes < cap) ? msg->sdbytes : cap;
        want = iov[0].iov_len;
        iov_count = 1;
        if (!msg->hdr_sent && NULL != msg->data && 0 < nbytes && want < cap) {
            iov[1].iov_base = msg->data->base_ptr;
            iov[1].iov_len = (nbytes < cap - want) ? nbytes : cap - want;
            want += iov[1].iov_len;
            iov_count = 2;
        }

        rc = writev(sd, iov, iov_count);
        if (rc < 0) {
            if (pmix_socket_errno == EINTR) {
                continue;
            } else if (pmix_socket_errno == EAGAIN) {
                /* tell the caller to keep this message on active,
                 * but let the event lib cycle so other messages
                 * can progress while this socket is busy
                 */
                return PMIX_ERR_RESOURCE_BUSY;
            } else if (pmix_socket_errno == EWOULDBLOCK) {
                /* tell the caller to keep this message on active,
                 * but let the event lib cycle so other messages
                 * can progress while this socket is busy
                 */
                return PMIX_ERR_WOULD_BLOCK;
            }
            /* we hit an error and cannot progress this message */
            pmix_output(0, "pmix_ptl_base: send_msg: write failed: %s (%d) [sd = %d]",
                        strerror(pmix_socket_errno), pmix_socket_errno, sd);
            return PMIX_ERR_UNREACH;
        }

        /* account for what went out */
        if ((size_t) rc < msg->sdbytes) {
            /* part of the current region - header or payload */
            msg->sdptr = (char *) msg->sdptr + rc;
            msg->sdbytes -= (size_t) rc;
        } else if (!msg->hdr_sent) {
            /* the rest of the header, and perhaps some of the payload */
            done = (size_t) rc - msg->sdbytes;
            msg->hdr_sent = true;
            msg->sdptr = (NULL == msg->data) ? NULL : (char *) msg->data->base_ptr + done;
            msg->sdbytes = nbytes - done;
        } else {
            /* the rest of the payload */
            msg->sdptr = (char *) msg->sdptr + rc;
            msg->sdbytes = 0;
        }

        if (msg->hdr_sent && 0 == msg->sdbytes) {
            /* we successfully sent the header and the msg data if any */
            return PMIX_SUCCESS;
        }
        if ((size_t) rc < want) {
            /* short writev. This usually means the kernel buffer is full,
             * so there is no point for retrying at that time */
            return PMIX_ERR_RESOURCE_BUSY;
        }
    }
}

/* Write out everything already queued for this peer, right now.
 *
 * A peer's queued sends normally drain from its send_event, which cannot
 * fire until the event base next polls. That is fine while the peer is
 * alive, and wrong at the moment we are about to close its socket in an
 * orderly teardown: the bytes sitting on send_msg are a reply we owe that
 * peer, and closing the socket under them discards the reply with no notice
 * to either side. The peer is then left waiting for an answer that was
 * packed, queued and thrown away - which for the finalize reply means
 * waiting out the client's or tool's finalize guard timer, seconds spent
 * for nothing on every clean departure.
 *
 * Bounded and best-effort by design: the socket is non-blocking, so a peer
 * that has stopped reading gets a short grace period and then loses the
 * remainder. Truncating a message to a peer that is not draining it is much
 * cheaper than letting that peer stall the progress thread, which is what
 * an unbounded wait here would do.
 */
void pmix_ptl_base_flush_sends(pmix_peer_t *peer)
{
    pmix_ptl_send_t *msg;
    pmix_status_t rc;
    int retries;
    fd_set wfds;
    struct timeval tv;

    if (NULL == peer || peer->sd < 0) {
        return;
    }

    /* stop the send event: we are draining by hand, and leaving it armed
     * would let it fire against a socket that is about to be closed */
    if (peer->send_ev_active) {
        pmix_event_del(&peer->send_event);
        peer->send_ev_active = false;
    }

    while (1) {
        if (NULL == peer->send_msg) {
            peer->send_msg = (pmix_ptl_send_t *) pmix_list_remove_first(&peer->send_queue);
            if (NULL == peer->send_msg) {
                break;
            }
        }
        msg = peer->send_msg;
        retries = 0;
        while (PMIX_SUCCESS != (rc = send_msg(peer->sd, msg))) {
            if ((PMIX_ERR_RESOURCE_BUSY != rc && PMIX_ERR_WOULD_BLOCK != rc) ||
                PMIX_PTL_FLUSH_MAX_RETRIES <= retries) {
                /* the peer is unreachable, or is not reading fast enough to
                 * be worth waiting on - drop this message and everything
                 * queued behind it */
                pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                                    "%s ptl:base:flush_sends giving up on sd %d: %s",
                                    PMIX_NAME_PRINT(&pmix_globals.myid), peer->sd,
                                    PMIx_Error_string(rc));
                PMIX_RELEASE(msg);
                peer->send_msg = NULL;
                while (NULL != (msg = (pmix_ptl_send_t *)
                                pmix_list_remove_first(&peer->send_queue))) {
                    PMIX_RELEASE(msg);
                }
                return;
            }
            /* the kernel buffer is full - give it a moment to drain. select
             * is used rather than poll because sys/select.h is what PMIx
             * already requires of the platform.
             *
             * An fd_set holds only descriptors below FD_SETSIZE, and FD_SET
             * does not check: a larger one writes past the end of the set,
             * on the stack. A server hosting a few hundred local procs has
             * descriptors well past 1024, so such a socket just waits out
             * the interval - select with no sets is a portable sleep. */
            ++retries;
            tv.tv_sec = 0;
            tv.tv_usec = PMIX_PTL_FLUSH_RETRY_USEC;
            if (FD_SETSIZE > peer->sd) {
                FD_ZERO(&wfds);
                FD_SET(peer->sd, &wfds);
                select(peer->sd + 1, NULL, &wfds, NULL, &tv);
            } else {
                select(0, NULL, NULL, NULL, &tv);
            }
        }
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "%s ptl:base:flush_sends MSG SENT on sd %d tag %u",
                            PMIX_NAME_PRINT(&pmix_globals.myid), peer->sd,
                            ntohl(msg->hdr.tag));
        PMIX_RELEASE(msg);
        peer->send_msg = NULL;
    }
}

static pmix_status_t read_bytes(int sd, char **buf, size_t *remain)
{
    pmix_status_t ret = PMIX_SUCCESS;
    ssize_t rc;
    size_t chunk;
    char *ptr = *buf;

    /* read until all bytes recvd or error */
    while (0 < *remain) {
        /* a message may be up to 4 GB, and macOS refuses a read() of more
         * than INT_MAX bytes with EINVAL rather than returning a short
         * count - which would drop the connection */
        chunk = (INT_MAX < *remain) ? (size_t) INT_MAX : *remain;
        rc = read(sd, ptr, chunk);
        if (rc < 0) {
            if (pmix_socket_errno == EINTR) {
                continue;
            } else if (pmix_socket_errno == EAGAIN) {
                /* tell the caller to keep this message on active,
                 * but let the event lib cycle so other messages
                 * can progress while this socket is busy
                 */
                ret = PMIX_ERR_RESOURCE_BUSY;
                goto exit;
            } else if (pmix_socket_errno == EWOULDBLOCK) {
                /* tell the caller to keep this message on active,
                 * but let the event lib cycle so other messages
                 * can progress while this socket is busy
                 */
                ret = PMIX_ERR_WOULD_BLOCK;
                goto exit;
            }
            /* we hit an error and cannot progress this message - let
             * the caller know to abort it
             */
            pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                                "pmix_ptl_base_msg_recv: read failed: %s (%d)",
                                strerror(pmix_socket_errno), pmix_socket_errno);
            ret = PMIX_ERR_UNREACH;
            goto exit;
        } else if (0 == rc) {
            /* the remote peer closed the connection */
            ret = PMIX_ERR_UNREACH;
            goto exit;
        }
        /* we were able to read something, so adjust counters and location */
        *remain -= (size_t) rc;
        ptr += rc;
    }
    /* we read the full data block */
exit:
    *buf = ptr;
    return ret;
}

/*
 * A file descriptor is available/ready for send. Check the state
 * of the socket and take the appropriate action.
 */
void pmix_ptl_base_send_handler(int sd, short flags, void *cbdata)
{
    pmix_peer_t *peer = (pmix_peer_t *) cbdata;
    pmix_ptl_send_t *msg = peer->send_msg;
    pmix_status_t rc;
    PMIX_HIDE_UNUSED_PARAMS(sd, flags);

    /* acquire the object */
    PMIX_ACQUIRE_OBJECT(peer);

    pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                        "%s ptl:base:send_handler SENDING TO PEER %s tag %u with %s msg",
                        PMIX_NAME_PRINT(&pmix_globals.myid), PMIX_PNAME_PRINT(&peer->info->pname),
                        (NULL == msg) ? UINT_MAX : ntohl(msg->hdr.tag),
                        (NULL == msg) ? "NULL" : "NON-NULL");

    if (NULL != msg) {
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "ptl:base:send_handler SENDING MSG TO %s TAG %u",
                            PMIX_PNAME_PRINT(&peer->info->pname), ntohl(msg->hdr.tag));
        if (PMIX_SUCCESS == (rc = send_msg(peer->sd, msg))) {
            // message is complete
            pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                                "ptl:base:send_handler MSG SENT");
            PMIX_RELEASE(msg);
            peer->send_msg = NULL;
        } else if (PMIX_ERR_RESOURCE_BUSY == rc || PMIX_ERR_WOULD_BLOCK == rc) {
            /* exit this event and let the event lib progress */
            pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                                "ptl:base:send_handler RES BUSY OR WOULD BLOCK");
            /* ensure we post the modified peer object before another thread
             * picks it back up */
            PMIX_POST_OBJECT(peer);
            return;
        } else {
            pmix_output_verbose(5, pmix_ptl_base_framework.framework_output, "%s SEND ERROR %s",
                                PMIX_NAME_PRINT(&pmix_globals.myid), PMIx_Error_string(rc));
            // report the error
            pmix_event_del(&peer->send_event);
            peer->send_ev_active = false;
            PMIX_RELEASE(msg);
            peer->send_msg = NULL;
            lost_connection(peer);
            /* ensure we post the modified peer object before another thread
             * picks it back up */
            PMIX_POST_OBJECT(peer);
            return;
        }

        /* if current message completed - progress any pending sends by
         * moving the next in the queue into the "on-deck" position. Note
         * that this doesn't mean we send the message right now - we will
         * wait for another send_event to fire before doing so. This gives
         * us a chance to service any pending recvs.
         */
        peer->send_msg = (pmix_ptl_send_t *) pmix_list_remove_first(&peer->send_queue);
    }

    /* if nothing else to do unregister for send event notifications */
    if (NULL == peer->send_msg && peer->send_ev_active) {
        pmix_event_del(&peer->send_event);
        peer->send_ev_active = false;
    }
    /* ensure we post the modified peer object before another thread
     * picks it back up */
    PMIX_POST_OBJECT(peer);
}

/*
 * Dispatch to the appropriate action routine based on the state
 * of the connection with the peer.
 */

void pmix_ptl_base_recv_handler(int sd, short flags, void *cbdata)
{
    pmix_status_t rc;
    pmix_peer_t *peer = (pmix_peer_t *) cbdata;
    pmix_ptl_recv_t *msg = NULL;
    PMIX_HIDE_UNUSED_PARAMS(flags);

    /* acquire the object */
    PMIX_ACQUIRE_OBJECT(peer);

    pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                        "%s ptl:base:recv:handler called with peer %s:%u",
                        PMIX_NAME_PRINT(&pmix_globals.myid),
                        (NULL == peer) ? "NULL" : peer->info->pname.nspace,
                        (NULL == peer) ? PMIX_RANK_UNDEF : peer->info->pname.rank);

    if (NULL == peer) {
        return;
    }
    /* allocate a new message and setup for recv */
    if (NULL == peer->recv_msg) {
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "ptl:base:recv:handler allocate new recv msg");
        peer->recv_msg = PMIX_NEW(pmix_ptl_recv_t);
        if (NULL == peer->recv_msg) {
            pmix_output(0, "ptl:base:recv_handler: unable to allocate recv message\n");
            goto err_close;
        }
        PMIX_RETAIN(peer);
        peer->recv_msg->peer = peer; // provide a handle back to the peer object
        /* start by reading the header */
        peer->recv_msg->rdptr = (char *) &peer->recv_msg->hdr;
        peer->recv_msg->rdbytes = sizeof(pmix_ptl_hdr_t);
    }
    msg = peer->recv_msg;
    msg->sd = sd;
    /* if the header hasn't been completely read, read it
     *
     * Straight into the message, through the cursor set up above, so a
     * header that arrives in pieces resumes where it stopped. It used to
     * be read into a local copy that each call started afresh, which threw
     * away whatever part had arrived before the socket ran dry - and from
     * then on every header was read from the wrong offset in the stream.
     * A sender produces exactly that split when its own kernel buffer
     * fills part-way through a header (see send_msg). */
    if (!msg->hdr_recvd) {
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "ptl:base:recv:handler read hdr on socket %d", peer->sd);
        if (PMIX_SUCCESS == (rc = read_bytes(peer->sd, &msg->rdptr, &msg->rdbytes))) {
            /* completed reading the header */
            peer->recv_msg->hdr_recvd = true;
            /* convert the hdr to host format */
            peer->recv_msg->hdr.pindex = ntohl(peer->recv_msg->hdr.pindex);
            peer->recv_msg->hdr.tag = ntohl(peer->recv_msg->hdr.tag);
            peer->recv_msg->hdr.nbytes = ntohl(peer->recv_msg->hdr.nbytes);
            pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                                "%s RECVD MSG FROM %s FOR TAG %d SIZE %d",
                                PMIX_NAME_PRINT(&pmix_globals.myid),
                                PMIX_PNAME_PRINT(&peer->info->pname), (int) peer->recv_msg->hdr.tag,
                                (int) peer->recv_msg->hdr.nbytes);
            /* if this is a zero-byte message, then we are done */
            if (0 == peer->recv_msg->hdr.nbytes) {
                pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                                    "%s RECVD ZERO-BYTE MESSAGE FROM %s for tag %d",
                                    PMIX_NAME_PRINT(&pmix_globals.myid),
                                    PMIX_PNAME_PRINT(&peer->info->pname), peer->recv_msg->hdr.tag);
                peer->recv_msg->data = NULL; // make sure
                peer->recv_msg->rdptr = NULL;
                peer->recv_msg->rdbytes = 0;
                /* post it for delivery */
                PMIX_ACTIVATE_POST_MSG(peer->recv_msg);
                peer->recv_msg = NULL;
                PMIX_POST_OBJECT(peer);
                return;
            } else {
                pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                                    "ptl:base:recv:handler allocate data region of size %lu",
                                    (unsigned long) peer->recv_msg->hdr.nbytes);
                /* allocate the data region */
                if (pmix_ptl_base.max_msg_size < peer->recv_msg->hdr.nbytes) {
                    pmix_show_help("help-pmix-runtime.txt", "ptl:msg_size", true,
                                   (unsigned long) peer->recv_msg->hdr.nbytes,
                                   (unsigned long) pmix_ptl_base.max_msg_size);
                    goto err_close;
                }
                /* The size asked for came off the wire. It is bounded by
                 * max_msg_size above, but that ceiling is the taint limit
                 * when the parameter says "no limit", so this can still
                 * be a request for as much as a uint32_t can name - and
                 * an allocation that large is entitled to fail. */
                peer->recv_msg->data = (char *) malloc(peer->recv_msg->hdr.nbytes);
                if (NULL == peer->recv_msg->data) {
                    pmix_output(0, "ptl:base:recv_handler: cannot allocate %lu bytes "
                                "for a message from %s",
                                (unsigned long) peer->recv_msg->hdr.nbytes,
                                PMIX_PNAME_PRINT(&peer->info->pname));
                    goto err_close;
                }
                memset(peer->recv_msg->data, 0, peer->recv_msg->hdr.nbytes);
                /* point to it */
                peer->recv_msg->rdptr = peer->recv_msg->data;
                peer->recv_msg->rdbytes = peer->recv_msg->hdr.nbytes;
            }
            /* fall thru and attempt to read the data */
        } else if (PMIX_ERR_RESOURCE_BUSY == rc || PMIX_ERR_WOULD_BLOCK == rc) {
            /* exit this event and let the event lib progress */
            return;
        } else {
            /* the remote peer closed the connection - report that condition
             * and let the caller know
             */
            pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                                "%s ptl:base:msg_recv: peer %s closed connection",
                                PMIX_NAME_PRINT(&pmix_globals.myid),
                                PMIX_PNAME_PRINT(&peer->info->pname));
            goto err_close;
        }
    }

    if (peer->recv_msg->hdr_recvd) {
        /* continue to read the data block - we start from
         * wherever we left off, which could be at the
         * beginning or somewhere in the message
         */
        if (PMIX_SUCCESS == (rc = read_bytes(peer->sd, &msg->rdptr, &msg->rdbytes))) {
            /* we recvd all of the message */
            pmix_output_verbose(
                2, pmix_ptl_base_framework.framework_output,
                "%s:%d RECVD COMPLETE MESSAGE FROM SERVER OF %d BYTES FOR TAG %d ON PEER SOCKET %d",
                pmix_globals.myid.nspace, pmix_globals.myid.rank, (int) peer->recv_msg->hdr.nbytes,
                peer->recv_msg->hdr.tag, peer->sd);
            /* post it for delivery */
            PMIX_ACTIVATE_POST_MSG(peer->recv_msg);
            peer->recv_msg = NULL;
            /* ensure we post the modified peer object before another thread
             * picks it back up */
            PMIX_POST_OBJECT(peer);
            return;
        } else if (PMIX_ERR_RESOURCE_BUSY == rc || PMIX_ERR_WOULD_BLOCK == rc) {
            /* exit this event and let the event lib progress */
            /* ensure we post the modified peer object before another thread
             * picks it back up */
            PMIX_POST_OBJECT(peer);
            return;
        } else {
            /* the remote peer closed the connection - report that condition
             * and let the caller know
             */
            pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                                "%s:%d ptl:base:msg_recv: peer %s:%d closed connection",
                                pmix_globals.myid.nspace, pmix_globals.myid.rank,
                                peer->nptr->nspace, peer->info->pname.rank);
            goto err_close;
        }
    }
    /* success */
    return;

err_close:
    /* Everything this used to do before calling lost_connection - stop the
     * two events, release peer->recv_msg - lost_connection does itself, on
     * exactly the same object, as its first three statements.  Doing it
     * here as well was not merely redundant: releasing peer->recv_msg drops
     * the reference it holds on this peer (taken above, when the message
     * was created), so this teardown could free the peer and then hand the
     * dangling pointer to lost_connection.  Just call it. */
    lost_connection(peer);
    /* ensure we post the modified peer object before another thread
     * picks it back up.  lost_connection may have freed the peer, which is
     * why this is a write barrier and not a dereference. */
    PMIX_POST_OBJECT(peer);
}

/* Deliver a message to ourselves, bypassing the socket we do not have.
 *
 * A one-way loopback (kind NONE) matches like anything off a socket. A
 * sendrecv to ourselves is harder, because the request and its reply carry
 * the same tag. The request is posted AFTER the recv for its reply, and a
 * dynamic-tag recv is prepended, so the request would match that recv
 * first and be handed to the reply callback as if it were the answer. And
 * the reply, arriving where nothing waits for it, would fall through to a
 * server's wildcard recv and be read as a command - the error that draws
 * is itself a reply to ourselves, and the two go round forever. So
 * pmix_ptl_base_process_msg keeps them apart: a REQUEST never matches a
 * dynamic-tag recv, and a REPLY never matches the wildcard.
 */
pmix_status_t pmix_ptl_base_post_loopback(struct pmix_peer_t *pr, pmix_ptl_tag_t tag,
                                          pmix_buffer_t *buf, pmix_ptl_loopback_t kind)
{
    pmix_peer_t *peer = (pmix_peer_t *) pr;
    pmix_ptl_recv_t *msg;

    msg = PMIX_NEW(pmix_ptl_recv_t);
    if (NULL == msg) {
        return PMIX_ERR_NOMEM;
    }
    PMIX_RETAIN(peer);
    msg->peer = peer;
    msg->hdr.pindex = pmix_globals.pindex;
    msg->hdr.tag = tag;
    msg->loopback = kind;
    if (NULL != buf) {
        msg->hdr.nbytes = buf->bytes_used;
        msg->data = buf->base_ptr;
        buf->base_ptr = NULL;
        buf->bytes_used = 0;
        PMIX_RELEASE(buf);
    }
    PMIX_ACTIVATE_POST_MSG(msg);
    return PMIX_SUCCESS;
}

void pmix_ptl_base_send(int sd, short args, void *cbdata)
{
    pmix_ptl_queue_t *queue = (pmix_ptl_queue_t *) cbdata;
    pmix_ptl_send_t *snd;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    /* acquire the object */
    PMIX_ACQUIRE_OBJECT(queue);

    if (NULL == queue->peer || NULL == queue->peer->info || NULL == queue->peer->nptr) {
        /* we don't know this peer */
        if (NULL != queue->buf) {
            PMIX_RELEASE(queue->buf);
        }
        PMIX_RELEASE(queue);
        return;
    }

    pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                        "[%s:%d] send to %s:%u of size %u on tag %d", __FILE__, __LINE__,
                        (queue->peer)->info->pname.nspace, (queue->peer)->info->pname.rank,
                        (NULL == queue->buf) ? 0 : (unsigned) queue->buf->bytes_used, (queue->tag));

    if (NULL == queue->buf) {
        /* nothing to send? */
        PMIX_RELEASE(queue);
        return;
    }

    if (PMIX_PTL_MSG_TOO_BIG(queue->buf)) {
        /* see PMIX_PTL_MSG_TOO_BIG - nobody waits on a one-way send, so
         * refusing it is all there is to do, but say so */
        PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
        PMIX_RELEASE(queue->buf);
        PMIX_RELEASE(queue);
        return;
    }

    /* is this a send to myself? */
    if (queue->peer == pmix_globals.mypeer) {
        /* just push it to the matching code */
        if (PMIX_SUCCESS != pmix_ptl_base_post_loopback(queue->peer, queue->tag, queue->buf,
                                                        PMIX_PTL_LOOPBACK_NONE)) {
            goto nomem;
        }
        queue->buf = NULL;
        PMIX_RELEASE(queue);
        return;
    }

    /* do we have a live connection? */
    if (queue->peer->sd < 0) {
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output, "%s no connection",
                            PMIX_NAME_PRINT(&pmix_globals.myid));
        if (NULL != queue->buf) {
            PMIX_RELEASE(queue->buf);
        }
        PMIX_RELEASE(queue);
        return;
    }

    snd = PMIX_NEW(pmix_ptl_send_t);
    if (NULL == snd) {
        goto nomem;
    }
    snd->hdr.pindex = htonl(pmix_globals.pindex);
    snd->hdr.tag = htonl(queue->tag);
    snd->hdr.nbytes = htonl((queue->buf)->bytes_used);
    snd->data = (queue->buf);
    /* always start with the header */
    snd->sdptr = (char *) &snd->hdr;
    snd->sdbytes = sizeof(pmix_ptl_hdr_t);

    /* if there is no message on-deck, put this one there */
    if (NULL == (queue->peer)->send_msg) {
        (queue->peer)->send_msg = snd;
    } else {
        /* add it to the queue */
        pmix_list_append(&(queue->peer)->send_queue, &snd->super);
    }
    /* Ensure the send event is active.
     *
     * Only claim it is if it really became so. A peer is on
     * pmix_server_globals.clients and has its sd from the moment the
     * connection is accepted, but its send_event is not assigned a base
     * until the handshake completes - so a send posted in that window
     * reaches here, passes the sd test above, and finds an event
     * libevent will not take ("event_add: event has no event_base
     * set"). Recording that as active is what makes the failure
     * permanent rather than transient: the flag says an event will
     * drain this peer, no event ever fires, and this message and every
     * one queued behind it sit on send_msg for the life of the peer.
     * A collective involving it then never completes and nothing says
     * why.
     *
     * Leaving the flag false keeps the message queued and lets the next
     * send - or the connection completing, which activates a pending
     * one - try again against an event that by then has a base. */
    if (!(queue->peer)->send_ev_active) {
        PMIX_POST_OBJECT(queue->peer);
        if (0 == pmix_event_add(&(queue->peer)->send_event, 0)) {
            (queue->peer)->send_ev_active = true;
        }
    }
    PMIX_RELEASE(queue);
    PMIX_POST_OBJECT(snd);
    return;

nomem:
    /* a one-way send has nobody waiting on it, so dropping it is all
     * there is to do - but say so */
    PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
    PMIX_RELEASE(queue->buf);
    PMIX_RELEASE(queue);
}

void pmix_ptl_base_send_recv(int fd, short args, void *cbdata)
{
    pmix_ptl_sr_t *ms = (pmix_ptl_sr_t *) cbdata;
    pmix_ptl_posted_recv_t *req = NULL;
    pmix_ptl_send_t *snd;
    uint32_t tag;
    PMIX_HIDE_UNUSED_PARAMS(fd, args);

    /* acquire the object */
    PMIX_ACQUIRE_OBJECT(ms);

    if (NULL == ms->peer || NULL == ms->peer->info || NULL == ms->peer->nptr ||
        (ms->peer != pmix_globals.mypeer && ms->peer->sd < 0)) {
        /* this peer has lost connection - or lost it after the caller
         * checked, which is the race that matters. The caller is waiting
         * on this callback, blocked if the API is, so it must still be
         * answered: with the empty buffer a sendrecv outstanding when a
         * connection drops gets, which every callback already reads as
         * "no reply". Dropping the request silently left it waiting
         * forever. */
        goto unanswered;
    }

    if (ms->peer == pmix_globals.mypeer && !PMIX_PEER_IS_SERVER(pmix_globals.mypeer)) {
        /* a request to ourselves is answered by our own server half - a
         * process without one has nobody to read it, and would wait on
         * the reply forever */
        goto unanswered;
    }

    if (NULL == ms->bfr) {
        /* nothing to send? */
        PMIX_RELEASE(ms);
        return;
    }

    if (PMIX_PTL_MSG_TOO_BIG(ms->bfr)) {
        /* see PMIX_PTL_MSG_TOO_BIG - the caller is waiting, so answer it */
        PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
        goto unanswered;
    }

    /* take the next tag in the sequence of tags for this peer */
    ms->peer->dyn_tags_current++;
    if (ms->peer->dyn_tags_current == ms->peer->dyn_tags_end) {
        ms->peer->dyn_tags_current = ms->peer->dyn_tags_start;
    }
    tag = ms->peer->dyn_tags_current;

    if (NULL != ms->cbfunc) {
        /* if a callback msg is expected, setup a recv for it */
        req = PMIX_NEW(pmix_ptl_posted_recv_t);
        if (NULL == req) {
            goto unanswered;
        }
        req->peer = ms->peer;
        req->tag = tag;
        req->cbfunc = ms->cbfunc;
        req->cbdata = ms->cbdata;

        pmix_output_verbose(5, pmix_ptl_base_framework.framework_output,
                            "posting recv on tag %d",
                            req->tag);
        /* add it to the list of recvs - ahead of any wildcard recv, so
         * the reply is matched here rather than read as a command */
        pmix_list_prepend(&pmix_ptl_base.posted_recvs, &req->super);
    }

    pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                        "QUEUEING MSG TO SERVER %s ON SOCKET %d OF SIZE %d",
                        PMIX_PNAME_PRINT(&ms->peer->info->pname), ms->peer->sd,
                        (int) ms->bfr->bytes_used);

    /* is this a send to myself? The request goes to our own command
     * handler, and its reply comes back through PMIX_SERVER_QUEUE_REPLY to
     * the recv just posted - see pmix_ptl_base_post_loopback for how the
     * two are kept apart, since they carry the same tag */
    if (ms->peer == pmix_globals.mypeer) {
        if (PMIX_SUCCESS != pmix_ptl_base_post_loopback(ms->peer, tag, ms->bfr,
                                                        PMIX_PTL_LOOPBACK_REQUEST)) {
            goto unposted;
        }
        ms->bfr = NULL;
        PMIX_RELEASE(ms);
        return;
    }

    snd = PMIX_NEW(pmix_ptl_send_t);
    if (NULL == snd) {
        goto unposted;
    }
    snd->hdr.pindex = htonl(pmix_globals.pindex);
    snd->hdr.tag = htonl(tag);
    snd->hdr.nbytes = htonl(ms->bfr->bytes_used);
    snd->data = ms->bfr;
    /* always start with the header */
    snd->sdptr = (char *) &snd->hdr;
    snd->sdbytes = sizeof(pmix_ptl_hdr_t);

    /* if there is no message on-deck, put this one there */
    if (NULL == ms->peer->send_msg) {
        ms->peer->send_msg = snd;
    } else {
        /* add it to the queue */
        pmix_list_append(&ms->peer->send_queue, &snd->super);
    }
    /* ensure the send event is active - see the note on the same
     * sequence in pmix_ptl_base_send() for why the flag follows the
     * event rather than leading it */
    if (!ms->peer->send_ev_active) {
        PMIX_POST_OBJECT(snd);
        if (0 == pmix_event_add(&ms->peer->send_event, 0)) {
            ms->peer->send_ev_active = true;
        }
    }

    /* cleanup */
    PMIX_RELEASE(ms);
    PMIX_POST_OBJECT(snd);
    return;

unposted:
    /* the message cannot be sent, so take back the recv posted for its
     * reply before answering the caller ourselves */
    if (NULL != req) {
        pmix_list_remove_item(&pmix_ptl_base.posted_recvs, &req->super);
        PMIX_RELEASE(req);
    }
unanswered:
    pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                        "%s ptl:base:send_recv could not send to %s",
                        PMIX_NAME_PRINT(&pmix_globals.myid),
                        (NULL == ms->peer) ? "NULL" : PMIX_PEER_PRINT(ms->peer));
    if (NULL != ms->bfr) {
        PMIX_RELEASE(ms->bfr);
    }
    if (NULL != ms->cbfunc) {
        deliver_empty(ms->peer, UINT32_MAX, ms->cbfunc, ms->cbdata);
    }
    PMIX_RELEASE(ms);
}

void pmix_ptl_base_process_msg(int fd, short flags, void *cbdata)
{
    pmix_ptl_recv_t *msg = (pmix_ptl_recv_t *) cbdata;
    pmix_ptl_posted_recv_t *rcv;
    pmix_buffer_t buf;
    PMIX_HIDE_UNUSED_PARAMS(fd, flags);

    /* acquire the object */
    PMIX_ACQUIRE_OBJECT(msg);

    pmix_output_verbose(5, pmix_ptl_base_framework.framework_output,
                        "%s message received from %s with %d bytes for tag %u on socket %d",
                        PMIX_NAME_PRINT(&pmix_globals.myid), PMIX_PEER_PRINT(msg->peer),
                        (int) msg->hdr.nbytes, msg->hdr.tag, msg->sd);

    /* see if we have a waiting recv for this message */
    PMIX_LIST_FOREACH (rcv, &pmix_ptl_base.posted_recvs, pmix_ptl_posted_recv_t) {
        pmix_output_verbose(5, pmix_ptl_base_framework.framework_output,
                            "checking msg from %s on tag %u for peer %s tag %u",
                            (NULL == msg->peer) ? "NULL" : PMIX_PEER_PRINT(msg->peer), msg->hdr.tag,
                            (NULL == rcv->peer) ? "NULL" : PMIX_PEER_PRINT(rcv->peer), rcv->tag);

        // if the msg and rcv peers don't match, then skip this rcv
        if (NULL != rcv->peer && msg->peer != rcv->peer && UINT_MAX != rcv->tag) {
            continue;
        }
        /* a request to ourselves is not the reply to itself, and a reply
         * to ourselves is never a command - see pmix_ptl_base_post_loopback */
        if (PMIX_PTL_LOOPBACK_REQUEST == msg->loopback &&
            PMIX_PTL_TAG_DYNAMIC <= rcv->tag && UINT_MAX != rcv->tag) {
            continue;
        }
        if (PMIX_PTL_LOOPBACK_REPLY == msg->loopback && UINT_MAX == rcv->tag) {
            continue;
        }
        if (msg->hdr.tag == rcv->tag || UINT_MAX == rcv->tag) {
            /* a dynamic-tag recv is one-shot: take it off the list before
             * its callback runs, so nothing the callback does - posting
             * another recv, or a lost connection completing this peer's
             * outstanding ones - can find it still there */
            if (PMIX_PTL_TAG_DYNAMIC <= rcv->tag && UINT_MAX != rcv->tag) {
                pmix_list_remove_item(&pmix_ptl_base.posted_recvs, &rcv->super);
            }
            if (NULL != rcv->cbfunc) {
                /* construct and load the buffer */
                PMIX_CONSTRUCT(&buf, pmix_buffer_t);
                if (NULL != msg->data) {
                    PMIX_LOAD_BUFFER(msg->peer, &buf, msg->data, msg->hdr.nbytes);
                } else {
                    /* we need to at least set the buffer type so
                     * unpack of a zero-byte message doesn't error */
                    buf.type = msg->peer->nptr->compat.type;
                }
                msg->data = NULL; // protect the data region
                pmix_output_verbose(5, pmix_ptl_base_framework.framework_output,
                                    "%s:%d EXECUTE CALLBACK for tag %u with %d bytes",
                                    pmix_globals.myid.nspace, pmix_globals.myid.rank,
                                    msg->hdr.tag, (int)buf.bytes_used);
                rcv->cbfunc(msg->peer, &msg->hdr, &buf, rcv->cbdata);
                pmix_output_verbose(5, pmix_ptl_base_framework.framework_output,
                                    "%s:%d CALLBACK COMPLETE", pmix_globals.myid.nspace,
                                    pmix_globals.myid.rank);
                PMIX_DESTRUCT(&buf); // free's the msg data
            }
            /* done with the recv if it is a dynamic tag */
            if (PMIX_PTL_TAG_DYNAMIC <= rcv->tag && UINT_MAX != rcv->tag) {
                PMIX_RELEASE(rcv);
            }
            PMIX_RELEASE(msg);
            return;
        }
    }

    /* Nothing was waiting for this message. That is unusual, but it is
     * not necessarily a defect, and it is never something the user can
     * act on - so it is a framework trace rather than a show_help that
     * asks them to report a bug to the PMIx developers.
     *
     * Note that on a server this is nearly unreachable: the command
     * switchyard posts a wildcard (UINT_MAX) recv, which matches
     * anything. So an unmatched message is almost always a *reply* that
     * arrived after the recv expecting it was removed, or one sent on a
     * reserved tag by a peer whose partner never posted for it. Raise
     * the framework verbosity to see these. */
    pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                        "%s discarding unexpected message from %s on tag %u",
                        PMIX_NAME_PRINT(&pmix_globals.myid),
                        PMIX_PEER_PRINT(msg->peer), msg->hdr.tag);
    if (PMIX_PTL_LOOPBACK_REPLY != msg->loopback) {
        /* a reply to ourselves that nobody waits for is our own server
         * half answering a request its caller has already given up on -
         * nothing to tell anyone */
        PMIX_REPORT_EVENT(PMIX_ERROR, msg->peer, PMIX_RANGE_NAMESPACE, _notify_complete);
    }
    PMIX_RELEASE(msg);
}
