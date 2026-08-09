/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2014-2015 Artem Y. Polyakov <artpol84@gmail.com>.
 *                         All rights reserved.
 * Copyright (c) 2016-2019 Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2016-2020 IBM Corporation.  All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * Copyright (c) 2022-2023 Triad National Security, LLC. All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/* The directive and service commands a client or tool issues to its host
 * through us: query, log, allocation requests, job control, monitoring,
 * credential get and validate, session control, resource blocks, and the
 * job-data cache refresh. Each unpacks the request, hands it to the
 * matching pmix_host_server entry point, and leaves the reply to the
 * callback the switchyard supplied. */

#include "src/include/pmix_config.h"

#include "src/include/pmix_stdint.h"

#include "include/pmix_server.h"
#include "src/include/pmix_globals.h"

#ifdef HAVE_STRING_H
#    include <string.h>
#endif
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif
#ifdef HAVE_TIME_H
#    include <time.h>
#endif

#include "src/class/pmix_list.h"
#include "src/common/pmix_attributes.h"
#include "src/common/pmix_monitor.h"
#include "src/mca/bfrops/base/base.h"
#include "src/mca/gds/base/base.h"
#include "src/mca/plog/base/base.h"
#include "src/mca/ptl/base/base.h"
#include "src/mca/psensor/psensor.h"
#include "src/runtime/pmix_rte.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_name_fns.h"
#include "src/util/pmix_output.h"

#include "pmix_server_ops.h"
#include "src/client/pmix_client_ops.h"

pmix_status_t pmix_server_query(pmix_peer_t *peer, pmix_buffer_t *buf,
                                pmix_info_cbfunc_t cbfunc, void *cbdata)
{
    int32_t cnt;
    pmix_status_t rc;
    pmix_query_caddy_t *cd;

     if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        return PMIX_ERR_NOT_AVAILABLE;
    }

   pmix_output_verbose(2, pmix_server_globals.base_output,
                        "recvd query from client");

    cd = PMIX_NEW(pmix_query_caddy_t);
    if (NULL == cd) {
        return PMIX_ERR_NOMEM;
    }
    cd->cbfunc = cbfunc;
    cd->cbdata = cbdata;  // this is the pmix_server_caddy_t we were given
    /* unpack the number of queries */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &cd->nqueries, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(cd);
        return rc;
    }
    /* unpack the queries */
    if (0 < cd->nqueries) {
        PMIX_QUERY_CREATE(cd->queries, cd->nqueries);
        if (NULL == cd->queries) {
            rc = PMIX_ERR_NOMEM;
            PMIX_RELEASE(cd);
            return rc;
        }
        cnt = cd->nqueries;
        PMIX_BFROPS_UNPACK(rc, peer, buf, cd->queries, &cnt, PMIX_QUERY);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(cd);
            return rc;
        }
    }
    PMIX_THREADSHIFT(cd, pmix_parse_localquery);
    return PMIX_SUCCESS;
}

static void localcbfn(pmix_status_t status, void *cbdata)
{
    pmix_shift_caddy_t *cb = (pmix_shift_caddy_t *) cbdata;

    if (NULL != cb->cbfunc.opcbfn) {
        cb->cbfunc.opcbfn(status, cb->cbdata);
    }
    PMIX_RELEASE(cb);
}

