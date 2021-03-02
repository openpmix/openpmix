/*
 * Copyright tracker->fs_info.id(c) 2015-2020 Intel, Inc.  All rights reserved.
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
#ifdef HAVE_SYS_STATVFS_H
#include <sys/statvfs.h>
#endif
#ifdef HAVE_MNTENT_H
#include <mntent.h>
#endif

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

#include "pstrg_vfs.h"
#include "src/mca/pstrg/base/base.h"
#include "src/mca/pstrg/pstrg.h"

static pmix_status_t vfs_init(void);
static void vfs_finalize(void);
static pmix_status_t query(pmix_query_t queries[], size_t nqueries, pmix_list_t *results,
                           pmix_pstrg_query_cbfunc_t cbfunc, void *cbdata);

pmix_pstrg_base_module_t pmix_pstrg_vfs_module = {
    .name = "vfs",
    .init = vfs_init,
    .finalize = vfs_finalize,
    .query = query
};

typedef struct pmix_pstrg_vfs_tracker {
    pmix_list_item_t super;
    pmix_pstrg_fs_info_t fs_info;
    /* more FS state can go here */
} pmix_pstrg_vfs_tracker_t;

pmix_list_t registered_fs_list;

static void tcon(pmix_pstrg_vfs_tracker_t *t)
{
    t->fs_info.id = NULL;
    t->fs_info.mount_dir = NULL;
}
static void tdes(pmix_pstrg_vfs_tracker_t *t)
{
    if (NULL != t->fs_info.id) {
        free(t->fs_info.id);
    }
    if (NULL != t->fs_info.mount_dir) {
        free(t->fs_info.mount_dir);
    }
}
static PMIX_CLASS_INSTANCE(pmix_pstrg_vfs_tracker_t,
                           pmix_list_item_t,
                           tcon, tdes);


