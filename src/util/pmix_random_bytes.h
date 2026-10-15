/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PMIX_RANDOM_BYTES_H
#define PMIX_RANDOM_BYTES_H

#include "src/include/pmix_config.h"

#include <stddef.h>

#include "pmix_common.h"

BEGIN_C_DECLS

/* Fill buf with len bytes from the operating system's cryptographically
 * secure random source - arc4random_buf where the platform has it, else
 * getrandom, else /dev/urandom. pmix_random() is a deterministic
 * generator, and is not such a source.
 *
 * Returns PMIX_SUCCESS, or PMIX_ERROR if no source could supply them, in
 * which case buf holds nothing to use. */
PMIX_EXPORT pmix_status_t pmix_util_random_bytes(void *buf, size_t len);

END_C_DECLS

#endif
