/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/* Access to a namespace by user and group - see docs/security-plan.rst.
 *
 * A namespace has an owner (pmix_access_t in pmix_globals.h) and may
 * name further users and groups allowed to access it. Everything here
 * runs on the progress thread. */

#include "src/include/pmix_config.h"

#include <grp.h>
#include <pwd.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif

#include "pmix_common.h"
#include "src/class/pmix_list.h"
#include "src/client/pmix_client_ops.h"
#include "src/include/pmix_globals.h"
#include "src/util/pmix_output.h"

#include "pmix_server_ops.h"

/* ------------------------------------------------------------------ */
/* group membership, cached per user                                   */
/* ------------------------------------------------------------------ */

typedef struct {
    pmix_list_item_t super;
    uid_t uid;
    gid_t *groups;
    size_t ngroups;
    time_t stamp;
} pmix_access_groups_t;

static void agcon(pmix_access_groups_t *p)
{
    p->uid = 0;
    p->groups = NULL;
    p->ngroups = 0;
    p->stamp = 0;
}
static void agdes(pmix_access_groups_t *p)
{
    if (NULL != p->groups) {
        free(p->groups);
    }
}
static PMIX_CLASS_INSTANCE(pmix_access_groups_t, pmix_list_item_t, agcon, agdes);

static pmix_list_t group_cache;
static bool group_cache_constructed = false;

/* the most groups one user may be found to belong to */
#define PMIX_ACCESS_MAX_GROUPS 65536

/* Look up every group the user belongs to, primary and supplementary. A
 * user with no passwd entry belongs to none that can be found. */
static void lookup_groups(uid_t uid, gid_t **out, size_t *nout)
{
    struct passwd pw, *res = NULL;
    long bufsz;
    char *buf;
    int ng, n, rc, i;

    *out = NULL;
    *nout = 0;

    bufsz = sysconf(_SC_GETPW_R_SIZE_MAX);
    if (0 >= bufsz) {
        bufsz = 16384;
    }
    buf = (char *) malloc((size_t) bufsz);
    if (NULL == buf) {
        return;
    }
    if (0 != getpwuid_r(uid, &pw, buf, (size_t) bufsz, &res) || NULL == res) {
        free(buf);
        return;
    }

    for (ng = 64; ng <= PMIX_ACCESS_MAX_GROUPS; ng *= 2) {
#if defined(__APPLE__)
        /* macOS declares the list as int */
        int *list = (int *) malloc((size_t) ng * sizeof(int));
        if (NULL == list) {
            break;
        }
        n = ng;
        rc = getgrouplist(pw.pw_name, (int) pw.pw_gid, list, &n);
#else
        gid_t *list = (gid_t *) malloc((size_t) ng * sizeof(gid_t));
        if (NULL == list) {
            break;
        }
        n = ng;
        rc = getgrouplist(pw.pw_name, pw.pw_gid, list, &n);
#endif
        if (0 <= rc && 0 <= n && n <= ng) {
            *out = (gid_t *) malloc(((size_t) n + 1) * sizeof(gid_t));
            if (NULL != *out) {
                for (i = 0; i < n; i++) {
                    (*out)[i] = (gid_t) list[i];
                }
                *nout = (size_t) n;
            }
            free(list);
            break;
        }
        free(list);
    }
    free(buf);
}

pmix_status_t pmix_server_access_groups(uid_t uid, const gid_t **groups, size_t *ngroups)
{
    pmix_access_groups_t *ag, *found = NULL;
    time_t now = time(NULL);

    if (!group_cache_constructed) {
        PMIX_CONSTRUCT(&group_cache, pmix_list_t);
        group_cache_constructed = true;
    }
    PMIX_LIST_FOREACH (ag, &group_cache, pmix_access_groups_t) {
        if (ag->uid == uid) {
            found = ag;
            break;
        }
    }
    if (NULL != found && 0 < pmix_server_globals.access_group_timeout &&
        now - found->stamp >= (time_t) pmix_server_globals.access_group_timeout) {
        /* stale - look it up again */
        free(found->groups);
        found->groups = NULL;
        found->ngroups = 0;
        lookup_groups(uid, &found->groups, &found->ngroups);
        found->stamp = now;
    }
    if (NULL == found) {
        found = PMIX_NEW(pmix_access_groups_t);
        if (NULL == found) {
            return PMIX_ERR_NOMEM;
        }
        found->uid = uid;
        lookup_groups(uid, &found->groups, &found->ngroups);
        found->stamp = now;
        pmix_list_append(&group_cache, &found->super);
    }
    *groups = found->groups;
    *ngroups = found->ngroups;
    return PMIX_SUCCESS;
}

