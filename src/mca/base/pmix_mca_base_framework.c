/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2012-2015 Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2015      Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2016-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "src/include/pmix_config.h"

#include "pmix_common.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_printf.h"

#include "pmix_mca_base_framework.h"
#include "pmix_mca_base_var.h"
#include "src/mca/base/pmix_base.h"

bool pmix_mca_base_framework_is_registered(struct pmix_mca_base_framework_t *framework)
{
    return !!(framework->framework_flags & PMIX_MCA_BASE_FRAMEWORK_FLAG_REGISTERED);
}

bool pmix_mca_base_framework_is_open(struct pmix_mca_base_framework_t *framework)
{
    return !!(framework->framework_flags & PMIX_MCA_BASE_FRAMEWORK_FLAG_OPEN);
}

static void framework_open_output(struct pmix_mca_base_framework_t *framework)
{
    if (0 < framework->framework_verbose) {
        if (-1 == framework->framework_output) {
            framework->framework_output = pmix_output_open(NULL);
        }
        pmix_output_set_verbosity(framework->framework_output, framework->framework_verbose);
    } else if (-1 != framework->framework_output) {
        pmix_output_close(framework->framework_output);
        framework->framework_output = -1;
    }
}

static void framework_close_output(struct pmix_mca_base_framework_t *framework)
{
    if (-1 != framework->framework_output) {
        pmix_output_close(framework->framework_output);
        framework->framework_output = -1;
    }
}

int pmix_mca_base_framework_register(struct pmix_mca_base_framework_t *framework,
                                     pmix_mca_base_register_flag_t flags)
{
    char *desc;
    int ret;

    assert(NULL != framework);

    framework->framework_refcnt++;

    if (pmix_mca_base_framework_is_registered(framework)) {
        return PMIX_SUCCESS;
    }

    PMIX_CONSTRUCT(&framework->framework_components, pmix_list_t);
    PMIX_CONSTRUCT(&framework->framework_failed_components, pmix_list_t);

    if (framework->framework_flags & PMIX_MCA_BASE_FRAMEWORK_FLAG_NO_DSO) {
        flags |= PMIX_MCA_BASE_REGISTER_STATIC_ONLY;
    }

    if (!(PMIX_MCA_BASE_FRAMEWORK_FLAG_NOREGISTER & framework->framework_flags)) {
        /* NOTE: every failure below must undo the refcnt bump above, or
         * the framework can never reach zero in
         * pmix_mca_base_framework_close() and its components are never
         * closed. pmix_mca_base_framework_open() does the same on its
         * own failure path. */

        /* register this framework with the MCA variable system */
        ret = pmix_mca_base_var_group_register(framework->framework_project,
                                               framework->framework_name, NULL,
                                               framework->framework_description);
        if (0 > ret) {
            goto error;
        }

        ret = pmix_asprintf(&desc,
                       "Default selection set of components for the %s framework (<none>"
                       " means use all components that can be found)",
                       framework->framework_name);
        if (0 > ret) {
            ret = PMIX_ERR_OUT_OF_RESOURCE;
            goto error;
        }

        ret = pmix_mca_base_var_register(framework->framework_project, framework->framework_name,
                                         NULL, NULL, desc, PMIX_MCA_BASE_VAR_TYPE_STRING,
                                         &framework->framework_selection);
        free(desc);
        if (0 > ret) {
            goto error;
        }

        /* register a verbosity variable for this framework */
        ret = pmix_asprintf(&desc, "Verbosity level for the %s framework (default: 0)",
                       framework->framework_name);
        if (0 > ret) {
            ret = PMIX_ERR_OUT_OF_RESOURCE;
            goto error;
        }

        framework->framework_verbose = PMIX_MCA_BASE_VERBOSE_ERROR;
        ret = pmix_mca_base_framework_var_register(framework, "verbose", desc,
                                                   PMIX_MCA_BASE_VAR_TYPE_INT,
                                                   &framework->framework_verbose);
        free(desc);
        if (0 > ret) {
            goto error;
        }

        /* check the initial verbosity and open the output if necessary. we
           will recheck this on open */
        framework_open_output(framework);

        /* register framework variables */
        if (NULL != framework->framework_register) {
            ret = framework->framework_register(flags);
            if (PMIX_SUCCESS != ret) {
                goto error;
            }
        }

        /* register components variables */
        ret = pmix_mca_base_framework_components_register(framework, flags);
        if (PMIX_SUCCESS != ret) {
            goto error;
        }
    }

    framework->framework_flags |= PMIX_MCA_BASE_FRAMEWORK_FLAG_REGISTERED;

    /* framework did not provide a register function */
    return PMIX_SUCCESS;

error:
    /* Undo everything this call built, not just the reference count.
     *
     * Nothing else can do it for us: with neither REGISTERED nor OPEN
     * set, pmix_mca_base_framework_close() returns immediately, so
     * whatever is left on framework_components here is orphaned for
     * good. And that list can be non-empty on a plain user error rather
     * than only on an allocation failure -- component_find() appends
     * every component that matched the selection and only afterwards
     * does component_find_check() reject the run over a requested name
     * that matched nothing (which it does when the user asked for
     * mca_base_abort_on_load_error).
     *
     * This mirrors the not-open branch of
     * pmix_mca_base_framework_close(), which is what would have run had
     * the registration got far enough to be closeable. */
    if (0 == --framework->framework_refcnt) {
        pmix_list_item_t *item;
        int group_id;

        group_id = pmix_mca_base_var_group_find(framework->framework_project,
                                                framework->framework_name, NULL);
        if (0 <= group_id) {
            (void) pmix_mca_base_var_group_deregister(group_id);
        }

        while (NULL != (item = pmix_list_remove_first(&framework->framework_components))) {
            pmix_mca_base_component_list_item_t *cli;
            cli = (pmix_mca_base_component_list_item_t *) item;
            pmix_mca_base_component_unload(cli->cli_component, framework->framework_output);
            PMIX_RELEASE(item);
        }

        PMIX_DESTRUCT(&framework->framework_components);
        PMIX_LIST_DESTRUCT(&framework->framework_failed_components);
        framework_close_output(framework);
    }
    return ret;
}