static pmix_status_t vfs_init(void)
{
    pmix_value_array_t *mounts;
    size_t nmounts, n;
    pmix_pstrg_mntent_t ent;
    pmix_pstrg_vfs_tracker_t *tracker;
    pmix_status_t rc;

    pmix_output_verbose(2, pmix_pstrg_base_framework.framework_output,
                        "pstrg: vfs init");

    PMIX_CONSTRUCT(&registered_fs_list, pmix_list_t);

    rc = pmix_pstrg_get_fs_mounts(&mounts);
    if (PMIX_SUCCESS == rc) {
        nmounts = pmix_value_array_get_size(mounts);

        /* iterate returned mounts and register ones we want to track */
        for (n = 0; n < nmounts; n++) {
            ent = PMIX_VALUE_ARRAY_GET_ITEM(mounts, pmix_pstrg_mntent_t, n);

            /* XXX FILTER OUT ANY UNWANTED FILE SYSTEMS HERE */
            if (strcmp(ent.mnt_type, "ext4") != 0) continue;

            /* allocate a tracker so we can track FSes we register */
            tracker = PMIX_NEW(pmix_pstrg_vfs_tracker_t);
            if (NULL == tracker) {
                rc = PMIX_ERR_NOMEM;
                break;
            }

            /* create a unique string ID for the file system.
             * currently the ID is just '<mnt_type>-<fs_mount_dir>'
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

            pmix_list_append(&registered_fs_list, &tracker->super);
        }

        pmix_pstrg_free_fs_mounts(&mounts);
    }

    return rc;
}

static void vfs_finalize(void)
{
    pmix_pstrg_vfs_tracker_t *tracker;

    pmix_output_verbose(2, pmix_pstrg_base_framework.framework_output,
                        "pstrg: vfs finalize");

    PMIX_LIST_FOREACH(tracker, &registered_fs_list, pmix_pstrg_vfs_tracker_t) {
        pmix_pstrg_deregister_fs(tracker->fs_info);
    }
    PMIX_LIST_DESTRUCT(&registered_fs_list);
}

static pmix_status_t query(pmix_query_t queries[], size_t nqueries, pmix_list_t *results,
                           pmix_pstrg_query_cbfunc_t cbfunc, void *cbdata)
{
    size_t n, m, k, i;
    char **sid, **mount; //XXX free?
    char *tmp_id, *tmp_mount;
    bool takeus;
    pmix_pstrg_vfs_tracker_t *tracker;
    struct statvfs stat_buf;
    int ret;
    pmix_status_t rc = PMIX_SUCCESS;

    pmix_output_verbose(2, pmix_pstrg_base_framework.framework_output,
                        "pstrg: vfs query");

    for (n=0; n < nqueries; n++) {
        takeus=true;
        sid = NULL;
        mount = NULL;

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
            else {
                /* we don't know how to handle any other qualifiers currently */
                takeus = false;
                break;
            }
        }
        if (!takeus) {
            //XXX do we set an error? add an error to the response? just leave it blank?
            rc = PMIX_ERR_NOT_SUPPORTED;
            continue;
        }

        /* the remaining query keys all accept the storage ID or path qualifiers */
        ///XXX if there are multiple mounts and/or storage IDs, how do we account
        ///    for that properly in the returned results. Note that we do not allow
        ///    mixing of STORAGE_ID and STORAGE_PATH qualifiers for simplicity

        for (m=0; NULL != queries[n].keys[m]; m++) {
            printf("%s:\n", queries[n].keys[m]);

            if (0 == strcmp(queries[n].keys[m], PMIX_QUERY_STORAGE_LIST)) {
                printf("\t%s: ", pmix_pstrg_vfs_module.name); 
                if (queries[n].nqual == 0) {
                    PMIX_LIST_FOREACH(tracker, &registered_fs_list, pmix_pstrg_vfs_tracker_t) {
                        printf("%s,", tracker->fs_info.id);
                    }
                    printf("\n");
                }
                else {
                    /* no qualifiers supported for querying the storage list currently */
                    printf("ERR_NOT_SUPPORTED\n");
                }
                continue;
            }
            else if (0 == strcmp(queries[n].keys[m], PMIX_STORAGE_ID)) {
                if (mount && !sid) {
                    for (i = 0; mount[i] != NULL; i++) {
                        printf("\t%s: ", mount[i]);
                        tmp_id = pmix_pstrg_get_registered_fs_id_by_mount(mount[i]);
                        if (tmp_id) {
                            printf("%s\n", tmp_id);
                        }
                        else {
                            printf("ERR_NOT_FOUND\n");
                        }
                    }
                }
                else {
                    printf("\tERR_INVALID_ARGS\n");
                }
            }
            else if (0 == strcmp(queries[n].keys[m], PMIX_STORAGE_PATH)) {
                if (sid && !mount) {
                    for (i = 0; sid[i] != NULL; i++) {
                        printf("\t%s: ", sid[i]);
                        tmp_mount = pmix_pstrg_get_registered_fs_mount_by_id(sid[i]);
                        if (tmp_mount) {
                            printf("%s\n", tmp_mount);
                        }
                        else {
                            printf("ERR_NOT_FOUND\n");
                        }
                    }
                }
                else {
                    printf("\tERR_INVALID_ARGS\n");
                }
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

                        /* run statvfs on the mount point of the query */
                        ret = statvfs(tmp_mount, &stat_buf);
                        if (ret != 0) {
                            printf("\t%s: ERR_NOT_FOUND\n", marker);
                            continue;
                        }

                        if (0 == strcmp(queries[n].keys[m], PMIX_STORAGE_CAPACITY_LIMIT)) {
                            uint64_t cap_limit = (stat_buf.f_blocks * stat_buf.f_frsize) /
                                (1024 * 1024);
                            printf("\t%s: %lu MB\n", marker, cap_limit);
                        } else if (0 == strcmp(queries[n].keys[m], PMIX_STORAGE_CAPACITY_USED)) {
                            uint64_t cap_used =
                                ((stat_buf.f_blocks - stat_buf.f_bavail) * stat_buf.f_frsize) /
                                (1024 * 1024);
                            printf("\t%s: %lu MB\n", marker, cap_used);
                        } else if (0 == strcmp(queries[n].keys[m], PMIX_STORAGE_OBJECT_LIMIT)) {
                            uint64_t obj_limit = stat_buf.f_files;
                            printf("\t%s: %lu\n", marker, obj_limit);
                        } else if (0 == strcmp(queries[n].keys[m], PMIX_STORAGE_OBJECTS_USED)) {
                            uint64_t obj_used = stat_buf.f_files - stat_buf.f_favail;
                            printf("\t%s: %lu\n", marker, obj_used);
                        } else if (0 == strcmp(queries[n].keys[m], PMIX_STORAGE_MINIMAL_XFER_SIZE)) {
                            uint64_t xfer_size = stat_buf.f_bsize / 1024;
                            printf("\t%s: %lu KB\n", marker, xfer_size);
                        } else {
                            printf("\t%s: ERR_NOT_SUPPORTED\n", marker);
                        }
                    }
                }
                else {
                    printf("\tERR_INVALID_ARGS\n");
                }

            }
        }
    }

    return rc;
}
