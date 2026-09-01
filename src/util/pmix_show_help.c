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
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "src/include/pmix_config.h"

#include <errno.h>
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

static void local_delivery(const char *file,
                           const char *topic,
                           const char *msg)
{
    pmix_shift_caddy_t *cd;

    if (NULL == msg) {
        /* nothing to deliver - and the load below would put a NULL
         * string on the wire for the host to render */
        return;
    }
    /* the file and topic only label the message; a caller that could not
     * build one still gets the message itself delivered */
    if (NULL == file) {
        file = "";
    }
    if (NULL == topic) {
        topic = "";
    }

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
    if (NULL == cd) {
        /* losing the message entirely is worse than losing the
         * aggregation, so say it here */
        fprintf(stderr, "%s", msg);
        return;
    }
    cd->ninfo = 1;
    PMIX_INFO_CREATE(cd->info, cd->ninfo);
    PMIX_INFO_LOAD(&cd->info[0], PMIX_LOG_STDERR, msg, PMIX_STRING);
    cd->infocopy = true;
    cd->ndirs = 2;
    PMIX_INFO_CREATE(cd->directives, cd->ndirs);
    PMIX_INFO_LOAD(&cd->directives[0], PMIX_LOG_KEY, file, PMIX_STRING);
    PMIX_INFO_LOAD(&cd->directives[1], PMIX_LOG_VAL, topic, PMIX_STRING);
    cd->dircopy = true;
    PMIX_PROC_CREATE(cd->proc, 1);
    memcpy(cd->proc, &pmix_globals.myid, sizeof(pmix_proc_t));
    PMIX_THREADSHIFT(cd, pmix_log_local_op); // function will release the caddy
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
    p->array = NULL;
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
    if (0 == strcmp(a, b)) {
        return PMIX_SUCCESS;
    }

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
    if (NULL == tli->tli_filename || NULL == tli->tli_topic) {
        /* match() hands both of these straight to strcmp(), so an entry
         * carrying a NULL is a segfault on the very next lookup - and
         * this list outlives the call that built it */
        PMIX_RELEASE(tli);
        return PMIX_ERR_OUT_OF_RESOURCE;
    }
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
        if (tli->tli_display && 0 < tli->tli_count_since_last_display) {
            static bool first = true;
            char stamp[50] = {0};
            char *buf = NULL;
            struct tm *when;

            if (0 > pmix_asprintf(&tmp, "%d more process%s sent help message %s / %s\n",
                                  tli->tli_count_since_last_display,
                                  (1 < tli->tli_count_since_last_display) ? "es have" : " has",
                                  tli->tli_filename, tli->tli_topic)) {
                continue;
            }
            tli->tli_time_displayed = time(NULL);
            when = localtime(&tli->tli_time_displayed);
            if (NULL != when) {
                strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", when);
            }
            /* the stamped name only labels the notice - if we cannot
             * build it, deliver the notice under the bare filename
             * rather than dropping it */
            if (0 > pmix_asprintf(&buf, "%s-%s", tli->tli_filename, stamp)) {
                buf = NULL;
            }
            local_delivery((NULL == buf) ? tli->tli_filename : buf,
                           tli->tli_topic, tmp);
            free(buf);
            /* local_delivery copies the message, so we own tmp */
            free(tmp);
            tli->tli_count_since_last_display = 0;

            if (first) {
                if (0 <= pmix_asprintf(&tmp, "%s", "Set MCA parameter \"base_help_aggregate\" to 0 to see all help / error messages\n")) {
                    local_delivery(tli->tli_filename, tli->tli_topic, tmp);
                    free(tmp);
                }
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

    /* both of these reach strcmp() by way of match() */
    if (NULL == filename || NULL == topic) {
        return PMIX_ERR_BAD_PARAM;
    }

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
    PMIX_DESTRUCT(&lds);
    PMIX_CONSTRUCT(&abd_tuples, pmix_list_t);

    PMIX_CONSTRUCT(&data_arrays, pmix_list_t);
    da = PMIX_NEW(tuple_array_item_t);
    if (NULL == da) {
        goto failed;
    }
    da->project = strdup("pmix");
    if (NULL == da->project) {
        PMIX_RELEASE(da);
        goto failed;
    }
    da->array = pmix_show_help_data;
    pmix_list_append(&data_arrays, &da->super);

    pmix_show_help_initialized = true;

    return PMIX_SUCCESS;

failed:
    /* leave nothing half-built: every caller of this function ignores
     * what it answers, and the next one in re-enters it */
    pmix_output_close(output_stream);
    output_stream = -1;
    PMIX_DESTRUCT(&data_arrays);
    PMIX_DESTRUCT(&abd_tuples);
    return PMIX_ERR_OUT_OF_RESOURCE;
}

pmix_status_t pmix_show_help_finalize(void)
{
    if (!pmix_show_help_initialized) {
        return PMIX_SUCCESS;
    }

    /* Say what we were holding back.  pmix_help_check_dups() promises
     * that accumulated duplicates are shown unconditionally at
     * termination, and this is the only place left to do it - the list
     * goes away below.  Clearing the initialized flag first sends them
     * down local_delivery()'s direct path: pmix_rte_finalize() has
     * already paused the progress thread by the time we are called, so
     * anything thread-shifted onto the event base from here would never
     * run and never be released. */
    pmix_show_help_initialized = false;
    pmix_show_accumulated_duplicates(0, 0, NULL);

    /* This state is process-global and outlives a finalize: PMIx can be
     * initialized again in the same process, and a timer flag left set
     * would keep the aggregation timer from ever being armed again. */
    if (show_help_timer_set) {
        pmix_event_del(&show_help_timer_event);
        show_help_timer_set = false;
    }
    show_help_time_last_displayed = 0;

    pmix_output_close(output_stream);
    output_stream = -1;

    PMIX_LIST_DESTRUCT(&abd_tuples);
    PMIX_LIST_DESTRUCT(&data_arrays);

    return PMIX_SUCCESS;
}

/*
 * Make one big string with all the lines.  This isn't the most
 * efficient method in the world, but we're going for clarity here --
 * not optimization.  :-)
 */
static pmix_status_t array2string(char **outstring,
                                  int want_error_header,
                                  char **lines)
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

    (*outstring) = (char *) calloc((len + 1), sizeof(char));
    if (NULL == *outstring) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    }

    /* Fill the big string - calloc already left it terminated */

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

/* An "#include" chain has to terminate: a topic that includes itself,
 * or two that include each other, would otherwise recurse until the
 * stack runs out.  This content is generated from the help-*.txt files,
 * so that is a typo away rather than an attack, but the failure is the
 * same either way. */
#define PMIX_SHOW_HELP_MAX_INCLUDE_DEPTH 16

/*
 * Parse an include directive - "#include#FILE#TOPIC", or
 * "#include#PROJECT#FILE#TOPIC" - taking the fields from the right.
 *
 * The fields point into *line, which the caller owns and must free;
 * *project is left NULL when the directive names none, and the caller
 * supplies its own default.  Answers false, having freed nothing and
 * set nothing, if the line is not a well-formed directive: it is
 * generated content, so that is an authoring mistake, but it must not
 * be a crash.
 */
static bool parse_include(const char *content, char **line,
                          char **project, char **file, char **topic)
{
    char *work;
    char *p;

    work = strdup(content);
    if (NULL == work) {
        return false;
    }

    p = strrchr(work, '#');
    if (NULL == p) {
        free(work);
        return false;
    }
    *p = '\0';
    *topic = p + 1;

    p = strrchr(work, '#');
    if (NULL == p) {
        free(work);
        return false;
    }
    *p = '\0';
    *file = p + 1;

    p = strrchr(work, '#');
    if (NULL == p) {
        free(work);
        return false;
    }
    ++p;
    /* if this isn't pointing at "include", then it must be a project;
     * otherwise the caller's default applies */
    if (0 != strncmp(p, "include", strlen("include"))) {
        *project = p;
    } else {
        *project = NULL;
    }

    *line = work;
    return true;
}

static void get_content(char ***output,
                        char *project,
                        const char *filename,
                        const char* topic,
                        int depth)
{
    tuple_array_item_t *da;
    pmix_show_help_file_t *fe;
    pmix_show_help_entry_t *ie;
    int i, j, m;
    char *tp, *file, *pj, *line;

    /* Both of these are handed straight to strcmp() below, so a caller
     * that has no file or topic to name has to be turned away here
     * rather than crashing the lookup. Leaving *output alone makes this
     * read as "no such help", which is what the caller does with every
     * other lookup that finds nothing. */
    if (NULL == filename || NULL == topic) {
        return;
    }

    if (!pmix_show_help_initialized || NULL == project ||
        0 == strcmp(project, "pmix")) {
        // restrict to local array
        for (i = 0; NULL != pmix_show_help_data[i].filename; ++i) {
            fe = &(pmix_show_help_data[i]);
            if (0 == strcmp(fe->filename, filename)) {
                for (j = 0; NULL != fe->entries[j].topic; ++j) {
                    ie = &(fe->entries[j]);
                    if (0 == strcmp(ie->topic, topic)) {
                        // found a matching entry
                        for (m=0; NULL != ie->content[m]; m++) {
                            if (0 == strncmp(ie->content[m], "#include", strlen("#include"))) {
                                // parse the project (if provided), file and
                                // topic being included. parse_include works on
                                // a writable copy so it can NUL-terminate each
                                // field - ie->content[m] is a read-only string
                                // literal.
                                if (PMIX_SHOW_HELP_MAX_INCLUDE_DEPTH <= depth) {
                                    continue;
                                }
                                if (!parse_include(ie->content[m], &line, &pj, &file, &tp)) {
                                    continue;
                                }
                                get_content(output, (NULL == pj) ? "pmix" : pj,
                                            file, tp, depth + 1);
                                free(line);
                            } else {
                                PMIx_Argv_append_nosize(output, ie->content[m]);
                            }
                        }
                        return;
                    }
                }
            }
        }
        // if we didn't find an entry and the project is NULL and
        // we are initialized, then check the other projects
        if (pmix_show_help_initialized && NULL == project) {
            goto next;
        }
        return;
    }

next:
    PMIX_LIST_FOREACH(da, &data_arrays, tuple_array_item_t) {
        if (NULL != project && 0 != strcmp(project, da->project)) {
            continue;
        }
        for (i = 0; NULL != da->array[i].filename; ++i) {
            fe = &(da->array[i]);
            if (0 == strcmp(fe->filename, filename)) {
                for (j = 0; NULL != fe->entries[j].topic; ++j) {
                    ie = &(fe->entries[j]);
                    if (0 == strcmp(ie->topic, topic)) {
                        for (m=0; NULL != ie->content[m]; m++) {
                            if (0 == strncmp(ie->content[m], "#include", strlen("#include"))) {
                                // parse the project (if provided), file and topic being included
                                if (PMIX_SHOW_HELP_MAX_INCLUDE_DEPTH <= depth) {
                                    continue;
                                }
                                if (!parse_include(ie->content[m], &line, &pj, &file, &tp)) {
                                    continue;
                                }
                                get_content(output, (NULL == pj) ? da->project : pj,
                                            file, tp, depth + 1);
                                free(line);
                            } else {
                                PMIx_Argv_append_nosize(output, ie->content[m]);
                            }
                        }
                        return;
                    }
                }
            }
        }
    }
}

char *pmix_show_help_vstring(const char *filename,
                             const char *topic,
                             int want_error_header,
                             va_list arglist)
{
    int rc;
    char *single_string = NULL;
    char *output = NULL;
    char **content = NULL;
    char *msg;

    /* Load the message */
    get_content(&content, NULL, filename, topic, 0);
    if (NULL == content) {
        /* a NULL here is a caller bug, but naming it in the notice is
         * more use than passing it to a %s that has no defined answer
         * for one */
        if (0 > pmix_asprintf(&msg, "%sSorry!  You were supposed to get help about:\n\n    Filename: %s\n    Topic: %s\n\nBut I couldn't find "
                              "that help reference.\n\nSorry!\n%s", dash_line,
                              (NULL == filename) ? "(none given)" : filename,
                              (NULL == topic) ? "(none given)" : topic, dash_line)) {
            return NULL;
        }
        local_delivery(filename, topic, msg);
        free(msg);
        return NULL;
   }

    /* Convert it to a single raw string */
    rc = array2string(&single_string, want_error_header, content);
    PMIx_Argv_free(content);

    if (PMIX_SUCCESS == rc) {
        /* Apply the formatting to make the final output string */
        if (0 > pmix_vasprintf(&output, single_string, arglist)) {
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

    /* local_delivery copies the message, so we own the rendered string */
    local_delivery(filename, topic, output);
    free(output);
    return PMIX_SUCCESS;
}

pmix_status_t pmix_show_help_add_data(const char *project,
                                      pmix_show_help_file_t *array)
{
    tuple_array_item_t *da;
    pmix_show_help_file_t *fe;
    int i, j;

    /* project reaches strcmp() and strdup() below, and array is walked
     * without a guard of its own */
    if (NULL == project || NULL == array) {
        return PMIX_ERR_BAD_PARAM;
    }

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
    if (NULL == da) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    }
    da->project = strdup(project);
    if (NULL == da->project) {
        /* the search loop above compares this against strcmp() */
        PMIX_RELEASE(da);
        return PMIX_ERR_OUT_OF_RESOURCE;
    }
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
