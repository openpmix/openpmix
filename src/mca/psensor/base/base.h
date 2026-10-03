/*
 * Copyright (c) 2009      Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2013      Los Alamos National Security, LLC.  All rights reserved.
 * Copyright (c) 2017-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2020      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
/** @file:
 */

#ifndef PMIX_PSENSOR_BASE_H_
#define PMIX_PSENSOR_BASE_H_

#include "src/include/pmix_config.h"

#include "src/class/pmix_list.h"
#include "src/mca/base/pmix_mca_base_framework.h"
#include "src/mca/mca.h"

#include "src/mca/psensor/psensor.h"
#include "src/threads/pmix_mutex.h"

BEGIN_C_DECLS

/*
 * MCA Framework
 */
PMIX_EXPORT extern pmix_mca_base_framework_t pmix_psensor_base_framework;

PMIX_EXPORT int pmix_psensor_base_select(void);

/* define a struct to hold framework-global values */
typedef struct {
    pmix_list_t actives;
    pmix_event_base_t *evbase;
    bool selected;
    /* how many monitors each peer holds, across every component - see
     * pmix_psensor_base_claim() */
    pmix_mutex_t lock;
    pmix_list_t claims;
    int max_per_peer;
} pmix_psensor_base_t;

typedef struct {
    pmix_list_item_t super;
    pmix_psensor_base_component_t *component;
    pmix_psensor_base_module_t *module;
    int priority;
} pmix_psensor_active_module_t;
PMIX_EXPORT PMIX_CLASS_DECLARATION(pmix_psensor_active_module_t);

PMIX_EXPORT extern pmix_psensor_base_t pmix_psensor_base;

PMIX_EXPORT pmix_status_t pmix_psensor_base_start(pmix_peer_t *requestor, pmix_status_t error,
                                                  const pmix_info_t *monitor,
                                                  const pmix_info_t directives[], size_t ndirs);

PMIX_EXPORT pmix_status_t pmix_psensor_base_stop(pmix_peer_t *requestor, char *id);

/* The event base the monitors run on, starting the monitor thread if this
 * is the first monitor (see psensor_base_use_separate_thread). NULL if the
 * thread cannot be started. A component calls it when it hands a new
 * tracker to the monitor thread; until then pmix_psensor_base.evbase may
 * be NULL, meaning no monitor has ever started. Called only on the
 * library's progress thread, as start is. */
PMIX_EXPORT pmix_event_base_t *pmix_psensor_base_get_evbase(void);

/* Count one more monitor held by peer, unless it already holds
 * psensor_base_max_monitors_per_peer of them - then
 * PMIX_ERR_OUT_OF_RESOURCE. A component claims before it builds a
 * tracker, and the tracker releases the claim when it is destructed; the
 * tracker retains the peer, so the peer outlives its claims. Either may
 * be called from any thread. */
PMIX_EXPORT pmix_status_t pmix_psensor_base_claim(pmix_peer_t *peer);
PMIX_EXPORT void pmix_psensor_base_unclaim(pmix_peer_t *peer);

END_C_DECLS
#endif
