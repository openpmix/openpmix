/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2016      Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2018      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * Copyright (c) 2022      Triad National Security, LLC. All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "src/include/pmix_config.h"

#include <string.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif
#ifdef HAVE_SYS_TYPES_H
#    include <sys/types.h>
#endif
#ifdef HAVE_SYS_STAT_H
#    include <sys/stat.h>
#endif
#ifdef HAVE_FCNTL_H
#    include <fcntl.h>
#endif
#ifdef HAVE_SYS_UTSNAME_H
#    include <sys/utsname.h>
#endif
#include <time.h>
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>

#include <hwloc.h>

#include "src/class/pmix_list.h"
#include "src/client/pmix_client_ops.h"
#include "src/include/pmix_globals.h"
#include "src/mca/bfrops/base/base.h"
#include "src/mca/pnet/pnet.h"
#include "src/server/pmix_server_ops.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_fd.h"
#include "src/util/pmix_name_fns.h"
#include "src/util/pmix_path.h"
#include "src/util/pmix_printf.h"
#include "src/util/pmix_show_help.h"
#include "src/util/pmix_vmem.h"

#include "pmix_common.h"

#include "pmix_hwloc.h"
#include <hwloc/shmem.h>

static bool passed_thru = false;
static char *vmhole = "biggest";
static pmix_vmem_hole_kind_t hole_kind = VMEM_HOLE_BIGGEST;
static char *topo_file = NULL;
static char *testcpuset = NULL;
static int pmix_hwloc_output = -1;
static int pmix_hwloc_verbose = 0;

static size_t shmemsize = 0;
static size_t shmemaddr;
static char *shmemfile = NULL;
static int shmemfd = -1;
static bool space_available = false;
static uint64_t amount_space_avail = 0;

static int enough_space(const char *filename, size_t space_req, uint64_t *space_avail,
                        bool *result);
static pmix_status_t load_xml(char *xml);
static char *popstr(pmix_cb_t *cb);
static size_t popsize(pmix_cb_t *cb);
static void print_maps(void);
static pmix_topology_t *popptr(pmix_cb_t *cb);
static int get_locality_string_by_depth(int d, hwloc_cpuset_t cpuset, hwloc_cpuset_t result);
static int set_flags(hwloc_topology_t topo, unsigned int flags);

pmix_status_t pmix_hwloc_register(void)
{
    (void) pmix_mca_base_var_register("pmix", "pmix", "hwloc", "verbose",
                                      "Verbosity for HWLOC operations",
                                      PMIX_MCA_BASE_VAR_TYPE_INT,
                                      &pmix_hwloc_verbose);
    if (0 < pmix_hwloc_verbose) {
        /* set default output */
        pmix_hwloc_output = pmix_output_open(NULL);
        pmix_output_set_verbosity(pmix_hwloc_output, pmix_hwloc_verbose);
    }

    vmhole = "biggest";
    (void) pmix_mca_base_var_register("pmix", "pmix", "hwloc", "hole_kind",
                                      "Kind of VM hole to identify - none, begin, biggest, libs, heap, stack (default=biggest)",
                                      PMIX_MCA_BASE_VAR_TYPE_STRING, &vmhole);
    if (0 == strcasecmp(vmhole, "none")) {
        hole_kind = VMEM_HOLE_NONE;
    } else if (0 == strcasecmp(vmhole, "begin")) {
        hole_kind = VMEM_HOLE_BEGIN;
    } else if (0 == strcasecmp(vmhole, "biggest")) {
        hole_kind = VMEM_HOLE_BIGGEST;
    } else if (0 == strcasecmp(vmhole, "libs")) {
        hole_kind = VMEM_HOLE_IN_LIBS;
    } else if (0 == strcasecmp(vmhole, "heap")) {
        hole_kind = VMEM_HOLE_AFTER_HEAP;
    } else if (0 == strcasecmp(vmhole, "stack")) {
        hole_kind = VMEM_HOLE_BEFORE_STACK;
    } else {
        pmix_output(0, "INVALID VM HOLE TYPE");
        return PMIX_ERROR;
    }

    (void) pmix_mca_base_var_register("pmix", "pmix", "hwloc", "topo_file",
                                      "Topology file to use instead of discovering it (mostly for testing purposes)",
                                      PMIX_MCA_BASE_VAR_TYPE_STRING, &topo_file);

    (void) pmix_mca_base_var_register("pmix", "pmix", "hwloc", "test_cpuset",
                                      "Cpuset for testing purposes",
                                      PMIX_MCA_BASE_VAR_TYPE_STRING,
                                      &testcpuset);

    return PMIX_SUCCESS;
}

void pmix_hwloc_finalize(void)
{
    /* Reclaim the shmem segment we created, if we created one. This is NOT
     * gated on a "did we adopt a shmem topology" flag, as it once was. That
     * flag was set only on the ADOPT path (the client), and a client never
     * sets shmemfile or shmemfd; the process that owns those two is the server
     * that WROTE the segment in setup_topology, and it never took the adopt
     * path. Gating on the flag therefore made this cleanup unreachable for the
     * only process it applies to, leaking the descriptor and the path string
     * and relying entirely on session-dir teardown to remove the file. Do it
     * unconditionally, and do it before the early-out below so it also runs
     * when the topology itself is externally owned. */
    if (NULL != shmemfile) {
        unlink(shmemfile);
        free(shmemfile);
        shmemfile = NULL;
        shmemsize = 0;
    }
    if (0 <= shmemfd) {
        close(shmemfd);
        shmemfd = -1;
    }

    if (NULL == pmix_globals.topology.topology ||
        pmix_globals.external_topology) {
        return;
    }

    hwloc_topology_destroy(pmix_globals.topology.topology);
    pmix_globals.topology.topology = NULL;
    if (NULL != pmix_globals.topology.source) {
        free(pmix_globals.topology.source);
        pmix_globals.topology.source = NULL;
    }
    return;
}

pmix_status_t pmix_hwloc_setup_topology(pmix_info_t *info, size_t ninfo)
{
    pmix_cb_t cb;
    pmix_proc_t wildcard;
    char *xmlbuffer = NULL;
    int len;
    size_t n;
    pmix_kval_t kv, *kptr;
    pmix_value_t val;
    bool share = false;
    bool found = false;
    pmix_topology_t *topo;
    char *file;
    pmix_status_t rc;

    /* only go thru here ONCE! */
    if (passed_thru) {
        return PMIX_SUCCESS;
    }
    passed_thru = true;

    pmix_output_verbose(2, pmix_hwloc_output,
                        "%s:%s", __FILE__, __func__);

    /* see if they want us to share the topology with our clients */
    for (n = 0; n < ninfo; n++) {
        if (PMIX_CHECK_KEY(&info[n], PMIX_SERVER_SHARE_TOPOLOGY)) {
            share = PMIX_INFO_TRUE(&info[n]);
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_TOPOLOGY2)) {
            if (found) {
                continue;
            }
            /* prefer this option */
            topo = info[n].value.data.topo;
            if (NULL == topo || NULL == topo->topology) {
                /* nothing usable was handed to us */
                continue;
            }
            if (NULL != pmix_globals.topology.source) {
                free(pmix_globals.topology.source);
            }
            /* the host is not required to label the topology it gives us,
             * and strdup(NULL) is undefined - fall back to the unversioned
             * spelling, exactly as the deprecated PMIX_TOPOLOGY path does */
            pmix_globals.topology.source = strdup((NULL == topo->source) ? "hwloc" : topo->source);
            pmix_globals.topology.topology = topo->topology;
            pmix_globals.external_topology = true;
            found = true;
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_TOPOLOGY)) {
            if (!found) { // prefer PMIX_TOPOLOGY2
                if (NULL != pmix_globals.topology.source) {
                    free(pmix_globals.topology.source);
                }
                pmix_globals.topology.source = strdup("hwloc"); // we cannot know the version they used
                pmix_globals.topology.topology = (hwloc_topology_t) info[n].value.data.ptr;
                pmix_globals.external_topology = true;
            }
        }
    }

    if (NULL != pmix_globals.topology.topology) {
        pmix_output_verbose(2, pmix_hwloc_output,
                            "%s:%s topology externally provided", __FILE__, __func__);
        /* record locally in case someone does a PMIx_Get to retrieve it */
        kv.key = PMIX_TOPOLOGY2;
        kv.value = &val;
        val.type = PMIX_TOPO;
        val.data.topo = &pmix_globals.topology;
        PMIX_GDS_STORE_KV(rc, pmix_globals.mypeer, &pmix_globals.myid, PMIX_INTERNAL, &kv);
        pmix_output_verbose(2, pmix_hwloc_output, "%s:%s stored", __FILE__, __func__);
        if (PMIX_SUCCESS != rc) {
            return rc;
        }
        /* if we need to share it, go do that */
        if (share) {
            goto sharetopo;
        }
        /* otherwise, we are done */
        return PMIX_SUCCESS;
    }
    PMIX_LOAD_PROCID(&wildcard, pmix_globals.myid.nspace, PMIX_RANK_WILDCARD);

    /* try to get it ourselves */
    int fd;
    uint64_t addr, size;

    pmix_output_verbose(2, pmix_hwloc_output, "%s:%s checking shmem",
                        __FILE__, __func__);

    /* first try to get the shmem link, if available */
    PMIX_CONSTRUCT(&cb, pmix_cb_t);
    cb.key = PMIX_HWLOC_SHMEM_FILE;
    cb.proc = &wildcard;
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
        /* it may be that a tool has connected to a remote
         * daemon, in which case the file won't be found.
         * Could also be some other error, but let's not
         * treat this as fatal */
        goto tryself;
    }
    free(file);
    rc = hwloc_shmem_topology_adopt((hwloc_topology_t *) &pmix_globals.topology.topology, fd, 0,
                                    (void *) addr, size, 0);
    /* the topology has been mmap'd, so the fd is no longer
     * needed regardless of whether the adopt succeeded */
    close(fd);
    if (0 == rc) {
        pmix_output_verbose(2, pmix_hwloc_output, "%s:%s shmem adopted",
                            __FILE__, __func__);
        /* got it - we are done */
        pmix_asprintf(&pmix_globals.topology.source, "hwloc:%s", HWLOC_VERSION);
        /* record locally in case someone does a PMIx_Get to retrieve it */
        kv.key = PMIX_TOPOLOGY2;
        kv.value = &val;
        val.type = PMIX_TOPO;
        val.data.topo = &pmix_globals.topology;
        PMIX_GDS_STORE_KV(rc, pmix_globals.mypeer, &pmix_globals.myid, PMIX_INTERNAL, &kv);
        pmix_output_verbose(2, pmix_hwloc_output, "%s:%s stored", __FILE__,
                            __func__);
        return PMIX_SUCCESS;
    }

    /* failed to adopt from shmem, so provide some feedback and
     * then fallback to other ways to get the topology */
    if (4 < pmix_output_get_verbosity(pmix_hwloc_output)) {
        print_maps();
    }

