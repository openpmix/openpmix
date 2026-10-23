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
 * Copyright (c) 2006-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2013      Los Alamos National Security, LLC.  All rights reserved.
 * Copyright (c) 2013-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "pmix_config.h"
#include "pmix_common.h"

/* This component is only compiled on FreeBSD 12 and later, where the
 * sysctl(3) records and the getifaddrs(3) counters below have the shape
 * read here. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifdef HAVE_SYS_TIME_H
#    include <sys/time.h>
#endif
#include <sys/param.h>
#include <sys/types.h>
#include <sys/devicestat.h>
#include <sys/proc.h> /* for the SIDL/SRUN/... process-state codes */
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#include <vm/vm_param.h>

#include <ifaddrs.h>
#include <net/if.h>

#include "src/include/pmix_globals.h"
#include "src/server/pmix_server_ops.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_printf.h"

#include "src/mca/pstat/base/base.h"
#include "pstat_pfreebsd.h"

/*
 * API functions
 */
static pmix_status_t pfreebsd_module_init(void);
static pmix_status_t query(pmix_proc_t *requestor,
                           const pmix_info_t *monitor, pmix_status_t error,
                           const pmix_info_t directives[], size_t ndirs,
                           pmix_info_t **results, size_t *nresults);
static pmix_status_t pfreebsd_module_fini(void);

/*
 * FreeBSD pstat module
 */
const pmix_pstat_base_module_t pmix_pstat_pfreebsd_module = {
    /* Initialization function */
    .init = pfreebsd_module_init,
    .query = query,
    .finalize = pfreebsd_module_fini
};

static pmix_status_t pfreebsd_module_init(void)
{
    return PMIX_SUCCESS;
}

static pmix_status_t pfreebsd_module_fini(void)
{
    return PMIX_SUCCESS;
}

/* Translate a kinfo_proc status code into the single-character
 * process-state string used by the Linux reader, so callers see a
 * consistent representation regardless of platform. The two FreeBSD
 * states Linux has no letter for take the ones ps(1) gives them. */
static const char *proc_state_string(char status)
{
    switch (status) {
    case SIDL:
        return "I"; /* being created */
    case SRUN:
        return "R"; /* runnable */
    case SSLEEP:
        return "S"; /* sleeping */
    case SSTOP:
        return "T"; /* stopped */
    case SZOMB:
        return "Z"; /* zombie */
    case SWAIT:
        return "W"; /* waiting for an interrupt */
    case SLOCK:
        return "L"; /* blocked on a lock */
    default:
        return "?";
    }
}

static pmix_status_t proc_stat(void *answer, const pmix_pstat_target_t *tgt, pid_t pid,
                               pmix_procstats_t *pst)
{
    pmix_proc_t proc;
    pmix_status_t rc;
    struct kinfo_proc kp;
    size_t len = sizeof(kp);
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, (int) pid};
    float fval;
    uint16_t u16;
    int32_t i32;
    struct timeval evtime;
    void *cache;
    time_t sample_time;
    pmix_data_array_t darray;

    /* One record holds everything reported here, and the kernel fills it
     * from the process in a single pass - so unlike libproc on macOS
     * there is no second read for the process to have been replaced
     * between, and the owner checked is the owner of the numbers. A pid
     * with no process answers ESRCH, and so does another user's process
     * to a server that is not root while security.bsd.see_other_uids is
     * 0: either way there is nothing of it to report, and update() skips
     * it. */
    if (0 != sysctl(mib, 4, &kp, &len, NULL, 0) || sizeof(kp) != len ||
        sizeof(kp) != (size_t) kp.ki_structsize) {
        return PMIX_ERR_NOT_FOUND;
    }
    /* We read with our own privilege, so the owner check is what keeps a
     * sample to the process it is meant to be - a pid the host recorded
     * outlives its process and is reused */
    if ((uid_t) -1 != tgt->owner && kp.ki_uid != tgt->owner) {
        pmix_output_verbose(5, pmix_pstat_base_framework.framework_output,
                            "pstat: pid %d is not a process of uid %lu - not sampled",
                            (int) pid, (unsigned long) tgt->owner);
        return PMIX_ERR_NOT_FOUND;
    }

    time(&sample_time);
    cache = PMIx_Info_list_start();

    // start with the proc ID, for a PMIx process
    if (tgt->named) {
        PMIx_Load_procid(&proc, tgt->name.nspace, tgt->name.rank);
        rc = PMIx_Info_list_add(cache, PMIX_PROCID, &proc, PMIX_PROC);
        if (PMIX_SUCCESS != rc) {
            PMIx_Info_list_release(cache);
            return rc;
        }
    }
    // add the pid
    rc = PMIx_Info_list_add(cache, PMIX_PROC_PID, &pid, PMIX_PID);
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

    /* the command name */
    if (pst->cmdline) {
        kp.ki_comm[sizeof(kp.ki_comm) - 1] = '\0';
        rc = PMIx_Info_list_add(cache, PMIX_CMD_LINE, kp.ki_comm, PMIX_STRING);
        if (PMIX_SUCCESS != rc) {
            PMIx_Info_list_release(cache);
            return rc;
        }
    }

    /* the process state */
    if (pst->state) {
        rc = PMIx_Info_list_add(cache, PMIX_PROC_OS_STATE,
                                (char *) proc_state_string(kp.ki_stat), PMIX_STRING);
        if (PMIX_SUCCESS != rc) {
            PMIx_Info_list_release(cache);
            return rc;
        }
    }

    if (pst->time) {
        /* the CPU time of all the process's threads, in microseconds -
         * what ps(1) shows as its time - split with integer arithmetic
         * and carried at the width of a time_t */
        evtime.tv_sec = (time_t) (kp.ki_runtime / 1000000ULL);
        evtime.tv_usec = (suseconds_t) (kp.ki_runtime % 1000000ULL);
        rc = PMIx_Info_list_add(cache, PMIX_PROC_TIME, (void *) &evtime, PMIX_TIMEVAL);
        if (PMIX_SUCCESS != rc) {
            PMIx_Info_list_release(cache);
            return rc;
        }
    }

    if (pst->pctcpu) {
        /* the kernel's decaying average, a fixed-point fraction of one
         * CPU - scaled as ps(1) scales it */
        fval = (float) (100.0 * (double) kp.ki_pctcpu / (double) FSCALE);
        rc = PMIx_Info_list_add(cache, PMIX_PROC_PERCENT_CPU, &fval, PMIX_FLOAT);
        if (PMIX_SUCCESS != rc) {
            PMIx_Info_list_release(cache);
            return rc;
        }
    }

    if (pst->pri) {
        /* as ps(1) shows it: relative to the first time-sharing level */
        i32 = (int32_t) kp.ki_pri.pri_level - PUSER;
        rc = PMIx_Info_list_add(cache, PMIX_PROC_PRIORITY, &i32, PMIX_INT32);
        if (PMIX_SUCCESS != rc) {
            PMIx_Info_list_release(cache);
            return rc;
        }
    }

    if (pst->nthreads) {
        u16 = (uint16_t) kp.ki_numthreads;
        rc = PMIx_Info_list_add(cache, PMIX_PROC_NUM_THREADS, &u16, PMIX_UINT16);
        if (PMIX_SUCCESS != rc) {
            PMIx_Info_list_release(cache);
            return rc;
        }
    }

    if (pst->cpu) {
        u16 = (uint16_t) kp.ki_lastcpu;
        rc = PMIx_Info_list_add(cache, PMIX_PROC_CPU, &u16, PMIX_UINT16);
        if (PMIX_SUCCESS != rc) {
            PMIx_Info_list_release(cache);
            return rc;
        }
    }

    if (pst->vsize) {
        /* bytes, normalized to MB to match the Linux reader */
        fval = (float) kp.ki_size / (1024.0 * 1024.0);
        rc = PMIx_Info_list_add(cache, PMIX_PROC_VSIZE, &fval, PMIX_FLOAT);
        if (PMIX_SUCCESS != rc) {
            PMIx_Info_list_release(cache);
            return rc;
        }
    }

    if (pst->rss) {
        /* pages */
        fval = (float) ((double) kp.ki_rssize * (double) getpagesize() / (1024.0 * 1024.0));
        rc = PMIx_Info_list_add(cache, PMIX_PROC_RSS, &fval, PMIX_FLOAT);
        if (PMIX_SUCCESS != rc) {
            PMIx_Info_list_release(cache);
            return rc;
        }
    }

    /* NOTE: FreeBSD keeps no proportional set size (pss) or peak virtual
     * size (pkvsize) for a process, so those fields are not reported. */

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
        /* the conversion creates its array before filling it, so a
         * failure partway still leaves one here to release */
        PMIX_DATA_ARRAY_DESTRUCT(&darray);
        return rc;
    }
    rc = PMIx_Info_list_add(answer, PMIX_PROC_RESOURCE_USAGE, &darray, PMIX_DATA_ARRAY);
    /* the add deep-copies the array into the list, so this one is still
     * ours - keeping it leaks a full sample for every process on every
     * tick of a periodic monitor */
    PMIX_DATA_ARRAY_DESTRUCT(&darray);

    return rc;
}

