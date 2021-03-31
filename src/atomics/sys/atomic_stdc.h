/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2018      Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2018-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * Copyright (c) 2021      Amazon.com, Inc. or its affiliates.  All Rights
 *                         reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/* This file provides shims between the pmix atomics interface and the C11 atomics interface. It
 * is intended as the first step in moving to using C11 atomics across the entire codebase. Once
 * all officially supported compilers offer C11 atomic (GCC 4.9.0+, icc 2018+, pgi, xlc, etc) then
 * this shim will go away and the codebase will be updated to use C11's atomic support
 * directly.
 * This shim contains some functions already present in atomic_impl.h because we do not include
 * atomic_impl.h when using C11 atomics. It would require alot of #ifdefs to avoid duplicate
 * definitions to be worthwhile. */

#if !defined(PMIX_ATOMIC_STDC_H)
#    define PMIX_ATOMIC_STDC_H

#    include "src/include/pmix_stdint.h"
#    include <stdatomic.h>
#    include <stdint.h>

#    define PMIX_HAVE_ATOMIC_MEM_BARRIER 1

#    define PMIX_HAVE_ATOMIC_COMPARE_EXCHANGE_32 1
#    define PMIX_HAVE_ATOMIC_SWAP_32             1

#    define PMIX_HAVE_ATOMIC_MATH_32 1
#    define PMIX_HAVE_ATOMIC_ADD_32  1
#    define PMIX_HAVE_ATOMIC_AND_32  1
#    define PMIX_HAVE_ATOMIC_OR_32   1
#    define PMIX_HAVE_ATOMIC_XOR_32  1
#    define PMIX_HAVE_ATOMIC_SUB_32  1

#    define PMIX_HAVE_ATOMIC_COMPARE_EXCHANGE_64 1
#    define PMIX_HAVE_ATOMIC_SWAP_64             1

#    define PMIX_HAVE_ATOMIC_MATH_64 1
#    define PMIX_HAVE_ATOMIC_ADD_64  1
#    define PMIX_HAVE_ATOMIC_AND_64  1
#    define PMIX_HAVE_ATOMIC_OR_64   1
#    define PMIX_HAVE_ATOMIC_XOR_64  1
#    define PMIX_HAVE_ATOMIC_SUB_64  1

#    define PMIX_HAVE_ATOMIC_LLSC_32  0
#    define PMIX_HAVE_ATOMIC_LLSC_64  0
#    define PMIX_HAVE_ATOMIC_LLSC_PTR 0

#    define PMIX_HAVE_ATOMIC_MIN_32 1
#    define PMIX_HAVE_ATOMIC_MAX_32 1

#    define PMIX_HAVE_ATOMIC_MIN_64 1
#    define PMIX_HAVE_ATOMIC_MAX_64 1

#    define PMIX_HAVE_ATOMIC_SPINLOCKS 1

static inline void pmix_atomic_mb(void)
{
    atomic_thread_fence(memory_order_seq_cst);
}

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
#    define pmix_atomic_compare_exchange_strong_64(addr, compare, value)                    \
        atomic_compare_exchange_strong_explicit(addr, compare, value, memory_order_relaxed, \
                                                memory_order_relaxed)
#    define pmix_atomic_compare_exchange_strong_ptr(addr, compare, value)                   \
        atomic_compare_exchange_strong_explicit(addr, compare, value, memory_order_relaxed, \
                                                memory_order_relaxed)
#    define pmix_atomic_compare_exchange_strong_acq_32(addr, compare, value)                \
        atomic_compare_exchange_strong_explicit(addr, compare, value, memory_order_acquire, \
                                                memory_order_relaxed)
#    define pmix_atomic_compare_exchange_strong_acq_64(addr, compare, value)                \
        atomic_compare_exchange_strong_explicit(addr, compare, value, memory_order_acquire, \
                                                memory_order_relaxed)
#    define pmix_atomic_compare_exchange_strong_acq_ptr(addr, compare, value)               \
        atomic_compare_exchange_strong_explicit(addr, compare, value, memory_order_acquire, \
                                                memory_order_relaxed)

