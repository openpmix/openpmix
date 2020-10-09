/*
 * Copyright (C) 2014      Artem Polyakov <artpol84@gmail.com>
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "src/include/pmix_config.h"

#include "include/pmix_common.h"

#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>

#ifdef HAVE_STRING_H
#include <string.h>
#endif

#include <errno.h>

#include "src/class/pmix_pointer_array.h"
#include "src/class/pmix_list.h"
#include "src/server/pmix_server_ops.h"
#include "src/util/basename.h"
#include "src/util/error.h"
#include "src/util/name_fns.h"
#include "src/util/output.h"
#include "src/util/os_path.h"
#include "src/util/path.h"
#include "src/util/show_help.h"
#include "src/mca/base/pmix_mca_base_var.h"

#include "src/util/vmhole.h"

static char *vmhole = "biggest";
static pmix_vm_hole_kind_t hole_kind = VM_HOLE_BIGGEST;
static bool pmix_vmhole_debug = false;

static int parse_map_line(const char *line,
                          unsigned long *beginp,
                          unsigned long *endp,
                          pmix_vm_map_kind_t *kindp);
static int use_hole(unsigned long holebegin,
                    unsigned long holesize,
                    unsigned long *addrp,
                    unsigned long size);
static int find_hole(pmix_vm_hole_kind_t hkind,
                     size_t *addrp,
                     size_t size);
static int enough_space(const char *filename,
                        size_t space_req,
                        uint64_t *space_avail,
                        bool *result);

pmix_status_t pmix_vmhole_register_params(void)
{
    (void) pmix_mca_base_var_register ("pmix", "pmix", NULL, "vmhole_debug",
                                           "Debug vmhole code",
                                           PMIX_MCA_BASE_VAR_TYPE_BOOL, NULL, 0, 0,
                                           PMIX_INFO_LVL_9,
                                           PMIX_MCA_BASE_VAR_SCOPE_READONLY,
                                           &pmix_vmhole_debug);

    (void) pmix_mca_base_var_register ("pmix", "pmix", NULL, "vmhole_kind",
                                           "Kind of VM hole to identify - none, begin, biggest, libs, heap, stack (default=biggest)",
                                           PMIX_MCA_BASE_VAR_TYPE_STRING, NULL, 0, 0,
                                           PMIX_INFO_LVL_9,
                                           PMIX_MCA_BASE_VAR_SCOPE_READONLY,
                                           &vmhole);
    if (0 == strcasecmp(vmhole, "none")) {
        hole_kind = VM_HOLE_NONE;
    } else if (0 == strcasecmp(vmhole, "begin")) {
        hole_kind = VM_HOLE_BEGIN;
    } else if (0 == strcasecmp(vmhole, "biggest")) {
        hole_kind = VM_HOLE_BIGGEST;
    } else if (0 == strcasecmp(vmhole, "libs")) {
        hole_kind = VM_HOLE_IN_LIBS;
    } else if (0 == strcasecmp(vmhole, "heap")) {
        hole_kind = VM_HOLE_AFTER_HEAP;
    } else if (0 == strcasecmp(vmhole, "stack")) {
        hole_kind = VM_HOLE_BEFORE_STACK;
    } else {
        pmix_output(0, "INVALID VM HOLE TYPE");
        return PMIX_ERROR;
    }

    return PMIX_SUCCESS;
}

pmix_status_t pmix_vmhole_find(size_t shmemsize, const char *filename,
                               char **shmemfile, size_t *shmemaddr)
{
    char *file;
    bool space = false;
    uint64_t amount_space_avail;
    pmix_status_t rc;

    if (PMIX_SUCCESS != (rc = find_hole(hole_kind, shmemaddr, shmemsize))) {
        /* we couldn't find a hole, so don't use the shmem support - provide
         * some diagnostic output if requested */
        if (pmix_vmhole_debug) {
            FILE *file = fopen("/proc/self/maps", "r");
            if (file) {
                char line[256];
                pmix_output(0, "%s Dumping /proc/self/maps",
                            PMIX_NAME_PRINT(&pmix_globals.myid));
                while (fgets(line, sizeof(line), file) != NULL) {
                    char *end = strchr(line, '\n');
                    if (end) {
                       *end = '\0';
                    }
                    pmix_output(0, "%s", line);
                }
                fclose(file);
            }
        }
        return rc;
    }

    /* we will create the shmem file in our session dir so it
     * will automatically get cleaned up */
    file = pmix_os_path(false, pmix_server_globals.tmpdir, filename, NULL);
    /* let's make sure we have enough space for the backing file */
    if (PMIX_SUCCESS != (rc = enough_space(file, shmemsize,
                                           &amount_space_avail,
                                           &space))) {
        if (pmix_vmhole_debug) {
            pmix_output(0, "%s an error occurred while determining "
                            "whether or not %s could be created for shmem.",
                            PMIX_NAME_PRINT(&pmix_globals.myid), file);
        }
        free(file);
        return rc;
    }
    if (!space) {
        if (pmix_vmhole_debug) {
            pmix_show_help("help-pmix-ploc-hwloc.txt", "target full", true,
                           shmemfile, pmix_globals.hostname,
                           (unsigned long)shmemsize,
                           (unsigned long long)amount_space_avail);
        }
        free(file);
        return PMIX_ERR_NOT_AVAILABLE;
    }

    *shmemfile = file;
    return PMIX_SUCCESS;
}

