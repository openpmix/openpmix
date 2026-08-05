/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2015-2017 Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2016-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "pmix_config.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif /* HAVE_UNISTD_H */
#include <stdlib.h>
#if HAVE_SYS_STAT_H
#    include <sys/stat.h>
#endif /* HAVE_SYS_STAT_H */
#ifdef HAVE_SYS_TYPES_H
#    include <sys/types.h>
#endif /* HAVE_SYS_TYPES_H */
#ifdef HAVE_DIRENT_H
#    include <dirent.h>
#endif /* HAVE_DIRENT_H */

#include "pmix_common.h"
#include "src/include/pmix_globals.h"
#include "src/mca/ptl/base/base.h"
#include "src/server/pmix_server_ops.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_os_dirpath.h"
#include "src/util/pmix_os_path.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_show_help.h"

static const char path_sep[] = PMIX_PATH_SEP;

/**
 * The named path already exists: make sure it really is a directory,
 * and give it (at least) the requested mode bits.
 *
 * The inspection and the mode change are both done through a
 * descriptor, so the object that gets inspected is the object that
 * gets modified. A path-based stat()/chmod() pair is the classic
 * TOCTOU race: the path can be swapped between the two calls,
 * redirecting the chmod onto an object that was never inspected. The
 * paths that reach this code are the rendezvous and session
 * directories, whose names are fully predictable and whose default
 * root (/tmp) is world-writable, so the race is not theoretical.
 *
 * O_DIRECTORY: a plain file sitting at the requested name is an error
 * rather than something we chmod and report as usable.
 *
 * O_NOFOLLOW: refuse a symlink planted at the final component.
 * Following one would point every subsequent operation at the link's
 * *target* -- which an attacker gets to choose, which we would chmod,
 * and which pmix_os_dirpath_destroy() would later recurse into.
 *
 * Note that we deliberately do not ask whether the directory is
 * writable. Whether the caller can put files here is settled by the
 * caller actually creating one; testing for permission first and
 * acting on the answer afterwards is itself a time-of-check /
 * time-of-use race, and one that access()/faccessat() cannot avoid
 * no matter which uid they resolve against.
 *
 * @retval PMIX_SUCCESS       It is a directory; mode is now adequate.
 * @retval PMIX_ERR_NOT_FOUND It vanished under us. No error has been
 *                            displayed; the caller is expected to
 *                            retry or report.
 * @retval PMIX_ERR_SILENT    Refused; an error has been displayed.
 */
static int dirpath_ensure_mode(const char *path, const mode_t mode)
{
    struct stat buf;
    int fd, ret;

    fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (0 > fd) {
        if (ENOENT == errno) {
            return PMIX_ERR_NOT_FOUND;
        }
        /* ELOOP (symlink) and ENOTDIR (plain file) land here:
         * something that is not a directory is occupying the name we
         * were asked to create */
        pmix_show_help("help-pmix-util.txt", "mkdir-failed", true,
                       path, strerror(errno));
        return PMIX_ERR_SILENT;
    }

    if (0 == fstat(fd, &buf) && mode != (mode & buf.st_mode)) {
        // try to add the requested bits.
        // Silently fail the chmod if it hits an error - we'll
        // let us fail later when we try to actually create a
        // file if we aren't allowed to do so. However, we have
        // to capture the return to silence static code
        // analyzer complaints
        ret = fchmod(fd, buf.st_mode | mode);
        if (0 != ret) {
            pmix_output_verbose(2, pmix_globals.debug_output,
                                "PATH %s ALREADY EXISTS AND CHMOD FAILED: %s",
                                path, strerror(errno));
        }
    }
    close(fd);
    return PMIX_SUCCESS;
}