pmix_status_t pmix_server_log(pmix_peer_t *peer, pmix_buffer_t *buf,
                              pmix_op_cbfunc_t cbfunc,
                              void *cbdata)
{
    int32_t cnt;
    pmix_status_t rc;
    pmix_shift_caddy_t *cd;
    pmix_proc_t proc;
    time_t timestamp;
    PMIX_HIDE_UNUSED_PARAMS(cbfunc, cbdata);

    pmix_output_verbose(2, pmix_plog_base_framework.framework_output,
                        "pmix:server recvd log from client");

    /* setup the requesting peer name */
    pmix_strncpy(proc.nspace, peer->info->pname.nspace, PMIX_MAX_NSLEN);
    proc.rank = peer->info->pname.rank;

    cd = PMIX_NEW(pmix_shift_caddy_t);
    if (NULL == cd) {
        return PMIX_ERR_NOMEM;
    }
    if (PMIX_PEER_IS_EARLIER(peer, 3, 0, 0)) {
        timestamp = -1;
    } else {
        /* unpack the timestamp */
        cnt = 1;
        PMIX_BFROPS_UNPACK(rc, peer, buf, &timestamp, &cnt, PMIX_TIME);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto exit;
        }
    }

    /* unpack the number of data */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &cd->ninfo, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto exit;
    }
    cnt = cd->ninfo;
    PMIX_INFO_CREATE(cd->info, cd->ninfo);
    /* the caddy owns this array and must release it when done */
    cd->infocopy = true;
    /* unpack the data */
    if (0 < cnt) {
        PMIX_BFROPS_UNPACK(rc, peer, buf, cd->info, &cnt, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto exit;
        }
    }
    /* unpack the number of directives */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &cd->ndirs, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto exit;
    }
    cnt = cd->ndirs;
    /* always add the source to the directives so we
     * can tell downstream if this gets "upcalled" to
     * our host for relay */
    cd->ndirs = cnt + 1;
    /* if a timestamp was sent, then we add it to the directives */
    if (0 < timestamp) {
        cd->ndirs++;
        PMIX_INFO_CREATE(cd->directives, cd->ndirs);
        PMIX_INFO_LOAD(&cd->directives[cnt], PMIX_LOG_SOURCE, &proc, PMIX_PROC);
        PMIX_INFO_LOAD(&cd->directives[cnt + 1], PMIX_LOG_TIMESTAMP, &timestamp, PMIX_TIME);
    } else {
        PMIX_INFO_CREATE(cd->directives, cd->ndirs);
        PMIX_INFO_LOAD(&cd->directives[cnt], PMIX_LOG_SOURCE, &proc, PMIX_PROC);
    }
    /* the caddy owns this array and must release it when done */
    cd->dircopy = true;

    /* unpack the directives */
    if (0 < cnt) {
        PMIX_BFROPS_UNPACK(rc, peer, buf, cd->directives, &cnt, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto exit;
        }
    }

    /* if we are not the gateway, or the host-only log param is set,
     * then pass this up to the host for transfer to the gateway, if
     * available */
    if (!PMIX_PEER_IS_GATEWAY(pmix_globals.mypeer) || pmix_log_host_only) {
        pmix_output_verbose(2, pmix_plog_base_framework.framework_output,
                            "pmix:server not gateway");
        cd->cbfunc.opcbfn = cbfunc;
        cd->cbdata = cbdata;
        if (NULL != pmix_host_server.log2) {
            pmix_output_verbose(2, pmix_plog_base_framework.framework_output,
                                "pmix:server using log2 upcall");
            rc = pmix_host_server.log2(&proc, cd->info, cd->ninfo,
                                       cd->directives, cd->ndirs,
                                       localcbfn, (void *) cd);
            if (PMIX_SUCCESS != rc) {
                PMIX_RELEASE(cd);
                return rc;
            }

        } else if (NULL != pmix_host_server.log) {
            pmix_output_verbose(2, pmix_plog_base_framework.framework_output,
                                "pmix:server using log upcall");
            pmix_host_server.log(&proc, cd->info, cd->ninfo,
                                 cd->directives, cd->ndirs,
                                 localcbfn, (void *) cd);
        } else {
            // tell the client that we cannot do this
            PMIX_RELEASE(cd);
            return PMIX_ERR_NOT_AVAILABLE;
        }
        return PMIX_SUCCESS;
    }

    /* pass it down */
    pmix_output_verbose(2, pmix_plog_base_framework.framework_output,
                        "pmix:server processing locally");
    rc = pmix_plog.log(&proc, cd->info, cd->ninfo, cd->directives, cd->ndirs);
    if (PMIX_SUCCESS == rc) {
        // mark that it was done atomically
        rc = PMIX_OPERATION_SUCCEEDED;
    }

exit:
    PMIX_RELEASE(cd);
    return rc;
}

