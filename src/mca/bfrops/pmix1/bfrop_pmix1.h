/*
 * Copyright (c) 2004-2008 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2006 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2016-2017 Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PMIX_BFROPS_PMIX1_H
#define PMIX_BFROPS_PMIX1_H

#include "src/mca/bfrops/bfrops.h"

BEGIN_C_DECLS

/* the component must be visible data for the linker to find it */
 PMIX_EXPORT extern pmix_bfrops_base_component_t mca_bfrops_pmix1_component;

extern pmix_bfrops_module_t pmix_bfrops_pmix1_module;


/* internal packing routines */
extern pmix_status_t pmix1_pack(pmix_buffer_t *buffer,
                                 const void *src, int num_vals,
                                 pmix_data_type_t type);

extern pmix_status_t pmix1_pack_value(pmix_buffer_t *buffer, const void *src,
                                       int32_t num_vals, pmix_data_type_t type);
extern pmix_status_t pmix1_pack_info(pmix_buffer_t *buffer, const void *src,
                                      int32_t num_vals, pmix_data_type_t type);
extern pmix_status_t pmix1_pack_pdata(pmix_buffer_t *buffer, const void *src,
                                       int32_t num_vals, pmix_data_type_t type);
extern pmix_status_t pmix1_pack_app(pmix_buffer_t *buffer, const void *src,
                                     int32_t num_vals, pmix_data_type_t type);
extern pmix_status_t pmix1_pack_kval(pmix_buffer_t *buffer, const void *src,
                                      int32_t num_vals, pmix_data_type_t type);
extern pmix_status_t pmix1_pack_array(pmix_buffer_t *buffer, const void *src,
                                       int32_t num_vals, pmix_data_type_t type);
extern pmix_status_t pmix1_pack_modex(pmix_buffer_t *buffer, const void *src,
                                       int32_t num_vals, pmix_data_type_t type);
extern pmix_status_t pmix1_pack_persist(pmix_buffer_t *buffer, const void *src,
                                         int32_t num_vals, pmix_data_type_t type);
extern pmix_status_t pmix1_pack_datatype(pmix_buffer_t *buffer, const void *src,
                                          int32_t num_vals, pmix_data_type_t type);
extern pmix_status_t pmix1_pack_scope(pmix_buffer_t *buffer, const void *src,
                                       int32_t num_vals, pmix_data_type_t type);
extern pmix_status_t pmix1_pack_range(pmix_buffer_t *buffer, const void *src,
                                       int32_t num_vals, pmix_data_type_t type);
extern pmix_status_t pmix1_pack_cmd(pmix_buffer_t *buffer, const void *src,
                                     int32_t num_vals, pmix_data_type_t type);
extern pmix_status_t pmix1_pack_info_directives(pmix_buffer_t *buffer, const void *src,
                                                 int32_t num_vals, pmix_data_type_t type);
extern pmix_status_t pmix1_pack_darray(pmix_buffer_t *buffer, const void *src,
                                        int32_t num_vals, pmix_data_type_t type);
extern pmix_status_t pmix1_pack_rank(pmix_buffer_t *buffer, const void *src,
                                      int32_t num_vals, pmix_data_type_t type);
extern pmix_status_t pmix1_pack_unsupported(pmix_buffer_t *buffer, const void *src,
                                             int32_t num_vals, pmix_data_type_t type);

/* internal unpacking routines */
extern pmix_status_t pmix1_unpack(pmix_buffer_t *buffer,
                                   void *dst, int32_t *num_vals,
                                   pmix_data_type_t type);
extern pmix_status_t pmix1_unpack_value(pmix_buffer_t *buffer,
                                         void *dst, int32_t *num_vals,
                                         pmix_data_type_t type);
extern pmix_status_t pmix1_unpack_info(pmix_buffer_t *buffer,
                                        void *dst, int32_t *num_vals,
                                        pmix_data_type_t type);
extern pmix_status_t pmix1_unpack_pdata(pmix_buffer_t *buffer,
                                         void *dst, int32_t *num_vals,
                                         pmix_data_type_t type);
extern pmix_status_t pmix1_unpack_app(pmix_buffer_t *buffer,
                                       void *dst, int32_t *num_vals,
                                       pmix_data_type_t type);
extern pmix_status_t pmix1_unpack_kval(pmix_buffer_t *buffer,
                                        void *dst, int32_t *num_vals,
                                        pmix_data_type_t type);
extern pmix_status_t pmix1_unpack_array(pmix_buffer_t *buffer,
                                         void *dst, int32_t *num_vals,
                                         pmix_data_type_t type);
extern pmix_status_t pmix1_unpack_modex(pmix_buffer_t *buffer,
                                         void *dst, int32_t *num_vals,
                                         pmix_data_type_t type);
extern pmix_status_t pmix1_unpack_persist(pmix_buffer_t *buffer,
                                           void *dst, int32_t *num_vals,
                                           pmix_data_type_t type);
extern pmix_status_t pmix1_unpack_datatype(pmix_buffer_t *buffer,
                                            void *dst, int32_t *num_vals,
                                            pmix_data_type_t type);
extern pmix_status_t pmix1_unpack_scope(pmix_buffer_t *buffer,
                                         void *dst, int32_t *num_vals,
                                         pmix_data_type_t type);
extern pmix_status_t pmix1_unpack_range(pmix_buffer_t *buffer,
                                         void *dst, int32_t *num_vals,
                                         pmix_data_type_t type);
extern pmix_status_t pmix1_unpack_cmd(pmix_buffer_t *buffer,
                                       void *dst, int32_t *num_vals,
                                       pmix_data_type_t type);
extern pmix_status_t pmix1_unpack_info_directives(pmix_buffer_t *buffer,
                                                   void *dst, int32_t *num_vals,
                                                   pmix_data_type_t type);
extern pmix_status_t pmix1_unpack_info_darray(pmix_buffer_t *buffer,
                                               void *dst, int32_t *num_vals,
                                               pmix_data_type_t type);
extern pmix_status_t pmix1_unpack_rank(pmix_buffer_t *buffer,
                                        void *dst, int32_t *num_vals,
                                        pmix_data_type_t type);
extern pmix_status_t pmix1_unpack_unsupported(pmix_buffer_t *buffer,
                                               void *dst, int32_t *num_vals,
                                               pmix_data_type_t type);


/* internal copy routines */
extern pmix_status_t pmix1_copy(void **dest, void *src,
                                 pmix_data_type_t type);
extern pmix_status_t pmix1_std_copy(void **dest, void *src,
                              pmix_data_type_t type);

/* internal print routines */
extern pmix_status_t pmix1_print(char **output, char *prefix,
                                  void *src, pmix_data_type_t type);
extern pmix_status_t pmix1_print_array(char **output, char *prefix,
                                        pmix_info_array_t *src,
                                        pmix_data_type_t type);
extern pmix_status_t pmix1_print_app(char **output, char *prefix,
                                      pmix_app_t *src, pmix_data_type_t type);
extern pmix_status_t pmix1_print_kval(char **output, char *prefix,
                                       pmix_kval_t *src, pmix_data_type_t type);
extern pmix_status_t pmix1_print_info(char **output, char *prefix,
                                       pmix_info_t *src, pmix_data_type_t type);
extern pmix_status_t pmix1_print_pdata(char **output, char *prefix,
                                        pmix_pdata_t *src, pmix_data_type_t type);

END_C_DECLS

#endif /* PMIX_BFROPS_PMIX1_H */
