#!/bin/bash

PMIX_BASE="<path_to_pmix>/install/"

INC="-I$PMIX_BASE/include/"
LIB="-L$PMIX_BASE/lib/ -lpmix"

gcc -o pmix_intra_perf -g -O0  pmi_intra_perf.c pmix.c $INC $LIB
