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
 * Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
 * Copyright (c) 2022-2023 Triad National Security, LLC. All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "src/include/pmix_config.h"

#include "src/include/pmix_socket_errno.h"
#include "src/include/pmix_stdint.h"

#include "include/pmix_server.h"
#include "src/include/pmix_globals.h"

#ifdef HAVE_STRING_H
#    include <string.h>
#endif
#ifdef HAVE_SYS_STAT_H
#    include <sys/stat.h>
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
#ifdef HAVE_TIME_H
#    include <time.h>
#endif
#include <event.h>

#ifndef MAX
#    define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

#include "src/class/pmix_hotel.h"
#include "src/class/pmix_list.h"
#include "src/common/pmix_attributes.h"
#include "src/common/pmix_iof.h"
#include "src/hwloc/pmix_hwloc.h"
#include "src/mca/bfrops/base/base.h"
#include "src/mca/gds/base/base.h"
#include "src/mca/plog/plog.h"
#include "src/mca/pnet/pnet.h"
#include "src/mca/psensor/psensor.h"
#include "src/mca/ptl/base/base.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_name_fns.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_environ.h"

#include "src/client/pmix_client_ops.h"
#include "src/server/pmix_server_ops.h"

pmix_status_t pmix_server_resolve_peers(pmix_server_caddy_t *cd,
                                        pmix_buffer_t *buf,
                                        pmix_info_cbfunc_t cbfunc)
{
    pmix_cb_t cb;
    int32_t cnt;
    pmix_status_t rc;
    char *nodename = NULL;
    pmix_nspace_t nspace;
    pmix_info_t info[3], *iptr;
    pmix_proc_t proc;
    pmix_kval_t *kv;
    char **p, **tmp = NULL, *prs;
    pmix_proc_t *pa = NULL;
    size_t m, n, np = 0, ninfo = 3;
    pmix_namespace_t *ns;
    char *nd, *str;
    pmix_data_array_t darray;

    /* unpack the nodename */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, cd->peer, buf, &nodename, &cnt, PMIX_STRING);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    /* unpack the nspace */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, cd->peer, buf, &str, &cnt, PMIX_STRING);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        if (NULL != nodename) {
            free(nodename);
        }
        return rc;
    }
    PMIX_LOAD_NSPACE(nspace, str);
    free(str);

    // construct these so we don't have to worry about
    // destruct later
    for (n=0; n < ninfo; n++) {
        PMIX_INFO_CONSTRUCT(&info[n]);
    }

    // if the host supports the "query" interface, then
    // pass this up to the host for processing as it has
    // the latest information
    if (NULL != pmix_host_server.query) {
        PMIX_QUERY_CREATE(cd->query, 1);
        PMIX_ARGV_APPEND(rc, cd->query->keys, PMIX_QUERY_RESOLVE_PEERS);
        PMIX_INFO_CREATE(iptr, 2);
        str = (char*)nspace;
        PMIX_INFO_LOAD(&iptr[0], PMIX_NSPACE, str, PMIX_STRING);
        PMIX_INFO_SET_QUALIFIER(&iptr[0]);
        PMIX_INFO_LOAD(&iptr[1], PMIX_HOSTNAME, nodename, PMIX_STRING);
        PMIX_INFO_SET_QUALIFIER(&iptr[1]);
        cd->query->qualifiers = iptr;
        cd->query->nqual = 2;
        rc = pmix_host_server.query(&pmix_globals.myid,
                                    cd->query, 1,
                                    cbfunc, (void*)cd);
        if (PMIX_SUCCESS != rc) {
            // they either don't support this query, or have
            // nothing to contribute - so use what we know
            PMIX_QUERY_FREE(cd->query, 1);
            cd->query = NULL;
            goto local;
        }
        return PMIX_SUCCESS;
    }

