/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2014-2015 Artem Y. Polyakov <artpol84@gmail.com>.
 *                         All rights reserved.
 * Copyright (c) 2016-2020 Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2016-2018 IBM Corporation.  All rights reserved.
 * Copyright (c) 2018      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * Copyright (c) 2022-2023 Triad National Security, LLC. All rights reserved.
 * Copyright (c) 2026      Jeff Squyres  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/* The switchyard: dispatch of inbound client and tool commands to their
 * handlers, and the family of host-server callbacks that queue the
 * resulting replies. */

#include "src/include/pmix_config.h"

#include "src/include/pmix_stdint.h"

#include "pmix_common.h"
#include "include/pmix_server.h"

#include "src/include/pmix_globals.h"

#ifdef HAVE_STRING_H
#    include <string.h>
#endif
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif

#include "src/mca/bfrops/base/base.h"
#include "src/mca/gds/base/base.h"
#include "src/mca/ptl/base/base.h"
#include "src/tool/pmix_tool_ops.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_name_fns.h"
#include "src/util/pmix_output.h"

#include "pmix_server_ops.h"

/****    THE FOLLOWING CALLBACK FUNCTIONS ARE USED BY THE HOST SERVER    ****
 ****    THEY THEREFORE CAN OCCUR IN EITHER THE HOST SERVER'S THREAD     ****
 ****    CONTEXT, OR IN OUR OWN THREAD CONTEXT IF THE CALLBACK OCCURS    ****
 ****    IMMEDIATELY. THUS ANYTHING THAT ACCESSES A GLOBAL ENTITY        ****
 ****    MUST BE PUSHED INTO AN EVENT FOR PROTECTION                     ****/

static void _opcbfunc(int sd, short args, void *cbdata)
{
    pmix_shift_caddy_t *scd = (pmix_shift_caddy_t*)cbdata;
    pmix_server_caddy_t *cd = (pmix_server_caddy_t*)scd->cbdata;
    pmix_buffer_t *reply;
    pmix_status_t rc;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_ACQUIRE_OBJECT(scd);

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        PMIX_RELEASE(cd);
        PMIX_RELEASE(scd);
        return;
    }

    /* setup the reply with the returned status */
    if (NULL == (reply = PMIX_NEW(pmix_buffer_t))) {
        PMIX_ERROR_LOG(PMIX_ERR_OUT_OF_RESOURCE);
        goto cleanup;
    }
    PMIX_BFROPS_PACK(rc, cd->peer, reply, &scd->status, 1, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(reply);
        goto cleanup;
    }

    /* the function that created the server_caddy did a
     * retain on the peer, so we don't have to worry about
     * it still being present - send a copy to the originator */
    PMIX_PTL_SEND_ONEWAY(rc, cd->peer, reply, cd->hdr.tag);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(reply);
    }

    if (scd->enviro) {
        /* ensure that we know the peer has finalized else we
         * will generate an event when the socket closes - yes,
         * it should have been done, but it is REALLY important
         * that it be set */
        cd->peer->finalized = true;
    }

cleanup:
    /* cleanup */
    PMIX_RELEASE(cd);
    PMIX_RELEASE(scd);
}

static void op_cbfunc(pmix_status_t status, void *cbdata)
{
    pmix_shift_caddy_t *scd;
    pmix_server_caddy_t *cd = (pmix_server_caddy_t*)cbdata;

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "server:op_cbfunc called with %s status",
                        PMIx_Error_string(status));

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        PMIX_RELEASE(cd);
        return;
    }

    /* need to thread-shift this callback */
    scd = PMIX_NEW(pmix_shift_caddy_t);
    if (NULL == scd) {
        /* we cannot answer the client, but the caddy is ours either way -
         * dropping it strands the RETAIN it holds on the peer, and with it
         * the peer and everything hanging off it, for the life of the
         * server. Same as the finalize-race arm above. */
        PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
        PMIX_RELEASE(cd);
        return;
    }
    scd->status = status;
    scd->cbdata = cbdata;
    scd->enviro = false;  // flag that we are not finalizing the peer
    PMIX_THREADSHIFT(scd, _opcbfunc);
}

