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

#include <errno.h>
#include <pwd.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef HAVE_GRP_H
#    include <grp.h>
#endif

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

/* Read one passwd or group entry into a buffer sized for it. The _r
 * interfaces report ERANGE for a buffer that is too small rather than
 * truncating, and sysconf() may not know the size at all. */
static char *entry_buffer(int which, size_t *len)
{
    long sz = sysconf(which);

    *len = (0 < sz) ? (size_t) sz : 16384;
    return (char *) malloc(*len);
}

bool pmix_psec_base_gid_held(uid_t uid, gid_t gid)
{
    struct passwd pwd, *pw = NULL;
    char *pwbuf, *grbuf = NULL, *tmp;
    size_t pwlen, grlen;
    bool held = false;
    int rc;
#ifdef HAVE_GRP_H
    struct group grp, *gr = NULL;
    size_t n;
#endif

    if (0 == uid) {
        return true;
    }

    pwbuf = entry_buffer(_SC_GETPW_R_SIZE_MAX, &pwlen);
    while (NULL != pwbuf &&
           ERANGE == (rc = getpwuid_r(uid, &pwd, pwbuf, pwlen, &pw))) {
        pwlen *= 2;
        tmp = (char *) realloc(pwbuf, pwlen);
        if (NULL == tmp) {
            break;
        }
        pwbuf = tmp;
    }
    if (NULL == pw) {
        /* no passwd entry for this uid - nothing we can say it holds */
        goto done;
    }
    if (pw->pw_gid == gid) {
        held = true;
        goto done;
    }

#ifdef HAVE_GRP_H
    grbuf = entry_buffer(_SC_GETGR_R_SIZE_MAX, &grlen);
    while (NULL != grbuf &&
           ERANGE == (rc = getgrgid_r(gid, &grp, grbuf, grlen, &gr))) {
        grlen *= 2;
        tmp = (char *) realloc(grbuf, grlen);
        if (NULL == tmp) {
            break;
        }
        grbuf = tmp;
    }
    if (NULL != gr && NULL != gr->gr_mem) {
        for (n = 0; NULL != gr->gr_mem[n]; n++) {
            if (0 == strcmp(gr->gr_mem[n], pw->pw_name)) {
                held = true;
                break;
            }
        }
    }
#else
    PMIX_HIDE_UNUSED_PARAMS(grlen);
#endif

done:
    free(pwbuf);
    free(grbuf);
    return held;
}
