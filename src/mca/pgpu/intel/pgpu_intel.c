/*
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2016      IBM Corporation.  All rights reserved.
 *
 * Copyright (c) 2021-2022 Nanook Consulting.  All rights reserved.
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "src/include/pmix_config.h"

#include <string.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif
#ifdef HAVE_SYS_TYPES_H
#    include <sys/types.h>
#endif
#ifdef HAVE_SYS_STAT_H
#    include <sys/stat.h>
#endif
#ifdef HAVE_FCNTL_H
#    include <fcntl.h>
#endif
#include <time.h>

#include "pmix_common.h"

#include "src/class/pmix_list.h"
#include "src/hwloc/pmix_hwloc.h"
#include "src/include/pmix_globals.h"
#include "src/include/pmix_socket_errno.h"
#include "src/mca/base/pmix_mca_base_var.h"
#include "src/mca/pcompress/pcompress.h"
#include "src/mca/preg/preg.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_name_fns.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_show_help.h"
#include "src/util/pmix_environ.h"

#include "pgpu_intel.h"
#include "src/mca/pgpu/base/base.h"
#include "src/mca/pgpu/pgpu.h"

static pmix_status_t allocate(pmix_namespace_t *nptr, pmix_info_t info[], size_t ninfo,
                              pmix_list_t *ilist);
static pmix_status_t setup_local(pmix_nspace_env_cache_t *ns,
                                 pmix_info_t info[], size_t ninfo);
static pmix_status_t setup_fork(const pmix_proc_t *proc, char ***env);
static pmix_status_t collect_inventory(pmix_info_t directives[], size_t ndirs,
                                       pmix_list_t *inventory);
static pmix_status_t deliver_inventory(pmix_info_t info[], size_t ninfo,
                                       pmix_info_t directives[], size_t ndirs);

pmix_pgpu_module_t pmix_pgpu_intel_module = {
    .name = "intel",
    .allocate = allocate,
    .setup_local = setup_local,
    .setup_fork = setup_fork,
    .collect_inventory = collect_inventory,
    .deliver_inventory = deliver_inventory
};

/* NOTE: if there is any binary data to be transferred, then
 * this function MUST pack it for transport as the host will
 * not know how to do so */
static pmix_status_t allocate(pmix_namespace_t *nptr,
                              pmix_info_t info[], size_t ninfo,
                              pmix_list_t *ilist)
{
    pmix_buffer_t mydata; // Buffer used to store information to be transmitted (scratch storage)
    pmix_kval_t *kv;
    pmix_byte_object_t bo;
    bool envars = false;
    pmix_status_t rc;
    pmix_list_t cache;
    size_t n;

    pmix_output_verbose(2, pmix_pgpu_base_framework.framework_output,
                        "pgpu:intel:allocate for nspace %s", nptr->nspace);

    if (NULL == info) {
        return PMIX_ERR_TAKE_NEXT_OPTION;
    }

    for (n = 0; n < ninfo; n++) {
        if (PMIX_CHECK_KEY(&info[n], PMIX_SETUP_APP_ENVARS)) {
            envars = PMIX_INFO_TRUE(&info[n]);
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_SETUP_APP_ALL)) {
            envars = PMIX_INFO_TRUE(&info[n]);
        }
    }
    if (!envars || NULL == pmix_mca_pgpu_intel_component.include) {
        /* nothing for us to harvest.  Declining says so; appending an
         * empty blob would ship one to every daemon in the job to say
         * the same thing */
        return PMIX_ERR_TAKE_NEXT_OPTION;
    }

    /* setup a buffer - we will pack the info into it for transmission to
     * the backend compute node daemons */
    PMIX_CONSTRUCT(&mydata, pmix_buffer_t);

    pmix_output_verbose(2, pmix_pgpu_base_framework.framework_output,
                        "pgpu: intel harvesting envars %s excluding %s",
                        (NULL == pmix_mca_pgpu_intel_component.incparms)
                            ? "NONE" : pmix_mca_pgpu_intel_component.incparms,
                        (NULL == pmix_mca_pgpu_intel_component.excparms)
                            ? "NONE" : pmix_mca_pgpu_intel_component.excparms);

    /* harvest envars to pass along */
    PMIX_CONSTRUCT(&cache, pmix_list_t);
    rc = pmix_util_harvest_envars(pmix_mca_pgpu_intel_component.include,
                                  pmix_mca_pgpu_intel_component.exclude, &cache);
    if (PMIX_SUCCESS != rc) {
        PMIX_LIST_DESTRUCT(&cache);
        PMIX_DESTRUCT(&mydata);
        return rc;
    }
    /* pack anything that was found */
    PMIX_LIST_FOREACH (kv, &cache, pmix_kval_t) {
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, &mydata, &kv->value->data.envar, 1, PMIX_ENVAR);
        if (PMIX_SUCCESS != rc) {
            /* a short blob is not a smaller job - the compute nodes would
             * replay part of the environment and never know the rest was
             * dropped */
            PMIX_ERROR_LOG(rc);
            PMIX_LIST_DESTRUCT(&cache);
            PMIX_DESTRUCT(&mydata);
            return rc;
        }
    }
    PMIX_LIST_DESTRUCT(&cache);

    /* load all our results into a buffer for xmission to the backend */
    PMIX_KVAL_NEW(kv, PMIX_PGPU_INTEL_BLOB);
    if (NULL == kv || NULL == kv->value) {
        PMIX_DESTRUCT(&mydata);
        return PMIX_ERR_NOMEM;
    }
    kv->value->type = PMIX_BYTE_OBJECT;
    PMIX_UNLOAD_BUFFER(&mydata, bo.bytes, bo.size);
    /* to help scalability, compress this blob */
    if (pmix_compress.compress((uint8_t *) bo.bytes, bo.size,
                               (uint8_t **) &kv->value->data.bo.bytes, &kv->value->data.bo.size)) {
        kv->value->type = PMIX_COMPRESSED_BYTE_OBJECT;
        /* the compressed copy now holds the payload - release the
         * uncompressed buffer we unloaded above */
        free(bo.bytes);
    } else {
        kv->value->data.bo.bytes = bo.bytes;
        kv->value->data.bo.size = bo.size;
    }
    PMIX_DESTRUCT(&mydata);
    pmix_list_append(ilist, &kv->super);
    return PMIX_SUCCESS;
}

