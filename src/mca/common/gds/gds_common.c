/*
 * This file is derived from the gds package - per the BSD license, the following
 * info is copied here:
 ***********************
 * This file is part of the gds package, copyright (c) 2011, 2012, @radiospiel.
 * It is copyrighted under the terms of the modified BSD license, see LICENSE.BSD.
 *
 * For more information see https://https://github.com/radiospiel/gds.
 ***********************
 * Copyright (c) 2020      Intel, Inc.  All rights reserved.
 *
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
#include "src/include/pmix_config.h"
#include "include/pmix_common.h"

#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdio.h>
#include <curl/curl.h>

#include "src/class/pmix_list.h"
#include "src/include/types.h"
#include "src/include/pmix_globals.h"
#include "src/util/hash.h"

#include "src/mca/gds/gds.h"
#include "gds_common.h"

/**********************************************/
/* class instantiations */
static void scon(pmix_session_t *s)
{
    s->session = UINT32_MAX;
    PMIX_CONSTRUCT(&s->sessioninfo, pmix_list_t);
    PMIX_CONSTRUCT(&s->nodeinfo, pmix_list_t);
}
static void sdes(pmix_session_t *s)
{
    PMIX_LIST_DESTRUCT(&s->sessioninfo);
    PMIX_LIST_DESTRUCT(&s->nodeinfo);
}
PMIX_CLASS_INSTANCE(pmix_session_t,
                    pmix_list_item_t,
                    scon, sdes);

static void htcon(pmix_job_t *p)
{
    p->ns = NULL;
    p->nptr = NULL;
    PMIX_CONSTRUCT(&p->jobinfo, pmix_list_t);
    PMIX_CONSTRUCT(&p->internal, pmix_hash_table_t);
    pmix_hash_table_init(&p->internal, 256);
    PMIX_CONSTRUCT(&p->remote, pmix_hash_table_t);
    pmix_hash_table_init(&p->remote, 256);
    PMIX_CONSTRUCT(&p->local, pmix_hash_table_t);
    pmix_hash_table_init(&p->local, 256);
    p->gdata_added = false;
    PMIX_CONSTRUCT(&p->apps, pmix_list_t);
    PMIX_CONSTRUCT(&p->nodeinfo, pmix_list_t);
    p->session = NULL;
}
static void htdes(pmix_job_t *p)
{
    if (NULL != p->ns) {
        free(p->ns);
    }
    if (NULL != p->nptr) {
        PMIX_RELEASE(p->nptr);
    }
    PMIX_LIST_DESTRUCT(&p->jobinfo);
    pmix_hash_remove_data(&p->internal, PMIX_RANK_WILDCARD, NULL);
    PMIX_DESTRUCT(&p->internal);
    pmix_hash_remove_data(&p->remote, PMIX_RANK_WILDCARD, NULL);
    PMIX_DESTRUCT(&p->remote);
    pmix_hash_remove_data(&p->local, PMIX_RANK_WILDCARD, NULL);
    PMIX_DESTRUCT(&p->local);
    PMIX_LIST_DESTRUCT(&p->apps);
    PMIX_LIST_DESTRUCT(&p->nodeinfo);
    if (NULL != p->session) {
        PMIX_RELEASE(p->session);
    }
}
PMIX_CLASS_INSTANCE(pmix_job_t,
                    pmix_list_item_t,
                    htcon, htdes);

static void apcon(pmix_apptrkr_t *p)
{
    p->appnum = 0;
    PMIX_CONSTRUCT(&p->appinfo, pmix_list_t);
    PMIX_CONSTRUCT(&p->nodeinfo, pmix_list_t);
    p->job = NULL;
}
static void apdes(pmix_apptrkr_t *p)
{
    PMIX_LIST_DESTRUCT(&p->appinfo);
    PMIX_LIST_DESTRUCT(&p->nodeinfo);
    if (NULL != p->job) {
        PMIX_RELEASE(p->job);
    }
}
PMIX_CLASS_INSTANCE(pmix_apptrkr_t,
                    pmix_list_item_t,
                    apcon, apdes);

static void ndinfocon(pmix_nodeinfo_t *p)
{
    p->nodeid = UINT32_MAX;
    p->hostname = NULL;
    p->aliases = NULL;
    PMIX_CONSTRUCT(&p->info, pmix_list_t);
}
static void ndinfodes(pmix_nodeinfo_t *p)
{
    if (NULL != p->hostname) {
        free(p->hostname);
    }
    if (NULL != p->aliases) {
        pmix_argv_free(p->aliases);
    }
    PMIX_LIST_DESTRUCT(&p->info);
}
PMIX_CLASS_INSTANCE(pmix_nodeinfo_t,
                    pmix_list_item_t,
                    ndinfocon, ndinfodes);

