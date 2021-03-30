/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2016      Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2018      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 *
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
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

#include "include/pmix_common.h"
#include "src/include/pmix_globals.h"

#include "src/class/pmix_list.h"
#include "src/mca/ploc/base/base.h"
#include "src/util/error.h"

pmix_status_t pmix_ploc_base_setup_topology(pmix_info_t *info, size_t ninfo)
{
    pmix_ploc_base_active_module_t *active;
    pmix_status_t rc;

    if (!pmix_ploc_globals.initialized) {
        return PMIX_ERR_INIT;
    }

    pmix_output_verbose(2, pmix_ploc_base_framework.framework_output, "ploc:setup_topology called");

    /* if we are not a server, then this is an error */
    if (!PMIX_PROC_IS_SERVER(&pmix_globals.mypeer->proc_type)
        && !PMIX_PROC_IS_LAUNCHER(&pmix_globals.mypeer->proc_type)) {
        return PMIX_ERR_NOT_SUPPORTED;
    }

    /* process the request */
    PMIX_LIST_FOREACH (active, &pmix_ploc_globals.actives, pmix_ploc_base_active_module_t) {
        if (NULL != active->module->setup_topology) {
            rc = active->module->setup_topology(info, ninfo);
            if (PMIX_SUCCESS == rc) {
                return rc;
            }
            if (PMIX_ERR_TAKE_NEXT_OPTION != rc) {
                /* true error */
                return rc;
            }
        }
    }

    return PMIX_ERR_NOT_SUPPORTED;
}

pmix_status_t pmix_ploc_base_load_topology(pmix_topology_t *topo)
{
    pmix_ploc_base_active_module_t *active;
    pmix_status_t rc;

    if (!pmix_ploc_globals.initialized) {
        return PMIX_ERR_INIT;
    }

    pmix_output_verbose(2, pmix_ploc_base_framework.framework_output, "ploc:load_topology called");

    /* process the request */
    PMIX_LIST_FOREACH (active, &pmix_ploc_globals.actives, pmix_ploc_base_active_module_t) {
        if (NULL != active->module->load_topology) {
            rc = active->module->load_topology(topo);
            if (PMIX_SUCCESS == rc) {
                return rc;
            }
            if (PMIX_ERR_TAKE_NEXT_OPTION != rc) {
                /* true error */
                return rc;
            }
        }
    }

    return PMIX_ERR_NOT_SUPPORTED;
}

pmix_status_t pmix_ploc_base_generate_cpuset_string(const pmix_cpuset_t *cpuset,
                                                    char **cpuset_string)
{
    pmix_ploc_base_active_module_t *active;
    pmix_status_t rc;

    if (!pmix_ploc_globals.initialized) {
        return PMIX_ERR_INIT;
    }

    pmix_output_verbose(2, pmix_ploc_base_framework.framework_output,
                        "ploc:generate_cpuset_string called");

    /* process the request */
    PMIX_LIST_FOREACH (active, &pmix_ploc_globals.actives, pmix_ploc_base_active_module_t) {
        if (NULL != active->module->get_cpuset) {
            rc = active->module->generate_cpuset_string(cpuset, cpuset_string);
            if (PMIX_SUCCESS == rc) {
                return rc;
            }
            if (PMIX_ERR_TAKE_NEXT_OPTION != rc) {
                /* true error */
                return rc;
            }
        }
    }

    return PMIX_ERR_NOT_SUPPORTED;
}

pmix_status_t pmix_ploc_base_parse_cpuset_string(const char *cpuset_string, pmix_cpuset_t *cpuset)
{
    pmix_ploc_base_active_module_t *active;
    pmix_status_t rc;

    if (!pmix_ploc_globals.initialized) {
        return PMIX_ERR_INIT;
    }

    pmix_output_verbose(2, pmix_ploc_base_framework.framework_output, "ploc:get_cpuset called");

    /* process the request */
    PMIX_LIST_FOREACH (active, &pmix_ploc_globals.actives, pmix_ploc_base_active_module_t) {
        if (NULL != active->module->parse_cpuset_string) {
            rc = active->module->parse_cpuset_string(cpuset_string, cpuset);
            if (PMIX_SUCCESS == rc) {
                return rc;
            }
            if (PMIX_ERR_TAKE_NEXT_OPTION != rc) {
                /* true error */
                return rc;
            }
        }
    }

    return PMIX_ERR_NOT_SUPPORTED;
}

