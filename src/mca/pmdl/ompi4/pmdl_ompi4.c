/*
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2016      IBM Corporation.  All rights reserved.
 *
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "src/include/pmix_config.h"

#include <string.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif
#ifdef HAVE_SYS_UTSNAME_H
#include <sys/utsname.h>
#endif
#include <time.h>

#include "include/pmix.h"

#include "src/mca/base/pmix_mca_base_var.h"
#include "src/mca/base/pmix_mca_base_vari.h"
#include "src/include/pmix_socket_errno.h"
#include "src/include/pmix_globals.h"
#include "src/class/pmix_list.h"
#include "src/class/pmix_pointer_array.h"
#include "src/util/alfg.h"
#include "src/util/argv.h"
#include "src/util/error.h"
#include "src/util/name_fns.h"
#include "src/util/output.h"
#include "src/util/os_path.h"
#include "src/util/printf.h"
#include "src/util/pmix_environ.h"
#include "src/mca/preg/preg.h"

#include "src/mca/pmdl/pmdl.h"
#include "src/mca/pmdl/base/base.h"
#include "pmdl_ompi4.h"

static pmix_status_t ompi4_init(void);
static void ompi4_finalize(void);
static pmix_status_t harvest_envars(pmix_namespace_t *nptr,
                                    const pmix_info_t info[], size_t ninfo,
                                    pmix_list_t *ilist,
                                    char ***priors);
static pmix_status_t setup_nspace(pmix_namespace_t *nptr,
                                  pmix_info_t *info);
static pmix_status_t setup_nspace_kv(pmix_namespace_t *nptr,
                                     pmix_kval_t *kv);
static pmix_status_t register_nspace(pmix_namespace_t *nptr);
static pmix_status_t setup_fork(const pmix_proc_t *proc,
                                char ***env,
                                char ***priors);
static void deregister_nspace(pmix_namespace_t *nptr);
pmix_pmdl_module_t pmix_pmdl_ompi4_module = {
    .name = "ompi4",
    .init = ompi4_init,
    .finalize = ompi4_finalize,
    .harvest_envars = harvest_envars,
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
	bool datacollected;
	uint32_t univ_size;
	uint32_t job_size;
	uint32_t local_size;
	uint32_t num_apps;
} pmdl_nspace_t;
static void nscon(pmdl_nspace_t *p)
{
	p->datacollected = false;
	p->univ_size = 0;
	p->job_size = 0;
	p->local_size = 0;
	p->num_apps = 0;
}
static PMIX_CLASS_INSTANCE(pmdl_nspace_t,
						   pmix_list_item_t,
						   nscon, NULL);

/* internal variables */
static pmix_list_t mynspaces;

static pmix_status_t ompi4_init(void)
{
    pmix_output_verbose(2, pmix_pmdl_base_framework.framework_output,
                        "pmdl: ompi4 init");

    PMIX_CONSTRUCT(&mynspaces, pmix_list_t);

    return PMIX_SUCCESS;
}

static void ompi4_finalize(void)
{
    PMIX_LIST_DESTRUCT(&mynspaces);
}

static bool checkus(const pmix_info_t info[], size_t ninfo)
{
    bool takeus = false;
    char **tmp, *ptr;
    size_t n, m;
    uint vers;

    if (NULL == info) {
        return false;
    }

    /* check the directives */
    for (n=0; n < ninfo && !takeus; n++) {
        /* check the attribute */
        if (PMIX_CHECK_KEY(&info[n], PMIX_PROGRAMMING_MODEL) ||
            PMIX_CHECK_KEY(&info[n], PMIX_PERSONALITY)) {
            tmp = pmix_argv_split(info[n].value.data.string, ',');
            for (m=0; NULL != tmp[m]; m++) {
                if (0 == strcmp(tmp[m], "ompi")) {
                    /* they didn't specify a level, so we will service
                     * them just in case */
                    takeus = true;
                    break;
                }
                if (0 == strncmp(tmp[m], "ompi", 4)) {
                    /* if they specifically requested an ompi level less
                     * than or equal to us, then we service it */
                    ptr = &tmp[m][4];
                    vers = strtoul(ptr, NULL, 10);
                    if (4 >= vers) {
                        takeus = true;
                    }
                    break;
                }
            }
            pmix_argv_free(tmp);
        }
    }

    return takeus;
}

