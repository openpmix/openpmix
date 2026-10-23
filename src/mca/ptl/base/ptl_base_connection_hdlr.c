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
 * Copyright (c) 2026      Jeff Squyres  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "src/include/pmix_config.h"
#include "include/pmix_stdint.h"

#include <ctype.h>
#include <stdio.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif
#ifdef HAVE_FCNTL_H
#    include <fcntl.h>
#endif
#ifdef HAVE_SYS_SOCKET_H
#    include <sys/socket.h>
#endif

#include "include/pmix_socket_errno.h"
#include "src/client/pmix_client_ops.h"
#include "src/common/pmix_pfexec.h"
#include "src/include/pmix_globals.h"
#include "src/mca/bfrops/base/base.h"
#include "src/mca/gds/base/base.h"
#include "src/server/pmix_server_ops.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_getid.h"
#include "src/util/pmix_net.h"
#include "src/util/pmix_random_bytes.h"
#include "src/util/pmix_show_help.h"
#include "src/util/pmix_strnlen.h"

#include "src/mca/ptl/base/base.h"
#include "src/mca/ptl/base/ptl_base_handshake.h"
#include "src/mca/psec/psec.h"

/* Accepting a connection.
 *
 * A connection is accepted as a sequence of steps, each run on the
 * progress thread from an event: its connect-ack arriving, the host
 * deciding.
 * Nothing waits on a peer in between, so a slow or hostile peer holds only
 * its own connection. See docs/how-things-work/connections.rst.
 *
 * The pending connection carries everything the connection holds until
 * it is accepted - its socket, a scratch peer, the namespace and rank
 * entry it will join, a namespace or rank entry built for it - and nothing
 * of it is attached to anything shared until then. So refusing a
 * connection, at any step, is the same act: send the peer the reason, in
 * the status word it reads next, and release the pending connection. Its
 * destructor gives everything back.
 *
 * The status word the peer reads next is its first reply - which a tool
 * is sent once the host has decided on it. A tool reads one last status
 * after the identity replies. */
static void client_settled(pmix_pending_connection_t *pnd, pmix_status_t status);
static void client_answered(int sd, short args, void *cbdata);
static void tool_settled(pmix_pending_connection_t *pnd, pmix_status_t status);
static void next_tool(void);
static void next_tool_cb(int sd, short args, void *cbdata);
static void ask_about_tool(pmix_pending_connection_t *pnd);
static void tool_answered(int sd, short args, void *cbdata);
static void cnct_cbfunc(pmix_status_t status, pmix_proc_t *proc, void *cbdata);
static void _check_cached_events(pmix_peer_t *peer);
static void accept_client(pmix_pending_connection_t *pnd);
static void accept_tool(pmix_pending_connection_t *pnd);
static void kick_tool_queue(void);
static bool local_peer(pmix_pending_connection_t *pnd);

/* Turn a connection away: tell the peer why, in the status it reads next,
 * and release it. */
static void refuse(pmix_pending_connection_t *pnd, pmix_status_t why)
{
    uint32_t u32;

    if (PMIX_SUCCESS == why || PMIX_OPERATION_SUCCEEDED == why) {
        /* a refusal is never reported as a success */
        why = PMIX_ERROR;
    }
    pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                        "ptl:base: refusing the connection on socket %d: %s", pnd->sd,
                        PMIx_Error_string(why));
    if (0 <= pnd->sd) {
        u32 = htonl((uint32_t) why);
        /* nothing more is owed if the peer is not there to read it */
        (void) pmix_ptl_base_send_blocking(pnd->sd, (char *) &u32, sizeof(uint32_t));
    }
    PMIX_RELEASE(pnd);
}

/* A scratch peer to validate a connection with: what the security module
 * reads and may change - the identity, the protocol, the socket - and
 * nothing else. The identity is the caller's copy, so a module settling an
 * identity changes nothing shared. */
static pmix_peer_t *scratch_peer(pmix_pending_connection_t *pnd, const char *nspace,
                                 pmix_rank_t rank, uid_t uid, gid_t gid, bool registered)
{
    pmix_peer_t *peer;
    pmix_rank_info_t *info;

    peer = PMIX_NEW(pmix_peer_t);
    info = PMIX_NEW(pmix_rank_info_t);
    if (NULL == peer || NULL == info) {
        if (NULL != peer) {
            PMIX_RELEASE(peer);
        }
        if (NULL != info) {
            PMIX_RELEASE(info);
        }
        return NULL;
    }
    info->pname.nspace = strdup(nspace);
    info->pname.rank = rank;
    info->uid = uid;
    info->gid = gid;
    info->host_registered = registered;
    peer->info = info;
    peer->index = -1;
    memcpy(&peer->proc_type, &pnd->proc_type, sizeof(pmix_proc_type_t));
    peer->protocol = pnd->protocol;
    peer->transport = pnd->transport;
    /* the socket stays the connection's: it is lent to the module only
     * while it validates */
    peer->sd = -1;
    return peer;
}

/* A peer on another host, to which this server has proved who it is:
 * its credential must have been made for that exchange, as a tool's, with
 * the mechanism the exchange used - and say the identity it claimed */
static pmix_status_t validate_bound(pmix_pending_connection_t *pnd)
{
    pmix_psec_module_t *psec = (pmix_psec_module_t *) pnd->psecmod;
    pmix_peer_t *peer = (pmix_peer_t *) pnd->peer;
    pmix_byte_object_t bind, cred;
    char conn[PMIX_PTL_AUTH_CONN_LEN];
    uid_t uid;
    gid_t gid;
    pmix_status_t rc;

    if (psec != pnd->authmod || NULL == psec->validate_bound) {
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "ptl:base: refusing a peer at %s - it names security mechanism "
                            "\"%s\", not the one this server proved who it is with",
                            pmix_net_get_hostname((struct sockaddr *) &pnd->addr), psec->name);
        return PMIX_ERR_NO_PERMISSIONS;
    }
    /* a peer that said it binds the connection's two ends did */
    if (pnd->auth_bound && PMIX_SUCCESS != (rc = pmix_ptl_base_auth_conn(pnd->sd, true, conn))) {
        return rc;
    }
    rc = pmix_ptl_base_auth_bind(false, pnd->auth_tool, pnd->auth_srv,
                                 pnd->auth_bound ? conn : NULL, &bind);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    cred.bytes = pnd->cred;
    cred.size = pnd->len;
    rc = psec->validate_bound(&bind, false, &cred, &uid, &gid);
    PMIX_BYTE_OBJECT_DESTRUCT(&bind);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    /* and it must be the identity it connected as, and the user its
     * opening named */
    if (NULL == peer->info || uid != peer->info->uid || gid != peer->info->gid ||
        uid != pnd->auth_uid) {
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "ptl:base: refusing a peer at %s - its credential is uid %lu gid "
                            "%lu, not the identity it connected as",
                            pmix_net_get_hostname((struct sockaddr *) &pnd->addr),
                            (unsigned long) uid, (unsigned long) gid);
        return PMIX_ERR_INVALID_CRED;
    }
    return PMIX_SUCCESS;
}

/* Validate the credential the peer sent */
static pmix_status_t validate(pmix_pending_connection_t *pnd)
{
    pmix_psec_module_t *psec = (pmix_psec_module_t *) pnd->psecmod;
    pmix_peer_t *peer = (pmix_peer_t *) pnd->peer;
    pmix_byte_object_t cred;
    pmix_status_t rc;

    if (pnd->proved) {
        return validate_bound(pnd);
    }
    /* A peer at an address that is not this host's is taken only once this
     * server has proved who it is to it - which a peer of an earlier
     * release never asks, and which a server that takes no tools from
     * other hosts never does. A peer of this release that did not ask -
     * one that reached us through a port forward, say, and took us to be
     * on its own host - is told to, when we would */
    if (!local_peer(pnd) && pmix_ptl_base_tcp_peer_remote(&pnd->addr)) {
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "ptl:base: refusing a peer at %s, not on this host, that did not "
                            "first have this server prove who it is",
                            pmix_net_get_hostname((struct sockaddr *) &pnd->addr));
        if (PMIX_ENABLE_REMOTE_TOOLS && pmix_ptl_base.remote_connections &&
            PMIX_PTL_LOCAL_MAJOR <= pnd->proc_type.major &&
            PMIX_MAJOR_WILDCARD != pnd->proc_type.major) {
            return PMIX_ERR_PROVE_FIRST;
        }
        return PMIX_ERR_NO_PERMISSIONS;
    }

    cred.bytes = pnd->cred;
    cred.size = pnd->len;
    peer->sd = pnd->sd;
    PMIX_PSEC_VALIDATE_CONNECTION_WITH(rc, psec, peer, NULL, 0, NULL, NULL, &cred);
    peer->sd = -1;
    return rc;
}

/* A tool that brings no identity of its own is known to the server by the
 * one the host gives it, and is sent it: a tool that asks for one gets the
 * namespace and rank, and a tool of the legacy kinds - which only v2.1
 * sends, and which takes its rank to be 0 - gets the namespace alone */
static bool host_names_tool(pmix_pending_connection_t *pnd)
{
    return pnd->need_id || PMIX_LEGACY_TOOL == pnd->flag || PMIX_LEGACY_LAUNCHER == pnd->flag;
}

/* How much longer than the time it asks the host to answer in the server
 * waits, for an answer given at the end of that time to arrive */
#define PMIX_PTL_HOST_GRACE_SEC 2

/* Whether a process of this name has a live connection */
static bool name_is_live(const pmix_proc_t *id)
{
    pmix_peer_t *pr;
    int n;

    for (n = 0; n < pmix_server_globals.clients.size; n++) {
        pr = (pmix_peer_t *) pmix_pointer_array_get_item(&pmix_server_globals.clients, n);
        if (NULL != pr && NULL != pr->info && !pr->finalized && 0 <= pr->sd &&
            pr->info->pname.rank == id->rank &&
            PMIx_Check_nspace(pr->info->pname.nspace, id->nspace)) {
            return true;
        }
    }
    return false;
}

/* Whether this is a tool's connection rather than a client's */
/* Whether a connection is on the local transport, whose far end the
 * kernel identifies */
static bool local_peer(pmix_pending_connection_t *pnd)
{
    return NULL != pnd->transport && NULL != pnd->transport->peer_identity;
}

/* The pid the kernel reports for a local connection's far end, or 0 if it
 * gives none */
static pid_t kernel_pid(pmix_pending_connection_t *pnd)
{
    uid_t uid;
    gid_t gid;
    pid_t pid = 0;

    if (PMIX_SUCCESS != pnd->transport->peer_identity(pnd->sd, &uid, &gid, &pid)) {
        return 0;
    }
    return pid;
}

static bool is_tool(pmix_pending_connection_t *pnd)
{
    return PMIX_SIMPLE_CLIENT != pnd->flag && PMIX_SINGLETON_CLIENT != pnd->flag;
}

/* The name a tool's connection is known by: the one it brought, or the one
 * the host gave it */
static const pmix_proc_t *tool_name(pmix_pending_connection_t *pnd)
{
    return host_names_tool(pnd) ? &pnd->assigned : &pnd->proc;
}

/* The host has been handed the notice, and the name the connection held
 * is free */
static void noticed(pmix_status_t status, void *cbdata)
{
    pmix_pending_connection_t *pnd = (pmix_pending_connection_t *) cbdata;
    PMIX_HIDE_UNUSED_PARAMS(status);

    if (pnd->late_listed) {
        pmix_list_remove_item(&pmix_ptl_base.tool_late, &pnd->super);
        pnd->late_listed = false;
    }
    PMIX_RELEASE(pnd);
    kick_tool_queue();
}

/* The host accepted `id`, and it is not connected - the connection could
 * not be completed, or the server had stopped waiting for the answer.
 * Tell the host, as for any process that leaves without finalizing, and
 * release the connection once the host has been given the notice. Until
 * then the connection keeps the name: a client's rank, and a tool's on a
 * rank it was counted on, by `connecting`, and any tool's by its place on
 * tool_late, so nothing accepted under the name meanwhile can be what the
 * notice names. If a live connection has the name already, the notice
 * would name it, and the host is not told */
static void release_not_connected(pmix_pending_connection_t *pnd, const pmix_proc_t *id)
{
    if (0 <= pnd->sd) {
        CLOSE_THE_SOCKET(pnd->sd);
        pnd->sd = -1;
    }
    if (name_is_live(id)) {
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "ptl:base: %s:%u was accepted but not connected, and a live "
                            "connection has the name - the host is not told",
                            id->nspace, id->rank);
        noticed(PMIX_SUCCESS, pnd);
        return;
    }
    if (is_tool(pnd) && !pnd->late_listed) {
        pmix_list_append(&pmix_ptl_base.tool_late, &pnd->super);
        pnd->late_listed = true;
    }
    pmix_ptl_base_report_not_connected(id, noticed, pnd);
}

/* Refuse a connection the host accepted, with `why`, and tell the host */
static void refuse_accepted(pmix_pending_connection_t *pnd, const pmix_proc_t *id,
                            pmix_status_t why)
{
    uint32_t u32;

    pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                        "ptl:base: refusing %s:%u, which the host accepted: %s", id->nspace,
                        id->rank, PMIx_Error_string(why));
    if (0 <= pnd->sd) {
        u32 = htonl((uint32_t) why);
        (void) pmix_ptl_base_send_blocking(pnd->sd, (char *) &u32, sizeof(uint32_t));
    }
    release_not_connected(pnd, id);
}

/* The host's late answer for a connection the server stopped waiting on.
 * A yes is for a process that is not connected - tell the host so. `id`
 * is NULL when the answer named nobody */
static void late_answer(pmix_pending_connection_t *pnd, const pmix_proc_t *id)
{
    if (PMIX_SUCCESS == pnd->status && PMIX_SUCCESS == pnd->upcall_rc && NULL != id) {
        release_not_connected(pnd, id);
        return;
    }
    noticed(PMIX_SUCCESS, pnd);
}

