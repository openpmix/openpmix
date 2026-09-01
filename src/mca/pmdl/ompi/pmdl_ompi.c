/*
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2016      IBM Corporation.  All rights reserved.
 *
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * Copyright (c) 2026      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "src/include/pmix_config.h"

#include <string.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif
#ifdef HAVE_SYS_TYPES_H
#    include <sys/types.h>
#endif
#ifdef HAVE_SYS_UTSNAME_H
#    include <sys/utsname.h>
#endif

#include "include/pmix.h"

#include "src/class/pmix_list.h"
#include "src/include/pmix_globals.h"
#include "src/mca/base/pmix_mca_base_var.h"
#include "src/mca/base/pmix_mca_base_vari.h"
#include "src/mca/pinstalldirs/pinstalldirs.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_basename.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_name_fns.h"
#include "src/util/pmix_os_path.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_environ.h"
#include "src/util/pmix_printf.h"

#include "pmdl_ompi.h"
#include "src/mca/pmdl/base/base.h"
#include "src/mca/pmdl/pmdl.h"

static pmix_status_t ompi_init(void);
static void ompi_finalize(void);
static pmix_status_t harvest_envars(pmix_namespace_t *nptr,
                                    const pmix_info_t info[], size_t ninfo,
                                    pmix_list_t *ilist, char ***priors);
static void parse_file_envars(pmix_list_t *ilist);
static pmix_status_t setup_nspace(pmix_namespace_t *nptr, pmix_info_t *info);
static pmix_status_t setup_nspace_kv(pmix_namespace_t *nptr, pmix_kval_t *kv);
static pmix_status_t register_nspace(pmix_namespace_t *nptr);
static pmix_status_t setup_fork(const pmix_proc_t *proc, char ***env, char ***priors);
static void deregister_nspace(pmix_namespace_t *nptr);
pmix_pmdl_module_t pmix_pmdl_ompi_module = {
    .name = "ompi",
    .init = ompi_init,
    .finalize = ompi_finalize,
    .harvest_envars = harvest_envars,
    .parse_file_envars = parse_file_envars,
    .setup_nspace = setup_nspace,
    .setup_nspace_kv = setup_nspace_kv,
    .register_nspace = register_nspace,
    .setup_fork = setup_fork,
    .deregister_nspace = deregister_nspace
};

/* internal structures */
typedef struct {
    pmix_list_item_t super;
    pmix_nspace_t nspace;
    uint32_t univ_size;
    uint32_t job_size;
    uint32_t local_size;
    uint32_t num_apps;
} pmdl_nspace_t;
static void nscon(pmdl_nspace_t *p)
{
    PMIX_LOAD_NSPACE(p->nspace, NULL);
    p->univ_size = UINT32_MAX;
    p->job_size = UINT32_MAX;
    p->local_size = UINT32_MAX;
    p->num_apps = UINT32_MAX;
}
static PMIX_CLASS_INSTANCE(pmdl_nspace_t,
                           pmix_list_item_t,
                           nscon, NULL);

/* Cache of the invariant harvest results (system and user MCA param
 * files, the local envar harvest, and the OPAL_PREFIX derivation).
 * These depend only on the server's environment and the requesting
 * user's home directory, so we parse them once and keep the resulting
 * kvals here, copying them into each caller's list on subsequent
 * requests instead of re-reading and re-parsing the files.  The entry
 * is keyed by uid because the user param file is resolved from the
 * user's home directory, and a server may service more than one user. */
typedef struct {
    pmix_list_item_t super;
    uint32_t uid;
    pmix_list_t results;
} pmdl_cache_t;
static void ccon(pmdl_cache_t *p)
{
    p->uid = UINT32_MAX;
    PMIX_CONSTRUCT(&p->results, pmix_list_t);
}
static void cdes(pmdl_cache_t *p)
{
    PMIX_LIST_DESTRUCT(&p->results);
}
static PMIX_CLASS_INSTANCE(pmdl_cache_t,
                           pmix_list_item_t,
                           ccon, cdes);

/* internal variables */
static pmix_list_t mynspaces;
static pmix_list_t myenvars;
static pmix_list_t mycache;

static pmix_status_t ompi_init(void)
{
    pmix_output_verbose(2, pmix_pmdl_base_framework.framework_output, "pmdl: ompi init");

    PMIX_CONSTRUCT(&mynspaces, pmix_list_t);
    PMIX_CONSTRUCT(&myenvars, pmix_list_t);
    PMIX_CONSTRUCT(&mycache, pmix_list_t);

    return PMIX_SUCCESS;
}

static void ompi_finalize(void)
{
    PMIX_LIST_DESTRUCT(&mynspaces);
    PMIX_LIST_DESTRUCT(&myenvars);
    PMIX_LIST_DESTRUCT(&mycache);
}

/* the string a model/personality key carries, or NULL if it is not
 * carrying one.
 *
 * These keys arrive in the info array a host handed to
 * PMIx_server_register_nspace, and in job-level data unpacked from a
 * peer - neither of which this component gets to police.  A value whose
 * type is not PMIX_STRING has something else in the union, so reading
 * data.string from it is a read through whatever bit pattern that
 * something else happened to be. */
static const char *model_string(const pmix_value_t *value)
{
    if (PMIX_STRING != value->type || NULL == value->data.string) {
        return NULL;
    }
    return value->data.string;
}

static bool checkus(const pmix_info_t info[], size_t ninfo)
{
    bool takeus = false;
    const char *str;
    size_t n;

    if (NULL == info) {
        return false;
    }

    /* check the directives */
    for (n = 0; n < ninfo && !takeus; n++) {
        /* check the attribute */
        if (PMIX_CHECK_KEY(&info[n], PMIX_PROGRAMMING_MODEL) ||
            PMIX_CHECK_KEY(&info[n], PMIX_PERSONALITY)) {
            str = model_string(&info[n].value);
            if (NULL != str && NULL != strstr(str, "ompi")) {
                takeus = true;
                break;
            }
        }
    }

    return takeus;
}

/* Append one "set this in the child's environment" directive to ilist.
 *
 * Every caller here was open-coding the same dozen lines, and each copy
 * had to remember that PMIX_ENVAR_LOAD writes only the fields it is
 * handed while the pmix_value_t behind the kval is plain malloc'd.  A
 * param-file line that names a variable but gives it no value ("foo =")
 * parses to a NULL value, so those copies left
 * kv->value->data.envar.value holding uninitialized heap - which the
 * owner of the list later strdup()s and free()s.  Absent is empty here,
 * and there is one copy of the rule. */
