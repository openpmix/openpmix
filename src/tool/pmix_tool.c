/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2014-2016 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014-2016 Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2014      Artem Y. Polyakov <artpol84@gmail.com>.
 *                         All rights reserved.
 * Copyright (c) 2016      Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2016      IBM Corporation.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include <src/include/pmix_config.h>

#include <src/include/types.h>
#include "pmix_stdint.h"
#include <src/include/pmix_socket_errno.h>

#include "src/client/pmix_client_ops.h"
#include <pmix_tool.h>

#include "src/include/pmix_globals.h"

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
#include PMIX_EVENT_HEADER

#if PMIX_CC_USE_PRAGMA_IDENT
#pragma ident PMIX_VERSION
#elif PMIX_CC_USE_IDENT
#ident PMIX_VERSION
#endif
static const char pmix_version_string[] = PMIX_VERSION;

extern pmix_client_globals_t pmix_client_globals;

#include "src/class/pmix_list.h"
#include "src/buffer_ops/buffer_ops.h"
#include "src/util/argv.h"
#include "src/util/error.h"
#include "src/util/hash.h"
#include "src/util/output.h"
#include "src/util/progress_threads.h"
#include "src/usock/usock.h"
#include "src/sec/pmix_sec.h"
#include "src/include/pmix_globals.h"
#if defined(PMIX_ENABLE_DSTORE) && (PMIX_ENABLE_DSTORE == 1)
#include "src/dstore/pmix_dstore.h"
#endif /* PMIX_ENABLE_DSTORE */

#define PMIX_MAX_RETRIES 10

static pmix_status_t usock_connect(struct sockaddr_un *address, int *fd);
 static void myerrhandler(pmix_status_t status,
                          pmix_proc_t procs[], size_t nprocs,
                          pmix_info_t info[], size_t ninfo)
 {
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix:client default errhandler activated");
}

static void pmix_client_notify_recv(struct pmix_peer_t *peer, pmix_usock_hdr_t *hdr,
                                    pmix_buffer_t *buf, void *cbdata)
{
    pmix_status_t pstatus, status, rc;
    int32_t cnt;
    pmix_proc_t *procs=NULL;
    size_t nprocs, ninfo;
    pmix_info_t *info=NULL;
    pmix_cmd_t cmd;
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix:client_notify_recv - processing error");
    if (0 == pmix_pointer_array_get_size(&pmix_globals.errregs)) {
        return;
    }
    cnt=1;
    if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, &cmd, &cnt, PMIX_CMD))) {
        PMIX_ERROR_LOG(rc);
        return;
    }
    /* unpack the status */
    cnt=1;
    if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, &status, &cnt, PMIX_INT))) {
        PMIX_ERROR_LOG(rc);
        return;
    }
    pstatus = status;

    /* unpack the procs that are impacted */
    cnt=1;
    if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, &nprocs, &cnt, PMIX_SIZE))) {
        PMIX_ERROR_LOG(rc);
        goto error;
    }
    if (0 < nprocs) {
        PMIX_PROC_CREATE(procs, nprocs);
        cnt = nprocs;
        if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, procs, &cnt, PMIX_PROC))) {
            PMIX_ERROR_LOG(rc);
            goto error;
        }
    }

    /* unpack the info that might have been provided */
    cnt=1;
    if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, &ninfo, &cnt, PMIX_SIZE))) {
        PMIX_ERROR_LOG(rc);
        goto error;
    }
    if (0 < ninfo) {
        PMIX_INFO_CREATE(info, ninfo);
        cnt = ninfo;
        if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, info, &cnt, PMIX_INFO))) {
            PMIX_ERROR_LOG(rc);
            goto error;
        }
    }
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix:client_notify_recv - processed error %d, calling errhandler", pstatus);
    pmix_errhandler_invoke(pstatus, procs, nprocs, info, ninfo);

    /* cleanup */
    PMIX_PROC_FREE(procs, nprocs);
    PMIX_INFO_FREE(info, ninfo);
    return;

    error:
    /* we always need to return */
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix:client_notify_recv - pack error status =%d, calling def errhandler", rc);
    pmix_errhandler_invoke(rc, NULL, 0, NULL, 0);
    PMIX_PROC_FREE(procs, nprocs);
    PMIX_INFO_FREE(info, ninfo);
}



