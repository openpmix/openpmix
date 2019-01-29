/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2016      Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2016      IBM Corporation.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
#include <src/include/pmix_config.h>

#include <pmix.h>
#include <pmix_common.h>
#include <pmix_server.h>
#include <pmix_rename.h>

#include "src/mca/bfrops/bfrops.h"
#include "src/mca/gds/base/base.h"
#include "src/include/pmix_globals.h"
#include "src/threads/threads.h"
#include "src/client/pmix_client_ops.h"

#include "src/common/pmix_attributes.h"

static bool initialized = false;
static pmix_list_t client_attrs;
static pmix_list_t server_attrs;
static pmix_list_t host_attrs;
static pmix_list_t tool_attrs;

typedef struct {
    pmix_list_item_t super;
    char *function;
    pmix_regattr_t *attrs;
    size_t nattrs;
} pmix_attribute_trk_t;

static void atrkcon(pmix_attribute_trk_t *p)
{
    p->function = NULL;
    p->attrs = NULL;
    p->nattrs = 0;
}
static void atrkdes(pmix_attribute_trk_t *p)
{
    if (NULL != p->function) {
        free(p->function);
    }
    if (NULL != p->attrs) {
        PMIX_REGATTR_FREE(p->attrs, p->nattrs);
    }
}
static PMIX_CLASS_INSTANCE(pmix_attribute_trk_t,
                           pmix_list_item_t,
                           atrkcon, atrkdes);

PMIX_EXPORT void pmix_init_registered_attrs(void)
{
    if (!initialized) {
        PMIX_CONSTRUCT(&client_attrs, pmix_list_t);
        PMIX_CONSTRUCT(&server_attrs, pmix_list_t);
        PMIX_CONSTRUCT(&host_attrs, pmix_list_t);
        PMIX_CONSTRUCT(&tool_attrs, pmix_list_t);
        initialized = true;
    }
}

static pmix_status_t process_reg(char *level, char *function,
                                 pmix_regattr_t attrs[], size_t nattrs)
{
    pmix_attribute_trk_t *fnptr;
    pmix_list_t *lst;
    size_t n;

    /* select the list this will appear on */
    if (0 == strcmp(level, PMIX_CLIENT_ATTRIBUTES)) {
        lst = &client_attrs;
    } else if (0 == strcmp(level, PMIX_SERVER_ATTRIBUTES)) {
        lst = &server_attrs;
    } else if (0 == strcmp(level, PMIX_HOST_ATTRIBUTES)) {
        lst = &host_attrs;
    } else {
        return PMIX_ERR_BAD_PARAM;
    }

    /* see if we already have this function */
    PMIX_LIST_FOREACH(fnptr, lst, pmix_attribute_trk_t) {
        if (0 == strcmp(function, fnptr->function)) {
            /* we already have this function at this level
             * so we must return an error */
            return PMIX_ERR_REPEAT_ATTR_REGISTRATION;
        }
    }

    pmix_attributes_print_attr(level, function, attrs, nattrs);
    fprintf(stderr, "\n\n");
    fnptr = PMIX_NEW(pmix_attribute_trk_t);
    fnptr->function = strdup(function);
    if (0 < nattrs) {
        fnptr->nattrs = nattrs;
        PMIX_REGATTR_CREATE(fnptr->attrs, fnptr->nattrs);
        for (n=0; n < nattrs; n++) {
            fnptr->attrs[n].key = strdup(attrs[n].key);
            fnptr->attrs[n].type = attrs[n].type;
            PMIX_ARGV_COPY(fnptr->attrs[n].valid_values, attrs[n].valid_values);
        }
    }
    return PMIX_SUCCESS;
}

PMIX_EXPORT pmix_status_t PMIx_Register_attributes(char *function,
                                                   pmix_regattr_t attrs[], size_t nattrs)
{
    pmix_status_t rc;

    PMIX_ACQUIRE_THREAD(&pmix_global_lock);
    if (pmix_globals.init_cntr <= 0) {
        PMIX_RELEASE_THREAD(&pmix_global_lock);
        return PMIX_ERR_INIT;
    }

    rc = process_reg(PMIX_HOST_ATTRIBUTES, function, attrs, nattrs);
    PMIX_RELEASE_THREAD(&pmix_global_lock);
    return rc;
}

PMIX_EXPORT void pmix_release_registered_attrs(void)
{
    if (initialized) {
        PMIX_LIST_DESTRUCT(&client_attrs);
        PMIX_LIST_DESTRUCT(&server_attrs);
        PMIX_LIST_DESTRUCT(&host_attrs);
        PMIX_LIST_DESTRUCT(&tool_attrs);
    }
}

/* sadly, we cannot dynamically register our supported attributes
 * as that would require the user to first call the function whose
 * attributes they want to know about - which somewhat defeats the
 * purpose. Until someone comes up with a better solution, we will
 * manually maintain the list */
