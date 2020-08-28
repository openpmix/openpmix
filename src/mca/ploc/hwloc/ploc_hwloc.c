/*
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2017      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2017      Inria.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "src/include/pmix_config.h"
#include "include/pmix_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <assert.h>
#include <errno.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif  /* HAVE_UNISTD_H */
#include <string.h>
#include <sys/mman.h>
#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif
#if HAVE_FCNTL_H
#include <fcntl.h>
#endif

#include "src/util/error.h"
#include "src/util/fd.h"
#include "src/util/name_fns.h"
#include "src/util/path.h"
#include "src/util/printf.h"
#include "src/util/show_help.h"
#include "src/mca/bfrops/bfrops_types.h"
#include "src/server/pmix_server_ops.h"

#include "src/mca/ploc/base/base.h"
#include "ploc_hwloc.h"

#if HWLOC_API_VERSION >= 0x20000
#include <hwloc/shmem.h>
#endif

static pmix_status_t setup_topology(pmix_info_t *info, size_t ninfo);
static pmix_status_t load_topology(pmix_topology_t *topo);
static pmix_status_t generate_cpuset_string(const pmix_cpuset_t *cpuset,
                                            char **cpuset_string);
static pmix_status_t get_cpuset(const char *cpuset_string,
                                pmix_cpuset_t *cpuset);
static pmix_status_t generate_locality_string(const pmix_cpuset_t *cpuset,
                                              char **locality);
static pmix_status_t get_relative_locality(const char *loc1,
                                           const char *loc2,
                                           pmix_locality_t *locality);
static void finalize(void);

pmix_ploc_module_t pmix_ploc_hwloc_module = {
    .finalize = finalize,
    .setup_topology = setup_topology,
    .load_topology = load_topology,
    .generate_cpuset_string = generate_cpuset_string,
    .get_cpuset = get_cpuset,
    .generate_locality_string = generate_locality_string,
    .get_relative_locality = get_relative_locality
};

static bool topo_in_shmem = false;

#if HWLOC_API_VERSION >= 0x20000
static bool shmem_available = false;
static size_t shmemsize = 0;
static size_t shmemaddr;
static char *shmemfile = NULL;
static int shmemfd = -1;
static bool space_available = false;
static uint64_t amount_space_avail = 0;

static int parse_map_line(const char *line,
                          unsigned long *beginp,
                          unsigned long *endp,
                          pmix_hwloc_vm_map_kind_t *kindp);
static int use_hole(unsigned long holebegin,
                    unsigned long holesize,
                    unsigned long *addrp,
                    unsigned long size);
static int find_hole(pmix_hwloc_vm_hole_kind_t hkind,
                     size_t *addrp,
                     size_t size);
static int enough_space(const char *filename,
                        size_t space_req,
                        uint64_t *space_avail,
                        bool *result);
#endif

static int set_flags(hwloc_topology_t topo, unsigned int flags)
{
    #if HWLOC_API_VERSION < 0x20000
            flags = HWLOC_TOPOLOGY_FLAG_IO_DEVICES;
    #else
            int ret = hwloc_topology_set_io_types_filter(topo, HWLOC_TYPE_FILTER_KEEP_IMPORTANT);
            if (0 != ret) return ret;
    #endif
    if (0 != hwloc_topology_set_flags(topo, flags)) {
        return PMIX_ERR_INIT;
    }
    return PMIX_SUCCESS;
}

static void finalize(void)
{
#if HWLOC_API_VERSION >= 0x20000
    if (NULL != shmemfile) {
        unlink(shmemfile);
        free(shmemfile);
    }
    if (0 <= shmemfd) {
        close(shmemfd);
    }
#endif
    if (NULL != pmix_globals.topology.topology && !pmix_globals.external_topology && !topo_in_shmem) {
        hwloc_topology_destroy(pmix_globals.topology.topology);
    }
    return;
}