pmix_status_t pmix_server_alloc(pmix_peer_t *peer, pmix_buffer_t *buf,
                                pmix_info_cbfunc_t cbfunc,
                                void *cbdata)
{
    int32_t cnt;
    pmix_status_t rc;
    pmix_query_caddy_t *cd;
    pmix_proc_t proc;
    pmix_alloc_directive_t directive;

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "%s recvd allocate request from client %s",
                        PMIX_NAME_PRINT(&pmix_globals.myid),
                        PMIX_PEER_PRINT(peer));

    if (NULL == pmix_host_server.allocate) {
        return PMIX_ERR_NOT_SUPPORTED;
    }

    cd = PMIX_NEW(pmix_query_caddy_t);
    if (NULL == cd) {
        return PMIX_ERR_NOMEM;
    }
    cd->cbdata = cbdata;

    /* unpack the directive */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &directive, &cnt, PMIX_ALLOC_DIRECTIVE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto exit;
    }

    /* unpack the number of info objects */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &cd->ninfo, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto exit;
    }
    /* unpack the info */
    if (0 < cd->ninfo) {
        PMIX_INFO_CREATE(cd->info, cd->ninfo);
        cnt = cd->ninfo;
        PMIX_BFROPS_UNPACK(rc, peer, buf, cd->info, &cnt, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto exit;
        }
    }

    /* setup the requesting peer name */
    pmix_strncpy(proc.nspace, peer->info->pname.nspace, PMIX_MAX_NSLEN);
    proc.rank = peer->info->pname.rank;

    /* ask the host to execute the request */
    rc = pmix_host_server.allocate(&proc, directive, cd->info, cd->ninfo, cbfunc, cd);
    if (PMIX_SUCCESS != rc) {
        goto exit;
    }
    return PMIX_SUCCESS;

exit:
    PMIX_RELEASE(cd);
    return rc;
}

typedef struct {
    pmix_list_item_t super;
    pmix_epilog_t *epi;
} pmix_srvr_epi_caddy_t;
static PMIX_CLASS_INSTANCE(pmix_srvr_epi_caddy_t, pmix_list_item_t, NULL, NULL);

