#!/bin/bash

LIBEV_BASE=/home/artpol/WORK/Development/OpenMPI/ACTIVITY/TASKS/PMIx/libpmix/libevent_install/
LIBEV_LIB=$LIBEV_BASE/lib/
LD_LIBRARY_PATH=$LIBEV_LIB:$LD_LIBRARY_PATH

PMIX_BASE=/home/artpol/WORK/Development/OpenMPI/ACTIVITY/TASKS/PMIx/libpmix/pmix
PMIX_API=$PMIX_BASE/src/
PMIX_INC=$PMIX_BASE/src/include
PMIX_LIB=$PMIX_BASE/.libs/

export LD_LIBRARY_PATH=$PMIX_LIB:$LD_LIBRARY_PATH

rm -f pmix
gdb ./server
