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
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
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

/* Every occurrence of one option, plus where each of them fell on the
 * command line.
 *
 * The values array groups the occurrences by option, which is what nearly
 * every consumer wants and is all this used to record. It cannot, however,
 * express the interleaving of two repeated options: a second "--set-env"
 * is filed behind the first, so an intervening "--prepend-env" appears to
 * have come after both. That matters to a consumer whose directives edit
 * each other - "--set-env FOO=1 --prepend-env FOO[:] x --set-env FOO=2"
 * and "--set-env FOO=2 --prepend-env FOO[:] x" are different requests and
 * the grouped view renders them identically.
 *
 * So each stored value also carries the position at which it was given:
 * seq[n] is the occurrence index of values[n], counting every stored
 * occurrence of every option from zero. Comparing two of these answers
 * "which of these did the user type first?" - see
 * pmix_cmd_line_get_nth_seq() for the narrow question and
 * pmix_cmd_line_get_ordered() for the whole command line in order.
 *
 * A value carries -1 if its position is not known - i.e. it was added to
 * the results by the caller after the parse rather than by the parser.
 * firstseq is the position of the option's first appearance, and is the
 * only position recorded for an option given without a value (a boolean
 * flag stores nothing, so repeats of it are indistinguishable anyway).
 */
typedef struct {
    pmix_list_item_t super;
    char *key;
    char **values;
    int *seq;       // command-line position of each entry in values
    int nseq;       // number of entries in seq
    int firstseq;   // position at which this option first appeared
} pmix_cli_item_t;
PMIX_CLASS_DECLARATION(pmix_cli_item_t);

typedef struct {
    pmix_object_t super;
    pmix_list_t instances;  // comprised of pmix_cli_item_t's
    char **tail;  // remainder of argv
    int nseq;  // number of occurrences recorded so far
} pmix_cli_result_t;
PMIX_CLASS_DECLARATION(pmix_cli_result_t);

#define PMIX_CLI_RESULT_STATIC_INIT                 \
{                                                   \
    .super = PMIX_OBJ_STATIC_INIT(pmix_object_t),   \
    .instances = PMIX_LIST_STATIC_INIT,             \
    .tail = NULL,                                   \
    .nseq = 0                                       \
}

/* One occurrence of one option, as reported by
 * pmix_cmd_line_get_ordered() */
typedef struct {
    const char *key;    // option name - points into the results, not a copy
    const char *value;  // value given, or NULL if the option took none
    int seq;            // position on the command line, -1 if not known
} pmix_cli_occurrence_t;

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

/* A note on the options marked "requires TWO" below.
 *
 * getopt is told those options take ONE argument, and the parser then takes
 * a second token of its own accord from argv[optind]. Deciding whether that
 * token is the value or the next option is therefore the parser's problem,
 * not getopt's, and it is answered by asking this table whether the token
 * names a registered option (see is_option_token() in pmix_cmd_line.c).
 *
 * It must not be answered by looking for a dash. A value may legitimately
 * contain one ("0-1") or begin with one ("-1" - negative MCA values are
 * real), and a short option is a single dash, so neither argv[n][0] nor
 * argv[n][1] separates the two cases. Both spellings have been tried and
 * both broke a working command line.
 */

//      NAME                            STRING                      ARGUMENT

// Basic options
#define PMIX_CLI_HELP                   "help"                      // optional
#define PMIX_CLI_VERSION                "version"                   // none
#define PMIX_CLI_VERBOSE                "verbose"                   // number of instances => verbosity level
#define PMIX_CLI_PMIXMCA                "pmixmca"                   // requires TWO
#define PMIX_CLI_PRETTY_PRINT           "pretty-print"              // none
#define PMIX_CLI_PARSABLE               "parsable"                  // none
#define PMIX_CLI_PARSEABLE              "parseable"                 // none