/* PMIx_server_setup_local_support calls the "setup_local" function.
 * The Standard requires that this come _after_ the host calls the
 * PMIx_server_register_nspace function to ensure that any required information
 * is available to the components. Thus, we have the PMIX_NODE_MAP and
 * PMIX_PROC_MAP available to us and can use them here.
 *
 * When the host calls "setup_local", it passes down an array
 * containing the information the "lead" server (e.g., "mpirun") collected
 * from PMIx_server_setup_application. In this case, we search for a blob
 * that our "allocate" function may have included in that info.
 */
static pmix_status_t setup_local(pmix_nspace_env_cache_t *ns,
                                 pmix_info_t info[], size_t ninfo)
{
    size_t n;
    pmix_buffer_t bkt;
    int32_t cnt;
    pmix_status_t rc = PMIX_SUCCESS;
    uint8_t *data;
    size_t size;
    bool release = false;
    pmix_envar_list_item_t *ev;

    pmix_output_verbose(2, pmix_pgpu_base_framework.framework_output,
                        "pgpu:intel:setup_local with %lu info", (unsigned long) ninfo);

    /* prep the unpack buffer */
    PMIX_CONSTRUCT(&bkt, pmix_buffer_t);

    for (n = 0; n < ninfo; n++) {
        /* look for my key */
        if (PMIX_CHECK_KEY(&info[n], PMIX_PGPU_INTEL_BLOB)) {
            pmix_output_verbose(2, pmix_pgpu_base_framework.framework_output,
                                "pgpu:intel:setup_local found my blob");

            /* if this is a compressed byte object, decompress it */
            if (PMIX_COMPRESSED_BYTE_OBJECT == info[n].value.type) {
                /* this fails on a node that built no compression component
                 * as well as on a damaged blob, and it leaves the output
                 * pointer untouched - so we must not go on to read, or
                 * free, whatever it did not write */
                if (!pmix_compress.decompress(&data, &size,
                                              (uint8_t *) info[n].value.data.bo.bytes,
                                              info[n].value.data.bo.size)) {
                    PMIX_ERROR_LOG(PMIX_ERR_UNPACK_FAILURE);
                    return PMIX_ERR_UNPACK_FAILURE;
                }
                release = true;
            } else {
                data = (uint8_t *) info[n].value.data.bo.bytes;
                size = info[n].value.data.bo.size;
            }
            PMIX_LOAD_BUFFER_NON_DESTRUCT(pmix_globals.mypeer, &bkt, data, size);

            /* all we packed was envars, so just cycle thru */
            ev = PMIX_NEW(pmix_envar_list_item_t);
            cnt = 1;
            PMIX_BFROPS_UNPACK(rc, pmix_globals.mypeer, &bkt, &ev->envar, &cnt, PMIX_ENVAR);
            while (PMIX_SUCCESS == rc) {
                pmix_list_append(&ns->envars, &ev->super);
                /* get the next envar */
                ev = PMIX_NEW(pmix_envar_list_item_t);
                cnt = 1;
                PMIX_BFROPS_UNPACK(rc, pmix_globals.mypeer, &bkt, &ev->envar, &cnt, PMIX_ENVAR);
            }
            // we will have created one more envar than we want
            PMIX_RELEASE(ev);

            /* we are done */
            break;
        }
    }

    /* the unpack loop terminates on the trailing empty read, which is
     * the normal end of the blob - not an error to report to the base */
    if (PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER == rc) {
        rc = PMIX_SUCCESS;
    }

    if (release) {
        free(data);
    }

    return rc;
}

/* Does the child's hierarchy setting enumerate devices the same way the
 * topology's ordinals were numbered under?
 *
 * COMBINED differs from FLAT only in whether the tiles can be navigated
 * back to their card; zeDeviceGet reports the same devices in the same
 * order under both, which is all an ordinal depends on.  So the question
 * is only ever COMPOSITE or not.
 */
