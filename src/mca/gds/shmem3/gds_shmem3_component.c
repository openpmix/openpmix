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
 * Copyright (c) 2015      Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2016-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2022-2025 Nanook Consulting  All rights reserved.
 * Copyright (c) 2022-2023 Triad National Security, LLC. All rights reserved.
 * Copyright (c) 2026      Jeff Squyres  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * These symbols are in a file by themselves to provide nice linker
 * semantics.  Since linkers generally pull in symbols by object
 * files, keeping these symbols as the only symbols in this file
 * prevents utility programs such as "pmix_info" from having to import
 * entire components just to query their version and parameters.
 */

#include "gds_shmem3.h"

static int
gds_shmem3_component_register(void);

static int
component_query(pmix_mca_base_module_t **module,
                int *priority)
{
    // See if the required system file is present.
    // See pmix_vmem_find_hole() for more information.
    if (access("/proc/self/maps", F_OK) == -1) {
        *priority = 0;
        *module = NULL;
        return PMIX_ERROR;
    }
    *priority = PMIX_GDS_SHMEM3_DEFAULT_PRIORITY;
    *module = (pmix_mca_base_module_t *)&pmix_shmem3_module;
    return PMIX_SUCCESS;
}

/**
 * Instantiate the public struct with all of our public
 * information and pointers to our public functions in it.
 */
pmix_gds_shmem3_component_t pmix_mca_gds_shmem3_component = {
    .super = {
        PMIX_MCA_BASE_VERSION(gds),
        /** Component name and version. */
        .pmix_mca_component_name = PMIX_GDS_SHMEM3_NAME,
        PMIX_MCA_BASE_MAKE_VERSION(
            component,
            PMIX_MAJOR_VERSION,
            PMIX_MINOR_VERSION,
            PMIX_RELEASE_VERSION
        ),
        /** Component register. */
        .pmix_mca_register_component_params = gds_shmem3_component_register,
        /** Component query function. */
        .pmix_mca_query_component = component_query,
        .reserved = {0}
    },
    .jobs = PMIX_LIST_STATIC_INIT,
    .sessions = PMIX_LIST_STATIC_INIT
};

double pmix_gds_shmem3_segment_size_multiplier = 1.0;

bool pmix_gds_shmem3_force_client_attach_failure = false;

bool pmix_gds_shmem3_force_modex_attach_failure = false;

/* One gibibyte per slot. Nothing is committed, so the cost of being
 * generous is a virtual address range and a VMA; the cost of being
 * stingy is a modex that does not fit and has to be placed the old
 * way. */
size_t pmix_gds_shmem3_arena_slot_size = 1024UL * 1024UL * 1024UL;

/* Four generations live at once. One is the steady state; more than one
 * only arises where a fence contributed just what changed, and each such
 * fence adds one until a cumulative contribution collapses them. Four
 * covers the shapes seen in practice and costs nothing but address
 * space; past it, a generation is placed outside the arena. */
size_t pmix_gds_shmem3_arena_modex_slots = 4;

bool pmix_gds_shmem3_offset_placement = true;

static int
gds_shmem3_component_register(void)
{
    int varidx;

    varidx = pmix_mca_base_component_var_register(
        &pmix_mca_gds_shmem3_component.super,
        "segment_size_multiplier",
        "Multiplier that influences the ultimate sizes of the shared-memory "
        "segments used for gds data storage. As a percentage, values less or "
        "greater than 1.0 decrease or increase the final segment sizes, "
        "respectively.",
        PMIX_MCA_BASE_VAR_TYPE_DOUBLE,
        &pmix_gds_shmem3_segment_size_multiplier
    );
    if (varidx < 0) {
        return PMIX_ERROR;
    }

    varidx = pmix_mca_base_component_var_register(
        &pmix_mca_gds_shmem3_component.super,
        "force_client_attach_failure",
        "(Testing only) Force a client's fixed-address segment attach to "
        "fail so the graceful fallback to the next GDS module can be "
        "exercised. Do not set this in production.",
        PMIX_MCA_BASE_VAR_TYPE_BOOL,
        &pmix_gds_shmem3_force_client_attach_failure
    );
    if (varidx < 0) {
        return PMIX_ERROR;
    }

    varidx = pmix_mca_base_component_var_register(
        &pmix_mca_gds_shmem3_component.super,
        "force_modex_attach_failure",
        "(Testing only) Force a client's attach of the MODEX segment to "
        "fail, leaving its job and session attaches alone. Unlike "
        "force_client_attach_failure, this reaches the fence-time attach, "
        "because the client still completes PMIx_Init on shmem3. Do not "
        "set this in production.",
        PMIX_MCA_BASE_VAR_TYPE_BOOL,
        &pmix_gds_shmem3_force_modex_attach_failure
    );
    if (varidx < 0) {
        return PMIX_ERROR;
    }

    varidx = pmix_mca_base_component_var_register(
        &pmix_mca_gds_shmem3_component.super,
        "arena_slot_size",
        "Bytes of address space reserved for each modex slot in a job's "
        "arena. The reservation holds the addresses a client will later "
        "need to map at; nothing is committed, so this costs virtual "
        "address space only. Set to 0 to disable the arena and place "
        "every segment independently.",
        PMIX_MCA_BASE_VAR_TYPE_SIZE_T,
        &pmix_gds_shmem3_arena_slot_size
    );
    if (varidx < 0) {
        return PMIX_ERROR;
    }

    varidx = pmix_mca_base_component_var_register(
        &pmix_mca_gds_shmem3_component.super,
        "arena_modex_slots",
        "How many modex generations a job's arena can hold at once. More "
        "than one is live whenever a fence contributed only what changed, "
        "since the generations before such a one are still the only copy "
        "of what it did not repeat. A generation that arrives with every "
        "slot taken is placed outside the arena. Capped at 32.",
        PMIX_MCA_BASE_VAR_TYPE_SIZE_T,
        &pmix_gds_shmem3_arena_modex_slots
    );
    if (varidx < 0) {
        return PMIX_ERROR;
    }

    varidx = pmix_mca_base_component_var_register(
        &pmix_mca_gds_shmem3_component.super,
        "offset_placement",
        "Place shared-memory segments a quarter of the way into the biggest "
        "hole in the address space rather than at its midpoint. The midpoint "
        "is where hwloc, Open MPI and this component's own former default "
        "all aim, so it is the address most likely to be taken already in "
        "some other process that has to map here too.",
        PMIX_MCA_BASE_VAR_TYPE_BOOL,
        &pmix_gds_shmem3_offset_placement
    );
    if (varidx < 0) {
        return PMIX_ERROR;
    }
    return PMIX_SUCCESS;
}
PMIX_MCA_BASE_COMPONENT_INIT(pmix, gds, shmem3)

/*
 * vim: ft=cpp ts=4 sts=4 sw=4 expandtab
 */
