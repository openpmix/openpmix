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
 * name further users and groups allowed to access it; a user
 * (pmix_user_t) belongs to groups. Everything here runs on the progress
 * thread, except the functions that touch nothing but their arguments -
 * pmix_server_access_check(), pmix_server_access_load() and the
 * pmix_user_t and pmix_access_t helpers - which a host keeping its own
 * copies calls from its own. */

#include "src/include/pmix_config.h"

#include <grp.h>
#include <pwd.h>
#include <stdlib.h>
#include <string.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif

#include "pmix_common.h"
#include "src/class/pmix_list.h"
#include "src/client/pmix_client_ops.h"
#include "src/include/pmix_globals.h"
#include "src/util/pmix_idname.h"
#include "src/util/pmix_output.h"

#include "pmix_server_ops.h"

/* ------------------------------------------------------------------ */
/* users, and the groups they belong to                                */
/* ------------------------------------------------------------------ */

static void ucon(pmix_user_t *p)
{
    p->uid = 0;
    p->gids = NULL;
    p->ngids = 0;
}
static void udes(pmix_user_t *p)
{
    free(p->gids);
}
PMIX_CLASS_INSTANCE(pmix_user_t, pmix_list_item_t, ucon, udes);

/* The users this server knows - see pmix_server_user_get(). Progress
 * thread only; a host keeps its own */
static pmix_list_t users;
static bool users_constructed = false;

/* the most groups one user may be found to belong to */
#define PMIX_ACCESS_MAX_GROUPS 65535

pmix_user_t *pmix_server_user_create(uid_t uid)
{
    pmix_user_t *user = PMIX_NEW(pmix_user_t);

    if (NULL != user) {
        user->uid = uid;
    }
    return user;
}

pmix_status_t pmix_server_user_refresh(pmix_user_t *user)
{
    struct passwd pw, *res = NULL;
    long bufsz;
    char *buf;
    gid_t *gids;
    int ng, n, rc, i;

    if (NULL == user) {
        return PMIX_ERR_BAD_PARAM;
    }
    bufsz = sysconf(_SC_GETPW_R_SIZE_MAX);
    if (0 >= bufsz) {
        bufsz = 16384;
    }
    buf = (char *) malloc((size_t) bufsz);
    if (NULL == buf) {
        return PMIX_ERR_NOMEM;
    }
    if (0 != getpwuid_r(user->uid, &pw, buf, (size_t) bufsz, &res) || NULL == res) {
        /* no account - no groups to be found */
        free(buf);
        free(user->gids);
        user->gids = NULL;
        user->ngids = 0;
        return PMIX_SUCCESS;
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
            gids = (gid_t *) malloc(((size_t) n + 1) * sizeof(gid_t));
            if (NULL != gids) {
                for (i = 0; i < n; i++) {
                    gids[i] = (gid_t) list[i];
                }
                free(user->gids);
                user->gids = gids;
                user->ngids = (uint16_t) n;
            }
            free(list);
            break;
        }
        free(list);
    }
    free(buf);
    return PMIX_SUCCESS;
}

pmix_user_t *pmix_server_user_get(uid_t uid)
{
    pmix_user_t *user;

    if (!users_constructed) {
        PMIX_CONSTRUCT(&users, pmix_list_t);
        users_constructed = true;
    }
    PMIX_LIST_FOREACH (user, &users, pmix_user_t) {
        if (user->uid == uid) {
            return user;
        }
    }
    user = pmix_server_user_create(uid);
    if (NULL != user) {
        pmix_list_append(&users, &user->super);
    }
    return user;
}

void pmix_server_user_add(uid_t uid)
{
    pmix_user_t *user;

    /* root and our own user are allowed everything - their groups are
     * never needed */
    if (0 == uid || geteuid() == uid) {
        return;
    }
    if (!users_constructed) {
        PMIX_CONSTRUCT(&users, pmix_list_t);
        users_constructed = true;
    }
    PMIX_LIST_FOREACH (user, &users, pmix_user_t) {
        if (user->uid == uid) {
            return;
        }
    }
    user = pmix_server_user_create(uid);
    if (NULL == user) {
        return;
    }
    (void) pmix_server_user_refresh(user);
    pmix_list_append(&users, &user->super);
}

