/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
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
 * Copyright (c) 2007      Sun Microsystems, Inc.  All rights reserved.
 * Copyright (c) 2011      Sandia National Laboratories. All rights reserved.
 * Copyright (c) 2011-2017 Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2017      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2018-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021-2023 Nanook Consulting  All rights reserved.
 * Copyright (c) 2021      Amazon.com, Inc. or its affiliates.  All Rights
 *                         reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/** @file
 *
 * Atomic operations.
 *
 * This API is patterned after the FreeBSD kernel atomic interface
 * (which is influenced by Intel's ia64 architecture).  The
 * FreeBSD interface is documented at
 *
 * http://www.freebsd.org/cgi/man.cgi?query=atomic&sektion=9
 *
 * Only the necessary subset of functions are implemented here.
 *
 * The following #defines will be true / false based on
 * assembly support:
 *
 *  - \c PMIX_HAVE_ATOMIC_MEM_BARRIER atomic memory barriers
 *  - \c PMIX_HAVE_ATOMIC_SPINLOCKS atomic spinlocks
 *  - \c PMIX_HAVE_ATOMIC_MATH_32 if 32 bit add/sub/compare-exchange can be done "atomically"
 *  - \c PMIX_HAVE_ATOMIC_MATH_64 if 64 bit add/sub/compare-exchange can be done "atomically"
 *
 * Note that for the Atomic math, atomic add/sub may be implemented as
 * C code using pmix_atomic_compare_exchange.  The appearance of atomic
 * operation will be upheld in these cases.
 */

#ifndef PMIX_SYS_ATOMIC_H
#define PMIX_SYS_ATOMIC_H 1

#include "src/include/pmix_config.h"

#if PMIX_ATOMIC_C11

#include <stdatomic.h>

typedef atomic_int pmix_atomic_int_t;
typedef atomic_long pmix_atomic_long_t;

typedef _Atomic bool pmix_atomic_bool_t;
typedef _Atomic int32_t pmix_atomic_int32_t;
typedef _Atomic uint32_t pmix_atomic_uint32_t;
typedef _Atomic int64_t pmix_atomic_int64_t;
typedef _Atomic uint64_t pmix_atomic_uint64_t;

typedef _Atomic size_t pmix_atomic_size_t;
typedef _Atomic ssize_t pmix_atomic_ssize_t;
typedef _Atomic intptr_t pmix_atomic_intptr_t;
typedef _Atomic uintptr_t pmix_atomic_uintptr_t;

static inline void pmix_atomic_wmb(void)
{
    atomic_thread_fence(memory_order_release);
}

static inline void pmix_atomic_rmb(void)
{
#    if defined(PMIX_ATOMIC_X86_64)
    /* work around a bug in older gcc versions (observed in gcc 6.x)
     * where acquire seems to get treated as a no-op instead of being
     * equivalent to __asm__ __volatile__("": : :"memory") on x86_64 */
    __asm__ __volatile__("" : : : "memory");
#    else
    atomic_thread_fence(memory_order_acquire);
#    endif
}

#    define pmix_atomic_compare_exchange_strong_32(addr, compare, value)                    \
        atomic_compare_exchange_strong_explicit(addr, compare, value, memory_order_relaxed, \
                                                memory_order_relaxed)


#else

typedef volatile int pmix_atomic_int_t;
typedef volatile long pmix_atomic_long_t;

typedef volatile bool pmix_atomic_bool_t;
typedef volatile int32_t pmix_atomic_int32_t;
typedef volatile uint32_t pmix_atomic_uint32_t;
typedef volatile int64_t pmix_atomic_int64_t;
typedef volatile uint64_t pmix_atomic_uint64_t;

typedef volatile size_t pmix_atomic_size_t;
typedef volatile ssize_t pmix_atomic_ssize_t;
typedef volatile intptr_t pmix_atomic_intptr_t;
typedef volatile uintptr_t pmix_atomic_uintptr_t;

static inline void pmix_atomic_wmb(void)
{
    __atomic_thread_fence(__ATOMIC_RELEASE);
}

static inline void pmix_atomic_rmb(void)
{
#if defined(PMIX_ATOMIC_X86_64)
    /* work around a bug in older gcc versions where ACQUIRE seems to get
     * treated as a no-op instead of being equivalent to
     * __asm__ __volatile__("": : :"memory") */
    __asm__ __volatile__("" : : : "memory");
#else
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
#endif
}

static inline bool pmix_atomic_compare_exchange_strong_32(pmix_atomic_int32_t *addr,
                                                          int32_t *oldval, int32_t newval)
{
    return __atomic_compare_exchange_n(addr, oldval, newval, false,
                                       __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

#endif

#endif /* PMIX_SYS_ATOMIC_H */
