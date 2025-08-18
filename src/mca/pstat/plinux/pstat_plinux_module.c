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
 *
 * Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "pmix_config.h"
#include "pmix_common.h"

/* This component will only be compiled on Plinux, where we are
   guaranteed to have <unistd.h> and friends */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifdef HAVE_SYS_TIME_H
#    include <sys/time.h>
#endif

#include <sys/param.h> /* for HZ to convert jiffies to actual time */

#include "src/include/pmix_globals.h"
#include "src/server/pmix_server_ops.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_printf.h"

#include "src/mca/pstat/base/base.h"
#include "pstat_plinux.h"

/*
 * API functions
 */
static pmix_status_t plinux_module_init(void);
static pmix_status_t query(pmix_proc_t *requestor,
                           const pmix_info_t *monitor, pmix_status_t error,
                           const pmix_info_t directives[], size_t ndirs,
                           pmix_info_t **results, size_t *nresults);
static pmix_status_t plinux_module_fini(void);

/*
 * Plinux pstat module
 */
const pmix_pstat_base_module_t pmix_pstat_plinux_module = {
    /* Initialization function */
    .init = plinux_module_init,
    .query = query,
    .finalize = plinux_module_fini
};

#define PMIX_STAT_MAX_LENGTH 1024

/* Local functions */
static char *local_getline(FILE *fp);
static char *local_stripper(char *data);
static void local_getfields(char *data, char ***fields);

/* Local data */
static char input[PMIX_STAT_MAX_LENGTH];

static pmix_status_t plinux_module_init(void)
{
    return PMIX_SUCCESS;
}

static pmix_status_t plinux_module_fini(void)
{
    return PMIX_SUCCESS;
}

static char *next_field(char *ptr, int barrier)
{
    int i = 0;

    /* we are probably pointing to the last char
     * of the current field, so look for whitespace
     */
    while (!isspace(*ptr) && i < barrier) {
        ptr++; /* step over the current char */
        i++;
    }

    /* now look for the next field */
    while (isspace(*ptr) && i < barrier) {
        ptr++;
        i++;
    }

    return ptr;
}

static float convert_value(char *value)
{
    char *ptr;
    float fval;

    /* compute base value */
    fval = (float) strtoul(value, &ptr, 10);
    /* get the unit multiplier */
    if (NULL != ptr && NULL != strstr(ptr, "kB")) {
        fval /= 1024.0;
    }
    return fval;
}

