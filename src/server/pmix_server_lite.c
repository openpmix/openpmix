/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2015      Intel, Inc.  All rights reserved.
 * Copyright (c) 2015      Artem Y. Polyakov <artpol84@gmail.com>.
 *                         All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "pmix_config.h"
#include "src/api/pmix_server.h"
#include "src/util/error.h"

#include "pmix_server_ops.h"
#include "pmix_server_lite.h"

static void setup_op_response(int status, pmix_message_t **reply);
static void op_cbfunc(int status, void *cbdata);
static void server_switchyard(pmix_peer_t *peer, uint32_t tag,
                              pmix_buffer_t *buf,
                              pmix_message_t **reply);
static void setup_get_response(pmix_cb_t *cb, pmix_message_t **reply);
static void setup_lookup_response(pmix_cb_t *cb, pmix_message_t **reply);
static void setup_spawn_response(pmix_cb_t *cb, pmix_message_t **reply);

pmix_message_t *PMIx_message_new(void)
{
      pmix_message_inst_t *msg = malloc(sizeof(pmix_message_inst_t));
      if( NULL == msg ){
          return NULL;
      }
      memset(msg, 0, sizeof(*msg));
      return (pmix_message_t *)msg;
}

uint32_t PMIx_message_hdr_size(pmix_message_t *msg_opaq)
{
    pmix_message_inst_t *msg = (pmix_message_inst_t *)msg_opaq;
    if( NULL == msg){
        return 0;
    }
    return sizeof(msg->hdr);
}

void *PMIx_message_hdr_ptr(pmix_message_t *msg_opaq)
{
    pmix_message_inst_t *msg = (pmix_message_inst_t *)msg_opaq;
    if( NULL == msg){
        return NULL;
    }
    return &(msg->hdr);
}

int PMIx_message_hdr_fix(pmix_message_t *msg_opaq)
{
    pmix_message_inst_t *msg = (pmix_message_inst_t *)msg_opaq;
    msg->hdr_rcvd = 1;
    if( 0 < msg->hdr.nbytes ){
        msg->payload = malloc(msg->hdr.nbytes);
        if( NULL == msg->payload ){
            return PMIX_ERR_OUT_OF_RESOURCE;
        }
    }else {
        msg->payload = NULL;
    }
    return PMIX_SUCCESS;
}

void PMIx_message_tag_set(pmix_message_t *msg_opaq, uint32_t tag)
{
    pmix_message_inst_t *msg = (pmix_message_inst_t *)msg_opaq;
    if( NULL == msg ){
        return;
    }
    msg->hdr.tag = tag;
}


int PMIx_message_set_payload(pmix_message_t *msg_opaq, void *payload, size_t size)
{
    pmix_message_inst_t *msg = (pmix_message_inst_t *)msg_opaq;
    msg->hdr_rcvd = 1;
    if( 0 < size ){
        msg->hdr.nbytes = size;
        msg->payload = payload;
    }else {
        msg->payload = NULL;
    }
    return PMIX_SUCCESS;
}

uint32_t PMIx_message_pay_size(pmix_message_t *msg_opaq)
{
    pmix_message_inst_t *msg = (pmix_message_inst_t *)msg_opaq;
    if( NULL == msg || !msg->hdr_rcvd ){
        return 0;
    }
    return msg->hdr.nbytes;
}

void *PMIx_message_pay_ptr(pmix_message_t *msg_opaq)
{
    pmix_message_inst_t *msg = (pmix_message_inst_t *)msg_opaq;
    if( NULL == msg || !msg->hdr_rcvd ){
        return NULL;
    }
    return msg->payload;
}

void PMIx_message_free(pmix_message_t *msg_opaq)
{
    pmix_message_inst_t *msg = (pmix_message_inst_t *)msg_opaq;
    if( NULL == msg ){
        return;
    }
    if( msg->hdr_rcvd && (NULL != msg->payload) ){
        free(msg->payload);
    }
    free(msg);
}


size_t PMIx_server_process_msg(int sd, pmix_message_t *msg,
                               pmix_message_t **reply_msg)
{
    pmix_usock_hdr_t *hdr = PMIx_message_hdr_ptr(msg);
    pmix_buffer_t buf;
    size_t ret = 0;
    pmix_peer_t peer;
    
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "PMIx_server_process_msg for %s:%d:%d", hdr->nspace, hdr->rank, sd);

    *reply_msg = NULL;

    /* Load payload into the buffer */
    OBJ_CONSTRUCT(&buf, pmix_buffer_t);
    PMIX_LOAD_BUFFER(&buf,PMIx_message_pay_ptr(msg),
                     PMIx_message_pay_size(msg));

    /* setup the peer object */
    OBJ_CONSTRUCT(&peer, pmix_peer_t);
    (void)strncpy(peer.nspace, hdr->nspace, PMIX_MAX_NSLEN);
    peer.rank = hdr->rank;
    server_switchyard(&peer, hdr->tag, &buf, reply_msg);
    OBJ_DESTRUCT(&peer);
    
    /* Free buffer protecting the data */
    buf.base_ptr = NULL;
    OBJ_DESTRUCT(&buf);

    return ret;
}

