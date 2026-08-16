/*
 * Copyright (c) 2015      Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2016      IBM Corporation.  All rights reserved.
 * Copyright (c) 2017-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "src/include/pmix_config.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "pmix_common.h"
#include "src/mca/pdl/pdl.h"

#include "pdl_libltdl.h"

static int plibltdl_open(const char *fname, bool use_ext, bool private_namespace,
                         pmix_pdl_handle_t **handle, char **err_msg)
{
    assert(handle);

    *handle = NULL;
    if (NULL != err_msg) {
        *err_msg = NULL;
    }

    lt_dlhandle local_handle = NULL;

#if PMIX_PDL_PLIBLTDL_HAVE_LT_DLADVISE
    pmix_pdl_plibltdl_component_t *c = &pmix_mca_pdl_plibltdl_component;

    if (use_ext && private_namespace) {
        local_handle = lt_dlopenadvise(fname, c->advise_private_ext);
    } else if (use_ext && !private_namespace) {
        local_handle = lt_dlopenadvise(fname, c->advise_public_ext);
    } else if (!use_ext && private_namespace) {
        local_handle = lt_dlopenadvise(fname, c->advise_private_noext);
    } else if (!use_ext && !private_namespace) {
        local_handle = lt_dlopenadvise(fname, c->advise_public_noext);
    }
#else
    /* Without lt_dladvise there is no way to say private vs. public, so
       that argument is simply not expressible here. */
    (void) private_namespace;

    if (use_ext) {
        local_handle = lt_dlopenext(fname);
    } else {
        local_handle = lt_dlopen(fname);
    }
#endif

    if (NULL != local_handle) {
        *handle = calloc(1, sizeof(pmix_pdl_handle_t));
        if (NULL == *handle) {
            lt_dlclose(local_handle);
            return PMIX_ERR_OUT_OF_RESOURCE;
        }
        (*handle)->ltdl_handle = local_handle;

#if PMIX_ENABLE_DEBUG
        if (NULL != fname) {
            (*handle)->filename = strdup(fname);
        } else {
            (*handle)->filename = strdup("(null)");
        }
#endif

        return PMIX_SUCCESS;
    }

    if (NULL != err_msg) {
        *err_msg = strdup((char *) lt_dlerror());
    }
    return PMIX_ERROR;
}

static int plibltdl_lookup(pmix_pdl_handle_t *handle, const char *symbol, void **ptr,
                           char **err_msg)
{
    assert(handle);
    assert(handle->ltdl_handle);
    assert(symbol);
    assert(ptr);

    if (NULL != err_msg) {
        *err_msg = NULL;
    }

    *ptr = lt_dlsym(handle->ltdl_handle, symbol);
    if (NULL != *ptr) {
        return PMIX_SUCCESS;
    }

    if (NULL != err_msg) {
        *err_msg = strdup((char *) lt_dlerror());
    }
    return PMIX_ERROR;
}

static int plibltdl_close(pmix_pdl_handle_t *handle)
{
    assert(handle);

    int ret;
    ret = lt_dlclose(handle->ltdl_handle);

#if PMIX_ENABLE_DEBUG
    free(handle->filename);
#endif
    free(handle);

    return ret;
}

static int plibltdl_foreachfile(const char *search_path,
                                int (*func)(const char *filename, void *data), void *data)
{
    assert(search_path);
    assert(func);

    int ret = lt_dlforeachfile(search_path, func, data);
    return (0 == ret) ? PMIX_SUCCESS : PMIX_ERROR;
}

/*
 * Module definition
 */
pmix_pdl_base_module_t pmix_pdl_plibltdl_module = {.open = plibltdl_open,
                                                   .lookup = plibltdl_lookup,
                                                   .close = plibltdl_close,
                                                   .foreachfile = plibltdl_foreachfile};
