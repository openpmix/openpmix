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
#include "src/util/pmix_show_help.h"
#include "src/util/pmix_strnlen.h"

#include "src/mca/ptl/base/base.h"
#include "src/mca/ptl/base/ptl_base_handshake.h"

static void process_cbfunc(int sd, short args, void *cbdata);
static void cnct_cbfunc(pmix_status_t status, pmix_proc_t *proc, void *cbdata);
static void _check_cached_events(pmix_peer_t *peer);
static pmix_status_t process_tool_request(pmix_pending_connection_t *pnd, char *mg, size_t cnt);

// Local objects
typedef struct {
    pmix_object_t super;
    pmix_event_t ev;
    pmix_status_t status;
    pmix_status_t reply;
    pmix_pending_connection_t *pnd;
    pmix_peer_t *peer;
    pmix_info_t *info;
    size_t ninfo;
} cnct_hdlr_t;
static void chcon(cnct_hdlr_t *p)
{
    memset(&p->ev, 0, sizeof(pmix_event_t));
    p->status = PMIX_SUCCESS;
    p->reply = PMIX_SUCCESS;
    p->pnd = NULL;
    p->peer = NULL;
    p->info = NULL;
    p->ninfo = 0;
}
static void chdes(cnct_hdlr_t *p)
{
    if (NULL != p->pnd) {
        PMIX_RELEASE(p->pnd);
    }
    if (NULL != p->info) {
        PMIX_INFO_FREE(p->info, p->ninfo);
    }
}
static PMIX_CLASS_INSTANCE(cnct_hdlr_t,
                           pmix_object_t,
                           chcon, chdes);

static void _cnct_complete(int sd, short args, void *cbdata)
{
    cnct_hdlr_t *ch = (cnct_hdlr_t *) cbdata;
    uint32_t u32;
    pmix_status_t rc;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    if (PMIX_SUCCESS != ch->status) {
        goto error;
    }

    /* tell the client all is good */
    u32 = htonl(ch->reply);
    rc = pmix_ptl_base_send_blocking(ch->pnd->sd, (char *) &u32, sizeof(uint32_t));
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto error;
    }

    /* If needed, perform the handshake. The macro will update reply */
    PMIX_PSEC_SERVER_HANDSHAKE_IFNEED(ch->reply, ch->peer);

    /* It is possible that connection validation failed */
    if (PMIX_SUCCESS != ch->reply) {
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "validation of client connection failed");
        goto error;
    }

    /* send the client's array index */
    u32 = htonl(ch->peer->index);
    rc = pmix_ptl_base_send_blocking(ch->pnd->sd, (char *) &u32, sizeof(uint32_t));
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto error;
    }

    pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                        "connect-ack from client completed");

    pmix_ptl_base_set_nonblocking(ch->pnd->sd);

    /* start the events for this client */
    pmix_event_assign(&ch->peer->recv_event, pmix_globals.evbase, ch->pnd->sd, EV_READ | EV_PERSIST,
                      pmix_ptl_base_recv_handler, ch->peer);
    pmix_event_add(&ch->peer->recv_event, NULL);
    ch->peer->recv_ev_active = true;
    pmix_event_assign(&ch->peer->send_event, pmix_globals.evbase, ch->pnd->sd, EV_WRITE | EV_PERSIST,
                      pmix_ptl_base_send_handler, ch->peer);
    /* A send may have been queued while this event had no base - see
     * pmix_ptl_base_send(). It could not be activated then; activate it
     * now, or it waits for a send that may never come. */
    if (NULL != ch->peer->send_msg && !ch->peer->send_ev_active) {
        if (0 == pmix_event_add(&ch->peer->send_event, 0)) {
            ch->peer->send_ev_active = true;
        }
    }
    pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                        "pmix:server client %s:%u has connected on socket %d",
                        ch->peer->info->pname.nspace, ch->peer->info->pname.rank, ch->peer->sd);

    /* check the cached events and update the client */
    _check_cached_events(ch->peer);
    PMIX_RELEASE(ch);
    return;

error:
    if (NULL != ch->peer) {
        /* Undo the bookkeeping the connection handler did on this rank's
         * behalf before it handed us the peer: the live-process count it
         * incremented and the clients-array slot it pointed the rank at.
         * Leaving proc_cnt raised strands the rank at a count that never
         * drops again, which blocks the tombstone reclaim on a later
         * reconnect (that reclaim requires proc_cnt == 0), and leaving
         * peerid set points the rank at a slot we are about to empty. */
        if (NULL != ch->peer->info) {
            if (0 < ch->peer->info->proc_cnt) {
                --ch->peer->info->proc_cnt;
            }
            if (ch->peer->info->peerid == ch->peer->index) {
                ch->peer->info->peerid = -1;
            }
        }
        if (0 <= ch->peer->index) {
            pmix_pointer_array_set_item(&pmix_server_globals.clients, ch->peer->index, NULL);
        }
        PMIX_RELEASE(ch->peer);
    }
    CLOSE_THE_SOCKET(ch->pnd->sd);
    PMIX_RELEASE(ch);
}

static void _connect_complete(pmix_status_t status, void *cbdata)
{
    cnct_hdlr_t *ch = (cnct_hdlr_t *) cbdata;
    /* need to thread-shift this response */
    ch->status = status;
    PMIX_THREADSHIFT(ch, _cnct_complete);
}