/* The seconds the host can be given to answer in: what a process waits
 * for each reply while it connects, less what it has waited for this one
 * already - in the queue for the host, say. 0 when that wait is
 * unbounded; less than 1 once it is used up */
static int host_timeout(pmix_pending_connection_t *pnd)
{
    struct timeval now;
    long waited;

    if (0 >= pmix_ptl_base.handshake_wait_time) {
        return 0;
    }
    pmix_ptl_base_monotime(&now);
    waited = (long) (now.tv_sec - pnd->waiting.tv_sec);
    if (now.tv_usec < pnd->waiting.tv_usec) {
        --waited;
    }
    if (0 > waited) {
        waited = 0;
    }
    if (waited >= (long) pmix_ptl_base.handshake_wait_time) {
        return -1;
    }
    return pmix_ptl_base.handshake_wait_time - (int) waited;
}

static void host_expired(int sd, short args, void *cbdata);

/* Wait for the host's answer no longer than it was asked to give it in,
 * and a little more */
static void await_host(pmix_pending_connection_t *pnd)
{
    struct timeval tv;

    if (0 >= pnd->host_tmo) {
        return;
    }
    tv.tv_sec = pnd->host_tmo + PMIX_PTL_HOST_GRACE_SEC;
    tv.tv_usec = 0;
    pmix_event_evtimer_set(pmix_globals.evbase, &pnd->timer, host_expired, pnd);
    if (0 == pmix_event_evtimer_add(&pnd->timer, &tv)) {
        pnd->timer_active = true;
    }
}

/* the host has answered: stop waiting for it */
static void host_answered(pmix_pending_connection_t *pnd)
{
    if (pnd->timer_active) {
        pmix_event_evtimer_del(&pnd->timer);
        pnd->timer_active = false;
    }
}

/* the namespace of this name on the server's list, if there is one */
static pmix_namespace_t *find_nspace(const char *nspace)
{
    pmix_namespace_t *nptr;

    if (NULL == nspace || '\0' == nspace[0]) {
        return NULL;
    }
    PMIX_LIST_FOREACH (nptr, &pmix_globals.nspaces, pmix_namespace_t) {
        if (NULL != nptr->nspace && 0 == strcmp(nptr->nspace, nspace)) {
            return nptr;
        }
    }
    return NULL;
}

/* whether a namespace is still on the server's list - the host may
 * deregister one while it decides on a connection */
static bool nspace_listed(pmix_namespace_t *nptr)
{
    pmix_namespace_t *ns;

    PMIX_LIST_FOREACH (ns, &pmix_globals.nspaces, pmix_namespace_t) {
        if (ns == nptr) {
            return true;
        }
    }
    return false;
}

static pmix_rank_info_t *find_rank(pmix_namespace_t *nptr, pmix_rank_t rank)
{
    pmix_rank_info_t *info;

    PMIX_LIST_FOREACH (info, &nptr->ranks, pmix_rank_info_t) {
        if (info->pname.rank == rank) {
            return info;
        }
    }
    return NULL;
}

/* A namespace's wire format is shared by all of its peers - what the
 * server packs for one it packs for every one. The first peer to connect
 * sets it, and a later one that disagrees cannot be served. */
static bool wire_format_fits(pmix_pending_connection_t *pnd, pmix_namespace_t *nptr)
{
    pmix_bfrops_module_t *bfrops = (pmix_bfrops_module_t *) pnd->bfropsmod;

    if (NULL != nptr->compat.bfrops &&
        (nptr->compat.bfrops != bfrops || nptr->compat.type != pnd->buffer_type)) {
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "%s:%u wire format %s does not match its namespace's %s",
                            pnd->proc.nspace, pnd->proc.rank, pnd->bfrops,
                            nptr->compat.bfrops->version);
        return false;
    }
    return true;
}

/* An accepted peer joins its namespace's wire format, which the first of
 * its peers sets and no later one replaces */
static void adopt_modules(pmix_pending_connection_t *pnd, pmix_peer_t *peer,
                          pmix_namespace_t *nptr)
{
    pmix_info_t ginfo;
    pmix_status_t rc;

    if (NULL == nptr->compat.bfrops) {
        nptr->compat.bfrops = (pmix_bfrops_module_t *) pnd->bfropsmod;
        nptr->compat.type = pnd->buffer_type;
    }
    if (NULL == nptr->compat.psec) {
        nptr->compat.psec = (pmix_psec_module_t *) pnd->psecmod;
    }
    if (NULL == nptr->compat.gds) {
        nptr->compat.gds = (pmix_gds_base_module_t *) pnd->gdsmod;
    }
    /* track this peer's GDS module on the peer itself, so a later
     * per-client fallback (PMIX_GDS_FALLBACK_CMD) can change it without
     * affecting the other peers sharing this nspace */
    peer->gds = (pmix_gds_base_module_t *) pnd->gdsmod;

    /* If we haven't previously stored the wire format for this nspace, do
     * so now. This is what PMIx_Data_pack/unpack fall back on for an
     * nspace with no connected peer left, and they hand it to
     * pmix_bfrops_base_assign_module() - which matches a component name
     * ("v61"), not the peer's library release ("7.0.0") */
    if (!nptr->version_stored) {
        PMIX_INFO_LOAD(&ginfo, PMIX_BFROPS_MODULE, nptr->compat.bfrops->version, PMIX_STRING);
        PMIX_GDS_CACHE_JOB_INFO(rc, pmix_globals.mypeer, nptr, &ginfo, 1);
        PMIX_INFO_DESTRUCT(&ginfo);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
        } else {
            nptr->version_stored = true;
        }
    }
}

/* If this rank previously finalized, a harmless finalized "tombstone"
 * peer from that prior cycle may still occupy its clients-array slot -
 * pmix_server_peer_finalized leaves it in place at socket-close rather
 * than mutating shared state amid concurrent collectives. Now that the
 * rank is reconnecting we are at a safe point (no collective from the
 * prior cycle can still be in flight), so reclaim that tombstone: null
 * its slot, drop the finalized count it was still contributing, and
 * release it. This is what keeps the clients array and nptr->nfinalized
 * from drifting across repeated init/finalize cycles. Only when no live
 * process remains for the rank (proc_cnt == 0), so a live connection
 * for the rank is never disturbed. See
 * docs/how-things-work/init-finalize.rst. */
static void reclaim_tombstone(pmix_rank_info_t *info)
{
    pmix_peer_t *stale;

    if (0 > info->peerid || 0 != info->proc_cnt) {
        return;
    }
    stale = (pmix_peer_t *) pmix_pointer_array_get_item(&pmix_server_globals.clients,
                                                        info->peerid);
    if (NULL != stale && stale->finalized) {
        pmix_pointer_array_set_item(&pmix_server_globals.clients, info->peerid, NULL);
        /* the count is on the tombstone's namespace - that is the one its
         * finalize raised */
        if (NULL != stale->nptr && 0 < stale->nptr->nfinalized) {
            --stale->nptr->nfinalized;
        }
        PMIX_RELEASE(stale);
    }
}

/* the connection is the peer's now: start serving it */
static void activate(pmix_pending_connection_t *pnd, pmix_peer_t *peer)
{
    peer->sd = pnd->sd;
    pnd->sd = -1;
    pnd->peer = NULL;
    PMIX_RELEASE(pnd);

    pmix_ptl_base_set_nonblocking(peer->sd);
    pmix_event_assign(&peer->recv_event, pmix_globals.evbase, peer->sd, EV_READ | EV_PERSIST,
                      pmix_ptl_base_recv_handler, peer);
    pmix_event_add(&peer->recv_event, NULL);
    peer->recv_ev_active = true;
    pmix_event_assign(&peer->send_event, pmix_globals.evbase, peer->sd, EV_WRITE | EV_PERSIST,
                      pmix_ptl_base_send_handler, peer);
    /* A send may have been queued while this event had no base - see
     * pmix_ptl_base_send(). It could not be activated then; activate it
     * now, or it waits for a send that may never come. */
    if (NULL != peer->send_msg && !peer->send_ev_active) {
        if (0 == pmix_event_add(&peer->send_event, 0)) {
            peer->send_ev_active = true;
        }
    }
    pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                        "pmix:server %s %s:%u has connected on socket %d",
                        PMIX_PEER_IS_TOOL(peer) ? "tool" : "client",
                        peer->info->pname.nspace, peer->info->pname.rank, peer->sd);

    /* check the cached events and update the peer */
    _check_cached_events(peer);
}

/* Take into buf, at *recvd, whatever the socket has of the size bytes
 * we want. Returns PMIX_ERR_WOULD_BLOCK when it has given us all it has
 * and that is not yet everything. */
static pmix_status_t read_available(int sd, char *buf, size_t size, size_t *recvd)
{
    ssize_t rc;

    while (*recvd < size) {
        rc = recv(sd, buf + *recvd, size - *recvd, 0);
        if (0 < rc) {
            *recvd += (size_t) rc;
            continue;
        }
        if (0 == rc) {
            /* the far end closed the connection */
            pmix_output_verbose(8, pmix_ptl_base_framework.framework_output,
                                "ptl:base:connection_handler: remote closed socket %d", sd);
            return PMIX_ERR_UNREACH;
        }
        if (EINTR == pmix_socket_errno) {
            continue;
        }
        if (EAGAIN == pmix_socket_errno || EWOULDBLOCK == pmix_socket_errno) {
            return PMIX_ERR_WOULD_BLOCK;
        }
        pmix_output_verbose(8, pmix_ptl_base_framework.framework_output,
                            "ptl:base:connection_handler: recv on socket %d failed: %s (%d)",
                            sd, strerror(pmix_socket_errno), pmix_socket_errno);
        return PMIX_ERR_UNREACH;
    }
    return PMIX_SUCCESS;
}

/* Read what has arrived of a connect-ack.
 *
 * This runs on the progress thread, for a socket that anything able to
 * reach our listener can open - and the connect-ack arrives before any
 * credential has been checked. So it must never wait for bytes: it takes
 * what the (non-blocking) socket has, keeps its place in the pending
 * connection, and returns PMIX_ERR_WOULD_BLOCK when that is not yet the
 * whole message. A blocking read, even one bounded by a receive timeout,
 * let a peer that sent part of a request and stalled hold the progress
 * thread - and with it every client of this server - for the length of
 * the timeout, as often as it cared to reconnect and do it again.
 *
 * The header is judged the moment it is complete, before any payload is
 * sized from the length it names. The payload is allocated one byte
 * longer than that and zeroed, so the string fields parsed out of it are
 * always terminated. */
static pmix_status_t read_connect_ack(pmix_pending_connection_t *pnd)
{
    pmix_status_t rc;

    rc = read_available(pnd->sd, (char *) &pnd->hdr, sizeof(pmix_ptl_hdr_t), &pnd->hdr_recvd);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    /* get the id, authentication and version payload (and possibly
     * security credential) - no larger than PMIX_MAX_CRED_SIZE. The
     * length is judged on every pass, not only the one that allocates: it
     * is the bound the read below runs to, and it comes off the wire */
    if (PMIX_MAX_CRED_SIZE < pnd->hdr.nbytes) {
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "ptl:base:connection_handler: connect-ack of %u bytes "
                            "on socket %d exceeds the limit",
                            pnd->hdr.nbytes, pnd->sd);
        return PMIX_ERR_BAD_PARAM;
    }
    if (NULL == pnd->msg) {
        pnd->msg = (char *) pmix_calloc(pnd->hdr.nbytes + 1, sizeof(char));
        if (NULL == pnd->msg) {
            return PMIX_ERR_NOMEM;
        }
    }
    return read_available(pnd->sd, pnd->msg, pnd->hdr.nbytes, &pnd->msg_recvd);
}

/* The server's answer to a peer on another host is a few words and a
 * credential, written to a socket in blocking mode - bounded by the
 * connect-ack timeout, as a peer that stops reading must not hold the
 * progress thread. pmix_ptl_base_send_blocking cycles on EAGAIN, which is
 * how a send that times out reports it, so it cannot be used here.
 * PMIX_ERR_UNREACH if the peer is gone or has stopped reading */
static pmix_status_t send_bounded(int sd, const char *ptr, size_t size)
{
    size_t cnt = 0;
    ssize_t n;

    while (cnt < size) {
        n = send(sd, ptr + cnt, size - cnt, PMIX_PTL_NOSIGNAL);
        if (0 < n) {
            cnt += (size_t) n;
            continue;
        }
        if (0 > n && EINTR == pmix_socket_errno) {
            continue;
        }
        return PMIX_ERR_UNREACH;
    }
    return PMIX_SUCCESS;
}

