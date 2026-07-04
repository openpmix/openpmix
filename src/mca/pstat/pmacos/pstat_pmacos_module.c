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
 * Copyright (c) 2025      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "pmix_config.h"
#include "pmix_common.h"

/* This component is only compiled on macOS (Darwin), where the
 * mach, libproc, sysctl, IOKit, and BSD networking interfaces below
 * are all guaranteed to be present. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifdef HAVE_SYS_TIME_H
#    include <sys/time.h>
#endif
#include <sys/proc.h> /* for the SIDL/SRUN/... process-state codes */
#include <sys/sysctl.h>
#include <sys/types.h>

#include <libproc.h>
#include <mach/mach.h>
#include <mach/mach_host.h>
#include <mach/vm_statistics.h>

#include <ifaddrs.h>
#include <net/if.h>
#include <net/if_dl.h>

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOBSD.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/storage/IOBlockStorageDriver.h>

#include "src/include/pmix_globals.h"
#include "src/server/pmix_server_ops.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_printf.h"

#include "src/mca/pstat/base/base.h"
#include "pstat_pmacos.h"

/*
 * API functions
 */
static pmix_status_t pmacos_module_init(void);
static pmix_status_t query(pmix_proc_t *requestor,
                           const pmix_info_t *monitor, pmix_status_t error,
                           const pmix_info_t directives[], size_t ndirs,
                           pmix_info_t **results, size_t *nresults);
static pmix_status_t pmacos_module_fini(void);

/*
 * macOS pstat module
 */
const pmix_pstat_base_module_t pmix_pstat_pmacos_module = {
    /* Initialization function */
    .init = pmacos_module_init,
    .query = query,
    .finalize = pmacos_module_fini
};

/* Passing 0 (MACH_PORT_NULL) as the IOKit main port requests the
 * default port. Doing so works across all macOS releases without
 * depending on either kIOMainPortDefault (only defined on macOS 12+)
 * or the deprecated kIOMasterPortDefault (which trips -Werror on newer
 * SDKs). */
#define PMIX_PSTAT_IOMAIN_PORT ((mach_port_t) 0)

static pmix_status_t pmacos_module_init(void)
{
    return PMIX_SUCCESS;
}

static pmix_status_t pmacos_module_fini(void)
{
    return PMIX_SUCCESS;
}

/* Translate a macOS proc_bsdinfo status code into the single-character
 * process-state string used by the Linux reader, so callers see a
 * consistent representation regardless of platform. */
static const char *proc_state_string(uint32_t status)
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
    default:
        return "?";
    }
}

