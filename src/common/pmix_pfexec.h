/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2008 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2011-2015 Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2016-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
/**
 * @file
 *
 * The PMIx Fork/Exec Subsystem
 *
 */

#ifndef PMIX_PFEXEC_H
#define PMIX_PFEXEC_H

#include "pmix_config.h"

#include "src/class/pmix_list.h"
#include "src/common/pmix_iof.h"
#include "src/server/pmix_server_ops.h"

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
    /* whether this child is currently on pmix_pfexec_globals.children,
     * and therefore whether the list's reference is still outstanding.
     * Two independent paths take a child off that list - the completion
     * path and the kill sequence - and either can get there first, so
     * neither may assume it is the one doing the removal. Not derived
     * from pmix_list_remove_item's return: that only reports a missing
     * item under PMIX_ENABLE_DEBUG, and in a release build it splices
     * the list using the item's stale pointers instead. */
    bool onlist;
    int exitcode;
    int keepalive[2];
    pmix_pfexec_base_io_conf_t opts;
    pmix_iof_sink_t stdinsink;
    pmix_iof_read_event_t *stdoutev;
    pmix_iof_read_event_t *stderrev;
} pmix_pfexec_child_t;
PMIX_EXPORT PMIX_CLASS_DECLARATION(pmix_pfexec_child_t);

/* Floor and ceiling of the child-reaping poll's backoff, in ms. Also the
 * defaults of the two MCA parameters that override them. */
#define PMIX_PFEXEC_POLL_INTERVAL     50
#define PMIX_PFEXEC_POLL_MAX_INTERVAL 1000

typedef struct {
    /* The child-reaping poll.
     *
     * PMIx is a library, and the signal dispositions of a process belong
     * to whoever wrote its main(), so this used to be wrong twice over:
     * a SIGCHLD event on an event base of ours, whose handler then swept
     * with waitpid(-1). The event took the signal away from the host --
     * libevent keeps ONE process-wide record of which base owns signals
     * (evsig_base in its signal.c), so a second base carrying signals
     * swallows the first's -- and the sweep took the host's *children*,
     * reaping procs it had forked and discarding their exit status,
     * because a pid we do not recognize is silently dropped. Both halves
     * are the same bug in both directions: PRRTE's own SIGCHLD handler
     * does exactly the same sweep, so in one process the two stole from
     * each other and whichever lost simply never heard that its child
     * had died (prrte issue #2709 reached this way too).
     *
     * So we ask instead of being told, and we ask only about our own
     * pids: waitpid(pid, WNOHANG) is a complete answer -- 0 means alive,
     * the pid means dead with a status, and ECHILD means dead with the
     * status lost to whoever reaped first. We never miss a death, only
     * possibly a status, which is strictly better than the sweep (where
     * the loser got no notification at all).
     *
     * The timer exists only while we have children the host asked us to
     * fork, and the delay backs off from poll_interval to
     * poll_max_interval -- there is no latency requirement on noticing
     * that a child has exited, and the IOF EOF path already gives us a
     * prompt free kick via pmix_pfexec_reap_check(). */
    pmix_event_t *poll_ev;
    bool poll_active;
    int poll_delay;
    int poll_interval;
    int poll_max_interval;
    /* set by pmix_pfexec_base_open, cleared by pmix_pfexec_base_close.
     * The children list below only exists between those two calls, and
     * not every role opens this framework, so anything that walks or
     * tears down that list must check this first */
    bool initialized;
    pmix_list_t children;
    int timeout_before_sigkill;
    size_t nextid;
    bool selected;
} pmix_pfexec_globals_t;

PMIX_EXPORT extern pmix_pfexec_globals_t pmix_pfexec_globals;

typedef struct {
    pmix_object_t super;
    pmix_event_t ev;
    pmix_proc_t *proc;
    int signal;
    pmix_lock_t *lock;
    /* used only by the kill sequence to carry the target child across
     * the SIGCONT/SIGTERM/SIGKILL timer stages */
    pmix_pfexec_child_t *child;
} pmix_pfexec_signal_caddy_t;
PMIX_EXPORT PMIX_CLASS_DECLARATION(pmix_pfexec_signal_caddy_t);

PMIX_EXPORT int pmix_pfexec_base_open(void);

PMIX_EXPORT int pmix_pfexec_register(void);

PMIX_EXPORT int pmix_pfexec_base_close(void);

PMIX_EXPORT pmix_status_t pmix_pfexec_base_spawn_job(pmix_setup_caddy_t *fcd);

PMIX_EXPORT void pmix_pfexec_base_spawn_proc(int sd, short args, void *cbdata);

PMIX_EXPORT void pmix_pfexec_base_kill_proc(int sd, short args, void *cbdata);

PMIX_EXPORT void pmix_pfexec_base_signal_proc(int sd, short args, void *cbdata);

PMIX_EXPORT void pmix_pfexec_check_complete(int sd, short args, void *cbdata);

/* Look at our children's exit status right now rather than waiting for
 * the next poll, and reset the backoff. Called from the IOF read handler
 * when a child's last output pipe hits EOF, which is the moment a child
 * that forwards its output usually dies - so the common case costs no
 * timer latency at all. */
