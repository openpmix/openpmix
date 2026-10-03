/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2008-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "pmix_config.h"

#include "pmix_common.h"
#include "src/include/pmix_globals.h"
#include "src/mca/pstat/base/base.h"
#include "src/mca/gds/base/base.h"
#include "src/mca/pstat/pstat.h"
#include "src/server/pmix_server_ops.h"
#include "src/util/pmix_error.h"

#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#if defined(__APPLE__)
#    include <libproc.h>
#    include <sys/sysctl.h>
#endif

/* Collect a device identifier into the caller's argv.
 *
 * The info array these helpers walk is the data array carried inside the
 * caller's monitor value, and for a request that arrived from a client
 * that array came straight off the wire - nothing between the unpack and
 * here validates it. So the declared type has to be checked before the
 * value is read: PMIX_DISK_ID and PMIX_NETWORK_ID are published as char*,
 * and reading data.string out of a value the sender typed as, say,
 * PMIX_SIZE reinterprets an integer as a pointer and hands it to strdup.
 */
static void collect_id(char ***argv, const pmix_info_t *info, bool *logged)
{
    pmix_status_t rc;

    if (PMIX_STRING != info->value.type || NULL == info->value.data.string) {
        rc = PMIX_ERR_BAD_PARAM;
    } else {
        /* the device list is a filter - dropping an entry silently would
         * widen the request to every device instead of the named one.
         * Append uniquely: it names a set of devices to report, so a
         * repeated ID must not make the device appear twice in the
         * results. The op's process targets are deduplicated the same
         * way, by pid (pmix_pstat_base_targets) */
        rc = PMIx_Argv_append_unique_nosize(argv, info->value.data.string);
    }
    /* at most one diagnostic per request. PMIX_ERROR_LOG writes to stream 0,
     * which is never off, and the array being walked is caller-supplied -
     * reporting every bad entry would let one malformed request write an
     * arbitrary number of lines to the server's stderr */
    if (PMIX_SUCCESS != rc && !*logged) {
        *logged = true;
        PMIX_ERROR_LOG(rc);
    }
}

void pmix_pstat_parse_procstats(pmix_procstats_t *pst,
                                pmix_info_t *info, size_t sz)
{
    size_t n;

    if (NULL == info) {
        PMIX_PROCSTATS_ALL(pst);
    } else {
        PMIX_PROCSTATS_INIT(pst);
        for (n = 0; n < sz; n++) {
            if (PMIx_Check_key(info[n].key, PMIX_CMD_LINE)) {
                pst->cmdline = true;
            } else if (PMIx_Check_key(info[n].key, PMIX_PROC_OS_STATE)) {
                pst->state = true;
            } else if (PMIx_Check_key(info[n].key, PMIX_PROC_TIME)) {
                pst->time = true;
            } else if (PMIx_Check_key(info[n].key, PMIX_PROC_PERCENT_CPU)) {
                pst->pctcpu = true;
            } else if (PMIx_Check_key(info[n].key, PMIX_PROC_PRIORITY)) {
                pst->pri = true;
            } else if (PMIx_Check_key(info[n].key, PMIX_PROC_NUM_THREADS)) {
                pst->nthreads = true;
            } else if (PMIx_Check_key(info[n].key, PMIX_PROC_PSS)) {
                pst->pss = true;
            } else if (PMIx_Check_key(info[n].key, PMIX_PROC_VSIZE)) {
                pst->vsize = true;
            } else if (PMIx_Check_key(info[n].key, PMIX_PROC_RSS)) {
                pst->rss = true;
            } else if (PMIx_Check_key(info[n].key, PMIX_PROC_PEAK_VSIZE)) {
                pst->pkvsize = true;
            } else if (PMIx_Check_key(info[n].key, PMIX_PROC_CPU)) {
                pst->cpu = true;
            }
        }
    }
}