pmix_status_t setup_topology(pmix_info_t *info, size_t ninfo)
{
    char *xmlbuffer=NULL;
    int len;
    size_t n;
    pmix_kval_t *kv;
    bool share = false;
#if HWLOC_API_VERSION >= 0x20000
    pmix_status_t rc;
#endif

    /* see if they want us to share the topology with our clients */
    for (n=0; n < ninfo; n++) {
        if (PMIX_CHECK_KEY(&info[n], PMIX_SERVER_SHARE_TOPOLOGY)) {
            share = PMIX_INFO_TRUE(&info[n]);
            break;
        }
    }

    if (NULL != pmix_globals.topology.topology) {
        /* if we need to share it, go do that */
        if (share) {
            goto sharetopo;
        }
        /* otherwise, we are done */
        return PMIX_SUCCESS;
    }

    /* see if they stipulated the type of topology they want */
    if (NULL != pmix_globals.topology.source) {
        if (0 != strcasecmp(pmix_globals.topology.source, "hwloc")) {
            /* they want somebody else */
            return PMIX_ERR_TAKE_NEXT_OPTION;
        }
    }

    /* we weren't given a topology, so get it for ourselves */
    if (0 != hwloc_topology_init((hwloc_topology_t*)&pmix_globals.topology.topology)) {
        return PMIX_ERR_TAKE_NEXT_OPTION;
    }

    if (0 != set_flags(pmix_globals.topology.topology, 0)) {
        hwloc_topology_destroy(pmix_globals.topology.topology);
        return PMIX_ERR_INIT;
    }

    if (0 != hwloc_topology_load(pmix_globals.topology.topology)) {
        PMIX_ERROR_LOG(PMIX_ERR_NOT_SUPPORTED);
        hwloc_topology_destroy(pmix_globals.topology.topology);
        return PMIX_ERR_NOT_SUPPORTED;
    }

    /* if we don't need to share it, then we are done */
    if (!share) {
        return PMIX_SUCCESS;
    }

  sharetopo:
    /* setup the XML representation(s) */

#if HWLOC_API_VERSION < 0x20000
     /* pass the topology string as we don't
      * have HWLOC shared memory available - we do
      * this so the procs won't read the topology
      * themselves as this could overwhelm the local
      * system on large-scale SMPs */
    kv = PMIX_NEW(pmix_kval_t);
    kv->key = strdup(PMIX_HWLOC_XML_V1);
    if (0 != hwloc_topology_export_xmlbuffer(pmix_globals.topology.topology, &xmlbuffer, &len)) {
        PMIX_RELEASE(kv);
    } else {
        PMIX_VALUE_LOAD(kv->value, xmlbuffer, PMIX_STRING);
        hwloc_free_xmlbuffer(pmix_globals.topology.topology, xmlbuffer);
        pmix_list_append(&pmix_server_globals.gdata, &kv->super);
    }
    /* we don't have the ability to do shared memory, so we are done */
    return PMIX_SUCCESS;
#else
    /* pass the topology as a v2 xml string */
    kv = PMIX_NEW(pmix_kval_t);
    kv->key = strdup(PMIX_HWLOC_XML_V2);
    if (0 != hwloc_topology_export_xmlbuffer(pmix_globals.topology.topology, &xmlbuffer, &len, 0)) {
        PMIX_RELEASE(kv);
    } else {
        kv->value = (pmix_value_t*)malloc(sizeof(pmix_value_t));
        PMIX_VALUE_LOAD(kv->value, xmlbuffer, PMIX_STRING);
        hwloc_free_xmlbuffer(pmix_globals.topology.topology, xmlbuffer);
        pmix_list_append(&pmix_server_globals.gdata, &kv->super);
    }
    /* and as a v1 xml string, should an older client attach */
    kv = PMIX_NEW(pmix_kval_t);
    kv->key = strdup(PMIX_HWLOC_XML_V1);
    if (0 != hwloc_topology_export_xmlbuffer(pmix_globals.topology.topology, &xmlbuffer, &len, HWLOC_TOPOLOGY_EXPORT_XML_FLAG_V1)) {
        PMIX_RELEASE(kv);
    } else {
        kv->value = (pmix_value_t*)malloc(sizeof(pmix_value_t));
        PMIX_VALUE_LOAD(kv->value, xmlbuffer, PMIX_STRING);
        hwloc_free_xmlbuffer(pmix_globals.topology.topology, xmlbuffer);
        pmix_list_append(&pmix_server_globals.gdata, &kv->super);
    }

    /* if they specified no shared memory, then we are done */
    if (VM_HOLE_NONE == mca_ploc_hwloc_component.hole_kind) {
        return PMIX_SUCCESS;
    }

    /* try and find a hole */
    if (PMIX_SUCCESS != (rc = find_hole(mca_ploc_hwloc_component.hole_kind,
                                        &shmemaddr, shmemsize))) {
        /* we couldn't find a hole, so don't use the shmem support */
        if (4 < pmix_output_get_verbosity(pmix_ploc_base_framework.framework_output)) {
            FILE *file = fopen("/proc/self/maps", "r");
            if (file) {
                char line[256];
                pmix_output(0, "%s Dumping /proc/self/maps",
                            PMIX_NAME_PRINT(&pmix_globals.myid));
                while (fgets(line, sizeof(line), file) != NULL) {
                    char *end = strchr(line, '\n');
                    if (end) {
                       *end = '\0';
                    }
                    pmix_output(0, "%s", line);
                }
                fclose(file);
            }
        }
        return PMIX_SUCCESS;
    }
    /* create the shmem file in our session dir so it
     * will automatically get cleaned up */
    pmix_asprintf(&shmemfile, "%s/hwloc.sm", pmix_server_globals.tmpdir);
    /* let's make sure we have enough space for the backing file */
    if (PMIX_SUCCESS != (rc = enough_space(shmemfile, shmemsize,
                                           &amount_space_avail,
                                           &space_available))) {
        pmix_output_verbose(2, pmix_ploc_base_framework.framework_output,
                            "%s an error occurred while determining "
                            "whether or not %s could be created for topo shmem.",
                            PMIX_NAME_PRINT(&pmix_globals.myid), shmemfile);
        free(shmemfile);
        shmemfile = NULL;
        return PMIX_SUCCESS;
    }
    if (!space_available) {
        if (1 < pmix_output_get_verbosity(pmix_ploc_base_framework.framework_output)) {
            pmix_show_help("help-pmix-ploc-hwloc.txt", "target full", true,
                           shmemfile, pmix_globals.hostname,
                           (unsigned long)shmemsize,
                           (unsigned long long)amount_space_avail);
        }
        free(shmemfile);
        shmemfile = NULL;
        return PMIX_SUCCESS;
    }
    /* enough space is available, so create the segment */
    if (-1 == (shmemfd = open(shmemfile, O_CREAT | O_RDWR, 0600))) {
        int err = errno;
        if (1 < pmix_output_get_verbosity(pmix_ploc_base_framework.framework_output)) {
            pmix_show_help("help-pmix-ploc-hwloc-hwloc.txt", "sys call fail", true,
                           pmix_globals.hostname,
                           "open(2)", "", strerror(err), err);
        }
        free(shmemfile);
        shmemfile = NULL;
        return PMIX_SUCCESS;
    }
    /* ensure nobody inherits this fd */
    pmix_fd_set_cloexec(shmemfd);
    /* populate the shmem segment with the topology */
    if (0 != (rc = hwloc_shmem_topology_write(pmix_globals.topology.topology, shmemfd, 0,
                                              (void*)shmemaddr, shmemsize, 0))) {
        pmix_output_verbose(2, pmix_ploc_base_framework.framework_output,
                            "%s an error occurred while writing topology to %s",
                            PMIX_NAME_PRINT(&pmix_globals.myid), shmemfile);
        unlink(shmemfile);
        free(shmemfile);
        shmemfile = NULL;
        close(shmemfd);
        shmemfd = -1;
        return PMIX_SUCCESS;
    }
    /* record that we did this so we know to clean it up */
    shmem_available = true;

    /* add the requisite key-values to the global data to be
     * give to each client */
    kv = PMIX_NEW(pmix_kval_t);
    kv->key = strdup(PMIX_HWLOC_SHMEM_FILE);
    kv->value = (pmix_value_t*)malloc(sizeof(pmix_value_t));
    PMIX_VALUE_LOAD(kv->value, shmemfile, PMIX_STRING);
    pmix_list_append(&pmix_server_globals.gdata, &kv->super);

    kv = PMIX_NEW(pmix_kval_t);
    kv->key = strdup(PMIX_HWLOC_SHMEM_ADDR);
    kv->value = (pmix_value_t*)malloc(sizeof(pmix_value_t));
    PMIX_VALUE_LOAD(kv->value, &shmemaddr, PMIX_SIZE);
    pmix_list_append(&pmix_server_globals.gdata, &kv->super);

    kv = PMIX_NEW(pmix_kval_t);
    kv->key = strdup(PMIX_HWLOC_SHMEM_SIZE);
    kv->value = (pmix_value_t*)malloc(sizeof(pmix_value_t));
    PMIX_VALUE_LOAD(kv->value, &shmemsize, PMIX_SIZE);
    pmix_list_append(&pmix_server_globals.gdata, &kv->super);

#endif

    return PMIX_SUCCESS;
}

