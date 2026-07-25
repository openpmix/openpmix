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

#include "src/class/pmix_list.h"
#include "src/include/pmix_globals.h"
#include "src/mca/base/pmix_base.h"
#include "src/mca/base/pmix_mca_base_vari.h"
#include "src/mca/mca.h"
#include "src/util/pmix_keyval_parse.h"

static void save_value(const char *file, int lineno,
                       const char *name, const char *value,
                       void *cbdata);

int pmix_mca_base_parse_paramfile(const char *paramfile, pmix_list_t *list)
{
    return pmix_util_keyval_parse(paramfile, save_value, list);
}

int pmix_mca_base_internal_env_store(pmix_list_t *list)
{
    return pmix_util_keyval_save_internal_envars(save_value, list);
}

/* the parser hands us everything we need - the target list rides
 * along in cbdata, and the origin of the pair comes in as file/lineno
 * - so this callback touches no state shared with its caller and
 * requires no serialization of its own */
static void save_value(const char *file, int lineno,
                       const char *name, const char *value,
                       void *cbdata)
{
    pmix_list_t *param_list = (pmix_list_t *) cbdata;
    pmix_mca_base_var_file_value_t *fv;
    bool found = false;

    /* First traverse through the list and ensure that we don't
       already have a param of this name.  If we do, just replace the
       value. */

    PMIX_LIST_FOREACH (fv, param_list, pmix_mca_base_var_file_value_t) {
        if (0 == strcmp(name, fv->mbvfv_var)) {
            if (NULL != fv->mbvfv_value) {
                free(fv->mbvfv_value);
            }
            found = true;
            break;
        }
    }

    if (!found) {
        /* We didn't already have the param, so append it to the list */
        fv = PMIX_NEW(pmix_mca_base_var_file_value_t);
        if (NULL == fv) {
            return;
        }

        fv->mbvfv_var = strdup(name);
        pmix_list_append(param_list, &fv->super);
    }

    fv->mbvfv_value = value ? strdup(value) : NULL;
    /* the file name is not ours to own - it belongs to the caller that
     * asked for the file to be parsed, and outlives this list */
    fv->mbvfv_file = (char *) file;
    fv->mbvfv_lineno = lineno;
}
