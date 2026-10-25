/*
 * Copyright (c) 2016-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PMIX_GETID_H
#define PMIX_GETID_H

#include "src/include/pmix_config.h"
#include "pmix_common.h"

#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif
#ifdef HAVE_SYS_TYPES_H
#    include <sys/types.h>
#endif

BEGIN_C_DECLS

/* Ask the kernel for the credentials of the process on the far end of
 * the connected socket `sd`, and write them to *uid and *gid.
 *
 * Returns PMIX_SUCCESS, in which case both out-parameters have been
 * written; PMIX_ERR_INVALID_CRED if the descriptor has no credentials
 * to report, in which case neither has; or PMIX_ERR_NOT_SUPPORTED on a
 * platform that offers no way to ask. Only a connected AF_UNIX socket
 * gives a meaningful answer - see src/util/AGENTS.md. */
PMIX_EXPORT pmix_status_t pmix_util_getid(int sd, uid_t *uid, gid_t *gid);

/* Ask the kernel for the pid of the process on the far end of the
 * connected socket `sd`, as this process's pid namespace numbers it, and
 * write it to *pid.
 *
 * Returns PMIX_SUCCESS, in which case *pid has been written;
 * PMIX_ERR_NOT_FOUND if the descriptor has no pid to report - a TCP
 * socket has none; or PMIX_ERR_NOT_SUPPORTED on a platform that offers
 * no way to ask. *pid is written only on PMIX_SUCCESS. */
PMIX_EXPORT pmix_status_t pmix_util_getpid(int sd, pid_t *pid);

/* Ask the kernel which user owns the far end of the connected TCP
 * socket `sd`, and write it to *uid. This works only when that far end
 * is a socket on this host: the answer comes from the kernel's own table
 * of TCP connections, looked up by the connection's addresses and ports,
 * so it is the uid that created the peer's socket - not anything the
 * peer said. The kernel does not record a group, so none is returned.
 * When the peer reaches us through a relay on this host - a process
 * that accepts its connection and opens one of its own to us - the far
 * end is the relay's socket, and the uid is the relay's.
 *
 * Returns:
 *   PMIX_SUCCESS            *uid holds the owner
 *   PMIX_ERR_NOT_FOUND      no established connection with that address
 *                           pair exists in this host's table - the peer is
 *                           on another host or in another network
 *                           namespace, or has already gone away
 *   PMIX_ERR_NOT_SUPPORTED  this platform offers no way to ask
 *   PMIX_ERR_OUT_OF_RESOURCE  it does, but this process could not ask it
 *                           now - it is out of descriptors or memory, or
 *                           the table kept growing past the buffer. No
 *                           answer, and not to be taken for the one above
 *   PMIX_ERR_BAD_PARAM      `sd` is not a connected IPv4/IPv6 socket
 *
 * *uid is written only on PMIX_SUCCESS. */
PMIX_EXPORT pmix_status_t pmix_util_getid_tcp(int sd, uid_t *uid);

/* For the unit tests alone, and nothing outside a test's own code sets
 * it: pmix_util_getid_tcp() answers as on a platform with no table of TCP
 * connections */
PMIX_EXPORT extern bool pmix_util_getid_test_no_tcp_table;

/* An id the kernel reported, in this process's user namespace, as the
 * parent of that namespace numbers it - where a server outside it runs.
 * The kernel reports a peer's ids translated into the reader's namespace,
 * by the mapping in `mapfile` (/proc/self/uid_map or gid_map): lines of
 * "<first id inside> <first id outside> <count>". In the initial
 * namespace that mapping is the identity.
 *
 * Returns PMIX_SUCCESS with *out written; PMIX_ERR_NOT_FOUND if no line
 * maps `id`; PMIX_ERR_NOT_SUPPORTED where there is no such file - a
 * platform without user namespaces. */
PMIX_EXPORT pmix_status_t pmix_util_id_to_parent(const char *mapfile, uint32_t id,
                                                 uint32_t *out);

END_C_DECLS

#endif /* PMIX_GETID_H */
