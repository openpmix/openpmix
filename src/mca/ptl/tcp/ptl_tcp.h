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
 * Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PMIX_PTL_TCP_H
#define PMIX_PTL_TCP_H

#include "src/include/pmix_config.h"

#ifdef HAVE_SYS_SOCKET_H
#    include <sys/socket.h>
#endif

#include "src/mca/ptl/ptl.h"

BEGIN_C_DECLS

typedef struct {
    pmix_ptl_base_component_t super;
    /* the address the primary listener is bound to */
    struct sockaddr_storage connection;
} pmix_ptl_tcp_component_t;

PMIX_EXPORT extern pmix_ptl_tcp_component_t pmix_mca_ptl_tcp_component;
PMIX_EXPORT extern pmix_ptl_module_t pmix_ptl_tcp_module;

/* Resolve a comma-delimited list of interface names and subnets
 * (a.b.c.d/e, IPv4 only) to interface names - see ptl_tcp.c for what is
 * refused */
PMIX_EXPORT pmix_status_t pmix_ptl_tcp_split_and_resolve(const char *orig_str,
                                                         const char *name,
                                                         char ***interfaces);

END_C_DECLS

#endif /* PMIX_PTL_TCP_H */