pmix_status_t pmix_ploc_base_generate_locality_string(const pmix_cpuset_t *cpuset, char **locality)
{
    pmix_ploc_base_active_module_t *active;
    pmix_status_t rc;

    if (!pmix_ploc_globals.initialized) {
        return PMIX_ERR_INIT;
    }

    pmix_output_verbose(2, pmix_ploc_base_framework.framework_output,
                        "ploc:generate_locality_string called");

    /* if we are not a server, then this is an error */
    if (!PMIX_PROC_IS_SERVER(&pmix_globals.mypeer->proc_type)
        && !PMIX_PROC_IS_LAUNCHER(&pmix_globals.mypeer->proc_type)) {
        return PMIX_ERR_NOT_SUPPORTED;
    }

    /* process the request */
    PMIX_LIST_FOREACH (active, &pmix_ploc_globals.actives, pmix_ploc_base_active_module_t) {
        if (NULL != active->module->generate_locality_string) {
            rc = active->module->generate_locality_string(cpuset, locality);
            if (PMIX_SUCCESS == rc) {
                return rc;
            }
            if (PMIX_ERR_TAKE_NEXT_OPTION != rc) {
                /* true error */
                return rc;
            }
        }
    }

    return PMIX_ERR_NOT_SUPPORTED;
}

pmix_status_t pmix_ploc_base_get_relative_locality(const char *loc1, const char *loc2,
                                                   pmix_locality_t *locality)
{
    pmix_ploc_base_active_module_t *active;
    pmix_status_t rc;

    if (!pmix_ploc_globals.initialized) {
        return PMIX_ERR_INIT;
    }

    pmix_output_verbose(2, pmix_ploc_base_framework.framework_output,
                        "ploc:get_relative_locality called");

    /* process the request */
    PMIX_LIST_FOREACH (active, &pmix_ploc_globals.actives, pmix_ploc_base_active_module_t) {
        if (NULL != active->module->get_relative_locality) {
            rc = active->module->get_relative_locality(loc1, loc2, locality);
            if (PMIX_SUCCESS == rc) {
                return rc;
            }
            if (PMIX_ERR_TAKE_NEXT_OPTION != rc) {
                /* true error */
                return rc;
            }
        }
    }

    return PMIX_ERR_NOT_SUPPORTED;
}

pmix_status_t pmix_ploc_base_get_cpuset(pmix_cpuset_t *cpuset, pmix_bind_envelope_t ref)
{
    pmix_ploc_base_active_module_t *active;
    pmix_status_t rc;

    if (!pmix_ploc_globals.initialized) {
        return PMIX_ERR_INIT;
    }

    pmix_output_verbose(2, pmix_ploc_base_framework.framework_output, "ploc:get_cpuset called");

    /* process the request */
    PMIX_LIST_FOREACH (active, &pmix_ploc_globals.actives, pmix_ploc_base_active_module_t) {
        if (NULL != active->module->get_cpuset) {
            rc = active->module->get_cpuset(cpuset, ref);
            if (PMIX_SUCCESS == rc) {
                return rc;
            }
            if (PMIX_ERR_TAKE_NEXT_OPTION != rc) {
                /* true error */
                return rc;
            }
        }
    }

    return PMIX_ERR_NOT_SUPPORTED;
}

pmix_status_t pmix_ploc_base_compute_distances(pmix_topology_t *topo, pmix_cpuset_t *cpuset,
                                               pmix_info_t info[], size_t ninfo,
                                               pmix_device_distance_t **dist, size_t *ndist)
{
    pmix_ploc_base_active_module_t *active;
    pmix_status_t rc;

    if (!pmix_ploc_globals.initialized) {
        return PMIX_ERR_INIT;
    }

    pmix_output_verbose(2, pmix_ploc_base_framework.framework_output,
                        "ploc:compute_distances called");

    /* process the request */
    PMIX_LIST_FOREACH (active, &pmix_ploc_globals.actives, pmix_ploc_base_active_module_t) {
        if (NULL != active->module->compute_distances) {
            rc = active->module->compute_distances(topo, cpuset, info, ninfo, dist, ndist);
            if (PMIX_SUCCESS == rc) {
                return rc;
            }
            if (PMIX_ERR_TAKE_NEXT_OPTION != rc) {
                /* indicate that we were able to process the
                 * request, but didn't get a successful answer.
                 * We need to return a PMIX_ERR_NOT_AVAILABLE
                 * response so the caller knows not to raise
                 * the request to our host */
                return PMIX_ERR_NOT_AVAILABLE;
            }
        }
    }

    return PMIX_ERR_NOT_SUPPORTED;
}

pmix_status_t pmix_ploc_base_pack_cpuset(pmix_buffer_t *buf, pmix_cpuset_t *src,
                                         pmix_pointer_array_t *regtypes)
{
    pmix_ploc_base_active_module_t *active;
    pmix_status_t rc;

    if (!pmix_ploc_globals.initialized) {
        return PMIX_ERR_INIT;
    }

    pmix_output_verbose(2, pmix_ploc_base_framework.framework_output, "ploc:pack_cpuset called");

    /* process the request */
    PMIX_LIST_FOREACH (active, &pmix_ploc_globals.actives, pmix_ploc_base_active_module_t) {
        if (NULL != active->module->pack_cpuset) {
            rc = active->module->pack_cpuset(buf, src, regtypes);
            if (PMIX_SUCCESS == rc) {
                return rc;
            }
            if (PMIX_ERR_TAKE_NEXT_OPTION != rc) {
                /* true error */
                return rc;
            }
        }
    }

    return PMIX_ERR_NOT_SUPPORTED;
}