static pmix_status_t add_envar(pmix_list_t *ilist, const char *key,
                               const char *var, const char *value, char separator)
{
    pmix_kval_t *kv;

    PMIX_KVAL_NEW(kv, key);
    if (NULL == kv || NULL == kv->key) {
        if (NULL != kv) {
            PMIX_RELEASE(kv);
        }
        return PMIX_ERR_OUT_OF_RESOURCE;
    }
    if (NULL == kv->value) {
        PMIX_RELEASE(kv);
        return PMIX_ERR_OUT_OF_RESOURCE;
    }
    kv->value->type = PMIX_ENVAR;
    PMIX_ENVAR_LOAD(&kv->value->data.envar, (char *) var,
                    (NULL == value) ? "" : (char *) value, separator);
    if (NULL == kv->value->data.envar.envar || NULL == kv->value->data.envar.value) {
        PMIX_RELEASE(kv);
        return PMIX_ERR_OUT_OF_RESOURCE;
    }
    pmix_list_append(ilist, &kv->super);
    return PMIX_SUCCESS;
}

/* the same, for a variable whose name needs a prefix put on it */
static pmix_status_t add_prefixed_envar(pmix_list_t *ilist, const char *prefix,
                                        const char *var, const char *value)
{
    pmix_status_t rc;
    char *tmp;

    if (0 > pmix_asprintf(&tmp, "%s%s", prefix, var)) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    }
    rc = add_envar(ilist, PMIX_SET_ENVAR, tmp, value, ':');
    free(tmp);
    return rc;
}

static pmix_status_t process_param_file(char *file, pmix_list_t *ilist)
{
    pmix_list_t params;
    pmix_mca_base_var_file_value_t *fv;
    pmix_status_t rc;
    const char *prefix;

    if (NULL == file) {
        /* every caller builds the name with pmix_os_path(), which
         * answers NULL when the assembled path exceeds PMIX_PATH_MAX or
         * memory ran out. There is no file to read, and the name must
         * not reach fopen() */
        return PMIX_ERR_OUT_OF_RESOURCE;
    }

    PMIX_CONSTRUCT(&params, pmix_list_t);
    pmix_mca_base_parse_paramfile(file, &params);
    PMIX_LIST_FOREACH (fv, &params, pmix_mca_base_var_file_value_t) {
        /* a single param file can hold values directed at any of the
         * three libraries, and each one has to carry the prefix its
         * owner will look for */
        if (pmix_pmdl_base_check_pmix_param(fv->mbvfv_var)) {
            prefix = "PMIX_MCA_";
        } else if (pmix_pmdl_base_check_prte_param(fv->mbvfv_var)) {
            /* an old ORTE or a PRRTE value */
            prefix = "PRTE_MCA_";
        } else {
            /* assume this is an OMPI param */
            prefix = "OMPI_MCA_";
        }
        rc = add_prefixed_envar(ilist, prefix, fv->mbvfv_var, fv->mbvfv_value);
        if (PMIX_SUCCESS != rc) {
            PMIX_LIST_DESTRUCT(&params);
            return rc;
        }
    }
    PMIX_LIST_DESTRUCT(&params);
    return PMIX_SUCCESS;
}

static pmdl_cache_t *find_cache(uint32_t uid)
{
    pmdl_cache_t *cache;

    PMIX_LIST_FOREACH (cache, &mycache, pmdl_cache_t) {
        if (uid == cache->uid) {
            return cache;
        }
    }
    return NULL;
}

/* copy the cached kvals into the caller's list - the caller takes
 * ownership of and eventually destructs what it receives, so we must
 * hand out fresh copies rather than the cached objects themselves */
static pmix_status_t copy_cache(pmix_list_t *src, pmix_list_t *ilist)
{
    pmix_kval_t *kptr;
    pmix_status_t rc;

    PMIX_LIST_FOREACH (kptr, src, pmix_kval_t) {
        rc = add_envar(ilist, kptr->key, kptr->value->data.envar.envar,
                       kptr->value->data.envar.value, kptr->value->data.envar.separator);
        if (PMIX_SUCCESS != rc) {
            return rc;
        }
    }
    return PMIX_SUCCESS;
}

/* Parse the system and user MCA param files, harvest the local envars,
 * and derive the OPAL_PREFIX side effects for this uid - once.  The
 * result set is invariant for the life of the process, so we build it
 * into a cache entry that later requests reuse via copy_cache().  The
 * entry is only appended to mycache on full success, so a failure part
 * way through never leaves a poisoned (partial) cache behind. */
