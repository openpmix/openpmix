/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2006-2013 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2009-2012 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2011      Oak Ridge National Labs.  All rights reserved.
 * Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2016      IBM Corporation.  All rights reserved.
 * Copyright (c) 2021-2023 Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#include "src/include/pmix_config.h"
#include "include/pmix.h"
#include "include/pmix_server.h"
#include "src/include/pmix_globals.h"
#include "src/include/pmix_types.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <hwloc.h>

#include "src/class/pmix_list.h"
#include "src/include/pmix_globals.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_environ.h"
#include "src/util/pmix_printf.h"

#include "simptest.h"

static pmix_server_module_t mymodule = {0};

typedef struct {
    mylock_t lock;
    pmix_status_t status;
    pmix_info_t *info;
    size_t ninfo;
} mycaddy_t;

static void output_usage(void)
{
    fprintf(stderr, "Usage: simpdist [OPTIONS]\n");
    fprintf(stderr, "   --gpu <devid>    Specify the GPU assigned to me,\n");
    fprintf(stderr, "                    returns conjoined NICs\n");
    fprintf(stderr, "   --nic <devid>    Specify the NIC assigned to me,\n");
    fprintf(stderr, "                    returns conjoined GPUs\n");
    fprintf(stderr, "   --bind <loc>     Specify the location where this proc is\n");
    fprintf(stderr, "                    to be bound - defaults to detected binding if\n");
    fprintf(stderr, "                    not provided\n");
    fprintf(stderr, "   --list <nic|gpu> List the NIC or GPU IDs in this topology\n");
    fprintf(stderr, "   --num <devtype>  Output the number of objects of the given type\n");
    fprintf(stderr, "                    in this topology\n");
    fprintf(stderr, "\nBinding location can be given as #:type - e.g., 3:L2 indicates\n");
    fprintf(stderr, "that the proc is to be considered bound to the third L2 cache.\n");
    fprintf(stderr, "Similarly, 2-3:L2 indicates that the proc is to be considered\n");
    fprintf(stderr, "bound to the second-thru-third L2 caches, while 2,4,6:PU indicates\n");
    fprintf(stderr, "that the proc is to be considered bound to the #2, #4, and #6\n");
    fprintf(stderr, "hwthreads\n");
    fprintf(stderr, "\nsimpdist will output the distances to corresponding\n");
    fprintf(stderr, "devices that conjoin the specified device\n");
}

static hwloc_obj_type_t convert_type(const char *name, char **strname)
{
    if (0 == strncasecmp(name, "L3", 2)) {
        if (NULL != strname) {
            *strname = "L3CACHE";
        }
        return HWLOC_OBJ_L3CACHE;
    } else if (0 == strncasecmp(name, "L2", 2)) {
        if (NULL != strname) {
            *strname = "L2CACHE";
        }
        return HWLOC_OBJ_L2CACHE;
    } else if (0 == strncasecmp(name, "L1", 2)) {
        if (NULL != strname) {
            *strname = "L1CACHE";
        }
        return HWLOC_OBJ_L1CACHE;
    } else if (0 == strncasecmp(name, "C", 1)) {
        if (NULL != strname) {
            *strname = "CORE";
        }
        return HWLOC_OBJ_CORE;
    } else if (0 == strncasecmp(name, "HWT", 3) ||
               0 == strncasecmp(name, "PU", 2)) {
        if (NULL != strname) {
            *strname = "HWTHREAD";
        }
        return HWLOC_OBJ_PU;
    } else if (0 == strncasecmp(name, "NU", 2)) {
        if (NULL != strname) {
            *strname = "NUMA";
        }
        return HWLOC_OBJ_NUMANODE;
    } else if (0 == strncasecmp(name, "PKG", 3)) {
        if (NULL != strname) {
            *strname = "PACKAGE";
        }
        return HWLOC_OBJ_PACKAGE;
    } else {
        fprintf(stderr, "UNRECOGNIZED HWLOC TYPE: %s\n", name);
        return HWLOC_OBJ_TYPE_MAX;
    }
}

