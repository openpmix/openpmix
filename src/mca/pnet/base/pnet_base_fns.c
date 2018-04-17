/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2015-2018 Intel, Inc.  All rights reserved.
 * Copyright (c) 2016      Mellanox Technologies, Inc.
 *                         All rights reserved.
 *
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include <src/include/pmix_config.h>

#include <pmix_common.h>
#include "src/include/pmix_globals.h"

#include "src/class/pmix_list.h"
#include "src/util/error.h"
#include "src/server/pmix_server_ops.h"

#include "src/mca/pnet/base/base.h"


/* NOTE: a tool (e.g., prun) may call this function to
 * harvest local envars for inclusion in a call to
 * PMIx_Spawn, or it might be called in response to
 * a call to PMIx_Allocate_resources */
pmix_status_t pmix_pnet_base_allocate(char *nspace,
                                      pmix_info_t info[], size_t ninfo,
                                      pmix_list_t *ilist)
{
    pmix_pnet_base_active_module_t *active;
    pmix_status_t rc;
    pmix_nspace_t *nptr, *ns;
    size_t n;

    if (!pmix_pnet_globals.initialized) {
        return PMIX_ERR_INIT;
    }

    pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                        "pnet:allocate called");

    /* protect against bozo inputs */
    if (NULL == nspace || NULL == ilist) {
        return PMIX_ERR_BAD_PARAM;
    }
    if (PMIX_PROC_IS_GATEWAY(pmix_globals.mypeer)) {
        nptr = NULL;
        /* find this nspace - note that it may not have
         * been registered yet */
        PMIX_LIST_FOREACH(ns, &pmix_server_globals.nspaces, pmix_nspace_t) {
            if (0 == strcmp(ns->nspace, nspace)) {
                nptr = ns;
                break;
            }
        }
        if (NULL == nptr) {
            /* add it */
            nptr = PMIX_NEW(pmix_nspace_t);
            if (NULL == nptr) {
                return PMIX_ERR_NOMEM;
            }
            nptr->nspace = strdup(nspace);
            pmix_list_append(&pmix_server_globals.nspaces, &nptr->super);
        }

        /* if the info param is NULL, then we make one pass thru the actives
         * in case someone specified an allocation or collection of envars
         * via MCA param */
        if (NULL == info) {
            PMIX_LIST_FOREACH(active, &pmix_pnet_globals.actives, pmix_pnet_base_active_module_t) {
                if (NULL != active->module->allocate) {
                    if (PMIX_SUCCESS == (rc = active->module->allocate(nptr, NULL, ilist))) {
                        break;
                    }
                }
                if (PMIX_ERR_TAKE_NEXT_OPTION != rc) {
                    /* true error */
                    return rc;
                }
            }
        } else {
            /* process the allocation request */
            for (n=0; n < ninfo; n++) {
                PMIX_LIST_FOREACH(active, &pmix_pnet_globals.actives, pmix_pnet_base_active_module_t) {
                    if (NULL != active->module->allocate) {
                        if (PMIX_SUCCESS == (rc = active->module->allocate(nptr, &info[n], ilist))) {
                            break;
                        }
                    }
                    if (PMIX_ERR_TAKE_NEXT_OPTION != rc) {
                        /* true error */
                        return rc;
                    }
                }
            }
        }
    }

    return PMIX_SUCCESS;
}

/* can only be called by a server */
pmix_status_t pmix_pnet_base_setup_local_network(char *nspace,
                                                 pmix_info_t info[],
                                                 size_t ninfo)
{
    pmix_pnet_base_active_module_t *active;
    pmix_status_t rc;
    pmix_nspace_t *nptr, *ns;

    if (!pmix_pnet_globals.initialized) {
        return PMIX_ERR_INIT;
    }

    pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                        "pnet: setup_local_network called");

    /* protect against bozo inputs */
    if (NULL == nspace) {
        return PMIX_ERR_BAD_PARAM;
    }

    /* find this proc's nspace object */
    nptr = NULL;
    PMIX_LIST_FOREACH(ns, &pmix_server_globals.nspaces, pmix_nspace_t) {
        if (0 == strcmp(ns->nspace, nspace)) {
            nptr = ns;
            break;
        }
    }
    if (NULL == nptr) {
        /* add it */
        nptr = PMIX_NEW(pmix_nspace_t);
        if (NULL == nptr) {
            return PMIX_ERR_NOMEM;
        }
        nptr->nspace = strdup(nspace);
        pmix_list_append(&pmix_server_globals.nspaces, &nptr->super);
    }

    PMIX_LIST_FOREACH(active, &pmix_pnet_globals.actives, pmix_pnet_base_active_module_t) {
        if (NULL != active->module->setup_local_network) {
            if (PMIX_SUCCESS != (rc = active->module->setup_local_network(nptr, info, ninfo))) {
                return rc;
            }
        }
    }

    return PMIX_SUCCESS;
}