// Tool connection options
#define PMIX_CLI_SYS_SERVER_FIRST       "system-server-first"       // none
#define PMIX_CLI_SYSTEM_SERVER          "system-server"             // none
#define PMIX_CLI_SYS_SERVER_ONLY        "system-server-only"        // none
#define PMIX_CLI_DO_NOT_CONNECT         "do-not-connect"            // none
#define PMIX_CLI_WAIT_TO_CONNECT        "wait-to-connect"           // required
#define PMIX_CLI_NUM_CONNECT_RETRIES    "num-connect-retries"       // required
#define PMIX_CLI_PID                    "pid"                       // required
#define PMIX_CLI_NAMESPACE              "namespace"                 // required
#define PMIX_CLI_NSPACE                 "nspace"                    // required
#define PMIX_CLI_URI                    "uri"                       // required
#define PMIX_CLI_TIMEOUT                "timeout"                   // required
#define PMIX_CLI_TMPDIR                 "tmpdir"                    // required
#define PMIX_CLI_CONNECTION_ORDER       "connect-order"             // required
#define PMIX_CLI_SYS_CONTROLLER         "system-controller"         // none
#define PMIX_CLI_SCHEDULER              "scheduler"                 // none

// Allocation request options
#define PMIX_CLI_REQ_ID                 "request-id"                // required
#define PMIX_CLI_QUEUE                  "queue"                     // required, short is 'q'
#define PMIX_CLI_RESOURCES              "resources"                 // required
#define PMIX_CLI_NODES                  "nodes"                     // required, short is 'N'
#define PMIX_CLI_IMAGE                  "image"                     // required, short is 'i'
#define PMIX_CLI_EXCLUDE                "exclude"                   // required, short is 'x'
#define PMIX_CLI_WAIT_ALL_NODES         "wait-all-nodes"            // none
#define PMIX_CLI_NODELIST               "nodelist"                  // required, short is 'w'
#define PMIX_CLI_UID                    "uid"                       // required
#define PMIX_CLI_GID                    "gid"                       // required
#define PMIX_CLI_TIME                   "time"                      // required, short is 't'
#define PMIX_CLI_SIGNAL                 "signal"                    // required
#define PMIX_CLI_SHARE                  "share"                     // none
#define PMIX_CLI_EXTEND                 "extend"                    // none
#define PMIX_CLI_SHRINK                 "shrink"                    // none
#define PMIX_CLI_NO_SHELL               "no-shell"                  // none
#define PMIX_CLI_BEGIN                  "begin"                     // required
#define PMIX_CLI_IMMEDIATE              "immediate"                 // optional, short is 'I'
#define PMIX_CLI_DEPENDENCY             "dependency"                // required, short is 'd'
#define PMIX_CLI_DO_NOT_WAIT            "do-not-wait"               // none

// Job control options
#define PMIX_CLI_PAUSE                  "pause"                     // none
#define PMIX_CLI_RESUME                 "resume"                    // none
#define PMIX_CLI_CANCEL                 "cancel"                    // required
#define PMIX_CLI_KILL                   "kill"                      // none
#define PMIX_CLI_RESTART                "restart"                   // required
#define PMIX_CLI_CHKPT                  "checkpoint"                // required
#define PMIX_CLI_TARGETS                "targets"                   // required
#define PMIX_CLI_TERMINATE              "terminate"                 // none
#define PMIX_CLI_PSET_NAME              "pset"                      // required
#define PMIX_CLI_FWD_ENVAR              "x"                         // required
#define PMIX_CLI_SET_ENVAR              "set-env"                   // required
#define PMIX_CLI_PREPEND_ENVAR          "prepend-env"               // required
#define PMIX_CLI_APPEND_ENVAR           "append-env"                // required
#define PMIX_CLI_UNSET_ENVAR            "unset-env"                 // required

// Info options
#define PMIX_CLI_INFO_ALL               "all"                       // none
#define PMIX_CLI_INFO_ARCH              "arch"                      // none
#define PMIX_CLI_INFO_CONFIG            "config"                    // none
#define PMIX_CLI_INFO_HOSTNAME          "hostname"                  // none
#define PMIX_CLI_INFO_INTERNAL          "internal"                  // none
#define PMIX_CLI_INFO_PARAM             "param"                     // required
#define PMIX_CLI_INFO_PARAMS            "params"                    // required
#define PMIX_CLI_INFO_PATH              "path"                      // required
#define PMIX_CLI_INFO_VERSION           "show-version"              // required
#define PMIX_CLI_INFO_TYPES             "type"                      // required
#define PMIX_CLI_INFO_COLOR             "color"                     // required
#define PMIX_CLI_INFO_SHOW_FAILED       "show-failed"               //none
#define PMIX_CLI_INFO_SELECTED_ONLY     "selected-only"             // none