static char* popstr(pmix_cb_t *cb)
{
    pmix_list_t *kvs = &cb->kvs;
    pmix_kval_t *kv;
    char *str;

    if (1 != pmix_list_get_size(kvs)) {
        return NULL;
    }
    kv = (pmix_kval_t*)pmix_list_get_first(kvs);
    if (PMIX_STRING != kv->value->type) {
        return NULL;
    }
    str = kv->value->data.string;
    kv->value->data.string = NULL;
    PMIX_LIST_DESTRUCT(&cb->kvs);
    return str;
}

#if HWLOC_API_VERSION >= 0x20000
static size_t popsize(pmix_cb_t *cb)
{
    pmix_list_t *kvs = &cb->kvs;
    pmix_kval_t *kv;
    size_t sz;

    if (1 != pmix_list_get_size(kvs)) {
        return UINT64_MAX;
    }
    kv = (pmix_kval_t*)pmix_list_get_first(kvs);
    if (PMIX_SIZE != kv->value->type) {
        return UINT64_MAX;
    }
    sz = kv->value->data.size;
    PMIX_LIST_DESTRUCT(&cb->kvs);
    return sz;
}
#endif

static int topology_set_flags (hwloc_topology_t topology, unsigned long flags) {
#if HWLOC_API_VERSION < 0x20000
    flags |= HWLOC_TOPOLOGY_FLAG_IO_DEVICES;
#else
    int ret = hwloc_topology_set_io_types_filter(topology, HWLOC_TYPE_FILTER_KEEP_IMPORTANT);
    if (0 != ret) {
        return ret;
    }
#endif
    return hwloc_topology_set_flags(topology, flags);
}

