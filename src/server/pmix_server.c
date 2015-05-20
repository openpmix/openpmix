/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2014-2015 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2014-2015 Artem Y. Polyakov <artpol84@gmail.com>.
 *                         All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "pmix_config.h"
#include "src/api/pmix_server.h"
#include "src/include/types.h"
#include "src/include/pmix_globals.h"
#include "src/include/pmix_stdint.h"
#include "src/include/pmix_socket_errno.h"

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
#include <ctype.h>
#include <event.h>

#include "src/class/pmix_list.h"
#include "src/buffer_ops/buffer_ops.h"
#include "src/util/argv.h"
#include "src/util/error.h"
#include "src/util/output.h"
#include "src/util/pmix_environ.h"
#include "src/util/progress_threads.h"
#include "src/usock/usock.h"
#include "src/sec/pmix_sec.h"

#include "pmix_server_ops.h"

// local variables
static int init_cntr = 0;
static char *myuri = NULL;
static pmix_event_t listen_ev;
static bool listening = false;
static int mysocket = -1;
static struct sockaddr_un myaddress;
static bool using_internal_comm = false;
static char *security_mode = NULL;

// local functions
static int start_listening(struct sockaddr_un *address);
static void connection_handler(int incoming_sd, short flags, void* cbdata);
static void server_message_handler(struct pmix_peer_t *pr, pmix_usock_hdr_t *hdr,
                                   pmix_buffer_t *buf, void *cbdata);
// global variables
pmix_globals_t pmix_globals = {
    .evbase = NULL,
    .debug_output = -1,
    .errhandler = NULL,
    .server = true,
};
pmix_server_globals_t pmix_server_globals;

/* queue a message to be sent to one of our procs - must
 * provide the following params:
 *
 * p - the peer object of the process
 * t - tag to be sent to
 * b - buffer to be sent
 */
#define PMIX_SERVER_QUEUE_REPLY(p, t, b)                                \
    do {                                                                \
        pmix_usock_send_t *snd;                                         \
        pmix_output_verbose(2, pmix_globals.debug_output,               \
                            "[%s:%d] queue reply to %s:%d on tag %d",   \
                            __FILE__, __LINE__,                         \
                            (p)->info->nptr->nspace,                    \
                            (p)->info->rank, (t));                      \
        snd = PMIX_NEW(pmix_usock_send_t);                              \
        snd->hdr.pindex = pmix_globals.pindex;                          \
        snd->hdr.tag = (t);                                             \
        snd->hdr.nbytes = (b)->bytes_used;                              \
        snd->data = (b);                                                \
        /* always start with the header */                              \
        snd->sdptr = (char*)&snd->hdr;                                  \
        snd->sdbytes = sizeof(pmix_usock_hdr_t);                        \
                                                                        \
        /* if there is no message on-deck, put this one there */        \
        if (NULL == (p)->send_msg) {                                    \
            (p)->send_msg = snd;                                        \
        } else {                                                        \
            /* add it to the queue */                                   \
            pmix_list_append(&(p)->send_queue, &snd->super);            \
        }                                                               \
        /* ensure the send event is active */                           \
        if (!(p)->send_ev_active) {                                     \
            event_add(&(p)->send_event, 0);                             \
            (p)->send_ev_active = true;                                 \
        }                                                               \
    } while(0);

static pmix_status_t initialize_server_base(pmix_server_module_t *module)
{
    int debug_level;
    pid_t pid;
    char *tdir, *evar;

    /* initialize the output system */
    if (!pmix_output_init()) {
        return PMIX_ERR_INIT;
    }
    /* Zero globals */
    memset(&pmix_globals, 0, sizeof(pmix_globals));

    /* setup the globals */

    (void)strncpy(pmix_globals.nspace, "pmix-server", PMIX_MAX_NSLEN);
    pmix_globals.debug_output = -1;
    PMIX_CONSTRUCT(&pmix_server_globals.nspaces, pmix_list_t);
    PMIX_CONSTRUCT(&pmix_server_globals.clients, pmix_pointer_array_t);
    pmix_pointer_array_init(&pmix_server_globals.clients, 1, INT_MAX, 1);

    PMIX_CONSTRUCT(&pmix_server_globals.collectives, pmix_list_t);

    /* see if debug is requested */
    if (NULL != (evar = getenv("PMIX_DEBUG"))) {
        debug_level = strtol(evar, NULL, 10);
        pmix_globals.debug_output = pmix_output_open(NULL);
        pmix_output_set_verbosity(pmix_globals.debug_output, debug_level);
    }

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix:server init called");

    /* setup the function pointers */
    memset(&pmix_host_server, 0, sizeof(pmix_server_module_t));
    pmix_host_server = *module;

    /* initialize the datatype support */
    pmix_bfrop_open();

    /* init security */
    pmix_sec_init();
    security_mode = strdup(pmix_sec.name);
    
    /* setup the path to the daemon rendezvous point, using our
     * pid as the "rank" */
    pid = getpid();

    /* find the temp dir */
    if (NULL == (tdir = getenv("TMPDIR"))) {
        if (NULL == (tdir = getenv("TEMP"))) {
            if (NULL == (tdir = getenv("TMP"))) {
                tdir = "/tmp";
            }
        }
    }
    /* now set the address */
    memset(&myaddress, 0, sizeof(struct sockaddr_un));
    myaddress.sun_family = AF_UNIX;
    snprintf(myaddress.sun_path, sizeof(myaddress.sun_path)-1, "%s/pmix-%d", tdir, pid);
    asprintf(&myuri, "%lu:%s", (unsigned long)pid, myaddress.sun_path);


    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix:server constructed uri %s", myuri);
    return PMIX_SUCCESS;
}

const char* PMIx_Get_version(void)
{
    return pmix_version_string;
}

pmix_status_t PMIx_server_init(pmix_server_module_t *module,
                               int use_internal_comm)
{
    pmix_usock_posted_recv_t *req;
    int rc;
    
    ++init_cntr;
    if (1 < init_cntr) {
        return PMIX_SUCCESS;
    }

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix:server init called");

    /* save the comm flag for when we finalize */
    using_internal_comm = use_internal_comm;
    
    if (0 != (rc = initialize_server_base(module))) {
        return rc;
    }

    /* if the caller doesn't want us to use our own internal
     * messaging system, then we are done */
    if (!use_internal_comm) {
        return PMIX_SUCCESS;
    }
    
    /* and the usock system */
    pmix_usock_init(NULL);
    
    /* create an event base and progress thread for us */
    if (NULL == (pmix_globals.evbase = pmix_start_progress_thread())) {
        return PMIX_ERR_INIT;
    }

    /* setup the wildcard recv for inbound messages from clients */
    req = PMIX_NEW(pmix_usock_posted_recv_t);
    req->tag = UINT32_MAX;
    req->cbfunc = server_message_handler;
    /* add it to the end of the list of recvs */
    pmix_list_append(&pmix_usock_globals.posted_recvs, &req->super);

    /* start listening */
    if (0 != start_listening(&myaddress)) {
        PMIx_server_finalize();
        return PMIX_ERR_INIT;
    }

    return PMIX_SUCCESS;
}

pmix_status_t PMIx_get_rendezvous_address(struct sockaddr_un *address, char **path)
{
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix:server get rendezvous address");

    memcpy(address, &myaddress, sizeof(struct sockaddr_un));
    if (NULL == path) {
        return PMIX_SUCCESS;
    }
    /* return the URI itself */
    if (NULL != myuri) {
        *path = strdup(myuri);
    } else {
        *path = NULL;
    }
    return PMIX_SUCCESS;
}

