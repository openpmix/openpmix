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

/** @file
 *
 * Cycle counter reading instructions.  Do not use directly - see the
 * timer interface instead
 */

#ifndef PMIX_SYS_TIMER_H
#define PMIX_SYS_TIMER_H 1

#include "pmix_config.h"

#include "pmix/sys/architecture.h"

#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif

/* do some quick #define cleanup in cases where we are doing
   testing... */
#ifdef PMIX_DISABLE_INLINE_ASM
#undef PMIX_C_GCC_INLINE_ASSEMBLY
#define PMIX_C_GCC_INLINE_ASSEMBLY 0
#undef PMIX_CXX_GCC_INLINE_ASSEMBLY
#define PMIX_CXX_GCC_INLINE_ASSEMBLY 0
#undef PMIX_C_DEC_INLINE_ASSEMBLY
#define PMIX_C_DEC_INLINE_ASSEMBLY 0
#undef PMIX_CXX_DEC_INLINE_ASSEMBLY
#define PMIX_CXX_DEC_INLINE_ASSEMBLY 0
#undef PMIX_C_XLC_INLINE_ASSEMBLY
#define PMIX_C_XLC_INLINE_ASSEMBLY 0
#undef PMIX_CXX_XLC_INLINE_ASSEMBLY
#define PMIX_CXX_XLC_INLINE_ASSEMBLY 0
#endif

/* define PMIX_{GCC,DEC,XLC}_INLINE_ASSEMBLY based on the
   PMIX_{C,CXX}_{GCC,DEC,XLC}_INLINE_ASSEMBLY defines and whether we
   are in C or C++ */
#if defined(c_plusplus) || defined(__cplusplus)
#define PMIX_GCC_INLINE_ASSEMBLY PMIX_CXX_GCC_INLINE_ASSEMBLY
#define PMIX_DEC_INLINE_ASSEMBLY PMIX_CXX_DEC_INLINE_ASSEMBLY
#define PMIX_XLC_INLINE_ASSEMBLY PMIX_CXX_XLC_INLINE_ASSEMBLY
#else
#define PMIX_GCC_INLINE_ASSEMBLY PMIX_C_GCC_INLINE_ASSEMBLY
#define PMIX_DEC_INLINE_ASSEMBLY PMIX_C_DEC_INLINE_ASSEMBLY
#define PMIX_XLC_INLINE_ASSEMBLY PMIX_C_XLC_INLINE_ASSEMBLY
#endif

/**********************************************************************
 *
 * Load the appropriate architecture files and set some reasonable
 * default values for our support
 *
 *********************************************************************/

BEGIN_C_DECLS

/* If you update this list, you probably also want to update
   pmix/mca/timer/linux/configure.m4.  Or not. */

#if PMIX_ASSEMBLY_ARCH == PMIX_AMD64
#include "pmix/sys/amd64/timer.h"
#elif PMIX_ASSEMBLY_ARCH == PMIX_ARM
#include "pmix/sys/arm/timer.h"
#elif PMIX_ASSEMBLY_ARCH == PMIX_IA32
#include "pmix/sys/ia32/timer.h"
#elif PMIX_ASSEMBLY_ARCH == PMIX_IA64
#include "pmix/sys/ia64/timer.h"
#elif PMIX_ASSEMBLY_ARCH == PMIX_POWERPC32
#include "pmix/sys/powerpc/timer.h"
#elif PMIX_ASSEMBLY_ARCH == PMIX_POWERPC64
#include "pmix/sys/powerpc/timer.h"
#elif PMIX_ASSEMBLY_ARCH == PMIX_SPARCV9_32
#include "pmix/sys/sparcv9/timer.h"
#elif PMIX_ASSEMBLY_ARCH == PMIX_SPARCV9_64
#include "pmix/sys/sparcv9/timer.h"
#elif PMIX_ASSEMBLY_ARCH == PMIX_MIPS
#include "pmix/sys/mips/timer.h"
#endif

#ifndef PMIX_HAVE_SYS_TIMER_GET_CYCLES
#define PMIX_HAVE_SYS_TIMER_GET_CYCLES 0

typedef long pmix_timer_t;
#endif

END_C_DECLS

#endif /* PMIX_SYS_TIMER_H */
