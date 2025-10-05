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
 * Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
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
    pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                        "pmix:server client %s:%u has connected on socket %d",
                        ch->peer->info->pname.nspace, ch->peer->info->pname.rank, ch->peer->sd);

    /* check the cached events and update the client */
    _check_cached_events(ch->peer);
    PMIX_RELEASE(ch);
    return;

error:
    if (NULL != ch->peer) {
        pmix_pointer_array_set_item(&pmix_server_globals.clients, ch->peer->index, NULL);
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
    char *msg = NULL, *mg, *p, *blob = NULL;
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
    major = strtoul(pnd->version, &p, 10);
    ++p;
    minor = strtoul(p, &p, 10);
    ++p;
    release = strtoul(p, NULL, 10);
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
            i32 = cnt;
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
    if (PMIX_SUCCESS != reply) {
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
    if (NULL != info) {
        info->proc_cnt--;
        PMIX_RELEASE(info);
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
        pmix_pointer_array_set_item(&pmix_server_globals.clients, peer->index, NULL);
        PMIX_RELEASE(peer);
    }
    CLOSE_THE_SOCKET(pnd->sd);
    PMIX_RELEASE(pnd);
    return;
}

/* process the host's callback with tool connection info */
static void process_cbfunc(int sd, short args, void *cbdata)
{
    pmix_setup_caddy_t *cd = (pmix_setup_caddy_t *) cbdata;
    pmix_pending_connection_t *pnd = (pmix_pending_connection_t *) cd->cbdata;
    pmix_namespace_t *nptr = NULL, *nptr2, *nptr3;
    pmix_rank_info_t *info;
    pmix_peer_t *peer = NULL, *pr2;
    pmix_status_t rc, reply;
    uint32_t u32;
    int n;
    pmix_info_t ginfo;
    pmix_byte_object_t cred;
    pmix_iof_req_t *req = NULL;

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
    } else if (pnd->nspace_created) {
        // must add it to the global list - but check to ensure
        // it is unique as the host may have "registered" this
        // namespace, and so we would already have that record
        nptr3 = NULL;
        PMIX_LIST_FOREACH(nptr2, &pmix_globals.nspaces, pmix_namespace_t) {
            if (PMIx_Check_nspace(nptr->nspace, nptr2->nspace)) {
                nptr3 = nptr2;
                break;
            }
        }
        if (NULL == nptr3) {
            pmix_list_append(&pmix_globals.nspaces, &nptr->super);
        } else {
            // need to release this to avoid memory leak
            PMIX_RELEASE(nptr);
            nptr = nptr3;
            // reconnect the peer
            peer->nptr = nptr;
        }
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

    /* get the appropriate compatibility modules based on the
     * info provided by the tool during the initial connection request */
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
    if (PMIX_SUCCESS != reply) {
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "validation of tool credentials failed: %s",
                            PMIx_Error_string(reply));
    }

    /* communicate the result to the other side */
    u32 = htonl(reply);
    rc = pmix_ptl_base_send_blocking(pnd->sd, (char *) &u32, sizeof(uint32_t));
    if (PMIX_SUCCESS != rc || PMIX_SUCCESS != reply) {
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
            pmix_list_remove_item(&peer->nptr->ranks, &peer->info->super);
            // the info object will be released along with the peer
        }
        if (NULL != peer->nptr && pnd->nspace_created) {
            pmix_list_remove_item(&pmix_globals.nspaces, &peer->nptr->super);
            // the nptr will be released along with the peer
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
        PMIX_RETAIN(nptr);

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
            /* this proc has not connected - this cannot happen
             * for a tool as we otherwise would not know of it */
            PMIX_RELEASE(peer);
            PMIX_ERROR_LOG(PMIX_ERR_NOT_SUPPORTED);
            return PMIX_ERR_NOT_SUPPORTED;
        }
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
        PMIX_RETAIN(nptr);

    }

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
            PMIX_RELEASE(peer);
            PMIx_Info_list_release(ilist);
            return rc;
        }
        foo = (int32_t) sz;
        PMIX_INFO_CREATE(iptr, sz);
        PMIX_BFROPS_UNPACK(rc, peer, &buf, iptr, &foo, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(peer);
            PMIX_INFO_FREE(iptr, sz);
            PMIx_Info_list_release(ilist);
            return rc;
        }
        for (n=0; n < sz; n++) {
            PMIx_Info_list_xfer(ilist, &iptr[n]);
        }
        PMIX_INFO_FREE(iptr, sz);
    }

    /* does the server support tool connections? */
    if (NULL == pmix_host_server.tool_connected) {
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
    pmix_host_server.tool_connected(pnd->info, pnd->ninfo, cnct_cbfunc, pnd);
    return PMIX_SUCCESS;

cleanup:
    if (pnd->rinfo_created) {
        pmix_list_remove_item(&nptr->ranks, &peer->info->super);
        PMIX_RELEASE(pnd->info);
    } else {
        peer->info = NULL;
    }

    if (pnd->nspace_created) {
        pmix_list_remove_item(&pmix_globals.nspaces, &nptr->super);
        PMIX_RELEASE(nptr);
    } else {
        peer->nptr = NULL;  // protect it
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
                    /* track the number of targets we have left to notify */
                    --cd->nleft;
                    /* if this is the last one, then evict this event
                     * from the cache */
                    if (0 == cd->nleft) {
                        pmix_hotel_checkout(&pmix_globals.notifications, cd->room);
                        found = true; // mark that we should release cd
                    }
                    break;
                }
            }
            if (!matched) {
                /* do not notify this one */
                continue;
            }
        }

        /* all matches - notify */
        relay = PMIX_NEW(pmix_buffer_t);
        if (NULL == relay) {
            /* nothing we can do */
            PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
            break;
        }
        /* pack the info data stored in the event */
        PMIX_BFROPS_PACK(ret, peer, relay, &cmd, 1, PMIX_COMMAND);
        if (PMIX_SUCCESS != ret) {
            PMIX_ERROR_LOG(ret);
            PMIX_RELEASE(relay);
            break;
        }
        PMIX_BFROPS_PACK(ret, peer, relay, &cd->status, 1, PMIX_STATUS);
        if (PMIX_SUCCESS != ret) {
            PMIX_ERROR_LOG(ret);
            PMIX_RELEASE(relay);
            break;
        }
        PMIX_BFROPS_PACK(ret, peer, relay, &cd->source, 1, PMIX_PROC);
        if (PMIX_SUCCESS != ret) {
            PMIX_RELEASE(relay);
            PMIX_ERROR_LOG(ret);
            break;
        }
        PMIX_BFROPS_PACK(ret, peer, relay, &cd->ninfo, 1, PMIX_SIZE);
        if (PMIX_SUCCESS != ret) {
            PMIX_ERROR_LOG(ret);
            PMIX_RELEASE(relay);
            break;
        }
        if (0 < cd->ninfo) {
            PMIX_BFROPS_PACK(ret, peer, relay, cd->info, cd->ninfo, PMIX_INFO);
            if (PMIX_SUCCESS != ret) {
                PMIX_ERROR_LOG(ret);
                PMIX_RELEASE(relay);
                break;
            }
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