pmix_status_t pmix_server_user_register(uid_t uid, const gid_t *gids, size_t ngids)
{
    pmix_user_t *user = NULL, *u;
    gid_t *copy;

    if (NULL == gids) {
        /* the host did not say - find out ourselves */
        pmix_server_user_add(uid);
        return PMIX_SUCCESS;
    }
    if (0 == uid || geteuid() == uid) {
        return PMIX_SUCCESS;
    }
    if (UINT16_MAX < ngids) {
        return PMIX_ERR_BAD_PARAM;
    }
    if (!users_constructed) {
        PMIX_CONSTRUCT(&users, pmix_list_t);
        users_constructed = true;
    }
    PMIX_LIST_FOREACH (u, &users, pmix_user_t) {
        if (u->uid == uid) {
            user = u;
            break;
        }
    }
    copy = (gid_t *) malloc((0 < ngids ? ngids : 1) * sizeof(gid_t));
    if (NULL == copy) {
        return PMIX_ERR_NOMEM;
    }
    if (0 < ngids) {
        memcpy(copy, gids, ngids * sizeof(gid_t));
    }
    if (NULL == user) {
        user = pmix_server_user_create(uid);
        if (NULL == user) {
            free(copy);
            return PMIX_ERR_NOMEM;
        }
        pmix_list_append(&users, &user->super);
    }
    /* the host's word replaces what we had - no lookup of our own */
    free(user->gids);
    user->gids = copy;
    user->ngids = (uint16_t) ngids;
    return PMIX_SUCCESS;
}

