/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2016      Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2018      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 *
 * Copyright (c) 2021-2022 Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "src/include/pmix_config.h"

#include "pmix_common.h"
#include "src/include/pmix_globals.h"

#include "src/class/pmix_list.h"
#include "src/hwloc/pmix_hwloc.h"
#include "src/mca/gds/gds.h"
#include "src/mca/preg/preg.h"
#include "src/server/pmix_server_ops.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_environ.h"

#include "src/mca/pgpu/base/base.h"

/* NOTE: a tool (e.g., prun) may call this function to
 * harvest local envars for inclusion in a call to
 * PMIx_Spawn, or it might be called in response to
 * a call to PMIx_Allocate_resources */
pmix_status_t pmix_pgpu_base_allocate(char *nspace, pmix_info_t info[], size_t ninfo,
                                      pmix_list_t *ilist)
{
    pmix_pgpu_base_active_module_t *active;
    pmix_status_t rc;
    pmix_namespace_t *nptr, *ns;

    pmix_output_verbose(2, pmix_pgpu_base_framework.framework_output, "pgpu:allocate called");

    /* protect against bozo inputs */
    if (NULL == nspace || NULL == ilist) {
        return PMIX_ERR_BAD_PARAM;
    }

    if (0 == pmix_list_get_size(&pmix_pgpu_globals.actives)) {
        return PMIX_SUCCESS;
    }

    /* find this proc's nspace object */
    nptr = NULL;
    PMIX_LIST_FOREACH (ns, &pmix_globals.nspaces, pmix_namespace_t) {
        if (0 == strcmp(ns->nspace, nspace)) {
            nptr = ns;
            break;
        }
    }
    if (NULL == nptr) {
        /* add it */
        nptr = PMIX_NEW(pmix_namespace_t);
        if (NULL == nptr) {
            return PMIX_ERR_NOMEM;
        }
        nptr->nspace = strdup(nspace);
        pmix_list_append(&pmix_globals.nspaces, &nptr->super);
    }

    if (PMIX_PEER_IS_SERVER(pmix_globals.mypeer)) {
        /* process the allocation request */
        PMIX_LIST_FOREACH (active, &pmix_pgpu_globals.actives, pmix_pgpu_base_active_module_t) {
            if (NULL != active->module->allocate) {
                rc = active->module->allocate(nptr, info, ninfo, ilist);
                if (PMIX_SUCCESS != rc && PMIX_ERR_NOT_AVAILABLE != rc
                    && PMIX_ERR_TAKE_NEXT_OPTION != rc) {
                    /* true error */
                    return rc;
                }
            }
        }
    }

    return PMIX_SUCCESS;
}

/* can only be called by a server from within an event! */
pmix_status_t pmix_pgpu_base_setup_local(char *nspace, pmix_info_t info[], size_t ninfo)
{
    pmix_pgpu_base_active_module_t *active;
    pmix_status_t rc;
    pmix_nspace_env_cache_t *ns, *ns2;
    pmix_namespace_t *nsp, *nsp2;

    pmix_output_verbose(2, pmix_pgpu_base_framework.framework_output,
                        "pgpu: setup_local_network called");

    /* protect against bozo inputs */
    if (NULL == nspace) {
        return PMIX_ERR_BAD_PARAM;
    }

    if (0 == pmix_list_get_size(&pmix_pgpu_globals.actives)) {
        return PMIX_SUCCESS;
    }

    /* find this proc's nspace object */
    ns = NULL;
    PMIX_LIST_FOREACH (ns2, &pmix_pgpu_globals.nspaces, pmix_nspace_env_cache_t) {
        if (PMIX_CHECK_NSPACE(ns2->ns->nspace, nspace)) {
            ns = ns2;
            break;
        }
    }
    if (NULL == ns) {
        /* find the namespace object for this nspace */
        nsp = NULL;
        PMIX_LIST_FOREACH (nsp2, &pmix_globals.nspaces, pmix_namespace_t) {
            if (0 == strcmp(nsp2->nspace, nspace)) {
                nsp = nsp2;
                break;
            }
        }
        if (NULL == nsp) {
            /* add it */
            nsp = PMIX_NEW(pmix_namespace_t);
            if (NULL == nsp) {
                return PMIX_ERR_NOMEM;
            }
            nsp->nspace = strdup(nspace);
            pmix_list_append(&pmix_globals.nspaces, &nsp->super);
        }
        ns = PMIX_NEW(pmix_nspace_env_cache_t);
        PMIX_RETAIN(nsp);
        ns->ns = nsp;
        pmix_list_append(&pmix_pgpu_globals.nspaces, &ns->super);
    }

    PMIX_LIST_FOREACH (active, &pmix_pgpu_globals.actives, pmix_pgpu_base_active_module_t) {
        if (NULL != active->module->setup_local) {
            rc = active->module->setup_local(ns, info, ninfo);
            if (PMIX_SUCCESS != rc && PMIX_ERR_NOT_AVAILABLE != rc
                && PMIX_ERR_TAKE_NEXT_OPTION != rc) {
                return rc;
            }
        }
    }

    return PMIX_SUCCESS;
}