static pmix_status_t proc_stat(void *answer, pmix_peer_t *peer,
                               pmix_procstats_t *pst)
{
    pmix_proc_t proc;
    pmix_status_t rc;
    char data[4096], state[2];
    int fd;
    int32_t i32;
    size_t numchars;
    char *ptr, *eptr;
    int len, itime;
    float fval;
    double dtime;
    uint16_t u16;
    FILE *fp;
    char *dptr, *value;
    void *cache;
    struct timeval evtime;
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

    /* create the stat filename for this proc */
    numchars = pmix_snprintf(data, sizeof(data), "/proc/%d/stat", peer->info->pid);
    if (numchars >= sizeof(data)) {
        PMIx_Info_list_release(cache);
        return PMIX_ERROR;
    }

    if (0 > (fd = open(data, O_RDONLY))) {
        /* can't access this file - most likely, this means we
         * aren't really on a supported system, or the proc no
         * longer exists. Just return an error
         */
        PMIx_Info_list_release(cache);
        return PMIX_ERROR;
    }

    /* absorb all of the file's contents in one gulp - we'll process
     * it once it is in memory for speed
     */
    memset(data, 0, sizeof(data));
    len = read(fd, data, sizeof(data) - 1);
    if (len < 0) {
        /* This shouldn't happen! */
        close(fd);
        PMIx_Info_list_release(cache);
        return PMIX_ERROR;
    }
    close(fd);

    /* remove newline at end */
    data[len] = '\0';

    /* the stat file consists of a single line in a carefully formatted
     * form. Parse it field by field as per proc(3) to get the ones we want
     */

    /* the cmd is surrounded by parentheses - find the start */
    if (NULL == (ptr = strchr(data, '('))) {
        /* no cmd => something wrong with data, return error */
        PMIx_Info_list_release(cache);
        return PMIX_ERR_BAD_PARAM;
    }
    /* step over the paren */
    ptr++;

    /* find the ending paren */
    if (NULL == (eptr = strchr(ptr, ')'))) {
        /* no end to cmd => something wrong with data, return error */
        PMIx_Info_list_release(cache);
        return PMIX_ERR_BAD_PARAM;
    }

    /* save the cmd name, up to the limit of the array */
    *eptr = '\0';
    if (pst->cmdline) {
        rc = PMIx_Info_list_add(cache, PMIX_CMD_LINE, ptr, PMIX_STRING);
        if (PMIX_SUCCESS != rc) {
            PMIx_Info_list_release(cache);
            return rc;
        }
    }
    *eptr = ')';

    /* move to the next field in the data */
    ptr = next_field(eptr, len);

    /* next is the process state - a single character */
    state[0] = *ptr;
    state[1] = '\0';
    if (pst->state) {
        rc = PMIx_Info_list_add(cache, PMIX_PROC_OS_STATE, state, PMIX_STRING);
        if (PMIX_SUCCESS != rc) {
            PMIx_Info_list_release(cache);
            return rc;
        }
    }
    /* move to next field */
    ptr = next_field(ptr, len);

    /* skip fields until we get to the times */
    ptr = next_field(ptr, len); /* ppid */
    ptr = next_field(ptr, len); /* pgrp */
    ptr = next_field(ptr, len); /* session */
    ptr = next_field(ptr, len); /* tty_nr */
    ptr = next_field(ptr, len); /* tpgid */
    ptr = next_field(ptr, len); /* flags */
    ptr = next_field(ptr, len); /* minflt */
    ptr = next_field(ptr, len); /* cminflt */
    ptr = next_field(ptr, len); /* majflt */
    ptr = next_field(ptr, len); /* cmajflt */

    /* grab the process time usage fields */
    itime = strtoul(ptr, &ptr, 10);  /* utime */
    itime += strtoul(ptr, &ptr, 10); /* add the stime */
    /* convert to time in seconds */
    dtime = (double) itime / (double) HZ;
    evtime.tv_sec = (int) dtime;
    evtime.tv_usec = (int) (1000000.0 * (dtime - evtime.tv_sec));
    if (pst->time) {
        rc = PMIx_Info_list_add(cache, PMIX_PROC_TIME, (void*)&evtime, PMIX_TIMEVAL);
        if (PMIX_SUCCESS != rc) {
            PMIx_Info_list_release(cache);
            return rc;
        }
    }
    /* move to next field */
    ptr = next_field(ptr, len);

    /* skip fields until we get to priority */
    ptr = next_field(ptr, len); /* cutime */
    ptr = next_field(ptr, len); /* cstime */

    /* save the priority */
    i32 = strtol(ptr, &ptr, 10);
    if (pst->pri) {
        rc = PMIx_Info_list_add(cache, PMIX_PROC_PRIORITY, &i32, PMIX_INT32);
        if (PMIX_SUCCESS != rc) {
            PMIx_Info_list_release(cache);
            return rc;
        }
    }
    /* move to next field */
    ptr = next_field(ptr, len);

    /* skip nice */
    ptr = next_field(ptr, len);

    /* get number of threads */
    u16 = strtoul(ptr, &ptr, 10);
    if (pst->nthreads) {
        rc = PMIx_Info_list_add(cache, PMIX_PROC_NUM_THREADS, &u16, PMIX_UINT16);
        if (PMIX_SUCCESS != rc) {
            PMIx_Info_list_release(cache);
            return rc;
        }
    }
    /* move to next field */
    ptr = next_field(ptr, len);

    /* skip fields until we get to processor id */
    ptr = next_field(ptr, len); /* itrealvalue */
    ptr = next_field(ptr, len); /* starttime */
    ptr = next_field(ptr, len); /* vsize */
    ptr = next_field(ptr, len); /* rss */
    ptr = next_field(ptr, len); /* rss limit */
    ptr = next_field(ptr, len); /* startcode */
    ptr = next_field(ptr, len); /* endcode */
    ptr = next_field(ptr, len); /* startstack */
    ptr = next_field(ptr, len); /* kstkesp */
    ptr = next_field(ptr, len); /* kstkeip */
    ptr = next_field(ptr, len); /* signal */
    ptr = next_field(ptr, len); /* blocked */
    ptr = next_field(ptr, len); /* sigignore */
    ptr = next_field(ptr, len); /* sigcatch */
    ptr = next_field(ptr, len); /* wchan */
    ptr = next_field(ptr, len); /* nswap */
    ptr = next_field(ptr, len); /* cnswap */
    ptr = next_field(ptr, len); /* exit_signal */

    /* finally - get the processor */
    u16 = strtol(ptr, NULL, 10);
    if (pst->cpu) {
        rc = PMIx_Info_list_add(cache, PMIX_PROC_CPU, &u16, PMIX_UINT16);
        if (PMIX_SUCCESS != rc) {
            PMIx_Info_list_release(cache);
            return rc;
        }
    }
    /* that's all we care about from this data - ignore the rest */

    /* now create the status filename for this proc */
    memset(data, 0, sizeof(data));
    numchars = pmix_snprintf(data, sizeof(data), "/proc/%d/status", peer->info->pid);
    if (numchars >= sizeof(data)) {
        PMIx_Info_list_release(cache);
        return PMIX_ERROR;
    }

    if (NULL == (fp = fopen(data, "r"))) {
        /* ignore this */
        goto complete;
    }

    /* parse it according to proc(3) */
    while (NULL != (dptr = local_getline(fp))) {
        if (NULL == (value = local_stripper(dptr))) {
            /* cannot process */
            continue;
        }
        /* look for VmPeak */
        if (0 == strncmp(dptr, "VmPeak", strlen("VmPeak"))) {
            if (pst->pkvsize) {
                fval = convert_value(value);
                rc = PMIx_Info_list_add(cache, PMIX_PROC_PEAK_VSIZE, &fval, PMIX_FLOAT);
                if (PMIX_SUCCESS != rc) {
                    PMIx_Info_list_release(cache);
                    fclose(fp);
                    return rc;
                }
            }
        } else if (0 == strncmp(dptr, "VmSize", strlen("VmSize"))) {
            if (pst->vsize) {
                fval = convert_value(value);
                rc = PMIx_Info_list_add(cache, PMIX_PROC_VSIZE, &fval, PMIX_FLOAT);
                if (PMIX_SUCCESS != rc) {
                    PMIx_Info_list_release(cache);
                    fclose(fp);
                    return rc;
                }
            }
        } else if (0 == strncmp(dptr, "VmRSS", strlen("VmRSS"))) {
            if (pst->rss) {
                fval = convert_value(value);
                rc = PMIx_Info_list_add(cache, PMIX_PROC_RSS, &fval, PMIX_FLOAT);
                if (PMIX_SUCCESS != rc) {
                    PMIx_Info_list_release(cache);
                    fclose(fp);
                    return rc;
                }
            }
        }
    }
    fclose(fp);

    /* now create the smaps filename for this proc */
    memset(data, 0, sizeof(data));
    numchars = pmix_snprintf(data, sizeof(data), "/proc/%d/smaps", peer->info->pid);
    if (numchars >= sizeof(data)) {
        PMIx_Info_list_release(cache);
        return PMIX_ERROR;
    }

    if (NULL == (fp = fopen(data, "r"))) {
        /* ignore this */
        goto complete;
    }

    /* parse it to find lines that start with "Pss" */
    fval = 0.0;
    while (NULL != (dptr = local_getline(fp))) {
        if (NULL == (value = local_stripper(dptr))) {
            /* cannot process */
            continue;
        }
        /* look for Pss */
        if (0 == strncmp(dptr, "Pss", strlen("Pss"))) {
            fval += convert_value(value);
        }
    }
    if (0.0 < fval && pst->pss) {
        rc = PMIx_Info_list_add(cache, PMIX_PROC_PSS, &fval, PMIX_FLOAT);
        if (PMIX_SUCCESS != rc) {
            PMIx_Info_list_release(cache);
            fclose(fp);
            return rc;
        }
    }
    fclose(fp);

complete:
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
    FILE *fp;
    char *dptr;
    char **fields = NULL;
    void *cache;
    size_t n;
    bool takeit;
    uint64_t u64;
    pmix_data_array_t darray;
    time_t sample_time;
    pmix_status_t rc;

    /* look for the diskstats file */
    if (NULL == (fp = fopen("/proc/diskstats", "r"))) {
        /* not an error if we don't find this one as it
         * isn't critical
         */
        return PMIX_ERR_NOT_FOUND;
    }

    /* read the file one line at a time */
    while (NULL != (dptr = local_getline(fp))) {
        /* look for the local disks */
        if (NULL == strstr(dptr, "sd")) {
            continue;
        }
        /* parse to extract the fields */
        fields = NULL;
        local_getfields(dptr, &fields);
        if (NULL == fields) {
            continue;
        }
        if (14 < PMIx_Argv_count(fields)) {
            PMIx_Argv_free(fields);
            continue;
        }
        // see if this is a disk they want
        if (NULL == disks) {
            takeit = true;
        } else {
            takeit = false;
            for (n=0; NULL != disks[n]; n++) {
                if (0 == strcmp(fields[2], disks[n])) {
                    takeit = true;
                    break;
                }
            }
        }
        if (!takeit) {
            PMIx_Argv_free(fields);
            continue;
        }
        time(&sample_time);

        /* take the fields of interest */
        cache = PMIx_Info_list_start();
        // start with the diskID
        rc = PMIx_Info_list_add(cache, PMIX_DISK_ID, fields[2], PMIX_STRING);
        if (PMIX_SUCCESS != rc) {
            PMIx_Info_list_release(cache);
            PMIx_Argv_free(fields);
            fclose(fp);
            return rc;
        }
        if (dkst->rdcompleted) {
            u64 = strtoul(fields[3], NULL, 10);
            rc = PMIx_Info_list_add(cache, PMIX_DISK_READ_COMPLETED, &u64, PMIX_UINT64);
            if (PMIX_SUCCESS != rc) {
                PMIx_Info_list_release(cache);
                PMIx_Argv_free(fields);
                fclose(fp);
                return rc;
            }
        }
        if (dkst->rdmerged) {
            u64 = strtoul(fields[4], NULL, 10);
            rc = PMIx_Info_list_add(cache, PMIX_DISK_READ_MERGED, &u64, PMIX_UINT64);
            if (PMIX_SUCCESS != rc) {
                PMIx_Info_list_release(cache);
                PMIx_Argv_free(fields);
                fclose(fp);
                return rc;
            }
        }
        if (dkst->rdsectors) {
            u64 = strtoul(fields[5], NULL, 10);
            rc = PMIx_Info_list_add(cache, PMIX_DISK_READ_SECTORS, &u64, PMIX_UINT64);
            if (PMIX_SUCCESS != rc) {
                PMIx_Info_list_release(cache);
                PMIx_Argv_free(fields);
                fclose(fp);
                return rc;
            }
        }
        if (dkst->rdms) {
            u64 = strtoul(fields[6], NULL, 10);
            rc = PMIx_Info_list_add(cache, PMIX_DISK_READ_MILLISEC, &u64, PMIX_UINT64);
            if (PMIX_SUCCESS != rc) {
                PMIx_Info_list_release(cache);
                PMIx_Argv_free(fields);
                fclose(fp);
                return rc;
            }
        }

        if (dkst->wrtcompleted) {
            u64 = strtoul(fields[7], NULL, 10);
            rc = PMIx_Info_list_add(cache, PMIX_DISK_WRITE_COMPLETED, &u64, PMIX_UINT64);
            if (PMIX_SUCCESS != rc) {
                PMIx_Info_list_release(cache);
                PMIx_Argv_free(fields);
                fclose(fp);
                return rc;
            }
        }
        if (dkst->wrtmerged) {
            u64 = strtoul(fields[8], NULL, 10);
            rc = PMIx_Info_list_add(cache, PMIX_DISK_WRITE_MERGED, &u64, PMIX_UINT64);
            if (PMIX_SUCCESS != rc) {
                PMIx_Info_list_release(cache);
                PMIx_Argv_free(fields);
                fclose(fp);
                return rc;
            }
        }
        if (dkst->wrtsectors) {
            u64 = strtoul(fields[9], NULL, 10);
            rc = PMIx_Info_list_add(cache, PMIX_DISK_WRITE_SECTORS, &u64, PMIX_UINT64);
            if (PMIX_SUCCESS != rc) {
                PMIx_Info_list_release(cache);
                PMIx_Argv_free(fields);
                fclose(fp);
                return rc;
            }
        }
        if (dkst->wrtms) {
            u64 = strtoul(fields[10], NULL, 10);
            rc = PMIx_Info_list_add(cache, PMIX_DISK_WRITE_MILLISEC, &u64, PMIX_UINT64);
            if (PMIX_SUCCESS != rc) {
                PMIx_Info_list_release(cache);
                PMIx_Argv_free(fields);
                fclose(fp);
                return rc;
            }
        }

        if (dkst->ioprog) {
            u64 = strtoul(fields[10], NULL, 11);
            rc = PMIx_Info_list_add(cache, PMIX_DISK_IO_IN_PROGRESS, &u64, PMIX_UINT64);
            if (PMIX_SUCCESS != rc) {
                PMIx_Info_list_release(cache);
                PMIx_Argv_free(fields);
                fclose(fp);
                return rc;
            }
        }
        if (dkst->ioms) {
            u64 = strtoul(fields[10], NULL, 12);
            rc = PMIx_Info_list_add(cache, PMIX_DISK_IO_MILLISEC, &u64, PMIX_UINT64);
            if (PMIX_SUCCESS != rc) {
                PMIx_Info_list_release(cache);
                PMIx_Argv_free(fields);
                fclose(fp);
                return rc;
            }
        }
        if (dkst->ioweight) {
            u64 = strtoul(fields[10], NULL, 13);
            rc = PMIx_Info_list_add(cache, PMIX_DISK_IO_WEIGHTED, &u64, PMIX_UINT64);
            if (PMIX_SUCCESS != rc) {
                PMIx_Info_list_release(cache);
                PMIx_Argv_free(fields);
                fclose(fp);
                return rc;
            }
        }

        // record the sample time
        rc = PMIx_Info_list_add(cache, PMIX_DISK_SAMPLE_TIME, &sample_time, PMIX_TIME);
        if (PMIX_SUCCESS != rc) {
            PMIx_Info_list_release(cache);
            PMIx_Argv_free(fields);
            fclose(fp);
            return rc;
        }

        // add this to the final answer
        rc = PMIx_Info_list_convert(cache, &darray);
        PMIx_Info_list_release(cache);
        PMIx_Argv_free(fields);
        if (PMIX_SUCCESS != rc) {
            fclose(fp);
            return rc;
        }
        rc = PMIx_Info_list_add(answer, PMIX_DISK_RESOURCE_USAGE, &darray, PMIX_DATA_ARRAY);
        if (PMIX_SUCCESS != rc) {
            fclose(fp);
            return rc;
        }
    }
    fclose(fp);
    return PMIX_SUCCESS;
}

