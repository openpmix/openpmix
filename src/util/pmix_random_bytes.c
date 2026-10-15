/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "src/include/pmix_config.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif
#ifdef HAVE_SYS_RANDOM_H
#    include <sys/random.h>
#endif

#include "src/util/pmix_random_bytes.h"

#if !defined(HAVE_ARC4RANDOM_BUF)
/* read len bytes of the kernel's random device */
static pmix_status_t from_urandom(unsigned char *p, size_t len)
{
    ssize_t n;
    int fd;

    fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (0 > fd) {
        return PMIX_ERROR;
    }
    while (0 < len) {
        n = read(fd, p, len);
        if (0 > n && EINTR == errno) {
            continue;
        }
        if (0 >= n) {
            close(fd);
            return PMIX_ERROR;
        }
        p += n;
        len -= (size_t) n;
    }
    close(fd);
    return PMIX_SUCCESS;
}
#endif

pmix_status_t pmix_util_random_bytes(void *buf, size_t len)
{
#if defined(HAVE_ARC4RANDOM_BUF)
    arc4random_buf(buf, len);
    return PMIX_SUCCESS;
#else
    unsigned char *p = (unsigned char *) buf;
#    if defined(HAVE_GETRANDOM)
    ssize_t n;

    while (0 < len) {
        n = getrandom(p, len, 0);
        if (0 > n && EINTR == errno) {
            continue;
        }
        if (0 > n) {
            /* a kernel without the call: its random device has the same
             * bytes */
            return from_urandom(p, len);
        }
        p += n;
        len -= (size_t) n;
    }
    return PMIX_SUCCESS;
#    else
    return from_urandom(p, len);
#    endif
#endif
}
