/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PMIX_PTL_USOCK_H
#define PMIX_PTL_USOCK_H

#include "src/include/pmix_config.h"

#include "src/mca/ptl/ptl.h"

BEGIN_C_DECLS

typedef struct {
    pmix_ptl_base_component_t super;
    /* the directory this process made for its socket, and the socket in
     * it - both removed at close */
    char *dir;
    char *path;
    /* the directory, held open: every change in it is made through this,
     * never by its path */
    int dfd;
} pmix_ptl_usock_component_t;

PMIX_EXPORT extern pmix_ptl_usock_component_t pmix_mca_ptl_usock_component;
PMIX_EXPORT extern pmix_ptl_module_t pmix_ptl_usock_module;

/* the scheme a usock URI's address begins with */
#define PMIX_PTL_USOCK_SCHEME "usock://"

END_C_DECLS

#endif /* PMIX_PTL_USOCK_H */
