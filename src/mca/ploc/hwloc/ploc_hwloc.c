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
#include PMIX_HWLOC_HEADER

#include "src/util/error.h"
#include "src/util/fd.h"
#include "src/util/name_fns.h"
#include "src/util/path.h"
#include "src/util/printf.h"
#include "src/util/show_help.h"
#include "src/util/vmhole.h"
#include "src/mca/bfrops/base/base.h"
#include "src/mca/pnet/pnet.h"
#include "src/server/pmix_server_ops.h"
#include "src/client/pmix_client_ops.h"

#include "src/mca/ploc/base/base.h"
#include "ploc_hwloc.h"

#if HWLOC_API_VERSION >= 0x20000
#include <hwloc/shmem.h>
#endif

static pmix_status_t setup_topology(pmix_info_t *info, size_t ninfo);
static pmix_status_t load_topology(pmix_topology_t *topo);
static pmix_status_t generate_cpuset_string(const pmix_cpuset_t *cpuset,
                                            char **cpuset_string);
static pmix_status_t parse_cpuset_string(const char *cpuset_string,
                                         pmix_cpuset_t *cpuset);
static pmix_status_t generate_locality_string(const pmix_cpuset_t *cpuset,
                                              char **locality);
static pmix_status_t get_relative_locality(const char *loc1,
                                           const char *loc2,
                                           pmix_locality_t *locality);
static pmix_status_t get_cpuset(pmix_cpuset_t *cpuset,
                                pmix_bind_envelope_t ref);
static pmix_status_t compute_distances(pmix_topology_t *topo,
                                       pmix_cpuset_t *cpuset,
                                       pmix_info_t info[], size_t ninfo,
                                       pmix_device_distance_t **dist,
                                       size_t *ndist);
static pmix_status_t pack_cpuset(pmix_buffer_t *buf, pmix_cpuset_t *src,
                                 pmix_pointer_array_t *regtypes);
static pmix_status_t unpack_cpuset(pmix_buffer_t *buf, pmix_cpuset_t *dest,
                                   pmix_pointer_array_t *regtypes);
static pmix_status_t copy_cpuset(pmix_cpuset_t *dest, pmix_cpuset_t *src);
static char* print_cpuset(pmix_cpuset_t *src);
static pmix_status_t destruct_cpuset(pmix_cpuset_t *src);
static pmix_status_t release_cpuset(pmix_cpuset_t *src, size_t ncpu);
static pmix_status_t pack_topology(pmix_buffer_t *buf, pmix_topology_t *src,
                                   pmix_pointer_array_t *regtypes);
static pmix_status_t unpack_topology(pmix_buffer_t *buf, pmix_topology_t *dest,
                                     pmix_pointer_array_t *regtypes);
static pmix_status_t copy_topology(pmix_topology_t *dest, pmix_topology_t *src);
static char* print_topology(pmix_topology_t *src);
static pmix_status_t destruct_topology(pmix_topology_t *src);
static pmix_status_t release_topology(pmix_topology_t *src, size_t ncpu);

static void finalize(void);

pmix_ploc_module_t pmix_ploc_hwloc_module = {
    .finalize = finalize,
    .setup_topology = setup_topology,
    .load_topology = load_topology,
    .generate_cpuset_string = generate_cpuset_string,
    .parse_cpuset_string = parse_cpuset_string,
    .generate_locality_string = generate_locality_string,
    .get_relative_locality = get_relative_locality,
    .get_cpuset = get_cpuset,
    .compute_distances = compute_distances,
    .pack_cpuset = pack_cpuset,
    .unpack_cpuset = unpack_cpuset,
    .copy_cpuset = copy_cpuset,
    .print_cpuset = print_cpuset,
    .destruct_cpuset = destruct_cpuset,
    .release_cpuset = release_cpuset,
    .pack_topology = pack_topology,
    .unpack_topology = unpack_topology,
    .copy_topology = copy_topology,
    .print_topology = print_topology,
    .destruct_topology = destruct_topology,
    .release_topology = release_topology,
};

static bool topo_in_shmem = false;
#if HWLOC_API_VERSION >= 0x20000
static size_t shmemsize = 0;
static size_t shmemaddr;
static char *shmemfile = NULL;
static int shmemfd = -1;
#endif