static void cleanup_server_state(void)
{
    int i;
    pmix_peer_t *peer;

    for (i=0; i < pmix_server_globals.clients.size; i++) {
        if (NULL != (peer = (pmix_peer_t*)pmix_pointer_array_get_item(&pmix_server_globals.clients, i))) {
            PMIX_RELEASE(peer);
        }
    }
    PMIX_DESTRUCT(&pmix_server_globals.clients);
    PMIX_LIST_DESTRUCT(&pmix_server_globals.nspaces);
    PMIX_LIST_DESTRUCT(&pmix_server_globals.collectives);

    if (NULL != myuri) {
        free(myuri);
    }

    pmix_bfrop_close();
    pmix_sec_finalize();
    
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix:server finalize complete");

    pmix_output_close(pmix_globals.debug_output);
    pmix_output_finalize();
    pmix_class_finalize();
}

pmix_status_t PMIx_server_finalize(void)
{
    if (1 != init_cntr) {
        --init_cntr;
        return PMIX_SUCCESS;
    }
    init_cntr = 0;

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix:server finalize called");
    
    if (using_internal_comm) {
        if (listening) {
            event_del(&listen_ev);
        }
    
        pmix_stop_progress_thread(pmix_globals.evbase);
        event_base_free(pmix_globals.evbase);
#ifdef HAVE_LIBEVENT_SHUTDOWN
        libevent_global_shutdown();
#endif
        
        if (0 <= mysocket) {
            CLOSE_THE_SOCKET(mysocket);
        }

        pmix_usock_finalize();

        /* cleanup the rendezvous file */
        unlink(myaddress.sun_path);
    }
    
    cleanup_server_state();
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix:server finalize complete");
    return PMIX_SUCCESS;
}

static void _register_nspace(int sd, short args, void *cbdata)
{
    pmix_setup_caddy_t *cd = (pmix_setup_caddy_t*)cbdata;
    pmix_nspace_t *nptr, *tmp;
    pmix_status_t rc;
    size_t i, j, size, rank;
    pmix_kval_t kv;
    char **nodes=NULL, **procs=NULL;
    pmix_buffer_t buf2;
    pmix_info_t *iptr;
    pmix_value_t val;
    char *msg;
    
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix:server _register_nspace");
    
    /* see if we already have this nspace */
    nptr = NULL;
    PMIX_LIST_FOREACH(tmp, &pmix_server_globals.nspaces, pmix_nspace_t) {
        if (0 == strcmp(tmp->nspace, cd->nspace)) {
            nptr = tmp;
            /* release any existing packed data - we will replace it */
            if (0 < nptr->job_info.bytes_used) {
                PMIX_DESTRUCT(&nptr->job_info);
                PMIX_CONSTRUCT(&nptr->job_info, pmix_buffer_t);
            }
            break;
        }
    }
    if (NULL == nptr) {
        nptr = PMIX_NEW(pmix_nspace_t);
        (void)strncpy(nptr->nspace, cd->nspace, PMIX_MAX_NSLEN);
        nptr->nlocalprocs = cd->nlocalprocs;
        pmix_list_append(&pmix_server_globals.nspaces, &nptr->super);
    }
    /* pack the name of the nspace */
    msg = nptr->nspace;
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(&nptr->job_info, &msg, 1, PMIX_STRING))) {
        PMIX_ERROR_LOG(rc);
        pmix_list_remove_item(&pmix_server_globals.nspaces, &nptr->super);
        PMIX_RELEASE(nptr);
        goto release;
    }

    /* pack the provided info */
    PMIX_CONSTRUCT(&kv, pmix_kval_t);
    for (i=0; i < cd->ninfo; i++) {
        pmix_output_verbose(2, pmix_globals.debug_output,
                            "pmix:server _register_nspace recording %s",
                            cd->info[i].key);
    
        if (0 == strcmp(cd->info[i].key, PMIX_NODE_MAP)) {
            /* parse the regex to get the argv array of node names */
            if (PMIX_SUCCESS != (rc = pmix_regex_parse_nodes(cd->info[i].value.data.string, &nodes))) {
                PMIX_ERROR_LOG(rc);
                continue;
            }
            /* if we have already found the proc map, then pass
             * the detailed map */
            if (NULL != procs) {
                pmix_pack_proc_map(&nptr->job_info, nodes, procs);
                pmix_argv_free(nodes);
                nodes = NULL;
                pmix_argv_free(procs);
                procs = NULL;
            }
        } else if (0 == strcmp(cd->info[i].key, PMIX_PROC_MAP)) {
            /* parse the regex to get the argv array containg proc ranks on each node */
            if (PMIX_SUCCESS != (rc = pmix_regex_parse_procs(cd->info[i].value.data.string, &procs))) {
                PMIX_ERROR_LOG(rc);
                continue;
            }
            /* if we have already recv'd the node map, then record
             * the detailed map */
            if (NULL != nodes) {
                pmix_pack_proc_map(&nptr->job_info, nodes, procs);
                pmix_argv_free(nodes);
                nodes = NULL;
                pmix_argv_free(procs);
                procs = NULL;
            }
        } else if (0 == strcmp(cd->info[i].key, PMIX_PROC_DATA)) {
            /* an array of data pertaining to a specific proc */
            if (PMIX_INFO_ARRAY != cd->info[i].value.type) {
                PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
                goto release;
            }
            size = cd->info[i].value.data.array.size;
            iptr = (pmix_info_t*)cd->info[i].value.data.array.array;
            PMIX_CONSTRUCT(&buf2, pmix_buffer_t);
            /* first element of the array must be the rank */
            if (0 != strcmp(iptr[0].key, PMIX_RANK)) {
                PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
                PMIX_DESTRUCT(&buf2);
                goto release;
            }
            /* pack it separately */
            rank = iptr[0].value.data.integer;
            if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(&buf2, &rank, 1, PMIX_INT))) {
                PMIX_ERROR_LOG(rc);
                pmix_list_remove_item(&pmix_server_globals.nspaces, &nptr->super);
                PMIX_RELEASE(nptr);
                PMIX_DESTRUCT(&buf2);
                goto release;
            }
            /* cycle thru the values for this rank and pack them */
            for (j=1; j < size; j++) {
                kv.key = iptr[j].key;
                kv.value = &iptr[j].value;
                if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(&buf2, &kv, 1, PMIX_KVAL))) {
                    PMIX_ERROR_LOG(rc);
                    pmix_list_remove_item(&pmix_server_globals.nspaces, &nptr->super);
                    PMIX_RELEASE(nptr);
                    PMIX_DESTRUCT(&buf2);
                    goto release;
                }
            }
            /* now add the blob */
            kv.key = PMIX_PROC_BLOB;
            kv.value = &val;
            val.type = PMIX_BYTE_OBJECT;
            val.data.bo.bytes = buf2.base_ptr;
            val.data.bo.size = buf2.bytes_used;
            if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(&nptr->job_info, &kv, 1, PMIX_KVAL))) {
                PMIX_ERROR_LOG(rc);
                pmix_list_remove_item(&pmix_server_globals.nspaces, &nptr->super);
                PMIX_RELEASE(nptr);
                PMIX_DESTRUCT(&buf2);
                goto release;
            }
            PMIX_DESTRUCT(&buf2);
        } else {
            /* just a value relating to the entire job */
            kv.key = cd->info[i].key;
            kv.value = &cd->info[i].value;
            if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(&nptr->job_info, &kv, 1, PMIX_KVAL))) {
                PMIX_ERROR_LOG(rc);
                pmix_list_remove_item(&pmix_server_globals.nspaces, &nptr->super);
                PMIX_RELEASE(nptr);
                goto release;
            }
        }
    }
    /* do not destruct the kv object - no memory leak will result */
    
 release:
    if (NULL != nodes) {
        pmix_argv_free(nodes);
    }
    if (NULL != procs) {
        pmix_argv_free(procs);
    }
    cd->active = false;
}

