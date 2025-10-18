/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2006 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2008      Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2015      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2016-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "src/include/pmix_config.h"

#include <errno.h>
#include <locale.h>
#include <stdio.h>
#include <string.h>

#include "pmix.h"
#include "pmix_common.h"
#include "src/include/pmix_globals.h"
#include "src/mca/pinstalldirs/pinstalldirs.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_os_path.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_printf.h"
#include "src/util/pmix_show_help.h"

int pmix_show_help_enabled = 0;
static time_t show_help_time_last_displayed = 0;
static bool show_help_timer_set = false;
static pmix_event_t show_help_timer_event;
static int output_stream = -1;
static bool pmix_show_help_initialized = false;

/* How long to wait between displaying duplicate show_help notices */
static struct timeval show_help_interval = {5, 0};

static void show_help_cbfunc(pmix_status_t status, void *cbdata)
{
    pmix_shift_caddy_t *cd = (pmix_shift_caddy_t *) cbdata;
    PMIX_HIDE_UNUSED_PARAMS(status);

    if (NULL != cd->directives) {
        PMIX_INFO_FREE(cd->directives, cd->ndirs);
    }
    if (NULL != cd->info) {
        PMIX_INFO_FREE(cd->info, cd->ninfo);
    }
    PMIX_RELEASE(cd);
}

static void local_delivery(const char *file,
                           const char *topic,
                           const char *msg)
{
    pmix_shift_caddy_t *cd;

    if (!pmix_show_help_initialized ||
        0 == pmix_atomic_load_int(&pmix_show_help_enabled) ||
        NULL == pmix_globals.evbase) {
        /* the show help subsystem has not yet been enabled,
         * likely because we haven't gotten far enough thru
         * client/server/tool "init". In this case, we can
         * only output the help locally as we don't have
         * access to anything else */
        fprintf(stderr, "%s", msg);
        return;
    }

    cd = PMIX_NEW(pmix_shift_caddy_t);
    cd->ninfo = 1;
    PMIX_INFO_CREATE(cd->info, cd->ninfo);
    PMIX_INFO_LOAD(&cd->info[0], PMIX_LOG_STDERR, msg, PMIX_STRING);
    cd->ndirs = 2;
    PMIX_INFO_CREATE(cd->directives, cd->ndirs);
    PMIX_INFO_LOAD(&cd->directives[0], PMIX_LOG_KEY, file, PMIX_STRING);
    PMIX_INFO_LOAD(&cd->directives[1], PMIX_LOG_VAL, topic, PMIX_STRING);
    cd->cbfunc.opcbfn = show_help_cbfunc;
    cd->cbdata = cd;
    cd->proc = NULL;
    PMIX_THREADSHIFT(cd, pmix_log_local_op);
}

/* List items for arrays to search */
typedef struct {
    pmix_list_item_t super;
    char *project;
    pmix_show_help_file_t *array;
} tuple_array_item_t;
static void tacon(tuple_array_item_t *p)
{
    p->project = NULL;
}
static void tades(tuple_array_item_t *p)
{
    if (NULL != p->project) {
        free(p->project);
    }
}
static PMIX_CLASS_INSTANCE(tuple_array_item_t,
                           pmix_list_item_t,
                           tacon, tades);

// list of arrays to search
static pmix_list_t data_arrays;

/* List items for holding (filename, topic) tuples */
typedef struct {
    pmix_list_item_t super;
    /* The filename */
    char *tli_filename;
    /* The topic */
    char *tli_topic;
    /* List of process names that have displayed this (filename, topic) */
    pmix_list_t tli_processes;
    /* Time this message was displayed */
    time_t tli_time_displayed;
    /* Count of processes since last display (i.e., "new" processes
       that have showed this message that have not yet been output) */
    int tli_count_since_last_display;
    /* Do we want to display these? */
    bool tli_display;
} tuple_list_item_t;

static void tuple_list_item_constructor(tuple_list_item_t *obj)
{
    obj->tli_filename = NULL;
    obj->tli_topic = NULL;
    PMIX_CONSTRUCT(&(obj->tli_processes), pmix_list_t);
    obj->tli_time_displayed = time(NULL);
    obj->tli_count_since_last_display = 0;
    obj->tli_display = true;
}