tryxml:
    pmix_output_verbose(2, pmix_hwloc_output, "%s:%s checking v2 xml",
                        __FILE__, __func__);

    /* try to get the v2 XML string */
    PMIX_CONSTRUCT(&cb, pmix_cb_t);
    cb.key = PMIX_HWLOC_XML_V2;
    cb.proc = &wildcard;
    PMIX_GDS_FETCH_KV(rc, pmix_client_globals.myserver, &cb);
    if (PMIX_SUCCESS == rc) {
        file = popstr(&cb);
        if (NULL == file) {
            rc = PMIX_ERR_NOT_FOUND;
        } else {
            rc = load_xml(file);
            free(file);
        }
        cb.key = NULL;
        PMIX_DESTRUCT(&cb);
        if (PMIX_SUCCESS != rc) {
            goto tryv1;
        }

        pmix_output_verbose(2, pmix_hwloc_output,
                            "%s:%s v2 xml adopted", __FILE__, __func__);
        /* record locally in case someone does a PMIx_Get to retrieve it */
        kv.key = PMIX_TOPOLOGY2;
        kv.value = &val;
        val.type = PMIX_TOPO;
        val.data.topo = &pmix_globals.topology;
        PMIX_GDS_STORE_KV(rc, pmix_globals.mypeer, &pmix_globals.myid, PMIX_INTERNAL, &kv);
        pmix_output_verbose(2, pmix_hwloc_output, "%s:%s stored",
                            __FILE__, __func__);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
        }
        if (share) {
            goto sharetopo;
        }
        return rc;
    }

tryv1:

    /* try to get the v1 XML string */
    pmix_output_verbose(2, pmix_hwloc_output,
                        "%s:%s checking v1 xml",
                        __FILE__, __func__);

    PMIX_CONSTRUCT(&cb, pmix_cb_t);
    cb.key = PMIX_HWLOC_XML_V1;
    cb.proc = &wildcard;
    PMIX_GDS_FETCH_KV(rc, pmix_client_globals.myserver, &cb);
    if (PMIX_SUCCESS == rc) {
        file = popstr(&cb);
        if (NULL == file) {
            rc = PMIX_ERR_NOT_FOUND;
        } else {
            rc = load_xml(file);
            free(file);
        }
        cb.key = NULL;
        PMIX_DESTRUCT(&cb);
        if (PMIX_SUCCESS != rc) {
            goto tryself;
        }
        pmix_output_verbose(2, pmix_hwloc_output,
                            "%s:%s v1 xml adopted", __FILE__, __func__);

        /* record locally in case someone does a PMIx_Get to retrieve it */
        kv.key = PMIX_TOPOLOGY2;
        kv.value = &val;
        val.type = PMIX_TOPO;
        val.data.topo = &pmix_globals.topology;
        PMIX_GDS_STORE_KV(rc, pmix_globals.mypeer, &pmix_globals.myid, PMIX_INTERNAL, &kv);
        pmix_output_verbose(2, pmix_hwloc_output, "%s:%s stored",
                            __FILE__, __func__);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
        }
        if (share) {
            goto sharetopo;
        }
        return rc;
    }