static pmix_status_t build_cache(uint32_t uid, pmdl_cache_t **out)
{
    pmdl_cache_t *cache;
    pmix_status_t rc;
    const char *home;
    pmix_kval_t *kptr;
    char *file, *evar, *tmp;

    cache = PMIX_NEW(pmdl_cache_t);
    if (NULL == cache) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    }
    cache->uid = uid;

    /* check if the user has set OMPIHOME in their environment */
    if (NULL != (evar = getenv("OMPIHOME"))) {
        /* look for the default MCA param file */
        file = pmix_os_path(false, evar, "etc", "openmpi-mca-params.conf", NULL);
        rc = process_param_file(file, &cache->results);
        free(file);
        if (PMIX_SUCCESS != rc) {
            goto error;
        }
        /* add an envar indicating that we did this so the OMPI
         * processes won't duplicate it */
        rc = add_envar(&cache->results, PMIX_SET_ENVAR, "OPAL_SYS_PARAMS_GIVEN", "1", ':');
        if (PMIX_SUCCESS != rc) {
            goto error;
        }
    }

    /* try to get their home directory */
    home = pmix_home_directory(uid);
    if (NULL != home) {
        file = pmix_os_path(false, home, ".openmpi", "mca-params.conf", NULL);
        rc = process_param_file(file, &cache->results);
        free(file);
        if (PMIX_SUCCESS != rc) {
            goto error;
        }
        /* add an envar indicating that we did this so the OMPI
         * processes won't duplicate it */
        rc = add_envar(&cache->results, PMIX_SET_ENVAR, "OPAL_USER_PARAMS_GIVEN", "1", ':');
        if (PMIX_SUCCESS != rc) {
            goto error;
        }
    }

    /* harvest our local envars */
    if (NULL != pmix_mca_pmdl_ompi_component.include) {
        pmix_output_verbose(2, pmix_pmdl_base_framework.framework_output,
                            "pmdl: ompi harvesting envars %s excluding %s",
                            (NULL == pmix_mca_pmdl_ompi_component.incparms)
                                ? "NONE"
                                : pmix_mca_pmdl_ompi_component.incparms,
                            (NULL == pmix_mca_pmdl_ompi_component.excparms)
                                ? "NONE"
                                : pmix_mca_pmdl_ompi_component.excparms);
        rc = pmix_util_harvest_envars(pmix_mca_pmdl_ompi_component.include,
                                      pmix_mca_pmdl_ompi_component.exclude, &cache->results);
        if (PMIX_SUCCESS != rc) {
            goto error;
        }
    }

    /* check if one of the envars is OPAL_PREFIX as we need to
     * do more to set that up.  Param-file values are always re-prefixed
     * (OMPI_MCA_/PMIX_MCA_/PRTE_MCA_), so an unprefixed OPAL_PREFIX can
     * only have come from the environment harvest above. */
    PMIX_LIST_FOREACH (kptr, &cache->results, pmix_kval_t) {
        if (PMIX_ENVAR != kptr->value->type) {
            continue;
        }
        if (0 == strcmp(kptr->value->data.envar.envar, "OPAL_PREFIX")) {
            // need to modify LD_LIBRARY_PATH as well - can only assume
            // that they used the same libdir name as we did
            evar = pmix_basename(pmix_pinstall_dirs.libdir);
            if (NULL == evar) {
                rc = PMIX_ERR_OUT_OF_RESOURCE;
                goto error;
            }
            if (0 > pmix_asprintf(&tmp, "%s/%s", kptr->value->data.envar.value, evar)) {
                free(evar);
                rc = PMIX_ERR_OUT_OF_RESOURCE;
                goto error;
            }
            free(evar);
            rc = add_envar(&cache->results, PMIX_PREPEND_ENVAR, "LD_LIBRARY_PATH", tmp, ':');
            free(tmp);
            if (PMIX_SUCCESS != rc) {
                goto error;
            }
            break;
        }
    }

    pmix_list_append(&mycache, &cache->super);
    *out = cache;
    return PMIX_SUCCESS;

error:
    PMIX_RELEASE(cache);
    return rc;
}

static pmix_status_t harvest_envars(pmix_namespace_t *nptr,
                                    const pmix_info_t info[], size_t ninfo,
                                    pmix_list_t *ilist, char ***priors)
{
    pmdl_nspace_t *ns, *ns2;
    pmdl_cache_t *cache;
    pmix_status_t rc;
    uint32_t uid = UINT32_MAX;
    pmix_mca_base_var_file_value_t *fv;
    size_t n;

    pmix_output_verbose(2, pmix_pmdl_base_framework.framework_output,
                        "pmdl:ompi:harvest envars");

    if (!checkus(info, ninfo)) {
        return PMIX_ERR_TAKE_NEXT_OPTION;
    }

    /* don't do OMPI again if already done */
    if (NULL != *priors) {
        char **t2 = *priors;
        for (n = 0; NULL != t2[n]; n++) {
            if (0 == strcmp(t2[n], "ompi")) {
                return PMIX_ERR_TAKE_NEXT_OPTION;
            }
        }
    }
    /* flag that we worked on this */
    PMIx_Argv_append_nosize(priors, "ompi");

    pmix_output_verbose(2, pmix_pmdl_base_framework.framework_output,
                        "pmdl:ompi:harvest envars active");

    /* are we to harvest envars? */
    for (n=0; n < ninfo; n++) {
        if (PMIX_CHECK_KEY(&info[n], PMIX_SETUP_APP_ENVARS)) {
            goto harvest;
        }
    }
    pmix_output_verbose(2, pmix_pmdl_base_framework.framework_output,
                        "pmdl:ompi:harvest envars: NO");
    return PMIX_ERR_TAKE_NEXT_OPTION;

harvest:
    if (NULL != nptr) {
        /* see if we already have this nspace */
        ns = NULL;
        PMIX_LIST_FOREACH (ns2, &mynspaces, pmdl_nspace_t) {
            if (PMIX_CHECK_NSPACE(ns2->nspace, nptr->nspace)) {
                ns = ns2;
                break;
            }
        }
        if (NULL == ns) {
            ns = PMIX_NEW(pmdl_nspace_t);
            if (NULL == ns) {
                return PMIX_ERR_OUT_OF_RESOURCE;
            }
            PMIX_LOAD_NSPACE(ns->nspace, nptr->nspace);
            pmix_list_append(&mynspaces, &ns->super);
        }
    }

    /* determine which user we are servicing - the user's default MCA
     * param file is resolved from their home directory */
    for (n = 0; n < ninfo; n++) {
        if (PMIX_CHECK_KEY(&info[n], PMIX_USERID)) {
            rc = PMIx_Value_get_number(&info[n].value, &uid, PMIX_UINT32);
            if (PMIX_SUCCESS != rc) {
                return rc;
            }
            break;
        }
    }
    if (UINT32_MAX == uid) {
        uid = geteuid();
    }

    /* the param files and local envars are invariant for the life of
     * the process, so parse them only once per user and reuse the
     * result on subsequent requests */
    cache = find_cache(uid);
    if (NULL == cache) {
        rc = build_cache(uid, &cache);
        if (PMIX_SUCCESS != rc) {
            return rc;
        }
    }
    rc = copy_cache(&cache->results, ilist);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }

    /* add in any OMPI-specific envars that were in an MCA
     * base paramfile since PMIx overlaps in that area */
    PMIX_LIST_FOREACH(fv, &myenvars, pmix_mca_base_var_file_value_t) {
        // the OMPI_MCA_ has already been prefixed
        rc = add_envar(ilist, PMIX_SET_ENVAR, fv->mbvfv_var, fv->mbvfv_value, ':');
        if (PMIX_SUCCESS != rc) {
            return rc;
        }
    }
    return PMIX_SUCCESS;
}