static void tuple_list_item_destructor(tuple_list_item_t *obj)
{
    if (NULL != obj->tli_filename) {
        free(obj->tli_filename);
    }
    if (NULL != obj->tli_topic) {
        free(obj->tli_topic);
    }
    PMIX_LIST_DESTRUCT(&(obj->tli_processes));
}
static PMIX_CLASS_INSTANCE(tuple_list_item_t,
                           pmix_list_item_t,
                           tuple_list_item_constructor,
                           tuple_list_item_destructor);

/* List of (filename, topic) tuples that have already been displayed */
static pmix_list_t abd_tuples;


/*
 * Private variables
 */
static const char *dash_line
    = "--------------------------------------------------------------------------\n";

static pmix_status_t match(const char *a, const char *b)
{
    int rc = PMIX_ERROR;
    char *p1, *p2, *tmp1 = NULL, *tmp2 = NULL;
    size_t min;

    /* Check straight string match first */
    if (0 == strcmp(a, b))
        return PMIX_SUCCESS;

    if (NULL != strchr(a, '*') || NULL != strchr(b, '*')) {
        tmp1 = strdup(a);
        if (NULL == tmp1) {
            return PMIX_ERR_OUT_OF_RESOURCE;
        }
        tmp2 = strdup(b);
        if (NULL == tmp2) {
            free(tmp1);
            return PMIX_ERR_OUT_OF_RESOURCE;
        }
        p1 = strchr(tmp1, '*');
        p2 = strchr(tmp2, '*');

        if (NULL != p1) {
            *p1 = '\0';
        }
        if (NULL != p2) {
            *p2 = '\0';
        }
        min = strlen(tmp1);
        if (strlen(tmp2) < min) {
            min = strlen(tmp2);
        }
        if (0 == min || 0 == strncmp(tmp1, tmp2, min)) {
            rc = PMIX_SUCCESS;
        }
        free(tmp1);
        free(tmp2);
        return rc;
    }

    /* No match */
    return PMIX_ERROR;
}


static pmix_status_t pmix_get_tli(const char *filename,
                                  const char *topic,
                                  tuple_list_item_t **tli_)
{
    tuple_list_item_t *tli;

    /* Search the list for a duplicate. */
    PMIX_LIST_FOREACH(tli, &abd_tuples, tuple_list_item_t)
    {
        if (PMIX_SUCCESS == match(tli->tli_filename, filename) &&
            PMIX_SUCCESS == match(tli->tli_topic, topic)) {
            *tli_ = tli;
            return PMIX_SUCCESS;
        }
    }

    /* Nope, we didn't find it -- make a new one */
    tli = PMIX_NEW(tuple_list_item_t);
    if (NULL == tli) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    }
    tli->tli_filename = strdup(filename);
    tli->tli_topic = strdup(topic);
    pmix_list_append(&abd_tuples, &(tli->super));
    *tli_ = tli;

    return PMIX_ERR_NOT_FOUND;
}

static void pmix_show_accumulated_duplicates(int fd, short event, void *context)
{
    time_t now = time(NULL);
    tuple_list_item_t *tli;
    char *tmp;
    PMIX_HIDE_UNUSED_PARAMS(fd, event, context);

    /* Loop through all the messages we've displayed and see if any
       processes have sent duplicates that have not yet been displayed
       yet */
    PMIX_LIST_FOREACH(tli, &abd_tuples, tuple_list_item_t)
    {
        if (tli->tli_display && tli->tli_count_since_last_display > 0) {
            static bool first = true;
            pmix_asprintf(&tmp, "%d more process%s sent help message %s / %s\n",
                          tli->tli_count_since_last_display,
                          (tli->tli_count_since_last_display > 1) ? "es have" : " has",
                          tli->tli_filename, tli->tli_topic);
            tli->tli_time_displayed = time(NULL);
            char stamp[50] = {0};
            strftime(stamp, 50, "%Y-%m-%d %H:%M:%S", localtime(&tli->tli_time_displayed));
            char *buf;
            pmix_asprintf(&buf, "%s-%s", tli->tli_filename, stamp);
            local_delivery(buf, tli->tli_topic, tmp);
            free(buf);
            tli->tli_count_since_last_display = 0;

            if (first) {
                pmix_asprintf(&tmp, "%s", "Set MCA parameter \"base_help_aggregate\" to 0 to see all help / error messages\n");
                local_delivery(tli->tli_filename, tli->tli_topic, tmp);
                first = false;
            }
        }
    }

    show_help_time_last_displayed = now;
    show_help_timer_set = false;
}


