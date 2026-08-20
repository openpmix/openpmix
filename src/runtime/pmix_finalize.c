/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2008-2015 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2010-2015 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2016-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2021-2023 Triad National Security, LLC. All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/** @file **/

#include "src/include/pmix_config.h"

#include "src/class/pmix_object.h"
#include "src/client/pmix_client_ops.h"
#include "src/common/pmix_attributes.h"
#include "src/common/pmix_iof.h"
#include "src/hwloc/pmix_hwloc.h"
#include "src/mca/base/pmix_base.h"
#include "src/mca/base/pmix_mca_base_var.h"
#include "src/mca/bfrops/base/base.h"
#include "src/mca/gds/base/base.h"
#include "src/mca/pcompress/base/base.h"
#include "src/mca/pif/base/base.h"
#include "src/mca/pinstalldirs/base/base.h"
#include "src/mca/plog/base/base.h"
#include "src/mca/pnet/base/base.h"
#include "src/mca/preg/base/base.h"
#include "src/mca/psec/base/base.h"
#include "src/mca/ptl/base/base.h"
#include "src/threads/pmix_tsd.h"
#include "src/util/pmix_keyval_parse.h"
#include "src/util/pmix_name_fns.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_show_help.h"
#include "src/util/pmix_net.h"
#include "src/runtime/pmix_init_util.h"
#include <event.h>

#include "src/runtime/pmix_progress_threads.h"
#include "src/runtime/pmix_rte.h"

extern int pmix_initialized;
extern bool pmix_init_called;

/* Release an output channel opened by pmix_rte_init and restore the -1
 * sentinel it started life with.
 *
 * This has to happen, and has to happen before pmix_output_finalize:
 * pmix_output_close is a no-op once the output system is down, and the
 * descriptor owns a strdup'ed prefix that nothing else frees. The next
 * pmix_output_init marks every slot unused again without clearing what
 * the slot pointed at, so an unclosed channel loses its prefix on every
 * init/finalize cycle. Restoring -1 matters just as much: the id is
 * cached in a file-scope struct that outlives the library, and after a
 * finalize it names a slot the next cycle may hand to someone else.
 *
 * This mirrors what framework_close_output does for the per-framework
 * channels in src/mca/base/pmix_mca_base_framework.c. */
static void close_output(int *id)
{
    if (0 <= *id) {
        pmix_output_close(*id);
        *id = -1;
    }
}