static pmix_status_t load_xml(char *xml);

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

    /* did they give us one to use? */
    if (NULL != mca_ploc_hwloc_component.topo_file) {
        if (0 != hwloc_topology_init((hwloc_topology_t*)&pmix_globals.topology.topology)) {
            return PMIX_ERR_TAKE_NEXT_OPTION;
        }
        if (0 != hwloc_topology_set_xml((hwloc_topology_t)pmix_globals.topology.topology,
                                        mca_ploc_hwloc_component.topo_file)) {
            return PMIX_ERR_NOT_SUPPORTED;
        }
        /* since we are loading this from an external source, we have to
         * explicitly set a flag so hwloc sets things up correctly
         */
        if (0 != set_flags(pmix_globals.topology.topology,
                           HWLOC_TOPOLOGY_FLAG_IS_THISSYSTEM)) {
            hwloc_topology_destroy(pmix_globals.topology.topology);
            return PMIX_ERROR;
        }
        /* now load the topology */
        if (0 != hwloc_topology_load(pmix_globals.topology.topology)) {
            hwloc_topology_destroy(pmix_globals.topology.topology);
            return PMIX_ERROR;
        }
    } else {
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
    }
    pmix_globals.topology.source = strdup("hwloc");

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
    if (0 == hwloc_topology_export_xmlbuffer(pmix_globals.topology.topology, &xmlbuffer, &len)) {
        kv = PMIX_NEW(pmix_kval_t);
        kv->key = strdup(PMIX_HWLOC_XML_V1);
        kv->value = (pmix_value_t*)malloc(sizeof(pmix_value_t));
        PMIX_VALUE_LOAD(kv->value, xmlbuffer, PMIX_STRING);
        pmix_list_append(&pmix_server_globals.gdata, &kv->super);
        /* save it with the deprecated key for older RMs */
        kv = PMIX_NEW(pmix_kval_t);
        kv->key = strdup(PMIX_LOCAL_TOPO);
        kv->value = (pmix_value_t*)malloc(sizeof(pmix_value_t));
        PMIX_VALUE_LOAD(kv->value, xmlbuffer, PMIX_STRING);
        pmix_list_append(&pmix_server_globals.gdata, &kv->super);
        /* done with the buffer */
        hwloc_free_xmlbuffer(pmix_globals.topology.topology, xmlbuffer);
    }
    /* we don't have the ability to do shared memory, so we are done */
    return PMIX_SUCCESS;
#else
    /* pass the topology as a v2 xml string */
    if (0 == hwloc_topology_export_xmlbuffer(pmix_globals.topology.topology, &xmlbuffer, &len, 0)) {
        kv = PMIX_NEW(pmix_kval_t);
        kv->key = strdup(PMIX_HWLOC_XML_V2);
        kv->value = (pmix_value_t*)malloc(sizeof(pmix_value_t));
        PMIX_VALUE_LOAD(kv->value, xmlbuffer, PMIX_STRING);
        pmix_list_append(&pmix_server_globals.gdata, &kv->super);
        /* save it with the deprecated key for older RMs */
        kv = PMIX_NEW(pmix_kval_t);
        kv->key = strdup(PMIX_LOCAL_TOPO);
        kv->value = (pmix_value_t*)malloc(sizeof(pmix_value_t));
        PMIX_VALUE_LOAD(kv->value, xmlbuffer, PMIX_STRING);
        pmix_list_append(&pmix_server_globals.gdata, &kv->super);
        hwloc_free_xmlbuffer(pmix_globals.topology.topology, xmlbuffer);
    }
    /* and as a v1 xml string, should an older client attach */
    if (0 == hwloc_topology_export_xmlbuffer(pmix_globals.topology.topology, &xmlbuffer, &len, HWLOC_TOPOLOGY_EXPORT_XML_FLAG_V1)) {
        kv = PMIX_NEW(pmix_kval_t);
        kv->key = strdup(PMIX_HWLOC_XML_V1);
        kv->value = (pmix_value_t*)malloc(sizeof(pmix_value_t));
        PMIX_VALUE_LOAD(kv->value, xmlbuffer, PMIX_STRING);
        hwloc_free_xmlbuffer(pmix_globals.topology.topology, xmlbuffer);
        pmix_list_append(&pmix_server_globals.gdata, &kv->super);
        /* cannot support the deprecated key here as it would overwrite the HWLOC v2 string */
    }

    /* get the size of the topology shared memory segment */
    if (0 != hwloc_shmem_topology_get_length(pmix_globals.topology.topology, &shmemsize, 0)) {
        pmix_output_verbose(2, pmix_ploc_base_framework.framework_output,
                            "%s hwloc topology shmem not available",
                            PMIX_NAME_PRINT(&pmix_globals.myid));
        return PMIX_SUCCESS;
    }
    /* try and find a hole */
    rc = pmix_vmhole_find(shmemsize, "hwloc.shm", &shmemfile, &shmemaddr);
    if (PMIX_SUCCESS == rc) {
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
    }
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
    kv = (pmix_kval_t*)pmix_list_remove_first(kvs);
    while (NULL != kv) {
        PMIX_RELEASE(kv);
        kv = (pmix_kval_t*)pmix_list_remove_first(kvs);
    }
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
    kv = (pmix_kval_t*)pmix_list_remove_first(kvs);
    while (NULL != kv) {
        PMIX_RELEASE(kv);
        kv = (pmix_kval_t*)pmix_list_remove_first(kvs);
    }
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

#if HWLOC_API_VERSION >= 0x20000
    int fd;
    uint64_t addr, size;

    /* first try to get the shmem link, if available */
    cb.key = PMIX_HWLOC_SHMEM_FILE;
    PMIX_GDS_FETCH_KV(rc, pmix_client_globals.myserver, &cb);
    if (PMIX_SUCCESS != rc) {
        cb.key = NULL;
        PMIX_DESTRUCT(&cb);
        goto tryxml;
    }
    file = popstr(&cb);

    cb.key = PMIX_HWLOC_SHMEM_ADDR;
    PMIX_GDS_FETCH_KV(rc, pmix_client_globals.myserver, &cb);
    if (PMIX_SUCCESS != rc) {
        cb.key = NULL;
        PMIX_DESTRUCT(&cb);
        free(file);
        goto tryxml;
    }
    addr = popsize(&cb);

    cb.key = PMIX_HWLOC_SHMEM_SIZE;
    PMIX_GDS_FETCH_KV(rc, pmix_client_globals.myserver, &cb);
    if (PMIX_SUCCESS != rc) {
        cb.key = NULL;
        PMIX_DESTRUCT(&cb);
        free(file);
        goto tryxml;
    }
    size = popsize(&cb);
    cb.key = NULL;
    PMIX_DESTRUCT(&cb);

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
    PMIX_CONSTRUCT(&cb, pmix_cb_t);
    cb.key = PMIX_HWLOC_XML_V2;
    PMIX_GDS_FETCH_KV(rc, pmix_client_globals.myserver, &cb);
    if (PMIX_SUCCESS == rc) {
        file = popstr(&cb);
        rc = load_xml(file);
        free(file);
        cb.key = NULL;
        PMIX_DESTRUCT(&cb);
        return rc;
    }

