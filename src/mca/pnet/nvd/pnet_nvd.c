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

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pmix_common.h"

#include "src/class/pmix_list.h"
#include "src/hwloc/pmix_hwloc.h"
#include "src/include/pmix_globals.h"
#include "src/mca/pcompress/pcompress.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_environ.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_name_fns.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_printf.h"

#include "pnet_nvd.h"
#include "src/mca/pnet/base/base.h"
#include "src/mca/pnet/pnet.h"

static pmix_status_t allocate(pmix_namespace_t *nptr, pmix_info_t info[], size_t ninfo,
                              pmix_list_t *ilist);
static pmix_status_t setup_local_network(pmix_nspace_env_cache_t *nptr,
                                         pmix_info_t info[], size_t ninfo);
static pmix_status_t setup_fork(const pmix_proc_t *proc, char ***env);
static pmix_status_t collect_inventory(pmix_info_t directives[], size_t ndirs,
                                       void *inventory);
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

/* The PCI families this component opens for - see component_open.  A
 * process's assignment is filtered by the same table, because a node can
 * carry more than one fabric and naming somebody else's NIC in these
 * variables is worse than naming none.
 *
 * The variables this component writes belong to libraries - NCCL's IB
 * transport, UCX, libfabric's verbs provider - that drive any InfiniBand
 * or RoCE verbs device, not only NVIDIA's.  So every vendor whose RDMA
 * adapters those libraries accept is an entry here, rather than a
 * component of its own: two components writing UCX_NET_DEVICES for one
 * process would overwrite each other, and whichever ran last would win.
 *
 * A RoCE adapter reports the ordinary ethernet class, 0x200, which every
 * plain ethernet port from the same vendor reports too.  What makes it an
 * RDMA device is the OpenFabrics device hwloc found on the function, so
 * those entries require that type.  Omni-Path's class 0x208 stays with the
 * opa component, and Amazon's EFA - an OpenFabrics device none of these
 * libraries drive - with the efa component. */
const pmix_pnet_pcimatch_t pmix_pnet_nvd_match[] = {
    {0x15b3, 0x207, 0},                         /* Mellanox InfiniBand controller */
    {0x10de, 0x207, 0},                         /* the same under NVIDIA's vendor id */
    {0x15b3, 0x200, PMIX_DEVTYPE_OPENFABRICS},  /* Mellanox ConnectX in ethernet mode (RoCE) */
    {0x10de, 0x200, PMIX_DEVTYPE_OPENFABRICS},  /* the same under NVIDIA's vendor id */
    {0x14e4, 0x200, PMIX_DEVTYPE_OPENFABRICS},  /* Broadcom NetXtreme-E (bnxt_re) */
    {0x8086, 0x200, PMIX_DEVTYPE_OPENFABRICS}   /* Intel E810 / X722 (irdma) */
};
const size_t pmix_pnet_nvd_nmatch = sizeof(pmix_pnet_nvd_match) / sizeof(pmix_pnet_nvd_match[0]);

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

/* Can NCCL and UCX drive this verbs device?
 *
 * Both drive InfiniBand and RoCE, and neither drives iWARP.  Intel's irdma
 * runs either, depending on how the driver was loaded, and an X722 runs
 * iWARP only - and nothing in the topology says which, since hwloc does not
 * record it.  The kernel does, in the device's node_type: "1: CA" for an
 * InfiniBand or RoCE channel adapter, "4: RNIC" for iWARP.  An iWARP device
 * named alone in UCX_NET_DEVICES leaves UCX no device for any transport, so
 * it fails rather than falling back; it is therefore left out.
 *
 * This runs on the node that will fork the process, so the file read is
 * the one describing the device the process will see.  A device whose
 * file cannot be read is left out too: on Linux it always exists, and
 * nowhere else does any of this hardware.  The root of the tree is a
 * parameter only so the check can be run against a captured one. */
static bool is_channel_adapter(const char *name)
{
    char path[PMIX_PATH_MAX], buf[32];
    const char *root = pmix_mca_pnet_nvd_component.sysfs_root;
    FILE *fp;
    char *end;
    long type;
    int len;

    /* the name becomes a path component, so it must be one */
    if (NULL == name || '\0' == name[0] || NULL != strchr(name, '/')
        || 0 == strcmp(name, ".") || 0 == strcmp(name, "..")) {
        return false;
    }
    if (NULL == root || '\0' == root[0]) {
        root = "/sys";
    }
    len = snprintf(path, sizeof(path), "%s/class/infiniband/%s/node_type", root, name);
    if (0 > len || (size_t) len >= sizeof(path)) {
        return false;
    }
    fp = fopen(path, "r");
    if (NULL == fp) {
        return false;
    }
    if (NULL == fgets(buf, sizeof(buf), fp)) {
        fclose(fp);
        return false;
    }
    fclose(fp);
    errno = 0;
    type = strtol(buf, &end, 10);
    if (end == buf || 0 != errno) {
        return false;
    }
    return (1 == type);
}

/* Would FI_VERBS_DEVICE_NAME select a device this process was not given?
 *
 * libfabric's verbs provider treats each entry of that variable as a
 * *prefix*, comparing it with strncasecmp over the entry's own length, so
 * "mlx5_1" also selects mlx5_10 through mlx5_19, and no spelling of the
 * entry anchors it.  The variable can therefore be written exactly only
 * when no other verbs device on this node begins with an assigned name.
 * The devices the provider sees are the ones under /sys/class/infiniband,
 * which is where hwloc's OpenFabrics devices come from.  If the node's
 * devices cannot be listed, assume the worst. */
