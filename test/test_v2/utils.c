/*
 * Copyright (c) 2015-2018 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015-2018 Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2016      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2020      Triad National Security, LLC.
 *                         All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#include "utils.h"
#include "test_common.h"
#include "pmix_server.h"
#include "cli_stages.h"
#include "test_server.h"

void set_client_argv(test_params *params, char ***argv)
{
    pmix_argv_append_nosize(argv, params->binary);
    pmix_argv_append_nosize(argv, "-n");
    if (NULL == params->np) {
        pmix_argv_append_nosize(argv, "1");
    } else {
        pmix_argv_append_nosize(argv, params->np);
    }
    if( params->verbose ){
        pmix_argv_append_nosize(argv, "-v");
    }
    if (NULL != params->prefix) {
        pmix_argv_append_nosize(argv, "-o");
        pmix_argv_append_nosize(argv, params->prefix);
    }
    if (params->nonblocking) {
        pmix_argv_append_nosize(argv, "-nb");
    }
    if (params->collect) {
        pmix_argv_append_nosize(argv, "-c");
    }
    if (params->collect_bad) {
        pmix_argv_append_nosize(argv, "--collect-corrupt");
    }

}
