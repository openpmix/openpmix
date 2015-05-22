/*
 * Copyright (c) 2015      Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#include <time.h>
#include "test_common.h"
#include "src/api/pmix_common.h"
#include "src/api/pmix.h"

int test_fence(test_params params, char *my_nspace, int my_rank);
int test_job_fence(test_params params, char *my_nspace, int my_rank);