pmix_status_t pmix_server_job_ctrl(pmix_peer_t *peer, pmix_buffer_t *buf,
                                   pmix_info_cbfunc_t cbfunc,
                                   void *cbdata)
{
    int32_t cnt, m;
    pmix_status_t rc;
    pmix_query_caddy_t *cd;
    pmix_namespace_t *nptr, *tmp;
    pmix_peer_t *pr;
    pmix_proc_t proc;
    size_t n;
    bool recurse = false, leave_topdir = false, duplicate;
    pmix_list_t cachedirs, cachefiles, ignorefiles, epicache;
    pmix_srvr_epi_caddy_t *epicd = NULL;
    pmix_cleanup_file_t *cf, *cf2, *cfptr;
    pmix_cleanup_dir_t *cdir, *cdir2, *cdirptr;

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "recvd job control request from client");

    if (NULL == pmix_host_server.job_control) {
        return PMIX_ERR_NOT_SUPPORTED;
    }

    cd = PMIX_NEW(pmix_query_caddy_t);
    if (NULL == cd) {
        return PMIX_ERR_NOMEM;
    }
    cd->cbdata = cbdata;

    /* construct every local list up front so that a single cleanup at the
     * exit label covers all of them - the bad-param and no-memory paths in
     * the directive scan below used to jump past their destructors and
     * leak whatever cleanup entries had already been cached. Draining a
     * list twice is harmless (PMIX_LIST_DESTRUCT leaves it re-initialized),
     * so the mid-function destructs can stay where they are. */
    PMIX_CONSTRUCT(&epicache, pmix_list_t);
    PMIX_CONSTRUCT(&cachedirs, pmix_list_t);
    PMIX_CONSTRUCT(&cachefiles, pmix_list_t);
    PMIX_CONSTRUCT(&ignorefiles, pmix_list_t);

    /* unpack the number of targets */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &cd->ntargets, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto exit;
    }
    if (0 < cd->ntargets) {
        PMIX_PROC_CREATE(cd->targets, cd->ntargets);
        cnt = cd->ntargets;
        PMIX_BFROPS_UNPACK(rc, peer, buf, cd->targets, &cnt, PMIX_PROC);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto exit;
        }
    }

    /* check targets to find proper place to put any epilog requests */
    if (NULL == cd->targets) {
        epicd = PMIX_NEW(pmix_srvr_epi_caddy_t);
        epicd->epi = &peer->nptr->epilog;
        pmix_list_append(&epicache, &epicd->super);
    } else {
        for (n = 0; n < cd->ntargets; n++) {
            /* find the nspace of this proc */
            nptr = NULL;
            PMIX_LIST_FOREACH (tmp, &pmix_globals.nspaces, pmix_namespace_t) {
                if (0 == strcmp(tmp->nspace, cd->targets[n].nspace)) {
                    nptr = tmp;
                    break;
                }
            }
            if (NULL == nptr) {
                nptr = PMIX_NEW(pmix_namespace_t);
                if (NULL == nptr) {
                    rc = PMIX_ERR_NOMEM;
                    goto exit;
                }
                nptr->nspace = strdup(cd->targets[n].nspace);
                pmix_list_append(&pmix_globals.nspaces, &nptr->super);
            }
            /* if the rank is wildcard, then we use the epilog for the nspace */
            if (PMIX_RANK_WILDCARD == cd->targets[n].rank) {
                epicd = PMIX_NEW(pmix_srvr_epi_caddy_t);
                epicd->epi = &nptr->epilog;
                pmix_list_append(&epicache, &epicd->super);
            } else {
                /* we need to find the precise peer - we can only
                 * do cleanup for a local client */
                for (m = 0; m < pmix_server_globals.clients.size; m++) {
                    pr = (pmix_peer_t *)pmix_pointer_array_get_item(&pmix_server_globals.clients, m);
                    if (NULL == pr) {
                        continue;
                    }
                    if (!PMIX_CHECK_NSPACE(pr->info->pname.nspace, cd->targets[n].nspace)) {
                        continue;
                    }
                    if (pr->info->pname.rank == cd->targets[n].rank) {
                        epicd = PMIX_NEW(pmix_srvr_epi_caddy_t);
                        epicd->epi = &pr->epilog;
                        pmix_list_append(&epicache, &epicd->super);
                        break;
                    }
                }
            }
        }
    }

    /* unpack the number of info objects */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &cd->ninfo, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto exit;
    }
    /* unpack the info */
    if (0 < cd->ninfo) {
        PMIX_INFO_CREATE(cd->info, cd->ninfo);
        cnt = cd->ninfo;
        PMIX_BFROPS_UNPACK(rc, peer, buf, cd->info, &cnt, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto exit;
        }
    }

    /* if this includes a request for post-termination cleanup, we handle
     * that request ourselves */
    cnt = 0; // track how many infos are cleanup related
    for (n = 0; n < cd->ninfo; n++) {
        if (PMIX_CHECK_KEY(&cd->info[n], PMIX_REGISTER_CLEANUP)) {
            ++cnt;
            if (PMIX_STRING != cd->info[n].value.type || NULL == cd->info[n].value.data.string) {
                /* return an error */
                rc = PMIX_ERR_BAD_PARAM;
                goto exit;
            }
            cf = PMIX_NEW(pmix_cleanup_file_t);
            if (NULL == cf) {
                /* return an error */
                rc = PMIX_ERR_NOMEM;
                goto exit;
            }
            cf->path = strdup(cd->info[n].value.data.string);
            pmix_list_append(&cachefiles, &cf->super);
        } else if (PMIX_CHECK_KEY(&cd->info[n], PMIX_REGISTER_CLEANUP_DIR)) {
            ++cnt;
            if (PMIX_STRING != cd->info[n].value.type || NULL == cd->info[n].value.data.string) {
                /* return an error */
                rc = PMIX_ERR_BAD_PARAM;
                goto exit;
            }
            cdir = PMIX_NEW(pmix_cleanup_dir_t);
            if (NULL == cdir) {
                /* return an error */
                rc = PMIX_ERR_NOMEM;
                goto exit;
            }
            cdir->path = strdup(cd->info[n].value.data.string);
            pmix_list_append(&cachedirs, &cdir->super);
        } else if (PMIX_CHECK_KEY(&cd->info[n], PMIX_CLEANUP_RECURSIVE)) {
            recurse = PMIX_INFO_TRUE(&cd->info[n]);
            ++cnt;
        } else if (PMIX_CHECK_KEY(&cd->info[n], PMIX_CLEANUP_IGNORE)) {
            if (PMIX_STRING != cd->info[n].value.type || NULL == cd->info[n].value.data.string) {
                /* return an error */
                rc = PMIX_ERR_BAD_PARAM;
                goto exit;
            }
            cf = PMIX_NEW(pmix_cleanup_file_t);
            if (NULL == cf) {
                /* return an error */
                rc = PMIX_ERR_NOMEM;
                goto exit;
            }
            cf->path = strdup(cd->info[n].value.data.string);
            pmix_list_append(&ignorefiles, &cf->super);
            ++cnt;
        } else if (PMIX_CHECK_KEY(&cd->info[n], PMIX_CLEANUP_LEAVE_TOPDIR)) {
            leave_topdir = PMIX_INFO_TRUE(&cd->info[n]);
            ++cnt;
        }
    }
    if (0 < cnt) {
        /* handle any ignore directives first */
        PMIX_LIST_FOREACH (cf, &ignorefiles, pmix_cleanup_file_t) {
            PMIX_LIST_FOREACH (epicd, &epicache, pmix_srvr_epi_caddy_t) {
                /* scan the existing list of files for any duplicate */
                duplicate = false;
                PMIX_LIST_FOREACH (cf2, &epicd->epi->cleanup_files, pmix_cleanup_file_t) {
                    if (0 == strcmp(cf2->path, cf->path)) {
                        duplicate = true;
                        break;
                    }
                }
                if (!duplicate) {
                    /* append a copy to the end of the list - cf itself is a
                     * member of the local ignorefiles list (destructed below),
                     * so appending &cf->super would put one item on two lists
                     * and leave a dangling entry when ignorefiles is freed */
                    cfptr = PMIX_NEW(pmix_cleanup_file_t);
                    cfptr->path = strdup(cf->path);
                    pmix_list_append(&epicd->epi->ignores, &cfptr->super);
                }
            }
        }
        PMIX_LIST_DESTRUCT(&ignorefiles);
        /* now look at the directories */
        PMIX_LIST_FOREACH (cdir, &cachedirs, pmix_cleanup_dir_t) {
            PMIX_LIST_FOREACH (epicd, &epicache, pmix_srvr_epi_caddy_t) {
                /* scan the existing list of directories for any duplicate */
                duplicate = false;
                PMIX_LIST_FOREACH (cdir2, &epicd->epi->cleanup_dirs, pmix_cleanup_dir_t) {
                    if (0 == strcmp(cdir2->path, cdir->path)) {
                        /* duplicate - check for difference in flags per RFC
                         * precedence rules */
                        if (!cdir->recurse && recurse) {
                            cdir->recurse = recurse;
                        }
                        if (!cdir->leave_topdir && leave_topdir) {
                            cdir->leave_topdir = leave_topdir;
                        }
                        duplicate = true;
                        break;
                    }
                }
                if (!duplicate) {
                    /* check for conflict with ignore */
                    PMIX_LIST_FOREACH (cf, &epicd->epi->ignores, pmix_cleanup_file_t) {
                        if (0 == strcmp(cf->path, cdir->path)) {
                            /* return an error */
                            rc = PMIX_ERR_CONFLICTING_CLEANUP_DIRECTIVES;
                            PMIX_LIST_DESTRUCT(&cachedirs);
                            PMIX_LIST_DESTRUCT(&cachefiles);
                            goto exit;
                        }
                    }
                    /* append it to the end of the list */
                    cdirptr = PMIX_NEW(pmix_cleanup_dir_t);
                    cdirptr->path = strdup(cdir->path);
                    cdirptr->recurse = recurse;
                    cdirptr->leave_topdir = leave_topdir;
                    pmix_list_append(&epicd->epi->cleanup_dirs, &cdirptr->super);
                }
            }
        }
        PMIX_LIST_DESTRUCT(&cachedirs);
        PMIX_LIST_FOREACH (cf, &cachefiles, pmix_cleanup_file_t) {
            PMIX_LIST_FOREACH (epicd, &epicache, pmix_srvr_epi_caddy_t) {
                /* scan the existing list of files for any duplicate */
                duplicate = false;
                PMIX_LIST_FOREACH (cf2, &epicd->epi->cleanup_files, pmix_cleanup_file_t) {
                    if (0 == strcmp(cf2->path, cf->path)) {
                        duplicate = true;
                        break;
                    }
                }
                if (!duplicate) {
                    /* check for conflict with ignore */
                    PMIX_LIST_FOREACH (cf2, &epicd->epi->ignores, pmix_cleanup_file_t) {
                        if (0 == strcmp(cf->path, cf2->path)) {
                            /* return an error - cachedirs was already
                             * destructed above, so only cachefiles remains */
                            rc = PMIX_ERR_CONFLICTING_CLEANUP_DIRECTIVES;
                            PMIX_LIST_DESTRUCT(&cachefiles);
                            goto exit;
                        }
                    }
                    /* append it to the end of the list */
                    cfptr = PMIX_NEW(pmix_cleanup_file_t);
                    cfptr->path = strdup(cf->path);
                    pmix_list_append(&epicd->epi->cleanup_files, &cfptr->super);
                }
            }
        }
        PMIX_LIST_DESTRUCT(&cachefiles);
        if (cnt == (int) cd->ninfo) {
            /* nothing more to do */
            rc = PMIX_OPERATION_SUCCEEDED;
            goto exit;
        }
    }

    /* setup the requesting peer name */
    pmix_strncpy(proc.nspace, peer->info->pname.nspace, PMIX_MAX_NSLEN);
    proc.rank = peer->info->pname.rank;

    /* ask the host to execute the request */
    if (PMIX_SUCCESS
        != (rc = pmix_host_server.job_control(&proc, cd->targets, cd->ntargets, cd->info, cd->ninfo,
                                              cbfunc, cd))) {
        goto exit;
    }
    PMIX_LIST_DESTRUCT(&epicache);
    PMIX_LIST_DESTRUCT(&cachedirs);
    PMIX_LIST_DESTRUCT(&cachefiles);
    PMIX_LIST_DESTRUCT(&ignorefiles);
    return PMIX_SUCCESS;

