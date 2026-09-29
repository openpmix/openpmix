/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2014-2015 Artem Y. Polyakov <artpol84@gmail.com>.
 *                         All rights reserved.
 * Copyright (c) 2016-2019 Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2016-2020 IBM Corporation.  All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * Copyright (c) 2022-2023 Triad National Security, LLC. All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "src/include/pmix_config.h"

#include "src/include/pmix_socket_errno.h"
#include "src/include/pmix_stdint.h"

#include "include/pmix_server.h"
#include "src/mca/pcompress/pcompress.h"
#include "src/include/pmix_globals.h"
#include "src/runtime/pmix_rte.h"

#ifdef HAVE_STRING_H
#    include <string.h>
#endif
#ifdef HAVE_SYS_STAT_H
#    include <sys/stat.h>
#endif
#include <fcntl.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif
#ifdef HAVE_SYS_SOCKET_H
#    include <sys/socket.h>
#endif
#ifdef HAVE_SYS_UN_H
#    include <sys/un.h>
#endif
#ifdef HAVE_SYS_UIO_H
#    include <sys/uio.h>
#endif
#ifdef HAVE_SYS_TYPES_H
#    include <sys/types.h>
#endif
#ifdef HAVE_TIME_H
#    include <time.h>
#endif
#include <event.h>

#include "src/class/pmix_hotel.h"
#include "src/class/pmix_list.h"
#include "src/common/pmix_attributes.h"
#include "src/common/pmix_iof.h"
#include "src/common/pmix_pfexec.h"
#include "src/common/pmix_monitor.h"
#include "src/hwloc/pmix_hwloc.h"
#include "src/mca/bfrops/base/base.h"
#include "src/mca/gds/base/base.h"
#include "src/mca/plog/base/base.h"
#include "src/mca/pnet/pnet.h"
#include "src/mca/psensor/psensor.h"
#include "src/mca/ptl/base/base.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_idname.h"
#include "src/util/pmix_name_fns.h"
#include "src/util/pmix_show_help.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_environ.h"

#include "src/client/pmix_client_ops.h"
#include "pmix_server_ops.h"

pmix_server_module_t pmix_host_server = {
    .client_connected = NULL,
    .client_finalized = NULL,
    .abort = NULL,
    .fence_nb = NULL,
    .direct_modex = NULL,
    .publish = NULL,
    .lookup = NULL,
    .unpublish = NULL,
    .spawn = NULL,
    .connect = NULL,
    .disconnect = NULL,
    .register_events = NULL,
    .deregister_events = NULL,
    .listener = NULL,
    .notify_event = NULL,
    .query = NULL,
    .tool_connected = NULL,
    .tool_connected2 = NULL,
    .log = NULL,
    .log2 = NULL,
    .allocate = NULL,
    .job_control = NULL,
    .monitor = NULL,
    .get_credential = NULL,
    .validate_credential = NULL,
    .iof_pull = NULL,
    .push_stdin = NULL,
    .group = NULL,
    .fabric = NULL,
    .client_connected2 = NULL,
    .session_control = NULL
};

static void abcbfn(pmix_status_t status, void *cbdata)
{
    pmix_setup_caddy_t *cd = (pmix_setup_caddy_t*)cbdata;

    if (NULL != cd->opcbfunc) {
        cd->opcbfunc(status, cd->cbdata);
    }
    /* the abort message was unpacked into the caddy, and the caddy's
     * destructor does not free that member - it is borrowed as often
     * as it is owned */
    if (NULL != cd->nspace) {
        free(cd->nspace);
    }
    PMIX_RELEASE(cd);
}

pmix_status_t pmix_server_abort(pmix_peer_t *peer, pmix_buffer_t *buf,
                                pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    pmix_setup_caddy_t *cd;
    int32_t cnt;
    pmix_status_t rc;
    pmix_proc_t proc;

    pmix_output_verbose(2, pmix_server_globals.base_output,
                        "recvd ABORT");

    cd = PMIX_NEW(pmix_setup_caddy_t);
    if (NULL == cd) {
        return PMIX_ERR_NOMEM;
    }
    cd->opcbfunc = cbfunc;
    cd->cbdata = cbdata;

    /* unpack the status */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &cd->status, &cnt, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(cd);
        return rc;
    }
    /* unpack the message - this allocates, and from here on every path
     * that discards the caddy owes it a free */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &cd->nspace, &cnt, PMIX_STRING);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(cd);
        return rc;
    }
    /* unpack the number of procs */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &cd->nprocs, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto error;
    }
    /* check the count before it sizes an allocation - scaddes walks
     * the same size_t when it frees. See pmix_bfrop_count_fits() */
    cnt = cd->nprocs;
    if (!pmix_bfrop_count_fits(buf, cd->nprocs, PMIX_PROC)) {
        rc = PMIX_ERR_BAD_PARAM;
        PMIX_ERROR_LOG(rc);
        goto error;
    }

    /* unpack any provided procs - these are the procs the caller
     * wants aborted */
    if (0 < cd->nprocs) {
        PMIX_PROC_CREATE(cd->procs, cd->nprocs);
        if (NULL == cd->procs) {
            rc = PMIX_ERR_NOMEM;
            PMIX_ERROR_LOG(rc);
            goto error;
        }
        cnt = cd->nprocs;
        PMIX_BFROPS_UNPACK(rc, peer, buf, cd->procs, &cnt, PMIX_PROC);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto error;
        }
    }

    /* let the local host's server execute it */
    if (NULL == pmix_host_server.abort) {
        rc = PMIX_ERR_NOT_SUPPORTED;
        goto error;
    }
    pmix_strncpy(proc.nspace, peer->info->pname.nspace, PMIX_MAX_NSLEN);
    proc.rank = peer->info->pname.rank;
    rc = pmix_host_server.abort(&proc, peer->info->server_object, cd->status,
                                cd->nspace, cd->procs, cd->nprocs,
                                abcbfn, cd);
    if (PMIX_SUCCESS != rc) {
        goto error;
    }

    return rc;