static void set_send_timeout(int sd, int secs)
{
    struct timeval tv;

    tv.tv_sec = secs;
    tv.tv_usec = 0;
    (void) setsockopt(sd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

/* A tool on another host - or on this one, where its kernel cannot name
 * this server's user - asks this server to prove who it is, before it
 * sends its connect-ack - see "Between hosts" in base.h. What is left of
 * the message after the flag is the tool's version and its nonce. The
 * answer is a status and, on success, our nonce and a credential bound
 * to both; the connection then waits for the tool's connect-ack as it
 * waited for this message. On failure the caller refuses the connection */
static pmix_status_t answer_auth(pmix_pending_connection_t *pnd, char *mg, size_t cnt)
{
    pmix_psec_module_t *psec;
    pmix_byte_object_t bind, cred;
    char *version = NULL, conn[PMIX_PTL_AUTH_CONN_LEN];
    uid_t kuid;
    uint8_t u8;
    uint32_t len, u32;
    pmix_status_t rc = PMIX_ERR_BAD_PARAM;

    PMIX_BYTE_OBJECT_CONSTRUCT(&bind);
    PMIX_BYTE_OBJECT_CONSTRUCT(&cred);

    /* asked once, over TCP, with no credential of the tool's */
    if (local_peer(pnd) || pnd->proved || 0 != pnd->len) {
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "ptl:base: refusing a request to prove who this server is on "
                            "socket %d - %s",
                            pnd->sd, local_peer(pnd) ? "it came over the local socket"
                                     : pnd->proved ? "it was made already"
                                                   : "it carried a credential");
        rc = local_peer(pnd) ? PMIX_ERR_NOT_SUPPORTED : PMIX_ERR_BAD_PARAM;
        goto error;
    }
    PMIX_PTL_GET_STRING(version);
    PMIX_PTL_GET_U32(len);
    /* then its nonce, its user and whether it binds the connection -
     * anything after them is a later release's, and ignored */
    if (PMIX_PTL_AUTH_NONCE_LEN != len || cnt < len + sizeof(uint32_t)) {
        rc = PMIX_ERR_BAD_PARAM;
        goto error;
    }
    memcpy(pnd->auth_tool, mg, len);
    mg += len;
    cnt -= len;
    PMIX_PTL_GET_U32(u32);
    pnd->auth_uid = (uid_t) u32;
    /* and whether it binds the connection's two ends too - which one
     * that sends no more does not */
    pnd->auth_bound = false;
    if (0 < cnt) {
        PMIX_PTL_GET_U8(u8);
        pnd->auth_bound = (0 != u8);
    }

    /* only a server that takes tools proves who it is to one, and only
     * with a mechanism that can. A tool that binds the connection is on
     * this host, where the kernel cannot name this server's user - nor,
     * sharing that kernel, this server's peers: one at another address,
     * or on a host whose kernel can, has no need. Any other is a tool on
     * another host, which this server proves who it is to only when it
     * takes tools from other hosts. All of this is settled before
     * anything is made for the peer */
    rc = PMIX_SUCCESS;
    if (!pmix_ptl_base.tool_support) {
        rc = PMIX_ERR_NOT_SUPPORTED;
    } else if (pnd->auth_bound) {
        if (!pmix_ptl_base_tcp_peer_held(&pnd->addr) ||
            (!pmix_ptl_base_test_no_tcp_table &&
             PMIX_ERR_NOT_SUPPORTED != pmix_util_getid_tcp(pnd->sd, &kuid))) {
            rc = PMIX_ERR_NOT_SUPPORTED;
        }
    } else if (!PMIX_ENABLE_REMOTE_TOOLS || !pmix_ptl_base.remote_connections) {
        rc = PMIX_ERR_NOT_SUPPORTED;
    }
    if (PMIX_ERR_NOT_SUPPORTED == rc) {
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "ptl:base: refusing a peer at %s - this server takes no %s",
                            pmix_net_get_hostname((struct sockaddr *) &pnd->addr),
                            !pmix_ptl_base.tool_support ? "tools"
                            : pnd->auth_bound ? "request to prove who it is from there"
                                              : "tools from other hosts");
        goto error;
    }
    psec = pmix_psec_base_assign_module(pnd->psec);
    if (NULL == psec || 0 != strcmp(psec->name, pnd->psec) ||
        !pmix_psec_base_remote_capable(psec)) {
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "ptl:base: refusing a peer at %s - security mechanism \"%s\" "
                            "cannot authenticate across hosts here",
                            pmix_net_get_hostname((struct sockaddr *) &pnd->addr), pnd->psec);
        rc = PMIX_ERR_NOT_SUPPORTED;
        goto error;
    }

    /* our nonce, and a credential made as a server for both */
    rc = pmix_util_random_bytes(pnd->auth_srv, PMIX_PTL_AUTH_NONCE_LEN);
    if (PMIX_SUCCESS != rc) {
        goto error;
    }
    /* bound to the connection's two ends as well, when the tool binds
     * them - it reads them from its own end of it */
    if (pnd->auth_bound && PMIX_SUCCESS != (rc = pmix_ptl_base_auth_conn(pnd->sd, true, conn))) {
        goto error;
    }
    rc = pmix_ptl_base_auth_bind(true, pnd->auth_tool, pnd->auth_srv,
                                 pnd->auth_bound ? conn : NULL, &bind);
    if (PMIX_SUCCESS != rc) {
        goto error;
    }
    /* for that user to check alone, where the mechanism can say so */
    rc = psec->create_bound(&bind, true, pnd->auth_uid, &cred);
    if (PMIX_SUCCESS != rc) {
        goto error;
    }
    /* no larger than a tool will read */
    if (PMIX_MAX_CRED_SIZE < cred.size) {
        rc = PMIX_ERR_BAD_PARAM;
        goto error;
    }
    /* status, our nonce, the credential's length, the credential */
    if (0 < pmix_ptl_base.connect_ack_timeout) {
        set_send_timeout(pnd->sd, pmix_ptl_base.connect_ack_timeout);
    }
    u32 = htonl((uint32_t) PMIX_SUCCESS);
    rc = send_bounded(pnd->sd, (char *) &u32, sizeof(u32));
    if (PMIX_SUCCESS == rc) {
        rc = send_bounded(pnd->sd, (char *) pnd->auth_srv, PMIX_PTL_AUTH_NONCE_LEN);
    }
    if (PMIX_SUCCESS == rc) {
        u32 = htonl((uint32_t) cred.size);
        rc = send_bounded(pnd->sd, (char *) &u32, sizeof(u32));
    }
    if (PMIX_SUCCESS == rc) {
        rc = send_bounded(pnd->sd, cred.bytes, cred.size);
    }
    /* the replies to its connect-ack go out unbounded, as every other's */
    set_send_timeout(pnd->sd, 0);
    if (PMIX_SUCCESS != rc) {
        /* the peer is gone - nothing more to tell it */
        rc = PMIX_ERR_UNREACH;
        goto error;
    }
    /* the peer's version goes into the log only as printable characters */
    for (len = 0; '\0' != version[len]; len++) {
        if (!isprint((unsigned char) version[len])) {
            version[len] = '?';
        }
    }
    pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                        "ptl:base: proved who this server is with %s to a peer at %s "
                        "(release %s)",
                        psec->name, pmix_net_get_hostname((struct sockaddr *) &pnd->addr),
                        version);

    /* its connect-ack comes next, and is read as this was */
    pnd->proved = true;
    pnd->authmod = psec;
    free(pnd->psec);
    pnd->psec = NULL;
    pnd->flag = PMIX_SIMPLE_CLIENT;
    memset(&pnd->hdr, 0, sizeof(pnd->hdr));
    pnd->hdr_recvd = 0;
    pnd->msg_recvd = 0;
    pmix_ptl_base_set_nonblocking(pnd->sd);
    pmix_ptl_base_await_connect_ack(pnd, true);
    rc = PMIX_SUCCESS;

error:
    free(version);
    PMIX_BYTE_OBJECT_DESTRUCT(&bind);
    PMIX_BYTE_OBJECT_DESTRUCT(&cred);
    return rc;
}

/* PMIX_PTL_LEGACY_PAD: a trailer too short to hold an info count is not an
 * info blob.
 *
 * The v4.1 series counted the connector-flag byte twice when it sized its
 * connect-ack - once in send_connect_ack, once in construct_message - so
 * every connect-ack from a v4.1 client or tool ends with one zero byte
 * after the gds name. A peer that also sends info has that byte after the
 * blob, where nothing reads it; a peer that does not has it where the blob
 * would be, and unpacking a count out of it reads past the end. That is a
 * released wire format and must keep connecting, so both blob parsers
 * below read "past the end" on the count as "no blob". Any other failure,
 * and a count that does not fit the bytes received, is still refused.
 */
void pmix_ptl_base_connection_handler(int sd, short args, void *cbdata)
{
    pmix_pending_connection_t *pnd = (pmix_pending_connection_t *) cbdata;
    pmix_status_t rc;
    char *msg = NULL, *mg;
    size_t cnt;
    uint8_t major, minor, release;

    /* acquire the object */
    PMIX_ACQUIRE_OBJECT(pnd);

    // must use sd, args to avoid -Werror
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    pmix_output_verbose(8, pmix_ptl_base_framework.framework_output,
                        "ptl:base:connection_handler: readable connection: %d", pnd->sd);

    /* collect what has arrived of the connect-ack */
    rc = read_connect_ack(pnd);
    if (PMIX_ERR_WOULD_BLOCK == rc) {
        /* the rest of it has not arrived - pnd holds what has, so wait for
         * the socket to become readable again. The listener's timer, if
         * the timeout is enabled, is still running */
        PMIX_POST_OBJECT(pnd);
        if (0 != pmix_event_add(&pnd->ev, NULL)) {
            PMIX_ERROR_LOG(PMIX_ERROR);
            pmix_ptl_base_drop_pending_connection(pnd, PMIX_ERROR);
        }
        return;
    }
    /* complete or failed, it is no longer waiting on its socket */
    pmix_list_remove_item(&pmix_ptl_base.pending_connections, &pnd->super);
    if (pnd->timer_active) {
        pmix_event_evtimer_del(&pnd->timer);
        pnd->timer_active = false;
    }
    if (PMIX_SUCCESS != rc) {
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "ptl:base:connection_handler unable to complete recv of connect-ack "
                            "on socket %d: %s",
                            pnd->sd, PMIx_Error_string(rc));
        if (PMIX_ERR_UNREACH == rc) {
            /* the peer closed the connection - nobody to tell */
            PMIX_RELEASE(pnd);
        } else {
            /* a connect-ack too large to accept, or one we could not make
             * room for */
            refuse(pnd, rc);
        }
        return;
    }
    /* the payload is ours from here */
    msg = pnd->msg;
    pnd->msg = NULL;

    /* The replies to this connect-ack are a few words written into a
     * socket nothing else has written to, so they are written as blocking
     * sends. Nothing below reads from the socket */
    pmix_ptl_base_set_blocking(pnd->sd);

    cnt = pnd->hdr.nbytes;
    mg = msg;
    /* a field that is missing or malformed is refused as such */
    rc = PMIX_ERR_BAD_PARAM;
    /* extract the name of the sec module they used */
    PMIX_PTL_GET_STRING(pnd->psec);

    /* extract any credential so we can validate this connection
     * before doing anything else */
    PMIX_PTL_GET_U32(pnd->len);
    if (PMIX_TAINT_UINT_LIMIT < pnd->len) {    // a credential is no longer than this
        rc = PMIX_ERR_BAD_PARAM;
        goto error;
    }

    /* if a credential is present, then create space and
     * extract it for processing */
    PMIX_PTL_GET_BLOB(pnd->cred, pnd->len);

    /* get the process type of the connecting peer */
    PMIX_PTL_GET_U8(pnd->flag);

    /* not a connect-ack: a tool on another host asking us to prove who we
     * are first */
    if (PMIX_REMOTE_AUTH == pnd->flag) {
        size_t nbytes = pnd->hdr.nbytes;

        rc = answer_auth(pnd, mg, cnt);
        pmix_ptl_base_wipe(msg, nbytes);
        free(msg);
        if (PMIX_ERR_UNREACH == rc) {
            PMIX_RELEASE(pnd);
        } else if (PMIX_SUCCESS != rc) {
            refuse(pnd, rc);
        }
        return;
    }

    switch (pnd->flag) {
    case PMIX_SIMPLE_CLIENT:
        /* simple client process */
        PMIX_SET_PROC_TYPE(&pnd->proc_type, PMIX_PROC_CLIENT);
        /* get their identifier */
        PMIX_PTL_GET_PROCID(pnd->proc);
        break;

    case PMIX_LEGACY_TOOL:
        /* legacy tool - may or may not have an identifier */
        PMIX_SET_PROC_TYPE(&pnd->proc_type, PMIX_PROC_TOOL);
        /* get their uid/gid */
        PMIX_PTL_GET_U32(pnd->uid);
        PMIX_PTL_GET_U32(pnd->gid);
        break;

    case PMIX_LEGACY_LAUNCHER:
        /* legacy launcher - may or may not have an identifier */
        PMIX_SET_PROC_TYPE(&pnd->proc_type, PMIX_PROC_LAUNCHER);
        /* get their uid/gid */
        PMIX_PTL_GET_U32(pnd->uid);
        PMIX_PTL_GET_U32(pnd->gid);
        break;

    case PMIX_TOOL_NEEDS_ID:
    case PMIX_LAUNCHER_NEEDS_ID:
        /* self-started tool/launcher process that needs an identifier */
        if (PMIX_TOOL_NEEDS_ID == pnd->flag) {
            PMIX_SET_PROC_TYPE(&pnd->proc_type, PMIX_PROC_TOOL);
        } else {
            PMIX_SET_PROC_TYPE(&pnd->proc_type, PMIX_PROC_LAUNCHER);
        }
        /* get their uid/gid */
        PMIX_PTL_GET_U32(pnd->uid);
        PMIX_PTL_GET_U32(pnd->gid);
        /* they need an id */
        pnd->need_id = true;
        break;

    case PMIX_TOOL_GIVEN_ID:
    case PMIX_LAUNCHER_GIVEN_ID:
    case PMIX_SINGLETON_CLIENT:
    case PMIX_SCHEDULER_WITH_ID:
        /* self-started tool/launcher process that was given an identifier by caller */
        if (PMIX_TOOL_GIVEN_ID == pnd->flag) {
            PMIX_SET_PROC_TYPE(&pnd->proc_type, PMIX_PROC_TOOL);
        } else if (PMIX_LAUNCHER_GIVEN_ID == pnd->flag) {
            PMIX_SET_PROC_TYPE(&pnd->proc_type, PMIX_PROC_LAUNCHER);
        } else if (PMIX_SCHEDULER_WITH_ID == pnd->flag) {
            PMIX_SET_PROC_TYPE(&pnd->proc_type, PMIX_PROC_SCHEDULER);
        } else {
            PMIX_SET_PROC_TYPE(&pnd->proc_type, PMIX_PROC_CLIENT);
        }
        /* get their uid/gid */
        PMIX_PTL_GET_U32(pnd->uid);
        PMIX_PTL_GET_U32(pnd->gid);
        /* get their identifier */
        PMIX_PTL_GET_PROCID(pnd->proc);
        break;

    case PMIX_TOOL_CLIENT:
    case PMIX_LAUNCHER_CLIENT:
        /* tool/launcher that was started by a PMIx server - identifier specified by server */
        if (PMIX_TOOL_CLIENT == pnd->flag) {
            PMIX_SET_PROC_TYPE(&pnd->proc_type, PMIX_PROC_TOOL);
        } else {
            PMIX_SET_PROC_TYPE(&pnd->proc_type, PMIX_PROC_LAUNCHER);
        }
        /* get their uid/gid */
        PMIX_PTL_GET_U32(pnd->uid);
        PMIX_PTL_GET_U32(pnd->gid);
        /* get their identifier */
        PMIX_PTL_GET_PROCID(pnd->proc);
        break;

    default:
        /* we don't know what they are! */
        rc = PMIX_ERR_NOT_SUPPORTED;
        goto error;
    }

    /* extract their VERSION */
    PMIX_PTL_GET_STRING(pnd->version);
    pmix_ptl_base_parse_version(pnd->version, &major, &minor, &release);
    PMIX_SET_PROC_MAJOR(&pnd->proc_type, major);
    PMIX_SET_PROC_MINOR(&pnd->proc_type, minor);
    PMIX_SET_PROC_RELEASE(&pnd->proc_type, release);

    /* no release is numbered 0 - this is not a version at all */
    if (0 == major) {
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "ptl:base:connection_handler: connect-ack version \"%s\" is "
                            "not a PMIx version",
                            (NULL == pnd->version) ? "(none)" : pnd->version);
        rc = PMIX_ERR_BAD_PARAM;
        goto error;
    }

    /* The oldest peer we serve is v2.1. A v1.x peer cannot reach us at all
     * (it speaks only the usock transport, which is gone), and a v2.0
     * peer's handshake ends here, without the wire format, buffer type or
     * datastore that everything below needs - its buffer type in
     * particular cannot be inferred, so its very first message is
     * misread. Say so rather than fail on its traffic. */
    if (1 == major || (2 == major && 0 == minor)) {
        pmix_show_help("help-ptl-base.txt", "unsupported-client-version", true,
                       pnd->version);
        /* It cannot name the code, but it will fail instead of carrying
         * on as though no server were there */
        rc = PMIX_ERR_OUTDATED;
        goto error;
    }

    /* A peer that had this server prove who it is is of this release or
     * later: an earlier one has no such exchange */
    if (pnd->proved && PMIX_PTL_LOCAL_MAJOR > major) {
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "ptl:base:connection_handler: refusing a peer that asked this "
                            "server to prove who it is, then named an earlier release");
        rc = PMIX_ERR_BAD_PARAM;
        goto error;
    }

    /* A client or tool of an earlier release is taken only when this
     * server takes them */
    if (PMIX_PTL_LOCAL_MAJOR > major && !pmix_ptl_base.legacy_peers) {
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "ptl:base:connection_handler: refusing a peer of release %s - "
                            "this server takes no clients or tools of earlier releases",
                            pnd->version);
        rc = PMIX_ERR_NOT_SUPPORTED;
        goto error;
    }
    /* and only over TCP: no earlier release has the local transport */
    if (PMIX_PTL_LOCAL_MAJOR > major && local_peer(pnd)) {
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "ptl:base:connection_handler: refusing a peer of release %s on "
                            "the local socket, which no earlier release uses",
                            pnd->version);
        rc = PMIX_ERR_NOT_SUPPORTED;
        goto error;
    }

    /* A process that names itself must name a process: a namespace, and a
     * rank. An empty namespace would match every namespace wherever names
     * are compared. A tool of an earlier release with its server's
     * address in its environment but no namespace and rank says it is on
     * a client's rank, and sends no name: it is given one by the host, as
     * a tool that needs one is - but it reads none back, so none is sent,
     * and it knows itself by no name. A tool of this release with no
     * namespace and rank asks for an identity instead */
    if (PMIX_SIMPLE_CLIENT != pnd->flag && !pnd->need_id && PMIX_LEGACY_TOOL != pnd->flag &&
        PMIX_LEGACY_LAUNCHER != pnd->flag &&
        ('\0' == pnd->proc.nspace[0] || !PMIX_RANK_IS_VALID(pnd->proc.rank))) {
        if (PMIX_TOOL_CLIENT == pnd->flag || PMIX_LAUNCHER_CLIENT == pnd->flag) {
            pnd->flag = (PMIX_TOOL_CLIENT == pnd->flag) ? PMIX_TOOL_NEEDS_ID
                                                        : PMIX_LAUNCHER_NEEDS_ID;
            pnd->need_id = true;
            pnd->id_unsent = true;
            PMIX_LOAD_PROCID(&pnd->proc, NULL, PMIX_RANK_UNDEF);
        } else {
            pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                                "ptl:base:connection_handler: connect-ack names no process "
                                "(\"%s\":%u)",
                                pnd->proc.nspace, pnd->proc.rank);
            rc = PMIX_ERR_BAD_PARAM;
            goto error;
        }
    }

    /* extract the name of the bfrops module they used */
    PMIX_PTL_GET_STRING(pnd->bfrops);

    /* extract the type of buffer they used */
    PMIX_PTL_GET_U8(pnd->buffer_type);

    /* extract the name of the gds module they used */
    PMIX_PTL_GET_STRING(pnd->gds);

    /* keep the trailing info until the peer is validated - it is read
     * with the wire format the peer named */
    if (0 < cnt) {
        pnd->bloblen = cnt;
        PMIX_PTL_GET_BLOB(pnd->blob, pnd->bloblen);
    }
    /* the info it holds may carry a connection token */
    pmix_ptl_base_wipe(msg, pnd->hdr.nbytes);
    free(msg);
    msg = NULL;

    if (PMIX_SIMPLE_CLIENT == pnd->flag || PMIX_SINGLETON_CLIENT == pnd->flag) {
        accept_client(pnd);
    } else {
        accept_tool(pnd);
    }
    return;

