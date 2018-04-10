/*
 * Copyright (c)  2018       The University of Tennessee and The University
 *                           of Tennessee Research Foundation.  All rights
 *                           reserved.
 *
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "memory.h"

/* parent pid */
int ppid, Ppid;

/* sum of RSS/PSS from /proc/pid/smaps */
int RssSum, PssSum, pRssSum, pPssSum;
int mapwr, pmapwr;
/* parent Vmrss/vmsize */
long pVmRSS, pVmSize;

int get_memory_usage_kb(struct mem_info *meminfo)
{
    /* Get the the current process' status file from the proc filesystem */
    FILE* procfile = fopen("/proc/self/status", "r");

    long to_read = 8192;
    char buffer[to_read];
    if( !fread(buffer, sizeof(char), to_read, procfile) )
    {
       printf("Read proc/self/status error");
       return -1;
    }
    fclose(procfile);

    char* search_result;
    /* Look through proc status contents line by line */
    char delims[] = "\n";
    char* line = strtok(buffer, delims);
    while (line != NULL )
    {
        search_result = strstr(line, "VmRSS:");
        if (search_result != NULL)
        {
            sscanf(line, "%*s %ld", &meminfo->VmRss);
        }
        search_result = strstr(line, "VmSize:");
        if (search_result != NULL)
        {
            sscanf(line, "%*s %ld", &meminfo->VmSize);
        }
        search_result = strstr(line, "PPid:");
        if (search_result != NULL)
        {
            sscanf(line, "%*s %d", &ppid);
        }
        line = strtok(NULL, delims);
    }
    smap_pss_rss(0);
    smap_pss_rss(ppid);
    maps_wr(0);
    maps_wr(ppid);
    parent_info();
    return 0;
}

void parent_info()
{
    char* search_result;
    char pfilename[50];
    long prt_to_read = 8192;
    char prt_buffer[prt_to_read];
    Ppid=ppid;
    /* Look through proc status contents line by line */
    char delims[] = "\n";

    snprintf(pfilename, sizeof pfilename, "/proc/%d/status", ppid);
    FILE* pfile = fopen(pfilename, "r");
    if( !fread(prt_buffer, sizeof(char), prt_to_read, pfile))
    {
        printf("Read /proc/parent/status/ error");
    }
    fclose(pfile);

    char* pline = strtok(prt_buffer, delims);
    while (pline != NULL )
    {
        search_result = strstr(pline, "VmRSS:");
        if (search_result != NULL)
        {
            sscanf(pline, "%*s %ld", &pVmRSS);
        }
        search_result = strstr(pline, "VmSize:");
        if (search_result != NULL)
        {
            sscanf(pline, "%*s %ld", &pVmSize);
        }
        pline = strtok(NULL, delims);
    }
}

void smap_pss_rss(int pid)
{
    char pfilename[50];
    FILE *procfile;

    if( pid==0 )
    {
        procfile = fopen("/proc/self/smaps", "r");
    }
    else
    {
        snprintf(pfilename, sizeof pfilename, "/proc/%d/smaps", pid);
        procfile = fopen(pfilename, "r");
    }
    long to_read = 500000;
    char buffer[to_read];
    if( !fread(buffer, sizeof(char), to_read, procfile) )
    {
        printf("Read /proc/pid/smaps error");
    }
    fclose(procfile);
    int Rss, Pss;
    char* search_result;
    /* Look through proc status contents line by line */
    char delims[] = "\n";
    char* line = strtok(buffer, delims);
    while (line != NULL )
    {
        search_result = strstr(line, "Rss:");
        if (search_result != NULL)
        {
            sscanf(line, "%*s %d", &Rss);
            if( pid==0 )
            {
                RssSum = RssSum + Rss;
            }
            else
            {
                pRssSum = pRssSum + Rss;
            }
        }
        search_result = strstr(line, "Pss:");
        if (search_result != NULL)
        {
            sscanf(line, "%*s %d", &Pss);
            if( pid==0 )
            {
                PssSum = PssSum + Pss;
            }
            else
            {
                pPssSum = pPssSum + Pss;
            }
        }
        line = strtok(NULL, delims);
    }
}

void maps_wr(int pid)
{
    char pfilename[50];
    FILE *procfile;

    /* if pid is 0 we want to get info of ourself, otherwise pPid for parent*/
    if( pid==0 )
    {
        procfile = fopen("/proc/self/maps", "r");
    }
    else
    {
        snprintf(pfilename, sizeof pfilename, "/proc/%d/maps", pid);
        procfile = fopen(pfilename, "r");
    }
    char line[256];

    char* search_result;
    char* token;

    char *start, *end;
    long sum=0, temp=0;
    /* get info line by line, only care line with rw-p, sub start addr - end addr */
    while (fgets(line, sizeof(line), procfile))
    {
        search_result = strstr(line, "rw-p");
        if (search_result != NULL)
        {
            token = strtok(line, " ");
            start = strtok(token, "-");
            token = strtok(NULL, "-");
            temp = (strtol(token, &end,16) - strtol(start, *end,16))/1024;
            sum = sum +temp;
        }
    }
    if(pid==0)
    {
        mapwr = sum;
    }
    else
    {
        pmapwr =sum;
    }
    fclose(procfile);
}

void print_info()
{
    printf("%6d,  %6ld, %6ld, %6d, %6d, %6d, %6ld, %6ld, %6d, %6d, %6d, %6d, MEM_info\n", rank, meminfo.VmRss, meminfo.VmSize, RssSum, PssSum, Ppid, pVmRSS, pVmSize, pRssSum, pPssSum, mapwr, pmapwr);
}