void pmix_pstat_parse_dkstats(char ***disks, pmix_dkstats_t *dkst,
                              pmix_info_t *info, size_t sz)
{
    size_t n;
    bool logged = false;

    if (NULL == info) {
        PMIX_DKSTATS_ALL(dkst);
    } else {
        PMIX_DKSTATS_INIT(dkst);
        for (n = 0; n < sz; n++) {
            if (PMIx_Check_key(info[n].key, PMIX_DISK_ID)) {
                collect_id(disks, &info[n], &logged);

            } else if (PMIx_Check_key(info[n].key, PMIX_DISK_READ_COMPLETED)) {
                dkst->rdcompleted = true;

            } else if (PMIx_Check_key(info[n].key, PMIX_DISK_READ_MERGED)) {
                dkst->rdmerged = true;

            } else if (PMIx_Check_key(info[n].key, PMIX_DISK_READ_SECTORS)) {
                dkst->rdsectors = true;

            } else if (PMIx_Check_key(info[n].key, PMIX_DISK_READ_MILLISEC)) {
                dkst->rdms = true;

            } else if (PMIx_Check_key(info[n].key, PMIX_DISK_WRITE_COMPLETED)) {
                dkst->wrtcompleted = true;

            } else if (PMIx_Check_key(info[n].key, PMIX_DISK_WRITE_MERGED)) {
                dkst->wrtmerged = true;

            } else if (PMIx_Check_key(info[n].key, PMIX_DISK_WRITE_SECTORS)) {
                dkst->wrtsectors = true;

            } else if (PMIx_Check_key(info[n].key, PMIX_DISK_WRITE_MILLISEC)) {
                dkst->wrtms = true;

            } else if (PMIx_Check_key(info[n].key, PMIX_DISK_IO_IN_PROGRESS)) {
                dkst->ioprog = true;

            } else if (PMIx_Check_key(info[n].key, PMIX_DISK_IO_MILLISEC)) {
                dkst->ioms = true;

            } else if (PMIx_Check_key(info[n].key, PMIX_DISK_IO_WEIGHTED)) {
                dkst->ioweight = true;
            }
        }
    }
}

void pmix_pstat_parse_netstats(char ***nets, pmix_netstats_t *netst,
                               pmix_info_t *info, size_t sz)
{
    size_t n;
    bool logged = false;

    if (NULL == info) {
        PMIX_NETSTATS_ALL(netst);
    } else {
        PMIX_NETSTATS_INIT(netst);
        for (n = 0; n < sz; n++) {
            if (PMIx_Check_key(info[n].key, PMIX_NETWORK_ID)) {
                collect_id(nets, &info[n], &logged);

            } else if (PMIx_Check_key(info[n].key, PMIX_NET_RECVD_BYTES)) {
                netst->rcvdb = true;

            } else if (PMIx_Check_key(info[n].key, PMIX_NET_RECVD_PCKTS)) {
                netst->rcvdp = true;

            } else if (PMIx_Check_key(info[n].key, PMIX_NET_RECVD_ERRS)) {
                netst->rcvde = true;

            } else if (PMIx_Check_key(info[n].key, PMIX_NET_SENT_BYTES)) {
                netst->sntb = true;

            } else if (PMIx_Check_key(info[n].key, PMIX_NET_SENT_PCKTS)) {
                netst->sntp = true;

            } else if (PMIx_Check_key(info[n].key, PMIX_NET_SENT_ERRS)) {
                netst->snte = true;
            }
        }
    }
}

void pmix_pstat_parse_ndstats(pmix_ndstats_t *ndst,
                              pmix_info_t *info, size_t sz)
{
    size_t n;

    if (NULL == info) {
        PMIX_NDSTATS_ALL(ndst);
    } else {
        PMIX_NDSTATS_INIT(ndst);
        for (n = 0; n < sz; n++) {
            if (PMIx_Check_key(info[n].key, PMIX_NODE_LOAD_AVG)) {
                ndst->la = true;

            } else if (PMIx_Check_key(info[n].key, PMIX_NODE_LOAD_AVG5)) {
                ndst->la5 = true;

            } else if (PMIx_Check_key(info[n].key, PMIX_NODE_LOAD_AVG15)) {
                ndst->la15 = true;

            } else if (PMIx_Check_key(info[n].key, PMIX_NODE_MEM_TOTAL)) {
                ndst->mtot = true;

            } else if (PMIx_Check_key(info[n].key, PMIX_NODE_MEM_FREE)) {
                ndst->mfree = true;

            } else if (PMIx_Check_key(info[n].key, PMIX_NODE_MEM_BUFFERS)) {
                ndst->mbuf = true;

            } else if (PMIx_Check_key(info[n].key, PMIX_NODE_MEM_CACHED)) {
                ndst->mcached = true;

            } else if (PMIx_Check_key(info[n].key, PMIX_NODE_MEM_SWAP_CACHED)) {
                ndst->mswapcached = true;

            } else if (PMIx_Check_key(info[n].key, PMIX_NODE_SWAP_TOTAL)) {
                ndst->mswaptot = true;

            } else if (PMIx_Check_key(info[n].key, PMIX_NODE_MEM_SWAP_FREE)) {
                ndst->mswapfree = true;

            } else if (PMIx_Check_key(info[n].key, PMIX_NODE_MEM_MAPPED)) {
                ndst->mmap = true;
            }
        }
    }
}


