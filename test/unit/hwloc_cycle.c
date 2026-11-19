/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * The topology cache across init/finalize cycles.
 *
 * Acquisition runs once per cycle, behind a latch that remembers what it
 * answered. The latch used to be local to pmix_hwloc_setup_topology, so it
 * outlived the cache it described: finalize emptied the cache, the next
 * PMIx_Init found the latch set and did not acquire again, and
 * PMIx_Load_topology reported success while handing back a NULL topology.
 *
 * A topology the host hands in with PMIX_TOPOLOGY2 is the host's, so
 * finalize does not destroy it - but it also left the pointer in the cache,
 * where the next cycle's acquisition would find it and treat it as current
 * whether or not the host still had it.
 *
 * Three cycles, so each one starts from what the one before it left:
 *
 *   1. self-discovered: there is a topology to load;
 *   2. host-provided: the host's topology is the one loaded, and finalize
 *      leaves it intact for the host to destroy;
 *   3. self-discovered again: the host's topology is not carried over.
 */

#include "src/include/pmix_config.h"
#include "include/pmix.h"
#include "include/pmix_server.h"

#include <hwloc.h>
#include <stdio.h>
#include <stdlib.h>

static pmix_server_module_t mymodule = {0};

static int failures = 0;
static int checks = 0;

static void ok(bool cond, const char *what)
{
    ++checks;
    if (cond) {
        fprintf(stdout, "  PASS: %s\n", what);
    } else {
        ++failures;
        fprintf(stderr, "  FAIL: %s\n", what);
    }
}

/* run one cycle, optionally handing the library a topology, and report
 * the topology PMIx_Load_topology hands back */
static void *cycle(int n, hwloc_topology_t hosttopo)
{
    pmix_topology_t topo = PMIX_TOPOLOGY_STATIC_INIT;
    pmix_topology_t provided = PMIX_TOPOLOGY_STATIC_INIT;
    pmix_info_t info;
    pmix_status_t rc;
    void *loaded = NULL;
    char what[256];

    if (NULL != hosttopo) {
        /* built by hand, as PRRTE does, so the library is handed the
         * host's own pointer rather than a copy - and never destructed,
         * since destructing it would destroy the host's topology */
        provided.source = "hwloc";
        provided.topology = hosttopo;
        PMIX_INFO_CONSTRUCT(&info);
        PMIX_LOAD_KEY(info.key, PMIX_TOPOLOGY2);
        info.value.type = PMIX_TOPO;
        info.value.data.topo = &provided;
        rc = PMIx_server_init(&mymodule, &info, 1);
    } else {
        rc = PMIx_server_init(&mymodule, NULL, 0);
    }
    snprintf(what, sizeof(what), "cycle %d: init", n);
    ok(PMIX_SUCCESS == rc, what);
    if (PMIX_SUCCESS != rc) {
        return NULL;
    }

    rc = PMIx_Load_topology(&topo);
    snprintf(what, sizeof(what), "cycle %d: load_topology hands back a topology", n);
    ok(PMIX_SUCCESS == rc && NULL != topo.topology, what);
    if (PMIX_SUCCESS == rc && NULL != topo.topology) {
        loaded = topo.topology;
        snprintf(what, sizeof(what), "cycle %d: the topology is a loaded one", n);
        ok(0 < hwloc_get_nbobjs_by_type((hwloc_topology_t) loaded, HWLOC_OBJ_PU), what);
    }

    rc = PMIx_server_finalize();
    snprintf(what, sizeof(what), "cycle %d: finalize", n);
    ok(PMIX_SUCCESS == rc, what);
    return loaded;
}

int main(int argc, char **argv)
{
    hwloc_topology_t hosttopo;
    void *loaded;

    (void) argc;
    (void) argv;

    cycle(1, NULL);

    if (0 != hwloc_topology_init(&hosttopo) ||
        0 != hwloc_topology_load(hosttopo)) {
        fprintf(stderr, "SKIP: could not load a host topology\n");
        return 77;
    }

    loaded = cycle(2, hosttopo);
    ok(loaded == (void *) hosttopo, "cycle 2: the host's topology is the one loaded");
    /* finalize must have left it alone - it is still ours */
    ok(0 < hwloc_get_nbobjs_by_type(hosttopo, HWLOC_OBJ_PU),
       "cycle 2: finalize leaves the host's topology intact");

    /* hosttopo is still alive here, so its address cannot be reused by
     * the next cycle's discovery and the comparison below means something */
    loaded = cycle(3, NULL);
    ok(NULL != loaded && loaded != (void *) hosttopo,
       "cycle 3: the previous host's topology is not carried over");

    hwloc_topology_destroy(hosttopo);

    fprintf(stderr, "%s: %d checks, %d failures\n",
            (0 == failures) ? "PASS" : "FAIL", checks, failures);
    return (0 == failures) ? 0 : 1;
}
