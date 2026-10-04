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
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
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

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#ifdef HAVE_STRINGS_H
#    include <strings.h>
#endif
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
    const char *msg;
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
 * Convert lone \n's and lone \r's to \r\n, so that every line break
 * in the result is a CRLF pair
 */
static char *crnl(const char *orig)
{
    size_t i, j, max, count;
    char *str;

    /* Count how much space we need - orig[i + 1] is at worst the
     * terminator */
    count = max = strlen(orig);
    for (i = 0; i < max; ++i) {
        if ('\r' == orig[i] && '\n' != orig[i + 1]) {
            ++count;
        } else if ('\n' == orig[i] && (0 == i || '\r' != orig[i - 1])) {
            ++count;
        }
    }

    str = malloc(count + 1);
    if (NULL == str) {
        return NULL;
    }
    for (j = i = 0; i < max; ++i) {
        /* SMTP wants the CR ahead of the LF - writing a second '\n'
         * here just doubles the line break and never produces the CRLF
         * this function exists to produce */
        if ('\r' == orig[i]) {
            str[j++] = '\r';
            if ('\n' != orig[i + 1]) {
                str[j++] = '\n';
            }
        } else if ('\n' == orig[i]) {
            if (0 == i || '\r' != orig[i - 1]) {
                str[j++] = '\r';
            }
            str[j++] = '\n';
        } else {
            str[j++] = orig[i];
        }
    }
    str[j] = '\0';
    return str;
}

/*
 * Copy a string for use as a header value, replacing each control
 * character with a space so that the value stays on its one line.
 * A value that goes inside a quoted phrase also may not carry a
 * quote or a backslash.
 */
static char *header_text(const char *orig, bool quoted)
{
    char *str;
    size_t i;
    unsigned char c;

    str = strdup(orig);
    if (NULL == str) {
        return NULL;
    }
    for (i = 0; '\0' != str[i]; i++) {
        c = (unsigned char) str[i];
        if (iscntrl(c)) {
            str[i] = ' ';
        } else if (quoted && ('"' == c || '\\' == c)) {
            str[i] = '\'';
        }
    }
    return str;
}

/*
 * Remove leading and trailing white space in place
 */
static void trim(char *str)
{
    size_t len, start;

    len = strlen(str);
    while (0 < len && isspace((unsigned char) str[len - 1])) {
        str[--len] = '\0';
    }
    for (start = 0; start < len && isspace((unsigned char) str[start]); start++) {
    }
    if (0 < start) {
        memmove(str, str + start, len - start + 1);
    }
}

/*
 * Callback function invoked via smtp_start_session()
 */
static const char *message_cb(void **buf, int *len, void *arg)
{
    message_status_t *ms = (message_status_t *) arg;
    PMIX_HIDE_UNUSED_PARAMS(buf);

    /* libesmtp calls us with a NULL length to tell us to rewind */
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
        ms->sent_flag = SENT_BODY_PREFIX;
        if (NULL != pmix_mca_plog_smtp_component.body_prefix) {
            ms->prev_string = crnl(pmix_mca_plog_smtp_component.body_prefix);
            if (NULL != ms->prev_string) {
                *len = strlen(ms->prev_string);
                return ms->prev_string;
            }
        }
        /* returning NULL ends the message as far as libesmtp is
         * concerned, so an absent prefix must fall through to the body
         * rather than stop here - otherwise configuring no prefix sends
         * an empty email */
        /* fall through */

    case SENT_BODY_PREFIX:
        ms->sent_flag = SENT_BODY;
        ms->prev_string = crnl(ms->msg);
        if (NULL == ms->prev_string) {
            *len = 0;
            return NULL;
        }
        *len = strlen(ms->prev_string);
        return ms->prev_string;

    case SENT_BODY:
        ms->sent_flag = SENT_BODY_SUFFIX;
        if (NULL != pmix_mca_plog_smtp_component.body_suffix) {
            ms->prev_string = crnl(pmix_mca_plog_smtp_component.body_suffix);
            if (NULL != ms->prev_string) {
                *len = strlen(ms->prev_string);
                return ms->prev_string;
            }
        }
        *len = 0;
        return NULL;

    case SENT_BODY_SUFFIX:
    case SENT_ALL:
    default:
        ms->sent_flag = SENT_ALL;
        *len = 0;
        return NULL;
    }
}

/*
 * Pick the requested recipients out of the plog_smtp_to list. Every
 * address in the comma-delimited "requested" list must match one of
 * the configured ones (ignoring case and surrounding white space).
 * The returned argv holds the configured spelling of each match.
 */