/* setup the data for a job */
pmix_status_t PMIx_server_register_nspace(const char nspace[], int nlocalprocs,
                                          pmix_info_t info[], size_t ninfo)
{
    pmix_setup_caddy_t *cd;
    size_t i;
    
    cd = PMIX_NEW(pmix_setup_caddy_t);
    (void)strncpy(cd->nspace, nspace, PMIX_MAX_NSLEN);
    cd->nlocalprocs = nlocalprocs;
    /* copy across the info array, if given */
    if (0 < ninfo) {
        cd->ninfo = ninfo;
        PMIX_INFO_CREATE(cd->info, ninfo);
        for (i=0; i < ninfo; i++) {
            (void)strncpy(cd->info[i].key, info[i].key, PMIX_MAX_KEYLEN);
            pmix_value_xfer(&cd->info[i].value, &info[i].value);
        }
    }
    
    if (using_internal_comm) {
        /* we have to push this into our event library to avoid
         * potential threading issues */
        event_assign(&cd->ev, pmix_globals.evbase, -1,
                          EV_WRITE, _register_nspace, cd);
        event_active(&cd->ev, EV_WRITE, 1);
        PMIX_WAIT_FOR_COMPLETION(cd->active);
        PMIX_RELEASE(cd);
    } else {
        /* the caller is responsible for thread protection */
        _register_nspace(0, 0, (void*)cd);
    }
    return PMIX_SUCCESS;
}

static void _execute_collective(int sd, short args, void *cbdata)
{
    pmix_trkr_caddy_t *tcd = (pmix_trkr_caddy_t*)cbdata;
    pmix_server_trkr_t *trk = tcd->trk;

    /* we don't need to check for non-NULL APIs here as
     * that was already done when the tracker was created */
    if (PMIX_FENCENB_CMD == trk->type) {
        pmix_host_server.fence_nb(trk->rngs, pmix_list_get_size(&trk->ranges),
                                  trk->collect_data, trk->modexcbfunc, trk);
    } else if (PMIX_CONNECTNB_CMD == trk->type) {
        pmix_host_server.connect(trk->rngs, pmix_list_get_size(&trk->ranges),
                                 trk->op_cbfunc, trk);
    } else if (PMIX_DISCONNECTNB_CMD == trk->type) {
        pmix_host_server.disconnect(trk->rngs, pmix_list_get_size(&trk->ranges),
                                    trk->op_cbfunc, trk);
    } else {
        /* unknown type */
        PMIX_ERROR_LOG(PMIX_ERR_NOT_FOUND);
        pmix_list_remove_item(&pmix_server_globals.collectives, &trk->super);
        PMIX_RELEASE(trk);
    }
    PMIX_RELEASE(tcd);
}

static void _register_client(int sd, short args, void *cbdata)
{
    pmix_setup_caddy_t *cd = (pmix_setup_caddy_t*)cbdata;
    pmix_rank_info_t *info;
    pmix_nspace_t *nptr, *tmp;
    pmix_server_trkr_t *trk;
    pmix_trkr_caddy_t *tcd;
    
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix:server _register_client for nspace %s rank %d",
                        cd->nspace, cd->rank);
    
    /* see if we already have this nspace */
    nptr = NULL;
    PMIX_LIST_FOREACH(tmp, &pmix_server_globals.nspaces, pmix_nspace_t) {
        if (0 == strcmp(tmp->nspace, cd->nspace)) {
            nptr = tmp;
            break;
        }
    }
    if (NULL == nptr) {
        nptr = PMIX_NEW(pmix_nspace_t);
        (void)strncpy(nptr->nspace, cd->nspace, PMIX_MAX_NSLEN);
        pmix_list_append(&pmix_server_globals.nspaces, &nptr->super);
    }
    /* setup a peer object for this client - since the host server
     * only deals with the original processes and not any clones,
     * we know this function will be called only once per rank */
    info = PMIX_NEW(pmix_rank_info_t);
    info->nptr = nptr;
    info->rank = cd->rank;
    info->uid = cd->uid;
    info->gid = cd->gid;
    info->server_object = cd->server_object;
    pmix_list_append(&nptr->ranks, &info->super);
    /* see if we have everyone */
    if (nptr->nlocalprocs == pmix_list_get_size(&nptr->ranks)) {
        nptr->all_registered = true;
        /* check any pending trackers to see if they are
         * waiting for us */
        PMIX_LIST_FOREACH(trk, &pmix_server_globals.collectives, pmix_server_trkr_t) {
            /* if this tracker is already complete, then we
             * don't need to update it */
            if (trk->def_complete) {
                continue;
            }
            /* update and see if that completes it */
            if (pmix_server_trk_update(trk)) {
                /* it did, so now we need to process it */
                if (using_internal_comm) {
                    /* we don't want to block someone
                     * here, so kick any completed trackers into a
                     * new event for processing */
                    PMIX_EXECUTE_COLLECTIVE(tcd, trk, _execute_collective);
                } else {
                    /* have to do it here as there is no
                     * event engine behind us */
                    PMIX_SETUP_COLLECTIVE(tcd, trk);
                    _execute_collective(0, 0, tcd);
                }
            }
        } 
    }
    /* let the caller know we are done */
    cd->active = false;
}

pmix_status_t PMIx_server_register_client(const char nspace[], int rank,
                                          uid_t uid, gid_t gid,
                                          void *server_object)
{
    pmix_setup_caddy_t *cd;

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix:server register client %s:%d",
                        nspace, rank);
    
     cd = PMIX_NEW(pmix_setup_caddy_t);
    (void)strncpy(cd->nspace, nspace, PMIX_MAX_NSLEN);
    cd->rank = rank;
    cd->uid = uid;
    cd->gid = gid;
    cd->server_object = server_object;
    
    if (using_internal_comm) {
        /* we have to push this into our event library to avoid
         * potential threading issues */
        event_assign(&cd->ev, pmix_globals.evbase, -1,
                          EV_WRITE, _register_client, cd);
        event_active(&cd->ev, EV_WRITE, 1);
        PMIX_WAIT_FOR_COMPLETION(cd->active);
    } else {
        /* the caller is responsible for thread protection */
        _register_client(0, 0, (void*)cd);
    }
    PMIX_RELEASE(cd);
    return PMIX_SUCCESS;
}

/* setup the envars for a child process */
pmix_status_t PMIx_server_setup_fork(const char nspace[],
                                     int rank, char ***env)
{
    char rankstr[PMIX_MAX_VALLEN+1];
    
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix:server setup_fork for nspace %s rank %d",
                        nspace, rank);
    
    /* pass the nspace */
    pmix_setenv("PMIX_NAMESPACE", nspace, true, env);
    /* pass the rank */
    (void)snprintf(rankstr, PMIX_MAX_VALLEN, "%d", rank);
    pmix_setenv("PMIX_RANK", rankstr, true, env);
    /* pass our rendezvous info */
    pmix_setenv("PMIX_SERVER_URI", myuri, true, env);
    /* pass our active security mode */
    pmix_setenv("PMIX_SECURITY_MODE", security_mode, true, env);

    return PMIX_SUCCESS;
}