// These frameworks are current as of 16 Sep, 2022, and are the list
// of frameworks that are planned to be in Open MPI v5.0.0.
static char *ompi_frameworks_static_5_0_0[] = {
    // Generic prefixes used by OMPI
    "mca",
    "opal",
    "ompi",

    /* OPAL frameworks */
    "allocator",
    "backtrace",
    "btl",
    "dl",
    "hwloc",
    "if",
    "installdirs",
    "memchecker",
    "memcpy",
    "memory",
    "mpool",
    "patcher",
    "pmix",
    "rcache",
    "reachable",
    "shmem",
    "smsc",
    "threads",
    "timer",
    /* OMPI frameworks */
    "mpi", /* global options set in runtime/ompi_mpi_params.c */
    "bml",
    "coll",
    "fbtl",
    "fcoll",
    "fs",
    "hook",
    "io",
    "mtl",
    "op",
    "osc",
    "part",
    "pml",
    "sharedfp",
    "topo",
    "vprotocol",
    /* OSHMEM frameworks */
    "memheap",
    "scoll",
    "spml",
    "sshmem",
    NULL,
};
static char **ompi_frameworks = ompi_frameworks_static_5_0_0;
static bool ompi_frameworks_setup = false;

static void setup_ompi_frameworks(void)
{
    if (ompi_frameworks_setup) {
        return;
    }
    ompi_frameworks_setup = true;

    char *env = getenv("OMPI_MCA_PREFIXES");
    if (NULL == env) {
        return;
    }

    // If we found the env variable, it will be a comma-delimited list
    // of values.  Split it into an argv-style array.
    char **tmp = PMIx_Argv_split(env, ',');
    if (NULL != tmp) {
        ompi_frameworks = tmp;
    }
}

/* does "var" name something inside framework "fw"?  An MCA variable is
 * <framework>_<component>_<name>, or the bare framework name for the
 * framework's own value ("btl = self,tcp"), so the match has to end on
 * that boundary: a plain strncmp of the framework's length also claims
 * "iface" for "if" and "opal_x" for "op". */
static bool in_framework(const char *var, const char *fw)
{
    size_t len = strlen(fw);

    if (0 != strncmp(var, fw, len)) {
        return false;
    }
    return ('\0' == var[len] || '_' == var[len]);
}

static void parse_file_envars(pmix_list_t *ilist)
{
    pmix_mca_base_var_file_value_t *fv, *fvsave;
    char *tmp;

    // ensure we have the current list of frameworks
    setup_ompi_frameworks();

    // scan the list for values directed at OMPI
    // frameworks/components
    PMIX_LIST_FOREACH_SAFE (fv, fvsave, ilist, pmix_mca_base_var_file_value_t) {
        /* the list we are handed holds what PMIx read from *its* param
         * files, and two names appear in both framework tables - "mca",
         * and "pmix" (OPAL carried a pmix framework of its own).  Taking
         * those meant a value the user set for PMIx, such as
         * pmix_hwloc_topo_file, left this list renamed OMPI_MCA_* and so
         * reached neither library: PMIx children never saw it because it
         * was gone from the list harvest_envars prefixes, and Open MPI
         * has no such parameter.  process_param_file already resolves the
         * overlap in PMIx's favor; do the same here. */
        if (pmix_pmdl_base_check_pmix_param(fv->mbvfv_var)) {
            continue;
        }
        for (int j = 0; NULL != ompi_frameworks[j]; j++) {
            if (in_framework(fv->mbvfv_var, ompi_frameworks[j])) {
                /* rename it before taking it out of the caller's list, so
                 * a failure here leaves the entry intact rather than
                 * moving it across with no name at all */
                if (0 > pmix_asprintf(&tmp, "OMPI_MCA_%s", fv->mbvfv_var)) {
                    break;
                }
                pmix_list_remove_item(ilist, &fv->super);
                free(fv->mbvfv_var);
                fv->mbvfv_var = tmp;
                pmix_list_append(&myenvars, &fv->super);
                break;
            }
        }
    }
}

static pmix_status_t setup_nspace(pmix_namespace_t *nptr, pmix_info_t *info)
{
    pmdl_nspace_t *ns, *ns2;
    const char *str;

    str = model_string(&info->value);
    pmix_output_verbose(2, pmix_pmdl_base_framework.framework_output,
                        "pmdl:ompi: setup nspace for nspace %s with %s", nptr->nspace,
                        (NULL == str) ? "<non-string>" : str);

    if (!checkus(info, 1)) {
        return PMIX_ERR_TAKE_NEXT_OPTION;
    }

    /* see if we already have this nspace */
    ns = NULL;
    PMIX_LIST_FOREACH (ns2, &mynspaces, pmdl_nspace_t) {
        if (PMIX_CHECK_NSPACE(ns2->nspace, nptr->nspace)) {
            ns = ns2;
            break;
        }
    }
    if (NULL == ns) {
        ns = PMIX_NEW(pmdl_nspace_t);
        if (NULL == ns) {
            return PMIX_ERR_OUT_OF_RESOURCE;
        }
        PMIX_LOAD_NSPACE(ns->nspace, nptr->nspace);
        pmix_list_append(&mynspaces, &ns->super);
    }

    return PMIX_SUCCESS;
}

static pmix_status_t setup_nspace_kv(pmix_namespace_t *nptr, pmix_kval_t *kv)
{
    pmdl_nspace_t *ns, *ns2;
    const char *str;
    char **tmp, *ptr;
    size_t m;
    unsigned long vers;
    bool takeus = false;

    str = model_string(kv->value);
    pmix_output_verbose(2, pmix_pmdl_base_framework.framework_output,
                        "pmdl:ompi: setup nspace_kv for nspace %s with %s", nptr->nspace,
                        (NULL == str) ? "<non-string>" : str);

    /* check the attribute */
    if (NULL != str &&
        (PMIX_CHECK_KEY(kv, PMIX_PROGRAMMING_MODEL) || PMIX_CHECK_KEY(kv, PMIX_PERSONALITY))) {
        /* a string with nothing in it splits to no argv at all */
        tmp = PMIx_Argv_split(str, ',');
        for (m = 0; NULL != tmp && NULL != tmp[m]; m++) {
            if (0 == strcmp(tmp[m], "ompi")) {
                /* they didn't specify a level, so we will service
                 * them just in case */
                takeus = true;
                break;
            }
            if (0 == strncmp(tmp[m], "ompi", 4)) {
                /* if they specifically requested an ompi level greater
                 * than or equal to us, then we service it */
                ptr = &tmp[m][4];
                vers = strtoul(ptr, NULL, 10);
                if (vers >= 5) {
                    takeus = true;
                }
                break;
            }
        }
        PMIx_Argv_free(tmp);
    }
    if (!takeus) {
        return PMIX_ERR_TAKE_NEXT_OPTION;
    }

    /* see if we already have this nspace */
    ns = NULL;
    PMIX_LIST_FOREACH (ns2, &mynspaces, pmdl_nspace_t) {
        if (PMIX_CHECK_NSPACE(ns2->nspace, nptr->nspace)) {
            ns = ns2;
            break;
        }
    }
    if (NULL == ns) {
        ns = PMIX_NEW(pmdl_nspace_t);
        if (NULL == ns) {
            return PMIX_ERR_OUT_OF_RESOURCE;
        }
        PMIX_LOAD_NSPACE(ns->nspace, nptr->nspace);
        pmix_list_append(&mynspaces, &ns->super);
    }

    return PMIX_SUCCESS;
}