void pmix_server_access_finalize(void)
{
    if (group_cache_constructed) {
        PMIX_LIST_DESTRUCT(&group_cache);
        group_cache_constructed = false;
    }
}

/* ------------------------------------------------------------------ */
/* the rule                                                            */
/* ------------------------------------------------------------------ */

bool pmix_server_access_permitted(uid_t uid, gid_t gid, const pmix_namespace_t *nptr)
{
    const gid_t *groups;
    size_t n, m, ngroups;
    uid_t owner;

    /* root, and the account the server itself runs as */
    if (0 == uid || geteuid() == uid) {
        return true;
    }
    if (NULL == nptr) {
        return false;
    }
    /* the owner - with no owner known, the server's identity stands in,
     * and that was answered above */
    owner = (PMIX_OWNER_UNKNOWN == nptr->access.source) ? geteuid() : nptr->access.uid;
    if (uid == owner) {
        return true;
    }
    /* users the owner allowed */
    for (n = 0; n < nptr->access.nuids; n++) {
        if ((uint32_t) uid == nptr->access.uids[n]) {
            return true;
        }
    }
    /* groups the owner allowed - the requester's own group first, then
     * every group its user belongs to */
    if (0 == nptr->access.ngids) {
        return false;
    }
    for (n = 0; n < nptr->access.ngids; n++) {
        if ((uint32_t) gid == nptr->access.gids[n]) {
            return true;
        }
    }
    if (PMIX_SUCCESS != pmix_server_access_groups(uid, &groups, &ngroups)) {
        return false;
    }
    for (m = 0; m < ngroups; m++) {
        for (n = 0; n < nptr->access.ngids; n++) {
            if ((uint32_t) groups[m] == nptr->access.gids[n]) {
                return true;
            }
        }
    }
    return false;
}

bool pmix_server_peer_permitted(const pmix_peer_t *peer, const pmix_namespace_t *nptr)
{
    if (NULL == peer || NULL == peer->info) {
        return false;
    }
    /* a job's own processes always access their own job, and the
     * server's own namespace describes the server, not anyone's job */
    if (NULL != nptr && (peer->nptr == nptr ||
                         (NULL != pmix_globals.mypeer && pmix_globals.mypeer->nptr == nptr))) {
        return true;
    }
    return pmix_server_access_permitted(peer->info->uid, peer->info->gid, nptr);
}

bool pmix_server_peer_may_access_nspace(const pmix_peer_t *peer, const char *nspace)
{
    pmix_namespace_t *ns;

    if (NULL == nspace || PMIx_Nspace_invalid(nspace)) {
        return true;
    }
    PMIX_LIST_FOREACH (ns, &pmix_globals.nspaces, pmix_namespace_t) {
        if (NULL != ns->nspace && 0 == strcmp(nspace, ns->nspace)) {
            return pmix_server_peer_may_access(peer, ns);
        }
    }
    return true;
}

bool pmix_server_peer_may_access(const pmix_peer_t *peer, const pmix_namespace_t *nptr)
{
    /* a job the host never registered with us has no permissions here
     * to judge by - data for it comes from the host, and is checked
     * where it is held */
    if (NULL == nptr || !nptr->access.registered) {
        return true;
    }
    return pmix_server_peer_permitted(peer, nptr);
}

/* ------------------------------------------------------------------ */
/* acting on local processes                                           */
/* ------------------------------------------------------------------ */

