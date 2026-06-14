/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2006-2013 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2009-2012 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2016      IBM Corporation.  All rights reserved.
 * Copyright (c) 2023-2024 Triad National Security, LLC. All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * Copyright (c) 2026      Jeff Squyres  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

/*
 * Unit tests for the GDS per-peer fallback infrastructure:
 *
 *   - pmix_gds_base_get_fallback_module(): returns the highest-priority
 *     active module other than the failing one (no module names are
 *     hard-coded), or NULL when none remains.
 *   - PMIX_GDS_PEER_MODULE(): resolves a peer's GDS module, preferring a
 *     per-peer override (peer->gds) over the nspace-level default
 *     (peer->nptr->compat.gds).
 *
 * These exercise the framework primitives that let a single client fall
 * back to another GDS module at runtime. The full two-process,
 * attach-failure-driven fallback path (a shmem2 client that cannot map a
 * fixed-address segment and re-requests its job data via hash) depends on
 * divergent per-process virtual-memory layouts and is an integration
 * scenario, not something this unit test can reproduce.
 */

#include "src/include/pmix_config.h"
#include "include/pmix_server.h"
#include "src/include/pmix_globals.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/class/pmix_list.h"
#include "src/mca/gds/gds.h"
#include "src/mca/gds/base/base.h"

static int failures = 0;

static void check(const char *what, int ok)
{
    fprintf(stdout, "  %s: %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) {
        ++failures;
    }
}

static pmix_server_module_t mymodule = {0};

