/*
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2016      IBM Corporation.  All rights reserved.
 *
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
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
#ifdef HAVE_SYS_STAT_H
#    include <sys/stat.h>
#endif
#ifdef HAVE_FCNTL_H
#    include <fcntl.h>
#endif
#include <time.h>

#include "include/pmix_common.h"

#include "src/class/pmix_list.h"
#include "src/include/pmix_globals.h"
#include "src/include/pmix_socket_errno.h"
#include "src/mca/base/pmix_mca_base_var.h"
#include "src/mca/preg/preg.h"
#include "src/util/alfg.h"
#include "src/util/argv.h"
#include "src/util/error.h"
#include "src/util/output.h"
#include "src/util/pmix_environ.h"
#include "src/util/printf.h"

#include "pstrg_lustre.h"
#include "src/mca/pstrg/base/base.h"
#include "src/mca/pstrg/pstrg.h"

#include <lustre/lustreapi.h>

static pmix_status_t lustre_init(void);
static void lustre_finalize(void);
static pmix_status_t query(pmix_query_t queries[], size_t nqueries, pmix_list_t *results,
                           pmix_pstrg_query_cbfunc_t cbfunc, void *cbdata);

pmix_pstrg_base_module_t pmix_pstrg_lustre_module = {.name = "lustre",
                                                     .init = lustre_init,
                                                     .finalize = lustre_finalize,
                                                     .query = query};

static pmix_status_t lustre_init(void)
{
    pmix_output_verbose(2, pmix_pstrg_base_framework.framework_output, "pstrg: lustre init");

    /* ADD HERE:
     *
     * Discover/connect to any available Lustre systems. Return an error
     * if none are preset, or you are unable to connect to them
     */
    return PMIX_SUCCESS;
}

static void lustre_finalize(void)
{
    pmix_output_verbose(2, pmix_pstrg_base_framework.framework_output, "pstrg: lustre finalize");

    /* ADD HERE:
     *
     * Disconnect from any Lustre systems to which you have connected
     */
}