static pmix_status_t load_xml(char *xml)
{
    /* load the topology */
    if (0 != hwloc_topology_init((hwloc_topology_t*)&pmix_globals.topology.topology)) {
        return PMIX_ERROR;
    }
    if (0 != hwloc_topology_set_xmlbuffer(pmix_globals.topology.topology, xml, strlen(xml)+1)) {
        hwloc_topology_destroy(pmix_globals.topology.topology);
        return PMIX_ERROR;
    }
    /* since we are loading this from an external source, we have to
     * explicitly set a flag so hwloc sets things up correctly
     */
    if (0 != topology_set_flags(pmix_globals.topology.topology,
                                HWLOC_TOPOLOGY_FLAG_IS_THISSYSTEM)) {
        hwloc_topology_destroy(pmix_globals.topology.topology);
        return PMIX_ERROR;
    }
    /* now load the topology */
    if (0 != hwloc_topology_load(pmix_globals.topology.topology)) {
        hwloc_topology_destroy(pmix_globals.topology.topology);
        return PMIX_ERROR;
    }

    return PMIX_SUCCESS;
}

static pmix_status_t load_topology(pmix_topology_t *topo)
{
    pmix_cb_t cb;
    pmix_proc_t wildcard;
    pmix_status_t rc;
    char *file;

    PMIX_CONSTRUCT(&cb, pmix_cb_t);
    PMIX_LOAD_PROCID(&wildcard, pmix_globals.myid.nspace, PMIX_RANK_WILDCARD);
    cb.proc = &wildcard;
    cb.copy = true;

#if HWLOC_API_VERSION >= 0x20000
    int fd;
    uint64_t addr, size;

    /* first try to get the shmem link, if available */
    cb.key = PMIX_HWLOC_SHMEM_FILE;
    PMIX_GDS_FETCH_KV(rc, pmix_globals.mypeer, &cb);
    if (PMIX_SUCCESS != rc) {
        cb.key = NULL;
        PMIX_DESTRUCT(&cb);
        goto tryxml;
    }
    file = popstr(&cb);

    cb.key = PMIX_HWLOC_SHMEM_ADDR;
    PMIX_GDS_FETCH_KV(rc, pmix_globals.mypeer, &cb);
    if (PMIX_SUCCESS != rc) {
        cb.key = NULL;
        PMIX_DESTRUCT(&cb);
        free(file);
        goto tryxml;
    }
    addr = popsize(&cb);

    cb.key = PMIX_HWLOC_SHMEM_SIZE;
    PMIX_GDS_FETCH_KV(rc, pmix_globals.mypeer, &cb);
    if (PMIX_SUCCESS != rc) {
        cb.key = NULL;
        PMIX_DESTRUCT(&cb);
        free(file);
        goto tryxml;
    }
    size = popsize(&cb);

    if (0 > (fd = open(file, O_RDONLY))) {
        free(file);
        PMIX_ERROR_LOG(PMIX_ERROR);
        PMIX_DESTRUCT(&cb);
        return PMIX_ERROR;
    }
    free(file);
    rc = hwloc_shmem_topology_adopt((hwloc_topology_t*)&pmix_globals.topology.topology,
                                    fd, 0, (void*)addr, size, 0);
    if (0 == rc) {
        /* got it - we are done */
        pmix_globals.topology.source = strdup("hwloc");
        topo_in_shmem = true;
        PMIX_DESTRUCT(&cb);
        return PMIX_SUCCESS;
    }

    /* failed to adopt from shmem, so provide some feedback and
     * then fallback to other ways to get the topology */
    if (4 < pmix_output_get_verbosity(pmix_ploc_base_framework.framework_output)) {
        FILE *file = fopen("/proc/self/maps", "r");
        if (file) {
            char line[256];
            pmix_output(0, "Dumping /proc/self/maps");

            while (fgets(line, sizeof(line), file) != NULL) {
                char *end = strchr(line, '\n');
                if (end) {
                    *end = '\0';
                }
                pmix_output(0, "%s", line);
            }
            fclose(file);
        }
    }

  tryxml:
    /* try to get the v2 XML string */
    cb.key = PMIX_HWLOC_XML_V2;
    PMIX_GDS_FETCH_KV(rc, pmix_globals.mypeer, &cb);
    if (PMIX_SUCCESS == rc) {
        file = popstr(&cb);
        rc = load_xml(file);
        free(file);
        PMIX_DESTRUCT(&cb);
        return rc;
    }

#endif

    /* try to get the v1 XML string */
    cb.key = PMIX_HWLOC_XML_V1;
    PMIX_GDS_FETCH_KV(rc, pmix_globals.mypeer, &cb);
    if (PMIX_SUCCESS == rc) {
        file = popstr(&cb);
        rc = load_xml(file);
        free(file);
        PMIX_DESTRUCT(&cb);
        return rc;
    }
    /* if we got here, then we couldn't find anything */
    PMIX_DESTRUCT(&cb);

    /* try discovering it for ourselves */
    if (0 != hwloc_topology_init((hwloc_topology_t*)&pmix_globals.topology.topology) ||
        0 != topology_set_flags(pmix_globals.topology.topology, 0) ||
        0 != hwloc_topology_load(pmix_globals.topology.topology)) {
        return PMIX_ERR_NOT_SUPPORTED;
    }

    return PMIX_SUCCESS;
}