/* can only be called by a server from within an event! */
pmix_status_t pmix_pgpu_base_setup_fork(const pmix_proc_t *proc, char ***env)
{
    pmix_nspace_env_cache_t *ns, *ns2;
    pmix_envar_list_item_t *ev;
    pmix_pgpu_base_active_module_t *active;
    pmix_status_t rc;

    pmix_output_verbose(2, pmix_pgpu_base_framework.framework_output, "pgpu: setup_fork called");

    /* protect against bozo inputs */
    if (NULL == proc || NULL == env) {
        return PMIX_ERR_BAD_PARAM;
    }

    /* see if we have this nspace */
    ns = NULL;
    PMIX_LIST_FOREACH (ns2, &pmix_pgpu_globals.nspaces, pmix_nspace_env_cache_t) {
        if (PMIX_CHECK_NSPACE(ns2->ns->nspace, proc->nspace)) {
            ns = ns2;
            break;
        }
    }
    if (NULL != ns) {
        PMIX_LIST_FOREACH (ev, &ns->envars, pmix_envar_list_item_t) {
            PMIx_Setenv(ev->envar.envar, ev->envar.value, true, env);
        }
    }

    /* Then let each module contribute anything that is specific to THIS
     * process.  The cache above is per namespace, so it can carry only
     * what every rank of the job shares; a device assignment is per rank
     * and has nowhere else to be set.
     *
     * A module declining is the normal case - most processes are not
     * mapped against a device at all - so only a genuine failure is
     * propagated, and it aborts the fork rather than launching a process
     * that would silently use the wrong hardware. */
    PMIX_LIST_FOREACH (active, &pmix_pgpu_globals.actives, pmix_pgpu_base_active_module_t) {
        if (NULL == active->module->setup_fork) {
            continue;
        }
        rc = active->module->setup_fork(proc, env);
        if (PMIX_SUCCESS != rc && PMIX_ERR_NOT_AVAILABLE != rc
            && PMIX_ERR_TAKE_NEXT_OPTION != rc) {
            return rc;
        }
    }

    return PMIX_SUCCESS;
}

/* Build the value of a vendor's "visible devices" variable for one process
 * from the devices it was mapped against.
 *
 * Every GPU vendor spells this the same way at bottom - a variable naming
 * the subset of devices the process may use - so the loop belongs here and
 * the components differ only in which vendor's devices are theirs, what
 * the variable is called, and what its grammar accepts. That last is the
 * device's "selector", which the topology layer produces: an identity for
 * the vendors whose variable takes one, an ordinal for Intel's, which
 * takes nothing else.
 *
 * Two things it deliberately does not do. It does not invent a selector
 * the topology did not supply - notably it never falls back to guessing an
 * index for a vendor whose variable would accept one, because the runtimes
 * number devices in an order PMIx does not know (CUDA's default is
 * fastest-first, not bus order), and a wrong value in these variables
 * truncates the visible set silently rather than failing. And it does not
 * consult the mapper's decision - it reads what the process was told, so a
 * process that was not mapped against a device is simply left alone.
 *
 * The selector is read from THIS node's topology, which is the reason this
 * runs at fork time on the daemon rather than anywhere on the head node: a
 * device's vendor identity differs between nodes, and the head node's copy
 * of a topology may belong to whichever node reported it first.
 */
