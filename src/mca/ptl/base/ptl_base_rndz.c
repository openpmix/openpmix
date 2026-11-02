/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Where rendezvous files are written and looked for - see
 * docs/how-things-work/ptl.rst, "Rendezvous files".
 *
 * A server writes the files tools find it by - "<host>.tool", by pid, by
 * namespace - in a directory of its user's own beneath the system tmpdir,
 * pmix-user.<uid>, whatever its session tmpdir is: so only that user can
 * put a file at those names, and a tool has one place to look. A root
 * system server, scheduler or system controller writes its file in
 * /run/pmix (/var/run/pmix where there is no /run), which only root can
 * write; a non-root one in its own pmix-user directory. A host that names
 * the system tmpdir gets the system files there instead, as it always did.
 *
 * A reader looks in its own user's directory, root's, and those of the
 * users ptl_base_server_access names - the users whose servers it may talk
 * to - and, for the system roles, the system directory first. The walk of the
 * whole system tmpdir that finds a server of an earlier release is made
 * only with legacy peers on.
 */

#include "src/include/pmix_config.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "src/include/pmix_globals.h"
#include "src/mca/ptl/base/base.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_os_dirpath.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_printf.h"
#include "src/util/pmix_show_help.h"

char *pmix_ptl_base_user_dir(uid_t uid)
{
    char *path = NULL;

    if (NULL == pmix_ptl_base.system_tmpdir ||
        0 > pmix_asprintf(&path, "%s/%s%lu", pmix_ptl_base.system_tmpdir,
                          PMIX_PTL_USER_DIR_PREFIX, (unsigned long) uid)) {
        return NULL;
    }
    return path;
}

/* The directory a server of ours may write in: `name` under `parentfd`,
 * made with `mode` if it is not there. One that is there is used only if
 * it is a directory - not a link to one - that `owner` owns and that
 * nobody else can write; it is then given `mode`. Returns a descriptor on
 * it, or -1 with *found saying what was there instead */
