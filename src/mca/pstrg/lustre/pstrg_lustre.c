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

typedef struct pmix_pstrg_lustre_tracker {
    pmix_list_item_t super;
    pmix_pstrg_fs_info_t fs_info;
    /* more Lustre FS state can go here */
} pmix_pstrg_lustre_tracker_t;

pmix_list_t registered_lustre_fs_list;

static void tcon(pmix_pstrg_lustre_tracker_t *t)
{
    t->fs_info.id = NULL;
    t->fs_info.mount_dir = NULL;
}
static void tdes(pmix_pstrg_lustre_tracker_t *t)
{
    if (NULL != t->fs_info.id) {
        free(t->fs_info.id);
    }
    if (NULL != t->fs_info.mount_dir) {
        free(t->fs_info.mount_dir);
    }
}
static PMIX_CLASS_INSTANCE(pmix_pstrg_lustre_tracker_t,
                           pmix_list_item_t,
                           tcon, tdes);

static pmix_status_t lustre_init(void)
{
<<<<<<< 19df213936a0c95fe2f3ce484790d73ea8babcab
    pmix_output_verbose(2, pmix_pstrg_base_framework.framework_output, "pstrg: lustre init");
||||||| merged common ancestors
    pmix_output_verbose(2, pmix_pstrg_base_framework.framework_output,
                        "pstrg: lustre init");
=======
    pmix_value_array_t *mounts;
    size_t nmounts, n;
    pmix_pstrg_mntent_t ent;
    pmix_pstrg_lustre_tracker_t *tracker;
    pmix_status_t rc;

    pmix_output_verbose(2, pmix_pstrg_base_framework.framework_output,
                        "pstrg: lustre init");
>>>>>>> add first cut at functioning lustre module

    PMIX_CONSTRUCT(&registered_lustre_fs_list, pmix_list_t);

    /* ADD HERE:
     *
     * Discover/connect to any available Lustre systems. Return an error
     * if none are preset, or you are unable to connect to them
     */

    ///XXX for now, just check the type of mount as specified in /etc/mtab
    rc = pmix_pstrg_get_fs_mounts(&mounts);
    if (PMIX_SUCCESS == rc) {
        nmounts = pmix_value_array_get_size(mounts);

	/* iterate returned mounts and register lustre ones we want to track */
        for (n = 0; n < nmounts; n++) {
            ent = PMIX_VALUE_ARRAY_GET_ITEM(mounts, pmix_pstrg_mntent_t, n);

            /* skip non-lustre mounts */
            if (strcmp(ent.mnt_type, "lustre") != 0) continue;

            /* allocate a tracker so we can track Lustre mounts we register */
            tracker = PMIX_NEW(pmix_pstrg_lustre_tracker_t);
            if (NULL == tracker) {
                rc = PMIX_ERR_NOMEM;
                break;
            }

            /* create a unique string ID for the file system. 
             * currently the ID is just 'lustre-<fs_mount_dir>'
             */
            tracker->fs_info.id = malloc(strlen(ent.mnt_type) + strlen(ent.mnt_dir) + 2);
            if (NULL == tracker->fs_info.id) {
                PMIX_RELEASE(tracker);
                rc = PMIX_ERR_NOMEM;
                break;
            }
            sprintf(tracker->fs_info.id, "%s-%s", ent.mnt_type, ent.mnt_dir);
            tracker->fs_info.mount_dir = strdup(ent.mnt_dir);
            if (NULL == tracker->fs_info.mount_dir) {
                PMIX_RELEASE(tracker);
                rc = PMIX_ERR_NOMEM;
                break;
            }

            rc = pmix_pstrg_register_fs(tracker->fs_info);
            if (PMIX_SUCCESS != rc) {
                PMIX_RELEASE(tracker);
                break;
            }

            pmix_list_append(&registered_lustre_fs_list, &tracker->super);
	}
        pmix_pstrg_free_fs_mounts(&mounts);
    }

    return PMIX_SUCCESS;
}

static void lustre_finalize(void)
{
<<<<<<< 19df213936a0c95fe2f3ce484790d73ea8babcab
    pmix_output_verbose(2, pmix_pstrg_base_framework.framework_output, "pstrg: lustre finalize");
||||||| merged common ancestors
    pmix_output_verbose(2, pmix_pstrg_base_framework.framework_output,
                        "pstrg: lustre finalize");
=======
    pmix_pstrg_lustre_tracker_t *tracker;

    pmix_output_verbose(2, pmix_pstrg_base_framework.framework_output,
                        "pstrg: lustre finalize");
>>>>>>> add first cut at functioning lustre module

    /* ADD HERE:
     *
     * Disconnect from any Lustre systems to which you have connected
     */
    ///XXX just free memory allocated above in lustre_init
    PMIX_LIST_FOREACH(tracker, &registered_lustre_fs_list, pmix_pstrg_lustre_tracker_t) {
        pmix_pstrg_deregister_fs(tracker->fs_info);
    }
    PMIX_LIST_DESTRUCT(&registered_lustre_fs_list);
}