#endif

    /* try to get the v1 XML string */
    cb.key = PMIX_HWLOC_XML_V1;
    PMIX_GDS_FETCH_KV(rc, pmix_client_globals.myserver, &cb);
    if (PMIX_SUCCESS == rc) {
        file = popstr(&cb);
        rc = load_xml(file);
        free(file);
        cb.key = NULL;
        PMIX_DESTRUCT(&cb);
        return rc;
    }
    /* if we got here, then we couldn't find anything */
    cb.key = NULL;
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
    char *tmp;

    if (NULL == cpuset || NULL == cpuset->bitmap) {
        *cpuset_string = NULL;
        return PMIX_ERR_BAD_PARAM;
    }

    /* if we aren't the source, then nothing we can do */
    if (0 != strcasecmp(cpuset->source, "hwloc")) {
        return PMIX_ERR_TAKE_NEXT_OPTION;
    }

    hwloc_bitmap_list_asprintf(&tmp, cpuset->bitmap);
    pmix_asprintf(cpuset_string, "hwloc:%s", tmp);
    free(tmp);

    return PMIX_SUCCESS;
}

static pmix_status_t parse_cpuset_string(const char *cpuset_string,
                                         pmix_cpuset_t *cpuset)
{
    char *src;

    /* if we aren't the source, then pass */
    src = strchr(cpuset_string, ':');
    if (NULL == src) {
        /* bad string */
        return PMIX_ERR_BAD_PARAM;
    }
    *src = '\0';
    if (0 != strcasecmp(cpuset_string, "hwloc")) {
        *src = ':';
        return PMIX_ERR_TAKE_NEXT_OPTION;
    }
    *src = ':';
    ++src;

    cpuset->source = strdup("hwloc");
    cpuset->bitmap = hwloc_bitmap_alloc();
    hwloc_bitmap_list_sscanf(cpuset->bitmap, src);

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

static pmix_status_t get_cpuset(pmix_cpuset_t *cpuset,
                                pmix_bind_envelope_t ref)
{
    int rc, flag;

    if (NULL != cpuset->source && 0 != strcasecmp(cpuset->source, "hwloc")) {
        return PMIX_ERR_TAKE_NEXT_OPTION;
    }

    if (PMIX_CPUBIND_PROCESS == ref) {
        flag = HWLOC_CPUBIND_PROCESS;
    } else if (PMIX_CPUBIND_THREAD == ref) {
        flag = HWLOC_CPUBIND_THREAD;
    } else {
        return PMIX_ERR_BAD_PARAM;
    }

    cpuset->bitmap = hwloc_bitmap_alloc();
    if (NULL != mca_ploc_hwloc_component.testcpuset) {
        rc = hwloc_bitmap_sscanf(cpuset->bitmap, mca_ploc_hwloc_component.testcpuset);
    } else {
        rc = hwloc_get_cpubind(pmix_globals.topology.topology, cpuset->bitmap, flag);
    }
    if (0 != rc) {
        hwloc_bitmap_free(cpuset->bitmap);
        return PMIX_ERR_TAKE_NEXT_OPTION;
    }
    if (NULL == cpuset->source) {
        cpuset->source = strdup("hwloc");
    }

    return PMIX_SUCCESS;
}

static hwloc_obj_t dsearch(hwloc_topology_t t, int depth,
                           hwloc_cpuset_t cpuset)
{
    hwloc_obj_t obj;
    unsigned width, w;

    /* get the width of the topology at this depth */
    width = hwloc_get_nbobjs_by_depth(t, depth);
    if (0 == width) {
        return NULL;
    }
    /* scan all objects at this depth to see if
     * the location is under one of them
     */
    for (w=0; w < width; w++) {
        /* get the object at this depth/index */
        obj = hwloc_get_obj_by_depth(t, depth, w);
        /* if this object doesn't have a cpuset, then ignore it */
        if (NULL == obj->cpuset) {
            continue;
        }
        /* see if the provided cpuset is completely included in this object */
        if (hwloc_bitmap_isincluded(cpuset, obj->cpuset)) {
            return obj;
        }
    }

    return NULL;
}

typedef struct {
    pmix_list_item_t super;
    pmix_device_distance_t dist;
} pmix_devdist_item_t;
static void dvcon(pmix_devdist_item_t *p)
{
    PMIX_DEVICE_DIST_CONSTRUCT(&p->dist);
}
static void dvdes(pmix_devdist_item_t *p)
{
    PMIX_DEVICE_DIST_DESTRUCT(&p->dist);
}
static PMIX_CLASS_INSTANCE(pmix_devdist_item_t,
                           pmix_list_item_t,
                           dvcon, dvdes);

typedef struct {
    hwloc_obj_osdev_type_t hwtype;
    pmix_device_type_t pxtype;
    char *name;
} pmix_type_conversion_t;

static pmix_type_conversion_t table[] =
{
    {.hwtype = HWLOC_OBJ_OSDEV_BLOCK, .pxtype = PMIX_DEVTYPE_BLOCK, .name = "BLOCK"},
    {.hwtype = HWLOC_OBJ_OSDEV_GPU, .pxtype = PMIX_DEVTYPE_GPU, .name = "GPU"},
    {.hwtype = HWLOC_OBJ_OSDEV_NETWORK, .pxtype = PMIX_DEVTYPE_NETWORK, .name = "NETWORK"},
    {.hwtype = HWLOC_OBJ_OSDEV_OPENFABRICS, .pxtype = PMIX_DEVTYPE_OPENFABRICS, .name = "OPENFABRICS"},
    {.hwtype = HWLOC_OBJ_OSDEV_DMA, .pxtype = PMIX_DEVTYPE_DMA, .name = "DMA"},
    {.hwtype = HWLOC_OBJ_OSDEV_COPROC, .pxtype = PMIX_DEVTYPE_COPROC, .name = "COPROCESSOR"},
};

static int countcolons(char *str)
{
    int cnt=0;
    char *p;

    p = strchr(str, ':');
    while (NULL != p) {
        ++cnt;
        ++p;
        p = strchr(p, ':');
    }

    return cnt;
}

static pmix_status_t compute_distances(pmix_topology_t *topo,
                                       pmix_cpuset_t *cpuset,
                                       pmix_info_t info[], size_t ninfo,
                                       pmix_device_distance_t **dist,
                                       size_t *ndist)
{
    hwloc_obj_t obj = NULL;
    hwloc_obj_t tgt;
    hwloc_obj_t device;
    hwloc_obj_t ancestor;
    hwloc_obj_t pu;
    unsigned dp, depth;
    unsigned maxdist = 0;
    unsigned mindist = UINT_MAX;
    unsigned i;
    pmix_list_t dists;
    pmix_devdist_item_t *d;
    pmix_device_distance_t *array;
    size_t n, ntypes, dn;
    int cnt;
    unsigned w, width, pudepth;
    pmix_device_type_t type=0;
    char **devids = NULL;
    bool found;

    if (NULL == topo->source || NULL == cpuset->source) {
        return PMIX_ERR_BAD_PARAM;
    }

    if (0 != strcasecmp(topo->source, "hwloc") || 0 != strcasecmp(cpuset->source, "hwloc")) {
        return PMIX_ERR_TAKE_NEXT_OPTION;
    }

    /* set default returns */
    *dist = NULL;
    *ndist = 0;

    /* determine number of types we support */
    ntypes = sizeof(table) / sizeof(pmix_type_conversion_t);

    /* determine what they want us to look at */
    if (NULL == info) {
        /* find everything */
        for (n=0; n < ntypes; n++) {
            type |= table[n].pxtype;
        }
    } else {
        for (n=0; n < ninfo; n++) {
            if (PMIX_CHECK_KEY(&info[n], PMIX_DEVICE_TYPE)) {
                type |= info[n].value.data.devtype;
            } else if (PMIX_CHECK_KEY(&info[n], PMIX_DEVICE_ID)) {
                pmix_argv_append_nosize(&devids, info[n].value.data.string);
            }
        }
    }

    /* find the max depth of this topology */
    depth = hwloc_topology_get_depth(topo->topology);
    /* get the lowest object that completely covers the cpuset */
    for (dp=1; dp < depth; dp++) {
        tgt = dsearch(topo->topology, dp, cpuset->bitmap);
        if (NULL == tgt) {
            /* nothing found at that depth, so we are done */
            break;
        }
        obj = tgt;
    }
    if (NULL == obj) {
        /* only the entire machine covers this cpuset - typically,
         * this means we are in some odd container where every
         * PU is in its own package. There is nothing useful
         * that can be done here */
        return PMIX_ERR_NOT_AVAILABLE;
    }
    /* get the PU depth */
    pudepth = (unsigned)hwloc_get_type_depth(topo->topology, HWLOC_OBJ_PU);
    width = hwloc_get_nbobjs_by_depth(topo->topology, pudepth);

    PMIX_CONSTRUCT(&dists, pmix_list_t);

    /* loop over the specified devices in the topology */
    for (n=0; n < ntypes; n++) {
        if (!(type & table[n].pxtype)) {
            continue;
        }
        if (HWLOC_OBJ_OSDEV_BLOCK == table[n].hwtype ||
            HWLOC_OBJ_OSDEV_DMA == table[n].hwtype ||
            HWLOC_OBJ_OSDEV_COPROC == table[n].hwtype) {
            continue;
        }
        device = hwloc_get_obj_by_type(topo->topology, HWLOC_OBJ_OS_DEVICE, 0);
        while (NULL != device) {
            if (device->attr->osdev.type == table[n].hwtype) {

                d = PMIX_NEW(pmix_devdist_item_t);
                pmix_list_append(&dists, &d->super);

                d->dist.type = table[n].pxtype;

                /* Construct a UUID for this device */
                if (HWLOC_OBJ_OSDEV_NETWORK == table[n].hwtype) {
                    char *addr = NULL;
                    /* find the address */
                    for (i=0; i < device->infos_count; i++) {
                        if (0 == strcasecmp(device->infos[i].name, "Address")) {
                            addr = device->infos[i].value;
                            break;
                        }
                    }
                    if (NULL == addr) {
                        /* couldn't find an address - report it as an error */
                        PMIX_LIST_DESTRUCT(&dists);
                        return PMIX_ERROR;
                    }
                    /* could be IPv4 or IPv6 */
                    cnt = countcolons(addr);
                    if (5 == cnt) {
                        pmix_asprintf(&d->dist.uuid, "ipv4://%s", addr);
                    } else if (19 == cnt) {
                        pmix_asprintf(&d->dist.uuid, "ipv6://%s", addr);
                    } else {
                        /* unknown address type */
                        PMIX_LIST_DESTRUCT(&dists);
                        return PMIX_ERROR;
                    }
                } else if (HWLOC_OBJ_OSDEV_OPENFABRICS == table[n].hwtype) {
                    char *ngid = NULL;
                    char *sgid = NULL;
                    /* find the UIDs */
                    for (i=0; i < device->infos_count; i++) {
                        if (0 == strcasecmp(device->infos[i].name, "NodeGUID")) {
                            ngid = device->infos[i].value;
                        } else if (0 == strcasecmp(device->infos[i].name, "SysImageGUID")) {
                            sgid = device->infos[i].value;
                        }
                    }
                    if (NULL == ngid || NULL == sgid) {
                        PMIX_LIST_DESTRUCT(&dists);
                        return PMIX_ERROR;
                    }
                    pmix_asprintf(&d->dist.uuid, "fab://%s::%s", ngid, sgid);
                } else if (HWLOC_OBJ_OSDEV_GPU == table[n].hwtype) {
                    /* if the name starts with "card", then this is just the aux card of the GPU */
                    if (0 == strncasecmp(device->name, "card", 4)) {
                        pmix_list_remove_item(&dists, &d->super);
                        PMIX_RELEASE(d);
                        device = hwloc_get_next_osdev(topo->topology, device);
                        continue;
                    }
                    pmix_asprintf(&d->dist.uuid, "gpu://%s::%s", pmix_globals.hostname, device->name);
                } else {
                    /* unknown type */
                    pmix_list_remove_item(&dists, &d->super);
                    PMIX_RELEASE(d);
                    device = hwloc_get_next_osdev(topo->topology, device);
                    continue;
                }

                /* if device id was given, then check if this one matches either
                 * the UUID or osname */
                if (NULL != devids) {
                    found = false;
                    for (dn=0; NULL != devids[dn]; dn++) {
                        if (0 == strcasecmp(devids[dn], device->name) ||
                            0 == strcasecmp(devids[dn], d->dist.uuid)) {
                            found = true;
                        }
                    }
                    if (!found) {
                        /* skip this one */
                        pmix_list_remove_item(&dists, &d->super);
                        PMIX_RELEASE(d);
                        device = hwloc_get_next_osdev(topo->topology, device);
                        continue;
                    }
                }
                /* save the osname */
                d->dist.osname = strdup(device->name);
                if (NULL == device->cpuset) {
                    /* climb the topology until we find a non-NULL cpuset */
                    tgt = device->parent;
                    while (NULL != tgt && NULL == tgt->cpuset) {
                        tgt = tgt->parent;
                    }
                    if (NULL == tgt) {
                        PMIX_LIST_DESTRUCT(&dists);
                        return PMIX_ERR_NOT_FOUND;
                    }
                } else {
                    tgt = device;
                }
                /* loop over the PUs on this node */
                maxdist = 0;
                mindist = UINT_MAX;
                for (w=0; w < width; w++) {
                    /* get the pu at this index */
                    pu = hwloc_get_obj_by_depth(topo->topology, pudepth, w);
                    /* is this PU in our cpuset? */
                    if (!hwloc_bitmap_intersects(pu->cpuset, cpuset->bitmap)) {
                        continue;
                    }
                    /* find the common ancestor between the cpuset and NIC objects */
                    ancestor = hwloc_get_common_ancestor_obj(topo->topology, obj, tgt);
                    if (NULL != ancestor) {
                        if (0 == ancestor->depth) {
                            /* we only share the machine - need to do something more
                             * to compute the distance. This can, however, get a little
                             * hairy as there is no good measure of package-to-package
                             * distance - it is all typically given in terms of NUMA
                             * domains, which is no longer a valid way of looking at
                             * locations due to overlapping domains. For now, we will
                             * just take the depth of the device in its package and
                             * add that to the depth of the object in its package
                             * plus the depth of a package to ensure it is further away */
                            dp = obj->depth + depth;
                        } else {
                            /* the depth value can be used as an indicator of relative
                             * locality - the higher the value, the closer the device.
                             * We invert the pyramid to set the dist to be closer for
                             * smaller values */
                            dp = depth - ancestor->depth;
                        }
                    } else {
                        /* shouldn't happen - consider this an error condition */
                        PMIX_LIST_DESTRUCT(&dists);
                        return PMIX_ERROR;
                    }
                    if (mindist > dp) {
                        mindist = dp;
                    }
                    if (maxdist < dp) {
                        maxdist = dp;
                    }
                }
                d->dist.mindist = mindist;
                d->dist.maxdist = maxdist;
            }
            device = hwloc_get_next_osdev(topo->topology, device);
        }
    }

    /* create the return array */
    n = pmix_list_get_size(&dists);
    if (0 == n) {
        /* no devices found */
        return PMIX_ERR_NOT_FOUND;
    }
    PMIX_DEVICE_DIST_CREATE(array, n);
    *ndist = n;
    n = 0;
    PMIX_LIST_FOREACH(d, &dists, pmix_devdist_item_t) {
        array[n].uuid = strdup(d->dist.uuid);
        array[n].osname = strdup(d->dist.osname);
        array[n].type = d->dist.type;
        array[n].mindist = d->dist.mindist;
        array[n].maxdist = d->dist.maxdist;
        ++n;
    }
    PMIX_LIST_DESTRUCT(&dists);
    *dist = array;

    return PMIX_SUCCESS;
}

static pmix_status_t pack_cpuset(pmix_buffer_t *buf, pmix_cpuset_t *src,
                                 pmix_pointer_array_t *regtypes)
{
    char *tmp;
    pmix_status_t rc;

    if (NULL == src->source || 0 != strcasecmp(src->source, "hwloc")) {
        return PMIX_ERR_TAKE_NEXT_OPTION;
    }

    /* express the cpuset as a string */
    if (0 != hwloc_bitmap_list_asprintf(&tmp, src->bitmap)) {
        return PMIX_ERROR;
    }

    /* pack the string */
    PMIX_BFROPS_PACK_TYPE(rc, buf, &tmp, 1, PMIX_STRING, regtypes);
    free(tmp);

    return rc;
}

static pmix_status_t unpack_cpuset(pmix_buffer_t *buf, pmix_cpuset_t *dest,
                                   pmix_pointer_array_t *regtypes)
{
    pmix_status_t rc;
    int cnt;
    char *tmp;

    /* unpack the cpustring */
    cnt=1;
    PMIX_BFROPS_UNPACK_TYPE(rc, buf, &tmp, &cnt, PMIX_STRING, regtypes);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }

    /* convert to a bitmap */
    dest->source = strdup("hwloc");
    dest->bitmap = hwloc_bitmap_alloc();
    hwloc_bitmap_list_sscanf(dest->bitmap, tmp);
    free(tmp);

    return PMIX_SUCCESS;
}

