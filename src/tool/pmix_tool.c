/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2014      Artem Y. Polyakov <artpol84@gmail.com>.
 *                         All rights reserved.
 * Copyright (c) 2016      Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2016-2021 IBM Corporation.  All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "src/include/pmix_config.h"

#include "src/include/pmix_socket_errno.h"

#include "include/pmix_server.h"
#include "include/pmix_tool.h"
#include "src/client/pmix_client_ops.h"

#include "src/include/pmix_globals.h"

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
#ifdef HAVE_DIRENT_H
#    include <dirent.h>
#endif /* HAVE_DIRENT_H */

#include "src/class/pmix_list.h"
#include "src/client/pmix_client_ops.h"
#include "src/common/pmix_attributes.h"
#include "src/common/pmix_iof.h"
#include "src/common/pmix_pfexec.h"
#include "src/hwloc/pmix_hwloc.h"
#include "src/include/pmix_globals.h"
#include "src/mca/bfrops/base/base.h"
#include "src/mca/gds/base/base.h"
#include "src/mca/pmdl/base/base.h"
#include "src/mca/pnet/base/base.h"
#include "src/mca/psec/psec.h"
#include "src/mca/ptl/base/base.h"
#include "src/runtime/pmix_progress_threads.h"
#include "src/runtime/pmix_rte.h"
#include "src/server/pmix_server_ops.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_environ.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_name_fns.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_printf.h"
#include "src/util/pmix_show_help.h"

#include "src/tool/pmix_tool_ops.h"

#define PMIX_MAX_RETRIES 10

static pmix_event_t parentdied;
static pmix_proc_t myparent;

/* Records the descriptor PMIX_KEEPALIVE_PIPE named, so finalize can take
 * that event back down and close it. It is a static rather than a local
 * because finalize cannot re-derive the answer - the pipe was a directive
 * to *init*. (The stdin read event and its SIGCONT handler need no such
 * bookkeeping here: they belong to src/common/pmix_iof.c, and
 * pmix_iof_finalize releases them from pmix_rte_finalize.) */
static int keepalive_fd = -1;

static void _pdiedcb(pmix_status_t status, void *cbdata)
{
    pmix_cb_t *cb = (pmix_cb_t*)cbdata;
    PMIX_HIDE_UNUSED_PARAMS(status);
    PMIX_RELEASE(cb);
}

static void pdiedfn(int fd, short flags, void *arg)
{
    pmix_proc_t keepalive;
    pmix_cb_t *cb;
    PMIX_HIDE_UNUSED_PARAMS(fd, flags, arg);

    PMIX_LOAD_PROCID(&keepalive, "PMIX_KEEPALIVE_PIPE", PMIX_RANK_UNDEF);

    /* generate a job-terminated event */
    cb = PMIX_NEW(pmix_cb_t);
    cb->ninfo = 2;
    PMIX_INFO_CREATE(cb->info, cb->ninfo);
    PMIX_INFO_LOAD(&cb->info[0], PMIX_EVENT_NON_DEFAULT, NULL, PMIX_BOOL);
    PMIX_INFO_LOAD(&cb->info[1], PMIX_EVENT_AFFECTED_PROC, &keepalive, PMIX_PROC);
    cb->infocopy = true;  // ensure cleanup
    PMIx_Notify_event(PMIX_ERR_JOB_TERMINATED, &pmix_globals.myid,
                      PMIX_RANGE_PROC_LOCAL, cb->info, cb->ninfo,
                      _pdiedcb, (void*)cb);
}

/* Release the carrier chain once pmix_server_notify_client_of_event has
 * finished with the event it was built from. That call is where a tool's
 * local delivery happens: it caches the event, fans it out to any tools
 * connected to us, and walks our own handlers against a chain of its own
 * making. So this chain never visits a handler list, and the status
 * handed here is the notification's, not a report of whether anything
 * matched.
 *
 * That is why this is not pmix_event_notify_complete, which parks an
 * event nothing accepted: the client hangs that on final_cbfunc because
 * its chain really is the one walked, and a tool has no equivalent
 * moment. The tool carried a copy of the parking code here for years
 * with no way to reach it - see openpmix#4101. */
static void release_chain(pmix_status_t status, void *cbdata)
{
    pmix_event_chain_t *chain = (pmix_event_chain_t *) cbdata;
    PMIX_HIDE_UNUSED_PARAMS(status);

    PMIX_ACQUIRE_OBJECT(chain);
    PMIX_RELEASE(chain);
}

static void pmix_tool_notify_recv(struct pmix_peer_t *peer, pmix_ptl_hdr_t *hdr,
                                  pmix_buffer_t *buf, void *cbdata)
{
    pmix_status_t rc;
    int32_t cnt;
    pmix_cmd_t cmd;
    pmix_event_chain_t *chain;
    size_t ninfo;
    pmix_data_range_t range;
    PMIX_HIDE_UNUSED_PARAMS(peer, hdr, cbdata);

    pmix_output_verbose(2, pmix_client_globals.event_output,
                        "pmix:tool_notify_recv - processing event");

    /* a zero-byte buffer indicates that this recv is being
     * completed due to a lost connection */
    if (PMIX_BUFFER_IS_EMPTY(buf)) {
        return;
    }

    /* Build the chain we will hand to pmix_server_notify_client_of_event
     * below. No final_cbfunc: nothing walks a handler list against this
     * chain, so there is no completion for one to be the completion of.
     * See release_chain above. */
    chain = PMIX_NEW(pmix_event_chain_t);

    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, pmix_client_globals.myserver, buf, &cmd, &cnt, PMIX_COMMAND);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(chain);
        goto error;
    }
    /* unpack the status */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, pmix_client_globals.myserver, buf, &chain->status, &cnt, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(chain);
        goto error;
    }

    /* unpack the source of the event */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, pmix_client_globals.myserver, buf, &chain->source, &cnt, PMIX_PROC);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(chain);
        goto error;
    }

    /* unpack the info that might have been provided */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, pmix_client_globals.myserver, buf, &ninfo, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(chain);
        goto error;
    }

    /* we always leave space for event hdlr name and a callback object */
    chain->nallocated = ninfo + 2;
    PMIX_INFO_CREATE(chain->info, chain->nallocated);
    if (NULL == chain->info) {
        PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
        PMIX_RELEASE(chain);
        return;
    }

    if (0 < ninfo) {
        chain->ninfo = ninfo;
        cnt = ninfo;
        PMIX_BFROPS_UNPACK(rc, pmix_client_globals.myserver, buf, chain->info, &cnt, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(chain);
            goto error;
        }
    }
    /* unpack the range, if provided */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, pmix_client_globals.myserver, buf, &range, &cnt, PMIX_DATA_RANGE);
    if (PMIX_SUCCESS != rc && PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(chain);
        goto error;
    }
    if (PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER == rc) {
        range = PMIX_RANGE_LOCAL;
    }
    /* Record the range on the chain and translate the directives the
     * event carried into the chain's own fields, the way the client's
     * equivalent (pmix_client_notify_recv) always has. Nothing below
     * reads them from here - pmix_server_notify_client_of_event is given
     * the info array and works out the range, targets and affected procs
     * for itself - so this is consistency between the two recv paths
     * rather than a fix for an observed failure. Keep them in step: a
     * reader comparing the two should not have to work out that the
     * difference does not matter. */
    chain->range = range;
    pmix_prep_event_chain(chain, chain->info, ninfo, false);

    if (PMIX_RANGE_LOCAL != range && pmix_atomic_check_bool(&pmix_globals.connected) &&
        !(PMIX_CHECK_NSPACE(peer->nptr->nspace, pmix_client_globals.myserver->nptr->nspace) &&
          peer->info->pname.rank == pmix_client_globals.myserver->info->pname.rank)) {
        pmix_output_verbose(2, pmix_client_globals.event_output,
                            "[%s:%d] pmix:tool_notify_recv - relaying to server",
                            pmix_globals.myid.nspace, pmix_globals.myid.rank);
        rc = pmix_notify_server_of_event(chain->status, &chain->source, range,
                                         chain->info, chain->ninfo, NULL, NULL, false);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(chain);
            goto error;
        }
    }

    pmix_output_verbose(2, pmix_client_globals.event_output,
        "[%s:%d] pmix:tool_notify_recv - processing event %s from source %s:%d, calling errhandler",
        pmix_globals.myid.nspace, pmix_globals.myid.rank, PMIx_Error_string(chain->status),
        chain->source.nspace, chain->source.rank);

    rc = pmix_server_notify_client_of_event(chain->status, &chain->source, range,
                                            chain->info, chain->ninfo, release_chain, chain);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(chain);
        goto error;
    }
    return;

error:
    /* we always need to return */
    pmix_output_verbose(2, pmix_client_globals.event_output,
                        "pmix:tool_notify_recv - unpack error status =%d, calling def errhandler",
                        rc);
    chain = PMIX_NEW(pmix_event_chain_t);
    chain->status = rc;
    pmix_invoke_local_event_hdlr(chain);
}

static void tool_iof_handler(struct pmix_peer_t *pr, pmix_ptl_hdr_t *hdr,
                             pmix_buffer_t *buf, void *cbdata)
{
    pmix_peer_t *peer = (pmix_peer_t *) pr;
    pmix_proc_t source;
    pmix_iof_channel_t channel;
    pmix_byte_object_t bo;
    int32_t cnt;
    pmix_status_t rc;
    size_t refid, ninfo = 0;
    pmix_iof_req_t *req;
    pmix_info_t *info = NULL;
    PMIX_HIDE_UNUSED_PARAMS(hdr, cbdata);

    pmix_output_verbose(2, pmix_client_globals.iof_output,
                        "recvd IOF with %d bytes",
                        (int) buf->bytes_used);

    /* if the buffer is empty, they are simply closing the socket */
    if (0 == buf->bytes_used) {
        return;
    }
    PMIX_BYTE_OBJECT_CONSTRUCT(&bo);

    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &source, &cnt, PMIX_PROC);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return;
    }
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &channel, &cnt, PMIX_IOF_CHANNEL);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return;
    }
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &refid, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return;
    }
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &ninfo, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return;
    }
    if (0 < ninfo) {
        PMIX_INFO_CREATE(info, ninfo);
        cnt = ninfo;
        PMIX_BFROPS_UNPACK(rc, peer, buf, info, &cnt, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto cleanup;
        }
    }
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &bo, &cnt, PMIX_BYTE_OBJECT);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto cleanup;
    }
    /* lookup the handler for this IOF package */
    req = (pmix_iof_req_t *) pmix_pointer_array_get_item(&pmix_globals.iof_requests, refid);
    if (NULL != req && NULL != req->cbfunc) {
        req->cbfunc(refid, channel, &source, &bo, info, ninfo);
    } else {
        /* otherwise, simply write it out to the specified std IO channel */
        if (NULL != bo.bytes && 0 < bo.size) {
            pmix_iof_write_output(&source, channel, &bo);
        }
    }

cleanup:
    /* cleanup the memory */
    if (0 < ninfo) {
        PMIX_INFO_FREE(info, ninfo);
    }
    PMIX_BYTE_OBJECT_DESTRUCT(&bo);
}

/* callback to receive job info */
static void job_data(struct pmix_peer_t *pr, pmix_ptl_hdr_t *hdr,
                     pmix_buffer_t *buf, void *cbdata)
{
    pmix_status_t rc;
    char *nspace;
    int32_t cnt = 1;
    pmix_cb_t *cb = (pmix_cb_t *) cbdata;
    PMIX_HIDE_UNUSED_PARAMS(pr, hdr);

    /* unpack the nspace - should be same as our own */
    PMIX_BFROPS_UNPACK(rc, pmix_client_globals.myserver, buf, &nspace, &cnt, PMIX_STRING);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        cb->status = PMIX_ERROR;
        PMIX_POST_OBJECT(cb);
        PMIX_WAKEUP_THREAD(&cb->lock);
        return;
    }

    /* decode it */
    PMIX_GDS_STORE_JOB_INFO(cb->status, pmix_client_globals.myserver, nspace, buf);
    cb->status = PMIX_SUCCESS;
    PMIX_POST_OBJECT(cb);
    PMIX_WAKEUP_THREAD(&cb->lock);
}

