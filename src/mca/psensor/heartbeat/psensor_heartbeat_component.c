/*
 * Copyright (c) 2010      Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2012      Los Alamos National Security, Inc. All rights reserved.
 * Copyright (c) 2017-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "src/include/pmix_config.h"
#include "pmix_common.h"

#include "src/mca/psensor/base/base.h"
#include "src/mca/psensor/heartbeat/psensor_heartbeat.h"
#include "src/mca/ptl/base/base.h"
#include "src/mca/ptl/ptl.h"

/*
 * Local functions
 */

static int heartbeat_open(void);
static int heartbeat_close(void);
static int heartbeat_query(pmix_mca_base_module_t **module, int *priority);

pmix_psensor_heartbeat_component_t pmix_mca_psensor_heartbeat_component = {
    .super = {
          PMIX_MCA_BASE_VERSION(psensor),

          /* Component name and version */
          .pmix_mca_component_name = "heartbeat",
          PMIX_MCA_BASE_MAKE_VERSION(component, PMIX_MAJOR_VERSION, PMIX_MINOR_VERSION,
                                     PMIX_RELEASE_VERSION),

          /* Component open and close functions */
          heartbeat_open,  /* component open  */
          heartbeat_close, /* component close */
          heartbeat_query  /* component query */
    }
};
PMIX_MCA_BASE_COMPONENT_INIT(pmix, psensor, heartbeat)

/**
 * component open/close/init function
 */
static int heartbeat_open(void)
{
    PMIX_CONSTRUCT(&pmix_mca_psensor_heartbeat_component.trackers, pmix_list_t);

    return PMIX_SUCCESS;
}

static int heartbeat_query(pmix_mca_base_module_t **module, int *priority)
{
    *priority = 5; // irrelevant
    *module = (pmix_mca_base_module_t *) &pmix_psensor_heartbeat_module;
    return PMIX_SUCCESS;
}

/**
 *  Close all subsystems.
 */

static int heartbeat_close(void)
{
    pmix_ptl_posted_recv_t *rcv, *rnext;

    PMIX_LIST_DESTRUCT(&pmix_mca_psensor_heartbeat_component.trackers);

    /* Retire the beat recv the first heartbeat_start posted, and clear
     * the flag that says one is posted. Both halves matter, and neither
     * happens on its own:
     *
     * - the recv names pmix_psensor_heartbeat_recv_beats, which lives in
     *   this component. The ptl framework closes after we do, so between
     *   the two the list holds a callback pointing into a component that
     *   has been closed - and, in a DSO build, unloaded.
     * - recv_active is a field of a file-scope component struct, so it
     *   outlives the library. The ptl's own close frees the recv object
     *   but knows nothing about our flag, so a second PMIx_server_init
     *   in the same process would find recv_active still set and never
     *   re-post. No beat would ever be counted, and the first window
     *   would declare every monitored client dead. */
    PMIX_LIST_FOREACH_SAFE (rcv, rnext, &pmix_ptl_base.posted_recvs, pmix_ptl_posted_recv_t) {
        if (pmix_psensor_heartbeat_recv_beats == rcv->cbfunc) {
            pmix_list_remove_item(&pmix_ptl_base.posted_recvs, &rcv->super);
            PMIX_RELEASE(rcv);
        }
    }
    pmix_mca_psensor_heartbeat_component.recv_active = false;

    return PMIX_SUCCESS;
}