static pmix_status_t harvest_envars(pmix_namespace_t *nptr,
                                    const pmix_info_t info[], size_t ninfo,
                                    pmix_list_t *ilist,
                                    char ***priors)
{
    pmdl_nspace_t *ns, *ns2;
    pmix_status_t rc;
    size_t n;
    uint32_t uid = UINT32_MAX;
    const char *home;
    pmix_list_t params;
    pmix_mca_base_var_file_value_t *fv;
    pmix_kval_t *kv;
    char *file, *tmp;

    pmix_output_verbose(2, pmix_pmdl_base_framework.framework_output,
                        "pmdl:ompi4:harvest envars");

    if (!checkus(info, ninfo)) {
        return PMIX_ERR_TAKE_NEXT_OPTION;
    }

    /* don't do OMPI again if already done - ompi5 has higher priority */
    if (NULL != *priors) {
        char **t2 = *priors;
        for (n=0; NULL != t2[n]; n++) {
            if (0 == strncmp(t2[n], "ompi", 4)) {
                return PMIX_ERR_TAKE_NEXT_OPTION;
            }
        }
    }
    /* flag that we worked on this */
    pmix_argv_append_nosize(priors, "ompi4");

    if (NULL != nptr) {
        /* see if we already have this nspace */
        ns = NULL;
        PMIX_LIST_FOREACH(ns2, &mynspaces, pmdl_nspace_t) {
            if (PMIX_CHECK_NSPACE(ns2->nspace, nptr->nspace)) {
                ns = ns2;
                break;
            }
        }
        if (NULL == ns) {
            ns = PMIX_NEW(pmdl_nspace_t);
            PMIX_LOAD_NSPACE(ns->nspace, nptr->nspace);
            pmix_list_append(&mynspaces, &ns->super);
        }
    }

    /* see if the user has a default MCA param file */
    if (NULL != info) {
        for (n=0; n < ninfo; n++) {
            if (PMIX_CHECK_KEY(&info[n], PMIX_USERID)) {
                PMIX_VALUE_GET_NUMBER(rc, &info[n].value, uid, uint32_t);
                if (PMIX_SUCCESS != rc) {
                    return rc;
                }
                break;
            }
        }
    }
    if (UINT32_MAX == uid) {
        uid = geteuid();
    }
    /* try to get their home directory */
    home = pmix_home_directory(uid);
    if (NULL != home) {
        file = pmix_os_path(false, home, ".openmpi", "mca-params.conf", NULL);
        PMIX_CONSTRUCT(&params, pmix_list_t);
        pmix_mca_base_parse_paramfile(file, &params);
        free(file);
        PMIX_LIST_FOREACH(fv, &params, pmix_mca_base_var_file_value_t) {
            /* need to prefix the param name */
            kv = PMIX_NEW(pmix_kval_t);
            if (NULL == kv) {
                PMIX_LIST_DESTRUCT(&params);
                return PMIX_ERR_OUT_OF_RESOURCE;
            }
            kv->key = strdup(PMIX_SET_ENVAR);
            kv->value = (pmix_value_t*)malloc(sizeof(pmix_value_t));
            if (NULL == kv->value) {
                PMIX_RELEASE(kv);
                PMIX_LIST_DESTRUCT(&params);
                return PMIX_ERR_OUT_OF_RESOURCE;
            }
            kv->value->type = PMIX_ENVAR;
            pmix_asprintf(&tmp, "OMPI_MCA_%s", fv->mbvfv_var);
            PMIX_ENVAR_LOAD(&kv->value->data.envar, tmp, fv->mbvfv_value, ':');
            free(tmp);
            pmix_list_append(ilist, &kv->super);
        }
        PMIX_LIST_DESTRUCT(&params);
        /* add an envar indicating that we did this so the OMPI
         * processes won't duplicate it */
        kv = PMIX_NEW(pmix_kval_t);
        if (NULL == kv) {
            return PMIX_ERR_OUT_OF_RESOURCE;
        }
        kv->key = strdup(PMIX_SET_ENVAR);
        kv->value = (pmix_value_t*)malloc(sizeof(pmix_value_t));
        if (NULL == kv->value) {
            PMIX_RELEASE(kv);
            return PMIX_ERR_OUT_OF_RESOURCE;
        }
        kv->value->type = PMIX_ENVAR;
        PMIX_ENVAR_LOAD(&kv->value->data.envar, "OPAL_USER_PARAMS_GIVEN", "1", ':');
        pmix_list_append(ilist, &kv->super);
    }

    /* harvest our local envars */
    if (NULL != mca_pmdl_ompi4_component.include) {
        pmix_output_verbose(2, pmix_pmdl_base_framework.framework_output,
                            "pmdl: ompi4 harvesting envars %s excluding %s",
                            (NULL == mca_pmdl_ompi4_component.incparms) ? "NONE" : mca_pmdl_ompi4_component.incparms,
                            (NULL == mca_pmdl_ompi4_component.excparms) ? "NONE" : mca_pmdl_ompi4_component.excparms);
        rc = pmix_util_harvest_envars(mca_pmdl_ompi4_component.include,
                                      mca_pmdl_ompi4_component.exclude,
                                      ilist);
        if (PMIX_SUCCESS != rc) {
            return rc;
        }
    }

    return PMIX_SUCCESS;
}