error:
    if (NULL != cd->nspace) {
        free(cd->nspace);
    }
    PMIX_RELEASE(cd);
    return rc;
}

static void opcbfunc(pmix_status_t status, void *cbdata)
{
    pmix_setup_caddy_t *cd = (pmix_setup_caddy_t *) cbdata;

    if (NULL != cd->keys) {
        PMIx_Argv_free(cd->keys);
    }
    if (NULL != cd->codes) {
        free(cd->codes);
    }
    if (NULL != cd->info) {
        PMIX_INFO_FREE(cd->info, cd->ninfo);
    }
    if (NULL != cd->opcbfunc) {
        cd->opcbfunc(status, cd->cbdata);
    }
    PMIX_RELEASE(cd);
}

pmix_status_t pmix_server_add_requester_id(pmix_peer_t *peer, pmix_info_t **info,
                                           size_t *ninfo)
{
    pmix_info_t *old = *info, *new;
    size_t n, m, nold = *ninfo;
    uint32_t id, gid;
    pmix_status_t rc;

    if (NULL == peer || NULL == peer->info) {
        return PMIX_ERR_BAD_PARAM;
    }
    /* the group is the requester's choice if it named one - by number or
     * by name. A group it named that cannot be resolved refuses the
     * request rather than quietly charging it to another */
    gid = (uint32_t) peer->info->gid;
    for (n = 0; n < nold; n++) {
        if (PMIx_Check_key(old[n].key, PMIX_GRPID)) {
            rc = pmix_util_gid_from_value(&old[n].value, &gid);
            if (PMIX_SUCCESS != rc) {
                return rc;
            }
            break;
        }
    }
    new = PMIx_Info_create(nold + 2);
    if (NULL == new) {
        return PMIX_ERR_NOMEM;
    }
    /* move every entry except the uid and gid the requester supplied -
     * the entries are moved, not copied, so the old block is released
     * below without destructing them */
    m = 0;
    for (n = 0; n < nold; n++) {
        if (PMIx_Check_key(old[n].key, PMIX_USERID) ||
            PMIx_Check_key(old[n].key, PMIX_GRPID)) {
            PMIx_Info_destruct(&old[n]);
            continue;
        }
        memcpy(&new[m], &old[n], sizeof(pmix_info_t));
        ++m;
    }
    id = (uint32_t) peer->info->uid;
    PMIx_Info_load(&new[m], PMIX_USERID, &id, PMIX_UINT32);
    ++m;
    PMIx_Info_load(&new[m], PMIX_GRPID, &gid, PMIX_UINT32);
    ++m;
    if (NULL != old) {
        PMIx_Info_free(old, 0);
    }
    *info = new;
    *ninfo = m;
    return PMIX_SUCCESS;
}

/* nested info arrays are followed no deeper than this */
#define PMIX_SERVER_IDS_MAXDEPTH 8

static bool is_id_key(const pmix_info_t *info)
{
    return (PMIx_Check_key(info->key, PMIX_USERID) || PMIx_Check_key(info->key, PMIX_GRPID));
}

/* PMIX_ACCESS_USERIDS / PMIX_ACCESS_GRPIDS - a list of ids, each of which
 * may be a name */
static bool is_id_list_key(const pmix_info_t *info)
{
    return (PMIx_Check_key(info->key, PMIX_ACCESS_USERIDS) ||
            PMIx_Check_key(info->key, PMIX_ACCESS_GRPIDS));
}

static bool is_user_key(const pmix_info_t *info)
{
    return (PMIx_Check_key(info->key, PMIX_USERID) ||
            PMIx_Check_key(info->key, PMIX_ACCESS_USERIDS));
}

/* an id list given by name: one string, or a data array of strings */
static bool is_named_id_list(const pmix_info_t *info)
{
    if (!is_id_list_key(info)) {
        return false;
    }
    if (PMIX_STRING == info->value.type) {
        return true;
    }
    return (PMIX_DATA_ARRAY == info->value.type && NULL != info->value.data.darray &&
            PMIX_STRING == info->value.data.darray->type);
}

static pmix_status_t resolve_one(const pmix_info_t *info, const char *name, uint32_t *id)
{
    if (is_user_key(info)) {
        return pmix_util_uid_from_string(name, id);
    }
    return pmix_util_gid_from_string(name, id);
}

