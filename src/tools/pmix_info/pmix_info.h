/*
 * Copyright (c) 2004-2009 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2007-2010 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2015      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2017-2018 Intel, Inc. All rights reserved.
 * Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PMIX_INFO_TOOL_H
#define PMIX_INFO_TOOL_H
#include "pmix_config.h"

#include "src/class/pmix_list.h"
#include "src/class/pmix_pointer_array.h"
#include "src/mca/mca.h"
#include "src/util/pmix_cmd_line.h"

BEGIN_C_DECLS

/*
 * Globals
 */

extern bool pmix_info_pretty;
extern pmix_cli_result_t *pmix_info_cmd_line;

extern const char *pmix_info_type_base;

extern pmix_pointer_array_t mca_types;

/*
 * Parameter/configuration-related functions
 */

extern const char *pmix_info_component_all;
extern const char *pmix_info_param_all;

extern const char *pmix_info_path_bindir;
extern const char *pmix_info_path_libdir;
extern const char *pmix_info_path_incdir;
extern const char *pmix_info_path_mandir;
extern const char *pmix_info_path_pkglibdir;
extern const char *pmix_info_path_sysconfdir;
extern const char *pmix_info_path_exec_prefix;
extern const char *pmix_info_path_sbindir;
extern const char *pmix_info_path_libexecdir;
extern const char *pmix_info_path_datarootdir;
extern const char *pmix_info_path_datadir;
extern const char *pmix_info_path_sharedstatedir;
extern const char *pmix_info_path_localstatedir;
extern const char *pmix_info_path_infodir;
extern const char *pmix_info_path_pkgdatadir;
extern const char *pmix_info_path_pkgincludedir;
extern pmix_mca_base_register_flag_t pmix_info_register_flags;

void pmix_info_do_config(bool want_all);

extern pmix_pointer_array_t pmix_component_map;

#define PMIX_CLI_ALL            "all"
#define PMIX_CLI_ARCH           "arch"
#define PMIX_CLI_CONFIG         "config"
#define PMIX_CLI_HOSTNAME       "hostname"
#define PMIX_CLI_INTERNAL       "internal"
#define PMIX_CLI_PARAM          "param"
#define PMIX_CLI_PARAMS 		"params"
#define PMIX_CLI_PATH           "path"
#define PMIX_CLI_SHOW_VERSION   "show-version"
#define PMIX_CLI_PRETTY_PRINT   "pretty-print"
#define PMIX_CLI_PARSABLE       "parsable"
#define PMIX_CLI_PARSEABLE      "parseable"
#define PMIX_CLI_SHOW_FAILED    "show-failed"
#define PMIX_CLI_SELECTED_ONLY  "selected-only"
#define PMIX_CLI_TYPE 			"type"

END_C_DECLS

#endif /* PMIX_INFO_H */