exit:
    PMIX_RELEASE(cd);
    PMIX_LIST_DESTRUCT(&epicache);
    PMIX_LIST_DESTRUCT(&cachedirs);
    PMIX_LIST_DESTRUCT(&cachefiles);
    PMIX_LIST_DESTRUCT(&ignorefiles);
    return rc;
}

pmix_status_t pmix_server_monitor(pmix_peer_t *peer, pmix_buffer_t *buf,
                                  pmix_info_cbfunc_t cbfunc,
                                  void *cbdata)
{
    int32_t cnt;
    pmix_status_t rc;
    pmix_cb_t *cb;

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        return PMIX_ERR_NOT_AVAILABLE;
    }

   pmix_output_verbose(2, pmix_server_globals.base_output,
                        "recvd monitor request from client %s",
                        PMIX_PEER_PRINT(peer));

    cb = PMIX_NEW(pmix_cb_t);
    if (NULL == cb) {
        return PMIX_ERR_NOMEM;
    }
    PMIX_PROC_CREATE(cb->proc, 1);
    PMIX_LOAD_PROCID(&cb->proc[0], peer->info->pname.nspace, peer->info->pname.rank);
    cb->nprocs = 1;
    cb->cbdata = cbdata;
    cb->cbfunc.infofn = cbfunc;

    /* unpack what is to be monitored */
    cb->info = PMIx_Info_create(1);
    cb->infocopy = true;
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, cb->info, &cnt, PMIX_INFO);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto exit;
    }

    /* unpack the error code */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &cb->status, &cnt, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto exit;
    }

    /* unpack the number of directives */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &cb->ndirs, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto exit;
    }
    /* unpack the directives */
    if (0 < cb->ndirs) {
        PMIX_INFO_CREATE(cb->directives, cb->ndirs);
        cnt = cb->ndirs;
        PMIX_BFROPS_UNPACK(rc, peer, buf, cb->directives, &cnt, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto exit;
        }
    }

    // pass this over to be processed
    PMIX_THREADSHIFT(cb, pmix_monitor_processing);
    return PMIX_SUCCESS;

