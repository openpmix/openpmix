/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2008 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2009     Cisco Systems, Inc. All rights reserved.
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015      Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * Simple smtp plog (using libesmtp)
 */

#include "pmix_config.h"

#include "src/mca/base/pmix_mca_base_var.h"

#include "pmix_common.h"
#include "src/util/pmix_show_help.h"
#include "src/mca/plog/base/base.h"
#include "plog_smtp.h"

static pmix_status_t smtp_component_query(pmix_mca_base_module_t **module, int *priority);
static pmix_status_t smtp_close(void);
static pmix_status_t smtp_register(void);

/*
 * Struct of function pointers that need to be initialized
 */
pmix_plog_smtp_component_t pmix_mca_plog_smtp_component = {
    .super = {
        PMIX_PLOG_BASE_VERSION_1_0_0,

        .pmix_mca_component_name = "smtp",

        PMIX_MCA_BASE_MAKE_VERSION(component, PMIX_MAJOR_VERSION, PMIX_MINOR_VERSION,
                                   PMIX_RELEASE_VERSION),
        .pmix_mca_close_component = smtp_close,
        .pmix_mca_query_component = smtp_component_query,
        .pmix_mca_register_component_params = smtp_register,
    },
    .version = NULL,
    .server = NULL,
    .to = NULL,
    .from_name = NULL,
    .subject = NULL,
    .body_prefix = NULL,
    .body_suffix = NULL
};
PMIX_MCA_BASE_COMPONENT_INIT(pmix, plog, smtp)

static char *server = NULL;
static char *myfrom = NULL;
static char *subject = NULL;
static char *suffix = NULL;
static char *prefix = NULL;

static pmix_status_t smtp_register(void)
{
    char version[256];

    /* Server stuff */
    (void) pmix_mca_base_component_var_register(&pmix_mca_plog_smtp_component.super, "server",
                                                "SMTP server name or IP address",
                                                PMIX_MCA_BASE_VAR_TYPE_STRING,
                                                &server);

    if (NULL != server) {
        pmix_mca_plog_smtp_component.server = strdup(server);
    } else {
        pmix_mca_plog_smtp_component.server = strdup("localhost");
   }

    pmix_mca_plog_smtp_component.port = 25;
    (void) pmix_mca_base_component_var_register(&pmix_mca_plog_smtp_component.super, "port",
                                                "SMTP server port", PMIX_MCA_BASE_VAR_TYPE_INT,
                                                &pmix_mca_plog_smtp_component.port);

    /* Email stuff */
    (void) pmix_mca_base_component_var_register(&pmix_mca_plog_smtp_component.super, "to",
                                                "Comma-delimited list of default email addresses to send to",
                                                PMIX_MCA_BASE_VAR_TYPE_STRING,
                                                &pmix_mca_plog_smtp_component.to);

    (void) pmix_mca_base_component_var_register(&pmix_mca_plog_smtp_component.super, "from_addr",
                                                "Email address that messages will be from",
                                                PMIX_MCA_BASE_VAR_TYPE_STRING,
                                                &pmix_mca_plog_smtp_component.from_addr);

    
    (void) pmix_mca_base_component_var_register(&pmix_mca_plog_smtp_component.super, "from_name",
                                                "Email name that messages will be from",
                                                PMIX_MCA_BASE_VAR_TYPE_STRING,
                                                &myfrom);
    if (NULL != myfrom) {
        pmix_mca_plog_smtp_component.from_name = strdup(myfrom);
    } else {
        pmix_mca_plog_smtp_component.from_name = strdup("PMIx Plog");
    }

    (void) pmix_mca_base_component_var_register(&pmix_mca_plog_smtp_component.super, "subject",
                                                "Default email subject", PMIX_MCA_BASE_VAR_TYPE_STRING,
                                                &subject);
    if (NULL != subject) {
        pmix_mca_plog_smtp_component.subject = strdup(subject);
    } else {
        pmix_mca_plog_smtp_component.subject = strdup("PMIx Plog");
    }

    /* Mail body prefix and suffix */
     (void) pmix_mca_base_component_var_register(&pmix_mca_plog_smtp_component.super, "body_prefix",
                                                "Text to put at the beginning of the mail message",
                                                PMIX_MCA_BASE_VAR_TYPE_STRING,
                                                &prefix);

     if (NULL != prefix) {
        pmix_mca_plog_smtp_component.body_prefix = strdup(prefix);
     } else {
        pmix_mca_plog_smtp_component.body_prefix = strdup(
        "The PMIx SMTP plog wishes to inform you of the following message:\n\n");
     }

    (void) pmix_mca_base_component_var_register(&pmix_mca_plog_smtp_component.super, "body_prefix",
                                                "Text to put at the end of the mail message",
                                                PMIX_MCA_BASE_VAR_TYPE_STRING,
                                                &suffix);

    if (NULL != suffix) {
        pmix_mca_plog_smtp_component.body_suffix = strdup(suffix);
    } else {
        pmix_mca_plog_smtp_component.body_suffix = strdup("\n\nSincerely,\nOscar the PMIx Owl");
    }

    /* Priority */
    pmix_mca_plog_smtp_component.priority = 10;
    (void) pmix_mca_base_component_var_register(&pmix_mca_plog_smtp_component.super, "priority",
                                                "Priority of this component",
                                                PMIX_MCA_BASE_VAR_TYPE_INT,
                                                &pmix_mca_plog_smtp_component.priority);
    /* Libesmtp version */
    smtp_version(version, sizeof(version), 0);
    version[sizeof(version) - 1] = '\0';
    pmix_mca_plog_smtp_component.version = strdup(version);

    return PMIX_SUCCESS;
}

static pmix_status_t smtp_close(void)
{
    if (NULL != pmix_mca_plog_smtp_component.version) {
        free(pmix_mca_plog_smtp_component.version);
    }
    if (NULL != pmix_mca_plog_smtp_component.server) {
        free(pmix_mca_plog_smtp_component.server);
    }
    if (NULL != pmix_mca_plog_smtp_component.body_prefix) {
        free(pmix_mca_plog_smtp_component.body_prefix);
    }
    if (NULL != pmix_mca_plog_smtp_component.body_suffix) {
        free(pmix_mca_plog_smtp_component.body_suffix);
    }
    if (NULL != pmix_mca_plog_smtp_component.from_name) {
        free(pmix_mca_plog_smtp_component.from_name);
    }
    if (NULL != pmix_mca_plog_smtp_component.subject) {
        free(pmix_mca_plog_smtp_component.subject);
    }
    return PMIX_SUCCESS;
}

static pmix_status_t smtp_component_query(pmix_mca_base_module_t **module, int *priority)
{
    *priority = 0;
    *module = NULL;


    /* Since we have to open a socket later, try to resolve the IP
       address of the server now.  Save the result, or abort if we
       can't resolve it. */
    pmix_mca_plog_smtp_component.server_hostent = gethostbyname(pmix_mca_plog_smtp_component.server);
    if (NULL == pmix_mca_plog_smtp_component.server_hostent) {
        pmix_output_verbose(5, pmix_plog_base_framework.framework_output,
                            "SMTP: unable to resolve server %s; disabled",
                            pmix_mca_plog_smtp_component.server);
        return PMIX_ERR_NOT_FOUND;
    }

    *priority = 10;
    *module = (pmix_mca_base_module_t *) &pmix_plog_smtp_module;
    return PMIX_SUCCESS;
}