/* a struct bintime in milliseconds */
static uint64_t bintime_ms(const struct bintime *bt)
{
    return (uint64_t) bt->sec * 1000ULL + (uint64_t) ((bt->frac >> 32) * 1000ULL >> 32);
}

/* The whole devstat table, as iostat(8) reads it without libdevstat: a
 * long generation count and then one struct devstat per device. The
 * kernel updates a record without a lock, bracketed by sequence0 and
 * sequence1, and copies the table out as it stands - so a record read
 * while it was changing has the two unequal. Read again until no record
 * is torn, or give up and use what we have: a torn record is off by a
 * single transfer at most */
static pmix_status_t read_devstat(char **bufp, size_t *lenp)
{
    char *buf;
    size_t len, off;
    struct devstat ds;
    int version, tries;
    bool torn;

    len = sizeof(version);
    if (0 != sysctlbyname("kern.devstat.version", &version, &len, NULL, 0) ||
        DEVSTAT_VERSION != version) {
        return PMIX_ERR_NOT_FOUND;
    }
    for (tries = 0; tries < 5; tries++) {
        if (0 != sysctlbyname("kern.devstat.all", NULL, &len, NULL, 0)) {
            return PMIX_ERR_NOT_FOUND;
        }
        len += len / 8 + 4 * sizeof(struct devstat);
        buf = (char *) malloc(len);
        if (NULL == buf) {
            return PMIX_ERR_NOMEM;
        }
        if (0 != sysctlbyname("kern.devstat.all", buf, &len, NULL, 0)) {
            free(buf);
            if (ENOMEM == errno) {
                continue; /* devices came since we asked the size */
            }
            return PMIX_ERR_NOT_FOUND;
        }
        torn = false;
        for (off = sizeof(long); off + sizeof(ds) <= len; off += sizeof(ds)) {
            memcpy(&ds, buf + off, sizeof(ds));
            if (ds.sequence0 != ds.sequence1) {
                torn = true;
                break;
            }
        }
        if (!torn || 4 == tries) {
            *bufp = buf;
            *lenp = len;
            return PMIX_SUCCESS;
        }
        free(buf);
    }
    return PMIX_ERR_NOT_FOUND;
}