pmix_status_t pmix_help_check_dups(const char *filename, const char *topic)
{

    tuple_list_item_t *tli;
    time_t now = time(NULL);
    int rc;

    rc = pmix_get_tli(filename, topic, &tli);
    if (PMIX_SUCCESS == rc) {
        /* Already  displayed!
           But do we want to print anything?  That's complicated.
           We always show the first message of a given (filename,
           topic) tuple as soon as it arrives.  But we don't want to
           show duplicate notices often, because we could get overrun
           with them.  So we want to gather them up and say "We got N
           duplicates" every once in a while.

           And keep in mind that at termination, we'll unconditionally
           show all accumulated duplicate notices.

           A simple scheme is as follows:
              - when the first of a (filename, topic) tuple arrives
              - print the message
              - if a timer is not set, set T=now
              - when a duplicate (filename, topic) tuple arrives
              - if now>(T+5) and timer is not set (due to
                non-pre-emptiveness of our libevent, a timer *could* be
                set!)
              - print all accumulated duplicates
              - reset T=now
              - else if a timer was not set, set the timer for T+5
              - else if a timer was set, do nothing (just wait)
              - set T=now when the timer expires
        */
        ++tli->tli_count_since_last_display;
        if (now > show_help_time_last_displayed + 5 && !show_help_timer_set) {
            pmix_show_accumulated_duplicates(0, 0, NULL);
        }
        if (!show_help_timer_set) {
            pmix_event_evtimer_set(pmix_globals.evbase, &show_help_timer_event,
                                   pmix_show_accumulated_duplicates, NULL);
            pmix_event_evtimer_add(&show_help_timer_event, &show_help_interval);
            show_help_timer_set = true;
        }
    }
    /* Not already displayed */
    else if (PMIX_ERR_NOT_FOUND == rc) {
        if (!show_help_timer_set) {
            show_help_time_last_displayed = now;
        }
    }
    else {
        /* Some other error occurred */
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    return rc;
}


/*
 * Local functions
 */
pmix_status_t pmix_show_help_init(void)
{
    pmix_output_stream_t lds;
    tuple_array_item_t *da;

    if (pmix_show_help_initialized) {
        return PMIX_SUCCESS;
    }

    PMIX_CONSTRUCT(&lds, pmix_output_stream_t);
    lds.lds_want_stderr = true;
    output_stream = pmix_output_open(&lds);
    PMIX_CONSTRUCT(&abd_tuples, pmix_list_t);

    PMIX_CONSTRUCT(&data_arrays, pmix_list_t);
    da = PMIX_NEW(tuple_array_item_t);
    da->project = strdup("pmix");
    da->array = pmix_show_help_data;
    pmix_list_append(&data_arrays, &da->super);

    pmix_show_help_initialized = true;

    return PMIX_SUCCESS;
}

pmix_status_t pmix_show_help_finalize(void)
{
    if (!pmix_show_help_initialized) {
        return PMIX_SUCCESS;
    }

    pmix_output_close(output_stream);
    output_stream = -1;

    PMIX_LIST_DESTRUCT(&abd_tuples);
    PMIX_LIST_DESTRUCT(&data_arrays);
    pmix_show_help_initialized = false;

    return PMIX_SUCCESS;
}

/*
 * Make one big string with all the lines.  This isn't the most
 * efficient method in the world, but we're going for clarity here --
 * not optimization.  :-)
 */
static pmix_status_t array2string(char **outstring,
                                  int want_error_header,
                                  const char **lines)
{
    int i, count;
    size_t len;

    /* See how much space we need */

    len = want_error_header ? 2 * strlen(dash_line) : 0;
    count = PMIx_Argv_count((char**)lines);
    for (i = 0; i < count; ++i) {
        if (NULL == lines[i]) {
            break;
        }
        len += strlen(lines[i]) + 1;
    }

    /* Malloc it out */

    (*outstring) = (char *) malloc(len + 1);
    if (NULL == *outstring) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    }

    /* Fill the big string */

    *(*outstring) = '\0';
    if (want_error_header) {
        strcat(*outstring, dash_line);
    }
    for (i = 0; i < count; ++i) {
        if (NULL == lines[i]) {
            break;
        }
        strcat(*outstring, lines[i]);
        strcat(*outstring, "\n");
    }
    if (want_error_header) {
        strcat(*outstring, dash_line);
    }

    return PMIX_SUCCESS;
}

