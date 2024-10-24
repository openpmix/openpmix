/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2016-2019 Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2018      IBM Corporation.  All rights reserved.
 * Copyright (c) 2018-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 *
 * Copyright (c) 2021-2024 Nanook Consulting  All rights reserved.
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
#include "src/util/pmix_argv.h"
#include "src/util/pmix_error.h"

#include "src/mca/gds/base/base.h"
#include "src/server/pmix_server_ops.h"

char *pmix_gds_base_get_available_modules(void)
{
    if (!pmix_gds_globals.initialized) {
        return NULL;
    }

    return strdup(pmix_gds_globals.all_mods);
}

/* Select a gds module per the given directives */
pmix_gds_base_module_t *pmix_gds_base_assign_module(pmix_info_t *info, size_t ninfo)
{
    pmix_gds_base_active_module_t *active;
    pmix_gds_base_module_t *mod = NULL;
    int pri, priority = -1;

    if (!pmix_gds_globals.initialized) {
        return NULL;
    }

    PMIX_LIST_FOREACH (active, &pmix_gds_globals.actives, pmix_gds_base_active_module_t) {
        if (NULL == active->module->assign_module) {
            continue;
        }
        if (PMIX_SUCCESS == active->module->assign_module(info, ninfo, &pri)) {
            if (pri < 0) {
                /* use the default priority from the component */
                pri = active->pri;
            }
            if (priority < pri) {
                mod = active->module;
                priority = pri;
            }
        }
    }

    return mod;
}

pmix_status_t pmix_gds_base_setup_fork(const pmix_proc_t *proc, char ***env)
{
    pmix_gds_base_active_module_t *active;
    pmix_status_t rc;

    if (!pmix_gds_globals.initialized) {
        return PMIX_ERR_INIT;
    }

    PMIX_LIST_FOREACH (active, &pmix_gds_globals.actives, pmix_gds_base_active_module_t) {
        if (NULL == active->module->setup_fork) {
            continue;
        }
        rc = active->module->setup_fork(proc, env);
        if (PMIX_SUCCESS != rc && PMIX_ERR_NOT_AVAILABLE != rc) {
            return rc;
        }
    }

    return PMIX_SUCCESS;
}

