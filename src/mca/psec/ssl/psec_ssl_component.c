/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "src/include/pmix_config.h"
#include "pmix_common.h"

#include "psec_ssl.h"
#include "src/mca/psec/psec.h"

static pmix_status_t component_register(void);
static pmix_status_t component_open(void);
static pmix_status_t component_close(void);
static pmix_status_t component_query(pmix_mca_base_module_t **module, int *priority);
static pmix_psec_module_t *assign_module(void);

pmix_psec_ssl_params_t pmix_psec_ssl_params = {
    .priority = 5,
    .ca_file = NULL,
    .crl_file = NULL,
    .cert_file = NULL,
    .key_file = NULL,
    .max_skew = 300
};

/*
 * Instantiate the public struct with all of our public information
 * and pointers to our public functions in it
 */
pmix_psec_base_component_t pmix_mca_psec_ssl_component = {
    .base = {
        PMIX_MCA_BASE_VERSION(psec),

        /* Component name and version */
        .pmix_mca_component_name = "ssl",
        PMIX_MCA_BASE_MAKE_VERSION(component,
                                   PMIX_MAJOR_VERSION,
                                   PMIX_MINOR_VERSION,
                                   PMIX_RELEASE_VERSION),

        /* Component open and close functions */
        .pmix_mca_register_component_params = component_register,
        .pmix_mca_open_component = component_open,
        .pmix_mca_close_component = component_close,
        .pmix_mca_query_component = component_query,
    },
    .assign_module = assign_module
};
PMIX_MCA_BASE_COMPONENT_INIT(pmix, psec, ssl)

static int component_register(void)
{
    pmix_mca_base_component_t *c = &pmix_mca_psec_ssl_component.base;

    /* below native's 10, so a process with nothing to say about it keeps
     * authenticating local peers the way it always has; a remote peer
     * asks for ssl by name */
    (void) pmix_mca_base_component_var_register(
        c, "priority", "Priority of the ssl security component",
        PMIX_MCA_BASE_VAR_TYPE_INT, &pmix_psec_ssl_params.priority);
    (void) pmix_mca_base_component_var_register(
        c, "ca_file",
        "PEM file of the certificate authorities a connecting peer's certificate "
        "must chain to. Needed to accept ssl connections",
        PMIX_MCA_BASE_VAR_TYPE_STRING, &pmix_psec_ssl_params.ca_file);
    (void) pmix_mca_base_component_var_register(
        c, "crl_file",
        "PEM file of certificate revocation lists. When given, a peer's "
        "certificate is refused if revoked - or if no list from its issuer is present",
        PMIX_MCA_BASE_VAR_TYPE_STRING, &pmix_psec_ssl_params.crl_file);
    (void) pmix_mca_base_component_var_register(
        c, "cert_file",
        "PEM file holding this process's certificate, followed by any intermediate "
        "certificates. Its subject's common name must be the name of the user it "
        "authenticates. Needed to connect with ssl",
        PMIX_MCA_BASE_VAR_TYPE_STRING, &pmix_psec_ssl_params.cert_file);
    (void) pmix_mca_base_component_var_register(
        c, "key_file",
        "PEM file holding the unencrypted private key for cert_file. It must not be "
        "readable by anyone but its owner",
        PMIX_MCA_BASE_VAR_TYPE_STRING, &pmix_psec_ssl_params.key_file);
    (void) pmix_mca_base_component_var_register(
        c, "max_skew",
        "Seconds a credential remains acceptable either side of the time it was made. "
        "Bounds both the clock difference allowed between hosts and how long a "
        "server must remember a credential to refuse its replay",
        PMIX_MCA_BASE_VAR_TYPE_INT, &pmix_psec_ssl_params.max_skew);
    return PMIX_SUCCESS;
}

static int component_open(void)
{
    return PMIX_SUCCESS;
}

static int component_query(pmix_mca_base_module_t **module, int *priority)
{
    /* nothing to authenticate with and nothing to authenticate against:
     * stay out of the way without a word, as munge does without munged */
    if (NULL == pmix_psec_ssl_params.ca_file &&
        (NULL == pmix_psec_ssl_params.cert_file || NULL == pmix_psec_ssl_params.key_file)) {
        *module = NULL;
        return PMIX_ERR_NOT_AVAILABLE;
    }
    *priority = pmix_psec_ssl_params.priority;
    *module = (pmix_mca_base_module_t *) &pmix_ssl_module;
    return PMIX_SUCCESS;
}

static int component_close(void)
{
    return PMIX_SUCCESS;
}

static pmix_psec_module_t *assign_module(void)
{
    return &pmix_ssl_module;
}
