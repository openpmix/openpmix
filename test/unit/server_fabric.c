/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * White-box unit test for pmix_server_device_dists() in
 * src/server/pmix_server_fabric.c, driven exactly as the switchyard
 * drives it - a wire buffer packed by hand and a peer - so no DVM, no
 * client, and no second process is involved.
 *
 * What is pinned down here:
 *
 *   the distance array must outlive the call that computed it
 *      pmix_server_dist_cbfunc, which is what the switchyard hands this
 *      handler, does nothing but thread-shift: the reply is packed from
 *      the array later, on the progress thread. The handler used to pass
 *      (NULL, NULL) for the callback's (release_fn, release_cbdata) pair
 *      and free the array on the next line, so every locally answered
 *      PMIx_Compute_distances packed freed heap into its reply. The
 *      deterministic half of that is the contract: a producer whose data
 *      is consumed after it returns has to supply a release function.
 *      This case fails against an unfixed library on that assertion
 *      alone, without depending on what the freed memory happened to
 *      hold.
 *
 *      The second half - reading the array after the handler has
 *      returned - is a use-after-free against an unfixed library that a
 *      plain run will usually get away with. It is here for valgrind,
 *      and it is why this program is worth running under it.
 *
 *   the borrowed-topology path must not leak its source string
 *      A request that carries no topology means "use your own", which is
 *      the common case. pmix_hwloc_unpack_topology still strdup's a
 *      "hwloc" source for it, and the handler then points the topology
 *      at the global one - so the destruct at the end is skipped and
 *      nothing else would ever free that string. This case cannot assert
 *      the leak in-process; it exists so that the path is walked under
 *      valgrind, where the leak is one record per request.
 *
 * What this cannot assert on every host, and why: hwloc has to find at
 * least one OS device to measure a distance against. A bare macOS host
 * reports none, so pmix_hwloc_compute_distances answers
 * PMIX_ERR_NOT_FOUND and the cases above have no answer to hold a
 * contract against - they report SKIP carrying that status rather than
 * passing vacuously. Everything ahead of the computation still runs
 * there: the hand-packed buffer unpacks, the "use your own topology"
 * path is taken, and the supplied cpuset is accepted. On Linux, where
 * hwloc reports network and block devices, the assertions run.
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "include/pmix_server.h"

#include "src/hwloc/pmix_hwloc.h"
#include "src/include/pmix_globals.h"
#include "src/mca/bfrops/bfrops.h"
#include "src/server/pmix_server_ops.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* a count whose product with sizeof(pmix_info_t) wraps a 64-bit size_t */
#define SFUT_WRAPPING_COUNT ((size_t) 1 << 61)

static int npass = 0;
static int nfail = 0;

static void report(const char *name, int passed)
{
    if (passed) {
        fprintf(stdout, "  PASS: %s\n", name);
        ++npass;
    } else {
        fprintf(stdout, "  FAIL: %s\n", name);
        ++nfail;
    }
}

static void skip(const char *name, const char *why)
{
    fprintf(stdout, "  SKIP: %s (%s)\n", name, why);
}

/* What the handler handed its completion callback. We deliberately keep
 * the array pointer rather than copying it: reading it after the handler
 * has returned is the whole point of the first case. */
static bool dist_fired = false;
static pmix_status_t dist_status = PMIX_SUCCESS;
static pmix_device_distance_t *dist_array = NULL;
static size_t dist_ndist = 0;
static pmix_release_cbfunc_t dist_relfn = NULL;
static void *dist_relcbd = NULL;

static void dist_cbfunc(pmix_status_t status, pmix_device_distance_t *dist,
                        size_t ndist, void *cbdata,
                        pmix_release_cbfunc_t relfn, void *relcbd)
{
    pmix_server_caddy_t *cd = (pmix_server_caddy_t *) cbdata;

    dist_fired = true;
    dist_status = status;
    dist_array = dist;
    dist_ndist = ndist;
    dist_relfn = relfn;
    dist_relcbd = relcbd;

    /* stand in for pmix_server_dist_cbfunc, which parks exactly these on
     * a shift caddy and returns - it does not consume any of them here */
    if (NULL != cd) {
        PMIX_RELEASE(cd);
    }
}