/* replace an id list given by name with the same list as numbers */
static pmix_status_t resolve_id_list(pmix_info_t *info)
{
    pmix_data_array_t *da, *nda;
    char **names;
    uint32_t *ids, id;
    pmix_status_t rc;
    size_t n;

    if (PMIX_STRING == info->value.type) {
        rc = resolve_one(info, info->value.data.string, &id);
        if (PMIX_SUCCESS != rc) {
            return rc;
        }
        PMIx_Value_destruct(&info->value);
        return PMIx_Value_load(&info->value, &id, PMIX_UINT32);
    }
    da = info->value.data.darray;
    nda = PMIx_Data_array_create(da->size, PMIX_UINT32);
    if (NULL == nda) {
        return PMIX_ERR_NOMEM;
    }
    names = (char **) da->array;
    ids = (uint32_t *) nda->array;
    for (n = 0; n < da->size; n++) {
        rc = resolve_one(info, (NULL == names) ? NULL : names[n], &ids[n]);
        if (PMIX_SUCCESS != rc) {
            PMIx_Data_array_free(nda);
            return rc;
        }
    }
    PMIx_Value_destruct(&info->value);
    info->value.type = PMIX_DATA_ARRAY;
    info->value.data.darray = nda;
    return PMIX_SUCCESS;
}

static bool is_info_array(const pmix_info_t *info)
{
    return (PMIX_DATA_ARRAY == info->value.type && NULL != info->value.data.darray &&
            PMIX_INFO == info->value.data.darray->type && NULL != info->value.data.darray->array);
}

/* true if the array, or an info array nested in it, gives a user or group
 * by name - PMIX_USERID, PMIX_GRPID, or an entry of PMIX_ACCESS_USERIDS or
 * PMIX_ACCESS_GRPIDS */
static bool has_id_name(const pmix_info_t *info, size_t ninfo, int depth)
{
    size_t n;

    for (n = 0; n < ninfo; n++) {
        if ((is_id_key(&info[n]) && PMIX_STRING == info[n].value.type) ||
            is_named_id_list(&info[n])) {
            return true;
        }
        if (depth < PMIX_SERVER_IDS_MAXDEPTH && is_info_array(&info[n]) &&
            has_id_name((pmix_info_t *) info[n].value.data.darray->array,
                        info[n].value.data.darray->size, depth + 1)) {
            return true;
        }
    }
    return false;
}

/* resolve every user and group given by name, in an array we own */
static pmix_status_t resolve_id_names(pmix_info_t *info, size_t ninfo, int depth)
{
    pmix_status_t rc;
    uint32_t id;
    size_t n;

    for (n = 0; n < ninfo; n++) {
        if (is_id_key(&info[n]) && PMIX_STRING == info[n].value.type) {
            if (PMIx_Check_key(info[n].key, PMIX_USERID)) {
                rc = pmix_util_uid_from_string(info[n].value.data.string, &id);
            } else {
                rc = pmix_util_gid_from_string(info[n].value.data.string, &id);
            }
            if (PMIX_SUCCESS != rc) {
                return rc;
            }
            PMIx_Value_destruct(&info[n].value);
            PMIx_Value_load(&info[n].value, &id, PMIX_UINT32);
        } else if (is_named_id_list(&info[n])) {
            rc = resolve_id_list(&info[n]);
            if (PMIX_SUCCESS != rc) {
                return rc;
            }
        } else if (depth < PMIX_SERVER_IDS_MAXDEPTH && is_info_array(&info[n])) {
            rc = resolve_id_names((pmix_info_t *) info[n].value.data.darray->array,
                                  info[n].value.data.darray->size, depth + 1);
            if (PMIX_SUCCESS != rc) {
                return rc;
            }
        }
    }
    return PMIX_SUCCESS;
}

pmix_status_t pmix_server_normalize_ids(const pmix_info_t *info, size_t ninfo,
                                        pmix_info_t **out, size_t *nout)
{
    pmix_info_t *copy;
    pmix_status_t rc;
    size_t n;

    *out = NULL;
    *nout = 0;
    if (NULL == info || 0 == ninfo || !has_id_name(info, ninfo, 0)) {
        return PMIX_SUCCESS;
    }
    copy = PMIx_Info_create(ninfo);
    if (NULL == copy) {
        return PMIX_ERR_NOMEM;
    }
    for (n = 0; n < ninfo; n++) {
        rc = PMIx_Info_xfer(&copy[n], &info[n]);
        if (PMIX_SUCCESS != rc) {
            PMIx_Info_free(copy, ninfo);
            return rc;
        }
    }
    rc = resolve_id_names(copy, ninfo, 0);
    if (PMIX_SUCCESS != rc) {
        PMIx_Info_free(copy, ninfo);
        return rc;
    }
    *out = copy;
    *nout = ninfo;
    return PMIX_SUCCESS;
}

