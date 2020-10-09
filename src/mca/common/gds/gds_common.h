/*
 * This file is derived from the gds package - per the BSD license, the following
 * info is copied here:
 ***********************
 * This file is part of the gds package, copyright (c) 2011, 2012, @radiospiel.
 * It is copyrighted under the terms of the modified BSD license, see LICENSE.BSD.
 *
 * For more information see https://https://github.com/radiospiel/gds.
 ***********************
 * Copyright (c) 2020      Intel, Inc.  All rights reserved.
 *
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PMIX_GDS_COMMON_H
#define PMIX_GDS_COMMON_H

#include "src/include/pmix_config.h"

#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdio.h>

#include "src/class/pmix_object.h"

/* Define a bitmask to track what information may not have
 * been provided but is computable from other info */
#define PMIX_HASH_PROC_DATA     0x00000001
#define PMIX_HASH_JOB_SIZE      0x00000002
#define PMIX_HASH_MAX_PROCS     0x00000004
#define PMIX_HASH_NUM_NODES     0x00000008
#define PMIX_HASH_PROC_MAP      0x00000010
#define PMIX_HASH_NODE_MAP      0x00000020

/**********************************************/
/* struct definitions */
typedef struct {
    pmix_list_item_t super;
    uint32_t session;
    pmix_list_t sessioninfo;
    pmix_list_t nodeinfo;
} pmix_session_t;
PMIX_CLASS_DECLARATION(pmix_session_t);

typedef struct {
    pmix_list_item_t super;
    char *ns;
    pmix_namespace_t *nptr;
    pmix_hash_table_t internal;
    pmix_hash_table_t remote;
    pmix_hash_table_t local;
    bool gdata_added;
    pmix_list_t jobinfo;
    pmix_list_t apps;
    pmix_list_t nodeinfo;
    pmix_session_t *session;
} pmix_job_t;
PMIX_CLASS_DECLARATION(pmix_job_t);

typedef struct {
    pmix_list_item_t super;
    uint32_t appnum;
    pmix_list_t appinfo;
    pmix_list_t nodeinfo;
    pmix_job_t *job;
} pmix_apptrkr_t;
PMIX_CLASS_DECLARATION(pmix_apptrkr_t);

typedef struct {
    pmix_list_item_t super;
    uint32_t nodeid;
    char *hostname;
    char **aliases;
    pmix_list_t info;
} pmix_nodeinfo_t;
PMIX_CLASS_DECLARATION(pmix_nodeinfo_t);


#endif /* PMIX_GDS_COMMON_H */