pmix_status_t pmix_server_gids_from_value(const pmix_value_t *val, gid_t **gids, size_t *ngids)
{
    pmix_data_array_t *da;
    pmix_value_t elem;
    uint32_t id;
    size_t n, esize;
    gid_t *out;
    pmix_status_t rc;

    *gids = NULL;
    *ngids = 0;
    if (PMIX_DATA_ARRAY != val->type) {
        rc = pmix_util_gid_from_value(val, &id);
        if (PMIX_SUCCESS != rc) {
            return rc;
        }
        out = (gid_t *) malloc(sizeof(gid_t));
        if (NULL == out) {
            return PMIX_ERR_NOMEM;
        }
        out[0] = (gid_t) id;
        *gids = out;
        *ngids = 1;
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
    case PMIX_STRING:
        esize = sizeof(char *);
        break;
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
    out = (gid_t *) malloc(da->size * sizeof(gid_t));
    if (NULL == out) {
        return PMIX_ERR_NOMEM;
    }
    for (n = 0; n < da->size; n++) {
        memset(&elem, 0, sizeof(elem));
        elem.type = da->type;
        memcpy(&elem.data, (char *) da->array + n * esize, esize);
        rc = pmix_util_gid_from_value(&elem, &id);
        if (PMIX_SUCCESS != rc) {
            free(out);
            return rc;
        }
        out[n] = (gid_t) id;
    }
    *gids = out;
    *ngids = da->size;
    return PMIX_SUCCESS;
}

void pmix_server_user_remove(uid_t uid)
{
    pmix_user_t *user, *next;

    if (!users_constructed) {
        return;
    }
    PMIX_LIST_FOREACH_SAFE (user, next, &users, pmix_user_t) {
        if (user->uid == uid) {
            pmix_list_remove_item(&users, &user->super);
            PMIX_RELEASE(user);
        }
    }
}

void pmix_server_access_finalize(void)
{
    if (users_constructed) {
        PMIX_LIST_DESTRUCT(&users);
        users_constructed = false;
    }
}

/* ------------------------------------------------------------------ */
/* the rule                                                            */
/* ------------------------------------------------------------------ */

static bool in_groups(const pmix_user_t *user, const pmix_access_t *job)
{
    size_t n, m;

    for (m = 0; NULL != user->gids && m < user->ngids; m++) {
        for (n = 0; n < job->ngids; n++) {
            if ((uint32_t) user->gids[m] == job->gids[n]) {
                return true;
            }
        }
    }
    return false;
}

pmix_status_t pmix_server_access_check(pmix_user_t *requester, const pmix_access_t *job)
{
    uid_t uid, owner;
    size_t n;

    if (NULL == requester) {
        return PMIX_ERR_BAD_PARAM;
    }
    uid = requester->uid;
    /* root, and the account this process runs as */
    if (0 == uid || geteuid() == uid) {
        return PMIX_SUCCESS;
    }
    if (NULL == job) {
        return PMIX_ERR_NO_PERMISSIONS;
    }
    /* the owner - with no owner known, our own identity stands in, and
     * that was answered above */
    owner = (PMIX_OWNER_UNKNOWN == job->source) ? geteuid() : job->uid;
    if (uid == owner) {
        return PMIX_SUCCESS;
    }
    /* users the owner allowed */
    for (n = 0; n < job->nuids; n++) {
        if ((uint32_t) uid == job->uids[n]) {
            return PMIX_SUCCESS;
        }
    }
    /* groups the owner allowed - and if the user is in none of them, its
     * groups may have changed since they were last looked up (or never
     * have been), so look again before refusing */
    if (0 == job->ngids) {
        return PMIX_ERR_NO_PERMISSIONS;
    }
    if (in_groups(requester, job)) {
        return PMIX_SUCCESS;
    }
    if (PMIX_SUCCESS == pmix_server_user_refresh(requester) && in_groups(requester, job)) {
        return PMIX_SUCCESS;
    }
    return PMIX_ERR_NO_PERMISSIONS;
}

bool pmix_server_access_permitted(uid_t uid, const pmix_namespace_t *nptr)
{
    pmix_user_t *user;

    /* root and our own user need no record */
    if (0 == uid || geteuid() == uid) {
        return true;
    }
    if (NULL == nptr) {
        return false;
    }
    user = pmix_server_user_get(uid);
    if (NULL == user) {
        return false;
    }
    return (PMIX_SUCCESS == pmix_server_access_check(user, &nptr->access));
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
    return pmix_server_access_permitted(peer->info->uid, nptr);
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
    uint32_t uid = 0;
    const pmix_proc_t *requestor = NULL;
    bool haveuid = false;
    size_t n;

    for (n = 0; n < ninfo; n++) {
        if (PMIX_CHECK_KEY(&info[n], PMIX_USERID)) {
            if (PMIX_SUCCESS != PMIx_Value_get_number(&info[n].value, &uid, PMIX_UINT32)) {
                return PMIX_ERR_BAD_PARAM;
            }
            haveuid = true;
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
    if (pmix_server_access_permitted((uid_t) uid, nptr)) {
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

pmix_status_t pmix_server_access_requester(const pmix_proc_t *requestor,
                                           const pmix_info_t *directives, size_t ndirs,
                                           pmix_peer_t **peer, bool *host, uid_t *uid)
{
    pmix_peer_t *rpeer;
    uint32_t u32 = 0;
    bool have_uid = false, relayed = false;
    size_t n;

    *peer = NULL;
    *host = false;
    *uid = (uid_t) -1;

    rpeer = (NULL == requestor) ? NULL : pmix_server_access_find_peer(requestor);
    if (NULL != rpeer) {
        *peer = rpeer;
        *uid = rpeer->info->uid;
        return PMIX_SUCCESS;
    }
    /* a request that is not from one of our clients or tools is our
     * host's own, and the host is not restricted - unless it says whom it
     * makes the request for, as a host relaying a request from another
     * node does (PMIX_MONITOR_PROXY, with the PMIX_USERID the requester's
     * own server gave it) */
    for (n = 0; NULL != directives && n < ndirs; n++) {
        if (PMIx_Check_key(directives[n].key, PMIX_USERID)) {
            if (PMIX_SUCCESS != PMIx_Value_get_number(&directives[n].value, &u32, PMIX_UINT32)) {
                return PMIX_ERR_BAD_PARAM;
            }
            have_uid = true;
        } else if (PMIx_Check_key(directives[n].key, PMIX_MONITOR_PROXY)) {
            relayed = true;
        }
    }
    if (have_uid) {
        *uid = (uid_t) u32;
        return PMIX_SUCCESS;
    }
    if (relayed) {
        /* relayed for somebody, but not saying who - not the host's own */
        return PMIX_ERR_NO_PERMISSIONS;
    }
    *host = true;
    *uid = geteuid();
    return PMIX_SUCCESS;
}

bool pmix_server_access_requester_may(pmix_peer_t *peer, bool host, uid_t uid,
                                      pmix_namespace_t *nptr)
{
    if (host) {
        return true;
    }
    if (NULL != peer) {
        return pmix_server_peer_permitted(peer, nptr);
    }
    return pmix_server_access_permitted(uid, nptr);
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

void pmix_server_access_construct(pmix_access_t *acc)
{
    memset(acc, 0, sizeof(*acc));
    acc->registered = false;
    acc->source = PMIX_OWNER_UNKNOWN;
    acc->uid = geteuid();
    acc->gid = getegid();
}

void pmix_server_access_destruct(pmix_access_t *acc)
{
    free(acc->uids);
    free(acc->gids);
    free(acc->apv_uids);
    free(acc->apv_gids);
    pmix_server_access_construct(acc);
}

pmix_status_t pmix_server_access_load(pmix_access_t *acc, const pmix_info_t *info, size_t ninfo)
{
    pmix_access_t tmp;
    pmix_info_t *resolved = NULL;
    size_t nresolved = 0;
    pmix_status_t rc;
    bool uids_given = false, gids_given = false;

    if (NULL == acc) {
        return PMIX_ERR_BAD_PARAM;
    }
    if (NULL == info || 0 == ninfo) {
        acc->registered = true;
        return PMIX_SUCCESS;
    }
    /* a host may name users and groups; only their numbers are kept */
    rc = pmix_server_normalize_ids(info, ninfo, &resolved, &nresolved);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    if (NULL != resolved) {
        info = resolved;
        ninfo = nresolved;
    }
    /* work on a copy, so a malformed entry changes nothing */
    tmp = *acc;
    tmp.uids = NULL;
    tmp.nuids = 0;
    tmp.gids = NULL;
    tmp.ngids = 0;
    rc = set_from(&tmp, info, ninfo, 0, &uids_given, &gids_given);
    if (NULL != resolved) {
        PMIx_Info_free(resolved, nresolved);
    }
    if (PMIX_SUCCESS != rc) {
        free(tmp.uids);
        free(tmp.gids);
        return rc;
    }
    acc->registered = true;
    acc->source = tmp.source;
    acc->uid = tmp.uid;
    acc->gid = tmp.gid;
    /* a list the registration did not mention is kept as it was */
    if (uids_given) {
        replace_ids(&acc->uids, &acc->nuids, tmp.uids, tmp.nuids);
    }
    if (gids_given) {
        replace_ids(&acc->gids, &acc->ngids, tmp.gids, tmp.ngids);
    }
    return PMIX_SUCCESS;
}

pmix_status_t pmix_server_access_set(pmix_namespace_t *nptr, const pmix_info_t *info,
                                     size_t ninfo)
{
    if (NULL == nptr) {
        return PMIX_ERR_BAD_PARAM;
    }
    return pmix_server_access_load(&nptr->access, info, ninfo);
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
    /* a user this server now knows - a tool, or the user a job's clients
     * run as */
    pmix_server_user_add(uid);
}
