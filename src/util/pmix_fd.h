/*
 * Copyright (c) 2008-2014 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2009      Sandia National Laboratories. All rights reserved.
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 *
 * Copyright (c) 2015      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/* @file */

#ifndef PMIX_UTIL_FD_H_
#define PMIX_UTIL_FD_H_

#include "src/include/pmix_config.h"
#include "pmix_common.h"

BEGIN_C_DECLS

/**
 * Read a complete buffer from a file descriptor.
 *
 * @param fd File descriptor
 * @param len Number of bytes to read
 * @param buffer Pre-allocated buffer (large enough to hold len bytes)
 *
 * @returns PMIX_SUCCESS upon success.
 * @returns PMIX_ERR_BAD_PARAM if len is negative.
 * @returns PMIX_ERR_TIMEOUT if the fd closes before reading the full amount.
 * @returns PMIX_ERR_IN_ERRNO otherwise.
 *
 * Loop over reading from the fd until len bytes are read or an error
 * occurs.  EAGAIN and EINTR are transparently handled.
 *
 * EAGAIN is handled by retrying immediately, so this must only be
 * given a *blocking* fd: on a non-blocking one it becomes a spin loop
 * that burns a core until the peer supplies the data.
 */
PMIX_EXPORT pmix_status_t pmix_fd_read(int fd, int len, void *buffer);

/**
 * Write a complete buffer to a file descriptor.
 *
 * @param fd File descriptor
 * @param len Number of bytes to write
 * @param buffer Buffer to write from
 *
 * @returns PMIX_SUCCESS upon success.
 * @returns PMIX_ERR_BAD_PARAM if len is negative.
 * @returns PMIX_ERR_IN_ERRNO otherwise.
 *
 * Loop over writing to the fd until len bytes are written or an error
 * occurs.  EAGAIN and EINTR are transparently handled - so, as with
 * pmix_fd_read(), the fd must be a blocking one.
 */
PMIX_EXPORT pmix_status_t pmix_fd_write(int fd, int len, const void *buffer);

/**
 * Convenience function to set a file descriptor to be close-on-exec.
 *
 * @param fd File descriptor
 *
 * @returns PMIX_SUCCESS upon success (or if the system does not
 * support close-on-exec behavior).
 * @returns PMIX_ERR_IN_ERRNO otherwise.
 *
 * This is simply a convenience function because there's a few steps
 * to setting a file descriptor to be close-on-exec.
 */
PMIX_EXPORT pmix_status_t pmix_fd_set_cloexec(int fd);

/**
 * Convenience function to check if fd point to an accessible regular file.
 *
 * @param fd File descriptor
 *
 * @returns true if "fd" points to a regular file.
 * @returns false otherwise (including when "fd" cannot be fstat'd).
 */
PMIX_EXPORT bool pmix_fd_is_regular(int fd);

/**
 * Convenience function to check if fd point to an accessible character device.
 *
 * @param fd File descriptor
 *
 * @returns true if "fd" points to a character device.
 * @returns false otherwise (including when "fd" cannot be fstat'd).
 */
PMIX_EXPORT bool pmix_fd_is_chardev(int fd);

/**
 * Convenience function to check if fd point to an accessible block device.
 *
 * @param fd File descriptor
 *
 * @returns true if "fd" points to a block device.
 * @returns false otherwise (including when "fd" cannot be fstat'd).
 */
PMIX_EXPORT bool pmix_fd_is_blkdev(int fd);

/**
 * Close all open file descriptors other than stdin/out/err prior to
 * exec'ing a new binary.
 *
 * @param protected_fd An additional fd to leave open, or -1 for none.
 *
 * Where the OS lists the process's open descriptors (/dev/fd,
 * /proc/self/fd) only those are closed.  Everywhere else this falls
 * back to closing every fd below a bound, which is the smaller of
 * sysconf(_SC_OPEN_MAX) and the pmix_maxfd MCA parameter - see the
 * comment on that fallback for why the cap is needed.
 */
PMIX_EXPORT void pmix_close_open_file_descriptors(int protected_fd);

/**
 * Return a printable form of the address at the far end of a socket.
 *
 * @param fd A connected socket.
 *
 * @returns A NUL-terminated string, never NULL.  A peer that cannot be
 * named - getpeername failed, or the address family is not one this
 * renders - comes back as "Unknown" rather than as an error.
 *
 * The string lives in a single file-static buffer, so the result is
 * only valid until the next call and this is not safe to call from two
 * threads at once.  Copy it if you need to keep it.
 */
PMIX_EXPORT const char *pmix_fd_get_peer_name(int fd);

/**
 * dup2(2) with the standard streams named by number.
 *
 * @param src The fd to duplicate.
 * @param target 0, 1 or 2 to mean stdin/stdout/stderr as this process's
 * stdio actually has them, or any other fd number to pass straight to
 * dup2().
 *
 * @returns whatever dup2() returned: the new descriptor, or -1 with
 * errno set.  Note this is NOT a pmix_status_t.
 */
PMIX_EXPORT int pmix_fd_dup2(int src, int target);

END_C_DECLS

#endif