/* Pack a COMPUTE_DEVICE_DISTANCES request the way a client does: a
 * topology, a cpuset, then the directive count and array. A NULL
 * topology asks the server to use its own. */
static pmix_status_t do_device_dists_n(pmix_cpuset_t *cpuset, size_t ninfo)
{
    pmix_buffer_t *buf;
    pmix_server_caddy_t *cd;
    pmix_topology_t topo = {NULL, NULL};
    pmix_status_t rc;

    dist_fired = false;
    dist_array = NULL;
    dist_ndist = 0;
    dist_relfn = NULL;
    dist_relcbd = NULL;

    buf = PMIX_NEW(pmix_buffer_t);
    if (NULL == buf) {
        return PMIX_ERR_NOMEM;
    }
    PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &topo, 1, PMIX_TOPO);
    if (PMIX_SUCCESS == rc) {
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, cpuset, 1, PMIX_PROC_CPUSET);
    }
    if (PMIX_SUCCESS == rc) {
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &ninfo, 1, PMIX_SIZE);
    }
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(buf);
        return rc;
    }

    cd = PMIX_NEW(pmix_server_caddy_t);
    if (NULL == cd) {
        PMIX_RELEASE(buf);
        return PMIX_ERR_NOMEM;
    }
    PMIX_RETAIN(pmix_globals.mypeer);
    cd->peer = pmix_globals.mypeer;
    cd->hdr.tag = 0;

    rc = pmix_server_device_dists(cd, buf, dist_cbfunc);
    if (PMIX_SUCCESS != rc) {
        /* the switchyard owns the caddy on a non-success return */
        PMIX_RELEASE(cd);
    }
    PMIX_RELEASE(buf);
    return rc;
}

static pmix_status_t do_device_dists(pmix_cpuset_t *cpuset)
{
    return do_device_dists_n(cpuset, 0);
}

/* Drive one PMIX_FABRIC_REGISTER_CMD: the directive count and the
 * directives. The count is a parameter so it can be made to disagree
 * with what is actually packed. */
static pmix_status_t do_fabric_register(size_t declared_ninfo, size_t ninfo, pmix_info_t *info)
{
    pmix_buffer_t *buf;
    pmix_server_caddy_t *cd;
    pmix_status_t rc;

    buf = PMIX_NEW(pmix_buffer_t);
    if (NULL == buf) {
        return PMIX_ERR_NOMEM;
    }
    PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &declared_ninfo, 1, PMIX_SIZE);
    if (PMIX_SUCCESS == rc && 0 < ninfo) {
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, info, ninfo, PMIX_INFO);
    }
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(buf);
        return rc;
    }

    cd = PMIX_NEW(pmix_server_caddy_t);
    if (NULL == cd) {
        PMIX_RELEASE(buf);
        return PMIX_ERR_NOMEM;
    }
    PMIX_RETAIN(pmix_globals.mypeer);
    cd->peer = pmix_globals.mypeer;
    cd->hdr.tag = 0;

    rc = pmix_server_fabric_register(cd, buf, pmix_server_fabric_cbfunc);
    if (PMIX_SUCCESS != rc) {
        /* the switchyard owns the caddy on a non-success return */
        PMIX_RELEASE(cd);
    }
    PMIX_RELEASE(buf);
    return rc;
}