static pmix_status_t proc_stat(void *answer, pmix_peer_t *peer,
                               pmix_procstats_t *pst)
{
    pmix_proc_t proc;
    pmix_status_t rc;
    struct proc_taskinfo pti;
    struct proc_bsdinfo bsd;
    char comm[256];
    int nb;
    float fval;
    uint16_t u16;
    int32_t i32;
    struct timeval evtime;
    double dtime;
    void *cache;
    time_t sample_time;
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

    /* pull the task-level info: CPU time, thread count, priority, and
     * virtual/resident sizes */
    nb = proc_pidinfo(peer->info->pid, PROC_PIDTASKINFO, 0, &pti, sizeof(pti));
    if (nb == (int) sizeof(pti)) {
        if (pst->time) {
            /* pti reports CPU time in nanoseconds */
            dtime = (double) (pti.pti_total_user + pti.pti_total_system) / 1.0e9;
            evtime.tv_sec = (int) dtime;
            evtime.tv_usec = (int) (1000000.0 * (dtime - evtime.tv_sec));
            rc = PMIx_Info_list_add(cache, PMIX_PROC_TIME, (void *) &evtime, PMIX_TIMEVAL);
            if (PMIX_SUCCESS != rc) {
                PMIx_Info_list_release(cache);
                return rc;
            }
        }
        if (pst->pri) {
            i32 = (int32_t) pti.pti_priority;
            rc = PMIx_Info_list_add(cache, PMIX_PROC_PRIORITY, &i32, PMIX_INT32);
            if (PMIX_SUCCESS != rc) {
                PMIx_Info_list_release(cache);
                return rc;
            }
        }
        if (pst->nthreads) {
            u16 = (uint16_t) pti.pti_threadnum;
            rc = PMIx_Info_list_add(cache, PMIX_PROC_NUM_THREADS, &u16, PMIX_UINT16);
            if (PMIX_SUCCESS != rc) {
                PMIx_Info_list_release(cache);
                return rc;
            }
        }
        if (pst->vsize) {
            /* normalize bytes to MB to match the Linux reader */
            fval = (float) pti.pti_virtual_size / (1024.0 * 1024.0);
            rc = PMIx_Info_list_add(cache, PMIX_PROC_VSIZE, &fval, PMIX_FLOAT);
            if (PMIX_SUCCESS != rc) {
                PMIx_Info_list_release(cache);
                return rc;
            }
        }
        if (pst->rss) {
            fval = (float) pti.pti_resident_size / (1024.0 * 1024.0);
            rc = PMIx_Info_list_add(cache, PMIX_PROC_RSS, &fval, PMIX_FLOAT);
            if (PMIX_SUCCESS != rc) {
                PMIx_Info_list_release(cache);
                return rc;
            }
        }
    }

    /* pull the BSD-level info for the process state */
    if (pst->state) {
        nb = proc_pidinfo(peer->info->pid, PROC_PIDTBSDINFO, 0, &bsd, sizeof(bsd));
        if (nb == (int) sizeof(bsd)) {
            rc = PMIx_Info_list_add(cache, PMIX_PROC_OS_STATE,
                                    (char *) proc_state_string(bsd.pbi_status), PMIX_STRING);
            if (PMIX_SUCCESS != rc) {
                PMIx_Info_list_release(cache);
                return rc;
            }
        }
    }

    /* the command name */
    if (pst->cmdline) {
        memset(comm, 0, sizeof(comm));
        if (0 < proc_name(peer->info->pid, comm, sizeof(comm))) {
            rc = PMIx_Info_list_add(cache, PMIX_CMD_LINE, comm, PMIX_STRING);
            if (PMIX_SUCCESS != rc) {
                PMIx_Info_list_release(cache);
                return rc;
            }
        }
    }

    /* NOTE: macOS does not expose an inexpensive per-process last-CPU
     * (pctcpu/cpu) or proportional/peak virtual size (pss/pkvsize), so
     * those fields are simply not reported here. */

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

/* Pull a signed 64-bit integer property out of an IOKit statistics
 * dictionary; returns 0 if the key is absent or not a number. */
static uint64_t cf_dict_u64(CFDictionaryRef dict, CFStringRef key)
{
    CFNumberRef num;
    int64_t val = 0;

    num = (CFNumberRef) CFDictionaryGetValue(dict, key);
    if (NULL != num && CFGetTypeID(num) == CFNumberGetTypeID()) {
        CFNumberGetValue(num, kCFNumberSInt64Type, &val);
    }
    return (uint64_t) val;
}

/* Recover the BSD device name (e.g. "disk0") for an IOBlockStorageDriver
 * by searching its child IOMedia objects for the kIOBSDNameKey property.
 * Returns true and fills name/namelen on success. */
static bool disk_bsd_name(io_registry_entry_t drive, char *name, size_t namelen)
{
    io_iterator_t children;
    io_registry_entry_t child;
    CFStringRef cfname;
    bool found = false;
    /* The IOKit registry-plane argument is typed io_name_t (char[128]);
     * copy the plane name into a full-sized buffer so the compiler's
     * buffer-size analysis doesn't flag the short string literal. */
    io_name_t plane;

    memset(plane, 0, sizeof(plane));
    strlcpy(plane, kIOServicePlane, sizeof(plane));

    if (KERN_SUCCESS != IORegistryEntryGetChildIterator(drive, plane, &children)) {
        return false;
    }
    while (!found && 0 != (child = IOIteratorNext(children))) {
        cfname = (CFStringRef) IORegistryEntrySearchCFProperty(child, plane,
                                                               CFSTR(kIOBSDNameKey),
                                                               kCFAllocatorDefault,
                                                               kIORegistryIterateRecursively);
        if (NULL != cfname) {
            if (CFStringGetCString(cfname, name, namelen, kCFStringEncodingUTF8)) {
                found = true;
            }
            CFRelease(cfname);
        }
        IOObjectRelease(child);
    }
    IOObjectRelease(children);
    return found;
}

static pmix_status_t disk_stat(void *answer,
                               char **disks,
                               pmix_dkstats_t *dkst)
{
    io_iterator_t drives;
    io_registry_entry_t drive;
    CFMutableDictionaryRef match;
    CFDictionaryRef stats;
    char bsdname[128];
    void *cache;
    size_t n;
    bool takeit;
    uint64_t u64;
    pmix_data_array_t darray;
    time_t sample_time;
    pmix_status_t rc;

    /* enumerate the block-storage drivers - each carries a per-device
     * statistics dictionary */
    match = IOServiceMatching(kIOBlockStorageDriverClass);
    if (NULL == match) {
        return PMIX_ERR_NOT_FOUND;
    }
    if (KERN_SUCCESS != IOServiceGetMatchingServices(PMIX_PSTAT_IOMAIN_PORT, match, &drives)) {
        /* IOServiceGetMatchingServices consumes the match dictionary */
        return PMIX_ERR_NOT_FOUND;
    }

    while (0 != (drive = IOIteratorNext(drives))) {
        /* determine the device's BSD name (disk0, disk1, ...) */
        if (!disk_bsd_name(drive, bsdname, sizeof(bsdname))) {
            IOObjectRelease(drive);
            continue;
        }
        // see if this is a disk they want
        if (NULL == disks) {
            takeit = true;
        } else {
            takeit = false;
            for (n = 0; NULL != disks[n]; n++) {
                if (0 == strcmp(bsdname, disks[n])) {
                    takeit = true;
                    break;
                }
            }
        }
        if (!takeit) {
            IOObjectRelease(drive);
            continue;
        }

        stats = (CFDictionaryRef) IORegistryEntryCreateCFProperty(drive,
                                        CFSTR(kIOBlockStorageDriverStatisticsKey),
                                        kCFAllocatorDefault, 0);
        if (NULL == stats) {
            IOObjectRelease(drive);
            continue;
        }
        time(&sample_time);

        /* take the fields of interest */
        cache = PMIx_Info_list_start();
        // start with the diskID
        rc = PMIx_Info_list_add(cache, PMIX_DISK_ID, bsdname, PMIX_STRING);
        if (PMIX_SUCCESS != rc) {
            goto diskerr;
        }
        if (dkst->rdcompleted) {
            u64 = cf_dict_u64(stats, CFSTR(kIOBlockStorageDriverStatisticsReadsKey));
            rc = PMIx_Info_list_add(cache, PMIX_DISK_READ_COMPLETED, &u64, PMIX_UINT64);
            if (PMIX_SUCCESS != rc) {
                goto diskerr;
            }
        }
        if (dkst->rdsectors) {
            /* macOS reports bytes; normalize to 512-byte sectors to
             * match the Linux reader's units */
            u64 = cf_dict_u64(stats, CFSTR(kIOBlockStorageDriverStatisticsBytesReadKey)) / 512;
            rc = PMIx_Info_list_add(cache, PMIX_DISK_READ_SECTORS, &u64, PMIX_UINT64);
            if (PMIX_SUCCESS != rc) {
                goto diskerr;
            }
        }
        if (dkst->rdms) {
            /* total read time is reported in nanoseconds */
            u64 = cf_dict_u64(stats, CFSTR(kIOBlockStorageDriverStatisticsTotalReadTimeKey))
                  / 1000000;
            rc = PMIx_Info_list_add(cache, PMIX_DISK_READ_MILLISEC, &u64, PMIX_UINT64);
            if (PMIX_SUCCESS != rc) {
                goto diskerr;
            }
        }
        if (dkst->wrtcompleted) {
            u64 = cf_dict_u64(stats, CFSTR(kIOBlockStorageDriverStatisticsWritesKey));
            rc = PMIx_Info_list_add(cache, PMIX_DISK_WRITE_COMPLETED, &u64, PMIX_UINT64);
            if (PMIX_SUCCESS != rc) {
                goto diskerr;
            }
        }
        if (dkst->wrtsectors) {
            u64 = cf_dict_u64(stats, CFSTR(kIOBlockStorageDriverStatisticsBytesWrittenKey)) / 512;
            rc = PMIx_Info_list_add(cache, PMIX_DISK_WRITE_SECTORS, &u64, PMIX_UINT64);
            if (PMIX_SUCCESS != rc) {
                goto diskerr;
            }
        }
        if (dkst->wrtms) {
            u64 = cf_dict_u64(stats, CFSTR(kIOBlockStorageDriverStatisticsTotalWriteTimeKey))
                  / 1000000;
            rc = PMIx_Info_list_add(cache, PMIX_DISK_WRITE_MILLISEC, &u64, PMIX_UINT64);
            if (PMIX_SUCCESS != rc) {
                goto diskerr;
            }
        }
        /* NOTE: macOS does not expose merged-request counts (rdmerged/
         * wrtmerged) nor the in-progress/weighted-io fields (ioprog/ioms/
         * ioweight), so those requested fields are not reported. */

        // record the sample time
        rc = PMIx_Info_list_add(cache, PMIX_DISK_SAMPLE_TIME, &sample_time, PMIX_TIME);
        if (PMIX_SUCCESS != rc) {
            goto diskerr;
        }

        // add this to the final answer
        rc = PMIx_Info_list_convert(cache, &darray);
        PMIx_Info_list_release(cache);
        CFRelease(stats);
        IOObjectRelease(drive);
        if (PMIX_SUCCESS != rc) {
            IOObjectRelease(drives);
            return rc;
        }
        rc = PMIx_Info_list_add(answer, PMIX_DISK_RESOURCE_USAGE, &darray, PMIX_DATA_ARRAY);
        if (PMIX_SUCCESS != rc) {
            IOObjectRelease(drives);
            return rc;
        }
        continue;

    diskerr:
        PMIx_Info_list_release(cache);
        CFRelease(stats);
        IOObjectRelease(drive);
        IOObjectRelease(drives);
        return rc;
    }
    IOObjectRelease(drives);
    return PMIX_SUCCESS;
}

static pmix_status_t net_stat(void *answer, char **nets,
                              pmix_netstats_t *netst)
{
    struct ifaddrs *ifap, *ifa;
    struct if_data *ifd;
    void *cache;
    size_t n;
    bool takeit;
    uint64_t u64;
    pmix_data_array_t darray;
    time_t sample_time;
    pmix_status_t rc;

    if (0 != getifaddrs(&ifap)) {
        /* not an error if we cannot read this as it isn't critical */
        return PMIX_ERR_NOT_FOUND;
    }

    for (ifa = ifap; NULL != ifa; ifa = ifa->ifa_next) {
        /* the per-interface counters live on the AF_LINK entry */
        if (NULL == ifa->ifa_addr || AF_LINK != ifa->ifa_addr->sa_family ||
            NULL == ifa->ifa_data) {
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
        time(&sample_time);

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
            freeifaddrs(ifap);
            return rc;
        }
        rc = PMIx_Info_list_add(answer, PMIX_NETWORK_RESOURCE_USAGE, &darray, PMIX_DATA_ARRAY);
        if (PMIX_SUCCESS != rc) {
            freeifaddrs(ifap);
            return rc;
        }
        continue;

    neterr:
        PMIx_Info_list_release(cache);
        freeifaddrs(ifap);
        return rc;
    }
    freeifaddrs(ifap);
    return PMIX_SUCCESS;
}

static pmix_status_t node_stat(void *ilist, pmix_ndstats_t *ndst)
{
    double loadavg[3];
    int nla;
    int64_t memsize;
    struct xsw_usage swap;
    vm_statistics64_data_t vm;
    vm_size_t pagesize = 0;
    mach_msg_type_number_t cnt;
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
        len = sizeof(memsize);
        if (0 == sysctlbyname("hw.memsize", &memsize, &len, NULL, 0)) {
            fval = (float) memsize / (1024.0 * 1024.0);
            rc = PMIx_Info_list_add(ilist, PMIX_NODE_MEM_TOTAL, &fval, PMIX_FLOAT);
            if (PMIX_SUCCESS != rc) {
                return rc;
            }
        }
    }

    /* free and cached memory from the mach VM statistics */
    if (ndst->mfree || ndst->mcached) {
        if (KERN_SUCCESS == host_page_size(mach_host_self(), &pagesize)) {
            cnt = HOST_VM_INFO64_COUNT;
            if (KERN_SUCCESS == host_statistics64(mach_host_self(), HOST_VM_INFO64,
                                                  (host_info64_t) &vm, &cnt)) {
                if (ndst->mfree) {
                    fval = (float) ((uint64_t) vm.free_count * (uint64_t) pagesize)
                           / (1024.0 * 1024.0);
                    rc = PMIx_Info_list_add(ilist, PMIX_NODE_MEM_FREE, &fval, PMIX_FLOAT);
                    if (PMIX_SUCCESS != rc) {
                        return rc;
                    }
                }
                if (ndst->mcached) {
                    /* externally-backed (file-cache) pages are the closest
                     * analog to Linux "cached" memory */
                    fval = (float) ((uint64_t) vm.external_page_count * (uint64_t) pagesize)
                           / (1024.0 * 1024.0);
                    rc = PMIx_Info_list_add(ilist, PMIX_NODE_MEM_CACHED, &fval, PMIX_FLOAT);
                    if (PMIX_SUCCESS != rc) {
                        return rc;
                    }
                }
            }
        }
    }

    /* swap usage (bytes) - normalize to MB */
    if (ndst->mswaptot || ndst->mswapfree) {
        len = sizeof(swap);
        if (0 == sysctlbyname("vm.swapusage", &swap, &len, NULL, 0)) {
            if (ndst->mswaptot) {
                fval = (float) swap.xsu_total / (1024.0 * 1024.0);
                rc = PMIx_Info_list_add(ilist, PMIX_NODE_SWAP_TOTAL, &fval, PMIX_FLOAT);
                if (PMIX_SUCCESS != rc) {
                    return rc;
                }
            }
            if (ndst->mswapfree) {
                fval = (float) swap.xsu_avail / (1024.0 * 1024.0);
                rc = PMIx_Info_list_add(ilist, PMIX_NODE_MEM_SWAP_FREE, &fval, PMIX_FLOAT);
                if (PMIX_SUCCESS != rc) {
                    return rc;
                }
            }
        }
    }

    /* NOTE: macOS has no direct analog for the buffers, swap-cached, or
     * mapped totals, so those requested fields are not reported. */

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

    // start with general directives

    if (op->active) {
        // avoid the default event
        PMIX_INFO_LIST_ADD(rc, answer, PMIX_EVENT_NON_DEFAULT, NULL, PMIX_BOOL);

        /* target this notification solely to the requestor */
        PMIX_INFO_LIST_ADD(rc, answer, PMIX_EVENT_CUSTOM_RANGE, &op->requestor, PMIX_PROC);
    }

    // check for pstat request
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

    // check for node stats
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

    // check for net stats
    if (!nettaken && 0 != memcmp(&op->netstats, &znet, sizeof(pmix_netstats_t))) {
        rc = net_stat(answer, op->nets, &op->netstats);
        if (PMIX_SUCCESS != rc) {
            PMIx_Info_list_release(answer);
            goto reset;
        }
    }

    // check for disk stats
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
        cb->info = (pmix_info_t *) darray.array;
        cb->ninfo = darray.size;
        cb->infocopy = true;
        rc = PMIx_Notify_event(op->eventcode, &pmix_globals.myid,
                               PMIX_RANGE_CUSTOM, cb->info, cb->ninfo,
                               evrelease, (void *) cb);
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
    memcpy(&op->requestor, requestor, sizeof(pmix_proc_t));
    op->eventcode = eventcode;

    for (n = 0; n < ndirs; n++) {
        // did they give us an ID for this request?
        if (PMIx_Check_key(directives[n].key, PMIX_MONITOR_ID)) {
            op->id = strdup(directives[n].value.data.string);

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

    // see what data we are being asked to collect
    if (PMIx_Check_key(monitor->key, PMIX_MONITOR_PROC_RESOURCE_USAGE)) {
        tgtprocsgiven = false;
        // see which values are to be returned
        iptr = (pmix_info_t *) monitor->value.data.darray->array;
        sz = monitor->value.data.darray->size;
        pmix_pstat_parse_procstats(&op->pstats, iptr, sz);

        // we already know this request involves us since it was checked
        // before calling us, so see which of our processes are included
        if (NULL == directives) {
            // all of our processes are included - no ID or rate can be included
            for (k = 0; k < pmix_server_globals.clients.size; k++) {
                ptr = (pmix_peer_t *) pmix_pointer_array_get_item(&pmix_server_globals.clients, k);
                if (NULL == ptr) {
                    continue;
                }
                PMIX_PSTAT_APPEND_PEER_UNIQUE(&op->peers, ptr);
            }
            goto processprocs;
        }

        // check for specific procs and directives
        for (n = 0; n < ndirs; n++) {
            if (PMIx_Check_key(directives[n].key, PMIX_MONITOR_TARGET_PROCS)) {
                if (PMIX_DATA_ARRAY != directives[n].value.type ||
                    NULL == directives[n].value.data.darray ||
                    NULL == directives[n].value.data.darray->array) {
                    PMIX_RELEASE(op);
                    return PMIX_ERR_BAD_PARAM;
                }
                tgtprocsgiven = true;
                // they specified the procs
                procs = (pmix_proc_t *) directives[n].value.data.darray->array;
                sz = directives[n].value.data.darray->size;
                for (m = 0; m < sz; m++) {
                    for (k = 0; k < pmix_server_globals.clients.size; k++) {
                        ptr = (pmix_peer_t *) pmix_pointer_array_get_item(&pmix_server_globals.clients, k);
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
                ppid = (pmix_node_pid_t *) directives[n].value.data.darray->array;
                sz = directives[n].value.data.darray->size;
                for (m = 0; m < sz; m++) {
                    // see if this node is us
                    if (ppid[m].nodeid == pmix_globals.nodeid ||
                        pmix_check_local(ppid[m].hostname)) {
                        // is this one of our procs?
                        for (k = 0; k < pmix_server_globals.clients.size; k++) {
                            ptr = (pmix_peer_t *) pmix_pointer_array_get_item(&pmix_server_globals.clients, k);
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
            for (k = 0; k < pmix_server_globals.clients.size; k++) {
                ptr = (pmix_peer_t *) pmix_pointer_array_get_item(&pmix_server_globals.clients, k);
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
        update(0, 0, (void *) op);
        // convert to info array
        rc = PMIx_Info_list_convert(cb.cbdata, &darray);
        PMIx_Info_list_release(cb.cbdata);
        PMIX_DESTRUCT(&cb);
        op->cb = NULL;
        if (PMIX_SUCCESS != rc) {
            PMIX_DATA_ARRAY_DESTRUCT(&darray);
            PMIX_RELEASE(op);
            if (PMIX_ERR_EMPTY == rc) {
                // this is not an error
                rc = PMIX_SUCCESS;
            }
            return rc;
        }
        *results = (pmix_info_t *) darray.array;
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
        iptr = (pmix_info_t *) monitor->value.data.darray->array;
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
        update(0, 0, (void *) op);
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
        // if the array has only two items in it, then those are the hostname
        // and nodeID we provided and not any data - this is equivalent to
        // "empty" as no data was found
        if (3 > darray.size) {
            PMIX_DATA_ARRAY_DESTRUCT(&darray);
            PMIX_RELEASE(op);
            return PMIX_SUCCESS;
        }
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
        iptr = (pmix_info_t *) monitor->value.data.darray->array;
        sz = monitor->value.data.darray->size;
        // assemble any provided disk IDs and/or stat specifications
        pmix_pstat_parse_dkstats(&op->disks, &op->dkstats, iptr, sz);
        // collect the stats
        PMIX_CONSTRUCT(&cb, pmix_cb_t);
        cb.cbdata = PMIx_Info_list_start();
        op->cb = &cb;
        update(0, 0, (void *) op);
        // convert to info array
        rc = PMIx_Info_list_convert(cb.cbdata, &darray);
        PMIx_Info_list_release(cb.cbdata);
        PMIX_DESTRUCT(&cb);
        op->cb = NULL;
        if (PMIX_SUCCESS != rc) {
            PMIX_DATA_ARRAY_DESTRUCT(&darray);
            PMIX_RELEASE(op);
            if (PMIX_ERR_EMPTY == rc) {
                // empty = nothing found, not an error
                rc = PMIX_SUCCESS;
            }
            return rc;
        }
        *results = (pmix_info_t *) darray.array;
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
        iptr = (pmix_info_t *) monitor->value.data.darray->array;
        sz = monitor->value.data.darray->size;
        // assemble any provided network IDs and/or stat specifications
        pmix_pstat_parse_netstats(&op->nets, &op->netstats, iptr, sz);
        // collect the stats
        PMIX_CONSTRUCT(&cb, pmix_cb_t);
        cb.cbdata = PMIx_Info_list_start();
        op->cb = &cb;
        update(0, 0, (void *) op);
        // convert to info array
        rc = PMIx_Info_list_convert(cb.cbdata, &darray);
        PMIx_Info_list_release(cb.cbdata);
        PMIX_DESTRUCT(&cb);
        op->cb = NULL;
        if (PMIX_SUCCESS != rc) {
            PMIX_RELEASE(op);
            if (PMIX_ERR_EMPTY == rc) {
                // empty = nothing found, not an error
                rc = PMIX_SUCCESS;
            }
            return rc;
        }
        *results = (pmix_info_t *) darray.array;
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