static pmix_status_t generate_cpuset_string(const pmix_cpuset_t *cpuset,
                                            char **cpuset_string)
{
    if (NULL == cpuset || NULL == cpuset->bitmap) {
        *cpuset_string = NULL;
        return PMIX_ERR_BAD_PARAM;
    }

    /* if we aren't the source, then nothing we can do */
    if (0 != strcasecmp(cpuset->source, "hwloc")) {
        return PMIX_ERR_TAKE_NEXT_OPTION;
    }

    hwloc_bitmap_list_asprintf(cpuset_string, cpuset->bitmap);

    return PMIX_SUCCESS;
}

static pmix_status_t get_cpuset(const char *cpuset_string,
                                pmix_cpuset_t *cpuset)
{
    /* if we aren't the source, then pass */
    if (0 != strcasecmp(cpuset->source, "hwloc")) {
        return PMIX_ERR_TAKE_NEXT_OPTION;
    }

    cpuset->source = strdup("hwloc");
    cpuset->bitmap = hwloc_bitmap_alloc();
    hwloc_bitmap_list_sscanf(cpuset->bitmap, cpuset_string);

    return PMIX_SUCCESS;
}

static int get_locality_string_by_depth(int d,
                                        hwloc_cpuset_t cpuset,
                                        hwloc_cpuset_t result)
{
    hwloc_obj_t obj;
    unsigned width, w;

    /* get the width of the topology at this depth */
    width = hwloc_get_nbobjs_by_depth(pmix_globals.topology.topology, d);
    if (0 == width) {
        return -1;
    }

    /* scan all objects at this depth to see if
     * the location overlaps with them
     */
    for (w=0; w < width; w++) {
        /* get the object at this depth/index */
        obj = hwloc_get_obj_by_depth(pmix_globals.topology.topology, d, w);
        /* see if the location intersects with it */
        if (hwloc_bitmap_intersects(obj->cpuset, cpuset)) {
            hwloc_bitmap_set(result, w);
        }
    }

    return 0;
}