/* callback for wait completion */
static void wait_cbfunc(struct pmix_peer_t *pr, pmix_usock_hdr_t *hdr,
                        pmix_buffer_t *buf, void *cbdata)
{
    pmix_cb_t *cb = (pmix_cb_t*)cbdata;

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix:client recv callback activated with %d bytes",
                        (NULL == buf) ? -1 : (int)buf->bytes_used);

    cb->active = false;
}

extern void pmix_client_process_nspace_blob(const char *nspace, pmix_buffer_t *bptr);

/* callback to receive job info */
static void job_data(struct pmix_peer_t *pr, pmix_usock_hdr_t *hdr,
                     pmix_buffer_t *buf, void *cbdata)
{
    pmix_status_t rc;
    char *nspace;
    int32_t cnt = 1;
    pmix_cb_t *cb = (pmix_cb_t*)cbdata;
    
    /* unpack the nspace - we don't really need it, but have to
     * unpack it to maintain sequence */
    if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, &nspace, &cnt, PMIX_STRING))) {
        PMIX_ERROR_LOG(rc);
        return;
    }
    /* decode it */
    pmix_client_process_nspace_blob(pmix_globals.myid.nspace, buf);
    cb->status = PMIX_SUCCESS;
    cb->active = false;
}

static pmix_status_t connect_to_server(struct sockaddr_un *address, void *cbdata)
{
    int sd;
    pmix_status_t ret;
    pmix_cmd_t cmd = PMIX_REQ_CMD;
    pmix_buffer_t *req;

    if (PMIX_SUCCESS != (ret=usock_connect(address, &sd))) {
        PMIX_ERROR_LOG(ret);
        return ret;
    }
    pmix_client_globals.myserver.sd = sd;
    /* setup recv event */
   event_assign(&pmix_client_globals.myserver.recv_event,
                 pmix_globals.evbase,
                 pmix_client_globals.myserver.sd,
                 EV_READ | EV_PERSIST,
                 pmix_usock_recv_handler, &pmix_client_globals.myserver);
    event_add(&pmix_client_globals.myserver.recv_event, 0);
    pmix_client_globals.myserver.recv_ev_active = true;

    /* setup send event */
    event_assign(&pmix_client_globals.myserver.send_event,
                 pmix_globals.evbase,
                 pmix_client_globals.myserver.sd,
                 EV_WRITE|EV_PERSIST,
                 pmix_usock_send_handler, &pmix_client_globals.myserver);
    pmix_client_globals.myserver.send_ev_active = false;

    /* send a request for our job info - we do this as a non-blocking
     * transaction because some systems cannot handle very large
     * blocking operations and error out if we try them. */
     req = PMIX_NEW(pmix_buffer_t);
     if (PMIX_SUCCESS != (ret = pmix_bfrop.pack(req, &cmd, 1, PMIX_CMD))) {
        PMIX_ERROR_LOG(ret);
        PMIX_RELEASE(req);
        return ret;
    }
    PMIX_ACTIVATE_SEND_RECV(&pmix_client_globals.myserver, req, job_data, cbdata);
    return PMIX_SUCCESS;
}