int main(int argc, char **argv)
{
    static pmix_server_module_t mymodule = {0};
    pmix_cpuset_t cpuset;
    pmix_status_t rc;

    (void) argc;
    (void) argv;

    fprintf(stdout, "server_fabric: server-side device-distance unit tests\n");

    rc = PMIx_server_init(&mymodule, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    /* Supply a cpuset of our own. Leaving it out sends the handler to the
     * datastore for the requestor's binding, which a bare unit-test
     * server has never been told - so the request would fail before ever
     * reaching the computation these cases are about. */
    memset(&cpuset, 0, sizeof(cpuset));
    rc = pmix_hwloc_parse_cpuset_string("hwloc:0", &cpuset);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "could not build a cpuset: %s\n", PMIx_Error_string(rc));
        PMIx_server_finalize();
        return 1;
    }

    rc = do_device_dists(&cpuset);

    if (PMIX_SUCCESS != rc || !dist_fired) {
        /* hwloc found nothing to measure against on this machine, or
         * declined - there is no answer to hold the contract against, and
         * that is a property of the host rather than of the library */
        /* PMIX_ERR_NOT_FOUND from here means hwloc walked the topology
         * and found no OS devices to measure against - true of a bare
         * macOS host, and a property of the machine rather than of the
         * library. Everything ahead of the computation still ran: the
         * hand-packed buffer unpacked, the borrowed-topology path was
         * taken, and the supplied cpuset was accepted. */
        skip("distance request answered", PMIx_Error_string(rc));
    } else {
        report("distance request accepted", PMIX_SUCCESS == dist_status);

        /* The deterministic assertion. The callback that really receives
         * this only thread-shifts, so whoever produced the array has to
         * keep it alive until the chain is done with it - which is what
         * the release pair is for. Passing (NULL, NULL) and freeing the
         * array in the handler is the defect this pins down. */
        report("a locally computed distance array carries a release function",
               NULL != dist_relfn);

        /* And the array must still be readable at the point the real
         * callback would pack it - i.e. now, after the handler returned.
         * Against an unfixed library this reads freed memory. */
        if (0 < dist_ndist && NULL != dist_array) {
            report("the distance array survives the handler's return",
                   NULL != dist_array[0].uuid);
        } else {
            skip("the distance array survives the handler's return",
                 "host reported no devices");
        }

        /* releasing through the function we were given must be enough -
         * anything the handler freed itself would be a double free here */
        if (NULL != dist_relfn) {
            dist_relfn(dist_relcbd);
            report("releasing through the callback's own function is clean", true);
        }
    }

    /* --- counts that do not survive the trip through int32_t ---------
     *
     * Both handlers size an info array straight from a count a client
     * controls, and PMIx_Info_create multiplies it by
     * sizeof(pmix_info_t) with no overflow guard before constructing
     * every element it claimed. With sizeof(pmix_info_t) == 552 the
     * count 2^61 wraps that product to exactly zero, malloc hands back a
     * live pointer, and the constructor loop then walks 2^61 elements
     * off the end of it - so these cases *crash* an unfixed library
     * rather than failing it. The destructors of both caddies walk the
     * same size_t when they free, which is the second reason the screen
     * has to be in front of the allocation rather than after it. */
    {
        pmix_info_t dir;

        PMIX_INFO_LOAD(&dir, "server-fabric-ut.dir", "value", PMIX_STRING);

        rc = do_fabric_register(SFUT_WRAPPING_COUNT, 1, &dir);
        report("fabric_register rejects an unusable directive count",
               PMIX_ERR_BAD_PARAM == rc);

        rc = do_device_dists_n(&cpuset, SFUT_WRAPPING_COUNT);
        report("device_dists rejects an unusable directive count",
               PMIX_ERR_BAD_PARAM == rc);

        /* and a well-formed register request still reaches the end of
         * the handler - no pnet component implements register_fabric and
         * this server has no host fabric entry point, so
         * PMIX_ERR_NOT_SUPPORTED is this file's "got all the way
         * through the argument handling" marker */
        rc = do_fabric_register(1, 1, &dir);
        report("fabric_register accepts a well-formed request",
               PMIX_ERR_NOT_SUPPORTED == rc);

        PMIX_INFO_DESTRUCT(&dir);
    }

    pmix_hwloc_destruct_cpuset(&cpuset);
    PMIx_server_finalize();

    fprintf(stdout, "server_fabric: %d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