pmix_status_t pmix_ploc_base_unpack_cpuset(pmix_buffer_t *buf, pmix_cpuset_t *dest,
                                           pmix_pointer_array_t *regtypes)
{
    pmix_ploc_base_active_module_t *active;
    pmix_status_t rc;

    if (!pmix_ploc_globals.initialized) {
        return PMIX_ERR_INIT;
    }

    pmix_output_verbose(2, pmix_ploc_base_framework.framework_output, "ploc:unpack_cpuset called");

    /* process the request */
    PMIX_LIST_FOREACH (active, &pmix_ploc_globals.actives, pmix_ploc_base_active_module_t) {
        if (NULL != active->module->unpack_cpuset) {
            rc = active->module->unpack_cpuset(buf, dest, regtypes);
            if (PMIX_SUCCESS == rc) {
                return rc;
            }
            if (PMIX_ERR_TAKE_NEXT_OPTION != rc) {
                /* true error */
                return rc;
            }
        }
    }

    return PMIX_ERR_NOT_SUPPORTED;
}

pmix_status_t pmix_ploc_base_copy_cpuset(pmix_cpuset_t *dest, pmix_cpuset_t *src)
{
    pmix_ploc_base_active_module_t *active;
    pmix_status_t rc;

    if (!pmix_ploc_globals.initialized) {
        return PMIX_ERR_INIT;
    }

    pmix_output_verbose(2, pmix_ploc_base_framework.framework_output, "ploc:copy_cpuset called");

    /* process the request */
    PMIX_LIST_FOREACH (active, &pmix_ploc_globals.actives, pmix_ploc_base_active_module_t) {
        if (NULL != active->module->copy_cpuset) {
            rc = active->module->copy_cpuset(dest, src);
            if (PMIX_SUCCESS == rc) {
                return rc;
            }
            if (PMIX_ERR_TAKE_NEXT_OPTION != rc) {
                /* true error */
                return rc;
            }
        }
    }

    return PMIX_ERR_NOT_SUPPORTED;
}

char *pmix_ploc_base_print_cpuset(pmix_cpuset_t *src)
{
    pmix_ploc_base_active_module_t *active;
    char *string;

    if (!pmix_ploc_globals.initialized) {
        return NULL;
    }

    pmix_output_verbose(2, pmix_ploc_base_framework.framework_output, "ploc:print_cpuset called");

    /* process the request */
    PMIX_LIST_FOREACH (active, &pmix_ploc_globals.actives, pmix_ploc_base_active_module_t) {
        if (NULL != active->module->print_cpuset) {
            string = active->module->print_cpuset(src);
            if (NULL != string) {
                return string;
            }
        }
    }

    return NULL;
}

void pmix_ploc_base_destruct_cpuset(pmix_cpuset_t *cpuset)
{
    pmix_ploc_base_active_module_t *active;
    pmix_status_t rc;

    if (!pmix_ploc_globals.initialized) {
        return;
    }

    pmix_output_verbose(2, pmix_ploc_base_framework.framework_output,
                        "ploc:destruct_cpuset called");

    /* process the request */
    PMIX_LIST_FOREACH (active, &pmix_ploc_globals.actives, pmix_ploc_base_active_module_t) {
        if (NULL != active->module->destruct_cpuset) {
            rc = active->module->destruct_cpuset(cpuset);
            if (PMIX_SUCCESS == rc) {
                return;
            }
        }
    }
}

void pmix_ploc_base_release_cpuset(pmix_cpuset_t *ptr, size_t sz)
{
    pmix_ploc_base_active_module_t *active;
    pmix_status_t rc;

    if (!pmix_ploc_globals.initialized) {
        return;
    }

    pmix_output_verbose(2, pmix_ploc_base_framework.framework_output, "ploc:release_cpuset called");

    /* process the request */
    PMIX_LIST_FOREACH (active, &pmix_ploc_globals.actives, pmix_ploc_base_active_module_t) {
        if (NULL != active->module->release_cpuset) {
            rc = active->module->release_cpuset(ptr, sz);
            if (PMIX_SUCCESS == rc) {
                return;
            }
        }
    }
}