static void spawn_cbfunc(int status, char *nspace, void *cbdata)
{
    pmix_server_caddy_t *cd = (pmix_server_caddy_t*)cbdata;
    pmix_buffer_t *reply;
    int rc;

    /* setup the reply with the returned status */
    reply = OBJ_NEW(pmix_buffer_t);
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(reply, &status, 1, PMIX_INT))) {
        PMIX_ERROR_LOG(rc);
        OBJ_RELEASE(reply);
        return;
    }
    /* add the nspace */
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(reply, &nspace, 1, PMIX_STRING))) {
        PMIX_ERROR_LOG(rc);
        OBJ_RELEASE(reply);
        return;
    }
    /* tell the originator the result */
    /* cleanup */
    OBJ_RELEASE(cd);
}

static void lookup_cbfunc(int status, pmix_info_t info[], size_t ninfo,
                          char nspace[], void *cbdata)
{
}

static void modex_cbfunc(int status, pmix_modex_data_t data[],
                         size_t ndata, void *cbdata)
{
}

static void cnct_cbfunc(int status, void *cbdata)
{
    pmix_server_trkr_t *trk = (pmix_server_trkr_t*)cbdata;
    pmix_buffer_t *reply;
    int rc;

    /* setup the reply with the returned status */
    reply = OBJ_NEW(pmix_buffer_t);
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(reply, &status, 1, PMIX_INT))) {
        PMIX_ERROR_LOG(rc);
        OBJ_RELEASE(reply);
        return;
    }
    /* send it to everyone on the list */
    /* cleanup */
    OBJ_RELEASE(trk);
}