static pmix_status_t query(pmix_query_t queries[], size_t nqueries, pmix_list_t *results,
                           pmix_pstrg_query_cbfunc_t cbfunc, void *cbdata)
{
<<<<<<< 19df213936a0c95fe2f3ce484790d73ea8babcab

    pmix_output_verbose(2, pmix_pstrg_base_framework.framework_output, "pstrg: lustre query");

#if 0
    size_t n, m, k, i, j;
    char **sid, **mountpt;
    bool takeus;
||||||| merged common ancestors
    size_t n, m, k, i, j;
    char **sid, **mountpt;
    bool takeus;
=======
    size_t n, m, k, i;
    char **sid, **mount; //XXX free?
    char *tmp_id, *tmp_mount;
>>>>>>> add first cut at functioning lustre module
    uid_t uid = UINT32_MAX;
    gid_t gid = UINT32_MAX;
    pmix_pstrg_lustre_tracker_t *tracker;

<<<<<<< 19df213936a0c95fe2f3ce484790d73ea8babcab
||||||| merged common ancestors
    pmix_output_verbose(2, pmix_pstrg_base_framework.framework_output,
                        "pstrg: lustre query");

#if 0
=======
    pmix_output_verbose(2, pmix_pstrg_base_framework.framework_output,
                        "pstrg: lustre query");

>>>>>>> add first cut at functioning lustre module
    for (n=0; n < nqueries; n++) {
<<<<<<< 19df213936a0c95fe2f3ce484790d73ea8babcab
        /* did they specify a storage type for this query? */
        takeus = true;
        for (k = 0; k < queries[n].nqual; k++) {
            if (0 == strcmp(queries[n].qualifiers[k].key, PMIX_STORAGE_TYPE)) {
||||||| merged common ancestors
        /* did they specify a storage type for this query? */
        takeus = true;
        for (k=0; k < queries[n].nqual; k++) {
            if (0 == strcmp(queries[n].qualifiers[k].key, PMIX_STORAGE_TYPE)) {
=======
	sid = NULL;
	mount = NULL;
>>>>>>> add first cut at functioning lustre module

<<<<<<< 19df213936a0c95fe2f3ce484790d73ea8babcab
                /* NOTE: I only included "lustre" as an accepted type, but we might
                 * want to consider other types as well - e.g., "parallel", "persistent",...) */

                if (NULL == strcasestr(queries[n].qualifiers[k].value.data.string, "lustre")) {
                    /* they are not interested in us */
                    takeus = false;
                    ;
                }
                break;
||||||| merged common ancestors
                /* NOTE: I only included "lustre" as an accepted type, but we might
                 * want to consider other types as well - e.g., "parallel", "persistent",...) */

                if (NULL == strcasestr(queries[n].qualifiers[k].value.data.string, "lustre")) {
                    /* they are not interested in us */
                    takeus = false;;
                }
                break;
=======
        for (k=0; k < queries[n].nqual; k++) {
            if (0 == strcmp(queries[n].qualifiers[k].key, PMIX_STORAGE_ID)) {
                /* we can answer generic queries for storage ids */
                /* there may be more than one (comma-delimited) storage id, so
                 * split them into a NULL-terminated argv-type array */
                sid = pmix_argv_split(queries[n].qualifiers[k].value.data.string, ',');
            }
            else if (0 == strcmp(queries[n].qualifiers[k].key, PMIX_STORAGE_PATH)) {
                /* we can answer generic queries for storage paths */
                /* there may be more than one (comma-delimited) mount pt, so
                 * split them into a NULL-terminated argv-type array */
                mount = pmix_argv_split(queries[n].qualifiers[k].value.data.string, ',');
            }
            else if (0 == strcmp(queries[n].qualifiers[k].key, PMIX_USERID)) {
	        uid = queries[n].qualifiers[k].value.data.uint32;
            }
	    else if (0 == strcmp(queries[n].qualifiers[k].key, PMIX_GRPID)) {
                gid = queries[n].qualifiers[k].value.data.uint32;
>>>>>>> add first cut at functioning lustre module
            }
        }

<<<<<<< 19df213936a0c95fe2f3ce484790d73ea8babcab
        /* see if they want the list of storage systems - this doesn't take any
         * qualifiers */
||||||| merged common ancestors
=======
        /* the remaining query keys all accept the storage ID or path qualifiers */
        ///XXX if there are multiple mounts and/or storage IDs, how do we account
        ///    for that properly in the returned results. Note that we do not allow

>>>>>>> add first cut at functioning lustre module
        for (m=0; NULL != queries[n].keys[m]; m++) {
<<<<<<< 19df213936a0c95fe2f3ce484790d73ea8babcab
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
||||||| merged common ancestors
            printf("%s:\n", queries[n].keys[m]);

            /* the remaining query keys all accept the storage ID and/or path qualifiers */
            sid = NULL;
            mountpt = NULL;
            for (k=0; k < queries[n].nqual; k++) {
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
=======
            printf("%s:\n", queries[n].keys[m]);

            if (0 == strcmp(queries[n].keys[m], PMIX_QUERY_STORAGE_LIST)) {
                printf("\t%s: ", pmix_pstrg_lustre_module.name);
                if (queries[n].nqual == 0) {
                    PMIX_LIST_FOREACH(tracker, &registered_lustre_fs_list,
				pmix_pstrg_lustre_tracker_t) {
                        printf("%s,", tracker->fs_info.id);
>>>>>>> add first cut at functioning lustre module
                    }
<<<<<<< 19df213936a0c95fe2f3ce484790d73ea8babcab
||||||| merged common ancestors
                    j++;
                }
                else {
                    /* no more storage systems to query */
                    break;
=======
                    printf("\n");
                }
                else {
                    /* no qualifiers supported for querying the storage list currently */
                    printf("ERR_NOT_SUPPORTED\n");
>>>>>>> add first cut at functioning lustre module
                }
<<<<<<< 19df213936a0c95fe2f3ce484790d73ea8babcab

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
||||||| merged common ancestors
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
                printf("\t%s: NOT SUPPORTED!\n", q_str);

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
                printf("\t%s: NOT SUPPORTED!\n", q_str);

                /*
                 * Let me know if you need further info to process the request - e.g.,
                 * we could provide the app's jobid or argv[0]. If they ask for
                 * a specific storage system, then get process that one.
                 */

                /* I will package the data for return once I see what Lustre provides */
=======
>>>>>>> add first cut at functioning lustre module
                continue;
            }
	    else {
               /* the following queries are all based on statvfs;
                * other generic queries should probably be handled before this
                */
                char *marker;
                if ((sid && !mount) || (mount && !sid)) {
                    for (i = 0; (sid && sid[i] != NULL) ||
                            (mount && mount[i] != NULL); i++) {
                        if (sid) {
                            marker = sid[i];
                            tmp_id = sid[i];
                            /* get mount to use for statfs */
                            tmp_mount = pmix_pstrg_get_registered_fs_mount_by_id(sid[i]);
                            if (!tmp_mount) {
                                printf("\t%s: ERR_NOT_FOUND", marker);
                            }
                        }
                        else
                        {
                            marker = mount[i];
                            tmp_mount = mount[i];
                            /* sanity check this mount has been registered before */
                            tmp_id = pmix_pstrg_get_registered_fs_id_by_mount(mount[i]);
                            if (!tmp_id) {
                                printf("\t%s: ERR_NOT_FOUND", marker);
                            }
                        }

			///XXX tmp_mount and tmp_id contain ID and mount for storage system being queried for this key

			///XXX stubbed out query handlers for Lustre
                        if (0 == strcmp(queries[n].keys[m], PMIX_STORAGE_CAPACITY_LIMIT)) {
			   if (uid != UINT32_MAX || gid != UINT32_MAX) {
				///XXX Lustre module will handle capacity/object queries when given specific uid/gid
			   }
			   else {
				///XXX fall back to VFS module to handle generic queries
			   }
			}
			else if (0 == strcmp(queries[n].keys[m], PMIX_STORAGE_CAPACITY_USED)) {
			   if (uid != UINT32_MAX || gid != UINT32_MAX) {
				///XXX Lustre module will handle capacity/object queries when given specific uid/gid
			   }
			   else {
				///XXX fall back to VFS module to handle generic queries
			   }
			}
			else if (0 == strcmp(queries[n].keys[m], PMIX_STORAGE_OBJECT_LIMIT)) {
			   if (UINT32_MAX != uid || UINT32_MAX != gid) {
				///XXX Lustre module will handle capacity/object queries when given specific uid/gid
			   }
			   else {
				///XXX fall back to VFS module to handle generic queries
			   }
			}
			else if (0 == strcmp(queries[n].keys[m], PMIX_STORAGE_OBJECTS_USED)) {
			   if (UINT32_MAX != uid || UINT32_MAX != gid) {
				///XXX Lustre module will handle capacity/object queries when given specific uid/gid
			   }
			   else {
				///XXX fall back to VFS module to handle generic queries
			   }
			}
			else if (0 == strcmp(queries[n].keys[m], PMIX_STORAGE_BW_LIMIT)) {
				///XXX if possible, implement BW attribute
			}
			else if (0 == strcmp(queries[n].keys[m], PMIX_STORAGE_BW)) {
				///XXX if possible, implement BW attribute
			}
		    }
		}
            }
        }
    }

    return PMIX_ERR_NOT_SUPPORTED;
}
