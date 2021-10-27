/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2013 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2012-2017 Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2012-2015 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2015-2017 Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2016-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2017      IBM Corporation. All rights reserved.
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
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

#include "src/class/pmix_list.h"
#include "src/class/pmix_object.h"
#include "src/class/pmix_value_array.h"
#include "src/include/frameworks.h"
#include "src/include/pmix_portable_platform.h"
#include "src/mca/base/pmix_mca_base_component_repository.h"
#include "src/mca/pinstalldirs/pinstalldirs.h"
#include "src/util/error.h"
#include "src/util/printf.h"

#include "include/pmix_common.h"
#include "src/util/construct_config.h"

#define BFRSIZE 1024

static void do_group_params(const pmix_mca_base_var_group_t *group,
                            void *list)
{
    const int *variables, *groups;
    const char *group_component;
    const pmix_mca_base_var_t *var;
    char **strings, *message;
    bool requested = true;
    int ret, i, j, count;
    const pmix_mca_base_var_group_t *curr_group = NULL;
    char *component_msg = NULL;

    variables = PMIX_VALUE_ARRAY_GET_BASE(&group->group_vars, const int);
    count = pmix_value_array_get_size((pmix_value_array_t *) &group->group_vars);

    /* the default component name is "base". depending on how the
     * group was registered the group may or not have this set.  */
    group_component = group->group_component ? group->group_component : "base";

    if (0 > asprintf(&component_msg, " %s", group_component)) {
        return;
    }

    for (i = 0; i < count; ++i) {
        ret = pmix_mca_base_var_get(variables[i], &var);
        if (PMIX_SUCCESS != ret) {
            PMIX_ERROR_LOG(ret);
            continue;
        }
        ret = pmix_mca_base_var_dump(variables[i], &strings, PMIX_MCA_BASE_VAR_DUMP_READABLE);
        if (PMIX_SUCCESS != ret) {
            PMIX_ERROR_LOG(ret);
            continue;
        }
        // join the strings to make the description
        message = pmix_argv_join(strings, '\n');
        PMIX_INFO_LIST_ADD(ret, list, component_msg, message, PMIX_STRING);

        groups = PMIX_VALUE_ARRAY_GET_BASE(&group->group_subgroups, const int);
        count = pmix_value_array_get_size((pmix_value_array_t *) &group->group_subgroups);

        for (i = 0; i < count; ++i) {
            ret = pmix_mca_base_var_group_get(groups[i], &group);
            if (PMIX_SUCCESS != ret) {
                PMIX_ERROR_LOG(ret);
                continue;
            }
            do_group_params(group, list);
        }
    }
    free(component_msg);
}

