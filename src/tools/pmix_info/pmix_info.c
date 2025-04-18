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
 * Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
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
#include "src/mca/base/pmix_base.h"
#include "src/mca/pinstalldirs/base/base.h"
#include "src/runtime/pmix_rte.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_cmd_line.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_keyval_parse.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_show_help.h"

#include "pmix_info.h"
#include "support.h"

/*
 * Public variables
 */

pmix_cli_result_t *pmix_info_cmd_line = NULL;
pmix_cli_result_t results = PMIX_CLI_RESULT_STATIC_INIT;
pmix_pointer_array_t pmix_component_map = PMIX_POINTER_ARRAY_STATIC_INIT;
pmix_pointer_array_t mca_types = PMIX_POINTER_ARRAY_STATIC_INIT;

const char *pmix_info_type_base = "base";

static struct option poptions[] = {
    PMIX_OPTION_SHORT_DEFINE(PMIX_CLI_HELP, PMIX_ARG_OPTIONAL, 'h'),
    PMIX_OPTION_SHORT_DEFINE(PMIX_CLI_VERSION, PMIX_ARG_NONE, 'V'),
    PMIX_OPTION_SHORT_DEFINE(PMIX_CLI_VERBOSE, PMIX_ARG_NONE, 'v'),

    PMIX_OPTION_SHORT_DEFINE(PMIX_CLI_ALL, PMIX_ARG_NONE, 'a'),
    PMIX_OPTION_DEFINE(PMIX_CLI_ARCH, PMIX_ARG_NONE),
    PMIX_OPTION_SHORT_DEFINE(PMIX_CLI_CONFIG, PMIX_ARG_NONE, 'c'),
    PMIX_OPTION_DEFINE(PMIX_CLI_HOSTNAME, PMIX_ARG_NONE),
    PMIX_OPTION_DEFINE(PMIX_CLI_INTERNAL, PMIX_ARG_NONE),

    PMIX_OPTION_DEFINE(PMIX_CLI_PARAM, PMIX_ARG_REQD),
    PMIX_OPTION_DEFINE(PMIX_CLI_PATH, PMIX_ARG_REQD),
    PMIX_OPTION_DEFINE(PMIX_CLI_SHOW_VERSION, PMIX_ARG_REQD),
    PMIX_OPTION_DEFINE(PMIX_CLI_PRETTY_PRINT, PMIX_ARG_NONE),
    PMIX_OPTION_DEFINE(PMIX_CLI_PARSABLE, PMIX_ARG_NONE),
    PMIX_OPTION_DEFINE(PMIX_CLI_PARSEABLE, PMIX_ARG_NONE),

    PMIX_OPTION_DEFINE(PMIX_CLI_SHOW_FAILED, PMIX_ARG_NONE),
    PMIX_OPTION_DEFINE(PMIX_CLI_SELECTED_ONLY, PMIX_ARG_NONE),

    PMIX_OPTION_END
};
static char *pshorts = "h::vVac";

