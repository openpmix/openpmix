/*
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2016-2018 IBM Corporation.  All rights reserved.
 * Copyright (c) 2018      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2018-2020 Mellanox Technologies, Inc.
 *                         All rights reserved.
 *
 * Copyright (c) 2022      Nanook Consulting.  All rights reserved.
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

#include "include/pmix_common.h"

#include "src/include/pmix_globals.h"
#include "src/class/pmix_list.h"
#include "src/client/pmix_client_ops.h"
#include "src/server/pmix_server_ops.h"
#include "src/mca/pcompress/base/base.h"
#include "src/mca/pmdl/pmdl.h"
#include "src/mca/preg/preg.h"
#include "src/mca/ptl/base/base.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_name_fns.h"
#include "src/util/pmix_environ.h"
#include "src/util/pmix_shmem.h"

#include "src/mca/gds/base/base.h"
#include "gds_shmem.h"

static pmix_status_t shmem_init(pmix_info_t info[], size_t ninfo);
static void shmem_finalize(void);

static pmix_status_t shmem_assign_module(pmix_info_t *info, size_t ninfo,
                                        int *priority);

static pmix_status_t shmem_cache_job_info(struct pmix_namespace_t *ns, pmix_info_t info[],
                                          size_t ninfo);

static pmix_status_t shmem_register_job_info(struct pmix_peer_t *pr, pmix_buffer_t *reply);

static pmix_status_t shmem_store_job_info(const char *nspace, pmix_buffer_t *buf);

static pmix_status_t shmem_store(const pmix_proc_t *proc, pmix_scope_t scope,
                                 pmix_kval_t *kv);

static pmix_status_t shmem_store_modex(struct pmix_namespace_t *ns,
                                       pmix_buffer_t *buff,
                                       void *cbdata);

static pmix_status_t shmem_fetch(const pmix_proc_t *proc, pmix_scope_t scope, bool copy,
                                 const char *key, pmix_info_t qualifiers[], size_t nqual,
                                 pmix_list_t *kvs);

static pmix_status_t setup_fork(const pmix_proc_t *peer, char ***env);

static pmix_status_t nspace_add(const char *nspace, uint32_t nlocalprocs, pmix_info_t info[],
                                size_t ninfo);

static pmix_status_t nspace_del(const char *nspace);

static pmix_status_t assemb_kvs_req(const pmix_proc_t *proc, pmix_list_t *kvs, pmix_buffer_t *bo,
                                    void *cbdata);

static pmix_status_t accept_kvs_resp(pmix_buffer_t *buf);

pmix_gds_base_module_t pmix_shmem_module = {
    .name = "shmem",
    .is_tsafe = false,
    .init = shmem_init,
    .finalize = shmem_finalize,
    .assign_module = shmem_assign_module,
    .cache_job_info = shmem_cache_job_info,
    .register_job_info = shmem_register_job_info,
    .store_job_info = shmem_store_job_info,
    .store = shmem_store,
    .store_modex = shmem_store_modex,
    .fetch = shmem_fetch,
    .setup_fork = setup_fork,
    .add_nspace = nspace_add,
    .del_nspace = nspace_del,
    .assemb_kvs_req = assemb_kvs_req,
    .accept_kvs_resp = accept_kvs_resp
};

static pmix_status_t shmem_init(pmix_info_t info[], size_t ninfo)
{
    PMIX_HIDE_UNUSED_PARAMS(info, ninfo);

    pmix_output_verbose(2, pmix_gds_base_framework.framework_output,
                        "gds: shmem init");

    return PMIX_SUCCESS;
}

static void shmem_finalize(void)
{
    pmix_output_verbose(2, pmix_gds_base_framework.framework_output,
                        "gds: shmem finalize");

}

static pmix_status_t shmem_assign_module(pmix_info_t *info, size_t ninfo,
                                        int *priority)
{
    size_t n, m;
    char **options;
    bool specified = false;

    /* ONLY CLIENTS ENTER HERE */
    /* the incoming info always overrides anything in the environment
     * as it is set by the application itself */
    *priority = 0;
    if (NULL != info) {
        for (n=0; n < ninfo; n++) {
            if (0 == strncmp(info[n].key, PMIX_GDS_MODULE, PMIX_MAX_KEYLEN)) {
                specified = true;  // they specified who they want
                options = pmix_argv_split(info[n].value.data.string, ',');
                for (m=0; NULL != options[m]; m++) {
                    if (0 == strcmp(options[m], "shmem")) {
                        /* they specifically asked for us */
                        *priority = 100;
                        break;
                    }
                }
                pmix_argv_free(options);
                break;
            }
        }
    }
    /* if they didn't specify, or they specified us, then we are
     * a candidate for use */
    if (!specified || 100 == *priority) {
        /* look for the rendezvous envars - if they are found, then
         * we connect to that hole. Otherwise, we have to reject */
        *priority = 0;
    }

    return PMIX_SUCCESS;
}