static pmix_status_t register_nspace(pmix_namespace_t *nptr)
{
    pmdl_nspace_t *ns, *ns2;
    char *ev1, **tmp;
    pmix_proc_t wildcard, undef;
    pmix_status_t rc;
    pmix_kval_t *kv;
    pmix_info_t info[2];
    uint32_t n;
    pmix_cb_t cb;

    pmix_output_verbose(2, pmix_pmdl_base_framework.framework_output,
                        "pmdl:ompi: register_nspace for %s", nptr->nspace);

    /* see if we already have this nspace */
    ns = NULL;
    PMIX_LIST_FOREACH (ns2, &mynspaces, pmdl_nspace_t) {
        if (PMIX_CHECK_NSPACE(ns2->nspace, nptr->nspace)) {
            ns = ns2;
            break;
        }
    }
    if (NULL == ns) {
        /* we don't know anything about this one or
         * it doesn't have any ompi-based apps */
        return PMIX_ERR_TAKE_NEXT_OPTION;
    }

    /* do we already have the data we need here? Servers are
     * allowed to call register_nspace multiple times with
     * different info, so we really need to recheck those
     * values that haven't already been filled */
    PMIX_LOAD_PROCID(&wildcard, nptr->nspace, PMIX_RANK_WILDCARD);

    /* fetch the universe size */
    if (UINT32_MAX == ns->univ_size) {
        PMIX_CONSTRUCT(&cb, pmix_cb_t);
        cb.proc = &wildcard;
        cb.copy = true;
        cb.key = PMIX_UNIV_SIZE;
        PMIX_GDS_FETCH_KV(rc, pmix_globals.mypeer, &cb);
        cb.key = NULL;
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DESTRUCT(&cb);
            return rc;
        }
        /* the data is the first value on the cb.kvs list */
        if (1 != pmix_list_get_size(&cb.kvs)) {
            PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
            PMIX_DESTRUCT(&cb);
            return PMIX_ERR_BAD_PARAM;
        }
        kv = (pmix_kval_t *) pmix_list_get_first(&cb.kvs);
        ns->univ_size = kv->value->data.uint32;
        PMIX_DESTRUCT(&cb);
    }

    /* fetch the job size */
    if (UINT32_MAX == ns->job_size) {
        PMIX_CONSTRUCT(&cb, pmix_cb_t);
        cb.proc = &wildcard;
        cb.copy = true;
        cb.key = PMIX_JOB_SIZE;
        PMIX_GDS_FETCH_KV(rc, pmix_globals.mypeer, &cb);
        cb.key = NULL;
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DESTRUCT(&cb);
            return rc;
        }
        /* the data is the first value on the cb.kvs list */
        if (1 != pmix_list_get_size(&cb.kvs)) {
            PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
            PMIX_DESTRUCT(&cb);
            return PMIX_ERR_BAD_PARAM;
        }
        kv = (pmix_kval_t *) pmix_list_get_first(&cb.kvs);
        ns->job_size = kv->value->data.uint32;
        PMIX_DESTRUCT(&cb);
    }

    /* fetch the number of apps */
    if (UINT32_MAX == ns->num_apps) {
        PMIX_CONSTRUCT(&cb, pmix_cb_t);
        cb.proc = &wildcard;
        cb.copy = true;
        cb.key = PMIX_JOB_NUM_APPS;
        PMIX_GDS_FETCH_KV(rc, pmix_globals.mypeer, &cb);
        cb.key = NULL;
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DESTRUCT(&cb);
            return rc;
        }
        /* the data is the first value on the cb.kvs list */
        if (1 != pmix_list_get_size(&cb.kvs)) {
            PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
            PMIX_DESTRUCT(&cb);
            return PMIX_ERR_BAD_PARAM;
        }
        kv = (pmix_kval_t *) pmix_list_get_first(&cb.kvs);
        ns->num_apps = kv->value->data.uint32;
        PMIX_DESTRUCT(&cb);
    }

    /* fetch the number of local peers */
    if (UINT32_MAX == ns->local_size) {
        PMIX_CONSTRUCT(&cb, pmix_cb_t);
        cb.proc = &wildcard;
        cb.copy = true;
        cb.key = PMIX_LOCAL_SIZE;
        PMIX_GDS_FETCH_KV(rc, pmix_globals.mypeer, &cb);
        cb.key = NULL;
        /* it is okay if there are no local procs */
        if (PMIX_SUCCESS == rc) {
            /* the data is the first value on the cb.kvs list */
            if (1 != pmix_list_get_size(&cb.kvs)) {
                PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
                PMIX_DESTRUCT(&cb);
                return PMIX_ERR_BAD_PARAM;
            }
            kv = (pmix_kval_t *) pmix_list_get_first(&cb.kvs);
            ns->local_size = kv->value->data.uint32;
        }
        /* the cb owns whatever the fetch put on its list either way */
        PMIX_DESTRUCT(&cb);
    }

    if (1 == ns->num_apps) {
        return PMIX_SUCCESS;
    }

    /* construct the list of app sizes */
    PMIX_LOAD_PROCID(&undef, nptr->nspace, PMIX_RANK_UNDEF);
    PMIX_INFO_LOAD(&info[0], PMIX_APP_INFO, NULL, PMIX_BOOL);
    tmp = NULL;
    for (n = 0; n < ns->num_apps; n++) {
        PMIX_CONSTRUCT(&cb, pmix_cb_t);
        cb.proc = &undef;
        cb.copy = true;
        cb.info = info;
        cb.ninfo = 2;
        cb.key = PMIX_APP_SIZE;
        PMIX_INFO_LOAD(&info[1], PMIX_APPNUM, &n, PMIX_UINT32);
        PMIX_GDS_FETCH_KV(rc, pmix_globals.mypeer, &cb);
        PMIX_INFO_DESTRUCT(&info[1]);
        cb.key = NULL;
        cb.info = NULL;
        cb.ninfo = 0;
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DESTRUCT(&cb);
            return rc;
        }
        /* the data is the first value on the cb.kvs list */
        if (1 != pmix_list_get_size(&cb.kvs)) {
            PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
            PMIX_DESTRUCT(&cb);
            return PMIX_ERR_BAD_PARAM;
        }
        kv = (pmix_kval_t *) pmix_list_get_first(&cb.kvs);
        pmix_asprintf(&ev1, "%u", kv->value->data.uint32);
        PMIx_Argv_append_nosize(&tmp, ev1);
        free(ev1);
        PMIX_DESTRUCT(&cb);
    }
    PMIX_INFO_DESTRUCT(&info[0]);

    if (NULL != tmp) {
        ev1 = PMIx_Argv_join(tmp, ' ');
        PMIx_Argv_free(tmp);
        PMIX_INFO_LOAD(&info[0], "OMPI_APP_SIZES", ev1, PMIX_STRING);
        free(ev1);
        PMIX_GDS_CACHE_JOB_INFO(rc, pmix_globals.mypeer, nptr, info, 1);
        PMIX_INFO_DESTRUCT(&info[0]);
        if (PMIX_SUCCESS != rc) {
            /* setup_fork hands this to the child from the datastore, so a
             * silent failure here is a job that comes up without it */
            PMIX_ERROR_LOG(rc);
            return rc;
        }
    }

    /* construct the list of app leaders */
    PMIX_INFO_LOAD(&info[0], PMIX_APP_INFO, NULL, PMIX_BOOL);
    tmp = NULL;
    for (n = 0; n < ns->num_apps; n++) {
        PMIX_CONSTRUCT(&cb, pmix_cb_t);
        cb.proc = &undef;
        cb.copy = true;
        cb.info = info;
        cb.ninfo = 2;
        cb.key = PMIX_APPLDR;
        PMIX_INFO_LOAD(&info[1], PMIX_APPNUM, &n, PMIX_UINT32);
        PMIX_GDS_FETCH_KV(rc, pmix_globals.mypeer, &cb);
        PMIX_INFO_DESTRUCT(&info[1]);
        cb.key = NULL;
        cb.info = NULL;
        cb.ninfo = 0;
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DESTRUCT(&cb);
            return rc;
        }
        /* the data is the first value on the cb.kvs list */
        if (1 != pmix_list_get_size(&cb.kvs)) {
            PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
            PMIX_DESTRUCT(&cb);
            return PMIX_ERR_BAD_PARAM;
        }
        kv = (pmix_kval_t *) pmix_list_get_first(&cb.kvs);
        pmix_asprintf(&ev1, "%u", kv->value->data.uint32);
        PMIx_Argv_append_nosize(&tmp, ev1);
        free(ev1);
        PMIX_DESTRUCT(&cb);
    }
    PMIX_INFO_DESTRUCT(&info[0]);

    if (NULL != tmp) {
        ev1 = PMIx_Argv_join(tmp, ' ');
        PMIx_Argv_free(tmp);
        tmp = NULL;
        PMIX_INFO_LOAD(&info[0], "OMPI_FIRST_RANKS", ev1, PMIX_STRING);
        free(ev1);
        PMIX_GDS_CACHE_JOB_INFO(rc, pmix_globals.mypeer, nptr, info, 1);
        PMIX_INFO_DESTRUCT(&info[0]);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            return rc;
        }
    }

    return PMIX_SUCCESS;
}