error:
    if (NULL != msg) {
        pmix_ptl_base_wipe(msg, pnd->hdr.nbytes);
        free(msg);
    }
    refuse(pnd, rc);
}

/* What a client's connect-ack info says: its pid and real ids, as its
 * PMIx library sent them, recorded only once the connection is accepted.
 * A uid or gid in it is ignored - the identity is the one the security
 * module settles, and nothing a peer states can add to that */
static pmix_status_t read_client_info(pmix_pending_connection_t *pnd)
{
    pmix_bfrops_module_t *bfrops = (pmix_bfrops_module_t *) pnd->bfropsmod;
    pmix_buffer_t buf;
    pmix_info_t *iblob = NULL;
    size_t nblob = 0, n;
    int32_t i32;
    pmix_status_t rc;

    if (NULL == pnd->blob) {
        return PMIX_SUCCESS;
    }
    PMIX_CONSTRUCT(&buf, pmix_buffer_t);
    PMIX_LOAD_BUFFER_NON_DESTRUCT(pmix_globals.mypeer, &buf, pnd->blob, pnd->bloblen);
    buf.type = pnd->buffer_type;
    i32 = 1;
    rc = bfrops->unpack(&buf, &nblob, &i32, PMIX_SIZE);
    if (PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER == rc) {
        /* too short to hold even the count - see PMIX_PTL_LEGACY_PAD */
        return PMIX_SUCCESS;
    }
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    /* the blob must still hold nblob packed pmix_info_t. This mirrors the
     * check on the tool path */
    if (!pmix_bfrop_count_fits(&buf, nblob, PMIX_INFO)) {
        return PMIX_ERR_BAD_PARAM;
    }
    if (0 == nblob) {
        return PMIX_SUCCESS;
    }
    PMIX_INFO_CREATE(iblob, nblob);
    if (NULL == iblob) {
        return PMIX_ERR_NOMEM;
    }
    i32 = (int32_t) nblob;
    rc = bfrops->unpack(&buf, iblob, &i32, PMIX_INFO);
    if (PMIX_SUCCESS != rc) {
        pmix_ptl_base_wipe_token_info(iblob, nblob);
        PMIX_INFO_FREE(iblob, nblob);
        return rc;
    }
    for (n = 0; n < nblob; n++) {
        if (PMIx_Check_key(iblob[n].key, PMIX_PROC_PID)) {
            pnd->pid = iblob[n].value.data.pid;
        } else if (PMIx_Check_key(iblob[n].key, PMIX_REALUID)) {
            pnd->realuid = iblob[n].value.data.uint32;
            pnd->realuid_given = true;
        } else if (PMIx_Check_key(iblob[n].key, PMIX_REALGID)) {
            pnd->realgid = iblob[n].value.data.uint32;
            pnd->realgid_given = true;
        } else if (PMIx_Check_key(iblob[n].key, PMIX_PTL_TOKEN_KEY) && NULL == pnd->token &&
                   PMIX_STRING == iblob[n].value.type && NULL != iblob[n].value.data.string) {
            pnd->token = strdup(iblob[n].value.data.string);
        }
    }
    pmix_ptl_base_wipe_token_info(iblob, nblob);
    PMIX_INFO_FREE(iblob, nblob);
    return rc;
}

/* A client: a process the host registered, before it started, under the
 * namespace and rank it names */
static void accept_client(pmix_pending_connection_t *pnd)
{
    pmix_namespace_t *nptr;
    pmix_rank_info_t *info;
    pmix_info_t ginfo;

    nptr = find_nspace(pnd->proc.nspace);
    if (NULL == nptr) {
        refuse(pnd, PMIX_ERR_NOT_FOUND);
        return;
    }
    info = find_rank(nptr, pnd->proc.rank);
    if (NULL == info) {
        refuse(pnd, PMIX_ERR_NOT_FOUND);
        return;
    }
    PMIX_RETAIN(nptr);
    pnd->nptr = nptr;
    PMIX_RETAIN(info);
    pnd->rinfo = info;

    /* the modules it asked for */
    pnd->psecmod = pmix_psec_base_assign_module(pnd->psec);
    pnd->bfropsmod = pmix_bfrops_base_assign_module(pnd->bfrops);
    if (NULL != pnd->gds) {
        PMIX_INFO_LOAD(&ginfo, PMIX_GDS_MODULE, pnd->gds, PMIX_STRING);
        pnd->gdsmod = pmix_gds_base_assign_module(&ginfo, 1);
        PMIX_INFO_DESTRUCT(&ginfo);
    } else {
        pnd->gdsmod = pmix_gds_base_assign_module(NULL, 0);
    }
    if (NULL == pnd->psecmod || NULL == pnd->bfropsmod || NULL == pnd->gdsmod) {
        refuse(pnd, PMIX_ERR_NOT_SUPPORTED);
        return;
    }
    if (!wire_format_fits(pnd, nptr)) {
        refuse(pnd, PMIX_ERR_NOT_SUPPORTED);
        return;
    }

    /* The rank's entry is the identity the client must have, whoever made
     * it - the host, or this server for a singleton that started it - so
     * it is validated as a registration, on a copy: a module settling an
     * identity must never rewrite the entry itself */
    pnd->peer = scratch_peer(pnd, nptr->nspace, info->pname.rank, info->uid, info->gid, true);
    if (NULL == pnd->peer) {
        refuse(pnd, PMIX_ERR_NOMEM);
        return;
    }
    client_settled(pnd, validate(pnd));
}

/* Whether a rank already has a live connection, or one waiting on the
 * host's answer. A finalized connection is a tombstone the rank left
 * behind, and one whose socket has closed is a connection that was lost -
 * a process may connect again after either.
 *
 * A socket can have closed before its receive handler has run to see it:
 * the close is read on the progress thread and a new connection arrives
 * on the listener thread, so a rank whose process died can connect again
 * - a process the host started again, say - before the server has seen
 * the first one go. Such a connection is handled as lost here, first, so
 * the new one is not refused as a second connection for the rank */
static bool rank_connected(pmix_rank_info_t *info)
{
    pmix_peer_t *pr;
    int n;

    if (0 < info->connecting) {
        return true;
    }
    for (n = 0; n < pmix_server_globals.clients.size; n++) {
        pr = (pmix_peer_t *) pmix_pointer_array_get_item(&pmix_server_globals.clients, n);
        if (NULL != pr && pr->info == info && !pr->finalized && 0 <= pr->sd) {
            if (pmix_ptl_base_reap_if_closed(pr)) {
                continue;
            }
            return true;
        }
    }
    return false;
}

/* The host answers a connection up-call exactly once: either an error
 * from the up-call, or PMIX_SUCCESS and then the callback. A host that
 * gets this wrong must not cost the server its memory, so the two cases
 * that can be caught are: a second call of the callback, ignored while
 * the connection is still waiting for the first to be acted on; and a
 * callback followed by an error return, which is then the answer. A
 * callback that comes after the connection was refused cannot be told
 * from any other use of freed memory - see pmix_server_module_t(5). */
static bool first_answer(pmix_pending_connection_t *pnd)
{
    if (atomic_exchange(&pnd->answered, true)) {
        pmix_output(0, "PMIX ERROR: the host answered a connection request on socket %d "
                       "more than once - ignoring all but the first answer",
                    pnd->sd);
        return false;
    }
    return true;
}

/* What the up-call returned. The host answers through the callback when it
 * returns PMIX_SUCCESS (or, for a client, PMIX_OPERATION_SUCCEEDED with no
 * callback). Returns true if the caller is to act on `rc` itself */
static bool upcall_returned(pmix_pending_connection_t *pnd, pmix_status_t rc)
{
    if (PMIX_SUCCESS == rc) {
        return false;
    }
    if (pmix_atomic_check_bool(&pnd->answered)) {
        /* it answered through the callback as well - that answer is
         * queued, and acts on this one */
        pmix_output(0, "PMIX ERROR: the host both called back and returned %s for a "
                       "connection request on socket %d",
                    PMIx_Error_string(rc), pnd->sd);
        pnd->upcall_rc = (PMIX_OPERATION_SUCCEEDED == rc) ? PMIX_SUCCESS : rc;
        return false;
    }
    return true;
}