/* can only be called by a server */
pmix_status_t pmix_pnet_base_setup_fork(const pmix_proc_t *proc, char ***env)
{
    pmix_pnet_base_active_module_t *active;
    pmix_status_t rc;
    pmix_nspace_t *nptr, *ns;

    if (!pmix_pnet_globals.initialized) {
        return PMIX_ERR_INIT;
    }

    /* protect against bozo inputs */
    if (NULL == proc || NULL == env) {
        return PMIX_ERR_BAD_PARAM;
    }

    /* find this proc's nspace object */
    nptr = NULL;
    PMIX_LIST_FOREACH(ns, &pmix_server_globals.nspaces, pmix_nspace_t) {
        if (0 == strcmp(ns->nspace, proc->nspace)) {
            nptr = ns;
            break;
        }
    }
    if (NULL == nptr) {
        /* add it */
        nptr = PMIX_NEW(pmix_nspace_t);
        if (NULL == nptr) {
            return PMIX_ERR_NOMEM;
        }
        nptr->nspace = strdup(proc->nspace);
        pmix_list_append(&pmix_server_globals.nspaces, &nptr->super);
    }

    PMIX_LIST_FOREACH(active, &pmix_pnet_globals.actives, pmix_pnet_base_active_module_t) {
        if (NULL != active->module->setup_fork) {
            if (PMIX_SUCCESS != (rc = active->module->setup_fork(nptr, proc, env))) {
                return rc;
            }
        }
    }

    return PMIX_SUCCESS;
}

void pmix_pnet_base_child_finalized(pmix_peer_t *peer)
{
    pmix_pnet_base_active_module_t *active;

    if (!pmix_pnet_globals.initialized) {
        return;
    }

    /* protect against bozo inputs */
    if (NULL == peer) {
        PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
        return;
    }

    PMIX_LIST_FOREACH(active, &pmix_pnet_globals.actives, pmix_pnet_base_active_module_t) {
        if (NULL != active->module->child_finalized) {
            active->module->child_finalized(peer);
        }
    }

    return;
}

void pmix_pnet_base_local_app_finalized(char *nspace)
{
    pmix_pnet_base_active_module_t *active;
    pmix_nspace_t *nptr, *ns;

    if (!pmix_pnet_globals.initialized) {
        return;
    }

    /* protect against bozo inputs */
    if (NULL == nspace) {
        return;
    }

    /* find this nspace object */
    nptr = NULL;
    PMIX_LIST_FOREACH(ns, &pmix_server_globals.nspaces, pmix_nspace_t) {
        if (0 == strcmp(ns->nspace, nspace)) {
            nptr = ns;
            break;
        }
    }
    if (NULL == nptr) {
        /* nothing we can do */
        return;
    }

    PMIX_LIST_FOREACH(active, &pmix_pnet_globals.actives, pmix_pnet_base_active_module_t) {
        if (NULL != active->module->local_app_finalized) {
            active->module->local_app_finalized(nptr);
        }
    }

    return;
}

void pmix_pnet_base_deregister_nspace(char *nspace)
{
    pmix_pnet_base_active_module_t *active;
    pmix_nspace_t *nptr, *ns;

    if (!pmix_pnet_globals.initialized) {
        return;
    }

    /* protect against bozo inputs */
    if (NULL == nspace) {
        return;
    }

    /* find this nspace object */
    nptr = NULL;
    PMIX_LIST_FOREACH(ns, &pmix_server_globals.nspaces, pmix_nspace_t) {
        if (0 == strcmp(ns->nspace, nspace)) {
            nptr = ns;
            break;
        }
    }
    if (NULL == nptr) {
        /* nothing we can do */
        return;
    }

    PMIX_LIST_FOREACH(active, &pmix_pnet_globals.actives, pmix_pnet_base_active_module_t) {
        if (NULL != active->module->deregister_nspace) {
            active->module->deregister_nspace(nptr);
        }
    }

    return;
}

