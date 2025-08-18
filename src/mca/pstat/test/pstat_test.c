/*
 * Copyright (c) 2004-2008 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2007-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2008      Sun Microsystems, Inc.  All rights reserved.
 *
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "pmix_config.h"
#include "pmix_common.h"

#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif
#include <string.h>
#ifdef HAVE_SYS_TIME_H
#    include <sys/time.h>
#endif

#include "src/include/pmix_globals.h"
#include "src/mca/pstat/base/base.h"
#include "src/mca/pstat/pstat.h"
#include "src/server/pmix_server_ops.h"
#include "src/util/pmix_printf.h"

#include "pstat_test.h"

/*
 * API functions
 */
static pmix_status_t module_init(void);
static pmix_status_t query(pmix_proc_t *requestor,
                           const pmix_info_t *monitor, pmix_status_t error,
                           const pmix_info_t directives[], size_t ndirs,
                           pmix_info_t **results, size_t *nresults);
static pmix_status_t module_fini(void);

/*
 * Plinux pstat module
 */
const pmix_pstat_base_module_t pmix_pstat_test_module = {
    /* Initialization function */
    .init = module_init,
    .query = query,
    .finalize = module_fini
};


static pmix_status_t module_init(void)
{
    return PMIX_SUCCESS;
}

static pmix_status_t module_fini(void)
{
    return PMIX_SUCCESS;
}


static pmix_status_t proc_stat(void *answer, pmix_peer_t *peer,
                               pmix_procstats_t *pst)
{
    pmix_proc_t proc;
    pmix_status_t rc;
    char state[2];
    int32_t i32;
    float fval;
    uint16_t u16;
    void *cache;
    time_t sample_time;
    struct timeval evtime;
    pmix_data_array_t darray;

    time(&sample_time);
    cache = PMIx_Info_list_start();

    // start with the proc ID
    PMIx_Load_procid(&proc, peer->info->pname.nspace, peer->info->pname.rank);
    rc = PMIx_Info_list_add(cache, PMIX_PROCID, &proc, PMIX_PROC);
    if (PMIX_SUCCESS != rc) {
        PMIx_Info_list_release(cache);
        return rc;
    }
    // add the pid
    rc = PMIx_Info_list_add(cache, PMIX_PROC_PID, &peer->info->pid, PMIX_PID);
    if (PMIX_SUCCESS != rc) {
        PMIx_Info_list_release(cache);
        return rc;
    }
    // add the hostname
    rc = PMIx_Info_list_add(cache, PMIX_HOSTNAME, pmix_globals.hostname, PMIX_STRING);
    if (PMIX_SUCCESS != rc) {
        PMIx_Info_list_release(cache);
        return rc;
    }

    /* save the cmd name */
    if (pst->cmdline) {
        rc = PMIx_Info_list_add(cache, PMIX_CMD_LINE, "test-stats", PMIX_STRING);
        if (PMIX_SUCCESS != rc) {
            PMIx_Info_list_release(cache);
            return rc;
        }
    }
    state[0] = 'R';
    state[1] = '\0';
    if (pst->state) {
        rc = PMIx_Info_list_add(cache, PMIX_PROC_OS_STATE, state, PMIX_STRING);
        if (PMIX_SUCCESS != rc) {
            PMIx_Info_list_release(cache);
            return rc;
        }
    }

    evtime.tv_sec = 1234;
    evtime.tv_usec = 5678;
    if (pst->time) {
        rc = PMIx_Info_list_add(cache, PMIX_PROC_TIME, (void*)&evtime, PMIX_TIMEVAL);
        if (PMIX_SUCCESS != rc) {
            PMIx_Info_list_release(cache);
            return rc;
        }
    }

    /* save the priority */
    if (pst->pri) {
        i32 = 5;
        rc = PMIx_Info_list_add(cache, PMIX_PROC_PRIORITY, &i32, PMIX_INT32);
        if (PMIX_SUCCESS != rc) {
            PMIx_Info_list_release(cache);
            return rc;
        }
    }
    /* get number of threads */
    if (pst->nthreads) {
        u16 = 10;
        rc = PMIx_Info_list_add(cache, PMIX_PROC_NUM_THREADS, &u16, PMIX_UINT16);
        if (PMIX_SUCCESS != rc) {
            PMIx_Info_list_release(cache);
            return rc;
        }
    }

    /* finally - get the processor */
    if (pst->cpu) {
        u16 = 2;
        rc = PMIx_Info_list_add(cache, PMIX_PROC_CPU, &u16, PMIX_UINT16);
        if (PMIX_SUCCESS != rc) {
            PMIx_Info_list_release(cache);
            return rc;
        }
    }

    if (pst->pkvsize) {
        fval = 1.38;
        rc = PMIx_Info_list_add(cache, PMIX_PROC_PEAK_VSIZE, &fval, PMIX_FLOAT);
        if (PMIX_SUCCESS != rc) {
            PMIx_Info_list_release(cache);
            return rc;
        }
    }
    if (pst->vsize) {
        fval = 7.92;
        rc = PMIx_Info_list_add(cache, PMIX_PROC_VSIZE, &fval, PMIX_FLOAT);
        if (PMIX_SUCCESS != rc) {
            PMIx_Info_list_release(cache);
            return rc;
        }
    }

    if (pst->rss) {
        fval = 3.01;
        rc = PMIx_Info_list_add(cache, PMIX_PROC_RSS, &fval, PMIX_FLOAT);
        if (PMIX_SUCCESS != rc) {
            PMIx_Info_list_release(cache);
            return rc;
        }
    }

    if (pst->pss) {
        fval = 0.32;
        rc = PMIx_Info_list_add(cache, PMIX_PROC_PSS, &fval, PMIX_FLOAT);
        if (PMIX_SUCCESS != rc) {
            PMIx_Info_list_release(cache);
            return rc;
        }
    }

    // record the sample time
    rc = PMIx_Info_list_add(cache, PMIX_PROC_SAMPLE_TIME, &sample_time, PMIX_TIME);
    if (PMIX_SUCCESS != rc) {
        PMIx_Info_list_release(cache);
        return rc;
    }
    // add this to the final answer
    rc = PMIx_Info_list_convert(cache, &darray);
    PMIx_Info_list_release(cache);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    rc = PMIx_Info_list_add(answer, PMIX_PROC_RESOURCE_USAGE, &darray, PMIX_DATA_ARRAY);

    return rc;
}