/* the host's answer for a client, on whatever thread it gave it */
static void client_host_cbfunc(pmix_status_t status, void *cbdata)
{
    pmix_pending_connection_t *pnd = (pmix_pending_connection_t *) cbdata;

    if (!first_answer(pnd)) {
        return;
    }
    pnd->status = status;
    PMIX_THREADSHIFT(pnd, client_answered);
}

/* The client is who its registration says. Ask the host */
static void client_settled(pmix_pending_connection_t *pnd, pmix_status_t status)
{
    pmix_rank_info_t *info = (pmix_rank_info_t *) pnd->rinfo;
    pmix_peer_t *peer = (pmix_peer_t *) pnd->peer;
    pmix_proc_t proc;
    void *ilist;
    pmix_data_array_t darray;
    pid_t kpid;
    int tmo;
    pmix_status_t rc;

    if (PMIX_SUCCESS != status) {
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "validation of client connection failed");
        refuse(pnd, status);
        return;
    }
    pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                        "client connection validated");

    /* what its info says - read only now that it is who it says */
    rc = read_client_info(pnd);
    if (PMIX_SUCCESS != rc) {
        refuse(pnd, rc);
        return;
    }

    /* A process its host prepared with PMIx_server_setup_fork was given a
     * token for its rank, which a client of this release presents - an
     * earlier release has none to present */
    if (PMIX_PTL_LOCAL_MAJOR <= pnd->proc_type.major) {
        rc = pmix_ptl_base_check_token(info->pname.nspace, info->pname.rank, pnd->token);
        if (PMIX_SUCCESS != rc) {
            pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                                "refusing %s:%u - it did not present its rank's token",
                                info->pname.nspace, info->pname.rank);
            refuse(pnd, rc);
            return;
        }
    }

    /* The process's pid, as the kernel reports it for this socket, is the
     * one this host can act on - it is in our pid namespace. On the local
     * transport it is the only one: a client whose kernel gives none has
     * none. Over TCP the pid the client's library sent is used */
    if (local_peer(pnd)) {
        pnd->pid = kernel_pid(pnd);
    } else if (PMIX_SUCCESS == pmix_util_getpid(pnd->sd, &kpid)) {
        pnd->pid = kpid;
    }

    /* A rank has one connection at a time. A second one - from a process
     * the client forked, or the program it exec'd, say - is refused, and
     * the host is not asked about it */
    if (rank_connected(info)) {
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "refusing a second connection for %s:%u",
                            info->pname.nspace, info->pname.rank);
        refuse(pnd, PMIX_ERR_EXISTS);
        return;
    }

    /* The host has what is left of the time the client waits for its
     * answer. A client with none left is not worth asking about */
    if (0 > (tmo = host_timeout(pnd))) {
        refuse(pnd, PMIX_ERR_TIMEOUT);
        return;
    }
    pnd->host_tmo = tmo;

    ilist = PMIx_Info_list_start();
    if (0 < pnd->pid) {
        PMIx_Info_list_add(ilist, PMIX_PROC_PID, &pnd->pid, PMIX_PID);
    }
    PMIx_Info_list_add(ilist, PMIX_USERID, &peer->info->uid, PMIX_UINT32);
    PMIx_Info_list_add(ilist, PMIX_GRPID, &peer->info->gid, PMIX_UINT32);
    /* the security module that settled them */
    PMIx_Info_list_add(ilist, PMIX_CRED_TYPE, ((pmix_psec_module_t *) pnd->psecmod)->name,
                       PMIX_STRING);
    if (pnd->realuid_given) {
        PMIx_Info_list_add(ilist, PMIX_REALUID, &pnd->realuid, PMIX_UINT32);
    }
    if (pnd->realgid_given) {
        PMIx_Info_list_add(ilist, PMIX_REALGID, &pnd->realgid, PMIX_UINT32);
    }
    /* the time the host is asked to answer in */
    if (0 < tmo) {
        PMIx_Info_list_add(ilist, PMIX_TIMEOUT, &tmo, PMIX_INT);
    }
    rc = PMIx_Info_list_convert(ilist, &darray);
    PMIx_Info_list_release(ilist);
    if (PMIX_SUCCESS != rc) {
        /* the conversion creates its array before filling it */
        PMIX_DATA_ARRAY_DESTRUCT(&darray);
        refuse(pnd, rc);
        return;
    }
    pnd->info = (pmix_info_t *) darray.array;
    pnd->ninfo = darray.size;

    /* let the host server know that this client has connected. Until it
     * answers, this connection holds the rank: another arriving meanwhile
     * is refused */
    ++info->connecting;
    pnd->connecting = true;
    PMIX_LOAD_PROCID(&proc, info->pname.nspace, info->pname.rank);
    if (NULL != pmix_host_server.client_connected2 || NULL != pmix_host_server.client_connected) {
        await_host(pnd);
    }
    if (NULL != pmix_host_server.client_connected2) {
        rc = pmix_host_server.client_connected2(&proc, info->server_object, pnd->info,
                                                pnd->ninfo, client_host_cbfunc, pnd);
    } else if (NULL != pmix_host_server.client_connected) {
        rc = pmix_host_server.client_connected(&proc, info->server_object, client_host_cbfunc,
                                               pnd);
    } else {
        /* if neither is provided, then we simply assume the host is ready */
        rc = PMIX_OPERATION_SUCCEEDED;
    }
    if (!upcall_returned(pnd, rc)) {
        /* the host answers through the callback */
        return;
    }
    if (PMIX_OPERATION_SUCCEEDED == rc) {
        pmix_atomic_set_bool(&pnd->answered);
        pnd->status = PMIX_SUCCESS;
        client_answered(0, 0, pnd);
        return;
    }
    refuse(pnd, rc);
}

/* The host has answered */
static void client_answered(int sd, short args, void *cbdata)
{
    pmix_pending_connection_t *pnd = (pmix_pending_connection_t *) cbdata;
    pmix_namespace_t *nptr = (pmix_namespace_t *) pnd->nptr;
    pmix_rank_info_t *info = (pmix_rank_info_t *) pnd->rinfo;
    pmix_peer_t *peer = (pmix_peer_t *) pnd->peer;
    pmix_proc_t id;
    uint32_t u32;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_ACQUIRE_OBJECT(pnd);
    host_answered(pnd);
    PMIX_LOAD_PROCID(&id, info->pname.nspace, info->pname.rank);

    if (pnd->abandoned) {
        /* the client was refused when the host took too long - its rank
         * is free again once this is released */
        late_answer(pnd, &id);
        return;
    }
    /* the connection holds the rank until it is accepted, or until the
     * host has been told it was not */
    if (PMIX_SUCCESS != pnd->upcall_rc) {
        refuse(pnd, pnd->upcall_rc);
        return;
    }
    if (PMIX_SUCCESS != pnd->status) {
        refuse(pnd, pnd->status);
        return;
    }
    /* The host has accepted the client. From here, a client that cannot
     * be connected after all is one the host is told of */

    /* The host may have deregistered the client, or the job, while it
     * decided - and another client of the job may have connected
     * meanwhile and fixed its wire format */
    if (!nspace_listed(nptr) || info != find_rank(nptr, info->pname.rank)) {
        refuse_accepted(pnd, &id, PMIX_ERR_NOT_FOUND);
        return;
    }
    if (!wire_format_fits(pnd, nptr)) {
        refuse_accepted(pnd, &id, PMIX_ERR_NOT_SUPPORTED);
        return;
    }

    /* The connection is accepted. Take its place in the clients array
     * first, so that the index we are about to send is the one it has */
    reclaim_tombstone(info);
    peer->index = pmix_pointer_array_add(&pmix_server_globals.clients, peer);
    if (0 > peer->index) {
        refuse_accepted(pnd, &id, PMIX_ERR_NOMEM);
        return;
    }
    /* tell the client all is good, and send its array index */
    u32 = htonl((uint32_t) PMIX_SUCCESS);
    if (PMIX_SUCCESS != pmix_ptl_base_send_blocking(pnd->sd, (char *) &u32, sizeof(uint32_t)) ||
        (u32 = htonl((uint32_t) peer->index),
         PMIX_SUCCESS != pmix_ptl_base_send_blocking(pnd->sd, (char *) &u32, sizeof(uint32_t)))) {
        /* the client has gone */
        pmix_pointer_array_set_item(&pmix_server_globals.clients, peer->index, NULL);
        peer->index = -1;
        release_not_connected(pnd, &id);
        return;
    }
    pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                        "connect-ack from client completed");

    /* It is connected: the rank is held by its connection from here */
    if (pnd->connecting) {
        pnd->connecting = false;
        if (0 < info->connecting) {
            --info->connecting;
        }
    }

    /* now everything else the connection changes */
    if (0 == nptr->version.major) {
        nptr->version.major = pnd->proc_type.major;
        nptr->version.minor = pnd->proc_type.minor;
        nptr->version.release = pnd->proc_type.release;
    }
    /* a peer can connect on multiple sockets since it can fork/exec a
     * child that also calls PMIx_Init. Assign the upper half of the tag
     * space for sendrecvs */
    peer->dyn_tags_start = PMIX_PTL_TAG_DYNAMIC + (UINT32_MAX - PMIX_PTL_TAG_DYNAMIC) / 2 + 1;
    peer->dyn_tags_end = UINT32_MAX;
    peer->dyn_tags_current = peer->dyn_tags_start;
    /* the peer takes the references the connection held, on the real
     * namespace and rank entry in place of the scratch identity it was
     * validated with */
    peer->nptr = nptr;
    pnd->nptr = NULL;
    PMIX_RELEASE(peer->info);
    peer->info = info;
    pnd->rinfo = NULL;
    adopt_modules(pnd, peer, nptr);
    info->pid = pnd->pid;
    if (pnd->realuid_given) {
        info->realuid = pnd->realuid;
    }
    if (pnd->realgid_given) {
        info->realgid = pnd->realgid;
    }
    info->proc_cnt++; /* increase number of processes on this rank */
    info->peerid = peer->index;

    activate(pnd, peer);
}

/* A tool, launcher or scheduler: a process that may or may not have been
 * registered, and may or may not bring an identity. Its credential is
 * checked first, against the uid and gid it claimed - and a module that
 * establishes the identity some other way (the owner of its socket)
 * replaces the claim with what it found */
static void accept_tool(pmix_pending_connection_t *pnd)
{
    pmix_psec_module_t *psec;

    /* A server takes tools only when its host said it would serve them,
     * with PMIX_SERVER_TOOL_SUPPORT - however the tool came by the
     * server's address */
    if (!pmix_ptl_base.tool_support) {
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "refusing a tool - this server does not support tools");
        refuse(pnd, PMIX_ERR_NOT_SUPPORTED);
        return;
    }

    psec = pmix_psec_base_assign_module(pnd->psec);
    if (NULL == psec) {
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "tool asked for security module \"%s\", which is not available",
                            (NULL == pnd->psec) ? "(none)" : pnd->psec);
        refuse(pnd, PMIX_ERR_NOT_SUPPORTED);
        return;
    }
    pnd->psecmod = psec;
    pnd->bfropsmod = pmix_bfrops_base_assign_module(pnd->bfrops);
    if (NULL == pnd->bfropsmod) {
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "tool asked for wire format \"%s\", which is not available",
                            (NULL == pnd->bfrops) ? "(none)" : pnd->bfrops);
        refuse(pnd, PMIX_ERR_NOT_AVAILABLE);
        return;
    }
    pnd->peer = scratch_peer(pnd, pnd->proc.nspace, pnd->proc.rank, pnd->uid, pnd->gid, false);
    if (NULL == pnd->peer) {
        refuse(pnd, PMIX_ERR_NOMEM);
        return;
    }
    tool_settled(pnd, validate(pnd));
}

static bool nspace_is_users(pmix_namespace_t *nptr, uid_t uid, gid_t gid);

/* Whether a namespace is this uid and gid's: its owner - our own identity
 * where no owner was named - and every process in it */
static bool nspace_is_tools(pmix_namespace_t *nptr, uid_t uid, gid_t gid)
{
    uid_t ouid;
    gid_t ogid;

    if (PMIX_OWNER_UNKNOWN == nptr->access.source) {
        ouid = geteuid();
        ogid = getegid();
    } else {
        ouid = nptr->access.uid;
        ogid = nptr->access.gid;
    }
    return ouid == uid && ogid == gid && nspace_is_users(nptr, uid, gid);
}

/* Where a tool belongs: the namespace and rank entry it will join, when
 * either already exists, or ones built for it - which are not listed
 * until the connection is accepted. Returns the reason for a refusal */
