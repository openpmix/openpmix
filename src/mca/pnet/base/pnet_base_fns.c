/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2016      Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2018      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 *
 * Copyright (c) 2021-2026 Nanook Consulting.  All rights reserved.
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

#include "src/mca/pnet/base/base.h"

/* Reached from _setup_app, on behalf of PMIx_server_setup_application.
 * The module fan-out is gated on being a server; the namespace lookup
 * above it is not, because the object is what the fan-out is given. */
pmix_status_t pmix_pnet_base_allocate(char *nspace, pmix_info_t info[], size_t ninfo,
                                      pmix_list_t *ilist)
{
    pmix_pnet_base_active_module_t *active;
    pmix_status_t rc;
    pmix_namespace_t *nptr, *ns;

    pmix_output_verbose(2, pmix_pnet_base_framework.framework_output, "pnet:allocate called");

    /* protect against bozo inputs.  An *empty* nspace has to be caught here
     * too, and not just a NULL one: PMIX_CHECK_NSPACE calls one a wildcard
     * and matches it against anything, so letting one through would have
     * these lookups answer with whatever namespace happened to be first on
     * the list */
    if (PMIX_NSPACE_INVALID(nspace) || NULL == ilist) {
        return PMIX_ERR_BAD_PARAM;
    }

    /* NOT a guard on the walk below - the walk is safe on an empty list,
     * as every other PMIX_LIST_FOREACH in the tree relies on. This is an
     * early-out that also skips the namespace object built for the
     * modules to hang data on: with no module to receive it, publishing a
     * bare namespace onto pmix_globals.nspaces would change what walkers
     * of that list find (_dmodex_req takes its "we know this nspace"
     * branch on the strength of an entry with no ranks). */
    if (0 == pmix_list_get_size(&pmix_pnet_globals.actives)) {
        return PMIX_SUCCESS;
    }

    /* find this proc's nspace object */
    nptr = NULL;
    PMIX_LIST_FOREACH (ns, &pmix_globals.nspaces, pmix_namespace_t) {
        if (PMIX_CHECK_NSPACE(ns->nspace, nspace)) {
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
        if (NULL == nptr->nspace) {
            /* every lookup of this list compares that string, so an object
             * carrying a NULL one must never be published */
            PMIX_RELEASE(nptr);
            return PMIX_ERR_NOMEM;
        }
        pmix_list_append(&pmix_globals.nspaces, &nptr->super);
    }

    if (PMIX_PEER_IS_SERVER(pmix_globals.mypeer)) {
        /* process the allocation request */
        PMIX_LIST_FOREACH (active, &pmix_pnet_globals.actives, pmix_pnet_base_active_module_t) {
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
pmix_status_t pmix_pnet_base_setup_local_network(char *nspace, pmix_info_t info[], size_t ninfo)
{
    pmix_pnet_base_active_module_t *active;
    pmix_status_t rc;
    pmix_namespace_t *nsp, *nsp2;
    pmix_nspace_env_cache_t *ns, *ns2;

    pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                        "pnet: setup_local_network called");

    /* protect against bozo inputs - see pmix_pnet_base_allocate on why an
     * empty nspace is as dangerous as a NULL one */
    if (PMIX_NSPACE_INVALID(nspace)) {
        return PMIX_ERR_BAD_PARAM;
    }

    /* NOT a guard on the walk below - see the note in allocate(). What
     * this skips is the pmix_pnet_globals.nspaces cache entry built for
     * the modules; with none selected, nothing would ever read it. */
    if (0 == pmix_list_get_size(&pmix_pnet_globals.actives)) {
        return PMIX_SUCCESS;
    }

    /* find this proc's nspace object */
    ns = NULL;
    PMIX_LIST_FOREACH (ns2, &pmix_pnet_globals.nspaces, pmix_nspace_env_cache_t) {
        if (PMIX_CHECK_NSPACE(ns2->ns->nspace, nspace)) {
            ns = ns2;
            break;
        }
    }
    if (NULL == ns) {
        /* find the namespace object for this nspace */
        nsp = NULL;
        PMIX_LIST_FOREACH (nsp2, &pmix_globals.nspaces, pmix_namespace_t) {
            if (PMIX_CHECK_NSPACE(nsp2->nspace, nspace)) {
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
            if (NULL == nsp->nspace) {
                PMIX_RELEASE(nsp);
                return PMIX_ERR_NOMEM;
            }
            pmix_list_append(&pmix_globals.nspaces, &nsp->super);
        }
        ns = PMIX_NEW(pmix_nspace_env_cache_t);
        if (NULL == ns) {
            return PMIX_ERR_NOMEM;
        }
        /* the cache owns this reference for as long as it lives; the class
         * destructor is what gives it back */
        PMIX_RETAIN(nsp);
        ns->ns = nsp;
        pmix_list_append(&pmix_pnet_globals.nspaces, &ns->super);
    }

    PMIX_LIST_FOREACH (active, &pmix_pnet_globals.actives, pmix_pnet_base_active_module_t) {
        if (NULL != active->module->setup_local_network) {
            rc = active->module->setup_local_network(ns, info, ninfo);
            if (PMIX_SUCCESS != rc && PMIX_ERR_NOT_AVAILABLE != rc
                && PMIX_ERR_TAKE_NEXT_OPTION != rc) {
                return rc;
            }
        }
    }

    return PMIX_SUCCESS;
}

/* can only be called by a server from within an event! */
pmix_status_t pmix_pnet_base_setup_fork(const pmix_proc_t *proc, char ***env)
{
    pmix_nspace_env_cache_t *ns, *ns2;
    pmix_envar_list_item_t *ev;
    pmix_pnet_base_active_module_t *active;
    pmix_status_t rc;

    pmix_output_verbose(2, pmix_pnet_base_framework.framework_output, "pnet: setup_fork called");

    /* protect against bozo inputs.  The nspace check is not pedantry: the
     * cache lookup below matches with PMIX_CHECK_NSPACE, which calls an
     * empty nspace a wildcard, so a proc that carried one would be forked
     * with the first job on the list's envars - its transport key
     * included */
    if (NULL == proc || NULL == env || PMIX_NSPACE_INVALID(proc->nspace)) {
        return PMIX_ERR_BAD_PARAM;
    }

    /* see if we have this nspace */
    ns = NULL;
    PMIX_LIST_FOREACH (ns2, &pmix_pnet_globals.nspaces, pmix_nspace_env_cache_t) {
        if (PMIX_CHECK_NSPACE(ns2->ns->nspace, proc->nspace)) {
            ns = ns2;
            break;
        }
    }
    if (NULL != ns) {
        PMIX_LIST_FOREACH (ev, &ns->envars, pmix_envar_list_item_t) {
            /* overwrite is true, so the only ways this fails are a bad env
             * pointer and out-of-memory - both genuine.  Swallowing them
             * would fork a process missing a job-wide setting while
             * reporting success, which is the very thing the module loop
             * below refuses to do */
            rc = PMIx_Setenv(ev->envar.envar, ev->envar.value, true, env);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                return rc;
            }
        }
    }

    /* Then let each module contribute anything specific to THIS process.
     * The cache above is per namespace, so it can carry only what every
     * rank of the job shares; a device assignment is per rank and has
     * nowhere else to be set.
     *
     * A module declining is the normal case - most processes are not
     * mapped against a device at all - so only a genuine failure is
     * propagated, and it aborts the fork rather than launching a process
     * that would silently use the wrong hardware. */
    PMIX_LIST_FOREACH (active, &pmix_pnet_globals.actives, pmix_pnet_base_active_module_t) {
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

/* Does this device belong to one of the PCI families the caller named? */
static bool device_matches(const pmix_hwloc_device_t *dev,
                           const pmix_pnet_pcimatch_t *match, size_t nmatch)
{
    size_t m;

    for (m = 0; m < nmatch; m++) {
        if (dev->pci_vendor != match[m].vendor) {
            continue;
        }
        if (0 == match[m].devclass || dev->pci_class == match[m].devclass) {
            return true;
        }
    }
    return false;
}

/* Build the value of a device-selection variable for one process from the
 * network devices it was mapped against.
 *
 * Every fabric library spells this the same way at bottom - a variable
 * naming the devices the process may use - so the loop belongs here and the
 * components differ only in which devices are theirs and what the variable
 * is called.  What goes in it is the device's "selector", which for a NIC
 * is the OS device name; that is what UCX_NET_DEVICES, NCCL_IB_HCA and
 * PSM3_NIC all accept, and unlike a GPU's vendor identity it is never
 * missing.
 *
 * Two things it deliberately does not do.  It does not invent a selector
 * the topology did not supply - in particular it never derives a unit
 * ordinal from a device name for the variables that take one, because an
 * ordinal is meaningful only against an enumeration PMIx did not perform.
 * And it does not consult the mapper's decision - it reads what the process
 * was told, so a process that was not mapped against a device is left
 * alone.
 *
 * The selector is read from THIS node's topology, which is why this runs at
 * fork time on the daemon rather than anywhere on the head node: the head
 * node's copy of a topology may belong to whichever node reported it first.
 */
pmix_status_t pmix_pnet_base_get_assigned_devices(const pmix_proc_t *proc,
                                                  const pmix_pnet_pcimatch_t *match,
                                                  size_t nmatch,
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

    if (NULL == proc || NULL == match || 0 == nmatch || NULL == value) {
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
    /* and it must really be an array of devices.  PMIX_DEVICE_ID is
     * documented as a string, and the library reads it that way elsewhere,
     * so a host with more than one device to report can just as reasonably
     * hand us an array of names - reading that as pmix_device_t would walk
     * off the end of the allocation.  We do not know how to use such an
     * array, so we decline it rather than trust it */
    if (PMIX_DEVICE != darray->type || NULL == darray->array) {
        PMIX_DESTRUCT(&cb);
        return PMIX_ERR_TAKE_NEXT_OPTION;
    }
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
                                    PMIX_DEVTYPE_NETWORK | PMIX_DEVTYPE_OPENFABRICS,
                                    dev[n].osname, &devs, &ndevs);
        if (PMIX_SUCCESS != rc) {
            continue;
        }
        for (d = 0; d < ndevs; d++) {
            /* a node may carry more than one fabric, and each wants its own
             * variable, so take only our own */
            if (NULL == devs[d].selector || !device_matches(&devs[d], match, nmatch)) {
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

pmix_status_t pmix_pnet_base_set_assigned_devices(const pmix_proc_t *proc,
                                                  const pmix_pnet_pcimatch_t *match,
                                                  size_t nmatch,
                                                  const char *envar,
                                                  char ***env)
{
    pmix_status_t rc;
    char *val = NULL;

    if (NULL == envar || NULL == env) {
        return PMIX_ERR_BAD_PARAM;
    }

    rc = pmix_pnet_base_get_assigned_devices(proc, match, nmatch, &val);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                        "pnet: %s=%s for %s", envar, val, PMIX_NAME_PRINT(proc));
    /* setting it is the whole job: reporting success without having done
     * so launches the process pointed at nothing in particular */
    rc = PMIx_Setenv(envar, val, true, env);
    free(val);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
    }
    return rc;
}

void pmix_pnet_base_child_finalized(pmix_proc_t *peer)
{
    pmix_pnet_base_active_module_t *active;

    pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                        "pnet: child_finalized called");

    /* protect against bozo inputs */
    if (NULL == peer) {
        PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
        return;
    }

    PMIX_LIST_FOREACH (active, &pmix_pnet_globals.actives, pmix_pnet_base_active_module_t) {
        if (NULL != active->module->child_finalized) {
            active->module->child_finalized(peer);
        }
    }

    return;
}

void pmix_pnet_base_local_app_finalized(pmix_namespace_t *nptr)
{
    pmix_pnet_base_active_module_t *active;

    pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                        "pnet: local_app_finalized called");

    /* protect against bozo inputs */
    if (NULL == nptr) {
        return;
    }

    PMIX_LIST_FOREACH (active, &pmix_pnet_globals.actives, pmix_pnet_base_active_module_t) {
        if (NULL != active->module->local_app_finalized) {
            active->module->local_app_finalized(nptr);
        }
    }

    return;
}

void pmix_pnet_base_deregister_nspace(char *nspace)
{
    pmix_pnet_base_active_module_t *active;
    pmix_nspace_env_cache_t *ns, *ns2;
    pmix_namespace_t *nptr, *nsp;

    pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                        "pnet: deregister_nspace called");

    /* protect against bozo inputs - an empty nspace would match, and so
     * release, whichever job's cache happens to be first on the list */
    if (PMIX_NSPACE_INVALID(nspace)) {
        return;
    }

    /* remove this nspace's envar cache, if it has one */
    ns = NULL;
    PMIX_LIST_FOREACH (ns2, &pmix_pnet_globals.nspaces, pmix_nspace_env_cache_t) {
        if (PMIX_CHECK_NSPACE(ns2->ns->nspace, nspace)) {
            ns = ns2;
            pmix_list_remove_item(&pmix_pnet_globals.nspaces, &ns->super);
            break;
        }
    }

    /* Only setup_local_network creates that cache, and a process that never
     * runs it - a scheduler, or a daemon this job put no procs on - still
     * has to tell the modules to release whatever allocate took for this
     * job, or a component holding static ports never gets them back.  So
     * fall back to the namespace object itself. */
    if (NULL != ns) {
        nptr = ns->ns;
    } else {
        nptr = NULL;
        PMIX_LIST_FOREACH (nsp, &pmix_globals.nspaces, pmix_namespace_t) {
            if (PMIX_CHECK_NSPACE(nsp->nspace, nspace)) {
                nptr = nsp;
                break;
            }
        }
    }
    if (NULL == nptr) {
        return;
    }

    PMIX_LIST_FOREACH (active, &pmix_pnet_globals.actives, pmix_pnet_base_active_module_t) {
        if (NULL != active->module->deregister_nspace) {
            active->module->deregister_nspace(nptr);
        }
    }
    /* after the fan-out, not before: the cache holds the only reference
     * that keeps nptr alive once the server has dropped its own */
    if (NULL != ns) {
        PMIX_RELEASE(ns);
    }
}

pmix_status_t pmix_pnet_base_collect_inventory(pmix_info_t directives[], size_t ndirs,
                                               pmix_list_t *inventory)
{
    pmix_pnet_base_active_module_t *active;
    pmix_status_t rc;

    PMIX_LIST_FOREACH (active, &pmix_pnet_globals.actives, pmix_pnet_base_active_module_t) {
        if (NULL != active->module->collect_inventory) {
            pmix_output_verbose(5, pmix_pnet_base_framework.framework_output, "COLLECTING %s",
                                active->module->name);
            rc = active->module->collect_inventory(directives, ndirs, inventory);
            if (PMIX_SUCCESS != rc) {
                /* unlike allocate/setup_fork, this fan-out has no decline
                 * convention - any error is unexpected, and it aborts the
                 * collection, so say which module ended it */
                PMIX_ERROR_LOG(rc);
                return rc;
            }
        }
    }
    return PMIX_SUCCESS;
}

pmix_status_t pmix_pnet_base_deliver_inventory(pmix_info_t info[], size_t ninfo,
                                               pmix_info_t directives[], size_t ndirs)
{
    pmix_pnet_base_active_module_t *active;
    pmix_status_t rc;

    PMIX_LIST_FOREACH (active, &pmix_pnet_globals.actives, pmix_pnet_base_active_module_t) {
        if (NULL != active->module->deliver_inventory) {
            pmix_output_verbose(5, pmix_pnet_base_framework.framework_output, "DELIVERING TO %s",
                                active->module->name);
            rc = active->module->deliver_inventory(info, ninfo, directives, ndirs);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                return rc;
            }
        }
    }
    return PMIX_SUCCESS;
}

pmix_status_t pmix_pnet_base_register_fabric(pmix_fabric_t *fabric, const pmix_info_t directives[],
                                             size_t ndirs, pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    pmix_pnet_base_active_module_t *active;
    pmix_status_t rc;
    pmix_pnet_fabric_t *ft;

    /* protect against bozo input, as update and deregister both do */
    if (NULL == fabric) {
        return PMIX_ERR_BAD_PARAM;
    }

    /* ensure our fields of the fabric object are initialized */
    fabric->info = NULL;
    fabric->ninfo = 0;
    fabric->module = NULL;

    /* NOT a guard on the walk below, which answers PMIX_ERR_NOT_FOUND
     * when it ends without a claim. The two are different answers worth
     * keeping apart: no component at all cannot support fabrics, while a
     * component that declined this one simply did not find it. */
    if (0 == pmix_list_get_size(&pmix_pnet_globals.actives)) {
        return PMIX_ERR_NOT_SUPPORTED;
    }

    /* scan across active modules until one returns success */
    PMIX_LIST_FOREACH (active, &pmix_pnet_globals.actives, pmix_pnet_base_active_module_t) {
        if (NULL != active->module->register_fabric) {
            rc = active->module->register_fabric(fabric, directives, ndirs, cbfunc, cbdata);
            if (PMIX_OPERATION_SUCCEEDED == rc) {
                /* track this fabric so we can respond to remote requests */
                ft = PMIX_NEW(pmix_pnet_fabric_t);
                if (NULL == ft) {
                    return PMIX_ERR_NOMEM;
                }
                ft->index = fabric->index;
                if (NULL != fabric->name) {
                    ft->name = strdup(fabric->name);
                    if (NULL == ft->name) {
                        PMIX_RELEASE(ft);
                        return PMIX_ERR_NOMEM;
                    }
                }
                ft->module = active->module;
                pmix_list_append(&pmix_pnet_globals.fabrics, &ft->super);
                return rc;
            } else if (PMIX_ERR_TAKE_NEXT_OPTION != rc) {
                /* just return the result */
                return rc;
            }
        }
    }

    return PMIX_ERR_NOT_FOUND;
}

/* Which locally-registered plane is this request about?
 *
 * A request relayed from a remote peer arrives with nothing but the index
 * and/or name the fabric was registered under, so this tracker list is the
 * only route back to the module that owns it.  First match wins: scanning
 * on and letting the last one win made two planes that happen to share an
 * index resolve differently here than in the caller that registered them.
 */
static pmix_pnet_fabric_t *find_fabric(const pmix_fabric_t *fabric)
{
    pmix_pnet_fabric_t *ft;

    PMIX_LIST_FOREACH (ft, &pmix_pnet_globals.fabrics, pmix_pnet_fabric_t) {
        if (fabric->index == ft->index) {
            return ft;
        }
        if (NULL != fabric->name && NULL != ft->name
            && 0 == strcmp(ft->name, fabric->name)) {
            return ft;
        }
    }
    return NULL;
}

pmix_status_t pmix_pnet_base_update_fabric(pmix_fabric_t *fabric)
{
    pmix_status_t rc = PMIX_SUCCESS;
    pmix_pnet_module_t *module = NULL;
    pmix_pnet_fabric_t *ft;

    /* protect against bozo input */
    if (NULL == fabric) {
        return PMIX_ERR_BAD_PARAM;
    }
    if (NULL == fabric->module) {
        ft = find_fabric(fabric);
    } else {
        ft = (pmix_pnet_fabric_t *) fabric->module;
    }
    if (NULL == ft) {
        return PMIX_ERR_BAD_PARAM;
    }
    module = ft->module;
    if (NULL == module) {
        return PMIX_ERR_BAD_PARAM;
    }

    if (NULL != module->update_fabric) {
        rc = module->update_fabric(fabric);
    }
    return rc;
}

pmix_status_t pmix_pnet_base_deregister_fabric(pmix_fabric_t *fabric)
{
    pmix_status_t rc = PMIX_SUCCESS;
    pmix_pnet_module_t *module = NULL;
    pmix_pnet_fabric_t *ft;

    /* protect against bozo input */
    if (NULL == fabric) {
        return PMIX_ERR_BAD_PARAM;
    }
    if (NULL == fabric->module) {
        ft = find_fabric(fabric);
    } else {
        ft = (pmix_pnet_fabric_t *) fabric->module;
    }
    if (NULL == ft) {
        return PMIX_ERR_BAD_PARAM;
    }
    module = ft->module;
    if (NULL == module) {
        return PMIX_ERR_BAD_PARAM;
    }

    if (NULL != module->deregister_fabric) {
        rc = module->deregister_fabric(fabric);
    }

    /* Drop the tracker whatever the module made of the request.  Keeping it
     * grew the list without bound across register/deregister cycles, and -
     * the part that bites - a later update or deregister carrying the same
     * index or name still resolved to this module and handed it a plane it
     * had already torn down. */
    pmix_list_remove_item(&pmix_pnet_globals.fabrics, &ft->super);
    PMIX_RELEASE(ft);

    return rc;
}