typedef void (*pmix_cmd_line_store_fn_t)(const char *name, const char *option,
                                         pmix_cli_result_t *results);

PMIX_EXPORT int pmix_cmd_line_parse(char **argv, char *shorts,
                                    struct option myoptions[],
                                    pmix_cmd_line_store_fn_t storefn,
                                    pmix_cli_result_t *results,
                                    char *helpfile);

/* Report every recorded occurrence of every option, in the order the
 * options were given.
 *
 * This is the flat view of a parse result: one entry per stored value
 * (plus one entry, with a NULL value, for an option given without one),
 * sorted by command-line position. It is the same information the
 * instances list holds - the entries point into that list's strings
 * rather than copying them - just ordered by when rather than grouped by
 * what, so a consumer whose directives are order-sensitive can walk it
 * instead of reconstructing the order itself.
 *
 * The array is valid until "results" is modified or destructed, and is
 * released with a plain free(). Values whose position was not recorded
 * (see pmix_cli_item_t) are reported last, in instances order.
 */
PMIX_EXPORT int pmix_cmd_line_get_ordered(pmix_cli_result_t *results,
                                          pmix_cli_occurrence_t **occurrences,
                                          size_t *nocc);

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
    return PMIx_Argv_count(opt->values);
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
    ninst = PMIx_Argv_count(opt->values);
    if (ninst < idx) {
        return NULL;
    }
    return opt->values[idx];
}

/* Where on the command line the idx'th instance of this option was given.
 *
 * Returns the occurrence index - a number that is only meaningful when
 * compared against another one from the same parse - or -1 if the option
 * was not given, the instance does not exist, or its position was not
 * recorded. Answers questions of the form "was the --set-env I am looking
 * at given before or after that --prepend-env?" without building the full
 * ordered view. */
static inline int pmix_cmd_line_get_nth_seq(pmix_cli_result_t *results,
                                            const char *key, int idx)
{
    pmix_cli_item_t *opt;

    opt = pmix_cmd_line_get_param(results, key);
    if (NULL == opt) {
        return -1;
    }
    if (0 > idx || idx >= opt->nseq) {
        return -1;
    }
    return opt->seq[idx];
}

/* Where on the command line this option FIRST appeared - the only
 * position recorded for an option that takes no value. Returns -1 if the
 * option was not given or its position was not recorded. */
static inline int pmix_cmd_line_get_first_seq(pmix_cli_result_t *results,
                                              const char *key)
{
    pmix_cli_item_t *opt;

    opt = pmix_cmd_line_get_param(results, key);
    if (NULL == opt) {
        return -1;
    }
    return opt->firstseq;
}

/* USAGE:
 *  param "a" is the input command line string
 *  param "b" is the defined CLI option
 */
static inline bool pmix_check_cli_option(char *ain, char *bin)
{
    size_t len1, len2, len, n;
    char *a, *b, *p;
    char **asplit, **bsplit;
    int match, acnt, bcnt;

    // protect the input
    a = strdup(ain);
    b = strdup(bin);

    /* if there is an '=' in the option, then we only
     * check up to that position in the option as
     * everything past it is the value the option
     * is being assigned */
    p = strchr(a, '=');
    if (NULL != p) {
        *p = '\0';
    }
    p = strchr(b, '=');
    if (NULL != p) {
        *p = '\0';
    }

    /* if there exists a '-' in either argument,
     * then we are dealing with a multi-word
     * option. Parse those by checking each
     * word segment individually for a match
     * so the user doesn't have to spell it all
     * out unless necessary. We consider it a
     * valid match if all provided segments match
     * that of the target option */
    if (NULL != strchr(b, '-') ||
        NULL != strchr(a, '-')) {
        asplit = PMIx_Argv_split(a, '-');
        bsplit = PMIx_Argv_split(b, '-');
        acnt = PMIx_Argv_count(asplit);
        bcnt = PMIx_Argv_count(bsplit);
        if (acnt > bcnt) {
            PMIx_Argv_free(asplit);
            PMIx_Argv_free(bsplit);
            free(a);
            free(b);
            return false;
        }
        match = 0;
        for (n=0; NULL != asplit[n] && NULL != bsplit[n]; n++) {
            len1 = strlen(asplit[n]);
            len2 = strlen(bsplit[n]);
            len = len1 < len2 ? len1 : len2;
            if (0 == strncasecmp(asplit[n], bsplit[n], len)) {
                ++match;
            } else {
                PMIx_Argv_free(asplit);
                PMIx_Argv_free(bsplit);
                free(a);
                free(b);
                return false;
            }
        }
        PMIx_Argv_free(asplit);
        PMIx_Argv_free(bsplit);
        if (match == acnt) {
            /* all provided segments match */
            free(a);
            free(b);
            return true;
        }
        free(a);
        free(b);
        return false;
    }

    /* if this is not a multi-word option, we just
     * check the strings */
    len1 = strlen(a);
    len2 = strlen(b);
    len = (len1 < len2) ? len1 : len2;
    if (0 == strncasecmp(a, b, len)) {
        free(a);
        free(b);
        return true;
    }

    free(a);
    free(b);
    return false;
}