static pmix_status_t construct_cpuset(hwloc_topology_t topo,
                                      hwloc_bitmap_t cpuset,
                                      const char *bind)
{
    char **map, **rngs, **endpts;
    int n;
    unsigned m, start, stop;
    hwloc_obj_type_t type;
    hwloc_obj_t obj;

    map = PMIx_Argv_split(bind, ':');
    if (NULL == map[1]) {
        fprintf(stderr, "Bad param - requires #:type\n");
        return PMIX_ERR_BAD_PARAM;
    }
    // get the object type
    type = convert_type(map[1], NULL);
    if (HWLOC_OBJ_TYPE_MAX == type) {
        PMIx_Argv_free(map);
        return PMIX_ERR_BAD_PARAM;
    }
    // number could be comma-delim set of ranges
    rngs = PMIx_Argv_split(map[0], ',');
    // cycle thru the ranges
    for (n=0; NULL != rngs[n]; n++) {
        endpts = PMIx_Argv_split(rngs[n], '-');
        start = strtoul(endpts[0], NULL, 10);
        if (NULL == endpts[1]) {
            // just a number
            obj = hwloc_get_obj_by_type(topo, type, start);
            hwloc_bitmap_or(cpuset, cpuset, obj->cpuset);
        } else {
            // cycle across the range
            stop = strtoul(endpts[1], NULL, 10);
            for (m=start; m <=stop; m++) {
                obj = hwloc_get_obj_by_type(topo, type, m);
                hwloc_bitmap_or(cpuset, cpuset, obj->cpuset);
            }
        }
        PMIx_Argv_free(endpts);
    }
    PMIx_Argv_free(rngs);
    PMIx_Argv_free(map);
    return PMIX_SUCCESS;
}

