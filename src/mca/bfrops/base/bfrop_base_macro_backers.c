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

void PMIx_Load_key(pmix_key_t key, const char *src)
{
    memset(key, 0, PMIX_MAX_KEYLEN+1);
    if (NULL != src) {
        pmix_strncpy((char*)key, src, PMIX_MAX_KEYLEN);
    }
}

void PMIx_Value_construct(pmix_value_t *val)
{
    memset(val, 0, sizeof(pmix_value_t));
    val->type = PMIX_UNDEF;
}

void PMIx_Value_destruct(pmix_value_t *val)
{
    pmix_bfrops_base_value_destruct(val);
}

pmix_value_t* PMIx_Value_create(size_t n)
{
    pmix_value_t *v;
    size_t m;

    if (0 == n) {
        return NULL;
    }
    v = (pmix_value_t*)pmix_malloc(n * sizeof(pmix_value_t));
    if (NULL == v) {
        return NULL;
    }
    for (m=0; m < n; m++) {
        PMIx_Value_construct(&v[m]);
    }
    return v;
}

void PMIx_Value_free(pmix_value_t *v, size_t n)
{
    size_t m;

    if (NULL == v) {
        return;
    }
    for (m=0; m < n; m++) {
        PMIx_Value_destruct(&v[m]);
    }
}

pmix_boolean_t PMIx_Value_true(const pmix_value_t *value)
{
    char *ptr;

    if (PMIX_UNDEF == value->type) {
        return PMIX_BOOL_TRUE; // default to true
    }
    if (PMIX_BOOL == value->type) {
        if (value->data.flag) {
            return PMIX_BOOL_TRUE;
        } else {
            return PMIX_BOOL_FALSE;
        }
    }
    if (PMIX_STRING == value->type) {
        if (NULL == value->data.string) {
            return PMIX_BOOL_TRUE;
        }
        ptr = value->data.string;
        /* Trim leading whitespace */
        while (isspace(*ptr)) {
            ++ptr;
        }
        if ('\0' == *ptr) {
            return PMIX_BOOL_TRUE;
        }
        if (isdigit(*ptr)) {
            if (0 == atoi(ptr)) {
                return PMIX_BOOL_FALSE;
            } else {
                return PMIX_BOOL_TRUE;
            }
        } else if (0 == strncasecmp(ptr, "yes", 3) ||
                   0 == strncasecmp(ptr, "true", 4)) {
            return PMIX_BOOL_TRUE;
        } else if (0 == strncasecmp(ptr, "no", 2) ||
                   0 == strncasecmp(ptr, "false", 5)) {
            return PMIX_BOOL_FALSE;
        }
    }

    return PMIX_NON_BOOL;
}

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


PMIX_EXPORT void PMIx_Info_construct(pmix_info_t *p)
{
    PMIx_Load_key(p->key, NULL);
    PMIx_Value_construct(&p->value);
}

PMIX_EXPORT void PMIx_Info_destruct(pmix_info_t *p);
PMIX_EXPORT pmix_info_t* PMIx_Info_create(size_t n);
PMIX_EXPORT void PMIx_Info_free(pmix_info_t *p, size_t n);