/* Fetch one value from our own datastore, copying it into "value" - which
 * the caller destructs.  "qual"/"nqual" carry the app-level qualifiers,
 * if any.  Every call site used to open-code this, and each copy had to
 * remember to destruct the cb on all three exits; two of them did not. */
static pmix_status_t fetch_value(const pmix_proc_t *proc, const char *key,
                                 pmix_info_t *qual, size_t nqual,
                                 pmix_value_t *value)
{
    pmix_cb_t cb;
    pmix_kval_t *kv;
    pmix_status_t rc;

    PMIX_CONSTRUCT(&cb, pmix_cb_t);
    cb.proc = (pmix_proc_t *) proc;
    cb.copy = true;
    cb.info = qual;
    cb.ninfo = nqual;
    cb.key = (char *) key;
    PMIX_GDS_FETCH_KV(rc, pmix_globals.mypeer, &cb);
    cb.key = NULL;
    cb.info = NULL;
    cb.ninfo = 0;
    if (PMIX_SUCCESS == rc) {
        /* the data is the first value on the cb.kvs list */
        if (1 != pmix_list_get_size(&cb.kvs)) {
            rc = PMIX_ERR_BAD_PARAM;
        } else {
            kv = (pmix_kval_t *) pmix_list_get_first(&cb.kvs);
            rc = PMIx_Value_xfer(value, kv->value);
        }
    }
    PMIX_DESTRUCT(&cb);
    return rc;
}

/* the same, for a value the caller wants as a string it does not own */
static pmix_status_t setenv_string(const pmix_proc_t *proc, const char *key,
                                   pmix_info_t *qual, size_t nqual,
                                   const char *var, char ***env)
{
    pmix_value_t val;
    pmix_status_t rc;

    PMIX_VALUE_CONSTRUCT(&val);
    rc = fetch_value(proc, key, qual, nqual, &val);
    if (PMIX_SUCCESS != rc) {
        PMIX_VALUE_DESTRUCT(&val);
        return rc;
    }
    if (PMIX_STRING != val.type || NULL == val.data.string) {
        /* the host stored something else under a key we can only use as
         * a string - saying so beats setting the variable from whatever
         * else was in the union */
        PMIX_VALUE_DESTRUCT(&val);
        return PMIX_ERR_TYPE_MISMATCH;
    }
    rc = PMIx_Setenv(var, val.data.string, true, env);
    PMIX_VALUE_DESTRUCT(&val);
    return rc;
}