pmix_status_t pmix_ploc_base_pack_topology(pmix_buffer_t *buf, pmix_topology_t *src,
                                           pmix_pointer_array_t *regtypes)
{
    pmix_ploc_base_active_module_t *active;
    pmix_status_t rc;

    if (!pmix_ploc_globals.initialized) {
        return PMIX_ERR_INIT;
    }

    pmix_output_verbose(2, pmix_ploc_base_framework.framework_output, "ploc:pack_topology called");

    /* process the request */
    PMIX_LIST_FOREACH (active, &pmix_ploc_globals.actives, pmix_ploc_base_active_module_t) {
        if (NULL != active->module->pack_topology) {
            rc = active->module->pack_topology(buf, src, regtypes);
            if (PMIX_SUCCESS == rc) {
                return rc;
            }
            if (PMIX_ERR_TAKE_NEXT_OPTION != rc) {
                /* true error */
                return rc;
            }
        }
    }

    return PMIX_ERR_NOT_SUPPORTED;
}

pmix_status_t pmix_ploc_base_unpack_topology(pmix_buffer_t *buf, pmix_topology_t *dest,
                                             pmix_pointer_array_t *regtypes)
{
    pmix_ploc_base_active_module_t *active;
    pmix_status_t rc;

    if (!pmix_ploc_globals.initialized) {
        return PMIX_ERR_INIT;
    }

    pmix_output_verbose(2, pmix_ploc_base_framework.framework_output,
                        "ploc:unpack_topology called");

    /* process the request */
    PMIX_LIST_FOREACH (active, &pmix_ploc_globals.actives, pmix_ploc_base_active_module_t) {
        if (NULL != active->module->unpack_topology) {
            rc = active->module->unpack_topology(buf, dest, regtypes);
            if (PMIX_SUCCESS == rc) {
                return rc;
            }
            if (PMIX_ERR_TAKE_NEXT_OPTION != rc) {
                /* true error */
                return rc;
            }
        }
    }

    return PMIX_ERR_NOT_SUPPORTED;
}

pmix_status_t pmix_ploc_base_copy_topology(pmix_topology_t *dest, pmix_topology_t *src)
{
    pmix_ploc_base_active_module_t *active;
    pmix_status_t rc;

    if (!pmix_ploc_globals.initialized) {
        return PMIX_ERR_INIT;
    }

    pmix_output_verbose(2, pmix_ploc_base_framework.framework_output, "ploc:copy_topology called");

    /* process the request */
    PMIX_LIST_FOREACH (active, &pmix_ploc_globals.actives, pmix_ploc_base_active_module_t) {
        if (NULL != active->module->copy_topology) {
            rc = active->module->copy_topology(dest, src);
            if (PMIX_SUCCESS == rc) {
                return rc;
            }
            if (PMIX_ERR_TAKE_NEXT_OPTION != rc) {
                /* true error */
                return rc;
            }
        }
    }

    return PMIX_ERR_NOT_SUPPORTED;
}

char *pmix_ploc_base_print_topology(pmix_topology_t *src)
{
    pmix_ploc_base_active_module_t *active;
    char *string;

    if (!pmix_ploc_globals.initialized) {
        return NULL;
    }

    pmix_output_verbose(2, pmix_ploc_base_framework.framework_output, "ploc:print_topology called");

    /* process the request */
    PMIX_LIST_FOREACH (active, &pmix_ploc_globals.actives, pmix_ploc_base_active_module_t) {
        if (NULL != active->module->print_topology) {
            string = active->module->print_topology(src);
            if (NULL != string) {
                return string;
            }
        }
    }

    return NULL;
}

void pmix_ploc_base_destruct_topology(pmix_topology_t *ptr)
{
    pmix_ploc_base_active_module_t *active;
    pmix_status_t rc;

    if (!pmix_ploc_globals.initialized) {
        return;
    }

    pmix_output_verbose(2, pmix_ploc_base_framework.framework_output,
                        "ploc:destruct_topology called");

    /* process the request */
    PMIX_LIST_FOREACH (active, &pmix_ploc_globals.actives, pmix_ploc_base_active_module_t) {
        if (NULL != active->module->destruct_topology) {
            rc = active->module->destruct_topology(ptr);
            if (PMIX_SUCCESS == rc) {
                return;
            }
        }
    }
}

void pmix_ploc_base_release_topology(pmix_topology_t *ptr, size_t sz)
{
    pmix_ploc_base_active_module_t *active;
    pmix_status_t rc;

    if (!pmix_ploc_globals.initialized) {
        return;
    }

    pmix_output_verbose(2, pmix_ploc_base_framework.framework_output,
                        "ploc:release_topology called");

    /* process the request */
    PMIX_LIST_FOREACH (active, &pmix_ploc_globals.actives, pmix_ploc_base_active_module_t) {
        if (NULL != active->module->release_topology) {
            rc = active->module->release_topology(ptr, sz);
            if (PMIX_SUCCESS == rc) {
                return;
            }
        }
    }
}