/* ------------------------------------------------------------------ */
/* the processes a monitor samples                                     */
/* ------------------------------------------------------------------ */

pmix_status_t pmix_pstat_base_pid_owner(pid_t pid, uid_t *owner)
{
    if (0 >= pid) {
        return PMIX_ERR_NOT_FOUND;
    }
#if defined(__linux__)
    {
        char path[64];
        struct stat st;

        (void) snprintf(path, sizeof(path), "/proc/%d", (int) pid);
        if (0 != stat(path, &st)) {
            return PMIX_ERR_NOT_FOUND;
        }
        *owner = st.st_uid;
        return PMIX_SUCCESS;
    }
#elif defined(__APPLE__)
    {
        /* not proc_pidinfo, which refuses another user's process to
         * anyone but root */
        struct kinfo_proc kp;
        size_t len = sizeof(kp);
        int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, (int) pid};

        if (0 != sysctl(mib, 4, &kp, &len, NULL, 0) || sizeof(kp) != len) {
            return PMIX_ERR_NOT_FOUND;
        }
        *owner = kp.kp_eproc.e_ucred.cr_uid;
        return PMIX_SUCCESS;
    }
#else
    PMIX_HIDE_UNUSED_PARAMS(owner);
    return PMIX_ERR_NOT_SUPPORTED;
#endif
}

pmix_status_t pmix_pstat_base_pids_of(uid_t uid, pid_t **pids, size_t *npids)
{
    pid_t *out = NULL;
    size_t n = 0, cap = 0;

    *pids = NULL;
    *npids = 0;
#if defined(__linux__)
    {
        DIR *dp;
        struct dirent *ep;
        pid_t *tmp;
        struct stat st;
        char *end;
        long val;
        int dfd;

        dp = opendir("/proc");
        if (NULL == dp) {
            return PMIX_ERR_NOT_SUPPORTED;
        }
        dfd = dirfd(dp);
        while (NULL != (ep = readdir(dp))) {
            val = strtol(ep->d_name, &end, 10);
            if ('\0' != *end || end == ep->d_name || 0 >= val) {
                continue;
            }
            if (0 != fstatat(dfd, ep->d_name, &st, 0) || st.st_uid != uid) {
                continue;
            }
            if (n == cap) {
                cap = (0 == cap) ? 64 : 2 * cap;
                tmp = (pid_t *) realloc(out, cap * sizeof(pid_t));
                if (NULL == tmp) {
                    free(out);
                    closedir(dp);
                    return PMIX_ERR_NOMEM;
                }
                out = tmp;
            }
            out[n++] = (pid_t) val;
        }
        closedir(dp);
    }
#elif defined(__APPLE__)
    {
        int nb, m;

        /* sized by a first call, with room for processes started since */
        nb = proc_listpids(PROC_UID_ONLY, (uint32_t) uid, NULL, 0);
        if (0 >= nb) {
            return PMIX_SUCCESS;
        }
        cap = (size_t) nb / sizeof(pid_t) + 32;
        out = (pid_t *) calloc(cap, sizeof(pid_t));
        if (NULL == out) {
            return PMIX_ERR_NOMEM;
        }
        nb = proc_listpids(PROC_UID_ONLY, (uint32_t) uid, out, (int) (cap * sizeof(pid_t)));
        if (0 >= nb) {
            free(out);
            return PMIX_SUCCESS;
        }
        for (m = 0; m < nb / (int) sizeof(pid_t); m++) {
            if (0 < out[m]) {
                out[n++] = out[m];
            }
        }
    }
#else
    PMIX_HIDE_UNUSED_PARAMS(uid, cap);
    return PMIX_ERR_NOT_SUPPORTED;
#endif
    if (0 == n) {
        free(out);
        return PMIX_SUCCESS;
    }
    *pids = out;
    *npids = n;
    return PMIX_SUCCESS;
}