pmix_status_t pmix_pgpu_base_get_visible_devices(const pmix_proc_t *proc,
                                                 const char *vendor,
                                                 char **value)
{
    pmix_cb_t cb;
    pmix_kval_t *kv;
    pmix_data_array_t *darray;
    pmix_device_t *dev;
    pmix_hwloc_device_t *devs;
    size_t ndevs, n, d;
    char **ids = NULL;
    pmix_status_t rc;

    if (NULL == proc || NULL == vendor || NULL == value) {
        return PMIX_ERR_BAD_PARAM;
    }
    *value = NULL;

    /* what device(s) was this process given?  Absent is the common case -
     * the job was not mapped by device - and is not an error */
    PMIX_CONSTRUCT(&cb, pmix_cb_t);
    cb.proc = (pmix_proc_t *) proc;
    cb.copy = true;
    cb.key = PMIX_DEVICE_ID;
    PMIX_GDS_FETCH_KV(rc, pmix_globals.mypeer, &cb);
    cb.key = NULL;
    if (PMIX_SUCCESS != rc || 1 != pmix_list_get_size(&cb.kvs)) {
        PMIX_DESTRUCT(&cb);
        return PMIX_ERR_TAKE_NEXT_OPTION;
    }
    kv = (pmix_kval_t *) pmix_list_get_first(&cb.kvs);
    if (NULL == kv->value || PMIX_DATA_ARRAY != kv->value->type
        || NULL == kv->value->data.darray) {
        PMIX_DESTRUCT(&cb);
        return PMIX_ERR_TAKE_NEXT_OPTION;
    }
    darray = kv->value->data.darray;
    dev = (pmix_device_t *) darray->array;

    for (n = 0; n < darray->size; n++) {
        if (NULL == dev[n].osname) {
            continue;
        }
        /* resolve the assignment against the local topology.  The name is
         * the handle: it is what the enumerator called this device on this
         * node, and it is what the mapper recorded */
        devs = NULL;
        ndevs = 0;
        rc = pmix_hwloc_get_devices(&pmix_globals.topology, pmix_globals.hostname,
                                    PMIX_DEVTYPE_GPU | PMIX_DEVTYPE_COPROC,
                                    dev[n].osname, &devs, &ndevs);
        if (PMIX_SUCCESS != rc) {
            continue;
        }
        for (d = 0; d < ndevs; d++) {
            /* a node may carry cards from more than one vendor, and each
             * wants its own variable, so take only our own */
            if (NULL == devs[d].selector || NULL == devs[d].vendor
                || 0 != strcasecmp(devs[d].vendor, vendor)) {
                continue;
            }
            PMIx_Argv_append_nosize(&ids, devs[d].selector);
        }
        pmix_hwloc_release_devices(devs, ndevs);
    }
    PMIX_DESTRUCT(&cb);

    if (NULL == ids) {
        return PMIX_ERR_TAKE_NEXT_OPTION;
    }
    *value = PMIx_Argv_join(ids, ',');
    PMIx_Argv_free(ids);
    if (NULL == *value) {
        return PMIX_ERR_NOMEM;
    }
    return PMIX_SUCCESS;
}

pmix_status_t pmix_pgpu_base_set_visible_devices(const pmix_proc_t *proc,
                                                 const char *vendor,
                                                 const char *envar,
                                                 char ***env)
{
    pmix_status_t rc;
    char *val = NULL;

    if (NULL == envar || NULL == env) {
        return PMIX_ERR_BAD_PARAM;
    }

    rc = pmix_pgpu_base_get_visible_devices(proc, vendor, &val);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    pmix_output_verbose(2, pmix_pgpu_base_framework.framework_output,
                        "pgpu: %s=%s for %s", envar, val, PMIX_NAME_PRINT(proc));
    PMIx_Setenv(envar, val, true, env);
    free(val);
    return PMIX_SUCCESS;
}