PMIX_EXPORT int PMIx_tool_init(pmix_proc_t *proc,
                               pmix_info_t info[], size_t ninfo)
{
    char *evar;
    int rc, debug_level;
    struct sockaddr_un address;
    pmix_nspace_t *nsptr;
    pmix_cb_t cb;
    int errhandler_ref;
    size_t n;

    if (NULL == proc) {
        return PMIX_ERR_BAD_PARAM;
    }

    if (0 < pmix_globals.init_cntr) {
        /* since we have been called before, the nspace and
         * rank should be known. So return them here if
         * requested */
         if (NULL != proc) {
            (void)strncpy(proc->nspace, pmix_globals.myid.nspace, PMIX_MAX_NSLEN);
            proc->rank = pmix_globals.myid.rank;
        }
        ++pmix_globals.init_cntr;
        return PMIX_SUCCESS;
    }

    /* scan incoming info for directives */
    if (NULL != info) {
        for (n=0; n < ninfo; n++) {
            if (0 == strcmp(PMIX_EVENT_BASE, info[n].key)) {
                pmix_globals.evbase = (pmix_event_base_t*)info[n].value.data.ptr;
                pmix_globals.external_evbase = true;
            }
        }
    }
    /* setup the globals */
    pmix_globals_init();
    PMIX_CONSTRUCT(&pmix_client_globals.pending_requests, pmix_list_t);
    PMIX_CONSTRUCT(&pmix_client_globals.myserver, pmix_peer_t);
    /* mark that we are a client */
    pmix_globals.server = false;
    /* get our effective id's */
    pmix_globals.uid = geteuid();
    pmix_globals.gid = getegid();
    /* initialize the output system */
    if (!pmix_output_init()) {
        return PMIX_ERROR;
    }

    /* default to our internal errhandler */
    errhandler_ref = 0;
    pmix_add_errhandler(myerrhandler, NULL, 0, &errhandler_ref);
    /* see if debug is requested */
    if (NULL != (evar = getenv("PMIX_DEBUG"))) {
        debug_level = strtol(evar, NULL, 10);
        pmix_globals.debug_output = pmix_output_open(NULL);
        pmix_output_set_verbosity(pmix_globals.debug_output, debug_level);
    }

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix: init called");

    nsptr = PMIX_NEW(pmix_nspace_t);

     /* Get the local hostname, and look for a file named
     * /tmp/pmix.hostname.tool - this file will contain
     * the URI where the server is listening. The URI consists
     * of 3 parts - the code below will parse the string read
     * from the file and connect accordingly */
 
    int i, server_pid = -1;
    for(i = 0; i < (int)ninfo; i++) {
      if(strcmp(info[i].key, PMIX_SERVER_PIDINFO) == 0) {
        server_pid = info[i].value.data.integer;
      }
    }

    if(server_pid == -1) {
      return PMIX_ERR_NOT_FOUND;
    }

    /* setup the path to the daemon rendezvous point */
    memset(&address, 0, sizeof(struct sockaddr_un));
    address.sun_family = AF_UNIX;
    
    /* Get first 10 char's of hostname to match what the server is doing */
    int hostnamelen = 10;
    char hostname[hostnamelen];
    gethostname(hostname, hostnamelen);

    snprintf(address.sun_path, sizeof(address.sun_path)-1, "/tmp/pmix.%s.%d", hostname, server_pid);

    /* if the rendezvous file doesn't exist, that's an error */
    if (0 != access(address.sun_path, R_OK)) {
        pmix_output_close(pmix_globals.debug_output);
        pmix_output_finalize();
        pmix_class_finalize();
        return PMIX_ERR_NOT_FOUND;
    }

    pmix_bfrop_open();
    pmix_usock_init(pmix_client_notify_recv);
    pmix_sec_init();

    if (!pmix_globals.external_evbase) {
        /* create an event base and progress thread for us */
        if (NULL == (pmix_globals.evbase = pmix_start_progress_thread())) {
            pmix_sec_finalize();
            pmix_usock_finalize();
            pmix_bfrop_close();
            pmix_output_close(pmix_globals.debug_output);
            pmix_output_finalize();
            pmix_class_finalize();
            return -1;

        }
    }

    /* setup an object to track server connection */
    PMIX_CONSTRUCT(&cb, pmix_cb_t);
    cb.active = true;
    /* connect to the server */
    if (PMIX_SUCCESS != (rc = connect_to_server(&address, &cb))){
        PMIX_DESTRUCT(&cb);
        pmix_stop_progress_thread(pmix_globals.evbase);
        pmix_sec_finalize();
        pmix_usock_finalize();
        pmix_bfrop_close();
        pmix_output_close(pmix_globals.debug_output);
        pmix_output_finalize();
        pmix_class_finalize();
        return rc;
    }
    PMIX_WAIT_FOR_COMPLETION(cb.active);
    rc = cb.status;
    PMIX_DESTRUCT(&cb);
     
    if (PMIX_SUCCESS == rc) {
        pmix_globals.init_cntr++;
    }
    
    /* Success, so copy the nspace and rank */
    (void)strncpy(proc->nspace, pmix_globals.myid.nspace, PMIX_MAX_NSLEN);
    proc->rank = pmix_globals.myid.rank;
   
    return rc;
}