pmix_status_t pmix_pstat_base_sample_targets(void *answer, pmix_pstat_op_t *op,
                                             pmix_pstat_base_sample_fn_t fn)
{
    pmix_pstat_target_t *tgt;
    pid_t *pids;
    size_t npids, m;
    pmix_status_t rc;

    PMIX_LIST_FOREACH (tgt, &op->targets, pmix_pstat_target_t) {
        if (-1 != tgt->pid) {
            rc = fn(answer, tgt, tgt->pid, &op->pstats);
            /* PMIX_ERR_NOT_FOUND: the process is gone, or is no longer the
             * one meant. That is ordinary, and the rest still have data to
             * report, so skip it rather than abandoning the whole sample */
            if (PMIX_SUCCESS != rc && PMIX_ERR_NOT_FOUND != rc) {
                return rc;
            }
            continue;
        }
        rc = pmix_pstat_base_pids_of(tgt->owner, &pids, &npids);
        if (PMIX_SUCCESS != rc) {
            return rc;
        }
        for (m = 0; m < npids; m++) {
            rc = fn(answer, tgt, pids[m], &op->pstats);
            if (PMIX_SUCCESS != rc && PMIX_ERR_NOT_FOUND != rc) {
                free(pids);
                return rc;
            }
        }
        free(pids);
    }
    return PMIX_SUCCESS;
}

/* the pid the host recorded for a PMIx process (PMIX_PROC_PID) */
static bool recorded_pid(const pmix_proc_t *name, pid_t *pid)
{
    pmix_cb_t cb;
    pmix_kval_t *kv;
    pmix_info_t optional;
    pmix_status_t rc;
    bool found = false;

    PMIX_INFO_LOAD(&optional, PMIX_OPTIONAL, NULL, PMIX_BOOL);
    PMIX_CONSTRUCT(&cb, pmix_cb_t);
    cb.proc = (pmix_proc_t *) name;
    cb.key = PMIX_PROC_PID;
    cb.info = &optional;
    cb.ninfo = 1;
    PMIX_GDS_FETCH_KV(rc, pmix_globals.mypeer, &cb);
    if (PMIX_SUCCESS == rc || PMIX_OPERATION_SUCCEEDED == rc) {
        kv = (pmix_kval_t *) pmix_list_remove_first(&cb.kvs);
        /* PMIx_Value_get_number reads value->type without screening the
         * pointer, so the kval has to carry a value at all */
        if (NULL != kv && NULL != kv->value &&
            PMIX_SUCCESS == PMIx_Value_get_number(kv->value, pid, PMIX_PID) && 0 < *pid) {
            found = true;
        }
        if (NULL != kv) {
            PMIX_RELEASE(kv);
        }
    }
    cb.key = NULL;
    cb.proc = NULL;
    cb.info = NULL;
    cb.ninfo = 0;
    PMIX_DESTRUCT(&cb);
    PMIX_INFO_DESTRUCT(&optional);
    return found;
}

/* a pid is sampled once; so is "every process of" one owner */
static bool have_target(pmix_list_t *targets, pid_t pid, uid_t owner)
{
    pmix_pstat_target_t *t;

    PMIX_LIST_FOREACH (t, targets, pmix_pstat_target_t) {
        if (t->pid == pid && (-1 != pid || t->owner == owner)) {
            return true;
        }
    }
    return false;
}

static pmix_status_t add_target(pmix_list_t *targets, const pmix_proc_t *name, pid_t pid,
                                uid_t owner)
{
    pmix_pstat_target_t *t;

    if (have_target(targets, pid, owner)) {
        return PMIX_SUCCESS;
    }
    t = PMIX_NEW(pmix_pstat_target_t);
    if (NULL == t) {
        return PMIX_ERR_NOMEM;
    }
    if (NULL != name) {
        t->named = true;
        PMIX_LOAD_PROCID(&t->name, name->nspace, name->rank);
    }
    t->pid = pid;
    t->owner = owner;
    pmix_list_append(targets, &t->super);
    return PMIX_SUCCESS;
}

/* one PMIx process of this node, at the pid its host recorded - skipped
 * if there is none */
