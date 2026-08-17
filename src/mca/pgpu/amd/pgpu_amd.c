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
#include "src/util/pmix_environ.h"

#include "pgpu_amd.h"
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

pmix_pgpu_module_t pmix_pgpu_amd_module = {
    .name = "amd",
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
                        "pgpu:amd:allocate for nspace %s", nptr->nspace);

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
        pmix_output_verbose(2, pmix_pgpu_base_framework.framework_output,
                            "pgpu: amd harvesting envars %s excluding %s",
                            (NULL == pmix_mca_pgpu_amd_component.incparms)
                                ? "NONE" : pmix_mca_pgpu_amd_component.incparms,
                            (NULL == pmix_mca_pgpu_amd_component.excparms)
                                ? "NONE" : pmix_mca_pgpu_amd_component.excparms);
        /* harvest envars to pass along */
        PMIX_CONSTRUCT(&cache, pmix_list_t);
        if (NULL != pmix_mca_pgpu_amd_component.include) {
            rc = pmix_util_harvest_envars(pmix_mca_pgpu_amd_component.include,
                                          pmix_mca_pgpu_amd_component.exclude, &cache);
            if (PMIX_SUCCESS != rc) {
                PMIX_LIST_DESTRUCT(&cache);
                PMIX_DESTRUCT(&mydata);
                return rc;
            }
            /* pack anything that was found */
            PMIX_LIST_FOREACH (kv, &cache, pmix_kval_t) {
                PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, &mydata, &kv->value->data.envar, 1, PMIX_ENVAR);
            }
            PMIX_LIST_DESTRUCT(&cache);
        }
    }

    /* load all our results into a buffer for xmission to the backend */
    PMIX_KVAL_NEW(kv, PMIX_PGPU_AMD_BLOB);
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
                        "pgpu:amd:setup_local with %lu info", (unsigned long) ninfo);

    /* prep the unpack buffer */
    PMIX_CONSTRUCT(&bkt, pmix_buffer_t);

    for (n = 0; n < ninfo; n++) {
        /* look for my key */
        if (PMIX_CHECK_KEY(&info[n], PMIX_PGPU_AMD_BLOB)) {
            pmix_output_verbose(2, pmix_pgpu_base_framework.framework_output,
                                "pgpu:amd:setup_local found my blob");

            /* if this is a compressed byte object, decompress it */
            if (PMIX_COMPRESSED_BYTE_OBJECT == info[n].value.type) {
                pmix_compress.decompress(&data, &size, (uint8_t *) info[n].value.data.bo.bytes,
                                         info[n].value.data.bo.size);
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

/* Name this process's AMD GPUs in ROCR_VISIBLE_DEVICES.
 *
 * ROCR_VISIBLE_DEVICES accepts the vendor's own device identifier, and
 * that is the only form used here.  The alternative it also accepts is an
 * index into the runtime's own device ordering, which PMIx has no way to
 * reproduce - and a value naming a device that is not there does not
 * error, it silently shrinks the visible set - so an index is never
 * guessed.  Nothing here touches the runtime's device-ordering variable
 * either: the identifier does not depend on it, which is the whole reason
 * for preferring it, and overriding an ordering the user may have chosen
 * deliberately would renumber devices for the rest of their program.
 *
 * The variable is overwritten if already set.  A process asking to be
 * mapped against a device has made the more specific request, and where a
 * resource manager has already narrowed the visible set, the identifiers
 * named here are drawn from the topology as seen through that same
 * narrowing - so they are a subset of what is visible, and naming a subset
 * composes correctly.  An index-based value would not.
 */
static pmix_status_t setup_fork(const pmix_proc_t *proc, char ***env)
{
    return pmix_pgpu_base_set_visible_devices(proc, "AMD",
                                              "ROCR_VISIBLE_DEVICES", env);
}

static pmix_status_t collect_inventory(pmix_info_t directives[], size_t ndirs,
                                       pmix_list_t *inventory)
{
    /* search the topology for AMD GPUs */
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