pmix_status_t pmix_gds_base_store_modex(pmix_buffer_t *buff,
                                        pmix_gds_base_store_modex_cb_fn_t cb_fn,
                                        void *cbdata)
{
    pmix_status_t rc = PMIX_SUCCESS;
    pmix_buffer_t bkt, bkt2, bkt3;
    pmix_byte_object_t bo, bo2, bo3, bo4;
    int32_t cnt = 1;
    pmix_server_trkr_t *trk = (pmix_server_trkr_t *) cbdata;
    pmix_proc_t proc;
    pmix_buffer_t pbkt;
    bool compressed, found;
    pmix_collect_t last_blob_info_byte, blob_info_byte;
    pmix_list_t nspaces;
    pmix_proclist_t *plist;

    PMIX_CONSTRUCT(&nspaces, pmix_list_t);

    /* Loop over the enclosed byte object envelopes and
     * store them in our GDS module */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, pmix_globals.mypeer, buff, &bo, &cnt, PMIX_BYTE_OBJECT);
    /* If the collect flag is set, we should have some data for unpacking */
    if ((PMIX_COLLECT_YES == trk->collect_type) &&
        (PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER == rc)) {
       goto exit;
    }

    // setup to unpack the blob
    PMIX_CONSTRUCT(&bkt, pmix_buffer_t);
    PMIX_LOAD_BUFFER(pmix_globals.mypeer, &bkt, bo.bytes, bo.size);

    // the byte object contains a series of byte objects, one from each
    // participating PMIx server - so unpack them in order

    // unpack the first server byte object
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, pmix_globals.mypeer, buff, &bo2, &cnt, PMIX_BYTE_OBJECT);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DESTRUCT(&bkt);
        goto exit;
    }

    while (PMIX_SUCCESS == rc) {

        // each server contribution contains a compression flag followed by
        // a series of rank-level byte objects
        PMIX_CONSTRUCT(&bkt2, pmix_buffer_t);
        PMIX_LOAD_BUFFER(pmix_globals.mypeer, &bkt2, bo2.bytes, bo2.size);

        // first element is a bool indicating if the collection of rank-level
        // objects was compressed
        cnt = 1;
        PMIX_BFROPS_UNPACK(rc, pmix_globals.mypeer, &bkt2, &compressed, &cnt, PMIX_BOOL);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DESTRUCT(&bkt);
            PMIX_DESTRUCT(&bkt2);
            goto exit;
        }

        // unpack the byte object containing all the rank-level objects
        // provided by this server
        cnt = 1;
        PMIX_BFROPS_UNPACK(rc, pmix_globals.mypeer, &bkt2, &bo3, &cnt, PMIX_BYTE_OBJECT);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DESTRUCT(&bkt);
            PMIX_DESTRUCT(&bkt2);
            goto exit;
        }

        // decompress it if required
        if (compressed) {

        }

        // set it up for unpacking
        PMIX_CONSTRUCT(&bkt3, pmix_buffer_t);
        PMIX_LOAD_BUFFER(pmix_globals.mypeer, &bkt3, bo3.bytes, bo3.size);

        // unpack the flag indicating if data was collected on that node
        cnt = 1;
        PMIX_BFROPS_UNPACK(rc, pmix_globals.mypeer, &bkt3, &blob_info_byte, &cnt, PMIX_BYTE);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DESTRUCT(&bkt);
            PMIX_DESTRUCT(&bkt2);
            goto exit;
        }
        if (PMIX_COLLECT_INVALID == last_blob_info_byte) {
            last_blob_info_byte = blob_info_byte;
        } else if (last_blob_info_byte != blob_info_byte) {
            // we have a mismatch - report the error
        }

       /* unpack the enclosed blobs from the various peers */
        cnt = 1;
        PMIX_BFROPS_UNPACK(rc, pmix_globals.mypeer, &bkt3, &bo4, &cnt, PMIX_BYTE_OBJECT);
        while (PMIX_SUCCESS == rc) {
            /* unpack all the kval's from this peer and store them in
             * our GDS. Note that PMIx by design holds all data at
             * the server level until requested. If our GDS is a
             * shared memory region, then the data may be available
             * right away - but the client still has to be notified
             * of its presence. */

            /* setup the byte object for unpacking */
            PMIX_CONSTRUCT(&pbkt, pmix_buffer_t);
            PMIX_LOAD_BUFFER(pmix_globals.mypeer, &pbkt, bo4.bytes, bo4.size);

            // unpack the proc that provided this data
            cnt = 1;
            PMIX_BFROPS_UNPACK(rc, pmix_globals.mypeer, &pbkt, &proc, &cnt, PMIX_PROC);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DESTRUCT(&pbkt);
                break;
            }

            // track the nspace involved
            found = false;
            PMIX_LIST_FOREACH(plist, &nspaces, pmix_proclist_t) {
                if (PMIX_CHECK_NSPACE(plist->proc.nspace, proc.nspace)) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                plist = PMIX_NEW(pmix_proclist_t);
                PMIX_LOAD_NSPACE(plist->proc.nspace, proc.nspace);
                pmix_list_append(&nspaces, &plist->super);
            }

            // call the specific GDS component-provided function to store it
            rc = cb_fn(&proc, &pbkt);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DESTRUCT(&pbkt);
                break;
            }
            PMIX_DESTRUCT(&pbkt);
            PMIX_BYTE_OBJECT_DESTRUCT(&bo4);
            /* get the next blob */
            cnt = 1;
            PMIX_BFROPS_UNPACK(rc, pmix_globals.mypeer, &bkt3, &bo4, &cnt, PMIX_BYTE_OBJECT);
        }
        PMIX_DESTRUCT(&bkt);

        if (PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER == rc) {
            rc = PMIX_SUCCESS;
        } else if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto exit;
        }
        /* unpack and process the next blob */
        cnt = 1;
        PMIX_BFROPS_UNPACK(rc, pmix_globals.mypeer, buff, &bo, &cnt, PMIX_BYTE_OBJECT);
    }

    if (PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER == rc) {
        rc = PMIX_SUCCESS;
    } else if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto exit;
    }
    // indicate that we are done processing the modex - need to alert
    // for each nspace involved
    PMIX_LIST_FOREACH(plist, &nspaces, pmix_proclist_t) {
        rc = cb_fn(&plist->proc, NULL);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            break;
        }
    }

exit:
    PMIX_LIST_DESTRUCT(&nspaces);
    return rc;
}