static void _notify_error(int sd, short args, void *cbdata)
{
    pmix_notify_caddy_t *cd = (pmix_notify_caddy_t*)cbdata;
    int rc;
    int i;
    pmix_peer_t *peer;
    
    /* pack the status */
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(cd->buf, &cd->status, 1, PMIX_INT))) {
        PMIX_ERROR_LOG(rc);
        return;
    }

    /* pack the error ranges */
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(cd->buf, &cd->error_nranges, 1, PMIX_SIZE))) {
        PMIX_ERROR_LOG(rc);
        return;
    }
    if (0 < cd->error_nranges) {
        if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(cd->buf, cd->error_ranges, cd->error_nranges, PMIX_RANGE))) {
            PMIX_ERROR_LOG(rc);
            return;
        }
    }

    /* pack the info */
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(cd->buf, &cd->ninfo, 1, PMIX_SIZE))) {
        PMIX_ERROR_LOG(rc);
        return;
    }
    if (0 < cd->ninfo) {
        if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(cd->buf, cd->info, cd->ninfo, PMIX_INFO))) {
            PMIX_ERROR_LOG(rc);
            return;
        }
    }

    if (!using_internal_comm) {
        /* it is up to the host server to decide who should get this
         * message, and to send it - so just return the data here */
        return;
    }

    /* cycle across our connected clients and send the message to
     * any within the specified range */
    for (i=0; i < pmix_server_globals.clients.size; i++) {
        if (NULL == (peer = (pmix_peer_t*)pmix_pointer_array_get_item(&pmix_server_globals.clients, i))) {
            continue;
        }
        /* if the range is NULL, then send it to everyone */
        if (NULL == cd->ranges) {
            PMIX_RETAIN(cd->buf);
            PMIX_SERVER_QUEUE_REPLY(peer, 0, cd->buf);
            continue;
        }
    }
    cd->active = false;
}


pmix_status_t PMIx_server_notify_error(pmix_status_t status,
                                       pmix_range_t ranges[], size_t nranges,
                                       pmix_range_t error_ranges[], size_t error_nranges,
                                       pmix_info_t info[], size_t ninfo,
                                       char **payload, size_t *size)
{
    pmix_notify_caddy_t *cd;

    cd = PMIX_NEW(pmix_notify_caddy_t);

    if (using_internal_comm) {
        /* we have to push this into our event library to avoid
         * potential threading issues */
        event_assign(&cd->ev, pmix_globals.evbase, -1,
                          EV_WRITE, _notify_error, cd);
        event_active(&cd->ev, EV_WRITE, 1);
        PMIX_WAIT_FOR_COMPLETION(cd->active);
        PMIX_RELEASE(cd);
        return PMIX_SUCCESS;
    }

    /* the caller is responsible for thread protection */
    _notify_error(0, 0, (void*)cd);
    *payload = cd->buf->base_ptr;
    *size = cd->buf->bytes_used;
    /* protect the data */
    cd->buf->bytes_used = 0;
    cd->buf->base_ptr = NULL;
    PMIX_RELEASE(cd);
    return PMIX_SUCCESS;
    
}

void PMIx_Register_errhandler(pmix_notification_fn_t err)
{
    pmix_globals.errhandler = err;
}

void PMIx_Deregister_errhandler(void)
{
   pmix_globals.errhandler = NULL;
}

#define PMIX_MAX_NODE_PREFIX        50

pmix_status_t PMIx_generate_regex(const char *input, char **regexp)
{
    char *vptr, *vsave;
    char prefix[PMIX_MAX_NODE_PREFIX];
    int i, j, len, startnum, vnum, numdigits;
    bool found, fullval;
    char *suffix, *sfx;
    pmix_regex_value_t *vreg;
    pmix_regex_range_t *range;
    pmix_list_t vids;
    char **regexargs = NULL, *tmp, *tmp2;
    char *cptr;

    /* define the default */
    *regexp = NULL;

    cptr = strchr(input, ',');
    if (NULL == cptr) {
        /* if there is only one value, don't bother */
        *regexp = strdup(input);
        return PMIX_SUCCESS;
    }

    /* setup the list of results */
    PMIX_CONSTRUCT(&vids, pmix_list_t);

    /* cycle thru the array of input values - first copy
     * it so we don't overwrite what we were given*/
    vsave = strdup(input);
    vptr = vsave;
    while (NULL != (cptr = strchr(vptr, ',')) || 0 < strlen(vptr)) {
        if (NULL != cptr) {
            *cptr = '\0';
        }
        /* determine this node's prefix by looking for first non-alpha char */
        fullval = false;
        len = strlen(vptr);
        startnum = -1;
        memset(prefix, 0, PMIX_MAX_NODE_PREFIX);
        numdigits = 0;
        for (i=0, j=0; i < len; i++) {
            if (!isalpha(vptr[i])) {
                /* found a non-alpha char */
                if (!isdigit(vptr[i])) {
                    /* if it is anything but a digit, we just use
                     * the entire name
                     */
                    fullval = true;
                    break;
                }
                /* count the size of the numeric field - but don't
                 * add the digits to the prefix
                 */
                numdigits++;
                if (startnum < 0) {
                    /* okay, this defines end of the prefix */
                    startnum = i;
                }
                continue;
            }
            if (startnum < 0) {
                prefix[j++] = vptr[i];
            }
        }
        if (fullval || startnum < 0) {
            /* can't compress this name - just add it to the list */
            vreg = PMIX_NEW(pmix_regex_value_t);
            vreg->prefix = strdup(vptr);
            pmix_list_append(&vids, &vreg->super);
            /* move to the next posn */
            if (NULL == cptr) {
                break;
            }
            vptr = cptr + 1;
            continue;
        }
        /* convert the digits and get any suffix */
        vnum = strtol(&vptr[startnum], &sfx, 10);
        if (NULL != sfx) {
            suffix = strdup(sfx);
        } else {
            suffix = NULL;
        }
        /* is this value already on our list? */
        found = false;
        PMIX_LIST_FOREACH(vreg, &vids, pmix_regex_value_t) {
            if (0 < strlen(prefix) && NULL == vreg->prefix) {
                continue;
            }
            if (0 == strlen(prefix) && NULL != vreg->prefix) {
                continue;
            }
            if (0 < strlen(prefix) && NULL != vreg->prefix
                && 0 != strcmp(prefix, vreg->prefix)) {
                continue;
            }
            if (NULL == suffix && NULL != vreg->suffix) {
                continue;
            }
            if (NULL != suffix && NULL == vreg->suffix) {
                continue;
            }
            if (NULL != suffix && NULL != vreg->suffix &&
                0 != strcmp(suffix, vreg->suffix)) {
                continue;
            }
            if (numdigits != vreg->num_digits) {
                continue;
            }
            /* found a match - flag it */
            found = true;
            /* get the last range on this nodeid - we do this
             * to preserve order
             */
            range = (pmix_regex_range_t*)pmix_list_get_last(&vreg->ranges);
            if (NULL == range) {
                /* first range for this value */
                range = PMIX_NEW(pmix_regex_range_t);
                range->start = vnum;
                range->cnt = 1;
                pmix_list_append(&vreg->ranges, &range->super);
                break;
            }
            /* see if the value is out of sequence */
            if (vnum != (range->start + range->cnt)) {
                /* start a new range */
                range = PMIX_NEW(pmix_regex_range_t);
                range->start = vnum;
                range->cnt = 1;
                pmix_list_append(&vreg->ranges, &range->super);
                break;
            }
            /* everything matches - just increment the cnt */
            range->cnt++;
            break;
        }
        if (!found) {
            /* need to add it */
            vreg = PMIX_NEW(pmix_regex_value_t);
            if (0 < strlen(prefix)) {
                vreg->prefix = strdup(prefix);
            }
            if (NULL != suffix) {
                vreg->suffix = strdup(suffix);
            }
            vreg->num_digits = numdigits;
            pmix_list_append(&vids, &vreg->super);
            /* record the first range for this value - we took
             * care of values we can't compress above
             */
            range = PMIX_NEW(pmix_regex_range_t);
            range->start = vnum;
            range->cnt = 1;
            pmix_list_append(&vreg->ranges, &range->super);
        }
        if (NULL != suffix) {
            free(suffix);
        }
        /* move to the next posn */
        if (NULL == cptr) {
            break;
        }
        vptr = cptr + 1;
    }
    free(vsave);

    /* begin constructing the regular expression */
    while (NULL != (vreg = (pmix_regex_value_t*)pmix_list_remove_first(&vids))) {        
        /* if no ranges, then just add the name */
        if (0 == pmix_list_get_size(&vreg->ranges)) {
            if (NULL != vreg->prefix) {
                /* solitary value */
                asprintf(&tmp, "%s", vreg->prefix);
                pmix_argv_append_nosize(&regexargs, tmp);
                free(tmp);
            }
            PMIX_RELEASE(vreg);
            continue;
        }
        /* start the regex for this value with the prefix */
        if (NULL != vreg->prefix) {
            asprintf(&tmp, "%s[%d:", vreg->prefix, vreg->num_digits);
        } else {
            asprintf(&tmp, "[%d:", vreg->num_digits);
        }
        /* add the ranges */
        while (NULL != (range = (pmix_regex_range_t*)pmix_list_remove_first(&vreg->ranges))) {
            if (1 == range->cnt) {
                asprintf(&tmp2, "%s%d,", tmp, range->start);
            } else {
                asprintf(&tmp2, "%s%d-%d,", tmp, range->start, range->start + range->cnt - 1);
            }
            free(tmp);
            tmp = tmp2;
            PMIX_RELEASE(range);
        }
        /* replace the final comma */
        tmp[strlen(tmp)-1] = ']';
        if (NULL != vreg->suffix) {
            /* add in the suffix, if provided */
            asprintf(&tmp2, "%s%s", tmp, vreg->suffix);
            free(tmp);
            tmp = tmp2;
        }
        pmix_argv_append_nosize(&regexargs, tmp);
        free(tmp);
        PMIX_RELEASE(vreg);
    }
    
    /* assemble final result */
    tmp = pmix_argv_join(regexargs, ',');
    asprintf(regexp, "pmix[%s]", tmp);
    free(tmp);
    
    /* cleanup */
    pmix_argv_free(regexargs);

    PMIX_DESTRUCT(&vids);
    return PMIX_SUCCESS;
}