static char *client_fns[] = {
    "PMIx_Init",
    "PMIx_Get",
    "PMIx_Get_nb",
    "PMIx_Group_construct",
    "PMIx_Group_construct_nb",
    "PMIx_Group_destruct",
    "PMIx_Group_destruct_nb",
    "PMIx_Group_invite",
    "PMIx_Group_invite_nb",
    "PMIx_Group_join",
    "PMIx_Group_join_nb",
    "PMIx_Spawn",
    "PMIx_Spawn_nb",
    "PMIx_Finalize",
    "PMIx_Log",
    "PMIx_Log_nb"
};

static pmix_regattr_t client_attributes[] = {
        {.key = "PMIX_GDS_MODULE", .type = PMIX_STRING, .valid_values = (char *[]){"UNRESTRICTED", NULL}},
        {.key = "PMIX_EVENT_BASE", .type = PMIX_POINTER, .valid_values = (char *[]){"VALID MEMORY REFERENCE", NULL}},
        {.key = "PMIX_HOSTNAME", .type = PMIX_STRING, .valid_values = (char *[]){"UNRESTRICTED", NULL}},
        {.key = "PMIX_NODEID", .type = PMIX_UINT32, .valid_values = (char *[]){"POSITIVE INTEGERS", NULL}},
        {.key = "PMIX_PROGRAMMING_MODEL", .type = PMIX_STRING, .valid_values = (char *[]){"UNRESTRICTED", NULL}},
        {.key = "PMIX_MODEL_LIBRARY_NAME", .type = PMIX_STRING, .valid_values = (char *[]){"UNRESTRICTED", NULL}},
        {.key = "PMIX_MODEL_LIBRARY_VERSION", .type = PMIX_STRING, .valid_values = (char *[]){"UNRESTRICTED", NULL}},
        {.key = "PMIX_THREADING_MODEL", .type = PMIX_STRING, .valid_values = (char *[]){"UNRESTRICTED", NULL}},
        {.key = ""},
        {.key = "PMIX_DATA_SCOPE", .type = PMIX_SCOPE, .valid_values = (char *[]){"PMIX_SCOPE_UNDEF,PMIX_LOCAL,", "PMIX_REMOTE,PMIX_GLOBAL,PMIX_INTERNAL", NULL}},
        {.key = "PMIX_OPTIONAL", .type = PMIX_BOOL, .valid_values = (char *[]){"True,False", NULL}},
        {.key = "PMIX_IMMEDIATE", .type = PMIX_BOOL, .valid_values = (char *[]){"True,False", NULL}},
        {.key = "PMIX_TIMEOUT", .type = PMIX_INT, .valid_values = (char *[]){"POSITIVE INTEGERS", NULL}},
        {.key = ""},
        {.key = "PMIX_DATA_SCOPE", .type = PMIX_SCOPE, .valid_values = (char *[]){"PMIX_SCOPE_UNDEF,PMIX_LOCAL,","PMIX_REMOTE,PMIX_GLOBAL,PMIX_INTERNAL", NULL}},
        {.key = "PMIX_OPTIONAL", .type = PMIX_BOOL, .valid_values = (char *[]){"True,False", NULL}},
        {.key = "PMIX_IMMEDIATE", .type = PMIX_BOOL, .valid_values = (char *[]){"True,False", NULL}},
        {.key = "PMIX_TIMEOUT", .type = PMIX_INT, .valid_values = (char *[]){"POSITIVE INTEGERS", NULL}},
        {.key = ""},
        {.key = "PMIX_EMBED_BARRIER", .type = PMIX_BOOL, .valid_values = (char *[]){"True,False", NULL}},
        {.key = ""},
        {.key = "PMIX_EMBED_BARRIER", .type = PMIX_BOOL, .valid_values = (char *[]){"True,False", NULL}},
        {.key = ""},
        {.key = "PMIX_EMBED_BARRIER", .type = PMIX_BOOL, .valid_values = (char *[]){"True,False", NULL}},
        {.key = ""},
        {.key = "PMIX_EMBED_BARRIER", .type = PMIX_BOOL, .valid_values = (char *[]){"True,False", NULL}},
        {.key = ""},
        {.key = "PMIX_TIMEOUT", .type = PMIX_INT, .valid_values = (char *[]){"POSITIVE INTEGERS", NULL}},
        {.key = ""},
        {.key = "PMIX_TIMEOUT", .type = PMIX_INT, .valid_values = (char *[]){"POSITIVE INTEGERS", NULL}},
        {.key = ""},
        {.key = "PMIX_TIMEOUT", .type = PMIX_INT, .valid_values = (char *[]){"POSITIVE INTEGERS", NULL}},
        {.key = ""},
        {.key = "PMIX_TIMEOUT", .type = PMIX_INT, .valid_values = (char *[]){"POSITIVE INTEGERS", NULL}},
        {.key = ""},
        {.key = "PMIX_SETUP_APP_ENVARS", .type = PMIX_STRING, .valid_values = (char *[]){"UNRESTRICTED", NULL}},
        {.key = ""},
        {.key = "PMIX_SETUP_APP_ENVARS", .type = PMIX_STRING, .valid_values = (char *[]){"UNRESTRICTED", NULL}},
        {.key = ""},
        {.key = "PMIX_EMBED_BARRIER", .type = PMIX_BOOL, .valid_values = (char *[]){"True,False", NULL}},
        {.key = ""},
        {.key = "PMIX_LOG_GENERATE_TIMESTAMP", .type = PMIX_BOOL, .valid_values = (char *[]){"True,False", NULL}},
        {.key = ""},
        {.key = "PMIX_LOG_GENERATE_TIMESTAMP", .type = PMIX_BOOL, .valid_values = (char *[]){"True,False", NULL}},
        {.key = ""},
};

