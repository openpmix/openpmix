/*
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 *
 * Copyright (c) 2021-2022 Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PMIX_NATIVE_H
#define PMIX_NATIVE_H

#include "src/include/pmix_config.h"

#include "src/mca/psec/psec.h"

BEGIN_C_DECLS

/* the component must be visible data for the linker to find it */
PMIX_EXPORT extern pmix_psec_base_component_t pmix_mca_psec_native_component;
extern pmix_psec_module_t pmix_native_module;

typedef struct {
    /* the directory under which a server makes the sockets for its
     * local-socket check, and where a peer looks for them - each as that
     * process sees it. NULL means the server's own tmpdir, and the
     * directory the server names */
    char *socket_dir;
    /* accept a peer from a release without the local-socket check on
     * the identity it claims, when the kernel cannot confirm it */
    bool legacy_auth;
    /* (testing only) never look the peer up in the TCP table, so every
     * peer able to run the local-socket check is asked to */
    bool force_handshake;
} pmix_psec_native_params_t;
PMIX_EXPORT extern pmix_psec_native_params_t pmix_psec_native_params;

END_C_DECLS

#endif