void pmix_ptl_base_connection_handler(int sd, short args, void *cbdata)
{
    pmix_pending_connection_t *pnd = (pmix_pending_connection_t *) cbdata;
    pmix_ptl_hdr_t hdr;
    pmix_peer_t *peer = NULL;
    pmix_status_t rc, reply;
    char *msg = NULL, *mg, *blob = NULL;
    size_t cnt, n, nblob = 0;
    size_t len = 0;
    int32_t i32;
    pmix_namespace_t *nptr, *tmp;
    pmix_rank_info_t *info = NULL, *iptr;
    pmix_proc_t proc;
    pmix_info_t ginfo, *iblob = NULL;
    pmix_byte_object_t cred;
    pmix_buffer_t buf;
    uint8_t major, minor, release;
    cnct_hdlr_t *ch;
    void *ilist;
    pmix_data_array_t darray;
    pmix_peer_t *stale;
    bool counted = false;

    /* acquire the object */
    PMIX_ACQUIRE_OBJECT(pnd);

    // must use sd, args to avoid -Werror
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    pmix_output_verbose(8, pmix_ptl_base_framework.framework_output,
                        "ptl:base:connection_handler: new connection: %d", pnd->sd);

    /* ensure the socket is in blocking mode */
    pmix_ptl_base_set_blocking(pnd->sd);

    /* ensure all is zero'd */
    memset(&hdr, 0, sizeof(pmix_ptl_hdr_t));

    /* get the header */
    rc = pmix_ptl_base_recv_blocking(pnd->sd, (char *) &hdr, sizeof(pmix_ptl_hdr_t));
    if (PMIX_SUCCESS != rc) {
        goto error;
    }

    /* get the id, authentication and version payload (and possibly
     * security credential) - to guard against potential attacks,
     * we'll set an arbitrary limit per a define */
    if (PMIX_MAX_CRED_SIZE < hdr.nbytes) {
        goto error;
    }
    if (NULL == (msg = (char *) malloc(hdr.nbytes+1))) {
        goto error;
    }
    memset(msg, 0, hdr.nbytes + 1);  // ensure NULL termination of result
    if (PMIX_SUCCESS != pmix_ptl_base_recv_blocking(pnd->sd, msg, hdr.nbytes)) {
        /* unable to complete the recv */
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "ptl:tool:connection_handler unable to complete recv of connect-ack "
                            "with client ON SOCKET %d",
                            pnd->sd);
        goto error;
    }

    cnt = hdr.nbytes;
    mg = msg;
    /* extract the name of the sec module they used */
    PMIX_PTL_GET_STRING(pnd->psec);

    /* extract any credential so we can validate this connection
     * before doing anything else */
    PMIX_PTL_GET_U32(pnd->len);
    if (PMIX_TAINT_UINT_LIMIT < pnd->len) {    // arbitrary value to guard against tainted input
        PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
        goto error;
    }

    /* if a credential is present, then create space and
     * extract it for processing */
    PMIX_PTL_GET_BLOB(pnd->cred, pnd->len);

    /* get the process type of the connecting peer */
    PMIX_PTL_GET_U8(pnd->flag);

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
        PMIX_ERROR_LOG(PMIX_ERR_NOT_SUPPORTED);
        goto error;
    }

    /* extract their VERSION */
    PMIX_PTL_GET_STRING(pnd->version);
    pmix_ptl_base_parse_version(pnd->version, &major, &minor, &release);
    PMIX_SET_PROC_MAJOR(&pnd->proc_type, major);
    PMIX_SET_PROC_MINOR(&pnd->proc_type, minor);
    PMIX_SET_PROC_RELEASE(&pnd->proc_type, release);

    if (2 == major && 0 == minor) {
        /* the 2.0 release handshake ends with the version string */
        pnd->bfrops = strdup("v20");
        pnd->buffer_type = pmix_bfrops_globals.default_type; // we can't know any better
        pnd->gds = strdup("ds12,hash");
        cnt = 0;
    } else {
        /* extract the name of the bfrops module they used */
        PMIX_PTL_GET_STRING(pnd->bfrops);

        /* extract the type of buffer they used */
        PMIX_PTL_GET_U8(pnd->buffer_type);

        /* extract the name of the gds module they used */
        PMIX_PTL_GET_STRING(pnd->gds);

        /* extract the blob */
        if (0 < cnt) {
            len = cnt;
            PMIX_PTL_GET_BLOB(blob, len);
        }
    }

    /* see if this is a tool connection request */
    if (PMIX_SIMPLE_CLIENT != pnd->flag &&
        PMIX_SINGLETON_CLIENT != pnd->flag) {
        /* nope, it's for a tool, so process it
         * separately - it is a 2-step procedure */
        rc = process_tool_request(pnd, blob, len);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto error;
        }
        if (NULL != blob) {
            free(blob);
            blob = NULL;
        }
        free(msg);
        return;
    }

    /* it is a client that is connecting, so it should have
     * been registered with us prior to being started.
     * See if we know this nspace */
    nptr = NULL;
    PMIX_LIST_FOREACH (tmp, &pmix_globals.nspaces, pmix_namespace_t) {
        if (0 == strcmp(tmp->nspace, pnd->proc.nspace)) {
            nptr = tmp;
            break;
        }
    }
    if (NULL == nptr) {
        /* we don't know this namespace, reject it */
        rc = PMIX_ERR_NOT_FOUND;
        goto error;
    }

    /* likewise, we should have this peer in our list */
    info = NULL;
    PMIX_LIST_FOREACH (iptr, &nptr->ranks, pmix_rank_info_t) {
        if (iptr->pname.rank == pnd->proc.rank) {
            info = iptr;
            break;
        }
    }
    if (NULL == info) {
        /* rank unknown, reject it */
        rc = PMIX_ERR_NOT_FOUND;
        goto error;
    }

    /* save the version in the namespace object */
    if (0 == nptr->version.major) {
        nptr->version.major = pnd->proc_type.major;
        nptr->version.minor = pnd->proc_type.minor;
        nptr->version.release = pnd->proc_type.release;
    }

    /* If this rank previously finalized, a harmless finalized "tombstone"
     * peer from that prior cycle may still occupy its clients-array slot -
     * pmix_server_peer_finalized leaves it in place at socket-close rather
     * than mutating shared state amid concurrent collectives. Now that the
     * rank is reconnecting we are at a safe point (no collective from the
     * prior cycle can still be in flight), so reclaim that tombstone
     * before allocating a fresh peer for this connection: null its slot,
     * drop the finalized count it was still contributing, and release it.
     * This is what keeps the clients array and nptr->nfinalized from
     * drifting across repeated init/finalize cycles. We only reclaim when
     * no live process remains for the rank (proc_cnt == 0) so a surviving
     * fork/exec'd clone is never disturbed. See
     * docs/how-things-work/init-finalize.rst. */
    if (0 <= info->peerid && 0 == info->proc_cnt) {
        stale = (pmix_peer_t *) pmix_pointer_array_get_item(&pmix_server_globals.clients,
                                                            info->peerid);
        if (NULL != stale && stale->finalized) {
            pmix_pointer_array_set_item(&pmix_server_globals.clients, info->peerid, NULL);
            if (0 < nptr->nfinalized) {
                --nptr->nfinalized;
            }
            PMIX_RELEASE(stale);
        }
    }

    /* a peer can connect on multiple sockets since it can fork/exec
     * a child that also calls PMIX_Init, so add it here if necessary.
     * Create the tracker for this peer */
    peer = PMIX_NEW(pmix_peer_t);
    if (NULL == peer) {
        goto error;
    }

    /* Assign the upper half of the tag space for sendrecvs */
    peer->dyn_tags_start    = PMIX_PTL_TAG_DYNAMIC + (UINT32_MAX - PMIX_PTL_TAG_DYNAMIC)/2 + 1;
    peer->dyn_tags_end      = UINT32_MAX;
    peer->dyn_tags_current  = peer->dyn_tags_start;
    /* mark that this peer is a client of the given type */
    memcpy(&peer->proc_type, &pnd->proc_type, sizeof(pmix_proc_type_t));
    /* save the protocol */
    peer->protocol = pnd->protocol;
    /* add in the nspace pointer */
    PMIX_RETAIN(nptr);
    peer->nptr = nptr;
    PMIX_RETAIN(info);
    peer->info = info;
    /* update the epilog fields */
    peer->epilog.uid = info->uid;
    peer->epilog.gid = info->gid;
    /* ensure the nspace epilog is updated too */
    nptr->epilog.uid = info->uid;
    nptr->epilog.gid = info->gid;
    info->proc_cnt++; /* increase number of processes on this rank */
    counted = true;
    peer->sd = pnd->sd;
    if (0 > (peer->index = pmix_pointer_array_add(&pmix_server_globals.clients, peer))) {
        goto error;
    }
    info->peerid = peer->index;

    /* set the sec module to match this peer */
    peer->nptr->compat.psec = pmix_psec_base_assign_module(pnd->psec);
    if (NULL == peer->nptr->compat.psec) {
        goto error;
    }

    /* set the bfrops module to match this peer */
    peer->nptr->compat.bfrops = pmix_bfrops_base_assign_module(pnd->bfrops);
    if (NULL == peer->nptr->compat.bfrops) {
        goto error;
    }
    /* and the buffer type to match */
    peer->nptr->compat.type = pnd->buffer_type;

    /* set the gds module to match this peer */
    if (NULL != pnd->gds) {
        PMIX_INFO_LOAD(&ginfo, PMIX_GDS_MODULE, pnd->gds, PMIX_STRING);
        peer->nptr->compat.gds = pmix_gds_base_assign_module(&ginfo, 1);
        PMIX_INFO_DESTRUCT(&ginfo);
    } else {
        peer->nptr->compat.gds = pmix_gds_base_assign_module(NULL, 0);
    }
    if (NULL == peer->nptr->compat.gds) {
        goto error;
    }
    /* track this client's GDS module on the peer itself, so a later
     * per-client fallback (PMIX_GDS_FALLBACK_CMD) can change it without
     * affecting the other peers sharing this nspace */
    peer->gds = peer->nptr->compat.gds;

    /* if we haven't previously stored the version for this
     * nspace, do so now */
    if (!nptr->version_stored) {
        PMIX_INFO_LOAD(&ginfo, PMIX_BFROPS_MODULE, pnd->version, PMIX_STRING);
        PMIX_GDS_CACHE_JOB_INFO(rc, pmix_globals.mypeer, peer->nptr, &ginfo, 1);
        PMIX_INFO_DESTRUCT(&ginfo);
        nptr->version_stored = true;
    }

    ilist = PMIx_Info_list_start();
    // if a blob was provided, then unpack it
    if (NULL != blob) {
        PMIX_CONSTRUCT(&buf, pmix_buffer_t);
        PMIX_LOAD_BUFFER_NON_DESTRUCT(peer, &buf, blob, len); // allocates no memory
        i32 = 1;
        PMIX_BFROPS_UNPACK(rc, peer, &buf, &nblob, &i32, PMIX_SIZE);
        if (0 < nblob) {
            PMIX_INFO_CREATE(iblob, nblob);
            i32 = nblob;
            PMIX_BFROPS_UNPACK(rc, peer, &buf, iblob, &i32, PMIX_INFO);
            // process the data
            for (n=0; n < nblob; n++) {
                if (PMIx_Check_key(iblob[n].key, PMIX_PROC_PID)) {
                    info->pid = iblob[n].value.data.pid;
                    PMIx_Info_list_add(ilist, PMIX_PROC_PID, &info->pid, PMIX_PID);

                } else if (PMIx_Check_key(iblob[n].key, PMIX_REALUID)) {
                    info->realuid = iblob[n].value.data.uint32;
                    PMIx_Info_list_add(ilist, PMIX_REALUID, &info->realuid, PMIX_UINT32);

                } else if (PMIx_Check_key(iblob[n].key, PMIX_USERID)) {
                    // check if the client is claiming to be someone other
                    // than what they were registered as
                    if (info->uid != iblob[n].value.data.uint32) {
                        // mismatch
                        PMIx_Info_list_release(ilist);
                        pmix_show_help("help-ptl-base.txt", "mismatch-id", true,
                                       "user", iblob[n].value.data.uint32, info->uid);
                        goto error;
                    }

                } else if (PMIx_Check_key(iblob[n].key, PMIX_REALGID)) {
                    info->realgid = iblob[n].value.data.uint32;
                    PMIx_Info_list_add(ilist, PMIX_REALGID, &info->realgid, PMIX_UINT32);

                } else if (PMIx_Check_key(iblob[n].key, PMIX_GRPID)) {
                    // check if the client is claiming to be someone other
                    // than what they were registered as
                    if (info->gid != iblob[n].value.data.uint32) {
                        // mismatch
                        PMIx_Info_list_release(ilist);
                        pmix_show_help("help-ptl-base.txt", "mismatch-id", true,
                                       "group", iblob[n].value.data.uint32, info->uid);
                        goto error;
                    }
                }
            }
            PMIX_INFO_FREE(iblob, nblob);
            iblob = NULL;
            nblob = 0;
        }
        free(blob);
        blob = NULL;
    }

    free(msg); // can now release the data buffer
    msg = NULL;

    /* validate the connection */
    cred.bytes = pnd->cred;
    cred.size = pnd->len;
    PMIX_PSEC_VALIDATE_CONNECTION(reply, peer, NULL, 0, NULL, NULL, &cred);
    /* PMIX_ERR_READY_FOR_HANDSHAKE is not a failure - it is how a psec
     * module that authenticates with a live exchange rather than with a
     * credential asks us to run that exchange. We carry it in ch->reply
     * so that _cnct_complete can report it to the client and then drive
     * the handshake; bailing out here would leave the handshake half of
     * psec permanently unreachable */
    if (PMIX_SUCCESS != reply && PMIX_ERR_READY_FOR_HANDSHAKE != reply) {
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "validation of client connection failed");
        PMIx_Info_list_release(ilist);
        goto error;
    }

    pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                        "client connection validated");


    // prep for processing
    ch = PMIX_NEW(cnct_hdlr_t);
    ch->peer = peer;
    ch->pnd = pnd;
    ch->reply = reply;

    PMIx_Info_list_add(ilist, PMIX_USERID, &info->uid, PMIX_UINT32);
    PMIx_Info_list_add(ilist, PMIX_GRPID, &info->gid, PMIX_UINT32);
    PMIx_Info_list_convert(ilist, &darray);
    ch->info = (pmix_info_t*)darray.array;
    ch->ninfo = darray.size;
    PMIx_Info_list_release(ilist);

    /* let the host server know that this client has connected */
    if (NULL != pmix_host_server.client_connected2) {
        PMIX_LOAD_PROCID(&proc, peer->info->pname.nspace, peer->info->pname.rank);
        rc = pmix_host_server.client_connected2(&proc, peer->info->server_object,
                                                ch->info, ch->ninfo,
                                                _connect_complete, ch);
        if (PMIX_OPERATION_SUCCEEDED == rc) {
            ch->status = PMIX_SUCCESS;
            _cnct_complete(0, 0, ch);
            return;
        }
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            /* the error path below closes the socket and releases pnd, so
             * detach it from the caddy first - the caddy's destructor
             * would otherwise release it out from under us */
            ch->pnd = NULL;
            PMIX_RELEASE(ch);
            goto error;
        }

    } else if (NULL != pmix_host_server.client_connected) {
        PMIX_LOAD_PROCID(&proc, peer->info->pname.nspace, peer->info->pname.rank);
        rc = pmix_host_server.client_connected(&proc, peer->info->server_object, _connect_complete, ch);
        if (PMIX_OPERATION_SUCCEEDED == rc) {
            ch->status = PMIX_SUCCESS;
            _cnct_complete(0, 0, ch);
            return;
        }
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            /* as above - the error path owns pnd from here on */
            ch->pnd = NULL;
            PMIX_RELEASE(ch);
            goto error;
        }
    } else {
        // if neither of those conditions are met, then we simply assume the host is ready
        ch->status = PMIX_SUCCESS;
        _cnct_complete(0, 0, ch);
    }

    return;