tryself:
    /* did they give us one to use? */
    if (NULL != topo_file) {
        pmix_output_verbose(2, pmix_hwloc_output,
                            "%s:%s using MCA provided topo file", __FILE__, __func__);

        if (0 != hwloc_topology_init((hwloc_topology_t *) &pmix_globals.topology.topology)) {
            return PMIX_ERR_TAKE_NEXT_OPTION;
        }
        if (0 != hwloc_topology_set_xml((hwloc_topology_t) pmix_globals.topology.topology, topo_file)) {
            return PMIX_ERR_NOT_SUPPORTED;
        }
        /* since we are loading this from an external source, we have to
         * explicitly set a flag so hwloc sets things up correctly
         */
        if (0 != set_flags(pmix_globals.topology.topology, HWLOC_TOPOLOGY_FLAG_IS_THISSYSTEM)) {
            hwloc_topology_destroy(pmix_globals.topology.topology);
            return PMIX_ERROR;
        }
        /* now load the topology */
        if (0 != hwloc_topology_load(pmix_globals.topology.topology)) {
            hwloc_topology_destroy(pmix_globals.topology.topology);
            return PMIX_ERROR;
        }
        /* we don't know the version */
        if (NULL != pmix_globals.topology.source) {
            free(pmix_globals.topology.source);
        }
        pmix_globals.topology.source = strdup("hwloc");
    } else {
        pmix_output_verbose(2, pmix_hwloc_output, "%s:%s doing discovery",
                            __FILE__, __func__);
        /* we weren't given a topology, so get it for ourselves */
        if (0 != hwloc_topology_init((hwloc_topology_t *) &pmix_globals.topology.topology)) {
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
        pmix_asprintf(&pmix_globals.topology.source, "hwloc:%s", HWLOC_VERSION);
        pmix_output_verbose(2, pmix_hwloc_output,
                            "%s:%s discovery complete - source %s", __FILE__, __func__,
                            pmix_globals.topology.source);
    }

    /* record locally in case someone does a PMIx_Get to retrieve it */
    kv.key = PMIX_TOPOLOGY2;
    kv.value = &val;
    val.type = PMIX_TOPO;
    val.data.topo = &pmix_globals.topology;
    PMIX_GDS_STORE_KV(rc, pmix_globals.mypeer, &pmix_globals.myid, PMIX_INTERNAL, &kv);
    pmix_output_verbose(2, pmix_hwloc_output, "%s:%s stored", __FILE__,
                        __func__);

    /* if we don't need to share it, then we are done */
    if (!share) {
        return PMIX_SUCCESS;
    }

sharetopo:
    /* setup the XML representation(s) */
    pmix_output_verbose(2, pmix_hwloc_output,
                        "%s:%s sharing topology",
                        __FILE__, __func__);

    /* pass the topology as a v2 xml string */
    if (0 == hwloc_topology_export_xmlbuffer(pmix_globals.topology.topology, &xmlbuffer, &len, 0)) {
        pmix_output_verbose(2, pmix_hwloc_output, "%s:%s export v2 xml",
                            __FILE__, __func__);
        kptr = PMIX_NEW(pmix_kval_t);
        kptr->key = strdup(PMIX_HWLOC_XML_V2);
        kptr->value = (pmix_value_t *) malloc(sizeof(pmix_value_t));
        PMIX_VALUE_LOAD(kptr->value, xmlbuffer, PMIX_STRING);
        pmix_list_append(&pmix_server_globals.gdata, &kptr->super);
        /* save it with the deprecated key for older RMs */
        kptr = PMIX_NEW(pmix_kval_t);
        kptr->key = strdup(PMIX_LOCAL_TOPO);
        kptr->value = (pmix_value_t *) malloc(sizeof(pmix_value_t));
        PMIX_VALUE_LOAD(kptr->value, xmlbuffer, PMIX_STRING);
        pmix_list_append(&pmix_server_globals.gdata, &kptr->super);
        hwloc_free_xmlbuffer(pmix_globals.topology.topology, xmlbuffer);
    }
    /* and as a v1 xml string, should an older client attach */
    if (0 == hwloc_topology_export_xmlbuffer(pmix_globals.topology.topology, &xmlbuffer, &len,
                                             HWLOC_TOPOLOGY_EXPORT_XML_FLAG_V1)) {
        pmix_output_verbose(2, pmix_hwloc_output, "%s:%s export v1 xml",
                            __FILE__, __func__);
        kptr = PMIX_NEW(pmix_kval_t);
        kptr->key = strdup(PMIX_HWLOC_XML_V1);
        kptr->value = (pmix_value_t *) malloc(sizeof(pmix_value_t));
        PMIX_VALUE_LOAD(kptr->value, xmlbuffer, PMIX_STRING);
        hwloc_free_xmlbuffer(pmix_globals.topology.topology, xmlbuffer);
        pmix_list_append(&pmix_server_globals.gdata, &kptr->super);
        /* cannot support the deprecated PMIX_LOCAL_TOPO key here as it would
         * overwrite the HWLOC v2 string */
    }

    /* if they specified no shared memory, then we are done */
    if (VMEM_HOLE_NONE == hole_kind) {
        pmix_output_verbose(2, pmix_hwloc_output,
                            "%s:%s no shmem requested", __FILE__, __func__);
        return PMIX_SUCCESS;
    }

    /* get the size of the topology shared memory segment */
    if (0 != hwloc_shmem_topology_get_length(pmix_globals.topology.topology, &shmemsize, 0)) {
        pmix_output_verbose(2, pmix_hwloc_output,
                            "%s hwloc topology shmem not available",
                            PMIX_NAME_PRINT(&pmix_globals.myid));
        return PMIX_SUCCESS;
    }

    /* try and find a hole */
    if (PMIX_SUCCESS != pmix_vmem_find_hole(hole_kind, &shmemaddr, shmemsize)) {
        /* we couldn't find a hole, so don't use the shmem support */
        if (4 < pmix_output_get_verbosity(pmix_hwloc_output)) {
            print_maps();
        }
        return PMIX_SUCCESS;
    }
    /* create the shmem file in our session dir so it
     * will automatically get cleaned up */
    pmix_asprintf(&shmemfile, "%s/hwloc.sm", pmix_server_globals.tmpdir);
    /* let's make sure we have enough space for the backing file */
    if (PMIX_SUCCESS != enough_space(shmemfile, shmemsize, &amount_space_avail, &space_available)) {
        pmix_output_verbose(2, pmix_hwloc_output,
                            "%s an error occurred while determining "
                            "whether or not %s could be created for topo shmem.",
                            PMIX_NAME_PRINT(&pmix_globals.myid), shmemfile);
        free(shmemfile);
        shmemfile = NULL;
        return PMIX_SUCCESS;
    }
    if (!space_available) {
        if (1 < pmix_output_get_verbosity(pmix_hwloc_output)) {
            pmix_show_help("help-ploc.txt", "target full", true, shmemfile,
                           pmix_globals.hostname, (unsigned long) shmemsize,
                           (unsigned long long) amount_space_avail);
        }
        free(shmemfile);
        shmemfile = NULL;
        return PMIX_SUCCESS;
    }
    /* enough space is available, so create the segment */
    if (-1 == (shmemfd = open(shmemfile, O_CREAT | O_RDWR, 0600))) {
        int err = errno;
        if (1 < pmix_output_get_verbosity(pmix_hwloc_output)) {
            pmix_show_help("help-ploc.txt", "sys call fail", true,
                           pmix_globals.hostname, "open(2)", "", strerror(err), err);
        }
        free(shmemfile);
        shmemfile = NULL;
        return PMIX_SUCCESS;
    }
    /* ensure nobody inherits this fd */
    pmix_fd_set_cloexec(shmemfd);
    /* populate the shmem segment with the topology */
    rc = hwloc_shmem_topology_write(pmix_globals.topology.topology, shmemfd, 0, (void *) shmemaddr,
                                    shmemsize, 0);
    if (0 != rc) {
        pmix_output_verbose(2, pmix_hwloc_output,
                            "%s an error %d (%s) occurred while writing topology to %s",
                            PMIX_NAME_PRINT(&pmix_globals.myid), rc, strerror(errno), shmemfile);
        unlink(shmemfile);
        free(shmemfile);
        shmemfile = NULL;
        close(shmemfd);
        shmemfd = -1;
        return PMIX_SUCCESS;
    }
    pmix_output_verbose(2, pmix_hwloc_output, "%s:%s exported shmem",
                        __FILE__, __func__);

    /* add the requisite key-values to the global data to be
     * given to each client for older PMIx versions */
    kptr = PMIX_NEW(pmix_kval_t);
    kptr->key = strdup(PMIX_HWLOC_SHMEM_FILE);
    kptr->value = (pmix_value_t *) malloc(sizeof(pmix_value_t));
    PMIX_VALUE_LOAD(kptr->value, shmemfile, PMIX_STRING);
    pmix_list_append(&pmix_server_globals.gdata, &kptr->super);

    kptr = PMIX_NEW(pmix_kval_t);
    kptr->key = strdup(PMIX_HWLOC_SHMEM_ADDR);
    kptr->value = (pmix_value_t *) malloc(sizeof(pmix_value_t));
    PMIX_VALUE_LOAD(kptr->value, &shmemaddr, PMIX_SIZE);
    pmix_list_append(&pmix_server_globals.gdata, &kptr->super);

    kptr = PMIX_NEW(pmix_kval_t);
    kptr->key = strdup(PMIX_HWLOC_SHMEM_SIZE);
    kptr->value = (pmix_value_t *) malloc(sizeof(pmix_value_t));
    PMIX_VALUE_LOAD(kptr->value, &shmemsize, PMIX_SIZE);
    pmix_list_append(&pmix_server_globals.gdata, &kptr->super);

    return PMIX_SUCCESS;
}

pmix_status_t pmix_hwloc_load_topology(pmix_topology_t *topo)
{
    pmix_cb_t cb;
    pmix_proc_t wildcard;
    pmix_status_t rc;
    pmix_topology_t *t;
    /* Source string reported for a caller-provided topology when the caller
     * did not request a specific source. It has static storage duration, so
     * the caller must treat it as read-only and must NOT free it - consistent
     * with the returned topology being a library-managed, read-only object.
     * A source the caller DID provide remains theirs to free. We never assign
     * this to our own cached topology, whose source is heap-allocated and
     * released when the library finalizes. */
    static const char hwloc_source[] = "hwloc:" HWLOC_VERSION;
    bool source_given;

    /* reachable from the public PMIx_Load_topology with whatever the caller
     * passed, and this is both the IN parameter (they may stipulate a source)
     * and the OUT parameter (we fill in the topology) - so there is nothing
     * to do with a NULL. Screen it here as the siblings below do, rather
     * than dereferencing it blind. */
    if (NULL == topo) {
        return PMIX_ERR_BAD_PARAM;
    }
    source_given = (NULL != topo->source);

    pmix_output_verbose(2, pmix_hwloc_output, "%s:%s", __FILE__, __func__);

    /* see if they stipulated the type of topology they want */
    if (NULL != topo->source) {
        if (0 != strncasecmp(topo->source, "hwloc", strlen("hwloc"))) {
            /* they want somebody else */
            pmix_output_verbose(2, pmix_hwloc_output,
                                "%s:%s no match - wanted %s", __FILE__, __func__, topo->source);
            return PMIX_ERR_TAKE_NEXT_OPTION;
        }
        /* if we already have a suitable version, just return it */
        if (NULL != pmix_globals.topology.topology) {
            if (0
                == strncasecmp(pmix_globals.topology.source, topo->source, strlen(topo->source))) {
                pmix_output_verbose(2, pmix_hwloc_output,
                                    "%s:%s matched sources", __FILE__, __func__);
                topo->topology = pmix_globals.topology.topology;
                return PMIX_SUCCESS;
            }
            /* nope - not a suitable version */
            pmix_output_verbose(2, pmix_hwloc_output,
                                "%s:%s present but not suitable", __FILE__, __func__);
            return PMIX_ERR_TAKE_NEXT_OPTION;
        }
    } else {
        /* they didn't stipulate a source, so if we already have something, just return it */
        if (NULL != pmix_globals.topology.topology) {
            pmix_output_verbose(2, pmix_hwloc_output,
                                "%s:%s no source stipulated - returning current version", __FILE__,
                                __func__);
            /* read-only static source - caller must not free it */
            topo->source = (char *) hwloc_source;
            topo->topology = pmix_globals.topology.topology;
            return PMIX_SUCCESS;
        }
    }

    /* see if we have it in storage */
    pmix_output_verbose(2, pmix_hwloc_output, "%s:%s checking storage",
                        __FILE__, __func__);
    PMIX_CONSTRUCT(&cb, pmix_cb_t);
    PMIX_LOAD_PROCID(&wildcard, pmix_globals.myid.nspace, PMIX_RANK_WILDCARD);
    cb.proc = &wildcard;
    cb.copy = true;
    cb.key = PMIX_TOPOLOGY2;
    PMIX_GDS_FETCH_KV(rc, pmix_client_globals.myserver, &cb);
    if (PMIX_SUCCESS == rc) {
        cb.key = NULL;
        t = popptr(&cb);
        PMIX_DESTRUCT(&cb);
        if (NULL != t) {
            pmix_output_verbose(2, pmix_hwloc_output,
                                "%s:%s found in storage", __FILE__, __func__);
            /* populate the caller's topology (unless it IS our cache). A
             * caller-provided source stays theirs; otherwise report the
             * read-only static source they must not free. */
            if (topo != &pmix_globals.topology) {
                if (!source_given) {
                    topo->source = (char *) hwloc_source;
                }
                topo->topology = t->topology;
            }
            /* popptr handed us ownership of the fetched container. Adopt
             * its topology and source directly into our cache, then free
             * the now-empty struct - otherwise the container and its
             * source string leak on every call down this path. */
            pmix_globals.topology.source = t->source;
            pmix_globals.topology.topology = t->topology;
            free(t);
            return PMIX_SUCCESS;
        }
    }

    /* we don't have it - better set it up */
    pmix_output_verbose(2, pmix_hwloc_output,
                        "%s:%s nothing found - calling setup", __FILE__, __func__);
    rc = pmix_hwloc_setup_topology(NULL, 0);
    if (PMIX_SUCCESS == rc && topo != &pmix_globals.topology) {
        /* setup populated our cache; hand the caller the shared topology
         * plus a read-only static source (unless they provided their own) */
        if (!source_given) {
            topo->source = (char *) hwloc_source;
        }
        topo->topology = pmix_globals.topology.topology;
    }
    return rc;
}

