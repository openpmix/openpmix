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
#include "pmdl_oshmem.h"

static pmix_status_t oshmem_init(void);
static void oshmem_finalize(void);
static pmix_status_t harvest_envars(pmix_namespace_t *nptr,
                                    const pmix_info_t info[], size_t ninfo,
                                    pmix_list_t *ilist,
                                    char ***priors);
static pmix_status_t setup_nspace(pmix_namespace_t *nptr,
                                  pmix_info_t *info);
static pmix_status_t setup_nspace_kv(pmix_namespace_t *nptr,
                                     pmix_kval_t *kv);
static pmix_status_t register_nspace(pmix_namespace_t *nptr);
static void deregister_nspace(pmix_namespace_t *nptr);
pmix_pmdl_module_t pmix_pmdl_oshmem_module = {
    .name = "oshmem",
    .init = oshmem_init,
    .finalize = oshmem_finalize,
    .harvest_envars = harvest_envars,
    .setup_nspace = setup_nspace,
    .setup_nspace_kv = setup_nspace_kv,
    .register_nspace = register_nspace,
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

static pmix_status_t oshmem_init(void)
{
    pmix_output_verbose(2, pmix_pmdl_base_framework.framework_output,
                        "pmdl: oshmem init");

    PMIX_CONSTRUCT(&mynspaces, pmix_list_t);

    return PMIX_SUCCESS;
}

static void oshmem_finalize(void)
{
    PMIX_LIST_DESTRUCT(&mynspaces);
}

static bool checkus(const pmix_info_t info[], size_t ninfo)
{
    bool takeus = false;
    char **tmp;
    size_t n, m;

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
                if (0 == strcmp(tmp[m], "oshmem")) {
                    takeus = true;
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

    pmix_output_verbose(2, pmix_pmdl_base_framework.framework_output,
                        "pmdl:oshmem:harvest envars");


    if (!checkus(info, ninfo)) {
        return PMIX_ERR_TAKE_NEXT_OPTION;
    }

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

    /* harvest our local envars */
    if (NULL != mca_pmdl_oshmem_component.include) {
        pmix_output_verbose(2, pmix_pmdl_base_framework.framework_output,
                            "pmdl: oshmem harvesting envars %s excluding %s",
                            (NULL == mca_pmdl_oshmem_component.incparms) ? "NONE" : mca_pmdl_oshmem_component.incparms,
                            (NULL == mca_pmdl_oshmem_component.excparms) ? "NONE" : mca_pmdl_oshmem_component.excparms);
        rc = pmix_util_harvest_envars(mca_pmdl_oshmem_component.include,
                                      mca_pmdl_oshmem_component.exclude,
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
                        "pmdl:oshmem: setup nspace for nspace %s with %s",
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
                        "pmdl:oshmem: setup nspace_kv for nspace %s with %s",
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
	char *ev1, **tmp;
	pmix_proc_t wildcard, undef;
	pmix_status_t rc;
    pmix_kval_t *kv;
    pmix_info_t info[2];
    uint32_t n;
    pmix_cb_t cb;

    pmix_output_verbose(2, pmix_pmdl_base_framework.framework_output,
                        "pmdl:oshmem: register_nspace for %s", nptr->nspace);

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

	/* do we already have the data we need here? */
	if (!ns->datacollected) {
        PMIX_LOAD_PROCID(&wildcard, nptr->nspace, PMIX_RANK_WILDCARD);
        PMIX_CONSTRUCT(&cb, pmix_cb_t);
        cb.proc = &wildcard;
        cb.key = PMIX_JOB_NUM_APPS;
		/* fetch the number of apps */
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

    if (1 == ns->num_apps) {
        return PMIX_SUCCESS;
    }

    /* construct the list of app sizes */
    PMIX_LOAD_PROCID(&undef, nptr->nspace, PMIX_RANK_UNDEF);
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
        kv = PMIX_NEW(pmix_kval_t);
        kv->key = strdup("OMPI_APP_SIZES");
        kv->value = (pmix_value_t*)malloc(sizeof(pmix_value_t));
        kv->value->type = PMIX_STRING;
        kv->value->data.string = pmix_argv_join(tmp, ' ');
        pmix_argv_free(tmp);
        PMIX_GDS_STORE_KV(rc, pmix_globals.mypeer, &wildcard, PMIX_GLOBAL, kv);
        PMIX_RELEASE(kv);
    }

    /* construct the list of app leaders */
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
        kv = PMIX_NEW(pmix_kval_t);
        kv->key = strdup("OMPI_FIRST_RANKS");
        kv->value = (pmix_value_t*)malloc(sizeof(pmix_value_t));
        kv->value->type = PMIX_STRING;
        kv->value->data.string = pmix_argv_join(tmp, ' ');
        pmix_argv_free(tmp);
        PMIX_GDS_STORE_KV(rc, pmix_globals.mypeer, &wildcard, PMIX_GLOBAL, kv);
        PMIX_RELEASE(kv);
    }

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