int pmix_mca_base_framework_open(struct pmix_mca_base_framework_t *framework,
                                 pmix_mca_base_open_flag_t flags)
{
    int ret;

    assert(NULL != framework);

    /* register this framework before opening it */
    ret = pmix_mca_base_framework_register(framework, PMIX_MCA_BASE_REGISTER_DEFAULT);
    if (PMIX_SUCCESS != ret) {
        return ret;
    }

    /* check if this framework is already open */
    if (pmix_mca_base_framework_is_open(framework)) {
        return PMIX_SUCCESS;
    }

    if (PMIX_MCA_BASE_FRAMEWORK_FLAG_NOREGISTER & framework->framework_flags) {
        flags |= PMIX_MCA_BASE_OPEN_FIND_COMPONENTS;

        if (PMIX_MCA_BASE_FRAMEWORK_FLAG_NO_DSO & framework->framework_flags) {
            flags |= PMIX_MCA_BASE_OPEN_STATIC_ONLY;
        }
    }

    /* check the verbosity level and open (or close) the output */
    framework_open_output(framework);

    if (NULL != framework->framework_open) {
        ret = framework->framework_open(flags);
    } else {
        ret = pmix_mca_base_framework_components_open(framework, flags);
    }

    if (PMIX_SUCCESS != ret) {
        /* Undo the registration this call performed at the top. A bare
         * refcnt-- is not enough: when this call was the one that
         * registered the framework, dropping the count back to zero
         * while REGISTERED stays set leaves a framework that reports
         * itself registered with nobody holding it -- and the next
         * pmix_mca_base_framework_close() then decrements 0 to -1,
         * finds that non-zero, returns success without tearing
         * anything down, and wedges the framework permanently. (The
         * assert() at the top of close() catches it, but only in a
         * build with asserts enabled, which is not the one users get.)
         *
         * Closing here does exactly the right thing in both cases: if
         * someone else already held a reference, close() just gives
         * ours back; if we were the only holder, it deregisters the
         * variable group and tears the component lists down. It does
         * not call the framework's own close function, because OPEN was
         * never set -- which is correct, since its open failed. */
        (void) pmix_mca_base_framework_close(framework);
    } else {
        framework->framework_flags |= PMIX_MCA_BASE_FRAMEWORK_FLAG_OPEN;
    }

    return ret;
}

int pmix_mca_base_framework_close(struct pmix_mca_base_framework_t *framework)
{
    bool is_open = pmix_mca_base_framework_is_open(framework);
    bool is_registered = pmix_mca_base_framework_is_registered(framework);
    int ret, group_id;

    assert(NULL != framework);

    if (!(is_open || is_registered)) {
        return PMIX_SUCCESS;
    }

    assert(framework->framework_refcnt);
    if (--framework->framework_refcnt) {
        return PMIX_SUCCESS;
    }

    /* find and deregister all component groups and variables */
    group_id = pmix_mca_base_var_group_find(framework->framework_project, framework->framework_name,
                                            NULL);
    if (0 <= group_id) {
        (void) pmix_mca_base_var_group_deregister(group_id);
    }

    /* close the framework and all of its components */
    if (is_open) {
        if (NULL != framework->framework_close) {
            ret = framework->framework_close();
        } else {
            ret = pmix_mca_base_framework_components_close(framework, NULL);
        }

        if (PMIX_SUCCESS != ret) {
            return ret;
        }
    } else {
        pmix_list_item_t *item;
        while (NULL != (item = pmix_list_remove_first(&framework->framework_components))) {
            pmix_mca_base_component_list_item_t *cli;
            cli = (pmix_mca_base_component_list_item_t *) item;
            pmix_mca_base_component_unload(cli->cli_component, framework->framework_output);
            PMIX_RELEASE(item);
        }
        ret = PMIX_SUCCESS;
    }

    framework->framework_flags &= ~(PMIX_MCA_BASE_FRAMEWORK_FLAG_REGISTERED
                                    | PMIX_MCA_BASE_FRAMEWORK_FLAG_OPEN);

    PMIX_DESTRUCT(&framework->framework_components);
    PMIX_LIST_DESTRUCT(&framework->framework_failed_components);

    framework_close_output(framework);

    return ret;
}