static int parse_map_line(const char *line,
                          unsigned long *beginp,
                          unsigned long *endp,
                          pmix_vm_map_kind_t *kindp)
{
    const char *tmp = line, *next;
    unsigned long value;

    /* "beginaddr-endaddr " */
    value = strtoull(tmp, (char **) &next, 16);
    if (next == tmp) {
        return PMIX_ERROR;
    }

    *beginp = (unsigned long) value;

    if (*next != '-') {
        return PMIX_ERROR;
    }

     tmp = next + 1;

    value = strtoull(tmp, (char **) &next, 16);
    if (next == tmp) {
        return PMIX_ERROR;
    }
    *endp = (unsigned long) value;
    tmp = next;

    if (*next != ' ') {
        return PMIX_ERROR;
    }
    tmp = next + 1;

    /* look for ending absolute path */
    next = strchr(tmp, '/');
    if (next) {
        *kindp = VM_MAP_FILE;
    } else {
        /* look for ending special tag [foo] */
        next = strchr(tmp, '[');
        if (next) {
            if (!strncmp(next, "[heap]", 6)) {
                *kindp = VM_MAP_HEAP;
            } else if (!strncmp(next, "[stack]", 7)) {
                *kindp = VM_MAP_STACK;
            } else {
                char *end;
                if ((end = strchr(next, '\n')) != NULL) {
                    *end = '\0';
                }
                *kindp = VM_MAP_OTHER;
            }
        } else {
            *kindp = VM_MAP_ANONYMOUS;
        }
    }

    return PMIX_SUCCESS;
}

#define ALIGN2MB (2*1024*1024UL)

static int use_hole(unsigned long holebegin,
                    unsigned long holesize,
                    unsigned long *addrp,
                    unsigned long size)
{
    unsigned long aligned;
    unsigned long middle = holebegin+holesize/2;

    if (holesize < size) {
        return PMIX_ERROR;
    }

    /* try to align the middle of the hole on 64MB for POWER's 64k-page PMD */
    #define ALIGN64MB (64*1024*1024UL)
    aligned = (middle + ALIGN64MB) & ~(ALIGN64MB-1);
    if (aligned + size <= holebegin + holesize) {
        *addrp = aligned;
        return PMIX_SUCCESS;
    }

    /* try to align the middle of the hole on 2MB for x86 PMD */
    aligned = (middle + ALIGN2MB) & ~(ALIGN2MB-1);
    if (aligned + size <= holebegin + holesize) {
        *addrp = aligned;
        return PMIX_SUCCESS;
    }

    /* just use the end of the hole */
    *addrp = holebegin + holesize - size;
    return PMIX_SUCCESS;
}