static void easy_destruct(pmix_list_t *t)
{
    pmix_kval_t *kv;

    kv = (pmix_kval_t*)pmix_list_remove_first(t);
    while (NULL != kv) {
        kv->value = NULL;
        PMIX_RELEASE(kv);
        kv = (pmix_kval_t*)pmix_list_remove_first(t);
    }
    PMIX_DESTRUCT(t);
}

static void nodeinfo_copy(pmix_nodeinfo_t *dest,
                          pmix_nodeinfo_t *src
                          pmix_tma_t *tma)
{
    dest->nodeid = src->nodeid;
    if (NULL != src->hostname) {
        dest->hostname = pmix_tma_strdup(tma, src->hostname);
    }
    if (NULL != src->aliases) {

    }
}

/* process a node array - contains an array of
 * node-level info for a single node. Either the
 * nodeid, hostname, or both must be included
 * in the array to identify the node */
static pmix_status_t process_node_array(pmix_value_t *val,
                                        pmix_list_t *tgt,
                                        pmix_tma_t *tma)
{
    size_t size, j;
    pmix_info_t *iptr;
    pmix_status_t rc = PMIX_SUCCESS;
    pmix_kval_t *kp2, *k1;
    pmix_list_t cache;
    pmix_nodeinfo_t *nd = NULL, *ndptr;
    bool update;

    pmix_output_verbose(2, pmix_gds_base_framework.framework_output,
                        "PROCESSING NODE ARRAY");

    /* array of node-level info for a specific node */
    if (PMIX_DATA_ARRAY != val->type) {
        PMIX_ERROR_LOG(PMIX_ERR_TYPE_MISMATCH);
        return PMIX_ERR_TYPE_MISMATCH;
    }

    /* setup arrays */
    size = val->data.darray->size;
    iptr = (pmix_info_t*)val->data.darray->array;
    PMIX_CONSTRUCT(&cache, pmix_list_t);

    /* cache the values while searching for the nodeid
     * and/or hostname */
    for (j=0; j < size; j++) {
        pmix_output_verbose(10, pmix_gds_base_framework.framework_output,
                            "%s gds:hash:node_array for key %s",
                            PMIX_NAME_PRINT(&pmix_globals.myid), iptr[j].key);
        if (PMIX_CHECK_KEY(&iptr[j], PMIX_NODEID)) {
            if (NULL == nd) {
                nd = PMIX_NEW(pmix_nodeinfo_t);
            }
            PMIX_VALUE_GET_NUMBER(rc, &iptr[j].value, nd->nodeid, uint32_t);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_RELEASE(nd);
                PMIX_LIST_DESTRUCT(&cache);
                return rc;
            }
        } else if (PMIX_CHECK_KEY(&iptr[j], PMIX_HOSTNAME)) {
            if (NULL == nd) {
                nd = PMIX_NEW(pmix_nodeinfo_t);
            }
            nd->hostname = strdup(iptr[j].value.data.string);
        } else if (PMIX_CHECK_KEY(&iptr[j], PMIX_HOSTNAME_ALIASES)) {
            if (NULL == nd) {
                nd = PMIX_NEW(pmix_nodeinfo_t);
            }
            nd->aliases = pmix_argv_split(iptr[j].value.data.string, ',');
        }
        /* cache the value */
        kp2 = PMIX_NEW(pmix_kval_t);
        kp2->key = strdup(iptr[j].key);
        kp2->value = (pmix_value_t*)malloc(sizeof(pmix_value_t));
        PMIX_VALUE_XFER(rc, kp2->value, &iptr[j].value);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(kp2);
            if (NULL != nd) {
                PMIX_RELEASE(nd);
            }
            PMIX_LIST_DESTRUCT(&cache);
            return rc;
        }
        pmix_list_append(&cache, &kp2->super);
    }

    if (NULL == nd) {
        /* they forgot to pass us the ident for the node */
        PMIX_LIST_DESTRUCT(&cache);
        return PMIX_ERR_BAD_PARAM;
    }

    /* see if we already have this node on the
     * provided list */
    update = false;
    PMIX_LIST_FOREACH(ndptr, tgt, pmix_nodeinfo_t) {
        if (check_node(ndptr, nd)) {
            /* we assume that the data is updating the current
             * values */
            if (NULL == ndptr->hostname && NULL != nd->hostname) {
                ndptr->hostname = strdup(nd->hostname);
            }
            PMIX_RELEASE(nd);
            nd = ndptr;
            update = true;
            break;
        }
    }
    if (!update) {
        pmix_list_append(tgt, &nd->super);
    }

    /* transfer the cached items to the nodeinfo list */
    kp2 = (pmix_kval_t*)pmix_list_remove_first(&cache);
    while (NULL != kp2) {
        /* if this is an update, we have to ensure each data
         * item only appears once on the list */
        if (update) {
            PMIX_LIST_FOREACH(k1, &nd->info, pmix_kval_t) {
                if (PMIX_CHECK_KEY(k1, kp2->key)) {
                    pmix_list_remove_item(&nd->info, &k1->super);
                    PMIX_RELEASE(k1);
                    break;
                }
            }
        }
        pmix_output_verbose(10, pmix_gds_base_framework.framework_output,
                            "%s gds:hash:node_array node %s key %s",
                            PMIX_NAME_PRINT(&pmix_globals.myid),
                            nd->hostname, kp2->key);
        pmix_list_append(&nd->info, &kp2->super);
        kp2 = (pmix_kval_t*)pmix_list_remove_first(&cache);
    }
    PMIX_LIST_DESTRUCT(&cache);

    return PMIX_SUCCESS;
}

