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
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
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
#include "src/util/pmix_show_help.h"

#include "src/mca/plog/base/base.h"

/* Does this module service the channel the user named in
 * plog_base_order? The parameter is documented - in its own MCA
 * description and in the reqd-not-found help text - as a list of
 * *channels*, but a module's name and the channel tokens it claims are
 * not the same thing: the "stdfd" module services "stdout" and
 * "stderr". Matching only the name made every documented channel name
 * except the ones that happen to double as module names resolve to
 * nothing, so both are accepted here. A NULL "channels" is deliberately
 * not treated as a wildcard: a catch-all module would otherwise claim
 * whichever channel the user asked for first. */
static bool channel_matches(const char *request, size_t len,
                            pmix_plog_base_active_module_t *mod)
{
    int i;

    if (0 == strncasecmp(request, mod->module->name, len)) {
        return true;
    }
    if (NULL == mod->module->channels) {
        return false;
    }
    for (i = 0; NULL != mod->module->channels[i]; i++) {
        if (0 == strncasecmp(request, mod->module->channels[i], len)) {
            return true;
        }
    }
    return false;
}

/* release a module we are not going to use. Its init() has already
 * run by the time we get here, so it must be given the chance to undo
 * whatever that allocated before we drop the wrapper */
static void discard_module(pmix_plog_base_active_module_t *mod)
{
    if (NULL != mod->module->finalize) {
        mod->module->finalize();
    }
    PMIX_RELEASE(mod);
}

/* every module still on the working list has been init'd, so none of
 * them may simply be released */
static void discard_all(pmix_list_t *list)
{
    pmix_plog_base_active_module_t *mod;

    while (NULL != (mod = (pmix_plog_base_active_module_t *)
                        pmix_list_remove_first(list))) {
        discard_module(mod);
    }
    PMIX_DESTRUCT(list);
}

/* Function for selecting a prioritized array of components
 * from all those that are available. */