static pmix_status_t select_recipients(const char *requested, char ***rcpts)
{
    char **allowed, **wanted;
    char **out = NULL;
    size_t i, j;
    bool found;

    *rcpts = NULL;
    allowed = PMIx_Argv_split(pmix_mca_plog_smtp_component.to, ',');
    wanted = PMIx_Argv_split(requested, ',');
    if (NULL == allowed || NULL == wanted) {
        PMIx_Argv_free(allowed);
        PMIx_Argv_free(wanted);
        return PMIX_ERR_BAD_PARAM;
    }
    for (i = 0; NULL != allowed[i]; i++) {
        trim(allowed[i]);
    }
    for (i = 0; NULL != wanted[i]; i++) {
        trim(wanted[i]);
        if ('\0' == wanted[i][0]) {
            continue;
        }
        found = false;
        for (j = 0; NULL != allowed[j]; j++) {
            if ('\0' != allowed[j][0] &&
                0 == strcasecmp(wanted[i], allowed[j])) {
                found = true;
                break;
            }
        }
        if (!found) {
            pmix_output_verbose(2, pmix_plog_base_framework.framework_output,
                                "SMTP: requested recipient is not in plog_smtp_to");
            PMIx_Argv_free(allowed);
            PMIx_Argv_free(wanted);
            PMIx_Argv_free(out);
            return PMIX_ERR_NO_PERMISSIONS;
        }
        if (PMIX_SUCCESS != PMIx_Argv_append_unique_nosize(&out, allowed[j])) {
            PMIx_Argv_free(allowed);
            PMIx_Argv_free(wanted);
            PMIx_Argv_free(out);
            return PMIX_ERR_NOMEM;
        }
    }
    PMIx_Argv_free(allowed);
    PMIx_Argv_free(wanted);
    if (NULL == out) {
        return PMIX_ERR_BAD_PARAM;
    }
    *rcpts = out;
    return PMIX_SUCCESS;
}

/*
 * Back-end function to actually send the email. The recipients come
 * from select_recipients and the sender address is plog_smtp_from_addr;
 * the From phrase names the process that made the request.
 */
