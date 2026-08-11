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

#include "src/include/pmix_config.h"

#include "src/include/pmix_socket_errno.h"
#include "src/include/pmix_stdint.h"

#include "pmix_common.h"
#include "include/pmix_server.h"

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
#include <ctype.h>
#include <sys/stat.h>

#include "src/common/pmix_attributes.h"
#include "src/hwloc/pmix_hwloc.h"
#include "src/mca/base/pmix_base.h"
#include "src/mca/base/pmix_mca_base_var.h"
#include "src/mca/base/pmix_mca_base_vari.h"
#include "src/mca/bfrops/base/base.h"
#include "src/mca/gds/base/base.h"
#include "src/mca/pgpu/base/base.h"
#include "src/mca/pmdl/base/base.h"
#include "src/mca/pnet/base/base.h"
#include "src/mca/psensor/base/base.h"
#include "src/mca/pstat/base/base.h"
#include "src/mca/ptl/base/base.h"
#include "src/runtime/pmix_progress_threads.h"
#include "src/runtime/pmix_rte.h"
#include "src/tool/pmix_tool_ops.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_environ.h"
#include "src/util/pmix_printf.h"
#include "src/util/pmix_show_help.h"

/* the server also needs access to client operations
 * as it can, and often does, behave as a client */
#include "pmix_server_ops.h"
#include "src/client/pmix_client_ops.h"

// global variables
pmix_server_globals_t pmix_server_globals = {
    .module_set = false,
    .nspaces = PMIX_LIST_STATIC_INIT,
    .clients = PMIX_POINTER_ARRAY_STATIC_INIT,
    .peer_cache = PMIX_POINTER_ARRAY_STATIC_INIT,
    .collectives = PMIX_LIST_STATIC_INIT,
    .remote_pnd = PMIX_LIST_STATIC_INIT,
    .local_reqs = PMIX_LIST_STATIC_INIT,
    .gdata = PMIX_LIST_STATIC_INIT,
    .genvars = NULL,
    .events = PMIX_LIST_STATIC_INIT,
    .iof = PMIX_LIST_STATIC_INIT,
    .iof_residuals = PMIX_LIST_STATIC_INIT,
    .psets = PMIX_LIST_STATIC_INIT,
    .max_iof_cache = 0,
    .tool_connections_allowed = false,
    .tmpdir = NULL,
    .system_tmpdir = NULL,
    .fence_localonly_opt = false,
    .get_output = -1,
    .get_verbose = 0,
    .connect_output = -1,
    .connect_verbose = 0,
    .fence_output = -1,
    .fence_verbose = 0,
    .pub_output = -1,
    .pub_verbose = 0,
    .spawn_output = -1,
    .spawn_verbose = 0,
    .event_output = -1,
    .event_verbose = 0,
    .iof_output = -1,
    .iof_verbose = 0,
    .base_output = -1,
    .base_verbose = 0,
    .group_output = -1,
    .group_verbose = 0
};

// local variables
static pmix_event_t parentdied;
/* Records the descriptor PMIX_KEEPALIVE_PIPE named, so finalize can take
 * that event back down and close it. It is a static rather than a local
 * because finalize cannot re-derive the answer - the pipe was a directive
 * to *init*, and init removes it from the environment. Mirrors the same
 * bookkeeping in PMIx_tool_init/PMIx_tool_finalize. */
static int keepalive_fd = -1;
static char *security_mode = NULL;
static char *bfrops_mode = NULL;
static char *gds_mode = NULL;
static pid_t mypid;
static pmix_proc_t myparent;

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
    pmix_status_t rc;
    PMIX_HIDE_UNUSED_PARAMS(fd, flags, arg);

    PMIX_LOAD_PROCID(&keepalive, "PMIX_KEEPALIVE_PIPE", PMIX_RANK_UNDEF);

    /* generate a job-terminated event */
    cb = PMIX_NEW(pmix_cb_t);
    cb->ninfo = 2;
    PMIX_INFO_CREATE(cb->info, cb->ninfo);
    PMIX_INFO_LOAD(&cb->info[0], PMIX_EVENT_NON_DEFAULT, NULL, PMIX_BOOL);
    PMIX_INFO_LOAD(&cb->info[1], PMIX_EVENT_AFFECTED_PROC, &keepalive, PMIX_PROC);
    cb->infocopy = true; // ensure cleanup
    rc = PMIx_Notify_event(PMIX_ERR_JOB_TERMINATED, &pmix_globals.myid,
                           PMIX_RANGE_PROC_LOCAL, cb->info, cb->ninfo,
                           _pdiedcb, (void*)cb);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(cb);
    }
}

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

    /* decode it - leave cb->status set to the store result so PMIx_server_init
     * sees a storage failure rather than having it masked as success. The
     * "nothing more to unpack" case is normalized to PMIX_SUCCESS by the
     * module, so a parent that had no job data for us still succeeds here. */
    PMIX_GDS_STORE_JOB_INFO(cb->status, pmix_client_globals.myserver, nspace, buf);

    free(nspace);
    PMIX_POST_OBJECT(cb);
    PMIX_WAKEUP_THREAD(&cb->lock);
}