static pmix_status_t place_tool(pmix_pending_connection_t *pnd)
{
    pmix_namespace_t *nptr = NULL;
    pmix_rank_info_t *info;
    pmix_peer_t *p2;
    pmix_pfexec_child_t *child;
    bool found;

    if (!host_names_tool(pnd)) {
        nptr = find_nspace(pnd->proc.nspace);
    }

    if (PMIX_TOOL_CLIENT == pnd->flag || PMIX_LAUNCHER_CLIENT == pnd->flag) {
        /* A tool we launched - the host may have registered it as a
         * client. It is possible that this is a tool inside of a
         * job-script as part of a multi-spawn operation: each tool
         * invocation may have finalized and terminated, causing us to
         * clean up all references to it, and then reappear. So it is not
         * refused for being unknown - the namespace and rank are built
         * for it, and the host decides whether that is allowed */
        if (NULL == nptr) {
            nptr = PMIX_NEW(pmix_namespace_t);
            if (NULL == nptr) {
                return PMIX_ERR_NOMEM;
            }
            nptr->nspace = strdup(pnd->proc.nspace);
            /* the tool's job belongs to the user it connected as */
            pmix_server_access_set_owner(nptr, pnd->uid, pnd->gid, PMIX_OWNER_REGISTERED);
            nptr->version.major = pnd->proc_type.major;
            nptr->version.minor = pnd->proc_type.minor;
            nptr->version.release = pnd->proc_type.release;
            pnd->nspace_created = true;
            pnd->nptr = nptr;
        } else {
            PMIX_RETAIN(nptr);
            pnd->nptr = nptr;
        }
        info = find_rank(nptr, pnd->proc.rank);
        if (NULL != info) {
            /* it must be the identity its rank was registered with */
            if (info->uid != pnd->uid || info->gid != pnd->gid) {
                pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                                    "tool %s:%u is uid %lu gid %lu - its rank is uid %lu gid %lu",
                                    pnd->proc.nspace, pnd->proc.rank, (unsigned long) pnd->uid,
                                    (unsigned long) pnd->gid, (unsigned long) info->uid,
                                    (unsigned long) info->gid);
                return PMIX_ERR_INVALID_CRED;
            }
            PMIX_RETAIN(info);
        } else {
            /* A rank the namespace does not have is built for the tool -
             * see above - but only in a namespace that is the tool's own
             * user's: its owner, and every process already in it. A rank
             * in it would make the tool one of the job's own processes */
            if (!pnd->nspace_created && !nspace_is_tools(nptr, pnd->uid, pnd->gid)) {
                pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                                    "tool %s:%u is uid %lu gid %lu - the namespace is not "
                                    "that user's",
                                    pnd->proc.nspace, pnd->proc.rank,
                                    (unsigned long) pnd->uid, (unsigned long) pnd->gid);
                return PMIX_ERR_NO_PERMISSIONS;
            }
            /* see above about not finding the namespace */
            info = PMIX_NEW(pmix_rank_info_t);
            if (NULL == info) {
                return PMIX_ERR_NOMEM;
            }
            info->pname.nspace = strdup(pnd->proc.nspace);
            info->pname.rank = pnd->proc.rank;
            info->uid = pnd->uid;
            info->gid = pnd->gid;
            pnd->rinfo_created = true;
        }
        pnd->rinfo = info;
        return PMIX_SUCCESS;
    }

    if (NULL != nptr) {
        /* A tool or launcher that names a namespace we already know, which
         * the host did not register. A prior instance of the tool must
         * have connected for us to know it, and every process of a
         * namespace is of one uid and gid */
        if (pmix_list_is_empty(&nptr->ranks)) {
            /* the namespace can only exist because a prior instance of
             * the tool already connected */
            return PMIX_ERR_NOT_SUPPORTED;
        }
        info = (pmix_rank_info_t *) pmix_list_get_first(&nptr->ranks);
        if (-1 == info->peerid) {
            /* the tool may be one we fork/exec'd ourselves - check the
             * local children to see */
            found = false;
            PMIX_LIST_FOREACH (child, &pmix_pfexec_globals.children, pmix_pfexec_child_t) {
                if (PMIx_Check_nspace(child->proc.nspace, nptr->nspace)) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                return PMIX_ERR_NOT_SUPPORTED;
            }
        } else {
            p2 = (pmix_peer_t *) pmix_pointer_array_get_item(&pmix_server_globals.clients,
                                                             info->peerid);
            if (NULL == p2) {
                return PMIX_ERR_NOT_FOUND;
            }
            if (!PMIX_PEER_IS_TOOL(p2)) {
                /* the entire namespace must be a tool's if this tool
                 * claims to be a member of it */
                return PMIX_ERR_NOT_SUPPORTED;
            }
        }
        if (info->uid != pnd->uid || info->gid != pnd->gid) {
            return PMIX_ERR_NOT_SUPPORTED;
        }
        PMIX_RETAIN(nptr);
        pnd->nptr = nptr;
        /* its rank entry is found, or built, once its identity is final */
        return PMIX_SUCCESS;
    }

    /* a namespace of its own - named now if the tool named it, else once
     * the host has given it an identity */
    nptr = PMIX_NEW(pmix_namespace_t);
    if (NULL == nptr) {
        return PMIX_ERR_NOMEM;
    }
    if (!host_names_tool(pnd)) {
        nptr->nspace = strdup(pnd->proc.nspace);
    }
    /* the tool's job belongs to the user it connected as */
    pmix_server_access_set_owner(nptr, pnd->uid, pnd->gid, PMIX_OWNER_REGISTERED);
    nptr->version.major = pnd->proc_type.major;
    nptr->version.minor = pnd->proc_type.minor;
    nptr->version.release = pnd->proc_type.release;
    pnd->nspace_created = true;
    pnd->nptr = nptr;
    return PMIX_SUCCESS;
}

/* The keys the server sets in what the host is given for a tool. Every
 * copy the tool sent of any of them is dropped - one it sent twice would
 * otherwise reach the host beside the server's value. The pid is the
 * exception: over TCP the first copy that holds a pid is kept, and replaced
 * by the kernel's where there is one; on the local transport only the
 * kernel's is given */
static bool server_sets(const char *key)
{
    return PMIx_Check_key(key, PMIX_USERID) || PMIx_Check_key(key, PMIX_GRPID) ||
           PMIx_Check_key(key, PMIX_CRED_TYPE) || PMIx_Check_key(key, PMIX_VERSION_INFO) ||
           PMIx_Check_key(key, PMIX_NSPACE) || PMIx_Check_key(key, PMIX_RANK) ||
           PMIx_Check_key(key, PMIX_TIMEOUT);
}

/* What a tool's connect-ack info says, for the host - less what the
 * server sets itself */
static pmix_status_t read_tool_info(pmix_pending_connection_t *pnd, void *ilist)
{
    pmix_bfrops_module_t *bfrops = (pmix_bfrops_module_t *) pnd->bfropsmod;
    pmix_buffer_t buf;
    pmix_info_t *iptr;
    size_t sz, n;
    int32_t foo;
    bool havepid = false;
    pmix_status_t rc;

    if (NULL == pnd->blob) {
        return PMIX_SUCCESS;
    }
    PMIX_CONSTRUCT(&buf, pmix_buffer_t);
    PMIX_LOAD_BUFFER_NON_DESTRUCT(pmix_globals.mypeer, &buf, pnd->blob, pnd->bloblen);
    buf.type = pnd->buffer_type;
    foo = 1;
    rc = bfrops->unpack(&buf, &sz, &foo, PMIX_SIZE);
    if (PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER == rc) {
        /* too short to hold even the count - see PMIX_PTL_LEGACY_PAD */
        return PMIX_SUCCESS;
    }
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    /* the payload must still hold sz packed pmix_info_t */
    if (!pmix_bfrop_count_fits(&buf, sz, PMIX_INFO)) {
        return PMIX_ERR_BAD_PARAM;
    }
    if (0 == sz) {
        return PMIX_SUCCESS;
    }
    PMIX_INFO_CREATE(iptr, sz);
    if (NULL == iptr) {
        return PMIX_ERR_NOMEM;
    }
    foo = (int32_t) sz;
    rc = bfrops->unpack(&buf, iptr, &foo, PMIX_INFO);
    if (PMIX_SUCCESS != rc) {
        pmix_ptl_base_wipe_token_info(iptr, sz);
        PMIX_INFO_FREE(iptr, sz);
        return rc;
    }
    for (n = 0; n < sz; n++) {
        if (server_sets(iptr[n].key)) {
            continue;
        }
        /* the token is checked here, and goes no further */
        if (PMIx_Check_key(iptr[n].key, PMIX_PTL_TOKEN_KEY)) {
            if (NULL == pnd->token && PMIX_STRING == iptr[n].value.type &&
                NULL != iptr[n].value.data.string) {
                pnd->token = strdup(iptr[n].value.data.string);
            }
            continue;
        }
        if (PMIx_Check_key(iptr[n].key, PMIX_PROC_PID)) {
            /* on the local transport the kernel's pid is the only one */
            if (havepid || PMIX_PID != iptr[n].value.type || local_peer(pnd)) {
                continue;
            }
            havepid = true;
        }
        PMIx_Info_list_xfer(ilist, &iptr[n]);
    }
    pmix_ptl_base_wipe_token_info(iptr, sz);
    PMIX_INFO_FREE(iptr, sz);
    return PMIX_SUCCESS;
}

/* The tool is who its credential says. Check that it may connect at all,
 * then put it in the queue for the host */
static void tool_settled(pmix_pending_connection_t *pnd, pmix_status_t status)
{
    pmix_peer_t *vpeer = (pmix_peer_t *) pnd->peer;

    if (PMIX_SUCCESS != status) {
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "validation of tool credentials failed: %s",
                            PMIx_Error_string(status));
        refuse(pnd, status);
        return;
    }
    /* the identity validation settled on - the scratch peer has served
     * its purpose */
    pnd->uid = vpeer->info->uid;
    pnd->gid = vpeer->info->gid;
    PMIX_RELEASE(vpeer);
    pnd->peer = NULL;

    /* a tool of another user is refused unless allowed */
    if (!pmix_ptl_base.allow_foreign_tools && pnd->uid != pmix_globals.uid) {
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "refusing a tool of uid %lu - this server does not accept "
                            "tools of other users",
                            (unsigned long) pnd->uid);
        refuse(pnd, PMIX_ERR_NOT_SUPPORTED);
        return;
    }

    /* A tool waiting on the host holds its connection as one still sending
     * its connect-ack does. Waiting tools have a cap of their own, the
     * same size as that on connections still sending: at it, a further
     * tool is refused */
    if (0 < pmix_ptl_base.pending_cap &&
        (size_t) pmix_ptl_base.pending_cap <= pmix_ptl_base_waiting_tools()) {
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "refusing a tool - %d tools are waiting on the host",
                            pmix_ptl_base.pending_cap);
        refuse(pnd, PMIX_ERR_OUT_OF_RESOURCE);
        return;
    }

    /* The host is asked about one tool of a name at a time, so that it
     * never decides on two tools for one name at once; tools of other
     * names, and tools it is to name, it is asked about together. Where
     * the tool belongs is worked out when its turn comes - by then the
     * tool of its name ahead of it has been decided on */
    pmix_list_append(&pmix_ptl_base.tool_queue, &pnd->super);
    next_tool();
}

/* The host is no longer deciding on this tool: ask about the next one -
 * from an event of its own, so that one answer never runs the queue from
 * inside another */
static void kick_tool_queue(void)
{
    pmix_shift_caddy_t *scd;

    if (pmix_list_is_empty(&pmix_ptl_base.tool_queue)) {
        return;
    }
    scd = PMIX_NEW(pmix_shift_caddy_t);
    if (NULL == scd) {
        /* the next tool to settle runs the queue */
        return;
    }
    PMIX_THREADSHIFT(scd, next_tool_cb);
}

static void tool_decided(pmix_pending_connection_t *pnd)
{
    if (!pnd->asking) {
        return;
    }
    pmix_list_remove_item(&pmix_ptl_base.tool_asking, &pnd->super);
    pnd->asking = false;
    kick_tool_queue();
}

/* Whether a tool of this tool's name is ahead of it: one the host is
 * deciding on, or one not yet done with - the server stopped waiting on
 * the host's answer for it, or the host accepted it and is being told it
 * is not connected, which must not name a tool accepted since. A tool
 * that needs an identity has no name yet, and nothing is ahead of it: the
 * host keeps the names it gives unique */
static bool name_busy(pmix_pending_connection_t *pnd)
{
    pmix_pending_connection_t *ahead;
    const pmix_proc_t *held;

    if (host_names_tool(pnd)) {
        return false;
    }
    PMIX_LIST_FOREACH (ahead, &pmix_ptl_base.tool_asking, pmix_pending_connection_t) {
        if (!host_names_tool(ahead) && ahead->proc.rank == pnd->proc.rank &&
            PMIx_Check_nspace(ahead->proc.nspace, pnd->proc.nspace)) {
            return true;
        }
    }
    PMIX_LIST_FOREACH (ahead, &pmix_ptl_base.tool_late, pmix_pending_connection_t) {
        held = tool_name(ahead);
        if (held->rank == pnd->proc.rank && PMIx_Check_nspace(held->nspace, pnd->proc.nspace)) {
            return true;
        }
    }
    return false;
}

static void next_tool_cb(int sd, short args, void *cbdata)
{
    pmix_shift_caddy_t *scd = (pmix_shift_caddy_t *) cbdata;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_ACQUIRE_OBJECT(scd);
    PMIX_RELEASE(scd);
    next_tool();
}

/* Ask the host about every queued tool with no tool of its name ahead of
 * it, in the order they arrived. A tool that is refused before the host
 * is asked makes way at once. One that has closed its connection while it
 * waited - its own connect timed out, say - is dropped: there is nobody to
 * accept */
static void next_tool(void)
{
    pmix_pending_connection_t *pnd, *pnext;
    bool asked;

    do {
        asked = false;
        PMIX_LIST_FOREACH_SAFE (pnd, pnext, &pmix_ptl_base.tool_queue,
                                pmix_pending_connection_t) {
            if (pmix_ptl_base_sd_closed(pnd->sd)) {
                pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                                    "ptl:base: the tool on socket %d went away while it "
                                    "waited for the host - dropping it",
                                    pnd->sd);
                pmix_list_remove_item(&pmix_ptl_base.tool_queue, &pnd->super);
                PMIX_RELEASE(pnd);
                continue;
            }
            if (name_busy(pnd)) {
                /* it waits on, behind the tool of its name */
                continue;
            }
            pmix_list_remove_item(&pmix_ptl_base.tool_queue, &pnd->super);
            ask_about_tool(pnd);
            /* asking, or a refusal, may have changed the queue - start
             * again from the head */
            asked = true;
            break;
        }
    } while (asked);
}

/* Two tools that cannot both be accepted: the one the host has just
 * accepted, as `id` and of `uid` and `gid`, and a queued one that names
 * the same rank, or the same namespace as another user. The queued one
 * is refused now, without asking the host */