pmix_status_t PMIx_generate_ppn(const char *input, char **regexp)
{
    char **ppn, **npn;
    int i, j, start, end;
    pmix_regex_value_t *vreg;
    pmix_regex_range_t *rng;
    pmix_list_t nodes;
    char *tmp, *tmp2;
    char *cptr;

    /* define the default */
    *regexp = NULL;

    /* setup the list of results */
    PMIX_CONSTRUCT(&nodes, pmix_list_t);

    /* split the input by node */
    ppn = pmix_argv_split(input, ';');
    if (1 == pmix_argv_count(ppn)) {
        /* if there is only one node, don't bother */
        *regexp = strdup(input);
        pmix_argv_free(ppn);
        return PMIX_SUCCESS;
    }

    /* for each node, split the input by comma */
    for (i=0; NULL != ppn[i]; i++) {
        rng = NULL;
        /* create a record for this node */
        vreg = PMIX_NEW(pmix_regex_value_t);
        pmix_list_append(&nodes, &vreg->super);
        /* split the input for this node */
        npn = pmix_argv_split(ppn[i], ',');
        /* look at each element */
        for (j=0; NULL != npn[j]; j++) {
            /* is this a range? */
            if (NULL != (cptr = strchr(npn[j], '-'))) {
                /* terminate the string */
                *cptr = '\0';
                ++cptr;
                start = strtol(npn[j], NULL, 10);
                end = strtol(cptr, NULL, 10);
                /* are we collecting a range? */
                if (NULL == rng) {
                    /* no - better start one */
                    rng = PMIX_NEW(pmix_regex_range_t);
                    rng->start = start;
                    rng->cnt = end - start + 1;
                    pmix_list_append(&vreg->ranges, &rng->super);
                } else {
                    /* is this a continuation of the current range? */
                    if (start == (rng->start + rng->cnt)) {
                        /* just add it to the end of this range */
                        rng->cnt++;
                    } else {
                        /* nope, there is a break - create new range */
                        rng = PMIX_NEW(pmix_regex_range_t);
                        rng->start = start;
                        rng->cnt = end - start + 1;
                        pmix_list_append(&vreg->ranges, &rng->super);
                    }
                }
            } else {
                /* single rank given */
                start = strtol(npn[j], NULL, 10);
                /* are we collecting a range? */
                if (NULL == rng) {
                    /* no - better start one */
                    rng = PMIX_NEW(pmix_regex_range_t);
                    rng->start = start;
                    rng->cnt = 1;
                    pmix_list_append(&vreg->ranges, &rng->super);
                } else {
                    /* is this a continuation of the current range? */
                    if (start == (rng->start + rng->cnt)) {
                        /* just add it to the end of this range */
                        rng->cnt++;
                    } else {
                        /* nope, there is a break - create new range */
                        rng = PMIX_NEW(pmix_regex_range_t);
                        rng->start = start;
                        rng->cnt = 1;
                        pmix_list_append(&vreg->ranges, &rng->super);
                    }
                }
            }
        }
        pmix_argv_free(npn);
    }
    pmix_argv_free(ppn);
    

    /* begin constructing the regular expression */
    tmp = strdup("pmix[");
    PMIX_LIST_FOREACH(vreg, &nodes, pmix_regex_value_t) {
        while (NULL != (rng = (pmix_regex_range_t*)pmix_list_remove_first(&vreg->ranges))) {
            if (1 == rng->cnt) {
                asprintf(&tmp2, "%s%d,", tmp, rng->start);
            } else {
                asprintf(&tmp2, "%s%d-%d,", tmp, rng->start, rng->start + rng->cnt - 1);
            }
            free(tmp);
            tmp = tmp2;
            PMIX_RELEASE(rng);
        }
        /* replace the final comma */
        tmp[strlen(tmp)-1] = ';';
    }
    
    /* replace the final semi-colon */
    tmp[strlen(tmp)-1] = ']';
    
    /* assemble final result */
    *regexp = tmp;

    PMIX_LIST_DESTRUCT(&nodes);
    return PMIX_SUCCESS;
}

/*
 * start listening on our rendezvous file
 */
static int start_listening(struct sockaddr_un *address)
{
    int flags;
    unsigned int addrlen;

    /* create a listen socket for incoming connection attempts */
    mysocket = socket(PF_UNIX, SOCK_STREAM, 0);
    if (mysocket < 0) {
        printf("%s:%d socket() failed", __FILE__, __LINE__);
        return -1;
    }

    addrlen = sizeof(struct sockaddr_un);
    if (bind(mysocket, (struct sockaddr*)address, addrlen) < 0) {
        printf("%s:%d bind() failed", __FILE__, __LINE__);
        return -1;
    }
        
    /* setup listen backlog to maximum allowed by kernel */
    if (listen(mysocket, SOMAXCONN) < 0) {
        printf("%s:%d listen() failed", __FILE__, __LINE__);
        return -1;
    }
        
    /* set socket up to be non-blocking, otherwise accept could block */
    if ((flags = fcntl(mysocket, F_GETFL, 0)) < 0) {
        printf("%s:%d fcntl(F_GETFL) failed", __FILE__, __LINE__);
        return -1;
    }
    flags |= O_NONBLOCK;
    if (fcntl(mysocket, F_SETFL, flags) < 0) {
        printf("%s:%d fcntl(F_SETFL) failed", __FILE__, __LINE__);
        return -1;
    }

    /* setup to listen via the event lib */
    event_assign(&listen_ev, pmix_globals.evbase, mysocket,
                 EV_READ|EV_PERSIST, connection_handler, 0);
    event_add(&listen_ev, 0);
    return 0;
}