static void op_cbfunc2(pmix_status_t status, void *cbdata)
{
    pmix_shift_caddy_t *scd;
    pmix_server_caddy_t *cd = (pmix_server_caddy_t*)cbdata;

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "server:op_cbfunc2 called with %s status",
                        PMIx_Error_string(status));

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        PMIX_RELEASE(cd);
        return;
    }

    /* need to thread-shift this callback */
    scd = PMIX_NEW(pmix_shift_caddy_t);
    if (NULL == scd) {
        /* the caddy is ours either way - see op_cbfunc */
        PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
        PMIX_RELEASE(cd);
        return;
    }
    scd->status = status;
    scd->cbdata = cbdata;
    scd->enviro = true;  // flag that we are finalizing this peer
    PMIX_THREADSHIFT(scd, _opcbfunc);
}

static void _resopcbfunc(int sd, short args, void *cbdata)
{
    pmix_shift_caddy_t *scdwrapper = (pmix_shift_caddy_t *) cbdata;
    pmix_setup_caddy_t *scd = (pmix_setup_caddy_t*)scdwrapper->cbdata;
    pmix_server_caddy_t *cd = (pmix_server_caddy_t *) scd->cbdata;
    pmix_buffer_t *reply = NULL;
    pmix_status_t rc;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_ACQUIRE_OBJECT(scdwrapper);

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        goto cleanup;
    }

    /* setup the reply with the returned status */
    if (NULL == (reply = PMIX_NEW(pmix_buffer_t))) {
        PMIX_ERROR_LOG(PMIX_ERR_OUT_OF_RESOURCE);
        goto cleanup;
    }
    PMIX_BFROPS_PACK(rc, cd->peer, reply, &scdwrapper->status, 1, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto cleanup;
    }

    /* the function that created the server_caddy did a
     * retain on the peer, so we don't have to worry about
     * it still being present - send a copy to the originator */
    PMIX_PTL_SEND_ONEWAY(rc, cd->peer, reply, cd->hdr.tag);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto cleanup;
    }
    /* the PTL owns the reply on a successful send - fall through to
     * release the caddies (previously this path returned early, leaking
     * cd, scd, scd->nspace, and scdwrapper on every successful reply) */
    reply = NULL;

    /* cleanup */
cleanup:
    PMIX_RELEASE(cd);
    if (NULL != scd->nspace) {
        free(scd->nspace);
    }
    PMIX_RELEASE(scd);
    PMIX_RELEASE(scdwrapper);
    if (NULL != reply) {
        PMIX_RELEASE(reply);
    }
}

/* Unlike op_cbfunc, what the host hands back here is the resource-block
 * handler's own pmix_setup_caddy_t (pmix_server_resblk passes "scd" as the
 * cbdata of the up-call), with the switchyard's server caddy nested inside
 * it on cbdata. So this one owns three things, and neither destructor
 * reaches the other two: scaddes does not free "nspace" and does not touch
 * "cbdata". Both early returns below released only the outermost object -
 * declared, on top of that, as the wrong type - and stranded the server
 * caddy, the RETAIN it holds on the requesting peer, and the block name. */
static void resop_cbfunc(pmix_status_t status, void *cbdata)
{
    pmix_shift_caddy_t *scdwrapper;
    pmix_setup_caddy_t *scd = (pmix_setup_caddy_t *) cbdata;
    pmix_server_caddy_t *cd = (pmix_server_caddy_t *) scd->cbdata;

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "server:resop_cbfunc called with %s status",
                        PMIx_Error_string(status));

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        goto teardown;
    }

    /* need to thread-shift this callback */
    scdwrapper = PMIX_NEW(pmix_shift_caddy_t);
    if (NULL == scdwrapper) {
        PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
        goto teardown;
    }
    scdwrapper->status = status;
    scdwrapper->cbdata = cbdata;
    PMIX_THREADSHIFT(scdwrapper, _resopcbfunc);
    return;

teardown:
    /* the same three releases _resopcbfunc makes on its way out */
    PMIX_RELEASE(cd);
    if (NULL != scd->nspace) {
        free(scd->nspace);
    }
    PMIX_RELEASE(scd);
}