static void refuse_queued_conflicts(const pmix_proc_t *id, uid_t uid, gid_t gid)
{
    pmix_pending_connection_t *q, *qnext;

    PMIX_LIST_FOREACH_SAFE (q, qnext, &pmix_ptl_base.tool_queue, pmix_pending_connection_t) {
        if (host_names_tool(q) || !PMIx_Check_nspace(q->proc.nspace, id->nspace)) {
            continue;
        }
        if (q->proc.rank == id->rank) {
            pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                                "refusing a queued tool connecting as %s:%u - the host has "
                                "accepted a tool of that name",
                                q->proc.nspace, q->proc.rank);
            pmix_list_remove_item(&pmix_ptl_base.tool_queue, &q->super);
            refuse(q, PMIX_ERR_EXISTS);
        } else if (q->uid != uid || q->gid != gid) {
            pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                                "refusing a queued tool of uid %lu joining %s - the host has "
                                "accepted a tool of uid %lu into it",
                                (unsigned long) q->uid, q->proc.nspace, (unsigned long) uid);
            pmix_list_remove_item(&pmix_ptl_base.tool_queue, &q->super);
            refuse(q, PMIX_ERR_NOT_SUPPORTED);
        }
    }
}

/* The host has not answered in the time it was asked to answer in, and
 * the process has stopped waiting too. Refuse it, and keep the connection
 * only for the host's answer when it comes - the host holds it until then.
 * A client's rank, and a tool's name, stay held until the answer: if it
 * is yes, the host is told the process is not connected, and that must
 * not name a process accepted since. The next tool is asked about */
static void host_expired(int sd, short args, void *cbdata)
{
    pmix_pending_connection_t *pnd = (pmix_pending_connection_t *) cbdata;
    bool tool = pnd->asking;
    uint32_t u32;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_ACQUIRE_OBJECT(pnd);
    pnd->timer_active = false;
    if (pmix_atomic_check_bool(&pnd->answered)) {
        /* the answer is on its way to us */
        return;
    }
    pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                        "ptl:base: the host has not answered for the connection on socket %d "
                        "in %d seconds - refusing it",
                        pnd->sd, pnd->host_tmo + PMIX_PTL_HOST_GRACE_SEC);
    pnd->abandoned = true;
    if (0 <= pnd->sd) {
        u32 = htonl((uint32_t) PMIX_ERR_TIMEOUT);
        (void) pmix_ptl_base_send_blocking(pnd->sd, (char *) &u32, sizeof(uint32_t));
        CLOSE_THE_SOCKET(pnd->sd);
        pnd->sd = -1;
    }
    if (tool) {
        tool_decided(pnd);
        if (!host_names_tool(pnd)) {
            pmix_list_append(&pmix_ptl_base.tool_late, &pnd->super);
            pnd->late_listed = true;
        }
    }
}

/* The tool's turn: check where it belongs, then ask the host. Until the
 * host answers, the tool is on pmix_ptl_base.tool_asking */
static void ask_about_tool(pmix_pending_connection_t *pnd)
{
    pmix_namespace_t *nptr;
    pmix_rank_info_t *info;
    void *ilist;
    pmix_data_array_t darray;
    pid_t kpid;
    size_t n;
    int tmo;
    pmix_status_t rc;

    rc = place_tool(pnd);
    if (PMIX_SUCCESS != rc) {
        refuse(pnd, rc);
        return;
    }
    nptr = (pmix_namespace_t *) pnd->nptr;
    /* A tool that has waited in the queue for as long as it waits for a
     * reply has given up: it is not worth asking about. The host has what
     * is left of the wait */
    if (0 > (tmo = host_timeout(pnd))) {
        refuse(pnd, PMIX_ERR_TIMEOUT);
        return;
    }
    pnd->host_tmo = tmo;
    {
        pmix_info_t ginfo;

        PMIX_INFO_LOAD(&ginfo, PMIX_GDS_MODULE, pnd->gds, PMIX_STRING);
        pnd->gdsmod = pmix_gds_base_assign_module(&ginfo, 1);
        PMIX_INFO_DESTRUCT(&ginfo);
        if (NULL == pnd->gdsmod) {
            refuse(pnd, PMIX_ERR_NOT_SUPPORTED);
            return;
        }
    }

    ilist = PMIx_Info_list_start();
    rc = read_tool_info(pnd, ilist);
    if (PMIX_SUCCESS != rc) {
        PMIx_Info_list_release(ilist);
        refuse(pnd, rc);
        return;
    }
    /* A tool of this release that names a rank its host prepared a process
     * for with PMIx_server_setup_fork - one started on a client's rank -
     * presents that rank's token */
    if (PMIX_PTL_LOCAL_MAJOR <= pnd->proc_type.major && !host_names_tool(pnd)) {
        rc = pmix_ptl_base_check_token(pnd->proc.nspace, pnd->proc.rank, pnd->token);
        if (PMIX_SUCCESS != rc) {
            pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                                "refusing a tool as %s:%u - it did not present the rank's token",
                                pnd->proc.nspace, pnd->proc.rank);
            PMIx_Info_list_release(ilist);
            refuse(pnd, rc);
            return;
        }
    }
    /* provide the version, and the identity validation settled - and
     * the pid, where the kernel reports one for this socket, in place of
     * the one the tool sent. Any other the tool put in its own info was
     * dropped as it was read */
    PMIx_Info_list_add_unique(ilist, PMIX_VERSION_INFO, pnd->version, PMIX_STRING, true);
    PMIx_Info_list_add_unique(ilist, PMIX_USERID, &pnd->uid, PMIX_UINT32, true);
    if (local_peer(pnd)) {
        kpid = kernel_pid(pnd);
        if (0 < kpid) {
            PMIx_Info_list_add_unique(ilist, PMIX_PROC_PID, &kpid, PMIX_PID, true);
        }
    } else if (PMIX_SUCCESS == pmix_util_getpid(pnd->sd, &kpid)) {
        PMIx_Info_list_add_unique(ilist, PMIX_PROC_PID, &kpid, PMIX_PID, true);
    }
    PMIx_Info_list_add_unique(ilist, PMIX_GRPID, &pnd->gid, PMIX_UINT32, true);
    /* the security module that settled them */
    PMIx_Info_list_add_unique(ilist, PMIX_CRED_TYPE, ((pmix_psec_module_t *) pnd->psecmod)->name,
                              PMIX_STRING, true);
    /* if we have it, pass along their ID */
    if (!host_names_tool(pnd)) {
        PMIx_Info_list_add_unique(ilist, PMIX_NSPACE, pnd->proc.nspace, PMIX_STRING, true);
        PMIx_Info_list_add_unique(ilist, PMIX_RANK, &pnd->proc.rank, PMIX_PROC_RANK, true);
    }
    /* the time the host is asked to answer in */
    if (0 < tmo) {
        PMIx_Info_list_add_unique(ilist, PMIX_TIMEOUT, &tmo, PMIX_INT, true);
    }
    rc = PMIx_Info_list_convert(ilist, &darray);
    PMIx_Info_list_release(ilist);
    if (PMIX_SUCCESS != rc) {
        /* the conversion creates its array before filling it */
        PMIX_DATA_ARRAY_DESTRUCT(&darray);
        refuse(pnd, rc);
        return;
    }
    pnd->info = (pmix_info_t *) darray.array;
    pnd->ninfo = darray.size;
    /* the tool's pid: the kernel's, else the one the tool's library sent */
    for (n = 0; n < pnd->ninfo; n++) {
        if (PMIx_Check_key(pnd->info[n].key, PMIX_PROC_PID) &&
            PMIX_PID == pnd->info[n].value.type) {
            pnd->pid = pnd->info[n].value.data.pid;
            break;
        }
    }

    /* A rank has one connection at a time, so a tool joining a rank that
     * already has one is refused, as a client is. The rank entry it joins
     * is known now if it was found for a tool the host launched, or is the
     * one its namespace already lists for the rank it named */
    info = pnd->rinfo_created ? NULL : (pmix_rank_info_t *) pnd->rinfo;
    if (NULL == info && NULL == pnd->rinfo && !host_names_tool(pnd)) {
        info = find_rank(nptr, pnd->proc.rank);
        if (NULL != info) {
            /* the entry it joins, held while the host decides */
            PMIX_RETAIN(info);
            pnd->rinfo = info;
        }
    }
    if (NULL != info && rank_connected(info)) {
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "refusing a tool connecting as %s:%u - the rank is connected",
                            info->pname.nspace, info->pname.rank);
        refuse(pnd, PMIX_ERR_EXISTS);
        return;
    }
    /* Its wire format must be its namespace's - checked after the rank, so
     * that a tool whose rank is taken is told that, and not that its
     * format does not fit: a tool started on a client's rank falls back
     * to a namespace of its own on the one, where it can use its own
     * format, and not on the other */
    if (!wire_format_fits(pnd, nptr)) {
        refuse(pnd, PMIX_ERR_NOT_SUPPORTED);
        return;
    }

    /* does the host support tool connections? */
    if (NULL == pmix_host_server.tool_connected &&
        NULL == pmix_host_server.tool_connected2 && host_names_tool(pnd)) {
        /* someone must give the tool an identity, and nobody can */
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "refusing a tool that needs an identity - the host "
                            "provides no tool_connected upcall");
        refuse(pnd, PMIX_ERR_NOT_SUPPORTED);
        return;
    }

    pmix_list_append(&pmix_ptl_base.tool_asking, &pnd->super);
    pnd->asking = true;
    if (NULL != info) {
        /* until it is accepted, or refused, this connection holds the
         * rank */
        ++info->connecting;
        pnd->connecting = true;
    }
    if (NULL == pmix_host_server.tool_connected && NULL == pmix_host_server.tool_connected2) {
        /* nothing to ask: a host that takes every tool */
        cnct_cbfunc(PMIX_SUCCESS, &pnd->proc, (void *) pnd);
        return;
    }

    await_host(pnd);
    if (NULL != pmix_host_server.tool_connected2) {
        rc = pmix_host_server.tool_connected2(pnd->info, pnd->ninfo, cnct_cbfunc, pnd);
        if (PMIX_OPERATION_SUCCEEDED == rc && !pmix_atomic_check_bool(&pnd->answered)) {
            /* a tool's identity can only come back through the callback */
            pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                                "tool_connected2 returned PMIX_OPERATION_SUCCEEDED, which "
                                "gives the tool no identity - refusing it");
            tool_decided(pnd);
            refuse(pnd, PMIX_ERR_NOT_SUPPORTED);
            return;
        }
        if (upcall_returned(pnd, rc)) {
            tool_decided(pnd);
            refuse(pnd, rc);
        }
        return;
    }
    pmix_host_server.tool_connected(pnd->info, pnd->ninfo, cnct_cbfunc, pnd);
}

/* receive the host's answer for a tool, on whatever thread it gave it */
static void cnct_cbfunc(pmix_status_t status, pmix_proc_t *proc, void *cbdata)
{
    pmix_pending_connection_t *pnd = (pmix_pending_connection_t *) cbdata;

    if (!first_answer(pnd)) {
        return;
    }
    pnd->status = status;
    if (NULL != proc) {
        PMIX_LOAD_PROCID(&pnd->assigned, proc->nspace, proc->rank);
    }
    pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                        "pmix:tool:cnct_cbfunc returning %s:%d %s", pnd->assigned.nspace,
                        pnd->assigned.rank, PMIx_Error_string(status));
    PMIX_THREADSHIFT(pnd, tool_answered);
}

/* Whether every process of a namespace is of this uid and gid */
static bool nspace_is_users(pmix_namespace_t *nptr, uid_t uid, gid_t gid)
{
    pmix_rank_info_t *info;

    PMIX_LIST_FOREACH (info, &nptr->ranks, pmix_rank_info_t) {
        if (info->uid != uid || info->gid != gid) {
            return false;
        }
    }
    return true;
}

/* The tool's namespace has its final name now. If the host registered a
 * namespace of that name while it decided - or one was registered
 * meanwhile by another means - the tool joins that one: the server
 * resolves a namespace by scanning its list, so two objects for one name
 * would leave half the library reaching the tool through each */
static pmix_namespace_t *consolidate_nspace(pmix_pending_connection_t *pnd,
                                            pmix_namespace_t *nptr)
{
    pmix_namespace_t *existing;

    existing = find_nspace(nptr->nspace);
    if (NULL == existing) {
        return nptr;
    }
    /* someone got there first - use their object, and drop ours */
    PMIX_RETAIN(existing);
    PMIX_RELEASE(nptr);
    pnd->nptr = existing;
    pnd->nspace_created = false;
    return existing;
}