/* set one variable from an unsigned number we already have */
static pmix_status_t setenv_number(const char *var, unsigned long value, char ***env)
{
    pmix_status_t rc;
    char *param;

    if (0 > pmix_asprintf(&param, "%lu", value)) {
        return PMIX_ERR_NOMEM;
    }
    rc = PMIx_Setenv(var, param, true, env);
    free(param);
    return rc;
}

/* join one per-app value across every app in the job into the space
 * separated string Open MPI expects, and set it in the child's env */
static pmix_status_t setenv_per_app(const pmix_proc_t *proc, uint32_t num_apps,
                                    const char *key, const char *var, char ***env)
{
    pmix_proc_t undef;
    pmix_info_t info[2];
    pmix_value_t val;
    pmix_status_t rc;
    char *ev1, **tmp = NULL;
    uint32_t n, u32;

    PMIX_LOAD_PROCID(&undef, proc->nspace, PMIX_RANK_UNDEF);
    PMIX_INFO_LOAD(&info[0], PMIX_APP_INFO, NULL, PMIX_BOOL);
    for (n = 0; n < num_apps; n++) {
        PMIX_INFO_LOAD(&info[1], PMIX_APPNUM, &n, PMIX_UINT32);
        PMIX_VALUE_CONSTRUCT(&val);
        rc = fetch_value(&undef, key, info, 2, &val);
        PMIX_INFO_DESTRUCT(&info[1]);
        if (PMIX_SUCCESS == rc) {
            rc = PMIx_Value_get_number(&val, &u32, PMIX_UINT32);
        }
        PMIX_VALUE_DESTRUCT(&val);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_INFO_DESTRUCT(&info[0]);
            PMIx_Argv_free(tmp);
            return rc;
        }
        if (0 > pmix_asprintf(&ev1, "%u", u32)) {
            PMIX_INFO_DESTRUCT(&info[0]);
            PMIx_Argv_free(tmp);
            return PMIX_ERR_NOMEM;
        }
        PMIx_Argv_append_nosize(&tmp, ev1);
        free(ev1);
    }
    PMIX_INFO_DESTRUCT(&info[0]);

    if (NULL == tmp) {
        return PMIX_SUCCESS;
    }
    ev1 = PMIx_Argv_join(tmp, ' ');
    PMIx_Argv_free(tmp);
    if (NULL == ev1) {
        return PMIX_ERR_NOMEM;
    }
    rc = PMIx_Setenv(var, ev1, true, env);
    free(ev1);
    return rc;
}

static pmix_status_t setup_fork(const pmix_proc_t *proc, char ***env, char ***priors)
{
    pmdl_nspace_t *ns, *ns2;
    char *ev1, **tmp;
    pmix_proc_t undef;
    pmix_value_t val;
    pmix_status_t rc;
    pmix_info_t qual[2];
    uint32_t appnum, n;
    uint16_t u16;
    pmix_mca_base_var_file_value_t *fv;

    pmix_output_verbose(2, pmix_pmdl_base_framework.framework_output,
                        "pmdl:ompi: setup fork for %s", PMIX_NAME_PRINT(proc));

    /* don't do OMPI again if already done */
    if (NULL != *priors) {
        char **t2 = *priors;
        for (n = 0; NULL != t2[n]; n++) {
            if (0 == strcmp(t2[n], "ompi")) {
                return PMIX_ERR_TAKE_NEXT_OPTION;
            }
        }
    }
    /* flag that we worked on this */
    PMIx_Argv_append_nosize(priors, "ompi");

    /* see if we already have this nspace */
    ns = NULL;
    PMIX_LIST_FOREACH (ns2, &mynspaces, pmdl_nspace_t) {
        if (PMIX_CHECK_NSPACE(ns2->nspace, proc->nspace)) {
            ns = ns2;
            break;
        }
    }
    if (NULL == ns) {
        /* we don't know anything about this one or
         * it doesn't have any ompi-based apps */
        return PMIX_ERR_TAKE_NEXT_OPTION;
    }

    /* the sizes come from register_nspace, and a host is not obliged to
     * have given us every one of them.  UINT32_MAX is this module's "not
     * known", so pass on what we have rather than telling the child its
     * universe holds four billion procs */

    /* pass universe size */
    if (UINT32_MAX != ns->univ_size) {
        rc = setenv_number("OMPI_UNIVERSE_SIZE", ns->univ_size, env);
        if (PMIX_SUCCESS != rc) {
            return rc;
        }
    }

    /* pass the comm_world size in various formats */
    if (UINT32_MAX != ns->job_size) {
        if (PMIX_SUCCESS != (rc = setenv_number("OMPI_COMM_WORLD_SIZE", ns->job_size, env)) ||
            PMIX_SUCCESS != (rc = setenv_number("OMPI_WORLD_SIZE", ns->job_size, env)) ||
            PMIX_SUCCESS != (rc = setenv_number("OMPI_MCA_num_procs", ns->job_size, env))) {
            return rc;
        }
    }

    /* pass the local size in various formats */
    if (UINT32_MAX != ns->local_size) {
        if (PMIX_SUCCESS != (rc = setenv_number("OMPI_COMM_WORLD_LOCAL_SIZE", ns->local_size, env)) ||
            PMIX_SUCCESS != (rc = setenv_number("OMPI_WORLD_LOCAL_SIZE", ns->local_size, env))) {
            return rc;
        }
    }

    /* pass the number of apps in the job */
    if (UINT32_MAX != ns->num_apps) {
        rc = setenv_number("OMPI_NUM_APP_CTX", ns->num_apps, env);
        if (PMIX_SUCCESS != rc) {
            return rc;
        }
    }

    /* pass an envar so the proc can find any files it had prepositioned */
    rc = setenv_string(proc, PMIX_PROCDIR, NULL, 0, "OMPI_FILE_LOCATION", env);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    /* the cwd and the command are app-level values, so they have to be
     * fetched for the app *this* proc belongs to.  pmix_globals.appnum is
     * the appnum of whoever is running this library - for a server, its
     * own - so using it here handed every proc in an MPMD job the first
     * app's directory and command line */
    PMIX_VALUE_CONSTRUCT(&val);
    rc = fetch_value(proc, PMIX_APPNUM, NULL, 0, &val);
    if (PMIX_SUCCESS == rc) {
        rc = PMIx_Value_get_number(&val, &appnum, PMIX_UINT32);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_VALUE_DESTRUCT(&val);
            return rc;
        }
    } else {
        /* a host that never said is telling us there is only one app -
         * the same assumption gds/hash makes when registering a proc */
        appnum = 0;
    }
    PMIX_VALUE_DESTRUCT(&val);

    PMIX_LOAD_PROCID(&undef, proc->nspace, PMIX_RANK_UNDEF);
    PMIX_INFO_LOAD(&qual[0], PMIX_APP_INFO, NULL, PMIX_BOOL);
    PMIX_INFO_LOAD(&qual[1], PMIX_APPNUM, &appnum, PMIX_UINT32);

    /* pass the cwd */
    rc = setenv_string(&undef, PMIX_WDIR, qual, 2, "OMPI_MCA_initial_wdir", env);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto quals;
    }

    /* pass its command. */
    PMIX_VALUE_CONSTRUCT(&val);
    rc = fetch_value(&undef, PMIX_APP_ARGV, qual, 2, &val);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_VALUE_DESTRUCT(&val);
        goto quals;
    }
    if (PMIX_STRING != val.type || NULL == val.data.string) {
        PMIX_ERROR_LOG(PMIX_ERR_TYPE_MISMATCH);
        PMIX_VALUE_DESTRUCT(&val);
        rc = PMIX_ERR_TYPE_MISMATCH;
        goto quals;
    }
    /* an argv with nothing in it splits to no argv at all, so there is
     * no tmp[0] to read - and no command to report either */
    tmp = PMIx_Argv_split(val.data.string, ' ');
    PMIX_VALUE_DESTRUCT(&val);
    if (NULL != tmp) {
        rc = PMIx_Setenv("OMPI_COMMAND", tmp[0], true, env);
        if (PMIX_SUCCESS == rc) {
            ev1 = PMIx_Argv_join(&tmp[1], ' ');
            if (NULL == ev1) {
                rc = PMIX_ERR_NOMEM;
            } else {
                rc = PMIx_Setenv("OMPI_ARGV", ev1, true, env);
                free(ev1);
            }
        }
        PMIx_Argv_free(tmp);
        if (PMIX_SUCCESS != rc) {
            goto quals;
        }
    }

