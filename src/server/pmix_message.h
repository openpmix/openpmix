#ifndef PMIX_MESSAGE_H
#define PMIX_MESSAGE_H

#include "pmix_config.h"
#include "src/api/pmix_server.h"
#include "src/usock/usock.h"

typedef struct pmix_message_local {
    char hdr_rcvd;
    pmix_usock_hdr_t hdr;
    char *payload;
} pmix_message_inst_t;


#endif // PMIX_MESSAGE_H