/* Event handler registration callback.
 *
 * The registration path (pmix_internal_reg_event_hdlr) invokes this with
 * cd->cbdata, and the only caller here sets cd->cbdata to the caddy
 * itself - so cbdata is the pmix_rshift_caddy_t, NOT a bare pmix_lock_t.
 * Casting it to a lock would write the status over the object header and
 * take a "mutex" that is really part of that header, and it would leave
 * the caddy's own lock - the one the caller blocks on - untouched, so the
 * init would hang. Mirror the internal mycbfn()/src/server/pmix_server.c
 * version: record the handler index (or the failure) as the status and
 * wake the caddy's lock. */
static void evhandler_reg_callbk(pmix_status_t status, size_t evhandler_ref, void *cbdata)
{
    pmix_rshift_caddy_t *cd = (pmix_rshift_caddy_t *) cbdata;

    PMIX_ACQUIRE_OBJECT(cd);
    if (PMIX_SUCCESS == status) {
        cd->status = evhandler_ref;
    } else {
        cd->status = status;
    }
    PMIX_WAKEUP_THREAD(&cd->lock);
}

static void notification_fn(size_t evhdlr_registration_id, pmix_status_t status,
                            const pmix_proc_t *source, pmix_info_t info[], size_t ninfo,
                            pmix_info_t results[], size_t nresults,
                            pmix_event_notification_cbfunc_fn_t cbfunc, void *cbdata)
{
    pmix_lock_t *lock = NULL;
    char *name = NULL;
    size_t n;
    PMIX_HIDE_UNUSED_PARAMS(evhdlr_registration_id, status, source, results, nresults);

    pmix_output_verbose(2, pmix_client_globals.base_output,
                        "[%s:%d] DEBUGGER RELEASE RECVD",
                        pmix_globals.myid.nspace, pmix_globals.myid.rank);
    if (NULL != info) {
        lock = NULL;
        for (n = 0; n < ninfo; n++) {
            if (0 == strncmp(info[n].key, PMIX_EVENT_RETURN_OBJECT, PMIX_MAX_KEYLEN)) {
                lock = (pmix_lock_t *) info[n].value.data.ptr;
            } else if (0 == strncmp(info[n].key, PMIX_EVENT_HDLR_NAME, PMIX_MAX_KEYLEN)) {
                name = info[n].value.data.string;
            }
        }
        /* if the object wasn't returned, then that is an error */
        if (NULL == lock) {
            pmix_output_verbose(2, pmix_client_globals.base_output,
                                "event handler %s failed to return object",
                                (NULL == name) ? "NULL" : name);
            /* let the event handler progress */
            if (NULL != cbfunc) {
                cbfunc(PMIX_SUCCESS, NULL, 0, NULL, NULL, cbdata);
            }
            return;
        }
    }
    if (NULL != lock) {
        PMIX_WAKEUP_THREAD(lock);
    }

    if (NULL != cbfunc) {
        cbfunc(PMIX_EVENT_ACTION_COMPLETE, NULL, 0, NULL, NULL, cbdata);
    }
}

static int tool_init_cntr = 0;