/* The host has answered for the tool */
static void tool_answered(int sd, short args, void *cbdata)
{
    pmix_pending_connection_t *pnd = (pmix_pending_connection_t *) cbdata;
    pmix_namespace_t *nptr = (pmix_namespace_t *) pnd->nptr;
    pmix_rank_info_t *info;
    pmix_peer_t *peer;
    pmix_iof_req_t *req;
    pmix_proc_t id;
    uint32_t u32;
    bool named, accepted;
    pmix_status_t reply = PMIX_SUCCESS;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_ACQUIRE_OBJECT(pnd);
    host_answered(pnd);

    /* the host has answered - the next tool may be asked about. This
     * connection holds its rank until it is accepted, or until the host
     * has been told it was not */
    tool_decided(pnd);
    /* A tool that brought its own identity keeps it - the host's answer is
     * not sent to it, so it is not what the tool will use. One that did not
     * is known by the host's, which is sent to one that asked for it */
    named = !host_names_tool(pnd) ||
            ('\0' != pnd->assigned.nspace[0] && PMIX_RANK_IS_VALID(pnd->assigned.rank));
    if (host_names_tool(pnd)) {
        PMIX_LOAD_PROCID(&id, pnd->assigned.nspace, pnd->assigned.rank);
    } else {
        PMIX_LOAD_PROCID(&id, pnd->proc.nspace, pnd->proc.rank);
    }
    if (pnd->abandoned) {
        /* the tool was refused when the host took too long - its name is
         * free again once this is released */
        late_answer(pnd, named ? &id : NULL);
        return;
    }
    if (PMIX_SUCCESS != pnd->upcall_rc) {
        pnd->status = pnd->upcall_rc;
    }
    /* a tool that brings no identity must be given one */
    if (PMIX_SUCCESS == pnd->status && !named) {
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "the host accepted a tool without giving it an identity - "
                            "refusing it");
        pnd->status = PMIX_ERR_NOT_SUPPORTED;
    }
    /* the host's answer is the status the tool reads now */
    accepted = (PMIX_SUCCESS == pnd->status);
    if (!accepted) {
        refuse(pnd, pnd->status);
        return;
    }
    u32 = htonl((uint32_t) pnd->status);
    if (PMIX_SUCCESS != pmix_ptl_base_send_blocking(pnd->sd, (char *) &u32, sizeof(uint32_t))) {
        /* the tool has gone */
        release_not_connected(pnd, &id);
        return;
    }
    /* The host has accepted the tool. From here, a tool that cannot be
     * connected after all is one the host is told of */
    if (host_names_tool(pnd) && !pnd->id_unsent) {
        if (PMIX_SUCCESS != pmix_ptl_base_send_blocking(pnd->sd, id.nspace, PMIX_MAX_NSLEN + 1) ||
            (pnd->need_id &&
             (u32 = htonl((uint32_t) id.rank),
              PMIX_SUCCESS != pmix_ptl_base_send_blocking(pnd->sd, (char *) &u32,
                                                          sizeof(uint32_t))))) {
            release_not_connected(pnd, &id);
            return;
        }
    }
    /* send my nspace and rank back to the tool */
    u32 = htonl((uint32_t) pmix_globals.myid.rank);
    if (PMIX_SUCCESS != pmix_ptl_base_send_blocking(pnd->sd, pmix_globals.myid.nspace,
                                                    PMIX_MAX_NSLEN + 1) ||
        PMIX_SUCCESS != pmix_ptl_base_send_blocking(pnd->sd, (char *) &u32, sizeof(uint32_t))) {
        release_not_connected(pnd, &id);
        return;
    }
    /* What is left for the tool to read is the last status: anything that
     * turns it away from here on is reported there */

    if (pnd->nspace_created) {
        if (NULL == nptr->nspace) {
            nptr->nspace = strdup(id.nspace);
            if (NULL == nptr->nspace) {
                refuse_accepted(pnd, &id, PMIX_ERR_NOMEM);
                return;
            }
        }
        nptr = consolidate_nspace(pnd, nptr);
        /* one that appeared while the host decided - another tool's,
         * decided on at the same time - is joined only by its own user, as
         * a namespace known before the host was asked is */
        if (!pnd->nspace_created && !nspace_is_users(nptr, pnd->uid, pnd->gid)) {
            refuse_accepted(pnd, &id, PMIX_ERR_NOT_SUPPORTED);
            return;
        }
    } else if (!nspace_listed(nptr)) {
        /* the host deregistered it while it decided */
        refuse_accepted(pnd, &id, PMIX_ERR_NOT_FOUND);
        return;
    }
    if (!wire_format_fits(pnd, nptr)) {
        refuse_accepted(pnd, &id, PMIX_ERR_NOT_SUPPORTED);
        return;
    }

    /* its rank entry: the one found or built for a tool the host
     * launched, else the namespace's entry for this rank - one already
     * there is shared, not duplicated - or a new one */
    info = (pmix_rank_info_t *) pnd->rinfo;
    if (NULL != info && !pnd->rinfo_created && info != find_rank(nptr, info->pname.rank)) {
        /* the host deregistered the rank while it decided */
        refuse_accepted(pnd, &id, PMIX_ERR_NOT_FOUND);
        return;
    }
    if (NULL != info && pnd->rinfo_created && !pnd->nspace_created) {
        /* built before we knew whether the namespace would have one */
        pmix_rank_info_t *found = find_rank(nptr, info->pname.rank);
        if (NULL != found) {
            /* the rank's identity is its entry's */
            if (found->uid != pnd->uid || found->gid != pnd->gid) {
                refuse_accepted(pnd, &id, PMIX_ERR_NOT_SUPPORTED);
                return;
            }
            PMIX_RELEASE(info);
            PMIX_RETAIN(found);
            pnd->rinfo = info = found;
            pnd->rinfo_created = false;
        }
    }
    if (NULL == info) {
        info = find_rank(nptr, id.rank);
        if (NULL != info) {
            if (info->uid != pnd->uid || info->gid != pnd->gid) {
                refuse_accepted(pnd, &id, PMIX_ERR_NOT_SUPPORTED);
                return;
            }
            PMIX_RETAIN(info);
        } else {
            info = PMIX_NEW(pmix_rank_info_t);
            if (NULL == info) {
                refuse_accepted(pnd, &id, PMIX_ERR_NOMEM);
                return;
            }
            info->pname.nspace = strdup(nptr->nspace);
            info->pname.rank = id.rank;
            info->uid = pnd->uid;
            info->gid = pnd->gid;
            pnd->rinfo_created = true;
        }
        pnd->rinfo = info;
    }
    /* The connection no longer holds the rank by waiting on it: it is about
     * to hold it by being accepted. Another connection for the rank may
     * have been accepted while the host decided on a tool it did not hold
     * the rank for */
    if (pnd->connecting) {
        pnd->connecting = false;
        if (0 < info->connecting) {
            --info->connecting;
        }
    }
    if (!pnd->rinfo_created && rank_connected(info)) {
        refuse_accepted(pnd, &id, PMIX_ERR_EXISTS);
        return;
    }

    /* the peer, and the output forwarding every tool gets */
    peer = PMIX_NEW(pmix_peer_t);
    req = PMIX_NEW(pmix_iof_req_t);
    if (NULL == peer || NULL == req) {
        if (NULL != peer) {
            PMIX_RELEASE(peer);
        }
        if (NULL != req) {
            PMIX_RELEASE(req);
        }
        refuse_accepted(pnd, &id, PMIX_ERR_NOMEM);
        return;
    }
    peer->index = -1;
    pnd->peer = peer;
    reclaim_tombstone(info);
    peer->index = pmix_pointer_array_add(&pmix_server_globals.clients, peer);
    if (0 > peer->index) {
        PMIX_RELEASE(req);
        refuse_accepted(pnd, &id, PMIX_ERR_NOMEM);
        return;
    }

    /* The tool is accepted. Its namespace and rank entry join their lists
     * - the references the connection held become the lists' - and the
     * peer takes references of its own */
    if (pnd->nspace_created) {
        pmix_list_append(&pmix_globals.nspaces, &nptr->super);
        PMIX_RETAIN(nptr);
    }
    pnd->nptr = NULL;
    peer->nptr = nptr;
    if (pnd->rinfo_created) {
        pmix_list_append(&nptr->ranks, &info->super);
        PMIX_RETAIN(info);
    }
    pnd->rinfo = NULL;
    peer->info = info;
    info->peerid = peer->index;
    info->pid = pnd->pid;
    /* a live process on the rank - the count every departure gives back,
     * and the one a reconnect's tombstone reclaim waits on */
    info->proc_cnt++;

    memcpy(&peer->proc_type, &pnd->proc_type, sizeof(pmix_proc_type_t));
    peer->protocol = pnd->protocol;
    peer->transport = pnd->transport;
    peer->proc_cnt = 1;
    peer->dyn_tags_start = PMIX_PTL_TAG_DYNAMIC + (UINT32_MAX - PMIX_PTL_TAG_DYNAMIC) / 2 + 1;
    peer->dyn_tags_end = UINT32_MAX;
    peer->dyn_tags_current = peer->dyn_tags_start;
    adopt_modules(pnd, peer, nptr);

    /* automatically setup to forward output to the tool */
    PMIX_RETAIN(peer);
    req->requestor = peer;
    req->nprocs = 1;
    PMIX_PROC_CREATE(req->procs, req->nprocs);
    PMIX_LOAD_PROCID(&req->procs[0], pmix_globals.myid.nspace, pmix_globals.myid.rank);
    req->channels = PMIX_FWD_STDOUT_CHANNEL | PMIX_FWD_STDERR_CHANNEL | PMIX_FWD_STDDIAG_CHANNEL;
    // default to formatting output as we were directed to do
    req->flags = pmix_globals.iof_flags;
    req->remote_id = 0; // default ID for tool during init
    req->local_id = pmix_pointer_array_add(&pmix_globals.iof_requests, req);

    /* the last status the tool reads. If it is not there to read it, its
     * connection closing is handled as a lost connection */
    u32 = htonl((uint32_t) reply);
    (void) pmix_ptl_base_send_blocking(pnd->sd, (char *) &u32, sizeof(uint32_t));

    /* tools still waiting their turn that can no longer be accepted are
     * refused now, rather than put to the host */
    refuse_queued_conflicts(&id, pnd->uid, pnd->gid);
    activate(pnd, peer);
}

static void _check_cached_events(pmix_peer_t *peer)
{
    pmix_notify_caddy_t *cd;
    int i;
    size_t n;
    pmix_range_trkr_t rngtrk;
    pmix_buffer_t *relay;
    pmix_proc_t proc;
    pmix_status_t ret;
    pmix_cmd_t cmd = PMIX_NOTIFY_CMD;
    bool matched, found;

    PMIX_LOAD_PROCID(&proc, peer->info->pname.nspace, peer->info->pname.rank);

    for (i = 0; i < pmix_globals.max_events; i++) {
        pmix_hotel_knock(&pmix_globals.notifications, i, (void **) &cd);
        if (NULL == cd) {
            continue;
        }
        /* check the range */
        if (NULL == cd->targets) {
            rngtrk.procs = &cd->source;
            rngtrk.nprocs = 1;
        } else {
            rngtrk.procs = cd->targets;
            rngtrk.nprocs = cd->ntargets;
        }
        rngtrk.range = cd->range;
        if (!pmix_notify_check_range(&rngtrk, &proc)) {
            continue;
        }
        found = false;
        /* if we were given specific targets, check if this is one */
        if (NULL != cd->targets) {
            matched = false;
            for (n = 0; n < cd->ntargets; n++) {
                if (PMIX_CHECK_PROCID(&proc, &cd->targets[n])) {
                    matched = true;
                    break;
                }
            }
            if (!matched) {
                /* do not notify this one */
                continue;
            }
        }

        /* a peer can reach a replay of the same cached event more than
         * once - this one, and again from the registration replay in
         * pmix_server_events.c every time it registers another handler -
         * so skip anything it has already been sent rather than
         * delivering it twice and decrementing "nleft" twice */
        if (pmix_notify_mark_notified(cd, &proc)) {
            continue;
        }
        if (NULL != cd->targets && 0 < cd->nleft) {
            /* track the number of targets we have left to notify */
            --cd->nleft;
            /* if this is the last one, then evict this event
             * from the cache */
            if (0 == cd->nleft) {
                pmix_hotel_checkout(&pmix_globals.notifications, cd->room);
                found = true; // mark that we should release cd
            }
        }

        /* all matches - notify */
        relay = PMIX_NEW(pmix_buffer_t);
        if (NULL == relay) {
            /* nothing we can do */
            PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
            if (found) {
                PMIX_RELEASE(cd);
            }
            break;
        }
        /* pack the info data stored in the event */
        PMIX_BFROPS_PACK(ret, peer, relay, &cmd, 1, PMIX_COMMAND);
        if (PMIX_SUCCESS != ret) {
            PMIX_ERROR_LOG(ret);
            PMIX_RELEASE(relay);
            if (found) {
                /* we already checked this event out of the cache, so
                 * nothing else will ever release it */
                PMIX_RELEASE(cd);
            }
            break;
        }
        PMIX_BFROPS_PACK(ret, peer, relay, &cd->status, 1, PMIX_STATUS);
        if (PMIX_SUCCESS != ret) {
            PMIX_ERROR_LOG(ret);
            PMIX_RELEASE(relay);
            if (found) {
                /* we already checked this event out of the cache, so
                 * nothing else will ever release it */
                PMIX_RELEASE(cd);
            }
            break;
        }
        PMIX_BFROPS_PACK(ret, peer, relay, &cd->source, 1, PMIX_PROC);
        if (PMIX_SUCCESS != ret) {
            PMIX_RELEASE(relay);
            PMIX_ERROR_LOG(ret);
            if (found) {
                /* we already checked this event out of the cache, so
                 * nothing else will ever release it */
                PMIX_RELEASE(cd);
            }
            break;
        }
        PMIX_BFROPS_PACK(ret, peer, relay, &cd->ninfo, 1, PMIX_SIZE);
        if (PMIX_SUCCESS != ret) {
            PMIX_ERROR_LOG(ret);
            PMIX_RELEASE(relay);
            if (found) {
                /* we already checked this event out of the cache, so
                 * nothing else will ever release it */
                PMIX_RELEASE(cd);
            }
            break;
        }
        if (0 < cd->ninfo) {
            PMIX_BFROPS_PACK(ret, peer, relay, cd->info, cd->ninfo, PMIX_INFO);
            if (PMIX_SUCCESS != ret) {
                PMIX_ERROR_LOG(ret);
                PMIX_RELEASE(relay);
                if (found) {
                    /* we already checked this event out of the cache, so
                     * nothing else will ever release it */
                    PMIX_RELEASE(cd);
                }
                break;
            }
        }
        /* the range travels last, as it does on every other path that
         * sends a PMIX_NOTIFY_CMD - a tool recipient reads it to decide
         * whether to carry the event onward, and defaults a missing one
         * to PMIX_RANGE_LOCAL */
        PMIX_BFROPS_PACK(ret, peer, relay, &cd->range, 1, PMIX_DATA_RANGE);
        if (PMIX_SUCCESS != ret) {
            PMIX_ERROR_LOG(ret);
            PMIX_RELEASE(relay);
            if (found) {
                PMIX_RELEASE(cd);
            }
            break;
        }
        PMIX_SERVER_QUEUE_REPLY(ret, peer, 0, relay);
        if (PMIX_SUCCESS != ret) {
            PMIX_RELEASE(relay);
        }
        if (found) {
            PMIX_RELEASE(cd);
        }
    }
}