static int send_client_response(int sd, int status, pmix_buffer_t *payload)
{
    int rc;
    pmix_usock_hdr_t hdr;
    pmix_buffer_t buf;
    
    /* pack the status */
    PMIX_CONSTRUCT(&buf, pmix_buffer_t);
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(&buf, &status, 1, PMIX_INT))) {
        PMIX_ERROR_LOG(rc);
        PMIX_DESTRUCT(&buf);
        return rc;
    }
    if (NULL != payload) {
        pmix_bfrop.copy_payload(&buf, payload);
    }

    hdr.nbytes = buf.bytes_used;
    hdr.pindex = 0;
    hdr.tag = 0; // tag doesn't matter as we aren't matching to a recv

    if (PMIX_SUCCESS != (rc = pmix_usock_send_blocking(sd, (char*)&hdr, sizeof(hdr)))) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    if (PMIX_SUCCESS != (rc = pmix_usock_send_blocking(sd, (char*)buf.base_ptr, buf.bytes_used))) {
        PMIX_ERROR_LOG(rc);
        PMIX_DESTRUCT(&buf);
        return rc;
    }
    PMIX_DESTRUCT(&buf);
    
    return PMIX_SUCCESS;
}

/*
 * Handler for accepting connections from the event library
 */
static void connection_handler(int incoming_sd, short flags, void* cbdata)
{
    int sd;
    pmix_peer_t *peer;
    int rank;
    
    if( 0 > (sd = accept(incoming_sd,NULL,0)) ){
        printf("accept() failed");
        exit(0);
    }

    /* receive identifier info from the client and authenticate it - the
     * function will lookup and return the peer object if the connection
     * is successfully authenticated */
    if (PMIX_SUCCESS != pmix_server_authenticate(sd, &rank, &peer, NULL)) {
        CLOSE_THE_SOCKET(sd);
        return;
    }
    pmix_usock_set_nonblocking(sd);

    /* start the events for this client */
    event_assign(&peer->recv_event, pmix_globals.evbase, sd,
                 EV_READ|EV_PERSIST, pmix_usock_recv_handler, peer);
    event_add(&peer->recv_event, NULL);
    peer->recv_ev_active = true;
    event_assign(&peer->send_event, pmix_globals.evbase, sd,
                 EV_WRITE|EV_PERSIST, pmix_usock_send_handler, peer);
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix:server client %s:%d has connected on socket %d",
                        peer->info->nptr->nspace, peer->info->rank, peer->sd);
}

/*  Receive the peer's identification info from a newly
 *  connected socket and verify the expected response.
 */
pmix_status_t pmix_server_authenticate(int sd, int *out_rank, pmix_peer_t **peer,
                                       pmix_buffer_t **reply)
{
    char *msg, *nspace, *version, *cred, *ptr;
    int rc, rank;
    pmix_usock_hdr_t hdr;
    pmix_nspace_t *nptr, *tmp;
    pmix_rank_info_t *info;
    pmix_peer_t *psave = NULL;
    size_t csize;
    pmix_buffer_t *bptr;
    bool found;
    
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "RECV CONNECT ACK FROM PEER ON SOCKET %d", sd);

    /* ensure all is zero'd */
    memset(&hdr, 0, sizeof(pmix_usock_hdr_t));
    *peer = NULL;
    if (NULL != reply) {
        *reply = NULL;
    }

    /* get the header */
    if (PMIX_SUCCESS != (rc = pmix_usock_recv_blocking(sd, (char*)&hdr, sizeof(pmix_usock_hdr_t)))) {
        return rc;
    }

    /* get the id, authentication and version payload (and possibly
     * security credential) - to guard against potential attacks,
     * we'll set an arbitrary limit per a define */
    if (PMIX_MAX_CRED_SIZE < hdr.nbytes) {
        return PMIX_ERR_BAD_PARAM;
    }
    if (NULL == (msg = (char*)malloc(hdr.nbytes))) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    }
    if (PMIX_SUCCESS != pmix_usock_recv_blocking(sd, msg, hdr.nbytes)) {
        /* unable to complete the recv */
        pmix_output_verbose(2, pmix_globals.debug_output,
                            "unable to complete recv of connect-ack with client ON SOCKET %d", sd);
        free(msg);
        return PMIX_ERR_UNREACH;
    }

    /* get the nspace */
    nspace = msg;  // a NULL terminator is in the data
    
    /* get the rank */
    memcpy(&rank, msg+strlen(nspace)+1, sizeof(int));

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "connect-ack recvd from peer %s:%d",
                        nspace, rank);

    /* check that this is from a matching version - for ABI
     * compatibility, we only require that this match at the
     * major and minor levels: not the release */
    csize = strlen(nspace)+1+sizeof(int);
    version = (char*)(msg+csize);
    /* find the first '.' */
    ptr = strchr(version, '.');
    if (NULL != ptr) {
        ++ptr;
        /* stop it at the second '.', if present */
        if (NULL != (ptr = strchr(ptr, '.'))) {
            *ptr = '\0';
        }
    }
    if (0 != strcmp(version, PMIX_VERSION)) {
        pmix_output_verbose(2, pmix_globals.debug_output,
                            "pmix:server client/server PMIx versions mismatch");
        free(msg);
        return PMIX_ERR_NOT_SUPPORTED;
    }
    csize += strlen(version) + 1;
    
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "connect-ack version from client matches ours");

    /* see if we know this nspace */
    nptr = NULL;
    PMIX_LIST_FOREACH(tmp, &pmix_server_globals.nspaces, pmix_nspace_t) {
        if (0 == strcmp(tmp->nspace, nspace)) {
            nptr = tmp;
            break;
        }
    }
    if (NULL == nptr) {
        /* we don't know this namespace, reject it */
        free(msg);
        return PMIX_ERR_NOT_FOUND;
    }

    /* see if we have this peer in our list */
    info = NULL;
    found = false;
    PMIX_LIST_FOREACH(info, &nptr->ranks, pmix_rank_info_t) {
        if (info->rank == rank) {
            found = true;
            break;
        }
    }
    if (!found) {
        /* rank unknown, reject it */
        free(msg);
        return PMIX_ERR_NOT_FOUND;
    }
    *out_rank = rank;
    /* a peer can connect on multiple sockets since it can fork/exec
     * a child that also calls PMIx_Init, so add it here if necessary.
     * Create the tracker for this peer */
    psave = PMIX_NEW(pmix_peer_t);
    psave->info = info;
    info->proc_cnt++; /* increase number of processes on this rank */
    psave->sd = sd;
    if (0 > (psave->index = pmix_pointer_array_add(&pmix_server_globals.clients, psave))) {
        free(msg);
        PMIX_RELEASE(psave);
        return PMIX_ERR_OUT_OF_RESOURCE;
    }
    
    /* see if there is a credential */
    if (csize < hdr.nbytes) {
        cred = (char*)(msg + csize);
        if (NULL != cred && NULL != pmix_sec.validate_cred) {
            if (PMIX_SUCCESS != (rc = pmix_sec.validate_cred(psave, cred))) {
                pmix_output_verbose(2, pmix_globals.debug_output,
                                    "validation of client credential failed");
                free(msg);
                pmix_pointer_array_set_item(&pmix_server_globals.clients, psave->index, NULL);
                PMIX_RELEASE(psave);
                return rc;
            }
            pmix_output_verbose(2, pmix_globals.debug_output,
                                "client credential validated");
        }
    }
    free(msg);

    /* execute the handshake if the security mode calls for it */
    if (NULL != pmix_sec.server_handshake) {
        pmix_output_verbose(2, pmix_globals.debug_output,
                            "connect-ack executing handshake");
        if (PMIX_SUCCESS != send_client_response(sd, PMIX_ERR_READY_FOR_HANDSHAKE, NULL)) {
            pmix_pointer_array_set_item(&pmix_server_globals.clients, psave->index, NULL);
            PMIX_RELEASE(psave);
            return PMIX_ERR_UNREACH;
        }
        if (PMIX_SUCCESS != pmix_sec.server_handshake(psave)) {
            pmix_pointer_array_set_item(&pmix_server_globals.clients, psave->index, NULL);
            PMIX_RELEASE(psave);
            return PMIX_ERR_UNREACH;
        }
        pmix_output_verbose(2, pmix_globals.debug_output,
                            "connect-ack handshake complete");
    }

    /* create reply */
    bptr = PMIX_NEW(pmix_buffer_t);
    /* send this process its index */
    pmix_bfrop.pack(bptr, (void*)&psave->index, 1, PMIX_INT);
    /* copy any data across */
    pmix_bfrop.copy_payload(bptr, &nptr->job_info);
    
    if (NULL == reply) {
        /* let the client know we are ready to go */
        pmix_output_verbose(2, pmix_globals.debug_output,
                            "connect-ack sending client response with %d bytes",
                            (NULL == bptr) ? 0 : (int)bptr->bytes_used);
        if (PMIX_SUCCESS != (rc = send_client_response(sd, PMIX_SUCCESS, bptr))) {
            pmix_pointer_array_set_item(&pmix_server_globals.clients, psave->index, NULL);
            PMIX_RELEASE(psave);
            return rc;
        }
        PMIX_RELEASE(bptr);
    } else {
        *reply = bptr;
    }
    
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "connect-ack from client completed");

    *peer = psave;
    return rc;
}