local:
    // restrict our search to already available info
    PMIX_INFO_LOAD(&info[0], PMIX_OPTIONAL, NULL, PMIX_BOOL);

    // if the nodename is NULL, then they are asking for our
    // local host
    if (NULL == nodename) {
        nd = pmix_globals.hostname;
    } else {
        nd = nodename;
    }

    PMIX_CONSTRUCT(&cb, pmix_cb_t);
    proc.rank = PMIX_RANK_UNDEF;
    cb.proc = &proc;
    cb.key = PMIX_LOCAL_PEERS;
    cb.scope = PMIX_INTERNAL;
    cb.info = info;
    cb.ninfo = ninfo;

    // restrict our search to already available info - do
    // not allow the search to call the host. This
    // avoids a threadlock situation
    PMIX_INFO_LOAD(&info[0], PMIX_OPTIONAL, NULL, PMIX_BOOL);
    PMIX_INFO_LOAD(&info[1], PMIX_NODE_INFO, NULL, PMIX_BOOL);
    PMIX_INFO_LOAD(&info[2], PMIX_HOSTNAME, nd, PMIX_STRING);

    if (0 == pmix_nslen(nspace)) {
        // asking for a complete list of peers
        rc = PMIX_ERR_DATA_VALUE_NOT_FOUND;
        np = 0;

        /* cycle across all known nspaces and aggregate the results */
        PMIX_LIST_FOREACH (ns, &pmix_globals.nspaces, pmix_namespace_t) {
            PMIX_LOAD_NSPACE(proc.nspace, ns->nspace);
            PMIX_GDS_FETCH_KV(rc, pmix_globals.mypeer, &cb);
            if (PMIX_SUCCESS != rc) {
                if (PMIX_RANK_UNDEF == proc.rank) {
                    // try again with wildcard
                    proc.rank = PMIX_RANK_WILDCARD;
                    PMIX_GDS_FETCH_KV(rc, pmix_globals.mypeer, &cb);
                    if (PMIX_SUCCESS != rc) {
                        continue;
                    }
                } else {
                    continue;
                }
            }

            /* sanity check */
            if (0 == pmix_list_get_size(&cb.kvs)) {
                continue;
            }
            kv = (pmix_kval_t*)pmix_list_get_first(&cb.kvs);
            if (PMIX_STRING != kv->value->type) {
                PMIX_ERROR_LOG(PMIX_ERR_INVALID_VAL);
                PMIX_LIST_DESTRUCT(&cb.kvs);
                PMIX_CONSTRUCT(&cb.kvs, pmix_list_t);
                continue;
            }
            if (NULL == kv->value->data.string) {
                /* no local peers on this node */
                PMIX_LIST_DESTRUCT(&cb.kvs);
                PMIX_CONSTRUCT(&cb.kvs, pmix_list_t);
                continue;
            }
            /* prepend the nspace */
            if (0 > asprintf(&prs, "%s:%s", ns->nspace, kv->value->data.string)) {
                PMIX_LIST_DESTRUCT(&cb.kvs);
                PMIX_CONSTRUCT(&cb.kvs, pmix_list_t);
                continue;
            }
            /* add to our list of results */
            PMIx_Argv_append_nosize(&tmp, prs);
            /* split to count the npeers */
            p = PMIx_Argv_split(kv->value->data.string, ',');
            np += PMIx_Argv_count(p);
            /* done with this entry */
            PMIx_Argv_free(p);
            free(prs);
            // clean up and cycle around to next namespace
            PMIX_LIST_DESTRUCT(&cb.kvs);
            PMIX_CONSTRUCT(&cb.kvs, pmix_list_t);
        }
        if (0 < np) {
            /* allocate the proc array */
            PMIX_PROC_CREATE(pa, np);
            if (NULL == pa) {
                rc = PMIX_ERR_NOMEM;
                PMIx_Argv_free(tmp);
                np = 0;
                PMIX_DESTRUCT(&cb);
                goto done;
            }
            /* transfer the results */
            np = 0;
            for (n = 0; NULL != tmp[n]; n++) {
                /* find the nspace delimiter */
                prs = strchr(tmp[n], ':');
                if (NULL == prs) {
                    /* should never happen, but silence a Coverity warning */
                    rc = PMIX_ERR_BAD_PARAM;
                    PMIx_Argv_free(tmp);
                    PMIX_PROC_FREE(pa, np);
                    pa = NULL;
                    np = 0;
                    PMIX_DESTRUCT(&cb);
                    goto done;
                }
                *prs = '\0';
                ++prs;
                p = PMIx_Argv_split(prs, ',');
                for (m = 0; NULL != p[m]; m++) {
                    PMIX_LOAD_NSPACE(pa[np].nspace, tmp[n]);
                    pa[np].rank = strtoul(p[m], NULL, 10);
                    ++np;
                }
                PMIx_Argv_free(p);
            }
            PMIx_Argv_free(tmp);
            rc = PMIX_SUCCESS;
        }
        PMIX_DESTRUCT(&cb);
        goto done;
    }

    /* get the list of local peers for this nspace and node */
    PMIX_LOAD_PROCID(&proc, nspace, PMIX_RANK_UNDEF);

    PMIX_GDS_FETCH_KV(rc, pmix_globals.mypeer, &cb);
    if (PMIX_SUCCESS == rc) {
        goto process;
    }
    if (PMIX_ERR_INVALID_NAMESPACE == rc) {
        // this namespace is unknown
        PMIX_DESTRUCT(&cb);
        goto done;
    }
    if (PMIX_ERR_NOT_FOUND == rc) {
        // found the namespace, but the node is
        // not present on that namespace - the
        // default response is correct
        rc = PMIX_SUCCESS;
        PMIX_DESTRUCT(&cb);
        goto done;
    }
    if (PMIX_ERR_DATA_VALUE_NOT_FOUND == rc) {
        // found the namespace and node, but the
        // host did not provide the information
        PMIX_DESTRUCT(&cb);
        goto done;
    }
    // get here if we see a different error
    if (PMIX_RANK_UNDEF == proc.rank) {
        // try again with wildcard
        proc.rank = PMIX_RANK_WILDCARD;
        PMIX_GDS_FETCH_KV(rc, pmix_globals.mypeer, &cb);
        if (PMIX_SUCCESS == rc) {
            goto process;
        }
        if (PMIX_ERR_NOT_FOUND == rc) {
            // found the namespace, but the node is
            // not present on that namespace - the
            // default response is correct
            rc = PMIX_SUCCESS;
        }
        // couldn't find it
        PMIX_DESTRUCT(&cb);
        goto done;
    }

