/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/* Backing functions for deprecated APIs that no longer do anything.
 *
 * A deprecated API keeps its symbol for as long as binaries built
 * against it may still run, even when the library has nothing left
 * behind it. Those that have no remaining implementation live here,
 * reduced to the least that keeps the symbol: a function that returns
 * a status says PMIX_ERR_NOT_SUPPORTED, one that returns an object
 * returns NULL, and one that returns nothing does nothing. See
 * include/pmix_deprecated.h for what each replaced. */

#include "src/include/pmix_config.h"

#include "pmix.h"

/* v5 resource usage statistics - replaced by the arrays of pmix_info_t
 * that PMIx_Process_monitor returns. Nothing creates one of these
 * structures any more, so there is nothing to initialize or release. */

void PMIx_Proc_stats_construct(pmix_proc_stats_t *p __pmix_attribute_unused__)
{
}

void PMIx_Proc_stats_destruct(pmix_proc_stats_t *p __pmix_attribute_unused__)
{
}

pmix_proc_stats_t *PMIx_Proc_stats_create(size_t n __pmix_attribute_unused__)
{
    return NULL;
}

void PMIx_Proc_stats_free(pmix_proc_stats_t *p __pmix_attribute_unused__,
                          size_t n __pmix_attribute_unused__)
{
}

void PMIx_Disk_stats_construct(pmix_disk_stats_t *p __pmix_attribute_unused__)
{
}

void PMIx_Disk_stats_destruct(pmix_disk_stats_t *p __pmix_attribute_unused__)
{
}

pmix_disk_stats_t *PMIx_Disk_stats_create(size_t n __pmix_attribute_unused__)
{
    return NULL;
}

void PMIx_Disk_stats_free(pmix_disk_stats_t *p __pmix_attribute_unused__,
                          size_t n __pmix_attribute_unused__)
{
}

void PMIx_Net_stats_construct(pmix_net_stats_t *p __pmix_attribute_unused__)
{
}

void PMIx_Net_stats_destruct(pmix_net_stats_t *p __pmix_attribute_unused__)
{
}

pmix_net_stats_t *PMIx_Net_stats_create(size_t n __pmix_attribute_unused__)
{
    return NULL;
}

void PMIx_Net_stats_free(pmix_net_stats_t *p __pmix_attribute_unused__,
                         size_t n __pmix_attribute_unused__)
{
}

void PMIx_Node_stats_construct(pmix_node_stats_t *p __pmix_attribute_unused__)
{
}

void PMIx_Node_stats_destruct(pmix_node_stats_t *p __pmix_attribute_unused__)
{
}

pmix_node_stats_t *PMIx_Node_stats_create(size_t n __pmix_attribute_unused__)
{
    return NULL;
}

void PMIx_Node_stats_free(pmix_node_stats_t *p __pmix_attribute_unused__,
                          size_t n __pmix_attribute_unused__)
{
}