pmix_peer_t *pmix_server_access_find_peer(const pmix_proc_t *proc)
{
    pmix_peer_t *peer;
    int n;

    if (NULL == proc) {
        return NULL;
    }
    for (n = 0; n < pmix_server_globals.clients.size; n++) {
        peer = (pmix_peer_t *) pmix_pointer_array_get_item(&pmix_server_globals.clients, n);
        /* a tool acting as a server lists the server it connected to
         * among its clients - that is not a requester to restrict */
        if (NULL == peer || peer == pmix_client_globals.myserver ||
            NULL == peer->info || NULL == peer->info->pname.nspace) {
            continue;
        }
        if (PMIX_CHECK_NSPACE(peer->info->pname.nspace, proc->nspace) &&
            peer->info->pname.rank == proc->rank) {
            return peer;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* data held on another node                                           */
/* ------------------------------------------------------------------ */

pmix_status_t pmix_server_access_approve(pmix_namespace_t *nptr, uid_t uid, gid_t gid)
{
    uint32_t *u, *g;
    size_t n;

    if (NULL == nptr) {
        return PMIX_ERR_BAD_PARAM;
    }
    for (n = 0; n < nptr->access.napv; n++) {
        if (nptr->access.apv_uids[n] == (uint32_t) uid &&
            nptr->access.apv_gids[n] == (uint32_t) gid) {
            return PMIX_SUCCESS;
        }
    }
    u = (uint32_t *) realloc(nptr->access.apv_uids, (nptr->access.napv + 1) * sizeof(uint32_t));
    if (NULL == u) {
        return PMIX_ERR_NOMEM;
    }
    nptr->access.apv_uids = u;
    g = (uint32_t *) realloc(nptr->access.apv_gids, (nptr->access.napv + 1) * sizeof(uint32_t));
    if (NULL == g) {
        return PMIX_ERR_NOMEM;
    }
    nptr->access.apv_gids = g;
    u[nptr->access.napv] = (uint32_t) uid;
    g[nptr->access.napv] = (uint32_t) gid;
    nptr->access.napv++;
    return PMIX_SUCCESS;
}

bool pmix_server_peer_may_use_copy(const pmix_peer_t *peer, const pmix_namespace_t *nptr)
{
    uid_t uid;
    size_t n;

    if (NULL == nptr) {
        return false;
    }
    if (nptr->access.registered) {
        return pmix_server_peer_permitted(peer, nptr);
    }
    if (NULL == peer || NULL == peer->info) {
        return false;
    }
    /* the server's own namespace describes the server, and is anyone's */
    if (NULL != pmix_globals.mypeer && pmix_globals.mypeer->nptr == nptr) {
        return true;
    }
    /* a job's own processes, root and our own user need no one's say-so */
    uid = peer->info->uid;
    if (peer->nptr == nptr || 0 == uid || geteuid() == uid) {
        return true;
    }
    for (n = 0; n < nptr->access.napv; n++) {
        if (nptr->access.apv_uids[n] == (uint32_t) uid &&
            nptr->access.apv_gids[n] == (uint32_t) peer->info->gid) {
            return true;
        }
    }
    return false;
}

bool pmix_server_peer_may_use_copy_nspace(const pmix_peer_t *peer, const char *nspace)
{
    pmix_namespace_t *ns;

    if (NULL == nspace || PMIx_Nspace_invalid(nspace)) {
        return true;
    }
    PMIX_LIST_FOREACH (ns, &pmix_globals.nspaces, pmix_namespace_t) {
        if (NULL != ns->nspace && 0 == strcmp(nspace, ns->nspace)) {
            return pmix_server_peer_may_use_copy(peer, ns);
        }
    }
    /* we hold nothing for it */
    return true;
}

pmix_status_t pmix_server_access_check_remote(const pmix_namespace_t *nptr,
                                              const pmix_info_t *info, size_t ninfo)
{
    uint32_t uid = 0, gid = 0;
    const pmix_proc_t *requestor = NULL;
    bool haveuid = false;
    size_t n;

    for (n = 0; n < ninfo; n++) {
        if (PMIX_CHECK_KEY(&info[n], PMIX_USERID)) {
            if (PMIX_SUCCESS != PMIx_Value_get_number(&info[n].value, &uid, PMIX_UINT32)) {
                return PMIX_ERR_BAD_PARAM;
            }
            haveuid = true;
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_GRPID)) {
            if (PMIX_SUCCESS != PMIx_Value_get_number(&info[n].value, &gid, PMIX_UINT32)) {
                return PMIX_ERR_BAD_PARAM;
            }
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_REQUESTOR)) {
            if (PMIX_PROC != info[n].value.type || NULL == info[n].value.data.proc) {
                return PMIX_ERR_BAD_PARAM;
            }
            requestor = info[n].value.data.proc;
        }
    }
    /* no requester named: the host is asking for itself */
    if (!haveuid) {
        return PMIX_SUCCESS;
    }
    /* a job's own processes read their own job */
    if (NULL != requestor && NULL != nptr &&
        0 == strncmp(requestor->nspace, nptr->nspace, PMIX_MAX_NSLEN)) {
        return PMIX_SUCCESS;
    }
    if (pmix_server_access_permitted((uid_t) uid, (gid_t) gid, nptr)) {
        return PMIX_SUCCESS;
    }
    return PMIX_ERR_NO_PERMISSIONS;
}

pmix_status_t pmix_server_access_identify(const pmix_peer_t *peer, pmix_info_t **info,
                                          size_t *ninfo)
{
    pmix_info_t *iptr;
    pmix_proc_t me;
    uint32_t uid, gid;
    size_t n, m, nkeep = 0;

    if (NULL == peer || NULL == peer->info || NULL == peer->info->pname.nspace) {
        return PMIX_ERR_BAD_PARAM;
    }
    for (n = 0; n < *ninfo; n++) {
        if (!PMIX_CHECK_KEY(&(*info)[n], PMIX_USERID) &&
            !PMIX_CHECK_KEY(&(*info)[n], PMIX_GRPID) &&
            !PMIX_CHECK_KEY(&(*info)[n], PMIX_REQUESTOR)) {
            ++nkeep;
        }
    }
    PMIX_INFO_CREATE(iptr, nkeep + 3);
    if (NULL == iptr) {
        return PMIX_ERR_NOMEM;
    }
    m = 0;
    for (n = 0; n < *ninfo; n++) {
        if (!PMIX_CHECK_KEY(&(*info)[n], PMIX_USERID) &&
            !PMIX_CHECK_KEY(&(*info)[n], PMIX_GRPID) &&
            !PMIX_CHECK_KEY(&(*info)[n], PMIX_REQUESTOR)) {
            PMIX_INFO_XFER(&iptr[m], &(*info)[n]);
            ++m;
        }
    }
    uid = (uint32_t) peer->info->uid;
    gid = (uint32_t) peer->info->gid;
    PMIX_LOAD_PROCID(&me, peer->info->pname.nspace, peer->info->pname.rank);
    PMIX_INFO_LOAD(&iptr[m], PMIX_USERID, &uid, PMIX_UINT32);
    PMIX_INFO_LOAD(&iptr[m + 1], PMIX_GRPID, &gid, PMIX_UINT32);
    PMIX_INFO_LOAD(&iptr[m + 2], PMIX_REQUESTOR, &me, PMIX_PROC);
    if (NULL != *info) {
        PMIX_INFO_FREE(*info, *ninfo);
    }
    *info = iptr;
    *ninfo = nkeep + 3;
    return PMIX_SUCCESS;
}

pmix_status_t pmix_server_access_filter_peers(const pmix_proc_t *requestor, pmix_list_t *peers,
                                              bool strict)
{
    pmix_peer_t *rpeer;
    pmix_peerlist_t *pl, *plnext;

    /* a request that is not from one of our clients or tools is our
     * host's own, and the host is not restricted */
    rpeer = pmix_server_access_find_peer(requestor);
    if (NULL == rpeer) {
        return PMIX_SUCCESS;
    }
    PMIX_LIST_FOREACH_SAFE (pl, plnext, peers, pmix_peerlist_t) {
        if (pmix_server_peer_permitted(rpeer, pl->peer->nptr)) {
            continue;
        }
        if (strict) {
            return PMIX_ERR_NO_PERMISSIONS;
        }
        pmix_list_remove_item(peers, &pl->super);
        PMIX_RELEASE(pl);
    }
    return PMIX_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* a namespace's owner and access list                                 */
/* ------------------------------------------------------------------ */

/* One id from a value holding a number of any integer type */
static pmix_status_t get_one_id(const pmix_value_t *val, uint32_t *id)
{
    if (PMIX_SUCCESS != PMIx_Value_get_number(val, id, PMIX_UINT32)) {
        return PMIX_ERR_BAD_PARAM;
    }
    return PMIX_SUCCESS;
}

/* The ids in a PMIX_ACCESS_USERIDS or PMIX_ACCESS_GRPIDS value: a data
 * array of numbers, or a single number. Names were resolved to numbers
 * where the value entered the library (pmix_server_normalize_ids). */
static pmix_status_t get_ids(const pmix_value_t *val, uint32_t **ids, size_t *nids)
{
    pmix_data_array_t *da;
    pmix_value_t elem;
    uint32_t *out;
    size_t n, esize;
    pmix_status_t rc;

    *ids = NULL;
    *nids = 0;
    if (PMIX_DATA_ARRAY != val->type) {
        out = (uint32_t *) malloc(sizeof(uint32_t));
        if (NULL == out) {
            return PMIX_ERR_NOMEM;
        }
        rc = get_one_id(val, &out[0]);
        if (PMIX_SUCCESS != rc) {
            free(out);
            return rc;
        }
        *ids = out;
        *nids = 1;
        return PMIX_SUCCESS;
    }
    da = val->data.darray;
    if (NULL == da || 0 == da->size) {
        return PMIX_SUCCESS;
    }
    if (NULL == da->array) {
        return PMIX_ERR_BAD_PARAM;
    }
    switch (da->type) {
    case PMIX_UINT32:
    case PMIX_INT32:
        esize = 4;
        break;
    case PMIX_UINT:
    case PMIX_INT:
        esize = sizeof(int);
        break;
    case PMIX_UINT64:
    case PMIX_INT64:
        esize = 8;
        break;
    case PMIX_UINT16:
    case PMIX_INT16:
        esize = 2;
        break;
    default:
        return PMIX_ERR_BAD_PARAM;
    }
    out = (uint32_t *) malloc(da->size * sizeof(uint32_t));
    if (NULL == out) {
        return PMIX_ERR_NOMEM;
    }
    for (n = 0; n < da->size; n++) {
        memset(&elem, 0, sizeof(elem));
        elem.type = da->type;
        memcpy(&elem.data, (char *) da->array + n * esize, esize);
        rc = get_one_id(&elem, &out[n]);
        if (PMIX_SUCCESS != rc) {
            free(out);
            return rc;
        }
    }
    *ids = out;
    *nids = da->size;
    return PMIX_SUCCESS;
}

static void replace_ids(uint32_t **dst, size_t *ndst, uint32_t *ids, size_t nids)
{
    if (NULL != *dst) {
        free(*dst);
    }
    *dst = ids;
    *ndst = nids;
}

static pmix_status_t set_from(pmix_access_t *acc, const pmix_info_t *info, size_t ninfo, int depth,
                              bool *uids_given, bool *gids_given)
{
    pmix_data_array_t *da;
    pmix_status_t rc;
    uint32_t id, *ids;
    size_t n, nids;

    for (n = 0; n < ninfo; n++) {
        if (PMIx_Check_key(info[n].key, PMIX_USERID)) {
            /* a host has always been able to register an owner of
             * another type and have it ignored - keep that, and let the
             * fallback owner stand. The access lists are strict */
            if (PMIX_SUCCESS == get_one_id(&info[n].value, &id)) {
                acc->uid = (uid_t) id;
                acc->source = PMIX_OWNER_REGISTERED;
            }
        } else if (PMIx_Check_key(info[n].key, PMIX_GRPID)) {
            if (PMIX_SUCCESS == get_one_id(&info[n].value, &id)) {
                acc->gid = (gid_t) id;
            }
        } else if (PMIx_Check_key(info[n].key, PMIX_ACCESS_USERIDS)) {
            rc = get_ids(&info[n].value, &ids, &nids);
            if (PMIX_SUCCESS != rc) {
                return rc;
            }
            replace_ids(&acc->uids, &acc->nuids, ids, nids);
            *uids_given = true;
        } else if (PMIx_Check_key(info[n].key, PMIX_ACCESS_GRPIDS)) {
            rc = get_ids(&info[n].value, &ids, &nids);
            if (PMIX_SUCCESS != rc) {
                return rc;
            }
            replace_ids(&acc->gids, &acc->ngids, ids, nids);
            *gids_given = true;
        } else if ((2 > depth && PMIx_Check_key(info[n].key, PMIX_ACCESS_PERMISSIONS)) ||
                   (0 == depth && PMIx_Check_key(info[n].key, PMIX_JOB_INFO_ARRAY))) {
            /* the permissions arrive as an array of pmix_info_t, and a
             * host may send the job's info - permissions included -
             * inside a job-info array */
            if (PMIX_DATA_ARRAY != info[n].value.type ||
                NULL == (da = info[n].value.data.darray) || PMIX_INFO != da->type ||
                (0 < da->size && NULL == da->array)) {
                if (PMIx_Check_key(info[n].key, PMIX_ACCESS_PERMISSIONS)) {
                    return PMIX_ERR_BAD_PARAM;
                }
                continue;
            }
            rc = set_from(acc, (pmix_info_t *) da->array, da->size, depth + 1, uids_given,
                          gids_given);
            if (PMIX_SUCCESS != rc) {
                return rc;
            }
        }
    }
    return PMIX_SUCCESS;
}

pmix_status_t pmix_server_access_set(pmix_namespace_t *nptr, const pmix_info_t *info,
                                     size_t ninfo)
{
    pmix_access_t acc;
    pmix_status_t rc;
    bool uids_given = false, gids_given = false;

    if (NULL == nptr) {
        return PMIX_ERR_BAD_PARAM;
    }
    if (NULL == info || 0 == ninfo) {
        nptr->access.registered = true;
        return PMIX_SUCCESS;
    }
    /* work on a copy, so a malformed entry changes nothing */
    acc = nptr->access;
    acc.uids = NULL;
    acc.nuids = 0;
    acc.gids = NULL;
    acc.ngids = 0;
    rc = set_from(&acc, info, ninfo, 0, &uids_given, &gids_given);
    if (PMIX_SUCCESS != rc) {
        free(acc.uids);
        free(acc.gids);
        return rc;
    }
    nptr->access.registered = true;
    nptr->access.source = acc.source;
    nptr->access.uid = acc.uid;
    nptr->access.gid = acc.gid;
    /* a list the registration did not mention is kept as it was */
    if (uids_given) {
        replace_ids(&nptr->access.uids, &nptr->access.nuids, acc.uids, acc.nuids);
    }
    if (gids_given) {
        replace_ids(&nptr->access.gids, &nptr->access.ngids, acc.gids, acc.ngids);
    }
    return PMIX_SUCCESS;
}

void pmix_server_access_set_owner(pmix_namespace_t *nptr, uid_t uid, gid_t gid,
                                  pmix_owner_source_t source)
{
    if (NULL == nptr) {
        return;
    }
    nptr->access.registered = true;
    /* a weaker source never overrides a stronger one */
    if (source < nptr->access.source) {
        return;
    }
    nptr->access.uid = uid;
    nptr->access.gid = gid;
    nptr->access.source = source;
}
