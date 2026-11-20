/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * Where a test that runs as root puts files another user must reach.
 *
 * Such a test makes its scratch directory under $TMPDIR and gives it to,
 * or opens it to, a user other than root - a job user, a server's user, a
 * helper that creates files.  That user is neither the owner nor in the
 * group of the directories above it, so each of them has to let others
 * search it.  "mktemp -d" makes a 0700 directory, and TMPDIR=$(mktemp -d)
 * is the documented way to run this suite clean: run as root, it leaves
 * the other user unable to reach anything below it.
 */

#ifndef PMIX_TEST_SCRATCH_BASE_H
#define PMIX_TEST_SCRATCH_BASE_H

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* can a user who is neither the owner nor in the group of any directory on
 * the way reach "path"? */
static inline bool pmix_test_searchable_by_others(const char *path)
{
    char walk[512], *slash;
    struct stat sb;

    if (NULL == path || '/' != path[0] || strlen(path) >= sizeof(walk)) {
        return false;
    }
    snprintf(walk, sizeof(walk), "%s", path);
    for (;;) {
        if (0 != stat(walk, &sb) || !S_ISDIR(sb.st_mode) || 0 == (sb.st_mode & S_IXOTH)) {
            return false;
        }
        slash = strrchr(walk, '/');
        if (NULL == slash || slash == walk) {
            break;
        }
        *slash = '\0';
    }
    return (0 == stat("/", &sb) && 0 != (sb.st_mode & S_IXOTH));
}

/* the directory to make a scratch directory in that other users must
 * reach: $TMPDIR (or /tmp) when they can search it, else /tmp, saying so;
 * NULL when they cannot reach /tmp either */
static inline const char *pmix_test_scratch_base(void)
{
    const char *base = getenv("TMPDIR");

    if (NULL == base || '\0' == base[0]) {
        base = "/tmp";
    }
    if (pmix_test_searchable_by_others(base)) {
        return base;
    }
    if (0 != strcmp(base, "/tmp")) {
        fprintf(stdout, "  NOTE: other users cannot search %s - using /tmp instead\n", base);
        if (pmix_test_searchable_by_others("/tmp")) {
            return "/tmp";
        }
    }
    return NULL;
}

#endif /* PMIX_TEST_SCRATCH_BASE_H */