static pmix_status_t copy_cpuset(pmix_cpuset_t *dest, pmix_cpuset_t *src)
{
    if (NULL == src->source || 0 != strcasecmp(src->source, "hwloc")) {
        return PMIX_ERR_TAKE_NEXT_OPTION;
    }
    if (NULL == src->bitmap) {
        return PMIX_ERR_BAD_PARAM;
    }

    /* copy the src bitmap */
    dest->bitmap = hwloc_bitmap_dup(src->bitmap);
    dest->source = strdup("hwloc");

    return PMIX_SUCCESS;
}

static char* print_cpuset(pmix_cpuset_t *src)
{
    char *tmp;

    if (NULL == src->source || 0 != strcasecmp(src->source, "hwloc")) {
        return NULL;
    }
    if (NULL == src->bitmap) {
        return NULL;
    }

    /* express the cpuset as a string */
    if (0 != hwloc_bitmap_list_asprintf(&tmp, src->bitmap)) {
        return NULL;
    }

    return tmp;
}

static pmix_status_t destruct_cpuset(pmix_cpuset_t *src)
{
    if (NULL == src->source || 0 != strcasecmp(src->source, "hwloc")) {
        return PMIX_ERR_TAKE_NEXT_OPTION;
    }
    if (NULL == src->bitmap) {
        return PMIX_ERR_BAD_PARAM;
    }

    hwloc_bitmap_free(src->bitmap);
    src->bitmap = NULL;
    free(src->source);

    return PMIX_SUCCESS;
}