static pmix_status_t disk_stat(void *answer,
                               char **disks,
                               pmix_dkstats_t *dkst)
{
    char *buf;
    size_t len, off;
    struct devstat ds;
    char diskname[DEVSTAT_NAME_LEN + 16];
    void *cache;
    size_t n;
    bool takeit;
    uint64_t u64;
    pmix_data_array_t darray;
    time_t sample_time;
    pmix_status_t rc;

    rc = read_devstat(&buf, &len);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    time(&sample_time);

    /* the records are copied out: the buffer promises no alignment past
     * the generation count that opens it */
    for (off = sizeof(long); off + sizeof(ds) <= len; off += sizeof(ds)) {
        memcpy(&ds, buf + off, sizeof(ds));
        /* a pass(4) device is the CAM pass-through to a disk listed under
         * its own name, and iostat(8) leaves it out too */
        if (DEVSTAT_TYPE_PASS & ds.device_type) {
            continue;
        }
        ds.device_name[sizeof(ds.device_name) - 1] = '\0';
        snprintf(diskname, sizeof(diskname), "%s%d", ds.device_name, ds.unit_number);

        // see if this is a disk they want
        if (NULL == disks) {
            takeit = true;
        } else {
            takeit = false;
            for (n = 0; NULL != disks[n]; n++) {
                if (0 == strcmp(diskname, disks[n])) {
                    takeit = true;
                    break;
                }
            }
        }
        if (!takeit) {
            continue;
        }

        /* take the fields of interest */
        cache = PMIx_Info_list_start();
        // start with the diskID
        rc = PMIx_Info_list_add(cache, PMIX_DISK_ID, diskname, PMIX_STRING);
        if (PMIX_SUCCESS != rc) {
            goto diskerr;
        }
        if (dkst->rdcompleted) {
            u64 = ds.operations[DEVSTAT_READ];
            rc = PMIx_Info_list_add(cache, PMIX_DISK_READ_COMPLETED, &u64, PMIX_UINT64);
            if (PMIX_SUCCESS != rc) {
                goto diskerr;
            }
        }
        if (dkst->rdsectors) {
            /* FreeBSD counts bytes; normalize to 512-byte sectors to
             * match the Linux reader's units */
            u64 = ds.bytes[DEVSTAT_READ] / 512;
            rc = PMIx_Info_list_add(cache, PMIX_DISK_READ_SECTORS, &u64, PMIX_UINT64);
            if (PMIX_SUCCESS != rc) {
                goto diskerr;
            }
        }
        if (dkst->rdms) {
            u64 = bintime_ms(&ds.duration[DEVSTAT_READ]);
            rc = PMIx_Info_list_add(cache, PMIX_DISK_READ_MILLISEC, &u64, PMIX_UINT64);
            if (PMIX_SUCCESS != rc) {
                goto diskerr;
            }
        }
        if (dkst->wrtcompleted) {
            u64 = ds.operations[DEVSTAT_WRITE];
            rc = PMIx_Info_list_add(cache, PMIX_DISK_WRITE_COMPLETED, &u64, PMIX_UINT64);
            if (PMIX_SUCCESS != rc) {
                goto diskerr;
            }
        }
        if (dkst->wrtsectors) {
            u64 = ds.bytes[DEVSTAT_WRITE] / 512;
            rc = PMIx_Info_list_add(cache, PMIX_DISK_WRITE_SECTORS, &u64, PMIX_UINT64);
            if (PMIX_SUCCESS != rc) {
                goto diskerr;
            }
        }
        if (dkst->wrtms) {
            u64 = bintime_ms(&ds.duration[DEVSTAT_WRITE]);
            rc = PMIx_Info_list_add(cache, PMIX_DISK_WRITE_MILLISEC, &u64, PMIX_UINT64);
            if (PMIX_SUCCESS != rc) {
                goto diskerr;
            }
        }
        if (dkst->ioprog) {
            /* started and not yet ended */
            u64 = (uint64_t) (ds.start_count - ds.end_count);
            rc = PMIx_Info_list_add(cache, PMIX_DISK_IO_IN_PROGRESS, &u64, PMIX_UINT64);
            if (PMIX_SUCCESS != rc) {
                goto diskerr;
            }
        }
        if (dkst->ioms) {
            /* the time the device has had a transfer outstanding, which
             * the kernel brings up to date as each one ends */
            u64 = bintime_ms(&ds.busy_time);
            rc = PMIx_Info_list_add(cache, PMIX_DISK_IO_MILLISEC, &u64, PMIX_UINT64);
            if (PMIX_SUCCESS != rc) {
                goto diskerr;
            }
        }
        /* NOTE: FreeBSD does not merge requests the way the Linux block
         * layer does, and keeps no weighted I/O time, so the merged
         * counts (rdmerged/wrtmerged) and ioweight are not reported. */

        // record the sample time
        rc = PMIx_Info_list_add(cache, PMIX_DISK_SAMPLE_TIME, &sample_time, PMIX_TIME);
        if (PMIX_SUCCESS != rc) {
            goto diskerr;
        }

        // add this to the final answer
        rc = PMIx_Info_list_convert(cache, &darray);
        PMIx_Info_list_release(cache);
        if (PMIX_SUCCESS != rc) {
            /* the conversion creates its array before filling it, so a
             * failure partway still leaves one here to release */
            PMIX_DATA_ARRAY_DESTRUCT(&darray);
            free(buf);
            return rc;
        }
        rc = PMIx_Info_list_add(answer, PMIX_DISK_RESOURCE_USAGE, &darray, PMIX_DATA_ARRAY);
        PMIX_DATA_ARRAY_DESTRUCT(&darray);
        if (PMIX_SUCCESS != rc) {
            free(buf);
            return rc;
        }
        continue;

    diskerr:
        PMIx_Info_list_release(cache);
        free(buf);
        return rc;
    }
    free(buf);
    return PMIX_SUCCESS;
}

/* Unlike macOS, FreeBSD's getifaddrs() hands back the interface counters
 * at full width: since FreeBSD 11 every counter in struct if_data is a
 * uint64_t, so there is no wrap to read around. Each interface appears
 * once with an AF_LINK address, which carries them. */
