/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2007      Sun Microsystems, Inc.  All rights reserved.
 * Copyright (c) 2009      Cisco Systems, Inc. All rights reserved.
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * Send an email upon plog events.
 */

#include "pmix_config.h"
#include "pmix_common.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif
#include <signal.h>

#include "src/util/pmix_argv.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_name_fns.h"
#include "src/util/pmix_show_help.h"

#include "plog_smtp.h"
#include "src/mca/plog/base/base.h"

/* Static API's */
static pmix_status_t mylog(const pmix_proc_t *source, const pmix_info_t data[], size_t ndata,
                           const pmix_info_t directives[], size_t ndirs, pmix_op_cbfunc_t cbfunc,
                           void *cbdata);

/* Module */
pmix_plog_module_t pmix_plog_smtp_module = {.name = "smtp", .channels = "email", .log = mylog};

typedef enum {
    SENT_NONE,
    SENT_HEADER,
    SENT_BODY_PREFIX,
    SENT_BODY,
    SENT_BODY_SUFFIX,
    SENT_ALL
} sent_flag_t;

typedef struct {
    sent_flag_t sent_flag;
    char *msg;
    char *prev_string;
} message_status_t;

/*
 * Convert lone \n's to \r\n
 */
static char *crnl(char *orig)
{
    int i, j, max, count;
    char *str;
    return strdup(orig);

    /* Count how much space we need */
    count = max = strlen(orig);
    for (i = 0; i < max; ++i) {
        if (orig[i] == '\n' && i > 0 && orig[i - 1] != '\r') {
            ++count;
        }
    }

    /* Copy, changing \n to \r\n */
    str = malloc(count + 1);
    for (j = i = 0; i < max; ++i) {
        if (orig[i] == '\n' && i > 0 && orig[i - 1] != '\r') {
            str[j++] = '\n';
        }
        str[j++] = orig[i];
    }
    str[j] = '\0';
    return str;
}

/*
 * Callback function invoked via smtp_start_session()
 */
static const char *message_cb(void **buf, int *len, void *arg)
{
    message_status_t *ms = (message_status_t *) arg;

    if (NULL == *buf) {
        *buf = malloc(8192);
    }
    if (NULL == len) {
        ms->sent_flag = SENT_NONE;
        return NULL;
    }

    /* Free the previous string */
    if (NULL != ms->prev_string) {
        free(ms->prev_string);
        ms->prev_string = NULL;
    }

    switch (ms->sent_flag) {
    case SENT_NONE:
        /* Send a blank line to signify the end of the header */
        ms->sent_flag = SENT_HEADER;
        ms->prev_string = NULL;
        *len = 2;
        return "\r\n";

    case SENT_HEADER:
        if (NULL != pmix_mca_plog_smtp_component.body_prefix) {
            ms->sent_flag = SENT_BODY_PREFIX;
            ms->prev_string = crnl(pmix_mca_plog_smtp_component.body_prefix);
            *len = strlen(ms->prev_string);
            return ms->prev_string;
        }

    case SENT_BODY_PREFIX:
        ms->sent_flag = SENT_BODY;
        ms->prev_string = crnl(ms->msg);
        *len = strlen(ms->prev_string);
        return ms->prev_string;

    case SENT_BODY:
        if (NULL != pmix_mca_plog_smtp_component.body_suffix) {
            ms->sent_flag = SENT_BODY_SUFFIX;
            ms->prev_string = crnl(pmix_mca_plog_smtp_component.body_suffix);
            *len = strlen(ms->prev_string);
            return ms->prev_string;
        }

    case SENT_BODY_SUFFIX:
    case SENT_ALL:
    default:
        ms->sent_flag = SENT_ALL;
        *len = 0;
        return NULL;
    }
}

/*
 * Back-end function to actually send the email
 */