PMIX_EXPORT pmix_status_t PMIx_tool_finalize(void)
{
    pmix_buffer_t *msg;
    pmix_cb_t *cb;
    pmix_cmd_t cmd = PMIX_FINALIZE_CMD;
    pmix_status_t rc;

    if (1 != pmix_globals.init_cntr) {
        --pmix_globals.init_cntr;
        return PMIX_SUCCESS;
    }
    pmix_globals.init_cntr = 0;

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix:client finalize called");

    if ( 0 <= pmix_client_globals.myserver.sd ) {
        /* setup a cmd message to notify the PMIx
         * server that we are normally terminating */
        msg = PMIX_NEW(pmix_buffer_t);
        /* pack the cmd */
        if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(msg, &cmd, 1, PMIX_CMD))) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(msg);
            return rc;
        }

        /* create a callback object as we need to pass it to the
         * recv routine so we know which callback to use when
         * the return message is recvd */
         cb = PMIX_NEW(pmix_cb_t);
         cb->active = true;

         pmix_output_verbose(2, pmix_globals.debug_output,
                             "pmix:client sending finalize sync to server");

        /* push the message into our event base to send to the server */
         PMIX_ACTIVATE_SEND_RECV(&pmix_client_globals.myserver, msg, wait_cbfunc, cb);

        /* wait for the ack to return */
         PMIX_WAIT_FOR_COMPLETION(cb->active);
         PMIX_RELEASE(cb);
         pmix_output_verbose(2, pmix_globals.debug_output,
                             "pmix:client finalize sync received");
     }

     if (!pmix_globals.external_evbase) {
         pmix_stop_progress_thread(pmix_globals.evbase);
     }

     pmix_usock_finalize();
     PMIX_DESTRUCT(&pmix_client_globals.myserver);
     PMIX_LIST_DESTRUCT(&pmix_client_globals.pending_requests);

     if (0 <= pmix_client_globals.myserver.sd) {
        CLOSE_THE_SOCKET(pmix_client_globals.myserver.sd);
    }
    event_base_free(pmix_globals.evbase);
#ifdef HAVE_LIBEVENT_GLOBAL_SHUTDOWN
    libevent_global_shutdown();
#endif
    pmix_bfrop_close();
    pmix_sec_finalize();

    pmix_globals_finalize();

    pmix_output_close(pmix_globals.debug_output);
    pmix_output_finalize();
    pmix_class_finalize();

    return PMIX_SUCCESS;
}

/*
 * The sections below need to be updated to reflect tool
 * connection handshake protocols - in this case, we
 * don't know our nspace/rank in advance. So we need
 * the handshake to include the security credential
 * exchange, and then get our nspace/rank in return */