typedef struct {
    pmix_list_item_t super;
    pmix_info_t *info;
    size_t ninfo;
    pmix_release_cbfunc_t release_fn;
    void *release_cbdata;
} pmix_inventory_payload_t;
static void ipcon(pmix_inventory_payload_t *p)
{
    p->info = NULL;
    p->ninfo = 0;
    p->release_fn = NULL;
    p->release_cbdata = NULL;
}
static PMIX_CLASS_INSTANCE(pmix_inventory_payload_t,
                           pmix_list_item_t,
                           ipcon, NULL);

typedef struct {
    pmix_object_t super;
    pmix_lock_t lock;
    int requests;
    int replies;
    pmix_info_t *info;
    size_t ninfo;
    pmix_list_t payload;   // list of pmix_inventory_payload_t containing the replies
    pmix_info_cbfunc_t cbfunc;
    void *cbdata;
} pmix_pnet_inventory_lock_t;
static void ilcon(pmix_pnet_inventory_lock_t *p)
{
    PMIX_CONSTRUCT_LOCK(&p->lock);
    p->requests = 0;
    p->replies = 0;
    PMIX_CONSTRUCT(&p->payload, pmix_list_t);
}
static void ildes(pmix_pnet_inventory_lock_t *p)
{
    PMIX_DESTRUCT_LOCK(&p->lock);
    if (NULL != p->info) {
        PMIX_INFO_FREE(p->info, p->ninfo);
    }
    PMIX_LIST_DESTRUCT(&p->payload);
}
static PMIX_CLASS_INSTANCE(pmix_pnet_inventory_lock_t,
                           pmix_object_t,
                           ilcon, ildes);

static void cirelease(void *cbdata)
{
    pmix_pnet_inventory_lock_t *lock = (pmix_pnet_inventory_lock_t*)cbdata;
    PMIX_RELEASE_THREAD(&lock->lock);
    PMIX_RELEASE(lock);
}

static void cicbfunc(pmix_status_t status,
                     pmix_info_t *info, size_t ninfo,
                     void *cbdata,
                     pmix_release_cbfunc_t release_fn,
                     void *release_cbdata)
{
    pmix_pnet_inventory_lock_t *lock = (pmix_pnet_inventory_lock_t*)cbdata;
    pmix_inventory_payload_t *pyload;
    pmix_status_t ret;
    size_t n;

    PMIX_ACQUIRE_THREAD(&lock->lock);
    /* transfer the returned values */
    pyload = PMIX_NEW(pmix_inventory_payload_t);
    if (NULL == pyload) {
        ret = PMIX_ERR_NOMEM;
        goto error;
    }
    pyload->info = info;
    pyload->ninfo = ninfo;
    pyload->release_fn = release_fn;
    pyload->release_cbdata = release_cbdata;
    pmix_list_append(&lock->payload, &pyload->super);
    lock->ninfo += ninfo;
    lock->replies++;
    if (lock->replies < lock->requests) {
        PMIX_RELEASE_THREAD(&lock->lock);
        return;
    }

    /* if we get here, then collection is complete */
    ret = PMIX_SUCCESS;

    /* construct the reply info array */
    PMIX_INFO_CREATE(lock->info, lock->ninfo);
    if (NULL == lock->info) {
        ret = PMIX_ERR_NOMEM;
        goto error;
    }
    /* transfer the results */
    n=0;
    PMIX_LIST_FOREACH(pyload, &lock->payload, pmix_inventory_payload_t) {
        memcpy(&lock->info[n], pyload->info, sizeof(pmix_info_t));
        ++n;
    }
    /* now call the requestor back */
    lock->cbfunc(ret, lock->info, lock->ninfo, lock->cbdata, cirelease, lock);
    return;

  error:
    /* cycle across the collected payloads and let
     * providers release them */
    PMIX_LIST_FOREACH(pyload, &lock->payload, pmix_inventory_payload_t) {
        if (NULL != pyload->release_fn) {
            pyload->release_fn(pyload->release_cbdata);
        }
    }
    PMIX_RELEASE_THREAD(&lock->lock);
    PMIX_RELEASE(lock);
}

