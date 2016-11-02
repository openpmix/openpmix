#!/bin/bash

OMPI_BASE=<path_to_ompi>/install/
PMIX_LIB=<path_to_pmix>/lib/
#LIBEVENT=<path_to_libevent>/lib/
#HWLOC=<path_to_hwloc>/lib/

export PATH="$OMPI_BASE/bin:$PATH"
export LD_LIBRARY_PATH="$OMPI_BASE/lib:$PMIX_LIB:$LD_LIBRARY_PATH"

mpirun -np 2 `pwd`/pmix_intra_perf