PMIX_EXPORT int PMIx_tool_init(pmix_proc_t *proc, pmix_info_t info[], size_t ninfo)
{
    pmix_status_t rc;
    char *evar, *nspace = NULL, *suri;
    pmix_rank_t rank = PMIX_RANK_UNDEF;
    bool do_not_connect = false;
    bool nspace_given = false;
    bool nspace_in_enviro = false;
    bool rank_given = false;
    bool fwd_stdin = false;
    bool connect_optional = false;
    pmix_info_t ginfo, *iptr, evinfo[3];
    size_t n;
    pmix_ptl_posted_recv_t *rcv;
    pmix_proc_t wildcard, myserver;
    int fd;
    pmix_proc_type_t ptype = PMIX_PROC_TYPE_STATIC_INIT;
    pmix_cb_t cb, *cbptr;
    pmix_buffer_t *req;
    pmix_cmd_t cmd;
    pmix_iof_req_t *iofreq;
    pmix_lock_t releaselock;
    bool outputio = true;
    pmix_kval_t *kptr;
    pmix_rshift_caddy_t *cd;

    // check if an init has been called
    if (pmix_atomic_test_and_set(&pmix_globals.init_called)) {
        // track the ref count
        pmix_atomic_fetch_add(&tool_init_cntr, 1);
        // did the prior call get far enough? We might be in a tight
        // race between multiple calls to PMIx_tool_init - bad programming
        // technique, but all we can do is try to protect against it
        if (!pmix_atomic_check_bool(&pmix_globals.initialized)) {
            return PMIX_ERR_INIT;
        }
       // return our proc name if they requested it
        if (NULL != proc) {
            PMIX_LOAD_PROCID(proc, pmix_globals.myid.nspace, pmix_globals.myid.rank);
        }
        return PMIX_SUCCESS;

    } else {
        // require a location to return the proc name
        if (NULL == proc) {
            return PMIX_ERR_BAD_PARAM;
        }
        pmix_atomic_fetch_add(&tool_init_cntr, 1);
    }

    /* init the parent procid to something innocuous */
    PMIX_LOAD_PROCID(&myparent, NULL, PMIX_RANK_UNDEF);

    /* backward compatibility fix - remove any directive to use
     * the old usock component so we avoid a warning message */
    if (NULL != (evar = getenv("PMIX_MCA_ptl"))) {
        if (0 == strcmp(evar, "usock")) {
            /* we cannot support a usock-only environment */
            fprintf(stderr,
                    "-------------------------------------------------------------------\n");
            fprintf(stderr, "PMIx no longer supports the \"usock\" transport for client-server\n");
            fprintf(stderr,
                    "communication. A directive was detected that only allows that mode.\n");
            fprintf(stderr, "We cannot continue - please remove that constraint and try again.\n");
            fprintf(stderr,
                    "-------------------------------------------------------------------\n");
            return PMIX_ERR_INIT;
        }
        /* anything else should just be cleared */
        pmix_unsetenv("PMIX_MCA_ptl", &environ);
    }

    /* parse the input directives */
    PMIX_SET_PROC_TYPE(&ptype, PMIX_PROC_TOOL);
    if (NULL != info) {
        for (n = 0; n < ninfo; n++) {
            if (PMIX_CHECK_KEY(&info[n], PMIX_TOOL_DO_NOT_CONNECT)) {
                do_not_connect = PMIX_INFO_TRUE(&info[n]);
            } else if (PMIX_CHECK_KEY(&info[n], PMIX_TOOL_NSPACE)) {
                if (NULL != nspace) {
                    /* cannot define it twice */
                    free(nspace);
                    return PMIX_ERR_BAD_PARAM;
                }
                /* screen the value before strdup'ing it: a directive that
                 * names a string and carries none is an application error,
                 * and must be reported as one rather than dereferenced */
                if (PMIX_STRING != info[n].value.type || NULL == info[n].value.data.string) {
                    PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
                    return PMIX_ERR_BAD_PARAM;
                }
                nspace = strdup(info[n].value.data.string);
                nspace_given = true;
            } else if (PMIX_CHECK_KEY(&info[n], PMIX_TOOL_RANK)) {
                rank = info[n].value.data.rank;
                rank_given = true;
            } else if (PMIX_CHECK_KEY(&info[n], PMIX_FWD_STDIN)) {
                /* they want us to forward our stdin to someone */
                fwd_stdin = PMIX_INFO_TRUE(&info[n]);
            } else if (PMIX_CHECK_KEY(&info[n], PMIX_LAUNCHER)) {
                if (PMIX_INFO_TRUE(&info[n])) {
                    PMIX_SET_PROC_TYPE(&ptype, PMIX_PROC_LAUNCHER);
                }
            } else if (PMIX_CHECK_KEY(&info[n], PMIX_SERVER_SCHEDULER)) {
                if (PMIX_INFO_TRUE(&info[n])) {
                    PMIX_SET_PROC_TYPE(&ptype, PMIX_PROC_SCHEDULER);
                }
            } else if (PMIX_CHECK_KEY(&info[n], PMIX_SERVER_TMPDIR)) {
                /* same screen as PMIX_TOOL_NSPACE above - the guards below
                 * only refill these when they are NULL, so a bad value has
                 * to be refused rather than left half-applied */
                if (PMIX_STRING != info[n].value.type || NULL == info[n].value.data.string) {
                    PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
                    if (NULL != nspace) {
                        free(nspace);
                    }
                    return PMIX_ERR_BAD_PARAM;
                }
                if (NULL != pmix_server_globals.tmpdir) {
                    free(pmix_server_globals.tmpdir);
                }
                pmix_server_globals.tmpdir = strdup(info[n].value.data.string);
            } else if (PMIX_CHECK_KEY(&info[n], PMIX_SYSTEM_TMPDIR)) {
                if (PMIX_STRING != info[n].value.type || NULL == info[n].value.data.string) {
                    PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
                    if (NULL != nspace) {
                        free(nspace);
                    }
                    return PMIX_ERR_BAD_PARAM;
                }
                if (NULL != pmix_server_globals.system_tmpdir) {
                    free(pmix_server_globals.system_tmpdir);
                }
                pmix_server_globals.system_tmpdir = strdup(info[n].value.data.string);
            } else if (PMIX_CHECK_KEY(&info[n], PMIX_TOOL_CONNECT_OPTIONAL)) {
                connect_optional = PMIX_INFO_TRUE(&info[n]);
            } else if (PMIX_CHECK_KEY(&info[n], PMIX_IOF_LOCAL_OUTPUT)) {
                outputio = PMIX_INFO_TRUE(&info[n]);
            }
        }
    }
    if (NULL == pmix_server_globals.tmpdir) {
        if (NULL == (evar = getenv("PMIX_SERVER_TMPDIR"))) {
            pmix_server_globals.tmpdir = strdup(pmix_tmp_directory());
        } else {
            pmix_server_globals.tmpdir = strdup(evar);
        }
    }
    if (NULL == pmix_server_globals.system_tmpdir) {
        if (NULL == (evar = getenv("PMIX_SYSTEM_TMPDIR"))) {
            pmix_server_globals.system_tmpdir = strdup(pmix_tmp_directory());
        } else {
            pmix_server_globals.system_tmpdir = strdup(evar);
        }
    }

    if ((nspace_given && !rank_given) || (!nspace_given && rank_given)) {
        /* can't have one and not the other */
        PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
        if (NULL != nspace) {
            free(nspace);
        }
        return PMIX_ERR_BAD_PARAM;
    }

    /* if we were not passed an nspace in the info keys,
     * check to see if we were given one in the env - this
     * will be the case when we are launched by a PMIx-enabled
     * daemon */
    if (!nspace_given) {
        if (NULL != (evar = getenv("PMIX_NAMESPACE"))) {
            nspace = strdup(evar);
            nspace_in_enviro = true;
        }
    }
    /* also look for the rank - it normally is zero, but if we
     * were launched, then it might have been as part of a
     * multi-process tool */
    if (!rank_given) {
        if (NULL != (evar = getenv("PMIX_RANK"))) {
            rank = strtol(evar, NULL, 10);
            if (!nspace_in_enviro) {
                /* this is an error - we can't have one and not
                 * the other */
                PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
                return PMIX_ERR_BAD_PARAM;
            }
            /* flag that this tool is also a client */
            if (PMIX_PROC_IS_LAUNCHER(&ptype)) {
                PMIX_SET_PROC_TYPE(&ptype, PMIX_PROC_CLIENT_LAUNCHER);
            } else {
                PMIX_SET_PROC_TYPE(&ptype, PMIX_PROC_CLIENT_TOOL);
            }
        } else if (nspace_in_enviro) {
            /* this is an error - we can't have one and not
             * the other */
            PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
            if (NULL != nspace) {
                free(nspace);
            }
            return PMIX_ERR_BAD_PARAM;
        }
    }

    /* setup the runtime - this init's the globals,
     * opens and initializes the required frameworks */
    if (PMIX_SUCCESS != (rc = pmix_rte_init(ptype.type, info, ninfo, pmix_tool_notify_recv))) {
        PMIX_ERROR_LOG(rc);
        if (NULL != nspace) {
            free(nspace);
        }
        return rc;
    }

    /* if we were given a keepalive pipe, register an
     * event to capture the event */
    if (NULL != (evar = getenv("PMIX_KEEPALIVE_PIPE"))) {
        keepalive_fd = strtol(evar, NULL, 10);
        pmix_event_set(pmix_globals.evbase, &parentdied, keepalive_fd, PMIX_EV_READ,
                       pdiedfn, NULL);
        pmix_event_add(&parentdied, NULL);
        pmix_unsetenv("PMIX_KEEPALIVE_PIPE", &environ);
        pmix_fd_set_cloexec(keepalive_fd); // don't let children inherit this
    }

    /* if we were given a name, then set it now */
    if (nspace_given || nspace_in_enviro) {
        PMIX_LOAD_PROCID(&pmix_globals.myid, nspace, rank);
        free(nspace);
        nspace = NULL;
    }

    /* setup the IO Forwarding recv */
    rcv = PMIX_NEW(pmix_ptl_posted_recv_t);
    rcv->tag = PMIX_PTL_TAG_IOF;
    rcv->cbfunc = tool_iof_handler;
    /* add it to the end of the list of recvs */
    pmix_list_append(&pmix_ptl_base.posted_recvs, &rcv->super);
    /* and the IOF flow control recv - this is how our server tells us
     * to stop reading the stdin we are pushing to it */
    rcv = PMIX_NEW(pmix_ptl_posted_recv_t);
    rcv->tag = PMIX_PTL_TAG_IOF_CONTROL;
    rcv->cbfunc = pmix_iof_flow_control_handler;
    pmix_list_append(&pmix_ptl_base.posted_recvs, &rcv->super);
    /* default tools to outputting their IOF */
    pmix_globals.iof_flags.local_output = outputio;

    /* setup the globals */
    PMIX_CONSTRUCT(&pmix_client_globals.groups, pmix_list_t);
    PMIX_CONSTRUCT(&pmix_client_globals.pending_requests, pmix_list_t);
    PMIX_CONSTRUCT(&pmix_client_globals.peers, pmix_pointer_array_t);
    pmix_pointer_array_init(&pmix_client_globals.peers, 1, INT_MAX, 1);
    pmix_client_globals.myserver = PMIX_NEW(pmix_peer_t);
    if (NULL == pmix_client_globals.myserver) {
        return PMIX_ERR_NOMEM;
    }
    pmix_client_globals.myserver->nptr = PMIX_NEW(pmix_namespace_t);
    if (NULL == pmix_client_globals.myserver->nptr) {
        PMIX_RELEASE(pmix_client_globals.myserver);
        return PMIX_ERR_NOMEM;
    }
    pmix_client_globals.myserver->info = PMIX_NEW(pmix_rank_info_t);
    if (NULL == pmix_client_globals.myserver->info) {
        PMIX_RELEASE(pmix_client_globals.myserver);
        return PMIX_ERR_NOMEM;
    }

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix: init called");

    /* setup a rank_info object for us */
    pmix_globals.mypeer->info = PMIX_NEW(pmix_rank_info_t);
    if (NULL == pmix_globals.mypeer->info) {
        return PMIX_ERR_NOMEM;
    }
    if (PMIX_PEER_IS_CLIENT(pmix_globals.mypeer)) {
        /* if we are a client, then we need to pickup the
         * rest of the envar-based server assignments */
        pmix_globals.pindex = -1;
        pmix_globals.mypeer->info->pname.nspace = strdup(pmix_globals.myid.nspace);
        pmix_globals.mypeer->info->pname.rank = pmix_globals.myid.rank;
    }
    pmix_globals.mypeer->info->realuid = pmix_globals.realuid;
    pmix_globals.mypeer->info->uid = pmix_globals.uid;
    pmix_globals.mypeer->info->realgid = pmix_globals.realgid;
    pmix_globals.mypeer->info->gid = pmix_globals.gid;
    pmix_globals.mypeer->info->pid = pmix_globals.pid;

    /* select our bfrops compat module */
    pmix_globals.mypeer->nptr->compat.bfrops = pmix_bfrops_base_assign_module(NULL);
    if (NULL == pmix_globals.mypeer->nptr->compat.bfrops) {
        return PMIX_ERR_INIT;
    }
    /* the server will be using the same */
    pmix_client_globals.myserver->nptr->compat.bfrops = pmix_globals.mypeer->nptr->compat.bfrops;

    /* select our psec compat module - the selection may be based
     * on the corresponding envars that should have been passed
     * to us at launch */
    evar = getenv("PMIX_SECURITY_MODE");
    pmix_globals.mypeer->nptr->compat.psec = pmix_psec_base_assign_module(evar);
    if (NULL == pmix_globals.mypeer->nptr->compat.psec) {
        return PMIX_ERR_INIT;
    }
    /* the server will be using the same */
    pmix_client_globals.myserver->nptr->compat.psec = pmix_globals.mypeer->nptr->compat.psec;

    /* set the buffer type - the selection will be based
     * on the corresponding envars that should have been passed
     * to us at launch */
    evar = getenv("PMIX_BFROP_BUFFER_TYPE");
    if (NULL == evar) {
        /* just set to our default */
        pmix_globals.mypeer->nptr->compat.type = pmix_bfrops_globals.default_type;
    } else if (0 == strcmp(evar, "PMIX_BFROP_BUFFER_FULLY_DESC")) {
        pmix_globals.mypeer->nptr->compat.type = PMIX_BFROP_BUFFER_FULLY_DESC;
    } else {
        pmix_globals.mypeer->nptr->compat.type = PMIX_BFROP_BUFFER_NON_DESC;
    }
    /* the server will be using the same */
    pmix_client_globals.myserver->nptr->compat.type = pmix_globals.mypeer->nptr->compat.type;

    /* tools are restricted to the "hash" component for interacting
     * with a server's GDS framework */
    PMIX_INFO_LOAD(&ginfo, PMIX_GDS_MODULE, "hash", PMIX_STRING);
    pmix_globals.mypeer->nptr->compat.gds = pmix_gds_base_assign_module(&ginfo, 1);
    PMIX_INFO_DESTRUCT(&ginfo);
    if (NULL == pmix_globals.mypeer->nptr->compat.gds) {
        return PMIX_ERR_INIT;
    }
    pmix_client_globals.myserver->nptr->compat.gds = pmix_globals.mypeer->nptr->compat.gds;

    /* tools can, in some scenarios, act as servers,
     * so initialize the server globals too */
    if (PMIX_SUCCESS != (rc = pmix_server_initialize())) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    /* Setup the function pointers. The "have we been given a module"
     * latch has to be cleared with them: it is a global that no finalize
     * resets, so a tool that cycles init -> set_server_module ->
     * finalize -> init would find the latch still set and
     * PMIx_tool_set_server_module would refuse - leaving us claiming to
     * have a host module while the table we would call through is the
     * all-zero one we just wrote. */
    memset(&pmix_host_server, 0, sizeof(pmix_server_module_t));
    pmix_server_globals.module_set = false;

    if (do_not_connect) {
        /* ensure we mark that we are not connected */
        pmix_atomic_unset_bool(&pmix_globals.connected);
        /* it is not an error if we were not given an nspace/rank */
        if (!nspace_given || !rank_given) {
            /* self-assign a namespace and rank for ourselves. Use our hostname:pid
             * for the nspace, and rank clearly is 0 */
            pmix_snprintf(pmix_globals.myid.nspace, PMIX_MAX_NSLEN - 1, "%s:%lu", pmix_globals.hostname,
                     (unsigned long) pmix_globals.pid);
            pmix_globals.myid.rank = 0;
            nspace_given = false;
            rank_given = false;
            /* also setup the client myserver to point to ourselves. Fill in
             * the rank_info object we already allocated above rather than
             * allocating a second one - a fresh PMIX_NEW here would orphan
             * the first (the peer destructor only releases the pointer it
             * finds, so the original object would leak) */
            pmix_client_globals.myserver->nptr->nspace = strdup(pmix_globals.myid.nspace);
            if (NULL != pmix_client_globals.myserver->info->pname.nspace) {
                free(pmix_client_globals.myserver->info->pname.nspace);
            }
            pmix_client_globals.myserver->info->pname.nspace = strdup(pmix_globals.myid.nspace);
            pmix_client_globals.myserver->info->pname.rank = pmix_globals.myid.rank;
            pmix_client_globals.myserver->info->uid = pmix_globals.uid;
            pmix_client_globals.myserver->info->gid = pmix_globals.gid;
            // set the type of the server to be our own
            PMIX_SET_PEER_TYPE(pmix_client_globals.myserver, ptype.type);
        }
    } else {
        /* connect to the server */
        rc = pmix_ptl.connect_to_peer((struct pmix_peer_t *) pmix_client_globals.myserver, info,
                                      ninfo, &suri);
        if (PMIX_SUCCESS == rc) {
            /* store the URI for subsequent lookups */
            PMIX_KVAL_NEW(kptr, PMIX_SERVER_URI);
            kptr->value->type = PMIX_STRING;
            pmix_asprintf(&kptr->value->data.string, "%s.%u;%s",
                          pmix_client_globals.myserver->info->pname.nspace,
                          pmix_client_globals.myserver->info->pname.rank, suri);
            free(suri);
            PMIX_GDS_STORE_KV(rc, pmix_globals.mypeer, &pmix_globals.myid, PMIX_INTERNAL, kptr);
            PMIX_RELEASE(kptr); // maintain accounting
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                return rc;
            }
        } else {
            /* if connection wasn't optional, then error out */
            if (!connect_optional) {
                return rc;
            }
            /* if connection was optional, then we need to self-assign
             * a namespace and rank for ourselves. Use our hostname:pid
             * for the nspace, and rank clearly is 0 */
            pmix_snprintf(pmix_globals.myid.nspace, PMIX_MAX_NSLEN - 1, "%s:%lu", pmix_globals.hostname,
                     (unsigned long) pmix_globals.pid);
            pmix_globals.myid.rank = 0;
            nspace_given = false;
            rank_given = false;
            /* also setup the client myserver to point to ourselves. Fill in
             * the rank_info object we already allocated above rather than
             * allocating a second one - a fresh PMIX_NEW here would orphan
             * the first (the peer destructor only releases the pointer it
             * finds, so the original object would leak) */
            pmix_client_globals.myserver->nptr->nspace = strdup(pmix_globals.myid.nspace);
            if (NULL != pmix_client_globals.myserver->info->pname.nspace) {
                free(pmix_client_globals.myserver->info->pname.nspace);
            }
            pmix_client_globals.myserver->info->pname.nspace = strdup(pmix_globals.myid.nspace);
            pmix_client_globals.myserver->info->pname.rank = pmix_globals.myid.rank;
            pmix_client_globals.myserver->info->uid = pmix_globals.uid;
            pmix_client_globals.myserver->info->gid = pmix_globals.gid;
            // set the type of the server to be our own
            PMIX_SET_PEER_TYPE(pmix_client_globals.myserver, ptype.type);
            /* ensure we are marked as not connected - the flag is a global
             * that a prior init/finalize cycle in this process may have
             * left set */
            pmix_atomic_unset_bool(&pmix_globals.connected);
            /* mark us as not connecting to avoid asking for our job info */
            do_not_connect = true;
        }
    }

    /* setup the wildcard ID */
    PMIX_LOAD_PROCID(&wildcard, pmix_globals.myid.nspace, PMIX_RANK_WILDCARD);
    /* pass back the ID */
    PMIX_LOAD_PROCID(proc, pmix_globals.myid.nspace, pmix_globals.myid.rank);
    /* Cache the server in case we later want to call "set_server" on it.
     * Note the guard: on the do-not-connect and failed-optional-connect
     * paths "myserver" is a stand-in peer for *ourselves*, not a server we
     * reached, and the clients array is the tool's list of known servers.
     * Caching ourselves there makes PMIx_tool_get_servers report us as our
     * own server (the Standard says report PMIX_ERR_UNREACH when there is
     * none), and - worse - that stand-in may carry a NULL nspace, which
     * PMIX_CHECK_NSPACE treats as a wildcard, so it would match the first
     * lookup that disc()/pmix_tool_retry_set() performed for any name.
     * pmix_tool_retry_set has its own "switching back to me" branch and
     * does not need to find us in the array. The retain pairs with the
     * per-entry release in PMIx_tool_finalize; when we skip both, the
     * separate myserver release there still balances the PMIX_NEW. */
    if (pmix_atomic_check_bool(&pmix_globals.connected)) {
        PMIX_RETAIN(pmix_client_globals.myserver);
        pmix_pointer_array_add(&pmix_server_globals.clients, pmix_client_globals.myserver);
    }

    /* load into our own peer object */
    if (NULL == pmix_globals.mypeer->nptr->nspace) {
        pmix_globals.mypeer->nptr->nspace = strdup(pmix_globals.myid.nspace);
    }
    /* finish setting up the rank_info object we already allocated above,
     * now that our identity has been finalized (connect/self-assign may
     * have changed it). Reuse that object rather than allocating a second
     * one - a fresh PMIX_NEW here would leak the first and discard the
     * uid/gid/pid already stored on it. */
    if (NULL != pmix_globals.mypeer->info->pname.nspace) {
        free(pmix_globals.mypeer->info->pname.nspace);
    }
    pmix_globals.mypeer->info->pname.nspace = strdup(pmix_globals.myid.nspace);
    pmix_globals.mypeer->info->pname.rank = pmix_globals.myid.rank;
    /* record our identity in the pre-built values PMIx_Get hands back when
     * asked for PMIX_PROCID/PMIX_RANK with PMIX_GET_POINTER_VALUES. These
     * are allocated by pmix_rte_init with a NULL nspace and RANK_INVALID;
     * without this the tool answers those two queries with garbage. */
    PMIX_LOAD_PROCID(pmix_globals.myidval.data.proc, pmix_globals.myid.nspace,
                     pmix_globals.myid.rank);
    pmix_globals.myrankval.data.rank = pmix_globals.myid.rank;
    /* if we are acting as a server, then setup the global recv */
    if (PMIX_PEER_IS_LAUNCHER(pmix_globals.mypeer) ||
        PMIX_PEER_IS_SCHEDULER(pmix_globals.mypeer)) {
        /* setup the wildcard recv for inbound messages from clients */
        rcv = PMIX_NEW(pmix_ptl_posted_recv_t);
        rcv->tag = UINT32_MAX;
        rcv->cbfunc = pmix_server_message_handler;
        /* add it to the end of the list of recvs */
        pmix_list_append(&pmix_ptl_base.posted_recvs, &rcv->super);
    }

    /* open the pmdl framework and select the active modules for this environment
     * as we might need them if we are asking a server to launch something for us */
    rc = pmix_mca_base_framework_open(&pmix_pmdl_base_framework,
                                      PMIX_MCA_BASE_OPEN_DEFAULT);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    if (PMIX_SUCCESS != (rc = pmix_pmdl_base_select())) {
        return rc;
    }

    /* setup IOF */
    PMIX_IOF_SINK_DEFINE(&pmix_client_globals.iof_stdout, &pmix_globals.myid, 1,
                         PMIX_FWD_STDOUT_CHANNEL, pmix_iof_write_handler);
    PMIX_IOF_SINK_DEFINE(&pmix_client_globals.iof_stderr, &pmix_globals.myid, 2,
                         PMIX_FWD_STDERR_CHANNEL, pmix_iof_write_handler);
    /* create the default iof handler */
    iofreq = PMIX_NEW(pmix_iof_req_t);
    iofreq->channels = PMIX_FWD_STDOUT_CHANNEL | PMIX_FWD_STDERR_CHANNEL | PMIX_FWD_STDDIAG_CHANNEL;
    pmix_pointer_array_set_item(&pmix_globals.iof_requests, 0, iofreq);

    if (fwd_stdin) {
        /* Hand this to the library rather than building a read event of
         * our own. This file used to keep a private one, and every way
         * that could drift from src/common/pmix_iof.c's copy, it did: its
         * SIGCONT event went un-added so a backgrounded tool never
         * resumed reading, its teardown had to be bolted on separately,
         * and - the two that a local fix cannot reach -
         * pmix_iof_flow_control only knows the library's stdinev_global,
         * so nothing could suspend a tool's own stdin, and a later
         * PMIx_IOF_push would build a *second* read event on fd 0 that
         * stole bytes from the first. One read event per descriptor, and
         * one place that knows how to build it. */
        fd = fileno(stdin);
        rc = pmix_iof_setup_stdin_read(fd, NULL, 0, NULL, 0);
        if (PMIX_SUCCESS != rc) {
            return rc;
        }
    }

    /* fill in our local
     * datastore with typical job-related info. No point
     * in having the server generate these as we are
     * obviously a singleton, and so the values are well-known */
    rc = pmix_tool_init_info();
    if (PMIX_SUCCESS != rc) {
        return rc;
    }

    /* if we are connected, then send a request for our
     * job info - we do this as a non-blocking
     * transaction because some systems cannot handle very large
     * blocking operations and error out if we try them. */
    if (!do_not_connect && !PMIX_PEER_IS_SCHEDULER(pmix_client_globals.myserver)) {
        req = PMIX_NEW(pmix_buffer_t);
        cmd = PMIX_REQ_CMD;
        PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, req, &cmd, 1, PMIX_COMMAND);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(req);
            return rc;
        }
        /* send to the server */
        PMIX_CONSTRUCT(&cb, pmix_cb_t);
        PMIX_PTL_SEND_RECV(rc, pmix_client_globals.myserver, req, job_data, (void *) &cb);
        if (PMIX_SUCCESS != rc) {
            /* the transport refused the message without taking it, so the
             * recv callback will never fire - unwind both here */
            PMIX_RELEASE(req);
            PMIX_DESTRUCT(&cb);
            return rc;
        }
        /* wait for the data to return */
        PMIX_WAIT_THREAD(&cb.lock);
        rc = cb.status;
        PMIX_DESTRUCT(&cb);
        if (PMIX_SUCCESS != rc) {
            return rc;
        }
        /* quick check to see if we got something back. If this
         * is a launcher that is being executed multiple times
         * in a job-script, then the original registration data
         * may have been deleted after the first invocation. In
         * such a case, we simply regenerate it locally as it is
         * well-known */
        PMIX_CONSTRUCT(&cb, pmix_cb_t);
        cb.proc = &wildcard;
        cb.copy = true;
        PMIX_GDS_FETCH_KV(rc, pmix_globals.mypeer, &cb);
        if (PMIX_SUCCESS != rc) {
            pmix_output_verbose(5, pmix_client_globals.get_output,
                                "pmix:tool:client data not found in internal storage");
            rc = pmix_tool_init_info();
            if (PMIX_SUCCESS != rc) {
                PMIX_DESTRUCT(&cb);
                return rc;
            }
        }
        PMIX_DESTRUCT(&cb);
    }

    // enable show_help subsystem
    pmix_atomic_store_int(&pmix_show_help_enabled, 1);

    /* if we are acting as a server, then start listening
     * and register the server receive */
    if (PMIX_PEER_IS_LAUNCHER(pmix_globals.mypeer) ||
        PMIX_PEER_IS_SCHEDULER(pmix_globals.mypeer)) {
        /* setup the fork/exec framework */
        rc = pmix_pfexec_base_open();
        if (PMIX_SUCCESS != rc) {
            return rc;
        }

        /* if we don't know our topology, we better get it now as we
         * increasingly rely on it - note that our host will hopefully
         * have passed it to us so we don't duplicate their storage! */
        if (PMIX_SUCCESS != (rc = pmix_hwloc_setup_topology(info, ninfo))) {
            return rc;
        }

        /* open the pnet framework and select the active modules for this environment */
        rc = pmix_mca_base_framework_open(&pmix_pnet_base_framework,
                                          PMIX_MCA_BASE_OPEN_DEFAULT);
        if (PMIX_SUCCESS != rc) {
            return rc;
        }
        if (PMIX_SUCCESS != (rc = pmix_pnet_base_select())) {
            return rc;
        }

        /* start listening for connections */
        rc = pmix_ptl_base_start_listening(info, ninfo);
        if (PMIX_SUCCESS != rc) {
            if (PMIX_ERR_SILENT != rc) {
                pmix_show_help("help-pmix-server.txt", "listener-thread-start", true);
            }
            return PMIX_ERR_INIT;
        }
    }

    /* see if they gave us a rendezvous URI to which we are to call back */
    evar = getenv("PMIX_LAUNCHER_RNDZ_URI");
    if (NULL != evar) {
        /* Attach to the specified server so it can tell us what we are to
         * do - save our current server first, so we can go back to it once
         * the debugger releases us.
         *
         * If we have no server, "back to it" means back to ourselves, and
         * we have to say so with our own id. The stand-in peer that
         * myserver points at on the do-not-connect path may carry no name
         * at all (the self-assign block is skipped when the caller
         * supplied PMIX_TOOL_NSPACE/RANK), and PMIX_CHECK_NSPACE treats an
         * empty nspace as a wildcard - so restoring by that name would
         * match whichever server the array happened to hold first, and
         * quietly leave the debugger as our primary. */
        if (pmix_atomic_check_bool(&pmix_globals.connected)) {
            PMIX_LOAD_PROCID(&myserver, pmix_client_globals.myserver->info->pname.nspace,
                             pmix_client_globals.myserver->info->pname.rank);
        } else {
            PMIX_LOAD_PROCID(&myserver, pmix_globals.myid.nspace, pmix_globals.myid.rank);
        }
        PMIX_INFO_CREATE(iptr, 3);
        PMIX_INFO_LOAD(&iptr[0], PMIX_SERVER_URI, evar, PMIX_STRING);
        rc = 2; // give us two seconds to connect
        PMIX_INFO_LOAD(&iptr[1], PMIX_TIMEOUT, &rc, PMIX_INT);
        PMIX_INFO_LOAD(&iptr[2], PMIX_PRIMARY_SERVER, NULL, PMIX_BOOL);
        cbptr = PMIX_NEW(pmix_cb_t);
        cbptr->info = iptr;
        cbptr->ninfo = 3;
        /* the caddy's destructor releases its info array only when
         * infocopy says the array is its own - say so, or the three
         * directives (one of them carrying a strdup'd URI) are leaked */
        cbptr->infocopy = true;
        PMIX_THREADSHIFT(cbptr, pmix_tool_retry_attach);

        PMIX_WAIT_THREAD(&cbptr->lock);
        rc = cbptr->status;
        if (PMIX_SUCCESS == rc && NULL != cbptr->pname.nspace) {
            /* The attach is the only place the parent's identity ever
             * appears - pmix_tool_retry_attach reports it here and
             * nowhere else, and the caddy carrying it is released on the
             * next line. Nothing else assigned "myparent", so the
             * PMIX_PARENT_ID stored just below was a proc with an empty
             * namespace and PMIX_RANK_UNDEF: every PMIx_Get of that key
             * on this process answered successfully with nobody. */
            PMIX_LOAD_PROCID(&myparent, cbptr->pname.nspace, cbptr->pname.rank);
        }
        PMIX_RELEASE(cbptr);

        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            return PMIX_ERR_UNREACH;
        }

        /* save our parent ID - copy it into a heap-allocated proc so the
         * kval destructor's PMIX_RELEASE below frees owned memory rather
         * than the file-scope "myparent" static */
        PMIX_KVAL_NEW(kptr, PMIX_PARENT_ID);
        kptr->value->type = PMIX_PROC;
        PMIX_PROC_CREATE(kptr->value->data.proc, 1);
        PMIx_Proc_load(kptr->value->data.proc, myparent.nspace, myparent.rank);
        PMIX_GDS_STORE_KV(rc, pmix_globals.mypeer, &pmix_globals.myid, PMIX_INTERNAL, kptr);
        PMIX_RELEASE(kptr); // maintain accounting
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            return rc;
        }

        /* retrieve any job info it has for us */
        req = PMIX_NEW(pmix_buffer_t);
        cmd = PMIX_REQ_CMD;
        PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, req, &cmd, 1, PMIX_COMMAND);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(req);
            return rc;
        }
        /* send to the server */
        PMIX_CONSTRUCT(&cb, pmix_cb_t);
        PMIX_PTL_SEND_RECV(rc, pmix_client_globals.myserver, req, job_data, (void *) &cb);
        if (PMIX_SUCCESS != rc) {
            /* the transport refused the message without taking it, so the
             * recv callback will never fire - unwind both here */
            PMIX_RELEASE(req);
            PMIX_DESTRUCT(&cb);
            return rc;
        }
        /* wait for the data to return */
        PMIX_WAIT_THREAD(&cb.lock);
        rc = cb.status;
        PMIX_DESTRUCT(&cb);
        if (PMIX_SUCCESS != rc) {
            return rc;
        }

        /* if the value was found, then we need to wait for debugger attach here */
        /* register for the debugger release notification */
        PMIX_CONSTRUCT_LOCK(&releaselock);
        PMIX_INFO_LOAD(&evinfo[0], PMIX_EVENT_RETURN_OBJECT, &releaselock, PMIX_POINTER);
        PMIX_INFO_LOAD(&evinfo[1], PMIX_EVENT_HDLR_NAME, "WAIT-FOR-RELEASE", PMIX_STRING);
        PMIX_INFO_LOAD(&evinfo[2], PMIX_EVENT_ONESHOT, NULL, PMIX_BOOL);
        pmix_output_verbose(2, pmix_client_globals.event_output,
                            "[%s:%d] WAITING IN INIT FOR RELEASE", pmix_globals.myid.nspace,
                            pmix_globals.myid.rank);
        cd = PMIX_NEW(pmix_rshift_caddy_t);
        cd->codes = malloc(sizeof(int));
        cd->codes[0] = PMIX_DEBUGGER_RELEASE;
        cd->ncodes = 1;
        cd->info = evinfo;
        cd->ninfo = 3;
        cd->evhdlr = notification_fn;
        cd->evregcbfn = evhandler_reg_callbk;
        cd->cbdata = cd;
        PMIX_RETAIN(cd); // so pmix_internal_reg_event_hdlr doesn't wind up releasing it
        PMIX_THREADSHIFT(cd, pmix_internal_reg_event_hdlr);
        PMIX_WAIT_THREAD(&cd->lock);
        rc = cd->status;
        PMIX_RELEASE(cd);
        PMIX_INFO_DESTRUCT(&evinfo[0]);
        PMIX_INFO_DESTRUCT(&evinfo[1]);
        PMIX_INFO_DESTRUCT(&evinfo[2]);

        if (0 > rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DESTRUCT_LOCK(&releaselock);
            return rc;
        }

        /* wait for release to arrive */
        PMIX_WAIT_THREAD(&releaselock);
        PMIX_DESTRUCT_LOCK(&releaselock);

        /* restore our original primary server */
        cbptr = PMIX_NEW(pmix_cb_t);
        cbptr->proc = &myserver;
        PMIX_THREADSHIFT(cbptr, pmix_tool_retry_set);

        /* wait for completion */
        PMIX_WAIT_THREAD(&cbptr->lock);
        rc = cbptr->status;
        PMIX_RELEASE(cbptr);
        if (PMIX_SUCCESS != rc) {
            return rc;
        }

    }

    /* register the tool supported attrs */
    rc = pmix_register_tool_attrs();

    // mark ourselves as initialized
    pmix_atomic_set_bool(&pmix_globals.initialized);

    return rc;
}