/*****    REGISTER CLIENT ATTRS    *****/
PMIX_EXPORT pmix_status_t pmix_register_client_attrs(void)
{
    size_t nregs, nattrs, n;
    size_t cnt = 0;
    pmix_status_t rc = PMIX_SUCCESS;

    nregs = sizeof(client_fns) / sizeof(char*);

    /* we know we have to prep the GDS, PTL, BFROPS, and SEC
     * entries as these are dynamically defined */
    client_attributes[0].valid_values[0] = pmix_gds_base_get_available_modules();

    for (n=0; n < nregs; n++) {
        nattrs = 0;
        while (0 != strlen(client_attributes[cnt+nattrs].key)) {
            ++nattrs;
        }
        rc = process_reg(PMIX_CLIENT_ATTRIBUTES,
                         client_fns[n],
                         &client_attributes[cnt], nattrs);
        if (PMIX_SUCCESS != rc) {
            break;
        }
        cnt += nattrs + 1;
    }

    if (NULL != client_attributes[0].valid_values[0]) {
        free(client_attributes[0].valid_values[0]);
        client_attributes[0].valid_values[0] = NULL;
    }
    return PMIX_SUCCESS;
}

static char *server_fns[] = {
    "PMIx_server_init",
    "PMIx_server_finalize",
    "PMIx_generate_regex",
    "PMIx_generate_ppn",
    "PMIx_server_register_nspace",
    "PMIx_server_deregister_nspace",
    "PMIx_server_register_client",
    "PMIx_server_deregister_client",
    "PMIx_server_setup_fork",
    "PMIx_server_dmodex_request",
    "PMIx_server_setup_application",
    "PMIx_Register_attributes",
    "PMIx_server_setup_local_support",
    "PMIx_server_IOF_deliver",
    "PMIx_server_collect_inventory",
    "PMIx_server_deliver_inventory",
    "PMIx_Get",
    "PMIx_Get_nb",
    "PMIx_Fence",
    "PMIx_Fence_nb",
    "PMIx_Spawn",
    "PMIx_Spawn_nb",
    "PMIx_Connect",
    "PMIx_Connect_nb",
    "PMIx_Register_event_handler",
    "PMIx_Query_info_nb",
    "PMIx_Job_control",
    "PMIx_Job_control_nb"
};