void pmix_pnet_base_collect_inventory(pmix_info_t directives[], size_t ndirs,
                                      pmix_info_cbfunc_t cbfunc, void *cbdata)
{
    pmix_pnet_base_active_module_t *active;
    pmix_pnet_inventory_lock_t *mylock;
    pmix_status_t rc;

    /* we cannot block here as each plugin could take some time to
     * complete the request. So instead, we call each active plugin
     * and get their immediate response - if "in progress", then
     * we record that we have to wait for their answer before providing
     * the caller with a response. If "error", then we know we
     * won't be getting a response from them */

    if (!pmix_pnet_globals.initialized) {
        /* need to call them back so they know */
        if (NULL != cbfunc) {
            cbfunc(PMIX_ERR_INIT, NULL, 0, cbdata, NULL, NULL);
        }
        return;
    }
    /* create the lock */
    mylock = PMIX_NEW(pmix_pnet_inventory_lock_t);
    if (NULL == mylock) {
        /* need to call them back so they know */
        if (NULL != cbfunc) {
            cbfunc(PMIX_ERR_NOMEM, NULL, 0, cbdata, NULL, NULL);
        }
        return;
    }

    /* hold the lock until all active modules have been called
     * to avoid race condition where replies come in before
     * the requests counter has been fully updated */
    PMIX_ACQUIRE_THREAD(&mylock->lock);

    PMIX_LIST_FOREACH(active, &pmix_pnet_globals.actives, pmix_pnet_base_active_module_t) {
        if (NULL != active->module->collect_inventory) {
            rc = active->module->collect_inventory(directives, ndirs, cicbfunc, &mylock);
            if (PMIX_ERR_OPERATION_IN_PROGRESS == rc) {
                mylock->requests++;
            }
        }
    }
    if (0 == mylock->requests) {
        /* there will be no replies */
        if (NULL != cbfunc) {
            cbfunc(PMIX_SUCCESS, NULL, 0, cbdata, NULL, NULL);
        }
    } else {
        PMIX_INFO_CREATE(mylock->info, mylock->requests);
        if (NULL == mylock->info) {
            /* better let them know */
            if (NULL != cbfunc) {
                cbfunc(PMIX_ERR_NOMEM, NULL, 0, cbdata, NULL, NULL);
            }
        } else {
            mylock->ninfo = mylock->requests;
            PMIX_RELEASE_THREAD(&mylock->lock);
            return;
        }
    }

    PMIX_RELEASE_THREAD(&mylock->lock);
    PMIX_RELEASE(mylock);
    return;
}

pmix_status_t pmix_pnet_base_harvest_envars(char **incvars, char **excvars,
                                            pmix_list_t *ilist)
{
    int i, j;
    size_t len;
    pmix_kval_t *kv, *next;
    char *cs_env, *string_key;

    /* harvest envars to pass along */
    for (j=0; NULL != incvars[j]; j++) {
        len = strlen(incvars[j]);
        if ('*' == incvars[j][len-1]) {
            --len;
        }
        for (i = 0; NULL != environ[i]; ++i) {
            if (0 == strncmp(environ[i], incvars[j], len)) {
                cs_env = strdup(environ[i]);
                kv = PMIX_NEW(pmix_kval_t);
                if (NULL == kv) {
                    return PMIX_ERR_OUT_OF_RESOURCE;
                }
                kv->key = strdup(PMIX_SET_ENVAR);
                kv->value = (pmix_value_t*)malloc(sizeof(pmix_value_t));
                if (NULL == kv->value) {
                    PMIX_RELEASE(kv);
                    return PMIX_ERR_OUT_OF_RESOURCE;
                }
                kv->value->type = PMIX_ENVAR;
                string_key = strchr(cs_env, '=');
                *string_key = '\0';
                ++string_key;
                PMIX_ENVAR_LOAD(&kv->value->data.envar, cs_env, string_key, ':');
                pmix_list_append(ilist, &kv->super);
                free(cs_env);
            }
        }
    }

    /* now check the exclusions and remove any that match */
    if (NULL != excvars) {
        for (j=0; NULL != excvars[j]; j++) {
            len = strlen(excvars[j]);
            if ('*' == excvars[j][len-1]) {
                --len;
            }
            PMIX_LIST_FOREACH_SAFE(kv, next, ilist, pmix_kval_t) {
                if (0 == strncmp(kv->value->data.envar.envar, excvars[j], len)) {
                    pmix_list_remove_item(ilist, &kv->super);
                    PMIX_RELEASE(kv);
                }
            }
        }
    }
    return PMIX_SUCCESS;
}
