/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2018      Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * Copyright (c) 2021      Amazon.com, Inc. or its affiliates.
 *                         All Rights reserved.
 * Copyright (c) 2023      Triad National Security, LLC. All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PMIX_STDATOMIC_H
#define PMIX_STDATOMIC_H

#include "pmix_stdint.h"
#include <stdbool.h>

/* C11 atomics are a hard REQUIREMENT for PMIx, not a probed option: configure
 * (see the "Check required atomics" block in config/pmix.m4) errors out when
 * the compiler cannot provide them, and this header's own typedefs and macros
 * - along with the bare atomic_bool fields in pmix_globals_t, which reach
 * <stdatomic.h> only through here - do not compile without the header. So
 * include it unconditionally. Guarding it on HAVE_STDATOMIC_H suggested a
 * fallback that does not exist and would have turned a clear "no C11 atomics"
 * configure error into a pile of unknown-type errors deep in the build. */
#include <stdatomic.h>

typedef _Atomic bool pmix_atomic_bool_t;
typedef _Atomic int32_t pmix_atomic_int32_t;
typedef _Atomic uint32_t pmix_atomic_uint32_t;
typedef _Atomic int64_t pmix_atomic_int64_t;
typedef _Atomic uint64_t pmix_atomic_uint64_t;

typedef _Atomic size_t pmix_atomic_size_t;
typedef _Atomic ssize_t pmix_atomic_ssize_t;
typedef _Atomic intptr_t pmix_atomic_intptr_t;
typedef _Atomic uintptr_t pmix_atomic_uintptr_t;

#define pmix_atomic_store_int(addr, val) __atomic_store_n(addr, val, __ATOMIC_RELAXED)

#define pmix_atomic_load_int(addr) __atomic_load_n(addr, __ATOMIC_RELAXED)

#define pmix_atomic_set_bool(addr) atomic_store(addr, true)

#define pmix_atomic_unset_bool(addr) atomic_store(addr, false)

#define pmix_atomic_check_bool(addr) atomic_load(addr)

#define pmix_atomic_fetch_add(addr, val) __atomic_fetch_add(addr, val, __ATOMIC_SEQ_CST)

#define pmix_atomic_test_and_set(addr) __atomic_test_and_set(addr, __ATOMIC_SEQ_CST)

/* the required partner of pmix_atomic_test_and_set - a flag that was set with
 * a test-and-set must be cleared with this, not with a plain assignment, or
 * the clear is a non-atomic store racing an atomic read-modify-write. Both
 * take a pointer to an ordinary (non-_Atomic) object - pmix_globals.init_called
 * is a plain bool - which is why this pair spells itself with the compiler's
 * __atomic_* builtins rather than the C11 generic functions beside them:
 * atomic_store/atomic_load require an _Atomic-qualified operand. See the
 * design note in src/include/AGENTS.md */
#define pmix_atomic_clear(addr) __atomic_clear(addr, __ATOMIC_SEQ_CST)

#endif
