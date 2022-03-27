/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2007 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2012-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2016-2017 Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2017      IBM Corporation. All rights reserved.
 * Copyright (c) 2021-2022 Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PMIX_CMD_LINE_H
#define PMIX_CMD_LINE_H

#include "src/include/pmix_config.h"

#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <getopt.h>

#include "src/class/pmix_list.h"
#include "src/class/pmix_object.h"
#include "src/util/pmix_argv.h"

BEGIN_C_DECLS

typedef struct {
    pmix_list_item_t super;
    char *key;
    char **values;
} pmix_cli_item_t;
PMIX_CLASS_DECLARATION(pmix_cli_item_t);

typedef struct {
    pmix_object_t super;
    pmix_list_t instances;  // comprised of pmix_cli_item_t's
    char **tail;  // remainder of argv
} pmix_cli_result_t;
PMIX_CLASS_DECLARATION(pmix_cli_result_t);

#define PMIX_CLI_RESULT_STATIC_INIT                 \
{                                                   \
    .super = PMIX_OBJ_STATIC_INIT(pmix_object_t),   \
    .instances = PMIX_LIST_STATIC_INIT,             \
    .tail = NULL                                    \
}

/* define PMIX-named flags for argument required */
#define PMIX_ARG_REQD       required_argument
#define PMIX_ARG_NONE       no_argument
#define PMIX_ARG_OPTIONAL   optional_argument

/* define PMIX-named flags for whether parsing
 * CLI shall include deprecation warnings */
#define PMIX_CLI_SILENT     true
#define PMIX_CLI_WARN       false

/* define a long option that has no short option equivalent
 *
 * n = name of the option (see below for definitions)
 * a = whether or not it requires an argument
 */
#define PMIX_OPTION_DEFINE(n, a)    \
{                                   \
    .name = (n),                    \
    .has_arg = (a),                 \
    .flag = NULL,                   \
    .val = 0                        \
}
/* define a long option that has a short option equivalent
 *
 * n = name of the option (see below for definitions)
 * a = whether or not it requires an argument
 * c = single character equivalent option
 */
#define PMIX_OPTION_SHORT_DEFINE(n, a, c)   \
{                                           \
    .name = (n),                            \
    .has_arg = (a),                         \
    .flag = NULL,                           \
    .val = (c)                              \
}

#define PMIX_OPTION_END  {0, 0, 0, 0}

//      NAME                            STRING                      ARGUMENT

// Basic options
#define PMIX_CLI_HELP                   "help"                      // optional
#define PMIX_CLI_VERSION                "version"                   // none
#define PMIX_CLI_VERBOSE                "verbose"                   // number of instances => verbosity level
#define PMIX_CLI_PMIXMCA                "pmixmca"                   // requires TWO

// Tool connection options
#define PMIX_CLI_SYS_SERVER_FIRST       "system-server-first"       // none
#define PMIX_CLI_SYSTEM_SERVER          "system-server"             // none
#define PMIX_CLI_SYS_SERVER_ONLY        "system-server-only"        // none
#define PMIX_CLI_DO_NOT_CONNECT         "do-not-connect"            // none
#define PMIX_CLI_WAIT_TO_CONNECT        "wait-to-connect"           // required
#define PMIX_CLI_NUM_CONNECT_RETRIES    "num-connect-retries"       // required
#define PMIX_CLI_PID                    "pid"                       // required
#define PMIX_CLI_NAMESPACE              "namespace"                 // required
#define PMIX_CLI_URI                    "uri"                       // required
#define PMIX_CLI_TIMEOUT                "timeout"                   // required
#define PMIX_CLI_TMPDIR                 "tmpdir"                    // required


typedef void (*pmix_cmd_line_store_fn_t)(const char *name, const char *option,
                                         pmix_cli_result_t *results);

PMIX_EXPORT int pmix_cmd_line_parse(char **argv, char *shorts,
                                    struct option myoptions[],
                                    pmix_cmd_line_store_fn_t storefn,
                                    pmix_cli_result_t *results,
                                    char *helpfile);

static inline pmix_cli_item_t* pmix_cmd_line_get_param(pmix_cli_result_t *results,
                                                       const char *key)
{
    pmix_cli_item_t *opt;

    PMIX_LIST_FOREACH(opt, &results->instances, pmix_cli_item_t) {
        if (0 == strcmp(opt->key, key)) {
            return opt;
        }
    }
    return NULL;
}

static inline bool pmix_cmd_line_is_taken(pmix_cli_result_t *results,
                                          const char *key)
{
    if (NULL == pmix_cmd_line_get_param(results, key)) {
        return false;
    }
    return true;
}

static inline int pmix_cmd_line_get_ninsts(pmix_cli_result_t *results,
                                           const char *key)
{
    pmix_cli_item_t *opt;

    opt = pmix_cmd_line_get_param(results, key);
    if (NULL == opt) {
        return 0;
    }
    return pmix_argv_count(opt->values);
}

static inline char* pmix_cmd_line_get_nth_instance(pmix_cli_result_t *results,
                                                   const char *key, int idx)
{
    pmix_cli_item_t *opt;
    int ninst;

    opt = pmix_cmd_line_get_param(results, key);
    if (NULL == opt) {
        return NULL;
    }
    ninst = pmix_argv_count(opt->values);
    if (ninst < idx) {
        return NULL;
    }
    return opt->values[idx];
}

#define PMIX_CLI_DEBUG_LIST(r)  \
do {                                                                    \
    pmix_cli_item_t *_c;                                                \
    char *_tail;                                                        \
    pmix_output(0, "\n[%s:%s:%d]", __FILE__, __func__, __LINE__);       \
    PMIX_LIST_FOREACH(_c, &(r)->instances, pmix_cli_item_t) {           \
        pmix_output(0, "KEY: %s", _c->key);                             \
        if (NULL != _c->values) {                                       \
            for (int _n=0; NULL != _c->values[_n]; _n++) {              \
                pmix_output(0, "    VAL[%d]: %s", _n, _c->values[_n]);  \
            }                                                           \
        }                                                               \
    }                                                                   \
    _tail = pmix_argv_join((r)->tail, ' ');                             \
    pmix_output(0, "TAIL: %s", _tail);                                  \
    free(_tail);                                                        \
    pmix_output(0, "\n");                                               \
} while(0)

#define PMIX_CLI_REMOVE_DEPRECATED(r, o)    \
do {                                                        \
    pmix_list_remove_item(&(r)->instances, &(o)->super);    \
    PMIX_RELEASE(o);                                        \
} while(0)
END_C_DECLS

#endif /* PMIX_CMD_LINE_H */