error:
    /* 'info' is a member of the namespace's rank list, not something we
     * own - the only reference this function took on it is the one it
     * gave the peer, which PMIX_RELEASE(peer) below gives back. Releasing
     * it here as well drove its refcount to zero while it was still on
     * that list, which aborts the server in a debug build and leaves the
     * list holding freed memory in an optimized one. Undo the count we
     * added and leave the object alone. */
    if (NULL != info && counted) {
        info->proc_cnt--;
    }
    if (NULL != msg) {
        free(msg);
    }
    if (NULL != blob) {
        free(blob);
    }
    if (NULL != iblob) {
        PMIX_INFO_FREE(iblob, nblob);
    }
    if (NULL != peer) {
        if (0 <= peer->index) {
            pmix_pointer_array_set_item(&pmix_server_globals.clients, peer->index, NULL);
            /* the rank was pointed at the slot we just emptied */
            if (NULL != info && info->peerid == peer->index) {
                info->peerid = -1;
            }
        }
        PMIX_RELEASE(peer);
    }
    CLOSE_THE_SOCKET(pnd->sd);
    PMIX_RELEASE(pnd);
    return;
}

/* Put the namespace we built for a connecting tool onto the server's
 * global namespace list, and make sure that list holds exactly one
 * object per namespace name.
 *
 * The append cannot happen any earlier: for a tool that asked us for an
 * identity, the name is not known until the host's tool_connected
 * upcall returns. That upcall is also the window in which the host may
 * register a namespace of its own under the very name it just handed
 * us - so by the time we get here, ours may be a duplicate. It must not
 * be allowed to stand: the whole server resolves a namespace by
 * scanning this list, so a second object for the same name means half
 * the library reaches a tool through one of them and half through the
 * other, and whichever one is not found has an invisible rank list.
 *
 * Returns the namespace the peer should use. On return, pnd->nspace_created
 * is true only if the object we created is the one now on the list -
 * i.e. it is exactly "the caller must withdraw this on failure".
 */
