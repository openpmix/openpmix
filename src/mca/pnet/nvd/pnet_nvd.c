/*
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2016      IBM Corporation.  All rights reserved.
 *
 * Copyright (c) 2021-2026 Nanook Consulting.  All rights reserved.
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
#include "src/util/pmix_alfg.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_name_fns.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_printf.h"
#include "src/util/pmix_environ.h"

#include "pnet_nvd.h"
#include "src/mca/pnet/base/base.h"
#include "src/mca/pnet/pnet.h"

static pmix_status_t allocate(pmix_namespace_t *nptr, pmix_info_t info[], size_t ninfo,
                              pmix_list_t *ilist);
static pmix_status_t setup_local_network(pmix_nspace_env_cache_t *nptr,
                                         pmix_info_t info[], size_t ninfo);
static pmix_status_t setup_fork(const pmix_proc_t *proc, char ***env);
static pmix_status_t collect_inventory(pmix_info_t directives[], size_t ndirs,
                                       pmix_list_t *inventory);
static pmix_status_t deliver_inventory(pmix_info_t info[], size_t ninfo,
                                       pmix_info_t directives[], size_t ndirs);
pmix_pnet_module_t pmix_pnet_nvd_module = {
    .name = "nvd",
    .allocate = allocate,
    .setup_local_network = setup_local_network,
    .setup_fork = setup_fork,
    .collect_inventory = collect_inventory,
    .deliver_inventory = deliver_inventory
};

/* The PCI families this component opened for - see component_open.  A
 * process's assignment is filtered by the same pair, because a node can
 * carry more than one fabric and naming somebody else's NIC in these
 * variables is worse than naming none. */
static const pmix_pnet_pcimatch_t mymatch[] = {
    {0x15b3, 0x207},        /* Mellanox InfiniBand controller */
    {0x10de, 0x207}         /* the same under NVIDIA's vendor id */
};

/* NOTE: if there is any binary data to be transferred, then
 * this function MUST pack it for transport as the host will
 * not know how to do so */
static pmix_status_t allocate(pmix_namespace_t *nptr, pmix_info_t info[], size_t ninfo,
                              pmix_list_t *ilist)
{
    pmix_buffer_t mydata; // Buffer used to store information to be transmitted (scratch storage)
    pmix_kval_t *kv;
    pmix_byte_object_t bo;
    bool envars = false;
    pmix_status_t rc;
    pmix_list_t cache;
    size_t n;

    pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                        "pnet:nvd:allocate for nspace %s", nptr->nspace);

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
    /* setup a buffer - we will pack the info into it for transmission to
     * the backend compute node daemons */
    PMIX_CONSTRUCT(&mydata, pmix_buffer_t);

    if (envars) {
        pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                            "pnet: nvd harvesting envars %s excluding %s",
                            (NULL == pmix_mca_pnet_nvd_component.incparms)
                            ? "NONE" : pmix_mca_pnet_nvd_component.incparms,
                            (NULL == pmix_mca_pnet_nvd_component.excparms)
                            ? "NONE" : pmix_mca_pnet_nvd_component.excparms);
        /* harvest envars to pass along */
        PMIX_CONSTRUCT(&cache, pmix_list_t);
        if (NULL != pmix_mca_pnet_nvd_component.include) {
            rc = pmix_util_harvest_envars(pmix_mca_pnet_nvd_component.include,
                                          pmix_mca_pnet_nvd_component.exclude, &cache);
            if (PMIX_SUCCESS != rc) {
                PMIX_LIST_DESTRUCT(&cache);
                PMIX_DESTRUCT(&mydata);
                return rc;
            }
            /* pack anything that was found */
            PMIX_LIST_FOREACH (kv, &cache, pmix_kval_t) {
                PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, &mydata,
                                 &kv->value->data.envar, 1, PMIX_ENVAR);
                if (PMIX_SUCCESS != rc) {
                    /* a short blob is worse than no blob: the compute node
                     * would set the envars that made it into the buffer and
                     * never learn that the rest were dropped */
                    PMIX_ERROR_LOG(rc);
                    PMIX_LIST_DESTRUCT(&cache);
                    PMIX_DESTRUCT(&mydata);
                    return rc;
                }
            }
            PMIX_LIST_DESTRUCT(&cache);
        }
    }

    /* load all our results into a buffer for xmission to the backend */
    PMIX_KVAL_NEW(kv, PMIX_PNET_NVD_BLOB);
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

/* PMIx_server_setup_local_support calls the "setup_local_network" function.
 * The Standard requires that this come _after_ the host calls the
 * PMIx_server_register_nspace function to ensure that any required information
 * is available to the components. Thus, we have the PMIX_NODE_MAP and
 * PMIX_PROC_MAP available to us and can use them here.
 *
 * When the host calls "setup_local_support", it passes down an array
 * containing the information the "lead" server (e.g., "mpirun") collected
 * from PMIx_server_setup_application. In this case, we search for a blob
 * that our "allocate" function may have included in that info.
 */