static pmix_status_t disk_stat(void *answer,
                               char **disks,
                               pmix_dkstats_t *dkst)
{
    char *dptr;
    void *cache;
    int n;
    uint64_t u64;
    pmix_data_array_t darray;
    time_t sample_time;
    pmix_status_t rc;
    PMIX_HIDE_UNUSED_PARAMS(disks);

    time(&sample_time);

    for (n=0; n < 2; n++) {
        /* take the fields of interest */
        cache = PMIx_Info_list_start();
        // start with the diskID
        pmix_asprintf(&dptr, "sd%02d", n);
        rc = PMIx_Info_list_add(cache, PMIX_DISK_ID, dptr, PMIX_STRING);
        free(dptr);
        if (PMIX_SUCCESS != rc) {
            PMIx_Info_list_release(cache);
            return rc;
        }
        if (dkst->rdcompleted) {
            u64 = 123456;
            rc = PMIx_Info_list_add(cache, PMIX_DISK_READ_COMPLETED, &u64, PMIX_UINT64);
            if (PMIX_SUCCESS != rc) {
                PMIx_Info_list_release(cache);
                return rc;
            }
        }
        if (dkst->rdmerged) {
            u64 = 789;
            rc = PMIx_Info_list_add(cache, PMIX_DISK_READ_MERGED, &u64, PMIX_UINT64);
            if (PMIX_SUCCESS != rc) {
                PMIx_Info_list_release(cache);
                return rc;
            }
        }
        if (dkst->rdsectors) {
            u64 = 123;
            rc = PMIx_Info_list_add(cache, PMIX_DISK_READ_SECTORS, &u64, PMIX_UINT64);
            if (PMIX_SUCCESS != rc) {
                PMIx_Info_list_release(cache);
                return rc;
            }
        }
        if (dkst->rdms) {
            u64 = 1902;
            rc = PMIx_Info_list_add(cache, PMIX_DISK_READ_MILLISEC, &u64, PMIX_UINT64);
            if (PMIX_SUCCESS != rc) {
                PMIx_Info_list_release(cache);
                return rc;
            }
        }

        if (dkst->wrtcompleted) {
            u64 = 12334;
            rc = PMIx_Info_list_add(cache, PMIX_DISK_WRITE_COMPLETED, &u64, PMIX_UINT64);
            if (PMIX_SUCCESS != rc) {
                PMIx_Info_list_release(cache);
                return rc;
            }
        }
        if (dkst->wrtmerged) {
            u64 = 5678;
            rc = PMIx_Info_list_add(cache, PMIX_DISK_WRITE_MERGED, &u64, PMIX_UINT64);
            if (PMIX_SUCCESS != rc) {
                PMIx_Info_list_release(cache);
                return rc;
            }
        }
        if (dkst->wrtsectors) {
            u64 = 9032;
            rc = PMIx_Info_list_add(cache, PMIX_DISK_WRITE_SECTORS, &u64, PMIX_UINT64);
            if (PMIX_SUCCESS != rc) {
                PMIx_Info_list_release(cache);
                return rc;
            }
        }
        if (dkst->wrtms) {
            u64 = 89324;
            rc = PMIx_Info_list_add(cache, PMIX_DISK_WRITE_MILLISEC, &u64, PMIX_UINT64);
            if (PMIX_SUCCESS != rc) {
                PMIx_Info_list_release(cache);
                return rc;
            }
        }

        if (dkst->ioprog) {
            u64 = 123;
            rc = PMIx_Info_list_add(cache, PMIX_DISK_IO_IN_PROGRESS, &u64, PMIX_UINT64);
            if (PMIX_SUCCESS != rc) {
                PMIx_Info_list_release(cache);
                return rc;
            }
        }
        if (dkst->ioms) {
            u64 = 2343545546;
            rc = PMIx_Info_list_add(cache, PMIX_DISK_IO_MILLISEC, &u64, PMIX_UINT64);
            if (PMIX_SUCCESS != rc) {
                PMIx_Info_list_release(cache);
                return rc;
            }
        }
        if (dkst->ioweight) {
            u64 = 3452;
            rc = PMIx_Info_list_add(cache, PMIX_DISK_IO_WEIGHTED, &u64, PMIX_UINT64);
            if (PMIX_SUCCESS != rc) {
                PMIx_Info_list_release(cache);
                return rc;
            }
        }

        // record the sample time
        rc = PMIx_Info_list_add(cache, PMIX_DISK_SAMPLE_TIME, &sample_time, PMIX_TIME);
        if (PMIX_SUCCESS != rc) {
            PMIx_Info_list_release(cache);
            return rc;
        }

        // add this to the final answer
        rc = PMIx_Info_list_convert(cache, &darray);
        PMIx_Info_list_release(cache);
        if (PMIX_SUCCESS != rc) {
            return rc;
        }
        rc = PMIx_Info_list_add(answer, PMIX_DISK_RESOURCE_USAGE, &darray, PMIX_DATA_ARRAY);
        if (PMIX_SUCCESS != rc) {
            return rc;
        }
    }

    return PMIX_SUCCESS;
}