static pmix_regattr_t server_attributes[] = {
    // init
        {.key = "PMIX_GDS_MODULE", .type = PMIX_STRING, .valid_values = (char *[]){"UNRESTRICTED", NULL}},
        {.key = "PMIX_EVENT_BASE", .type = PMIX_POINTER, .valid_values = (char *[]){"VALID MEMORY REFERENCE", NULL}},
        {.key = "PMIX_HOSTNAME", .type = PMIX_STRING, .valid_values = (char *[]){"UNRESTRICTED", NULL}},
        {.key = "PMIX_NODEID", .type = PMIX_UINT32, .valid_values = (char *[]){"POSITIVE INTEGERS", NULL}},
        {.key = "PMIX_PROGRAMMING_MODEL", .type = PMIX_STRING, .valid_values = (char *[]){"UNRESTRICTED", NULL}},
        {.key = "PMIX_MODEL_LIBRARY_NAME", .type = PMIX_STRING, .valid_values = (char *[]){"UNRESTRICTED", NULL}},
        {.key = "PMIX_MODEL_LIBRARY_VERSION", .type = PMIX_STRING, .valid_values = (char *[]){"UNRESTRICTED", NULL}},
        {.key = "PMIX_THREADING_MODEL", .type = PMIX_STRING, .valid_values = (char *[]){"UNRESTRICTED", NULL}},
        {.key = ""},
    // finalize
        {.key = "PMIX_DATA_SCOPE", .type = PMIX_SCOPE, .valid_values = (char *[]){"PMIX_SCOPE_UNDEF,PMIX_LOCAL,", "PMIX_REMOTE,PMIX_GLOBAL,PMIX_INTERNAL", NULL}},
        {.key = "PMIX_OPTIONAL", .type = PMIX_BOOL, .valid_values = (char *[]){"True,False", NULL}},
        {.key = "PMIX_IMMEDIATE", .type = PMIX_BOOL, .valid_values = (char *[]){"True,False", NULL}},
        {.key = "PMIX_TIMEOUT", .type = PMIX_INT, .valid_values = (char *[]){"POSITIVE INTEGERS", NULL}},
        {.key = ""},
    // regex
        {.key = "PMIX_DATA_SCOPE", .type = PMIX_SCOPE, .valid_values = (char *[]){"PMIX_SCOPE_UNDEF,PMIX_LOCAL,","PMIX_REMOTE,PMIX_GLOBAL,PMIX_INTERNAL", NULL}},
        {.key = "PMIX_OPTIONAL", .type = PMIX_BOOL, .valid_values = (char *[]){"True,False", NULL}},
        {.key = "PMIX_IMMEDIATE", .type = PMIX_BOOL, .valid_values = (char *[]){"True,False", NULL}},
        {.key = "PMIX_TIMEOUT", .type = PMIX_INT, .valid_values = (char *[]){"POSITIVE INTEGERS", NULL}},
        {.key = ""},
    // ppn
        {.key = "PMIX_EMBED_BARRIER", .type = PMIX_BOOL, .valid_values = (char *[]){"True,False", NULL}},
        {.key = ""},
    // register_nspace
        {.key = "PMIX_EMBED_BARRIER", .type = PMIX_BOOL, .valid_values = (char *[]){"True,False", NULL}},
        {.key = ""},
    // deregister_nspace
        {.key = "PMIX_EMBED_BARRIER", .type = PMIX_BOOL, .valid_values = (char *[]){"True,False", NULL}},
        {.key = ""},
    // register_client
        {.key = "PMIX_EMBED_BARRIER", .type = PMIX_BOOL, .valid_values = (char *[]){"True,False", NULL}},
        {.key = ""},
    // deregister_client
        {.key = "PMIX_TIMEOUT", .type = PMIX_INT, .valid_values = (char *[]){"POSITIVE INTEGERS", NULL}},
        {.key = ""},
    // setup_fork
        {.key = "PMIX_TIMEOUT", .type = PMIX_INT, .valid_values = (char *[]){"POSITIVE INTEGERS", NULL}},
        {.key = ""},
    // dmodex_request
        {.key = "PMIX_TIMEOUT", .type = PMIX_INT, .valid_values = (char *[]){"POSITIVE INTEGERS", NULL}},
        {.key = ""},
    // setup_application
        {.key = "PMIX_TIMEOUT", .type = PMIX_INT, .valid_values = (char *[]){"POSITIVE INTEGERS", NULL}},
        {.key = ""},
    // register_attributes
        {.key = "PMIX_SETUP_APP_ENVARS", .type = PMIX_STRING, .valid_values = (char *[]){"UNRESTRICTED", NULL}},
        {.key = ""},
    // setup_local_support
        {.key = "PMIX_SETUP_APP_ENVARS", .type = PMIX_STRING, .valid_values = (char *[]){"UNRESTRICTED", NULL}},
        {.key = ""},
    // IOF deliver
        {.key = "PMIX_EMBED_BARRIER", .type = PMIX_BOOL, .valid_values = (char *[]){"True,False", NULL}},
        {.key = ""},
    // collect_inventory
        {.key = "PMIX_LOG_GENERATE_TIMESTAMP", .type = PMIX_BOOL, .valid_values = (char *[]){"True,False", NULL}},
        {.key = ""},
    // deliver_inventory
        {.key = "PMIX_LOG_GENERATE_TIMESTAMP", .type = PMIX_BOOL, .valid_values = (char *[]){"True,False", NULL}},
        {.key = ""},
    // get
        {.key = "PMIX_IMMEDIATE", .type = PMIX_BOOL, .valid_values = (char *[]){"True,False", NULL}},
        {.key = "PMIX_TIMEOUT", .type = PMIX_INT, .valid_values = (char *[]){"POSITIVE INTEGERS", NULL}},
        {.key = ""},
    // get_nb
        {.key = "PMIX_IMMEDIATE", .type = PMIX_BOOL, .valid_values = (char *[]){"True,False", NULL}},
        {.key = "PMIX_TIMEOUT", .type = PMIX_INT, .valid_values = (char *[]){"POSITIVE INTEGERS", NULL}},
        {.key = ""},
    // fence
        {.key = "PMIX_COLLECT_DATA", .type = PMIX_BOOL, .valid_values = (char *[]){"True,False", NULL}},
        {.key = "PMIX_TIMEOUT", .type = PMIX_INT, .valid_values = (char *[]){"POSITIVE INTEGERS", NULL}},
        {.key = ""},
    // fence_nb
        {.key = "PMIX_COLLECT_DATA", .type = PMIX_BOOL, .valid_values = (char *[]){"True,False", NULL}},
        {.key = "PMIX_TIMEOUT", .type = PMIX_INT, .valid_values = (char *[]){"POSITIVE INTEGERS", NULL}},
        {.key = ""},
    // spawn
        {.key = "PMIX_FWD_STDIN", .type = PMIX_BOOL, .valid_values = (char *[]){"True,False", NULL}},
        {.key = "PMIX_FWD_STDOUT", .type = PMIX_BOOL, .valid_values = (char *[]){"True,False", NULL}},
        {.key = "PMIX_FWD_STDERR", .type = PMIX_BOOL, .valid_values = (char *[]){"True,False", NULL}},
        {.key = "PMIX_FWD_STDDIAG", .type = PMIX_BOOL, .valid_values = (char *[]){"True,False", NULL}},
        {.key = ""},
    // spawn_nb
        {.key = "PMIX_FWD_STDIN", .type = PMIX_BOOL, .valid_values = (char *[]){"True,False", NULL}},
        {.key = "PMIX_FWD_STDOUT", .type = PMIX_BOOL, .valid_values = (char *[]){"True,False", NULL}},
        {.key = "PMIX_FWD_STDERR", .type = PMIX_BOOL, .valid_values = (char *[]){"True,False", NULL}},
        {.key = "PMIX_FWD_STDDIAG", .type = PMIX_BOOL, .valid_values = (char *[]){"True,False", NULL}},
        {.key = ""},
    // connect
        {.key = "PMIX_TIMEOUT", .type = PMIX_INT, .valid_values = (char *[]){"POSITIVE INTEGERS", NULL}},
        {.key = ""},
    // connect_nb
        {.key = "PMIX_TIMEOUT", .type = PMIX_INT, .valid_values = (char *[]){"POSITIVE INTEGERS", NULL}},
        {.key = ""},
    // register_event
        {.key = "PMIX_EVENT_AFFECTED_PROC", .type = PMIX_PROC, .valid_values = (char *[]){"pmix_proc_t*", NULL}},
        {.key = "PMIX_EVENT_AFFECTED_PROCS", .type = PMIX_DATA_ARRAY, .valid_values = (char *[]){"Array of pmix_proc_t", NULL}},
        {.key = ""},
    // query
        {.key = "PMIX_QUERY_REFRESH_CACHE", .type = PMIX_BOOL, .valid_values = (char *[]){"True,False", NULL}},
        {.key = "PMIX_PROCID", .type = PMIX_PROC, .valid_values = (char *[]){"pmix_proc_t*", NULL}},
        {.key = "PMIX_NSPACE", .type = PMIX_STRING, .valid_values = (char *[]){"UNRESTRICTED", NULL}},
        {.key = "PMIX_RANK", .type = PMIX_PROC_RANK, .valid_values = (char *[]){"UNSIGNED INT32", NULL}},
        {.key = "PMIX_HOSTNAME", .type = PMIX_STRING, .valid_values = (char *[]){"UNRESTRICTED", NULL}},
        {.key = ""},
    // job_ctrl
        {.key = "PMIX_REGISTER_CLEANUP", .type = PMIX_STRING, .valid_values = (char *[]){"UNRESTRICTED", NULL}},
        {.key = "PMIX_REGISTER_CLEANUP_DIR", .type = PMIX_STRING, .valid_values = (char *[]){"UNRESTRICTED", NULL}},
        {.key = "PMIX_CLEANUP_RECURSIVE", .type = PMIX_BOOL, .valid_values = (char *[]){"True,False", NULL}},
        {.key = "PMIX_CLEANUP_IGNORE", .type = PMIX_STRING, .valid_values = (char *[]){"UNRESTRICTED", NULL}},
        {.key = "PMIX_CLEANUP_LEAVE_TOPDIR", .type = PMIX_BOOL, .valid_values = (char *[]){"True,False", NULL}},
        {.key = ""},
    // job_ctrl_nb
        {.key = "PMIX_REGISTER_CLEANUP", .type = PMIX_STRING, .valid_values = (char *[]){"UNRESTRICTED", NULL}},
        {.key = "PMIX_REGISTER_CLEANUP_DIR", .type = PMIX_STRING, .valid_values = (char *[]){"UNRESTRICTED", NULL}},
        {.key = "PMIX_CLEANUP_RECURSIVE", .type = PMIX_BOOL, .valid_values = (char *[]){"True,False", NULL}},
        {.key = "PMIX_CLEANUP_IGNORE", .type = PMIX_STRING, .valid_values = (char *[]){"UNRESTRICTED", NULL}},
        {.key = "PMIX_CLEANUP_LEAVE_TOPDIR", .type = PMIX_BOOL, .valid_values = (char *[]){"True,False", NULL}},
        {.key = ""},
};