static void server_switchyard(pmix_peer_t *peer, uint32_t tag,
                              pmix_buffer_t *buf,
                              pmix_message_t **reply)
{
    int rc;
    int32_t cnt;
    pmix_cmd_t cmd;
    pmix_cb_t *cb;
    pmix_server_caddy_t *cd;
    
    /* setup default */
    *reply = NULL;
    
    /* retrieve the cmd */
    cnt = 1;
    if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, &cmd, &cnt, PMIX_CMD))) {
        PMIX_ERROR_LOG(rc);
        setup_op_response(rc, reply);
        return;
    }
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "recvd pmix cmd %d from %s:%d",
                        cmd, peer->nspace, peer->rank);

    if (PMIX_ABORT_CMD == cmd) {
        cb = OBJ_NEW(pmix_cb_t);
        cb->active = true;
        if (PMIX_SUCCESS != (rc = pmix_server_abort(buf, op_cbfunc, cb))) {
            PMIX_ERROR_LOG(rc);
            OBJ_RELEASE(cb);
            setup_op_response(rc, reply);
            return;
        }
        PMIX_WAIT_FOR_COMPLETION(cb->active);
        setup_op_response(cb->status, reply);
        OBJ_RELEASE(cb);
        return;
    }
        
    if (PMIX_FENCENB_CMD == cmd) {
        PMIX_PEER_CADDY(cd, peer, tag); 
        if (PMIX_SUCCESS != (rc = pmix_server_fence(cd, buf, modex_cbfunc, op_cbfunc))) {
            PMIX_ERROR_LOG(rc);
            setup_op_response(rc, reply);
        }
        OBJ_RELEASE(cd);
        return;
    }

    if (PMIX_GETNB_CMD == cmd) {
        cb = OBJ_NEW(pmix_cb_t);
        cb->active = true;
        if (PMIX_SUCCESS != (rc = pmix_server_get(buf, modex_cbfunc, cb))) {
            PMIX_ERROR_LOG(rc);
            OBJ_RELEASE(cb);
            setup_op_response(rc, reply);
            return;
        }
        PMIX_WAIT_FOR_COMPLETION(cb->active);
        setup_get_response(cb, reply);
        OBJ_RELEASE(cb);
        return;
    }
        
    if (PMIX_FINALIZE_CMD == cmd) {
        pmix_output_verbose(2, pmix_globals.debug_output,
                            "recvd FINALIZE");
        /* call the local server, if supported */
        rc = PMIX_ERR_NOT_SUPPORTED;
        if (NULL != pmix_host_server.terminated) {
            rc = pmix_host_server.terminated(peer->nspace, peer->rank);
        }
        setup_op_response(rc, reply);
        return;
    }

        
    if (PMIX_PUBLISHNB_CMD == cmd) {
        cb = OBJ_NEW(pmix_cb_t);
        cb->active = true;
        if (PMIX_SUCCESS != (rc = pmix_server_publish(buf, op_cbfunc, cb))) {
            PMIX_ERROR_LOG(rc);
            OBJ_RELEASE(cb);
            setup_op_response(rc, reply);
            return;
        }
        PMIX_WAIT_FOR_COMPLETION(cb->active);
        setup_op_response(cb->status, reply);
        OBJ_RELEASE(cb);
        return;
    }

    
    if (PMIX_LOOKUPNB_CMD == cmd) {
        cb = OBJ_NEW(pmix_cb_t);
        cb->active = true;
        if (PMIX_SUCCESS != (rc = pmix_server_lookup(buf, lookup_cbfunc, cb))) {
            PMIX_ERROR_LOG(rc);
            OBJ_RELEASE(cb);
            setup_op_response(rc, reply);
            return;
        }
        PMIX_WAIT_FOR_COMPLETION(cb->active);
        setup_lookup_response(cb, reply);
        OBJ_RELEASE(cb);
        return;
    }

        
    if (PMIX_UNPUBLISHNB_CMD == cmd) {
        cb = OBJ_NEW(pmix_cb_t);
        cb->active = true;
        if (PMIX_SUCCESS != (rc = pmix_server_unpublish(buf, op_cbfunc, cb))) {
            PMIX_ERROR_LOG(rc);
            OBJ_RELEASE(cb);
            setup_op_response(rc, reply);
            return;
        }
        PMIX_WAIT_FOR_COMPLETION(cb->active);
        setup_op_response(cb->status, reply);
        OBJ_RELEASE(cb);
        return;
    }

        
    if (PMIX_SPAWNNB_CMD == cmd) {
        cb = OBJ_NEW(pmix_cb_t);
        cb->active = true;
        if (PMIX_SUCCESS != (rc = pmix_server_spawn(buf, spawn_cbfunc, cb))) {
            PMIX_ERROR_LOG(rc);
            OBJ_RELEASE(cb);
            setup_op_response(rc, reply);
            return;
        }
        PMIX_WAIT_FOR_COMPLETION(cb->active);
        setup_spawn_response(cb, reply);
        OBJ_RELEASE(cb);
        return;
    }

        
    if (PMIX_CONNECTNB_CMD == cmd) {
        PMIX_PEER_CADDY(cd, peer, tag); 
        if (PMIX_SUCCESS != (rc = pmix_server_connect(cd, buf, false, cnct_cbfunc))) {
            PMIX_ERROR_LOG(rc);
            setup_op_response(rc, reply);
        }
        return;
    }

    if (PMIX_DISCONNECTNB_CMD == cmd) {
        PMIX_PEER_CADDY(cd, peer, tag); 
        if (PMIX_SUCCESS != (rc = pmix_server_connect(cd, buf, true, cnct_cbfunc))) {
            PMIX_ERROR_LOG(rc);
            setup_op_response(rc, reply);
        }
        return;
    }
}

static void op_cbfunc(int status, void *cbdata)
{
    pmix_cb_t *cb = (pmix_cb_t*)cbdata;

    cb->status = status;
    /* release the wait */
    cb->active = false;
}

static void setup_op_response(int status, pmix_message_t **reply)
{
    pmix_usock_hdr_t *rhdr = NULL;
    int rc;
    pmix_buffer_t buf;
    pmix_message_t *rmsg;
    
    /* pack the return status */
    OBJ_CONSTRUCT(&buf, pmix_buffer_t);
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(&buf, &status, 1, PMIX_INT))) {
        PMIX_ERROR_LOG(rc);
        OBJ_DESTRUCT(&buf);
        return;
    }
    
    /* Prepare the message */
    rmsg = PMIx_message_new();
    if( NULL == rmsg ){
        PMIX_ERROR_LOG(PMIX_ERR_OUT_OF_RESOURCE);
        OBJ_DESTRUCT(&buf);
        return;
    }
    rhdr = PMIx_message_hdr_ptr(rmsg);
    rhdr->rank = pmix_globals.rank;
    rhdr->type = PMIX_USOCK_USER;
    rhdr->tag = UINT_MAX;
    rhdr->nbytes = buf.bytes_used;
    rc = PMIx_message_set_payload((void*)rmsg, buf.base_ptr, buf.bytes_used);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(PMIX_ERR_OUT_OF_RESOURCE);
        /* release the buffer and it's content */
        OBJ_DESTRUCT(&buf);
        return;
    } else {
        /* release the buffer but retain the content */
        buf.base_ptr = NULL;
        OBJ_DESTRUCT(&buf);
    }
    *reply = rmsg;
}