static pmix_status_t generate_locality_string(const pmix_cpuset_t *cpuset,
                                              char **loc)
{
    char *locality=NULL, *tmp, *t2;
    unsigned depth, d;
    hwloc_cpuset_t result;
    hwloc_obj_type_t type;

    /* if we aren't the source, then pass */
    if (0 != strcasecmp(cpuset->source, "hwloc")) {
        return PMIX_ERR_TAKE_NEXT_OPTION;
    }

    /* if this proc is not bound, then there is no locality. We
     * know it isn't bound if the cpuset is NULL, or if it is
     * all 1's */
    if (NULL == cpuset->bitmap || hwloc_bitmap_isfull(cpuset->bitmap)) {
        *loc = NULL;
        return PMIX_SUCCESS;
    }

    /* we are going to use a bitmap to save the results so
     * that we can use a hwloc utility to print them */
    result = hwloc_bitmap_alloc();

    /* get the max depth of the topology */
    depth = hwloc_topology_get_depth(pmix_globals.topology.topology);

    /* start at the first depth below the top machine level */
    for (d=1; d < depth; d++) {
        /* get the object type at this depth */
        type = hwloc_get_depth_type(pmix_globals.topology.topology, d);
        /* if it isn't one of interest, then ignore it */
        if (HWLOC_OBJ_NODE != type &&
            HWLOC_OBJ_PACKAGE != type &&
#if HWLOC_API_VERSION < 0x20000
            HWLOC_OBJ_CACHE != type &&
#else
            HWLOC_OBJ_L1CACHE != type &&
            HWLOC_OBJ_L2CACHE != type &&
            HWLOC_OBJ_L3CACHE != type &&
#endif
            HWLOC_OBJ_CORE != type &&
            HWLOC_OBJ_PU != type) {
            continue;
        }

        if (get_locality_string_by_depth(d, cpuset->bitmap, result) < 0) {
            continue;
        }

        /* it should be impossible, but allow for the possibility
         * that we came up empty at this depth */
        if (!hwloc_bitmap_iszero(result)) {
            hwloc_bitmap_list_asprintf(&tmp, result);
            switch(type) {
                case HWLOC_OBJ_NODE:
                    pmix_asprintf(&t2, "%sNM%s:", (NULL == locality) ? "" : locality, tmp);
                    if (NULL != locality) {
                        free(locality);
                    }
                    locality = t2;
                    break;
                case HWLOC_OBJ_PACKAGE:
                    pmix_asprintf(&t2, "%sSK%s:", (NULL == locality) ? "" : locality, tmp);
                    if (NULL != locality) {
                        free(locality);
                    }
                    locality = t2;
                    break;
#if HWLOC_API_VERSION < 0x20000
                case HWLOC_OBJ_CACHE:
                {
                    unsigned cachedepth = hwloc_get_obj_by_depth(pmix_globals.topology.topology, d, 0)->attr->cache.depth;
                    if (3 == cachedepth) {
                        pmix_asprintf(&t2, "%sL3%s:", (NULL == locality) ? "" : locality, tmp);
                        if (NULL != locality) {
                            free(locality);
                        }
                        locality = t2;
                        break;
                    } else if (2 == cachedepth) {
                        pmix_asprintf(&t2, "%sL2%s:", (NULL == locality) ? "" : locality, tmp);
                        if (NULL != locality) {
                            free(locality);
                        }
                        locality = t2;
                        break;
                    } else {
                        pmix_asprintf(&t2, "%sL1%s:", (NULL == locality) ? "" : locality, tmp);
                        if (NULL != locality) {
                            free(locality);
                        }
                        locality = t2;
                        break;
                    }
                }
                    break;
#else
                case HWLOC_OBJ_L3CACHE:
                    pmix_asprintf(&t2, "%sL3%s:", (NULL == locality) ? "" : locality, tmp);
                    if (NULL != locality) {
                        free(locality);
                    }
                    locality = t2;
                    break;
                case HWLOC_OBJ_L2CACHE:
                    pmix_asprintf(&t2, "%sL2%s:", (NULL == locality) ? "" : locality, tmp);
                    if (NULL != locality) {
                        free(locality);
                    }
                    locality = t2;
                    break;
                case HWLOC_OBJ_L1CACHE:
                    pmix_asprintf(&t2, "%sL1%s:", (NULL == locality) ? "" : locality, tmp);
                    if (NULL != locality) {
                        free(locality);
                    }
                    locality = t2;
                    break;
#endif
                case HWLOC_OBJ_CORE:
                    pmix_asprintf(&t2, "%sCR%s:", (NULL == locality) ? "" : locality, tmp);
                    if (NULL != locality) {
                        free(locality);
                    }
                    locality = t2;
                    break;
                case HWLOC_OBJ_PU:
                    pmix_asprintf(&t2, "%sHT%s:", (NULL == locality) ? "" : locality, tmp);
                    if (NULL != locality) {
                        free(locality);
                    }
                    locality = t2;
                    break;
                default:
                    /* just ignore it */
                    break;
            }
            free(tmp);
        }
        hwloc_bitmap_zero(result);
    }

#if HWLOC_API_VERSION >= 0x20000
    if (get_locality_string_by_depth(HWLOC_TYPE_DEPTH_NUMANODE, cpuset->bitmap, result) == 0) {
        /* it should be impossible, but allow for the possibility
         * that we came up empty at this depth */
        if (!hwloc_bitmap_iszero(result)) {
            hwloc_bitmap_list_asprintf(&tmp, result);
            pmix_asprintf(&t2, "%sNM%s:", (NULL == locality) ? "" : locality, tmp);
            if (NULL != locality) {
                free(locality);
            }
            locality = t2;
            free(tmp);
        }
        hwloc_bitmap_zero(result);
    }
#endif

    hwloc_bitmap_free(result);

    /* remove the trailing colon */
    if (NULL != locality) {
        locality[strlen(locality)-1] = '\0';
    }
    *loc = locality;
    return PMIX_SUCCESS;
}

