/*
 * Copyright (c) 2007-2013 Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2008      Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2015-2017 Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2017-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PMIX_THREADS_TSD_H
#define PMIX_THREADS_TSD_H

#include "src/include/pmix_config.h"

#include <pthread.h>

#include "pmix_common.h"

BEGIN_C_DECLS

/**
 * @file
 *
 * Thread Specific Datastore Interface
 *
 * Functions for providing thread-specific datastore capabilities.
 */

/**
 * Prototype for callback when tsd data is being destroyed
 */
typedef void (*pmix_tsd_destructor_t)(void *value);

typedef pthread_key_t pmix_tsd_key_t;

static inline int pmix_tsd_key_delete(pmix_tsd_key_t key)
{
    return pthread_key_delete(key);
}

static inline int pmix_tsd_setspecific(pmix_tsd_key_t key, void *value)
{
    return pthread_setspecific(key, value);
}

static inline int pmix_tsd_getspecific(pmix_tsd_key_t key, void **valuep)
{
    *valuep = pthread_getspecific(key);
    return PMIX_SUCCESS;
}

/**
 * Create thread-specific data key
 *
 * Create a thread-specific data key visible to all threads in the
 * current process.  The returned key is valid in all threads,
 * although the values bound to the key by pmix_tsd_setspecific() are
 * allocated on a per-thread basis and persist for the life of the
 * calling thread.
 *
 * Upon key creation, the value NULL is associated with the new key in
 * all active threads.  When a new thread is created, the value NULL
 * is associated with all defined keys in the new thread.
 *
 * The destructor parameter may be NULL.  At thread exit, if
 * destructor is non-NULL AND the thread has a non-NULL value
 * associated with the key, the function is called with the current
 * value as its argument.
 *
 * @param key[out]       The key for accessing thread-specific data
 * @param destructor[in] Cleanup function to call when a thread exits
 *
 * Returns a pmix_status_t, not the pthread errno - callers hand the
 * result to PMIX_ERROR_LOG and return it out of the public API, and a
 * positive errno arriving there is read as some unrelated status.
 *
 * @retval PMIX_SUCCESS            Success
 * @retval PMIX_ERR_OUT_OF_RESOURCE The system lacked the necessary
 *                       resource to create another thread specific data
 *                       key (pthread's EAGAIN)
 * @retval PMIX_ERR_NOMEM Insufficient memory exists to create the key
 */
PMIX_EXPORT int pmix_tsd_key_create(pmix_tsd_key_t *key,
                                    pmix_tsd_destructor_t destructor);

/**
 * Destruct all thread-specific data keys
 *
 * Invoke each registered key's destructor on the calling thread's value
 * and then delete the key itself, releasing its slot.
 *
 * This is made necessary since destructors are not invoked on the
 * keys of the main thread, since there is no such thing as
 * pthread_join(main_thread)
 *
 * Two things are required of the caller, and both are about who else is
 * running. It must be the main thread, because that thread's values are
 * the ones nothing else will ever destroy. And it must be the *only*
 * thread left - every other thread joined - because this deletes the
 * keys outright: a thread that reaches pmix_tsd_getspecific() afterwards
 * is using a key that no longer exists, and one that re-creates a key
 * here registers it into a registry nothing will walk again, leaking the
 * slot this function exists to reclaim. See the call site at the end of
 * pmix_rte_finalize().
 *
 * Only the calling thread's value is destroyed. Values other threads
 * left behind were already handled by pthread when those threads exited.
 *
 * @retval PMIX_SUCCESS  Success
 */
PMIX_EXPORT int pmix_tsd_keys_destruct(void);

END_C_DECLS

#endif /* PMIX_THREADS_TSD_H */