quals:
    PMIX_INFO_DESTRUCT(&qual[0]);
    PMIX_INFO_DESTRUCT(&qual[1]);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }

    /* pass the arch - if available */
#ifdef HAVE_SYS_UTSNAME_H
    struct utsname sysname;
    memset(&sysname, 0, sizeof(sysname));
    if (-1 < uname(&sysname)) {
        if (sysname.machine[0] != '\0') {
            rc = PMIx_Setenv("OMPI_MCA_cpu_type", (const char *) &sysname.machine, true, env);
            if (PMIX_SUCCESS != rc) {
                return rc;
            }
        }
    }
#endif

    /* pass the rank */
    rc = setenv_number("OMPI_COMM_WORLD_RANK", (unsigned long) proc->rank, env);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }

    /* get the proc's local rank */
    PMIX_VALUE_CONSTRUCT(&val);
    rc = fetch_value(proc, PMIX_LOCAL_RANK, NULL, 0, &val);
    if (PMIX_SUCCESS == rc) {
        rc = PMIx_Value_get_number(&val, &u16, PMIX_UINT16);
    }
    PMIX_VALUE_DESTRUCT(&val);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    rc = setenv_number("OMPI_COMM_WORLD_LOCAL_RANK", (unsigned long) u16, env);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }

    /* get the proc's node rank */
    PMIX_VALUE_CONSTRUCT(&val);
    rc = fetch_value(proc, PMIX_NODE_RANK, NULL, 0, &val);
    if (PMIX_SUCCESS == rc) {
        rc = PMIx_Value_get_number(&val, &u16, PMIX_UINT16);
    }
    PMIX_VALUE_DESTRUCT(&val);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    rc = setenv_number("OMPI_COMM_WORLD_NODE_RANK", (unsigned long) u16, env);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }

    /* the per-app breakdown only means anything to an MPMD job */
    if (UINT32_MAX != ns->num_apps && 1 < ns->num_apps) {
        rc = setenv_per_app(proc, ns->num_apps, PMIX_APP_SIZE, "OMPI_APP_CTX_NUM_PROCS", env);
        if (PMIX_SUCCESS != rc) {
            return rc;
        }
        rc = setenv_per_app(proc, ns->num_apps, PMIX_APPLDR, "OMPI_FIRST_RANKS", env);
        if (PMIX_SUCCESS != rc) {
            return rc;
        }
    }

    /* provide the reincarnation number.  Nothing in PMIx sets this - it
     * is the host's to provide, and a host that has never restarted this
     * proc has nothing to say.  Absent means zero, not a failed launch:
     * this used to return the fetch error, which fails
     * PMIx_server_setup_fork for the whole child */
    PMIX_VALUE_CONSTRUCT(&val);
    rc = fetch_value(proc, PMIX_REINCARNATION, NULL, 0, &val);
    if (PMIX_SUCCESS == rc) {
        rc = PMIx_Value_get_number(&val, &n, PMIX_UINT32);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_VALUE_DESTRUCT(&val);
            return rc;
        }
        PMIX_VALUE_DESTRUCT(&val);
        rc = setenv_number("OMPI_MCA_num_restarts", n, env);
        if (PMIX_SUCCESS != rc) {
            return rc;
        }
    } else {
        PMIX_VALUE_DESTRUCT(&val);
    }

    /* add any envars we collected from param files.  This, and the
     * restart count above, used to sit behind an early return taken by
     * every single-app job - which is to say by almost every job */
    PMIX_LIST_FOREACH(fv, &myenvars, pmix_mca_base_var_file_value_t) {
        rc = PMIx_Setenv(fv->mbvfv_var, fv->mbvfv_value, true, env);
        if (PMIX_SUCCESS != rc) {
            return rc;
        }
    }

    return PMIX_SUCCESS;
}

static void deregister_nspace(pmix_namespace_t *nptr)
{
    pmdl_nspace_t *ns;

    /* find our tracker for this nspace */
    PMIX_LIST_FOREACH (ns, &mynspaces, pmdl_nspace_t) {
        if (PMIX_CHECK_NSPACE(ns->nspace, nptr->nspace)) {
            pmix_list_remove_item(&mynspaces, &ns->super);
            PMIX_RELEASE(ns);
            return;
        }
    }
}