static pmix_status_t setup_local_network(pmix_nspace_env_cache_t *ns,
                                         pmix_info_t info[], size_t ninfo)
{
    size_t n;
    pmix_buffer_t bkt;
    int32_t cnt;
    pmix_status_t rc = PMIX_SUCCESS;
    uint8_t *data = NULL;
    size_t size = 0;
    bool release = false;
    pmix_envar_list_item_t *ev;

    pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                        "pnet:nvd:setup_local with %lu info", (unsigned long) ninfo);

    /* prep the unpack buffer */
    PMIX_CONSTRUCT(&bkt, pmix_buffer_t);

    for (n = 0; n < ninfo; n++) {
        /* look for my key */
        if (PMIX_CHECK_KEY(&info[n], PMIX_PNET_NVD_BLOB)) {
            pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                                "pnet:nvd:setup_local found my blob");

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
            if (PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER == rc) {
                /* the loop can only end by running out of envars, so this
                 * is what success looks like.  Returning it would tell the
                 * base that local setup failed, which stops every pnet
                 * component behind us from being called at all */
                rc = PMIX_SUCCESS;
            }
            // we will have created one more envar than we want
            PMIX_RELEASE(ev);

            /* we are done */
            break;
        }
    }

    if (release) {
        free(data);
    }

    return rc;
}

/* Name this process's Mellanox NICs to the libraries that will use them.
 *
 * Both variables are overwritten if already set.  A process mapped against
 * a device has made the more specific request, and the names written here
 * come from the topology as this daemon sees it - so where a resource
 * manager has already narrowed what the node presents, they are a subset of
 * what is visible and naming a subset composes correctly.
 *
 * NCCL matches an HCA by name, so the selector - the OS device name - goes
 * in as it stands.
 *
 * UCX names a device *port*, "mlx5_0:1", and matches each entry of
 * UCX_NET_DEVICES as a glob, so "mlx5_0:*" is this card whatever ports it
 * turns out to have and without PMIx having to choose one.  The ":" is what
 * makes that safe: "mlx5_0*" would also match "mlx5_01" and "mlx5_10" on a
 * node with enough cards, which is precisely the silent over-selection
 * these variables exist to prevent.
 */
static pmix_status_t setup_fork(const pmix_proc_t *proc, char ***env)
{
    pmix_status_t rc;
    char *val = NULL, *ucx = NULL, *port = NULL;
    char **devs = NULL, **globs = NULL;
    int n;

    rc = pmix_pnet_base_get_assigned_devices(proc, mymatch,
                                             sizeof(mymatch) / sizeof(mymatch[0]),
                                             &val);
    if (PMIX_SUCCESS != rc) {
        /* this process was not mapped against a NIC of ours - the ordinary
         * case, and not something to report */
        return rc;
    }

    pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                        "pnet:nvd: assigning %s to %s", val, PMIX_NAME_PRINT(proc));

    /* setting these is the whole job: reporting success without having
     * done so launches the process pointed at nothing in particular */
    rc = PMIx_Setenv("NCCL_IB_HCA", val, true, env);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        free(val);
        return rc;
    }

    devs = PMIx_Argv_split(val, ',');
    if (NULL == devs) {
        free(val);
        return PMIX_ERR_NOMEM;
    }
    for (n = 0; NULL != devs[n]; n++) {
        pmix_asprintf(&port, "%s:*", devs[n]);
        if (NULL == port) {
            PMIx_Argv_free(devs);
            PMIx_Argv_free(globs);
            free(val);
            return PMIX_ERR_NOMEM;
        }
        PMIx_Argv_append_nosize(&globs, port);
        free(port);
    }
    PMIx_Argv_free(devs);
    ucx = PMIx_Argv_join(globs, ',');
    PMIx_Argv_free(globs);
    free(val);
    if (NULL == ucx) {
        return PMIX_ERR_NOMEM;
    }
    rc = PMIx_Setenv("UCX_NET_DEVICES", ucx, true, env);
    free(ucx);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
    }
    return rc;
}

static pmix_status_t collect_inventory(pmix_info_t directives[], size_t ndirs,
                                       pmix_list_t *inventory)
{
    pmix_status_t rc = PMIX_SUCCESS;

    PMIX_HIDE_UNUSED_PARAMS(directives,ndirs, inventory);

    /* search the topology for Mellanox/NVIDIA NICs */
    hwloc_obj_t device;

    if (NULL == pmix_globals.topology.source ||
        0 != strncasecmp(pmix_globals.topology.source, "hwloc", 5) ||
        NULL == pmix_globals.topology.topology) {
        return PMIX_ERR_NOT_SUPPORTED;
    }

    device = hwloc_get_next_pcidev(pmix_globals.topology.topology, NULL);
    while (NULL != device) {
        if (0x207 == device->attr->pcidev.class_id &&
            (device->attr->pcidev.vendor_id == 0x15b3 ||
             device->attr->pcidev.vendor_id == 0x10de)) {
            /* add this to the inventory */
            return PMIX_SUCCESS;
        }
        device = hwloc_get_next_pcidev(pmix_globals.topology.topology, device);
    }
    return rc;
}

static pmix_status_t deliver_inventory(pmix_info_t info[], size_t ninfo,
                                       pmix_info_t directives[], size_t ndirs)
{
    /* look for our inventory blob */

    PMIX_HIDE_UNUSED_PARAMS(info, ninfo, directives, ndirs);

    return PMIX_SUCCESS;
}
