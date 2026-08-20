/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2016      Mellanox Technologies, Inc.
 *                         All rights reserved.
 *
 * Copyright (c) 2021-2026 Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "src/include/pmix_config.h"

#include "pmix_common.h"
#include "src/include/pmix_globals.h"

#include "src/class/pmix_list.h"
#include "src/mca/ptl/base/base.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_output.h"

#include "src/mca/psec/base/base.h"

char *pmix_psec_base_get_available_modules(void)
{
    pmix_psec_base_active_module_t *active;
    char **tmp = NULL, *reply = NULL;

    if (!pmix_psec_globals.initialized) {
        return NULL;
    }

    PMIX_LIST_FOREACH (active, &pmix_psec_globals.actives, pmix_psec_base_active_module_t) {
        /* this list is advertised to our clients as the mechanisms they
         * may select from, so a silently truncated one would hide a
         * mechanism that is actually available */
        if (PMIX_SUCCESS
            != PMIx_Argv_append_nosize(&tmp, active->component->base.pmix_mca_component_name)) {
            PMIx_Argv_free(tmp);
            return NULL;
        }
    }
    if (NULL != tmp) {
        reply = PMIx_Argv_join(tmp, ',');
        PMIx_Argv_free(tmp);
    }
    return reply;
}

pmix_psec_module_t *pmix_psec_base_assign_module(const char *options)
{
    pmix_psec_base_active_module_t *active;
    pmix_psec_module_t *mod;
    char **tmp = NULL;
    int i;

    if (!pmix_psec_globals.initialized) {
        return NULL;
    }

    if (NULL != options) {
        tmp = PMIx_Argv_split(options, ',');
    }

    PMIX_LIST_FOREACH (active, &pmix_psec_globals.actives, pmix_psec_base_active_module_t) {
        if (NULL == active->component->assign_module) {
            /* a component with no way to hand out its module cannot
             * service anyone - skip it rather than dereferencing NULL */
            continue;
        }
        if (NULL == tmp) {
            if (NULL != (mod = active->component->assign_module())) {
                return mod;
            }
        } else {
            for (i = 0; NULL != tmp[i]; i++) {
                if (0 == strcmp(tmp[i], active->component->base.pmix_mca_component_name)) {
                    if (NULL != (mod = active->component->assign_module())) {
                        PMIx_Argv_free(tmp);
                        return mod;
                    }
                }
            }
        }
    }

    /* we only get here if nothing was found */
    if (NULL != tmp) {
        PMIx_Argv_free(tmp);
    }
    return NULL;
}

bool pmix_psec_base_check_directives(const char *name, const pmix_info_t directives[], size_t ndirs)
{
    char **types;
    size_t n, m;
    bool takeus;

    if (NULL == directives || 0 == ndirs) {
        /* the caller expressed no preference, so we are free to proceed */
        return true;
    }

    for (n = 0; n < ndirs; n++) {
        if (0 != strncmp(directives[n].key, PMIX_CRED_TYPE, PMIX_MAX_KEYLEN)) {
            continue;
        }
        /* the value must be a string holding the acceptable mechanisms.
         * Anything else cannot name us, and we must not hand it to the
         * argv splitter as if it were a string */
        if (PMIX_STRING != directives[n].value.type || NULL == directives[n].value.data.string) {
            return false;
        }
        types = PMIx_Argv_split(directives[n].value.data.string, ',');
        if (NULL == types) {
            /* the string held nothing but separators, so it names nobody */
            return false;
        }
        takeus = false;
        for (m = 0; NULL != types[m]; m++) {
            if (0 == strcmp(types[m], name)) {
                /* it's us! */
                takeus = true;
                break;
            }
        }
        PMIx_Argv_free(types);
        if (!takeus) {
            return false;
        }
    }

    return true;
}