static pmix_status_t net_stat(void *answer, char**nets,
                              pmix_netstats_t *netst)
{
    FILE *fp;
    char *dptr, *ptr;
    char **fields;
    void *cache;
    size_t n;
    bool takeit;
    uint64_t u64;
    pmix_data_array_t darray;
    time_t sample_time;
    pmix_status_t rc;

    /* look for the netstats file */
    if (NULL == (fp = fopen("/proc/net/dev", "r"))) {
        /* not an error if we don't find this one as it
         * isn't critical
         */
        return PMIX_ERR_NOT_FOUND;
    }

    /* skip the first two lines as they are headers */
    local_getline(fp);
    local_getline(fp);

    /* read the file one line at a time */
    while (NULL != (dptr = local_getline(fp))) {
        /* the interface is at the start of the line */
        if (NULL == (ptr = strchr(dptr, ':'))) {
            continue;
        }
        *ptr = '\0';
        ptr++;
        /* parse to extract the fields */
        fields = NULL;
        local_getfields(ptr, &fields);
        if (NULL == fields) {
            continue;
        }
        if (11 < PMIx_Argv_count(fields)) {
            PMIx_Argv_free(fields);
            continue;
        }
        // see if this is a network they want
        if (NULL == nets) {
            takeit = true;
        } else {
            takeit = false;
            for (n=0; NULL != nets[n]; n++) {
                if (0 == strcmp(dptr, nets[n])) {
                    takeit = true;
                    break;
                }
            }
        }
        if (!takeit) {
            PMIx_Argv_free(fields);
            continue;
        }
        time(&sample_time);

        /* take the fields of interest */
        cache = PMIx_Info_list_start();
        // start with the network ID
        rc = PMIx_Info_list_add(cache, PMIX_NETWORK_ID, dptr, PMIX_STRING);
        if (PMIX_SUCCESS != rc) {
            PMIx_Info_list_release(cache);
            PMIx_Argv_free(fields);
            fclose(fp);
            return rc;
        }

        if (netst->rcvdb) {
            u64 = strtoul(fields[0], NULL, 10);
            rc = PMIx_Info_list_add(cache, PMIX_NET_RECVD_BYTES, &u64, PMIX_UINT64);
            if (PMIX_SUCCESS != rc) {
                PMIx_Info_list_release(cache);
                PMIx_Argv_free(fields);
                fclose(fp);
                return rc;
            }
        }
        if (netst->rcvdp) {
            u64 = strtoul(fields[1], NULL, 10);
            rc = PMIx_Info_list_add(cache, PMIX_NET_RECVD_PCKTS, &u64, PMIX_UINT64);
            if (PMIX_SUCCESS != rc) {
                PMIx_Info_list_release(cache);
                PMIx_Argv_free(fields);
                fclose(fp);
                return rc;
            }
        }
        if (netst->rcvde) {
            u64 = strtoul(fields[2], NULL, 10);
            rc = PMIx_Info_list_add(cache, PMIX_NET_RECVD_ERRS, &u64, PMIX_UINT64);
            if (PMIX_SUCCESS != rc) {
                PMIx_Info_list_release(cache);
                PMIx_Argv_free(fields);
                fclose(fp);
                return rc;
            }
        }

        if (netst->sntb) {
            u64 = strtoul(fields[8], NULL, 10);
            rc = PMIx_Info_list_add(cache, PMIX_NET_SENT_BYTES, &u64, PMIX_UINT64);
            if (PMIX_SUCCESS != rc) {
                PMIx_Info_list_release(cache);
                PMIx_Argv_free(fields);
                fclose(fp);
                return rc;
            }
        }
        if (netst->sntp) {
            u64 = strtoul(fields[9], NULL, 10);
            rc = PMIx_Info_list_add(cache, PMIX_NET_SENT_PCKTS, &u64, PMIX_UINT64);
            if (PMIX_SUCCESS != rc) {
                PMIx_Info_list_release(cache);
                PMIx_Argv_free(fields);
                fclose(fp);
                return rc;
            }
        }
        if (netst->snte) {
            u64 = strtoul(fields[10], NULL, 10);
            rc = PMIx_Info_list_add(cache, PMIX_NET_SENT_ERRS, &u64, PMIX_UINT64);
            if (PMIX_SUCCESS != rc) {
                PMIx_Info_list_release(cache);
                PMIx_Argv_free(fields);
                fclose(fp);
                return rc;
            }
        }

        // record the sample time
        rc = PMIx_Info_list_add(cache, PMIX_NET_SAMPLE_TIME, &sample_time, PMIX_TIME);
        if (PMIX_SUCCESS != rc) {
            PMIx_Info_list_release(cache);
            PMIx_Argv_free(fields);
            fclose(fp);
            return rc;
        }
        // add this to the final answer
        rc = PMIx_Info_list_convert(cache, &darray);
        PMIx_Info_list_release(cache);
        PMIx_Argv_free(fields);
        if (PMIX_SUCCESS != rc) {
            fclose(fp);
            return rc;
        }

        rc = PMIx_Info_list_add(answer, PMIX_NETWORK_RESOURCE_USAGE, &darray, PMIX_DATA_ARRAY);
        if (PMIX_SUCCESS != rc) {
            fclose(fp);
            return rc;
        }
    }
    fclose(fp);
    return PMIX_SUCCESS;
}

