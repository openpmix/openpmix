/*
 * Copyright (c) 2009      Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2012      Los Alamos National Security, Inc. All rights reserved.
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 *
 * Copyright (c) 2021-2022 Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * @file:
 *
 */

#ifndef PMIX_PSENSOR_H_
#define PMIX_PSENSOR_H_

#include "src/include/pmix_config.h"

#include "src/class/pmix_list.h"
#include "src/include/pmix_globals.h"
#include "src/mca/mca.h"

BEGIN_C_DECLS

/*
 * Component functions - all MUST be provided!
 */

/* start a sensor operation:
 *
 * requestor - the process requesting this operation
 *
 * monitor - a PMIx attribute specifying what is to be monitored
 *
 * directives - an array of pmix_info_t specifying relevant limits on values, and action
 *              to be taken when limits exceeded. Can include
 *              user-provided "id" string */
typedef pmix_status_t (*pmix_psensor_base_module_start_fn_t)(pmix_peer_t *requestor,
                                                             pmix_status_t error,
                                                             const pmix_info_t *monitor,
                                                             const pmix_info_t directives[],
                                                             size_t ndirs);

/* stop a sensor operation:
 *
 * requestor - the process requesting this operation
 *
 * id - the "id" string provided by the user at the time the
 *      affected monitoring operation was started. A NULL indicates
 *      that all operations started by this requestor are to
 *      be terminated */
typedef pmix_status_t (*pmix_psensor_base_module_stop_fn_t)(pmix_peer_t *requestor, char *id);

/* API module */
/*
 * Ver 1.0
 */
typedef struct pmix_psensor_base_module_1_0_0_t {
    pmix_psensor_base_module_start_fn_t start;
    pmix_psensor_base_module_stop_fn_t stop;
} pmix_psensor_base_module_t;

/*
 * the standard component data structure
 */
typedef pmix_mca_base_component_t pmix_psensor_base_component_t;

/* The psensor framework interface version. It is stated here and nowhere
 * else: components stamp it into their struct with
 * PMIX_MCA_BASE_VERSION(psensor), and the framework's declaration
 * reaches the same three by pasting its name, so the two cannot drift
 * apart. Bump it on any change to the module interface that a component
 * built against the previous one would not survive. */
#define PMIX_MCA_psensor_MAJOR_VERSION   1
#define PMIX_MCA_psensor_MINOR_VERSION   0
#define PMIX_MCA_psensor_RELEASE_VERSION 0

/* Global structure for accessing sensor functions
 */
PMIX_EXPORT extern pmix_psensor_base_module_t pmix_psensor; /* holds API function pointers */

END_C_DECLS

#endif /* MCA_SENSOR_H */
