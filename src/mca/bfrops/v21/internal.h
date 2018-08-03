/* -*- C -*-
 *
 * Copyright (c) 2004-2007 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2006 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2012      Los Alamos National Security, Inc.  All rights reserved.
 * Copyright (c) 2014-2018 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2016      IBM Corporation.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */
#ifndef PMIX21_BFROP_INTERNAL_H_
#define PMIX21_BFROP_INTERNAL_H_

#include <src/include/pmix_config.h>


#ifdef HAVE_SYS_TIME_H
#include <sys/time.h> /* for struct timeval */
#endif

#include "src/class/pmix_pointer_array.h"

#include "src/mca/bfrops/base/base.h"

#ifdef HAVE_STRING_H
#include <string.h>
#endif

 BEGIN_C_DECLS

/*
 * Implementations of API functions
 */

pmix_status_t pmix21_bfrop_pack(pmix_buffer_t *buffer, const void *src,
                                int32_t num_vals,
                                pmix_data_type_t type);
pmix_status_t pmix21_bfrop_unpack(pmix_buffer_t *buffer, void *dest,
                                  int32_t *max_num_vals,
                                  pmix_data_type_t type);

pmix_status_t pmix21_bfrop_copy(void **dest, void *src, pmix_data_type_t type);

pmix_status_t pmix21_bfrop_print(char **output, char *prefix, void *src, pmix_data_type_t type);

pmix_status_t pmix21_bfrop_copy_payload(pmix_buffer_t *dest, pmix_buffer_t *src);

pmix_status_t pmix21_bfrop_value_xfer(pmix_value_t *p, pmix_value_t *src);

void pmix21_bfrop_value_load(pmix_value_t *v, const void *data,
                            pmix_data_type_t type);

pmix_status_t pmix21_bfrop_value_unload(pmix_value_t *kv,
                                       void **data,
                                       size_t *sz);

pmix_value_cmp_t pmix21_bfrop_value_cmp(pmix_value_t *p,
                                       pmix_value_t *p1);

/*
 * Specialized functions
 */
pmix_status_t pmix21_bfrop_pack_buffer(pmix_buffer_t *buffer, const void *src,
                                       int32_t num_vals, pmix_data_type_t type);

pmix_status_t pmix21_bfrop_unpack_buffer(pmix_buffer_t *buffer, void *dst,
                                         int32_t *num_vals, pmix_data_type_t type);

/*
 * Internal pack functions
 */

/**** DEPRECATED ****/
pmix_status_t pmix21_bfrop_pack_array(pmix_buffer_t *buffer, const void *src,
                                    int32_t num_vals, pmix_data_type_t type);
/********************/

/*
 * Internal unpack functions
 */
/**** DEPRECATED ****/
pmix_status_t pmix21_bfrop_unpack_array(pmix_buffer_t *buffer, void *dest,
                                      int32_t *num_vals, pmix_data_type_t type);
/********************/

/*
 * Internal copy functions
 */

/**** DEPRECATED ****/
pmix_status_t pmix21_bfrop_copy_array(pmix_info_array_t **dest,
                                    pmix_info_array_t *src,
                                    pmix_data_type_t type);

/********************/

/*
 * Internal print functions
 */
/**** DEPRECATED ****/
pmix_status_t pmix21_bfrop_print_array(char **output, char *prefix,
                                     pmix_info_array_t *src,
                                     pmix_data_type_t type);
/********************/

END_C_DECLS

#endif