int main(int argc, char **argv)
{
    pmix_status_t rc;
    pmix_gds_base_module_t *top, *fb, *fb2;
    pmix_gds_base_active_module_t *active;
    size_t nactive = 0;
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    /* server_init brings up the GDS framework and populates the list of
     * active modules that the fallback helper selects from */
    if (PMIX_SUCCESS != (rc = PMIx_server_init(&mymodule, NULL, 0))) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    fprintf(stdout, "=== gds fallback unit tests ===\n\n");

    PMIX_LIST_FOREACH (active, &pmix_gds_globals.actives, pmix_gds_base_active_module_t) {
        ++nactive;
    }
    fprintf(stdout, "  (%zu active GDS modules)\n", nactive);

    top = pmix_gds_base_assign_module(NULL, 0);
    check("assign_module returns a module", NULL != top);

    check("get_fallback_module(NULL) is NULL",
          NULL == pmix_gds_base_get_fallback_module(NULL));

    /* Exercise the selection logic deterministically by injecting two
     * synthetic active modules. This is necessary because some platforms
     * activate only a single real GDS module -- e.g. macOS has no
     * /proc/self/maps, so shmem2's component_query fails and it never
     * activates -- which would otherwise leave the multi-module path
     * untested. The synthetic modules are given the two highest
     * priorities so the expected fallbacks are unambiguous regardless of
     * which real modules are present. They are removed again before
     * PMIx_server_finalize so the framework tears down cleanly. */
    {
        static pmix_gds_base_module_t fake_lo = {.name = "unit-fake-lo"};
        static pmix_gds_base_module_t fake_hi = {.name = "unit-fake-hi"};
        pmix_gds_base_active_module_t *a_lo, *a_hi;
        int maxpri = 0;

        PMIX_LIST_FOREACH (active, &pmix_gds_globals.actives, pmix_gds_base_active_module_t) {
            if (active->pri > maxpri) {
                maxpri = active->pri;
            }
        }
        a_lo = PMIX_NEW(pmix_gds_base_active_module_t);
        a_lo->module = &fake_lo;
        a_lo->pri = maxpri + 500;
        pmix_list_append(&pmix_gds_globals.actives, &a_lo->super);
        a_hi = PMIX_NEW(pmix_gds_base_active_module_t);
        a_hi->module = &fake_hi;
        a_hi->pri = maxpri + 1000;
        pmix_list_append(&pmix_gds_globals.actives, &a_hi->super);

        /* fake_hi is the global maximum, so it is the fallback for every
         * other module. */
        check("fallback for a non-top module is the global highest",
              &fake_hi == pmix_gds_base_get_fallback_module(&fake_lo));

        /* fake_lo is the second highest, so it is the fallback for the
         * top (fake_hi) module. */
        check("fallback for the top module is the next highest",
              &fake_lo == pmix_gds_base_get_fallback_module(&fake_hi));

        /* The helper must never return the module it was told is failing,
         * for any active module in the list. */
        {
            int ok = 1;
            PMIX_LIST_FOREACH (active, &pmix_gds_globals.actives,
                               pmix_gds_base_active_module_t) {
                fb = pmix_gds_base_get_fallback_module(active->module);
                if (NULL != fb && 0 == strcmp(fb->name, active->module->name)) {
                    ok = 0;
                }
            }
            check("get_fallback_module never returns the failing module", ok);
        }

        /* Chaining off a fallback must not loop back to it. */
        fb = pmix_gds_base_get_fallback_module(&fake_hi); // == fake_lo
        fb2 = pmix_gds_base_get_fallback_module(fb);
        check("chained fallback never returns its own failing module",
              NULL == fb2 || 0 != strcmp(fb2->name, fb->name));

        // restore the actives list before finalize
        pmix_list_remove_item(&pmix_gds_globals.actives, &a_hi->super);
        pmix_list_remove_item(&pmix_gds_globals.actives, &a_lo->super);
        PMIX_RELEASE(a_hi);
        PMIX_RELEASE(a_lo);
    }

    /* PMIX_GDS_PEER_MODULE prefers the per-peer override over the
     * nspace-level default. Build minimal stack objects and read only the
     * fields the macro touches (peer->gds, peer->nptr->compat.gds). */
    {
        /* Designated initializers (not plain declarations) so the const
         * is_tsafe member is initialized; a bare declaration trips
         * -Wdefault-const-init-field-unsafe under -Werror. */
        pmix_gds_base_module_t moda = {.name = "modA"};
        pmix_gds_base_module_t modb = {.name = "modB"};
        pmix_namespace_t ns;
        pmix_peer_t peer;

        memset(&ns, 0, sizeof(ns));
        memset(&peer, 0, sizeof(peer));
        ns.compat.gds = &moda;
        peer.nptr = &ns;

        peer.gds = NULL;
        check("PMIX_GDS_PEER_MODULE uses nspace default when peer->gds is NULL",
              &moda == PMIX_GDS_PEER_MODULE(&peer));

        peer.gds = &modb;
        check("PMIX_GDS_PEER_MODULE uses peer->gds override when set",
              &modb == PMIX_GDS_PEER_MODULE(&peer));
    }

    /* The server's PMIX_GDS_FALLBACK_CMD handler resolves the client-named
     * module with pmix_gds_base_assign_module() and rejects the request
     * (PMIX_ERR_NOT_SUPPORTED) unless the returned module's name matches.
     * Modules always offer themselves (a requested name only bumps that
     * module's priority), so an unknown name resolves to some default
     * module whose name does not match - confirm that, so the handler's
     * rejection branch is real rather than dead. */
    {
        pmix_info_t ginfo;
        pmix_gds_base_module_t *m;

        PMIX_INFO_LOAD(&ginfo, PMIX_GDS_MODULE, "no-such-gds-module", PMIX_STRING);
        m = pmix_gds_base_assign_module(&ginfo, 1);
        PMIX_INFO_DESTRUCT(&ginfo);
        check("an unknown module name does not resolve to that module",
              NULL == m || 0 != strcmp(m->name, "no-such-gds-module"));
    }

    fprintf(stdout, "\nResults: %d failed\n", failures);

    if (PMIX_SUCCESS != (rc = PMIx_server_finalize())) {
        fprintf(stderr, "PMIx_server_finalize failed: %s\n", PMIx_Error_string(rc));
    }

    return (0 == failures) ? 0 : 1;
}