static pmix_status_t net_stat(void *answer, char**nets,
                              pmix_netstats_t *netst)
{
    char *dptr;
    void *cache;
    int n;
    uint64_t u64;
    pmix_data_array_t darray;
    time_t sample_time;
    pmix_status_t rc;
    PMIX_HIDE_UNUSED_PARAMS(nets);

    /* read the file one line at a time */
    for (n=0; n < 3; n++) {
        time(&sample_time);

        /* take the fields of interest */
        cache = PMIx_Info_list_start();
        // start with the network ID
        pmix_asprintf(&dptr, "net%03d", n);
        rc = PMIx_Info_list_add(cache, PMIX_NETWORK_ID, dptr, PMIX_STRING);
        free(dptr);
        if (PMIX_SUCCESS != rc) {
            PMIx_Info_list_release(cache);
            return rc;
        }

        if (netst->rcvdb) {
            u64 = 32353453;
            rc = PMIx_Info_list_add(cache, PMIX_NET_RECVD_BYTES, &u64, PMIX_UINT64);
            if (PMIX_SUCCESS != rc) {
                PMIx_Info_list_release(cache);
                return rc;
            }
        }
        if (netst->rcvdp) {
            u64 = 2325325;
            rc = PMIx_Info_list_add(cache, PMIX_NET_RECVD_PCKTS, &u64, PMIX_UINT64);
            if (PMIX_SUCCESS != rc) {
                PMIx_Info_list_release(cache);
                return rc;
            }
        }
        if (netst->rcvde) {
            u64 = 134;
            rc = PMIx_Info_list_add(cache, PMIX_NET_RECVD_ERRS, &u64, PMIX_UINT64);
            if (PMIX_SUCCESS != rc) {
                PMIx_Info_list_release(cache);
                return rc;
            }
        }

        if (netst->sntb) {
            u64 = 939092;
            rc = PMIx_Info_list_add(cache, PMIX_NET_SENT_BYTES, &u64, PMIX_UINT64);
            if (PMIX_SUCCESS != rc) {
                PMIx_Info_list_release(cache);
                return rc;
            }
        }
        if (netst->sntp) {
            u64 = 235235435;
            rc = PMIx_Info_list_add(cache, PMIX_NET_SENT_PCKTS, &u64, PMIX_UINT64);
            if (PMIX_SUCCESS != rc) {
                PMIx_Info_list_release(cache);
                return rc;
            }
        }
        if (netst->snte) {
            u64 = 23253;
            rc = PMIx_Info_list_add(cache, PMIX_NET_SENT_ERRS, &u64, PMIX_UINT64);
            if (PMIX_SUCCESS != rc) {
                PMIx_Info_list_release(cache);
                return rc;
            }
        }

        // record the sample time
        rc = PMIx_Info_list_add(cache, PMIX_NET_SAMPLE_TIME, &sample_time, PMIX_TIME);
        if (PMIX_SUCCESS != rc) {
            PMIx_Info_list_release(cache);
            return rc;
        }
        // add this to the final answer
        rc = PMIx_Info_list_convert(cache, &darray);
        PMIx_Info_list_release(cache);
        if (PMIX_SUCCESS != rc) {
            return rc;
        }

        rc = PMIx_Info_list_add(answer, PMIX_NETWORK_RESOURCE_USAGE, &darray, PMIX_DATA_ARRAY);
        if (PMIX_SUCCESS != rc) {
            return rc;
        }
    }
    return PMIX_SUCCESS;
}