static void setup_get_response(pmix_cb_t *cb, pmix_message_t **reply)
{
    pmix_usock_hdr_t *rhdr = NULL;
    int rc;
    pmix_buffer_t buf;
    pmix_message_t *rmsg;
    
    /* pack the return status */
    OBJ_CONSTRUCT(&buf, pmix_buffer_t);
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(&buf, &cb->status, 1, PMIX_INT))) {
        PMIX_ERROR_LOG(rc);
        OBJ_DESTRUCT(&buf);
        return;
    }
    /* pack the returned value */
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(&buf, cb->value, 1, PMIX_VALUE))) {
        PMIX_ERROR_LOG(rc);
        OBJ_DESTRUCT(&buf);
        return;
    }
   
    /* Prepare the message */
    rmsg = PMIx_message_new();
    if( NULL == rmsg ){
        PMIX_ERROR_LOG(PMIX_ERR_OUT_OF_RESOURCE);
        OBJ_DESTRUCT(&buf);
        return;
    }
    rhdr = PMIx_message_hdr_ptr(rmsg);
    rhdr->rank = pmix_globals.rank;
    rhdr->type = PMIX_USOCK_USER;
    rhdr->tag = UINT_MAX;
    rhdr->nbytes = buf.bytes_used;
    rc = PMIx_message_set_payload((void*)rmsg, buf.base_ptr, buf.bytes_used);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(PMIX_ERR_OUT_OF_RESOURCE);
        /* release the buffer and it's content */
        OBJ_DESTRUCT(&buf);
        return;
    } else {
        /* release the buffer but retain the content */
        buf.base_ptr = NULL;
        OBJ_DESTRUCT(&buf);
    }
    *reply = rmsg;
}

static void setup_lookup_response(pmix_cb_t *cb, pmix_message_t **reply)
{
    pmix_usock_hdr_t *rhdr = NULL;
    int rc;
    pmix_buffer_t buf;
    pmix_message_t *rmsg;
    
    /* pack the return status */
    OBJ_CONSTRUCT(&buf, pmix_buffer_t);
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(&buf, &cb->status, 1, PMIX_INT))) {
        PMIX_ERROR_LOG(rc);
        OBJ_DESTRUCT(&buf);
        return;
    }
    
    /* Prepare the message */
    rmsg = PMIx_message_new();
    if( NULL == rmsg ){
        PMIX_ERROR_LOG(PMIX_ERR_OUT_OF_RESOURCE);
        OBJ_DESTRUCT(&buf);
        return;
    }
    rhdr = PMIx_message_hdr_ptr(rmsg);
    rhdr->rank = pmix_globals.rank;
    rhdr->type = PMIX_USOCK_USER;
    rhdr->tag = UINT_MAX;
    rhdr->nbytes = buf.bytes_used;
    rc = PMIx_message_set_payload((void*)rmsg, buf.base_ptr, buf.bytes_used);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(PMIX_ERR_OUT_OF_RESOURCE);
        /* release the buffer and it's content */
        OBJ_DESTRUCT(&buf);
        return;
    } else {
        /* release the buffer but retain the content */
        buf.base_ptr = NULL;
        OBJ_DESTRUCT(&buf);
    }
    *reply = rmsg;
}

static void setup_spawn_response(pmix_cb_t *cb, pmix_message_t **reply)
{
    pmix_usock_hdr_t *rhdr = NULL;
    int rc;
    pmix_buffer_t buf;
    pmix_message_t *rmsg;
    
    /* pack the return status */
    OBJ_CONSTRUCT(&buf, pmix_buffer_t);
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(&buf, &cb->status, 1, PMIX_INT))) {
        PMIX_ERROR_LOG(rc);
        OBJ_DESTRUCT(&buf);
        return;
    }
    
    /* Prepare the message */
    rmsg = PMIx_message_new();
    if( NULL == rmsg ){
        PMIX_ERROR_LOG(PMIX_ERR_OUT_OF_RESOURCE);
        OBJ_DESTRUCT(&buf);
        return;
    }
    rhdr = PMIx_message_hdr_ptr(rmsg);
    rhdr->rank = pmix_globals.rank;
    rhdr->type = PMIX_USOCK_USER;
    rhdr->tag = UINT_MAX;
    rhdr->nbytes = buf.bytes_used;
    rc = PMIx_message_set_payload((void*)rmsg, buf.base_ptr, buf.bytes_used);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(PMIX_ERR_OUT_OF_RESOURCE);
        /* release the buffer and it's content */
        OBJ_DESTRUCT(&buf);
        return;
    } else {
        /* release the buffer but retain the content */
        buf.base_ptr = NULL;
        OBJ_DESTRUCT(&buf);
    }
    *reply = rmsg;
}