process:
    /* sanity check */
    if (0 == pmix_list_get_size(&cb.kvs)) {
        rc = PMIX_ERR_INVALID_VAL;
        PMIX_DESTRUCT(&cb);
        goto done;
    }
    kv = (pmix_kval_t*)pmix_list_get_first(&cb.kvs);
    if (PMIX_STRING != kv->value->type || NULL == kv->value->data.string) {
        rc = PMIX_ERR_INVALID_VAL;
        PMIX_DESTRUCT(&cb);
        goto done;
    }

    /* split the procs to get a list */
    p = PMIx_Argv_split(kv->value->data.string, ',');
    np = PMIx_Argv_count(p);

    /* allocate the proc array */
    PMIX_PROC_CREATE(pa, np);
    if (NULL == pa) {
        rc = PMIX_ERR_NOMEM;
        PMIx_Argv_free(p);
        PMIX_DESTRUCT(&cb);
        goto done;
    }
    /* transfer the results */
    for (n = 0; n < np; n++) {
        PMIX_LOAD_NSPACE(pa[n].nspace, nspace);
        pa[n].rank = strtoul(p[n], NULL, 10);
    }
    PMIx_Argv_free(p);
    rc = PMIX_SUCCESS;
    PMIX_DESTRUCT(&cb);

done:
    for (n=0; n < ninfo; n++) {
        PMIX_INFO_DESTRUCT(&info[n]);
    }
    if (NULL != nodename) {
        free(nodename);
    }

    if (PMIX_SUCCESS == rc) {
        // put the answer in an info
        cd->ninfo = 1;
        PMIX_INFO_CREATE(cd->info, cd->ninfo);
        // key doesn't matter here
        darray.type = PMIX_PROC;
        darray.array = pa;
        darray.size = np;
        PMIX_INFO_LOAD(&cd->info[0], PMIX_QUERY_RESOLVE_NODE, &darray, PMIX_DATA_ARRAY);
        PMIX_PROC_FREE(pa, np);
    }

    cbfunc(rc, cd->info, cd->ninfo, cd, NULL, NULL);
    return PMIX_SUCCESS;
}

pmix_status_t pmix_server_resolve_node(pmix_server_caddy_t *cd,
                                       pmix_buffer_t *buf,
                                       pmix_info_cbfunc_t cbfunc)
{
    pmix_cb_t cb;
    int32_t cnt;
    pmix_status_t rc;
    pmix_nspace_t nspace;
    pmix_info_t info, *iptr;
    pmix_proc_t proc;
    pmix_kval_t *kv;
    pmix_value_t *val;
    char **p, **tmp = NULL, *nodelist = NULL, *str;
    pmix_namespace_t *ns;
    size_t n;

    /* unpack the nspace */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, cd->peer, buf, &str, &cnt, PMIX_STRING);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    PMIX_LOAD_NSPACE(nspace, str);
    free(str);

    // if the host supports the "query" interface, then
    // pass this up to the host for processing as it has
    // the latest information
    if (NULL != pmix_host_server.query) {
        PMIX_QUERY_CREATE(cd->query, 1);
        PMIX_ARGV_APPEND(rc, cd->query->keys, PMIX_QUERY_RESOLVE_NODE);
        PMIX_INFO_CREATE(iptr, 1);
        str = (char*)nspace;
        PMIX_INFO_LOAD(&iptr[0], PMIX_NSPACE, str, PMIX_STRING);
        PMIX_INFO_SET_QUALIFIER(&iptr[0]);
        cd->query->qualifiers = iptr;
        cd->query->nqual = 1;
        rc = pmix_host_server.query(&pmix_globals.myid,
                                    cd->query, 1,
                                    cbfunc, (void*)cd);
        if (PMIX_SUCCESS != rc) {
            // they either don't support this query, or have
            // nothing to contribute - so use what we know
            PMIX_QUERY_FREE(cd->query, 1);
            cd->query = NULL;
            goto local;
        }
        return PMIX_SUCCESS;
    }