static pmix_status_t net_stat(void *answer, char **nets,
                              pmix_netstats_t *netst)
{
    struct ifaddrs *ifs, *ifa;
    struct if_data *ifd;
    void *cache;
    size_t n;
    bool takeit;
    uint64_t u64;
    pmix_data_array_t darray;
    time_t sample_time;
    pmix_status_t rc;

    if (0 != getifaddrs(&ifs)) {
        /* not an error if we cannot read this as it isn't critical */
        return PMIX_ERR_NOT_FOUND;
    }
    time(&sample_time);

    for (ifa = ifs; NULL != ifa; ifa = ifa->ifa_next) {
        if (NULL == ifa->ifa_addr || AF_LINK != ifa->ifa_addr->sa_family ||
            NULL == ifa->ifa_data || NULL == ifa->ifa_name) {
            continue;
        }

        // see if this is a network they want
        if (NULL == nets) {
            takeit = true;
        } else {
            takeit = false;
            for (n = 0; NULL != nets[n]; n++) {
                if (0 == strcmp(ifa->ifa_name, nets[n])) {
                    takeit = true;
                    break;
                }
            }
        }
        if (!takeit) {
            continue;
        }
        ifd = (struct if_data *) ifa->ifa_data;

        /* take the fields of interest */
        cache = PMIx_Info_list_start();
        // start with the network ID
        rc = PMIx_Info_list_add(cache, PMIX_NETWORK_ID, ifa->ifa_name, PMIX_STRING);
        if (PMIX_SUCCESS != rc) {
            goto neterr;
        }

        if (netst->rcvdb) {
            u64 = ifd->ifi_ibytes;
            rc = PMIx_Info_list_add(cache, PMIX_NET_RECVD_BYTES, &u64, PMIX_UINT64);
            if (PMIX_SUCCESS != rc) {
                goto neterr;
            }
        }
        if (netst->rcvdp) {
            u64 = ifd->ifi_ipackets;
            rc = PMIx_Info_list_add(cache, PMIX_NET_RECVD_PCKTS, &u64, PMIX_UINT64);
            if (PMIX_SUCCESS != rc) {
                goto neterr;
            }
        }
        if (netst->rcvde) {
            u64 = ifd->ifi_ierrors;
            rc = PMIx_Info_list_add(cache, PMIX_NET_RECVD_ERRS, &u64, PMIX_UINT64);
            if (PMIX_SUCCESS != rc) {
                goto neterr;
            }
        }

        if (netst->sntb) {
            u64 = ifd->ifi_obytes;
            rc = PMIx_Info_list_add(cache, PMIX_NET_SENT_BYTES, &u64, PMIX_UINT64);
            if (PMIX_SUCCESS != rc) {
                goto neterr;
            }
        }
        if (netst->sntp) {
            u64 = ifd->ifi_opackets;
            rc = PMIx_Info_list_add(cache, PMIX_NET_SENT_PCKTS, &u64, PMIX_UINT64);
            if (PMIX_SUCCESS != rc) {
                goto neterr;
            }
        }
        if (netst->snte) {
            u64 = ifd->ifi_oerrors;
            rc = PMIx_Info_list_add(cache, PMIX_NET_SENT_ERRS, &u64, PMIX_UINT64);
            if (PMIX_SUCCESS != rc) {
                goto neterr;
            }
        }

        // record the sample time
        rc = PMIx_Info_list_add(cache, PMIX_NET_SAMPLE_TIME, &sample_time, PMIX_TIME);
        if (PMIX_SUCCESS != rc) {
            goto neterr;
        }
        // add this to the final answer
        rc = PMIx_Info_list_convert(cache, &darray);
        PMIx_Info_list_release(cache);
        if (PMIX_SUCCESS != rc) {
            /* the conversion creates its array before filling it, so a
             * failure partway still leaves one here to release */
            PMIX_DATA_ARRAY_DESTRUCT(&darray);
            freeifaddrs(ifs);
            return rc;
        }
        rc = PMIx_Info_list_add(answer, PMIX_NETWORK_RESOURCE_USAGE, &darray, PMIX_DATA_ARRAY);
        PMIX_DATA_ARRAY_DESTRUCT(&darray);
        if (PMIX_SUCCESS != rc) {
            freeifaddrs(ifs);
            return rc;
        }
        continue;

    neterr:
        PMIx_Info_list_release(cache);
        freeifaddrs(ifs);
        return rc;
    }
    freeifaddrs(ifs);
    return PMIX_SUCCESS;
}

/* a counter of pages out of vm.stats.vm, in MB */
static bool vm_pages_mb(const char *name, float *mb)
{
    u_int pages;
    size_t len = sizeof(pages);

    if (0 != sysctlbyname(name, &pages, &len, NULL, 0) || sizeof(pages) != len) {
        return false;
    }
    *mb = (float) ((double) pages * (double) getpagesize() / (1024.0 * 1024.0));
    return true;
}

/* The total and free swap, in pages, summed over the devices - one
 * vm.swap_info.<n> record each, as swapinfo(8) reads them */
static bool swap_pages(uint64_t *total, uint64_t *used)
{
    int mib[16];
    size_t miblen = sizeof(mib) / sizeof(mib[0]) - 1;
    size_t len;
    struct xswdev xsw;
    int n;

    *total = 0;
    *used = 0;
    if (0 != sysctlnametomib("vm.swap_info", mib, &miblen)) {
        return false;
    }
    for (n = 0;; n++) {
        mib[miblen] = n;
        len = sizeof(xsw);
        if (0 != sysctl(mib, (u_int) (miblen + 1), &xsw, &len, NULL, 0)) {
            /* ENOENT past the last device: a node with no swap has a
             * total of zero, which is a value to report */
            return ENOENT == errno;
        }
        if (sizeof(xsw) != len || XSWDEV_VERSION != xsw.xsw_version) {
            return false;
        }
        *total += (uint64_t) xsw.xsw_nblks;
        *used += (uint64_t) xsw.xsw_used;
    }
}