static pmix_status_t release_cpuset(pmix_cpuset_t *src, size_t ncpu)
{
    size_t n;

    if (NULL == src->source || 0 != strcasecmp(src->source, "hwloc")) {
        return PMIX_ERR_TAKE_NEXT_OPTION;
    }

    for (n=0; n < ncpu; n++) {
        destruct_cpuset(&src[n]);
    }
    free(src);

    return PMIX_SUCCESS;
}


static pmix_status_t pack_topology(pmix_buffer_t *buf, pmix_topology_t *src,
                                   pmix_pointer_array_t *regtypes)
{
    /* NOTE: hwloc defines topology_t as a pointer to a struct! */
    pmix_status_t rc;
    char *xmlbuffer=NULL;
    int len;
    struct hwloc_topology_support *support;

    if (NULL == src->source || 0 != strcasecmp(src->source, "hwloc")) {
        return PMIX_ERR_TAKE_NEXT_OPTION;
    }

    /* extract an xml-buffer representation of the tree */
#if HWLOC_API_VERSION < 0x20000
    if (0 != hwloc_topology_export_xmlbuffer(src->topology, &xmlbuffer, &len)) {
        return PMIX_ERROR;
    }
#else
    if (0 != hwloc_topology_export_xmlbuffer(src->topology, &xmlbuffer, &len, 0)) {
        return PMIX_ERROR;
    }
#endif

    /* add to buffer */
    PMIX_BFROPS_PACK_TYPE(rc, buf, &xmlbuffer, 1, PMIX_STRING, regtypes);
    free(xmlbuffer);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }

    /* get the available support - hwloc unfortunately does
     * not include this info in its xml export!
     */
    support = (struct hwloc_topology_support*)hwloc_topology_get_support(src->topology);
    /* pack the discovery support */
    PMIX_BFROPS_PACK_TYPE(rc, buf, support->discovery,
                          sizeof(struct hwloc_topology_discovery_support),
                          PMIX_BYTE, regtypes);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    /* pack the cpubind support */
    PMIX_BFROPS_PACK_TYPE(rc, buf, support->cpubind,
                          sizeof(struct hwloc_topology_cpubind_support),
                          PMIX_BYTE, regtypes);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }

    /* pack the membind support */
    PMIX_BFROPS_PACK_TYPE(rc, buf, support->membind,
                          sizeof(struct hwloc_topology_membind_support),
                          PMIX_BYTE, regtypes);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }

    return PMIX_SUCCESS;
}

