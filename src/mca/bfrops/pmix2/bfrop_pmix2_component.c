/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2008 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennbfropsee and The University
 *                         of Tennbfropsee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2015      Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2016      Intel, Inc. All rights reserved
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * These symbols are in a file by themselves to provide nice linker
 * semantics.  Since linkers generally pull in symbols by object
 * files, keeping these symbols as the only symbols in this file
 * prevents utility programs such as "ompi_info" from having to import
 * entire components just to query their version and parameters.
 */

#include <src/include/pmix_config.h>
#include <pmix_common.h>

#include "src/util/error.h"
#include "src/server/pmix_server_ops.h"
#include "src/mca/bfrops/base/base.h"
#include "src/mca/bfrops/pmix2/bfrop_pmix2.h"

extern pmix_bfrops_module_t pmix_bfrops_pmix2_module;

static pmix_status_t component_open(void);
static pmix_status_t component_query(pmix_mca_base_module_t **module, int *priority);
static pmix_status_t component_close(void);
static pmix_bfrops_module_t* assign_module(const char *version);
static pmix_status_t define_rendezvous(pmix_list_t *listeners,
                                       char *tmpdir);
/*
 * Instantiate the public struct with all of our public information
 * and pointers to our public functions in it
 */
pmix_bfrops_base_component_t mca_bfrops_pmix2_component = {
    .base = {
        PMIX_BFROPS_BASE_VERSION_1_0_0,

        /* Component name and version */
        .pmix_mca_component_name = "pmix2",
        PMIX_MCA_BASE_MAKE_VERSION(component, PMIX_MAJOR_VERSION, PMIX_MINOR_VERSION,
                                   PMIX_RELEASE_VERSION),

        /* Component open and close functions */
        .pmix_mca_open_component = component_open,
        .pmix_mca_close_component = component_close,
        .pmix_mca_query_component = component_query,
    },
    .priority = 10,
    .assign_module = assign_module,
    .rendezvous = define_rendezvous
};


pmix_status_t component_open(void)
{
    /* setup the types array */
    PMIX_CONSTRUCT(&mca_bfrops_pmix2_component.types, pmix_pointer_array_t);

    return PMIX_SUCCESS;
}


pmix_status_t component_query(pmix_mca_base_module_t **module, int *priority)
{

    *priority = mca_bfrops_pmix2_component.priority;
    *module = (pmix_mca_base_module_t *)&pmix_bfrops_pmix2_module;
    return PMIX_SUCCESS;
}


pmix_status_t component_close(void)
{
    PMIX_DESTRUCT(&mca_bfrops_pmix2_component.types);
    return PMIX_SUCCESS;
}

static pmix_bfrops_module_t* assign_module(const char *version)
{
    if (NULL != version && NULL == strstr(version, "pmix2")) {
        return NULL;
    }
    return &pmix_bfrops_pmix2_module;
}

static pmix_status_t define_rendezvous(pmix_list_t *listeners,
                                       char *tmpdir)
{
    pid_t pid;
    char *pmix_pid;
    pmix_listener_t *listener;

    /* if we are not a server, then we shouldn't be doing this */
    if (PMIX_PROC_SERVER != pmix_globals.proc_type) {
        return PMIX_ERR_NOT_SUPPORTED;
    }

    /* setup the v2 series rendezvous point
     * we use the pid to reduce collisions */
    pid = getpid();
    if (0 > asprintf(&pmix_pid, "%s/pmix2-%d", tmpdir, pid)) {
        return PMIX_ERR_NOMEM;
    }

    listener = PMIX_NEW(pmix_listener_t);
    if ((strlen(pmix_pid) + 1) > sizeof(listener->address.sun_path)-1) {
        PMIX_ERROR_LOG(PMIX_ERR_INVALID_LENGTH);
        free(pmix_pid);
        PMIX_RELEASE(listener);
        return PMIX_ERR_INVALID_LENGTH;
    }

    snprintf(listener->address.sun_path, sizeof(listener->address.sun_path)-1, "%s", pmix_pid);
    if (0 > asprintf(&listener->uri, "%s:%lu:%s", pmix_globals.myid.nspace,
                    (unsigned long)pmix_globals.myid.rank, listener->address.sun_path)) {
        free(pmix_pid);
        PMIX_RELEASE(listener);
        return PMIX_ERR_NOMEM;
    }
    listener->varname = strdup("PMIX_SERVER_URI2");
    listener->protocol = PMIX_PROTOCOL_V2;
    pmix_list_append(&pmix_server_globals.listeners, &listener->super);
    free(pmix_pid);


    pmix_output_verbose(2, pmix_bfrops_base_framework.framework_output,
                        "pmix2:server constructed uri %s", listener->uri);
    return PMIX_SUCCESS;
}