pmix_status_t pmix_hwloc_generate_cpuset_string(const pmix_cpuset_t *cpuset,
                                                char **cpuset_string)
{
    char *tmp;

    /* we are being asked to serialize this cpuset, so it must tell us
     * who produced its bitmap - an unlabeled bitmap is not something we
     * can claim. A NULL source is only legal on an input cpuset we are
     * being asked to fill in. */
    if (NULL == cpuset || NULL == cpuset->bitmap || NULL == cpuset->source) {
        *cpuset_string = NULL;
        return PMIX_ERR_BAD_PARAM;
    }

    /* if we aren't the source, then nothing we can do */
    if (0 != strncasecmp(cpuset->source, "hwloc", 5)) {
        *cpuset_string = NULL;
        return PMIX_ERR_TAKE_NEXT_OPTION;
    }

    /* the return is the character count, with -1 signalling error - see the
     * note in pmix_hwloc_pack_cpuset. On error tmp is not set, so it must not
     * be handed to pmix_asprintf. */
    if (0 > hwloc_bitmap_list_asprintf(&tmp, cpuset->bitmap)) {
        *cpuset_string = NULL;
        return PMIX_ERROR;
    }
    pmix_asprintf(cpuset_string, "hwloc:%s", tmp);
    free(tmp);

    return PMIX_SUCCESS;
}

pmix_status_t pmix_hwloc_parse_cpuset_string(const char *cpuset_string, pmix_cpuset_t *cpuset)
{
    const char *src;
    int hrc;

    /* both of these come straight from the caller of the public
     * PMIx_Parse_cpuset_string, which does not screen them - a NULL string
     * used to be handed to strchr and crash the library */
    if (NULL == cpuset_string || NULL == cpuset) {
        return PMIX_ERR_BAD_PARAM;
    }

    /* the string is formatted as "<source>:<bitmap>" - find the delimiter */
    src = strchr(cpuset_string, ':');
    if (NULL == src) {
        /* bad string */
        return PMIX_ERR_BAD_PARAM;
    }
    /* if we aren't the source, then pass. Compare the prefix in place - we
     * must NOT modify the caller's const string (doing so crashes on a
     * read-only string literal), so we do not temporarily NUL-terminate it.
     * strncasecmp stops at 5 chars, which is exactly the length of "hwloc". */
    if (0 != strncasecmp(cpuset_string, "hwloc", 5)) {
        return PMIX_ERR_TAKE_NEXT_OPTION;
    }
    ++src;  /* advance past the ':' delimiter */

    cpuset->source = strdup("hwloc");
    cpuset->bitmap = hwloc_bitmap_alloc();
    hrc = hwloc_bitmap_list_sscanf(cpuset->bitmap, src);
    if (0 != hrc) {
        /* leave nothing half-built behind: a caller that gets an error back
         * has no reason to destruct the cpuset, so anything we allocated here
         * would simply leak */
        hwloc_bitmap_free(cpuset->bitmap);
        cpuset->bitmap = NULL;
        free(cpuset->source);
        cpuset->source = NULL;
        return PMIX_ERR_BAD_PARAM;
    }
    return PMIX_SUCCESS;
}

