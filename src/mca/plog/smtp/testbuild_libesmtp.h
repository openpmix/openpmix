/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Non-functional "test build" shim for the subset of the libesmtp API
 * used by this component. It is included in place of the real
 * <libesmtp.h> only when the library is configured with
 * --enable-test-build (PMIX_TESTBUILD) so that the component can be
 * compile-checked on a machine that lacks the libesmtp development
 * headers. The stubs return placeholder values and do NOT send any
 * email - a component built against this shim is not functional.
 */

#ifndef PMIX_PLOG_SMTP_TESTBUILD_H
#define PMIX_PLOG_SMTP_TESTBUILD_H

#include "pmix_config.h"

#include <stdlib.h>

typedef struct pmix_testbuild_smtp_session *smtp_session_t;
typedef struct pmix_testbuild_smtp_message *smtp_message_t;
typedef struct pmix_testbuild_smtp_recipient *smtp_recipient_t;

typedef const char *(*smtp_messagecb_t)(void **buf, int *len, void *arg);

static inline smtp_session_t smtp_create_session(void)
{
    return NULL;
}

static inline smtp_message_t smtp_add_message(smtp_session_t session)
{
    (void) session;
    return NULL;
}

static inline int smtp_set_server(smtp_session_t session, const char *hostport)
{
    (void) session;
    (void) hostport;
    return 1;
}

static inline int smtp_set_reverse_path(smtp_message_t message, const char *mailbox)
{
    (void) message;
    (void) mailbox;
    return 1;
}

static inline int smtp_set_header(smtp_message_t message, const char *header, ...)
{
    (void) message;
    (void) header;
    return 1;
}

static inline smtp_recipient_t smtp_add_recipient(smtp_message_t message, const char *mailbox)
{
    (void) message;
    (void) mailbox;
    return NULL;
}

static inline int smtp_set_messagecb(smtp_message_t message, smtp_messagecb_t cb, void *arg)
{
    (void) message;
    (void) cb;
    (void) arg;
    return 1;
}

static inline int smtp_start_session(smtp_session_t session)
{
    (void) session;
    return 1;
}

static inline void smtp_destroy_session(smtp_session_t session)
{
    (void) session;
}

static inline int smtp_errno(void)
{
    return 0;
}

static inline char *smtp_strerror(int e, char *buf, size_t buflen)
{
    (void) e;
    (void) buflen;
    return buf;
}

static inline int smtp_version(void *buf, size_t len, int what)
{
    (void) what;
    if (NULL != buf && 0 < len) {
        ((char *) buf)[0] = '\0';
    }
    return 0;
}

#endif /* PMIX_PLOG_SMTP_TESTBUILD_H */