pmix_status_t pmix_server_add_requester_proc(pmix_peer_t *peer, pmix_info_t **info,
                                             size_t *ninfo)
{
    pmix_info_t *old = *info, *new;
    size_t n, m, nold = *ninfo;
    pmix_proc_t proc;

    if (NULL == peer || NULL == peer->info) {
        return PMIX_ERR_BAD_PARAM;
    }
    new = PMIx_Info_create(nold + 1);
    if (NULL == new) {
        return PMIX_ERR_NOMEM;
    }
    /* move every entry except a PMIX_REQUESTOR the requester supplied -
     * moved, not copied, as in pmix_server_add_requester_id */
    m = 0;
    for (n = 0; n < nold; n++) {
        if (PMIx_Check_key(old[n].key, PMIX_REQUESTOR)) {
            PMIx_Info_destruct(&old[n]);
            continue;
        }
        memcpy(&new[m], &old[n], sizeof(pmix_info_t));
        ++m;
    }
    PMIX_LOAD_PROCID(&proc, peer->info->pname.nspace, peer->info->pname.rank);
    PMIx_Info_load(&new[m], PMIX_REQUESTOR, &proc, PMIX_PROC);
    ++m;
    if (NULL != old) {
        PMIx_Info_free(old, 0);
    }
    *info = new;
    *ninfo = m;
    return PMIX_SUCCESS;
}

pmix_status_t pmix_server_publish(pmix_peer_t *peer, pmix_buffer_t *buf,
                                  pmix_op_cbfunc_t cbfunc,
                                  void *cbdata)
{
    pmix_setup_caddy_t *cd;
    pmix_status_t rc;
    int32_t cnt;
    size_t ninfo;
    pmix_proc_t proc;
    uint32_t uid;

    pmix_output_verbose(2, pmix_server_globals.pub_output, "recvd PUBLISH");

    if (NULL == pmix_host_server.publish) {
        return PMIX_ERR_NOT_SUPPORTED;
    }

    /* consume the effective user id the message carries, and discard what
     * it says.  Access to published data is decided in terms of this pair,
     * so neither half may be a claim the requestor restates per command:
     * the connection handshake already carried both ids and the peer was
     * refused at that point if it claimed a uid or gid other than the one
     * it was registered with, which makes peer->info the pair the host
     * itself vouched for.  The field stays on the wire because the message
     * format is frozen - there is no version number that distinguishes a
     * peer built before its removal from one built after - and for that
     * same reason the group id never joined it there. */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &uid, &cnt, PMIX_UINT32);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    /* unpack the number of info objects */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &ninfo, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    /* the buffer must hold that many packed elements before the count
     * sizes an allocation - see pmix_bfrop_count_fits() */
    cnt = ninfo;
    if (!pmix_bfrop_count_fits(buf, ninfo, PMIX_INFO)) {
        rc = PMIX_ERR_BAD_PARAM;
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    cd = PMIX_NEW(pmix_setup_caddy_t);
    if (NULL == cd) {
        return PMIX_ERR_NOMEM;
    }
    cd->opcbfunc = cbfunc;
    cd->cbdata = cbdata;
    cd->ninfo = ninfo;
    if (0 < ninfo) {
        PMIX_INFO_CREATE(cd->info, cd->ninfo);
        if (NULL == cd->info) {
            rc = PMIX_ERR_NOMEM;
            goto cleanup;
        }
    }
    /* unpack the array of info objects */
    if (0 < ninfo) {
        cnt = ninfo;
        PMIX_BFROPS_UNPACK(rc, peer, buf, cd->info, &cnt, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto cleanup;
        }
    }
    /* the host decides access to published data by the requester's
     * identity, so pass the pair recorded for this peer at connection,
     * in place of any the requester supplied */
    rc = pmix_server_add_requester_id(peer, &cd->info, &cd->ninfo);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto cleanup;
    }

    /* call the local server */
    pmix_strncpy(proc.nspace, peer->info->pname.nspace, PMIX_MAX_NSLEN);
    proc.rank = peer->info->pname.rank;
    rc = pmix_host_server.publish(&proc, cd->info, cd->ninfo, opcbfunc, cd);

cleanup:
    if (PMIX_SUCCESS != rc) {
        if (NULL != cd->info) {
            PMIX_INFO_FREE(cd->info, cd->ninfo);
        }
        PMIX_RELEASE(cd);
    }
    return rc;
}

static void lkcbfunc(pmix_status_t status, pmix_pdata_t data[],
                     size_t ndata, void *cbdata)
{
    pmix_setup_caddy_t *cd = (pmix_setup_caddy_t *) cbdata;

    /* cleanup the caddy */
    if (NULL != cd->keys) {
        PMIx_Argv_free(cd->keys);
    }
    if (NULL != cd->info) {
        PMIX_INFO_FREE(cd->info, cd->ninfo);
    }

    /* return the results */
    if (NULL != cd->lkcbfunc) {
        cd->lkcbfunc(status, data, ndata, cd->cbdata);
    }
    PMIX_RELEASE(cd);
}