static pmix_namespace_t *consolidate_nspace(pmix_pending_connection_t *pnd,
                                            pmix_peer_t *peer,
                                            pmix_namespace_t *nptr)
{
    pmix_namespace_t *ns, *existing = NULL;
    pmix_rank_info_t *rinfo, *match = NULL;

    /* An object with no name cannot be looked up by one, so there is
     * nothing to reconcile against and nothing for the list to do with
     * it. Leave it to the peer. */
    if (NULL == nptr->nspace || '\0' == nptr->nspace[0]) {
        return nptr;
    }

    /* Compare names EXACTLY. Do not reach for PMIx_Check_nspace() here:
     * it reports a match whenever *either* name is absent, which is the
     * right answer for the wildcard matching its callers do and a
     * catastrophic one for us. The server's list legitimately carries
     * namespaces whose name is not set yet, and treating the first of
     * those as "the same namespace" hands this tool an object belonging
     * to something else - after which the job data the server packs for
     * it is somebody else's, and the tool fails to unpack its own
     * identity out of the reply. */
    PMIX_LIST_FOREACH (ns, &pmix_globals.nspaces, pmix_namespace_t) {
        if (ns != nptr && NULL != ns->nspace &&
            0 == strncmp(nptr->nspace, ns->nspace, PMIX_MAX_NSLEN)) {
            existing = ns;
            break;
        }
    }
    if (NULL == existing) {
        /* ours is the only one - the reference PMIX_NEW gave us becomes
         * the one the list holds */
        pmix_list_append(&pmix_globals.nspaces, &nptr->super);
        return nptr;
    }

    /* Someone got there first. Move what we built onto their object and
     * dispose of ours. */
    if (pnd->rinfo_created && NULL != peer->info) {
        PMIX_LIST_FOREACH (rinfo, &existing->ranks, pmix_rank_info_t) {
            if (rinfo->pname.rank == peer->info->pname.rank) {
                match = rinfo;
                break;
            }
        }
        pmix_list_remove_item(&nptr->ranks, &peer->info->super);
        if (NULL == match) {
            /* they have no entry for this rank - carry ours across. The
             * reference the rank list holds moves with it, so nothing is
             * released here, and rinfo_created stays true because the
             * entry is still ours to withdraw on failure */
            pmix_list_append(&existing->ranks, &peer->info->super);
        } else {
            /* they already describe this rank - use their entry and drop
             * ours entirely: the reference its rank list was holding and
             * the peer's both belong to an object nobody will reach */
            PMIX_RELEASE(peer->info);
            PMIX_RELEASE(peer->info);
            PMIX_RETAIN(match);
            peer->info = match;
            pnd->rinfo_created = false;
        }
    }

    /* the peer moves to their object, so it needs a reference on it;
     * ours gives back both the peer's and the one we were holding for
     * the list we are not going to join, which disposes of it */
    PMIX_RETAIN(existing);
    PMIX_RELEASE(nptr);
    PMIX_RELEASE(nptr);
    peer->nptr = existing;
    /* we no longer own a created namespace - theirs is on the list and
     * is not ours to withdraw */
    pnd->nspace_created = false;
    return existing;
}

