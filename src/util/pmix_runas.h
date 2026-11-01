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
 *     setgid(), then setuid(), and checks the drop took - and runs fn
 *     there. Only a privileged server can drop to another user; an
 *     unprivileged one fails the drop, and fn never runs. Credentials are
 *     per process, so dropping in place would run the whole server as the
 *     user meanwhile.
 *
 * fn returns a descriptor it opened (>= 0), or -1. With want_fd, that
 * descriptor is the result: from a child it is passed back over a
 * socketpair (SCM_RIGHTS), and the caller owns it. Without, the result
 * is 0 if fn succeeded and -1 if not. A child that cannot become the user
 * is -1 too.
 *
 * fn runs in a forked child of a threaded process: it may only make
 * system calls and touch memory it was given - no locks, no allocation
 * the parent's threads may hold the lock of, no NSS lookups. Look up
 * whatever it needs (the groups, among them) before calling. The parent
 * waits for the child, so this blocks the caller until fn is done */
PMIX_EXPORT int pmix_util_run_as(uid_t uid, gid_t gid, const gid_t *gids, size_t ngids,
                                 bool want_fd, int (*fn)(void *arg), void *arg);

/* True in the child pmix_util_run_as() forks. pmix_output() and
 * pmix_show_help() do nothing there: a lock another of the parent's
 * threads held at the fork stays held in the child, and the parent waits
 * for it. The parent says what went wrong instead */
PMIX_EXPORT extern volatile bool pmix_util_runas_child;

END_C_DECLS

#endif