pmix_status_t pmix_server_lookup(pmix_peer_t *peer, pmix_buffer_t *buf,
                                 pmix_lookup_cbfunc_t cbfunc,
                                 void *cbdata)
{
    pmix_setup_caddy_t *cd;
    int32_t cnt;
    pmix_status_t rc;
    size_t nkeys, i;
    char *sptr;
    size_t ninfo;
    pmix_proc_t proc;
    uint32_t uid;

    pmix_output_verbose(2, pmix_server_globals.pub_output, "recvd LOOKUP");

    if (NULL == pmix_host_server.lookup) {
        return PMIX_ERR_NOT_SUPPORTED;
    }

    /* consume the effective user id the message carries, and discard what
     * it says.  Access to published data is decided in terms of this pair,
     * so neither half may be a claim the requestor restates per command:
     * the connection handshake already carried both ids and the peer was
     * refused at that point if it claimed a uid or gid other than the one
     * it was registered with, which makes peer->info the pair the host
     * itself vouched for.  The field stays on the wire because the message
     * format is frozen - there is no version number that distinguishes a
     * peer built before its removal from one built after - and for that
     * same reason the group id never joined it there. */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &uid, &cnt, PMIX_UINT32);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    /* unpack the number of keys */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &nkeys, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    /* setup the caddy */
    cd = PMIX_NEW(pmix_setup_caddy_t);
    if (NULL == cd) {
        return PMIX_ERR_NOMEM;
    }
    cd->lkcbfunc = cbfunc;
    cd->cbdata = cbdata;
    /* unpack the array of keys */
    for (i = 0; i < nkeys; i++) {
        sptr = NULL;
        cnt = 1;
        PMIX_BFROPS_UNPACK(rc, peer, buf, &sptr, &cnt, PMIX_STRING);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto cleanup;
        }
        if (NULL == sptr) {
            rc = PMIX_ERR_BAD_PARAM;
            goto cleanup;
        }
        rc = PMIx_Argv_append_nosize(&cd->keys, sptr);
        free(sptr);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto cleanup;
        }
    }
    /* unpack the number of info objects */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &ninfo, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto cleanup;
    }
    /* screen the count before it sizes an allocation - see the note in
     * pmix_server_publish */
    cnt = ninfo;
    if (!pmix_bfrop_count_fits(buf, ninfo, PMIX_INFO)) {
        rc = PMIX_ERR_BAD_PARAM;
        PMIX_ERROR_LOG(rc);
        goto cleanup;
    }
    cd->ninfo = ninfo;
    if (0 < ninfo) {
        PMIX_INFO_CREATE(cd->info, cd->ninfo);
        if (NULL == cd->info) {
            rc = PMIX_ERR_NOMEM;
            goto cleanup;
        }
    }
    /* unpack the array of info objects */
    if (0 < ninfo) {
        cnt = ninfo;
        PMIX_BFROPS_UNPACK(rc, peer, buf, cd->info, &cnt, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto cleanup;
        }
    }
    /* the host decides access to published data by the requester's
     * identity, so pass the pair recorded for this peer at connection,
     * in place of any the requester supplied */
    rc = pmix_server_add_requester_id(peer, &cd->info, &cd->ninfo);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto cleanup;
    }

    /* call the local server */
    pmix_strncpy(proc.nspace, peer->info->pname.nspace, PMIX_MAX_NSLEN);
    proc.rank = peer->info->pname.rank;
    rc = pmix_host_server.lookup(&proc, cd->keys, cd->info, cd->ninfo, lkcbfunc, cd);
    /* PMIX_OPERATION_SUCCEEDED cannot express this operation's result -
     * the published data that was asked for, which comes back through the
     * callback we always supply. Left alone it would reach the requesting
     * client as a synthesized status-only reply, and the client reads the
     * value count out of the bytes that follow a successful status, so it
     * would end up reporting an unpack error for a lookup its host said
     * had succeeded */
    if (PMIX_UNLIKELY(PMIX_OPERATION_SUCCEEDED == rc)) {
        pmix_show_help("help-pmix-server.txt", "atomic-completion-unsupported",
                       true, "PMIx_Lookup from a client");
        rc = PMIX_ERR_NOT_SUPPORTED;
    }

cleanup:
    if (PMIX_SUCCESS != rc) {
        if (NULL != cd->keys) {
            PMIx_Argv_free(cd->keys);
        }
        if (NULL != cd->info) {
            PMIX_INFO_FREE(cd->info, cd->ninfo);
        }
        PMIX_RELEASE(cd);
    }
    return rc;
}