static pmix_status_t node_stat(void *ilist, pmix_ndstats_t *ndst)
{
    double loadavg[3];
    int nla;
    u_long physmem;
    long bufspace;
    uint64_t swtotal, swused;
    size_t len;
    float fval;
    pmix_status_t rc;
    time_t sample_time;

    time(&sample_time);

    /* load averages */
    nla = getloadavg(loadavg, 3);
    if (0 < nla && ndst->la) {
        fval = (float) loadavg[0];
        rc = PMIx_Info_list_add(ilist, PMIX_NODE_LOAD_AVG, &fval, PMIX_FLOAT);
        if (PMIX_SUCCESS != rc) {
            return rc;
        }
    }
    if (1 < nla && ndst->la5) {
        fval = (float) loadavg[1];
        rc = PMIx_Info_list_add(ilist, PMIX_NODE_LOAD_AVG5, &fval, PMIX_FLOAT);
        if (PMIX_SUCCESS != rc) {
            return rc;
        }
    }
    if (2 < nla && ndst->la15) {
        fval = (float) loadavg[2];
        rc = PMIx_Info_list_add(ilist, PMIX_NODE_LOAD_AVG15, &fval, PMIX_FLOAT);
        if (PMIX_SUCCESS != rc) {
            return rc;
        }
    }

    /* total physical memory (bytes) - normalize to MB */
    if (ndst->mtot) {
        len = sizeof(physmem);
        if (0 == sysctlbyname("hw.physmem", &physmem, &len, NULL, 0) && sizeof(physmem) == len) {
            fval = (float) ((double) physmem / (1024.0 * 1024.0));
            rc = PMIx_Info_list_add(ilist, PMIX_NODE_MEM_TOTAL, &fval, PMIX_FLOAT);
            if (PMIX_SUCCESS != rc) {
                return rc;
            }
        }
    }

    if (ndst->mfree && vm_pages_mb("vm.stats.vm.v_free_count", &fval)) {
        rc = PMIx_Info_list_add(ilist, PMIX_NODE_MEM_FREE, &fval, PMIX_FLOAT);
        if (PMIX_SUCCESS != rc) {
            return rc;
        }
    }

    /* the buffer cache - what Linux reports as Buffers */
    if (ndst->mbuf) {
        len = sizeof(bufspace);
        if (0 == sysctlbyname("vfs.bufspace", &bufspace, &len, NULL, 0) &&
            sizeof(bufspace) == len) {
            fval = (float) ((double) bufspace / (1024.0 * 1024.0));
            rc = PMIx_Info_list_add(ilist, PMIX_NODE_MEM_BUFFERS, &fval, PMIX_FLOAT);
            if (PMIX_SUCCESS != rc) {
                return rc;
            }
        }
    }

    /* FreeBSD keeps file pages in the same queues as every other page,
     * so it has no count of "cached" memory as such. The inactive queue
     * - pages not recently used, which the kernel reclaims first and
     * which hold most of the file cache - is the closest analog, as the
     * external pages are on macOS */
    if (ndst->mcached && vm_pages_mb("vm.stats.vm.v_inactive_count", &fval)) {
        rc = PMIx_Info_list_add(ilist, PMIX_NODE_MEM_CACHED, &fval, PMIX_FLOAT);
        if (PMIX_SUCCESS != rc) {
            return rc;
        }
    }

    /* swap (pages) - normalize to MB */
    if ((ndst->mswaptot || ndst->mswapfree) && swap_pages(&swtotal, &swused)) {
        if (ndst->mswaptot) {
            fval = (float) ((double) swtotal * (double) getpagesize() / (1024.0 * 1024.0));
            rc = PMIx_Info_list_add(ilist, PMIX_NODE_SWAP_TOTAL, &fval, PMIX_FLOAT);
            if (PMIX_SUCCESS != rc) {
                return rc;
            }
        }
        if (ndst->mswapfree) {
            fval = (float) ((double) (swtotal - swused) * (double) getpagesize()
                            / (1024.0 * 1024.0));
            rc = PMIx_Info_list_add(ilist, PMIX_NODE_MEM_SWAP_FREE, &fval, PMIX_FLOAT);
            if (PMIX_SUCCESS != rc) {
                return rc;
            }
        }
    }

    /* NOTE: FreeBSD has no direct analog for the swap-cached or mapped
     * totals, so those requested fields are not reported. */

    rc = PMIx_Info_list_add(ilist, PMIX_NODE_SAMPLE_TIME, &sample_time, PMIX_TIME);
    return rc;
}

static void evrelease(pmix_status_t status, void *cbdata)
{
    pmix_cb_t *cb = (pmix_cb_t *) cbdata;
    PMIX_HIDE_UNUSED_PARAMS(status);
    PMIX_RELEASE(cb);
}