void pmix_rte_finalize(void)
{
    int i;
    pmix_notify_caddy_t *cd;
    pmix_iof_req_t *req;

    if (!pmix_init_called) {
        return;
    }

    /* stop the main progress thread - does not release
     * the event base so we don't break the components */
    (void) pmix_progress_thread_pause(NULL);

    /* release the attribute support trackers */
    pmix_release_registered_attrs();

    /* close plog */
    (void) pmix_mca_base_framework_close(&pmix_plog_base_framework);

    /* close preg */
    (void) pmix_mca_base_framework_close(&pmix_preg_base_framework);

    /* cleanup communications */
    (void) pmix_mca_base_framework_close(&pmix_ptl_base_framework);

    /* close the security framework */
    (void) pmix_mca_base_framework_close(&pmix_psec_base_framework);

    /* close bfrops */
    (void) pmix_mca_base_framework_close(&pmix_bfrops_base_framework);

    /* close compress */
    (void) pmix_mca_base_framework_close(&pmix_pcompress_base_framework);

    /* close GDS */
    (void) pmix_mca_base_framework_close(&pmix_gds_base_framework);

    /* Finalize the network helper subsystem. */
    (void)pmix_net_finalize();

    /* finalize the mca */
    /* Clear out all the registered MCA params */
    pmix_deregister_params();
    pmix_mca_base_var_finalize();

    /* keyval lex-based parser */
    pmix_util_keyval_parse_finalize();

    (void) pmix_mca_base_framework_close(&pmix_pinstalldirs_base_framework);
    (void) pmix_mca_base_framework_close(&pmix_pif_base_framework);
    (void) pmix_mca_base_close();

    /* finalize the show_help system */
    pmix_show_help_finalize();

    /* release the verbosity channels pmix_rte_init opened - see
     * close_output above for why this cannot wait until after
     * pmix_output_finalize. Roles that open channels of their own
     * (pmix_client.c, pmix_server.c) release those themselves. */
    close_output(&pmix_client_globals.get_output);
    close_output(&pmix_client_globals.connect_output);
    close_output(&pmix_client_globals.fence_output);
    close_output(&pmix_client_globals.pub_output);
    close_output(&pmix_client_globals.spawn_output);
    close_output(&pmix_client_globals.event_output);
    close_output(&pmix_client_globals.iof_output);
    close_output(&pmix_client_globals.group_output);
    close_output(&pmix_globals.debug_output);

    /* finalize the output system.  This has to come *after* the
       malloc code, as the malloc code needs to call into this, but
       the malloc code turning off doesn't affect pmix_output that
       much */
    pmix_output_finalize();

    /* clean out the globals */

    /* mypeer carries TWO references by the time we get here: the one
     * pmix_rte_init created it with, and one the role took when it
     * pointed pmix_client_globals.myserver at the same object
     * (pmix_server.c, pmix_client.c, pmix_tool.c). This drops the first;
     * the role drops the second immediately after we return, which is
     * what actually frees it.
     *
     * So do NOT NULL the pointer here, however much the re-entrancy
     * rules for this file otherwise want it. Every role guards its
     * release with "if (NULL != pmix_globals.mypeer)", so clearing it
     * silently cancels the release that does the freeing and leaks the
     * peer - and its namespace - on every init/finalize cycle. The
     * pointer is not dangling at this point; the object is still alive
     * on the role's reference, and the role NULLs it once it is gone. */
    PMIX_RELEASE(pmix_globals.mypeer);
    PMIX_DESTRUCT(&pmix_globals.events);
    PMIX_LIST_DESTRUCT(&pmix_globals.cached_events);
    /* clear any notifications */
    for (i = 0; i < pmix_globals.max_events; i++) {
        pmix_hotel_checkout_and_return_occupant(&pmix_globals.notifications, i, (void **) &cd);
        if (NULL != cd) {
            PMIX_RELEASE(cd);
        }
    }
    PMIX_DESTRUCT(&pmix_globals.notifications);
    /* the stdin read event and SIGCONT handler in src/common/pmix_iof.c
     * are process-wide and outlive every request, so nothing else gives
     * them back. This has to happen while the event base is still up -
     * pmix_progress_thread_stop below is what frees it. */
    pmix_iof_finalize();
    for (i = 0; i < pmix_globals.iof_requests.size; i++) {
        req = (pmix_iof_req_t *) pmix_pointer_array_get_item(&pmix_globals.iof_requests, i);
        if (NULL != req) {
            PMIX_RELEASE(req);
        }
    }
    PMIX_DESTRUCT(&pmix_globals.iof_requests);
    PMIX_LIST_DESTRUCT(&pmix_globals.stdin_targets);
    if (NULL != pmix_globals.hostname) {
        free(pmix_globals.hostname);
        pmix_globals.hostname = NULL;
    }
    if (NULL != pmix_globals.aliases) {
        PMIx_Argv_free(pmix_globals.aliases);
        pmix_globals.aliases = NULL;
    }
    /* pmix_rte_init took ownership of these two when it copied the
     * directive-scan flags into pmix_globals; pmix_iof_check_flags
     * strdup'ed them out of PMIX_IOF_OUTPUT_TO_FILE /
     * PMIX_IOF_OUTPUT_TO_DIRECTORY. Only the namespace-level copies of
     * pmix_iof_flags_t are freed by a destructor - this one is a bare
     * struct member, so it is ours to release. */
    if (NULL != pmix_globals.iof_flags.file) {
        free(pmix_globals.iof_flags.file);
        pmix_globals.iof_flags.file = NULL;
    }
    if (NULL != pmix_globals.iof_flags.directory) {
        free(pmix_globals.iof_flags.directory);
        pmix_globals.iof_flags.directory = NULL;
    }
    PMIX_LIST_DESTRUCT(&pmix_globals.nspaces);
    PMIX_LIST_DESTRUCT(&pmix_client_globals.groups);
    PMIX_DESTRUCT(&pmix_globals.keyindex);
    free(pmix_globals.myidval.data.proc);
    pmix_globals.myidval.data.proc = NULL;

    // release the topology
    pmix_hwloc_finalize();

    pmix_finalize_util();
    // release the event base
    pmix_progress_thread_stop(NULL);
    /* the event base has now been freed - clear our cached pointers so a
     * subsequent PMIx_Init starts from a clean slate and nothing in the
     * gap dereferences freed memory. evauxbase either aliased evbase or
     * was supplied externally by the caller (who owns it); in both cases
     * we only drop our reference, we do not free it here */
    pmix_globals.evbase = NULL;
    pmix_globals.evauxbase = NULL;

    /* Only now, with the progress thread joined, is this thread the only
     * one left - which is what deleting the TSD keys requires. These used
     * to run further up, while the engine was still dispatching, and both
     * halves of that were wrong: the progress thread could reach a key
     * this thread had already deleted, and anything below here that
     * printed a process name re-created the print-buffer key into a
     * registry that had just been emptied, so that key - and its
     * PTHREAD_KEYS_MAX-limited slot - leaked for the rest of the process.
     * Leaking the slot is the exact failure pmix_tsd_keys_destruct exists
     * to prevent. */
    pmix_tsd_keys_destruct();
    /* clear the print-buffer TSD latch now that its key has been deleted,
     * so a subsequent PMIx_Init recreates it */
    pmix_name_fns_finalize();

    /* Put the scalar bootstrap state back to the values pmix_globals was
     * statically initialized with. These are all set from directives the
     * host passes to init, or discovered during it, and none of them is
     * re-initialized on the way in - pmix_rte_init only writes them when
     * the corresponding directive is present. So whatever cycle N was
     * told silently becomes cycle N+1's default, which is wrong in ways
     * that are hard to trace back here:
     *
     *   external_progress - the worst of them. Left set, the next
     *     pmix_progress_thread_start returns without spinning the engine
     *     because it believes the host is driving the loop. If that
     *     cycle's host is not, nothing progresses and the first blocking
     *     call hangs.
     *   external_topology - pmix_hwloc_finalize (just above) declines to
     *     destroy a topology it does not own. Left set, the next cycle's
     *     self-discovered topology is never destroyed either.
     *   nodeid / sessionid - UINT32_MAX is the "not told" sentinel, so a
     *     stale value is indistinguishable from a real one.
     *
     * This must come after the progress-thread teardown above, which
     * reads external_progress. */
    pmix_globals.nodeid = UINT32_MAX;
    pmix_globals.sessionid = UINT32_MAX;
    pmix_globals.appnum = 0;
    pmix_globals.pindex = 0;
    pmix_globals.external_progress = false;
    pmix_globals.external_topology = false;
    pmix_globals.pushstdin = false;
    /* pmix_iof_finalize above drained the pending-output cache and
     * destructed its list; put the accounting that goes with it back to
     * the static-init values so the next cycle does not start out
     * believing it is already holding bytes for a spawn that is gone */
    atomic_store(&pmix_globals.spawns_in_flight, 0);
    pmix_globals.iof_pending_bytes = 0;
    /* the file/directory strings were freed above; this clears the
     * formatting decisions that came with them */
    pmix_iof_init_flags(&pmix_globals.iof_flags);
    pmix_iof_init_flags(&pmix_globals.spawn_iof_flags);
    PMIX_LOAD_PROCID(&pmix_globals.myid, NULL, PMIX_RANK_INVALID);
}
