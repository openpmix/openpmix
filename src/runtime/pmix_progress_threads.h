/*
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015      Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2021-2022 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PMIX_PROGRESS_THREADS_H
#define PMIX_PROGRESS_THREADS_H

#include "pmix_config.h"

#include <pthread.h>
#include <event.h>

#include "src/include/pmix_types.h"

/**
 * Initialize a progress thread name; if a progress thread is not
 * already associated with that name, start a progress thread.
 *
 * If you have general events that need to run in *a* progress thread
 * (but not necessarily your own, dedicated progress thread), pass NULL
 * as the "name" argument to pmix_progress_thread_init() to glom on to
 * the general PMIX-wide progress thread.
 *
 * If a name is passed that was already used in a prior call to
 * pmix_progress_thread_init(), the event base associated with that
 * already-running progress thread will be returned (i.e., no new
 * progress thread will be started).
 */
PMIX_EXPORT pmix_event_base_t *pmix_progress_thread_init(const char *name);

PMIX_EXPORT pmix_status_t pmix_progress_thread_start(const char *name);

/**
 * Stop a progress thread name (reference counted).
 *
 * Once this function is invoked as many times as
 * pmix_progress_thread_init() was invoked on this name (or NULL), the
 * progress function is shut down.
 * it is destroyed.
 *
 * Will return PMIX_ERR_NOT_FOUND if the progress thread name does not
 * exist; PMIX_SUCCESS otherwise.
 */
PMIX_EXPORT pmix_status_t pmix_progress_thread_stop(const char *name);

/**
 * Temporarily pause the progress thread associated with this name.
 *
 * This function does not destroy the event base associated with this
 * progress thread name, but it does stop processing all events on
 * that event base until pmix_progress_thread_resume() is invoked on
 * that name.
 *
 * Will return PMIX_ERR_NOT_FOUND if the progress thread name does not
 * exist; PMIX_SUCCESS otherwise.
 */
PMIX_EXPORT pmix_status_t pmix_progress_thread_pause(const char *name);

/**
 * Restart a previously-paused progress thread associated with this
 * name.
 *
 * Will return PMIX_ERR_NOT_FOUND if the progress thread name does not
 * exist; PMIX_SUCCESS otherwise.
 */
PMIX_EXPORT pmix_status_t pmix_progress_thread_resume(const char *name);

/**
 * Is the calling thread the one currently running the shared progress
 * loop?
 *
 * This answers the question a blocking PMIx API has to ask before it
 * thread-shifts its work and waits for the result: am I about to post
 * that work to the very event loop I am standing in?  If so, waiting
 * for it can never succeed - the loop cannot service the new event
 * until the current one returns, and the current one is us.  See the
 * PMIX_CHECK_NOT_PROGRESS_THREAD comment in src/include/pmix_globals.h.
 *
 * True for the dedicated progress thread while it is inside the event
 * loop, and for whatever thread is inside PMIx_Progress() when the host
 * has taken over driving progress itself (PMIX_EXTERNAL_PROGRESS).
 * False everywhere else, including before the thread is started and
 * after it has been stopped.
 */
PMIX_EXPORT bool pmix_progress_thread_is_current(void);

/**
 * Guard the entry to a blocking PMIx API.
 *
 * Returns true - and reports the error to the user - when the caller is
 * on the progress thread, meaning the call it is about to make would
 * wait for an event loop that cannot run until the caller returns.  The
 * caller must then answer without waiting: PMIX_ERR_WOULD_BLOCK for an
 * API that reports a status, or by handing the work to the progress
 * thread asynchronously for one that does not.
 *
 * Note that this refuses nothing that used to work: every such call
 * deadlocks when made from the progress thread, so an error here can
 * only ever replace a hang.
 *
 * @param api  name of the API being guarded, for the diagnostic
 */
PMIX_EXPORT bool pmix_progress_thread_check_blocking(const char *api);

#endif
