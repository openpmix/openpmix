/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2010      Cisco Systems, Inc. All rights reserved.
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

#include "pmix_config.h"

#include <errno.h>

#include "pmix_common.h"
#include "src/threads/pmix_threads.h"
#include "src/threads/pmix_tsd.h"

bool pmix_debug_threads = false;

static void pmix_thread_construct(pmix_thread_t *t);

struct pmix_tsd_key_value {
    pmix_tsd_key_t key;
    pmix_tsd_destructor_t destructor;
};

static struct pmix_tsd_key_value *pmix_tsd_key_values = NULL;
static int pmix_tsd_key_values_count = 0;
/* protects the key registry above: pmix_tsd_key_create can run on any
 * thread (its callers create their key lazily on first use), so the
 * registry it mutates must be serialized */
static pthread_mutex_t pmix_tsd_key_values_lock = PTHREAD_MUTEX_INITIALIZER;

PMIX_EXPORT PMIX_CLASS_INSTANCE(pmix_thread_t, pmix_object_t, pmix_thread_construct, NULL);

/*
 * Constructor
 */
static void pmix_thread_construct(pmix_thread_t *t)
{
    t->t_run = 0;
    /* PMIX_NEW does not zero the object, so every field has to be set
     * here or it carries whatever was in the heap. t_arg is handed to
     * the thread body as its only argument */
    t->t_arg = NULL;
    t->t_handle = (pthread_t) -1;
}

int pmix_thread_start(pmix_thread_t *t)
{
    int rc;

    if (PMIX_ENABLE_DEBUG) {
        if (NULL == t->t_run || t->t_handle != (pthread_t) -1) {
            return PMIX_ERR_BAD_PARAM;
        }
    }

    rc = pthread_create(&t->t_handle, NULL, (void *(*) (void *) ) t->t_run, t);
    if (0 != rc) {
        /* POSIX leaves the contents of the handle unspecified when the
         * create fails, so put the sentinel back rather than trust that
         * pthread_create left it alone. Both the restart check above and
         * the join guard below key off it, and neither can tell a
         * half-written handle from a real thread */
        t->t_handle = (pthread_t) -1;
        return PMIX_ERROR;
    }

    return PMIX_SUCCESS;
}

int pmix_thread_join(pmix_thread_t *t, void **thr_return)
{
    int rc;

    /* The constructor parks t_handle at the "no thread here" sentinel and
     * we put it back below, so a thread that never started - or that has
     * already been joined - arrives here still carrying it. Handing that
     * to pthread_join is undefined behavior, and on glibc it is a
     * dereference of the sentinel itself: the caller gets a segfault
     * where it expected an error return. Callers do reach this state -
     * see the resume path in src/runtime/pmix_progress_threads.c, which
     * marks its tracker active before the create and can therefore ask
     * to join an engine that failed to start */
    if ((pthread_t) -1 == t->t_handle) {
        if (NULL != thr_return) {
            *thr_return = NULL;
        }
        return PMIX_ERR_BAD_PARAM;
    }

    rc = pthread_join(t->t_handle, thr_return);
    t->t_handle = (pthread_t) -1;
    return (rc == 0) ? PMIX_SUCCESS : PMIX_ERROR;
}

int pmix_tsd_key_create(pmix_tsd_key_t *key, pmix_tsd_destructor_t destructor)
{
    int rc;
    struct pmix_tsd_key_value *tmp;

    rc = pthread_key_create(key, destructor);
    if (0 != rc) {
        /* pthread reports errno here, but every caller of this function
         * feeds the result into a pmix_status_t channel - PMIX_ERROR_LOG,
         * and in pmix_net_init's case straight out of PMIx_Init to the
         * application. A positive errno is not a status: it names some
         * unrelated PMIx constant or none at all. Convert it here, where
         * we still know what the number means */
        if (EAGAIN == rc) {
            return PMIX_ERR_OUT_OF_RESOURCE;
        }
        if (ENOMEM == rc) {
            return PMIX_ERR_NOMEM;
        }
        return PMIX_ERROR;
    }

    /* Register the key so its slot can be reclaimed - and its
     * destructor invoked for the finalizing thread's value - by
     * pmix_tsd_keys_destruct at finalize. pthread never runs
     * destructors for the main thread (there is no pthread_join of
     * main), so without this the key and its PTHREAD_KEYS_MAX-limited
     * slot would leak across every init/finalize cycle. Callers create
     * their keys lazily on first use, which may land on any thread, so
     * we register regardless of which thread we are on and serialize the
     * registry with its own lock. */
    pthread_mutex_lock(&pmix_tsd_key_values_lock);
    tmp = (struct pmix_tsd_key_value *)
        realloc(pmix_tsd_key_values,
                (pmix_tsd_key_values_count + 1) * sizeof(struct pmix_tsd_key_value));
    if (NULL == tmp) {
        /* leave the existing registry intact; the key is still valid and
         * usable, it simply will not be auto-cleaned at finalize - so the
         * create itself succeeded and is reported as such */
        pthread_mutex_unlock(&pmix_tsd_key_values_lock);
        return PMIX_SUCCESS;
    }
    pmix_tsd_key_values = tmp;
    pmix_tsd_key_values[pmix_tsd_key_values_count].key = *key;
    pmix_tsd_key_values[pmix_tsd_key_values_count].destructor = destructor;
    pmix_tsd_key_values_count++;
    pthread_mutex_unlock(&pmix_tsd_key_values_lock);

    return PMIX_SUCCESS;
}

int pmix_tsd_keys_destruct(void)
{
    int i;
    void *ptr;

    pthread_mutex_lock(&pmix_tsd_key_values_lock);
    for (i = 0; i < pmix_tsd_key_values_count; i++) {
        /* Stand in for the thread-exit destructor run that never happens
         * for this thread - so honor the same contract pthread does and
         * only call the destructor when there is a value to destroy.
         * pthread never invokes a destructor with NULL, so one written to
         * that contract is entitled to dereference its argument, and this
         * is the one call site that could have handed it a NULL: the
         * finalizing thread has no value for a key it never used */
        if (PMIX_SUCCESS == pmix_tsd_getspecific(pmix_tsd_key_values[i].key, &ptr)
            && NULL != ptr) {
            if (NULL != pmix_tsd_key_values[i].destructor) {
                pmix_tsd_key_values[i].destructor(ptr);
                pmix_tsd_setspecific(pmix_tsd_key_values[i].key, NULL);
            }
        }
        /* delete the key itself so its slot is not leaked across an
         * init/finalize cycle - callers that created it will recreate
         * it on the next PMIx_Init */
        pmix_tsd_key_delete(pmix_tsd_key_values[i].key);
    }
    if (0 < pmix_tsd_key_values_count) {
        free(pmix_tsd_key_values);
        /* null the registry so the next pmix_tsd_key_create reallocs from
         * a clean base rather than a freed pointer */
        pmix_tsd_key_values = NULL;
        pmix_tsd_key_values_count = 0;
    }
    pthread_mutex_unlock(&pmix_tsd_key_values_lock);

    return PMIX_SUCCESS;
}
