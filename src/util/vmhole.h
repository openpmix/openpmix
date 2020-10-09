/*
 * Copyright (C) 2014      Artem Polyakov <artpol84@gmail.com>
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PMIX_UTIL_VMHOLE_H
#define PMIX_UTIL_VMHOLE_H

#include "src/include/pmix_config.h"

typedef enum {
    VM_HOLE_NONE = -1,
    VM_HOLE_BEGIN = 0,        /* use hole at the very beginning */
    VM_HOLE_AFTER_HEAP = 1,   /* use hole right after heap */
    VM_HOLE_BEFORE_STACK = 2, /* use hole right before stack */
    VM_HOLE_BIGGEST = 3,      /* use biggest hole */
    VM_HOLE_IN_LIBS = 4,      /* use biggest hole between heap and stack */
    VM_HOLE_CUSTOM = 5,       /* use given address if available */
} pmix_vm_hole_kind_t;

typedef enum {
    VM_MAP_FILE = 0,
    VM_MAP_ANONYMOUS = 1,
    VM_MAP_HEAP = 2,
    VM_MAP_STACK = 3,
    VM_MAP_OTHER = 4 /* vsyscall/vdso/vvar shouldn't occur since we stop after stack */
} pmix_vm_map_kind_t;


PMIX_EXPORT pmix_status_t pmix_vmhole_register_params(void);
PMIX_EXPORT pmix_status_t pmix_vmhole_find(size_t shmemsize, const char *filename,
                                           char **shmemfile, size_t *shmemaddr);

#endif
