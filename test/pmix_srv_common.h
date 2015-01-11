/*
 * Copyright (c) 2013-2015 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015      Artem Y. Polyakov <artpol84@gmail.com>.
 *                         All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#ifndef PMIX_SRV_SELF_COMMON_H
#define PMIX_SRV_SELF_COMMON_H

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <event.h>

/* Copy from src/client/usock.c */
int blocking_recv(int sd, void *buf, int size);
int blocking_send(int sd, void *ptr, size_t size);
int start_listening(struct sockaddr_un *address);
int run_client(struct sockaddr_un address, struct event_base *base,
               char **env, int *service);
int reply_cli_auth(int sd, int rc);
int verify_kvps(void *blob);

#endif // PMIX_SRV_SELF_COMMON_H