static pmix_status_t unpack_topology(pmix_buffer_t *buf, pmix_topology_t *dest,
                                     pmix_pointer_array_t *regtypes)
{
    /* NOTE: hwloc defines topology_t as a pointer to a struct! */
    pmix_status_t rc;
    char *xmlbuffer=NULL;
    int cnt;
    struct hwloc_topology_support *support;
    hwloc_topology_t t;
    unsigned long flags;

    /* unpack the xml string */
    cnt=1;
    PMIX_BFROPS_UNPACK_TYPE(rc, buf, &xmlbuffer, &cnt, PMIX_STRING, regtypes);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }

    /* convert the xml */
    if (0 != hwloc_topology_init(&t)) {
        rc = PMIX_ERROR;
        free(xmlbuffer);
        return rc;
    }
    if (0 != hwloc_topology_set_xmlbuffer(t, xmlbuffer, strlen(xmlbuffer))) {
        rc = PMIX_ERROR;
        free(xmlbuffer);
        hwloc_topology_destroy(t);
        return rc;
    }
    free(xmlbuffer);

    /* since we are loading this from an external source, we have to
     * explicitly set a flag so hwloc sets things up correctly
     */
    flags = HWLOC_TOPOLOGY_FLAG_IS_THISSYSTEM;
#if HWLOC_API_VERSION < 0x00020000
        flags |= HWLOC_TOPOLOGY_FLAG_WHOLE_SYSTEM;
        flags |= HWLOC_TOPOLOGY_FLAG_IO_DEVICES;