static void update(int sd, short args, void *cbdata)
{
    pmix_pstat_op_t *op = (pmix_pstat_op_t *) cbdata;
    void *answer, *ilist;
    pmix_status_t rc;
    pmix_data_array_t darray;
    pmix_cb_t *cb;
    pmix_procstats_t zproc;
    pmix_ndstats_t znd;
    pmix_netstats_t znet;
    pmix_dkstats_t zdk;
    bool nettaken, dktaken, syncpass;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    cb = op->cb;
    if (NULL == cb) {
        answer = PMIx_Info_list_start();
        syncpass = false;
    } else {
        answer = cb->cbdata;
        syncpass = true;
    }
    PMIX_PROCSTATS_INIT(&zproc);
    PMIX_NDSTATS_INIT(&znd);
    PMIX_NETSTATS_INIT(&znet);
    PMIX_DKSTATS_INIT(&zdk);
    nettaken = false;
    dktaken = false;

    // start with general directives

    if (op->active) {
        /* Avoid the default event. Both of these have to make it into the
         * answer: without them the sample is delivered as a default event
         * with no range restriction - to every handler in the job rather
         * than to the one process that asked for it - so a failure here
         * is not something to shrug off. */
        PMIX_INFO_LIST_ADD(rc, answer, PMIX_EVENT_NON_DEFAULT, NULL, PMIX_BOOL);
        if (PMIX_SUCCESS != rc) {
            goto error;
        }

        /* target this notification solely to the requestor */
        PMIX_INFO_LIST_ADD(rc, answer, PMIX_EVENT_CUSTOM_RANGE, &op->requestor, PMIX_PROC);
        if (PMIX_SUCCESS != rc) {
            goto error;
        }
    }

    // check for pstat request
    if (0 != memcmp(&op->pstats, &zproc, sizeof(pmix_procstats_t))) {
        rc = pmix_pstat_base_sample_targets(answer, op, proc_stat);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto error;
        }
    }

    // check for node stats
    if (0 != memcmp(&op->ndstats, &znd, sizeof(pmix_ndstats_t))) {
        ilist = PMIx_Info_list_start();
        // start with hostname
        rc = PMIx_Info_list_add(ilist, PMIX_HOSTNAME, pmix_globals.hostname, PMIX_STRING);
        if (PMIX_SUCCESS != rc) {
            PMIx_Info_list_release(ilist);
            goto error;
        }
        // add our nodeID
        rc = PMIx_Info_list_add(ilist, PMIX_NODEID, &pmix_globals.nodeid, PMIX_UINT32);
        if (PMIX_SUCCESS != rc) {
            PMIx_Info_list_release(ilist);
            goto error;
        }
        // collect the stats
        rc = node_stat(ilist, &op->ndstats);
        if (PMIX_SUCCESS != rc && PMIX_ERR_NOT_FOUND != rc) {
            PMIx_Info_list_release(ilist);
            goto error;
        }
        if (0 != memcmp(&op->netstats, &znet, sizeof(pmix_netstats_t))) {
            nettaken = true;
            rc = net_stat(ilist, op->nets, &op->netstats);
            if (PMIX_SUCCESS != rc && PMIX_ERR_NOT_FOUND != rc) {
                PMIx_Info_list_release(ilist);
                goto error;
            }
        }
        if (0 != memcmp(&op->dkstats, &zdk, sizeof(pmix_dkstats_t))) {
            dktaken = true;
            rc = disk_stat(ilist, op->disks, &op->dkstats);
            if (PMIX_SUCCESS != rc && PMIX_ERR_NOT_FOUND != rc) {
                PMIx_Info_list_release(ilist);
                goto error;
            }
        }
        // add this to the final answer
        rc = PMIx_Info_list_convert(ilist, &darray);
        PMIx_Info_list_release(ilist);
        if (PMIX_SUCCESS != rc) {
            /* the conversion creates its array before filling it, so a
             * failure partway still leaves one here to release */
            PMIX_DATA_ARRAY_DESTRUCT(&darray);
            goto error;
        }
        rc = PMIx_Info_list_add(answer, PMIX_NODE_RESOURCE_USAGE, &darray, PMIX_DATA_ARRAY);
        PMIX_DATA_ARRAY_DESTRUCT(&darray);
        if (PMIX_SUCCESS != rc) {
            goto error;
        }
    }

    // check for net stats
    if (!nettaken && 0 != memcmp(&op->netstats, &znet, sizeof(pmix_netstats_t))) {
        rc = net_stat(answer, op->nets, &op->netstats);
        if (PMIX_SUCCESS != rc && PMIX_ERR_NOT_FOUND != rc) {
            goto error;
        }
    }

    // check for disk stats
    if (!dktaken && 0 != memcmp(&op->dkstats, &zdk, sizeof(pmix_dkstats_t))) {
        rc = disk_stat(answer, op->disks, &op->dkstats);
        if (PMIX_SUCCESS != rc && PMIX_ERR_NOT_FOUND != rc) {
            goto error;
        }
    }

    if (NULL == cb) {
        // convert to info array
        rc = PMIx_Info_list_convert(answer, &darray);
        PMIx_Info_list_release(answer);
        if (PMIX_SUCCESS != rc) {
            /* nothing was collected this time around - that is an empty
             * sample, not a failure, and logging it would put a line in
             * the output on every tick of the timer */
            if (PMIX_ERR_EMPTY != rc) {
                PMIX_ERROR_LOG(rc);
            }
            PMIX_DATA_ARRAY_DESTRUCT(&darray);
            return;
        }
        // setup the event
        cb = PMIX_NEW(pmix_cb_t);
        cb->info = (pmix_info_t *) darray.array;
        cb->ninfo = darray.size;
        cb->infocopy = true;
        rc = PMIx_Notify_event(op->eventcode, &pmix_globals.myid,
                               PMIX_RANGE_CUSTOM, cb->info, cb->ninfo,
                               evrelease, (void *) cb);
        if (PMIX_SUCCESS != rc) {
            /* evrelease only runs once the notification has been
             * delivered, so a rejected notification leaves the caddy -
             * and the sample it carries - ours to release */
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(cb);
        }
    }
    return;

error:
    /* On the timer path the answer list is ours and has to go. On the
     * synchronous path it belongs to query(), which reads it as soon as
     * we return - releasing it here left query() converting and
     * releasing freed memory. Hand the failure back through the caddy
     * instead and leave the list alone. */
    if (syncpass) {
        op->cb->status = rc;
    } else {
        PMIx_Info_list_release(answer);
    }
}

/* The monitor's value carries the list of fields being requested as a
 * data array. For a client request that value came off the wire, and
 * nothing between the unpack in pmix_server_monitor() and here looks at
 * it - so check the declared type before reading data.darray, or a value
 * the sender typed as, say, an integer hands us those bytes as a
 * pointer. An absent value (PMIX_UNDEF) is not malformed: it is how a
 * caller asks for every field in the category, which is what the base
 * parse helpers do when handed a NULL array. */