static pmix_status_t get_relative_locality(const char *locality1,
                                           const char *locality2,
                                           pmix_locality_t *loc)
{
    pmix_locality_t locality;
    char *loc1, *loc2, **set1, **set2;
    hwloc_bitmap_t bit1, bit2;
    size_t n1, n2;
    pmix_status_t rc = PMIX_ERR_TAKE_NEXT_OPTION;

    /* check that locality was generated by us */
    if (0 != strncasecmp(locality1, "hwloc:", strlen("hwloc:")) ||
        0 != strncasecmp(locality2, "hwloc:", strlen("hwloc:"))) {
        return PMIX_ERR_TAKE_NEXT_OPTION;
    }
    /* point to the first character past the ':' delimiter */
    loc1 = (char*)&locality1[strlen("hwloc:")];
    loc2 = (char*)&locality2[strlen("hwloc:")];

    /* start with what we know - they share a node */
    locality = PMIX_LOCALITY_SHARE_NODE;

    /* if either location is NULL, then that isn't bound */
    if (NULL == loc1 || NULL == loc2) {
        *loc = locality;
        return PMIX_SUCCESS;
    }

    set1 = pmix_argv_split(loc1, ':');
    set2 = pmix_argv_split(loc2, ':');
    bit1 = hwloc_bitmap_alloc();
    bit2 = hwloc_bitmap_alloc();

    /* check each matching type */
    for (n1=0; NULL != set1[n1]; n1++) {
        /* convert the location into bitmap */
        hwloc_bitmap_list_sscanf(bit1, &set1[n1][2]);
        /* find the matching type in set2 */
        for (n2=0; NULL != set2[n2]; n2++) {
            if (0 == strncmp(set1[n1], set2[n2], 2)) {
                /* convert the location into bitmap */
                hwloc_bitmap_list_sscanf(bit2, &set2[n2][2]);
                /* see if they intersect */
                if (hwloc_bitmap_intersects(bit1, bit2)) {
                    /* set the corresponding locality bit */
                    if (0 == strncmp(set1[n1], "NM", 2)) {
                        locality |= PMIX_LOCALITY_SHARE_NUMA;
                    } else if (0 == strncmp(set1[n1], "SK", 2)) {
                        locality |= PMIX_LOCALITY_SHARE_PACKAGE;
                    } else if (0 == strncmp(set1[n1], "L3", 2)) {
                        locality |= PMIX_LOCALITY_SHARE_L3CACHE;
                    } else if (0 == strncmp(set1[n1], "L2", 2)) {
                        locality |= PMIX_LOCALITY_SHARE_L2CACHE;
                    } else if (0 == strncmp(set1[n1], "L1", 2)) {
                        locality |= PMIX_LOCALITY_SHARE_L1CACHE;
                    } else if (0 == strncmp(set1[n1], "CR", 2)) {
                        locality |= PMIX_LOCALITY_SHARE_CORE;
                    } else if (0 == strncmp(set1[n1], "HT", 2)) {
                        locality |= PMIX_LOCALITY_SHARE_HWTHREAD;
                    } else {
                        /* should never happen */
                        pmix_output(0, "UNRECOGNIZED LOCALITY %s", set1[n1]);
                        rc = PMIX_ERROR;
                    }
                }
                break;
            }
        }
    }
    pmix_argv_free(set1);
    pmix_argv_free(set2);
    hwloc_bitmap_free(bit1);
    hwloc_bitmap_free(bit2);
    *loc = locality;
    return rc;
}

#if HWLOC_API_VERSION >= 0x20000

static int parse_map_line(const char *line,
                          unsigned long *beginp,
                          unsigned long *endp,
                          pmix_hwloc_vm_map_kind_t *kindp)
{
    const char *tmp = line, *next;
    unsigned long value;

    /* "beginaddr-endaddr " */
    value = strtoull(tmp, (char **) &next, 16);
    if (next == tmp) {
        return PMIX_ERROR;
    }

    *beginp = (unsigned long) value;

    if (*next != '-') {
        return PMIX_ERROR;
    }

     tmp = next + 1;

    value = strtoull(tmp, (char **) &next, 16);
    if (next == tmp) {
        return PMIX_ERROR;
    }
    *endp = (unsigned long) value;
    tmp = next;

    if (*next != ' ') {
        return PMIX_ERROR;
    }
    tmp = next + 1;

    /* look for ending absolute path */
    next = strchr(tmp, '/');
    if (next) {
        *kindp = VM_MAP_FILE;
    } else {
        /* look for ending special tag [foo] */
        next = strchr(tmp, '[');
        if (next) {
            if (!strncmp(next, "[heap]", 6)) {
                *kindp = VM_MAP_HEAP;
            } else if (!strncmp(next, "[stack]", 7)) {
                *kindp = VM_MAP_STACK;
            } else {
                char *end;
                if ((end = strchr(next, '\n')) != NULL) {
                    *end = '\0';
                }
                *kindp = VM_MAP_OTHER;
            }
        } else {
            *kindp = VM_MAP_ANONYMOUS;
        }
    }

    return PMIX_SUCCESS;
}

#define ALIGN2MB (2*1024*1024UL)