#else
        if (0 != hwloc_topology_set_io_types_filter(t, HWLOC_TYPE_FILTER_KEEP_IMPORTANT)) {
            hwloc_topology_destroy(t);
            return PMIX_ERROR;
        }
#if HWLOC_API_VERSION < 0x00020100
        flags |= HWLOC_TOPOLOGY_FLAG_WHOLE_SYSTEM;
#else
        flags |= HWLOC_TOPOLOGY_FLAG_INCLUDE_DISALLOWED;
#endif
#endif
    if (0 != hwloc_topology_set_flags(t, flags)) {
        hwloc_topology_destroy(t);
        return PMIX_ERROR;
    }
    /* now load the topology */
    if (0 != hwloc_topology_load(t)) {
        hwloc_topology_destroy(t);
        return PMIX_ERROR;
    }

    /* get the available support - hwloc unfortunately does
     * not include this info in its xml import!
     */
    support = (struct hwloc_topology_support*)hwloc_topology_get_support(t);
    cnt = sizeof(struct hwloc_topology_discovery_support);
    PMIX_BFROPS_UNPACK_TYPE(rc, buf, support->discovery, &cnt, PMIX_BYTE, regtypes);
    if (PMIX_SUCCESS != rc) {
        hwloc_topology_destroy(t);
        return PMIX_ERROR;
    }
    cnt = sizeof(struct hwloc_topology_cpubind_support);
    PMIX_BFROPS_UNPACK_TYPE(rc, buf, support->cpubind, &cnt, PMIX_BYTE, regtypes);
    if (PMIX_SUCCESS != rc) {
        hwloc_topology_destroy(t);
        return PMIX_ERROR;
    }
    cnt = sizeof(struct hwloc_topology_membind_support);
    PMIX_BFROPS_UNPACK_TYPE(rc, buf, support->membind, &cnt, PMIX_BYTE, regtypes);
    if (PMIX_SUCCESS != rc) {
        hwloc_topology_destroy(t);
        return PMIX_ERROR;
    }

    dest->source = strdup("hwloc");
    dest->topology = (void*)t;

    return PMIX_SUCCESS;
}