local:
    // restrict our search to already available info
    PMIX_INFO_LOAD(&info, PMIX_OPTIONAL, NULL, PMIX_BOOL);

    PMIX_CONSTRUCT(&cb, pmix_cb_t);
    proc.rank = PMIX_RANK_WILDCARD;
    cb.proc = &proc;
    cb.key = PMIX_NODE_LIST;
    cb.scope = PMIX_INTERNAL;
    cb.info = &info;
    cb.ninfo = 1;

    if (0 == pmix_nslen(nspace)) {
        /* cycle across all known nspaces and aggregate the results */
        PMIX_LIST_FOREACH (ns, &pmix_globals.nspaces, pmix_namespace_t) {
            PMIX_LOAD_NSPACE(proc.nspace, ns->nspace);
            PMIX_GDS_FETCH_KV(rc, pmix_client_globals.myserver, &cb);
            if (PMIX_SUCCESS != rc) {
                continue;
            }

            /* sanity check */
            if (0 == pmix_list_get_size(&cb.kvs)) {
                continue;
            }
            kv = (pmix_kval_t*)pmix_list_get_first(&cb.kvs);
            val = kv->value;
            if (PMIX_STRING != val->type) {
                PMIX_ERROR_LOG(PMIX_ERR_INVALID_VAL);
                PMIX_LIST_DESTRUCT(&cb.kvs);
                PMIX_CONSTRUCT(&cb.kvs, pmix_list_t);
                continue;
            }
            if (NULL == val->data.string) {
                /* no nodes found */
                PMIX_LIST_DESTRUCT(&cb.kvs);
                PMIX_CONSTRUCT(&cb.kvs, pmix_list_t);
                continue;
            }
            /* add to our list of results, ensuring uniqueness */
            p = PMIx_Argv_split(val->data.string, ',');
            for (n = 0; NULL != p[n]; n++) {
                PMIx_Argv_append_unique_nosize(&tmp, p[n]);
            }
            PMIx_Argv_free(p);
            PMIX_LIST_DESTRUCT(&cb.kvs);
            PMIX_CONSTRUCT(&cb.kvs, pmix_list_t);
        }
        if (0 < PMIx_Argv_count(tmp)) {
            nodelist = PMIx_Argv_join(tmp, ',');
            PMIx_Argv_free(tmp);
        }
        rc = PMIX_SUCCESS;
        PMIX_DESTRUCT(&cb);
        goto done;
    }

    // looking for a specific nspace
    PMIX_LOAD_NSPACE(proc.nspace, nspace);
    PMIX_GDS_FETCH_KV(rc, pmix_client_globals.myserver, &cb);
    if (PMIX_SUCCESS != rc) {
        PMIX_DESTRUCT(&cb);
        goto done;
    }

    /* sanity check */
    if (0 == pmix_list_get_size(&cb.kvs)) {
        PMIX_DESTRUCT(&cb);
        goto done;
    }
    kv = (pmix_kval_t*)pmix_list_get_first(&cb.kvs);
    val = kv->value;
    if (PMIX_STRING != val->type || NULL == val->data.string) {
        PMIX_ERROR_LOG(PMIX_ERR_INVALID_VAL);
        rc = PMIX_ERR_INVALID_VAL;
        PMIX_DESTRUCT(&cb);
        goto done;
    }
    nodelist = strdup(val->data.string);
    PMIX_DESTRUCT(&cb);

done:
    if (PMIX_SUCCESS == rc) {
        // put the nodelist in an info
        cd->ninfo = 1;
        PMIX_INFO_CREATE(cd->info, cd->ninfo);
        // key doesn't matter here
        PMIX_INFO_LOAD(&cd->info[0], PMIX_QUERY_RESOLVE_NODE, &nodelist, PMIX_STRING);
    }

    cbfunc(rc, cd->info, cd->ninfo, cd, NULL, NULL);
    return PMIX_SUCCESS;
}