static const char **get_content(const char *filename,
                                const char* topic)
{
    tuple_array_item_t *da;
    pmix_show_help_file_t *fe;
    pmix_show_help_entry_t *ie;
    int i, j;

    if (!pmix_show_help_initialized) {
        // restrict to local array
        for (i = 0; NULL != pmix_show_help_data[i].filename; ++i) {
            fe = &(pmix_show_help_data[i]);
            if (0 == strcmp(fe->filename, filename)) {
                for (j = 0; NULL != fe->entries[j].topic; ++j) {
                    ie = &(fe->entries[j]);
                    if (0 == strcmp(ie->topic, topic)) {
                        return ie->content;
                    }
                }
            }
        }
        return NULL;
    }

    PMIX_LIST_FOREACH(da, &data_arrays, tuple_array_item_t) {
        for (i = 0; NULL != da->array[i].filename; ++i) {
            fe = &(da->array[i]);
            if (0 == strcmp(fe->filename, filename)) {
                for (j = 0; NULL != fe->entries[j].topic; ++j) {
                    ie = &(fe->entries[j]);
                    if (0 == strcmp(ie->topic, topic)) {
                        return ie->content;
                    }
                }
            }
        }
    }
    return NULL;
}

char *pmix_show_help_vstring(const char *filename,
                             const char *topic,
                             int want_error_header,
                             va_list arglist)
{
    int rc;
    char *single_string, *output;
    const char **content;
    char *msg;

    /* Load the message */
    content = get_content(filename, topic);
    if (NULL == content) {
        pmix_asprintf(&msg, "%sSorry!  You were supposed to get help about:\n\n    Filename: %s\n    Topic: %s\n\nBut I couldn't find "
                    "that help reference.\n\nSorry!\n%s", dash_line, filename, topic, dash_line);
        local_delivery(filename, topic, msg);
        free(msg);
        return NULL;
   }

    /* Convert it to a single raw string */
    rc = array2string(&single_string, want_error_header, content);

    if (PMIX_SUCCESS == rc) {
        /* Apply the formatting to make the final output string */
        if (0 > vasprintf(&output, single_string, arglist)) {
            output = NULL;
        }
        free(single_string);
    }

    return (PMIX_SUCCESS == rc) ? output : NULL;
}

char *pmix_show_help_string(const char *filename,
                            const char *topic,
                            int want_error_handler, ...)
{
    char *output;
    va_list arglist;

    va_start(arglist, want_error_handler);
    output = pmix_show_help_vstring(filename, topic,
                                    want_error_handler, arglist);
    va_end(arglist);

    return output;
}

pmix_status_t pmix_show_help(const char *filename,
                             const char *topic,
                             int want_error_header, ...)
{
    va_list arglist;
    char *output;

    va_start(arglist, want_error_header);
    output = pmix_show_help_vstring(filename, topic,
                                    want_error_header, arglist);
    va_end(arglist);

    /* If nothing came back, there's nothing to do */
    if (NULL == output) {
        return PMIX_SUCCESS;
    }

    local_delivery(filename, topic, output);
    return PMIX_SUCCESS;
}

pmix_status_t pmix_show_help_add_data(const char *project,
                                      pmix_show_help_file_t *array)
{
    tuple_array_item_t *da;
    pmix_show_help_file_t *fe;
    int i, j;

    // check for duplicate entries
    PMIX_LIST_FOREACH(da, &data_arrays, tuple_array_item_t) {
        for (i = 0; NULL != da->array[i].filename; ++i) {
            fe = &(da->array[i]);
            for (j=0; NULL != array[j].filename; j++) {
                if (0 == strcmp(fe->filename, array[j].filename)) {
                    // complain
                    pmix_show_help("help-pmix-util.txt", "duplicate-filename", true,
                                   project, da->project, fe->filename);
                    return PMIX_ERROR;
                }
            }
        }
    }
    da = PMIX_NEW(tuple_array_item_t);
    da->project = strdup(project);
    da->array = array;
    pmix_list_append(&data_arrays, &da->super);
    return PMIX_SUCCESS;
}

pmix_status_t pmix_show_help_norender(const char *filename,
                                      const char *topic,
                                      const char *output)
{
    local_delivery(filename, topic, output);
    return PMIX_SUCCESS;
}
