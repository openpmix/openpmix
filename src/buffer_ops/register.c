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
 * Copyright (c) 2012      Los Alamos National Security, Inc.  All rights reserved. 
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "pmix_config.h"

#include "src/buffer_ops/internal.h"

int pmix_bfrop_register(pmix_bfrop_pack_fn_t pack_fn,
                      pmix_bfrop_unpack_fn_t unpack_fn,
                      pmix_bfrop_copy_fn_t copy_fn,
                      pmix_bfrop_compare_fn_t compare_fn,
                      pmix_bfrop_print_fn_t print_fn,
                      bool structured,
                      const char *name, pmix_data_type_t *type)
{
    pmix_bfrop_type_info_t *info, *ptr;
    int32_t i;

    /* Check for bozo cases */

    if (NULL == pack_fn || NULL == unpack_fn || NULL == copy_fn || NULL == compare_fn ||
        NULL == print_fn || NULL == name || NULL == type) {
        return PMIX_ERR_BAD_PARAM;
    }

    /* check if this entry already exists - if so, error - we do NOT allow multiple type registrations */
    for (i=0; i < pmix_pointer_array_get_size(&pmix_bfrop_types); i++) {
        ptr = pmix_pointer_array_get_item(&pmix_bfrop_types, i);
        if (NULL != ptr) {
            /* check if the name exists */
            if (0 == strcmp(ptr->odti_name, name)) {
                return PMIX_ERR_DATA_TYPE_REDEF;
            }
            /* check if the specified type exists */
            if (*type > 0 && ptr->odti_type == *type) {
                return PMIX_ERR_DATA_TYPE_REDEF;
            }
        }
    }

    /* if type is given (i.e., *type > 0), then just use it.
     * otherwise, it is an error
     */
    if (0 >= *type) {
        return PMIX_ERR_BAD_PARAM;
    }

    /* Add a new entry to the table */
    info = (pmix_bfrop_type_info_t*) OBJ_NEW(pmix_bfrop_type_info_t);
    if (NULL == info) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    }
    info->odti_type = *type;
    info->odti_name = strdup(name);
    info->odti_pack_fn = pack_fn;
    info->odti_unpack_fn = unpack_fn;
    info->odti_copy_fn = copy_fn;
    info->odti_compare_fn = compare_fn;
    info->odti_print_fn = print_fn;
    info->odti_structured = structured;
    
    return pmix_pointer_array_set_item(&pmix_bfrop_types, *type, info);
}
