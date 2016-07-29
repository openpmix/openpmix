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
#ifdef HAVE_DIRENT_H
#include <dirent.h>
#endif  /* HAVE_DIRENT_H */

#include PMIX_EVENT_HEADER

#if PMIX_CC_USE_PRAGMA_IDENT
#pragma ident PMIX_VERSION
#elif PMIX_CC_USE_IDENT
#ident PMIX_VERSION
#endif

extern pmix_client_globals_t pmix_client_globals;

#include "src/class/pmix_list.h"
#include "src/mca/bfrops/bfrops.h"
#include "src/util/argv.h"
#include "src/util/error.h"
#include "src/util/hash.h"
#include "src/util/output.h"
#include "src/usock/usock.h"
#include "src/mca/psec/psec.h"
#include "src/include/pmix_globals.h"
#include "src/runtime/pmix_rte.h"
#if defined(PMIX_ENABLE_DSTORE) && (PMIX_ENABLE_DSTORE == 1)
#include "src/dstore/pmix_dstore.h"
#endif /* PMIX_ENABLE_DSTORE */

#define PMIX_MAX_RETRIES 10

static pmix_status_t usock_connect(struct sockaddr_un *address, int *fd);

static pmix_status_t connect_to_server(struct sockaddr_un *address)
{
    int sd;
    pmix_status_t ret;

    if (PMIX_SUCCESS != (ret = usock_connect(address, &sd))) {
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

    return PMIX_SUCCESS;
}

PMIX_EXPORT int PMIx_tool_init(pmix_proc_t *proc,
                               pmix_info_t info[], size_t ninfo)
{
    char *evar, *tdir, *tmp;
    struct sockaddr_un address;
    pmix_kval_t *kptr;
    pmix_status_t rc;
    pmix_nspace_t *nptr, *nsptr;
    int server_pid = -1;
    int hostnamelen = 30;
    char hostname[hostnamelen];
    DIR *cur_dirp = NULL;
    struct dirent * dir_entry;

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

    /* setup the runtime - this init's the globals,
     * opens and initializes the required frameworks */
    if (PMIX_SUCCESS != (rc = pmix_rte_init(PMIX_PROC_TOOL, info, ninfo))) {
        return rc;
    }
    /* get our best bfrop support */
    if (NULL == (pmix_globals.mypeer->comm.bfrops = pmix_bfrops_base_assign_module(NULL))) {
        PMIX_ERROR_LOG(PMIX_ERROR);
        return PMIX_ERROR;
    }

    /* setup the client globals */
    PMIX_CONSTRUCT(&pmix_client_globals.pending_requests, pmix_list_t);
    PMIX_CONSTRUCT(&pmix_client_globals.myserver, pmix_peer_t);

    /* find the temp dir */
    if (NULL == (tdir = getenv("TMPDIR"))) {
        if (NULL == (tdir = getenv("TEMP"))) {
            if (NULL == (tdir = getenv("TMP"))) {
                tdir = "/tmp";
            }
        }
    }

    /* setup the path to the daemon rendezvous point */
    memset(&address, 0, sizeof(struct sockaddr_un));
    address.sun_family = AF_UNIX;
    /* Get hostname to match what the server is doing */
    gethostname(hostname, hostnamelen);
    /* ensure it is NULL terminated */
    hostname[hostnamelen-1] = '\0';

    /* if they gave us a specific pid, then look for that
     * particular server - otherwise, see if there is only
     * one on this node and default to it */
    if (server_pid != -1) {
        snprintf(address.sun_path, sizeof(address.sun_path)-1, "%s/pmix.%s.%d", tdir, hostname, server_pid);
        /* if the rendezvous file doesn't exist, that's an error */
        if (0 != access(address.sun_path, R_OK)) {
            return PMIX_ERR_NOT_FOUND;
        }
    } else {
        /* open up the temp directory */
        if (NULL == (cur_dirp = opendir(tdir))) {
            return PMIX_ERR_NOT_FOUND;
        }
        /* search the entries for something that starts with pmix.hostname */
        if (0 > asprintf(&tmp, "pmix.%s", hostname)) {
            closedir(cur_dirp);
            return PMIX_ERR_NOMEM;
        }
        evar = NULL;
        while (NULL != (dir_entry = readdir(cur_dirp))) {
            if (0 == strncmp(dir_entry->d_name, tmp, strlen(tmp))) {
                /* found one - if more than one, then that's an error */
                if (NULL != evar) {
                    closedir(cur_dirp);
                    free(evar);
                    free(tmp);
                    return PMIX_ERR_INIT;
                }
                evar = strdup(dir_entry->d_name);
            }
        }
        free(tmp);
        closedir(cur_dirp);
        if (NULL == evar) {
            /* none found */
            return PMIX_ERR_INIT;
        }
        /* use the found one as our contact point */
        snprintf(address.sun_path, sizeof(address.sun_path)-1, "%s/%s", tdir, evar);
        free(evar);
    }

    /* connect to the server */
    if (PMIX_SUCCESS != (rc = connect_to_server(&address))) {
        return rc;
    }
    /* increment our init reference counter */
    pmix_globals.init_cntr++;

    /* Success, so copy the nspace and rank */
    (void)strncpy(proc->nspace, pmix_globals.myid.nspace, PMIX_MAX_NSLEN);
    proc->rank = pmix_globals.myid.rank;

    /* now finish the initialization by filling our local
     * datastore with typical job-related info. No point
     * in having the server generate these as we are
     * obviously a singleton, and so the values are well-known */
    nsptr = NULL;
    PMIX_LIST_FOREACH(nptr, &pmix_globals.nspaces, pmix_nspace_t) {
        if (0 == strncmp(pmix_globals.myid.nspace, nptr->nspace, PMIX_MAX_NSLEN)) {
            nsptr = nptr;
            break;
        }
    }
    if (NULL == nsptr) {
        /* should never happen */
        return PMIX_ERR_NOT_FOUND;
    }

    /* the jobid is just our nspace */
    kptr = PMIX_NEW(pmix_kval_t);
    kptr->key = strdup(PMIX_JOBID);
    PMIX_VALUE_CREATE(kptr->value, 1);
    kptr->value->type = PMIX_STRING;
    kptr->value->data.string = strdup(nsptr->nspace);
    if (PMIX_SUCCESS != (rc = pmix_hash_store(&nsptr->internal, pmix_globals.myid.rank, kptr))) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    PMIX_RELEASE(kptr); // maintain accounting

    /* our rank */
    kptr = PMIX_NEW(pmix_kval_t);
    kptr->key = strdup(PMIX_RANK);
    PMIX_VALUE_CREATE(kptr->value, 1);
    kptr->value->type = PMIX_INT;
    kptr->value->data.integer = 0;
    if (PMIX_SUCCESS != (rc = pmix_hash_store(&nsptr->internal, pmix_globals.myid.rank, kptr))) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    PMIX_RELEASE(kptr); // maintain accounting

    /* nproc offset */
    kptr = PMIX_NEW(pmix_kval_t);
    kptr->key = strdup(PMIX_NPROC_OFFSET);
    PMIX_VALUE_CREATE(kptr->value, 1);
    kptr->value->type = PMIX_UINT32;
    kptr->value->data.uint32 = 0;
    if (PMIX_SUCCESS != (rc = pmix_hash_store(&nsptr->internal, pmix_globals.myid.rank, kptr))) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    PMIX_RELEASE(kptr); // maintain accounting

    /* node size */
    kptr = PMIX_NEW(pmix_kval_t);
    kptr->key = strdup(PMIX_NODE_SIZE);
    PMIX_VALUE_CREATE(kptr->value, 1);
    kptr->value->type = PMIX_UINT32;
    kptr->value->data.uint32 = 1;
    if (PMIX_SUCCESS != (rc = pmix_hash_store(&nsptr->internal, pmix_globals.myid.rank, kptr))) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    PMIX_RELEASE(kptr); // maintain accounting

    /* local peers */
    kptr = PMIX_NEW(pmix_kval_t);
    kptr->key = strdup(PMIX_LOCAL_PEERS);
    PMIX_VALUE_CREATE(kptr->value, 1);
    kptr->value->type = PMIX_STRING;
    kptr->value->data.string = strdup("0");
    if (PMIX_SUCCESS != (rc = pmix_hash_store(&nsptr->internal, pmix_globals.myid.rank, kptr))) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    PMIX_RELEASE(kptr); // maintain accounting

    /* local leader */
    kptr = PMIX_NEW(pmix_kval_t);
    kptr->key = strdup(PMIX_LOCALLDR);
    PMIX_VALUE_CREATE(kptr->value, 1);
    kptr->value->type = PMIX_UINT32;
    kptr->value->data.uint32 = 0;
    if (PMIX_SUCCESS != (rc = pmix_hash_store(&nsptr->internal, pmix_globals.myid.rank, kptr))) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    PMIX_RELEASE(kptr); // maintain accounting

    /* universe size */
    kptr = PMIX_NEW(pmix_kval_t);
    kptr->key = strdup(PMIX_UNIV_SIZE);
    PMIX_VALUE_CREATE(kptr->value, 1);
    kptr->value->type = PMIX_UINT32;
    kptr->value->data.uint32 = 1;
    if (PMIX_SUCCESS != (rc = pmix_hash_store(&nsptr->internal, pmix_globals.myid.rank, kptr))) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    PMIX_RELEASE(kptr); // maintain accounting

    /* job size - we are our very own job, so we have no peers */
    kptr = PMIX_NEW(pmix_kval_t);
    kptr->key = strdup(PMIX_JOB_SIZE);
    PMIX_VALUE_CREATE(kptr->value, 1);
    kptr->value->type = PMIX_UINT32;
    kptr->value->data.uint32 = 1;
    if (PMIX_SUCCESS != (rc = pmix_hash_store(&nsptr->internal, pmix_globals.myid.rank, kptr))) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    PMIX_RELEASE(kptr); // maintain accounting

    /* local size - only us in our job */
    kptr = PMIX_NEW(pmix_kval_t);
    kptr->key = strdup(PMIX_LOCAL_SIZE);
    PMIX_VALUE_CREATE(kptr->value, 1);
    kptr->value->type = PMIX_UINT32;
    kptr->value->data.uint32 = 1;
    if (PMIX_SUCCESS != (rc = pmix_hash_store(&nsptr->internal, pmix_globals.myid.rank, kptr))) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    PMIX_RELEASE(kptr); // maintain accounting

    /* max procs - since we are a self-started tool, there is no
     * allocation within which we can grow ourselves */
    kptr = PMIX_NEW(pmix_kval_t);
    kptr->key = strdup(PMIX_MAX_PROCS);
    PMIX_VALUE_CREATE(kptr->value, 1);
    kptr->value->type = PMIX_UINT32;
    kptr->value->data.uint32 = 1;
    if (PMIX_SUCCESS != (rc = pmix_hash_store(&nsptr->internal, pmix_globals.myid.rank, kptr))) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    PMIX_RELEASE(kptr); // maintain accounting

    /* app number */
    kptr = PMIX_NEW(pmix_kval_t);
    kptr->key = strdup(PMIX_APPNUM);
    PMIX_VALUE_CREATE(kptr->value, 1);
    kptr->value->type = PMIX_UINT32;
    kptr->value->data.uint32 = 0;
    if (PMIX_SUCCESS != (rc = pmix_hash_store(&nsptr->internal, pmix_globals.myid.rank, kptr))) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    PMIX_RELEASE(kptr); // maintain accounting

    /* app leader */
    kptr = PMIX_NEW(pmix_kval_t);
    kptr->key = strdup(PMIX_APPLDR);
    PMIX_VALUE_CREATE(kptr->value, 1);
    kptr->value->type = PMIX_UINT32;
    kptr->value->data.uint32 = 0;
    if (PMIX_SUCCESS != (rc = pmix_hash_store(&nsptr->internal, pmix_globals.myid.rank, kptr))) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    PMIX_RELEASE(kptr); // maintain accounting

    /* app rank */
    kptr = PMIX_NEW(pmix_kval_t);
    kptr->key = strdup(PMIX_APP_RANK);
    PMIX_VALUE_CREATE(kptr->value, 1);
    kptr->value->type = PMIX_UINT32;
    kptr->value->data.uint32 = 0;
    if (PMIX_SUCCESS != (rc = pmix_hash_store(&nsptr->internal, pmix_globals.myid.rank, kptr))) {
        PMIX_ERROR_LOG(rc);
    }
    PMIX_RELEASE(kptr); // maintain accounting

    /* global rank */
    kptr = PMIX_NEW(pmix_kval_t);
    kptr->key = strdup(PMIX_GLOBAL_RANK);
    PMIX_VALUE_CREATE(kptr->value, 1);
    kptr->value->type = PMIX_UINT32;
    kptr->value->data.uint32 = 0;
    if (PMIX_SUCCESS != (rc = pmix_hash_store(&nsptr->internal, pmix_globals.myid.rank, kptr))) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    PMIX_RELEASE(kptr); // maintain accounting

    /* local rank - we are alone in our job */
    kptr = PMIX_NEW(pmix_kval_t);
    kptr->key = strdup(PMIX_LOCAL_RANK);
    PMIX_VALUE_CREATE(kptr->value, 1);
    kptr->value->type = PMIX_UINT32;
    kptr->value->data.uint32 = 0;
    if (PMIX_SUCCESS != (rc = pmix_hash_store(&nsptr->internal, pmix_globals.myid.rank, kptr))) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    PMIX_RELEASE(kptr); // maintain accounting

    /* we cannot know the node rank as we don't know what
     * other processes are executing on this node - so
     * we'll add that info to the server-tool handshake
     * and load it from there */

    /* hostname */
     gethostname(hostname, PMIX_MAX_NSLEN);
     kptr = PMIX_NEW(pmix_kval_t);
     kptr->key = strdup(PMIX_HOSTNAME);
     PMIX_VALUE_CREATE(kptr->value, 1);
     kptr->value->type = PMIX_STRING;
     kptr->value->data.string = strdup(hostname);
     if (PMIX_SUCCESS != (rc = pmix_hash_store(&nsptr->internal, pmix_globals.myid.rank, kptr))) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    PMIX_RELEASE(kptr); // maintain accounting

    /* we cannot know the RM's nodeid for this host, so
     * we'll add that info to the server-tool handshake
     * and load it from there */

    /* the nodemap is simply our hostname as there is no
     * regex to generate */
     kptr = PMIX_NEW(pmix_kval_t);
     kptr->key = strdup(PMIX_NODE_MAP);
     PMIX_VALUE_CREATE(kptr->value, 1);
     kptr->value->type = PMIX_STRING;
     kptr->value->data.string = strdup(hostname);
     if (PMIX_SUCCESS != (rc = pmix_hash_store(&nsptr->internal, pmix_globals.myid.rank, kptr))) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    PMIX_RELEASE(kptr); // maintain accounting

    /* likewise, the proc map is just our rank as we are
     * the only proc in this job */
    kptr = PMIX_NEW(pmix_kval_t);
    kptr->key = strdup(PMIX_PROC_MAP);
    PMIX_VALUE_CREATE(kptr->value, 1);
    kptr->value->type = PMIX_STRING;
    kptr->value->data.string = strdup("0");
    if (PMIX_SUCCESS != (rc = pmix_hash_store(&nsptr->internal, pmix_globals.myid.rank, kptr))) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    PMIX_RELEASE(kptr); // maintain accounting

    return rc;
}

PMIX_EXPORT pmix_status_t PMIx_tool_finalize(void)
{
    if (1 != pmix_globals.init_cntr) {
        --pmix_globals.init_cntr;
        return PMIX_SUCCESS;
    }
    pmix_globals.init_cntr = 0;

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix:tool finalize called");

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

    pmix_rte_finalize();

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
    char *bfrop, *sec;

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix: TOOL SEND CONNECT ACK");

    /* setup the header */
    memset(&hdr, 0, sizeof(pmix_usock_hdr_t));
    hdr.pindex = -1;
    hdr.tag = UINT32_MAX;

    /* get a credential, if the security system provides one. Not
     * every SPC will do so, thus we must first check */
    if (NULL != pmix_globals.mypeer->comm.sec->create_cred) {
        if (NULL == (cred = pmix_globals.mypeer->comm.sec->create_cred())) {
            /* an error occurred - we cannot continue */
            return PMIX_ERR_INVALID_CRED;
        }
        csize = strlen(cred) + 1;  // must NULL terminate the string!
    }

    /* add our active bfrops and sec module info, and what type
     * of buffers we are using */
    bfrop = pmix_globals.mypeer->comm.bfrops->name;
    sec = pmix_globals.mypeer->comm.sec->name;

    /* set the number of bytes to be read beyond the header - must
     * NULL terminate the strings and include space for buffer type */
    hdr.nbytes = strlen(PMIX_VERSION) + 1 +
                 strlen(bfrop) + 1 + strlen(sec) + 1 + sizeof(pmix_bfrop_buffer_type_t) + csize;

    /* create a space for our message */
    sdsize = (sizeof(hdr) + hdr.nbytes);
    if (NULL == (msg = (char*)malloc(sdsize))) {
        if (NULL != cred) {
            free(cred);
        }
        return PMIX_ERR_OUT_OF_RESOURCE;
    }
    memset(msg, 0, sdsize);

    /* load the message - put the credential at the end */
    csize=0;
    memcpy(msg, &hdr, sizeof(pmix_usock_hdr_t));
    csize += sizeof(pmix_usock_hdr_t);
    memcpy(msg+csize, PMIX_VERSION, strlen(PMIX_VERSION));
    csize += strlen(PMIX_VERSION)+1;
    memcpy(msg+csize, bfrop, strlen(bfrop));
    csize += strlen(bfrop)+1;
    memcpy(msg+csize, sec, strlen(sec));
    csize += strlen(sec)+1;
    memcpy(msg+csize, &pmix_globals.mypeer->comm.type, sizeof(pmix_bfrop_buffer_type_t));
    csize += sizeof(pmix_bfrop_buffer_type_t);
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
 * consisting of the status and (if success) the nspace assigned
 * to us */
static pmix_status_t recv_connect_ack(int sd)
{
    pmix_status_t reply;
    struct timeval tv, save;
    pmix_socklen_t sz;
    bool sockopt = true;
    pmix_nspace_t *nsptr;

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix: RECV CONNECT ACK FROM SERVER");

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

    /* get the returned status from the request for namespace */
    pmix_usock_recv_blocking(sd, (char*)&reply, sizeof(pmix_status_t));
    if (PMIX_SUCCESS != reply) {
        return reply;
    }

   /* get our assigned nspace */
    pmix_usock_recv_blocking(sd, pmix_globals.myid.nspace, PMIX_MAX_NSLEN+1);

    /* setup required bookkeeping */
    nsptr = PMIX_NEW(pmix_nspace_t);
    (void)strncpy(nsptr->nspace, pmix_globals.myid.nspace, PMIX_MAX_NSLEN);
    pmix_list_append(&pmix_globals.nspaces, &nsptr->super);
    /* our rank is always zero */
    pmix_globals.myid.rank = 0;

    /* get the server's nspace and rank so we can send to it */
    pmix_client_globals.myserver.info = PMIX_NEW(pmix_rank_info_t);
    pmix_client_globals.myserver.info->nptr = PMIX_NEW(pmix_nspace_t);
    pmix_usock_recv_blocking(sd, (char*)pmix_client_globals.myserver.info->nptr->nspace, PMIX_MAX_NSLEN+1);
    pmix_usock_recv_blocking(sd, (char*)&(pmix_client_globals.myserver.info->rank), sizeof(int));

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix: RECV CONNECT CONFIRMATION FOR TOOL %s:%d FROM SERVER %s:%d",
                        pmix_globals.myid.nspace, pmix_globals.myid.rank,
                        pmix_client_globals.myserver.info->nptr->nspace,
                        pmix_client_globals.myserver.info->rank);

    /* get the returned status from the security handshake */
    pmix_usock_recv_blocking(sd, (char*)&reply, sizeof(pmix_status_t));
    if (PMIX_SUCCESS != reply) {
        return reply;
    }

    /* see if they want us to do the handshake */
    if (PMIX_ERR_READY_FOR_HANDSHAKE == reply) {
        if (NULL == pmix_globals.mypeer->comm.sec->client_handshake) {
            return PMIX_ERR_HANDSHAKE_FAILED;
        }
        if (PMIX_SUCCESS != (reply = pmix_globals.mypeer->comm.sec->client_handshake(sd))) {
            return reply;
            }
    } else if (PMIX_SUCCESS != reply) {
        return reply;
    }

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
        if ((err = connect(sd, (struct sockaddr*)addr, addrlen)) < 0) {
            if (pmix_socket_errno == ETIMEDOUT) {
                /* The server may be too busy to accept new connections */
                pmix_output_verbose(2, pmix_globals.debug_output,
                                    "timeout connecting to server");
                CLOSE_THE_SOCKET(sd);
                continue;
            } else if (ECONNABORTED == pmix_socket_errno) {
                /* Some kernels (Linux 2.6) will automatically software
                  abort a connection that was ECONNREFUSED on the last
                  attempt, without even trying to establish the
                  connection.  Handle that case in a semi-rational
                  way by trying twice before giving up */
                pmix_output_verbose(2, pmix_globals.debug_output,
                                    "connection to server aborted by OS - retrying");
                CLOSE_THE_SOCKET(sd);
                continue;
            } else {
              pmix_output_verbose(2, pmix_globals.debug_output,
                                  "Failed to connect, errno = %d, err= %s\n", errno, strerror(errno));
              CLOSE_THE_SOCKET(sd);
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

    /* send any authentication credentials to the server */
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