PMIX_EXPORT pmix_status_t pmix_tool_init_info(void)
{
    pmix_kval_t *kptr;
    pmix_status_t rc;
    pmix_proc_t wildcard;

    PMIX_LOAD_PROCID(&wildcard, pmix_globals.myid.nspace, PMIX_RANK_WILDCARD);

    /* the jobid is just our nspace */
    kptr = PMIX_NEW(pmix_kval_t);
    kptr->key = strdup(PMIX_JOBID);
    PMIX_VALUE_CREATE(kptr->value, 1);
    kptr->value->type = PMIX_STRING;
    kptr->value->data.string = strdup(pmix_globals.myid.nspace);
    PMIX_GDS_STORE_KV(rc, pmix_globals.mypeer, &wildcard, PMIX_INTERNAL, kptr);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(kptr);
        return rc;
    }
    PMIX_RELEASE(kptr); // maintain accounting

    /* our rank - PMIX_RANK is a pmix_rank_t, so it must be stored as
     * PMIX_PROC_RANK. A tool that stored it as a plain int handed every
     * PMIx_Get of its own rank back with the wrong type */
    kptr = PMIX_NEW(pmix_kval_t);
    kptr->key = strdup(PMIX_RANK);
    PMIX_VALUE_CREATE(kptr->value, 1);
    kptr->value->type = PMIX_PROC_RANK;
    kptr->value->data.rank = pmix_globals.myid.rank;
    PMIX_GDS_STORE_KV(rc, pmix_globals.mypeer, &pmix_globals.myid, PMIX_INTERNAL, kptr);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(kptr);
        return rc;
    }
    PMIX_RELEASE(kptr); // maintain accounting

    /* nproc offset */
    kptr = PMIX_NEW(pmix_kval_t);
    kptr->key = strdup(PMIX_NPROC_OFFSET);
    PMIX_VALUE_CREATE(kptr->value, 1);
    kptr->value->type = PMIX_UINT32;
    kptr->value->data.uint32 = 0;
    PMIX_GDS_STORE_KV(rc, pmix_globals.mypeer, &wildcard, PMIX_INTERNAL, kptr);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(kptr);
        return rc;
    }
    PMIX_RELEASE(kptr); // maintain accounting

    /* node size */
    kptr = PMIX_NEW(pmix_kval_t);
    kptr->key = strdup(PMIX_NODE_SIZE);
    PMIX_VALUE_CREATE(kptr->value, 1);
    kptr->value->type = PMIX_UINT32;
    kptr->value->data.uint32 = 1;
    PMIX_GDS_STORE_KV(rc, pmix_globals.mypeer, &wildcard, PMIX_INTERNAL, kptr);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(kptr);
        return rc;
    }
    PMIX_RELEASE(kptr); // maintain accounting

    /* local peers */
    kptr = PMIX_NEW(pmix_kval_t);
    kptr->key = strdup(PMIX_LOCAL_PEERS);
    PMIX_VALUE_CREATE(kptr->value, 1);
    kptr->value->type = PMIX_STRING;
    kptr->value->data.string = strdup("0");
    PMIX_GDS_STORE_KV(rc, pmix_globals.mypeer, &wildcard, PMIX_INTERNAL, kptr);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(kptr);
        return rc;
    }
    PMIX_RELEASE(kptr); // maintain accounting

    /* local leader */
    kptr = PMIX_NEW(pmix_kval_t);
    kptr->key = strdup(PMIX_LOCALLDR);
    PMIX_VALUE_CREATE(kptr->value, 1);
    kptr->value->type = PMIX_UINT32;
    kptr->value->data.uint32 = 0;
    PMIX_GDS_STORE_KV(rc, pmix_globals.mypeer, &wildcard, PMIX_INTERNAL, kptr);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(kptr);
        return rc;
    }
    PMIX_RELEASE(kptr); // maintain accounting

    /* universe size */
    kptr = PMIX_NEW(pmix_kval_t);
    kptr->key = strdup(PMIX_UNIV_SIZE);
    PMIX_VALUE_CREATE(kptr->value, 1);
    kptr->value->type = PMIX_UINT32;
    kptr->value->data.uint32 = 1;
    PMIX_GDS_STORE_KV(rc, pmix_globals.mypeer, &wildcard, PMIX_INTERNAL, kptr);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(kptr);
        return rc;
    }
    PMIX_RELEASE(kptr); // maintain accounting

    /* job size - we are our very own job, so we have no peers */
    kptr = PMIX_NEW(pmix_kval_t);
    kptr->key = strdup(PMIX_JOB_SIZE);
    PMIX_VALUE_CREATE(kptr->value, 1);
    kptr->value->type = PMIX_UINT32;
    kptr->value->data.uint32 = 1;
    PMIX_GDS_STORE_KV(rc, pmix_globals.mypeer, &wildcard, PMIX_INTERNAL, kptr);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(kptr);
        return rc;
    }
    PMIX_RELEASE(kptr); // maintain accounting

    /* local size - only us in our job */
    kptr = PMIX_NEW(pmix_kval_t);
    kptr->key = strdup(PMIX_LOCAL_SIZE);
    PMIX_VALUE_CREATE(kptr->value, 1);
    kptr->value->type = PMIX_UINT32;
    kptr->value->data.uint32 = 1;
    PMIX_GDS_STORE_KV(rc, pmix_globals.mypeer, &wildcard, PMIX_INTERNAL, kptr);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(kptr);
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
    PMIX_GDS_STORE_KV(rc, pmix_globals.mypeer, &wildcard, PMIX_INTERNAL, kptr);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(kptr);
        return rc;
    }
    PMIX_RELEASE(kptr); // maintain accounting

    /* app number */
    kptr = PMIX_NEW(pmix_kval_t);
    kptr->key = strdup(PMIX_APPNUM);
    PMIX_VALUE_CREATE(kptr->value, 1);
    kptr->value->type = PMIX_UINT32;
    kptr->value->data.uint32 = 0;
    PMIX_GDS_STORE_KV(rc, pmix_globals.mypeer, &pmix_globals.myid, PMIX_INTERNAL, kptr);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(kptr);
        return rc;
    }
    PMIX_RELEASE(kptr); // maintain accounting

    /* app leader */
    kptr = PMIX_NEW(pmix_kval_t);
    kptr->key = strdup(PMIX_APPLDR);
    PMIX_VALUE_CREATE(kptr->value, 1);
    kptr->value->type = PMIX_UINT32;
    kptr->value->data.uint32 = 0;
    PMIX_GDS_STORE_KV(rc, pmix_globals.mypeer, &pmix_globals.myid, PMIX_INTERNAL, kptr);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(kptr);
        return rc;
    }
    PMIX_RELEASE(kptr); // maintain accounting

    /* app rank */
    kptr = PMIX_NEW(pmix_kval_t);
    kptr->key = strdup(PMIX_APP_RANK);
    PMIX_VALUE_CREATE(kptr->value, 1);
    kptr->value->type = PMIX_UINT32;
    kptr->value->data.uint32 = 0;
    PMIX_GDS_STORE_KV(rc, pmix_globals.mypeer, &pmix_globals.myid, PMIX_INTERNAL, kptr);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(kptr);
        return rc;
    }
    PMIX_RELEASE(kptr); // maintain accounting

    /* global rank */
    kptr = PMIX_NEW(pmix_kval_t);
    kptr->key = strdup(PMIX_GLOBAL_RANK);
    PMIX_VALUE_CREATE(kptr->value, 1);
    kptr->value->type = PMIX_UINT32;
    kptr->value->data.uint32 = 0;
    PMIX_GDS_STORE_KV(rc, pmix_globals.mypeer, &pmix_globals.myid, PMIX_INTERNAL, kptr);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(kptr);
        return rc;
    }
    PMIX_RELEASE(kptr); // maintain accounting

    /* local rank - we are alone in our job */
    kptr = PMIX_NEW(pmix_kval_t);
    kptr->key = strdup(PMIX_LOCAL_RANK);
    PMIX_VALUE_CREATE(kptr->value, 1);
    kptr->value->type = PMIX_UINT16;
    kptr->value->data.uint16 = 0;
    PMIX_GDS_STORE_KV(rc, pmix_globals.mypeer, &pmix_globals.myid, PMIX_INTERNAL, kptr);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(kptr);
        return rc;
    }
    PMIX_RELEASE(kptr); // maintain accounting

    /* we cannot know the node rank as we don't know what
     * other processes are executing on this node - so
     * we'll add that info to the server-tool handshake
     * and load it from there */

    /* hostname */
    kptr = PMIX_NEW(pmix_kval_t);
    kptr->key = strdup(PMIX_HOSTNAME);
    PMIX_VALUE_CREATE(kptr->value, 1);
    kptr->value->type = PMIX_STRING;
    kptr->value->data.string = strdup(pmix_globals.hostname);
    PMIX_GDS_STORE_KV(rc, pmix_globals.mypeer, &pmix_globals.myid, PMIX_INTERNAL, kptr);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(kptr);
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
    kptr->value->data.string = strdup(pmix_globals.hostname);
    PMIX_GDS_STORE_KV(rc, pmix_globals.mypeer, &wildcard, PMIX_INTERNAL, kptr);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(kptr);
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
    PMIX_GDS_STORE_KV(rc, pmix_globals.mypeer, &wildcard, PMIX_INTERNAL, kptr);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(kptr);
        return rc;
    }
    PMIX_RELEASE(kptr); // maintain accounting

    /* store our server's ID */
    if (NULL != pmix_client_globals.myserver && NULL != pmix_client_globals.myserver->info
        && NULL != pmix_client_globals.myserver->info->pname.nspace) {
        kptr = PMIX_NEW(pmix_kval_t);
        kptr->key = strdup(PMIX_SERVER_NSPACE);
        PMIX_VALUE_CREATE(kptr->value, 1);
        kptr->value->type = PMIX_STRING;
        kptr->value->data.string = strdup(pmix_client_globals.myserver->info->pname.nspace);
        PMIX_GDS_STORE_KV(rc, pmix_globals.mypeer, &pmix_globals.myid, PMIX_INTERNAL, kptr);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(kptr);
            return rc;
        }
        PMIX_RELEASE(kptr); // maintain accounting
        kptr = PMIX_NEW(pmix_kval_t);
        kptr->key = strdup(PMIX_SERVER_RANK);
        PMIX_VALUE_CREATE(kptr->value, 1);
        kptr->value->type = PMIX_PROC_RANK;
        kptr->value->data.rank = pmix_client_globals.myserver->info->pname.rank;
        PMIX_GDS_STORE_KV(rc, pmix_globals.mypeer, &pmix_globals.myid, PMIX_INTERNAL, kptr);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(kptr);
            return rc;
        }
        PMIX_RELEASE(kptr); // maintain accounting
    }

    return PMIX_SUCCESS;
}