#define PMIX_CHECK_CLI_OPTION(a, b) \
    pmix_check_cli_option(a, b)

/* USAGE:
 *  param "qual" is the input command line qualifier, e.g. "PE=2"
 *
 * Returns the value assigned to a qualifier - the text following its '=' -
 * or NULL if it carries none.
 *
 * This is the companion to pmix_check_cli_option above, and exists because
 * that function stops comparing at the '=' and then accepts any unambiguous
 * prefix of the option's name. A caller therefore cannot know how long the
 * name it just matched actually is: the user may have written "P=2" for
 * "PE=2", or "F=path" for "FILE=path". Reading the value at an offset fixed
 * to the option's full spelling reads the wrong bytes - or, for a qualifier
 * written with no value at all, reads past the end of the string entirely.
 */
static inline char *pmix_cli_qualifier_value(char *qual)
{
    char *ptr;

    if (NULL == qual) {
        return NULL;
    }
    ptr = strchr(qual, '=');
    if (NULL == ptr || '\0' == *(ptr + 1)) {
        return NULL;
    }
    return ptr + 1;
}

#define PMIX_CLI_QUALIFIER_VALUE(q) \
    pmix_cli_qualifier_value(q)

static inline unsigned int pmix_convert_string_to_time(const char *t)
{
    char **tmp = PMIx_Argv_split(t, ':');
    int sz = PMIx_Argv_count(tmp);
    unsigned int tm;

    /* work upwards from the bottom, where the
     * bottom represents seconds, then minutes,
     * then hours, and then days */
    tm = strtoul(tmp[sz-1], NULL, 10);
    if (0 <= (sz-2) && NULL != tmp[sz-2]) {
        tm += 60 * strtoul(tmp[sz-2], NULL, 10);
    }
    if (0 <= (sz-3) && NULL != tmp[sz-3]) {
        tm += 60 * 60 * strtoul(tmp[sz-3], NULL, 10);
    }
    if (0 <= (sz-4) && NULL != tmp[sz-4]) {
        tm += 24 * 60 * 60 * strtoul(tmp[sz-4], NULL, 10);
    }
    PMIx_Argv_free(tmp);
    return tm;
}

#define PMIX_CONVERT_TIME(s)    \
    pmix_convert_string_to_time(s)


#define PMIX_CLI_DEBUG_LIST(r)  \
do {                                                                    \
    pmix_cli_item_t *_c;                                                \
    char *_tail;                                                        \
    pmix_output(0, "\n[%s:%s:%d]", __FILE__, __func__, __LINE__);       \
    PMIX_LIST_FOREACH(_c, &(r)->instances, pmix_cli_item_t) {           \
        pmix_output(0, "KEY: %s (first seen at %d)",                    \
                    _c->key, _c->firstseq);                             \
        if (NULL != _c->values) {                                       \
            for (int _n=0; NULL != _c->values[_n]; _n++) {              \
                pmix_output(0, "    VAL[%d]: %s (given at %d)", _n,     \
                            _c->values[_n],                             \
                            (_n < _c->nseq) ? _c->seq[_n] : -1);        \
            }                                                           \
        }                                                               \
    }                                                                   \
    _tail = PMIx_Argv_join((r)->tail, ' ');                             \
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
