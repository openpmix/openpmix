/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
/**
 * @file
 *
 * Internal entry points for job control (PMIx_Job_control).
 */

#ifndef PMIX_CONTROL_H
#define PMIX_CONTROL_H

#include "src/include/pmix_config.h"

#include "pmix_common.h"

BEGIN_C_DECLS

/* Relay a job-control request to the server we are connected to.
 *
 * This is the client half of PMIx_Job_control_nb, factored out so that
 * library code can make the same request without calling the public
 * entry point - which the top-level threading rules forbid, because a
 * public API may thread-shift and back-end code can already be running
 * on the progress thread.
 *
 * The directives are packed before this returns, so a caller may
 * release them as soon as it does; the send itself is thread-shifted.
 * A NULL cbfunc means the caller does not want the answer.
 *
 * Returns PMIX_ERR_INIT until the library has finished initializing.
 * That is not a formality: the connection to our server exists well
 * before init completes, and a request sent in that window lands in the
 * middle of the init exchange.
 */
PMIX_EXPORT pmix_status_t pmix_job_control_relay(const pmix_proc_t targets[], size_t ntargets,
                                                 const pmix_info_t directives[], size_t ndirs,
                                                 pmix_info_cbfunc_t cbfunc, void *cbdata);

END_C_DECLS

#endif /* PMIX_CONTROL_H */