pmix_status_t PMIx_tool_set_server_module(pmix_server_module_t *module)
{
    /* we have no peer to mark as a server until the tool library has
     * been initialized, so refuse rather than dereference a NULL */
    if (!pmix_atomic_check_bool(&pmix_globals.initialized)) {
        return PMIX_ERR_INIT;
    }

    if (NULL == module) {
        return PMIX_ERR_BAD_PARAM;
    }

    if (pmix_server_globals.module_set) {
        pmix_show_help("help-pmix-runtime.txt", "module-set", true,
                       __func__);
        return PMIX_ERR_INIT;
    }
    pmix_host_server = *module;
    pmix_server_globals.module_set = true;

    /* mark that we are now a server */
    PMIX_SET_PEER_TYPE(pmix_globals.mypeer, PMIX_PROC_SERVER);
    return PMIX_SUCCESS;
}

typedef struct {
    pmix_lock_t lock;
    pmix_event_t ev;
    bool active;
} pmix_tool_timeout_t;

/* timer callback */
static void fin_timeout(int sd, short args, void *cbdata)
{
    pmix_tool_timeout_t *tev;
    tev = (pmix_tool_timeout_t *) cbdata;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    pmix_output_verbose(2, pmix_globals.debug_output, "pmix:tool finwait timeout fired");
    if (tev->active) {
        tev->active = false;
        PMIX_WAKEUP_THREAD(&tev->lock);
    }
}
/* callback for finalize completion */
static void finwait_cbfunc(struct pmix_peer_t *pr, pmix_ptl_hdr_t *hdr,
                           pmix_buffer_t *buf, void *cbdata)
{
    pmix_tool_timeout_t *tev;
    tev = (pmix_tool_timeout_t *) cbdata;
    PMIX_HIDE_UNUSED_PARAMS(pr, hdr, buf);

    pmix_output_verbose(2, pmix_globals.debug_output, "pmix:tool finwait_cbfunc received");
    /* The wakeup belongs INSIDE the active test, which is the whole point
     * of the flag. If the guard timer has already fired, PMIx_tool_finalize
     * has come back out of its wait and destructed this lock - and it is a
     * stack object in a frame that is still running the rest of the
     * teardown, so a reply that arrives late would take a destroyed mutex
     * rather than fault on freed memory. That is exactly the case the timer
     * exists for: a server too slow or too wedged to answer. src/client's
     * identically-shaped finwait_cbfunc guards it. */
    if (tev->active) {
        tev->active = false;
        pmix_event_del(&tev->ev); // stop the timer
        PMIX_WAKEUP_THREAD(&tev->lock);
    }
}

