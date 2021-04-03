/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2018      Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2018      Intel, Inc.  All rights reserved.
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * Copyright (c) 2021      Amazon.com, Inc. or its affiliates.  All Rights
 *                         reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#if !defined(PMIX_STDATOMIC_H)
#    define PMIX_STDATOMIC_H

#    include "pmix_stdint.h"

#    if PMIX_ATOMIC_C11

#        include <stdatomic.h>

typedef atomic_int pmix_atomic_int_t;
typedef atomic_long pmix_atomic_long_t;

typedef _Atomic int32_t pmix_atomic_int32_t;
typedef _Atomic uint32_t pmix_atomic_uint32_t;
typedef _Atomic int64_t pmix_atomic_int64_t;
typedef _Atomic uint64_t pmix_atomic_uint64_t;

typedef _Atomic size_t pmix_atomic_size_t;
typedef _Atomic ssize_t pmix_atomic_ssize_t;
typedef _Atomic intptr_t pmix_atomic_intptr_t;
typedef _Atomic uintptr_t pmix_atomic_uintptr_t;

#    else

typedef volatile int pmix_atomic_int_t;
typedef volatile long pmix_atomic_long_t;

typedef volatile int32_t pmix_atomic_int32_t;
typedef volatile uint32_t pmix_atomic_uint32_t;
typedef volatile int64_t pmix_atomic_int64_t;
typedef volatile uint64_t pmix_atomic_uint64_t;

typedef volatile size_t pmix_atomic_size_t;
typedef volatile ssize_t pmix_atomic_ssize_t;
typedef volatile intptr_t pmix_atomic_intptr_t;
typedef volatile uintptr_t pmix_atomic_uintptr_t;

#    endif /* PMIX_HAVE_C__ATOMIC */

#endif /* !defined(PMIX_STDATOMIC_H) */