/*****    REGISTER SERVER ATTRS    *****/
PMIX_EXPORT pmix_status_t pmix_register_server_attrs(void)
{
    size_t nregs, nattrs, n;
    size_t cnt = 0;
    pmix_status_t rc = PMIX_SUCCESS;

    nregs = sizeof(server_fns) / sizeof(char*);

    for (n=0; n < nregs; n++) {
        nattrs = 0;
        while (0 != strlen(server_attributes[cnt+nattrs].key)) {
            ++nattrs;
        }
        rc = process_reg(PMIX_SERVER_ATTRIBUTES,
                         server_fns[n],
                         &server_attributes[cnt], nattrs);
        if (PMIX_SUCCESS != rc) {
            break;
        }
        cnt += nattrs + 1;
    }

    return PMIX_SUCCESS;
}

static char *tool_fns[] = {
    "PMIx_server_init",
    "PMIx_server_finalize",
};

static pmix_regattr_t tool_attributes[] = {
    // init
        {.key = "PMIX_GDS_MODULE", .type = PMIX_STRING, .valid_values = (char *[]){"UNRESTRICTED", NULL}},
        {.key = "PMIX_EVENT_BASE", .type = PMIX_POINTER, .valid_values = (char *[]){"VALID MEMORY REFERENCE", NULL}},
        {.key = "PMIX_HOSTNAME", .type = PMIX_STRING, .valid_values = (char *[]){"UNRESTRICTED", NULL}},
        {.key = "PMIX_NODEID", .type = PMIX_UINT32, .valid_values = (char *[]){"POSITIVE INTEGERS", NULL}},
        {.key = "PMIX_PROGRAMMING_MODEL", .type = PMIX_STRING, .valid_values = (char *[]){"UNRESTRICTED", NULL}},
        {.key = "PMIX_MODEL_LIBRARY_NAME", .type = PMIX_STRING, .valid_values = (char *[]){"UNRESTRICTED", NULL}},
        {.key = "PMIX_MODEL_LIBRARY_VERSION", .type = PMIX_STRING, .valid_values = (char *[]){"UNRESTRICTED", NULL}},
        {.key = "PMIX_THREADING_MODEL", .type = PMIX_STRING, .valid_values = (char *[]){"UNRESTRICTED", NULL}},
        {.key = ""},
    // finalize
        {.key = "PMIX_DATA_SCOPE", .type = PMIX_SCOPE, .valid_values = (char *[]){"PMIX_SCOPE_UNDEF,PMIX_LOCAL,", "PMIX_REMOTE,PMIX_GLOBAL,PMIX_INTERNAL", NULL}},
        {.key = "PMIX_OPTIONAL", .type = PMIX_BOOL, .valid_values = (char *[]){"True,False", NULL}},
        {.key = "PMIX_IMMEDIATE", .type = PMIX_BOOL, .valid_values = (char *[]){"True,False", NULL}},
        {.key = "PMIX_TIMEOUT", .type = PMIX_INT, .valid_values = (char *[]){"POSITIVE INTEGERS", NULL}},
        {.key = ""},
};