static pmix_status_t setup_nspace(pmix_namespace_t *nptr,
                                  pmix_info_t *info)
{
	pmdl_nspace_t *ns, *ns2;

    pmix_output_verbose(2, pmix_pmdl_base_framework.framework_output,
                        "pmdl:ompi4: setup nspace for nspace %s with %s",
                        nptr->nspace, info->value.data.string);

    if (!checkus(info, 1)) {
        return PMIX_ERR_TAKE_NEXT_OPTION;
    }

	/* see if we already have this nspace */
	ns = NULL;
	PMIX_LIST_FOREACH(ns2, &mynspaces, pmdl_nspace_t) {
		if (PMIX_CHECK_NSPACE(ns2->nspace, nptr->nspace)) {
			ns = ns2;
			break;
		}
	}
	if (NULL == ns) {
		ns = PMIX_NEW(pmdl_nspace_t);
		PMIX_LOAD_NSPACE(ns->nspace, nptr->nspace);
		pmix_list_append(&mynspaces, &ns->super);
	}

	return PMIX_SUCCESS;
}

static pmix_status_t setup_nspace_kv(pmix_namespace_t *nptr,
                                     pmix_kval_t *kv)
{
	pmdl_nspace_t *ns, *ns2;
    char **tmp, *ptr;
    size_t m;
    uint vers;
    bool takeus = false;

    pmix_output_verbose(2, pmix_pmdl_base_framework.framework_output,
                        "pmdl:ompi4: setup nspace_kv for nspace %s with %s",
                        nptr->nspace, kv->value->data.string);

    /* check the attribute */
    if (PMIX_CHECK_KEY(kv, PMIX_PROGRAMMING_MODEL) ||
        PMIX_CHECK_KEY(kv, PMIX_PERSONALITY)) {
        tmp = pmix_argv_split(kv->value->data.string, ',');
        for (m=0; NULL != tmp[m]; m++) {
            if (0 == strcmp(tmp[m], "ompi")) {
                /* they didn't specify a level, so we will service
                 * them just in case */
                takeus = true;
                break;
            }
            if (0 == strncmp(tmp[m], "ompi", 4)) {
                /* if they specifically requested an ompi level less
                 * than or equal to us, then we service it */
                ptr = &tmp[m][4];
                vers = strtoul(ptr, NULL, 10);
                if (4 >= vers) {
                    takeus = true;
                }
                break;
            }
        }
        pmix_argv_free(tmp);
    }
    if (!takeus) {
        return PMIX_ERR_TAKE_NEXT_OPTION;
    }

	/* see if we already have this nspace */
	ns = NULL;
	PMIX_LIST_FOREACH(ns2, &mynspaces, pmdl_nspace_t) {
		if (PMIX_CHECK_NSPACE(ns2->nspace, nptr->nspace)) {
			ns = ns2;
			break;
		}
	}
	if (NULL == ns) {
		ns = PMIX_NEW(pmdl_nspace_t);
		PMIX_LOAD_NSPACE(ns->nspace, nptr->nspace);
		pmix_list_append(&mynspaces, &ns->super);
	}

	return PMIX_SUCCESS;
}