static pmix_status_t copy_topology(pmix_topology_t *dest, pmix_topology_t *src)
{
    if (NULL == src->source|| 0 != strcasecmp(src->source, "hwloc")) {
        return PMIX_ERR_TAKE_NEXT_OPTION;
    }

#if PMIX_HAVE_HWLOC_TOPOLOGY_DUP
    /* use the hwloc dup function */
    if (0 != hwloc_topology_dup((hwloc_topology_t*)&dest->topology, src->topology)) {
        return PMIX_ERROR;
    }
    dest->source = strdup("hwloc");

    return PMIX_SUCCESS;
#else
    /* we have to do this in a convoluted manner */
    char *xmlbuffer=NULL;
    int len;
    struct hwloc_topology_support *srcsup, *destsup;
    pmix_status_t rc;

    /* extract an xml-buffer representation of the tree */
#if HWLOC_API_VERSION < 0x20000
    if (0 != hwloc_topology_export_xmlbuffer(src->topology, &xmlbuffer, &len)) {
        return PMIX_ERROR;
    }
#else
    if (0 != hwloc_topology_export_xmlbuffer(src->topology, &xmlbuffer, &len, 0)) {
        return PMIX_ERROR;
    }
#endif

    /* convert the xml back */
    if (0 != hwloc_topology_init((hwloc_topology_t*)&dest->topology)) {
        rc = PMIX_ERROR;
        free(xmlbuffer);
        return rc;
    }
    if (0 != hwloc_topology_set_xmlbuffer(dest->topology, xmlbuffer, strlen(xmlbuffer))) {
        rc = PMIX_ERROR;
        free(xmlbuffer);
        hwloc_topology_destroy(dest->topology);
        return rc;
    }
    free(xmlbuffer);

    /* transfer the support struct */
    srcsup = (struct hwloc_topology_support*)hwloc_topology_get_support(src->topology);
    destsup = (struct hwloc_topology_support*)hwloc_topology_get_support(dest->topology);
    memcpy(destsup, srcsup, sizeof(struct hwloc_topology_support));

    return PMIX_SUCCESS;
#endif
}

#define PMIX_HWLOC_MAX_STRING   2048

static void print_hwloc_obj(char **output, char *prefix,
                            hwloc_topology_t topo, hwloc_obj_t obj)
{
    hwloc_obj_t obj2;
    char string[1024], *tmp, *tmp2, *pfx;
    unsigned i;
    struct hwloc_topology_support *support;

    /* print the object type */
    hwloc_obj_type_snprintf(string, 1024, obj, 1);
    pmix_asprintf(&pfx, "\n%s\t", (NULL == prefix) ? "" : prefix);
    pmix_asprintf(&tmp, "%sType: %s Number of child objects: %u%sName=%s",
             (NULL == prefix) ? "" : prefix, string, obj->arity,
             pfx, (NULL == obj->name) ? "NULL" : obj->name);
    if (0 < hwloc_obj_attr_snprintf(string, 1024, obj, pfx, 1)) {
        /* print the attributes */
        pmix_asprintf(&tmp2, "%s%s%s", tmp, pfx, string);
        free(tmp);
        tmp = tmp2;
    }
    /* print the cpusets - apparently, some new HWLOC types don't
     * have cpusets, so protect ourselves here
     */
    if (NULL != obj->cpuset) {
        hwloc_bitmap_snprintf(string, PMIX_HWLOC_MAX_STRING, obj->cpuset);
        pmix_asprintf(&tmp2, "%s%sCpuset:  %s", tmp, pfx, string);
        free(tmp);
        tmp = tmp2;
    }
    if (HWLOC_OBJ_MACHINE == obj->type) {
        /* root level object - add support values */
        support = (struct hwloc_topology_support*)hwloc_topology_get_support(topo);
        pmix_asprintf(&tmp2, "%s%sBind CPU proc:   %s%sBind CPU thread: %s", tmp, pfx,
                 (support->cpubind->set_thisproc_cpubind) ? "TRUE" : "FALSE", pfx,
                 (support->cpubind->set_thisthread_cpubind) ? "TRUE" : "FALSE");
        free(tmp);
        tmp = tmp2;
        pmix_asprintf(&tmp2, "%s%sBind MEM proc:   %s%sBind MEM thread: %s", tmp, pfx,
                 (support->membind->set_thisproc_membind) ? "TRUE" : "FALSE", pfx,
                 (support->membind->set_thisthread_membind) ? "TRUE" : "FALSE");
        free(tmp);
        tmp = tmp2;
    }
    pmix_asprintf(&tmp2, "%s%s\n", (NULL == *output) ? "" : *output, tmp);
    free(tmp);
    free(pfx);
    pmix_asprintf(&pfx, "%s\t", (NULL == prefix) ? "" : prefix);
    for (i=0; i < obj->arity; i++) {
        obj2 = obj->children[i];
        /* print the object */
        print_hwloc_obj(&tmp2, pfx, topo, obj2);
    }
    free(pfx);
    if (NULL != *output) {
        free(*output);
    }
    *output = tmp2;
}

static char* print_topology(pmix_topology_t *src)
{
    hwloc_obj_t obj;
    char *tmp=NULL;

    if (NULL == src->source|| 0 != strcasecmp(src->source, "hwloc")) {
        return NULL;
    }

    /* get root object */
    obj = hwloc_get_root_obj(src->topology);
    /* print it */
    print_hwloc_obj(&tmp, NULL, src->topology, obj);
    return tmp;
}

static pmix_status_t destruct_topology(pmix_topology_t *src)
{
    if (NULL == src->source|| 0 != strcasecmp(src->source, "hwloc")) {
        return PMIX_ERR_TAKE_NEXT_OPTION;
    }
    if (NULL == src->topology) {
        return PMIX_ERR_BAD_PARAM;
    }
    hwloc_topology_destroy(src->topology);
    free(src->source);

    return PMIX_SUCCESS;
}

static pmix_status_t release_topology(pmix_topology_t *src, size_t ncpu)
{
    size_t n;

    if (NULL == src->source|| 0 != strcasecmp(src->source, "hwloc")) {
        return PMIX_ERR_TAKE_NEXT_OPTION;
    }

    for (n=0; n < ncpu; n++) {
        destruct_topology(&src[n]);
    }
    free(src);

    return PMIX_SUCCESS;
}
