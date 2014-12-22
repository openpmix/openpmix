/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2006-2013 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2009-2012 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2011      Oak Ridge National Labs.  All rights reserved.
 * Copyright (c) 2013-2014 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014      Artem Y. Polyakov <artpol84@gmail.com>.
 *                         All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#ifndef USOCK_H
#define USOCK_H

#include <sys/socket.h>
#include <sys/un.h>

extern int pmix_usock_connect(struct sockaddr *addr, int max_retries);
extern void pmix_usock_process_msg(int fd, short flags, void *cbdata);
extern void pmix_usock_send_recv(int fd, short args, void *cbdata);
extern void pmix_usock_send_handler(int sd, short flags, void *cbdata);
extern void pmix_usock_recv_handler(int sd, short flags, void *cbdata);
extern void pmix_usock_dump(const char* msg);
extern int usock_send_connect_ack(int sd);
extern int usock_recv_connect_ack(int sd);
extern int usock_set_nonblocking(int sd);
extern int usock_set_blocking(int sd);
extern int send_bytes(int sd, char **buf, int *remain);
extern int read_bytes(int sd, char **buf, int *remain);

#endif // USOCK_H