static bool hierarchy_agrees(const char *set, const char *ours)
{
    bool set_composite = (0 == strcasecmp(set, "COMPOSITE"));
    bool our_composite = (0 == strcasecmp(ours, "COMPOSITE"));

    return (set_composite == our_composite);
}

/* Name this process's Intel GPUs in ZE_AFFINITY_MASK.
 *
 * This module used to contribute no environment at all, on the grounds
 * that ZE_AFFINITY_MASK takes Level Zero device ordinals - there is no
 * identifier form of the kind CUDA_VISIBLE_DEVICES and ROCR_VISIBLE_DEVICES
 * accept - and that PMIx could not reproduce the runtime's device
 * ordering.  The premise was wrong in one specific way: PMIx does not have
 * to reproduce that ordering, because hwloc's Level Zero backend recorded
 * it.  Each root device carries the driver and device index zeDeviceGet
 * handed back for it, so an ordinal here is read from the enumeration
 * rather than predicted, and it is read from THIS node's topology at the
 * moment of forking on the node that will run the process.
 *
 * What the topology cannot record is the model that enumeration ran under.
 * The spec is explicit that a driver reads ZE_FLAT_DEVICE_HIERARCHY first
 * and then interprets ZE_AFFINITY_MASK against the devices that model
 * exposes, so the same ordinals mean a card under COMPOSITE and a tile
 * under FLAT.  An ordinal is therefore not a complete statement on its
 * own, and this writes the model along with it:
 *
 *   - if the child's environment does not name a model, name the one the
 *     ordinals were numbered under.  Without this the process would take
 *     whatever its own Level Zero runtime defaults to, which need not be
 *     the default the daemon's did - the two are routinely different
 *     builds, and in a container they are certainly different installs.
 *   - if it does name one, and it disagrees, write nothing and say so.
 *     Overriding an explicit choice would change how many devices the
 *     program sees; writing a mask the process will read under a different
 *     model would silently hand it half the hardware it was assigned.
 *     Neither is ours to do quietly, so the assignment is dropped and the
 *     process keeps every device it can see - which is the same outcome as
 *     before any of this existed.
 *
 * The mask itself is overwritten if already set, for the same reason the
 * other vendor components overwrite theirs: a process asking to be mapped
 * against a device has made the more specific request, and the ordinals
 * come from the topology as seen through any narrowing already in force.
 */
static pmix_status_t setup_fork(const pmix_proc_t *proc, char ***env)
{
    pmix_status_t rc;
    char *mask = NULL, *mode = NULL, *set;

    rc = pmix_pgpu_base_get_visible_devices(proc, "INTEL", &mask);
    if (PMIX_SUCCESS != rc) {
        /* nothing to say about this process - the ordinary case */
        return rc;
    }

    rc = pmix_hwloc_levelzero_hierarchy(&pmix_globals.topology, &mode);
    if (PMIX_SUCCESS == rc && NULL != mode) {
        set = pmix_getenv("ZE_FLAT_DEVICE_HIERARCHY", *env);
        if (NULL == set) {
            rc = PMIx_Setenv("ZE_FLAT_DEVICE_HIERARCHY", mode, false, env);
            if (PMIX_SUCCESS != rc) {
                /* the mask below means a card under one model and a tile
                 * under the other, so writing it without the model it was
                 * numbered against is worse than writing neither */
                PMIX_ERROR_LOG(rc);
                free(mask);
                free(mode);
                return rc;
            }
        } else if (!hierarchy_agrees(set, mode)) {
            pmix_show_help("help-pgpu-intel.txt", "hierarchy-mismatch", true,
                           PMIX_NAME_PRINT(proc), pmix_globals.hostname,
                           set, mode, mask);
            free(mask);
            free(mode);
            return PMIX_ERR_TAKE_NEXT_OPTION;
        }
    }
    if (NULL != mode) {
        free(mode);
    }

    pmix_output_verbose(2, pmix_pgpu_base_framework.framework_output,
                        "pgpu:intel: ZE_AFFINITY_MASK=%s for %s",
                        mask, PMIX_NAME_PRINT(proc));
    /* setting this is the whole job: reporting success without having done
     * so launches a process that sees every device on the node instead of
     * the ones it was assigned */
    rc = PMIx_Setenv("ZE_AFFINITY_MASK", mask, true, env);
    free(mask);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
    }
    return rc;
}

static pmix_status_t collect_inventory(pmix_info_t directives[], size_t ndirs,
                                       pmix_list_t *inventory)
{
    /* search the topology for intel GPUs */
    PMIX_HIDE_UNUSED_PARAMS(directives, ndirs, inventory);

    return PMIX_SUCCESS;
}

static pmix_status_t deliver_inventory(pmix_info_t info[], size_t ninfo,
                                       pmix_info_t directives[], size_t ndirs)
{
    /* look for our inventory blob */
    PMIX_HIDE_UNUSED_PARAMS(info, ninfo, directives, ndirs);

    return PMIX_SUCCESS;
}