static int use_hole(unsigned long holebegin,
                    unsigned long holesize,
                    unsigned long *addrp,
                    unsigned long size)
{
    unsigned long aligned;
    unsigned long middle = holebegin+holesize/2;

    if (holesize < size) {
        return PMIX_ERROR;
    }

    /* try to align the middle of the hole on 64MB for POWER's 64k-page PMD */
    #define ALIGN64MB (64*1024*1024UL)
    aligned = (middle + ALIGN64MB) & ~(ALIGN64MB-1);
    if (aligned + size <= holebegin + holesize) {
        *addrp = aligned;
        return PMIX_SUCCESS;
    }

    /* try to align the middle of the hole on 2MB for x86 PMD */
    aligned = (middle + ALIGN2MB) & ~(ALIGN2MB-1);
    if (aligned + size <= holebegin + holesize) {
        *addrp = aligned;
        return PMIX_SUCCESS;
    }

    /* just use the end of the hole */
    *addrp = holebegin + holesize - size;
    return PMIX_SUCCESS;
}

static int find_hole(pmix_hwloc_vm_hole_kind_t hkind,
                     size_t *addrp, size_t size)
{
    unsigned long biggestbegin = 0;
    unsigned long biggestsize = 0;
    unsigned long prevend = 0;
    pmix_hwloc_vm_map_kind_t prevmkind = VM_MAP_OTHER;
    int in_libs = 0;
    FILE *file;
    char line[96];

    file = fopen("/proc/self/maps", "r");
    if (!file) {
        return PMIX_ERROR;
    }

    while (fgets(line, sizeof(line), file) != NULL) {
        unsigned long begin=0, end=0;
        pmix_hwloc_vm_map_kind_t mkind=VM_MAP_OTHER;

        if (!parse_map_line(line, &begin, &end, &mkind)) {
            switch (hkind) {
                case VM_HOLE_BEGIN:
                    fclose(file);
                    return use_hole(0, begin, addrp, size);

                case VM_HOLE_AFTER_HEAP:
                    if (prevmkind == VM_MAP_HEAP && mkind != VM_MAP_HEAP) {
                        /* only use HEAP when there's no other HEAP after it
                         * (there can be several of them consecutively).
                         */
                        fclose(file);
                        return use_hole(prevend, begin-prevend, addrp, size);
                    }
                    break;

                case VM_HOLE_BEFORE_STACK:
                    if (mkind == VM_MAP_STACK) {
                        fclose(file);
                        return use_hole(prevend, begin-prevend, addrp, size);
                    }
                    break;

                case VM_HOLE_IN_LIBS:
                    /* see if we are between heap and stack */
                    if (prevmkind == VM_MAP_HEAP) {
                        in_libs = 1;
                    }
                    if (mkind == VM_MAP_STACK) {
                        in_libs = 0;
                    }
                    if (!in_libs) {
                        /* we're not in libs, ignore this entry */
                        break;
                    }
                    /* we're in libs, consider this entry for searching the biggest hole below */
                    /* fallthrough */

                case VM_HOLE_BIGGEST:
                    if (begin-prevend > biggestsize) {
                        biggestbegin = prevend;
                        biggestsize = begin-prevend;
                    }
                    break;

                    default:
                        assert(0);
            }
        }

        while (!strchr(line, '\n')) {
            if (!fgets(line, sizeof(line), file)) {
                goto done;
            }
        }

        if (mkind == VM_MAP_STACK) {
          /* Don't go beyond the stack. Other VMAs are special (vsyscall, vvar, vdso, etc),
           * There's no spare room there. And vsyscall is even above the userspace limit.
           */
          break;
        }

        prevend = end;
        prevmkind = mkind;

    }

  done:
    fclose(file);
    if (hkind == VM_HOLE_IN_LIBS || hkind == VM_HOLE_BIGGEST) {
        return use_hole(biggestbegin, biggestsize, addrp, size);
    }

    return PMIX_ERROR;
}

static int enough_space(const char *filename,
                        size_t space_req,
                        uint64_t *space_avail,
                        bool *result)
{
    uint64_t avail = 0;
    size_t fluff = (size_t)(.05 * space_req);
    bool enough = false;
    char *last_sep = NULL;
    /* the target file name is passed here, but we need to check the parent
     * directory. store it so we can extract that info later. */
    char *target_dir = strdup(filename);
    int rc;

    if (NULL == target_dir) {
        rc = PMIX_ERR_OUT_OF_RESOURCE;
        goto out;
    }
    /* get the parent directory */
    last_sep = strrchr(target_dir, PMIX_PATH_SEP[0]);
    *last_sep = '\0';
    /* now check space availability */
    if (PMIX_SUCCESS != (rc = pmix_path_df(target_dir, &avail))) {
        goto out;
    }
    /* do we have enough space? */
    if (avail >= space_req + fluff) {
        enough = true;
    }

out:
    if (NULL != target_dir) {
        free(target_dir);
    }
    *result = enough;
    *space_avail = avail;
    return rc;
}
#endif