/* process the host's callback with tool connection info */
static void process_cbfunc(int sd, short args, void *cbdata)
{
    pmix_setup_caddy_t *cd = (pmix_setup_caddy_t *) cbdata;
    pmix_pending_connection_t *pnd = (pmix_pending_connection_t *) cd->cbdata;
    pmix_namespace_t *nptr = NULL;
    pmix_rank_info_t *info;
    pmix_peer_t *peer = NULL, *pr2;
    pmix_status_t rc, reply;
    uint32_t u32;
    int n;
    pmix_info_t ginfo;
    pmix_byte_object_t cred;
    pmix_iof_req_t *req = NULL;
    bool nspace_listed = false;

    /* acquire the object */
    PMIX_ACQUIRE_OBJECT(cd);
    // must use sd, args to avoid -Werror
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    /* send this status so they don't hang */
    u32 = ntohl(cd->status);
    rc = pmix_ptl_base_send_blocking(pnd->sd, (char *) &u32, sizeof(uint32_t));
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto error;
    }

    /* if the request failed, then we are done */
    if (PMIX_SUCCESS != cd->status) {
        goto error;
    }

    /* if we got an identifier, send it back to the tool */
    if (pnd->need_id) {
        /* start with the nspace */
        rc = pmix_ptl_base_send_blocking(pnd->sd, cd->proc.nspace, PMIX_MAX_NSLEN + 1);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto error;
        }

        /* now the rank, suitably converted */
        u32 = ntohl(cd->proc.rank);
        rc = pmix_ptl_base_send_blocking(pnd->sd, (char *) &u32, sizeof(uint32_t));
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto error;
        }
    }

    /* send my nspace back to the tool */
    rc = pmix_ptl_base_send_blocking(pnd->sd, pmix_globals.myid.nspace,
                                             PMIX_MAX_NSLEN + 1);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto error;
    }

    /* send my rank back to the tool */
    u32 = ntohl(pmix_globals.myid.rank);
    rc = pmix_ptl_base_send_blocking(pnd->sd, (char *) &u32, sizeof(uint32_t));
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto error;
    }

    /* shortcuts */
    peer = (pmix_peer_t *) pnd->peer;
    nptr = peer->nptr;

    /* if this tool is a client, then check against our list of
     * local clients to verify they are the same */
    if (PMIX_TOOL_CLIENT == pnd->flag || PMIX_LAUNCHER_CLIENT == pnd->flag) {
        for (n=0; n < pmix_server_globals.clients.size; n++) {
            pr2 = (pmix_peer_t*)pmix_pointer_array_get_item(&pmix_server_globals.clients, n);
            if (NULL == pr2) {
                continue;
            }
            if (PMIx_Check_nspace(pr2->info->pname.nspace, cd->proc.nspace) &&
                pr2->info->pname.rank == cd->proc.rank) {
                // this matches the existing client record - check uid/gid
                if (pr2->info->uid != pnd->uid) {
                    reply = PMIX_ERR_INVALID_CRED;
                    u32 = htonl(reply);
                    rc = pmix_ptl_base_send_blocking(pnd->sd, (char *) &u32, sizeof(uint32_t));
                    if (PMIX_SUCCESS != rc) {
                        PMIX_ERROR_LOG(rc);
                    }
                    goto error;
                }
            }
        }
    }
    /* if this tool wasn't initially registered as a client,
     * then add some required structures */
    if (PMIX_TOOL_CLIENT != pnd->flag && PMIX_LAUNCHER_CLIENT != pnd->flag) {
        if (NULL != nptr->nspace) {
            free(nptr->nspace);
        }
        nptr->nspace = strdup(cd->proc.nspace);
        info = PMIX_NEW(pmix_rank_info_t);
        info->pname.nspace = strdup(nptr->nspace);
        info->pname.rank = cd->proc.rank;
        info->uid = pnd->uid;
        info->gid = pnd->gid;
        pmix_list_append(&nptr->ranks, &info->super);
        PMIX_RETAIN(info);
        peer->info = info;
        pnd->rinfo_created = true;
    }

    /* The namespace has its final name now, so this is the point at
     * which it can join the server's list - and the point at which a
     * duplicate registered by the host during the upcall has to be
     * reconciled against. This applies to every tool we built a
     * namespace for, not only one the server had already registered as
     * a client: a self-started tool's namespace used to be left off the
     * list entirely, which stranded the reference held for it and left
     * the server able to hold two objects for one name. */
    if (pnd->nspace_created) {
        nptr = consolidate_nspace(pnd, peer, nptr);
        nspace_listed = pnd->nspace_created;
    }

    /* mark the peer proc type */
    memcpy(&peer->proc_type, &pnd->proc_type, sizeof(pmix_proc_type_t));
    /* save the protocol */
    peer->protocol = pnd->protocol;
    /* save the uid/gid */
    peer->epilog.uid = peer->info->uid;
    peer->epilog.gid = peer->info->gid;
    nptr->epilog.uid = peer->info->uid;
    nptr->epilog.gid = peer->info->gid;
    peer->proc_cnt = 1;
    peer->sd = pnd->sd;

    /* Get the appropriate compatibility modules based on the info
     * provided by the tool during the initial connection request.
     *
     * These have to be (re)assigned here rather than only in
     * process_tool_request, because the namespace this peer ends up on
     * need not be the object that ran: if the host registered this
     * namespace during its tool_connected upcall, we adopted its object
     * instead, and a host-registered namespace carries no compat
     * modules - nothing has connected through it yet. Leaving bfrops
     * unset that way is not a degraded mode, it is a NULL dereference on
     * the first message the tool sends. */
    peer->nptr->compat.bfrops = pmix_bfrops_base_assign_module(pnd->bfrops);
    if (NULL == peer->nptr->compat.bfrops) {
        goto error;
    }
    peer->nptr->compat.type = pnd->buffer_type;
    peer->nptr->compat.psec = pmix_psec_base_assign_module(pnd->psec);
    if (NULL == peer->nptr->compat.psec) {
        goto error;
    }
    /* set the gds */
    PMIX_INFO_LOAD(&ginfo, PMIX_GDS_MODULE, pnd->gds, PMIX_STRING);
    peer->nptr->compat.gds = pmix_gds_base_assign_module(&ginfo, 1);
    PMIX_INFO_DESTRUCT(&ginfo);
    if (NULL == peer->nptr->compat.gds) {
        goto error;
    }
    /* track this peer's GDS module on the peer itself (see the client
     * connection path for rationale) */
    peer->gds = peer->nptr->compat.gds;

    /* if we haven't previously stored the version for this
     * nspace, do so now */
    if (!peer->nptr->version_stored) {
        PMIX_INFO_LOAD(&ginfo, PMIX_BFROPS_MODULE, pnd->version, PMIX_STRING);
        PMIX_GDS_CACHE_JOB_INFO(rc, pmix_globals.mypeer, peer->nptr, &ginfo, 1);
        PMIX_INFO_DESTRUCT(&ginfo);
        nptr->version_stored = true;
    }

    /* automatically setup to forward output to the tool */
    req = PMIX_NEW(pmix_iof_req_t);
    if (NULL == req) {
        goto error;
    }
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

    /* validate the connection */
    cred.bytes = pnd->cred;
    cred.size = pnd->len;
    PMIX_PSEC_VALIDATE_CONNECTION(reply, peer, NULL, 0, NULL, NULL, &cred);
    /* as on the client path above, PMIX_ERR_READY_FOR_HANDSHAKE is a
     * request to run a live exchange, not a rejection */
    if (PMIX_SUCCESS != reply && PMIX_ERR_READY_FOR_HANDSHAKE != reply) {
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "validation of tool credentials failed: %s",
                            PMIx_Error_string(reply));
    }

    /* communicate the result to the other side */
    u32 = htonl(reply);
    rc = pmix_ptl_base_send_blocking(pnd->sd, (char *) &u32, sizeof(uint32_t));
    if (PMIX_SUCCESS != rc
        || (PMIX_SUCCESS != reply && PMIX_ERR_READY_FOR_HANDSHAKE != reply)) {
        goto error;
    }

    /* If needed perform the handshake. The macro will update reply */
    PMIX_PSEC_SERVER_HANDSHAKE_IFNEED(reply, peer);

    /* If verification wasn't successful - stop here */
    if (PMIX_SUCCESS != reply) {
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "security handshake for tool failed: %s",
                            PMIx_Error_string(reply));
        goto error;
    }

    /* set the socket non-blocking for all further operations */
    pmix_ptl_base_set_nonblocking(pnd->sd);

    /* If this rank previously finalized and left a tombstone peer behind
     * (a tool registered as a client keeps the client tombstone semantics,
     * which pmix_server_peer_finalized does not free at socket-close), the
     * stale finalized peer may still occupy the rank's clients slot. We are
     * now at a safe reconnect point, so reclaim it before allocating this
     * connection's fresh peer - mirroring the client path in the main
     * connection handler. Without this a tool that reuses its identity
     * across init/finalize cycles would leak one peer (and one nfinalized
     * count) per cycle. See docs/how-things-work/init-finalize.rst. */
    if (NULL != peer->info && 0 <= peer->info->peerid) {
        pr2 = (pmix_peer_t *) pmix_pointer_array_get_item(&pmix_server_globals.clients,
                                                          peer->info->peerid);
        if (NULL != pr2 && pr2 != peer && pr2->finalized) {
            pmix_pointer_array_set_item(&pmix_server_globals.clients, peer->info->peerid, NULL);
            if (NULL != peer->nptr && 0 < peer->nptr->nfinalized) {
                --peer->nptr->nfinalized;
            }
            PMIX_RELEASE(pr2);
        }
    }

    if (0 > (peer->index = pmix_pointer_array_add(&pmix_server_globals.clients, peer))) {
        goto error;
    }
    peer->info->peerid = peer->index;

    /* start the events for this tool */
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
                        "pmix:server tool %s:%d has connected on socket %d",
                        peer->info->pname.nspace, peer->info->pname.rank, peer->sd);

    /* check the cached events and update the tool */
    _check_cached_events(peer);
    PMIX_RELEASE(pnd);
    PMIX_RELEASE(cd);
    return;

