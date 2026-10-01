/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PMIX_PSEC_SSL_H
#define PMIX_PSEC_SSL_H

#include "src/include/pmix_config.h"

#include "src/mca/psec/psec.h"

BEGIN_C_DECLS

/* the component must be visible data for the linker to find it */
PMIX_EXPORT extern pmix_psec_base_component_t pmix_mca_psec_ssl_component;
extern pmix_psec_module_t pmix_ssl_module;

/* MCA parameters - see psec_ssl_component.c */
typedef struct {
    int priority;
    char *ca_file;   /* CAs a peer's certificate must chain to */
    char *crl_file;  /* revoked certificates, checked when given */
    char *cert_file; /* our own certificate chain, leaf first */
    char *key_file;  /* the private key for our certificate */
    int max_skew;    /* seconds a credential stays valid either side of now */
} pmix_psec_ssl_params_t;

extern pmix_psec_ssl_params_t pmix_psec_ssl_params;

END_C_DECLS

#endif