/****    THE FOLLOWING CALLBACK FUNCTIONS ARE USED BY THE HOST SERVER    ****
 ****    THEY THEREFORE CAN OCCUR IN EITHER THE HOST SERVER'S THREAD     ****
 ****    CONTEXT, OR IN OUR OWN THREAD CONTEXT IF THE CALLBACK OCCURS    ****
 ****    IMMEDIATELY. THUS ANYTHING THAT ACCESSES A GLOBAL ENTITY        ****
 ****    MUST BE PUSHED INTO AN EVENT FOR PROTECTION                     ****/

static void op_cbfunc(int status, void *cbdata)
{
    pmix_server_caddy_t *cd = (pmix_server_caddy_t*)cbdata;
    pmix_buffer_t *reply;
    int rc;

    /* setup the reply with the returned status */
    reply = PMIX_NEW(pmix_buffer_t);
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(reply, &status, 1, PMIX_INT))) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(reply);
        return;
    }
    /* the function that created the server_caddy did a
     * retain on the peer, so we don't have to worry about
     * it still being present - send a copy to the originator */
    PMIX_SERVER_QUEUE_REPLY(cd->peer, cd->hdr.tag, reply);
    /* cleanup */
    PMIX_RELEASE(cd);
}

static void spawn_cbfunc(int status, char *nspace, void *cbdata)
{
    pmix_server_caddy_t *cd = (pmix_server_caddy_t*)cbdata;
    pmix_buffer_t *reply;
    int rc;

    /* setup the reply with the returned status */
    reply = PMIX_NEW(pmix_buffer_t);
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(reply, &status, 1, PMIX_INT))) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(reply);
        return;
    }
    /* add the nspace */
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(reply, &nspace, 1, PMIX_STRING))) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(reply);
        return;
    }
    /* the function that created the server_caddy did a
     * retain on the peer, so we don't have to worry about
     * it still being present - tell the originator the result */
    PMIX_SERVER_QUEUE_REPLY(cd->peer, cd->hdr.tag, reply);
    /* cleanup */
    PMIX_RELEASE(cd);
}

static void lookup_cbfunc(int status, pmix_info_t info[], size_t ninfo,
                          char nspace[], void *cbdata)
{
    pmix_server_caddy_t *cd = (pmix_server_caddy_t*)cbdata;
    pmix_buffer_t *reply;
    int rc;

    /* setup the reply with the returned status */
    reply = PMIX_NEW(pmix_buffer_t);
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(reply, &status, 1, PMIX_INT))) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(reply);
        return;
    }
    if (PMIX_SUCCESS == status) {
        /* pack the returned nspace */
        if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(reply, &nspace, 1, PMIX_STRING))) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(reply);
            return;
        }

        /* pack the returned info objects */
        if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(reply, &ninfo, 1, PMIX_SIZE))) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(reply);
            return;
        }
        if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(reply, info, ninfo, PMIX_INFO))) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(reply);
            return;
        }
    }

    /* the function that created the server_caddy did a
     * retain on the peer, so we don't have to worry about
     * it still being present - tell the originator the result */
    PMIX_SERVER_QUEUE_REPLY(cd->peer, cd->hdr.tag, reply);
    /* cleanup */
    PMIX_RELEASE(cd);
}

static void _tracker_complete(int sd, short args, void *cbdata)
{
    pmix_trkr_caddy_t *cd = (pmix_trkr_caddy_t*)cbdata;
    
    pmix_list_remove_item(&pmix_server_globals.collectives, &cd->trk->super);
    PMIX_RELEASE(cd->trk);
    PMIX_RELEASE(cd);
}
    
static void modex_cbfunc(int status, pmix_modex_data_t data[],
                         size_t ndata, void *cbdata)
{
    pmix_server_trkr_t *tracker = (pmix_server_trkr_t*)cbdata;
    pmix_buffer_t *reply;
    int rc;
    pmix_server_caddy_t *cd;
    
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "server:modex_cbfunc called with %d elements", (int)ndata);

    if (NULL == tracker) {
        /* nothing to do */
        return;
    }
    
    /* setup the reply, starting with the returned status */
    reply = PMIX_NEW(pmix_buffer_t);
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(reply, &status, 1, PMIX_INT))) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(reply);
        return;
    }
    /* pack the nblobs being returned */
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(reply, &ndata, 1, PMIX_SIZE))) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(reply);
        return;
    }
    if (0 < ndata) {
        if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(reply, data, ndata, PMIX_MODEX))) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(reply);
            return;
        }
    }
    /* loop across all procs in the tracker, sending them the reply */
    PMIX_LIST_FOREACH(cd, &tracker->locals, pmix_server_caddy_t) {
        PMIX_RETAIN(reply);
        pmix_output_verbose(2, pmix_globals.debug_output,
                            "server:modex_cbfunc reply being sent to %s:%d",
                            cd->peer->info->nptr->nspace, cd->peer->info->rank);
       PMIX_SERVER_QUEUE_REPLY(cd->peer, cd->hdr.tag, reply);
    }
    PMIX_RELEASE(reply);  // maintain accounting
    /* push this into our own thread so we can remove it from the
     * global collectives */
    PMIX_MARK_COLLECTIVE_COMPLETE(tracker, _tracker_complete);
}

static void get_cbfunc(int status, pmix_modex_data_t data[],
                       size_t ndata, void *cbdata)
{
    pmix_server_caddy_t *cd = (pmix_server_caddy_t*)cbdata;
    pmix_buffer_t *reply;
    int rc;
    
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "server:modex_cbfunc called with %d elements", (int)ndata);

    if (NULL == cd) {
        /* nothing to do */
        return;
    }
    
    /* setup the reply, starting with the returned status */
    reply = PMIX_NEW(pmix_buffer_t);
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(reply, &status, 1, PMIX_INT))) {
        PMIX_ERROR_LOG(rc);
        goto cleanup;
    }
    /* pack the nblobs being returned */
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(reply, &ndata, 1, PMIX_SIZE))) {
        PMIX_ERROR_LOG(rc);
        goto cleanup;
    }
    if (0 < ndata) {
        if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(reply, data, ndata, PMIX_MODEX))) {
            PMIX_ERROR_LOG(rc);
        goto cleanup;
        }
    }
    /* send the data to the requestor */
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "server:get_cbfunc reply being sent to %s:%d",
                        cd->peer->info->nptr->nspace, cd->peer->info->rank);
    PMIX_SERVER_QUEUE_REPLY(cd->peer, cd->hdr.tag, reply);

 cleanup:
    PMIX_RELEASE(cd);
}