error:
    CLOSE_THE_SOCKET(pnd->sd);
    if (NULL != peer) {
        if (NULL != peer->info && pnd->rinfo_created) {
            /* withdraw it from the namespace's rank list and give back
             * the reference that list was holding - removing an item
             * does not release it. The peer's own reference is given
             * back when the peer is released below. */
            pmix_list_remove_item(&peer->nptr->ranks, &peer->info->super);
            PMIX_RELEASE(peer->info);
        }
        if (NULL != peer->nptr && pnd->nspace_created) {
            /* This namespace is one we built. Whether it reached the
             * global list depends on how far we got - several of the
             * gotos above fire before the block that appends it - so
             * withdraw it only if it is actually there. Either way we are
             * still holding the reference PMIX_NEW gave us on behalf of
             * that list, and removing an item does not release it, so
             * give it back here. The peer's own reference comes back when
             * the peer is released below. */
            if (nspace_listed) {
                pmix_list_remove_item(&pmix_globals.nspaces, &peer->nptr->super);
            }
            PMIX_RELEASE(peer->nptr);
        }
        PMIX_RELEASE(peer);
    }
    PMIX_RELEASE(cd);
    if (NULL != req) {
        pmix_pointer_array_set_item(&pmix_globals.iof_requests, req->local_id, NULL);
        PMIX_RELEASE(req);
    }
    PMIX_RELEASE(pnd);
}

/* receive a callback from the host RM with an nspace
 * for a connecting tool */
static void cnct_cbfunc(pmix_status_t status, pmix_proc_t *proc, void *cbdata)
{
    pmix_setup_caddy_t *cd;

    pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                        "pmix:tool:cnct_cbfunc returning %s:%d %s", proc->nspace, proc->rank,
                        PMIx_Error_string(status));

    /* need to thread-shift this into our context */
    cd = PMIX_NEW(pmix_setup_caddy_t);
    if (NULL == cd) {
        PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
        return;
    }
    cd->status = status;
    if (NULL != proc) {
        PMIX_LOAD_PROCID(&cd->proc, proc->nspace, proc->rank);
    }
    cd->cbdata = cbdata;
    PMIX_THREADSHIFT(cd, process_cbfunc);
}

