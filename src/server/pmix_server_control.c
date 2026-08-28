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
#include "src/util/pmix_show_help.h"

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
    /* the count arrives off the wire and is carried in a size_t, while the
     * allocation below multiplies it by sizeof(pmix_query_t) with no
     * overflow guard and then constructs every one of the elements it
     * claimed - so a count large enough to wrap that product yields a
     * short allocation whose constructor loop runs straight off the end,
     * before the unpack gets a chance to screen anything. Require the
     * count to survive the round trip through the int32_t the unpack
     * consumes it as, which bounds it well clear of the wrap */
    cnt = cd->nqueries;
    if (0 > cnt || (size_t) cnt != cd->nqueries) {
        rc = PMIX_ERR_BAD_PARAM;
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
        /* zero is the sender's own "no timestamp" value, and the only
         * test applied below is "0 < timestamp" - a -1 sentinel would
         * read as a very large positive time on a platform whose
         * time_t is unsigned, and manufacture a garbage timestamp
         * directive for exactly the peers too old to have sent one */
        timestamp = 0;
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
    /* the count arrives off the wire and is carried in a size_t while
     * every use of it goes through the int32_t unpack count, so screen
     * the truncation here. Unlike its siblings, this handler acts on
     * cd->ninfo even when the unpack below is skipped - it hands the
     * pair straight to plog - so a count that survives as zero would
     * walk a NULL array. The directive count below is worse still: it
     * indexes the array before anything is unpacked into it */
    cnt = cd->ninfo;
    if (0 > cnt || (size_t) cnt != cd->ninfo) {
        rc = PMIX_ERR_BAD_PARAM;
        PMIX_ERROR_LOG(rc);
        goto exit;
    }
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
    if (0 > cnt || (size_t) cnt != cd->ndirs) {
        rc = PMIX_ERR_BAD_PARAM;
        PMIX_ERROR_LOG(rc);
        goto exit;
    }
    /* always add the source to the directives so we
     * can tell downstream if this gets "upcalled" to
     * our host for relay */
    cd->ndirs = (size_t) cnt + 1;
    /* if a timestamp was sent, then we add it to the directives */
    if (0 < timestamp) {
        cd->ndirs++;
    }
    PMIX_INFO_CREATE(cd->directives, cd->ndirs);
    if (NULL == cd->directives) {
        rc = PMIX_ERR_NOMEM;
        goto exit;
    }
    /* the caddy owns this array and must release it when done - set
     * before the loads below so an early return cannot strand it */
    cd->dircopy = true;
    PMIX_INFO_LOAD(&cd->directives[(size_t) cnt], PMIX_LOG_SOURCE, &proc, PMIX_PROC);
    if (0 < timestamp) {
        PMIX_INFO_LOAD(&cd->directives[(size_t) cnt + 1], PMIX_LOG_TIMESTAMP,
                       &timestamp, PMIX_TIME);
    }

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
    /* screen the count before it reaches the allocator - see the note in
     * pmix_server_query above */
    cnt = cd->ninfo;
    if (0 > cnt || (size_t) cnt != cd->ninfo) {
        rc = PMIX_ERR_BAD_PARAM;
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

/* One target epilog, plus what this request would add to it. The three
 * staging lists are what make a job-control request atomic: nothing is
 * put on an epilog until the whole request is known to be acceptable,
 * because those lists outlive the request and the client that is told
 * the request failed has no way to take anything back off them. */
typedef struct {
    pmix_list_item_t super;
    pmix_epilog_t *epi;
    pmix_list_t newignores;
    pmix_list_t newdirs;
    pmix_list_t newfiles;
} pmix_srvr_epi_caddy_t;
static void epicon(pmix_srvr_epi_caddy_t *p)
{
    p->epi = NULL;
    PMIX_CONSTRUCT(&p->newignores, pmix_list_t);
    PMIX_CONSTRUCT(&p->newdirs, pmix_list_t);
    PMIX_CONSTRUCT(&p->newfiles, pmix_list_t);
}
static void epides(pmix_srvr_epi_caddy_t *p)
{
    /* on the way out of an accepted request these are empty, the entries
     * having been joined onto the epilog's own lists; on a refused one
     * they still hold everything the request would have applied */
    PMIX_LIST_DESTRUCT(&p->newignores);
    PMIX_LIST_DESTRUCT(&p->newdirs);
    PMIX_LIST_DESTRUCT(&p->newfiles);
}
static PMIX_CLASS_INSTANCE(pmix_srvr_epi_caddy_t, pmix_list_item_t, epicon, epides);

/* A directory already registered on a target epilog whose flags this
 * request would widen, per the RFC precedence rule. Staged rather than
 * applied in place for the same reason as the lists above - and with
 * more reason, since turning a non-recursive entry recursive is the most
 * destructive thing this handler can do. */
typedef struct {
    pmix_list_item_t super;
    pmix_cleanup_dir_t *dir;
} pmix_srvr_epi_upgrade_t;
static PMIX_CLASS_INSTANCE(pmix_srvr_epi_upgrade_t, pmix_list_item_t, NULL, NULL);

/* Is path already named on this list of cleanup files? Every entry
 * carries a non-NULL path by construction, so the strcmp is unguarded. */
static bool epi_has_file(pmix_list_t *list, const char *path)
{
    pmix_cleanup_file_t *cf;

    PMIX_LIST_FOREACH (cf, list, pmix_cleanup_file_t) {
        if (0 == strcmp(cf->path, path)) {
            return true;
        }
    }
    return false;
}

static pmix_cleanup_dir_t *epi_find_dir(pmix_list_t *list, const char *path)
{
    pmix_cleanup_dir_t *cd;

    PMIX_LIST_FOREACH (cd, list, pmix_cleanup_dir_t) {
        if (0 == strcmp(cd->path, path)) {
            return cd;
        }
    }
    return NULL;
}

/* Build a cleanup-file entry carrying its own copy of the path. Every
 * scan above strcmp's that member, so an entry with a NULL path must
 * never reach a list - which is why the strdup failure takes the entry
 * with it. */
static pmix_cleanup_file_t *epi_new_file(const char *path)
{
    pmix_cleanup_file_t *cf;

    cf = PMIX_NEW(pmix_cleanup_file_t);
    if (NULL == cf) {
        return NULL;
    }
    cf->path = strdup(path);
    if (NULL == cf->path) {
        PMIX_RELEASE(cf);
        return NULL;
    }
    return cf;
}

static pmix_cleanup_dir_t *epi_new_dir(const char *path)
{
    pmix_cleanup_dir_t *cdir;

    cdir = PMIX_NEW(pmix_cleanup_dir_t);
    if (NULL == cdir) {
        return NULL;
    }
    cdir->path = strdup(path);
    if (NULL == cdir->path) {
        PMIX_RELEASE(cdir);
        return NULL;
    }
    return cdir;
}

/* Expand a comma-delimited path list into one entry per path.
 * PMIX_REGISTER_CLEANUP, PMIX_REGISTER_CLEANUP_DIR and
 * PMIX_CLEANUP_IGNORE are all documented as comma-delimited lists, and
 * the expansion used to happen only in pmix_execute_epilog - so every
 * comparison in between was made against the unexpanded string, and
 * three separate things silently stopped working for a list of more
 * than one path. dirpath_destroy strcmp's an ignore against a filename
 * it has just constructed, so an ignore of "/a,/b" protected neither;
 * it decides PMIX_CLEANUP_LEAVE_TOPDIR by comparing the directory it is
 * descending against cd->path, which a list never equals, so the flag
 * was dropped; and neither the duplicate scan nor the conflict scan
 * here could see the paths inside one. Expanding at registration makes
 * every one of those a whole-path comparison, and leaves the epilog
 * nothing left to split. */
static pmix_status_t epi_cache_files(pmix_list_t *list, const char *paths)
{
    pmix_cleanup_file_t *cf;
    char **tmp;
    size_t n;

    tmp = PMIx_Argv_split(paths, ',');
    if (NULL == tmp) {
        /* the value named no path at all - a string of nothing but
         * delimiters, which reached pmix_execute_epilog as an entry
         * whose own split then returned NULL and was indexed */
        return PMIX_ERR_BAD_PARAM;
    }
    for (n = 0; NULL != tmp[n]; n++) {
        cf = epi_new_file(tmp[n]);
        if (NULL == cf) {
            PMIx_Argv_free(tmp);
            return PMIX_ERR_NOMEM;
        }
        pmix_list_append(list, &cf->super);
    }
    PMIx_Argv_free(tmp);
    return PMIX_SUCCESS;
}

static pmix_status_t epi_cache_dirs(pmix_list_t *list, const char *paths)
{
    pmix_cleanup_dir_t *cdir;
    char **tmp;
    size_t n;

    tmp = PMIx_Argv_split(paths, ',');
    if (NULL == tmp) {
        return PMIX_ERR_BAD_PARAM;
    }
    for (n = 0; NULL != tmp[n]; n++) {
        cdir = epi_new_dir(tmp[n]);
        if (NULL == cdir) {
            PMIx_Argv_free(tmp);
            return PMIX_ERR_NOMEM;
        }
        pmix_list_append(list, &cdir->super);
    }
    PMIx_Argv_free(tmp);
    return PMIX_SUCCESS;
}


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
    bool recurse = false, leave_topdir = false;
    pmix_list_t cachedirs, cachefiles, ignorefiles, epicache, upgrades;
    pmix_srvr_epi_caddy_t *epicd = NULL;
    pmix_srvr_epi_upgrade_t *upg;
    pmix_cleanup_file_t *cf, *cfptr;
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
     * leak whatever cleanup entries had already been cached. Every list is
     * destructed exactly once, on the way out: PMIX_LIST_DESTRUCT ends in
     * PMIX_DESTRUCT, which zeroes the object's magic id, so a second one
     * on the same list trips the magic-id assertion in a --enable-debug
     * build. Do not re-add the mid-function destructs. */
    PMIX_CONSTRUCT(&epicache, pmix_list_t);
    PMIX_CONSTRUCT(&cachedirs, pmix_list_t);
    PMIX_CONSTRUCT(&cachefiles, pmix_list_t);
    PMIX_CONSTRUCT(&ignorefiles, pmix_list_t);
    PMIX_CONSTRUCT(&upgrades, pmix_list_t);

    /* unpack the number of targets */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &cd->ntargets, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto exit;
    }
    /* screen the count before it reaches the allocator - see the note in
     * pmix_server_query above */
    cnt = cd->ntargets;
    if (0 > cnt || (size_t) cnt != cd->ntargets) {
        rc = PMIX_ERR_BAD_PARAM;
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
        /* the unpack fills min(packed, provided) and reports success, so
         * a peer that declares more targets than it sends leaves the tail
         * of the array default-constructed - an empty nspace at
         * PMIX_RANK_UNDEF. The walk below creates a pmix_namespace_t for
         * any target nspace it does not already know, so those phantoms
         * would each append an entry named "" to pmix_globals.nspaces for
         * the life of the server - and PMIX_CHECK_NSPACE reports an empty
         * name as matching *every* namespace, so every later list walk
         * that stops at the first match could stop there. Trust the count
         * the unpack came back with, not the one the peer declared; it is
         * also what the host is entitled to be handed below */
        if (0 == cnt) {
            /* nothing actually arrived, so give the storage back with the
             * count it was allocated with and let the "no targets" arm
             * below put the epilog on the requestor's own namespace -
             * which is what a request naming nobody means */
            PMIX_PROC_FREE(cd->targets, cd->ntargets);
            cd->targets = NULL;
        }
        cd->ntargets = (size_t) cnt;
    }

    /* check targets to find proper place to put any epilog requests */
    if (NULL == cd->targets) {
        epicd = PMIX_NEW(pmix_srvr_epi_caddy_t);
        if (NULL == epicd) {
            rc = PMIX_ERR_NOMEM;
            goto exit;
        }
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
                if (NULL == nptr->nspace) {
                    /* every lookup in this directory strcmp's that member,
                     * so a namespace carrying a NULL name is a permanent
                     * segfault of the progress thread - do not put a
                     * half-built one on the global list */
                    PMIX_RELEASE(nptr);
                    rc = PMIX_ERR_NOMEM;
                    goto exit;
                }
                pmix_list_append(&pmix_globals.nspaces, &nptr->super);
            }
            /* if the rank is wildcard, then we use the epilog for the nspace */
            if (PMIX_RANK_WILDCARD == cd->targets[n].rank) {
                epicd = PMIX_NEW(pmix_srvr_epi_caddy_t);
                if (NULL == epicd) {
                    rc = PMIX_ERR_NOMEM;
                    goto exit;
                }
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
                        if (NULL == epicd) {
                            rc = PMIX_ERR_NOMEM;
                            goto exit;
                        }
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
    /* screen the count before it reaches the allocator - see the note in
     * pmix_server_query above */
    cnt = cd->ninfo;
    if (0 > cnt || (size_t) cnt != cd->ninfo) {
        rc = PMIX_ERR_BAD_PARAM;
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
            rc = epi_cache_files(&cachefiles, cd->info[n].value.data.string);
            if (PMIX_SUCCESS != rc) {
                /* return an error */
                goto exit;
            }
        } else if (PMIX_CHECK_KEY(&cd->info[n], PMIX_REGISTER_CLEANUP_DIR)) {
            ++cnt;
            if (PMIX_STRING != cd->info[n].value.type || NULL == cd->info[n].value.data.string) {
                /* return an error */
                rc = PMIX_ERR_BAD_PARAM;
                goto exit;
            }
            rc = epi_cache_dirs(&cachedirs, cd->info[n].value.data.string);
            if (PMIX_SUCCESS != rc) {
                /* return an error */
                goto exit;
            }
        } else if (PMIX_CHECK_KEY(&cd->info[n], PMIX_CLEANUP_RECURSIVE)) {
            recurse = PMIX_INFO_TRUE(&cd->info[n]);
            ++cnt;
        } else if (PMIX_CHECK_KEY(&cd->info[n], PMIX_CLEANUP_IGNORE)) {
            if (PMIX_STRING != cd->info[n].value.type || NULL == cd->info[n].value.data.string) {
                /* return an error */
                rc = PMIX_ERR_BAD_PARAM;
                goto exit;
            }
            rc = epi_cache_files(&ignorefiles, cd->info[n].value.data.string);
            if (PMIX_SUCCESS != rc) {
                /* return an error */
                goto exit;
            }
            ++cnt;
        } else if (PMIX_CHECK_KEY(&cd->info[n], PMIX_CLEANUP_LEAVE_TOPDIR)) {
            leave_topdir = PMIX_INFO_TRUE(&cd->info[n]);
            ++cnt;
        }
    }
    if (0 < cnt) {
        /* Stage the whole request against every target epilog before any
         * of it is applied. The lists we would be appending to live as
         * long as the namespace or the peer, so anything put on one
         * outlives the request - and a request refused with
         * PMIX_ERR_CONFLICTING_CLEANUP_DIRECTIVES used to leave its
         * ignores, the directories accepted ahead of the conflicting one,
         * and any flag widening it had applied to already-registered
         * entries sitting there with the client told the whole thing had
         * failed. Widening is the sharpest of those: an entry the client
         * believes untouched becomes recursive, and deletes a subtree at
         * termination. The allocation arms had the same shape. So: build
         * everything first, and commit only once nothing can fail. */

        /* the ignores, which are what the two loops below conflict
         * against - a directive naming a path this request is also asking
         * to clean must be refused whichever order the info array put
         * them in, exactly as before */
        PMIX_LIST_FOREACH (cf, &ignorefiles, pmix_cleanup_file_t) {
            PMIX_LIST_FOREACH (epicd, &epicache, pmix_srvr_epi_caddy_t) {
                /* scan the list we are about to append to. This loop was
                 * cloned from the cleanup_files one below and kept its
                 * scan list, so it asked whether the path was already
                 * registered for *cleanup* and then appended to "ignores"
                 * regardless of the answer. Both halves of that were
                 * wrong: a repeat of the same ignore directive was never
                 * recognized as a duplicate, so every job-control request
                 * carrying one appended another copy to a list that is
                 * walked once per file by dirpath_destroy; and an ignore
                 * naming a path already registered for cleanup was
                 * silently dropped, so the epilog deleted a file the
                 * client had asked it to leave alone */
                if (epi_has_file(&epicd->epi->ignores, cf->path) ||
                    epi_has_file(&epicd->newignores, cf->path)) {
                    continue;
                }
                /* stage a copy - cf itself is a member of the local
                 * ignorefiles list (destructed below), so staging
                 * &cf->super would put one item on two lists and leave a
                 * dangling entry when ignorefiles is freed */
                cfptr = epi_new_file(cf->path);
                if (NULL == cfptr) {
                    rc = PMIX_ERR_NOMEM;
                    goto exit;
                }
                pmix_list_append(&epicd->newignores, &cfptr->super);
            }
        }
        /* now look at the directories */
        PMIX_LIST_FOREACH (cdir, &cachedirs, pmix_cleanup_dir_t) {
            PMIX_LIST_FOREACH (epicd, &epicache, pmix_srvr_epi_caddy_t) {
                /* scan the existing list of directories for any duplicate */
                cdir2 = epi_find_dir(&epicd->epi->cleanup_dirs, cdir->path);
                if (NULL != cdir2) {
                    /* duplicate - check for difference in flags per RFC
                     * precedence rules. The entry that has to absorb the
                     * more permissive flag is the one already registered
                     * on the epilog (cdir2); cdir is the request-local
                     * copy we are about to discard, and its flags are
                     * never read, so upgrading it did nothing at all.
                     * Note that a duplicate is never conflict-checked,
                     * here or before this change: the path is already
                     * registered for cleanup, so the question of whether
                     * it may be has been answered */
                    if ((!cdir2->recurse && recurse) ||
                        (!cdir2->leave_topdir && leave_topdir)) {
                        upg = PMIX_NEW(pmix_srvr_epi_upgrade_t);
                        if (NULL == upg) {
                            rc = PMIX_ERR_NOMEM;
                            goto exit;
                        }
                        upg->dir = cdir2;
                        pmix_list_append(&upgrades, &upg->super);
                    }
                    continue;
                }
                if (NULL != epi_find_dir(&epicd->newdirs, cdir->path)) {
                    /* an earlier copy in this same request already staged
                     * it, and every entry this request stages carries the
                     * request's own flags, so there is nothing to widen */
                    continue;
                }
                /* check for conflict with an ignore - one already
                 * registered, or one this request is adding */
                if (epi_has_file(&epicd->epi->ignores, cdir->path) ||
                    epi_has_file(&epicd->newignores, cdir->path)) {
                    /* return an error */
                    rc = PMIX_ERR_CONFLICTING_CLEANUP_DIRECTIVES;
                    goto exit;
                }
                cdirptr = epi_new_dir(cdir->path);
                if (NULL == cdirptr) {
                    rc = PMIX_ERR_NOMEM;
                    goto exit;
                }
                cdirptr->recurse = recurse;
                cdirptr->leave_topdir = leave_topdir;
                pmix_list_append(&epicd->newdirs, &cdirptr->super);
            }
        }
        PMIX_LIST_FOREACH (cf, &cachefiles, pmix_cleanup_file_t) {
            PMIX_LIST_FOREACH (epicd, &epicache, pmix_srvr_epi_caddy_t) {
                /* scan the existing list of files for any duplicate */
                if (epi_has_file(&epicd->epi->cleanup_files, cf->path) ||
                    epi_has_file(&epicd->newfiles, cf->path)) {
                    continue;
                }
                /* check for conflict with an ignore */
                if (epi_has_file(&epicd->epi->ignores, cf->path) ||
                    epi_has_file(&epicd->newignores, cf->path)) {
                    /* return an error */
                    rc = PMIX_ERR_CONFLICTING_CLEANUP_DIRECTIVES;
                    goto exit;
                }
                cfptr = epi_new_file(cf->path);
                if (NULL == cfptr) {
                    rc = PMIX_ERR_NOMEM;
                    goto exit;
                }
                pmix_list_append(&epicd->newfiles, &cfptr->super);
            }
        }

        /* the request is acceptable in full - commit it. Nothing below
         * here allocates or can fail, which is what makes the whole
         * request either applied or not applied */
        PMIX_LIST_FOREACH (upg, &upgrades, pmix_srvr_epi_upgrade_t) {
            if (recurse) {
                upg->dir->recurse = true;
            }
            if (leave_topdir) {
                upg->dir->leave_topdir = true;
            }
        }
        PMIX_LIST_FOREACH (epicd, &epicache, pmix_srvr_epi_caddy_t) {
            pmix_list_join(&epicd->epi->ignores,
                           pmix_list_get_end(&epicd->epi->ignores),
                           &epicd->newignores);
            pmix_list_join(&epicd->epi->cleanup_dirs,
                           pmix_list_get_end(&epicd->epi->cleanup_dirs),
                           &epicd->newdirs);
            pmix_list_join(&epicd->epi->cleanup_files,
                           pmix_list_get_end(&epicd->epi->cleanup_files),
                           &epicd->newfiles);
        }
        if ((size_t) cnt == cd->ninfo) {
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
    PMIX_LIST_DESTRUCT(&upgrades);
    return PMIX_SUCCESS;

exit:
    PMIX_RELEASE(cd);
    PMIX_LIST_DESTRUCT(&epicache);
    PMIX_LIST_DESTRUCT(&cachedirs);
    PMIX_LIST_DESTRUCT(&cachefiles);
    PMIX_LIST_DESTRUCT(&ignorefiles);
    PMIX_LIST_DESTRUCT(&upgrades);
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
    if (NULL == cb->proc) {
        PMIX_RELEASE(cb);
        return PMIX_ERR_NOMEM;
    }
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
    /* screen the count before it reaches the allocator - see the note in
     * pmix_server_query above */
    cnt = cb->ndirs;
    if (0 > cnt || (size_t) cnt != cb->ndirs) {
        rc = PMIX_ERR_BAD_PARAM;
        PMIX_ERROR_LOG(rc);
        goto exit;
    }
    /* unpack the directives */
    if (0 < cb->ndirs) {
        PMIX_INFO_CREATE(cb->directives, cb->ndirs);
        /* we unpacked these ourselves, so the caddy owns them - the
         * common monitor code borrows its directives from the caller
         * and so cannot free them on our behalf */
        cb->dircopy = true;
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
    /* the pmix_cb_t destructor does not free the proc array - only the
     * completion path through pmix_monitor_processing does, and we are
     * not going to reach it */
    PMIX_PROC_FREE(cb->proc, 1);
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
    /* screen the count before it reaches the allocator - see the note in
     * pmix_server_query above */
    cnt = cd->ninfo;
    if (0 > cnt || (size_t) cnt != cd->ninfo) {
        rc = PMIX_ERR_BAD_PARAM;
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
    rc = pmix_host_server.get_credential(&proc, cd->info, cd->ninfo, cbfunc, cd);
    /* PMIX_OPERATION_SUCCEEDED cannot express this operation's result -
     * the credential itself, which comes back through the callback the
     * switchyard always supplies, since pmix_credential_cbfunc_t carries
     * no other channel. Left alone it falls into the error arm below and
     * reaches the client as a synthesized status-only reply, which that
     * client reads as a success carrying no credential */
    if (PMIX_UNLIKELY(PMIX_OPERATION_SUCCEEDED == rc)) {
        pmix_show_help("help-pmix-server.txt", "atomic-completion-unsupported",
                       true, "PMIx_Get_credential");
        rc = PMIX_ERR_NOT_SUPPORTED;
    }
    if (PMIX_SUCCESS != rc) {
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
    /* screen the count before it reaches the allocator - see the note in
     * pmix_server_query above */
    cnt = cd->ninfo;
    if (0 > cnt || (size_t) cnt != cd->ninfo) {
        rc = PMIX_ERR_BAD_PARAM;
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
    rc = pmix_host_server.validate_credential(&proc, &cd->bo, cd->info, cd->ninfo, cbfunc, cd);
    /* as for get_credential above: the validation results are this
     * operation's product and pmix_validation_cbfunc_t is their only
     * channel, so there is no way to carry them alongside an atomic
     * completion */
    if (PMIX_UNLIKELY(PMIX_OPERATION_SUCCEEDED == rc)) {
        pmix_show_help("help-pmix-server.txt", "atomic-completion-unsupported",
                       true, "PMIx_Validate_credential");
        rc = PMIX_ERR_NOT_SUPPORTED;
    }
    if (PMIX_SUCCESS != rc) {
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
    /* the completion pair reads scd->cbdata (the server caddy nested
     * here) and takes the relfn off a wrapper caddy of its own, never
     * scd->cbfunc - so do not park the callback here as though it did */
    scd->cbdata = cd;

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
    /* screen the count before it reaches the allocator - see the note in
     * pmix_server_query above */
    cnt = scd->ninfo;
    if (0 > cnt || (size_t) cnt != scd->ninfo) {
        rc = PMIX_ERR_BAD_PARAM;
        PMIX_ERROR_LOG(rc);
        goto exit;
    }
    /* unpack the info */
    if (0 < scd->ninfo) {
        PMIX_INFO_CREATE(scd->info, scd->ninfo);
        /* we unpacked this array, so the caddy owns it - without the
         * flag the destructor leaves it behind on every path that does
         * not reach _sctrl_cbfunc's explicit free */
        scd->infocopy = true;
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
    /* screen the count before it reaches the allocator - see the note in
     * pmix_server_query above */
    cnt = scd->nunits;
    if (0 > cnt || (size_t) cnt != scd->nunits) {
        rc = PMIX_ERR_BAD_PARAM;
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
    /* screen the count before it reaches the allocator - see the note in
     * pmix_server_query above */
    cnt = scd->ninfo;
    if (0 > cnt || (size_t) cnt != scd->ninfo) {
        rc = PMIX_ERR_BAD_PARAM;
        PMIX_ERROR_LOG(rc);
        goto exit;
    }
    /* unpack the info */
    if (0 < scd->ninfo) {
        PMIX_INFO_CREATE(scd->info, scd->ninfo);
        /* the setup caddy frees info only when it is told it owns it,
         * and _resopcbfunc frees only the block name - so without this
         * the array leaked on every resource block request */
        scd->copied = true;
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
    /* the setup caddy destructor never frees nspace - it is borrowed as
     * often as it is owned - so the block name we unpacked is ours to
     * release here, exactly as _resopcbfunc does on the reply path */
    if (NULL != scd->nspace) {
        free(scd->nspace);
        scd->nspace = NULL;
    }
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
