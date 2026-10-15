/*
 * Copyright (c) 2004-2008 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2016-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2020      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "src/include/pmix_config.h"
#include "pmix_common.h"

#include <string.h>

#include "src/mca/base/pmix_base.h"
#include "src/mca/mca.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_show_help.h"

#include "src/mca/ptl/base/base.h"

/* The functions of each role. A client neither listens, nor forks
 * children, nor connects without blocking: callers test for those */
static const pmix_ptl_api_t client_api = {
    .name = "client",
    .connect_to_peer = pmix_ptl_base_client_connect_to_peer
};

static const pmix_ptl_api_t tool_api = {
    .name = "tool",
    .connect_to_peer = pmix_ptl_base_connect_to_peer,
    .connect_to_peer_nb = pmix_ptl_base_connect_to_peer_nb,
    .setup_fork = pmix_ptl_base_setup_fork,
    .setup_listener = pmix_ptl_base_tool_setup_listener
};

static const pmix_ptl_api_t server_api = {
    .name = "server",
    .connect_to_peer = pmix_ptl_base_connect_to_peer,
    .connect_to_peer_nb = pmix_ptl_base_connect_to_peer_nb,
    .setup_fork = pmix_ptl_base_setup_fork,
    .setup_listener = pmix_ptl_base_setup_listener
};

/* Take up every transport available, and the functions of this
 * process's role. A process that is a client and not a tool is a client;
 * any tool - a launcher, or a tool on a client's rank - is a tool; a
 * server that is not a tool is a server */
int pmix_ptl_base_select(void)
{
    pmix_peer_t *me = pmix_globals.mypeer;
    pmix_mca_base_component_list_item_t *cli;
    pmix_ptl_base_component_t *component;
    pmix_ptl_base_active_t *active, *newactive;
    pmix_mca_base_module_t *mod;
    int rc, pri;
    bool inserted;

    if (pmix_ptl_base.selected) {
        /* ensure we don't do this twice */
        return PMIX_SUCCESS;
    }
    pmix_ptl_base.selected = true;

    /* every transport available is used, highest priority first */
    PMIX_LIST_FOREACH (cli, &pmix_ptl_base_framework.framework_components,
                       pmix_mca_base_component_list_item_t) {
        component = (pmix_ptl_base_component_t *) cli->cli_component;
        pmix_output_verbose(5, pmix_ptl_base_framework.framework_output,
                            "mca:ptl:select: checking available component %s",
                            component->base.pmix_mca_component_name);
        rc = component->base.pmix_mca_query_component(&mod, &pri);
        if (PMIX_SUCCESS != rc || NULL == mod) {
            continue;
        }
        newactive = PMIX_NEW(pmix_ptl_base_active_t);
        if (NULL == newactive) {
            return PMIX_ERR_NOMEM;
        }
        newactive->priority = pri;
        newactive->component = component;
        newactive->module = (pmix_ptl_module_t *) mod;
        inserted = false;
        PMIX_LIST_FOREACH (active, &pmix_ptl_base.actives, pmix_ptl_base_active_t) {
            if (active->priority < pri) {
                pmix_list_insert_pos(&pmix_ptl_base.actives, &active->super,
                                     &newactive->super);
                inserted = true;
                break;
            }
        }
        if (!inserted) {
            pmix_list_append(&pmix_ptl_base.actives, &newactive->super);
        }
    }
    if (0 == pmix_list_get_size(&pmix_ptl_base.actives)) {
        pmix_show_help("help-pmix-runtime.txt", "no-plugins", true, "PTL");
        return PMIX_ERR_SILENT;
    }

    if (PMIX_PEER_IS_CLIENT(me) && !PMIX_PEER_IS_TOOL(me)) {
        pmix_ptl = client_api;
    } else if (PMIX_PEER_IS_TOOL(me)) {
        pmix_ptl = tool_api;
    } else if (PMIX_PEER_IS_SERVER(me)) {
        pmix_ptl = server_api;
    } else {
        pmix_show_help("help-pmix-runtime.txt", "no-plugins", true, "PTL");
        return PMIX_ERR_SILENT;
    }

    pmix_output_verbose(5, pmix_ptl_base_framework.framework_output,
                        "mca:ptl:select: acting as %s", pmix_ptl.name);
    return PMIX_SUCCESS;
}