int main(int argc, char **argv)
{
    pmix_info_t *info;
    pmix_status_t rc;
    size_t ninfo;
    int exit_code = 0;
    pmix_value_t *val;
    size_t n;
    int m;
    unsigned ui;
    char *gpu = NULL, *nic = NULL, *bind = NULL, *tobj, *tmp;
    bool list = false;
    pmix_topology_t *mytopo;
    pmix_proc_t myproc;
    pmix_cpuset_t mycpuset;
    pmix_device_distance_t *distances;
    size_t ndist;
    pmix_device_type_t type = PMIX_DEVTYPE_OPENFABRICS |
                              PMIX_DEVTYPE_NETWORK |
                              PMIX_DEVTYPE_COPROC |
                              PMIX_DEVTYPE_GPU;
    hwloc_obj_type_t objtype = HWLOC_OBJ_TYPE_MAX;

    for (m=1; m < argc; m++) {
        if (0 == strcmp(argv[m], "-h") ||
            0 == strcmp(argv[m], "--help")) {
            output_usage();
            exit(0);
        } else if (0 == strcmp(argv[m], "--gpu")) {
            if (NULL == argv[m+1]) {
                output_usage();
                exit(1);
            }
            gpu = argv[m+1];
            ++m;
        } else if (0 == strcmp(argv[m], "--nic")) {
            if (NULL == argv[m+1]) {
                output_usage();
                exit(1);
            }
            nic = argv[m+1];
            ++m;
        } else if (0 == strcmp(argv[m], "--bind")) {
            if (NULL == argv[m+1]) {
                output_usage();
                exit(1);
            }
            bind = argv[m+1];
            ++m;
        } else if (0 == strcmp(argv[m], "--list")) {
            if (NULL == argv[m+1]) {
                output_usage();
                exit(1);
            }
            tobj = argv[m+1];
            list = true;
            ++m;
        } else if (0 == strcmp(argv[m], "--num")) {
            if (NULL == argv[m+1]) {
                output_usage();
                exit(1);
            }
            // convert the type
            objtype = convert_type(argv[m+1], &tobj);
            if (HWLOC_OBJ_TYPE_MAX == objtype) {
                exit(1);
            }
            ++m;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[m]);
            output_usage();
            exit(1);
        }
    }
    if (NULL == gpu && NULL == nic &&
        NULL == bind && HWLOC_OBJ_TYPE_MAX == objtype) {
        output_usage();
        exit(1);
    }

    if (PMIX_SUCCESS != (rc = PMIx_server_init(&mymodule, NULL, 0))) {
        fprintf(stderr, "Init failed with error %d\n", rc);
        return rc;
    }

    /* get my procID */
    rc = PMIx_Get(NULL, PMIX_PROCID, NULL, 0, &val);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "Get of my procID failed: %s\n", PMIx_Error_string(rc));
        goto cleanup;
    }
    PMIX_LOAD_PROCID(&myproc, val->data.proc->nspace, val->data.proc->rank);
    PMIX_VALUE_FREE(val, 1);

    /* get my topology */
    rc = PMIx_Get(&myproc, PMIX_TOPOLOGY2, NULL, 0, &val);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "Get of my topology failed: %s\n", PMIx_Error_string(rc));
        goto cleanup;
    }
    mytopo = val->data.topo;
    val->data.topo = NULL;
    PMIX_VALUE_FREE(val, 1);
    fprintf(stderr, "Got my topology: Source = %s\n",
            (NULL == mytopo->source) ? "NULL" : mytopo->source);

    // did they just want to know how many obj's of a given
    // type are in this topology?
    if (HWLOC_OBJ_TYPE_MAX != objtype) {
        // they asked for the number of objects
        ui = hwloc_get_nbobjs_by_type(mytopo->topology, objtype);
        fprintf(stderr, "There are %u %s in this topology\n", ui, tobj);
        goto cleanup;
    }

    // did they just want to get a list of NICs or GPUs?
    if (list) {

    }

    /* get my cpuset */
    PMIX_CPUSET_CONSTRUCT(&mycpuset);
    if (NULL == bind) {
        rc = PMIx_Get_cpuset(&mycpuset, PMIX_CPUBIND_PROCESS);
        if (PMIX_SUCCESS != rc) {
            fprintf(stderr, "Get of my cpuset failed: %s\n", PMIx_Error_string(rc));
            goto cleanup;
        }
    } else {
        mycpuset.bitmap = hwloc_bitmap_alloc();
        rc = construct_cpuset(mytopo->topology, mycpuset.bitmap, bind);
        if (PMIX_SUCCESS != rc) {
            goto cleanup;
        }
        mycpuset.source = "hwloc";
    }
    PMIx_server_generate_cpuset_string(&mycpuset, &tmp);
    fprintf(stderr, "My cpuset: %s\n", tmp);
    free(tmp);

    ninfo = 1;
    PMIX_INFO_CREATE(info, ninfo);
    PMIX_INFO_LOAD(&info[0], PMIX_DEVICE_TYPE, &type, PMIX_DEVTYPE);
    rc = PMIx_Compute_distances(mytopo, &mycpuset, info, ninfo, &distances, &ndist);
    PMIX_INFO_FREE(info, ninfo);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "Compute distances failed: %s\n", PMIx_Error_string(rc));
    } else {
        for (n = 0; n < ndist; n++) {
            fprintf(stderr, "Device[%d]: UUID %s OSname: %s Type %s MinDist %u MaxDist %u\n",
                    (int) n, distances[n].uuid, distances[n].osname,
                    PMIx_Device_type_string(distances[n].type), distances[n].mindist,
                    distances[n].maxdist);
        }
    }

cleanup:
    if (PMIX_SUCCESS != rc) {
        exit_code = rc;
    }
    /* finalize the server library */
    if (PMIX_SUCCESS != (rc = PMIx_server_finalize())) {
        fprintf(stderr, "Finalize failed with error %s\n", PMIx_Error_string(rc));
        exit_code = rc;
    }

    if (0 == exit_code) {
        fprintf(stderr, "Test finished OK!\n");
    } else {
        fprintf(stderr, "TEST FAILED WITH ERROR %s\n", PMIx_Error_string(exit_code));
    }

    return exit_code;
}
