/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Whether a server's tmpdirs are ones only it, root, or whoever the host
 * gave them to can change - see docs/how-things-work/ptl.rst, "The
 * tmpdirs".
 *
 * A server makes its local socket's directory, and its pmix-user
 * directory, in its tmpdirs, and clients reach the socket by its path. A
 * user who could rename entries in a tmpdir, or in a directory above it,
 * could move the server's directory aside and put another at the name -
 * and the server's clients would then fail to connect. So at init, before
 * anything is made in them, a server walks each tmpdir's path from "/",
 * as OpenSSH's StrictModes does:
 *
 *   - every directory above the tmpdir must be owned by root, the
 *     server's user, or an id from outside the server's user namespace,
 *     and not writable by group or others - unless it has the sticky bit,
 *     or its group's only member is its owner (a user private group);
 *   - the tmpdir itself may be owned by anyone - a root server is often
 *     given a directory it made for a job's user - provided nobody but
 *     its owner can write it, with the same group exception; or else it
 *     passes the rule for the directories above, as /tmp does.
 *
 * Failing that, the server refuses to start - or only says so, with
 * ptl_base_tmpdir_check=warn, or does not look, with "silent": for a
 * container whose tmpdir is a world-writable volume no other user shares,
 * such as a Kubernetes emptyDir at /tmp.
 */

#include "src/include/pmix_config.h"

#include <errno.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "src/include/pmix_globals.h"
#include "src/mca/ptl/base/base.h"
#include "src/util/pmix_getid.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_printf.h"
#include "src/util/pmix_show_help.h"

pmix_ptl_dir_verdict_t pmix_ptl_base_judge_dir(uid_t owner, mode_t mode, bool last, uid_t euid,
                                               bool owner_unmapped, bool upg)
{
    bool trusted = (0 == owner || euid == owner || owner_unmapped);
    bool only_owner = (0 == (mode & S_IWOTH)) && (0 == (mode & S_IWGRP) || upg);
    bool sticky = (0 != (mode & S_ISVTX));

    if (last) {
        /* whoever owns it, if nobody else can write it */
        if (only_owner) {
            return PMIX_PTL_DIR_OK;
        }
        /* else as a directory above it: /tmp itself */
        if (trusted && sticky) {
            return PMIX_PTL_DIR_OK;
        }
        return PMIX_PTL_DIR_WRITABLE;
    }
    if (!trusted) {
        return PMIX_PTL_DIR_OWNER;
    }
    if (only_owner || sticky) {
        return PMIX_PTL_DIR_OK;
    }
    return PMIX_PTL_DIR_WRITABLE;
}

bool pmix_ptl_base_private_group(uid_t owner, gid_t group)
{
    struct passwd pw, *pwp = NULL;
    struct group gr, *grp = NULL;
    char *pbuf = NULL, *gbuf = NULL;
    long len;
    size_t n;
    bool ok = false;

    len = sysconf(_SC_GETPW_R_SIZE_MAX);
    if (0 >= len) {
        len = 16384;
    }
    pbuf = (char *) malloc((size_t) len);
    if (NULL == pbuf || 0 != getpwuid_r(owner, &pw, pbuf, (size_t) len, &pwp) || NULL == pwp) {
        goto done;
    }
    len = sysconf(_SC_GETGR_R_SIZE_MAX);
    if (0 >= len) {
        len = 16384;
    }
    gbuf = (char *) malloc((size_t) len);
    if (NULL == gbuf || 0 != getgrgid_r(group, &gr, gbuf, (size_t) len, &grp) || NULL == grp) {
        goto done;
    }
    /* every member listed is the owner - and the owner is in it, as a
     * member or by its primary group */
    ok = (pw.pw_gid == group);
    for (n = 0; NULL != gr.gr_mem && NULL != gr.gr_mem[n]; n++) {
        if (0 != strcmp(gr.gr_mem[n], pw.pw_name)) {
            ok = false;
            goto done;
        }
        ok = true;
    }

done:
    free(pbuf);
    free(gbuf);
    return ok;
}

/* an owner the kernel shows from outside our user namespace - made by the
 * host, or the container's setup, so counted as root */