static int own_dir(int parentfd, const char *name, uid_t owner, mode_t mode, struct stat *found)
{
    int fd;

    memset(found, 0, sizeof(*found));
    if (0 != mkdirat(parentfd, name, mode) && EEXIST != errno) {
        return -1;
    }
    fd = openat(parentfd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (0 > fd) {
        /* a link, or something else, at the name */
        (void) fstatat(parentfd, name, found, AT_SYMLINK_NOFOLLOW);
        return -1;
    }
    if (0 != fstat(fd, found) || !S_ISDIR(found->st_mode) || owner != found->st_uid ||
        0 != (found->st_mode & (S_IWGRP | S_IWOTH))) {
        close(fd);
        return -1;
    }
    /* ours - so the mode is ours to set, whatever the umask made it */
    if ((found->st_mode & 07777) != mode) {
        (void) fchmod(fd, mode);
    }
    return fd;
}

/* say once that a directory we write rendezvous files in is not ours */
static void not_ours(const char *path, const struct stat *found, const char *what)
{
    static bool said = false;
    char detail[128];

    if (said) {
        return;
    }
    said = true;
    if (0 == found->st_mode) {
        snprintf(detail, sizeof(detail), "it could not be made or opened: %s",
                 strerror(errno));
    } else if (S_ISLNK(found->st_mode)) {
        snprintf(detail, sizeof(detail), "it is a link, owned by uid %lu",
                 (unsigned long) found->st_uid);
    } else {
        snprintf(detail, sizeof(detail), "it is %s owned by uid %lu, mode %04o",
                 S_ISDIR(found->st_mode) ? "a directory" : "not a directory",
                 (unsigned long) found->st_uid, (unsigned) (found->st_mode & 07777));
    }
    pmix_show_help("help-ptl-base.txt", "rndz-dir-not-ours", true, path, detail, what);
}

int pmix_ptl_base_open_user_dir(char **path)
{
    struct stat found;
    char name[64];
    mode_t mode;
    int pfd, fd;

    *path = pmix_ptl_base_user_dir(geteuid());
    if (NULL == *path) {
        return -1;
    }
    /* a tool of another user we take must be able to open our files at
     * their names - not list them */
    mode = S_IRWXU;
    if (pmix_ptl_base.allow_foreign_tools) {
        mode |= S_IXGRP | S_IXOTH;
    }
    snprintf(name, sizeof(name), "%s%lu", PMIX_PTL_USER_DIR_PREFIX, (unsigned long) geteuid());
    pfd = pmix_os_dirpath_open_dir(pmix_ptl_base.system_tmpdir);
    if (0 > pfd) {
        memset(&found, 0, sizeof(found));
        not_ours(*path, &found, "tools of this user cannot find this server by its files");
        return -1;
    }
    fd = own_dir(pfd, name, geteuid(), mode, &found);
    close(pfd);
    if (0 > fd) {
        not_ours(*path, &found, "tools of this user cannot find this server by its files");
    }
    return fd;
}

/* /run/pmix, else /var/run/pmix: where /run, or /var/run, is a directory
 * only root can write */
static int open_run_dir(char **path)
{
    const char *runs[] = {"/run", "/var/run", NULL};
    struct stat st, found;
    int n, pfd, fd;

    for (n = 0; NULL != runs[n]; n++) {
        pfd = open(runs[n], O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (0 > pfd) {
            continue;
        }
        if (0 != fstat(pfd, &st) || 0 != st.st_uid || 0 != (st.st_mode & (S_IWGRP | S_IWOTH))) {
            close(pfd);
            continue;
        }
        fd = own_dir(pfd, "pmix", 0, S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH, &found);
        close(pfd);
        if (0 <= fd) {
            if (0 > pmix_asprintf(path, "%s/pmix", runs[n])) {
                close(fd);
                return -1;
            }
            return fd;
        }
    }
    return -1;
}

int pmix_ptl_base_open_system_dir(char **path)
{
    int fd;

    *path = NULL;
    /* a system tmpdir the host named is the host's choice, as before */
    if (pmix_ptl_base.system_tmpdir_given) {
        fd = pmix_os_dirpath_open_dir(pmix_ptl_base.system_tmpdir);
        if (0 <= fd && NULL == (*path = strdup(pmix_ptl_base.system_tmpdir))) {
            close(fd);
            fd = -1;
        }
        return fd;
    }
    if (0 == geteuid()) {
        fd = open_run_dir(path);
        if (0 <= fd) {
            return fd;
        }
    }
    /* a non-root one - or root where /run cannot be used - in its own */
    return pmix_ptl_base_open_user_dir(path);
}

/* append a directory, once */
static void add_dir(char ***dirs, char *dir)
{
    int n;

    if (NULL == dir) {
        return;
    }
    for (n = 0; NULL != *dirs && NULL != (*dirs)[n]; n++) {
        if (0 == strcmp((*dirs)[n], dir)) {
            free(dir);
            return;
        }
    }
    PMIx_Argv_append_nosize(dirs, dir);
    free(dir);
}

char **pmix_ptl_base_rndz_dirs(bool system_role)
{
    char **dirs = NULL;
    size_t n;

    if (system_role) {
        /* a system tmpdir that was named, first - and /run/pmix whether or
         * not one was: a process a server started is handed the server's
         * system tmpdir in its environment, named or not, so it cannot
         * tell which the server had, and only root can write there */
        if (pmix_ptl_base.system_tmpdir_given) {
            add_dir(&dirs, strdup(pmix_ptl_base.system_tmpdir));
        }
        add_dir(&dirs, strdup("/run/pmix"));
        add_dir(&dirs, strdup("/var/run/pmix"));
        /* root's own, where /run could not be used - and our own user's,
         * for a system server our user runs */
        add_dir(&dirs, pmix_ptl_base_user_dir(0));
        add_dir(&dirs, pmix_ptl_base_user_dir(geteuid()));
    } else {
        add_dir(&dirs, pmix_ptl_base_user_dir(geteuid()));
        add_dir(&dirs, pmix_ptl_base_user_dir(0));
    }
    for (n = 0; n < pmix_ptl_base.naccess_uids; n++) {
        add_dir(&dirs, pmix_ptl_base_user_dir(pmix_ptl_base.access_uids[n]));
    }
    return dirs;
}

bool pmix_ptl_base_legacy_walk(void)
{
#if PMIX_ENABLE_LEGACY_PEERS
    return pmix_ptl_base.legacy_peers || pmix_ptl_base.legacy_peers_param;
#else
    return false;
#endif
}

bool pmix_ptl_base_system_owner_ok(const pmix_connection_t *cn)
{
    size_t n;

    if (!cn->owner_known) {
        return false;
    }
    if (0 == cn->owner || geteuid() == cn->owner) {
        return true;
    }
    for (n = 0; n < pmix_ptl_base.naccess_uids; n++) {
        if (pmix_ptl_base.access_uids[n] == cn->owner) {
            return true;
        }
    }
    return false;
}