static pmix_status_t monitor_fields(const pmix_info_t *monitor,
                                    pmix_info_t **iptr, size_t *sz)
{
    *iptr = NULL;
    *sz = 0;

    if (PMIX_UNDEF == monitor->value.type) {
        return PMIX_SUCCESS;
    }
    /* A data array holding nothing asks for every field, as a NULL array
     * does - whatever element type it declares, since it has no elements
     * to read. Callers have long asked for "everything" with an empty
     * array typed PMIX_PROC or PMIX_STRING (the examples do), and refusing
     * that broke every one of them */
    if (PMIX_DATA_ARRAY == monitor->value.type &&
        (NULL == monitor->value.data.darray || 0 == monitor->value.data.darray->size)) {
        return PMIX_SUCCESS;
    }
    /* otherwise the fields must be a data array of pmix_info_t */
    if (PMIX_DATA_ARRAY != monitor->value.type ||
        NULL == monitor->value.data.darray ||
        PMIX_INFO != monitor->value.data.darray->type ||
        (0 < monitor->value.data.darray->size &&
         NULL == monitor->value.data.darray->array)) {
        return PMIX_ERR_BAD_PARAM;
    }
    *iptr = (pmix_info_t *) monitor->value.data.darray->array;
    *sz = monitor->value.data.darray->size;
    return PMIX_SUCCESS;
}

