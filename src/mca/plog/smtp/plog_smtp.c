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
#include "src/util/pmix_printf.h"
#include "src/util/pmix_show_help.h"

#include "plog_smtp.h"
#include "src/mca/plog/base/base.h"

/* Static API's */
static pmix_status_t init(void);
static void finalize(void);
static pmix_status_t mylog(const pmix_proc_t *source, const pmix_info_t data[], size_t ndata,
                           const pmix_info_t directives[], size_t ndirs);

/* Module */
pmix_plog_module_t pmix_plog_smtp_module = {
    .name = "smtp",
    .channels = NULL,
    .init = init,
    .finalize = finalize,
    .log = mylog
};

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

static pmix_status_t init(void)
{
    char *mychannels = "email";

    pmix_plog_smtp_module.channels = PMIx_Argv_split(mychannels, ',');
    return PMIX_SUCCESS;
}

static void finalize(void)
{
    PMIx_Argv_free(pmix_plog_smtp_module.channels);
}

/*
 * Convert lone \n's to \r\n
 */
static char *crnl(char *orig)
{
    int i, j, max, count;
    char *str;

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
        } else {
            *len = 0;
            return NULL;
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
        } else {
            *len = 0;
            return NULL;
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
static pmix_status_t send_email(char *msg, char *from, char *addrs,
                                char *subject, time_t timestamp)
{
    int i, err = PMIX_SUCCESS;
    char *str = NULL;
    char **myaddrs = NULL;
    char *myfrom;
    char *errmsg = NULL;
    struct sigaction sig, oldsig;
    bool set_oldsig = false;
    smtp_session_t session = NULL;
    smtp_message_t message = NULL;
    message_status_t ms;
    pmix_plog_smtp_component_t *c = &pmix_mca_plog_smtp_component;

    // check that we have recipients
    if (NULL == addrs) {
        if (NULL == c->to) {
            // nope - nobody to send to
            return PMIX_ERR_BAD_PARAM;
        } else {
            myaddrs = PMIx_Argv_split(c->to, ',');
        }
    } else {
            myaddrs = PMIx_Argv_split(addrs, ',');
    }

    if (NULL == from) {
        if (NULL == c->from_addr) {
            // nope - nobody to send from
            return PMIX_ERR_BAD_PARAM;
        } else {
            myfrom = c->from_addr;
        }
    } else {
        myfrom = from;
    }

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
    pmix_asprintf(&str, "%s:%d", c->server, c->port);
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

    // set the "To" header
    if (0 == smtp_set_header(message, "To", NULL, NULL)) {
        err = PMIX_ERROR;
        errmsg = "stmp_set_header TO";
        goto error;
    }

    /* Add the recipients */
    for (i = 0; NULL != myaddrs[i]; ++i) {
        if (NULL == smtp_add_recipient(message, myaddrs[i])) {
            err = PMIX_ERR_OUT_OF_RESOURCE;
            errmsg = "stmp_add_recipient";
            goto error;
        }
    }

    // set the subject header
    if (NULL == subject) {
        str = c->subject;
    } else {
        str = subject;
    }
    if (0 == smtp_set_header(message, "Subject", str)) {
        err = PMIX_ERROR;
        errmsg = "smtp_set_header SUBJECT";
        str = NULL;
        goto error;
    }


    /* set the X-Mailer */
    asprintf(&str, "PMIx SMTP Plog v%d.%d.%d", c->super.pmix_mca_component_major_version,
             c->super.pmix_mca_component_minor_version,
             c->super.pmix_mca_component_release_version);
    if (0 == smtp_set_header(message, "X-Mailer", str)) {
        err = PMIX_ERROR;
        errmsg = "smtp_set_header XMAILER";
        goto error;
    }

    if (0 == smtp_set_header(message, "From",
                             (NULL != c->from_name ? c->from_name : myfrom),
                              myfrom)) {
        err = PMIX_ERROR;
        errmsg = "smtp_set_header FROM";
        goto error;
    }
    free(str);
    str = NULL;

    // provide the timestamp
    if (0 < timestamp) {
        struct tm *local;
        char *t;

        local = localtime(&timestamp);
        t = asctime(local);
        t[strlen(t) - 1] = '\0'; // remove trailing newline
        if (0 == smtp_set_header(message, "Timestamp", t)) {
            err = PMIX_ERROR;
            errmsg = "smtp_set_header TIMESTAMP";
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

        memset(em, 0, 256);
        e = smtp_errno();
        smtp_strerror(e, em, sizeof(em));
        pmix_show_help("help-pmix-plog.txt", "smtp:send_email failed", true,
                       "libesmtp library call failed", errmsg, em, e, msg);
    }
    return err;
}

static pmix_status_t mylog(const pmix_proc_t *source, const pmix_info_t data[], size_t ndata,
                           const pmix_info_t directives[], size_t ndirs)
{
    char *addrs = NULL, *msg = NULL;
    char *subject = NULL, *from = NULL;
    size_t n;
    time_t timestamp = 0;
    pmix_info_t *input = NULL;
    size_t ninput;
    pmix_status_t rc;
    PMIX_HIDE_UNUSED_PARAMS(source);

    /* if there is no data, then we don't handle it */
    if (NULL == data || 0 == ndata) {
        return PMIX_ERR_NOT_AVAILABLE;
    }

    /* check to see if there is an email request */
    for (n = 0; n < ndata; n++) {
        if (PMIx_Check_key(data[n].key, PMIX_LOG_EMAIL)) {
            if (NULL != input) {
                // cannot have more than one email per call
                PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
                return PMIX_ERR_BAD_PARAM;
            }
            input = (pmix_info_t*)data[n].value.data.darray->array;
            ninput = data[n].value.data.darray->size;
        }
    }

    // check for directives
    for (n = 0; n < ndirs; n++) {
        if (PMIx_Check_key(directives[n].key, PMIX_LOG_TIMESTAMP)) {
            timestamp = directives[n].value.data.time;
        }
    }

    // if no email was requested, then move on
    if (NULL == input) {
        return PMIX_ERR_TAKE_NEXT_OPTION;
    }

    // check the array for input
    for (n=0; n < ninput; n++) {
        if (PMIx_Check_key(input[n].key, PMIX_LOG_EMAIL_ADDR)) {
            addrs = input[n].value.data.string;
            continue;
        }

        if (PMIx_Check_key(input[n].key, PMIX_LOG_EMAIL_SENDER_ADDR)) {
            from = input[n].value.data.string;
            continue;
        }

        if (PMIx_Check_key(input[n].key, PMIX_LOG_EMAIL_SUBJECT)) {
            subject = input[n].value.data.string;
            continue;
        }

        if (PMIx_Check_key(input[n].key, PMIX_LOG_MSG)) {
            if (NULL != msg) {
                // multiple messages are not supported
                PMIX_ERROR_LOG(PMIX_ERR_NOT_SUPPORTED);
                return PMIX_ERR_NOT_SUPPORTED;
            }
            if (PMIX_STRING == input[n].value.type) {
                msg = input[n].value.data.string;
            } else if (PMIX_BYTE_OBJECT == input[n].value.type) {
                msg = input[n].value.data.bo.bytes;
            } else {
                return PMIX_ERR_BAD_PARAM;
            }
            continue;
        }

    }

    /* If there wasn't a message, then we are done */
    if (NULL == msg) {
        return PMIX_ERR_TAKE_NEXT_OPTION;
    }

    rc = send_email(msg, from, addrs, subject, timestamp);
    return rc;
}
