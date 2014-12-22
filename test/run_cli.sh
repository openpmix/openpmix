#!/bin/bash


PMIX_BASE=/home/artpol/WORK/Development/OpenMPI/ACTIVITY/TASKS/PMIx/libpmix/pmix
PMIX_API=$PMIX_BASE/src/
PMIX_INC=$PMIX_BASE/src/include
PMIX_LIB=$PMIX_BASE/.libs/

export LD_LIBRARY_PATH=$PMIX_LIB:$LD_LIBRARY_PATH

export PMIX_SERVER_URI="0:pmix"
export PMIX_ID=1

gdb ./client
#./client