static pmix_status_t node_stat(void *ilist, pmix_ndstats_t *ndst)
{
    float fval;
    pmix_status_t rc;
    time_t sample_time;

    time(&sample_time);

    /* we only care about the first three numbers */
    fval = 0.34;
    if (ndst->la) {
        rc = PMIx_Info_list_add(ilist, PMIX_NODE_LOAD_AVG, &fval, PMIX_FLOAT);
        if (PMIX_SUCCESS != rc) {
            return rc;
        }
    }
    fval = 1.67;
    if (ndst->la5) {
        rc = PMIx_Info_list_add(ilist, PMIX_NODE_LOAD_AVG5, &fval, PMIX_FLOAT);
        if (PMIX_SUCCESS != rc) {
            return rc;
        }
    }
    fval = 3.789;
    if (ndst->la15) {
        rc = PMIx_Info_list_add(ilist, PMIX_NODE_LOAD_AVG15, &fval, PMIX_FLOAT);
        if (PMIX_SUCCESS != rc) {
            return rc;
        }
    }

    if (ndst->mtot) {
        fval = 17.9;
        rc = PMIx_Info_list_add(ilist, PMIX_NODE_MEM_TOTAL, &fval, PMIX_FLOAT);
        if (PMIX_SUCCESS != rc) {
            return rc;
        }
    }

    if (ndst->mfree) {
        fval = 12.8;
        rc = PMIx_Info_list_add(ilist, PMIX_NODE_MEM_FREE, &fval, PMIX_FLOAT);
        if (PMIX_SUCCESS != rc) {
            return rc;
        }
    }

    if (ndst->mbuf) {
        fval = 23.2;
        rc = PMIx_Info_list_add(ilist, PMIX_NODE_MEM_BUFFERS, &fval, PMIX_FLOAT);
        if (PMIX_SUCCESS != rc) {
            return rc;
        }
    }

    if (ndst->mcached) {
        fval = 1.343;
        rc = PMIx_Info_list_add(ilist, PMIX_NODE_MEM_CACHED, &fval, PMIX_FLOAT);
        if (PMIX_SUCCESS != rc) {
            return rc;
        }
    }

    if (ndst->mswapcached) {
        fval = 13.98;
        rc = PMIx_Info_list_add(ilist, PMIX_NODE_MEM_SWAP_CACHED, &fval, PMIX_FLOAT);
        if (PMIX_SUCCESS != rc) {
            return rc;
        }
    }

    if (ndst->mswaptot) {
        fval = 32.1;
        rc = PMIx_Info_list_add(ilist, PMIX_NODE_SWAP_TOTAL, &fval, PMIX_FLOAT);
        if (PMIX_SUCCESS != rc) {
            return rc;
        }
    }

    if (ndst->mswapfree) {
        fval = 9.3;
        rc = PMIx_Info_list_add(ilist, PMIX_NODE_MEM_SWAP_FREE, &fval, PMIX_FLOAT);
        if (PMIX_SUCCESS != rc) {
            return rc;
        }
    }

    if (ndst->mmap) {
        fval = 0.5;
        rc = PMIx_Info_list_add(ilist, PMIX_NODE_MEM_MAPPED, &fval, PMIX_FLOAT);
        if (PMIX_SUCCESS != rc) {
            return rc;
        }
    }

    rc = PMIx_Info_list_add(ilist, PMIX_NODE_SAMPLE_TIME, &sample_time, PMIX_TIME);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }

    return PMIX_SUCCESS;
}

static void evrelease(pmix_status_t status, void *cbdata)
{
    pmix_cb_t *cb = (pmix_cb_t*)cbdata;
    PMIX_HIDE_UNUSED_PARAMS(status);
    PMIX_RELEASE(cb);
}