PMIX_EXPORT pmix_status_t PMIx_tool_finalize(void)
{
    pmix_buffer_t *msg;
    pmix_cmd_t cmd = PMIX_FINALIZE_CMD;
    pmix_status_t rc, ret = PMIX_SUCCESS;
    pmix_tool_timeout_t tev;
    struct timeval tv = {5, 0};
    int n, i;
    pmix_peer_t *peer;
    pmix_pfexec_child_t *child;
    pmix_proc_t *dying = NULL;
    size_t ndying = 0, nd;
    pmix_dmdx_local_t *dlcd, *dnxt;
    pmix_lock_t lock;
    bool myserver_is_mypeer;

    if (!pmix_atomic_check_bool(&pmix_globals.initialized)) {
        return PMIX_ERR_INIT;
    }
    /* PMIx_tool_init is reference counted (a tool may init more than once);
     * only the last matching finalize tears the library down */
    i = pmix_atomic_fetch_add(&tool_init_cntr, -1);
    if (1 < i) {
        return PMIX_SUCCESS;
    }
    pmix_atomic_unset_bool(&pmix_globals.initialized);
    /* reset the one-time-init latch so a subsequent PMIx_tool_init in this
     * process starts a fresh library instance rather than short-circuiting
     * on the still-set flag and returning PMIX_ERR_INIT */
    pmix_atomic_clear(&pmix_globals.init_called);
    pmix_globals.mypeer->finalized = true;

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "pmix:tool finalize called");

    /* if we are connected, then disconnect */
    if (pmix_atomic_check_bool(&pmix_globals.connected)) {
        pmix_output_verbose(2, pmix_globals.debug_output,
                            "pmix:tool sending finalize sync to server");

        /* setup a cmd message to notify the PMIx
         * server that we are normally terminating */
        /* Note that nothing below this point aborts the teardown. We have
         * already dropped the initialized flag and the one-time-init
         * latch, so a caller that saw an error here could neither retry
         * nor finalize again - it would simply leak the entire library.
         * A server that cannot be told we are leaving is worth reporting,
         * not worth stranding our own state over, so failures here are
         * recorded and the teardown runs to completion. */
        msg = PMIX_NEW(pmix_buffer_t);
        /* pack the cmd */
        PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, msg, &cmd, 1, PMIX_COMMAND);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(msg);
            ret = rc;
        } else {
            /* setup a timer to protect ourselves should the server be
             * unable to answer for some reason */
            PMIX_CONSTRUCT_LOCK(&tev.lock);
            pmix_event_assign(&tev.ev, pmix_globals.evbase, -1, 0, fin_timeout, &tev);
            tev.active = true;
            PMIX_POST_OBJECT(&tev);
            pmix_event_add(&tev.ev, &tv);
            PMIX_PTL_SEND_RECV(rc, pmix_client_globals.myserver, msg, finwait_cbfunc,
                               (void *) &tev);
            if (PMIX_SUCCESS != rc) {
                /* the transport refused the message without taking it, so
                 * the recv callback will never fire - unwind everything we
                 * set up for it */
                if (tev.active) {
                    tev.active = false;
                    pmix_event_del(&tev.ev);
                }
                PMIX_DESTRUCT_LOCK(&tev.lock);
                PMIX_RELEASE(msg);
                ret = rc;
            } else {
                /* wait for the ack to return */
                PMIX_WAIT_THREAD(&tev.lock);
                PMIX_DESTRUCT_LOCK(&tev.lock);

                if (tev.active) {
                    pmix_event_del(&tev.ev);
                }
                pmix_output_verbose(2, pmix_globals.debug_output,
                                    "pmix:tool finalize sync received");
            }
        }
    }
    /* whatever happened above, we are on our way out */
    pmix_atomic_unset_bool(&pmix_globals.connected);

    /* Note the check on the pfexec framework itself rather than on our
     * peer type. Init only opens it for a launcher or scheduler, but a
     * plain tool can become a server after the fact by calling
     * PMIx_tool_set_server_module - and then this branch would walk a
     * children list that was never constructed */
    if (pmix_pfexec_globals.initialized &&
        (PMIX_PEER_IS_LAUNCHER(pmix_globals.mypeer) ||
         PMIX_PEER_IS_SERVER(pmix_globals.mypeer))) {
        /* if we have launched children, then we need to cleanly
         * terminate them - do this before stopping our progress
         * thread as we need it for terminating procs */
        if (pmix_pfexec_globals.poll_active) {
            pmix_event_del(pmix_pfexec_globals.poll_ev);
            pmix_pfexec_globals.poll_active = false;
        }
        /* Snapshot the identities before killing anything. Each kill parks
         * this thread on the lock for the whole SIGCONT/SIGTERM/SIGKILL
         * sequence - seconds - and the progress thread keeps running
         * underneath: it can complete and release other children while we
         * wait, so a pointer into the list cannot be carried across the
         * wait. That is what the "next" pointer of a list traversal is.
         * kill_proc looks its target up by identity anyway, so an identity
         * is all this loop ever needed. */
        ndying = pmix_list_get_size(&pmix_pfexec_globals.children);
        if (0 < ndying) {
            dying = (pmix_proc_t *) malloc(ndying * sizeof(pmix_proc_t));
        }
        if (NULL == dying) {
            ndying = 0;
        } else {
            nd = 0;
            PMIX_LIST_FOREACH (child, &pmix_pfexec_globals.children, pmix_pfexec_child_t) {
                memcpy(&dying[nd++], &child->proc, sizeof(pmix_proc_t));
            }
        }
        for (nd = 0; nd < ndying; nd++) {
            PMIX_CONSTRUCT_LOCK(&lock);
            /* the caddy holds this pointer until the sequence ends, which
             * is why the array outlives the loop */
            PMIX_PFEXEC_KILL(&dying[nd], &lock);
            PMIX_WAIT_THREAD(&lock);
            PMIX_DESTRUCT_LOCK(&lock);
        }
        if (NULL != dying) {
            free(dying);
        }
        pmix_pfexec_base_close();
    }

    /* Take down the file-scope events init may have registered, while the
     * bases they are on still exist. Each one has to go, or it survives
     * into a state where its base has been destroyed under it - and, for
     * the two that are re-armed on a later init, into a PMIX_CONSTRUCT
     * over a live object. */
    if (0 <= keepalive_fd) {
        pmix_event_del(&parentdied);
        close(keepalive_fd);
        keepalive_fd = -1;
    }

    /* wait here until all active events have been processed */
    PMIx_Progress_thread_stop(NULL, 0);

    /* flush anything that is still trying to be written out, then tear
     * down the static IOF sinks so their buffered-output and write-event
     * state does not leak across a re-init (matches the client finalize) */
    pmix_iof_static_dump_output(&pmix_client_globals.iof_stdout);
    pmix_iof_static_dump_output(&pmix_client_globals.iof_stderr);
    PMIX_DESTRUCT(&pmix_client_globals.iof_stdout);
    PMIX_DESTRUCT(&pmix_client_globals.iof_stderr);

    PMIX_LIST_DESTRUCT(&pmix_client_globals.pending_requests);
    /* drop the delta-commit record, and leave the flag set so a second
     * PMIx_tool_init in this process starts cumulative (matches the
     * client finalize) */
    pmix_client_commit_resync();
    for (n = 0; n < pmix_client_globals.peers.size; n++) {
        peer = (pmix_peer_t*)pmix_pointer_array_get_item(&pmix_client_globals.peers, n);
        if (NULL != peer) {
            PMIX_RELEASE(peer);
        }
    }
    /* release the peers array's backing store, matching the client
     * finalize - the loop above only released the items it held */
    PMIX_DESTRUCT(&pmix_client_globals.peers);

    pmix_ptl_base_stop_listening();

    /* drain any parked direct-modex tracker before the peers go - see the
     * matching comment in PMIx_server_finalize for why the list destruct
     * below cannot free these */
    PMIX_LIST_FOREACH_SAFE (dlcd, dnxt, &pmix_server_globals.local_reqs, pmix_dmdx_local_t) {
        pmix_server_fail_local_reqs(dlcd, PMIX_ERR_UNREACH);
    }

    for (n = 0; n < pmix_server_globals.clients.size; n++) {
        peer = (pmix_peer_t*)pmix_pointer_array_get_item(&pmix_server_globals.clients, n);
        if (NULL != peer) {
            PMIX_RELEASE(peer);
        }
    }

    (void) pmix_mca_base_framework_close(&pmix_pnet_base_framework);
    /* tear down every server_globals list that pmix_server_initialize
     * constructs on each init, so none of their contents leak across a
     * cycle. pmix_server_initialize (called unconditionally from
     * PMIx_tool_init) reconstructs all of these on the next init. */
    PMIX_DESTRUCT(&pmix_server_globals.clients);
    for (n = 0; n < pmix_server_globals.peer_cache.size; n++) {
        peer = (pmix_peer_t*)pmix_pointer_array_get_item(&pmix_server_globals.peer_cache, n);
        if (NULL != peer) {
            PMIX_RELEASE(peer);
        }
    }
    PMIX_DESTRUCT(&pmix_server_globals.peer_cache);
    PMIX_LIST_DESTRUCT(&pmix_server_globals.nspaces);
    PMIX_LIST_DESTRUCT(&pmix_server_globals.collectives);
    PMIX_LIST_DESTRUCT(&pmix_server_globals.remote_pnd);
    PMIX_LIST_DESTRUCT(&pmix_server_globals.local_reqs);
    PMIX_LIST_DESTRUCT(&pmix_server_globals.gdata);
    PMIX_LIST_DESTRUCT(&pmix_server_globals.events);
    PMIX_LIST_DESTRUCT(&pmix_server_globals.iof);
    PMIX_LIST_DESTRUCT(&pmix_server_globals.iof_residuals);
    PMIX_LIST_DESTRUCT(&pmix_server_globals.psets);
    PMIX_LIST_DESTRUCT(&pmix_server_globals.grp_collectives);

    (void) pmix_mca_base_framework_close(&pmix_pmdl_base_framework);

    /* free the tmpdir strings so they neither leak nor carry a stale
     * value into the next init: the init-time guards only refresh them
     * when they are NULL, so a surviving pointer would silently ignore a
     * changed PMIX_SERVER_TMPDIR/PMIX_SYSTEM_TMPDIR on the next cycle */
    if (NULL != pmix_server_globals.tmpdir) {
        free(pmix_server_globals.tmpdir);
        pmix_server_globals.tmpdir = NULL;
    }
    if (NULL != pmix_server_globals.system_tmpdir) {
        free(pmix_server_globals.system_tmpdir);
        pmix_server_globals.system_tmpdir = NULL;
    }

    /* Capture whether our active server currently aliases our own peer
     * BEFORE rte_finalize releases (and may NULL) mypeer. The switch paths
     * (disc, pmix_tool_retry_set) point myserver back at pmix_globals.mypeer
     * when we disconnect our primary, taking a reference on it. rte_finalize
     * plus the mypeer release below already drop mypeer's references, so
     * releasing myserver separately in that case would free mypeer a third
     * time. */
    myserver_is_mypeer = (pmix_client_globals.myserver == pmix_globals.mypeer);

    pmix_rte_finalize();
    if (NULL != pmix_globals.mypeer) {
        PMIX_RELEASE(pmix_globals.mypeer);
    }
    if (!myserver_is_mypeer && NULL != pmix_client_globals.myserver) {
        PMIX_RELEASE(pmix_client_globals.myserver);
    }
    /* in the aliased case the release above is skipped and the one
     * through mypeer is what freed the object, so this pointer would
     * otherwise be left naming freed memory */
    pmix_client_globals.myserver = NULL;

    /* finalize the class/object system */
    pmix_class_finalize();

    /* the library is fully down either way; "ret" only reports whether we
     * managed to tell our server about it */
    return ret;
}

