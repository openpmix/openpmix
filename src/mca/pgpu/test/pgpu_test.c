/*
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2016      IBM Corporation.  All rights reserved.
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

#include "pmix_common.h"

#include "src/class/pmix_list.h"
#include "src/include/pmix_globals.h"
#include "src/mca/pcompress/pcompress.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_environ.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_output.h"

#include "pgpu_test.h"
#include "src/mca/pgpu/base/base.h"
#include "src/mca/pgpu/pgpu.h"

static pmix_status_t allocate(pmix_namespace_t *nptr, pmix_info_t info[], size_t ninfo,
                              pmix_list_t *ilist);
static pmix_status_t setup_local(pmix_nspace_env_cache_t *ns,
                                 pmix_info_t info[], size_t ninfo);

pmix_pgpu_module_t pmix_pgpu_test_module = {
    .name = "test",
    .allocate = allocate,
    .setup_local = setup_local
};

/* The scheduler/lead server calls "allocate" from within
 * PMIx_server_setup_application. We harvest the configured envars from
 * our own environment and pack them into a blob appended to ilist for
 * transport to the compute-node daemons.
 *
 * NOTE: if there is any binary data to be transferred, then this
 * function MUST pack it for transport as the host will not know how to
 * do so. */
static pmix_status_t allocate(pmix_namespace_t *nptr,
                              pmix_info_t info[], size_t ninfo,
                              pmix_list_t *ilist)
{
    pmix_buffer_t mydata;
    pmix_kval_t *kv;
    pmix_byte_object_t bo;
    bool envars = false;
    pmix_status_t rc;
    pmix_list_t cache;
    size_t n;

    pmix_output_verbose(2, pmix_pgpu_base_framework.framework_output,
                        "pgpu:test:allocate for nspace %s", nptr->nspace);

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
    if (!envars) {
        /* nothing for us to do */
        return PMIX_ERR_TAKE_NEXT_OPTION;
    }

    /* setup a buffer - we will pack the info into it for transmission to
     * the backend compute node daemons */
    PMIX_CONSTRUCT(&mydata, pmix_buffer_t);

    pmix_output_verbose(2, pmix_pgpu_base_framework.framework_output,
                        "pgpu: test harvesting envars %s excluding %s",
                        (NULL == pmix_mca_pgpu_test_component.incparms)
                            ? "NONE" : pmix_mca_pgpu_test_component.incparms,
                        (NULL == pmix_mca_pgpu_test_component.excparms)
                            ? "NONE" : pmix_mca_pgpu_test_component.excparms);

    /* harvest envars to pass along */
    PMIX_CONSTRUCT(&cache, pmix_list_t);
    if (NULL != pmix_mca_pgpu_test_component.include) {
        rc = pmix_util_harvest_envars(pmix_mca_pgpu_test_component.include,
                                      pmix_mca_pgpu_test_component.exclude, &cache);
        if (PMIX_SUCCESS != rc) {
            PMIX_LIST_DESTRUCT(&cache);
            PMIX_DESTRUCT(&mydata);
            return rc;
        }
        /* pack anything that was found */
        PMIX_LIST_FOREACH (kv, &cache, pmix_kval_t) {
            PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, &mydata, &kv->value->data.envar, 1, PMIX_ENVAR);
        }
    }
    PMIX_LIST_DESTRUCT(&cache);

    /* load all our results into a buffer for xmission to the backend */
    PMIX_KVAL_NEW(kv, PMIX_PGPU_TEST_BLOB);
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
        free(bo.bytes);
    } else {
        kv->value->data.bo.bytes = bo.bytes;
        kv->value->data.bo.size = bo.size;
    }
    PMIX_DESTRUCT(&mydata);
    pmix_list_append(ilist, &kv->super);
    return PMIX_SUCCESS;
}

/* PMIx_server_setup_local_support calls the "setup_local" function on
 * each compute node. We search the incoming info array for the blob our
 * "allocate" produced, unpack the harvested envars, and cache them on
 * the namespace's envar list so the base setup_fork injects them into
 * each child. */
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
                        "pgpu:test:setup_local with %lu info", (unsigned long) ninfo);

    /* prep the unpack buffer */
    PMIX_CONSTRUCT(&bkt, pmix_buffer_t);

    for (n = 0; n < ninfo; n++) {
        /* look for my key */
        if (PMIX_CHECK_KEY(&info[n], PMIX_PGPU_TEST_BLOB)) {
            pmix_output_verbose(2, pmix_pgpu_base_framework.framework_output,
                                "pgpu:test:setup_local found my blob");

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

    /* the unpack loop stops on the trailing empty read, which is normal */
    if (PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER == rc) {
        rc = PMIX_SUCCESS;
    }

    /* NB: bkt was loaded NON_DESTRUCT - it only borrows the info blob's
     * bytes, which remain owned by the caller's info array. We must NOT
     * PMIX_DESTRUCT(&bkt) here or we would free that borrowed buffer and
     * leave the caller with a double free. */
    if (release) {
        free(data);
    }

    return rc;
}
