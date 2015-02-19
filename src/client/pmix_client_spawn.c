/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2014      Intel, Inc.  All rights reserved.
 * Copyright (c) 2014      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2014      Artem Y. Polyakov <artpol84@gmail.com>.
 *                         All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "pmix_config.h"
#include "src/api/pmix.h"
#include "src/include/types.h"
#include "src/include/pmix_globals.h"
#include "pmix_stdint.h"
#include "pmix_socket_errno.h"

#ifdef HAVE_STRING_H
#include <string.h>
#endif
#include <fcntl.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif
#ifdef HAVE_SYS_UN_H
#include <sys/un.h>
#endif
#ifdef HAVE_SYS_UIO_H
#include <sys/uio.h>
#endif
#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#include <event.h>

#include "src/class/pmix_list.h"
#include "src/buffer_ops/buffer_ops.h"
#include "src/util/argv.h"
#include "src/util/error.h"
#include "src/util/output.h"
#include "src/util/progress_threads.h"
#include "src/usock/usock.h"
#include "src/sec/pmix_sec.h"

#include "pmix_client_hash.h"
#include "pmix_client_ops.h"

static void wait_cbfunc(int sd, pmix_usock_hdr_t *hdr,
                        pmix_buffer_t *buf, void *cbdata);
static void spawn_cbfunc(int status, char nspace[], void *cbdata);

int PMIx_Spawn(const pmix_app_t apps[],
               size_t napps,
               char nspace[])
{
    int rc;
    pmix_cb_t *cb;
    
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix: spawn called");

    /* create a callback object */
    cb = PMIX_NEW(pmix_cb_t);
    cb->active = true;
    
    if (PMIX_SUCCESS != (rc = PMIx_Spawn_nb(apps, napps, spawn_cbfunc, cb))) {
        PMIX_RELEASE(cb);
        return rc;
    }
    
    /* wait for the result */
    PMIX_WAIT_FOR_COMPLETION(cb->active);
    rc = cb->status;
    if (NULL != nspace) {
        (void)strncpy(nspace, cb->nspace, PMIX_MAX_NSLEN);
    }
    PMIX_RELEASE(cb);
    return rc;
}

int PMIx_Spawn_nb(const pmix_app_t apps[], size_t napps,
                  pmix_spawn_cbfunc_t cbfunc, void *cbdata)
{
    pmix_buffer_t *msg;
    pmix_cmd_t cmd = PMIX_SPAWNNB_CMD;
    int rc;
    pmix_cb_t *cb;
    
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix: spawn called");

    if (pmix_client_globals.init_cntr <= 0) {
        return PMIX_ERR_INIT;
    }

    msg = PMIX_NEW(pmix_buffer_t);
    /* pack the cmd */
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, &cmd, 1, PMIX_CMD))) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        return rc;
    }

    /* pack the apps */
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, &napps, 1, PMIX_SIZE))) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        return rc;
    }
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, apps, napps, PMIX_APP))) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(msg);
        return rc;
    }
    
    /* create a callback object as we need to pass it to the
     * recv routine so we know which callback to use when
     * the return message is recvd */
    cb = PMIX_NEW(pmix_cb_t);
    cb->active = true;

    /* push the message into our event base to send to the server */
    PMIX_ACTIVATE_SEND_RECV(&pmix_client_globals.myserver, msg, wait_cbfunc, cb);

    return PMIX_SUCCESS;
}

/* callback for wait completion */
static void wait_cbfunc(int sd, pmix_usock_hdr_t *hdr,
                        pmix_buffer_t *buf, void *cbdata)
{
    pmix_cb_t *cb = (pmix_cb_t*)cbdata;
    char nspace[PMIX_MAX_NSLEN];
    char *n2;
    int rc, ret;
    int32_t cnt;
    
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix:client recv callback activated with %d bytes",
                        (NULL == buf) ? -1 : (int)buf->bytes_used);

    /* unpack the returned status */
    cnt = 1;
    if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, &ret, &cnt, PMIX_INT))) {
        PMIX_ERROR_LOG(rc);
        ret = rc;
    }
    /* unpack the namespace */
    cnt = 1;
    if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, &n2, &cnt, PMIX_STRING))) {
        PMIX_ERROR_LOG(rc);
        ret = rc;
    }
    if (NULL != n2) {
        (void)strncpy(nspace, n2, PMIX_MAX_NSLEN);
        free(n2);
    }
    if (NULL != cb->spawn_cbfunc) {
        cb->spawn_cbfunc(ret, nspace, cb->cbdata);
    }
}

static void spawn_cbfunc(int status, char nspace[], void *cbdata)
{
    pmix_cb_t *cb = (pmix_cb_t*)cbdata;
    
    cb->status = status;
    if (NULL != nspace) {
        (void)strncpy(cb->nspace, nspace, PMIX_MAX_NSLEN);
    }
    cb->active = false;
}