static pmix_status_t send_connect_ack(int sd)
{
    char *msg;
    pmix_usock_hdr_t hdr;
    size_t sdsize=0, csize=0;
    char *cred = NULL;

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix: TOOL SEND CONNECT ACK");

    /* setup the header */
    memset(&hdr, 0, sizeof(pmix_usock_hdr_t));
    hdr.pindex = -1;
    hdr.tag = UINT32_MAX;

    /* reserve space for the nspace and rank info */
    sdsize = strlen(pmix_globals.myid.nspace) + 1 + sizeof(int);

    /* get a credential, if the security system provides one. Not
     * every SPC will do so, thus we must first check */
    if (NULL != pmix_sec.create_cred) {
        if (NULL == (cred = pmix_sec.create_cred())) {
            /* an error occurred - we cannot continue */
            return PMIX_ERR_INVALID_CRED;
        }
        csize = strlen(cred) + 1;  // must NULL terminate the string!
    }
    /* set the number of bytes to be read beyond the header */
    hdr.nbytes = sdsize + strlen(PMIX_VERSION) + 1 + csize;  // must NULL terminate the VERSION string!

    /* create a space for our message */
    sdsize = (sizeof(hdr) + hdr.nbytes);
    if (NULL == (msg = (char*)malloc(sdsize))) {
        if (NULL != cred) {
            free(cred);
        }
        return PMIX_ERR_OUT_OF_RESOURCE;
    }
    memset(msg, 0, sdsize);
    
    csize=0;
    memcpy(msg, &hdr, sizeof(pmix_usock_hdr_t));
    csize += sizeof(pmix_usock_hdr_t);
    
    /* load the message */
    memcpy(msg+csize, PMIX_VERSION, strlen(PMIX_VERSION));
    csize += strlen(PMIX_VERSION)+1;
    if (NULL != cred) {
        memcpy(msg+csize, cred, strlen(cred));  // leaves last position in msg set to NULL
    }

  if (PMIX_SUCCESS != pmix_usock_send_blocking(sd, msg, sdsize)) {
      free(msg);
      if (NULL != cred) {
          free(cred);
      }
      return PMIX_ERR_UNREACH;
  }
  
  free(msg);
  if (NULL != cred) {
      free(cred);
  }
  
  return PMIX_SUCCESS;
}


/* we receive a connection acknowledgement from the server,
 * consisting of nothing more than a status report. If success,
 * then we initiate authentication method */
static pmix_status_t recv_connect_ack(int sd)
{
    pmix_status_t reply;
    pmix_status_t rc;
    struct timeval tv, save;
    pmix_socklen_t sz;
    bool sockopt = true;
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix: RECV CONNECT ACK FROM SERVER");
    char nspace[PMIX_MAX_NSLEN+1];

    /* get the current timeout value so we can reset to it */
    sz = sizeof(save);
    if (0 != getsockopt(sd, SOL_SOCKET, SO_RCVTIMEO, (void*)&save, &sz)) {
        if (ENOPROTOOPT == errno) {
            sockopt = false;
        } else {
           return PMIX_ERR_UNREACH;
       }
    } else {
        /* set a timeout on the blocking recv so we don't hang */
        tv.tv_sec  = 2;
        tv.tv_usec = 0;
        if (0 != setsockopt(sd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv))) {
            pmix_output_verbose(2, pmix_globals.debug_output,
                                "pmix: recv_connect_ack could not setsockopt SO_RCVTIMEO");
            return PMIX_ERR_UNREACH;
        }
    }
    
   /* Read servers nspace and rank (these are not real values, but they give some
      record of who the server is */
 
    pmix_usock_recv_blocking(sd, (char*)&reply, sizeof(pmix_status_t));
    if (PMIX_SUCCESS != reply) {
        PMIX_ERROR_LOG(reply);
        return reply;
    }

    pmix_client_globals.myserver.info = PMIX_NEW(pmix_rank_info_t);
    pmix_client_globals.myserver.info->nptr = PMIX_NEW(pmix_nspace_t);
    
    pmix_usock_recv_blocking(sd, (char *)pmix_globals.myid.nspace,  PMIX_MAX_NSLEN+1);
    pmix_usock_recv_blocking(sd, (char *) &pmix_globals.myid.rank, sizeof(pmix_globals.myid.rank));

    /* see if they want us to do the handshake */
    if (PMIX_ERR_READY_FOR_HANDSHAKE == reply) {
        if (NULL == pmix_sec.client_handshake) {
            return PMIX_ERR_HANDSHAKE_FAILED;
        }
        if (PMIX_SUCCESS != (rc = pmix_sec.client_handshake(sd))) {
            return rc;
        }
    } else if (PMIX_SUCCESS != reply) {
        return reply;
    }

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix: RECV CONNECT CONFIRMATION");
    if (sockopt) {
        if (0 != setsockopt(sd, SOL_SOCKET, SO_RCVTIMEO, &save, sz)) {
            return PMIX_ERR_UNREACH;
        }
    }

    return PMIX_SUCCESS;
}