static int send_email(char *msg)
{
    int i, err = PMIX_SUCCESS;
    char *str = NULL;
    char *errmsg = NULL;
    struct sigaction sig, oldsig;
    bool set_oldsig = false;
    smtp_session_t session = NULL;
    smtp_message_t message = NULL;
    message_status_t ms;
    pmix_plog_smtp_component_t *c = &pmix_mca_plog_smtp_component;

    if (NULL == c->to_argv) {
        c->to_argv = PMIx_Argv_split(c->to, ',');
        if (NULL == c->to_argv || NULL == c->to_argv[0]) {
            return PMIX_ERR_OUT_OF_RESOURCE;
        }
    }

    ms.sent_flag = SENT_NONE;
    ms.prev_string = NULL;
    ms.msg = msg;

    /* Temporarily disable SIGPIPE so that if remote servers timeout
       or hang up on us, it doesn't kill this application.  We'll
       restore the original SIGPIPE handler when we're done. */
    sig.sa_handler = SIG_IGN;
    sigemptyset(&sig.sa_mask);
    sig.sa_flags = 0;
    sigaction(SIGPIPE, &sig, &oldsig);
    set_oldsig = true;

    /* Try to get a libesmtp session.  If so, assume that libesmtp is
       happy and proceed */
    session = smtp_create_session();
    if (NULL == session) {
        err = PMIX_ERR_NOT_SUPPORTED;
        errmsg = "stmp_create_session";
        goto error;
    }

    /* Create the message */
    message = smtp_add_message(session);
    if (NULL == message) {
        err = PMIX_ERROR;
        errmsg = "stmp_add_message";
        goto error;
    }

    /* Set the SMTP server (yes, it's a weird return status!) */
    asprintf(&str, "%s:%d", c->server, c->port);
    if (0 == smtp_set_server(session, str)) {
        err = PMIX_ERROR;
        errmsg = "stmp_set_server";
        goto error;
    }
    free(str);
    str = NULL;

    /* Add the sender */
    if (0 == smtp_set_reverse_path(message, c->from_addr)) {
        err = PMIX_ERROR;
        errmsg = "stmp_set_reverse_path";
        goto error;
    }

    /* Set the subject and some headers */
    asprintf(&str, "PMIx SMTP Plog v%d.%d.%d", c->super.base.pmix_mca_component_major_version,
             c->super.base.pmix_mca_component_minor_version,
             c->super.base.pmix_mca_component_release_version);
    if (0 == smtp_set_header(message, "Subject", c->subject)
        || 0 == smtp_set_header_option(message, "Subject", Hdr_OVERRIDE, 1)
        || 0 == smtp_set_header(message, "To", NULL, NULL)
        || 0
               == smtp_set_header(message, "From",
                                  (NULL != c->from_name ? c->from_name : c->from_addr),
                                  c->from_addr)
        || 0 == smtp_set_header(message, "X-Mailer", str)
        || 0 == smtp_set_header_option(message, "Subject", Hdr_OVERRIDE, 1)) {
        err = PMIX_ERROR;
        errmsg = "smtp_set_header";
        goto error;
    }
    free(str);
    str = NULL;

    /* Add the recipients */
    for (i = 0; NULL != c->to_argv[i]; ++i) {
        if (NULL == smtp_add_recipient(message, c->to_argv[i])) {
            err = PMIX_ERR_OUT_OF_RESOURCE;
            errmsg = "stmp_add_recipient";
            goto error;
        }
    }

    /* Set the callback to get the message */
    if (0 == smtp_set_messagecb(message, message_cb, &ms)) {
        err = PMIX_ERROR;
        errmsg = "smtp_set_messagecb";
        goto error;
    }

    /* Send it! */
    if (0 == smtp_start_session(session)) {
        err = PMIX_ERROR;
        errmsg = "smtp_start_session";
        goto error;
    }

    /* Fall through */

error:
    if (NULL != str) {
        free(str);
    }
    if (NULL != session) {
        smtp_destroy_session(session);
    }
    /* Restore the SIGPIPE handler */
    if (set_oldsig) {
        sigaction(SIGPIPE, &oldsig, NULL);
    }
    if (PMIX_SUCCESS != err) {
        int e;
        char em[256];

        e = smtp_errno();
        smtp_strerror(e, em, sizeof(em));
        pmix_show_help("help-pmix-plog.txt", "smtp:send_email failed", true,
                       "libesmtp library call failed", errmsg, em, e, msg);
    }
    return err;
}

static pmix_status_t mylog(const pmix_proc_t *source, const pmix_info_t data[], size_t ndata,
                           const pmix_info_t directives[], size_t ndirs, pmix_op_cbfunc_t cbfunc,
                           void *cbdata)
{
    char *output, *msg;
    size_t n;
    bool generic = false, local = false, global = fals;
    time_t timestamp;

    /* if there are no directives, then we don't handle it */
    if (NULL == directives || 0 == ndirs) {
        return PMIX_ERR_NOT_AVAILABLE;
    }

    /* check to see if there are any email directives */
    for (n = 0; n < ndirs; n++) {
        if (0 == strncmp(directives[n].key, PMIX_LOG_EMAIL, PMIX_MAX_KEYLEN)) {
            /* we default to using the local syslog */
            generic = true;
            msg = strdup(directives[n].value.data.string);
        } else if (0 == strncmp(directives[n].key, PMIX_LOG_LOCAL_SYSLOG, PMIX_MAX_KEYLEN)) {
            local = true;
            msg = strdup(directives[n].value.data.string);
        } else if (0 == strncmp(directives[n].key, PMIX_LOG_GLOBAL_SYSLOG, PMIX_MAX_KEYLEN)) {
            global = true;
            msg = strdup(directives[n].value.data.string);
        } else if (0 == strncmp(directives[n].key, PMIX_LOG_SYSLOG_PRI, PMIX_MAX_KEYLEN)) {
            pri = directives[n].value.data.integer;
        } else if (0 == strncmp(directives[n].key, PMIX_LOG_TIMESTAMP, PMIX_MAX_KEYLEN)) {
            timestamp = directives[n].value.data.time;
        }
    }
    /* If there was a message, output it */
    vasprintf(&output, msg, ap);

    if (NULL != output) {
        send_email(output);
        free(output);
    }
}
