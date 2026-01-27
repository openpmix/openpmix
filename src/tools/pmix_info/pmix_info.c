/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2006 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2007 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2007-2012 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2010-2016 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "pmix_config.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif
#ifdef HAVE_NETDB_H
#    include <netdb.h>
#endif
#ifdef HAVE_SYS_PARAM_H
#    include <sys/param.h>
#endif
#include <errno.h>
#include <signal.h>

#include "src/class/pmix_object.h"
#include "src/class/pmix_pointer_array.h"
#include "src/include/pmix_portable_platform_real.h"
#include "src/mca/base/pmix_base.h"
#include "src/mca/pinstalldirs/base/base.h"
#include "src/runtime/pmix_info_support.h"
#include "src/runtime/pmix_rte.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_cmd_line.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_keyval_parse.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_show_help.h"

/*
 * Public variables
 */

pmix_cli_result_t results = PMIX_CLI_RESULT_STATIC_INIT;

const char *pmix_info_type_base = "base";

static struct option poptions[] = {
    PMIX_OPTION_SHORT_DEFINE(PMIX_CLI_HELP, PMIX_ARG_OPTIONAL, 'h'),
    PMIX_OPTION_SHORT_DEFINE(PMIX_CLI_VERSION, PMIX_ARG_NONE, 'V'),
    PMIX_OPTION_SHORT_DEFINE(PMIX_CLI_VERBOSE, PMIX_ARG_NONE, 'v'),
    PMIX_OPTION_DEFINE(PMIX_CLI_PMIXMCA, PMIX_ARG_REQD),

    PMIX_OPTION_SHORT_DEFINE(PMIX_CLI_INFO_ALL, PMIX_ARG_NONE, 'a'),
    PMIX_OPTION_DEFINE(PMIX_CLI_INFO_ARCH, PMIX_ARG_NONE),
    PMIX_OPTION_SHORT_DEFINE(PMIX_CLI_INFO_CONFIG, PMIX_ARG_NONE, 'c'),
    PMIX_OPTION_DEFINE(PMIX_CLI_INFO_HOSTNAME, PMIX_ARG_NONE),
    PMIX_OPTION_DEFINE(PMIX_CLI_INFO_INTERNAL, PMIX_ARG_NONE),

    PMIX_OPTION_DEFINE(PMIX_CLI_INFO_PARAM, PMIX_ARG_REQD),
    PMIX_OPTION_DEFINE(PMIX_CLI_INFO_PARAMS, PMIX_ARG_REQD),
    PMIX_OPTION_DEFINE(PMIX_CLI_INFO_PATH, PMIX_ARG_REQD),
    PMIX_OPTION_DEFINE(PMIX_CLI_INFO_VERSION, PMIX_ARG_OPTIONAL),
    PMIX_OPTION_DEFINE(PMIX_CLI_PRETTY_PRINT, PMIX_ARG_NONE),
    PMIX_OPTION_DEFINE(PMIX_CLI_PARSABLE, PMIX_ARG_NONE),
    PMIX_OPTION_DEFINE(PMIX_CLI_PARSEABLE, PMIX_ARG_NONE),

    PMIX_OPTION_DEFINE(PMIX_CLI_INFO_SHOW_FAILED, PMIX_ARG_NONE),
    PMIX_OPTION_DEFINE(PMIX_CLI_INFO_SELECTED_ONLY, PMIX_ARG_NONE),
    PMIX_OPTION_SHORT_DEFINE(PMIX_CLI_INFO_TYPES, PMIX_ARG_REQD, 't'),
    PMIX_OPTION_DEFINE(PMIX_CLI_INFO_COLOR, PMIX_ARG_REQD),

    PMIX_OPTION_END
};
static char *pshorts = "h::vVact:";