pmix_status_t pmix_server_unpublish(pmix_peer_t *peer, pmix_buffer_t *buf,
                                    pmix_op_cbfunc_t cbfunc,
                                    void *cbdata)
{
    pmix_setup_caddy_t *cd;
    int32_t cnt;
    pmix_status_t rc;
    size_t i, nkeys, ninfo;
    char *sptr;
    pmix_proc_t proc;
    uint32_t uid;

    pmix_output_verbose(2, pmix_server_globals.pub_output, "recvd UNPUBLISH");

    if (NULL == pmix_host_server.unpublish) {
        return PMIX_ERR_NOT_SUPPORTED;
    }

    /* consume the effective user id the message carries, and discard what
     * it says.  Access to published data is decided in terms of this pair,
     * so neither half may be a claim the requestor restates per command:
     * the connection handshake already carried both ids and the peer was
     * refused at that point if it claimed a uid or gid other than the one
     * it was registered with, which makes peer->info the pair the host
     * itself vouched for.  The field stays on the wire because the message
     * format is frozen - there is no version number that distinguishes a
     * peer built before its removal from one built after - and for that
     * same reason the group id never joined it there. */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &uid, &cnt, PMIX_UINT32);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    /* unpack the number of keys */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &nkeys, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    /* setup the caddy */
    cd = PMIX_NEW(pmix_setup_caddy_t);
    if (NULL == cd) {
        return PMIX_ERR_NOMEM;
    }
    cd->opcbfunc = cbfunc;
    cd->cbdata = cbdata;
    /* unpack the array of keys */
    for (i = 0; i < nkeys; i++) {
        sptr = NULL;
        cnt = 1;
        PMIX_BFROPS_UNPACK(rc, peer, buf, &sptr, &cnt, PMIX_STRING);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto cleanup;
        }
        if (NULL == sptr) {
            rc = PMIX_ERR_BAD_PARAM;
            goto cleanup;
        }
        rc = PMIx_Argv_append_nosize(&cd->keys, sptr);
        free(sptr);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto cleanup;
        }
    }
    /* unpack the number of info objects */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &ninfo, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto cleanup;
    }
    /* screen the count before it sizes an allocation - see the note in
     * pmix_server_publish */
    cnt = ninfo;
    if (!pmix_bfrop_count_fits(buf, ninfo, PMIX_INFO)) {
        rc = PMIX_ERR_BAD_PARAM;
        PMIX_ERROR_LOG(rc);
        goto cleanup;
    }
    cd->ninfo = ninfo;
    if (0 < ninfo) {
        PMIX_INFO_CREATE(cd->info, cd->ninfo);
        if (NULL == cd->info) {
            rc = PMIX_ERR_NOMEM;
            goto cleanup;
        }
    }
    /* unpack the array of info objects */
    if (0 < ninfo) {
        cnt = ninfo;
        PMIX_BFROPS_UNPACK(rc, peer, buf, cd->info, &cnt, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto cleanup;
        }
    }
    /* the host decides access to published data by the requester's
     * identity, so pass the pair recorded for this peer at connection,
     * in place of any the requester supplied */
    rc = pmix_server_add_requester_id(peer, &cd->info, &cd->ninfo);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto cleanup;
    }

    /* call the local server */
    pmix_strncpy(proc.nspace, peer->info->pname.nspace, PMIX_MAX_NSLEN);
    proc.rank = peer->info->pname.rank;
    rc = pmix_host_server.unpublish(&proc, cd->keys, cd->info, cd->ninfo, opcbfunc, cd);

cleanup:
    if (PMIX_SUCCESS != rc) {
        if (NULL != cd->keys) {
            PMIx_Argv_free(cd->keys);
        }
        if (NULL != cd->info) {
            PMIX_INFO_FREE(cd->info, cd->ninfo);
        }
        PMIX_RELEASE(cd);
    }
    return rc;
}

static void _spcbfunc(int sd, short args, void *cbdata)
{
    pmix_setup_caddy_t *cd = (pmix_setup_caddy_t *) cbdata;
    pmix_status_t rc;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    // pass along default status
    rc = cd->status;

    /* if it was successful, and there are IOF requests, then
     * register them now */
    if (PMIX_SUCCESS == cd->status) {
        rc = pmix_server_process_iof(cd, cd->nspace);
    }

    if (NULL != cd->spcbfunc) {
        cd->spcbfunc(rc, cd->nspace, cd->cbdata);
    }
    if (NULL != cd->nspace) {
        free(cd->nspace);
    }
    PMIX_RELEASE(cd);
}

void pmix_server_spcbfunc(pmix_status_t status, char nspace[], void *cbdata)
{
    pmix_setup_caddy_t *cd = (pmix_setup_caddy_t *) cbdata;
    pmix_server_caddy_t *scd;

    if (pmix_atomic_check_bool(&pmix_globals.progress_thread_stopped)) {
        /* the switchyard's caddy is parked on cbdata and scaddes does not
         * reach it, so releasing only this caddy strands it and the
         * retain it holds on the requesting peer. _spcbfunc, the arm that
         * would otherwise hand it to the spawn completion, never runs. */
        scd = (pmix_server_caddy_t *) cd->cbdata;
        if (NULL != scd) {
            PMIX_RELEASE(scd);
        }
        PMIX_RELEASE(cd);
        return;
    }

    cd->status = status;
    if (NULL != nspace) {
        cd->nspace = strdup(nspace);
    }
    PMIX_THREADSHIFT(cd, _spcbfunc);
}

