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
 * Copyright (c) 2021-2022 Nanook Consulting  All rights reserved.
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

#elif PMIX_ATOMIC_GCC_BUILTIN

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

#endif

#endif /* PMIX_SYS_ATOMIC_H */