int main(int argc, char *argv[])
{
    int ret = 0;
    bool acted = false;
    bool want_all = false;
    int i;
    char *str, *ptr;
    pmix_pointer_array_t pmix_component_map;
    pmix_pointer_array_t mca_types;
    pmix_info_component_map_t *map;
    pmix_cli_item_t *opt;
    PMIX_HIDE_UNUSED_PARAMS(argc);

    PMIX_CONSTRUCT(&results, pmix_cli_result_t);

    /* protect against problems if someone passes us thru a pipe
     * and then abnormally terminates the pipe early */
    signal(SIGPIPE, SIG_IGN);

    /* init globals */
    pmix_tool_basename = "pmix_info";

    /* initialize the output system */
    if (!pmix_output_init()) {
        return PMIX_ERROR;
    }

    /* initialize install dirs code */
    if (PMIX_SUCCESS
        != (ret = pmix_mca_base_framework_open(&pmix_pinstalldirs_base_framework,
                                               PMIX_MCA_BASE_OPEN_DEFAULT))) {
        fprintf(stderr,
                "pmix_pinstalldirs_base_open() failed -- process will likely abort (%s:%d, "
                "returned %d instead of PMIX_SUCCESS)\n",
                __FILE__, __LINE__, ret);
        return ret;
    }
    if (PMIX_SUCCESS != (ret = pmix_pinstall_dirs_base_init(NULL, 0))) {
        fprintf(stderr,
                "pmix_pinstalldirs_base_init() failed -- process will likely abort (%s:%d, "
                "returned %d instead of PMIX_SUCCESS)\n",
                __FILE__, __LINE__, ret);
        return ret;
    }

    /* initialize the command line, parse it, and return the directives
     * telling us what the user wants output
     */
    if (PMIX_SUCCESS != (ret = pmix_info_init(argc, argv, &results,
                                              poptions, pshorts, "help-pmix_info.txt"))) {
        exit(ret);
    }

    // we do NOT accept arguments other than our own
    if (NULL != results.tail) {
        str = PMIx_Argv_join(results.tail, ' ');
        if (0 != strcmp(str, argv[0])) {
            ptr = pmix_show_help_string("help-pmix_info.txt", "no-args", false,
                                        pmix_tool_basename, str, pmix_tool_basename);
            free(str);
            if (NULL != ptr) {
                printf("%s", ptr);
                free(ptr);
            }
            return -1;
        }
        free(str);
    }

    /* Get MCA parameters, if any */
    if (PMIX_SUCCESS != pmix_mca_base_open(NULL)) {
        pmix_show_help("help-pmix_info.txt", "lib-call-fail", true,
                       "pmix_mca_base_open", __FILE__, __LINE__);
        return PMIX_ERROR;
    }

    /* register params for pmix */
    if (PMIX_SUCCESS != (ret = pmix_register_params())) {
        fprintf(stderr, "pmix_register_params failed with %d\n", ret);
        return PMIX_ERROR;
    }

    /* setup the mca_types array */
    PMIX_CONSTRUCT(&mca_types, pmix_pointer_array_t);
    pmix_pointer_array_init(&mca_types, 256, INT_MAX, 128);
    pmix_info_register_types(&mca_types, false);

    /* init the component map */
    PMIX_CONSTRUCT(&pmix_component_map, pmix_pointer_array_t);
    pmix_pointer_array_init(&pmix_component_map, 64, INT_MAX, 32);

    /* Register PMIx's params */
    if (PMIX_SUCCESS != (ret = pmix_info_register_framework_params(&pmix_component_map))) {
        if (PMIX_ERR_BAD_PARAM == ret) {
            /* output what we got */
            pmix_info_do_params("PMIx", true, pmix_cmd_line_is_taken(&results, PMIX_CLI_INFO_INTERNAL),
                                &mca_types, &pmix_component_map, NULL);
        }
        exit(1);
    }

    /* Execute the desired action(s) */
    want_all = pmix_cmd_line_is_taken(&results, PMIX_CLI_INFO_ALL);

    opt = pmix_cmd_line_get_param(&results, PMIX_CLI_INFO_VERSION);
    if (want_all || NULL != opt) {
        if (NULL == opt) {
            pmix_info_show_pmix_package();
            pmix_info_show_pmix_version();
            pmix_info_show_component_version("pmix", &mca_types, &pmix_component_map, pmix_info_type_all,
                                             pmix_info_component_all, pmix_info_ver_full,
                                             pmix_info_ver_all);
        } else if (NULL == opt->values) {
            pmix_info_show_pmix_package();
            pmix_info_show_pmix_version();
            pmix_info_show_component_version("pmix", &mca_types, &pmix_component_map, pmix_info_type_all,
                                             pmix_info_component_all, pmix_info_ver_full,
                                             pmix_info_ver_all);
        } else {
            if (0 == strcasecmp(opt->values[0], "pmix") ||
                0 == strcasecmp(opt->values[0], "all")) {
                pmix_info_show_pmix_package();
                pmix_info_show_pmix_version();
                pmix_info_show_component_version("pmix", &mca_types, &pmix_component_map, pmix_info_type_all,
                                                 pmix_info_component_all, pmix_info_ver_full,
                                                 pmix_info_ver_all);
            } else {
                // the first arg is either the name of a framework, or a framework:component pair
                char **tmp = PMIx_Argv_split(opt->values[0], ':');
                const char *component = pmix_info_component_all;
                const char *modifier = pmix_info_ver_full;
                if (NULL != tmp[1]) {
                    component = tmp[1];
                }
                if (NULL != opt->values[1]) {
                    modifier = opt->values[1];
                }
                pmix_info_show_component_version("pmix", &mca_types, &pmix_component_map, tmp[0],
                                                 component, modifier,
                                                 pmix_info_ver_all);
                PMIx_Argv_free(tmp);
            }
            acted = true;
        }
    }
    if (want_all || pmix_cmd_line_is_taken(&results, PMIX_CLI_INFO_PATH)) {
        pmix_info_do_pmix_path(want_all, &results);
        acted = true;
    }
    if (want_all || pmix_cmd_line_is_taken(&results, PMIX_CLI_INFO_ARCH)) {
        pmix_info_do_arch();
        acted = true;
    }
    if (!want_all && pmix_cmd_line_is_taken(&results, PMIX_CLI_INFO_HOSTNAME)) {
        // hostname is contain in pmix_config, so don't duplicate it here
        pmix_info_do_hostname();
        acted = true;
    }
    if (want_all || pmix_cmd_line_is_taken(&results, PMIX_CLI_INFO_CONFIG)) {
        pmix_info_do_pmix_config(true);
        acted = true;
    }
    if (want_all || pmix_cmd_line_is_taken(&results, PMIX_CLI_INFO_PARAM) ||
        pmix_cmd_line_is_taken(&results, PMIX_CLI_INFO_PARAMS)) {
        pmix_info_do_params("PMIx", true, pmix_cmd_line_is_taken(&results, PMIX_CLI_INFO_INTERNAL),
                            &mca_types, &pmix_component_map, &results);
        acted = true;
    }
    if (pmix_cmd_line_is_taken(&results, PMIX_CLI_INFO_TYPES)) {
        pmix_info_do_type(&results);
        acted = true;
    }

    /* If no command line args are specified, show default set */

    if (!acted) {
        pmix_info_show_pmix_package();
        pmix_info_show_pmix_version();
        pmix_info_show_path(pmix_info_path_prefix, pmix_pinstall_dirs.prefix);
        pmix_info_do_arch();
        pmix_info_do_pmix_config(false);
        pmix_info_show_component_version("pmix", &mca_types, &pmix_component_map, pmix_info_type_all,
                                         pmix_info_component_all, pmix_info_ver_full,
                                         pmix_info_ver_all);
    }

    /* All done */
    pmix_info_close_components();
    PMIX_DESTRUCT(&results);
    PMIX_DESTRUCT(&mca_types);
    for (i = 0; i < pmix_component_map.size; i++) {
        map = (pmix_info_component_map_t *) pmix_pointer_array_get_item(&pmix_component_map, i);
        if (NULL != map) {
            PMIX_RELEASE(map);
        }
    }
    PMIX_DESTRUCT(&pmix_component_map);

    pmix_info_finalize();

    return 0;
}