static pmix_status_t node_stat(void *ilist, pmix_ndstats_t *ndst)
{
    int fd;
    FILE *fp;
    char data[4096], *ptr, *value, *dptr, *eptr;
    int len;
    float fval;
    pmix_status_t rc;
    time_t sample_time;

    time(&sample_time);

    /* get the loadavg data */
    if (0 > (fd = open("/proc/loadavg", O_RDONLY))) {
        /* not an error if we don't find this one as it
         * isn't critical
         */
        return PMIX_ERR_NOT_FOUND;
    }

    /* absorb all of the file's contents in one gulp - we'll process
     * it once it is in memory for speed
     */
    memset(data, 0, sizeof(data));
    len = read(fd, data, sizeof(data) - 1);
    close(fd);
    if (len < 0) {
        return PMIX_ERR_FILE_READ_FAILURE;
    }

    /* remove newline at end */
    data[len] = '\0';

    /* we only care about the first three numbers */
    fval = strtof(data, &ptr);
    if (ndst->la) {
        rc = PMIx_Info_list_add(ilist, PMIX_NODE_LOAD_AVG, &fval, PMIX_FLOAT);
        if (PMIX_SUCCESS != rc) {
            return rc;
        }
    }
    fval = strtof(ptr, &eptr);
    if (ndst->la5) {
        rc = PMIx_Info_list_add(ilist, PMIX_NODE_LOAD_AVG5, &fval, PMIX_FLOAT);
        if (PMIX_SUCCESS != rc) {
            return rc;
        }
    }
    fval = strtof(eptr, NULL);
    if (ndst->la15) {
        rc = PMIx_Info_list_add(ilist, PMIX_NODE_LOAD_AVG15, &fval, PMIX_FLOAT);
        if (PMIX_SUCCESS != rc) {
            return rc;
        }
    }

    /* see if we can open the meminfo file */
    if (NULL == (fp = fopen("/proc/meminfo", "r"))) {
        /* ignore this */
        return PMIX_ERR_NOT_FOUND;
    }

    /* read the file one line at a time */
    while (NULL != (dptr = local_getline(fp))) {
        if (NULL == (value = local_stripper(dptr))) {
            /* cannot process */
            continue;
        }
        if (0 == strcmp(dptr, "MemTotal")) {
            if (ndst->mtot) {
                fval = convert_value(value);
                rc = PMIx_Info_list_add(ilist, PMIX_NODE_MEM_TOTAL, &fval, PMIX_FLOAT);
                if (PMIX_SUCCESS != rc) {
                    fclose(fp);
                    return rc;
                }
            }

        } else if (0 == strcmp(dptr, "MemFree")) {
            if (ndst->mfree) {
                fval = convert_value(value);
                rc = PMIx_Info_list_add(ilist, PMIX_NODE_MEM_FREE, &fval, PMIX_FLOAT);
                if (PMIX_SUCCESS != rc) {
                    fclose(fp);
                    return rc;
                }
            }

        } else if (0 == strcmp(dptr, "Buffers")) {
            if (ndst->mbuf) {
                fval = convert_value(value);
                rc = PMIx_Info_list_add(ilist, PMIX_NODE_MEM_BUFFERS, &fval, PMIX_FLOAT);
                if (PMIX_SUCCESS != rc) {
                    fclose(fp);
                    return rc;
                }
            }

        } else if (0 == strcmp(dptr, "Cached")) {
            if (ndst->mcached) {
                fval = convert_value(value);
                rc = PMIx_Info_list_add(ilist, PMIX_NODE_MEM_CACHED, &fval, PMIX_FLOAT);
                if (PMIX_SUCCESS != rc) {
                    fclose(fp);
                    return rc;
                }
            }

        } else if (0 == strcmp(dptr, "SwapCached")) {
            if (ndst->mswapcached) {
                fval = convert_value(value);
                rc = PMIx_Info_list_add(ilist, PMIX_NODE_MEM_SWAP_CACHED, &fval, PMIX_FLOAT);
                if (PMIX_SUCCESS != rc) {
                    fclose(fp);
                    return rc;
                }
            }

        } else if (0 == strcmp(dptr, "SwapTotal")) {
            if (ndst->mswaptot) {
                fval = convert_value(value);
                rc = PMIx_Info_list_add(ilist, PMIX_NODE_SWAP_TOTAL, &fval, PMIX_FLOAT);
                if (PMIX_SUCCESS != rc) {
                    fclose(fp);
                    return rc;
                }
            }

        } else if (0 == strcmp(dptr, "SwapFree")) {
            if (ndst->mswapfree) {
                fval = convert_value(value);
                rc = PMIx_Info_list_add(ilist, PMIX_NODE_MEM_SWAP_FREE, &fval, PMIX_FLOAT);
                if (PMIX_SUCCESS != rc) {
                    fclose(fp);
                    return rc;
                }
            }

        } else if (0 == strcmp(dptr, "Mapped")) {
            if (ndst->mmap) {
                fval = convert_value(value);
                rc = PMIx_Info_list_add(ilist, PMIX_NODE_MEM_MAPPED, &fval, PMIX_FLOAT);
                if (PMIX_SUCCESS != rc) {
                    fclose(fp);
                    return rc;
                }
            }
        }
    }
    fclose(fp);

    rc = PMIx_Info_list_add(ilist, PMIX_NODE_SAMPLE_TIME, &sample_time, PMIX_TIME);
    return rc;
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
        cb->info = (pmix_info_t*)darray.array;
        cb->ninfo = darray.size;
        cb->infocopy = true;
        rc = PMIx_Notify_event(op->eventcode, &pmix_globals.myid,
                               PMIX_RANGE_CUSTOM, cb->info, cb->ninfo,
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
            PMIX_DATA_ARRAY_DESTRUCT(&darray);
            PMIX_RELEASE(op);
            if (PMIX_ERR_EMPTY == rc) {
                // this is not an error
                rc = PMIX_SUCCESS;
            }
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
        // if the array has only two items in it, then those are the hostname
        // and nodeID we provided and not any data - this is equivalent to
        // "empty" as no data was found
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
            PMIX_DATA_ARRAY_DESTRUCT(&darray);
            PMIX_RELEASE(op);
            if (PMIX_ERR_EMPTY == rc) {
                // empty = nothing found, not an error
                rc = PMIX_SUCCESS;
            }
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
            if (PMIX_ERR_EMPTY == rc) {
                // empty = nothing found, not an error
                rc = PMIX_SUCCESS;
            }
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

static char *local_getline(FILE *fp)
{
    char *ret, *ptr;

    ret = fgets(input, PMIX_STAT_MAX_LENGTH, fp);
    if (NULL != ret) {
        input[strlen(input) - 1] = '\0'; /* remove newline */
        /* strip leading white space */
        ptr = input;
        while (!isalnum(*ptr)) {
            ptr++;
        }
        return ptr;
    }

    return NULL;
}

static char *local_stripper(char *data)
{
    char *ptr, *end, *enddata;
    int len = strlen(data);

    /* find the colon */
    if (NULL == (end = strchr(data, ':'))) {
        return NULL;
    }
    ptr = end;
    --end;
    /* working backwards, look for first non-whitespace */
    while (end != data && !isalnum(*end)) {
        --end;
    }
    ++end;
    *end = '\0';
    /* now look for value */
    ptr++;
    enddata = &(data[len - 1]);
    while (ptr != enddata && !isalnum(*ptr)) {
        ++ptr;
    }
    return ptr;
}

static void local_getfields(char *dptr, char ***fields)
{
    char *ptr, *end;

    /* set default */
    *fields = NULL;

    /* find the beginning */
    ptr = dptr;
    while ('\0' != *ptr && !isalnum(*ptr)) {
        ptr++;
    }
    if ('\0' == *ptr) {
        return;
    }

    /* working from this point, find the end of each
     * alphanumeric field and store it on the stack.
     * Then shift across the white space to the start
     * of the next one
     */
    end = ptr; /* ptr points to an alnum */
    end++;     /* look at next character */
    while ('\0' != *end) {
        /* find the end of this alpha string */
        while ('\0' != *end && isalnum(*end)) {
            end++;
        }
        /* terminate it */
        *end = '\0';
        /* store it on the stack */
        PMIx_Argv_append_nosize(fields, ptr);
        /* step across any white space */
        end++;
        while ('\0' != *end && !isalnum(*end)) {
            end++;
        }
        if ('\0' == *end) {
            ptr = NULL;
            break;
        }
        ptr = end;
        end++;
    }
    if (NULL != ptr) {
        /* have a hanging field */
        PMIx_Argv_append_nosize(fields, ptr);
    }
}