static pmix_status_t process_session_array(pmix_value_t *val,
                                           pmix_job_t *trk,
                                           pmix_tma_t *tma)
{
    pmix_session_t *s = NULL, *sptr;
    size_t j, size;
    pmix_info_t *iptr;
    pmix_list_t cache, ncache;
    pmix_status_t rc;
    pmix_kval_t *kp2, *kv;
    pmix_nodeinfo_t *nd, *nd2;
    uint32_t sid;

    /* array of session-level info */
    if (PMIX_DATA_ARRAY != val->type) {
        PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
        return PMIX_ERR_TYPE_MISMATCH;
    }
    size = val->data.darray->size;
    iptr = (pmix_info_t*)val->data.darray->array;

    /* we put the cache lists into temporary memory */
    PMIX_CONSTRUCT(&cache, pmix_list_t);
    PMIX_CONSTRUCT(&ncache, pmix_list_t);

    for (j=0; j < size; j++) {
        if (PMIX_CHECK_KEY(&iptr[j], PMIX_SESSION_ID)) {
            PMIX_VALUE_GET_NUMBER(rc, &iptr[j].value, sid, uint32_t);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                easy_destruct(&cache);
                easy_destruct(&ncache);
                return rc;
            }
            /* see if we already have this session - it could have
             * been defined by a separate PMIX_SESSION_ID key */
            PMIX_LIST_FOREACH(sptr, &mysessions, pmix_session_t) {
                if (sptr->session == sid) {
                    s = sptr;
                    break;
                }
            }
            if (NULL == s) {
                /* wasn't found, so create one */
                s = PMIX_NEW_TMA(pmix_session_t, tma);
                s->session = sid;
                pmix_list_append(&mysessions, &s->super);
            }
        } else if (PMIX_CHECK_KEY(&iptr[j], PMIX_NODE_INFO_ARRAY)) {
            if (PMIX_SUCCESS != (rc = process_node_array(&iptr[j].value, &ncache))) {
                PMIX_ERROR_LOG(rc);
                easy_destruct(&cache);
                easy_destruct(&ncache);
                return rc;
            }
        } else {
            kp2 = PMIX_NEW(pmix_kval_t);
            kp2->key = strdup(iptr[j].key);
            kp2->value = &iptr[j].value;
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_RELEASE(kp2);
                easy_destruct(&cache);
                easy_destruct(&ncache);
                return rc;
            }
            pmix_list_append(&cache, &kp2->super);
        }
    }
    if (NULL == s) {
        /* this is not allowed to happen - they are required
         * to provide us with a session ID per the standard */
        easy_destruct(&cache);
        easy_destruct(&ncache);
        rc = PMIX_ERR_BAD_PARAM;
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    /* point the job at it */
    if (NULL != trk->session) {
        PMIX_RELEASE(trk->session);
    }
    PMIX_RETAIN(s);
    trk->session = s;
    /* transfer the data across */
    kp2 = (pmix_kval_t*)pmix_list_remove_first(&cache);
    while (NULL != kp2) {
        kv = PMIX_NEW_TMA(pmix_kval_t);
        kv->value = (pmix_value_t*)pmix_tma_malloc(tma, sizeof(pmix_value_t));
        PMIX_VALUE_XFER(kv->value, kp2->value);
        pmix_list_append(&s->sessioninfo, &kv->super);
        kp2->value = NULL;
        PMIX_RELEASE(kp2);
        kp2 = (pmix_kval_t*)pmix_list_remove_first(&cache);
    }
    easy_destruct(&cache);
    nd = (pmix_nodeinfo_t*)pmix_list_remove_first(&ncache);
    while (NULL != nd) {
        nd2 = PMIX_NEW_TMA(pmix_nodeinfo_t, tma);
        nodeinfo_copy(nd2, nd);
        pmix_list_append(&s->nodeinfo, &nd->super);
        PMIX_RELEASE(nd);
        nd = (pmix_nodeinfo_t*)pmix_list_remove_first(&ncache);
    }
    PMIX_LIST_DESTRUCT(&ncache);
    return PMIX_SUCCESS;
}

