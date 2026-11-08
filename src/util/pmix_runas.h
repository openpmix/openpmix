/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PMIX_UTIL_RUNAS_H
#define PMIX_UTIL_RUNAS_H

#include "src/include/pmix_config.h"
#include "pmix_common.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <sys/types.h>

BEGIN_C_DECLS

/* Work done as another user, so that the kernel - not a check of ours
 * racing the filesystem - decides what that user may reach.
 *
 * fn(arg) runs with uid, gid and the supplementary groups gids/ngids:
 *
 *   - in place, when this process already is that uid and gid. A
 *     per-user server is, and pays nothing;
 *   - otherwise in a forked child that drops to them - setgroups(), then
 *     setgid(), then setuid(), and checks the drop took; on Linux it
 *     then clears any capability it still holds, and gives up if one
 *     remains - and runs fn there. The child's groups are those
 *     pmix_util_runas_groups() makes of gid and gids. Only a privileged
 *     server can drop to another user; an unprivileged one fails the
 *     drop, and fn never runs. Credentials are per process, so dropping
 *     in place would run the whole server as the user meanwhile.
 *
 * fn returns a descriptor it opened (>= 0), or -1. With want_fd, that
 * descriptor is the result: from a child it is passed back over a
 * socketpair (SCM_RIGHTS), and the caller owns it. Without, the result
 * is 0 if fn succeeded and -1 if not. A child that cannot become the user
 * is -1 too.
 *
 * fn runs in a forked child of a threaded process, so it may take no
 * lock that another of the parent's threads may have held at the fork
 * and that nothing releases in the child: none of the library's own
 * (pmix_output() and pmix_show_help() do nothing there - see
 * pmix_util_runas_child), no stdio, and no NSS lookup. Look up whatever
 * it needs (the groups, among them) before calling, and use
 * PMIX_RUNAS_STRERROR() in place of strerror(). It may allocate, and use
 * the directory calls that do (fdopendir(), readdir()): glibc, musl,
 * macOS and FreeBSD each hold their allocator's locks across fork() so
 * that a child can. A child that blocks all the same is ended by the
 * timeout below.
 *
 * The caller is blocked until the child answers, but never for longer
 * than pmix_util_runas_timeout seconds. The child works on a path the
 * user chose, which may be on a filesystem that does not respond, and
 * the caller is usually the progress thread. A child that has not answered by then is
 * killed, and the result is -1 with errno ETIMEDOUT. A caller that may
 * be asked again for the same user's work should take that as an answer
 * for all of it, or each request costs the full timeout. The child is
 * reaped without waiting; one that has not gone yet is reaped later */
PMIX_EXPORT int pmix_util_run_as(uid_t uid, gid_t gid, const gid_t *gids, size_t ngids,
                                 bool want_fd, int (*fn)(void *arg), void *arg);

/* The supplementary groups pmix_util_run_as() gives its child, written
 * to out, which has room for max: gid first, then each of gids that is
 * not gid, in order, until out is full. Returns how many were written.
 *
 * gid leads because on BSD and macOS the first entry of the list given
 * to setgroups() is the effective gid, which the setgid() after it then
 * replaces; a group put there is lost. max is the most setgroups()
 * takes (sysconf(_SC_NGROUPS_MAX)): the call fails outright on a longer
 * list, so the groups past it are left out */
PMIX_EXPORT size_t pmix_util_runas_groups(gid_t gid, const gid_t *gids, size_t ngids,
                                          gid_t *out, size_t max);

/* Seconds pmix_util_run_as() waits for a child - MCA pmix_runas_timeout.
 * 0 waits without a bound */
PMIX_EXPORT extern int pmix_util_runas_timeout;

/* Reap the children pmix_util_run_as() stopped waiting for. Called from
 * pmix_rte_finalize() */
PMIX_EXPORT void pmix_util_runas_finalize(void);

/* TESTING ONLY. Set, a child pmix_util_run_as() forks never gets as far
 * as fn: once it has become the user it waits for a signal, for ever -
 * which is what a child working on a path that does not answer looks
 * like. test/unit/iof_file_perms.c sets it to reach the timeout through
 * the code that opens a job's output files. Nothing else may */
PMIX_EXPORT extern volatile bool pmix_util_runas_test_stall;

/* True in the child pmix_util_run_as() forks. pmix_output() and
 * pmix_show_help() do nothing there: a lock another of the parent's
 * threads held at the fork stays held in the child, and the parent waits
 * for it. The parent says what went wrong instead */
PMIX_EXPORT extern volatile bool pmix_util_runas_child;

/* strerror(err), or "" in the child pmix_util_run_as() forks. A message
 * is not shown there, and strerror() may look its text up under a lock
 * of the C library's that the child does not get back */
#define PMIX_RUNAS_STRERROR(err) (pmix_util_runas_child ? "" : strerror(err))

END_C_DECLS

#endif