int main(int argc, char *argv[])
{
    int ret = 0;
    bool acted = false;
    bool want_all = false;
    int i;
    pmix_info_component_map_t *map;
    PMIX_HIDE_UNUSED_PARAMS(argc);

    pmix_info_cmd_line = &results;
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

    /* initialize the help system */
    pmix_show_help_init();

    /* keyval lex-based parser */
    if (PMIX_SUCCESS != (ret = pmix_util_keyval_parse_init())) {
        fprintf(stderr, "pmix_util_keyval_parse_init failed with %d\n", ret);
        return PMIX_ERROR;
    }

    /* Setup the parameter system */
    if (PMIX_SUCCESS != (ret = pmix_mca_base_var_init())) {
        fprintf(stderr, "pmix_mca_base_var_init failed with %d\n", ret);
        return PMIX_ERROR;
    }

    /* register params for pmix */
    if (PMIX_SUCCESS != (ret = pmix_register_params())) {
        fprintf(stderr, "pmix_register_params failed with %d\n", ret);
        return PMIX_ERROR;
    }

    if (PMIX_SUCCESS != pmix_mca_base_open(NULL)) {
        pmix_show_help("help-pmix-info.txt", "lib-call-fail", true, "mca_base_open", __FILE__,
                       __LINE__);
        PMIX_RELEASE(pmix_info_cmd_line);
        exit(1);
    }

    /* Do the parsing */
    ret = pmix_cmd_line_parse(argv, pshorts, poptions,
                              NULL, pmix_info_cmd_line, "help-pmix-info.txt");
    if (PMIX_SUCCESS != ret) {
        if (PMIX_ERR_SILENT != ret && PMIX_OPERATION_SUCCEEDED != ret) {
            fprintf(stderr, "%s: command line error (%s)\n", argv[0], PMIx_Error_string(ret));
        }
        if (PMIX_OPERATION_SUCCEEDED == ret) {
            ret = PMIX_SUCCESS;
        }
        exit(ret);
    }

    /* set the flags */
    if (pmix_cmd_line_is_taken(pmix_info_cmd_line, PMIX_CLI_PRETTY_PRINT)) {
        pmix_info_pretty = true;
    } else if (pmix_cmd_line_is_taken(pmix_info_cmd_line, PMIX_CLI_PARSABLE) ||
               pmix_cmd_line_is_taken(pmix_info_cmd_line, PMIX_CLI_PARSEABLE)) {
        pmix_info_pretty = false;
    }

    if (pmix_cmd_line_is_taken(pmix_info_cmd_line, PMIX_CLI_SELECTED_ONLY)) {
        /* register only selected components */
        pmix_info_register_flags = PMIX_MCA_BASE_REGISTER_DEFAULT;
    }

    if (pmix_cmd_line_is_taken(pmix_info_cmd_line, PMIX_CLI_SHOW_FAILED)) {
        pmix_mca_base_component_track_load_errors = true;
    }


    /* setup the mca_types array */
    PMIX_CONSTRUCT(&mca_types, pmix_pointer_array_t);
    pmix_pointer_array_init(&mca_types, 256, INT_MAX, 128);
    pmix_info_register_types();

    /* init the component map */
    PMIX_CONSTRUCT(&pmix_component_map, pmix_pointer_array_t);
    pmix_pointer_array_init(&pmix_component_map, 64, INT_MAX, 32);

    /* Register PMIx's params */
    if (PMIX_SUCCESS != (ret = pmix_info_register_framework_params())) {
        if (PMIX_ERR_BAD_PARAM == ret) {
            /* output what we got */
            pmix_info_do_params(true);
        }
        exit(1);
    }

    /* Execute the desired action(s) */
    want_all = pmix_cmd_line_is_taken(pmix_info_cmd_line, PMIX_CLI_ALL);
    if (want_all) {
        pmix_info_out("Package", "package", PMIX_PACKAGE_STRING);
        pmix_info_show_pmix_version(pmix_info_ver_full);
    }
    if (want_all || pmix_cmd_line_is_taken(pmix_info_cmd_line, PMIX_CLI_PATH)) {
        pmix_info_do_path(want_all);
        acted = true;
    }
    if (want_all || pmix_cmd_line_is_taken(pmix_info_cmd_line, PMIX_CLI_ARCH)) {
        pmix_info_do_arch();
        acted = true;
    }
    if (want_all || pmix_cmd_line_is_taken(pmix_info_cmd_line, PMIX_CLI_HOSTNAME)) {
        pmix_info_do_hostname();
        acted = true;
    }
    if (want_all || pmix_cmd_line_is_taken(pmix_info_cmd_line, PMIX_CLI_CONFIG)) {
        pmix_info_do_config(true);
        acted = true;
    }
    if (want_all || pmix_cmd_line_is_taken(pmix_info_cmd_line, PMIX_CLI_PARAM)
        || pmix_cmd_line_is_taken(pmix_info_cmd_line, PMIX_CLI_PARAMS)) {
        pmix_info_do_params(want_all);
        acted = true;
    }
    if (pmix_cmd_line_is_taken(pmix_info_cmd_line, PMIX_CLI_TYPE)) {
        pmix_info_do_type();
        acted = true;
    }

    /* If no command line args are specified, show default set */

    if (!acted) {
        pmix_info_out("Package", "package", PMIX_PACKAGE_STRING);
        pmix_info_show_pmix_version(pmix_info_ver_full);
        pmix_info_show_path(pmix_info_path_prefix, pmix_pinstall_dirs.prefix);
        pmix_info_do_arch();
        pmix_info_do_hostname();
        pmix_info_do_config(false);
        pmix_info_show_component_version(pmix_info_type_all, pmix_info_component_all,
                                         pmix_info_ver_full, pmix_info_ver_all);
    }

    /* All done */
    pmix_info_close_components();
    PMIX_DESTRUCT(pmix_info_cmd_line);
    PMIX_DESTRUCT(&mca_types);
    for (i = 0; i < pmix_component_map.size; i++) {
        if (NULL
            != (map = (pmix_info_component_map_t *) pmix_pointer_array_get_item(&pmix_component_map,
                                                                                i))) {
            PMIX_RELEASE(map);
        }
    }
    PMIX_DESTRUCT(&pmix_component_map);

    pmix_info_finalize();

    return 0;
}