static void cnct_cbfunc(int status, void *cbdata)
{
    pmix_server_trkr_t *tracker = (pmix_server_trkr_t*)cbdata;
    pmix_buffer_t *reply;
    int rc, i;
    pmix_server_caddy_t *cd;
    char **nspaces=NULL;
    pmix_nspace_t *nptr;
    
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "server:cnct_cbfunc called");

    if (NULL == tracker) {
        /* nothing to do */
        return;
    }
    
    /* setup the reply, starting with the returned status */
    reply = PMIX_NEW(pmix_buffer_t);
    if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(reply, &status, 1, PMIX_INT))) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(reply);
        return;
    }
    /* find the unique nspaces that are participating */
    PMIX_LIST_FOREACH(cd, &tracker->locals, pmix_server_caddy_t) {
        pmix_argv_append_unique_nosize(&nspaces, cd->peer->info->nptr->nspace, false);
    }
    
    /* loop across all participating nspaces and include their
     * job-related info */
    for (i=0; NULL != nspaces[i]; i++) {
        PMIX_LIST_FOREACH(nptr, &pmix_server_globals.nspaces, pmix_nspace_t) {
            if (0 != strcmp(nspaces[i], nptr->nspace)) {
                continue;
            }
            if (PMIX_SUCCESS != (rc = pmix_bfrop.pack(reply, &nptr->job_info, 1, PMIX_BUFFER))) {
                PMIX_ERROR_LOG(rc);
                PMIX_RELEASE(reply);
                pmix_argv_free(nspaces);
                return;
            }
        }
    }
    pmix_argv_free(nspaces);
    
    /* loop across all procs in the tracker, sending them the reply */
    PMIX_LIST_FOREACH(cd, &tracker->locals, pmix_server_caddy_t) {
        PMIX_RETAIN(reply);
        pmix_output_verbose(2, pmix_globals.debug_output,
                            "server:cnct_cbfunc reply being sent to %s:%d",
                            cd->peer->info->nptr->nspace, cd->peer->info->rank);
       PMIX_SERVER_QUEUE_REPLY(cd->peer, cd->hdr.tag, reply);
    }
    PMIX_RELEASE(reply);  // maintain accounting
    /* push this into our own thread so we can remove it from the
     * global collectives */
    PMIX_MARK_COLLECTIVE_COMPLETE(tracker, _tracker_complete);
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
static int server_switchyard(pmix_peer_t *peer, uint32_t tag,
                             pmix_buffer_t *buf)
{
    int rc;
    int32_t cnt;
    pmix_cmd_t cmd;
    pmix_server_caddy_t *cd;
    
    /* retrieve the cmd */
    cnt = 1;
    if (PMIX_SUCCESS != (rc = pmix_bfrop.unpack(buf, &cmd, &cnt, PMIX_CMD))) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "recvd pmix cmd %d from %s:%d",
                        cmd, peer->info->nptr->nspace, peer->info->rank);

    if (PMIX_ABORT_CMD == cmd) {
        PMIX_PEER_CADDY(cd, peer, tag); 
        if (PMIX_SUCCESS != (rc = pmix_server_abort(peer, buf, op_cbfunc, cd))) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(cd);
        }
        return rc;
    }
        
    if (PMIX_COMMIT_CMD == cmd) {
        if (PMIX_SUCCESS != (rc = pmix_server_commit(peer, buf))) {
            PMIX_ERROR_LOG(rc);
        }
        return rc;
    }
        
    if (PMIX_FENCENB_CMD == cmd) {
        PMIX_PEER_CADDY(cd, peer, tag);
        if (PMIX_SUCCESS != (rc = pmix_server_fence(cd, buf, modex_cbfunc, op_cbfunc))) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(cd);
        }
        return rc;
    }

    if (PMIX_GETNB_CMD == cmd) {
        PMIX_PEER_CADDY(cd, peer, tag);
        if (PMIX_SUCCESS != (rc = pmix_server_get(buf, get_cbfunc, cd))) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(cd);
        }
        return rc;
    }
        
    if (PMIX_FINALIZE_CMD == cmd) {
        pmix_output_verbose(2, pmix_globals.debug_output,
                            "recvd FINALIZE");
        /* call the local server, if supported */
        if (NULL != pmix_host_server.finalized) {
            PMIX_PEER_CADDY(cd, peer, tag);
            if (PMIX_SUCCESS != (rc = pmix_host_server.finalized(peer->info->nptr->nspace,
                                                                 peer->info->rank,
                                                                 peer->info->server_object,
                                                                 op_cbfunc, cd))) {
                PMIX_ERROR_LOG(rc);
                PMIX_RELEASE(cd);
            }
        }
        /* turn off the recv event - we shouldn't hear anything
         * more from this proc */
        if (peer->recv_ev_active) {
            event_del(&peer->recv_event);
            peer->recv_ev_active = false;
        }
        return rc;
    }

        
    if (PMIX_PUBLISHNB_CMD == cmd) {
        PMIX_PEER_CADDY(cd, peer, tag);
        if (PMIX_SUCCESS != (rc = pmix_server_publish(buf, op_cbfunc, cd))) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(cd);
        }
        return rc;
    }

    
    if (PMIX_LOOKUPNB_CMD == cmd) {
        PMIX_PEER_CADDY(cd, peer, tag);
        if (PMIX_SUCCESS != (rc = pmix_server_lookup(buf, lookup_cbfunc, cd))) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(cd);
        }
        return rc;
    }

        
    if (PMIX_UNPUBLISHNB_CMD == cmd) {
        PMIX_PEER_CADDY(cd, peer, tag);
        if (PMIX_SUCCESS != (rc = pmix_server_unpublish(buf, op_cbfunc, cd))) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(cd);
        }
        return rc;
    }

        
    if (PMIX_SPAWNNB_CMD == cmd) {
        PMIX_PEER_CADDY(cd, peer, tag);
        if (PMIX_SUCCESS != (rc = pmix_server_spawn(buf, spawn_cbfunc, cd))) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(cd);
        }
        return rc;
    }

        
    if (PMIX_CONNECTNB_CMD == cmd) {
        PMIX_PEER_CADDY(cd, peer, tag);
        if (PMIX_SUCCESS != (rc = pmix_server_connect(cd, buf, false, cnct_cbfunc))) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(cd);
        }
        return rc;
    }

    if (PMIX_DISCONNECTNB_CMD == cmd) {
        PMIX_PEER_CADDY(cd, peer, tag);
        if (PMIX_SUCCESS != (rc = pmix_server_connect(cd, buf, true, cnct_cbfunc))) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(cd);
        }
        return rc;
    }

    return PMIX_ERR_NOT_SUPPORTED;
}

static void server_message_handler(struct pmix_peer_t *pr, pmix_usock_hdr_t *hdr,
                                   pmix_buffer_t *buf, void *cbdata)
{
    pmix_peer_t *peer = (pmix_peer_t*)pr;
    pmix_buffer_t *reply;
    int rc;

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "SWITCHYARD for %s:%d:%d",
                        peer->info->nptr->nspace,
                        peer->info->rank, peer->sd);

    rc = server_switchyard(peer, hdr->tag, buf);
    /* send the return, if there was an error returned */
    if (PMIX_SUCCESS != rc) {
        reply = PMIX_NEW(pmix_buffer_t);
        pmix_bfrop.pack(reply, &rc, 1, PMIX_INT);
        PMIX_SERVER_QUEUE_REPLY(peer, hdr->tag, reply);
        PMIX_RELEASE(reply);
    }
}
