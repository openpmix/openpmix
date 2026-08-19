/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2018-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
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

#include "src/include/pmix_config.h"
#include "pmix_common.h"

#include <string.h>

#include "pnet_tcp.h"
#include "src/include/pmix_globals.h"
#include "src/mca/base/pmix_mca_base_var.h"
#include "src/mca/pnet/pnet.h"
#include "src/util/pmix_argv.h"

static pmix_status_t component_register(void);
static pmix_status_t component_open(void);
static pmix_status_t component_close(void);
static pmix_status_t component_query(pmix_mca_base_module_t **module, int *priority);

/*
 * Instantiate the public struct with all of our public information
 * and pointers to our public functions in it
 */
pmix_pnet_tcp_component_t pmix_mca_pnet_tcp_component = {
    .super = {
        PMIX_MCA_BASE_VERSION(pnet),

        /* Component name and version */
        .pmix_mca_component_name = "tcp",
        PMIX_MCA_BASE_MAKE_VERSION(component,
                                   PMIX_MAJOR_VERSION,
                                   PMIX_MINOR_VERSION,
                                   PMIX_RELEASE_VERSION),

        /* Component open and close functions */
        .pmix_mca_register_component_params = component_register,
        .pmix_mca_open_component = component_open,
        .pmix_mca_close_component = component_close,
        .pmix_mca_query_component = component_query,
    },
    .static_ports = NULL,
    .default_request = NULL,
    .include = NULL,
    .exclude = NULL
};
PMIX_MCA_BASE_COMPONENT_INIT(pmix, pnet, tcp)

static pmix_status_t component_register(void)
{
    pmix_mca_base_component_t *component = &pmix_mca_pnet_tcp_component.super;

    pmix_mca_pnet_tcp_component.static_ports = NULL;
    (void) pmix_mca_base_component_var_register(
        component, "static_ports",
        "Static ports for procs, expressed as a semi-colon delimited "
        "list of type:(optional)plane:Comma-delimited list of ranges (e.g., "
        "\"tcp:10.10.10.0/24:32000-32100,33000;udp:40000,40005\")",
        PMIX_MCA_BASE_VAR_TYPE_STRING,
        &pmix_mca_pnet_tcp_component.static_ports);

    (void) pmix_mca_base_component_var_register(
        component, "default_network_allocation",
        "Semi-colon delimited list of (optional)type:(optional)plane:Comma-delimited list of "
        "ranges  "
        "(e.g., \"udp:10.10.10.0/24:3\", or \"5\" if the choice of "
        "type and plane isn't critical)",
        PMIX_MCA_BASE_VAR_TYPE_STRING,
         &pmix_mca_pnet_tcp_component.default_request);

    pmix_mca_pnet_tcp_component.incparms = NULL;
    (void) pmix_mca_base_component_var_register(
        component, "include_envars",
        "Comma-delimited list of envars to harvest (\'*\' and \'?\' supported)",
        PMIX_MCA_BASE_VAR_TYPE_STRING,
        &pmix_mca_pnet_tcp_component.incparms);
    if (NULL != pmix_mca_pnet_tcp_component.incparms) {
        pmix_mca_pnet_tcp_component.include = PMIx_Argv_split(pmix_mca_pnet_tcp_component.incparms, ',');
    }

    pmix_mca_pnet_tcp_component.excparms = NULL;
    (void) pmix_mca_base_component_var_register(
        component, "exclude_envars",
        "Comma-delimited list of envars to exclude (\'*\' and \'?\' supported)",
        PMIX_MCA_BASE_VAR_TYPE_STRING,
        &pmix_mca_pnet_tcp_component.excparms);
    if (NULL != pmix_mca_pnet_tcp_component.excparms) {
        pmix_mca_pnet_tcp_component.exclude = PMIx_Argv_split(pmix_mca_pnet_tcp_component.excparms, ',');
    }

    return PMIX_SUCCESS;
}

static pmix_status_t component_open(void)
{
    int index;
    const pmix_mca_base_var_storage_t *value = NULL;
    bool found = false;

    /* This component is experimental, which is why it is built only on
     * request.  Building it is not the same as asking for it, though: it
     * carries no hardware gate, so left ungated it would join the active
     * list on every server the library comes up in and answer the
     * inventory fan-out there.  So we only allow ourselves to be
     * considered IF the user specifically requested so - the same rule
     * simptest applies, and for the same reason.
     *
     * Note that being selected is still not enough to allocate anything:
     * the port pool comes from the static_ports parameter, and without it
     * tcp_init has nothing to parse. */
    if (0 > (index = pmix_mca_base_var_find("pmix", "pnet", NULL, NULL))) {
        return PMIX_ERR_NOT_AVAILABLE;
    }
    pmix_mca_base_var_get_value(index, &value, NULL, NULL);
    if (NULL != value && NULL != value->stringval && '\0' != value->stringval[0]) {
        /* the value is the framework's component list.  Match an entry of
         * it rather than searching the string: a substring test would take
         * "tcp" out of the name of any component that merely contains it,
         * and would read an exclusion ("^tcp") as a request */
        char **tmp = PMIx_Argv_split(value->stringval, ',');
        int n;

        for (n = 0; NULL != tmp && NULL != tmp[n]; n++) {
            char *nm = ('^' == tmp[n][0]) ? &tmp[n][1] : tmp[n];
            if (0 == strcasecmp(nm, "tcp")) {
                found = ('^' != tmp[n][0]);
                break;
            }
        }
        PMIx_Argv_free(tmp);
    }
    if (found) {
        return PMIX_SUCCESS;
    }

    /* PMIX_ERR_NOT_AVAILABLE is the MCA's "silently ignore me" cue.  Any
     * other status is reported as a component that failed to open, which
     * is not what declining an invitation looks like. */
    return PMIX_ERR_NOT_AVAILABLE;
}

static pmix_status_t component_query(pmix_mca_base_module_t **module, int *priority)
{
    *priority = 5;
    *module = (pmix_mca_base_module_t *) &pmix_tcp_module;
    return PMIX_SUCCESS;
}

static pmix_status_t component_close(void)
{
    /* component_register split the envar patterns into these two argv
     * arrays, and nothing else owns them. The MCA base calls us both when
     * the framework closes and when our open fails, so this is the one
     * place that runs either way - without it the arrays are rebuilt and
     * abandoned on every PMIx init/finalize cycle. */
    if (NULL != pmix_mca_pnet_tcp_component.include) {
        PMIx_Argv_free(pmix_mca_pnet_tcp_component.include);
        pmix_mca_pnet_tcp_component.include = NULL;
    }
    if (NULL != pmix_mca_pnet_tcp_component.exclude) {
        PMIx_Argv_free(pmix_mca_pnet_tcp_component.exclude);
        pmix_mca_pnet_tcp_component.exclude = NULL;
    }

    return PMIX_SUCCESS;
}