exit:
    PMIX_RELEASE(cb);
    return rc;
}

pmix_status_t pmix_server_get_credential(pmix_peer_t *peer, pmix_buffer_t *buf,
                                         pmix_credential_cbfunc_t cbfunc, void *cbdata)
{
    int32_t cnt;
    pmix_status_t rc;
    pmix_query_caddy_t *cd;
    pmix_proc_t proc;

    pmix_output_verbose(2, pmix_globals.debug_output, "recvd get credential request from client");

    if (NULL == pmix_host_server.get_credential) {
        return PMIX_ERR_NOT_SUPPORTED;
    }

    cd = PMIX_NEW(pmix_query_caddy_t);
    if (NULL == cd) {
        return PMIX_ERR_NOMEM;
    }
    cd->cbdata = cbdata;

    /* unpack the number of directives */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &cd->ninfo, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto exit;
    }
    /* unpack the directives */
    if (0 < cd->ninfo) {
        PMIX_INFO_CREATE(cd->info, cd->ninfo);
        cnt = cd->ninfo;
        PMIX_BFROPS_UNPACK(rc, peer, buf, cd->info, &cnt, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto exit;
        }
    }

    /* setup the requesting peer name */
    pmix_strncpy(proc.nspace, peer->info->pname.nspace, PMIX_MAX_NSLEN);
    proc.rank = peer->info->pname.rank;

    /* ask the host to execute the request */
    if (PMIX_SUCCESS
        != (rc = pmix_host_server.get_credential(&proc, cd->info, cd->ninfo, cbfunc, cd))) {
        goto exit;
    }
    return PMIX_SUCCESS;

