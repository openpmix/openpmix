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
 * Copyright (c) 2008      Sun Microsystems, Inc.  All rights reserved.
 * Copyright (c) 2019-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2022      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/** @file:
 *
 */

#ifndef _PMIX_CONTEXT_FNS_H_
#define _PMIX_CONTEXT_FNS_H_

#include "src/include/pmix_config.h"
#include "pmix_common.h"

BEGIN_C_DECLS

/* Move the calling process to the working directory an app asked for,
 * and report where it ended up.
 *
 * "want_chdir" false makes this a no-op that reports success: the only
 * check performed IS the chdir, so there is nothing to test without it.
 *
 * On failure to reach *incwd, what happens next depends on who chose the
 * directory. If "user_cwd" is set the user named it explicitly, so the
 * request cannot be honored and PMIX_ERR_JOB_WDIR_NOT_ACCESSIBLE is
 * returned. Otherwise it was a system-supplied default and going
 * elsewhere is acceptable: the process moves to $HOME and *incwd is
 * freed and replaced with a copy of it, so the caller must own the
 * string it passes in and takes ownership of whatever comes back.
 *
 * NOTE that chdir() moves the whole process, not one thread, and this
 * routine calls getpwuid(), which is not reentrant. Both make it a
 * caller-serialization problem - see the pmix_context_fns notes in
 * src/util/AGENTS.md.
 */
PMIX_EXPORT pmix_status_t pmix_util_check_context_cwd(char **incwd,
                                                      bool want_chdir,
                                                      bool user_cwd);

/* Resolve an app's executable to something that can actually be run,
 * and report it back through *incmd.
 *
 * A naked filename is searched for along "env"'s PATH, with "cwd" as the
 * directory relative paths in that PATH resolve against; on success
 * *incmd is freed and replaced with the full path found, so the caller
 * must own the string it passes in and takes ownership of the result. A
 * name that already carries a path is only checked for executability,
 * and a RELATIVE one is checked against the process's current directory
 * rather than "cwd" - callers reach here having already moved there with
 * pmix_util_check_context_cwd().
 *
 * Returns PMIX_ERR_JOB_EXE_NOT_FOUND if the search came up empty, or
 * PMIX_ERR_EXE_NOT_ACCESSIBLE if the named file cannot be executed.
 */
PMIX_EXPORT pmix_status_t pmix_util_check_context_app(char **incmd,
                                                      char *cwd,
                                                      char **env);

END_C_DECLS
#endif
