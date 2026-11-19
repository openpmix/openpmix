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
 * Destructing the handle PMIx_Load_topology filled in.
 *
 * PMIx_Load_topology lends the caller the library's cached topology - the
 * same hwloc object, not a copy - along with a copy of its source string,
 * which the caller owns. The caller is not expected to destruct the
 * topology, but applications have done so (MPICH's MPI_Finalize did), and
 * that was harmless until the library began destroying the cache at
 * finalize. After that, PMIX_TOPOLOGY_DESTRUCT on the lent handle meant the
 * hwloc object was destroyed twice - once by the caller, once by
 * pmix_hwloc_finalize - which hwloc reports with an assertion failure in
 * hwloc_components_fini.
 *
 * Destructing a lent handle must clear the handle and leave the topology
 * alone, whether it happens before PMIx_Finalize or after it. A topology
 * the caller really owns - a copy - is still destroyed.
 *
 * A regression here usually aborts rather than failing a check, so the
 * case progress lines are on stdout, unbuffered, to show how far it got.
 */

#include "src/include/pmix_config.h"
#include "pmix.h"
#include "pmix_server.h"

#include <hwloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static bool usable(void *topology)
{
    return (NULL != topology &&
            0 < hwloc_get_nbobjs_by_type((hwloc_topology_t) topology, HWLOC_OBJ_PU));
}

/* the cache is still there and still usable */
static void cache_intact(void *expected, const char *what)
{
    pmix_topology_t topo = PMIX_TOPOLOGY_STATIC_INIT;
    pmix_status_t rc;

    rc = PMIx_Load_topology(&topo);
    ok(PMIX_SUCCESS == rc && topo.topology == expected && usable(topo.topology), what);
    /* the source is a copy, and ours */
    free(topo.source);
}

int main(int argc, char **argv)
{
    pmix_topology_t topo = PMIX_TOPOLOGY_STATIC_INIT;
    pmix_topology_t late = PMIX_TOPOLOGY_STATIC_INIT;
    pmix_topology_t *copy = NULL;
    pmix_status_t rc;
    void *cached;

    (void) argc;
    (void) argv;
    setvbuf(stdout, NULL, _IONBF, 0);

    rc = PMIx_server_init(&mymodule, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    /* no source named: the library reports a copy of its own */
    rc = PMIx_Load_topology(&topo);
    ok(PMIX_SUCCESS == rc && usable(topo.topology), "load_topology lends a topology");
    if (PMIX_SUCCESS != rc || NULL == topo.topology) {
        PMIx_server_finalize();
        return 1;
    }
    cached = topo.topology;
    PMIx_Topology_destruct(&topo);
    ok(NULL == topo.topology && NULL == topo.source,
       "destructing a lent handle clears it");
    cache_intact(cached, "destructing a lent handle leaves the cache intact");

    /* and doing it again to the now-empty handle is harmless */
    PMIx_Topology_destruct(&topo);
    ok(NULL == topo.topology, "destructing it a second time is harmless");

    /* source named by the caller: that string is theirs, the topology is not */
    PMIX_TOPOLOGY_CONSTRUCT(&topo);
    topo.source = strdup("hwloc");
    rc = PMIx_Load_topology(&topo);
    ok(PMIX_SUCCESS == rc && topo.topology == cached,
       "load_topology with a source lends the same topology");
    PMIx_Topology_destruct(&topo);
    ok(NULL == topo.topology && NULL == topo.source,
       "destructing a lent handle with the caller's source clears it");
    cache_intact(cached, "... and leaves the cache intact");

    /* a copy is the caller's own, and is still destroyed - the pointer
     * check must not extend to it. That it really is destroyed shows
     * only as a leak, under valgrind. */
    PMIX_TOPOLOGY_CONSTRUCT(&topo);
    rc = PMIx_Load_topology(&topo);
    rc = PMIx_Data_copy((void **) &copy, &topo, PMIX_TOPO);
    ok(PMIX_SUCCESS == rc && NULL != copy && copy->topology != cached && usable(copy->topology),
       "a copy of the lent topology is a separate one");
    if (NULL != copy) {
        PMIx_Topology_free(copy, 1);
    }
    free(topo.source);
    cache_intact(cached, "freeing the copy leaves the cache intact");

    /* hold a lent handle across finalize, to be destructed after it */
    rc = PMIx_Load_topology(&late);
    ok(PMIX_SUCCESS == rc && late.topology == cached, "a handle to keep past finalize");

    /* the cache is destroyed in here - the second destroy used to be too */
    rc = PMIx_server_finalize();
    ok(PMIX_SUCCESS == rc, "finalize after the lent handles were destructed");

    PMIx_Topology_destruct(&late);
    ok(NULL == late.topology && NULL == late.source,
       "destructing a lent handle after finalize clears it");

    /* a second cycle starts from a fresh cache, and its lent handle is
     * recognized the same way */
    rc = PMIx_server_init(&mymodule, NULL, 0);
    ok(PMIX_SUCCESS == rc, "second cycle: init");
    if (PMIX_SUCCESS == rc) {
        PMIX_TOPOLOGY_CONSTRUCT(&topo);
        rc = PMIx_Load_topology(&topo);
        ok(PMIX_SUCCESS == rc && usable(topo.topology), "second cycle: load_topology");
        cached = topo.topology;
        PMIx_Topology_destruct(&topo);
        cache_intact(cached, "second cycle: destructing the lent handle leaves the cache");
        rc = PMIx_server_finalize();
        ok(PMIX_SUCCESS == rc, "second cycle: finalize");
    }

    fprintf(stderr, "%s: %d checks, %d failures\n",
            (0 == failures) ? "PASS" : "FAIL", checks, failures);
    return (0 == failures) ? 0 : 1;
}