void pmix_server_spawn_parser(pmix_peer_t *peer,
                              pmix_iof_channel_t *channels,
                              pmix_iof_flags_t *flags,
                              bool *inherit,
                              pmix_info_t *info,
                              size_t ninfo)
{
    size_t n;
    bool stdout_found = false, stderr_found = false, stddiag_found = false;

    /* run a quick check of the directives to see if any IOF
     * requests were included so we can set that up now - helps
     * to catch any early output - and a request for notification
     * of job termination so we can setup the event registration */
    *channels = PMIX_FWD_NO_CHANNELS;
    for (n = 0; n < ninfo; n++) {
        if (PMIX_CHECK_KEY(&info[n], PMIX_FWD_STDIN)) {
            if (PMIX_INFO_TRUE(&info[n])) {
                *channels |= PMIX_FWD_STDIN_CHANNEL;
            }
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_FWD_STDOUT)) {
            stdout_found = true;
            if (PMIX_INFO_TRUE(&info[n])) {
                *channels |= PMIX_FWD_STDOUT_CHANNEL;
            }
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_FWD_STDERR)) {
            stderr_found = true;
            if (PMIX_INFO_TRUE(&info[n])) {
                *channels |= PMIX_FWD_STDERR_CHANNEL;
            }
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_FWD_STDDIAG)) {
            stddiag_found = true;
            if (PMIX_INFO_TRUE(&info[n])) {
                *channels |= PMIX_FWD_STDDIAG_CHANNEL;
            }
        } else {
            pmix_iof_check_flags(&info[n], flags);
        }
    }
    /* we will construct any required iof request tracker upon completion of the spawn
     * as we need the nspace of the spawned application! */

    /* Naming ANY output channel decides the matter for all of them: the
     * request is taken verbatim, including a channel the requestor
     * explicitly turned off. Merging a partial request into an inherited
     * set would make "forward stdout" quietly mean something different
     * depending on what the parent job happened to be doing. */
    *inherit = !(stdout_found || stderr_found || stddiag_found);

    if (PMIX_PEER_IS_TOOL(peer)) {
        /* if the requestor is a tool, we default to forwarding all
         * output IO channels. A tool is not a member of a job, so it has
         * no parent whose settings it could inherit - this default IS its
         * setting, and it is what a launcher such as prun relies on. */
        if (!stdout_found) {
            *channels |= PMIX_FWD_STDOUT_CHANNEL;
        }
        if (!stderr_found) {
            *channels |= PMIX_FWD_STDERR_CHANNEL;
        }
        if (!stddiag_found) {
            *channels |= PMIX_FWD_STDDIAG_CHANNEL;
        }
        *inherit = false;
    }
}

/* Completion callback for a spawn we fork/exec'd ourselves. pfexec calls
 * this with the caddy IT owns (and releases on return), so all it does is
 * hand the result to the normal server completion path, which owns the
 * caddy holding the requester's callback and does the IOF registration
 * the requester asked for. */
static void pfexec_spcbfunc(pmix_status_t status, char nspace[], void *cbdata)
{
    pmix_setup_caddy_t *cd = (pmix_setup_caddy_t *) cbdata;

    pmix_server_spcbfunc(status, nspace, cd);
}

