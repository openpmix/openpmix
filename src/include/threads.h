/* -*- Mode: C; c-basic-offset:4 ; -*- */
/*
 * PMIx copyrights:
 * Copyright (c) 2013      Intel, Inc. All rights reserved
 *
 **********************************
 *
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2006 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart, 
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2007      Los Alamos National Security, LLC.  All rights
 *                         reserved. 
 **********************************
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */

#ifndef  PMIX_THREADS_H
#define  PMIX_THREADS_H

/**
 * @file:
 *
 * Mutual exclusion functions: Unix implementation.
 *
 * Functions for locking of critical sections.
 *
 * On unix, use pthreads or our own atomic operations as
 * available.
 */

#include "pmix_config.h"

#ifdef HAVE_PTHREAD_H
#include <pthread.h>
#endif
#include <errno.h>
#include <stdio.h>

#include "class/pmix_object.h"
#include "atomics/atomic.h"

BEGIN_C_DECLS

struct pmix_mutex_t {
    pmix_object_t super;
    pthread_mutex_t m_lock_pthread;
    pmix_atomic_lock_t m_lock_atomic;
};
typedef struct pmix_mutex_t pmix_mutex_t;
PMIX_DECLSPEC OBJ_CLASS_DECLARATION(pmix_mutex_t);

/************************************************************************
 *
 * mutex operations (non-atomic versions)
 *
 ************************************************************************/

static inline int pmix_mutex_trylock(pmix_mutex_t *m)
{
#if PMIX_ENABLE_DEBUG
    int ret = pthread_mutex_trylock(&m->m_lock_pthread);
    if (ret == EDEADLK) {
        errno = ret;
        perror("pmix_mutex_trylock()");
        abort();
    }
    return ret;
#else
    return pthread_mutex_trylock(&m->m_lock_pthread);
#endif
}

static inline void pmix_mutex_lock(pmix_mutex_t *m)
{
#if PMIX_ENABLE_DEBUG
    int ret = pthread_mutex_lock(&m->m_lock_pthread);
    if (ret == EDEADLK) {
        errno = ret;
        perror("pmix_mutex_lock()");
        abort();
    }
#else
    pthread_mutex_lock(&m->m_lock_pthread);
#endif
}

static inline void pmix_mutex_unlock(pmix_mutex_t *m)
{
#if PMIX_ENABLE_DEBUG
    int ret = pthread_mutex_unlock(&m->m_lock_pthread);
    if (ret == EPERM) {
        errno = ret;
        perror("pmix_mutex_unlock");
        abort();
    }
#else
    pthread_mutex_unlock(&m->m_lock_pthread);
#endif
}

/************************************************************************
 *
 * mutex operations (atomic versions)
 *
 ************************************************************************/

#if PMIX_HAVE_ATOMIC_SPINLOCKS

/************************************************************************
 * Spin Locks
 ************************************************************************/

static inline int pmix_mutex_atomic_trylock(pmix_mutex_t *m)
{
    return pmix_atomic_trylock(&m->m_lock_atomic);
}

static inline void pmix_mutex_atomic_lock(pmix_mutex_t *m)
{
    pmix_atomic_lock(&m->m_lock_atomic);
}

static inline void pmix_mutex_atomic_unlock(pmix_mutex_t *m)
{
    pmix_atomic_unlock(&m->m_lock_atomic);
}

#else

/************************************************************************
 * Standard locking
 ************************************************************************/

static inline int pmix_mutex_atomic_trylock(pmix_mutex_t *m)
{
    return pmix_mutex_trylock(m);
}

static inline void pmix_mutex_atomic_lock(pmix_mutex_t *m)
{
    pmix_mutex_lock(m);
}

static inline void pmix_mutex_atomic_unlock(pmix_mutex_t *m)
{
    pmix_mutex_unlock(m);
}

#endif

END_C_DECLS

#endif