/*****    REGISTER TOOL ATTRS    *****/
PMIX_EXPORT pmix_status_t pmix_register_tool_attrs(void)
{
    size_t nregs, nattrs, n;
    size_t cnt = 0;
    pmix_status_t rc = PMIX_SUCCESS;

    nregs = sizeof(tool_fns) / sizeof(char*);

    for (n=0; n < nregs; n++) {
        nattrs = 0;
        while (0 != strlen(tool_attributes[cnt+nattrs].key)) {
            ++nattrs;
        }
        rc = process_reg(PMIX_TOOL_ATTRIBUTES,
                         tool_fns[n],
                         &tool_attributes[cnt], nattrs);
        if (PMIX_SUCCESS != rc) {
            break;
        }
        cnt += nattrs + 1;
    }

    return PMIX_SUCCESS;
}

/*****   PROCESS QUERY ATTRS    *****/
static pmix_infolist_t* _get_attrs(char *function,
                                   char *level,
                                   pmix_list_t *attrs)
{
    pmix_attribute_trk_t *trk, *tptr;
    pmix_infolist_t *ip;
    pmix_data_array_t *darray;
    pmix_regattr_t *regarray;
    size_t m;

    /* search the list for this function */
    PMIX_LIST_FOREACH(tptr, attrs, pmix_attribute_trk_t) {
        trk = NULL;
        if (0 == strcmp(function, tptr->function)) {
            trk = tptr;
            break;
        }
        if (NULL == trk || NULL == trk->attrs) {
            /* function wasn't found - no attrs
             * registered for it */
            return NULL;
        }
        /* add the found attrs to the results */
        ip = PMIX_NEW(pmix_infolist_t);
        /* create the data array to hold the results */
        PMIX_DATA_ARRAY_CREATE(darray, trk->nattrs, PMIX_REGATTR);
        PMIX_INFO_LOAD(&ip->info, level, darray, PMIX_DATA_ARRAY);
        regarray = (pmix_regattr_t*)darray->array;
        for (m=0; m < trk->nattrs; m++) {
            PMIX_REGATTR_XFER(&regarray[m], &trk->attrs[m]);
        }
        return ip;
    }

    return NULL;
}

static void _local_relcb(void *cbdata)
{
    pmix_query_caddy_t *cd = (pmix_query_caddy_t*)cbdata;
    PMIX_RELEASE(cd);
}

