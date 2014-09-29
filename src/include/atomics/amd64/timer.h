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
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */

#ifndef PMIX_SYS_ARCH_TIMER_H
#define PMIX_SYS_ARCH_TIMER_H 1


typedef uint64_t pmix_timer_t;


#if PMIX_GCC_INLINE_ASSEMBLY

#if 0
static inline pmix_timer_t
pmix_sys_timer_get_cycles(void)
{
    pmix_timer_t ret;

    __asm__ __volatile__("rdtsc" : "=A"(ret));

    return ret;
}

#else

static inline pmix_timer_t 
pmix_sys_timer_get_cycles(void)
{
     unsigned a, d; 
     __asm__ __volatile__ ("rdtsc" : "=a" (a), "=d" (d));
     return ((pmix_timer_t)a) | (((pmix_timer_t)d) << 32);
}

#endif

#define PMIX_HAVE_SYS_TIMER_GET_CYCLES 1

#else

pmix_timer_t pmix_sys_timer_get_cycles(void);

#define PMIX_HAVE_SYS_TIMER_GET_CYCLES 1

#endif /* PMIX_GCC_INLINE_ASSEMBLY */

#endif /* ! PMIX_SYS_ARCH_TIMER_H */