static bool verbs_prefix_collides(char **names)
{
    pmix_hwloc_device_t *devs = NULL;
    size_t ndevs = 0, d;
    const char *osname;
    bool assigned, hit = false;
    pmix_status_t rc;
    int n;

    rc = pmix_hwloc_get_devices(&pmix_globals.topology, pmix_globals.hostname,
                                PMIX_DEVTYPE_OPENFABRICS, NULL, &devs, &ndevs);
    if (PMIX_SUCCESS != rc) {
        return true;
    }
    for (d = 0; d < ndevs && !hit; d++) {
        osname = devs[d].dev.osname;
        if (NULL == osname) {
            continue;
        }
        /* a device the process was given is meant to be selected */
        assigned = false;
        for (n = 0; NULL != names[n]; n++) {
            if (0 == strcasecmp(osname, names[n])) {
                assigned = true;
                break;
            }
        }
        if (assigned) {
            continue;
        }
        for (n = 0; NULL != names[n]; n++) {
            if (0 == strncasecmp(osname, names[n], strlen(names[n]))) {
                pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                                    "pnet:nvd: not setting FI_VERBS_DEVICE_NAME - "
                                    "%s would also select %s", names[n], osname);
                hit = true;
                break;
            }
        }
    }
    pmix_hwloc_release_devices(devs, ndevs);
    return hit;
}

/* Name this process's InfiniBand and RoCE adapters to the libraries that
 * will use them.
 *
 * All three variables are overwritten if already set.  A process mapped
 * against a device has made the more specific request, and the names
 * written here come from the topology as this daemon sees it - so where a
 * resource manager has already narrowed what the node presents, they are a
 * subset of what is visible and naming a subset composes correctly.
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
 *
 * libfabric's verbs provider takes the same names in FI_VERBS_DEVICE_NAME,
 * but as prefixes - see verbs_prefix_collides().
 */
static pmix_status_t setup_fork(const pmix_proc_t *proc, char ***env)
{
    pmix_status_t rc;
    char *val = NULL, *ucx = NULL, *port = NULL;
    char **all = NULL, **devs = NULL, **globs = NULL;
    int n;

    rc = pmix_pnet_base_get_assigned_devices(proc, pmix_pnet_nvd_match,
                                             pmix_pnet_nvd_nmatch, &val);
    if (PMIX_SUCCESS != rc) {
        /* this process was not mapped against a NIC of ours - the ordinary
         * case, and not something to report */
        return rc;
    }
    all = PMIx_Argv_split(val, ',');
    free(val);
    val = NULL;
    if (NULL == all) {
        return PMIX_ERR_NOMEM;
    }
    for (n = 0; NULL != all[n]; n++) {
        if (is_channel_adapter(all[n])) {
            PMIx_Argv_append_nosize(&devs, all[n]);
        } else {
            pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                                "pnet:nvd: leaving %s out for %s - not an InfiniBand "
                                "or RoCE adapter (iWARP, or its node type unreadable)",
                                all[n], PMIX_NAME_PRINT(proc));
        }
    }
    PMIx_Argv_free(all);
    if (NULL == devs) {
        /* nothing left that these libraries can drive */
        return PMIX_ERR_TAKE_NEXT_OPTION;
    }
    val = PMIx_Argv_join(devs, ',');
    if (NULL == val) {
        PMIx_Argv_free(devs);
        return PMIX_ERR_NOMEM;
    }

    pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                        "pnet:nvd: assigning %s to %s", val, PMIX_NAME_PRINT(proc));

    /* setting these is the whole job: reporting success without having
     * done so launches the process pointed at nothing in particular */
    rc = PMIx_Setenv("NCCL_IB_HCA", val, true, env);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto done;
    }

    if (!verbs_prefix_collides(devs)) {
        rc = PMIx_Setenv("FI_VERBS_DEVICE_NAME", val, true, env);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto done;
        }
    }

    for (n = 0; NULL != devs[n]; n++) {
        pmix_asprintf(&port, "%s:*", devs[n]);
        if (NULL == port) {
            rc = PMIX_ERR_NOMEM;
            goto done;
        }
        PMIx_Argv_append_nosize(&globs, port);
        free(port);
        port = NULL;
    }
    ucx = PMIx_Argv_join(globs, ',');
    if (NULL == ucx) {
        rc = PMIX_ERR_NOMEM;
        goto done;
    }
    rc = PMIx_Setenv("UCX_NET_DEVICES", ucx, true, env);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
    }

done:
    free(ucx);
    free(val);
    PMIx_Argv_free(globs);
    PMIx_Argv_free(devs);
    return rc;
}

static pmix_status_t collect_inventory(pmix_info_t directives[], size_t ndirs,
                                       void *inventory)
{
    PMIX_HIDE_UNUSED_PARAMS(directives, ndirs, inventory);

    /* Search the topology for one of our NICs, asking the same question
     * component_open asked of the same helper so the two cannot drift.
     * Nothing is added to the inventory list yet.
     *
     * "None here" is reported as PMIX_SUCCESS on purpose: unlike allocate
     * and setup_fork, this call has no decline convention - the base logs
     * any error and abandons the fan-out - so a component with nothing to
     * say has to say it quietly. */
    if (PMIX_SUCCESS == pmix_pnet_base_check_devices(pmix_pnet_nvd_match,
                                                     pmix_pnet_nvd_nmatch)) {
        /* add this to the inventory */
    }
    return PMIX_SUCCESS;
}

static pmix_status_t deliver_inventory(pmix_info_t info[], size_t ninfo,
                                       pmix_info_t directives[], size_t ndirs)
{
    /* look for our inventory blob */

    PMIX_HIDE_UNUSED_PARAMS(info, ninfo, directives, ndirs);

    return PMIX_SUCCESS;
}
