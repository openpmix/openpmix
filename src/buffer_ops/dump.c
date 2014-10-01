/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University.
 *                         All rights reserved.
 * Copyright (c) 2004-2005 The Trustees of the University of Tennessee.
 *                         All rights reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "pmix_config.h"

#include "src/util/output.h"

#include "src/buffer_ops/internal.h"

int pmix_bfrop_dump(int output_stream, void *src, pmix_data_type_t type)
{
    char *sptr;
    int rc;

    if (PMIX_SUCCESS != (rc = pmix_bfrop.print(&sptr, NULL, src, type))) {
        return rc;
    }

    pmix_output(output_stream, "%s", sptr);
    free(sptr);

    return PMIX_SUCCESS;
}


void pmix_bfrop_dump_data_types(int output)
{
    pmix_bfrop_type_info_t *ptr;
    pmix_data_type_t j;
    int32_t i;

    pmix_output(output, "DUMP OF REGISTERED DATA TYPES");

    j = 0;
    for (i=0; i < pmix_pointer_array_get_size(&pmix_bfrop_types); i++) {
        ptr = pmix_pointer_array_get_item(&pmix_bfrop_types, i);
        if (NULL != ptr) {
            j++;
            /* print out the info */
            pmix_output(output, "\tIndex: %lu\tData type: %lu\tName: %s",
                        (unsigned long)j,
                        (unsigned long)ptr->odti_type,
                        ptr->odti_name);
        }
    }
}