static pmix_status_t process_tool_request(pmix_pending_connection_t *pnd,
                                          char *mg, size_t cnt)
{
    pmix_peer_t *peer, *p2;
    pmix_namespace_t *nptr, *tmp;
    pmix_rank_info_t *info;
    bool found;
    size_t n, sz;
    pmix_buffer_t buf;
    pmix_status_t rc;
    void *ilist;
    pmix_info_t *iptr;
    pmix_data_array_t darray;
    pmix_pfexec_child_t *child;

    if (!pmix_ptl_base.allow_foreign_tools) {
        if (pnd->uid != pmix_globals.uid) {
            // reject this connection
            return PMIX_ERR_NOT_SUPPORTED;
        }
    }

    peer = PMIX_NEW(pmix_peer_t);
    if (NULL == peer) {
        PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
        return PMIX_ERR_NOMEM;
    }
    /* Assign the upper half of the tag space for sendrecvs */
    peer->dyn_tags_start    = PMIX_PTL_TAG_DYNAMIC + (UINT32_MAX - PMIX_PTL_TAG_DYNAMIC)/2 + 1;
    peer->dyn_tags_end      = UINT32_MAX;
    peer->dyn_tags_current  = peer->dyn_tags_start;
    pnd->peer = peer;

    /* see if we know this nspace - i.e., was it registered or did
     * another tool within it already connect */
    nptr = NULL;
    PMIX_LIST_FOREACH (tmp, &pmix_globals.nspaces, pmix_namespace_t) {
        if (0 == strcmp(tmp->nspace, pnd->proc.nspace)) {
            nptr = tmp;
            break;
        }
    }

    /* if this is a tool we launched, then the host may
     * have already registered it as a client - so check
     * to see if we already have a peer for it */
    if (PMIX_TOOL_CLIENT == pnd->flag || PMIX_LAUNCHER_CLIENT == pnd->flag) {
       if (NULL == nptr) {
           /* it is possible that this is a tool inside of
             * a job-script as part of a multi-spawn operation.
             * Since each tool invocation may have finalized and
             * terminated, the tool will appear to "terminate", thus
             * causing us to cleanup all references to it, and then
             * reappear. So we don't reject this connection request.
             * Instead, we create the nspace and rank objects for
             * it and let the RM/host decide if this behavior
             * is allowed */
            nptr = PMIX_NEW(pmix_namespace_t);
            if (NULL == nptr) {
                PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
                PMIX_RELEASE(peer);
                return PMIX_ERR_NOMEM;
            }
            nptr->nspace = strdup(pnd->proc.nspace);
            /* save the version */
            nptr->version.major = pnd->proc_type.major;
            nptr->version.minor = pnd->proc_type.minor;
            nptr->version.release = pnd->proc_type.release;
            pnd->nspace_created = true;
        }
        /* now look for the rank */
        info = NULL;
        found = false;
        PMIX_LIST_FOREACH (info, &nptr->ranks, pmix_rank_info_t) {
            if (info->pname.rank == pnd->proc.rank) {
                found = true;
                break;
            }
        }
        if (found) {
            /* check that the uid/gid of the connecting tool
             * matches the expected values */
            if (info->uid != pnd->uid ||
                info->gid != pnd->gid) {
                PMIX_RELEASE(peer);
                PMIX_ERROR_LOG(PMIX_ERR_INVALID_CRED);
                return PMIX_ERR_INVALID_CRED;
            }

        } else {
           /* see above note about not finding nspace */
            info = PMIX_NEW(pmix_rank_info_t);
            info->pname.nspace = strdup(pnd->proc.nspace);
            info->pname.rank = pnd->proc.rank;
            info->uid = pnd->uid;
            info->gid = pnd->gid;
            pmix_list_append(&nptr->ranks, &info->super);
            pnd->rinfo_created = true;
        }
        PMIX_RETAIN(info);
        peer->info = info;

    } else if (NULL != nptr) {
        /* this is an non-client tool/launcher that already
         * has a known nspace that wasn't defined by register_client.
         * We must therefore already have a rank for it that connected
         * (so we would know the nspace), and the uid/gid's of this proc
         * must match those already registered */
        info = (pmix_rank_info_t*)pmix_list_get_first(&nptr->ranks);
        if (NULL == info) {
            /* this cannot happen - the nspace can only exist because
             * a prior instance of the tool already connected */
            PMIX_RELEASE(peer);
            PMIX_ERROR_LOG(PMIX_ERR_NOT_SUPPORTED);
            return PMIX_ERR_NOT_SUPPORTED;
        }
        if (-1 == info->peerid) {
            /* the tool may be one we fork/exec'd ourselves - check
             * the local children to see */
            found = false;
            PMIX_LIST_FOREACH(child, &pmix_pfexec_globals.children, pmix_pfexec_child_t) {
                if (PMIx_Check_nspace(child->proc.nspace, nptr->nspace)) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                /* this proc is not a child of ours and has not connected - this
                 * cannot happen for a tool as we otherwise would not know of it */
                PMIX_RELEASE(peer);
                PMIX_ERROR_LOG(PMIX_ERR_NOT_SUPPORTED);
                return PMIX_ERR_NOT_SUPPORTED;
            }
        } else {
            p2 = (pmix_peer_t*)pmix_pointer_array_get_item(&pmix_server_globals.clients, info->peerid);
            if (NULL == p2) {
                // that's an error
                PMIX_RELEASE(peer);
                PMIX_ERROR_LOG(PMIX_ERR_NOT_SUPPORTED);
                return PMIX_ERR_NOT_FOUND;
            }
            if (!PMIX_PEER_IS_TOOL(p2)) {
                /* cannot happen - the entire nspace must be a tool if this proc claims
                 * to be a member of that nspace and is a tool */
                PMIX_RELEASE(peer);
                PMIX_ERROR_LOG(PMIX_ERR_NOT_SUPPORTED);
                return PMIX_ERR_NOT_SUPPORTED;
            }
        }
        /* all members of an nspace must be from the same uid and gid */
        if (info->uid != pnd->uid ||
            info->gid != pnd->gid) {
            PMIX_RELEASE(peer);
            PMIX_ERROR_LOG(PMIX_ERR_NOT_SUPPORTED);
            return PMIX_ERR_NOT_SUPPORTED;
        }

    } else {
        nptr = PMIX_NEW(pmix_namespace_t);
        if (NULL == nptr) {
            PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
            PMIX_RELEASE(peer);
            return PMIX_ERR_NOMEM;
        }
        if (!pnd->need_id) {
            // must have been given one
            nptr->nspace = strdup(pnd->proc.nspace);
        }
        /* save the version */
        nptr->version.major = pnd->proc_type.major;
        nptr->version.minor = pnd->proc_type.minor;
        nptr->version.release = pnd->proc_type.release;
        pnd->nspace_created = true;
        /* must add the nspace to the global list after we return
         * from the host's upcall since they can/will assign the
         * tool with a namespace */
    }

    /* the peer holds one reference on the namespace object. If we created
     * that object here, then the reference PMIX_NEW gave us is the one
     * held on behalf of the global namespace list - process_cbfunc either
     * appends it to that list or releases it once the host has told us
     * the tool's identity. If the namespace already existed, the list is
     * already holding its own reference, so all we add is the peer's. */
    PMIX_RETAIN(nptr);
    peer->nptr = nptr;
    /* select their bfrops compat module so we can unpack
     * any provided pmix_info_t structs */
    peer->nptr->compat.bfrops = pmix_bfrops_base_assign_module(pnd->bfrops);
    if (NULL == peer->nptr->compat.bfrops) {
        rc = PMIX_ERR_NOT_AVAILABLE;
        PMIX_ERROR_LOG(rc);
        goto cleanup;
    }
    /* set the buffer type */
    peer->nptr->compat.type = pnd->buffer_type;

    n = 0;
    /* if info structs need to be passed along, then unpack them */
    ilist = PMIx_Info_list_start();
    if (0 < cnt) {
        int32_t foo;
        PMIX_CONSTRUCT(&buf, pmix_buffer_t);
        PMIX_LOAD_BUFFER_NON_DESTRUCT(peer, &buf, mg, cnt); // allocates no memory
        foo = 1;
        PMIX_BFROPS_UNPACK(rc, peer, &buf, &sz, &foo, PMIX_SIZE);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIx_Info_list_release(ilist);
            goto cleanup;
        }
        /* the count came off the wire - a packed pmix_info_t is never
         * smaller than a byte, so anything larger than the bytes we
         * actually received is malformed and must not be allocated */
        if (sz > cnt) {
            rc = PMIX_ERR_BAD_PARAM;
            PMIX_ERROR_LOG(rc);
            PMIx_Info_list_release(ilist);
            goto cleanup;
        }
        foo = (int32_t) sz;
        PMIX_INFO_CREATE(iptr, sz);
        PMIX_BFROPS_UNPACK(rc, peer, &buf, iptr, &foo, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_INFO_FREE(iptr, sz);
            PMIx_Info_list_release(ilist);
            goto cleanup;
        }
        for (n=0; n < sz; n++) {
            PMIx_Info_list_xfer(ilist, &iptr[n]);
        }
        PMIX_INFO_FREE(iptr, sz);
    }

    /* does the server support tool connections? */
    if (NULL == pmix_host_server.tool_connected &&
        NULL == pmix_host_server.tool_connected2) {
        PMIx_Info_list_release(ilist);
        if (pnd->need_id) {
            /* we need someone to provide the tool with an
             * identifier and they aren't available */
            /* send an error reply to the client */
            rc = PMIX_ERR_NOT_SUPPORTED;
            goto cleanup;
        } else {
            /* just process it locally */
            cnct_cbfunc(PMIX_SUCCESS, &pnd->proc, (void *) pnd);
            /* release the msg */
            return PMIX_SUCCESS;
        }
    }

    /* setup the info array to pass the relevant info
     * to the server */
    /* provide the version */
    PMIx_Info_list_add_unique(ilist, PMIX_VERSION_INFO,
                              pnd->version, PMIX_STRING, true);

    /* provide the user id */
    PMIx_Info_list_add_unique(ilist, PMIX_USERID,
                              &pnd->uid, PMIX_UINT32, false);

    /* and the group id */
    PMIx_Info_list_add_unique(ilist, PMIX_GRPID,
                              &pnd->gid, PMIX_UINT32, false);

    /* if we have it, pass along their ID */
    if (!pnd->need_id) {
        PMIx_Info_list_add_unique(ilist, PMIX_NSPACE,
                                  pnd->proc.nspace, PMIX_STRING, true);
        PMIx_Info_list_add_unique(ilist, PMIX_RANK,
                                  &pnd->proc.rank, PMIX_PROC_RANK, true);
    }
    rc = PMIx_Info_list_convert(ilist, &darray);
    PMIx_Info_list_release(ilist);
    if (PMIX_SUCCESS  != rc) {
        PMIX_ERROR_LOG(rc);
        goto cleanup;
    }

    pnd->info = (pmix_info_t*)darray.array;
    pnd->ninfo = darray.size;

    /* pass it up for processing */
    if (NULL != pmix_host_server.tool_connected2) {
        rc = pmix_host_server.tool_connected2(pnd->info, pnd->ninfo, cnct_cbfunc, pnd);
        if (PMIX_SUCCESS != rc) {
            goto cleanup;
        }
        return PMIX_SUCCESS;
    } else {
        pmix_host_server.tool_connected(pnd->info, pnd->ninfo, cnct_cbfunc, pnd);
    }
    return PMIX_SUCCESS;

cleanup:
    /* Unwind exactly what we built here and nothing else. The peer holds
     * one reference on each of the rank-info and namespace objects, so
     * releasing it below gives those back - the pre-existing objects then
     * remain owned by the lists that already held them. pnd->info is
     * owned by the pending connection and is freed by its destructor when
     * our caller releases it. */
    if (pnd->rinfo_created) {
        /* we appended this rank info to the namespace - withdraw it, and
         * give back the reference that list was holding, since removing
         * an item does not release it. The peer's own reference is given
         * back when the peer is released below. */
        pmix_list_remove_item(&nptr->ranks, &peer->info->super);
        PMIX_RELEASE(peer->info);
    }
    if (pnd->nspace_created) {
        /* the namespace we created was never placed on the global list -
         * that only happens once the host has assigned the tool an
         * identity - so simply drop the reference we held for it */
        PMIX_RELEASE(nptr);
    }
    PMIX_RELEASE(peer);
    return rc;

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