bool PMIx_tool_is_connected(void)
{
    return pmix_atomic_check_bool(&pmix_globals.connected);
}

pmix_status_t PMIx_tool_connect_to_server(pmix_proc_t *proc, pmix_info_t info[], size_t ninfo)
{
    pmix_status_t rc;

    if (!pmix_atomic_check_bool(&pmix_globals.initialized)) {
        return PMIX_ERR_INIT;
    }
    rc = PMIx_tool_attach_to_server(proc, NULL, info, ninfo);
    return rc;
}

void pmix_tool_retry_attach(int sd, short args, void *cbdata)
{
    pmix_cb_t *cb = (pmix_cb_t *) cbdata;
    pmix_kval_t *kptr;
    pmix_peer_t *peer;
    size_t n;
    pmix_status_t rc;
    char *suri;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_ACQUIRE_OBJECT(cb);

    /* check for directives */
    cb->checked = false;
    for (n = 0; n < cb->ninfo; n++) {
        if (PMIX_CHECK_KEY(&cb->info[n], PMIX_PRIMARY_SERVER)) {
            cb->checked = PMIX_INFO_TRUE(&cb->info[n]);
            break;
        }
    }

    /* ask the ptl to establish connection to the new server */
    peer = PMIX_NEW(pmix_peer_t);
    /* setup the infrastructure - assume this new server will follow
     * same rules as our current one */
    peer->nptr = PMIX_NEW(pmix_namespace_t);
    peer->info = PMIX_NEW(pmix_rank_info_t);
    peer->nptr->compat.bfrops = pmix_globals.mypeer->nptr->compat.bfrops;
    peer->nptr->compat.psec = pmix_globals.mypeer->nptr->compat.psec;
    peer->nptr->compat.type = pmix_globals.mypeer->nptr->compat.type;
    peer->nptr->compat.gds = pmix_globals.mypeer->nptr->compat.gds;

    cb->status = pmix_ptl.connect_to_peer((struct pmix_peer_t *) peer, cb->info, cb->ninfo, &suri);

    if (PMIX_SUCCESS == cb->status) {
        /* return the name */
        cb->pname.nspace = strdup(peer->info->pname.nspace);
        cb->pname.rank = peer->info->pname.rank;
        /* add the peer to our known clients */
        pmix_pointer_array_add(&pmix_server_globals.clients, peer);
        if (cb->checked) {
            /* point our active server at this new one. myserver holds a
             * reference of its own, independent of the clients-array
             * entry (see PMIx_tool_init and PMIx_tool_finalize), so drop
             * the outgoing server's myserver reference and take one on the
             * new primary */
            /* the incoming server has seen nothing we published, so the
             * next commit owes it everything rather than a delta */
            pmix_client_commit_resync();
            if (NULL != pmix_client_globals.myserver) {
                PMIX_RELEASE(pmix_client_globals.myserver);
            }
            PMIX_RETAIN(peer);
            pmix_client_globals.myserver = peer;
            /* mark that we are connected */
            pmix_atomic_set_bool(&pmix_globals.connected);
            /* update our active server's ID in the local key-value store */
            kptr = PMIX_NEW(pmix_kval_t);
            kptr->key = strdup(PMIX_SERVER_NSPACE);
            PMIX_VALUE_CREATE(kptr->value, 1);
            kptr->value->type = PMIX_STRING;
            kptr->value->data.string = strdup(peer->info->pname.nspace);
            PMIX_GDS_STORE_KV(rc, pmix_globals.mypeer, &pmix_globals.myid, PMIX_INTERNAL, kptr);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
            }
            PMIX_RELEASE(kptr); // maintain accounting
            kptr = PMIX_NEW(pmix_kval_t);
            kptr->key = strdup(PMIX_SERVER_RANK);
            PMIX_VALUE_CREATE(kptr->value, 1);
            kptr->value->type = PMIX_PROC_RANK;
            kptr->value->data.rank = peer->info->pname.rank;
            PMIX_GDS_STORE_KV(rc, pmix_globals.mypeer, &pmix_globals.myid, PMIX_INTERNAL, kptr);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
            }
            PMIX_RELEASE(kptr); // maintain accounting
            /* store the URI for subsequent lookups */
            PMIX_KVAL_NEW(kptr, PMIX_SERVER_URI);
            kptr->value->type = PMIX_STRING;
            pmix_asprintf(&kptr->value->data.string, "%s.%u;%s",
                          peer->info->pname.nspace,
                          peer->info->pname.rank, suri);
            free(suri);
            PMIX_GDS_STORE_KV(rc, pmix_globals.mypeer, &pmix_globals.myid, PMIX_INTERNAL, kptr);
            PMIX_RELEASE(kptr); // maintain accounting
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
            }
        } else {
            /* connect_to_peer hands back a heap-allocated URI on every
             * success - only the branch above consumes it, so free it here
             * when this server is not becoming our primary */
            free(suri);
        }

    } else {
        PMIX_RELEASE(peer);
    }

    PMIX_POST_OBJECT(cb);
    PMIX_WAKEUP_THREAD(&cb->lock);
    return;
}

pmix_status_t PMIx_tool_attach_to_server(pmix_proc_t *myproc, pmix_proc_t *server,
                                         pmix_info_t info[], size_t ninfo)
{
    pmix_status_t rc;
    pmix_cb_t *cb;

    if (!pmix_atomic_check_bool(&pmix_globals.initialized)) {
        return PMIX_ERR_INIT;
    }

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        return PMIX_ERR_NOT_AVAILABLE;
    }

    /* check for bozo error */
    if (NULL == info || 0 == ninfo) {
        pmix_show_help("help-pmix-runtime.txt", "tool:no-server", true);
        return PMIX_ERR_BAD_PARAM;
    }

    /* what would release us runs on the progress thread, so waiting
     * for it from that thread waits for ourselves */
    if (PMIX_UNLIKELY(pmix_progress_thread_check_blocking("PMIx_tool_attach_to_server"))) {
        return PMIX_ERR_WOULD_BLOCK;
    }

    cb = PMIX_NEW(pmix_cb_t);
    cb->info = info;
    cb->ninfo = ninfo;
    PMIX_THREADSHIFT(cb, pmix_tool_retry_attach);

    PMIX_WAIT_THREAD(&cb->lock);
    rc = cb->status;

    /* if they gave us an address, we pass back our name */
    if (NULL != myproc) {
        memcpy(myproc, &pmix_globals.myid, sizeof(pmix_proc_t));
    }

    /* if the transition didn't succeed, then return at this point */
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(cb);
        return rc;
    }

    /* if they gave us an address, return the new server's ID */
    if (NULL != server) {
        PMIX_LOAD_PROCID(server, cb->pname.nspace, cb->pname.rank);
    }
    PMIX_RELEASE(cb);
    return PMIX_SUCCESS;
}

static void disc(int sd, short args, void *cbdata)
{
    pmix_cb_t *cb = (pmix_cb_t *) cbdata;
    pmix_peer_t *peer = NULL, *pr;
    int n;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_ACQUIRE_OBJECT(cb);

    if (NULL == cb->proc) {
        pmix_atomic_unset_bool(&pmix_globals.connected);
        cb->status = PMIX_SUCCESS;
        PMIX_POST_OBJECT(cb);
        PMIX_WAKEUP_THREAD(&cb->lock);
        return;
    }

    /* An unnamed server matches everything: PMIX_CHECK_NSPACE treats an
     * invalid nspace as a wildcard, so the walk below would drop whichever
     * server happens to be first in the array. Naming no server is not a
     * way to disconnect an arbitrary one. */
    if (PMIX_NSPACE_INVALID(cb->proc->nspace)) {
        cb->status = PMIX_ERR_BAD_PARAM;
        PMIX_POST_OBJECT(cb);
        PMIX_WAKEUP_THREAD(&cb->lock);
        return;
    }

    /* see if we have this server */
    for (n = 0; n < pmix_server_globals.clients.size; n++) {
        pr = (pmix_peer_t *) pmix_pointer_array_get_item(&pmix_server_globals.clients, n);
        if (NULL == pr) {
            continue;
        }
        if (PMIX_CHECK_NSPACE(cb->proc->nspace, pr->info->pname.nspace)
            && PMIX_CHECK_RANK(cb->proc->rank, pr->info->pname.rank)) {
            peer = pr;
            pmix_pointer_array_set_item(&pmix_server_globals.clients, n, NULL);
            break;
        }
    }
    if (NULL == peer) {
        cb->status = PMIX_ERR_NOT_FOUND;
        PMIX_POST_OBJECT(cb);
        PMIX_WAKEUP_THREAD(&cb->lock);
        return;
    }

    /* if we are disconnecting from the active server, then we enter a
     * "disconnected" state where we point the active server back at
     * ourselves - effectively the same as when we init without connecting */
    if (peer == pmix_client_globals.myserver) {
        pmix_peer_t *departing = peer;
        /* whatever we next connect to has seen nothing we published */
        pmix_client_commit_resync();
        PMIX_RETAIN(pmix_globals.mypeer);
        /* switch servers - we are in an event, so it is
         * safe to do so */
        pmix_client_globals.myserver = pmix_globals.mypeer;
        pmix_atomic_unset_bool(&pmix_globals.connected);
        /* release the reference myserver held on the departing server,
         * in addition to the clients-array reference dropped below */
        PMIX_RELEASE(departing);
    }

    /* now drop the connection - release the clients-array reference */
    PMIX_RELEASE(peer);

    cb->status = PMIX_SUCCESS;
    PMIX_POST_OBJECT(cb);
    PMIX_WAKEUP_THREAD(&cb->lock);
    return;
}

