/*
 * Copyright (c) 2010      Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2012      Los Alamos National Security, Inc. All rights reserved.
 * Copyright (c) 2017-2020 Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "src/include/pmix_config.h"
#include "include/pmix_common.h"

#include "src/mca/psensor/base/base.h"
#include "src/mca/psensor/hpesmf/psensor_hpe.h"

/*
 * Local functions
 */

static int hpesmf_open(void);
static int hpesmf_close(void);
static int hpesmf_query(pmix_mca_base_module_t **module, int *priority);

pmix_psensor_hpesmf_component_t mca_psensor_hpesmf_component = {
    .super = {
        .base = {
            PMIX_PSENSOR_BASE_VERSION_1_0_0,

            /* Component name and version */
            .pmix_mca_component_name = "hpesmf",
            PMIX_MCA_BASE_MAKE_VERSION(component,
                                       PMIX_MAJOR_VERSION,
                                       PMIX_MINOR_VERSION,
                                       PMIX_RELEASE_VERSION),

            /* Component open and close functions */
            hpesmf_open,  /* component open  */
            hpesmf_close, /* component close */
            hpesmf_query  /* component query */
        }
    }
};


/**
  * component open/close/init function
  */
static int hpesmf_open(void)
{
    return PMIX_SUCCESS;
}


static int hpesmf_query(pmix_mca_base_module_t **module, int *priority)
{
    *priority = 5;  // irrelevant
    *module = (pmix_mca_base_module_t *)&pmix_psensor_hpesmf_module;
    return PMIX_SUCCESS;
}

/**
 *  Close all subsystems.
 */

static int hpesmf_close(void)
{
    return PMIX_SUCCESS;
}