PMIX_EXPORT void pmix_pfexec_reap_check(void);

#define PMIX_PFEXEC_SPAWN(fcd)                                           \
    do {                                                                 \
        pmix_event_assign(&(fcd->ev), pmix_globals.evbase, -1, EV_WRITE, \
                          pmix_pfexec_base_spawn_proc, fcd);             \
        PMIX_POST_OBJECT((fcd));                                         \
        pmix_event_active(&((fcd)->ev), EV_WRITE, 1);                    \
    } while (0)

#define PMIX_PFEXEC_KILL(r, lk)                                            \
    do {                                                                   \
        pmix_pfexec_signal_caddy_t *scd;                                   \
        (scd) = PMIX_NEW(pmix_pfexec_signal_caddy_t);                      \
        (scd)->proc = (r);                                                 \
        (scd)->lock = (lk);                                                \
        pmix_event_assign(&((scd)->ev), pmix_globals.evbase, -1, EV_WRITE, \
                          pmix_pfexec_base_kill_proc, (scd));              \
        PMIX_POST_OBJECT((scd));                                           \
        pmix_event_active(&((scd)->ev), EV_WRITE, 1);                      \
    } while (0)

/* "scd" is a caddy pointer the CALLER declares and can still look at
 * afterwards, and "fn" is the handler to run - both as the parameter
 * names say. This used to declare a caddy of its own named "scd",
 * shadowing whatever the caller passed and leaving it untouched, and to
 * ignore "fn" in favor of a hard-coded handler. Nothing calls it, which
 * is the only reason that never showed up. */
#define PMIX_PFEXEC_SIGNAL(scd, r, nm, fn, lk)                             \
    do {                                                                   \
        (scd) = PMIX_NEW(pmix_pfexec_signal_caddy_t);                      \
        (scd)->proc = (r);                                                 \
        (scd)->signal = (nm);                                              \
        (scd)->lock = (lk);                                                \
        pmix_event_assign(&((scd)->ev), pmix_globals.evbase, -1, EV_WRITE, \
                          (fn), (scd));                                    \
        PMIX_POST_OBJECT((scd));                                           \
        pmix_event_active(&((scd)->ev), EV_WRITE, 1);                      \
    } while (0)

typedef struct {
    pmix_object_t super;
    pmix_event_t ev;
    pmix_pfexec_child_t *child;
    pmix_info_t info[2];
} pmix_pfexec_cmpl_caddy_t;
PMIX_EXPORT PMIX_CLASS_DECLARATION(pmix_pfexec_cmpl_caddy_t);

/* The caddy carries the child across an event boundary, so it takes its
 * own reference: between the post and the callback the kill sequence can
 * take this same child off the list and park it for the length of a
 * SIGCONT/SIGTERM/SIGKILL sequence. Released again by
 * pmix_pfexec_check_complete or _ntfy_done. */
#define PMIX_PFEXEC_CHK_COMPLETE(c)                                        \
    do {                                                                   \
        pmix_pfexec_cmpl_caddy_t *pc = PMIX_NEW(pmix_pfexec_cmpl_caddy_t); \
        PMIX_RETAIN((c));                                                  \
        pc->child = (c);                                                   \
        pmix_event_assign(&((pc)->ev), pmix_globals.evbase, -1, EV_WRITE,  \
                          pmix_pfexec_check_complete, (pc));               \
        PMIX_POST_OBJECT((pc));                                            \
        pmix_event_active(&((pc)->ev), EV_WRITE, 1);                       \
    } while (0)

/*
 * Identifies which point in the child's setup/exec sequence failed. The
 * child code that runs between fork() and execve() must be
 * async-signal-safe, so it cannot format a message or call show_help;
 * instead it reports one of these codes (plus errno) and the parent
 * renders the human-readable diagnostic.
 */
typedef enum {
    PMIX_PFEXEC_CHILD_ERR_NONE = 0,
    PMIX_PFEXEC_CHILD_ERR_SETUP, /* pmix_pfexec_base_setup_child failed */
    PMIX_PFEXEC_CHILD_ERR_WDIR,  /* chdir to the app's working dir failed */
    PMIX_PFEXEC_CHILD_ERR_EXEC   /* execve failed */
} pmix_pfexec_child_err_t;

/*
 * Fixed-size record written up the pipe from the child to the parent. It
 * carries no strings and needs no allocation, so it is safe to emit from
 * the async-signal-safe window between fork() and execve(). The parent
 * renders the message from `which` and `errnum`.
 */
typedef struct {
    /* True if the child has died; false if this is just a warning. */
    bool fatal;
    /* Relevant only if fatal==true */
    int exit_status;
    /* which failure occurred (pmix_pfexec_child_err_t) */
    int which;
    /* errno captured at the point of failure (0 if not applicable) */
    int errnum;
} pmix_pfexec_pipe_err_msg_t;

PMIX_EXPORT pmix_status_t pmix_pfexec_base_setup_child(pmix_pfexec_child_t *child);

END_C_DECLS

#endif /* PMIX_PFEXEC_H */
