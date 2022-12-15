/*
 * Copyright (c) 2022      Nanook Consulting  All rights reserved.
 * Copyright (c) 2022      Triad National Security, LLC. All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "src/include/pmix_config.h"

#include "pmix.h"

#include "src/include/pmix_globals.h"
#include "src/mca/bfrops/base/base.h"
#include "src/mca/bfrops/base/bfrop_base_tma.h"

void PMIx_Load_key(pmix_key_t key, const char *src)
{
    pmix_bfrops_base_load_key_tma(key, src, NULL);
}

bool PMIx_Check_key(const char *key, const char *str)
{
    return pmix_bfrops_base_check_key_tma(key, str, NULL);
}

void PMIx_Load_nspace(pmix_nspace_t nspace, const char *str)
{
    pmix_bfrops_base_load_nspace_tma(nspace, str, NULL);
}

bool PMIx_Check_nspace(const char *nspace1, const char *nspace2)
{
    return pmix_bfrops_base_check_nspace_tma(nspace1, nspace2, NULL);
}

bool PMIx_Nspace_invalid(const char *nspace)
{
    return pmix_bfrops_base_nspace_invalid_tma(nspace, NULL);
}

bool PMIx_Check_reserved_key(const char *key)
{
    return pmix_bfrops_base_check_reserved_key_tma(key, NULL);
}

void PMIx_Load_procid(pmix_proc_t *p,
                      const char *ns,
                      pmix_rank_t rk)
{
    pmix_bfrops_base_load_procid_tma(p, ns, rk, NULL);
}

void PMIx_Xfer_procid(pmix_proc_t *dst,
                      const pmix_proc_t *src)
{
    pmix_bfrops_base_xfer_procid_tma(dst, src, NULL);
}

bool PMIx_Check_procid(const pmix_proc_t *a,
                       const pmix_proc_t *b)
{
    return pmix_bfrops_base_check_procid_tma(a, b, NULL);
}

bool PMIx_Check_rank(pmix_rank_t a,
                     pmix_rank_t b)
{
    return pmix_bfrops_base_check_rank_tma(a, b, NULL);
}

bool PMIx_Procid_invalid(const pmix_proc_t *p)
{
    return pmix_bfrops_base_procid_invalid_tma(p, NULL);
}

int PMIx_Argv_count(char **argv)
{
    return pmix_bfrops_base_argv_count_tma(argv, NULL);
}

pmix_status_t PMIx_Argv_append_nosize(char ***argv, const char *arg)
{
    return pmix_bfrops_base_argv_append_nosize_tma(argv, arg, NULL);
}

pmix_status_t PMIx_Argv_prepend_nosize(char ***argv, const char *arg)
{
    return pmix_bfrops_base_argv_prepend_nosize_tma(argv, arg, NULL);
}

pmix_status_t PMIx_Argv_append_unique_nosize(char ***argv, const char *arg)
{
    return pmix_bfrops_base_argv_append_unique_nosize_tma(argv, arg, NULL);
}

void PMIx_Argv_free(char **argv)
{
    pmix_bfrops_base_argv_free_tma(argv, NULL);
}

char **PMIx_Argv_split_inter(const char *src_string,
                             int delimiter,
                             bool include_empty)
{
    return pmix_bfrops_base_argv_split_inter_tma(src_string, delimiter, include_empty, NULL);
}

char **PMIx_Argv_split_with_empty(const char *src_string, int delimiter)
{
    return pmix_bfrops_base_argv_split_with_empty_tma(src_string, delimiter, NULL);
}

char **PMIx_Argv_split(const char *src_string, int delimiter)
{
    return pmix_bfrops_base_argv_split_tma(src_string, delimiter, NULL);
}

char *PMIx_Argv_join(char **argv, int delimiter)
{
    return pmix_bfrops_base_argv_join_tma(argv, delimiter, NULL);
}

char **PMIx_Argv_copy(char **argv)
{
    return pmix_bfrops_base_argv_copy_tma(argv, NULL);
}

pmix_status_t PMIx_Setenv(const char *name,
                          const char *value,
                          bool overwrite,
                          char ***env)
{
    return pmix_bfrops_base_setenv_tma(name, value, overwrite, env, NULL);
}

void PMIx_Value_construct(pmix_value_t *val)
{
    pmix_bfrops_base_value_construct_tma(val, NULL);
}

void PMIx_Value_destruct(pmix_value_t *val)
{
    pmix_bfrops_base_value_destruct_tma(val, NULL);
}

pmix_value_t* PMIx_Value_create(size_t n)
{
    return pmix_bfrops_base_value_create_tma(n, NULL);
}

void PMIx_Value_free(pmix_value_t *v, size_t n)
{
    pmix_bfrops_base_value_free_tma(v, n, NULL);
}

pmix_boolean_t PMIx_Value_true(const pmix_value_t *value)
{
    return pmix_bfrops_base_value_true_tma(value, NULL);
}

pmix_status_t PMIx_Value_load(pmix_value_t *v,
                              const void *data,
                              pmix_data_type_t type)
{
    return pmix_bfrops_base_value_load_tma(v, data, type, NULL);
}

pmix_status_t PMIx_Value_unload(pmix_value_t *kv,
                                void **data,
                                size_t *sz)
{
    return pmix_bfrops_base_value_unload_tma(kv, data, sz, NULL);
}

pmix_status_t PMIx_Value_xfer(pmix_value_t *dest,
                              const pmix_value_t *src)
{
    return pmix_bfrops_base_value_xfer_tma(dest, src, NULL);
}

pmix_value_cmp_t PMIx_Value_compare(pmix_value_t *v1,
                                    pmix_value_t *v2)
{
    return pmix_bfrops_base_value_compare_tma(v1, v2, NULL);
}

PMIX_EXPORT void PMIx_Info_construct(pmix_info_t *p)
{
    pmix_bfrops_base_info_construct_tma(p, NULL);
}

PMIX_EXPORT void PMIx_Info_destruct(pmix_info_t *p)
{
    pmix_bfrops_base_info_destruct_tma(p, NULL);
}

PMIX_EXPORT pmix_info_t* PMIx_Info_create(size_t n)
{
    return pmix_bfrops_base_info_create_tma(n, NULL);
}

PMIX_EXPORT void PMIx_Info_free(pmix_info_t *p, size_t n)
{
    pmix_bfrops_base_info_free_tma(p, n, NULL);
}

pmix_boolean_t PMIx_Info_true(const pmix_info_t *p)
{
    return pmix_bfrops_base_info_true_tma(p, NULL);
}

pmix_status_t PMIx_Info_load(pmix_info_t *info,
                             const char *key,
                             const void *data,
                             pmix_data_type_t type)
{
    return pmix_bfrops_base_info_load_tma(info, key, data, type, NULL);
}

void PMIx_Info_required(pmix_info_t *p)
{
    pmix_bfrops_base_info_required_tma(p, NULL);
}

bool PMIx_Info_is_required(const pmix_info_t *p)
{
    return pmix_bfrops_base_info_is_required(p, NULL);
}

void PMIx_Info_optional(pmix_info_t *p)
{
    pmix_bfrops_base_info_optional_tma(p, NULL);
}

bool PMIx_Info_is_optional(const pmix_info_t *p)
{
    return pmix_bfrops_base_info_is_optional_tma(p, NULL);
}

void PMIx_Info_processed(pmix_info_t *p)
{
    p->flags |= PMIX_INFO_REQD_PROCESSED;
}

bool PMIx_Info_was_processed(const pmix_info_t *p)
{
    return pmix_bfrops_base_info_was_processed_tma(p, NULL);
}

void PMIx_Info_set_end(pmix_info_t *p)
{
    pmix_bfrops_base_info_set_end_tma(p, NULL);
}

bool PMIx_Info_is_end(const pmix_info_t *p)
{
    return pmix_bfrops_base_info_is_end_tma(p, NULL);
}

void PMIx_Info_qualifier(pmix_info_t *p)
{
    pmix_bfrops_base_info_qualifier_tma(p, NULL);
}

bool PMIx_Info_is_qualifier(const pmix_info_t *p)
{
    return pmix_bfrops_base_info_is_qualifier_tma(p, NULL);
}

void PMIx_Info_persistent(pmix_info_t *p)
{
    pmix_bfrops_base_info_persistent_tma(p, NULL);
}

bool PMIx_Info_is_persistent(const pmix_info_t *p)
{
    return pmix_bfrops_base_info_is_persistent_tma(p, NULL);
}

pmix_status_t PMIx_Info_xfer(pmix_info_t *dest,
                             const pmix_info_t *src)
{
    return pmix_bfrops_base_info_xfer_tma(dest, src, NULL);
}

void PMIx_Coord_construct(pmix_coord_t *m)
{
    pmix_bfrops_base_coord_construct_tma(m, NULL);
}

void PMIx_Coord_destruct(pmix_coord_t *m)
{
    pmix_bfrops_base_coord_destruct_tma(m, NULL);
}

pmix_coord_t* PMIx_Coord_create(size_t dims,
                                size_t number)
{
    return pmix_bfrops_base_coord_create_tma(dims, number, NULL);
}

void PMIx_Coord_free(pmix_coord_t *m, size_t number)
{
    pmix_bfrops_base_coord_free_tma(m, number, NULL);
}

void PMIx_Topology_construct(pmix_topology_t *t)
{
    pmix_bfrops_base_topology_construct_tma(t, NULL);
}

void PMIx_Topology_destruct(pmix_topology_t *t)
{
    pmix_bfrops_base_topology_destruct_tma(t, NULL);
}

pmix_topology_t* PMIx_Topology_create(size_t n)
{
    return pmix_bfrops_base_topology_create_tma(n, NULL);
}

void PMIx_Topology_free(pmix_topology_t *t, size_t n)
{
    pmix_bfrops_base_topology_free_tma(t, n, NULL);
}

void PMIx_Cpuset_construct(pmix_cpuset_t *c)
{
    pmix_bfrops_base_cpuset_construct_tma(c, NULL);
}

void PMIx_Cpuset_destruct(pmix_cpuset_t *c)
{
    pmix_bfrops_base_cpuset_destruct_tma(c, NULL);
}

pmix_cpuset_t* PMIx_Cpuset_create(size_t n)
{
    return pmix_bfrops_base_cpuset_create_tma(n, NULL);
}

void PMIx_Cpuset_free(pmix_cpuset_t *c, size_t n)
{
    pmix_bfrops_base_cpuset_free_tma(c, n, NULL);
}

void PMIx_Geometry_construct(pmix_geometry_t *g)
{
    pmix_bfrops_base_geometry_construct_tma(g, NULL);
}

void PMIx_Geometry_destruct(pmix_geometry_t *g)
{
    pmix_bfrops_base_geometry_destruct_tma(g, NULL);
}

pmix_geometry_t* PMIx_Geometry_create(size_t n)
{
    return pmix_bfrops_base_geometry_create_tma(n, NULL);
}

void PMIx_Geometry_free(pmix_geometry_t *g, size_t n)
{
    pmix_bfrops_base_geometry_free_tma(g, n, NULL);
}

void PMIx_Device_distance_construct(pmix_device_distance_t *d)
{
    pmix_bfrops_base_device_distance_construct_tma(d, NULL);
}

void PMIx_Device_distance_destruct(pmix_device_distance_t *d)
{
    pmix_bfrops_base_device_distance_destruct_tma(d, NULL);
}

pmix_device_distance_t* PMIx_Device_distance_create(size_t n)
{
    return pmix_bfrops_base_device_distance_create_tma(n, NULL);
}

void PMIx_Device_distance_free(pmix_device_distance_t *d, size_t n)
{
    pmix_bfrops_base_device_distance_free_tma(d, n, NULL);
}

void PMIx_Byte_object_construct(pmix_byte_object_t *b)
{
    pmix_bfrops_base_byte_object_construct_tma(b, NULL);
}

void PMIx_Byte_object_destruct(pmix_byte_object_t *b)
{
    pmix_bfrops_base_byte_object_destruct_tma(b, NULL);
}

pmix_byte_object_t* PMIx_Byte_object_create(size_t n)
{
    return pmix_bfrops_base_byte_object_create_tma(n, NULL);
}

void PMIx_Byte_object_free(pmix_byte_object_t *b, size_t n)
{
    pmix_bfrops_base_byte_object_free_tma(b, n, NULL);
}

void PMIx_Endpoint_construct(pmix_endpoint_t *e)
{
    pmix_bfrops_base_endpoint_construct_tma(e, NULL);
}

void PMIx_Endpoint_destruct(pmix_endpoint_t *e)
{
    pmix_bfrops_base_endpoint_destruct_tma(e, NULL);
}

pmix_endpoint_t* PMIx_Endpoint_create(size_t n)
{
    return pmix_bfrops_base_endpoint_create_tma(n, NULL);
}

void PMIx_Endpoint_free(pmix_endpoint_t *e, size_t n)
{
    pmix_bfrops_base_endpoint_free_tma(e, n, NULL);
}

PMIX_EXPORT void PMIx_Envar_construct(pmix_envar_t *e)
{
    pmix_bfrops_base_envar_construct_tma(e, NULL);
}

PMIX_EXPORT void PMIx_Envar_destruct(pmix_envar_t *e)
{
    pmix_bfrops_base_envar_destruct_tma(e, NULL);
}

PMIX_EXPORT pmix_envar_t* PMIx_Envar_create(size_t n)
{
    return pmix_bfrops_base_envar_create_tma(n, NULL);
}

void PMIx_Envar_free(pmix_envar_t *e, size_t n)
{
    pmix_bfrops_base_envar_free_tma(e, n, NULL);
}

void PMIx_Envar_load(pmix_envar_t *e,
                     char *var,
                     char *value,
                     char separator)
{
    pmix_bfrops_base_envar_load_tma(e, var, value, separator, NULL);
}

void PMIx_Data_buffer_construct(pmix_data_buffer_t *b)
{
    pmix_bfrops_base_data_buffer_construct_tma(b, NULL);
}

void PMIx_Data_buffer_destruct(pmix_data_buffer_t *b)
{
    pmix_bfrops_base_data_buffer_destruct_tma(b, NULL);
}

pmix_data_buffer_t* PMIx_Data_buffer_create(void)
{
    return pmix_bfrops_base_data_buffer_create_tma(NULL);
}

void PMIx_Data_buffer_release(pmix_data_buffer_t *b)
{
    pmix_bfrops_base_data_buffer_release_tma(b, NULL);
}

void PMIx_Data_buffer_load(pmix_data_buffer_t *b,
                           char *bytes, size_t sz)
{
    pmix_bfrops_base_data_buffer_load_tma(b, bytes, sz, NULL);
}

void PMIx_Data_buffer_unload(pmix_data_buffer_t *b,
                             char **bytes, size_t *sz)
{
    pmix_bfrops_base_data_buffer_unload_tma(b, bytes, sz, NULL);
}

void PMIx_Proc_construct(pmix_proc_t *p)
{
    pmix_bfrops_base_proc_construct_tma(p, NULL);
}

void PMIx_Proc_destruct(pmix_proc_t *p)
{
    // TODO(skg) Is this correct?
    pmix_bfrops_base_proc_construct_tma(p, NULL);
}

pmix_proc_t* PMIx_Proc_create(size_t n)
{
    return pmix_bfrops_base_proc_create_tma(n, NULL);
}

void PMIx_Proc_free(pmix_proc_t *p, size_t n)
{
    pmix_bfrops_base_proc_free_tma(p, n, NULL);
}

void PMIx_Proc_load(pmix_proc_t *p,
                    char *nspace, pmix_rank_t rank)
{
    pmix_bfrops_base_proc_load_tma(p, nspace, rank, NULL);
}

void PMIx_Multicluster_nspace_construct(pmix_nspace_t target,
                                        pmix_nspace_t cluster,
                                        pmix_nspace_t nspace)
{
    pmix_bfrops_base_multicluster_nspace_construct_tma(target, cluster, nspace, NULL);
}

void PMIx_Multicluster_nspace_parse(pmix_nspace_t target,
                                    pmix_nspace_t cluster,
                                    pmix_nspace_t nspace)
{
    pmix_bfrops_base_multicluster_nspace_parse_tma(target, cluster, nspace, NULL);
}

void PMIx_Proc_info_construct(pmix_proc_info_t *p)
{
    pmix_bfrops_base_proc_info_construct_tma(p, NULL);
}

void PMIx_Proc_info_destruct(pmix_proc_info_t *p)
{
    pmix_bfrops_base_proc_info_destruct_tma(p, NULL);
}

pmix_proc_info_t* PMIx_Proc_info_create(size_t n)
{
    return pmix_bfrops_base_proc_info_create_tma(n, NULL);
}

void PMIx_Proc_info_free(pmix_proc_info_t *p, size_t n)
{
    pmix_bfrops_base_proc_info_free_tma(p, n, NULL);
}

void PMIx_Proc_stats_construct(pmix_proc_stats_t *p)
{
    pmix_bfrops_base_proc_stats_construct_tma(p, NULL);
}

void PMIx_Proc_stats_destruct(pmix_proc_stats_t *p)
{
    pmix_bfrops_base_proc_stats_destruct_tma(p, NULL);
}

pmix_proc_stats_t* PMIx_Proc_stats_create(size_t n)
{
    return pmix_bfrops_base_proc_stats_create_tma(n, NULL);
}

void PMIx_Proc_stats_free(pmix_proc_stats_t *p, size_t n)
{
    pmix_bfrops_base_proc_stats_free_tma(p, n, NULL);
}

void PMIx_Disk_stats_construct(pmix_disk_stats_t *p)
{
    pmix_bfrops_base_disk_stats_construct_tma(p, NULL);
}

void PMIx_Disk_stats_destruct(pmix_disk_stats_t *p)
{
    pmix_bfrops_base_disk_stats_destruct_tma(p, NULL);
}

pmix_disk_stats_t* PMIx_Disk_stats_create(size_t n)
{
    return pmix_bfrops_base_disk_stats_create_tma(n, NULL);
}

void PMIx_Disk_stats_free(pmix_disk_stats_t *p, size_t n)
{
    pmix_bfrops_base_disk_stats_free_tma(p, n, NULL);
}

void PMIx_Net_stats_construct(pmix_net_stats_t *p)
{
    pmix_bfrops_base_net_stats_construct_tma(p, NULL);
}

void PMIx_Net_stats_destruct(pmix_net_stats_t *p)
{
    pmix_bfrops_base_net_stats_destruct_tma(p, NULL);
}

pmix_net_stats_t* PMIx_Net_stats_create(size_t n)
{
    return pmix_bfrops_base_net_stats_create_tma(n, NULL);
}

void PMIx_Net_stats_free(pmix_net_stats_t *p, size_t n)
{
    pmix_bfrops_base_net_stats_free_tma(p, n, NULL);
}

void PMIx_Node_stats_construct(pmix_node_stats_t *p)
{
    pmix_bfrops_base_node_stats_construct_tma(p, NULL);
}

void PMIx_Node_stats_destruct(pmix_node_stats_t *p)
{
    pmix_bfrops_base_node_stats_destruct_tma(p, NULL);
}

pmix_node_stats_t* PMIx_Node_stats_create(size_t n)
{
    return pmix_bfrops_base_node_stats_create_tma(n, NULL);
}

void PMIx_Node_stats_free(pmix_node_stats_t *p, size_t n)
{
    pmix_bfrops_base_node_stats_free_tma(p, n, NULL);
}

void PMIx_Pdata_construct(pmix_pdata_t *p)
{
    pmix_bfrops_base_pdata_construct_tma(p, NULL);
}

void PMIx_Pdata_destruct(pmix_pdata_t *p)
{
    pmix_bfrops_base_pdata_destruct_tma(p, NULL);
}

pmix_pdata_t* PMIx_Pdata_create(size_t n)
{
    return pmix_bfrops_base_pdata_create_tma(n, NULL);
}

void PMIx_Pdata_free(pmix_pdata_t *p, size_t n)
{
    pmix_bfrops_base_pdata_free_tma(p, n, NULL);
}

void PMIx_App_construct(pmix_app_t *p)
{
    pmix_bfrops_base_app_construct_tma(p, NULL);
}

void PMIx_App_destruct(pmix_app_t *p)
{
    pmix_bfrops_base_app_destruct_tma(p, NULL);
}

pmix_app_t* PMIx_App_create(size_t n)
{
    return pmix_bfrops_base_app_create_tma(n, NULL);
}

void PMIx_App_info_create(pmix_app_t *p, size_t n)
{
    pmix_bfrops_base_app_info_create_tma(p, n, NULL);
}

void PMIx_App_free(pmix_app_t *p, size_t n)
{
    pmix_bfrops_base_app_free_tma(p, n, NULL);
}

void PMIx_App_release(pmix_app_t *p)
{
    pmix_bfrops_base_app_release_tma(p, NULL);
}

void PMIx_Query_construct(pmix_query_t *p)
{
    pmix_bfrops_base_query_construct_tma(p, NULL);
}

void PMIx_Query_destruct(pmix_query_t *p)
{
    pmix_bfrops_base_query_destruct_tma(p, NULL);
}

pmix_query_t* PMIx_Query_create(size_t n)
{
    return pmix_bfrops_base_query_create_tma(n, NULL);
}

void PMIx_Query_qualifiers_create(pmix_query_t *p, size_t n)
{
    pmix_bfrops_base_query_qualifiers_create_tma(p, n, NULL);
}

void PMIx_Query_free(pmix_query_t *p, size_t n)
{
    pmix_bfrops_base_query_free_tma(p, n, NULL);
}

void PMIx_Query_release(pmix_query_t *p)
{
    pmix_bfrops_base_query_release_tma(p, NULL);
}

void PMIx_Regattr_construct(pmix_regattr_t *p)
{
    pmix_bfrops_base_regattr_construct_tma(p, NULL);
}

void PMIx_Regattr_destruct(pmix_regattr_t *p)
{
    pmix_bfrops_base_regattr_destruct_tma(p, NULL);
}

pmix_regattr_t* PMIx_Regattr_create(size_t n)
{
    return pmix_bfrops_base_regattr_create_tma(n, NULL);
}

void PMIx_Regattr_free(pmix_regattr_t *p, size_t n)
{
    pmix_bfrops_base_regattr_free_tma(p, n, NULL);
}

void PMIx_Regattr_load(pmix_regattr_t *p,
                       const char *name,
                       const char *key,
                       pmix_data_type_t type,
                       const char *description)
{
    pmix_bfrops_base_regattr_load_tma(p, name, key, type, description, NULL);
}

void PMIx_Regattr_xfer(pmix_regattr_t *dest,
                       const pmix_regattr_t *src)
{
    pmix_bfrops_base_regattr_xfer_tma(dest, src, NULL);
}

void PMIx_Data_array_init(pmix_data_array_t *p,
                          pmix_data_type_t type)
{
    pmix_bfrops_base_data_array_init_tma(p, type, NULL);
}

void PMIx_Data_array_construct(pmix_data_array_t *p,
                               size_t num, pmix_data_type_t type)
{
    pmix_bfrops_base_data_array_construct_tma(p, num, type, NULL);
}

void PMIx_Data_array_destruct(pmix_data_array_t *d)
{
    pmix_bfrops_base_data_array_destruct_tma(d, NULL);
}

PMIX_EXPORT pmix_data_array_t* PMIx_Data_array_create(size_t n, pmix_data_type_t type)
{
    return pmix_bfrops_base_data_array_create_tma(n, type, NULL);
}

PMIX_EXPORT void PMIx_Data_array_free(pmix_data_array_t *p)
{
    pmix_bfrops_base_data_array_free_tma(p, NULL);
}