#    define pmix_atomic_compare_exchange_strong_rel_32(addr, compare, value)                \
        atomic_compare_exchange_strong_explicit(addr, compare, value, memory_order_release, \
                                                memory_order_relaxed)
#    define pmix_atomic_compare_exchange_strong_rel_64(addr, compare, value)                \
        atomic_compare_exchange_strong_explicit(addr, compare, value, memory_order_release, \
                                                memory_order_relaxed)
#    define pmix_atomic_compare_exchange_strong_rel_ptr(addr, compare, value)               \
        atomic_compare_exchange_strong_explicit(addr, compare, value, memory_order_release, \
                                                memory_order_relaxed)

#    define pmix_atomic_compare_exchange_strong(addr, oldval, newval)                       \
        atomic_compare_exchange_strong_explicit(addr, oldval, newval, memory_order_relaxed, \
                                                memory_order_relaxed)
#    define pmix_atomic_compare_exchange_strong_acq(addr, oldval, newval)                   \
        atomic_compare_exchange_strong_explicit(addr, oldval, newval, memory_order_acquire, \
                                                memory_order_relaxed)
#    define pmix_atomic_compare_exchange_strong_rel(addr, oldval, newval)                   \
        atomic_compare_exchange_strong_explicit(addr, oldval, newval, memory_order_release, \
                                                memory_order_relaxed)

#    define pmix_atomic_swap_32(addr, value) \
        atomic_exchange_explicit(addr, value, memory_order_relaxed)
#    define pmix_atomic_swap_64(addr, value) \
        atomic_exchange_explicit(addr, value, memory_order_relaxed)
#    define pmix_atomic_swap_ptr(addr, value) \
        atomic_exchange_explicit(addr, value, memory_order_relaxed)

#    ifdef PMIX_HAVE_CLANG_BUILTIN_ATOMIC_C11_FUNC
#        define PMIX_ATOMIC_STDC_DEFINE_FETCH_OP(op, bits, type, operator)               \
            static inline type pmix_atomic_fetch_##op##_##bits(pmix_atomic_##type *addr, \
                                                               type value)               \
            {                                                                            \
                return atomic_fetch_##op##_explicit(addr, value, memory_order_relaxed);  \
            }                                                                            \
                                                                                         \
            static inline type pmix_atomic_##op##_fetch_##bits(pmix_atomic_##type *addr, \
                                                               type value)               \
            {                                                                            \
                return atomic_fetch_##op##_explicit(addr, value, memory_order_relaxed)   \
                operator value;                                                          \
            }
#    else
#        define PMIX_ATOMIC_STDC_DEFINE_FETCH_OP(op, bits, type, operator)                       \
            static inline type pmix_atomic_fetch_##op##_##bits(pmix_atomic_##type *addr,         \
                                                               type value)                       \
            {                                                                                    \
                return atomic_fetch_##op##_explicit((type *) addr, value, memory_order_relaxed); \
            }                                                                                    \
            static inline type pmix_atomic_##op##_fetch_##bits(pmix_atomic_##type *addr,         \
                                                               type value)                       \
            {                                                                                    \
                return atomic_fetch_##op##_explicit((type *) addr, value, memory_order_relaxed)  \
                operator value;                                                                  \
            }
#    endif

PMIX_ATOMIC_STDC_DEFINE_FETCH_OP(add, 32, int32_t, +)
PMIX_ATOMIC_STDC_DEFINE_FETCH_OP(add, 64, int64_t, +)
PMIX_ATOMIC_STDC_DEFINE_FETCH_OP(add, size_t, size_t, +)

PMIX_ATOMIC_STDC_DEFINE_FETCH_OP(sub, 32, int32_t, -)
PMIX_ATOMIC_STDC_DEFINE_FETCH_OP(sub, 64, int64_t, -)
PMIX_ATOMIC_STDC_DEFINE_FETCH_OP(sub, size_t, size_t, -)

PMIX_ATOMIC_STDC_DEFINE_FETCH_OP(or, 32, int32_t, |)
PMIX_ATOMIC_STDC_DEFINE_FETCH_OP(or, 64, int64_t, |)

PMIX_ATOMIC_STDC_DEFINE_FETCH_OP(xor, 32, int32_t, ^)
PMIX_ATOMIC_STDC_DEFINE_FETCH_OP(xor, 64, int64_t, ^)