static pmix_status_t add_named(pmix_list_t *targets, pmix_namespace_t *nptr,
                               pmix_rank_info_t *info)
{
    pmix_proc_t name;
    pid_t pid;

    PMIX_LOAD_PROCID(&name, nptr->nspace, info->pname.rank);
    if (!recorded_pid(&name, &pid)) {
        return PMIX_SUCCESS;
    }
    /* the process must still be the user's the host registered it as when
     * it is read - the recorded pid outlives the process, and is reused */
    return add_target(targets, &name, pid, info->uid);
}

/* every local process of this job that its host recorded a pid for */
static pmix_status_t add_job(pmix_list_t *targets, pmix_namespace_t *nptr)
{
    pmix_rank_info_t *info;
    pmix_status_t rc;

    PMIX_LIST_FOREACH (info, &nptr->ranks, pmix_rank_info_t) {
        rc = add_named(targets, nptr, info);
        if (PMIX_SUCCESS != rc) {
            return rc;
        }
    }
    return PMIX_SUCCESS;
}

static pmix_namespace_t *find_nspace(const char *nspace)
{
    pmix_namespace_t *ns;

    /* an exact comparison - PMIX_CHECK_NSPACE treats an empty name as a
     * wildcard */
    PMIX_LIST_FOREACH (ns, &pmix_globals.nspaces, pmix_namespace_t) {
        if (NULL != ns->nspace && 0 == strncmp(ns->nspace, nspace, PMIX_MAX_NSLEN)) {
            return ns;
        }
    }
    return NULL;
}

pmix_status_t pmix_pstat_base_targets(const pmix_proc_t *requestor,
                                      const pmix_info_t directives[], size_t ndirs,
                                      pmix_list_t *targets)
{
    pmix_peer_t *rpeer;
    pmix_namespace_t *nptr;
    pmix_proc_t *procs;
    pmix_node_pid_t *ppid;
    pmix_rank_info_t *info;
    bool host, given = false;
    uid_t uid, owner;
    size_t n, m, sz;
    pmix_status_t rc;

    rc = pmix_server_access_requester(requestor, directives, ndirs, &rpeer, &host, &uid);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }

    for (n = 0; NULL != directives && n < ndirs; n++) {
        if (PMIx_Check_key(directives[n].key, PMIX_MONITOR_TARGET_PROCS)) {
            if (PMIX_DATA_ARRAY != directives[n].value.type ||
                NULL == directives[n].value.data.darray ||
                PMIX_PROC != directives[n].value.data.darray->type ||
                NULL == directives[n].value.data.darray->array) {
                return PMIX_ERR_BAD_PARAM;
            }
            given = true;
            procs = (pmix_proc_t *) directives[n].value.data.darray->array;
            sz = directives[n].value.data.darray->size;
            for (m = 0; m < sz; m++) {
                nptr = find_nspace(procs[m].nspace);
                if (NULL == nptr) {
                    /* not a job of this node's - the host covers others */
                    continue;
                }
                if (!pmix_server_access_requester_may(rpeer, host, uid, nptr)) {
                    return PMIX_ERR_NO_PERMISSIONS;
                }
                if (PMIX_RANK_WILDCARD == procs[m].rank) {
                    rc = add_job(targets, nptr);
                } else {
                    /* only a rank of this node's */
                    rc = PMIX_SUCCESS;
                    PMIX_LIST_FOREACH (info, &nptr->ranks, pmix_rank_info_t) {
                        if (info->pname.rank == procs[m].rank) {
                            rc = add_named(targets, nptr, info);
                            break;
                        }
                    }
                }
                if (PMIX_SUCCESS != rc) {
                    return rc;
                }
            }

        } else if (PMIx_Check_key(directives[n].key, PMIX_MONITOR_TARGET_PIDS)) {
            if (PMIX_DATA_ARRAY != directives[n].value.type ||
                NULL == directives[n].value.data.darray ||
                PMIX_NODE_PID != directives[n].value.data.darray->type ||
                NULL == directives[n].value.data.darray->array) {
                return PMIX_ERR_BAD_PARAM;
            }
            given = true;
            ppid = (pmix_node_pid_t *) directives[n].value.data.darray->array;
            sz = directives[n].value.data.darray->size;
            for (m = 0; m < sz; m++) {
                if (ppid[m].nodeid != pmix_globals.nodeid &&
                    !pmix_check_local(ppid[m].hostname)) {
                    continue;
                }
                if (-1 == ppid[m].pid) {
                    /* every process with the requester's uid - found
                     * afresh at each sample, as processes come and go */
                    rc = add_target(targets, NULL, -1, uid);
                } else {
                    rc = pmix_pstat_base_pid_owner(ppid[m].pid, &owner);
                    if (PMIX_ERR_NOT_FOUND == rc) {
                        /* no such process - nothing to report */
                        continue;
                    }
                    if (PMIX_SUCCESS != rc) {
                        return rc;
                    }
                    if (!host && owner != uid) {
                        return PMIX_ERR_NO_PERMISSIONS;
                    }
                    /* it must still be this owner's when it is read */
                    rc = add_target(targets, NULL, ppid[m].pid, owner);
                }
                if (PMIX_SUCCESS != rc) {
                    return rc;
                }
            }
        }
    }

    if (!given) {
        /* every PMIx process of this node, in the jobs the requester may
         * access */
        PMIX_LIST_FOREACH (nptr, &pmix_globals.nspaces, pmix_namespace_t) {
            if (NULL == nptr->nspace ||
                !pmix_server_access_requester_may(rpeer, host, uid, nptr)) {
                continue;
            }
            rc = add_job(targets, nptr);
            if (PMIX_SUCCESS != rc) {
                return rc;
            }
        }
    }
    return PMIX_SUCCESS;
}