pmix_status_t pmix_hwloc_generate_locality_string(const pmix_cpuset_t *cpuset, char **loc)
{
    char *locality = NULL, *tmp, *t2;
    unsigned depth, d;
    hwloc_cpuset_t result;
    hwloc_obj_type_t type;

    /* we are being asked to interpret this cpuset, so it must tell us
     * who produced its bitmap - see the note in generate_cpuset_string */
    if (NULL == cpuset || NULL == cpuset->source) {
        *loc = NULL;
        return PMIX_ERR_BAD_PARAM;
    }

    /* if we aren't the source, then pass */
    if (0 != strncasecmp(cpuset->source, "hwloc", 5)) {
        *loc = NULL;
        return PMIX_ERR_TAKE_NEXT_OPTION;
    }

    /* if this proc is not bound, then there is no locality. We
     * know it isn't bound if the bitmap is NULL, or if it is
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
    for (d = 1; d < depth; d++) {
        /* get the object type at this depth */
        type = hwloc_get_depth_type(pmix_globals.topology.topology, d);
        /* if it isn't one of interest, then ignore it */
        if (HWLOC_OBJ_NUMANODE != type && HWLOC_OBJ_PACKAGE != type &&
            HWLOC_OBJ_L1CACHE != type && HWLOC_OBJ_L2CACHE != type && HWLOC_OBJ_L3CACHE != type &&
            HWLOC_OBJ_CORE != type && HWLOC_OBJ_PU != type) {
            continue;
        }

        if (get_locality_string_by_depth(d, cpuset->bitmap, result) < 0) {
            continue;
        }

        /* it should be impossible, but allow for the possibility
         * that we came up empty at this depth */
        if (!hwloc_bitmap_iszero(result)) {
            hwloc_bitmap_list_asprintf(&tmp, result);
            switch (type) {
                case HWLOC_OBJ_NUMANODE:
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

    hwloc_bitmap_free(result);

    /* remove the trailing colon */
    if (NULL != locality) {
        locality[strlen(locality) - 1] = '\0';
    }
    *loc = locality;
    return PMIX_SUCCESS;
}

/* The locality codes pmix_hwloc_generate_locality_string emits, in the same
 * order they are tested for below. */
static const char *pmix_hwloc_locality_types[] = {"NM", "SK", "L3", "L2",
                                                  "L1", "CR", "HT", NULL};

/* Find the first locality token in a locality string, or NULL if the string
 * was not produced by us.
 *
 * pmix_hwloc_generate_locality_string emits a bare colon-separated list of
 * two-letter type codes with their bitmaps -- "SK0:L20:CR0:HT0" -- with no
 * provider prefix, and that is exactly what a host environment stores as
 * PMIX_LOCALITY_STRING. Some callers nonetheless hand us an "hwloc:"-prefixed
 * string, which is what this routine used to demand. Accept both: requiring
 * the prefix made the usage documented for PMIx_Get_relative_locality -- pass
 * it the strings returned by PMIx_server_generate_locality_string -- fail with
 * PMIX_ERR_TAKE_NEXT_OPTION and report no locality at all.
 *
 * An unprefixed string is only claimed when it actually starts with one of our
 * type codes, so a string belonging to some other provider is still passed
 * along rather than misparsed.
 */
static const char *pmix_hwloc_locality_payload(const char *locality)
{
    size_t n;

    if (NULL == locality) {
        return NULL;
    }
    if (0 == strncasecmp(locality, "hwloc:", strlen("hwloc:"))) {
        return &locality[strlen("hwloc:")];
    }
    for (n = 0; NULL != pmix_hwloc_locality_types[n]; n++) {
        if (0 == strncasecmp(locality, pmix_hwloc_locality_types[n], 2)) {
            return locality;
        }
    }
    return NULL;
}

pmix_status_t pmix_hwloc_get_relative_locality(const char *locality1,
                                               const char *locality2,
                                               pmix_locality_t *loc)
{
    pmix_locality_t locality;
    const char *loc1, *loc2;
    char **set1, **set2;
    hwloc_bitmap_t bit1, bit2;
    size_t n1, n2;
    pmix_status_t rc = PMIX_ERR_TAKE_NEXT_OPTION;

    /* the answer is written through "loc" on every path that gets past the
     * checks below, so a NULL for it is a store to address zero - see the
     * companion checks in parse_cpuset_string/get_cpuset */
    if (NULL == loc) {
        return PMIX_ERR_BAD_PARAM;
    }

    /* check that locality was generated by us, tolerating either the bare or
     * the "hwloc:"-prefixed form, and point at the first locality token */
    loc1 = pmix_hwloc_locality_payload(locality1);
    loc2 = pmix_hwloc_locality_payload(locality2);
    if (NULL == loc1 || NULL == loc2) {
        return PMIX_ERR_TAKE_NEXT_OPTION;
    }

    /* the inputs were generated by us, so the operation will
     * succeed unless we hit an unrecognized locality below */
    rc = PMIX_SUCCESS;

    /* start with what we know - they share a node */
    locality = PMIX_LOCALITY_SHARE_NODE;

    set1 = PMIx_Argv_split(loc1, ':');
    set2 = PMIx_Argv_split(loc2, ':');
    bit1 = hwloc_bitmap_alloc();
    bit2 = hwloc_bitmap_alloc();

    /* check each matching type */
    for (n1 = 0; NULL != set1[n1]; n1++) {
        /* convert the location into bitmap */
        hwloc_bitmap_list_sscanf(bit1, &set1[n1][2]);
        /* find the matching type in set2 */
        for (n2 = 0; NULL != set2[n2]; n2++) {
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
    PMIx_Argv_free(set1);
    PMIx_Argv_free(set2);
    hwloc_bitmap_free(bit1);
    hwloc_bitmap_free(bit2);
    *loc = locality;
    return rc;
}

pmix_status_t pmix_hwloc_get_cpuset(pmix_cpuset_t *cpuset, pmix_bind_envelope_t ref)
{
    int rc, flag;

    /* reachable from the public PMIx_Get_cpuset with whatever the caller
     * passed, so screen it here rather than dereferencing blind */
    if (NULL == cpuset) {
        return PMIX_ERR_BAD_PARAM;
    }
    if (NULL != cpuset->source && 0 != strncasecmp(cpuset->source, "hwloc", 5)) {
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
    if (NULL != testcpuset) {
        rc = hwloc_bitmap_sscanf(cpuset->bitmap, testcpuset);
    } else {
        rc = hwloc_get_cpubind(pmix_globals.topology.topology, cpuset->bitmap, flag);
    }
    if (0 != rc) {
        hwloc_bitmap_free(cpuset->bitmap);
        cpuset->bitmap = NULL;
        return PMIX_ERR_NOT_FOUND;
    }
    if (NULL == cpuset->source) {
        cpuset->source = strdup("hwloc");
    }

    return PMIX_SUCCESS;
}

static hwloc_obj_t dsearch(hwloc_topology_t t, int depth, hwloc_cpuset_t cpuset)
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
    for (w = 0; w < width; w++) {
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
static PMIX_CLASS_INSTANCE(pmix_devdist_item_t, pmix_list_item_t, dvcon, dvdes);

typedef struct {
    hwloc_obj_osdev_type_t hwtype;
    pmix_device_type_t pxtype;
    char *name;
} pmix_type_conversion_t;

static pmix_type_conversion_t table[] = {
    {.hwtype = HWLOC_OBJ_OSDEV_BLOCK, .pxtype = PMIX_DEVTYPE_BLOCK, .name = "BLOCK"},
    {.hwtype = HWLOC_OBJ_OSDEV_GPU, .pxtype = PMIX_DEVTYPE_GPU, .name = "GPU"},
    {.hwtype = HWLOC_OBJ_OSDEV_NETWORK, .pxtype = PMIX_DEVTYPE_NETWORK, .name = "NETWORK"},
    {.hwtype = HWLOC_OBJ_OSDEV_OPENFABRICS, .pxtype = PMIX_DEVTYPE_OPENFABRICS, .name = "OPENFABRICS"},
    {.hwtype = HWLOC_OBJ_OSDEV_DMA, .pxtype = PMIX_DEVTYPE_DMA, .name = "DMA"},
    {.hwtype = HWLOC_OBJ_OSDEV_COPROC, .pxtype = PMIX_DEVTYPE_COPROC, .name = "COPROCESSOR"},
};

static int countcolons(char *str)
{
    int cnt = 0;
    char *p;

    p = strchr(str, ':');
    while (NULL != p) {
        ++cnt;
        ++p;
        p = strchr(p, ':');
    }

    return cnt;
}

pmix_status_t pmix_hwloc_compute_distances(pmix_topology_t *topo, pmix_cpuset_t *cpuset,
                                           pmix_info_t info[], size_t ninfo,
                                           pmix_device_distance_t **dist, size_t *ndist)
{
    hwloc_obj_t obj = NULL;
    hwloc_obj_t tgt;
    hwloc_obj_t ancestor;
    hwloc_obj_t pu;
    unsigned dp, depth;
    unsigned maxdist = 0;
    unsigned mindist = UINT_MAX;
    pmix_list_t dists;
    pmix_devdist_item_t *d;
    pmix_device_distance_t *array;
    size_t n, dn, k, ndevs = 0;
    unsigned w, width, pudepth;
    pmix_device_type_t type = 0;
    char **devids = NULL;
    bool found;
    pmix_hwloc_device_t *devs = NULL;
    pmix_status_t rc = PMIX_SUCCESS, prc;

    if (NULL == topo || NULL == cpuset || NULL == dist || NULL == ndist) {
        return PMIX_ERR_BAD_PARAM;
    }

    if (NULL == topo->source || NULL == cpuset->source) {
        return PMIX_ERR_BAD_PARAM;
    }

    if (0 != strncasecmp(topo->source, "hwloc", 5)
        || 0 != strncasecmp(cpuset->source, "hwloc", 5)) {
        return PMIX_ERR_TAKE_NEXT_OPTION;
    }

    /* set default returns */
    *dist = NULL;
    *ndist = 0;

    /* determine what they want us to look at */
    if (NULL == info) {
        /* The devices a process communicates through - which is what asking
         * "how far away is it?" is normally about.  Block and DMA devices
         * are deliberately not in the default: this function has never
         * reported a distance for one, and a caller that wants them can ask
         * by naming the type.  Coprocessors ARE included, because that is
         * where a vendor labels a GPU's compute node ("cuda0"), and leaving
         * them out lost every GPU on a machine whose backend was loaded. */
        type = PMIX_DEVTYPE_NETWORK | PMIX_DEVTYPE_OPENFABRICS
               | PMIX_DEVTYPE_GPU | PMIX_DEVTYPE_COPROC;
    } else {
        for (n = 0; n < ninfo; n++) {
            if (PMIX_CHECK_KEY(&info[n], PMIX_DEVICE_TYPE)) {
                type |= info[n].value.data.devtype;
            } else if (PMIX_CHECK_KEY(&info[n], PMIX_DEVICE_ID)) {
                PMIx_Argv_append_nosize(&devids, info[n].value.data.string);
            }
        }
    }

    /* Construct the result list here, before the first failure exit below, so
     * every path out of this function can share the single cleanup block at
     * the bottom. That block is also what releases devids: the argv array
     * assembled from PMIX_DEVICE_ID above used to be freed on no path at all,
     * success included, so every call that named a device leaked it. */
    PMIX_CONSTRUCT(&dists, pmix_list_t);

    /* find the max depth of this topology */
    depth = hwloc_topology_get_depth(topo->topology);

    /* get the lowest object that completely covers the cpuset */
    for (dp = 1; dp < depth; dp++) {
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
        rc = PMIX_ERR_NOT_AVAILABLE;
        goto cleanup;
    }

    /* get the PU depth */
    pudepth = (unsigned) hwloc_get_type_depth(topo->topology, HWLOC_OBJ_PU);
    width = hwloc_get_nbobjs_by_depth(topo->topology, pudepth);

    /* Enumerate the devices, then measure each one.
     *
     * The enumeration is pmix_hwloc_get_devices()' job rather than a second
     * walk written here.  These two answers are handed to the same
     * application - one says which devices exist, the other how far away
     * they are - so a device the two disagree about, in name or in
     * existence, is worse than either answer alone.  Sharing the enumerator
     * is what makes them agree by construction: it is also what gives this
     * function per-PCI-function dedup (a GPU exposing a card node, a render
     * node and a vendor node is one device, not three) and a deterministic
     * order, neither of which the walk here used to have.
     *
     * pmix_globals.hostname is the right node here, and is the only place in
     * the tree where that is true by construction: distances are measured
     * against a cpuset of this machine's PUs, so the topology being read can
     * only be the local one. */
    prc = pmix_hwloc_get_devices(topo, pmix_globals.hostname, type,
                                 (NULL == devids) ? NULL : devids[0],
                                 &devs, &ndevs);
    if (PMIX_SUCCESS != prc) {
        rc = prc;
        goto cleanup;
    }

    for (k = 0; k < ndevs; k++) {
        /* a caller may name several devices; get_devices() takes one, so
         * filter the rest here */
        if (NULL != devids) {
            found = false;
            for (dn = 0; NULL != devids[dn]; dn++) {
                if (0 == strcasecmp(devids[dn], devs[k].dev.osname)
                    || 0 == strcasecmp(devids[dn], devs[k].dev.uuid)) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                continue;
            }
        }

        d = PMIX_NEW(pmix_devdist_item_t);
        pmix_list_append(&dists, &d->super);
        d->dist.type = devs[k].dev.type;
        d->dist.uuid = strdup(devs[k].dev.uuid);
        d->dist.osname = strdup(devs[k].dev.osname);

        tgt = devs[k].locality;
        if (NULL == tgt) {
            /* nothing in the topology is local to it */
            d->dist.mindist = UINT16_MAX;
            d->dist.maxdist = UINT16_MAX;
            continue;
        }

        /* Loop over the PUs the process is bound to, measuring each one's
         * distance to this device.
         *
         * The min/max pair exists precisely because a process may be bound
         * to more than one location and those locations may sit at
         * different distances from the device - that is what
         * pmix_device_distance_t(5) says the two fields are for.  So the
         * ancestor has to be taken between THIS PU and the device. */
        maxdist = 0;
        mindist = UINT_MAX;
        for (w = 0; w < width; w++) {
            pu = hwloc_get_obj_by_depth(topo->topology, pudepth, w);
            if (NULL == pu || NULL == pu->cpuset
                || !hwloc_bitmap_intersects(pu->cpuset, cpuset->bitmap)) {
                continue;
            }
            ancestor = hwloc_get_common_ancestor_obj(topo->topology, pu, tgt);
            if (NULL == ancestor) {
                /* shouldn't happen - consider this an error condition */
                rc = PMIX_ERROR;
                goto cleanup;
            }
            if (0 == ancestor->depth) {
                /* we only share the machine - need to do something more
                 * to compute the distance. This can, however, get a little
                 * hairy as there is no good measure of package-to-package
                 * distance - it is all typically given in terms of NUMA
                 * domains, which is no longer a valid way of looking at
                 * locations due to overlapping domains. For now, we will
                 * just take the depth of this location and add the depth
                 * of the topology to ensure it sorts further away than
                 * anything that shares an ancestor below the machine
                 * (those are all < depth) */
                dp = pu->depth + depth;
            } else {
                /* the depth value can be used as an indicator of relative
                 * locality - the higher the value, the closer the device.
                 * We invert the pyramid to set the dist to be closer for
                 * smaller values */
                dp = depth - ancestor->depth;
            }
            if (mindist > dp) {
                mindist = dp;
            }
            if (maxdist < dp) {
                maxdist = dp;
            }
        }
        if (UINT_MAX == mindist) {
            /* no location in the cpuset lies in this topology, so we
             * measured nothing. Report the documented "distance is
             * unknown" sentinel in BOTH fields - leaving maxdist at 0
             * claimed the device was as close as possible while
             * mindist said it was unknown, and min > max besides. */
            d->dist.mindist = UINT16_MAX;
            d->dist.maxdist = UINT16_MAX;
        } else {
            d->dist.mindist = mindist;
            d->dist.maxdist = maxdist;
        }
    }
    /* create the return array */
    n = pmix_list_get_size(&dists);
    if (0 == n) {
        /* no devices found */
        rc = PMIX_ERR_NOT_FOUND;
        goto cleanup;
    }
    PMIX_DEVICE_DIST_CREATE(array, n);
    if (PMIX_UNLIKELY(NULL == array)) {
        rc = PMIX_ERR_NOMEM;
        goto cleanup;
    }
    /* claim the entries only once they exist - the count is set ahead of
     * the array below, so a failure here has to leave it at the zero the
     * default returns established, not at a size nothing backs */
    *ndist = n;
    n = 0;
    PMIX_LIST_FOREACH (d, &dists, pmix_devdist_item_t) {
        array[n].uuid = strdup(d->dist.uuid);
        array[n].osname = strdup(d->dist.osname);
        array[n].type = d->dist.type;
        array[n].mindist = d->dist.mindist;
        array[n].maxdist = d->dist.maxdist;
        ++n;
    }
    *dist = array;

cleanup:
    PMIX_LIST_DESTRUCT(&dists);
    if (NULL != devs) {
        pmix_hwloc_release_devices(devs, ndevs);
    }
    if (NULL != devids) {
        PMIx_Argv_free(devids);
    }
    return rc;
}

/* ---------------------------------------------------------------------
 * Device enumeration
 * --------------------------------------------------------------------- */

/* The vendor compute backends hwloc knows how to label.  An OS device
 * carrying one of these is unambiguously a compute device: it is the
 * runtime's own view of the card rather than a display node that happens to
 * sit on the same PCI function. */
static bool osdev_is_compute_backend(hwloc_obj_t osdev)
{
    const char *backend;

    if (HWLOC_OBJ_OSDEV_COPROC == osdev->attr->osdev.type) {
        return true;
    }
    backend = hwloc_obj_get_info_by_name(osdev, "Backend");
    if (NULL == backend) {
        return false;
    }
    return (0 == strcasecmp(backend, "CUDA")
            || 0 == strcasecmp(backend, "NVML")
            || 0 == strcasecmp(backend, "RSMI")
            || 0 == strcasecmp(backend, "LevelZero")
            || 0 == strcasecmp(backend, "OpenCL"));
}

static bool osdev_is_render_node(hwloc_obj_t osdev)
{
    return (NULL != osdev->name && 0 == strncasecmp(osdev->name, "renderD", 7));
}

/* The info key each vendor's hwloc backend records its device identity
 * under: NVML writes NVIDIAUUID, RSMI writes AMDUUID, Level Zero writes
 * LevelZeroUUID.  There is no generic spelling to fall back on, which is
 * why this is a table rather than a rule.
 *
 * Deliberately not BXIUUID, which hwloc also writes: that is a network
 * interconnect, and this is about naming a device to the vendor runtime
 * that will compute on it. */
static const struct {
    const char *key;
    const char *vendor;
} vendor_id_keys[] = {
    {"NVIDIAUUID", "NVIDIA"},
    {"AMDUUID", "AMD"},
    {"LevelZeroUUID", "INTEL"},
    {NULL, NULL}
};

/* The vendor's identifier for the device this OS device belongs to, or NULL.
 *
 * Scanned across the whole PCI function rather than read off the given OS
 * device, because the two are routinely different objects: a topology with
 * both the CUDA and NVML backends loaded exposes "cuda0" and "nvml0" on one
 * function, get_devices() names the function by the first vendor compute
 * node it meets - "cuda0" - and the uuid is on "nvml0".  Reading only the
 * named device would report "no identity" on precisely the machines that
 * have one. */
static bool vendor_id_read(hwloc_obj_t osdev, char **id, char **vendor)
{
    const char *val;
    unsigned k;

    for (k = 0; NULL != vendor_id_keys[k].key; k++) {
        val = hwloc_obj_get_info_by_name(osdev, vendor_id_keys[k].key);
        if (NULL != val) {
            *id = strdup(val);
            *vendor = strdup(vendor_id_keys[k].vendor);
            return true;
        }
    }
    return false;
}

static void vendor_id_of(hwloc_obj_t osdev, hwloc_obj_t pci,
                         char **id, char **vendor)
{
    hwloc_obj_t child;

    *id = NULL;
    *vendor = NULL;

    if (vendor_id_read(osdev, id, vendor)) {
        return;
    }
    if (NULL == pci) {
        return;
    }
    for (child = pci->io_first_child; NULL != child; child = child->next_sibling) {
        if (HWLOC_OBJ_OS_DEVICE != child->type) {
            continue;
        }
        if (vendor_id_read(child, id, vendor)) {
            return;
        }
    }
}

/* The PCI function an OS device hangs off, or NULL if it has none. */
static hwloc_obj_t pci_ancestor(hwloc_obj_t osdev)
{
    hwloc_obj_t p;

    for (p = osdev->parent; NULL != p; p = p->parent) {
        if (HWLOC_OBJ_PCI_DEVICE == p->type) {
            return p;
        }
        if (HWLOC_OBJ_BRIDGE != p->type && HWLOC_OBJ_OS_DEVICE != p->type) {
            /* climbed out of the I/O subtree without finding one */
            break;
        }
    }
    return NULL;
}

/* Whether a display-class device is one an application can compute on.
 *
 * hwloc reports a plain display adapter - a BMC's VGA controller, say -
 * with the same osdev type as a GPU, and on a machine whose topology was
 * gathered without the vendor backends there is no "cuda0" to tell them
 * apart.  Two rules decide it, applied together rather than as a ladder:
 *
 *   1. the function carries a vendor compute node (cuda0, rsmi0, ze0, ...)
 *      or a coprocessor OS device;
 *   2. the function is PCI class 03xx - a display controller - and exposes
 *      a DRM *render* node.  A render node is what makes a display device
 *      usable for compute; a management controller has only a card node.
 *
 * They are a union rather than a priority ladder because a mixed-vendor
 * node can carry one card whose backend is loaded and one whose is not, and
 * a ladder that stopped at rule 1 would report only the first.
 */
static bool is_compute_gpu(hwloc_obj_t osdev, hwloc_obj_t pci)
{
    hwloc_obj_t child;
    bool has_render = false;

    if (NULL == pci) {
        /* no function to group by - judge the OS device on its own */
        return osdev_is_compute_backend(osdev);
    }
    for (child = pci->io_first_child; NULL != child; child = child->next_sibling) {
        if (HWLOC_OBJ_OS_DEVICE != child->type) {
            continue;
        }
        if (osdev_is_compute_backend(child)) {
            return true;
        }
        if (osdev_is_render_node(child)) {
            has_render = true;
        }
    }
    return (has_render && 0x03 == (pci->attr->pcidev.class_id >> 8));
}

/* Which of two OS devices on one function should name it.  A vendor compute
 * node is the most useful name and the one an application is most likely to
 * recognize; a render node beats a card node (which is what PMIx has always
 * skipped); otherwise keep what we have, so the result follows hwloc's own
 * order and stays deterministic. */
static bool osdev_preferred(hwloc_obj_t candidate, hwloc_obj_t incumbent)
{
    if (osdev_is_compute_backend(candidate)) {
        return !osdev_is_compute_backend(incumbent);
    }
    if (osdev_is_compute_backend(incumbent)) {
        return false;
    }
    if (osdev_is_render_node(candidate)) {
        return !osdev_is_render_node(incumbent);
    }
    return false;
}

/* Build the uuid PMIx reports for a device.  An application correlates the
 * device it was given against the ones it can see, so this grammar must not
 * vary between the paths that produce it - hence one function.  Returns
 * PMIX_ERR_TAKE_NEXT_OPTION for a device type that has no uuid form, which
 * the caller reads as "not a device I can report".
 *
 * "hostname" is the node this topology describes and is supplied by the
 * caller rather than read from pmix_globals: it names the node the device
 * lives on, not the node this code is running on, and those are the same
 * thing only when the topology is the local one.  See the note on
 * pmix_hwloc_get_devices() in the header. */
static pmix_status_t build_device_uuid(hwloc_obj_t osdev, const char *hostname,
                                       char **uuid)
{
    unsigned i;
    char *addr = NULL, *ngid = NULL, *sgid = NULL;
    int cnt;

    *uuid = NULL;

    switch (osdev->attr->osdev.type) {
        case HWLOC_OBJ_OSDEV_NETWORK:
            for (i = 0; i < osdev->infos_count; i++) {
                if (0 == strcasecmp(osdev->infos[i].name, "Address")) {
                    addr = osdev->infos[i].value;
                    break;
                }
            }
            if (NULL == addr) {
                return PMIX_ERROR;
            }
            /* could be IPv4 or IPv6 */
            cnt = countcolons(addr);
            if (5 == cnt) {
                pmix_asprintf(uuid, "ipv4://%s", addr);
            } else if (19 == cnt) {
                pmix_asprintf(uuid, "ipv6://%s", addr);
            } else {
                return PMIX_ERROR;
            }
            break;

        case HWLOC_OBJ_OSDEV_OPENFABRICS:
            for (i = 0; i < osdev->infos_count; i++) {
                if (0 == strcasecmp(osdev->infos[i].name, "NodeGUID")) {
                    ngid = osdev->infos[i].value;
                } else if (0 == strcasecmp(osdev->infos[i].name, "SysImageGUID")) {
                    sgid = osdev->infos[i].value;
                }
            }
            if (NULL == ngid || NULL == sgid) {
                return PMIX_ERROR;
            }
            pmix_asprintf(uuid, "fab://%s::%s", ngid, sgid);
            break;

        case HWLOC_OBJ_OSDEV_GPU:
        case HWLOC_OBJ_OSDEV_COPROC:
            pmix_asprintf(uuid, "gpu://%s::%s", hostname, osdev->name);
            break;

        case HWLOC_OBJ_OSDEV_BLOCK:
            pmix_asprintf(uuid, "blk://%s::%s", hostname, osdev->name);
            break;

        default:
            return PMIX_ERR_TAKE_NEXT_OPTION;
    }

    if (NULL == *uuid) {
        return PMIX_ERR_NOMEM;
    }
    return PMIX_SUCCESS;
}

/* Does this OS device answer to the caller's name?  Either spelling counts:
 * the OS name hwloc gave it, or the uuid PMIx reports for it. */
static bool osdev_named(hwloc_obj_t osdev, const char *hostname, const char *devid)
{
    char *uuid = NULL;
    bool found;

    if (NULL == devid || NULL == osdev->name) {
        return false;
    }
    if (0 == strcasecmp(devid, osdev->name)) {
        return true;
    }
    if (PMIX_SUCCESS != build_device_uuid(osdev, hostname, &uuid)) {
        return false;
    }
    found = (0 == strcasecmp(devid, uuid));
    free(uuid);
    return found;
}

/* one candidate device while we are still collecting */
typedef struct {
    pmix_list_item_t super;
    hwloc_obj_t osdev;     /* the OS device that names the function */
    hwloc_obj_t pci;       /* the function, or NULL */
    pmix_device_type_t type;
    /* the caller asked for this device by name or uuid.  Recorded per
     * candidate because the name they used may belong to an OS device other
     * than the one chosen to name the function - a fabric device and its
     * network device commonly share a function, and only one of the two can
     * be the name we report. */
    bool named;
} pmix_devcand_t;
static void cndcon(pmix_devcand_t *p)
{
    p->osdev = NULL;
    p->pci = NULL;
    p->type = PMIX_DEVTYPE_UNKNOWN;
    p->named = false;
}
static PMIX_CLASS_INSTANCE(pmix_devcand_t, pmix_list_item_t, cndcon, NULL);

/* Order by PCI bus id, with a device that has no PCI ancestor sorting last
 * by name.  The busid is rendered zero-padded and fixed-width, so comparing
 * the strings orders them exactly as comparing domain/bus/device/function
 * numerically would. */
static int devcmp(const void *a, const void *b)
{
    const pmix_hwloc_device_t *da = (const pmix_hwloc_device_t *) a;
    const pmix_hwloc_device_t *db = (const pmix_hwloc_device_t *) b;
    uint64_t ka, kb;

    /* NULL busid means no PCI ancestor: sort those last, by name */
    ka = (NULL == da->busid) ? UINT64_MAX : 0;
    kb = (NULL == db->busid) ? UINT64_MAX : 0;
    if (ka != kb) {
        return (ka < kb) ? -1 : 1;
    }
    if (NULL == da->busid) {
        return strcmp((NULL == da->dev.osname) ? "" : da->dev.osname,
                      (NULL == db->dev.osname) ? "" : db->dev.osname);
    }
    return strcmp(da->busid, db->busid);
}

pmix_status_t pmix_hwloc_get_devices(pmix_topology_t *topo,
                                     const char *hostname,
                                     pmix_device_type_t type,
                                     const char *devid,
                                     pmix_hwloc_device_t **devs,
                                     size_t *ndevs)
{
    hwloc_obj_t osdev, pci, tgt;
    pmix_list_t cands;
    pmix_devcand_t *c, *cnext, *match;
    pmix_hwloc_device_t *array = NULL;
    pmix_device_type_t dtype, matchtype;
    pmix_status_t rc = PMIX_SUCCESS;
    size_t n, ncands, ntypes;
    char *uuid = NULL;

    /* the hostname is not optional: a uuid that names the wrong node is
     * worse than no uuid, and defaulting it to the local one is exactly how
     * that happens */
    if (NULL == topo || NULL == topo->topology || NULL == hostname
        || NULL == devs || NULL == ndevs) {
        return PMIX_ERR_BAD_PARAM;
    }
    *devs = NULL;
    *ndevs = 0;

    /* an unspecified type means every type we know */
    if (PMIX_DEVTYPE_UNKNOWN == type) {
        ntypes = sizeof(table) / sizeof(pmix_type_conversion_t);
        for (n = 0; n < ntypes; n++) {
            type |= table[n].pxtype;
        }
    }

    PMIX_CONSTRUCT(&cands, pmix_list_t);

    /* Collect one candidate per PCI function.  Walking OS devices and
     * grouping them is what makes a GPU exposing a card node, a render node
     * and a vendor node come out as one device instead of three. */
    osdev = hwloc_get_next_osdev(topo->topology, NULL);
    while (NULL != osdev) {
        dtype = PMIX_DEVTYPE_UNKNOWN;
        ntypes = sizeof(table) / sizeof(pmix_type_conversion_t);
        for (n = 0; n < ntypes; n++) {
            if (osdev->attr->osdev.type == table[n].hwtype) {
                dtype = table[n].pxtype;
                break;
            }
        }
        /* A GPU is a GPU whether hwloc labelled its compute node GPU or
         * COPROC: "cuda0" is a coprocessor OS device and "renderD128" a gpu
         * one, and they are commonly two views of the same card.  Since the
         * function is named by whichever of them is most useful, a caller
         * asking for GPU alone would otherwise lose every GPU on a machine
         * whose vendor backend is loaded. */
        matchtype = dtype;
        if (PMIX_DEVTYPE_GPU == dtype || PMIX_DEVTYPE_COPROC == dtype) {
            matchtype = PMIX_DEVTYPE_GPU | PMIX_DEVTYPE_COPROC;
        }
        if (PMIX_DEVTYPE_UNKNOWN == dtype || !(type & matchtype)) {
            goto next;
        }
        pci = pci_ancestor(osdev);
        /* a display-class device has to earn its place */
        if ((PMIX_DEVTYPE_GPU == dtype || PMIX_DEVTYPE_COPROC == dtype)
            && !is_compute_gpu(osdev, pci)) {
            goto next;
        }
        /* have we already got this function? */
        match = NULL;
        if (NULL != pci) {
            PMIX_LIST_FOREACH (c, &cands, pmix_devcand_t) {
                if (c->pci == pci) {
                    match = c;
                    break;
                }
            }
        }
        if (NULL != match) {
            /* the name the caller used wins the right to name the function -
             * asking for mlx5_0 and being told about ib0 is not an answer */
            if (osdev_named(osdev, hostname, devid)) {
                match->osdev = osdev;
                match->type = dtype;
                match->named = true;
            } else if (!match->named && osdev_preferred(osdev, match->osdev)) {
                match->osdev = osdev;
                match->type = dtype;
            }
            goto next;
        }
        c = PMIX_NEW(pmix_devcand_t);
        if (NULL == c) {
            rc = PMIX_ERR_NOMEM;
            goto cleanup;
        }
        c->osdev = osdev;
        c->pci = pci;
        c->type = dtype;
        c->named = osdev_named(osdev, hostname, devid);
        pmix_list_append(&cands, &c->super);

    next:
        osdev = hwloc_get_next_osdev(topo->topology, osdev);
    }

    /* drop the ones we cannot name, or that the caller did not ask for by
     * name, then build the result */
    PMIX_LIST_FOREACH_SAFE (c, cnext, &cands, pmix_devcand_t) {
        rc = build_device_uuid(c->osdev, hostname, &uuid);
        if (PMIX_SUCCESS != rc) {
            /* a device we cannot name is not one we can report */
            pmix_list_remove_item(&cands, &c->super);
            PMIX_RELEASE(c);
            continue;
        }
        if (NULL != devid && !c->named) {
            free(uuid);
            pmix_list_remove_item(&cands, &c->super);
            PMIX_RELEASE(c);
            continue;
        }
        free(uuid);
        uuid = NULL;
    }
    rc = PMIX_SUCCESS;

    ncands = pmix_list_get_size(&cands);
    if (0 == ncands) {
        /* not an error - the caller decides what an empty result means */
        goto cleanup;
    }

    array = (pmix_hwloc_device_t *) calloc(ncands, sizeof(pmix_hwloc_device_t));
    if (NULL == array) {
        rc = PMIX_ERR_NOMEM;
        goto cleanup;
    }

    n = 0;
    PMIX_LIST_FOREACH (c, &cands, pmix_devcand_t) {
        PMIx_Device_construct(&array[n].dev);
        rc = build_device_uuid(c->osdev, hostname, &array[n].dev.uuid);
        if (PMIX_SUCCESS != rc) {
            goto cleanup;
        }
        array[n].dev.osname = strdup(c->osdev->name);
        array[n].dev.type = c->type;
        vendor_id_of(c->osdev, c->pci, &array[n].vendor_id, &array[n].vendor);
        if (NULL != c->pci) {
            pmix_asprintf(&array[n].busid, "%04x:%02x:%02x.%01x",
                          c->pci->attr->pcidev.domain, c->pci->attr->pcidev.bus,
                          c->pci->attr->pcidev.dev, c->pci->attr->pcidev.func);
        }
        /* the device's locality is the nearest ancestor that has a cpuset */
        if (NULL == c->osdev->cpuset) {
            tgt = c->osdev->parent;
            while (NULL != tgt && NULL == tgt->cpuset) {
                tgt = tgt->parent;
            }
        } else {
            tgt = c->osdev;
        }
        array[n].locality = tgt;
        ++n;
    }

    qsort(array, ncands, sizeof(pmix_hwloc_device_t), devcmp);

    *devs = array;
    *ndevs = ncands;
    array = NULL;

cleanup:
    if (NULL != array) {
        pmix_hwloc_release_devices(array, ncands);
    }
    PMIX_LIST_DESTRUCT(&cands);
    return rc;
}

void pmix_hwloc_release_devices(pmix_hwloc_device_t *devs, size_t ndevs)
{
    size_t n;

    if (NULL == devs) {
        return;
    }
    for (n = 0; n < ndevs; n++) {
        PMIx_Device_destruct(&devs[n].dev);
        if (NULL != devs[n].busid) {
            free(devs[n].busid);
        }
        if (NULL != devs[n].vendor_id) {
            free(devs[n].vendor_id);
        }
        if (NULL != devs[n].vendor) {
            free(devs[n].vendor);
        }
        /* locality is borrowed from the topology */
    }
    free(devs);
}

pmix_status_t pmix_hwloc_check_vendor(pmix_topology_t *topo,
                                      unsigned short vendorID,
                                      uint16_t class)
{
    hwloc_obj_t device;

    if (NULL == topo || NULL == topo->topology) {
        return PMIX_ERR_BAD_PARAM;
    }
    if (NULL == topo->source || 0 != strncasecmp(topo->source, "hwloc", 5)) {
        return PMIX_ERR_TAKE_NEXT_OPTION;
    }

    device = hwloc_get_next_pcidev(topo->topology, NULL);
    while (NULL != device) {
        if (class == device->attr->pcidev.class_id &&
            device->attr->pcidev.vendor_id == vendorID) {
            return PMIX_SUCCESS;
        }
        device = hwloc_get_next_pcidev(topo->topology, device);
    }
    return PMIX_ERR_NOT_AVAILABLE;
}


static int set_flags(hwloc_topology_t topo, unsigned int flags)
{
    int ret = hwloc_topology_set_io_types_filter(topo, HWLOC_TYPE_FILTER_KEEP_IMPORTANT);
    if (0 != ret) {
        return ret;
    }
    if (0 != hwloc_topology_set_flags(topo, flags)) {
        return PMIX_ERR_INIT;
    }
    // Blacklist the "gl" component due to potential conflicts.
    // See "https://github.com/open-mpi/ompi/issues/10025" for
    // an explanation. Sadly, HWLOC doesn't define version numbers
    // until v2.0, so we cannot check versions here. Fortunately,
    // the blacklist ability was added in HWLOC v2.1, so we can't
    // do it for earlier versions anyway.
    hwloc_topology_set_components(topo, HWLOC_TOPOLOGY_COMPONENTS_FLAG_BLACKLIST, "gl");

    return PMIX_SUCCESS;
}

static char *popstr(pmix_cb_t *cb)
{
    pmix_list_t *kvs = &cb->kvs;
    pmix_kval_t *kv;
    char *str;

    if (1 != pmix_list_get_size(kvs)) {
        return NULL;
    }
    kv = (pmix_kval_t *) pmix_list_get_first(kvs);
    if (PMIX_STRING != kv->value->type) {
        return NULL;
    }
    str = kv->value->data.string;
    kv->value->data.string = NULL;
    kv = (pmix_kval_t *) pmix_list_remove_first(kvs);
    while (NULL != kv) {
        PMIX_RELEASE(kv);
        kv = (pmix_kval_t *) pmix_list_remove_first(kvs);
    }
    return str;
}

static size_t popsize(pmix_cb_t *cb)
{
    pmix_list_t *kvs = &cb->kvs;
    pmix_kval_t *kv;
    size_t sz;

    if (1 != pmix_list_get_size(kvs)) {
        return UINT64_MAX;
    }
    kv = (pmix_kval_t *) pmix_list_get_first(kvs);
    if (PMIX_SIZE != kv->value->type) {
        return UINT64_MAX;
    }
    sz = kv->value->data.size;
    kv = (pmix_kval_t *) pmix_list_remove_first(kvs);
    while (NULL != kv) {
        PMIX_RELEASE(kv);
        kv = (pmix_kval_t *) pmix_list_remove_first(kvs);
    }
    return sz;
}

static pmix_topology_t *popptr(pmix_cb_t *cb)
{
    pmix_list_t *kvs = &cb->kvs;
    pmix_kval_t *kv;
    pmix_topology_t *t;

    if (1 != pmix_list_get_size(kvs)) {
        return NULL;
    }
    kv = (pmix_kval_t *) pmix_list_get_first(kvs);
    if (PMIX_TOPO != kv->value->type) {
        return NULL;
    }
    t = kv->value->data.topo;
    kv->value->data.topo = NULL;
    kv = (pmix_kval_t *) pmix_list_remove_first(kvs);
    while (NULL != kv) {
        PMIX_RELEASE(kv);
        kv = (pmix_kval_t *) pmix_list_remove_first(kvs);
    }
    return t;
}

static pmix_status_t load_xml(char *xml)
{
    /* load the topology */
    if (0 != hwloc_topology_init((hwloc_topology_t *) &pmix_globals.topology.topology)) {
        return PMIX_ERROR;
    }
    if (0 != hwloc_topology_set_xmlbuffer(pmix_globals.topology.topology, xml, strlen(xml) + 1)) {
        hwloc_topology_destroy(pmix_globals.topology.topology);
        return PMIX_ERROR;
    }
    /* since we are loading this from an external source, we have to
     * explicitly set a flag so hwloc sets things up correctly
     */
    if (0 != set_flags(pmix_globals.topology.topology, HWLOC_TOPOLOGY_FLAG_IS_THISSYSTEM)) {
        hwloc_topology_destroy(pmix_globals.topology.topology);
        return PMIX_ERROR;
    }
    /* now load the topology */
    if (0 != hwloc_topology_load(pmix_globals.topology.topology)) {
        hwloc_topology_destroy(pmix_globals.topology.topology);
        return PMIX_ERROR;
    }
    pmix_globals.topology.source = strdup("hwloc"); // don't know the version?
    return PMIX_SUCCESS;
}

static void print_maps(void)
{

    FILE *maps_file = fopen("/proc/self/maps", "r");
    if (maps_file) {
        char line[256];
        pmix_output(0, "%s Dumping /proc/self/maps", PMIX_NAME_PRINT(&pmix_globals.myid));
        while (fgets(line, sizeof(line), maps_file) != NULL) {
            char *end = strchr(line, '\n');
            if (end) {
                *end = '\0';
            }
            pmix_output(0, "%s", line);
        }
        fclose(maps_file);
    }
}

static int get_locality_string_by_depth(int d, hwloc_cpuset_t cpuset, hwloc_cpuset_t result)
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
    for (w = 0; w < width; w++) {
        /* get the object at this depth/index */
        obj = hwloc_get_obj_by_depth(pmix_globals.topology.topology, d, w);
        /* see if the location intersects with it */
        if (hwloc_bitmap_intersects(obj->cpuset, cpuset)) {
            hwloc_bitmap_set(result, w);
        }
    }

    return 0;
}

static int enough_space(const char *filename, size_t space_req, uint64_t *space_avail, bool *result)
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
    if (NULL == last_sep) {
        /* no separator - cannot determine a parent directory */
        rc = PMIX_ERR_BAD_PARAM;
        goto out;
    }
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
