/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2013      Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2016-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021-2022 Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "src/include/pmix_config.h"

#include <stdio.h>
#include <string.h>

#include "src/class/pmix_hash_table.h"
#include "src/class/pmix_list.h"
#include "src/include/pmix_globals.h"
#include "src/mca/base/pmix_base.h"
#include "src/mca/base/pmix_mca_base_vari.h"
#include "src/mca/mca.h"
#include "src/util/pmix_keyval_parse.h"

/* The list a parse fills, and an index of it by name. A name seen
 * again replaces the value it already has, and finding it by walking
 * the list made a parse quadratic in the number of names. */
typedef struct {
    pmix_list_t *list;
    pmix_hash_table_t index;
    bool indexed;
} paramfile_ctx_t;

static void save_value(const char *file, int lineno,
                       const char *name, const char *value,
                       void *cbdata);

/* index what the list already holds - the caller may parse several
 * files into one list. Without an index, save_value walks the list */
static void ctx_setup(paramfile_ctx_t *ctx, pmix_list_t *list)
{
    pmix_mca_base_var_file_value_t *fv;

    ctx->list = list;
    PMIX_CONSTRUCT(&ctx->index, pmix_hash_table_t);
    ctx->indexed = (PMIX_SUCCESS == pmix_hash_table_init(&ctx->index, 64));
    PMIX_LIST_FOREACH (fv, list, pmix_mca_base_var_file_value_t) {
        if (!ctx->indexed) {
            break;
        }
        if (PMIX_SUCCESS != pmix_hash_table_set_value_ptr(&ctx->index, fv->mbvfv_var,
                                                          strlen(fv->mbvfv_var), fv)) {
            ctx->indexed = false;
        }
    }
}

int pmix_mca_base_parse_paramfile(const char *paramfile, pmix_list_t *list)
{
    paramfile_ctx_t ctx;
    int rc;

    ctx_setup(&ctx, list);
    rc = pmix_util_keyval_parse(paramfile, save_value, &ctx);
    PMIX_DESTRUCT(&ctx.index);
    return rc;
}

int pmix_mca_base_parse_paramfile_stream(FILE *fp, const char *paramfile, size_t maxbytes,
                                         pmix_list_t *list)
{
    paramfile_ctx_t ctx;
    int rc;

    ctx_setup(&ctx, list);
    rc = pmix_util_keyval_parse_stream(fp, paramfile, maxbytes, save_value, &ctx);
    PMIX_DESTRUCT(&ctx.index);
    return rc;
}

int pmix_mca_base_internal_env_store(pmix_list_t *list)
{
    paramfile_ctx_t ctx;
    int rc;

    ctx_setup(&ctx, list);
    rc = pmix_util_keyval_save_internal_envars(save_value, &ctx);
    PMIX_DESTRUCT(&ctx.index);
    return rc;
}

/* the parser hands us everything we need - the target list rides
 * along in cbdata, and the origin of the pair comes in as file/lineno
 * - so this callback touches no state shared with its caller and
 * requires no serialization of its own */
static void save_value(const char *file, int lineno,
                       const char *name, const char *value,
                       void *cbdata)
{
    paramfile_ctx_t *ctx = (paramfile_ctx_t *) cbdata;
    pmix_mca_base_var_file_value_t *fv = NULL;
    void *ptr;

    /* If we already have a param of this name, just replace the
       value. */
    if (ctx->indexed) {
        if (PMIX_SUCCESS == pmix_hash_table_get_value_ptr(&ctx->index, name,
                                                          strlen(name), &ptr)) {
            fv = (pmix_mca_base_var_file_value_t *) ptr;
        }
    } else {
        PMIX_LIST_FOREACH (fv, ctx->list, pmix_mca_base_var_file_value_t) {
            if (0 == strcmp(name, fv->mbvfv_var)) {
                break;
            }
        }
        if (pmix_list_get_end(ctx->list) == &fv->super) {
            fv = NULL;
        }
    }

    if (NULL != fv) {
        if (NULL != fv->mbvfv_value) {
            free(fv->mbvfv_value);
        }
    } else {
        /* We didn't already have the param, so append it to the list */
        fv = PMIX_NEW(pmix_mca_base_var_file_value_t);
        if (NULL == fv) {
            return;
        }

        fv->mbvfv_var = strdup(name);
        pmix_list_append(ctx->list, &fv->super);
        /* a name the index could not take is found by walking the list
         * from now on */
        if (ctx->indexed && (NULL == fv->mbvfv_var ||
            PMIX_SUCCESS != pmix_hash_table_set_value_ptr(&ctx->index, fv->mbvfv_var,
                                                          strlen(fv->mbvfv_var), fv))) {
            ctx->indexed = false;
        }
    }

    fv->mbvfv_value = value ? strdup(value) : NULL;
    /* the file name is not ours to own - it belongs to the caller that
     * asked for the file to be parsed, and outlives this list */
    fv->mbvfv_file = (char *) file;
    fv->mbvfv_lineno = lineno;
}