pmix_status_t pmix_pstat_base_may_add(const pmix_proc_t *requestor)
{
    pmix_pstat_op_t *op;
    int held = 0;

    if (NULL == requestor ||
        (0 == strncmp(requestor->nspace, pmix_globals.myid.nspace, PMIX_MAX_NSLEN) &&
         requestor->rank == pmix_globals.myid.rank)) {
        return PMIX_SUCCESS;
    }
    /* compared exactly - PMIX_CHECK_NSPACE takes an empty namespace as
     * a wildcard */
    PMIX_LIST_FOREACH (op, &pmix_pstat_base.ops, pmix_pstat_op_t) {
        if (0 == strncmp(op->requestor.nspace, requestor->nspace, PMIX_MAX_NSLEN) &&
            op->requestor.rank == requestor->rank) {
            ++held;
        }
    }
    if (held >= pmix_pstat_base.max_per_peer) {
        pmix_output_verbose(2, pmix_pstat_base_framework.framework_output,
                            "pstat: %s already holds %d periodic monitors - refused",
                            PMIX_NAME_PRINT(requestor), held);
        return PMIX_ERR_OUT_OF_RESOURCE;
    }
    return PMIX_SUCCESS;
}

void pmix_pstat_base_peer_lost(struct pmix_peer_t *p)
{
    pmix_peer_t *peer = (pmix_peer_t *) p;
    pmix_pstat_op_t *op, *opnext;
    pmix_peer_t *pr;
    int n;

    if (NULL == pmix_pstat_base.evbase || NULL == peer || NULL == peer->info ||
        NULL == peer->info->pname.nspace) {
        return;
    }
    /* A process and the clones it fork/exec'd share one name, and the
     * monitors are the name's until the last of them leaves */
    for (n = 0; n < pmix_server_globals.clients.size; n++) {
        pr = (pmix_peer_t *) pmix_pointer_array_get_item(&pmix_server_globals.clients, n);
        if (NULL == pr || pr == peer || NULL == pr->info || pr->finalized || 0 > pr->sd) {
            continue;
        }
        if (0 == strncmp(pr->info->pname.nspace, peer->info->pname.nspace, PMIX_MAX_NSLEN) &&
            pr->info->pname.rank == peer->info->pname.rank) {
            return;
        }
    }
    PMIX_LIST_FOREACH_SAFE (op, opnext, &pmix_pstat_base.ops, pmix_pstat_op_t) {
        /* compared exactly, as a cancel is - PMIX_CHECK_NSPACE treats an
         * empty name as a wildcard */
        if (0 == strncmp(op->requestor.nspace, peer->info->pname.nspace, PMIX_MAX_NSLEN) &&
            op->requestor.rank == peer->info->pname.rank) {
            pmix_list_remove_item(&pmix_pstat_base.ops, &op->super);
            PMIX_RELEASE(op); /* opdes stops the timer */
        }
    }
}
