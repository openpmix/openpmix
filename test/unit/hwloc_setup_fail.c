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
 * What happens after topology acquisition FAILS.
 *
 * Acquisition is allowed to fail without being fatal: a server that was not
 * asked to share its topology logs the failure and carries straight on, so
 * everything that runs afterwards - the pnet and pgpu components it opens
 * next, any PMIx_Get_cpuset or PMIx_Load_topology a caller makes, and
 * finalize - runs against whatever pmix_hwloc_setup_topology left behind.
 * That makes the failure path's postconditions load-bearing, and nothing
 * else in the suite reaches it: every other test acquires a topology
 * successfully.
 *
 * The failure is provoked with the pmix_hwloc_topo_file MCA parameter,
 * pointed at a topology that is well-formed XML - so hwloc's set_xml()
 * accepts it - but carries no machine, so hwloc's load() rejects it. That
 * is the one shape that reaches the destroy-and-return path rather than
 * failing earlier, and it needs a separate binary from the other hwloc
 * tests because acquisition runs exactly once per process.
 *
 * Two things are asserted, and they used to be two bugs:
 *
 *   - the cache is left empty. hwloc_topology_destroy() frees the object,
 *     and the cached pointer was not cleared with it, so the server walked
 *     a freed topology and pmix_hwloc_finalize() destroyed it a second
 *     time. The second destroy is why this test calls
 *     PMIx_server_finalize() rather than just exiting.
 *
 *   - a later caller is told the truth. The one-shot latch recorded that
 *     acquisition had RUN, not what it answered, so every subsequent call
 *     returned PMIX_SUCCESS - and PMIx_Load_topology reported success while
 *     handing back a NULL topology.
 */

#include "src/include/pmix_config.h"
#include "include/pmix.h"
#include "include/pmix_server.h"
#include "src/include/pmix_globals.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

int main(int argc, char **argv)
{
    char path[1024];
    pmix_topology_t topo = PMIX_TOPOLOGY_STATIC_INIT;
    pmix_cpuset_t cpuset = PMIX_CPUSET_STATIC_INIT;
    pmix_status_t rc;
    FILE *fp;

    (void) argc;
    (void) argv;

    /* Written here rather than shipped in test/topologies because it is not
     * a topology: it is the smallest document hwloc will parse and then
     * refuse to load. */
    snprintf(path, sizeof(path), "pmix_unloadable_topo_%lu.xml",
             (unsigned long) getpid());
    fp = fopen(path, "w");
    if (NULL == fp) {
        fprintf(stderr, "SKIP: could not write %s\n", path);
        return 77;
    }
    fprintf(fp, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    fprintf(fp, "<!DOCTYPE topology SYSTEM \"hwloc2.dtd\">\n");
    fprintf(fp, "<topology version=\"2.0\">\n</topology>\n");
    fclose(fp);

    setenv("PMIX_MCA_pmix_hwloc_topo_file", path, 1);

    /* init is expected to SUCCEED: we did not ask it to share the topology,
     * and a server that cannot get one is still a server */
    rc = PMIx_server_init(&mymodule, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        unlink(path);
        return 1;
    }

    ok(NULL == pmix_globals.topology.topology,
       "a failed acquisition leaves no topology behind");

    rc = PMIx_Load_topology(&topo);
    ok(PMIX_SUCCESS != rc || NULL != topo.topology,
       "load_topology does not report success with nothing to hand back");

    /* PMIx_Get_cpuset hands the cached topology straight to hwloc, which
     * dereferences it. It is not only a failed acquisition that gets here:
     * a tool that is neither a launcher nor a scheduler never runs
     * acquisition at all, so an empty cache is its NORMAL state and this
     * API is not restricted by role. */
    rc = PMIx_Get_cpuset(&cpuset, PMIX_CPUBIND_PROCESS);
    ok(PMIX_SUCCESS != rc,
       "get_cpuset declines when there is no topology to read it from");

    /* the second destroy of the freed topology used to happen in here */
    rc = PMIx_server_finalize();
    ok(PMIX_SUCCESS == rc, "finalize completes");

    unlink(path);

    fprintf(stderr, "%s: %d checks, %d failures\n",
            (0 == failures) ? "PASS" : "FAIL", checks, failures);
    return (0 == failures) ? 0 : 1;
}