static pmix_status_t register_nspace(pmix_namespace_t *nptr)
{
    pmdl_nspace_t *ns, *ns2;

    pmix_output_verbose(2, pmix_pmdl_base_framework.framework_output,
                        "pmdl:ompi4: register_nspace for %s", nptr->nspace);

    /* see if we already have this nspace */
    ns = NULL;
    PMIX_LIST_FOREACH(ns2, &mynspaces, pmdl_nspace_t) {
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

    /* just track the number of local procs so we don't have to
     * retrieve it later */
    ns->local_size = nptr->nlocalprocs;

    return PMIX_SUCCESS;
}

static pmix_status_t setup_fork(const pmix_proc_t *proc,
                                char ***env,
                                char ***priors)
{
	pmdl_nspace_t *ns, *ns2;
	char *param;
    char *ev1, **tmp;
	pmix_proc_t wildcard, undef;
	pmix_status_t rc;
	uint16_t u16;
    pmix_kval_t *kv;
    pmix_info_t info[2];
    uint32_t n;
    pmix_cb_t cb;

    pmix_output_verbose(2, pmix_pmdl_base_framework.framework_output,
                        "pmdl:ompi4: setup fork for %s", PMIX_NAME_PRINT(proc));

    /* don't do OMPI again if already done */
    if (NULL != *priors) {
        char **t2 = *priors;
        for (n=0; NULL != t2[n]; n++) {
            if (0 == strncmp(t2[n], "ompi", 4)) {
                return PMIX_ERR_TAKE_NEXT_OPTION;
            }
        }
    }
    /* flag that we worked on this */
    pmix_argv_append_nosize(priors, "ompi4");

	/* see if we already have this nspace */
	ns = NULL;
	PMIX_LIST_FOREACH(ns2, &mynspaces, pmdl_nspace_t) {
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

    PMIX_LOAD_PROCID(&wildcard, proc->nspace, PMIX_RANK_WILDCARD);
    PMIX_LOAD_PROCID(&undef, proc->nspace, PMIX_RANK_UNDEF);

	/* do we already have the data we need here? */
	if (!ns->datacollected) {

		/* fetch the universe size */
        PMIX_CONSTRUCT(&cb, pmix_cb_t);
        cb.proc = &wildcard;
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
        kv = (pmix_kval_t*)pmix_list_get_first(&cb.kvs);
        ns->univ_size = kv->value->data.uint32;
        PMIX_DESTRUCT(&cb);

		/* fetch the job size */
        PMIX_CONSTRUCT(&cb, pmix_cb_t);
        cb.proc = &wildcard;
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
        kv = (pmix_kval_t*)pmix_list_get_first(&cb.kvs);
        ns->job_size = kv->value->data.uint32;
        PMIX_DESTRUCT(&cb);

		/* fetch the number of apps */
        PMIX_CONSTRUCT(&cb, pmix_cb_t);
        cb.proc = &wildcard;
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
        kv = (pmix_kval_t*)pmix_list_get_first(&cb.kvs);
        ns->num_apps = kv->value->data.uint32;
        PMIX_DESTRUCT(&cb);

        ns->datacollected = true;
    }

    /* pass universe size */
    if (0 > asprintf(&param, "%u", ns->univ_size)) {
    	return PMIX_ERR_NOMEM;
    }
    pmix_setenv("OMPI_UNIVERSE_SIZE", param, true, env);
    free(param);

    /* pass the comm_world size in various formats */
    if (0 > asprintf(&param, "%u", ns->job_size)) {
    	return PMIX_ERR_NOMEM;
    }
    pmix_setenv("OMPI_COMM_WORLD_SIZE", param, true, env);
    pmix_setenv("OMPI_WORLD_SIZE", param, true, env);
    pmix_setenv("OMPI_MCA_num_procs", param, true, env);
    free(param);

    /* pass the local size in various formats */
    if (0 > asprintf(&param, "%u", ns->local_size)) {
    	return PMIX_ERR_NOMEM;
    }
    pmix_setenv("OMPI_COMM_WORLD_LOCAL_SIZE", param, true, env);
    pmix_setenv("OMPI_WORLD_LOCAL_SIZE", param, true, env);
    free(param);

    /* pass the number of apps in the job */
    if (0 > asprintf(&param, "%u", ns->num_apps)) {
        return PMIX_ERR_NOMEM;
    }
    pmix_setenv("OMPI_NUM_APP_CTX", param, true, env);
    free(param);

    /* pass an envar so the proc can find any files it had prepositioned */
    PMIX_CONSTRUCT(&cb, pmix_cb_t);
    cb.proc = (pmix_proc_t*)proc;
    cb.key = PMIX_PROCDIR;
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
    kv = (pmix_kval_t*)pmix_list_get_first(&cb.kvs);
    pmix_setenv("OMPI_FILE_LOCATION", kv->value->data.string, true, env);
    PMIX_DESTRUCT(&cb);

    /* pass the cwd */
    PMIX_INFO_LOAD(&info[0], PMIX_APP_INFO, NULL, PMIX_BOOL);
    PMIX_CONSTRUCT(&cb, pmix_cb_t);
    cb.proc = &undef;
    cb.info = info;
    cb.ninfo = 2;
    cb.key = PMIX_WDIR;
    PMIX_INFO_LOAD(&info[1], PMIX_APPNUM, &pmix_globals.appnum, PMIX_UINT32);
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
    kv = (pmix_kval_t*)pmix_list_get_first(&cb.kvs);
    pmix_setenv("OMPI_MCA_initial_wdir", kv->value->data.string, true, env);
    PMIX_DESTRUCT(&cb);
    PMIX_INFO_DESTRUCT(&info[0]);

    /* pass its command. */
    PMIX_INFO_LOAD(&info[0], PMIX_APP_INFO, NULL, PMIX_BOOL);
    PMIX_CONSTRUCT(&cb, pmix_cb_t);
    cb.proc = &undef;
    cb.info = info;
    cb.ninfo = 2;
    cb.key = PMIX_APP_ARGV;
    PMIX_INFO_LOAD(&info[1], PMIX_APPNUM, &pmix_globals.appnum, PMIX_UINT32);
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
    kv = (pmix_kval_t*)pmix_list_get_first(&cb.kvs);
    tmp = pmix_argv_split(kv->value->data.string, ' ');
    PMIX_DESTRUCT(&cb);
    PMIX_INFO_DESTRUCT(&info[0]);
    pmix_setenv("OMPI_COMMAND", tmp[0], true, env);
    ev1 = pmix_argv_join(&tmp[1], ' ');
    pmix_setenv("OMPI_ARGV", ev1, true, env);
    free(ev1);
    pmix_argv_free(tmp);

    /* pass the arch - if available */
#ifdef HAVE_SYS_UTSNAME_H
    struct utsname sysname;
    memset(&sysname, 0, sizeof(sysname));
    if (-1 < uname(&sysname)) {
        if (sysname.machine[0] != '\0') {
            pmix_setenv("OMPI_MCA_cpu_type", (const char *) &sysname.machine, true, env);
        }
    }
#endif

    /* pass the rank */
    if (0 > asprintf(&param, "%lu", (unsigned long)proc->rank)) {
    	return PMIX_ERR_NOMEM;
    }
    pmix_setenv("OMPI_COMM_WORLD_RANK", param, true, env);
    free(param);  /* done with this now */

    /* get the proc's local rank */
    PMIX_CONSTRUCT(&cb, pmix_cb_t);
    cb.proc = (pmix_proc_t*)proc;
    cb.key = PMIX_LOCAL_RANK;
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
    kv = (pmix_kval_t*)pmix_list_get_first(&cb.kvs);
    u16 = kv->value->data.uint16;
    PMIX_DESTRUCT(&cb);
    if (0 > asprintf(&param, "%lu", (unsigned long)u16)) {
        return PMIX_ERR_NOMEM;
    }
    pmix_setenv("OMPI_COMM_WORLD_LOCAL_RANK", param, true, env);
    free(param);

    /* get the proc's node rank */
    PMIX_CONSTRUCT(&cb, pmix_cb_t);
    cb.proc = (pmix_proc_t*)proc;
    cb.key = PMIX_NODE_RANK;
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
    kv = (pmix_kval_t*)pmix_list_get_first(&cb.kvs);
    u16 = kv->value->data.uint16;
    PMIX_DESTRUCT(&cb);
    if (0 > asprintf(&param, "%lu", (unsigned long)u16)) {
        return PMIX_ERR_NOMEM;
    }
    pmix_setenv("OMPI_COMM_WORLD_NODE_RANK", param, true, env);
    free(param);

    if (1 == ns->num_apps) {
        return PMIX_SUCCESS;
    }

    PMIX_LOAD_PROCID(&undef, proc->nspace, PMIX_RANK_UNDEF);
    PMIX_INFO_LOAD(&info[0], PMIX_APP_INFO, NULL, PMIX_BOOL);
    tmp = NULL;
    for (n=0; n < ns->num_apps; n++) {
        PMIX_CONSTRUCT(&cb, pmix_cb_t);
        cb.proc = &undef;
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
        kv = (pmix_kval_t*)pmix_list_get_first(&cb.kvs);
        pmix_asprintf(&ev1, "%u", kv->value->data.uint32);
        pmix_argv_append_nosize(&tmp, ev1);
        free(ev1);
        PMIX_DESTRUCT(&cb);
    }
    PMIX_INFO_DESTRUCT(&info[0]);

    if (NULL != tmp) {
        ev1 = pmix_argv_join(tmp, ' ');
        pmix_argv_free(tmp);
        pmix_setenv("OMPI_APP_CTX_NUM_PROCS", ev1, true, env);
        free(ev1);
    }


    PMIX_INFO_LOAD(&info[0], PMIX_APP_INFO, NULL, PMIX_BOOL);
    tmp = NULL;
    for (n=0; n < ns->num_apps; n++) {
        PMIX_CONSTRUCT(&cb, pmix_cb_t);
        cb.proc = &undef;
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
        kv = (pmix_kval_t*)pmix_list_get_first(&cb.kvs);
        pmix_asprintf(&ev1, "%u", kv->value->data.uint32);
        pmix_argv_append_nosize(&tmp, ev1);
        free(ev1);
        PMIX_DESTRUCT(&cb);
    }
    PMIX_INFO_DESTRUCT(&info[0]);

    if (NULL != tmp) {
        ev1 = pmix_argv_join(tmp, ' ');
        pmix_argv_free(tmp);
        tmp = NULL;
        pmix_setenv("OMPI_FIRST_RANKS", ev1, true, env);
        free(ev1);
    }

    /* provide the reincarnation number */
    PMIX_CONSTRUCT(&cb, pmix_cb_t);
    cb.proc = (pmix_proc_t*)proc;
    cb.key = PMIX_REINCARNATION;
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
    kv = (pmix_kval_t*)pmix_list_get_first(&cb.kvs);
    pmix_asprintf(&ev1, "%u", kv->value->data.uint32);
    pmix_setenv("OMPI_MCA_num_restarts", ev1, true, env);
    free(ev1);
    PMIX_DESTRUCT(&cb);

    return PMIX_SUCCESS;
}

static void deregister_nspace(pmix_namespace_t *nptr)
{
	pmdl_nspace_t *ns;

	/* find our tracker for this nspace */
	PMIX_LIST_FOREACH(ns, &mynspaces, pmdl_nspace_t) {
		if (PMIX_CHECK_NSPACE(ns->nspace, nptr->nspace)) {
			pmix_list_remove_item(&mynspaces, &ns->super);
			PMIX_RELEASE(ns);
			return;
		}
	}
}