void pmix_pgpu_base_child_finalized(pmix_proc_t *peer)
{
    pmix_pgpu_base_active_module_t *active;

    pmix_output_verbose(2, pmix_pgpu_base_framework.framework_output,
                        "pgpu: child_finalized called");

    /* protect against bozo inputs */
    if (NULL == peer) {
        PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
        return;
    }

    if (0 == pmix_list_get_size(&pmix_pgpu_globals.actives)) {
        return;
    }

    PMIX_LIST_FOREACH (active, &pmix_pgpu_globals.actives, pmix_pgpu_base_active_module_t) {
        if (NULL != active->module->child_finalized) {
            active->module->child_finalized(peer);
        }
    }

    return;
}

void pmix_pgpu_base_local_app_finalized(pmix_namespace_t *nptr)
{
    pmix_pgpu_base_active_module_t *active;

    pmix_output_verbose(2, pmix_pgpu_base_framework.framework_output,
                        "pgpu: local_app_finalized called");

    /* protect against bozo inputs */
    if (NULL == nptr) {
        return;
    }

    if (0 == pmix_list_get_size(&pmix_pgpu_globals.actives)) {
        return;
    }

    PMIX_LIST_FOREACH (active, &pmix_pgpu_globals.actives, pmix_pgpu_base_active_module_t) {
        if (NULL != active->module->local_app_finalized) {
            active->module->local_app_finalized(nptr);
        }
    }

    return;
}

void pmix_pgpu_base_deregister_nspace(char *nspace)
{
    pmix_pgpu_base_active_module_t *active;
    pmix_nspace_env_cache_t *ns, *ns2;

    pmix_output_verbose(2, pmix_pgpu_base_framework.framework_output,
                        "pgpu: deregister_nspace called");

    /* protect against bozo inputs */
    if (NULL == nspace) {
        return;
    }

    /* find this nspace object */
    ns = NULL;
    PMIX_LIST_FOREACH (ns2, &pmix_pgpu_globals.nspaces, pmix_nspace_env_cache_t) {
        if (PMIX_CHECK_NSPACE(ns2->ns->nspace, nspace)) {
            ns = ns2;
            pmix_list_remove_item(&pmix_pgpu_globals.nspaces, &ns->super);
            break;
        }
    }
    if (NULL == ns) {
        return;
    }

    PMIX_LIST_FOREACH (active, &pmix_pgpu_globals.actives, pmix_pgpu_base_active_module_t) {
        if (NULL != active->module->deregister_nspace) {
            active->module->deregister_nspace(ns->ns);
        }
    }
    PMIX_RELEASE(ns);
}

pmix_status_t pmix_pgpu_base_collect_inventory(pmix_info_t directives[], size_t ndirs,
                                               pmix_list_t *inventory)
{
    pmix_pgpu_base_active_module_t *active;
    pmix_status_t rc;

    PMIX_LIST_FOREACH (active, &pmix_pgpu_globals.actives, pmix_pgpu_base_active_module_t) {
        if (NULL != active->module->collect_inventory) {
            pmix_output_verbose(5, pmix_pgpu_base_framework.framework_output,
                                "COLLECTING %s", active->module->name);
            rc = active->module->collect_inventory(directives, ndirs, inventory);
            if (PMIX_SUCCESS != rc) {
                return rc;
            }
        }
    }
    return PMIX_SUCCESS;
}

pmix_status_t pmix_pgpu_base_deliver_inventory(pmix_info_t info[], size_t ninfo,
                                               pmix_info_t directives[], size_t ndirs)
{
    pmix_pgpu_base_active_module_t *active;
    pmix_status_t rc;

    PMIX_LIST_FOREACH (active, &pmix_pgpu_globals.actives, pmix_pgpu_base_active_module_t) {
        if (NULL != active->module->deliver_inventory) {
            pmix_output_verbose(5, pmix_pgpu_base_framework.framework_output, "DELIVERING TO %s",
                                active->module->name);
            rc = active->module->deliver_inventory(info, ninfo, directives, ndirs);
            if (PMIX_SUCCESS != rc) {
                return rc;
            }
        }
    }
    return PMIX_SUCCESS;
}
