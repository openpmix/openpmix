/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
/**
 * @file
 *
 * The variables a server or launcher puts in the environment of a process
 * it starts, to say who the process is and how it reaches its server:
 * PMIX_NAMESPACE, PMIX_RANK, PMIX_SERVER_NSPACE, PMIX_SERVER_RANK, every
 * PMIX_SERVER_URI<version>, PMIX_SERVER_LOCAL_URI, PMIX_CONNECT_TOKEN,
 * PMIX_LAUNCHER_RNDZ_URI, PMIX_SECURITY_MODE, PMIX_BFROP_BUFFER_TYPE,
 * PMIX_GDS_MODULE and PMIX_VERSION.
 *
 * A client or tool takes them out of its environment as it initializes,
 * so a process it starts - forked, or forked and exec'd - and a program it
 * execs inherit none of them. The library keeps them for its own process:
 * the init reads them from here, and so does a later init of the same
 * process. A process forked from it is not that process, and finds none.
 */

#ifndef PMIX_IDENV_H
#define PMIX_IDENV_H

#include "src/include/pmix_config.h"

#include "pmix_common.h"

BEGIN_C_DECLS

/* Take the variables out of the environment - every entry of each name -
 * and keep them, each as getenv() answers it. If the environment holds
 * PMIX_NAMESPACE, what it holds replaces everything kept already;
 * otherwise each variable it holds replaces the kept one of its name,
 * and if it holds none, what was kept stands. If they cannot all be kept,
 * none is taken. The
 * token's value is overwritten where the environment held it, wherever
 * this process may write. Called by PMIx_Init and PMIx_tool_init before
 * they read any of them */
PMIX_EXPORT void pmix_idenv_take(void);

/* The value of one of the variables: the one kept for this process, else
 * the environment's - a server takes none out of its own. NULL if
 * neither has it */
PMIX_EXPORT const char *pmix_idenv_get(const char *name);

/* Whether an environment entry, "name=value", is one of the variables */
PMIX_EXPORT bool pmix_idenv_is_identity(const char *entry);

END_C_DECLS

#endif
