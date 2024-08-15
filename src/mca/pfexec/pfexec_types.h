/*
 * Copyright (c) 2004-2006 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2011 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2013      Los Alamos National Security, LLC.  All rights reserved.
 * Copyright (c) 2017-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021-2024 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
/** @file:
 */

#ifndef MCA_PFEXEC_TYPES_H
#define MCA_PFEXEC_TYPES_H

/*
 * includes
 */
#include "pmix_config.h"

#include "src/class/pmix_list.h"
#include "src/common/pmix_iof.h"
#include "src/mca/mca.h"

BEGIN_C_DECLS

typedef struct {
    int usepty;
    bool connect_stdin;

    /* private - callers should not modify these fields */
    int p_stdin[2];
    int p_stdout[2];
    int p_stderr[2];
} pmix_pfexec_base_io_conf_t;

typedef struct {
    pmix_list_item_t super;
    pmix_event_t ev;
    pmix_proc_t proc;
    pid_t pid;
    bool completed;
    int exitcode;
    int keepalive[2];
    pmix_pfexec_base_io_conf_t opts;
    pmix_iof_sink_t stdinsink;
    pmix_iof_read_event_t *stdoutev;
    pmix_iof_read_event_t *stderrev;
} pmix_pfexec_child_t;
PMIX_EXPORT PMIX_CLASS_DECLARATION(pmix_pfexec_child_t);

/* define a function that will fork/exec a local proc */
typedef pmix_status_t (*pmix_pfexec_base_fork_proc_fn_t)(pmix_app_t *app,
                                                         pmix_pfexec_child_t *child, char **env);

/* define a function type for signaling a local proc */
typedef pmix_status_t (*pmix_pfexec_base_signal_local_fn_t)(pid_t pd, int signum);

typedef struct {
    pmix_object_t super;
    pmix_event_t ev;
    pmix_peer_t *peer;
    pmix_info_t *jobinfo;
    size_t njinfo;
    pmix_app_t *apps;
    size_t napps;
    pmix_iof_channel_t channels;
    pmix_iof_flags_t flags;
    pmix_pfexec_base_fork_proc_fn_t frkfn;
    pmix_spawn_cbfunc_t cbfunc;
    void *cbdata;
} pmix_pfexec_fork_caddy_t;
PMIX_EXPORT PMIX_CLASS_DECLARATION(pmix_pfexec_fork_caddy_t);

END_C_DECLS
#endif
