/*
 * Copyright (c) 2022      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "src/include/pmix_config.h"

#include "pmix.h"

#include "src/hwloc/pmix_hwloc.h"
#include "src/include/pmix_globals.h"
#include "src/mca/bfrops/base/base.h"

pmix_status_t PMIx_Value_load(pmix_value_t *v,
                              const void *data,
                              pmix_data_type_t type)
{
    pmix_bfrops_base_value_load(v, data, type);
    return PMIX_SUCCESS;
}

pmix_status_t PMIx_Value_unload(pmix_value_t *kv,
                                void **data,
                                size_t *sz)
{
    return pmix_bfrops_base_value_unload(kv, data, sz);
}

void PMIx_Value_destruct(pmix_value_t *val)
{
    pmix_bfrops_base_value_destruct(val);
}

pmix_status_t PMIx_Value_xfer(pmix_value_t *dest,
                              const pmix_value_t *src)
{
    return pmix_bfrops_base_value_xfer(dest, src);
}

pmix_value_cmp_t PMIx_Value_compare(pmix_value_t *v1,
                                    pmix_value_t *v2)
{
    return pmix_bfrops_base_value_cmp(v1, v2);
}

void PMIx_Data_array_destruct(pmix_data_array_t *d)
{
    pmix_bfrops_base_darray_destruct(d);
}

pmix_status_t PMIx_Info_load(pmix_info_t *info,
                             const char *key,
                             const void *data,
                             pmix_data_type_t type)
{
    if (NULL == key) {
        return PMIX_ERR_BAD_PARAM;
    }
    PMIX_LOAD_KEY(info->key, key);
    info->flags = 0;
    return PMIx_Value_load(&info->value, data, type);
}

pmix_status_t PMIx_Info_xfer(pmix_info_t *dest,
                             const pmix_info_t *src)
{
    pmix_status_t rc;

    if (NULL == dest || NULL == src) {
        return PMIX_ERR_BAD_PARAM;
    }
    PMIX_LOAD_KEY(dest->key, src->key);
    dest->flags = src->flags;
    if (PMIX_INFO_IS_PERSISTENT(src)) {
        memcpy(&dest->value, &src->value, sizeof(pmix_value_t));
        rc = PMIX_SUCCESS;
    } else {
        rc = PMIx_Value_xfer(&dest->value, &src->value);
    }
    return rc;
}

void PMIx_Topology_construct(pmix_topology_t *t)
{
    memset(t, 0, sizeof(pmix_topology_t));
}

void PMIx_Topology_destruct(pmix_topology_t *t)
{
    pmix_hwloc_destruct_topology(t);
}

pmix_topology_t* PMIx_Topology_create(size_t n)
{
    pmix_topology_t *t;
    size_t m;

    if (0 == n) {
        return NULL;
    }
    t = (pmix_topology_t*)pmix_malloc(n * sizeof(pmix_topology_t));
    if (NULL != t) {
        for (m=0; m < n; m++) {
            PMIx_Topology_construct(&t[m]);
        }
    }
    return t;
}

void PMIx_Topology_free(pmix_topology_t *t, size_t n)
{
    size_t m;

    if (NULL == t) {
        return;
    }
    for (m=0; m < n; m++) {
        PMIx_Topology_destruct(&t[m]);
    }
}

void PMIx_Cpuset_construct(pmix_cpuset_t *c)
{
    memset(c, 0, sizeof(pmix_cpuset_t));
}

void PMIx_Cpuset_destruct(pmix_cpuset_t *c)
{
    pmix_hwloc_destruct_cpuset(c);
}

pmix_cpuset_t* PMIx_Cpuset_create(size_t n)
{
    pmix_cpuset_t *c;
    size_t m;

    if (0 == n) {
        return NULL;
    }
    c = (pmix_cpuset_t*)pmix_malloc(n * sizeof(pmix_cpuset_t));
    if (NULL == c) {
        return NULL;
    }
    for (m=0; m < n; m++) {
        PMIx_Cpuset_construct(&c[m]);
    }
    return c;
}

void PMIx_Cpuset_free(pmix_cpuset_t *c, size_t n)
{
    size_t m;

    if (NULL == c) {
        return;
    }
    for (m=0; m < n; m++) {
        PMIx_Cpuset_destruct(&c[m]);
    }
}

void PMIx_Geometry_construct(pmix_geometry_t *g)
{
    memset(g, 0, sizeof(pmix_geometry_t));
}

void PMIx_Geometry_destruct(pmix_geometry_t *g)
{
    if (NULL != g->uuid) {
        free(g->uuid);
        g->uuid = NULL;
    }
    if (NULL != g->osname) {
        free(g->osname);
        g->osname = NULL;
    }
    if (NULL != g->coordinates) {
        PMIx_Coord_free(g->coordinates, g->ncoords);
    }
}

pmix_geometry_t* PMIx_Geometry_create(size_t n)
{
    pmix_geometry_t *g;
    size_t m;

    if (0 == n) {
        return NULL;
    }
    g = (pmix_geometry_t*)pmix_malloc(n * sizeof(pmix_geometry_t));
    if (NULL != g) {
        for (m=0; m < n; m++) {
            PMIx_Geometry_construct(&g[m]);
        }
    }
    return g;
}

void PMIx_Geometry_free(pmix_geometry_t *g, size_t n)
{
    size_t m;

    if (NULL == g) {
        return;
    }
    for (m=0; m < n; m++) {
        PMIx_Geometry_destruct(&g[m]);
    }
}

void PMIx_Device_distance_construct(pmix_device_distance_t *d)
{
    memset(d, 0, sizeof(pmix_device_distance_t));
    d->type = PMIX_DEVTYPE_UNKNOWN;
    d->mindist = UINT16_MAX;
    d->maxdist = UINT16_MAX;
}

void PMIx_Device_distance_destruct(pmix_device_distance_t *d)
{
    if (NULL != (d->uuid)) {
        pmix_free(d->uuid);
    }
    if (NULL != (d->osname)) {
        pmix_free(d->osname);
    }
}

pmix_device_distance_t* PMIx_Device_distance_create(size_t n)
{
    pmix_device_distance_t *d;
    size_t m;

    if (0 == n) {
        return NULL;
    }
    d = (pmix_device_distance_t*)pmix_malloc(n * sizeof(pmix_device_distance_t));
    if (NULL != d) {
        for (m=0; m < n; m++) {
            PMIx_Device_distance_construct(&d[m]);
        }
    }
    return d;
}

void PMIx_Device_distance_free(pmix_device_distance_t *d, size_t n)
{
    size_t m;

    if (NULL == d) {
        return;
    }
    for (m=0; m < n; m++) {
        PMIx_Device_distance_destruct(&d[m]);
    }
}

void PMIx_Byte_object_construct(pmix_byte_object_t *b)
{
    b->bytes = NULL;
    b->size = 0;
}

void PMIx_Byte_object_destruct(pmix_byte_object_t *b)
{
    if (NULL != b->bytes) {
        pmix_free(b->bytes);
    }
    b->bytes = NULL;
    b->size = 0;
}

pmix_byte_object_t* PMIx_Byte_object_create(size_t n)
{
    pmix_byte_object_t *b;
    size_t m;

    if (0 == n) {
        return NULL;
    }
    b = (pmix_byte_object_t*)pmix_malloc(n * sizeof(pmix_byte_object_t));
    if (NULL != b) {
        for (m=0; m < n; m++) {
            PMIx_Byte_object_construct(&b[m]);
        }
    }
    return b;
}
void PMIx_Byte_object_free(pmix_byte_object_t *b, size_t n)
{
    size_t m;

    if (NULL == b) {
        return;
    }
    for (m=0; m < n; m++) {
        PMIx_Byte_object_destruct(&b[m]);
    }
}

void PMIx_Endpoint_construct(pmix_endpoint_t *e)
{
    memset(e, 0, sizeof(pmix_endpoint_t));
}

void PMIx_Endpoint_destruct(pmix_endpoint_t *e)
{
    if (NULL != e->uuid) {
        free(e->uuid);
    }
    if (NULL != e->osname) {
        free(e->osname);
    }
    if (NULL != e->endpt.bytes) {
        free(e->endpt.bytes);
    }
}

pmix_endpoint_t* PMIx_Endpoint_create(size_t n)
{
    pmix_endpoint_t *e;
    size_t m;

    if (0 == n) {
        return NULL;
    }
    e = (pmix_endpoint_t*)pmix_malloc(n * sizeof(pmix_endpoint_t));
    if (NULL != e) {
        for (m=0; m < n; m++) {
            PMIx_Endpoint_construct(&e[m]);
        }
    }
    return e;
}

void PMIx_Endpoint_free(pmix_endpoint_t *e, size_t n)
{
    size_t m;

    if (NULL == e) {
        return;
    }
    for (m=0; m < n; m++) {
        PMIx_Endpoint_destruct(&e[m]);
    }
}