static void update(int sd, short args, void *cbdata)
{
    pmix_pstat_op_t *op = (pmix_pstat_op_t*)cbdata;
    void *answer, *ilist;
    pmix_peerlist_t *plist;
    pmix_status_t rc;
    pmix_data_array_t darray;
    pmix_cb_t *cb;
    pmix_procstats_t zproc;
    pmix_ndstats_t znd;
    pmix_netstats_t znet;
    pmix_dkstats_t zdk;
    bool nettaken, dktaken, noreset;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    cb = op->cb;
    if (NULL == cb) {
        answer = PMIx_Info_list_start();
        noreset = false;
    } else {
        answer = cb->cbdata;
        noreset = true;
    }
    PMIX_PROCSTATS_INIT(&zproc);
    PMIX_NDSTATS_INIT(&znd);
    PMIX_NETSTATS_INIT(&znet);
    PMIX_DKSTATS_INIT(&zdk);
    nettaken = false;
    dktaken = false;

    if (0 != memcmp(&op->pstats, &zproc, sizeof(pmix_procstats_t))) {
        PMIX_LIST_FOREACH(plist, &op->peers, pmix_peerlist_t) {
            rc = proc_stat(answer, plist->peer, &op->pstats);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIx_Info_list_release(answer);
                goto reset;
            }
        }
    }

    if (0 != memcmp(&op->ndstats, &znd, sizeof(pmix_ndstats_t))) {
        ilist = PMIx_Info_list_start();
        // start with hostname
        rc = PMIx_Info_list_add(ilist, PMIX_HOSTNAME, pmix_globals.hostname, PMIX_STRING);
        if (PMIX_SUCCESS != rc) {
            PMIx_Info_list_release(ilist);
            PMIx_Info_list_release(answer);
            goto reset;
        }
        // add our nodeID
        rc = PMIx_Info_list_add(ilist, PMIX_NODEID, &pmix_globals.nodeid, PMIX_UINT32);
        if (PMIX_SUCCESS != rc) {
            PMIx_Info_list_release(ilist);
            PMIx_Info_list_release(answer);
            goto reset;
        }
        // collect the stats
        rc = node_stat(ilist, &op->ndstats);
        if (PMIX_SUCCESS != rc) {
            PMIx_Info_list_release(ilist);
            PMIx_Info_list_release(answer);
            goto reset;
        }
        if (0 != memcmp(&op->netstats, &znet, sizeof(pmix_netstats_t))) {
            nettaken = true;
            rc = net_stat(ilist, op->nets, &op->netstats);
            if (PMIX_SUCCESS != rc) {
                PMIx_Info_list_release(ilist);
                PMIx_Info_list_release(answer);
                goto reset;
            }
        }
        if (0 != memcmp(&op->dkstats, &zdk, sizeof(pmix_dkstats_t))) {
            dktaken = true;
            rc = disk_stat(ilist, op->disks, &op->dkstats);
            if (PMIX_SUCCESS != rc) {
                PMIx_Info_list_release(ilist);
                PMIx_Info_list_release(answer);
                goto reset;
            }
        }
        // add this to the final answer
        rc = PMIx_Info_list_convert(ilist, &darray);
        if (PMIX_SUCCESS != rc) {
            PMIx_Info_list_release(ilist);
            PMIx_Info_list_release(answer);
            goto reset;
        }
        PMIx_Info_list_release(ilist);
        rc = PMIx_Info_list_add(answer, PMIX_NODE_RESOURCE_USAGE, &darray, PMIX_DATA_ARRAY);
        PMIX_DATA_ARRAY_DESTRUCT(&darray);
        if (PMIX_SUCCESS != rc) {
            PMIx_Info_list_release(answer);
            goto reset;
        }
    }

    if (!nettaken && 0 != memcmp(&op->netstats, &znet, sizeof(pmix_netstats_t))) {
        rc = net_stat(answer, op->nets, &op->netstats);
        if (PMIX_SUCCESS != rc) {
            PMIx_Info_list_release(answer);
            goto reset;
        }
    }

    if (!dktaken && 0 != memcmp(&op->dkstats, &zdk, sizeof(pmix_dkstats_t))) {
        rc = disk_stat(answer, op->disks, &op->dkstats);
        if (PMIX_SUCCESS != rc) {
            PMIx_Info_list_release(answer);
            goto reset;
        }
    }

    if (NULL == cb) {
        // convert to info array
        rc = PMIx_Info_list_convert(answer, &darray);
        PMIx_Info_list_release(answer);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto reset;
        }
        // setup the event
        cb = PMIX_NEW(pmix_cb_t);
        cb->info = (pmix_info_t*)darray.array;
        cb->ninfo = darray.size;
        cb->infocopy = true;
        rc = PMIx_Notify_event(op->eventcode, &pmix_globals.myid,
                               PMIX_RANGE_LOCAL, cb->info, cb->ninfo,
                               evrelease, (void*)cb);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
        }
    }