static pmix_status_t do_params(const char *type,
                               void *list)
{
    pmix_status_t rc;
    const pmix_mca_base_var_group_t *group;

    pmix_output(0, "LOOKING FOR %s", type);
    rc = pmix_mca_base_var_group_find("*", type, NULL);
    if (0 > rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    (void) pmix_mca_base_var_group_get(rc, &group);
    do_group_params(group, list);
    return PMIX_SUCCESS;
}

/* Return a pmix_data_array_t of key-value pairs corresponding
 * to what pmix_info presents */
pmix_status_t pmix_util_construct_config(pmix_value_t **val,
                                         pmix_get_logic_t *lg)
{
    void *list;
    pmix_status_t rc;
    char *tmp, *debug, *have_dl, *symbol_visibility;
    char bfr[BFRSIZE];
    pmix_data_array_t *darray;
    pmix_value_t *ival;

    PMIX_INFO_LIST_START(list);

    /* setup the strings that don't require allocations*/
    debug = PMIX_ENABLE_DEBUG ? "yes" : "no";
    have_dl = PMIX_HAVE_PDL_SUPPORT ? "yes" : "no";
    symbol_visibility = PMIX_HAVE_VISIBILITY ? "yes" : "no";

    PMIX_INFO_LIST_ADD(rc, list, "Package", PMIX_PACKAGE_STRING, PMIX_STRING);

    if (NULL != PMIX_GREEK_VERSION) {
        pmix_asprintf(&tmp, "%d.%d.%d%s",
                      PMIX_MAJOR_VERSION,
                      PMIX_MINOR_VERSION,
                      PMIX_RELEASE_VERSION,
                      PMIX_GREEK_VERSION);
    } else {
        pmix_asprintf(&tmp, "%d.%d.%d",
                      PMIX_MAJOR_VERSION,
                      PMIX_MINOR_VERSION,
                      PMIX_RELEASE_VERSION);
    }
    PMIX_INFO_LIST_ADD(rc, list, "PMIx", tmp, PMIX_STRING);
    free(tmp);

    // version
    PMIX_INFO_LIST_ADD(rc, list, "PMIx repo revision", PMIX_REPO_REV, PMIX_STRING);
    PMIX_INFO_LIST_ADD(rc, list, "PMIx release date", PMIX_RELEASE_DATE, PMIX_STRING);

    // paths
    PMIX_INFO_LIST_ADD(rc, list, "Prefix", pmix_pinstall_dirs.prefix, PMIX_STRING);
    PMIX_INFO_LIST_ADD(rc, list, "Exec_prefix", pmix_pinstall_dirs.exec_prefix, PMIX_STRING);
    PMIX_INFO_LIST_ADD(rc, list, "Bindir", pmix_pinstall_dirs.bindir, PMIX_STRING);
    PMIX_INFO_LIST_ADD(rc, list, "Sbindir", pmix_pinstall_dirs.sbindir, PMIX_STRING);
    PMIX_INFO_LIST_ADD(rc, list, "Libdir", pmix_pinstall_dirs.libdir, PMIX_STRING);
    PMIX_INFO_LIST_ADD(rc, list, "Incdir", pmix_pinstall_dirs.includedir, PMIX_STRING);
    PMIX_INFO_LIST_ADD(rc, list, "Mandir", pmix_pinstall_dirs.mandir, PMIX_STRING);
    PMIX_INFO_LIST_ADD(rc, list, "Pkglibdir", pmix_pinstall_dirs.pmixlibdir, PMIX_STRING);
    PMIX_INFO_LIST_ADD(rc, list, "Libexecdir", pmix_pinstall_dirs.libexecdir, PMIX_STRING);
    PMIX_INFO_LIST_ADD(rc, list, "Datarootdir", pmix_pinstall_dirs.datarootdir, PMIX_STRING);
    PMIX_INFO_LIST_ADD(rc, list, "Datadir", pmix_pinstall_dirs.datadir, PMIX_STRING);
    PMIX_INFO_LIST_ADD(rc, list, "Sysconfdir", pmix_pinstall_dirs.sysconfdir, PMIX_STRING);
    PMIX_INFO_LIST_ADD(rc, list, "Sharedstatedir", pmix_pinstall_dirs.sharedstatedir, PMIX_STRING);
    PMIX_INFO_LIST_ADD(rc, list, "Localstatedir", pmix_pinstall_dirs.localstatedir, PMIX_STRING);
    PMIX_INFO_LIST_ADD(rc, list, "Infodir", pmix_pinstall_dirs.infodir, PMIX_STRING);
    PMIX_INFO_LIST_ADD(rc, list, "Pkgdatadir", pmix_pinstall_dirs.pmixdatadir, PMIX_STRING);
    PMIX_INFO_LIST_ADD(rc, list, "Pkglibdir", pmix_pinstall_dirs.pmixlibdir, PMIX_STRING);
    PMIX_INFO_LIST_ADD(rc, list, "Pkgincludedir", pmix_pinstall_dirs.pmixincludedir, PMIX_STRING);

    // config
    PMIX_INFO_LIST_ADD(rc, list, "Configured architecture", PMIX_ARCH_STRING, PMIX_STRING);
    PMIX_INFO_LIST_ADD(rc, list, "Configure host", PMIX_CONFIGURE_HOST, PMIX_STRING);
    PMIX_INFO_LIST_ADD(rc, list, "Configured by", PMIX_CONFIGURE_USER, PMIX_STRING);
    PMIX_INFO_LIST_ADD(rc, list, "Configured on", PMIX_CONFIGURE_DATE, PMIX_STRING);
    PMIX_INFO_LIST_ADD(rc, list, "Configure command line", PMIX_CONFIGURE_CLI, PMIX_STRING);
    PMIX_INFO_LIST_ADD(rc, list, "Built by", PMIX_BUILD_USER, PMIX_STRING);
    PMIX_INFO_LIST_ADD(rc, list, "Built on", PMIX_BUILD_DATE, PMIX_STRING);
    PMIX_INFO_LIST_ADD(rc, list, "Built host", PMIX_BUILD_HOST, PMIX_STRING);
    PMIX_INFO_LIST_ADD(rc, list, "C compiler", PMIX_CC, PMIX_STRING);
    PMIX_INFO_LIST_ADD(rc, list, "C compiler absolute", PMIX_CC_ABSOLUTE, PMIX_STRING);
    PMIX_INFO_LIST_ADD(rc, list, "C compiler family name",
                       PLATFORM_STRINGIFY(PLATFORM_COMPILER_FAMILYNAME), PMIX_STRING);
    PMIX_INFO_LIST_ADD(rc, list, "C compiler version",
                       PLATFORM_STRINGIFY(PLATFORM_COMPILER_VERSION_STR), PMIX_STRING);
    PMIX_INFO_LIST_ADD(rc, list, "C compiler absolute", PMIX_CC_ABSOLUTE, PMIX_STRING);

    PMIX_INFO_LIST_ADD(rc, list, "Build CFLAGS", PMIX_BUILD_CFLAGS, PMIX_STRING);
    PMIX_INFO_LIST_ADD(rc, list, "Build LDFLAGS", PMIX_BUILD_LDFLAGS, PMIX_STRING);
    PMIX_INFO_LIST_ADD(rc, list, "Build LIBS", PMIX_BUILD_LIBS, PMIX_STRING);

    PMIX_INFO_LIST_ADD(rc, list, "Internal debug support", debug, PMIX_STRING);
    PMIX_INFO_LIST_ADD(rc, list, "dl support", have_dl, PMIX_STRING);
    PMIX_INFO_LIST_ADD(rc, list, "Symbol vis. support", symbol_visibility, PMIX_STRING);

    // base MCA params
    rc = do_params("mca", list);
    if (0 > rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    darray = (pmix_data_array_t*)malloc(sizeof(pmix_data_array_t));
    PMIX_INFO_LIST_CONVERT(rc, list, darray);
    PMIX_INFO_LIST_RELEASE(list);

    PMIX_VALUE_CREATE(ival, 1);
    if (NULL == ival) {
        return PMIX_ERR_NOMEM;
    }
    ival->type = PMIX_DATA_ARRAY;
    ival->data.darray = darray;
    *val = ival;
    return PMIX_OPERATION_SUCCEEDED;
}