static pmix_status_t query(pmix_query_t queries[], size_t nqueries, pmix_list_t *results,
                           pmix_pstrg_query_cbfunc_t cbfunc, void *cbdata)
{

    pmix_output_verbose(2, pmix_pstrg_base_framework.framework_output, "pstrg: lustre query");

#if 0
    size_t n, m, k, i, j;
    char **sid, **mountpt;
    bool takeus;
    uid_t uid = UINT32_MAX;
    gid_t gid = UINT32_MAX;
    lustre_storage_t *q_sys;
    char *q_str;
    pmix_status_t q_ret;

    for (n=0; n < nqueries; n++) {
        /* did they specify a storage type for this query? */
        takeus = true;
        for (k = 0; k < queries[n].nqual; k++) {
            if (0 == strcmp(queries[n].qualifiers[k].key, PMIX_STORAGE_TYPE)) {

                /* NOTE: I only included "lustre" as an accepted type, but we might
                 * want to consider other types as well - e.g., "parallel", "persistent",...) */

                if (NULL == strcasestr(queries[n].qualifiers[k].value.data.string, "lustre")) {
                    /* they are not interested in us */
                    takeus = false;
                    ;
                }
                break;
            }
        }
        if (!takeus) {
            continue;
        }

        /* see if they want the list of storage systems - this doesn't take any
         * qualifiers */
        for (m=0; NULL != queries[n].keys[m]; m++) {
            if (0 == strcmp(queries[n].keys[m], PMIX_QUERY_STORAGE_LIST)) {
                /* ADD HERE:
                 *
                 * Obtain a list of all available Lustre storage systems. The IDs
                 * we return should be those that we want the user to provide when
                 * asking about a specific Lustre system. Please get the corresponding
                 * mount pts for each identifier so I can track them for further queries.
                 */

                /* I will package the data for return once I see what Lustre provides */
                continue;
            }

            for (m=0; NULL != queries[n].keys[m]; m++) {
                printf("%s:\n", queries[n].keys[m]);

                /* the remaining query keys all accept the storage ID and/or path qualifiers */
                sid = NULL;
                mountpt = NULL;
                for (k = 0; k < queries[n].nqual; k++) {
                    if (0 == strcmp(queries[n].qualifiers[k].key, PMIX_STORAGE_ID)) {
                        /* there may be more than one (comma-delimited) storage ID, so
                         * split them into a NULL-terminated argv-type array */
                        sid = pmix_argv_split(queries[n].qualifiers[k].value.data.string, ',');
                    } else if (0 == strcmp(queries[n].qualifiers[k].key, PMIX_STORAGE_PATH)) {
                        /* there may be more than one (comma-delimited) mount pt, so
                         * split them into a NULL-terminated argv-type array */
                        mountpt = pmix_argv_split(queries[n].qualifiers[k].value.data.string, ',');
                    } else if (0 == strcmp(queries[n].qualifiers[k].key, PMIX_USERID)) {
                        uid = queries[n].qualifiers[k].value.data.uint32;
                    } else if (0 == strcmp(queries[n].qualifiers[k].key, PMIX_GRPID)) {
                        gid = queries[n].qualifiers[k].value.data.uint32;
                    }
                }

                /* XXX: if no sid/mntpnt for queries below, do we return info on every storage system we know about? */

                for (i = 0, j = 0;;) {
                    if (sid && sid[i] != NULL) {
                        /* storage id based query */
                        k  = 0;
                        while (availsys[k].name != NULL) {
                            if (strcmp(availsys[k].name, sid[i]) == 0) {
                                q_sys = &availsys[k];
                                q_str = availsys[k].name;
                                break;
                            }
                            k++;
                        }
                        i++;
                    }
                    else if (mountpt && mountpt[j] != NULL) {
                        /* mount point based query */
                        k = 0;
                        while (availsys[k].name != NULL) {
                            if (strcmp(availsys[k].mountpt, mountpt[j]) == 0) {
                                q_sys = &availsys[k];
                                q_str = availsys[k].mountpt;
                                break;
                            }
                            k++;
                        }
                        j++;
                    }
                    else {
                        /* no more storage systems to query */
                        break;
                    }
                }

            if (0 == strcmp(queries[n].keys[m], PMIX_STORAGE_ID)) {
                /* don't worry about this one for now - once I see how to get the
                 * storage list and mount pts, I will construct the response here
                 *
                 * NOTE to self: could me a comma-delimited set of storage IDs, so
                 * return the mount pt for each of them
                 */
                char *strg_id = q_sys->name;
                printf("\t%s: %s\n", q_str, strg_id);

                /* I will package the data for return once I see what Lustre provides */
                continue;
            } else if (0 == strcmp(queries[n].keys[m], PMIX_STORAGE_PATH)) {
                /* don't worry about this one for now - once I see how to get the
                 * storage list and mount pts, I will construct the response here
                 *
                 * Get the capacity of the Lustre storage systems that are available to the
                 * caller's userid/grpid, as provided in the qualifiers.
                 */
                if (UINT32_MAX == uid || UINT32_MAX == gid) {
                    /* they failed to provide the user's info - I will
                     * take care of returning an appropriate error */
                }

                /*
                 * Let me know if you need further info to process the request - e.g.,
                 * we could provide the app's jobid or argv[0]. If they ask for
                 * a specific storage system, then get process that one.
                 * NOTE to self: could me a comma-delimited set of paths, so
                 * return the ID for each of them
                 */
                char *mountpt = q_sys->mountpt;
                printf("\t%s: %s\n", q_str, mountpt);

                /* I will package the data for return once I see what Lustre provides */
                continue;
            } else if (0 == strcmp(queries[n].keys[m], PMIX_STORAGE_CAPACITY_LIMIT)) {
                /* if given a uid/gid, answer this query using Lustre-specific code;
                 * otherwise, fallthrough to base VFS query code
                 */
                if (UINT32_MAX != uid || UINT32_MAX != gid) {
                    /* ADD HERE:
                     *
                     * Get the total capacity of the Lustre storage system for the given
                     * uid and gid. If they ask for a specific one(s), then get just those.
                     */
                    printf("\t%s: NOT SUPPORTED!\n", q_str);

                    /*
                     * Let me know if you need further info to process the request - e.g.,
                     * we could provide the app's jobid or argv[0]. If they ask for
                     * a specific storage system, then get process that one.
                     */

                    /* I will package the data for return once I see what Lustre provides */
                    continue;
                }

            } else if (0 == strcmp(queries[n].keys[m], PMIX_STORAGE_CAPACITY_USED)) {
                /* if given a uid/gid, answer this query using Lustre-specific code;
                 * otherwise, fallthrough to base VFS query code
                 */
                if (UINT32_MAX != uid || UINT32_MAX != gid) {
                    /* ADD HERE:
                     *
                     * Get the used capacity of the Lustre storage system for the given
                     * uid and gid. If they ask for a specific one(s), then get just those.
                     */
                    printf("\t%s: NOT SUPPORTED!\n", q_str);

                    /*
                     * Let me know if you need further info to process the request - e.g.,
                     * we could provide the app's jobid or argv[0]. If they ask for
                     * a specific storage system, then get process that one.
                     */

                    /* I will package the data for return once I see what Lustre provides */
                    continue;
                }

            } else if (0 == strcmp(queries[n].keys[m], PMIX_STORAGE_BW)) {
                /* ADD HERE:
                 *
                 * Get the overall bandwidth of the Lustre storage systems. If they ask for
                 * a specific one, then get just that one.
                 */
                if (UINT32_MAX == uid || UINT32_MAX == gid) {
                    /* they failed to provide the user's info - I will
                     * take care of returning an appropriate error */
                }

                /*
                 * Let me know if you need further info to process the request - e.g.,
                 * we could provide the app's jobid or argv[0]. If they ask for
                 * a specific storage system, then get process that one.
                 */

                /* I will package the data for return once I see what Lustre provides */
                continue;

            } else if (0 == strcmp(queries[n].keys[m], PMIX_STORAGE_AVAIL_BW)) {
                /* ADD HERE:
                 *
                 * Get the bandwidth of the Lustre storage systems that is available to the
                 * caller's userid/grpid.
                 */
                if (UINT32_MAX == uid || UINT32_MAX == gid) {
                    /* they failed to provide the user's info - I will
                     * take care of returning an appropriate error */
                }

                /*
                 * Let me know if you need further info to process the request - e.g.,
                 * we could provide the app's jobid or argv[0]. If they ask for
                 * a specific storage system, then get process that one.
                 */

                /* I will package the data for return once I see what Lustre provides */
                continue;
            }

            /* as fallback, call into base code to run general VFS query */
            printf("\t%s: ", q_str);
            q_ret = pmix_pstrg_base_vfs_query(q_sys->mountpt, queries[n].keys[m],
                results);
        }
    }
#endif

    return PMIX_ERR_NOT_SUPPORTED;
}
