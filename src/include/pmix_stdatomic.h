/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2018      Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
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

#ifdef HAVE_STDATOMIC_H
#include <stdatomic.h>
#endif

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

#define pmix_atomic_store_int(addr , val) __atomic_store_n(addr, val, __ATOMIC_RELAXED)

#define pmix_atomic_load_int(addr) __atomic_load_n(addr, __ATOMIC_RELAXED)


#endif