static int find_hole(pmix_vm_hole_kind_t hkind,
                     size_t *addrp, size_t size)
{
    unsigned long biggestbegin = 0;
    unsigned long biggestsize = 0;
    unsigned long prevend = 0;
    pmix_vm_map_kind_t prevmkind = VM_MAP_OTHER;
    int in_libs = 0;
    FILE *file;
    char line[96];

    file = fopen("/proc/self/maps", "r");
    if (!file) {
        return PMIX_ERROR;
    }

    while (fgets(line, sizeof(line), file) != NULL) {
        unsigned long begin=0, end=0;
        pmix_vm_map_kind_t mkind=VM_MAP_OTHER;

        if (!parse_map_line(line, &begin, &end, &mkind)) {
            switch (hkind) {
                case VM_HOLE_BEGIN:
                    fclose(file);
                    return use_hole(0, begin, addrp, size);

                case VM_HOLE_AFTER_HEAP:
                    if (prevmkind == VM_MAP_HEAP && mkind != VM_MAP_HEAP) {
                        /* only use HEAP when there's no other HEAP after it
                         * (there can be several of them consecutively).
                         */
                        fclose(file);
                        return use_hole(prevend, begin-prevend, addrp, size);
                    }
                    break;

                case VM_HOLE_BEFORE_STACK:
                    if (mkind == VM_MAP_STACK) {
                        fclose(file);
                        return use_hole(prevend, begin-prevend, addrp, size);
                    }
                    break;

                case VM_HOLE_IN_LIBS:
                    /* see if we are between heap and stack */
                    if (prevmkind == VM_MAP_HEAP) {
                        in_libs = 1;
                    }
                    if (mkind == VM_MAP_STACK) {
                        in_libs = 0;
                    }
                    if (!in_libs) {
                        /* we're not in libs, ignore this entry */
                        break;
                    }
                    /* we're in libs, consider this entry for searching the biggest hole below */
                    /* fallthrough */

                case VM_HOLE_BIGGEST:
                    if (begin-prevend > biggestsize) {
                        biggestbegin = prevend;
                        biggestsize = begin-prevend;
                    }
                    break;

                    default:
                        assert(0);
            }
        }

        while (!strchr(line, '\n')) {
            if (!fgets(line, sizeof(line), file)) {
                goto done;
            }
        }

        if (mkind == VM_MAP_STACK) {
          /* Don't go beyond the stack. Other VMAs are special (vsyscall, vvar, vdso, etc),
           * There's no spare room there. And vsyscall is even above the userspace limit.
           */
          break;
        }

        prevend = end;
        prevmkind = mkind;

    }

  done:
    fclose(file);
    if (hkind == VM_HOLE_IN_LIBS || hkind == VM_HOLE_BIGGEST) {
        return use_hole(biggestbegin, biggestsize, addrp, size);
    }

    return PMIX_ERROR;
}

static int enough_space(const char *filename,
                        size_t space_req,
                        uint64_t *space_avail,
                        bool *result)
{
    uint64_t avail = 0;
    size_t fluff = (size_t)(.05 * space_req);
    bool enough = false;
    char *last_sep = NULL;
    /* the target file name is passed here, but we need to check the parent
     * directory. store it so we can extract that info later. */
    char *target_dir = strdup(filename);
    int rc;

    if (NULL == target_dir) {
        rc = PMIX_ERR_OUT_OF_RESOURCE;
        goto out;
    }
    /* get the parent directory */
    last_sep = strrchr(target_dir, PMIX_PATH_SEP[0]);
    *last_sep = '\0';
    /* now check space availability */
    if (PMIX_SUCCESS != (rc = pmix_path_df(target_dir, &avail))) {
        goto out;
    }
    /* do we have enough space? */
    if (avail >= space_req + fluff) {
        enough = true;
    }

out:
    if (NULL != target_dir) {
        free(target_dir);
    }
    *result = enough;
    *space_avail = avail;
    return rc;
}