pmix_status_t pmix_server_spawn(pmix_peer_t *peer, pmix_buffer_t *buf,
                                pmix_spawn_cbfunc_t cbfunc,
                                void *cbdata)
{
    pmix_setup_caddy_t *cd, *fcd;
    int32_t cnt;
    pmix_status_t rc;
    pmix_proc_t proc;

    pmix_output_verbose(2, pmix_server_globals.spawn_output,
                        "recvd SPAWN from %s",
                        PMIX_PNAME_PRINT(&peer->info->pname));

    /* Our host is the first choice. Failing that we can still service the
     * request if we are able to fork/exec it ourselves: a launcher that
     * has no server attached is expected to launch locally rather than
     * refuse (the same rule PMIx_Spawn applies to a launcher's own
     * requests). pfexec is opened only by a launcher or scheduler tool,
     * so this flag is exactly the "can I fork/exec" question - a plain
     * PMIx server never has it set and its behavior is unchanged. */
    if (NULL == pmix_host_server.spawn && !pmix_pfexec_globals.initialized) {
        return PMIX_ERR_NOT_SUPPORTED;
    }
    /* Without a host, the children are started by this process, as this
     * process's user - so only a peer of that same user may ask for it */
    if (NULL == pmix_host_server.spawn &&
        (NULL == peer->info || peer->info->uid != geteuid())) {
        pmix_output_verbose(2, pmix_server_globals.spawn_output,
                            "refusing SPAWN: requestor is not our user");
        return PMIX_ERR_NO_PERMISSIONS;
    }

    /* setup */
    cd = PMIX_NEW(pmix_setup_caddy_t);
    if (NULL == cd) {
        return PMIX_ERR_NOMEM;
    }
    PMIX_RETAIN(peer);
    cd->peer = peer;
    cd->spcbfunc = cbfunc;
    cd->cbdata = cbdata;

    /* unpack the number of job-level directives */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &cd->ninfo, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(cd);
        return rc;
    }
    /* screen the count before it sizes an allocation - see the note in
     * pmix_server_publish. The parser below also walks this size_t rather
     * than the int32_t the unpack consumed */
    cnt = cd->ninfo;
    if (!pmix_bfrop_count_fits(buf, cd->ninfo, PMIX_INFO)) {
        rc = PMIX_ERR_BAD_PARAM;
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(cd);
        return rc;
    }
    if (0 < cd->ninfo) {
        PMIX_INFO_CREATE(cd->info, cd->ninfo);
        if (NULL == cd->info) {
            rc = PMIX_ERR_NOMEM;
            goto cleanup;
        }
        cnt = cd->ninfo;
        PMIX_BFROPS_UNPACK(rc, peer, buf, cd->info, &cnt, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto cleanup;
        }
        /* a user or group given by name - an owner, or an entry of the
         * job's access list - is resolved here, where it enters */
        {
            pmix_info_t *resolved = NULL;
            size_t nresolved = 0;

            rc = pmix_server_normalize_ids(cd->info, cd->ninfo, &resolved, &nresolved);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                goto cleanup;
            }
            if (NULL != resolved) {
                PMIX_INFO_FREE(cd->info, cd->ninfo);
                cd->info = resolved;
                cd->ninfo = nresolved;
            }
        }
    }
    /* run a quick check of the directives to see if any IOF
     * requests were included so we can set that up now - helps
     * to catch any early output - and a request for notification
     * of job termination so we can setup the event registration.
     *
     * This runs even when the request carried no directives at all,
     * which is a legal spawn (PMIx_Spawn(NULL, 0, ...)) and NOT the
     * same thing as a request that asked for no forwarding. Naming no
     * channel is precisely what decides the two defaults the parser
     * exists to apply: a client's child inherits its parent's
     * forwarding, and a tool's child gets every output channel because
     * a tool has no parent to inherit from. Skipping the parse left
     * both at the caddy's constructed state - forward nothing, inherit
     * nothing - so a tool such as prun that spawned without directives
     * never saw a line of its job's output. Both call sites in
     * PMIx_Spawn_nb parse unconditionally for the same reason. */
    pmix_server_spawn_parser(peer, &cd->channels, &cd->flags,
                             &cd->inherit_iof, cd->info, cd->ninfo);

    /* unpack the number of apps */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &cd->napps, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto cleanup;
    }
    /* the same screen: PMIx_App_create multiplies and constructs exactly
     * as PMIx_Info_create does, and scaddes walks this size_t when it
     * frees the array */
    cnt = cd->napps;
    if (!pmix_bfrop_count_fits(buf, cd->napps, PMIX_APP)) {
        rc = PMIX_ERR_BAD_PARAM;
        PMIX_ERROR_LOG(rc);
        goto cleanup;
    }
    /* unpack the array of apps */
    if (0 < cd->napps) {
        PMIX_APP_CREATE(cd->apps, cd->napps);
        if (NULL == cd->apps) {
            rc = PMIX_ERR_NOMEM;
            goto cleanup;
        }
        cnt = cd->napps;
        PMIX_BFROPS_UNPACK(rc, peer, buf, cd->apps, &cnt, PMIX_APP);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto cleanup;
        }
    }
    /* mark that we created the data */
    cd->copied = true;

    if (NULL != pmix_host_server.spawn) {
        /* pass the requester's identity to the host */
        rc = pmix_server_add_requester_id(peer, &cd->info, &cd->ninfo);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto cleanup;
        }
        /* call the local server */
        PMIX_LOAD_PROCID(&proc, peer->info->pname.nspace, peer->info->pname.rank);
        rc = pmix_host_server.spawn(&proc, cd->info, cd->ninfo,
                                    cd->apps, cd->napps,
                                    pmix_server_spcbfunc, cd);
        /* PMIX_OPERATION_SUCCEEDED cannot express this operation's result -
         * the namespace of the job that was started, which comes back
         * through the callback we always supply. Left alone it would reach
         * the requesting client as a synthesized status-only reply, which
         * that client reads as a success carrying no namespace */
        if (PMIX_UNLIKELY(PMIX_OPERATION_SUCCEEDED == rc)) {
            pmix_show_help("help-pmix-server.txt", "atomic-completion-unsupported",
                           true, "PMIx_Spawn from a client");
            rc = PMIX_ERR_NOT_SUPPORTED;
        }
    } else {
        /* No host to ask, so fork/exec it ourselves. pfexec takes a caddy
         * of its own and releases it when the spawn completes, so give it
         * a second one that BORROWS this one's apps and directives -
         * "copied" stays false on it so its destructor leaves them to
         * "cd", which owns them and outlives it. */
        fcd = PMIX_NEW(pmix_setup_caddy_t);
        if (NULL == fcd) {
            rc = PMIX_ERR_NOMEM;
            goto cleanup;
        }
        fcd->info = cd->info;
        fcd->ninfo = cd->ninfo;
        fcd->apps = cd->apps;
        fcd->napps = cd->napps;
        /* what the parser above worked out about output - pfexec reads
         * it to decide which of the child's streams to write, which the
         * request asked to be silent, and whether it named any channel
         * at all. Only these two: the formatting flags reach the spawned
         * namespace through the job description pfexec registers, and
         * copying them here would hand a second caddy the file and
         * directory strings this one frees. */
        fcd->channels = cd->channels;
        fcd->inherit_iof = cd->inherit_iof;
        fcd->spcbfunc = pfexec_spcbfunc;
        fcd->cbdata = cd;
        rc = pmix_pfexec_base_spawn_job(fcd);
        if (PMIX_SUCCESS != rc) {
            PMIX_RELEASE(fcd);
        }
    }

cleanup:
    if (PMIX_SUCCESS != rc) {
        if (NULL != cd->info) {
            PMIX_INFO_FREE(cd->info, cd->ninfo);
        }
        if (NULL != cd->apps) {
            PMIX_APP_FREE(cd->apps, cd->napps);
        }
        PMIX_RELEASE(cd);
    }
    return rc;
}
