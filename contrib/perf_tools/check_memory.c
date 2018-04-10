/*
 * Copyright (c)  2018       The University of Tennessee and The University
 *                           of Tennessee Research Foundation.  All rights
 *                           reserved.
 *
 *  $COPYRIGHT$
 *
 *  Additional copyrights may follow
 *
 */

#include <mpi.h>
#include <stdio.h>
#include <unistd.h>
#include "memory.h"

int main (int argc, char *argv[])
{
    int np;
    char processor_name[MPI_MAX_PROCESSOR_NAME];
    int processor_name_len;

    MPI_Init(&argc, &argv);
    MPI_Barrier(MPI_COMM_WORLD);

    get_memory_usage_kb(&meminfo);

    MPI_Comm_size(MPI_COMM_WORLD, &np);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Get_processor_name(processor_name, &processor_name_len);
    print_info();
    sleep(3);

    MPI_Finalize();

    return 0;
}