/* the switchyard is the primary message handling function. It's purpose
 * is to take incoming commands (packed into a buffer), unpack them,
 * and then call the corresponding host server's function to execute
 * them. Some commands involve only a single proc (i.e., the one
 * sending the command) and can be executed while we wait. In these cases,
 * the switchyard will construct and pack a reply buffer to be returned
 * to the sender.
 *
 * Other cases (either multi-process collective or cmds that require
 * an async reply) cannot generate an immediate reply. In these cases,
 * the reply buffer will be NULL. An appropriate callback function will
 * be called that will be responsible for eventually replying to the
 * calling processes.
 *
 * Should an error be encountered at any time within the switchyard, an
 * error reply buffer will be returned so that the caller can be notified,
 * thereby preventing the process from hanging. */
static pmix_status_t server_switchyard(pmix_peer_t *peer, uint32_t tag, pmix_buffer_t *buf)
{
    pmix_status_t rc = PMIX_ERR_NOT_SUPPORTED;
    pmix_status_t ret;
    int32_t cnt;
    pmix_cmd_t cmd;
    pmix_server_caddy_t *cd;
    pmix_proc_t proc;
    pmix_buffer_t *reply;

    /* protect against zero-byte buffers - these can come if the
     * connection is dropped due to a process failure */
    if (PMIX_BUFFER_IS_EMPTY(buf)) {
        return PMIX_ERR_LOST_CONNECTION;
    }

    /* retrieve the cmd */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &cmd, &cnt, PMIX_COMMAND);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "%s recvd pmix cmd %s from %s bytes %u",
                        PMIX_NAME_PRINT(&pmix_globals.myid),
                        pmix_command_string(cmd),
                        PMIX_PEER_PRINT(peer),
                        (unsigned int) buf->bytes_used);

    /* If I am a tool, relay this to my primary server when I have one.
     * The rule is relay-if-attached, service-it-myself otherwise - the
     * same one PMIx_Spawn applies to a launcher's own requests. */
    if (PMIX_PEER_IS_TOOL(pmix_globals.mypeer)) {
        rc = pmix_tool_relay_op(cmd, peer, buf, tag);
        if (PMIX_ERR_NOT_SUPPORTED != rc && PMIX_ERR_UNREACH != rc) {
            return rc;
        }
        /* Fall through to the logic tree, either because this is not a
         * command we relay (PMIX_ERR_NOT_SUPPORTED) or because we have no
         * server to relay it to (PMIX_ERR_UNREACH). A launcher with no
         * server attached fork/execs a spawn itself from there; a role
         * that genuinely cannot service the command answers
         * PMIX_ERR_NOT_SUPPORTED from the handler, which is the right
         * answer to give the requester anyway. Note pmix_tool_relay_op
         * returns both of those BEFORE it rewinds the buffer, so our
         * unpack position is still where the cmd read left it. */
    }

    /* if I am a server, then redirect the cmd to the appropriate
     * function for processing */

    if (PMIX_REQ_CMD == cmd) {
        reply = PMIX_NEW(pmix_buffer_t);
        if (NULL == reply) {
            PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
            return PMIX_ERR_NOMEM;
        }
        PMIX_GDS_REGISTER_JOB_INFO(rc, peer, reply);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(reply);
            return rc;
        }
        PMIX_SERVER_QUEUE_REPLY(rc, peer, tag, reply);
        if (PMIX_SUCCESS != rc) {
            /* the same reasoning as the fallback arm below: nothing else
             * answers this client, so swallowing the failure left its
             * PMIx_Init blocked on a reply that was never queued. A
             * finalized peer (PMIX_ERR_UNREACH) has stopped waiting, but
             * a peer we simply could not allocate a send for has not. */
            PMIX_RELEASE(reply);
            return rc;
        }
        /* count the delivery only once it is really queued: gds/hash
         * drops its cached copy of the packed job data when ndelivered
         * reaches nlocalprocs, so a delivery counted but not made costs
         * the next client a re-pack */
        peer->nptr->ndelivered++;
        return PMIX_SUCCESS;
    }

    if (PMIX_GDS_FALLBACK_CMD == cmd) {
        /* the client could not use the GDS module it selected at connect
         * time and has switched to another one. Record the new module on
         * this peer (only this peer - its nspace peers are unaffected) and
         * re-register the job data in that module's format. */
        char *modname = NULL;
        pmix_gds_base_module_t *mod;
        pmix_info_t ginfo;

        cnt = 1;
        PMIX_BFROPS_UNPACK(rc, peer, buf, &modname, &cnt, PMIX_STRING);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            return rc;
        }
        /* guard against a malformed/malicious client: a zero-length string
         * unpacks to NULL, and PMIX_INFO_LOAD would strdup(NULL) and crash */
        if (NULL == modname) {
            return PMIX_ERR_BAD_PARAM;
        }
        PMIX_INFO_LOAD(&ginfo, PMIX_GDS_MODULE, modname, PMIX_STRING);
        mod = pmix_gds_base_assign_module(&ginfo, 1);
        PMIX_INFO_DESTRUCT(&ginfo);
        /* assign_module always returns some module (modules offer themselves
         * by priority and a requested name only bumps that module's
         * priority), so confirm we actually got the module the client named
         * rather than a higher-priority default. If not, we do not have it. */
        if (NULL == mod || 0 != strcmp(mod->name, modname)) {
            free(modname);
            return PMIX_ERR_NOT_SUPPORTED;
        }
        free(modname);
        peer->gds = mod;
        reply = PMIX_NEW(pmix_buffer_t);
        if (NULL == reply) {
            PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
            return PMIX_ERR_NOMEM;
        }
        PMIX_GDS_REGISTER_JOB_INFO(rc, peer, reply);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(reply);
            return rc;
        }
        PMIX_SERVER_QUEUE_REPLY(rc, peer, tag, reply);
        if (PMIX_SUCCESS != rc) {
            /* return the error so the switchyard caller sends a failure
             * reply rather than leaving the client's PMIx_Init blocked
             * waiting for a response that was never queued */
            PMIX_RELEASE(reply);
            return rc;
        }
        /* do NOT increment ndelivered: this client was already counted by
         * its original PMIX_REQ_CMD; this is the same client re-requesting
         * the same job data in a different format */
        return PMIX_SUCCESS;
    }

    if (PMIX_ABORT_CMD == cmd) {
        PMIX_GDS_CADDY(cd, peer, tag);
        if (NULL == cd) {
            return PMIX_ERR_NOMEM;
        }
        if (PMIX_SUCCESS != (rc = pmix_server_abort(peer, buf, op_cbfunc, cd))) {
            PMIX_RELEASE(cd);
        }
        return rc;
    }

    if (PMIX_COMMIT_CMD == cmd) {
        /* keep the commit's own status in a variable the pack does not
         * write: PMIX_BFROPS_PACK assigns the status of the *pack* to the
         * name it is given, so packing "rc" from "&rc" left the two
         * meanings sharing one variable */
        ret = pmix_server_commit(peer, buf);
        if (!PMIX_PEER_IS_V1(peer)) {
            reply = PMIX_NEW(pmix_buffer_t);
            if (NULL == reply) {
                PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
                return PMIX_ERR_NOMEM;
            }
            PMIX_BFROPS_PACK(rc, peer, reply, &ret, 1, PMIX_STATUS);
            if (PMIX_SUCCESS != rc) {
                /* an empty buffer is not an answer the client can read,
                 * and queuing one used it up as *the* reply. Hand the
                 * failure back instead and let the message handler build
                 * the status reply it builds for every other error */
                PMIX_ERROR_LOG(rc);
                PMIX_RELEASE(reply);
                return rc;
            }
            PMIX_SERVER_QUEUE_REPLY(rc, peer, tag, reply);
            if (PMIX_SUCCESS != rc) {
                PMIX_RELEASE(reply);
                return rc;
            }
        }
        return PMIX_SUCCESS; // don't reply twice
    }

    if (PMIX_FENCENB_CMD == cmd) {
        PMIX_GDS_CADDY(cd, peer, tag);
        if (NULL == cd) {
            return PMIX_ERR_NOMEM;
        }
        if (PMIX_SUCCESS != (rc = pmix_server_fence(cd, buf, pmix_server_modex_cbfunc))) {
            PMIX_RELEASE(cd);
        }
        return rc;
    }

    if (PMIX_GETNB_CMD == cmd) {
        PMIX_GDS_CADDY(cd, peer, tag);
        if (NULL == cd) {
            return PMIX_ERR_NOMEM;
        }
        if (PMIX_SUCCESS != (rc = pmix_server_get(buf, pmix_server_get_cbfunc, cd))) {
            PMIX_RELEASE(cd);
        }
        return rc;
    }

    if (PMIX_FINALIZE_CMD == cmd) {
        peer->nptr->nfinalized++;
        /* purge events.  This peer finalized - it did not vanish - so a
         * request parked on data it never published is answered "not
         * found", not "lost connection": the asker's own session is
         * perfectly healthy and telling it otherwise reads as fatal. */
        pmix_server_purge_events(peer, NULL, PMIX_ERR_NOT_FOUND);
        PMIX_GDS_CADDY(cd, peer, tag);
        if (NULL == cd) {
            return PMIX_ERR_NOMEM;
        }
        /* Call the local server, if supported. A tool counts here just as a
         * client does: the host was told when the tool connected, and this
         * is the only notice it will ever get that the tool has gone. The
         * connection drop that follows cannot serve instead - the peer is
         * marked finalized by then, so no lost-connection event is raised
         * for it - so a host that skipped this call would carry the tool's
         * state, and anything the tool was granted, for the rest of its own
         * lifetime. peer->info is set for a tool peer exactly as it is for
         * a client; server_object is simply NULL for one the host never
         * registered, which the host must already tolerate. */
        if (NULL != pmix_host_server.client_finalized &&
            (PMIX_PEER_IS_CLIENT(peer) || PMIX_PEER_IS_TOOL(peer))) {
            pmix_strncpy(proc.nspace, peer->info->pname.nspace, PMIX_MAX_NSLEN);
            proc.rank = peer->info->pname.rank;
            /* now tell the host server */
            rc = pmix_host_server.client_finalized(&proc, peer->info->server_object, op_cbfunc2, cd);
            if (PMIX_SUCCESS == rc) {
                /* don't reply to them ourselves - we will do so when the host
                 * server calls us back */
                return rc;
            } else if (PMIX_OPERATION_SUCCEEDED == rc) {
                /* they did it atomically */
                rc = PMIX_SUCCESS;
            }
            /* if the call doesn't succeed (e.g., they provided the stub
             * but return NOT_SUPPORTED), then the callback function
             * won't be called, but we still need to cleanup
             * any lingering references to this peer and answer
             * the client. Thus, we call the callback function ourselves
             * in this case */
            op_cbfunc2(rc, cd);
            /* return SUCCESS as the cbfunc generated the return msg
             * and released the cd object */
            return PMIX_SUCCESS;
        }
        /* if the host doesn't provide a client_finalized function,
         * we still need to ensure that we cleanup any lingering
         * references to this peer. We use the callback function
         * here as well to ensure the client gets its required
         * response and that we delay before cleaning up the
         * connection*/
        op_cbfunc2(PMIX_SUCCESS, cd);
        /* return SUCCESS as the cbfunc generated the return msg
         * and released the cd object */
        return PMIX_SUCCESS;
    }

    if (PMIX_PUBLISHNB_CMD == cmd) {
        PMIX_GDS_CADDY(cd, peer, tag);
        if (NULL == cd) {
            return PMIX_ERR_NOMEM;
        }
        if (PMIX_SUCCESS != (rc = pmix_server_publish(peer, buf, op_cbfunc, cd))) {
            PMIX_RELEASE(cd);
        }
        return rc;
    }

    if (PMIX_LOOKUPNB_CMD == cmd) {
        PMIX_GDS_CADDY(cd, peer, tag);
        if (NULL == cd) {
            return PMIX_ERR_NOMEM;
        }
        if (PMIX_SUCCESS != (rc = pmix_server_lookup(peer, buf, pmix_server_lookup_cbfunc, cd))) {
            PMIX_RELEASE(cd);
        }
        return rc;
    }

    if (PMIX_UNPUBLISHNB_CMD == cmd) {
        PMIX_GDS_CADDY(cd, peer, tag);
        if (NULL == cd) {
            return PMIX_ERR_NOMEM;
        }
        if (PMIX_SUCCESS != (rc = pmix_server_unpublish(peer, buf, op_cbfunc, cd))) {
            PMIX_RELEASE(cd);
        }
        return rc;
    }

    if (PMIX_SPAWNNB_CMD == cmd) {
        PMIX_GDS_CADDY(cd, peer, tag);
        if (NULL == cd) {
            return PMIX_ERR_NOMEM;
        }
        if (PMIX_SUCCESS != (rc = pmix_server_spawn(peer, buf, pmix_server_spawn_cbfunc, cd))) {
            PMIX_RELEASE(cd);
        }
        return rc;
    }

    if (PMIX_CONNECTNB_CMD == cmd) {
        PMIX_GDS_CADDY(cd, peer, tag);
        if (NULL == cd) {
            return PMIX_ERR_NOMEM;
        }
        rc = pmix_server_connect(cd, buf, pmix_server_cnct_cbfunc);
        if (PMIX_SUCCESS != rc) {
            PMIX_RELEASE(cd);
        }
        return rc;
    }

    if (PMIX_DISCONNECTNB_CMD == cmd) {
        PMIX_GDS_CADDY(cd, peer, tag);
        if (NULL == cd) {
            return PMIX_ERR_NOMEM;
        }
        rc = pmix_server_disconnect(cd, buf, pmix_server_discnct_cbfunc);
        if (PMIX_SUCCESS != rc) {
            PMIX_RELEASE(cd);
        }
        return rc;
    }

    if (PMIX_REGEVENTS_CMD == cmd) {
        PMIX_GDS_CADDY(cd, peer, tag);
        if (NULL == cd) {
            return PMIX_ERR_NOMEM;
        }
        rc = pmix_server_register_events(peer, buf, pmix_server_events_cbfunc, cd);
        if (PMIX_SUCCESS != rc) {
            PMIX_RELEASE(cd);
        }
        return rc;
    }

    if (PMIX_DEREGEVENTS_CMD == cmd) {
        pmix_server_deregister_events(peer, buf);
        return PMIX_SUCCESS;
    }

    if (PMIX_NOTIFY_CMD == cmd) {
        PMIX_GDS_CADDY(cd, peer, tag);
        if (NULL == cd) {
            return PMIX_ERR_NOMEM;
        }
        rc = pmix_server_event_recvd_from_client(peer, buf, pmix_server_events_cbfunc, cd);
        if (PMIX_SUCCESS != rc) {
            PMIX_RELEASE(cd);
        }
        return rc;
    }

    if (PMIX_QUERY_CMD == cmd) {
        PMIX_GDS_CADDY(cd, peer, tag);
        if (NULL == cd) {
            return PMIX_ERR_NOMEM;
        }
        rc = pmix_server_query(peer, buf, pmix_server_query_cbfunc, cd);
        if (PMIX_SUCCESS != rc) {
            PMIX_RELEASE(cd);
        }
        return rc;
    }

    if (PMIX_LOG_CMD == cmd) {
        PMIX_GDS_CADDY(cd, peer, tag);
        if (NULL == cd) {
            return PMIX_ERR_NOMEM;
        }
        if (PMIX_SUCCESS != (rc = pmix_server_log(peer, buf, op_cbfunc, cd))) {
            PMIX_RELEASE(cd);
        }
        return rc;
    }

    if (PMIX_ALLOC_CMD == cmd) {
        PMIX_GDS_CADDY(cd, peer, tag);
        if (NULL == cd) {
            return PMIX_ERR_NOMEM;
        }
        if (PMIX_SUCCESS != (rc = pmix_server_alloc(peer, buf, pmix_server_alloc_cbfunc, cd))) {
            PMIX_RELEASE(cd);
        }
        return rc;
    }

    if (PMIX_JOB_CONTROL_CMD == cmd) {
        PMIX_GDS_CADDY(cd, peer, tag);
        if (NULL == cd) {
            return PMIX_ERR_NOMEM;
        }
        if (PMIX_SUCCESS != (rc = pmix_server_job_ctrl(peer, buf, pmix_server_jctrl_cbfunc, cd))) {
            PMIX_RELEASE(cd);
        }
        return rc;
    }

    if (PMIX_MONITOR_CMD == cmd) {
        PMIX_GDS_CADDY(cd, peer, tag);
        if (NULL == cd) {
            return PMIX_ERR_NOMEM;
        }
        if (PMIX_SUCCESS != (rc = pmix_server_monitor(peer, buf, pmix_server_monitor_cbfunc, cd))) {
            PMIX_RELEASE(cd);
        }
        return rc;
    }

    if (PMIX_GET_CREDENTIAL_CMD == cmd) {
        PMIX_GDS_CADDY(cd, peer, tag);
        if (NULL == cd) {
            return PMIX_ERR_NOMEM;
        }
        rc = pmix_server_get_credential(peer, buf, pmix_server_cred_cbfunc, cd);
        if (PMIX_SUCCESS != rc) {
            PMIX_RELEASE(cd);
        }
        return rc;
    }

    if (PMIX_VALIDATE_CRED_CMD == cmd) {
        PMIX_GDS_CADDY(cd, peer, tag);
        if (NULL == cd) {
            return PMIX_ERR_NOMEM;
        }
        if (PMIX_SUCCESS
            != (rc = pmix_server_validate_credential(peer, buf, pmix_server_validate_cbfunc, cd))) {
            PMIX_RELEASE(cd);
        }
        return rc;
    }

    if (PMIX_IOF_PULL_CMD == cmd) {
        PMIX_GDS_CADDY(cd, peer, tag);
        if (NULL == cd) {
            return PMIX_ERR_NOMEM;
        }
        if (PMIX_SUCCESS != (rc = pmix_server_iofreg(peer, buf, pmix_server_iofreg_cbfunc, cd))) {
            PMIX_RELEASE(cd);
        }
        return rc;
    }

    if (PMIX_IOF_PUSH_CMD == cmd) {
        PMIX_GDS_CADDY(cd, peer, tag);
        if (NULL == cd) {
            return PMIX_ERR_NOMEM;
        }
        if (PMIX_SUCCESS != (rc = pmix_server_iofstdin(peer, buf, op_cbfunc, cd))) {
            PMIX_RELEASE(cd);
        }
        return rc;
    }

    if (PMIX_IOF_DEREG_CMD == cmd) {
        PMIX_GDS_CADDY(cd, peer, tag);
        if (NULL == cd) {
            return PMIX_ERR_NOMEM;
        }
        rc = pmix_server_iofdereg(peer, buf, pmix_server_iofdereg_cbfunc, cd);
        if (PMIX_SUCCESS != rc) {
            PMIX_RELEASE(cd);
        }
        return rc;
    }

    if (PMIX_GROUP_CONSTRUCT_CMD == cmd) {
        PMIX_GDS_CADDY(cd, peer, tag);
        if (NULL == cd) {
            return PMIX_ERR_NOMEM;
        }
        if (PMIX_SUCCESS != (rc = pmix_server_group(cd, buf, PMIX_GROUP_CONSTRUCT))) {
            PMIX_RELEASE(cd);
        }
        return rc;
    }

    if (PMIX_GROUP_DESTRUCT_CMD == cmd) {
        PMIX_GDS_CADDY(cd, peer, tag);
        if (NULL == cd) {
            return PMIX_ERR_NOMEM;
        }
        if (PMIX_SUCCESS != (rc = pmix_server_group(cd, buf, PMIX_GROUP_DESTRUCT))) {
            PMIX_RELEASE(cd);
        }
        return rc;
    }

    if (PMIX_FABRIC_REGISTER_CMD == cmd) {
        PMIX_GDS_CADDY(cd, peer, tag);
        if (NULL == cd) {
            return PMIX_ERR_NOMEM;
        }
        rc = pmix_server_fabric_register(cd, buf, pmix_server_fabric_cbfunc);
        if (PMIX_SUCCESS != rc) {
            PMIX_RELEASE(cd);
        }
        return rc;
    }

    if (PMIX_FABRIC_UPDATE_CMD == cmd) {
        PMIX_GDS_CADDY(cd, peer, tag);
        if (NULL == cd) {
            return PMIX_ERR_NOMEM;
        }
        if (PMIX_SUCCESS != (rc = pmix_server_fabric_update(cd, buf, pmix_server_fabric_cbfunc))) {
            PMIX_RELEASE(cd);
        }
        return rc;
    }

    if (PMIX_COMPUTE_DEVICE_DISTANCES_CMD == cmd) {
        PMIX_GDS_CADDY(cd, peer, tag);
        if (NULL == cd) {
            return PMIX_ERR_NOMEM;
        }
        if (PMIX_SUCCESS != (rc = pmix_server_device_dists(cd, buf, pmix_server_dist_cbfunc))) {
            PMIX_RELEASE(cd);
        }
        return rc;
    }

    if (PMIX_REFRESH_CACHE == cmd) {
        PMIX_GDS_CADDY(cd, peer, tag);
        if (NULL == cd) {
            return PMIX_ERR_NOMEM;
        }
        if (PMIX_SUCCESS != (rc = pmix_server_refresh_cache(cd, buf, op_cbfunc))) {
            PMIX_RELEASE(cd);
        }
        return rc;
    }

    if (PMIX_RESBLK_CMD == cmd) {
        PMIX_GDS_CADDY(cd, peer, tag);
        if (NULL == cd) {
            return PMIX_ERR_NOMEM;
        }
        if (PMIX_SUCCESS != (rc = pmix_server_resblk(cd, buf, resop_cbfunc))) {
            PMIX_RELEASE(cd);
        }
        return rc;
    }

    if (PMIX_SESSION_CTRL_CMD == cmd) {
        PMIX_GDS_CADDY(cd, peer, tag);
        if (NULL == cd) {
            return PMIX_ERR_NOMEM;
        }
        if (PMIX_SUCCESS != (rc = pmix_server_session_ctrl(cd, buf, pmix_server_sessctrl_cbfunc))) {
            PMIX_RELEASE(cd);
        }
        return rc;
    }

    if (PMIX_RESOLVE_PEERS_CMD == cmd) {
        PMIX_GDS_CADDY(cd, peer, tag);
        if (NULL == cd) {
            return PMIX_ERR_NOMEM;
        }
        rc = pmix_server_resolve_peers(cd, buf, pmix_server_respeers_cbfunc);
        if (PMIX_SUCCESS != rc) {
            PMIX_RELEASE(cd);
        }
        return rc;
    }

    if (PMIX_RESOLVE_NODE_CMD == cmd) {
        PMIX_GDS_CADDY(cd, peer, tag);
        if (NULL == cd) {
            return PMIX_ERR_NOMEM;
        }
        if (PMIX_SUCCESS != (rc = pmix_server_resolve_node(cd, buf, pmix_server_resnodes_cbfunc))) {
            PMIX_RELEASE(cd);
        }
        return rc;
    }

    return PMIX_ERR_NOT_SUPPORTED;
}