static pmix_status_t query(pmix_proc_t *requestor,
                           const pmix_info_t *monitor, pmix_status_t eventcode,
                           const pmix_info_t directives[], size_t ndirs,
                           pmix_info_t **results, size_t *nresults)
{
    pmix_info_t *iptr;
    size_t sz;
    size_t n;
    pmix_status_t rc;
    pmix_data_array_t darray;
    pmix_pstat_op_t *op;
    pmix_netstats_t znet;
    pmix_dkstats_t zdk;
    pmix_info_t *xfer;
    pmix_cb_t cb;

    *results = NULL;
    *nresults = 0;
    PMIX_NETSTATS_INIT(&znet);
    PMIX_DKSTATS_INIT(&zdk);

    if (PMIx_Check_key(monitor->key, PMIX_MONITOR_CANCEL)) {
        // cancel an existing monitor operation - ID must be the provided value
        if (PMIX_STRING != monitor->value.type ||
            NULL == monitor->value.data.string) {
            return PMIX_ERR_BAD_PARAM;
        }
        // cycle through our list of ops operations
        if (0 == pmix_list_get_size(&pmix_pstat_base.ops)) {
            // we don't have any operations - this is okay
            return PMIX_SUCCESS;
        }
        PMIX_LIST_FOREACH(op, &pmix_pstat_base.ops, pmix_pstat_op_t) {
            /* an op only carries an id if the request that created it
             * supplied one - a rate with no PMIX_MONITOR_ID is legal and
             * leaves it NULL, so it can never match a cancel. A monitor
             * is cancelled only by the process that started it, compared
             * exactly rather than with PMIX_CHECK_NSPACE, which treats
             * an empty namespace as a wildcard */
            if (NULL != op->id &&
                0 == strcmp(monitor->value.data.string, op->id) &&
                NULL != requestor &&
                0 == strncmp(op->requestor.nspace, requestor->nspace, PMIX_MAX_NSLEN) &&
                op->requestor.rank == requestor->rank) {
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
    memcpy(&op->requestor, requestor, sizeof(pmix_proc_t));
    op->eventcode = eventcode;

    for (n = 0; n < ndirs; n++) {
        // did they give us an ID for this request?
        if (PMIx_Check_key(directives[n].key, PMIX_MONITOR_ID)) {
            if (PMIX_STRING != directives[n].value.type ||
                NULL == directives[n].value.data.string) {
                PMIX_RELEASE(op);
                return PMIX_ERR_BAD_PARAM;
            }
            if (NULL == op->id) {
                /* nothing stops a caller from naming the monitor twice;
                 * the first name wins, and overwriting would lose the
                 * string we already took */
                op->id = strdup(directives[n].value.data.string);
            }

        } else if (PMIx_Check_key(directives[n].key, PMIX_MONITOR_RESOURCE_RATE)) {
            // they are asking us to update it at regular intervals
            rc = PMIx_Value_get_number(&directives[n].value, (void *) &op->rate, PMIX_UINT32);
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

    // a monitor that repeats is one more timer we service until it ends
    if (0 < op->rate) {
        rc = pmix_pstat_base_may_add(requestor);
        if (PMIX_SUCCESS != rc) {
            PMIX_RELEASE(op);
            return rc;
        }
    }

    // see what data we are being asked to collect
    if (PMIx_Check_key(monitor->key, PMIX_MONITOR_PROC_RESOURCE_USAGE)) {
        // see which values are to be returned
        rc = monitor_fields(monitor, &iptr, &sz);
        if (PMIX_SUCCESS != rc) {
            PMIX_RELEASE(op);
            return rc;
        }
        pmix_pstat_parse_procstats(&op->pstats, iptr, sz);

        // which processes, and which of them the requester may see
        rc = pmix_pstat_base_targets(requestor, directives, ndirs, &op->targets);
        if (PMIX_SUCCESS != rc) {
            PMIX_RELEASE(op);
            return rc;
        }
        PMIX_CONSTRUCT(&cb, pmix_cb_t);
        cb.cbdata = PMIx_Info_list_start();
        op->cb = &cb;
        update(0, 0, (void *) op);
        /* update() reports a collection failure through the caddy - the
         * list it filled is ours, so tear it down here rather than
         * handing the caller a half-built answer under a success */
        if (PMIX_SUCCESS != cb.status) {
            rc = cb.status;
            PMIx_Info_list_release(cb.cbdata);
            PMIX_DESTRUCT(&cb);
            op->cb = NULL;
            PMIX_RELEASE(op);
            return rc;
        }
        // convert to info array
        rc = PMIx_Info_list_convert(cb.cbdata, &darray);
        PMIx_Info_list_release(cb.cbdata);
        PMIX_DESTRUCT(&cb);
        op->cb = NULL;
        if (PMIX_SUCCESS == rc) {
            *results = (pmix_info_t *) darray.array;
            *nresults = darray.size;
        } else if (PMIX_ERR_EMPTY == rc) {
            // nothing found is not an error - and a monitor given a
            // rate still runs, reporting what each sample finds
            PMIX_DATA_ARRAY_DESTRUCT(&darray);
        } else {
            PMIX_DATA_ARRAY_DESTRUCT(&darray);
            PMIX_RELEASE(op);
            return rc;
        }

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
        rc = monitor_fields(monitor, &iptr, &sz);
        if (PMIX_SUCCESS != rc) {
            PMIX_RELEASE(op);
            return rc;
        }
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
        update(0, 0, (void *) op);
        /* update() reports a collection failure through the caddy - the
         * list it filled is ours, so tear it down here rather than
         * handing the caller a half-built answer under a success */
        if (PMIX_SUCCESS != cb.status) {
            rc = cb.status;
            PMIx_Info_list_release(cb.cbdata);
            PMIX_DESTRUCT(&cb);
            op->cb = NULL;
            PMIX_RELEASE(op);
            return rc;
        }
        // add this to the final answer
        rc = PMIx_Info_list_convert(cb.cbdata, &darray);
        if (PMIX_SUCCESS != rc) {
            PMIX_DATA_ARRAY_DESTRUCT(&darray);
            PMIx_Info_list_release(cb.cbdata);
            op->cb = NULL;
            PMIX_DESTRUCT(&cb);
            PMIX_RELEASE(op);
            return rc;
        }
        PMIx_Info_list_release(cb.cbdata);
        PMIX_DESTRUCT(&cb);
        op->cb = NULL;
        // if the array has only two items in it, then those are the hostname
        // and nodeID we provided and not any data - this is equivalent to
        // "empty" as no data was found
        if (3 > darray.size) {
            PMIX_DATA_ARRAY_DESTRUCT(&darray);
        } else {
            PMIX_INFO_CREATE(xfer, 1);
            PMIX_INFO_LOAD(xfer, PMIX_NODE_RESOURCE_USAGE, &darray, PMIX_DATA_ARRAY);
            PMIX_DATA_ARRAY_DESTRUCT(&darray);
            *results = xfer;
            *nresults = 1;
        }

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
        rc = monitor_fields(monitor, &iptr, &sz);
        if (PMIX_SUCCESS != rc) {
            PMIX_RELEASE(op);
            return rc;
        }
        // assemble any provided disk IDs and/or stat specifications
        pmix_pstat_parse_dkstats(&op->disks, &op->dkstats, iptr, sz);
        // collect the stats
        PMIX_CONSTRUCT(&cb, pmix_cb_t);
        cb.cbdata = PMIx_Info_list_start();
        op->cb = &cb;
        update(0, 0, (void *) op);
        /* update() reports a collection failure through the caddy - the
         * list it filled is ours, so tear it down here rather than
         * handing the caller a half-built answer under a success */
        if (PMIX_SUCCESS != cb.status) {
            rc = cb.status;
            PMIx_Info_list_release(cb.cbdata);
            PMIX_DESTRUCT(&cb);
            op->cb = NULL;
            PMIX_RELEASE(op);
            return rc;
        }
        // convert to info array
        rc = PMIx_Info_list_convert(cb.cbdata, &darray);
        PMIx_Info_list_release(cb.cbdata);
        PMIX_DESTRUCT(&cb);
        op->cb = NULL;
        if (PMIX_SUCCESS == rc) {
            *results = (pmix_info_t *) darray.array;
            *nresults = darray.size;
        } else if (PMIX_ERR_EMPTY == rc) {
            // nothing found is not an error - and a monitor given a
            // rate still runs, reporting what each sample finds
            PMIX_DATA_ARRAY_DESTRUCT(&darray);
        } else {
            PMIX_DATA_ARRAY_DESTRUCT(&darray);
            PMIX_RELEASE(op);
            return rc;
        }

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
        rc = monitor_fields(monitor, &iptr, &sz);
        if (PMIX_SUCCESS != rc) {
            PMIX_RELEASE(op);
            return rc;
        }
        // assemble any provided network IDs and/or stat specifications
        pmix_pstat_parse_netstats(&op->nets, &op->netstats, iptr, sz);
        // collect the stats
        PMIX_CONSTRUCT(&cb, pmix_cb_t);
        cb.cbdata = PMIx_Info_list_start();
        op->cb = &cb;
        update(0, 0, (void *) op);
        /* update() reports a collection failure through the caddy - the
         * list it filled is ours, so tear it down here rather than
         * handing the caller a half-built answer under a success */
        if (PMIX_SUCCESS != cb.status) {
            rc = cb.status;
            PMIx_Info_list_release(cb.cbdata);
            PMIX_DESTRUCT(&cb);
            op->cb = NULL;
            PMIX_RELEASE(op);
            return rc;
        }
        // convert to info array
        rc = PMIx_Info_list_convert(cb.cbdata, &darray);
        PMIx_Info_list_release(cb.cbdata);
        PMIX_DESTRUCT(&cb);
        op->cb = NULL;
        if (PMIX_SUCCESS == rc) {
            *results = (pmix_info_t *) darray.array;
            *nresults = darray.size;
        } else if (PMIX_ERR_EMPTY == rc) {
            // nothing found is not an error - and a monitor given a
            // rate still runs, reporting what each sample finds
            PMIX_DATA_ARRAY_DESTRUCT(&darray);
        } else {
            PMIX_DATA_ARRAY_DESTRUCT(&darray);
            PMIX_RELEASE(op);
            return rc;
        }

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

    // the monitor key matched none of the statistics we collect - say so
    // rather than reporting a spurious success, so the caller can fall
    // back to the host (or another framework)
    PMIX_RELEASE(op);
    return PMIX_ERR_NOT_SUPPORTED;
}