static bool unmapped(uid_t owner)
{
    uint32_t out;

    return PMIX_ERR_NOT_FOUND == pmix_util_id_to_parent("/proc/self/uid_map", (uint32_t) owner,
                                                        &out);
}

/* judge one directory of the path, reporting a failure */
static bool check_dir(const char *what, const char *tmpdir, const char *dir, bool last, bool warn)
{
    struct stat st;
    pmix_ptl_dir_verdict_t v;
    const char *why;

    if (0 != lstat(dir, &st)) {
        /* gone already: nothing here to judge */
        return true;
    }
    v = pmix_ptl_base_judge_dir(st.st_uid, st.st_mode, last, geteuid(), unmapped(st.st_uid),
                                0 != (st.st_mode & S_IWGRP) &&
                                    pmix_ptl_base_private_group(st.st_uid, st.st_gid));
    if (PMIX_PTL_DIR_OK == v) {
        return true;
    }
    why = (PMIX_PTL_DIR_OWNER == v) ? "not owned by root or this server's user"
                                    : "others may write it, and it is not sticky";
    if (warn) {
        pmix_show_help("help-ptl-base.txt", "tmpdir-unsafe-warn", true, what, tmpdir, dir,
                       (unsigned long) st.st_uid, (unsigned) (st.st_mode & 07777), why);
    } else {
        pmix_show_help("help-ptl-base.txt", "tmpdir-unsafe", true, what, tmpdir, dir,
                       (unsigned long) st.st_uid, (unsigned) (st.st_mode & 07777), why);
    }
    return false;
}

/* check one tmpdir; reports and returns false on the first directory of
 * its path that fails */
static bool check_one(const char *what, const char *tmpdir, bool warn)
{
    char resolved[PATH_MAX], *found, *p;
    bool exists = true;
    size_t i;

    /* the path as it is, links resolved - as OpenSSH does. One that is not
     * there yet is made by us below the deepest part that is, and that
     * part is judged as a directory above it */
    found = strdup(tmpdir);
    if (NULL == found) {
        return true;
    }
    while (NULL == realpath(found, resolved)) {
        p = strrchr(found, '/');
        if (ENOENT != errno || NULL == p || p == found) {
            free(found);
            return true;
        }
        *p = '\0';
        exists = false;
    }
    free(found);

    if (0 == strcmp(resolved, "/")) {
        return check_dir(what, tmpdir, "/", exists, warn);
    }
    if (!check_dir(what, tmpdir, "/", false, warn)) {
        return false;
    }
    /* each directory below the root, the last being the tmpdir itself */
    for (i = 1; '\0' != resolved[i]; i++) {
        if ('/' != resolved[i]) {
            continue;
        }
        resolved[i] = '\0';
        if (!check_dir(what, tmpdir, resolved, false, warn)) {
            resolved[i] = '/';
            return false;
        }
        resolved[i] = '/';
    }
    return check_dir(what, tmpdir, resolved, exists, warn);
}

pmix_status_t pmix_ptl_base_check_tmpdirs(void)
{
    bool warn, ok = true;

    if (NULL == pmix_ptl_base.tmpdir_check || 0 == strcmp(pmix_ptl_base.tmpdir_check, "refuse")) {
        warn = false;
    } else if (0 == strcmp(pmix_ptl_base.tmpdir_check, "warn")) {
        warn = true;
    } else if (0 == strcmp(pmix_ptl_base.tmpdir_check, "silent")) {
        return PMIX_SUCCESS;
    } else {
        pmix_show_help("help-ptl-base.txt", "tmpdir-check-bad", true,
                       pmix_ptl_base.tmpdir_check);
        return PMIX_ERR_SILENT;
    }
    if (NULL != pmix_ptl_base.session_tmpdir) {
        ok = check_one("session", pmix_ptl_base.session_tmpdir, warn);
    }
    if (ok && NULL != pmix_ptl_base.system_tmpdir &&
        (NULL == pmix_ptl_base.session_tmpdir ||
         0 != strcmp(pmix_ptl_base.system_tmpdir, pmix_ptl_base.session_tmpdir))) {
        ok = check_one("system", pmix_ptl_base.system_tmpdir, warn);
    }
    return (ok || warn) ? PMIX_SUCCESS : PMIX_ERR_SILENT;
}