reset:
    if (!noreset) {
        // reset the timer
        pmix_event_evtimer_add(&op->ev, &op->tv);
    }
}

/* if we are being called, then we already know that the request involves
 * this node - otherwise, we wouldn't have been called, except where noted
 */
static pmix_status_t query(pmix_proc_t *requestor,
                           const pmix_info_t *monitor, pmix_status_t eventcode,
                           const pmix_info_t directives[], size_t ndirs,
                           pmix_info_t **results, size_t *nresults)
{
    pmix_info_t *iptr;
    size_t sz;
    size_t n, m;
    int k;
    pmix_peer_t *ptr;
    pmix_proc_t proc, *procs;
    pmix_node_pid_t *ppid;
    pmix_status_t rc;
    bool tgtprocsgiven;
    pmix_data_array_t darray;
    pmix_pstat_op_t *op;
    pmix_netstats_t znet;
    pmix_dkstats_t zdk;
    pmix_info_t *xfer;
    pmix_cb_t cb;
    PMIX_HIDE_UNUSED_PARAMS(requestor);
    
    *results = NULL;
    *nresults = 0;
    PMIX_NETSTATS_INIT(&znet);
    PMIX_DKSTATS_INIT(&zdk);

    if (PMIx_Check_key(monitor->key, PMIX_MONITOR_CANCEL)) {
        // cancel an existing monitor operation - ID must be the provided value
        if (monitor->value.type != PMIX_STRING) {
            return PMIX_ERR_BAD_PARAM;
        }
        // cycle through our list of ops operations
        if (0 == pmix_list_get_size(&pmix_pstat_base.ops)) {
            // we don't have any operations - this is okay
            return PMIX_SUCCESS;
        }
        PMIX_LIST_FOREACH(op, &pmix_pstat_base.ops, pmix_pstat_op_t) {
            if (0 == strcmp(monitor->value.data.string, op->id)) {
                // terminate this operation
                pmix_list_remove_item(&pmix_pstat_base.ops, &op->super);
                PMIX_RELEASE(op);
                return PMIX_SUCCESS;
            }
        }
        // didn't find the operation - this is okay
        return PMIX_SUCCESS;
    }

    op = PMIX_NEW(pmix_pstat_op_t);
    op->eventcode = eventcode;

    for (n=0; n < ndirs; n++) {
        // did they give us an ID for this request?
        if (PMIx_Check_key(directives[n].key, PMIX_MONITOR_ID)) {
            op->id = strdup(directives[n].value.data.string);

        } else if (PMIx_Check_key(directives[n].key, PMIX_MONITOR_RESOURCE_RATE)) {
            // they are asking us to update it at regular intervals
            rc = PMIx_Value_get_number(&directives[n].value, (void*)&op->rate, PMIX_UINT32);
            if (PMIX_SUCCESS != rc) {
                PMIX_RELEASE(op);
                return rc;
            }
        }
        if (NULL != op->id && 0 < op->rate) {
            // don't keep searching if unnecessary
            break;
        }
    }

    // see what data we are being asked to collect
    if (PMIx_Check_key(monitor->key, PMIX_MONITOR_PROC_RESOURCE_USAGE)) {
        tgtprocsgiven = false;
        // see which values are to be returned
        iptr = (pmix_info_t*)monitor->value.data.darray->array;
        sz = monitor->value.data.darray->size;
        pmix_pstat_parse_procstats(&op->pstats, iptr, sz);

        // we already know this request involves us since it was checked
        // before calling us, so see which of our processes are included
        if (NULL == directives) {
            // all of our processes are included - no ID or rate can be included
            for (k=0; k < pmix_server_globals.clients.size; k++) {
                ptr = (pmix_peer_t*)pmix_pointer_array_get_item(&pmix_server_globals.clients, k);
                if (NULL == ptr) {
                    continue;
                }
                PMIX_PSTAT_APPEND_PEER_UNIQUE(&op->peers, ptr);
            }
            goto processprocs;
        }

        // check for specific procs and directives
        for (n=0; n < ndirs; n++) {
            if (PMIx_Check_key(directives[n].key, PMIX_MONITOR_TARGET_PROCS)) {
                if (PMIX_DATA_ARRAY != directives[n].value.type ||
                    NULL == directives[n].value.data.darray ||
                    NULL == directives[n].value.data.darray->array) {
                    PMIX_RELEASE(op);
                    return PMIX_ERR_BAD_PARAM;
                }
                tgtprocsgiven = true;
                // they specified the procs
                procs = (pmix_proc_t*)directives[n].value.data.darray->array;
                sz = directives[n].value.data.darray->size;
                for (m=0; m < sz; m++) {
                    for (k=0; k < pmix_server_globals.clients.size; k++) {
                        ptr = (pmix_peer_t*)pmix_pointer_array_get_item(&pmix_server_globals.clients, k);
                        if (NULL == ptr) {
                            continue;
                        }
                        PMIx_Load_procid(&proc, ptr->info->pname.nspace, ptr->info->pname.rank);
                        if (PMIx_Check_procid(&procs[m], &proc)) {
                            PMIX_PSTAT_APPEND_PEER_UNIQUE(&op->peers, ptr);
                            break;
                       }
                    }
                }
            }

            if (PMIx_Check_key(directives[n].key, PMIX_MONITOR_TARGET_PIDS)) {
                if (PMIX_DATA_ARRAY != directives[n].value.type ||
                    NULL == directives[n].value.data.darray ||
                    NULL == directives[n].value.data.darray->array) {
                    PMIX_RELEASE(op);
                    return PMIX_ERR_BAD_PARAM;
                }
                tgtprocsgiven = true;
                // is our node included?
                ppid = (pmix_node_pid_t*)directives[n].value.data.darray->array;
                sz = directives[n].value.data.darray->size;
                for (m=0; m < sz; m++) {
                    // see if this node is us
                    if (ppid[m].nodeid == pmix_globals.nodeid ||
                        pmix_check_local(ppid[m].hostname)) {
                        // is this one of our procs?
                        for (k=0; k < pmix_server_globals.clients.size; k++) {
                            ptr = (pmix_peer_t*)pmix_pointer_array_get_item(&pmix_server_globals.clients, k);
                            if (NULL == ptr) {
                                continue;
                            }
                            if (ppid[m].pid == ptr->info->pid) {
                                PMIX_PSTAT_APPEND_PEER_UNIQUE(&op->peers, ptr);
                                break;
                            }
                        }
                    }
                }
            }
        }

        // when we come out of the loop, it is possible that we were not
        // given any target procs via any of the attributes. In this case,
        // we assume that we are to take all local procs
        if (!tgtprocsgiven) {
            for (k=0; k < pmix_server_globals.clients.size; k++) {
                ptr = (pmix_peer_t*)pmix_pointer_array_get_item(&pmix_server_globals.clients, k);
                if (NULL == ptr) {
                    continue;
                }
                PMIX_PSTAT_APPEND_PEER_UNIQUE(&op->peers, ptr);
            }
        }

processprocs:
        PMIX_CONSTRUCT(&cb, pmix_cb_t);
        cb.cbdata = PMIx_Info_list_start();
        op->cb = &cb;
        update(0, 0, (void*)op);
        // convert to info array
        rc = PMIx_Info_list_convert(cb.cbdata, &darray);
        PMIx_Info_list_release(cb.cbdata);
        PMIX_DESTRUCT(&cb);
        op->cb = NULL;
        if (PMIX_SUCCESS != rc) {
            PMIX_RELEASE(op);
            return rc;
        }
        *results = (pmix_info_t*)darray.array;
        *nresults = darray.size;

        // if we were given a rate, then setup the event to generate
        // the requested updates
        if (0 < op->rate) {
            pmix_list_append(&pmix_pstat_base.ops, &op->super);
            PMIX_PSTAT_OP_START(op, op->rate, update);
        } else {
            // otherwise, this was a one-time request, so we are done with the op
            PMIX_RELEASE(op);
        }
        return PMIX_SUCCESS;
    }

    if (PMIx_Check_key(monitor->key, PMIX_MONITOR_NODE_RESOURCE_USAGE)) {
        // we already know we are a target node since we are
        // being called
        iptr = (pmix_info_t*)monitor->value.data.darray->array;
        sz = monitor->value.data.darray->size;
        // determine what stats to return - might include net and disk values
        pmix_pstat_parse_ndstats(&op->ndstats, iptr, sz);
        pmix_pstat_parse_netstats(&op->nets, &op->netstats, iptr, sz);
        pmix_pstat_parse_dkstats(&op->disks, &op->dkstats, iptr, sz);
        // start collecting response
        PMIX_CONSTRUCT(&cb, pmix_cb_t);
        cb.cbdata = PMIx_Info_list_start();
        op->cb = &cb;
        // start with hostname
        rc = PMIx_Info_list_add(cb.cbdata, PMIX_HOSTNAME, pmix_globals.hostname, PMIX_STRING);
        if (PMIX_SUCCESS != rc) {
            PMIx_Info_list_release(cb.cbdata);
            op->cb = NULL;
            PMIX_DESTRUCT(&cb);
            PMIX_RELEASE(op);
            return rc;
        }
        // add our nodeID
        rc = PMIx_Info_list_add(cb.cbdata, PMIX_NODEID, &pmix_globals.nodeid, PMIX_UINT32);
        if (PMIX_SUCCESS != rc) {
            PMIx_Info_list_release(cb.cbdata);
            op->cb = NULL;
            PMIX_DESTRUCT(&cb);
            PMIX_RELEASE(op);
            return rc;
        }
        // collect the stats
        update(0, 0, (void*)op);
        // add this to the final answer
        rc = PMIx_Info_list_convert(cb.cbdata, &darray);
        if (PMIX_SUCCESS != rc) {
            PMIx_Info_list_release(cb.cbdata);
            op->cb = NULL;
            PMIX_DESTRUCT(&cb);
            PMIX_RELEASE(op);
            return rc;
        }
        PMIx_Info_list_release(cb.cbdata);
        PMIX_DESTRUCT(&cb);
        op->cb = NULL;
        PMIX_INFO_CREATE(xfer, 1);
        PMIX_INFO_LOAD(xfer, PMIX_NODE_RESOURCE_USAGE, &darray, PMIX_DATA_ARRAY);
        PMIX_DATA_ARRAY_DESTRUCT(&darray);
        *results = xfer;
        *nresults = 1;

        // if we were given a rate, then setup the event to generate
        // the requested updates
        if (0 < op->rate) {
            pmix_list_append(&pmix_pstat_base.ops, &op->super);
            PMIX_PSTAT_OP_START(op, op->rate, update);
        } else {
            // otherwise, this was a one-time request, so we are done with the op
            PMIX_RELEASE(op);
        }
        return PMIX_SUCCESS;
    }

    if (PMIx_Check_key(monitor->key, PMIX_MONITOR_DISK_RESOURCE_USAGE)) {
        // we already know we are a target node since we are
        // being called
        iptr = (pmix_info_t*)monitor->value.data.darray->array;
        sz = monitor->value.data.darray->size;
        // assemble any provided disk IDs and/or stat specifications
        pmix_pstat_parse_dkstats(&op->disks, &op->dkstats, iptr, sz);
        // collect the stats
        PMIX_CONSTRUCT(&cb, pmix_cb_t);
        cb.cbdata = PMIx_Info_list_start();
        op->cb = &cb;
        update(0, 0, (void*)op);
        // convert to info array
        rc = PMIx_Info_list_convert(cb.cbdata, &darray);
        PMIx_Info_list_release(cb.cbdata);
        PMIX_DESTRUCT(&cb);
        op->cb = NULL;
        if (PMIX_SUCCESS != rc) {
            PMIX_RELEASE(op);
            return rc;
        }
        *results = (pmix_info_t*)darray.array;
        *nresults = darray.size;

        // if we were given a rate, then setup the event to generate
        // the requested updates
        if (0 < op->rate) {
            pmix_list_append(&pmix_pstat_base.ops, &op->super);
            PMIX_PSTAT_OP_START(op, op->rate, update);
        } else {
            // otherwise, this was a one-time request, so we are done with the op
            PMIX_RELEASE(op);
        }
        return PMIX_SUCCESS;
    }

    if (PMIx_Check_key(monitor->key, PMIX_MONITOR_NET_RESOURCE_USAGE)) {
        // we already know we are a target node since we are
        // being called
        iptr = (pmix_info_t*)monitor->value.data.darray->array;
        sz = monitor->value.data.darray->size;
        // assemble any provided network IDs and/or stat specifications
        pmix_pstat_parse_netstats(&op->nets, &op->netstats, iptr, sz);
        // collect the stats
        PMIX_CONSTRUCT(&cb, pmix_cb_t);
        cb.cbdata = PMIx_Info_list_start();
        op->cb = &cb;
        update(0, 0, (void*)op);
        // convert to info array
        rc = PMIx_Info_list_convert(cb.cbdata, &darray);
        PMIx_Info_list_release(cb.cbdata);
        PMIX_DESTRUCT(&cb);
        op->cb = NULL;
        if (PMIX_SUCCESS != rc) {
            PMIX_RELEASE(op);
            return rc;
        }
        *results = (pmix_info_t*)darray.array;
        *nresults = darray.size;

        // if we were given a rate, then setup the event to generate
        // the requested updates
        if (0 < op->rate) {
            pmix_list_append(&pmix_pstat_base.ops, &op->super);
            PMIX_PSTAT_OP_START(op, op->rate, update);
        } else {
            // otherwise, this was a one-time request, so we are done with the op
            PMIX_RELEASE(op);
        }
        return PMIX_SUCCESS;
    }

    return PMIX_SUCCESS;
}