/* event handler registration callback */
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

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "[%s:%d] DEBUGGER RELEASE RECVD",
                        pmix_globals.myid.nspace, pmix_globals.myid.rank);

    PMIX_HIDE_UNUSED_PARAMS(evhdlr_registration_id, status, source, results, nresults);

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
            pmix_output_verbose(2, pmix_server_globals.base_output,
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

static pmix_status_t register_singleton(char *name)
{
    char *tmp, *ptr, *endptr;
    pmix_namespace_t *nptr;
    pmix_rank_t rank;
    pmix_rank_info_t *rinfo;
    unsigned long val;
    size_t nslen;

    /* the value must be the string representation of a proc ID -
     * i.e., of the form "nspace.rank". Anything else is a mistake
     * on the part of whoever passed it to us, so tell them about
     * it instead of faulting */
    if (NULL == name) {
        pmix_show_help("help-pmix-server.txt", "bad-singleton", true, "NULL");
        return PMIX_ERR_BAD_PARAM;
    }
    ptr = strrchr(name, '.');
    if (NULL == ptr || ptr == name || '\0' == ptr[1]) {
        /* no separator, no nspace, or no rank */
        pmix_show_help("help-pmix-server.txt", "bad-singleton", true, name);
        return PMIX_ERR_BAD_PARAM;
    }
    nslen = (size_t) (ptr - name);
    if (PMIX_MAX_NSLEN < nslen) {
        pmix_show_help("help-pmix-server.txt", "bad-singleton", true, name);
        return PMIX_ERR_BAD_PARAM;
    }
    /* the rank must be a simple non-negative number - note that
     * strtoul returns ULONG_MAX on overflow, which the validity
     * check below rejects */
    val = strtoul(&ptr[1], &endptr, 10);
    if ('\0' != *endptr || !PMIX_RANK_IS_VALID(val)) {
        pmix_show_help("help-pmix-server.txt", "bad-singleton", true, name);
        return PMIX_ERR_BAD_PARAM;
    }
    rank = (pmix_rank_t) val;

    /* take a private copy and split it at the separator */
    tmp = strdup(name);
    if (NULL == tmp) {
        return PMIX_ERR_NOMEM;
    }
    tmp[nslen] = '\0';

    nptr = PMIX_NEW(pmix_namespace_t);
    if (NULL == nptr) {
        free(tmp);
        return PMIX_ERR_NOMEM;
    }
    nptr->nspace = strdup(tmp);
    nptr->nlocalprocs = 1;
    nptr->nprocs = 1;
    /* add this rank */
    rinfo = PMIX_NEW(pmix_rank_info_t);
    if (NULL == rinfo) {
        PMIX_RELEASE(nptr);
        free(tmp);
        return PMIX_ERR_NOMEM;
    }
    pmix_list_append(&pmix_globals.nspaces, &nptr->super);
    rinfo->pname.nspace = strdup(tmp);
    rinfo->pname.rank = rank;
    rinfo->realuid = getuid();
    rinfo->uid = geteuid();
    rinfo->realgid = getgid();
    rinfo->gid = getegid();
    pmix_list_append(&nptr->ranks, &rinfo->super);
    nptr->all_registered = true;
    free(tmp);

    return PMIX_SUCCESS;
}

// local functions for connection support
pmix_status_t pmix_server_initialize(void)
{
    /* setup the server-specific globals */
    PMIX_CONSTRUCT(&pmix_server_globals.clients, pmix_pointer_array_t);
    pmix_pointer_array_init(&pmix_server_globals.clients, 1, INT_MAX, 1);
    PMIX_CONSTRUCT(&pmix_server_globals.peer_cache, pmix_pointer_array_t);
    pmix_pointer_array_init(&pmix_server_globals.peer_cache, 1, INT_MAX, 1);
    PMIX_CONSTRUCT(&pmix_server_globals.nspaces, pmix_list_t);
    PMIX_CONSTRUCT(&pmix_server_globals.collectives, pmix_list_t);
    PMIX_CONSTRUCT(&pmix_server_globals.remote_pnd, pmix_list_t);
    PMIX_CONSTRUCT(&pmix_server_globals.local_reqs, pmix_list_t);
    PMIX_CONSTRUCT(&pmix_server_globals.gdata, pmix_list_t);
    PMIX_CONSTRUCT(&pmix_server_globals.events, pmix_list_t);
    PMIX_CONSTRUCT(&pmix_server_globals.iof, pmix_list_t);
    PMIX_CONSTRUCT(&pmix_server_globals.iof_residuals, pmix_list_t);
    PMIX_CONSTRUCT(&pmix_server_globals.psets, pmix_list_t);
    PMIX_CONSTRUCT(&pmix_server_globals.grp_collectives, pmix_list_t);

    pmix_output_verbose(2, pmix_server_globals.base_output, "pmix:server init called");

    /* setup the server verbosities */
    if (0 < pmix_server_globals.get_verbose) {
        /* set default output */
        pmix_server_globals.get_output = pmix_output_open(NULL);
        pmix_output_set_verbosity(pmix_server_globals.get_output, pmix_server_globals.get_verbose);
    }
    if (0 < pmix_server_globals.connect_verbose) {
        /* set default output */
        pmix_server_globals.connect_output = pmix_output_open(NULL);
        pmix_output_set_verbosity(pmix_server_globals.connect_output,
                                  pmix_server_globals.connect_verbose);
    }
    if (0 < pmix_server_globals.fence_verbose) {
        /* set default output */
        pmix_server_globals.fence_output = pmix_output_open(NULL);
        pmix_output_set_verbosity(pmix_server_globals.fence_output,
                                  pmix_server_globals.fence_verbose);
    }
    if (0 < pmix_server_globals.pub_verbose) {
        /* set default output */
        pmix_server_globals.pub_output = pmix_output_open(NULL);
        pmix_output_set_verbosity(pmix_server_globals.pub_output, \
                                  pmix_server_globals.pub_verbose);
    }
    if (0 < pmix_server_globals.spawn_verbose) {
        /* set default output */
        pmix_server_globals.spawn_output = pmix_output_open(NULL);
        pmix_output_set_verbosity(pmix_server_globals.spawn_output,
                                  pmix_server_globals.spawn_verbose);
    }
    if (0 < pmix_server_globals.event_verbose) {
        /* set default output */
        pmix_server_globals.event_output = pmix_output_open(NULL);
        pmix_output_set_verbosity(pmix_server_globals.event_output,
                                  pmix_server_globals.event_verbose);
    }
    if (0 < pmix_server_globals.iof_verbose) {
        /* set default output */
        pmix_server_globals.iof_output = pmix_output_open(NULL);
        pmix_output_set_verbosity(pmix_server_globals.iof_output,
                                  pmix_server_globals.iof_verbose);
    }
    /* setup the base verbosity */
    if (0 < pmix_server_globals.base_verbose) {
        /* set default output */
        pmix_server_globals.base_output = pmix_output_open(NULL);
        pmix_output_set_verbosity(pmix_server_globals.base_output,
                                  pmix_server_globals.base_verbose);
    }
    if (0 < pmix_server_globals.group_verbose) {
        /* set default output */
        pmix_server_globals.group_output = pmix_output_open(NULL);
        pmix_output_set_verbosity(pmix_server_globals.group_output,
                                  pmix_server_globals.group_verbose);
    }

    /* get our available security modules */
    security_mode = pmix_psec_base_get_available_modules();

    /* get our available bfrop modules */
    bfrops_mode = pmix_bfrops_base_get_available_modules();

    /* get available gds modules */
    gds_mode = pmix_gds_base_get_available_modules();

    return PMIX_SUCCESS;
}

static int server_init_cntr = 0;

PMIX_EXPORT pmix_status_t PMIx_server_init(pmix_server_module_t *module,
                                           pmix_info_t info[], size_t ninfo)
{
    pmix_ptl_posted_recv_t *req;
    pmix_status_t rc;
    size_t n;
    bool nspace_given = false, rank_given = false;
    bool share_topo = false;
    pmix_info_t ginfo, *iptr, evinfo[3];
    char *evar, *nspace = NULL, *suri = NULL;
    pmix_rank_t rank = PMIX_RANK_INVALID;
    pmix_rank_info_t *rinfo;
    pmix_proc_type_t ptype = PMIX_PROC_TYPE_STATIC_INIT;
    pmix_buffer_t *bfr;
    pmix_cmd_t cmd;
    pmix_cb_t cb, *cbptr;
    pmix_lock_t releaselock;
    pmix_ptl_posted_recv_t *rcv;
    bool outputio;
    bool connect_optional = false;
    bool connect_directed = false;
    char *singleton = NULL;
    pmix_data_array_t *mau = NULL;
    pmix_kval_t *kptr;
    pmix_rshift_caddy_t *cd;

    // check if an init has been called
    if (pmix_atomic_test_and_set(&pmix_globals.init_called)) {
        // did the prior call get far enough? We might be in a tight
        // race between multiple calls to PMIx_server_init - bad programming
        // technique, but all we can do is try to protect against it.
        // Test BEFORE counting the call: a caller that is handed an error
        // has no reason to call finalize, so a reference counted here
        // would never be given back and the real finalize would decline
        // to tear anything down.
        if (!pmix_atomic_check_bool(&pmix_globals.initialized)) {
            return PMIX_ERR_INIT;
        }
        // track the ref count
        pmix_atomic_fetch_add(&server_init_cntr, 1);
        /* setup the function pointers if they weren't previously defined */
        if (NULL != module && !pmix_server_globals.module_set) {
            pmix_host_server = *module;
            pmix_server_globals.module_set = true;
        }
        return PMIX_SUCCESS;
    } else {
        // track the ref count
        pmix_atomic_fetch_add(&server_init_cntr, 1);
    }

    pmix_output_verbose(2, pmix_server_globals.base_output,
        "pmix:server init called");

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

    /* init the parent procid to something innocuous */
    PMIX_LOAD_PROCID(&myparent, NULL, PMIX_RANK_UNDEF);

    if (NULL != getenv("PMIX_LAUNCHER_RNDZ_URI") ||
        NULL != getenv("PMIX_KEEPALIVE_PIPE")) {
        /* we have a parent tool, so default to
         * letting them output IOF */
        outputio = false;
    } else {
        outputio = true;
    }

    PMIX_SET_PROC_TYPE(&ptype, PMIX_PROC_SERVER);
    /* setup the function pointers */
    if (NULL != module && !pmix_server_globals.module_set) {
        pmix_host_server = *module;
        pmix_server_globals.module_set = true;
    }

    if (NULL != info) {
        for (n = 0; n < ninfo; n++) {
            if (PMIX_CHECK_KEY(&info[n], PMIX_SERVER_GATEWAY)) {
                if (PMIX_INFO_TRUE(&info[n])) {
                    PMIX_SET_PROC_TYPE(&ptype, PMIX_PROC_GATEWAY);
                }
            } else if (PMIX_CHECK_KEY(&info[n], PMIX_SERVER_SCHEDULER)) {
                if (PMIX_INFO_TRUE(&info[n])) {
                    PMIX_SET_PROC_TYPE(&ptype, PMIX_PROC_SCHEDULER);
                }
            } else if (PMIX_CHECK_KEY(&info[n], PMIX_SERVER_SYS_CONTROLLER)) {
                if (PMIX_INFO_TRUE(&info[n])) {
                    PMIX_SET_PROC_TYPE(&ptype, PMIX_PROC_SYS_CTRLR);
                }
            } else if (PMIX_CHECK_KEY(&info[n], PMIX_SERVER_TMPDIR)) {
                if (NULL != pmix_server_globals.tmpdir) {
                    free(pmix_server_globals.tmpdir);
                }
                pmix_server_globals.tmpdir = strdup(info[n].value.data.string);

            } else if (PMIX_CHECK_KEY(&info[n], PMIX_SYSTEM_TMPDIR)) {
                if (NULL != pmix_server_globals.system_tmpdir) {
                    free(pmix_server_globals.system_tmpdir);
                }
                pmix_server_globals.system_tmpdir = strdup(info[n].value.data.string);

            } else if (PMIX_CHECK_KEY(&info[n], PMIX_SERVER_NSPACE)) {
                nspace = info[n].value.data.string;
                nspace_given = true;

            } else if (PMIX_CHECK_KEY(&info[n], PMIX_SERVER_RANK)) {
                rank = info[n].value.data.rank;
                rank_given = true;

            } else if (PMIX_CHECK_KEY(&info[n], PMIX_SERVER_SHARE_TOPOLOGY)) {
                share_topo = true;

            } else if (PMIX_CHECK_KEY(&info[n], PMIX_IOF_LOCAL_OUTPUT)) {
                outputio = PMIX_INFO_TRUE(&info[n]);

            } else if (PMIX_CHECK_KEY(&info[n], PMIX_SINGLETON)) {
                if (PMIX_STRING != info[n].value.type) {
                    pmix_show_help("help-pmix-server.txt", "bad-singleton-type", true,
                                   PMIx_Data_type_string(info[n].value.type));
                    return PMIX_ERR_BAD_PARAM;
                }
                singleton = info[n].value.data.string;

            } else if (PMIX_CHECK_KEY(&info[n], PMIX_ALLOC_MAU)) {
                /* this comes straight from our host and nothing has
                 * validated it - the key says what it is, never what is
                 * in the value */
                if (PMIX_DATA_ARRAY != info[n].value.type ||
                    NULL == info[n].value.data.darray) {
                    PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
                    return PMIX_ERR_BAD_PARAM;
                }
                mau = info[n].value.data.darray;

            } else if (PMIX_CHECK_KEY(&info[n], PMIX_TOOL_CONNECT_OPTIONAL)) {
                connect_optional = PMIX_INFO_TRUE(&info[n]);

            } else if (PMIX_CHECK_KEY(&info[n], PMIX_CONNECT_TO_SYSTEM)) {
                if (PMIX_INFO_TRUE(&info[n])) {
                    connect_directed = true;
                }
            } else if (PMIX_CHECK_KEY(&info[n], PMIX_CONNECT_SYSTEM_FIRST)) {
                if (PMIX_INFO_TRUE(&info[n])) {
                    connect_directed = true;
                }
            } else if (PMIX_CHECK_KEY(&info[n], PMIX_CONNECT_TO_SCHEDULER)) {
                if (PMIX_INFO_TRUE(&info[n])) {
                    connect_directed = true;
                }
            } else if (PMIX_CHECK_KEY(&info[n], PMIX_CONNECT_TO_SYS_CONTROLLER)) {
                if (PMIX_INFO_TRUE(&info[n])) {
                    connect_directed = true;
                }
            } else if (PMIX_CHECK_KEY(&info[n], PMIX_CONNECTION_ORDER)) {
                if (PMIX_INFO_TRUE(&info[n])) {
                    connect_directed = true;
                }
            } else if (PMIX_CHECK_KEY(&info[n], PMIX_SERVER_PIDINFO)) {
                if (PMIX_INFO_TRUE(&info[n])) {
                    connect_directed = true;
                }
            } else if (PMIX_CHECK_KEY(&info[n], PMIX_TOOL_ATTACHMENT_FILE)) {
                if (PMIX_INFO_TRUE(&info[n])) {
                    connect_directed = true;
                }
            } else if (PMIX_CHECK_KEY(&info[n], PMIX_LAUNCHER_RENDEZVOUS_FILE)) {
                if (PMIX_INFO_TRUE(&info[n])) {
                    connect_directed = true;
                }
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

    /* setup the runtime - this init's the globals,
     * opens and initializes the required frameworks */
    if (PMIX_SUCCESS != (rc = pmix_rte_init(ptype.type, info, ninfo, NULL))) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    /* if we were given a keepalive pipe, register an
     * event to capture the event */
    if (NULL != (evar = getenv("PMIX_KEEPALIVE_PIPE"))) {
        keepalive_fd = strtol(evar, NULL, 10);
        pmix_event_set(pmix_globals.evbase, &parentdied, keepalive_fd,
                       PMIX_EV_READ, pdiedfn, NULL);
        pmix_event_add(&parentdied, NULL);
        pmix_unsetenv("PMIX_KEEPALIVE_PIPE", &environ);
        pmix_fd_set_cloexec(keepalive_fd); // don't let children inherit this
    }

    /* assign our internal bfrops module */
    pmix_globals.mypeer->nptr->compat.bfrops = pmix_bfrops_base_assign_module(NULL);
    if (NULL == pmix_globals.mypeer->nptr->compat.bfrops) {
        /* rc still holds SUCCESS from pmix_rte_init - report a real error */
        rc = PMIX_ERR_INIT;
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    /* set the buffer type accordingly if we are given any directives */
    evar = getenv("PMIX_BFROP_BUFFER_TYPE");
    if (NULL == evar) {
        /* just set to our default */
        pmix_globals.mypeer->nptr->compat.type = pmix_bfrops_globals.default_type;
    } else if (0 == strcmp(evar, "PMIX_BFROP_BUFFER_FULLY_DESC")) {
        pmix_globals.mypeer->nptr->compat.type = PMIX_BFROP_BUFFER_FULLY_DESC;
    } else {
        pmix_globals.mypeer->nptr->compat.type = PMIX_BFROP_BUFFER_NON_DESC;
    }

    /* if we were passed directives, use them to guide our selection
     * of security modules */
    evar = getenv("PMIX_SECURITY_MODE");
    pmix_globals.mypeer->nptr->compat.psec = pmix_psec_base_assign_module(evar);
    if (NULL == pmix_globals.mypeer->nptr->compat.psec) {
        /* rc still holds SUCCESS - report a real error */
        rc = PMIX_ERR_INIT;
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    /* assign our internal gds module */
    PMIX_INFO_LOAD(&ginfo, PMIX_GDS_MODULE, "hash", PMIX_STRING);
    pmix_globals.mypeer->nptr->compat.gds = pmix_gds_base_assign_module(&ginfo, 1);
    if (NULL == pmix_globals.mypeer->nptr->compat.gds) {
        /* rc still holds SUCCESS - report a real error */
        rc = PMIX_ERR_INIT;
        PMIX_ERROR_LOG(rc);
        PMIX_INFO_DESTRUCT(&ginfo);
        return rc;
    }
    PMIX_INFO_DESTRUCT(&ginfo);

    /* we are our own server until something else happens */
    PMIX_RETAIN(pmix_globals.mypeer);
    pmix_client_globals.myserver = pmix_globals.mypeer;

    /* setup the server-specific globals */
    if (PMIX_SUCCESS != (rc = pmix_server_initialize())) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    /* setup the IO Forwarding recv */
    rcv = PMIX_NEW(pmix_ptl_posted_recv_t);
    rcv->tag = PMIX_PTL_TAG_IOF;
    rcv->cbfunc = pmix_server_iof_handler;
    /* add it to the end of the list of recvs */
    pmix_list_append(&pmix_ptl_base.posted_recvs, &rcv->super);
    /* and the IOF flow control recv - a server may itself be feeding
     * stdin to a server above it */
    rcv = PMIX_NEW(pmix_ptl_posted_recv_t);
    rcv->tag = PMIX_PTL_TAG_IOF_CONTROL;
    rcv->cbfunc = pmix_iof_flow_control_handler;
    pmix_list_append(&pmix_ptl_base.posted_recvs, &rcv->super);
    /* set the default local output flag */
    pmix_globals.iof_flags.local_output = outputio;

    if (nspace_given) {
        PMIX_LOAD_NSPACE(pmix_globals.myid.nspace, nspace);
    } else {
        /* look for our namespace, if one was given */
        if (NULL == (evar = getenv("PMIX_SERVER_NAMESPACE"))) {
            /* use a fake namespace */
            PMIX_LOAD_NSPACE(pmix_globals.myid.nspace, "pmix-server");
        } else {
            PMIX_LOAD_NSPACE(pmix_globals.myid.nspace, evar);
        }
    }
    if (rank_given) {
        pmix_globals.myid.rank = rank;
    } else {
        /* look for our rank, if one was given */
        mypid = getpid();
        if (NULL == (evar = getenv("PMIX_SERVER_RANK"))) {
            /* use our pid */
            pmix_globals.myid.rank = mypid;
        } else {
            pmix_globals.myid.rank = strtol(evar, NULL, 10);
        }
    }

    /* copy it into mypeer entries */
    if (NULL == pmix_globals.mypeer->info) {
        rinfo = PMIX_NEW(pmix_rank_info_t);
        pmix_globals.mypeer->info = rinfo;
    } else {
        PMIX_RETAIN(pmix_globals.mypeer->info);
        rinfo = pmix_globals.mypeer->info;
    }
    if (NULL == pmix_globals.mypeer->nptr) {
        pmix_globals.mypeer->nptr = PMIX_NEW(pmix_namespace_t);
        /* ensure our own nspace is first on the list */
        PMIX_RETAIN(pmix_globals.mypeer->nptr);
        pmix_list_prepend(&pmix_globals.nspaces, &pmix_globals.mypeer->nptr->super);
    }
    pmix_globals.mypeer->nptr->nspace = strdup(pmix_globals.myid.nspace);
    rinfo->pname.nspace = strdup(pmix_globals.mypeer->nptr->nspace);
    rinfo->pname.rank = pmix_globals.myid.rank;
    rinfo->realuid = pmix_globals.realuid;
    rinfo->uid = pmix_globals.uid;
    rinfo->realgid = pmix_globals.realgid;
    rinfo->gid = pmix_globals.gid;
    pmix_client_globals.myserver->info = pmix_globals.mypeer->info;

    /* open the pmdl framework and select the active modules for this environment */
    rc = pmix_mca_base_framework_open(&pmix_pmdl_base_framework, PMIX_MCA_BASE_OPEN_DEFAULT);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    if (PMIX_SUCCESS != (rc = pmix_pmdl_base_select())) {
        return rc;
    }
    /* pass any params read from the default MCA
     * param files thru the pmdl components so they can
     * check for their vars and deal with them */
    pmix_pmdl.parse_file_envars(&pmix_mca_base_var_file_values);
    pmix_pmdl.parse_file_envars(&pmix_mca_base_var_override_values);

    /* open the psensor framework */
    rc = pmix_mca_base_framework_open(&pmix_psensor_base_framework,
                                      PMIX_MCA_BASE_OPEN_DEFAULT);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    if (PMIX_SUCCESS != (rc = pmix_psensor_base_select())) {
        return rc;
    }

    /* if we were started to support a singleton, register it now
     * so we won't reject it when it connects to us */
    if (NULL != singleton) {
        rc = register_singleton(singleton);
        if (PMIX_SUCCESS != rc) {
            return rc;
        }
    }

    /* setup the wildcard recv for inbound messages from clients */
    req = PMIX_NEW(pmix_ptl_posted_recv_t);
    req->tag = UINT32_MAX;
    req->cbfunc = pmix_server_message_handler;
    /* add it to the end of the list of recvs */
    pmix_list_append(&pmix_ptl_base.posted_recvs, &req->super);

    /* setup our IOF sinks */
    PMIX_IOF_SINK_DEFINE(&pmix_client_globals.iof_stdout, &pmix_globals.myid, 1,
                         PMIX_FWD_STDOUT_CHANNEL, pmix_iof_write_handler);
    PMIX_IOF_SINK_DEFINE(&pmix_client_globals.iof_stderr, &pmix_globals.myid, 2,
                         PMIX_FWD_STDERR_CHANNEL, pmix_iof_write_handler);

    /* register our attributes */
    if (PMIX_SUCCESS != (rc = pmix_register_server_attrs())) {
        return rc;
    }

    /* if we don't know our topology, we better get it now as we
     * increasingly rely on it - note that our host will hopefully
     * have passed it to us so we don't duplicate their storage! */
    if (PMIX_SUCCESS != (rc = pmix_hwloc_setup_topology(info, ninfo))) {
        /* if they told us to share our topology and we cannot do so,
         * then that is a reportable error */
        if (share_topo) {
            return rc;
        }
    }

    /* open the pnet and pgpu frameworks and select their active modules for this
     * environment Do this AFTER setting up the topology so the components can
     * check to see if they have any local assets */
    rc = pmix_mca_base_framework_open(&pmix_pnet_base_framework,
                                      PMIX_MCA_BASE_OPEN_DEFAULT);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    if (PMIX_SUCCESS != (rc = pmix_pnet_base_select())) {
        return rc;
    }

    rc = pmix_mca_base_framework_open(&pmix_pgpu_base_framework,
                                      PMIX_MCA_BASE_OPEN_DEFAULT);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    if (PMIX_SUCCESS != (rc = pmix_pgpu_base_select())) {
        return rc;
    }

    rc = pmix_mca_base_framework_open(&pmix_pstat_base_framework,
                                      PMIX_MCA_BASE_OPEN_DEFAULT);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    if (PMIX_SUCCESS != (rc = pmix_pstat_base_select())) {
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

    // if we were given an MAU, save it
    if (NULL != mau) {
        PMIX_KVAL_NEW(kptr, PMIX_ALLOC_MAU);
        kptr->value->type = PMIX_DATA_ARRAY;
        kptr->value->data.darray = mau;
        PMIX_GDS_STORE_KV(rc, pmix_globals.mypeer, &pmix_globals.myid, PMIX_INTERNAL, kptr);
        /* The store deep-copied the array, and this value only *borrowed*
         * our host's. Detach it before releasing: the kval destructor
         * runs value_destruct, which frees a PMIX_DATA_ARRAY payload
         * outright - so leaving it attached hands our caller's array back
         * to the heap while the caller still holds it in its own info
         * array and will free it again. Both neighbors below build a
         * payload they own (PMIX_PROC_CREATE, pmix_asprintf); this one
         * does not, which is what makes the detach necessary. */
        kptr->value->data.darray = NULL;
        PMIX_RELEASE(kptr); // maintain accounting
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            return rc;
        }
    }

    // enable show_help subsystem
    pmix_atomic_store_int(&pmix_show_help_enabled, 1);

    /* see if they gave us a rendezvous URI to which we are to call back */
    evar = getenv("PMIX_LAUNCHER_RNDZ_URI");
    if (NULL != evar) {
        /* attach to the specified tool so it can
         * tell us what we are to do */
        PMIX_INFO_CREATE(iptr, 3);
        PMIX_INFO_LOAD(&iptr[0], PMIX_SERVER_URI, evar, PMIX_STRING);
        rc = 2; // give us two seconds to connect
        PMIX_INFO_LOAD(&iptr[1], PMIX_TIMEOUT, &rc, PMIX_INT);
        /* since we are a server and they are a tool, we don't want
         * them to be our primary server to avoid circular logic */
        PMIX_INFO_LOAD(&iptr[2], PMIX_PRIMARY_SERVER, NULL, PMIX_BOOL);
        cbptr = PMIX_NEW(pmix_cb_t);
        cbptr->info = iptr;
        cbptr->ninfo = 3;
        /* the caddy's destructor releases its info array only when
         * infocopy says the array is its own - say so, or the three
         * directives (two of them carrying strdup'd strings) are leaked */
        cbptr->infocopy = true;
        PMIX_THREADSHIFT(cbptr, pmix_tool_retry_attach);

        PMIX_WAIT_THREAD(&cbptr->lock);
        rc = cbptr->status;
        PMIX_RELEASE(cbptr);

        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            return PMIX_ERR_UNREACH;
        }

        /* save our parent ID */
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
        bfr = PMIX_NEW(pmix_buffer_t);
        cmd = PMIX_REQ_CMD;
        PMIX_BFROPS_PACK(rc, pmix_client_globals.myserver, bfr, &cmd, 1, PMIX_COMMAND);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(bfr);
            return rc;
        }
        /* send to the server */
        PMIX_CONSTRUCT(&cb, pmix_cb_t);
        PMIX_PTL_SEND_RECV(rc, pmix_client_globals.myserver, bfr, job_data, (void *) &cb);
        if (PMIX_SUCCESS != rc) {
            /* the transport refused the message without taking it, so the
             * recv callback will never fire - unwind both here */
            PMIX_RELEASE(bfr);
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
        /* restore our original primary server */
        cbptr = PMIX_NEW(pmix_cb_t);
        cbptr->proc = &pmix_globals.myid;
        PMIX_THREADSHIFT(cbptr, pmix_tool_retry_set);

        /* wait for completion */
        PMIX_WAIT_THREAD(&cbptr->lock);
        rc = cbptr->status;
        PMIX_RELEASE(cbptr);
        if (PMIX_SUCCESS != rc) {
            return rc;
        }

        /* wait for debugger attach here */
        /* register for the debugger release notification */
        PMIX_CONSTRUCT_LOCK(&releaselock);
        PMIX_INFO_LOAD(&evinfo[0], PMIX_EVENT_RETURN_OBJECT, &releaselock, PMIX_POINTER);
        PMIX_INFO_LOAD(&evinfo[1], PMIX_EVENT_HDLR_NAME, "WAIT-FOR-RELEASE", PMIX_STRING);
        PMIX_INFO_LOAD(&evinfo[2], PMIX_EVENT_ONESHOT, NULL, PMIX_BOOL);
        pmix_output_verbose(2, pmix_client_globals.event_output,
                            "[%s:%d] WAITING IN INIT FOR RELEASE", pmix_globals.myid.nspace,
                            pmix_globals.myid.rank);
        cd = PMIX_NEW(pmix_rshift_caddy_t);
        cd->codes = malloc(sizeof(pmix_status_t));
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

    } else if (connect_directed) {
        /* connect to another server, if the info's direct us to */
        rc = pmix_ptl.connect_to_peer((struct pmix_peer_t *) pmix_client_globals.myserver,
                                      info, ninfo, &suri);
        if (PMIX_SUCCESS != rc) {
            if (!connect_optional) {
                return rc;
            }
            /* the connection was optional and did not happen, so there is
             * no URI to record. Falling through used to compose one anyway
             * out of a suri the failed call left NULL - undefined behavior
             * in the "%s" conversion, and on the platforms that survive it
             * a stored PMIX_SERVER_URI of "<nspace>.<rank>;(null)" that
             * every later lookup would hand back */
            free(suri);
            suri = NULL;
        } else {
            /* store the URI for subsequent lookups */
            PMIX_KVAL_NEW(kptr, PMIX_SERVER_URI);
            kptr->value->type = PMIX_STRING;
            pmix_asprintf(&kptr->value->data.string, "%s.%u;%s",
                          pmix_client_globals.myserver->info->pname.nspace,
                          pmix_client_globals.myserver->info->pname.rank, suri);
            free(suri);
            suri = NULL;
            PMIX_GDS_STORE_KV(rc, pmix_globals.mypeer, &pmix_globals.myid, PMIX_INTERNAL, kptr);
            PMIX_RELEASE(kptr); // maintain accounting
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                return rc;
            }
        }
    }

    // mark ourselves as initialized
    pmix_atomic_set_bool(&pmix_globals.initialized);

    return PMIX_SUCCESS;
}

PMIX_EXPORT pmix_status_t PMIx_server_finalize(void)
{
    int i;
    pmix_peer_t *peer;
    pmix_namespace_t *ns;

    if (!pmix_atomic_check_bool(&pmix_globals.initialized)) {
        return PMIX_ERR_INIT;
    }
    i = pmix_atomic_fetch_add(&server_init_cntr, -1);
    if (1 < i) {
        return PMIX_SUCCESS;
    }

    // mark we are no longer initialized
    pmix_atomic_unset_bool(&pmix_globals.initialized);
    pmix_globals.init_called = false;

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "pmix:server finalize called");

    /* Take down the file-scope event init may have registered, while the
     * base it is on still exists - otherwise it survives into a state
     * where its base has been destroyed under it, and the descriptor it
     * watches is never given back. Matches PMIx_tool_finalize. */
    if (0 <= keepalive_fd) {
        pmix_event_del(&parentdied);
        close(keepalive_fd);
        keepalive_fd = -1;
    }

    /* wait here until all active events have been processed */
    PMIx_Progress_thread_stop(NULL, 0);

    /* flush any residual IOF into their respective channels */
    pmix_iof_flush_residuals();
    /* flush anything that is still trying to be written out */
    pmix_iof_static_dump_output(&pmix_client_globals.iof_stdout);
    pmix_iof_static_dump_output(&pmix_client_globals.iof_stderr);

    /* pmix_rte_init constructs these two sinks for every role, so every
     * role has to tear them down - the client and tool finalize paths
     * already do, and a server leaked both (each holds a write event
     * carrying a sizable buffer) on every init/finalize cycle. It has to
     * happen here rather than in pmix_rte_finalize: iof_sink_destruct
     * reads pmix_globals.mypeer, which pmix_rte_finalize has released by
     * the time it destructs anything, and for a server it also walks
     * pmix_server_globals.iof_residuals, destructed just below. */
    PMIX_DESTRUCT(&pmix_client_globals.iof_stdout);
    PMIX_DESTRUCT(&pmix_client_globals.iof_stderr);

    pmix_ptl_base_stop_listening();

    for (i = 0; i < pmix_server_globals.clients.size; i++) {
        peer = (pmix_peer_t*)pmix_pointer_array_get_item(&pmix_server_globals.clients, i);
        if (NULL != peer) {
            /* ensure that we do the specified cleanup - if this is an
             * abnormal termination, then the peer object may not be
             * at zero refcount */
            pmix_execute_epilog(&peer->epilog);
            PMIX_RELEASE(peer);
        }
    }
    PMIX_DESTRUCT(&pmix_server_globals.clients);
    /* the synthetic peers we built to pack for foreign nspaces are ours
     * alone - nothing else holds a reference on them */
    for (i = 0; i < pmix_server_globals.peer_cache.size; i++) {
        peer = (pmix_peer_t*)pmix_pointer_array_get_item(&pmix_server_globals.peer_cache, i);
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
    // the list will be destructed in rte_finalize, but do the
    // epilog here
    PMIX_LIST_FOREACH (ns, &pmix_globals.nspaces, pmix_namespace_t) {
        /* ensure that we do the specified cleanup - if this is an
         * abnormal termination, then the nspace object may not be
         * at zero refcount */
        pmix_execute_epilog(&ns->epilog);
    }
    PMIX_LIST_DESTRUCT(&pmix_server_globals.iof);
    PMIX_LIST_DESTRUCT(&pmix_server_globals.iof_residuals);
    PMIX_LIST_DESTRUCT(&pmix_server_globals.psets);
    PMIX_LIST_DESTRUCT(&pmix_server_globals.grp_collectives);

    /* NULL each of these as it goes: they are file-scope statics that
     * outlive the library, and pmix_server_initialize() is the only thing
     * that repopulates them */
    if (NULL != security_mode) {
        free(security_mode);
        security_mode = NULL;
    }

    if (NULL != bfrops_mode) {
        free(bfrops_mode);
        bfrops_mode = NULL;
    }

    if (NULL != gds_mode) {
        free(gds_mode);
        gds_mode = NULL;
    }

    /* close the psensor framework */
    (void) pmix_mca_base_framework_close(&pmix_psensor_base_framework);
    /* close the pnet framework */
    (void) pmix_mca_base_framework_close(&pmix_pnet_base_framework);
    (void) pmix_mca_base_framework_close(&pmix_pgpu_base_framework);
    (void) pmix_mca_base_framework_close(&pmix_pstat_base_framework);

    pmix_rte_finalize();
    if (NULL != pmix_globals.mypeer) {
        PMIX_RELEASE(pmix_globals.mypeer);
        pmix_globals.mypeer = NULL;
    }

    if (NULL != pmix_server_globals.tmpdir) {
        free(pmix_server_globals.tmpdir);
        pmix_server_globals.tmpdir = NULL;
    }
    /* The system tmpdir is strdup'd alongside it in init and was being
     * kept: not just a leak per cycle, but stale state - the next init
     * finds it non-NULL and silently keeps the previous cycle's value
     * rather than re-reading the environment.
     *
     * THIS FREE MUST STAY BELOW pmix_rte_finalize(). We only ever free
     * the string; the directory itself is removed by pmix_ptl_close(),
     * which runs inside pmix_rte_finalize() and does so only if it
     * created the directory. But the second guard - the one in
     * pmix_os_dirpath_destroy() - decides by comparing its path against
     * THIS string, and reads a NULL here as "the caller has no system
     * tmpdir of its own", which is the ordinary case for a client or
     * tool and takes the *unconditional* rmdir arm. Freeing this while
     * any session-directory cleanup can still run would therefore turn
     * a guard into an rmdir of a system-defined directory. */
    if (NULL != pmix_server_globals.system_tmpdir) {
        free(pmix_server_globals.system_tmpdir);
        pmix_server_globals.system_tmpdir = NULL;
    }

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "pmix:server finalize complete");

    /* finalize the class/object system */
    pmix_class_finalize();

    return PMIX_SUCCESS;
}

/* Generic completion callback used by the blocking form of the public
 * server APIs: record the status where the waiting caller can read it
 * and wake the caller's lock. Shared by every PMIx_server_* entry point
 * that substitutes an internal callback when the caller passes NULL. */
void pmix_server_lock_opcbfunc(pmix_status_t status, void *cbdata)
{
    pmix_lock_t *lock = (pmix_lock_t *) cbdata;
    lock->status = status;
    PMIX_WAKEUP_THREAD(lock);
}

/* setup the envars for a child process */
PMIX_EXPORT pmix_status_t PMIx_server_setup_fork(const pmix_proc_t *proc, char ***env)
{
    char rankstr[128];
    pmix_listener_t *lt;
    pmix_status_t rc;
    char **varnames;
    int n;

    if (!pmix_atomic_check_bool(&pmix_globals.initialized)) {
        return PMIX_ERR_INIT;
    }

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        return PMIX_ERR_NOT_AVAILABLE;
    }

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "pmix:server setup_fork for nspace %s rank %u",
                        proc->nspace, proc->rank);

    /* pass the nspace */
    PMIx_Setenv("PMIX_NAMESPACE", proc->nspace, true, env);
    /* pass the rank */
    (void) pmix_snprintf(rankstr, 127, "%u", proc->rank);
    PMIx_Setenv("PMIX_RANK", rankstr, true, env);
    /* pass our rendezvous info */
    lt = &pmix_ptl_base.listener;
    if (NULL != lt->uri && NULL != lt->varname) {
        varnames = PMIx_Argv_split(lt->varname, ':');
        for (n = 0; NULL != varnames[n]; n++) {
            PMIx_Setenv(varnames[n], lt->uri, true, env);
        }
        PMIx_Argv_free(varnames);
    }

    /* pass our active security modules */
    PMIx_Setenv("PMIX_SECURITY_MODE", security_mode, true, env);
    /* pass the type of buffer we are using */
    if (PMIX_BFROP_BUFFER_FULLY_DESC == pmix_globals.mypeer->nptr->compat.type) {
        PMIx_Setenv("PMIX_BFROP_BUFFER_TYPE", "PMIX_BFROP_BUFFER_FULLY_DESC", true, env);
    } else {
        PMIx_Setenv("PMIX_BFROP_BUFFER_TYPE", "PMIX_BFROP_BUFFER_NON_DESC", true, env);
    }
    /* pass our available gds modules */
    PMIx_Setenv("PMIX_GDS_MODULE", gds_mode, true, env);

    /* get any PTL contribution such as tmpdir settings for session files */
    if (PMIX_SUCCESS != (rc = pmix_ptl_base_setup_fork(proc, env))) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    /* get any network contribution */
    if (PMIX_SUCCESS != (rc = pmix_pnet.setup_fork(proc, env))) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    /* get any GDS contributions */
    if (PMIX_SUCCESS != (rc = pmix_gds_base_setup_fork(proc, env))) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    /* get any contribution for the specific programming
     * model/implementation, if known */
    if (PMIX_SUCCESS != (rc = pmix_pmdl.setup_fork(proc, env))) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    /* ensure we agree on our hostname */
    PMIx_Setenv("PMIX_HOSTNAME", pmix_globals.hostname, true, env);

    /* communicate our version */
    PMIx_Setenv("PMIX_VERSION", PMIX_VERSION, true, env);

    /* pass any global contributions */
    if (NULL != pmix_server_globals.genvars) {
        for (n = 0; NULL != pmix_server_globals.genvars[n]; n++) {
            PMIx_Argv_append_nosize(env, pmix_server_globals.genvars[n]);
        }
    }

    return PMIX_SUCCESS;
}