pmix_status_t pmix_plog_base_select(void)
{
    pmix_mca_base_component_list_item_t *cli = NULL;
    pmix_mca_base_component_t *component = NULL;
    pmix_mca_base_module_t *module = NULL;
    pmix_plog_module_t *nmodule;
    pmix_plog_base_active_module_t *newmodule, *mod;
    int rc, priority, n, m;
    bool inserted, reqd;
    pmix_list_t actives;
    char *ptr;
    size_t len;

    if (pmix_plog_globals.selected) {
        /* ensure we don't do this twice */
        return PMIX_SUCCESS;
    }
    pmix_plog_globals.selected = true;

    PMIX_CONSTRUCT(&actives, pmix_list_t);

    /* Query all available components and ask if they have a module */
    PMIX_LIST_FOREACH (cli, &pmix_plog_base_framework.framework_components,
                       pmix_mca_base_component_list_item_t) {
        component = (pmix_mca_base_component_t *) cli->cli_component;

        pmix_output_verbose(5, pmix_plog_base_framework.framework_output,
                            "mca:plog:select: checking available component %s",
                            component->pmix_mca_component_name);

        /* If there's no query function, skip it */
        if (NULL == component->pmix_mca_query_component) {
            pmix_output_verbose(
                5, pmix_plog_base_framework.framework_output,
                "mca:plog:select: Skipping component [%s]. It does not implement a query function",
                component->pmix_mca_component_name);
            continue;
        }

        /* Query the component */
        pmix_output_verbose(5, pmix_plog_base_framework.framework_output,
                            "mca:plog:select: Querying component [%s]",
                            component->pmix_mca_component_name);
        rc = component->pmix_mca_query_component(&module, &priority);

        /* If no module was returned, then skip component */
        if (PMIX_SUCCESS != rc || NULL == module) {
            pmix_output_verbose(
                5, pmix_plog_base_framework.framework_output,
                "mca:plog:select: Skipping component [%s]. Query failed to return a module",
                component->pmix_mca_component_name);
            continue;
        }

        /* If we got a module, keep it */
        nmodule = (pmix_plog_module_t *) module;
        /* let it initialize */
        if (NULL != nmodule->init && PMIX_SUCCESS != nmodule->init()) {
            continue;
        }
        /* add to the list of selected modules */
        newmodule = PMIX_NEW(pmix_plog_base_active_module_t);
        if (NULL == newmodule) {
            if (NULL != nmodule->finalize) {
                nmodule->finalize();
            }
            discard_all(&actives);
            return PMIX_ERR_NOMEM;
        }
        newmodule->pri = priority;
        newmodule->module = nmodule;
        newmodule->component = (pmix_plog_base_component_t *) cli->cli_component;

        /* maintain priority order */
        inserted = false;
        PMIX_LIST_FOREACH (mod, &actives, pmix_plog_base_active_module_t) {
            if (priority > mod->pri) {
                pmix_list_insert_pos(&actives, (pmix_list_item_t *) mod, &newmodule->super);
                inserted = true;
                break;
            }
        }
        if (!inserted) {
            /* must be lowest priority - add to end */
            pmix_list_append(&actives, &newmodule->super);
        }
    }

    /* if they gave us a desired ordering, then impose it here */
    if (NULL != pmix_plog_globals.channels) {
        for (n = 0; NULL != pmix_plog_globals.channels[n]; n++) {
            len = strlen(pmix_plog_globals.channels[n]);
            /* check for the "req" modifier */
            reqd = false;
            ptr = strrchr(pmix_plog_globals.channels[n], ':');
            if (NULL != ptr) {
                /* get the length of the remaining string so we
                 * can constrain our comparison of the channel
                 * name itself */
                len = len - strlen(ptr);
                /* move over the ':' */
                ++ptr;
                /* we accept anything that starts with "req" */
                if (0 == strncasecmp(ptr, "req", 3)) {
                    reqd = true;
                }
            }
            if (0 == len) {
                /* the entry named no channel at all (e.g., a bare
                 * ":req") - a zero-length comparison matches whatever
                 * module happens to come first, so refuse it instead */
                pmix_show_help("help-pmix-plog.txt", "reqd-not-found", true,
                               pmix_plog_globals.channels[n]);
                discard_all(&actives);
                return PMIX_ERR_BAD_PARAM;
            }
            /* now search for this channel in our list of actives */
            inserted = false;
            PMIX_LIST_FOREACH (mod, &actives, pmix_plog_base_active_module_t) {
                if (channel_matches(pmix_plog_globals.channels[n], len, mod)) {
                    pmix_list_remove_item(&actives, &mod->super);
                    if (0 > pmix_pointer_array_add(&pmix_plog_globals.actives, mod)) {
                        discard_module(mod);
                        discard_all(&actives);
                        return PMIX_ERR_NOMEM;
                    }
                    mod->reqd = reqd;
                    inserted = true;
                    break;
                }
            }
            if (!inserted) {
                /* one module can service several channels, so an
                 * earlier entry in the order may already have claimed
                 * it - that is not a missing channel */
                for (m = 0; m < pmix_plog_globals.actives.size; m++) {
                    mod = (pmix_plog_base_active_module_t *)
                            pmix_pointer_array_get_item(&pmix_plog_globals.actives, m);
                    if (NULL != mod &&
                        channel_matches(pmix_plog_globals.channels[n], len, mod)) {
                        if (reqd) {
                            mod->reqd = true;
                        }
                        inserted = true;
                        break;
                    }
                }
            }
            if (!inserted && reqd) {
                /* we didn't find a supporting module and this channel isn't optional.
                 * Nothing we can do except report an error */
                pmix_show_help("help-pmix-plog.txt", "reqd-not-found", true,
                               pmix_plog_globals.channels[n]);
                discard_all(&actives);
                return PMIX_ERR_NOT_FOUND;
            }
        }
        /* if there are any modules left over, we need to discard them */
        discard_all(&actives);
    } else {
        /* insert the modules into the global array in priority order */
        while (NULL != (mod = (pmix_plog_base_active_module_t *) pmix_list_remove_first(&actives))) {
            if (0 > pmix_pointer_array_add(&pmix_plog_globals.actives, mod)) {
                discard_module(mod);
                discard_all(&actives);
                return PMIX_ERR_NOMEM;
            }
        }
        PMIX_DESTRUCT(&actives);
    }

    if (4 < pmix_output_get_verbosity(pmix_plog_base_framework.framework_output)) {
        pmix_output(0, "Final plog order");
        /* show the prioritized order */
        for (n = 0; n < pmix_plog_globals.actives.size; n++) {
            if (NULL
                != (mod = (pmix_plog_base_active_module_t *)
                        pmix_pointer_array_get_item(&pmix_plog_globals.actives, n))) {
                pmix_output(0, "\tplog[%d]: %s", n, mod->component->pmix_mca_component_name);
            }
        }
    }

    return PMIX_SUCCESS;
}
