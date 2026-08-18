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
#include "src/mca/pstat/pstat.h"
#include "src/util/pmix_error.h"

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
         * results. This matches PMIX_PSTAT_APPEND_PEER_UNIQUE, which
         * dedups the op's other target filter the same way */
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