pmix_boolean_t PMIx_Info_true(const pmix_info_t *p)
{
    pmix_boolean_t ret;

    ret = PMIx_Value_true(&p->value);
    return ret;
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

void PMIx_Coord_construct(pmix_coord_t *m)
{
    if (NULL == m) {
        return;
    }
    m->view = PMIX_COORD_VIEW_UNDEF;
    m->coord = NULL;
    m->dims = 0;
}

void PMIx_Coord_destruct(pmix_coord_t *m)
{
    if (NULL == m) {
        return;
    }
    m->view = PMIX_COORD_VIEW_UNDEF;
    if (NULL != m->coord) {
        pmix_free(m->coord);
        m->coord = NULL;
        m->dims = 0;
    }
}

pmix_coord_t* PMIx_Coord_create(size_t dims,
                                size_t number)
{
    pmix_coord_t *m;

    if (0 == number) {
        return NULL;
    }
    m = (pmix_coord_t*)pmix_malloc(number * sizeof(pmix_coord_t));
    if (NULL == m) {
        return NULL;
    }
    m->view = PMIX_COORD_VIEW_UNDEF;
    m->dims = dims;
    if (0 == dims) {
        m->coord = NULL;
    } else {
        m->coord = (uint32_t*)pmix_malloc(dims * sizeof(uint32_t));
        if (NULL != m->coord) {
            memset(m->coord, 0, dims * sizeof(uint32_t));
        }
    }
    return m;
}

void PMIx_Coord_free(pmix_coord_t *m, size_t number)
{
    size_t n;
    if (NULL == m) {
        return;
    }
    for (n=0; n < number; n++) {
        PMIx_Coord_destruct(&m[n]);
    }
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

PMIX_EXPORT void PMIx_Envar_construct(pmix_envar_t *e)
{
    e->envar = NULL;
    e->value = NULL;
    e->separator = '\0';
}

PMIX_EXPORT void PMIx_Envar_destruct(pmix_envar_t *e)
{
    if (NULL != e->envar) {
        pmix_free(e->envar);
        e->envar = NULL;
    }
    if (NULL != e->value) {
        pmix_free(e->value);
        e->value = NULL;
    }
}

PMIX_EXPORT pmix_envar_t* PMIx_Envar_create(size_t n)
{
    pmix_envar_t *e;
    size_t m;

    if (0 == n) {
        return NULL;
    }
    e = (pmix_envar_t*)pmix_malloc(n * sizeof(pmix_envar_t));
    if (NULL != e) {
        for (m=0; m < n; m++) {
            PMIx_Envar_construct(&e[m]);
        }
    }
    return e;
}

void PMIx_Envar_free(pmix_envar_t *e, size_t n)
{
    size_t m;

    if (NULL == e) {
        return;
    }
    for (m=0; m < n; m++) {
        PMIx_Envar_destruct(&e[m]);
    }
}

void PMIx_Envar_load(pmix_envar_t *e,
                     char *var,
                     char *value,
                     char separator)
{
    if (NULL != var) {
        e->envar = strdup(var);
    }
    if (NULL != value) {
        e->value = strdup(value);
    }
    e->separator = separator;
}

void PMIx_Data_buffer_construct(pmix_data_buffer_t *b)
{
    memset(b, 0, sizeof(pmix_data_buffer_t));
}

void PMIx_Data_buffer_destruct(pmix_data_buffer_t *b)
{
    if (NULL != b->base_ptr) {
        pmix_free(b->base_ptr);
        b->base_ptr = NULL;
    }
    b->pack_ptr = NULL;
    b->unpack_ptr = NULL;
    b->bytes_allocated = 0;
    b->bytes_used = 0;
}

pmix_data_buffer_t* PMIx_Data_buffer_create(void)
{
    pmix_data_buffer_t *b;

    b = (pmix_data_buffer_t*)pmix_malloc(sizeof(pmix_data_buffer_t));
    if (NULL != b) {
        PMIx_Data_buffer_construct(b);
    }
    return b;

}
void PMIx_Data_buffer_release(pmix_data_buffer_t *b)
{
    if (NULL == b) {
        return;
    }
    PMIx_Data_buffer_destruct(b);
}

void PMIx_Data_buffer_load(pmix_data_buffer_t *b,
                           char *bytes, size_t sz)
{
    pmix_byte_object_t bo;

    bo.bytes = bytes;
    bo.size = sz;
    PMIx_Data_load(b, &bo);
}

void PMIx_Data_buffer_unload(pmix_data_buffer_t *b,
                             char **bytes, size_t *sz)
{
    pmix_byte_object_t bo;
    pmix_status_t r;

    r = PMIx_Data_unload(b, &bo);
    if (PMIX_SUCCESS == r) {
        *bytes = bo.bytes;
        *sz = bo.size;
    } else {
        *bytes = NULL;
        *sz = 0;
    }
}

void PMIx_Proc_construct(pmix_proc_t *p)
{
    memset(p, 0, sizeof(pmix_proc_t));
    p->rank = PMIX_RANK_UNDEF;
}

void PMIx_Proc_destruct(pmix_proc_t *p)
{
    PMIx_Proc_construct(p);
    return;
}

pmix_proc_t* PMIx_Proc_create(size_t n)
{
    pmix_proc_t *p;
    size_t m;

    if (0 == n) {
        return NULL;
    }
    p = (pmix_proc_t*)pmix_malloc(n * sizeof(pmix_proc_t));
    if (NULL != p) {
        for (m=0; m < n; m++) {
            PMIx_Proc_construct(&p[m]);
        }
    }
    return p;
}

void PMIx_Proc_free(pmix_proc_t *p, size_t n)
{
    size_t m;

    if (NULL == p) {
        return;
    }
    for (m=0; m < n; m++) {
        PMIx_Proc_destruct(&p[m]);
    }
}

void PMIx_Proc_load(pmix_proc_t *p,
                    char *nspace, pmix_rank_t rank)
{
    PMIx_Proc_construct(p);
    PMIX_LOAD_PROCID(p, nspace, rank);
}

void PMIx_Multicluster_nspace_construct(pmix_nspace_t target,
                                        pmix_nspace_t cluster,
                                        pmix_nspace_t nspace)
{
    size_t len;

    PMIX_LOAD_NSPACE(target, NULL);
    len = pmix_nslen(cluster);
    if ((len + pmix_nslen(nspace)) < PMIX_MAX_NSLEN) {
        pmix_strncpy((char*)target, cluster, PMIX_MAX_NSLEN);
        target[len] = ':';
        pmix_strncpy((char*)&target[len+1], nspace, PMIX_MAX_NSLEN - len);
    }
}

void PMIx_Multicluster_nspace_parse(pmix_nspace_t target,
                                    pmix_nspace_t cluster,
                                    pmix_nspace_t nspace)
{
    size_t n, j;

    for (n=0; '\0' != target[n] && ':' != target[n] && n <= PMIX_MAX_NSLEN; n++) {
        cluster[n] = target[n];
    }
    n++;
    for (j=0; n <= PMIX_MAX_NSLEN && '\0' != target[n]; n++, j++) {
        nspace[j] = target[n];
    }
}

void PMIx_Proc_info_construct(pmix_proc_info_t *p)
{
    memset(p, 0, sizeof(pmix_proc_info_t));
    p->state = PMIX_PROC_STATE_UNDEF;
}

void PMIx_Proc_info_destruct(pmix_proc_info_t *p)
{
    if (NULL != p->hostname) {
        pmix_free(p->hostname);
    }
    if (NULL != p->executable_name) {
        pmix_free(p->executable_name);
    }
    PMIx_Proc_info_construct(p);
}

pmix_proc_info_t* PMIx_Proc_info_create(size_t n)
{
    pmix_proc_info_t *p;
    size_t m;

    if (0 == n) {
        return NULL;
    }
    p = (pmix_proc_info_t*)pmix_malloc(n * sizeof(pmix_proc_info_t));
    if (NULL != p) {
        for (m=0; m < n; m++) {
            PMIx_Proc_info_construct(&p[m]);
        }
    }
    return p;
}

void PMIx_Proc_info_free(pmix_proc_info_t *p, size_t n)
{
    size_t m;

    if (NULL == p) {
        return;
    }
    for (m=0; m < n; m++) {
        PMIx_Proc_info_destruct(&p[m]);
    }
}

void PMIx_Proc_stats_construct(pmix_proc_stats_t *p)
{
    memset(p, 0, sizeof(pmix_proc_stats_t));
}

void PMIx_Proc_stats_destruct(pmix_proc_stats_t *p)
{
    if (NULL != p->node) {
        pmix_free(p->node);
        p->node = NULL;
    }
    if (NULL != p->cmd) {
        pmix_free(p->cmd);
        p->cmd = NULL;
    }
}

pmix_proc_stats_t* PMIx_Proc_stats_create(size_t n)
{
    pmix_proc_stats_t *p;
    size_t m;

    if (0 == n) {
        return NULL;
    }
    p = (pmix_proc_stats_t*)pmix_malloc(n * sizeof(pmix_proc_stats_t));
    if (NULL != p) {
        for (m=0; m < n; m++) {
            PMIx_Proc_stats_construct(&p[m]);
        }
    }
    return p;
}

void PMIx_Proc_stats_free(pmix_proc_stats_t *p, size_t n)
{
    size_t m;

    if (NULL != p) {
        for (m=0; m < n; m++) {
            PMIx_Proc_stats_destruct(&p[m]);
        }
    }
}

void PMIx_Disk_stats_construct(pmix_disk_stats_t *p)
{
    memset(p, 0, sizeof(pmix_disk_stats_t));
}

void PMIx_Disk_stats_destruct(pmix_disk_stats_t *p)
{
    if (NULL != p->disk) {
        pmix_free(p->disk);
        p->disk = NULL;
    }
}

pmix_disk_stats_t* PMIx_Disk_stats_create(size_t n)
{
    pmix_disk_stats_t *p;
    size_t m;

    if (0 == n) {
        return NULL;
    }
    p = (pmix_disk_stats_t*)pmix_malloc(n * sizeof(pmix_disk_stats_t));
    if (NULL != p) {
        for (m=0; m < n; m++) {
            PMIx_Disk_stats_construct(&p[m]);
        }
    }
    return p;
}

void PMIx_Disk_stats_free(pmix_disk_stats_t *p, size_t n)
{
    size_t m;

    if (NULL != p) {
        for (m=0; m < n; m++) {
            PMIx_Disk_stats_destruct(&p[m]);
        }
    }
}

void PMIx_Net_stats_construct(pmix_net_stats_t *p)
{
    memset(p, 0, sizeof(pmix_net_stats_t));
}

void PMIx_Net_stats_destruct(pmix_net_stats_t *p)
{
    if (NULL != p->net_interface) {
        pmix_free(p->net_interface);
        p->net_interface = NULL;
    }
}

pmix_net_stats_t* PMIx_Net_stats_create(size_t n)
{
    pmix_net_stats_t *p;
    size_t m;

    if (0 == n) {
        return NULL;
    }
    p = (pmix_net_stats_t*)pmix_malloc(n * sizeof(pmix_net_stats_t));
    if (NULL != p) {
        for (m=0; m < n; m++) {
            PMIx_Net_stats_construct(&p[m]);
        }
    }
    return p;
}

void PMIx_Net_stats_free(pmix_net_stats_t *p, size_t n)
{
    size_t m;

    if (NULL != p) {
        for (m=0; m < n; m++) {
            PMIx_Net_stats_destruct(&p[m]);
        }
    }
}

void PMIx_Node_stats_construct(pmix_node_stats_t *p)
{
    memset(p, 0, sizeof(pmix_node_stats_t));
}

void PMIx_Node_stats_destruct(pmix_node_stats_t *p)
{
    if (NULL != p->node) {
        pmix_free(p->node);
        p->node = NULL;
    }
    if (NULL != p->diskstats) {
        PMIX_DISK_STATS_FREE(p->diskstats, p->ndiskstats);
    }
    if (NULL != p->netstats) {
        PMIX_NET_STATS_FREE(p->netstats, p->nnetstats);
    }
}

pmix_node_stats_t* PMIx_Node_stats_create(size_t n)
{
    pmix_node_stats_t *p;
    size_t m;

    if (0 == n) {
        return NULL;
    }
    p = (pmix_node_stats_t*)pmix_malloc(n * sizeof(pmix_node_stats_t));
    if (NULL != p) {
        for (m=0; m < n; m++) {
            PMIx_Node_stats_construct(&p[m]);
        }
    }
    return p;
}

void PMIx_Node_stats_free(pmix_node_stats_t *p, size_t n)
{
    size_t m;

    if (NULL != p) {
        for (m=0; m < n; m++) {
            PMIx_Node_stats_destruct(&p[m]);
        }
    }
}
