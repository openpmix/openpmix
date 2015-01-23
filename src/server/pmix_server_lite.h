/*
 * Copyright (c) 2015      Intel, Inc. All rights reserved
 * Copyright (c) 2015      Artem Y. Polyakov <artpol84@gmail.com>.
 *                         All rights reserved.
 * $COPYRIGHT$
 */

#ifndef PMIX_SERVER_LITE_H
#define PMIX_SERVER_LITE_H

#include "pmix_config.h"
#include "src/api/pmix_server.h"
#include "src/usock/usock.h"

typedef struct pmix_message_local_t {
    char hdr_rcvd;
    pmix_usock_hdr_t hdr;
    char *payload;
} pmix_message_inst_t;


#endif // PMIX_MESSAGE_H
