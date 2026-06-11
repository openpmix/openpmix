/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2006 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021-2022 Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "src/include/pmix_config.h"

#include <stdio.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif

#include "src/client/pmix_client_ops.h"
#include "src/include/pmix_globals.h"
#include "src/mca/bfrops/bfrops.h"
#include "src/mca/preg/base/base.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_error.h"

pmix_status_t pmix_preg_base_generate_node_regex(const char *input, char **regex)
{
    pmix_preg_base_active_module_t *active;

    PMIX_LIST_FOREACH (active, &pmix_preg_globals.actives, pmix_preg_base_active_module_t) {
        if (NULL != active->module->generate_node_regex) {
            if (PMIX_SUCCESS == active->module->generate_node_regex(input, regex)) {
                return PMIX_SUCCESS;
            }
        }
    }

    /* no regex could be generated */
    *regex = strdup(input);
    return PMIX_SUCCESS;
}

pmix_status_t pmix_preg_base_generate_ppn(const char *input, char **ppn)
{
    pmix_preg_base_active_module_t *active;

    PMIX_LIST_FOREACH (active, &pmix_preg_globals.actives, pmix_preg_base_active_module_t) {
        if (NULL != active->module->generate_ppn) {
            if (PMIX_SUCCESS == active->module->generate_ppn(input, ppn)) {
                return PMIX_SUCCESS;
            }
        }
    }

    /* no regex could be generated */
    *ppn = strdup(input);
    return PMIX_SUCCESS;
}

pmix_status_t pmix_preg_base_parse_nodes(const char *regexp, char ***names)
{
    pmix_preg_base_active_module_t *active;

    PMIX_LIST_FOREACH (active, &pmix_preg_globals.actives, pmix_preg_base_active_module_t) {
        if (NULL != active->module->parse_nodes) {
            if (PMIX_SUCCESS == active->module->parse_nodes(regexp, names)) {
                return PMIX_SUCCESS;
            }
        }
    }

    /* nobody could parse it, so just process it here */
    *names = PMIx_Argv_split(regexp, ',');
    return PMIX_SUCCESS;
}

pmix_status_t pmix_preg_base_parse_procs(const char *regexp, char ***procs)
{
    pmix_preg_base_active_module_t *active;

    PMIX_LIST_FOREACH (active, &pmix_preg_globals.actives, pmix_preg_base_active_module_t) {
        if (NULL != active->module->parse_procs) {
            if (PMIX_SUCCESS == active->module->parse_procs(regexp, procs)) {
                return PMIX_SUCCESS;
            }
        }
    }

    /* nobody could parse it, so just process it here */
    *procs = PMIx_Argv_split(regexp, ';');
    return PMIX_SUCCESS;
}

pmix_status_t pmix_preg_base_copy(char **dest, size_t *len, const char *input)
{
    pmix_preg_base_active_module_t *active;

    PMIX_LIST_FOREACH (active, &pmix_preg_globals.actives, pmix_preg_base_active_module_t) {
        if (NULL != active->module->copy) {
            if (PMIX_SUCCESS == active->module->copy(dest, len, input)) {
                return PMIX_SUCCESS;
            }
        }
    }

    /* nobody could handle it, so it must just be a string */
    *dest = strdup(input);
    *len = strlen(input) + 1;
    return PMIX_SUCCESS;
}

pmix_status_t pmix_preg_base_pack(pmix_buffer_t *buffer, const char *input)
{
    pmix_preg_base_active_module_t *active;
    pmix_status_t rc;

    PMIX_LIST_FOREACH (active, &pmix_preg_globals.actives, pmix_preg_base_active_module_t) {
        if (NULL != active->module->pack) {
            if (PMIX_SUCCESS == active->module->pack(buffer, input)) {
                return PMIX_SUCCESS;
            }
        }
    }

    /* just pack it as a string */
    PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buffer, input, 1, PMIX_STRING);
    return rc;
}

pmix_status_t pmix_preg_base_unpack(pmix_buffer_t *buffer, char **regex)
{
    pmix_preg_base_active_module_t *active;
    pmix_status_t rc;
    int32_t cnt = 1;

    PMIX_LIST_FOREACH (active, &pmix_preg_globals.actives, pmix_preg_base_active_module_t) {
        if (NULL != active->module->unpack) {
            if (PMIX_SUCCESS == active->module->unpack(buffer, regex)) {
                return PMIX_SUCCESS;
            }
        }
    }

    /* must just be a string */
    PMIX_BFROPS_UNPACK(rc, pmix_globals.mypeer, buffer, regex, &cnt, PMIX_STRING);
    return rc;
}

pmix_status_t pmix_preg_base_release(char *regexp)
{
    pmix_preg_base_active_module_t *active;

    PMIX_LIST_FOREACH (active, &pmix_preg_globals.actives, pmix_preg_base_active_module_t) {
        if (NULL != active->module->release) {
            if (PMIX_SUCCESS == active->module->release(regexp)) {
                return PMIX_SUCCESS;
            }
        }
    }
    return PMIX_ERR_BAD_PARAM;
}

pmix_status_t pmix_preg_base_parse_regex(const pmix_regex_t *regex,
                                          pmix_info_t info[], size_t ninfo,
                                          char **output)
{
    pmix_preg_base_active_module_t *active;

    PMIX_LIST_FOREACH (active, &pmix_preg_globals.actives, pmix_preg_base_active_module_t) {
        if (NULL != active->module->parse_regex) {
            if (PMIX_SUCCESS == active->module->parse_regex(regex, info, ninfo, output)) {
                return PMIX_SUCCESS;
            }
        }
    }
    return PMIX_ERR_NOT_SUPPORTED;
}

pmix_status_t pmix_preg_base_generate_regex(const char *input,
                                             pmix_info_t info[], size_t ninfo,
                                             pmix_regex_t *regex)
{
    pmix_preg_base_active_module_t *active;
    pmix_regex_t candidate = PMIX_REGEX_STATIC_INIT;
    pmix_regex_t best = PMIX_REGEX_STATIC_INIT;

    PMIX_LIST_FOREACH (active, &pmix_preg_globals.actives, pmix_preg_base_active_module_t) {
        if (NULL == active->module->generate_regex) {
            continue;
        }
        if (PMIX_SUCCESS != active->module->generate_regex(input, info, ninfo, &candidate)) {
            continue;
        }
        /* keep this result if it is the first or smaller than the current best */
        if (NULL == best.bytes || candidate.len < best.len) {
            /* release the previous best if there was one */
            if (NULL != best.type) {
                free(best.type);
            }
            if (NULL != best.bytes) {
                free(best.bytes);
            }
            best = candidate;
        } else {
            /* discard this candidate */
            if (NULL != candidate.type) {
                free(candidate.type);
            }
            if (NULL != candidate.bytes) {
                free(candidate.bytes);
            }
        }
        candidate = (pmix_regex_t) PMIX_REGEX_STATIC_INIT;
    }

    if (NULL == best.bytes) {
        return PMIX_ERR_NOT_SUPPORTED;
    }

    *regex = best;
    return PMIX_SUCCESS;
}
