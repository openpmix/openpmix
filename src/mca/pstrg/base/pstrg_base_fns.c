/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2010      Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2012-2013 Los Alamos National Security, Inc. All rights reserved.
 * Copyright (c) 2017-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2020      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */


#include "src/include/pmix_config.h"

#include <stdio.h>
#ifdef HAVE_MNTENT_H
#include <mntent.h>
#endif

#include "include/pmix_common.h"

#include "src/mca/pstrg/pstrg.h"
#include "src/mca/pstrg/base/base.h"

pmix_hash_table_t fs_mount_to_id_hash;
pmix_hash_table_t fs_id_to_mount_hash;

pmix_status_t pmix_pstrg_get_fs_mounts(pmix_value_array_t **mounts)
{
#ifdef HAVE_MNTENT_H
    FILE *mtab_fp;
    struct mntent *ent;
    pmix_pstrg_mntent_t tmp_pstrg_ent;
    pmix_status_t rc;

    mtab_fp = setmntent("/etc/mtab", "r");
    if (!mtab_fp) {
        rc = PMIX_ERROR;
        goto exit;
    }

    *mounts = PMIX_NEW(pmix_value_array_t);
    if (NULL == *mounts) {
        rc = PMIX_ERR_NOMEM;
        goto exit;
    }

    rc = pmix_value_array_init(*mounts, sizeof(pmix_pstrg_mntent_t));
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(*mounts);
        goto exit;
    }

    while (NULL != (ent = getmntent(mtab_fp))) {
        /* iterate mount entries and add them to array to be returned */
        tmp_pstrg_ent.mnt_fsname = strdup(ent->mnt_fsname);
        tmp_pstrg_ent.mnt_dir = strdup(ent->mnt_dir);
        tmp_pstrg_ent.mnt_type = strdup(ent->mnt_type);
        rc = pmix_value_array_append_item(*mounts, &tmp_pstrg_ent);
        if (PMIX_SUCCESS != rc) {
            PMIX_RELEASE(*mounts);
            goto exit;
        }
    }

exit:
    if (mtab_fp)
        endmntent(mtab_fp);
    return rc;
#else
    return PMIX_ERR_NOT_SUPPORTED;
#endif
}

pmix_status_t pmix_pstrg_free_fs_mounts(pmix_value_array_t **mounts)
{
    pmix_pstrg_mntent_t pstrg_ent;
    size_t nmounts, n;

    nmounts = pmix_value_array_get_size(*mounts);
    for (n = 0; n < nmounts; n++) {
        pstrg_ent = PMIX_VALUE_ARRAY_GET_ITEM(*mounts, pmix_pstrg_mntent_t, n);
        free(pstrg_ent.mnt_fsname);
        free(pstrg_ent.mnt_dir);
        free(pstrg_ent.mnt_type);
    }

    PMIX_RELEASE(*mounts);
    return PMIX_SUCCESS;
}

pmix_status_t pmix_pstrg_register_fs(pmix_pstrg_fs_info_t fs_info)
{
    pmix_status_t rc;

    /* maintain mappings from FS id->mount and mount->id */
    rc = pmix_hash_table_set_value_ptr(&fs_id_to_mount_hash,
        fs_info.id, strlen(fs_info.id)+1, fs_info.mount_dir);
    if (PMIX_SUCCESS == rc) {
        rc = pmix_hash_table_set_value_ptr(&fs_mount_to_id_hash,
            fs_info.mount_dir, strlen(fs_info.mount_dir)+1, fs_info.id);
        if (PMIX_SUCCESS != rc) {
            pmix_hash_table_remove_value_ptr(&fs_id_to_mount_hash,
                fs_info.id, strlen(fs_info.id)+1);
        }
    }

    return rc;
}

pmix_status_t pmix_pstrg_deregister_fs(pmix_pstrg_fs_info_t fs_info)
{
    pmix_status_t rc;

    /* try to remove from id->mnt and mnt->id tables */
    rc = pmix_hash_table_remove_value_ptr(&fs_id_to_mount_hash,
        fs_info.id, strlen(fs_info.id)+1);
    if (PMIX_SUCCESS == rc) {
        rc = pmix_hash_table_remove_value_ptr(&fs_mount_to_id_hash,
            fs_info.mount_dir, strlen(fs_info.mount_dir)+1);
    }

    return rc;
}

char *pmix_pstrg_get_registered_fs_id_by_mount(char *mount_dir)
{
    char *id;
    pmix_status_t rc;

    rc = pmix_hash_table_get_value_ptr(&fs_mount_to_id_hash,
        mount_dir, strlen(mount_dir)+1, (void **)&id);
    if (PMIX_SUCCESS == rc) {
        return id;
    }
    else {
        return NULL;
    }
}

char *pmix_pstrg_get_registered_fs_mount_by_id(char *id)
{
    char *mount_dir;
    pmix_status_t rc;

    rc = pmix_hash_table_get_value_ptr(&fs_id_to_mount_hash,
        id, strlen(id)+1, (void **)&mount_dir);
    if (PMIX_SUCCESS == rc) {
        return mount_dir;
    }
    else {
        return NULL;
    }
}