static pmix_status_t usock_connect(struct sockaddr_un *addr, int *fd)
{
    int sd=-1;
    pmix_status_t rc;
    pmix_socklen_t addrlen = 0;
    int retries = 0;

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "usock_peer_try_connect: attempting to connect to server");

    addrlen = sizeof(struct sockaddr_un);
    while (retries < PMIX_MAX_RETRIES) {
        retries++;
        /* Create the new socket */
        sd = socket(PF_UNIX, SOCK_STREAM, 0);
        if (sd < 0) {
            pmix_output(0, "pmix:create_socket: socket() failed: %s (%d)\n",
                        strerror(pmix_socket_errno),
                        pmix_socket_errno);
            continue;
        }
        pmix_output_verbose(2, pmix_globals.debug_output,
                            "usock_peer_try_connect: attempting to connect to server on socket %d", sd);
        /* try to connect */
        int err = -1;
	if ((err = connect(sd, addr, addrlen)) < 0) {
            if (pmix_socket_errno == ETIMEDOUT) {
                /* The server may be too busy to accept new connections */
                pmix_output_verbose(2, pmix_globals.debug_output,
                                    "timeout connecting to server");
                CLOSE_THE_SOCKET(sd);
                continue;
            }

            /* Some kernels (Linux 2.6) will automatically software
               abort a connection that was ECONNREFUSED on the last
               attempt, without even trying to establish the
               connection.  Handle that case in a semi-rational
               way by trying twice before giving up */
               else if (ECONNABORTED == pmix_socket_errno) {
                pmix_output_verbose(2, pmix_globals.debug_output,
                                    "connection to server aborted by OS - retrying");
                CLOSE_THE_SOCKET(sd);
                continue;
            }
	    else {
              pmix_output_verbose(2, "Failed to connect, errno = %d, err= %s\n", errno, strerror(errno));
	      continue;
	    }
	}
        /* otherwise, the connect succeeded - so break out of the loop */
        break;
    }

    if (retries == PMIX_MAX_RETRIES || sd < 0){
        /* We were unsuccessful in establishing this connection, and are
         * not likely to suddenly become successful */
        if (0 <= sd) {
            CLOSE_THE_SOCKET(sd);
        }
        return PMIX_ERR_UNREACH;
    }

    /* send our identity and any authentication credentials to the server */
    if (PMIX_SUCCESS != (rc = send_connect_ack(sd))) {
        CLOSE_THE_SOCKET(sd);
        return rc;
    }

    /* do whatever handshake is required */
    if (PMIX_SUCCESS != (rc = recv_connect_ack(sd))) {
        CLOSE_THE_SOCKET(sd);
        return rc;
    }

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "sock_peer_try_connect: Connection across to server succeeded");

    /* mark the connection as made */
    pmix_globals.connected = true;

    pmix_usock_set_nonblocking(sd);

    *fd = sd;
    return PMIX_SUCCESS;
}