PMIX_ATOMIC_STDC_DEFINE_FETCH_OP(and, 32, int32_t, &)
PMIX_ATOMIC_STDC_DEFINE_FETCH_OP(and, 64, int64_t, &)

#    define pmix_atomic_add(addr, value) \
        (void) atomic_fetch_add_explicit(addr, value, memory_order_relaxed)

static inline int32_t pmix_atomic_fetch_min_32(pmix_atomic_int32_t *addr, int32_t value)
{
    int32_t old = *addr;
    do {
        if (old <= value) {
            break;
        }
    } while (!pmix_atomic_compare_exchange_strong_32(addr, &old, value));

    return old;
}

static inline int32_t pmix_atomic_fetch_max_32(pmix_atomic_int32_t *addr, int32_t value)
{
    int32_t old = *addr;
    do {
        if (old >= value) {
            break;
        }
    } while (!pmix_atomic_compare_exchange_strong_32(addr, &old, value));

    return old;
}

static inline int64_t pmix_atomic_fetch_min_64(pmix_atomic_int64_t *addr, int64_t value)
{
    int64_t old = *addr;
    do {
        if (old <= value) {
            break;
        }
    } while (!pmix_atomic_compare_exchange_strong_64(addr, &old, value));

    return old;
}

static inline int64_t pmix_atomic_fetch_max_64(pmix_atomic_int64_t *addr, int64_t value)
{
    int64_t old = *addr;
    do {
        if (old >= value) {
            break;
        }
    } while (!pmix_atomic_compare_exchange_strong_64(addr, &old, value));

    return old;
}

static inline int32_t pmix_atomic_min_fetch_32(pmix_atomic_int32_t *addr, int32_t value)
{
    int32_t old = pmix_atomic_fetch_min_32(addr, value);
    return old <= value ? old : value;
}

static inline int32_t pmix_atomic_max_fetch_32(pmix_atomic_int32_t *addr, int32_t value)
{
    int32_t old = pmix_atomic_fetch_max_32(addr, value);
    return old >= value ? old : value;
}

static inline int64_t pmix_atomic_min_fetch_64(pmix_atomic_int64_t *addr, int64_t value)
{
    int64_t old = pmix_atomic_fetch_min_64(addr, value);
    return old <= value ? old : value;
}

static inline int64_t pmix_atomic_max_fetch_64(pmix_atomic_int64_t *addr, int64_t value)
{
    int64_t old = pmix_atomic_fetch_max_64(addr, value);
    return old >= value ? old : value;
}

#    define PMIX_ATOMIC_LOCK_UNLOCKED false
#    define PMIX_ATOMIC_LOCK_LOCKED   true

#    define PMIX_ATOMIC_LOCK_INIT ATOMIC_FLAG_INIT

typedef atomic_flag pmix_atomic_lock_t;

/*
 * Lock initialization function. It set the lock to UNLOCKED.
 */
static inline void pmix_atomic_lock_init(pmix_atomic_lock_t *lock, bool value)
{
    (void) value;
#    ifdef PMIX_HAVE_CLANG_BUILTIN_ATOMIC_C11_FUNC
    atomic_flag_clear(lock);
#    else
    atomic_flag_clear((volatile void *) lock);
#    endif
}

static inline int pmix_atomic_trylock(pmix_atomic_lock_t *lock)
{
#    ifdef PMIX_HAVE_CLANG_BUILTIN_ATOMIC_C11_FUNC
    return (int) atomic_flag_test_and_set(lock);
#    else
    return (int) atomic_flag_test_and_set((volatile void *) lock);
#    endif
}

static inline void pmix_atomic_lock(pmix_atomic_lock_t *lock)
{
    while (pmix_atomic_trylock(lock)) {
    }
}

static inline void pmix_atomic_unlock(pmix_atomic_lock_t *lock)
{
#    ifdef PMIX_HAVE_CLANG_BUILTIN_ATOMIC_C11_FUNC
    atomic_flag_clear(lock);
#    else
    atomic_flag_clear((volatile void *) lock);
#    endif
}

#endif /* !defined(PMIX_ATOMIC_STDC_H) */