static pmix_status_t shmem_cache_job_info(struct pmix_namespace_t *ns, pmix_info_t info[],
                                          size_t ninfo)
{
    PMIX_HIDE_UNUSED_PARAMS(ns, info, ninfo);

    /* ONLY SERVERS ENTER HERE */
    /* go and get a hole for this nspace, then record the
     * provided data in the shmem */
    return PMIX_ERR_NOT_SUPPORTED;

}

static pmix_status_t shmem_register_job_info(struct pmix_peer_t *pr, pmix_buffer_t *reply)
{
    PMIX_HIDE_UNUSED_PARAMS(pr, reply);

    /* ONLY SERVERS ENTER HERE */
    /* since the data is already in the shmem when we cached it, there
     * is nothing to do here */
    return PMIX_ERR_NOT_SUPPORTED;
}

static pmix_status_t shmem_store_job_info(const char *nspace, pmix_buffer_t *buf)
{
    PMIX_HIDE_UNUSED_PARAMS(nspace, buf);
    return PMIX_ERR_NOT_SUPPORTED;
}

static pmix_status_t shmem_store(const pmix_proc_t *proc,
                                pmix_scope_t scope,
                                pmix_kval_t *kv)
{
    PMIX_HIDE_UNUSED_PARAMS(proc, scope, kv);
    return PMIX_ERR_NOT_SUPPORTED;
}

static pmix_status_t shmem_store_modex(struct pmix_namespace_t *ns,
                                      pmix_buffer_t *buff,
                                      void *cbdata)
{
    PMIX_HIDE_UNUSED_PARAMS(ns, buff, cbdata);
    return PMIX_ERR_NOT_SUPPORTED;
}

static pmix_status_t shmem_fetch(const pmix_proc_t *proc, pmix_scope_t scope, bool copy,
                                 const char *key, pmix_info_t qualifiers[], size_t nqual,
                                 pmix_list_t *kvs)
{
    PMIX_HIDE_UNUSED_PARAMS(proc, scope, copy, key, qualifiers, nqual, kvs);
    return PMIX_ERR_NOT_SUPPORTED;
}


static pmix_status_t setup_fork(const pmix_proc_t *peer, char ***env)
{
    PMIX_HIDE_UNUSED_PARAMS(peer, env);

    /* add the hole rendezvous info for this proc's nspace
     * to the proc's environment */
    return PMIX_ERR_NOT_SUPPORTED;
}

static pmix_status_t nspace_add(const char *nspace, uint32_t nlocalprocs, pmix_info_t info[],
                                size_t ninfo)
{
    PMIX_HIDE_UNUSED_PARAMS(nspace, nlocalprocs, info, ninfo);
    return PMIX_ERR_NOT_SUPPORTED;
}

static pmix_status_t nspace_del(const char *nspace)
{
    PMIX_HIDE_UNUSED_PARAMS(nspace);
    return PMIX_ERR_NOT_SUPPORTED;
}

static pmix_status_t assemb_kvs_req(const pmix_proc_t *proc,
                                    pmix_list_t *kvs,
                                    pmix_buffer_t *bo,
                                    void *cbdata)
{
    PMIX_HIDE_UNUSED_PARAMS(proc, kvs, bo, cbdata);
    return PMIX_ERR_NOT_SUPPORTED;
}

static pmix_status_t accept_kvs_resp(pmix_buffer_t *buf)
{
    PMIX_HIDE_UNUSED_PARAMS(buf);
    return PMIX_ERR_NOT_SUPPORTED;
}