int pmix_os_dirpath_create(const char *path, const mode_t mode)
{
    char **parts, *tmp;
    int i, len;
    int ret;

    if (NULL == path) { /* protect ourselves from errors */
        return (PMIX_ERR_BAD_PARAM);
    }

    /* try to make directory */
    if (0 == mkdir(path, mode)) {
        return (PMIX_SUCCESS);
    }
    ret = errno; // preserve the error

    /* check the error */
    if (EEXIST == ret) {
        // already exists - make sure it really is a directory, and
        // that it carries the requested mode
        ret = dirpath_ensure_mode(path, mode);
        if (PMIX_SUCCESS == ret) {
            return PMIX_ERR_EXISTS;
        }
        if (PMIX_ERR_NOT_FOUND != ret) {
            return ret;
        }
        /* it vanished between the mkdir and the open - fall through
         * and build the tree, which will report any real error */
        ret = ENOENT;
    }
    if (ENOENT != ret) {
        // cannot create it
        pmix_show_help("help-pmix-util.txt", "mkdir-failed", true,
                       path, strerror(ret));
        return PMIX_ERR_SILENT;
    }

    /* didn't work, so now have to build our way down the tree */
    /* Split the requested path up into its individual parts */

    parts = PMIx_Argv_split(path, path_sep[0]);

    /* Ensure to allocate enough space for tmp: the strlen of the
       incoming path + 1 (for \0) */

    tmp = (char *) calloc((strlen(path) + 1), sizeof(char));
    if (NULL == tmp) {
        PMIx_Argv_free(parts);
        return PMIX_ERR_OUT_OF_RESOURCE;
    }
    tmp[0] = '\0';

    /* Iterate through all the subdirectory names in the path,
       building up a directory name.  Check to see if that dirname
       exists.  If it doesn't, create it. */

    len = PMIx_Argv_count(parts);
    for (i = 0; i < len; ++i) {
        if (i == 0) {
            /* If in POSIX-land, ensure that we never end a directory
               name with path_sep */

            if ('/' == path[0]) {
                strcat(tmp, path_sep);
            }
            strcat(tmp, parts[i]);
        }

        /* If it's not the first part, ensure that there's a
           preceding path_sep and then append this part */

        else {
            if (path_sep[0] != tmp[strlen(tmp) - 1]) {
                strcat(tmp, path_sep);
            }
            strcat(tmp, parts[i]);
        }

        /* Now that we have the name, try to create it */
        ret = mkdir(tmp, mode);
        if (0 == ret) {
            continue;
        }
        if (EEXIST != errno) {
            // true error
            pmix_show_help("help-pmix-util.txt", "mkdir-failed", true,
                           tmp, strerror(errno));
            PMIx_Argv_free(parts);
            free(tmp);
            return PMIX_ERR_SILENT;
        }
        /* This component already exists. Intermediate components need
         * only exist - we just have to be able to traverse them, and
         * a traverse-only (e.g. 0711) directory is legitimate. The
         * final component is the one the caller is going to use, so
         * it has to really be a directory and carry the mode we were
         * asked for */
        if (i == (len - 1)) {
            ret = dirpath_ensure_mode(tmp, mode);
            if (PMIX_SUCCESS != ret) {
                if (PMIX_ERR_NOT_FOUND == ret) {
                    pmix_show_help("help-pmix-util.txt", "mkdir-failed", true,
                                   tmp, strerror(ENOENT));
                    ret = PMIX_ERR_SILENT;
                }
                PMIx_Argv_free(parts);
                free(tmp);
                return ret;
            }
        }
    }

    /* All done */

    PMIx_Argv_free(parts);
    free(tmp);
    return PMIX_SUCCESS;
}

/**
 * Empty out the directory that fd refers to, and remove any
 * subdirectories under it.  Takes ownership of fd - it is closed by
 * the closedir() below before we return.
 *
 * Every entry is inspected and removed *relative to the open
 * descriptor* (fstatat/openat/unlinkat) rather than by rebuilding and
 * re-resolving its path, so no entry can be swapped between the point
 * where it is classified and the point where it is acted on: the
 * thing we looked at is always the thing we remove.
 *
 * AT_SYMLINK_NOFOLLOW and O_NOFOLLOW keep symlinks as entries to be
 * unlinked rather than paths to be followed. unlinkat() removes the
 * link itself, never its target, and an entry swapped for a symlink
 * just before we descend into it is refused by O_NOFOLLOW instead of
 * sending the recursion outside the tree we were asked to destroy.
 *
 * path is the printable path of this directory. It is used only for
 * the caller's callback, for building child display paths, and for
 * error messages.
 */