pmix_status_t PMIx_tool_disconnect(const pmix_proc_t *server)
{
    pmix_status_t rc;
    pmix_cb_t *cb;

    if (!pmix_atomic_check_bool(&pmix_globals.initialized)) {
        return PMIX_ERR_INIT;
    }

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        return PMIX_ERR_NOT_AVAILABLE;
    }

    /* what would release us runs on the progress thread, so waiting
     * for it from that thread waits for ourselves */
    if (PMIX_UNLIKELY(pmix_progress_thread_check_blocking("PMIx_tool_disconnect"))) {
        return PMIX_ERR_WOULD_BLOCK;
    }

    /* disc() dereferences this whenever it is not NULL; a NULL server is
     * the documented "just mark me disconnected" form */
    cb = PMIX_NEW(pmix_cb_t);
    cb->proc = (pmix_proc_t *) server;
    PMIX_THREADSHIFT(cb, disc);

    PMIX_WAIT_THREAD(&cb->lock);
    rc = cb->status;
    cb->proc = NULL;
    PMIX_RELEASE(cb);

    return rc;
}

static void getsrvrs(int sd, short args, void *cbdata)
{
    pmix_cb_t *cb = (pmix_cb_t *) cbdata;
    int n;
    size_t ns;
    pmix_list_t srvrs;
    pmix_proclist_t *ps;
    pmix_peer_t *pr;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_ACQUIRE_OBJECT(cb);

    /* get servers */
    PMIX_CONSTRUCT(&srvrs, pmix_list_t);
    /* Put our current active server at the front - but only if we have
     * one. A tool that came up with PMIX_TOOL_DO_NOT_CONNECT, or whose
     * optional connect failed, or that has disconnected its primary,
     * points myserver at a peer standing in for itself; that is not a
     * server we hold a connection to, and the Standard requires
     * PMIX_ERR_UNREACH (which the empty-list path below returns) when
     * there is none. */
    if (pmix_atomic_check_bool(&pmix_globals.connected) &&
        pmix_globals.mypeer != pmix_client_globals.myserver) {
        ps = PMIX_NEW(pmix_proclist_t);
        PMIX_LOAD_PROCID(&ps->proc, pmix_client_globals.myserver->info->pname.nspace,
                         pmix_client_globals.myserver->info->pname.rank);
        pmix_list_append(&srvrs, &ps->super);
    }

    for (n = 0; n < pmix_server_globals.clients.size; n++) {
        pr = (pmix_peer_t *) pmix_pointer_array_get_item(&pmix_server_globals.clients, n);
        if (NULL == pr) {
            continue;
        }
        /* if it is our current primary server, ignore it */
        if (pr == pmix_client_globals.myserver) {
            continue;
        }
        /* record it */
        ps = PMIX_NEW(pmix_proclist_t);
        PMIX_LOAD_PROCID(&ps->proc, pr->info->pname.nspace, pr->info->pname.rank);
        pmix_list_append(&srvrs, &ps->super);
    }

    ns = pmix_list_get_size(&srvrs);

    if (0 == ns) {
        /* we aren't connected to anyone */
        cb->status = PMIX_ERR_UNREACH;
        cb->nprocs = 0;
        cb->procs = NULL;
        PMIX_DESTRUCT(&srvrs);
        PMIX_POST_OBJECT(cb);
        PMIX_WAKEUP_THREAD(&cb->lock);
        return;
    }

    /* allocate the array */
    PMIX_PROC_CREATE(cb->procs, ns);
    cb->nprocs = ns;

    /* now load the array */
    n = 0;
    PMIX_LIST_FOREACH (ps, &srvrs, pmix_proclist_t) {
        memcpy(&cb->procs[n], &ps->proc, sizeof(pmix_proc_t));
        ++n;
    }
    cb->status = PMIX_SUCCESS;
    PMIX_LIST_DESTRUCT(&srvrs);

    PMIX_POST_OBJECT(cb);
    PMIX_WAKEUP_THREAD(&cb->lock);
    return;
}
pmix_status_t PMIx_tool_get_servers(pmix_proc_t *servers[], size_t *nservers)
{
    pmix_status_t rc;
    pmix_cb_t *cb;

    if (!pmix_atomic_check_bool(&pmix_globals.initialized)) {
        return PMIX_ERR_INIT;
    }

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        return PMIX_ERR_NOT_AVAILABLE;
    }

    /* both OUT parameters are written unconditionally below */
    if (NULL == servers || NULL == nservers) {
        return PMIX_ERR_BAD_PARAM;
    }

    /* what would release us runs on the progress thread, so waiting
     * for it from that thread waits for ourselves */
    if (PMIX_UNLIKELY(pmix_progress_thread_check_blocking("PMIx_tool_get_servers"))) {
        return PMIX_ERR_WOULD_BLOCK;
    }

    cb = PMIX_NEW(pmix_cb_t);

    PMIX_THREADSHIFT(cb, getsrvrs);
    PMIX_WAIT_THREAD(&cb->lock);
    rc = cb->status;
    *servers = cb->procs;
    *nservers = cb->nprocs;

    cb->procs = NULL; // protect the array
    cb->nprocs = 0;
    PMIX_RELEASE(cb);

    return rc;
}

void pmix_tool_retry_set(int sd, short args, void *cbdata)
{
    pmix_cb_t *cb = (pmix_cb_t *) cbdata;
    int n;
    pmix_peer_t *peer = NULL, *pr;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_ACQUIRE_OBJECT(cb);

    /* if we are switching back to me, then there is no point in
     * searching the array of clients - I definitely won't be there! */
    if (PMIX_CHECK_NSPACE(cb->proc->nspace, pmix_globals.myid.nspace)
        && PMIX_CHECK_RANK(cb->proc->rank, pmix_globals.myid.rank)) {
        /* drop the outgoing server's myserver reference and take one on
         * ourselves (see PMIx_tool_init and PMIx_tool_finalize) */
        /* whatever we next connect to has seen nothing we published */
        pmix_client_commit_resync();
        if (NULL != pmix_client_globals.myserver) {
            PMIX_RELEASE(pmix_client_globals.myserver);
        }
        PMIX_RETAIN(pmix_globals.mypeer);
        pmix_client_globals.myserver = pmix_globals.mypeer;
        /* pointing our active server at ourselves means we no longer have
         * a server, which is exactly the state disc() leaves behind when
         * it drops the primary - mark it the same way. Claiming to be
         * connected here would send the finalize handshake (and anything
         * else guarded on this flag, such as pmix_tool_relay_op) to our
         * own already-finalized peer. */
        pmix_atomic_unset_bool(&pmix_globals.connected);
        goto done;
    }

    /* see if we have this server */
    for (n = 0; n < pmix_server_globals.clients.size; n++) {
        pr = (pmix_peer_t *) pmix_pointer_array_get_item(&pmix_server_globals.clients, n);
        if (NULL == pr) {
            continue;
        }
        if (PMIX_CHECK_NSPACE(cb->proc->nspace, pr->info->pname.nspace)
            && PMIX_CHECK_RANK(cb->proc->rank, pr->info->pname.rank)) {
            peer = pr;
            break;
        }
    }
    if (NULL == peer) {
        /* do they want us to wait? */
        if (cb->checked) {
            /* cb->status carries the remaining budget, counted in 0.25sec
             * retry intervals; PMIx_tool_set_server loads it with INT_MAX
             * when no (or a zero) PMIX_TIMEOUT was given, as the Standard
             * defines that to mean "never time out" */
            --cb->status;
            if (cb->status < 0) {
                cb->status = PMIX_ERR_TIMEOUT;
                PMIX_POST_OBJECT(cb);
                PMIX_WAKEUP_THREAD(&cb->lock);
                return;
            }
            PMIX_THREADSHIFT_DELAY(cb, pmix_tool_retry_set, 0.25);
        } else {
            /* no - so just return failure */
            cb->status = PMIX_ERR_UNREACH;
            PMIX_POST_OBJECT(cb);
            PMIX_WAKEUP_THREAD(&cb->lock);
            return;
        }
        PMIX_POST_OBJECT(cb);
        return;
    }

    /* if this is the current active server, then ignore the request */
    if (peer == pmix_client_globals.myserver) {
        pmix_atomic_set_bool(&pmix_globals.connected); // just ensure we mark ourselves as connected
        cb->status = PMIX_SUCCESS;
        PMIX_POST_OBJECT(cb);
        PMIX_WAKEUP_THREAD(&cb->lock);
        return;
    }

    /* switch the active server - we are in an event, so it is safe to do
     * so. Drop the outgoing server's myserver reference and take one on
     * the new primary */
    /* the incoming server has seen nothing we published, so the next
     * commit owes it everything rather than a delta */
    pmix_client_commit_resync();
    if (NULL != pmix_client_globals.myserver) {
        PMIX_RELEASE(pmix_client_globals.myserver);
    }
    PMIX_RETAIN(peer);
    pmix_client_globals.myserver = peer;
    pmix_atomic_set_bool(&pmix_globals.connected);

done:
    cb->status = PMIX_SUCCESS;
    PMIX_POST_OBJECT(cb);
    PMIX_WAKEUP_THREAD(&cb->lock);
    return;
}

pmix_status_t PMIx_tool_set_server(const pmix_proc_t *server,
                                   pmix_info_t info[], size_t ninfo)
{
    pmix_status_t rc;
    pmix_cb_t *cb;
    size_t n;
    int timeout = 0; // zero means "never time out" per the Standard

    if (!pmix_atomic_check_bool(&pmix_globals.initialized)) {
        return PMIX_ERR_INIT;
    }

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        return PMIX_ERR_NOT_AVAILABLE;
    }

    if (NULL == server) {
        return PMIX_ERR_BAD_PARAM;
    }

    /* what would release us runs on the progress thread, so waiting
     * for it from that thread waits for ourselves */
    if (PMIX_UNLIKELY(pmix_progress_thread_check_blocking("PMIx_tool_set_server"))) {
        return PMIX_ERR_WOULD_BLOCK;
    }

    /* threadshift this so we can access global structures */
    cb = PMIX_NEW(pmix_cb_t);
    cb->proc = (pmix_proc_t *) server;
    for (n = 0; NULL != info && n < ninfo; n++) {
        if (PMIX_CHECK_KEY(&info[n], PMIX_TIMEOUT)) {
            timeout = info[n].value.data.integer;
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_WAIT_FOR_CONNECTION)) {
            cb->checked = PMIX_INFO_TRUE(&info[n]);
        }
    }
    /* pmix_tool_retry_set counts this down once per 0.25sec retry. An
     * absent or zero PMIX_TIMEOUT means the operation is never to time
     * out - leaving the budget at zero would instead make the very first
     * retry expire, so a "wait for connection" request would never wait */
    cb->status = (0 < timeout) ? (4 * timeout) : INT_MAX;
    PMIX_THREADSHIFT(cb, pmix_tool_retry_set);

    /* wait for completion */
    PMIX_WAIT_THREAD(&cb->lock);
    rc = cb->status;
    PMIX_RELEASE(cb);

    return rc;
}