void pmix_server_message_handler(struct pmix_peer_t *pr, pmix_ptl_hdr_t *hdr,
                                 pmix_buffer_t *buf, void *cbdata)
{
    pmix_peer_t *peer = (pmix_peer_t *) pr;
    pmix_buffer_t *reply;
    pmix_status_t rc, ret;

    PMIX_HIDE_UNUSED_PARAMS(cbdata);

    /* A heartbeat is not a command, and it is the one reserved tag that
     * can reach this handler. psensor/heartbeat posts a recv of its own
     * for the tag, but only once the first heartbeat monitor is armed -
     * so a beat sent before that is matched instead by the wildcard recv
     * this handler serves. Passing it down would hand server_switchyard
     * a zero-byte buffer it cannot read a command out of, and the error
     * that comes back would be queued to the client on tag 1, where
     * nothing is listening: PMIx_Heartbeat is one-way and no client ever
     * posts for it. Drop it instead. Nothing is lost - there is no
     * monitor to credit the beat to yet, which is precisely why the recv
     * it belongs to has not been posted. */
    if (PMIX_PTL_TAG_HEARTBEAT == hdr->tag) {
        pmix_output_verbose(2, pmix_server_globals.base_output,
                            "SWITCHYARD ignoring heartbeat from %s:%u with no monitor armed",
                            peer->info->pname.nspace, peer->info->pname.rank);
        return;
    }

    pmix_output_verbose(2, pmix_server_globals.base_output, "SWITCHYARD for %s:%u:%d",
                        peer->info->pname.nspace, peer->info->pname.rank, peer->sd);

    ret = server_switchyard(peer, hdr->tag, buf);
    /* send the return, if there was an error returned */
    if (PMIX_SUCCESS != ret) {
        reply = PMIX_NEW(pmix_buffer_t);
        if (NULL == reply) {
            PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
            return;
        }
        if (PMIX_OPERATION_SUCCEEDED == ret) {
            ret = PMIX_SUCCESS;
        }
        PMIX_BFROPS_PACK(rc, peer, reply, &ret, 1, PMIX_STATUS);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
        }
        PMIX_SERVER_QUEUE_REPLY(rc, peer, hdr->tag, reply);
        if (PMIX_SUCCESS != rc) {
            PMIX_RELEASE(reply);
        }
    }
}