static int dirpath_destroy_at(int fd, const char *path, bool recursive,
                              pmix_os_dirpath_destroy_callback_fn_t cbfunc)
{
    int rc, childfd, exit_status = PMIX_SUCCESS;
    DIR *dp;
    struct dirent *ep;
    struct stat buf;
    char *filenm;

    /* fdopendir() takes ownership of fd; closedir() will close it */
    dp = fdopendir(fd);
    if (NULL == dp) {
        close(fd);
        return PMIX_ERROR;
    }

    while (NULL != (ep = readdir(dp))) {
        /* skip:
         *  - . and ..
         */
        if ((0 == strcmp(ep->d_name, ".")) || (0 == strcmp(ep->d_name, ".."))) {
            continue;
        }

        /* Will the caller allow us to remove this file/directory? */
        if (NULL != cbfunc) {
            /*
             * Caller does not wish to remove this file/directory,
             * continue with the rest of the entries
             */
            if (!(cbfunc(path, ep->d_name))) {
                continue;
            }
        }

        if (0 != fstatat(dirfd(dp), ep->d_name, &buf, AT_SYMLINK_NOFOLLOW)) {
            /* it went away underneath us - that typically happens when
             * one task is removing the job session dir while another is
             * still removing its own proc session dir */
            continue;
        }

        if (!S_ISDIR(buf.st_mode)) {
            /* a plain file, or a symlink: unlinkat() removes the link
             * itself and leaves whatever it pointed at alone */
            if (0 != unlinkat(dirfd(dp), ep->d_name, 0)) {
                rc = errno;
                if (ENOENT == rc) {
                    /* someone else got there first */
                    continue;
                }
                if (EBUSY == rc) {
                    /* file system mount point or another process
                     * is using it */
                    exit_status = PMIX_ERROR;
                    continue;
                }
                // uncorrectable error
                filenm = pmix_os_path(false, path, ep->d_name, NULL);
                pmix_show_help("help-pmix-util.txt", "unlink-error", true,
                               (NULL == filenm) ? ep->d_name : filenm, strerror(rc));
                free(filenm);
                exit_status = PMIX_ERROR;
                break;
            }
            continue;
        }

        /* it's a directory - if it is empty we can drop it right here,
         * whether or not we were asked to recurse */
        if (0 == unlinkat(dirfd(dp), ep->d_name, AT_REMOVEDIR)) {
            continue;
        }
        if (ENOENT == errno) {
            continue;
        }
        if (!recursive) {
            /* it isn't empty and we were not told to descend into it,
             * so we cannot honor the request */
            exit_status = PMIX_ERROR;
            continue;
        }

        /* proceed downwards through the descriptor we already hold */
        childfd = openat(dirfd(dp), ep->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
        if (0 > childfd) {
            if (ENOENT != errno) {
                /* the entry is no longer an ordinary directory -
                 * leave it alone rather than chase it */
                exit_status = PMIX_ERROR;
            }
            continue;
        }
        filenm = pmix_os_path(false, path, ep->d_name, NULL);
        rc = dirpath_destroy_at(childfd, (NULL == filenm) ? ep->d_name : filenm,
                                recursive, cbfunc);
        free(filenm);
        if (PMIX_SUCCESS != rc) {
            exit_status = rc;
            break;
        }
        /* remove the now-empty subdirectory. This fails harmlessly if
         * the callback chose to preserve something inside it */
        unlinkat(dirfd(dp), ep->d_name, AT_REMOVEDIR);
    }

    /* Done with this directory */
    closedir(dp);

    return exit_status;
}

/**
 * This function attempts to remove a directory along with all the
 * files in it.  If the recursive variable is non-zero, then it will
 * try to recursively remove all directories.  If provided, the
 * callback function is executed prior to the directory or file being
 * removed.  If the callback returns non-zero, then no removal is
 * done.
 */
int pmix_os_dirpath_destroy(const char *path, bool recursive,
                            pmix_os_dirpath_destroy_callback_fn_t cbfunc)
{
    int fd, exit_status;

    if (NULL == path) { /* protect against error */
        return PMIX_ERROR;
    }

    /* Open up the directory. O_NOFOLLOW: never destroy through a
     * symlinked base path - the session directories sit under a
     * world-writable root with predictable names, so a link planted
     * there would otherwise redirect this whole recursive removal at
     * a tree of the attacker's choosing */
    fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (0 > fd) {
        /* per the documented contract, a directory that does not exist is
         * reported as NOT_FOUND; any other open failure is a generic error */
        return (ENOENT == errno) ? PMIX_ERR_NOT_FOUND : PMIX_ERROR;
    }

    exit_status = dirpath_destroy_at(fd, path, recursive, cbfunc);

    /*
     * If the directory is empty, then remove it - but
     * leave the system tmpdir alone unless we created it!
     */
    if (NULL == pmix_server_globals.system_tmpdir ||
        0 != strcmp(path, pmix_server_globals.system_tmpdir)) {
        rmdir(path);
    } else if (NULL != pmix_server_globals.system_tmpdir &&
               0 == strcmp(path, pmix_server_globals.system_tmpdir) &&
               pmix_ptl_base.created_system_tmpdir) {
        rmdir(path);
        free(pmix_ptl_base.system_tmpdir);
        pmix_ptl_base.system_tmpdir = NULL;
    }
    return exit_status;
}

bool pmix_os_dirpath_is_empty(const char *path)
{
    DIR *dp;
    struct dirent *ep;

    if (NULL != path) { /* protect against error */
        dp = opendir(path);
        if (NULL != dp) {
            while ((ep = readdir(dp))) {
                if ((0 != strcmp(ep->d_name, ".")) && (0 != strcmp(ep->d_name, ".."))) {
                    closedir(dp);
                    return false;
                }
            }
            closedir(dp);
            return true;
        }
        return false;
    }

    return true;
}

/**
 * Stale function left for PRRTE backward compatility
 */
int pmix_os_dirpath_access(const char *path, const mode_t mode)
{
    PMIX_HIDE_UNUSED_PARAMS(path, mode);
    return PMIX_SUCCESS;
}