PMIX_EXPORT void pmix_attrs_query_support(int sd, short args, void *cbdata)
{
    pmix_query_caddy_t *cd = (pmix_query_caddy_t*)cbdata;
    pmix_infolist_t *info, *head;
    pmix_list_t kyresults;
    size_t n, m, p;
    char **keys = NULL;
    pmix_info_t *iptr;
    pmix_data_array_t *darray;

    PMIX_ACQUIRE_THREAD(&pmix_global_lock);

    for (n=0; n < cd->nqueries; n++) {
        if (0 != strcmp(cd->queries[n].keys[0], PMIX_QUERY_ATTRIBUTE_SUPPORT)) {
            /* skip this one */
            continue;
        }
        head = NULL;
        for (m=0; NULL != cd->queries[n].keys[m]; m++) {
            PMIX_CONSTRUCT(&kyresults, pmix_list_t);
            if (NULL == cd->queries[n].qualifiers ||
                PMIX_CHECK_KEY(&cd->queries[n].qualifiers[m], PMIX_CLIENT_ATTRIBUTES)) {
                /* everyone has access to the client attrs */
                info = _get_attrs(cd->queries[n].keys[m], PMIX_CLIENT_ATTRIBUTES, &client_attrs);
                if (NULL != info) {
                    pmix_list_append(&kyresults, &info->super);
                }
            }
            if (NULL == cd->queries[n].qualifiers ||
                PMIX_CHECK_KEY(&cd->queries[n].qualifiers[m], PMIX_SERVER_ATTRIBUTES)) {
                /* if I am a server, add in my attrs */
                if (PMIX_PROC_IS_SERVER(pmix_globals.mypeer)) {
                    info = _get_attrs(cd->queries[n].keys[m], PMIX_SERVER_ATTRIBUTES, &server_attrs);
                    if (NULL != info) {
                        pmix_list_append(&kyresults, &info->super);
                    }
                } else {
                    /* we need to ask our server for them */
                    pmix_argv_append_unique_nosize(&keys, cd->queries[n].keys[m], false);
                }
            }
            if (NULL == cd->queries[n].qualifiers ||
                PMIX_CHECK_KEY(&cd->queries[n].qualifiers[m], PMIX_TOOL_ATTRIBUTES)) {
                info = _get_attrs(cd->queries[n].keys[m], PMIX_TOOL_ATTRIBUTES, &tool_attrs);
                if (NULL != info) {
                    pmix_list_append(&kyresults, &info->super);
                }
            }
            if (NULL == cd->queries[n].qualifiers ||
                PMIX_CHECK_KEY(&cd->queries[n].qualifiers[m], PMIX_HOST_ATTRIBUTES)) {
                /* if I am a server, add in the host's */
                if (PMIX_PROC_IS_SERVER(pmix_globals.mypeer)) {
                    info = _get_attrs(cd->queries[n].keys[m], PMIX_HOST_ATTRIBUTES, &host_attrs);
                    if (NULL != info) {
                        pmix_list_append(&kyresults, &info->super);
                    }
                } else {
                    /* we need to ask our server for them, if we didn't
                     * already add it to the list */
                    pmix_argv_append_unique_nosize(&keys, cd->queries[n].keys[m], false);
                }
            }
            if (0 < (p = pmix_list_get_size(&kyresults))) {
                head = PMIX_NEW(pmix_infolist_t);
                /* create the data array to hold the results */
                PMIX_DATA_ARRAY_CREATE(darray, p, PMIX_INFO);
                PMIX_INFO_LOAD(&head->info, cd->queries[n].keys[m], darray, PMIX_DATA_ARRAY);
                iptr = (pmix_info_t*)darray->array;
                p = 0;
                PMIX_LIST_FOREACH(info, &kyresults, pmix_infolist_t) {
                    PMIX_INFO_XFER(&iptr[p], &info->info);
                    ++p;
                }
                pmix_list_append(&cd->results, &head->super);
            }
            PMIX_LIST_DESTRUCT(&kyresults);
        }
    }
    PMIX_RELEASE_THREAD(&pmix_global_lock);

    /* if there is anything to request, do so now */
    if (0 < pmix_argv_count(keys)) {
        /* construct a message making the request */
        return;
    }

    /* otherwise, prep the response by converting the list
     * of results into an array */
    if (0 < (cd->ninfo = pmix_list_get_size(&cd->results))) {
        PMIX_INFO_CREATE(cd->info, cd->ninfo);
        n = 0;
        PMIX_LIST_FOREACH(info, &cd->results, pmix_infolist_t) {
            PMIX_INFO_XFER(&cd->info[n], &info->info);
            ++n;
        }
    }

    if (NULL != cd->cbfunc) {
        cd->status = PMIX_SUCCESS;
        cd->cbfunc(cd->status, cd->info, cd->ninfo, cd->cbdata, _local_relcb, cd);
        return;
    }

    PMIX_RELEASE(cd);
}


/*****   PRINT QUERY ATTRS RESULTS   *****/

#define PMIX_PRINT_KEY_COLUMN_WIDTH      30
#define PMIX_PRINT_TYPE_COLUMN_WIDTH     20
#define PMIX_PRINT_ATTR_COLUMN_WIDTH     100