exit:
    PMIX_RELEASE(cd);
    return rc;
}

pmix_status_t pmix_server_validate_credential(pmix_peer_t *peer, pmix_buffer_t *buf,
                                              pmix_validation_cbfunc_t cbfunc, void *cbdata)
{
    int32_t cnt;
    pmix_status_t rc;
    pmix_query_caddy_t *cd;
    pmix_proc_t proc;

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "recvd validate credential request from client");

    if (NULL == pmix_host_server.validate_credential) {
        return PMIX_ERR_NOT_SUPPORTED;
    }

    cd = PMIX_NEW(pmix_query_caddy_t);
    if (NULL == cd) {
        return PMIX_ERR_NOMEM;
    }
    cd->cbdata = cbdata;

    /* unpack the credential */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &cd->bo, &cnt, PMIX_BYTE_OBJECT);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto exit;
    }

    /* unpack the number of directives */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &cd->ninfo, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto exit;
    }
    /* unpack the directives */
    if (0 < cd->ninfo) {
        PMIX_INFO_CREATE(cd->info, cd->ninfo);
        cnt = cd->ninfo;
        PMIX_BFROPS_UNPACK(rc, peer, buf, cd->info, &cnt, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto exit;
        }
    }

    /* setup the requesting peer name */
    pmix_strncpy(proc.nspace, peer->info->pname.nspace, PMIX_MAX_NSLEN);
    proc.rank = peer->info->pname.rank;

    /* ask the host to execute the request */
    if (PMIX_SUCCESS
        != (rc = pmix_host_server.validate_credential(&proc, &cd->bo, cd->info, cd->ninfo, cbfunc,
                                                      cd))) {
        goto exit;
    }
    return PMIX_SUCCESS;

exit:
    PMIX_RELEASE(cd);
    return rc;
}

pmix_status_t pmix_server_session_ctrl(pmix_server_caddy_t *cd,
                                       pmix_buffer_t *buf,
                                       pmix_info_cbfunc_t cbfunc)
{
    int32_t cnt;
    pmix_status_t rc;
    pmix_shift_caddy_t *scd;
    pmix_proc_t proc;

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "%s recvd session ctrl request from client %s",
                        PMIX_NAME_PRINT(&pmix_globals.myid),
                        PMIX_PEER_PRINT(cd->peer));

    if (NULL == pmix_host_server.session_control) {
        return PMIX_ERR_NOT_SUPPORTED;
    }

    scd = PMIX_NEW(pmix_shift_caddy_t);
    if (NULL == scd) {
        return PMIX_ERR_NOMEM;
    }
    scd->cbdata = cd;
    scd->cbfunc.infocbfunc = cbfunc;

    /* unpack the sessionID */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, cd->peer, buf, &scd->sessionid, &cnt, PMIX_UINT32);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto exit;
    }

    /* unpack the number of info objects */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, cd->peer, buf, &scd->ninfo, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto exit;
    }
    /* unpack the info */
    if (0 < scd->ninfo) {
        PMIX_INFO_CREATE(scd->info, scd->ninfo);
        cnt = scd->ninfo;
        PMIX_BFROPS_UNPACK(rc, cd->peer, buf, scd->info, &cnt, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto exit;
        }
    }

    /* setup the requesting peer name */
    pmix_strncpy(proc.nspace, cd->peer->info->pname.nspace, PMIX_MAX_NSLEN);
    proc.rank = cd->peer->info->pname.rank;

    /* ask the host to execute the request */
    rc = pmix_host_server.session_control(&proc, scd->sessionid,
                                          scd->info, scd->ninfo, cbfunc, scd);
    if (PMIX_SUCCESS != rc) {
        goto exit;
    }
    return PMIX_SUCCESS;

exit:
    PMIX_RELEASE(scd);
    return rc;
}

