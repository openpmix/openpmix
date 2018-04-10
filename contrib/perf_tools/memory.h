/*
 * Copyright (c)  2018       The University of Tennessee and The University
 *                           of Tennessee Research Foundation.  All rights
 *                            reserved.
 *
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

int rank;

struct mem_info
{
    long VmSize;
    long VmRss;
};

struct mem_info meminfo;

/*  Get memory usage information from /proc/pid/status */
int get_memory_usage_kb(struct mem_info *meminfo);

/*  Get daemon memory usage information from /proc/pid/status */
void parent_info();

/* Get memory usage information from /proc/pid/smaps (both mpi process and daemon) */
void smap_pss_rss(int pid);

/* Get memory usage information from /proc/pid/maps (both mpi process and daemon) */
void maps_wr(int pid);

/* print result
 * Format: Rank,VmRSS(status),VmSize,Rss(smaps),Pss,Ppid,pVmRSS(status),pVmSize,pRss(smaps),pPss,mapwr,pmapwr
 */
void print_info();
