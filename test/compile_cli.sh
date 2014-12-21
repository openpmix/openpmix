#!/bin/bash


PMIX_BASE=/home/artpol/WORK/Development/OpenMPI/ACTIVITY/TASKS/PMIx/libpmix/pmix
PMIX_API=$PMIX_BASE/src/
PMIX_INC=$PMIX_BASE/src/include
PMIX_LIB=$PMIX_BASE/.libs/

gcc -I$PMIX_BASE -I$PMIX_API -I$PMIX_INC -g -o client client.c -L$PMIX_LIB -lpmix