pmix_status_t pmix_server_resblk(pmix_server_caddy_t *cd,
                                 pmix_buffer_t *buf,
                                 pmix_op_cbfunc_t cbfunc)
{
    int32_t cnt;
    pmix_status_t rc;
    pmix_setup_caddy_t *scd;
    pmix_proc_t proc;
    pmix_resource_block_directive_t directive;

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "%s recvd resource block request from client %s",
                        PMIX_NAME_PRINT(&pmix_globals.myid),
                        PMIX_PEER_PRINT(cd->peer));

    if (NULL == pmix_host_server.resource_block) {
        return PMIX_ERR_NOT_SUPPORTED;
    }

    scd = PMIX_NEW(pmix_setup_caddy_t);
    if (NULL == scd) {
        return PMIX_ERR_NOMEM;
    }
    scd->cbdata = cd;

    /* unpack the directive */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, cd->peer, buf, &directive, &cnt, PMIX_RESBLOCK_DIRECTIVE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto exit;
    }

    // unpack the block name
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, cd->peer, buf, &scd->nspace, &cnt, PMIX_STRING);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto exit;
    }

    /* unpack the number of units */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, cd->peer, buf, &scd->nunits, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto exit;
    }
    /* unpack the resource units */
    if (0 < scd->nunits) {
        PMIX_RESOURCE_UNIT_CREATE(scd->units, scd->nunits);
        cnt = scd->nunits;
        PMIX_BFROPS_UNPACK(rc, cd->peer, buf, scd->units, &cnt, PMIX_RESOURCE_UNIT);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto exit;
        }
    }

    /* unpack the number of info */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, cd->peer, buf, &scd->ninfo, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto exit;
    }
    /* unpack the info */
    if (0 < scd->ninfo) {
        PMIX_INFO_CREATE(scd->info, scd->ninfo);
        cnt = scd->ninfo;
        PMIX_BFROPS_UNPACK(rc, cd->peer, buf, scd->info, &cnt, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto exit;
        }
    }

    /* setup the requesting peer name */
    pmix_strncpy(proc.nspace, cd->peer->info->pname.nspace, PMIX_MAX_NSLEN);
    proc.rank = cd->peer->info->pname.rank;

    /* ask the host to execute the request */
    rc = pmix_host_server.resource_block(&proc, directive, scd->nspace,
                                         scd->units, scd->nunits,
                                         scd->info, scd->ninfo,
                                         cbfunc, scd);
    if (PMIX_SUCCESS != rc) {
        goto exit;
    }
    return PMIX_SUCCESS;

exit:
    PMIX_RELEASE(scd);
    return rc;
}

pmix_status_t pmix_server_refresh_cache(pmix_server_caddy_t *cd,
                                        pmix_buffer_t *buf,
                                        pmix_op_cbfunc_t cbfunc)
{
    pmix_proc_t p;
    char *nspace;
    int cnt;
    pmix_status_t rc;
    pmix_cb_t cb;
    pmix_buffer_t *pbkt;
    pmix_kval_t *kv;
    PMIX_HIDE_UNUSED_PARAMS(cbfunc);

    // unpack the ID of the proc being requested
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, cd->peer, buf, &nspace, &cnt, PMIX_STRING);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    PMIX_LOAD_NSPACE(p.nspace, nspace);
    free(nspace);

    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, cd->peer, buf, &p.rank, &cnt, PMIX_PROC_RANK);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    /* retrieve the data for the specific rank they are asking about */
    PMIX_CONSTRUCT(&cb, pmix_cb_t);
    cb.proc = &p;
    cb.scope = PMIX_REMOTE;
    cb.copy = false;
    PMIX_GDS_FETCH_KV(rc, pmix_globals.mypeer, &cb);
    /* the fetch reports its result through the macro's status argument -
     * cb.status is left at its constructed default, so packing that told
     * the client SUCCESS even when nothing was found */
    cb.status = rc;

    // pack it up
    pbkt = PMIX_NEW(pmix_buffer_t);
    if (NULL == pbkt) {
        PMIX_DESTRUCT(&cb);
        return PMIX_ERR_NOMEM;
    }
    // start with the status
    PMIX_BFROPS_PACK(rc, cd->peer, pbkt, &cb.status, 1, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(pbkt);
        PMIX_DESTRUCT(&cb);
        return rc;
    }
    PMIX_LIST_FOREACH(kv, &cb.kvs, pmix_kval_t) {
        PMIX_BFROPS_PACK(rc, cd->peer, pbkt, kv, 1, PMIX_KVAL);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(pbkt);
            PMIX_DESTRUCT(&cb);
            return rc;
        }
    }
    PMIX_DESTRUCT(&cb);

    // send it back to the requestor
    PMIX_SERVER_QUEUE_REPLY(rc, cd->peer, cd->hdr.tag, pbkt);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(pbkt);
    }

    /* we queued the reply ourselves and return SUCCESS, so the switchyard
     * will not release the caddy - we must do so here */
    PMIX_RELEASE(cd);
    return PMIX_SUCCESS;
}