static pmix_status_t send_email(const char *msg, char **myaddrs,
                                const pmix_proc_t *source,
                                const char *subject, time_t timestamp)
{
    int i;
    pmix_status_t err = PMIX_SUCCESS;
    char *str = NULL;
    char *phrase = NULL;
    char *subj = NULL;
    const char *errmsg = NULL;
    struct sigaction sig, oldsig;
    bool set_oldsig = false;
    smtp_session_t session = NULL;
    smtp_message_t message = NULL;
    message_status_t ms;
    pmix_plog_smtp_component_t *c = &pmix_mca_plog_smtp_component;

    /* the callback below reads every one of these fields on its first
     * invocation - leaving them as whatever was on the stack means
     * free()ing a wild pointer and rendering a garbage message */
    ms.sent_flag = SENT_NONE;
    ms.msg = msg;
    ms.prev_string = NULL;

    /* component_query does not select us without it, so this is
     * only a guard */
    if (NULL == c->from_addr) {
        return PMIX_ERR_NOT_AVAILABLE;
    }

    /* the From phrase identifies the requester */
    if (NULL != source) {
        pmix_asprintf(&str, "%s (%s:%s)",
                      (NULL != c->from_name) ? c->from_name : "PMIx Plog",
                      source->nspace, PMIX_RANK_PRINT(source->rank));
    } else {
        str = strdup((NULL != c->from_name) ? c->from_name : "PMIx Plog");
    }
    if (NULL != str) {
        phrase = header_text(str, true);
        free(str);
        str = NULL;
    }
    subj = header_text((NULL != subject) ? subject : c->subject, false);
    if (NULL == phrase || NULL == subj) {
        free(phrase);
        free(subj);
        return PMIX_ERR_NOMEM;
    }

    /* Temporarily disable SIGPIPE so that if remote servers timeout
       or hang up on us, it doesn't kill this application.  We'll
       restore the original SIGPIPE handler when we're done.

       This is libESMTP's requirement, not ours: it writes to a socket
       the remote SMTP server may drop at any moment (an idle timeout, a
       421), it does not set MSG_NOSIGNAL or SO_NOSIGPIPE on the sockets
       it owns, and it documents that the *application* must arrange for
       SIGPIPE to be ignored.  We are not the application - hence the
       save/ignore/restore around just the smtp_start_session() call
       rather than a disposition we set and keep.

       It is still the one place in the library that touches a
       process-wide signal disposition, which everything else here has
       been moved off (see the poll notes in src/common/pmix_iof.c and
       src/common/pmix_pfexec.h).  The thread-safe form is
       pthread_sigmask: SIGPIPE from write() is raised synchronously to
       the writing thread, so blocking it *in this thread* is sufficient
       and never reaches the host.  That needs a drain -
       sigtimedwait() with a zero timeout - or a SIGPIPE raised while
       blocked stays pending on the thread and kills the process the
       moment the mask is restored; and sigtimedwait does not exist on
       macOS, so it would take a configure test plus a fallback to
       exactly the code below.  Not worth it for a component that only
       builds with --with-smtp and restores what it changed - but do it
       that way if this ever moves onto a path that runs by default. */
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
        errmsg = "smtp_create_session";
        goto error;
    }

    /* Create the message */
    message = smtp_add_message(session);
    if (NULL == message) {
        err = PMIX_ERROR;
        errmsg = "smtp_add_message";
        goto error;
    }

    /* Set the SMTP server (yes, it's a weird return status!) */
    pmix_asprintf(&str, "%s:%d", c->server, c->port);
    if (0 == smtp_set_server(session, str)) {
        err = PMIX_ERROR;
        errmsg = "smtp_set_server";
        goto error;
    }
    free(str);
    str = NULL;

    /* Add the sender */
    if (0 == smtp_set_reverse_path(message, c->from_addr)) {
        err = PMIX_ERROR;
        errmsg = "smtp_set_reverse_path";
        goto error;
    }

    // set the "To" header
    if (0 == smtp_set_header(message, "To", NULL, NULL)) {
        err = PMIX_ERROR;
        errmsg = "smtp_set_header TO";
        goto error;
    }

    /* Add the recipients */
    for (i = 0; NULL != myaddrs[i]; ++i) {
        if (NULL == smtp_add_recipient(message, myaddrs[i])) {
            err = PMIX_ERR_OUT_OF_RESOURCE;
            errmsg = "smtp_add_recipient";
            goto error;
        }
    }

    // set the subject header
    if (0 == smtp_set_header(message, "Subject", subj)) {
        err = PMIX_ERROR;
        errmsg = "smtp_set_header SUBJECT";
        goto error;
    }

    /* set the X-Mailer */
    pmix_asprintf(&str, "PMIx SMTP Plog v%d.%d.%d", c->super.pmix_mca_component_major_version,
             c->super.pmix_mca_component_minor_version,
             c->super.pmix_mca_component_release_version);
    if (0 == smtp_set_header(message, "X-Mailer", str)) {
        err = PMIX_ERROR;
        errmsg = "smtp_set_header XMAILER";
        goto error;
    }

    if (0 == smtp_set_header(message, "From", phrase, c->from_addr)) {
        err = PMIX_ERROR;
        errmsg = "smtp_set_header FROM";
        goto error;
    }
    free(str);
    str = NULL;

    // provide the timestamp
    if (0 < timestamp) {
        struct tm local;
        char t[32];
        size_t tlen;

        /* localtime/asctime render into process-wide static storage, so
         * the strip below would corrupt whatever another thread is
         * holding onto - and either may decline a time it cannot
         * represent rather than hand back something to index into */
        if (NULL != localtime_r(&timestamp, &local) &&
            NULL != asctime_r(&local, t)) {
            tlen = strlen(t);
            if (0 < tlen && '\n' == t[tlen - 1]) {
                t[tlen - 1] = '\0'; // remove trailing newline
            }
            if (0 == smtp_set_header(message, "Timestamp", t)) {
                err = PMIX_ERROR;
                errmsg = "smtp_set_header TIMESTAMP";
                goto error;
            }
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
    free(phrase);
    free(subj);
    /* the callback holds the last chunk it handed libesmtp, and nothing
     * calls it again once the session is over */
    if (NULL != ms.prev_string) {
        free(ms.prev_string);
        ms.prev_string = NULL;
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
    char *msg = NULL, *scratch = NULL;
    char *subject = NULL, *addrs = NULL;
    char **rcpts = NULL;
    size_t n, mine = 0;
    time_t timestamp = 0;
    pmix_info_t *input = NULL;
    size_t ninput = 0;
    pmix_status_t rc;
    /* completion is tracked in the caller's array - see the plog
     * AGENTS.md on why that state is shared and mutable */
    pmix_info_t *dt = (pmix_info_t *) data;

    /* if there is no data, then we don't handle it */
    if (NULL == data || 0 == ndata) {
        return PMIX_ERR_NOT_AVAILABLE;
    }

    /* check to see if there is an email request */
    for (n = 0; n < ndata; n++) {
        if (PMIX_INFO_OP_IS_COMPLETE(&data[n])) {
            continue;
        }
        if (PMIx_Check_key(data[n].key, PMIX_LOG_EMAIL)) {
            if (NULL != input) {
                // cannot have more than one email per call
                PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
                return PMIX_ERR_BAD_PARAM;
            }
            /* the type is the caller's to set, and for a client's
             * PMIx_Log that caller is on the far end of a socket -
             * confirm this really is an array before walking it */
            if (PMIX_DATA_ARRAY != data[n].value.type ||
                NULL == data[n].value.data.darray ||
                PMIX_INFO != data[n].value.data.darray->type ||
                NULL == data[n].value.data.darray->array) {
                PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
                return PMIX_ERR_BAD_PARAM;
            }
            input = (pmix_info_t*)data[n].value.data.darray->array;
            ninput = data[n].value.data.darray->size;
            mine = n;
        }
    }

    // check for directives
    if (NULL != directives) {
        for (n = 0; n < ndirs; n++) {
            if (PMIx_Check_key(directives[n].key, PMIX_LOG_TIMESTAMP) &&
                PMIX_TIME == directives[n].value.type) {
                timestamp = directives[n].value.data.time;
            }
        }
    }

    // if no email was requested, then move on
    if (NULL == input) {
        return PMIX_ERR_TAKE_NEXT_OPTION;
    }

    /* check the array for input. The sender is the component's to
     * set, so PMIX_LOG_EMAIL_SENDER_ADDR is not read */
    for (n=0; n < ninput; n++) {
        if (PMIx_Check_key(input[n].key, PMIX_LOG_EMAIL_ADDR)) {
            if (PMIX_STRING == input[n].value.type) {
                addrs = input[n].value.data.string;
            }
            continue;
        }

        if (PMIx_Check_key(input[n].key, PMIX_LOG_EMAIL_SUBJECT)) {
            if (PMIX_STRING == input[n].value.type) {
                subject = input[n].value.data.string;
            }
            continue;
        }

        if (PMIx_Check_key(input[n].key, PMIX_LOG_MSG)) {
            if (NULL != msg) {
                // multiple messages are not supported
                PMIX_ERROR_LOG(PMIX_ERR_NOT_SUPPORTED);
                free(scratch);
                return PMIX_ERR_NOT_SUPPORTED;
            }
            if (PMIX_STRING == input[n].value.type) {
                msg = input[n].value.data.string;
            } else if (PMIX_BYTE_OBJECT == input[n].value.type) {
                /* a byte object carries a length, not a terminator, and
                 * everything downstream of here is strlen-based - so it
                 * has to be copied into a string before it is used */
                if (NULL == input[n].value.data.bo.bytes ||
                    0 == input[n].value.data.bo.size) {
                    continue;
                }
                scratch = malloc(input[n].value.data.bo.size + 1);
                if (NULL == scratch) {
                    return PMIX_ERR_NOMEM;
                }
                memcpy(scratch, input[n].value.data.bo.bytes,
                       input[n].value.data.bo.size);
                scratch[input[n].value.data.bo.size] = '\0';
                msg = scratch;
            } else {
                PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
                free(scratch);
                return PMIX_ERR_BAD_PARAM;
            }
            continue;
        }

    }

    /* If there wasn't a message, then we are done */
    if (NULL == msg) {
        free(scratch);
        return PMIX_ERR_TAKE_NEXT_OPTION;
    }

    /* the recipients must be named, and each must be one of the
     * plog_smtp_to addresses */
    if (NULL == addrs) {
        PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
        free(scratch);
        return PMIX_ERR_BAD_PARAM;
    }
    rc = select_recipients(addrs, &rcpts);
    if (PMIX_SUCCESS != rc) {
        free(scratch);
        return rc;
    }

    rc = send_email(msg, rcpts, source, subject, timestamp);
    PMIx_Argv_free(rcpts);
    free(scratch);
    if (PMIX_SUCCESS == rc) {
        /* nobody else should send this a second time */
        PMIX_INFO_OP_COMPLETED(&dt[mine]);
    }
    return rc;
}
