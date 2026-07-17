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
 * Copyright (c) 2021-2022 Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "pmix_config.h"

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

    return (rc == 0) ? PMIX_SUCCESS : PMIX_ERROR;
}

int pmix_thread_join(pmix_thread_t *t, void **thr_return)
{
    int rc = pthread_join(t->t_handle, thr_return);
    t->t_handle = (pthread_t) -1;
    return (rc == 0) ? PMIX_SUCCESS : PMIX_ERROR;
}

int pmix_tsd_key_create(pmix_tsd_key_t *key, pmix_tsd_destructor_t destructor)
{
    int rc;
    struct pmix_tsd_key_value *tmp;

    rc = pthread_key_create(key, destructor);
    if (0 != rc) {
        return rc;
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
         * usable, it simply will not be auto-cleaned at finalize */
        pthread_mutex_unlock(&pmix_tsd_key_values_lock);
        return rc;
    }
    pmix_tsd_key_values = tmp;
    pmix_tsd_key_values[pmix_tsd_key_values_count].key = *key;
    pmix_tsd_key_values[pmix_tsd_key_values_count].destructor = destructor;
    pmix_tsd_key_values_count++;
    pthread_mutex_unlock(&pmix_tsd_key_values_lock);

    return rc;
}

int pmix_tsd_keys_destruct(void)
{
    int i;
    void *ptr;

    pthread_mutex_lock(&pmix_tsd_key_values_lock);
    for (i = 0; i < pmix_tsd_key_values_count; i++) {
        if (PMIX_SUCCESS == pmix_tsd_getspecific(pmix_tsd_key_values[i].key, &ptr)) {
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
