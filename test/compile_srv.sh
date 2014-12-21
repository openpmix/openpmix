#!/bin/bash


LIBEV_BASE=/home/artpol/WORK/Development/OpenMPI/ACTIVITY/TASKS/PMIx/libpmix/libevent_install/
LIBEV_INC=$LIBEV_BASE/include/
LIBEV_LIB=$LIBEV_BASE/lib/
PMIX_BASE=/home/artpol/WORK/Development/OpenMPI/ACTIVITY/TASKS/PMIx/libpmix/pmix
PMIX_API=$PMIX_BASE/src/
PMIX_INC=$PMIX_BASE/src/include
PMIX_LIB=$PMIX_BASE/.libs/


gcc -I$LIBEV_INC -I..  -g -o server server.c -L$LIBEV_LIB -levent -L$PMIX_LIB -lpmix