PMIX_EXPORT void pmix_attributes_print_attr(char *level, char *function,
                                            pmix_regattr_t *src, size_t nattrs)
{
    size_t left, n, m, len;
    char *title1 = "CLIENT SUPPORTED ATTRIBUTES: ";
    char *title2 = "SERVER SUPPORTED ATTRIBUTES: ";
    char *title3 = "HOST SUPPORTED ATTRIBUTES: ";
    char line[PMIX_PRINT_ATTR_COLUMN_WIDTH], *title;
    char *tmp;

    /* select title */
    if (0 == strcmp(level, PMIX_CLIENT_ATTRIBUTES)) {
        title = title1;
    } else if (0 == strcmp(level, PMIX_SERVER_ATTRIBUTES)) {
        title = title2;
    } else if (0 == strcmp(level, PMIX_HOST_ATTRIBUTES)) {
        title = title3;
    } else {
        title = level;
    }

    /* print the headers */
    memset(line, ' ', PMIX_PRINT_ATTR_COLUMN_WIDTH);
    line[PMIX_PRINT_ATTR_COLUMN_WIDTH-1] = '\0';
    left = (PMIX_PRINT_ATTR_COLUMN_WIDTH / 2) - ((strlen(title) + strlen(function) + 6) / 2);
    m=0;
    for (n=0; n < left-3; n++) {
        line[n] = '*';
        ++m;
    }
    m = left; // leave a gap
    for (n=0; n < strlen(title); n++) {
        line[m] = title[n];
        ++m;
    }
    for (n=0; n < strlen(function); n++) {
        line[m] = function[n];
        ++m;
    }
    m += 3;
    while (m < PMIX_PRINT_ATTR_COLUMN_WIDTH-1) {
        line[m] = '*';
        ++m;
    }
    fprintf(stderr, "%s\n", line);
    /* print the column headers */
    memset(line, ' ', PMIX_PRINT_ATTR_COLUMN_WIDTH);
    line[PMIX_PRINT_ATTR_COLUMN_WIDTH-1] = '\0';
    left = PMIX_PRINT_KEY_COLUMN_WIDTH/2 - 1;
    memcpy(&line[left], "KEY", 3);
    left = 3 + PMIX_PRINT_KEY_COLUMN_WIDTH + (PMIX_PRINT_TYPE_COLUMN_WIDTH/2) - 2;
    memcpy(&line[left], "TYPE", 4);
    left = PMIX_PRINT_KEY_COLUMN_WIDTH + PMIX_PRINT_KEY_COLUMN_WIDTH +
           ((PMIX_PRINT_ATTR_COLUMN_WIDTH-PMIX_PRINT_KEY_COLUMN_WIDTH-PMIX_PRINT_KEY_COLUMN_WIDTH)/2) - 3 - strlen("VALID VALUES")/2;
    memcpy(&line[left], "VALID VALUES", strlen("VALID VALUES"));
    fprintf(stderr, "%s\n", line);

    /* print the dashes under the column headers */
    memset(line, ' ', PMIX_PRINT_ATTR_COLUMN_WIDTH);
    line[PMIX_PRINT_ATTR_COLUMN_WIDTH-1] = '\0';
    m=0;
    for (n=0; n < PMIX_PRINT_KEY_COLUMN_WIDTH; n++) {
        line[m] = '-';
        ++m;
    }
    m += 2; // leave gap
    for (n=0; n < PMIX_PRINT_TYPE_COLUMN_WIDTH; n++) {
        line[m] = '-';
        ++m;
    }
    m += 2; // leave gap
    while (m < PMIX_PRINT_ATTR_COLUMN_WIDTH-1) {
        line[m] = '-';
        ++m;
    }
    fprintf(stderr, "%s\n", line);

    for (n=0; n < nattrs; n++) {
        memset(line, ' ', PMIX_PRINT_ATTR_COLUMN_WIDTH);
        line[PMIX_PRINT_ATTR_COLUMN_WIDTH-1] = '\0';
        len = strlen(src[n].key);
        if (PMIX_PRINT_KEY_COLUMN_WIDTH < len) {
            len = PMIX_PRINT_KEY_COLUMN_WIDTH;
        }
        memcpy(line, src[n].key, len);

        tmp = (char*)PMIx_Data_type_string(src[n].type);
        len = strlen(tmp);
        if (PMIX_PRINT_TYPE_COLUMN_WIDTH < len) {
            len = PMIX_PRINT_TYPE_COLUMN_WIDTH;
        }
        memcpy(&line[PMIX_PRINT_KEY_COLUMN_WIDTH+2], tmp, len);

        len = strlen(src[n].valid_values[0]);
        if ((PMIX_PRINT_ATTR_COLUMN_WIDTH-PMIX_PRINT_KEY_COLUMN_WIDTH-PMIX_PRINT_TYPE_COLUMN_WIDTH-2) < len) {
            len = PMIX_PRINT_TYPE_COLUMN_WIDTH;
        }
        memcpy(&line[PMIX_PRINT_KEY_COLUMN_WIDTH+PMIX_PRINT_TYPE_COLUMN_WIDTH+4], src[n].valid_values[0], len);

        fprintf(stderr, "%s\n", line);

        for (m=1; NULL != src[n].valid_values[m]; m++) {
            memset(line, ' ', PMIX_PRINT_ATTR_COLUMN_WIDTH);
            line[PMIX_PRINT_ATTR_COLUMN_WIDTH-1] = '\0';
            len = strlen(src[n].valid_values[m]);
            if ((PMIX_PRINT_ATTR_COLUMN_WIDTH-PMIX_PRINT_KEY_COLUMN_WIDTH-PMIX_PRINT_TYPE_COLUMN_WIDTH-2) < len) {
                len = PMIX_PRINT_TYPE_COLUMN_WIDTH;
            }
            memcpy(&line[PMIX_PRINT_KEY_COLUMN_WIDTH+PMIX_PRINT_TYPE_COLUMN_WIDTH+4], src[n].valid_values[m], len);
            fprintf(stderr, "%s\n", line);
        }
    }
}
