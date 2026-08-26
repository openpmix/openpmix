/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2007 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2009-2014 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2010      IBM Corporation.  All rights reserved.
 * Copyright (c) 2012-2013 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2016      University of Houston. All rights reserved.
 * Copyright (c) 2018      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "src/include/pmix_config.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif
#ifdef HAVE_SHLWAPI_H
#    include <shlwapi.h>
#endif
#ifdef HAVE_SYS_PARAM_H
#    include <sys/param.h>
#endif
#ifdef HAVE_SYS_MOUNT_H
#    include <sys/mount.h>
#endif
#ifdef HAVE_SYS_TYPES_H
#    include <sys/types.h>
#endif
#ifdef HAVE_SYS_STAT_H
#    include <sys/stat.h>
#endif
#ifdef HAVE_SYS_VFS_H
#    include <sys/vfs.h>
#endif
#ifdef HAVE_SYS_STATFS_H
#    include <sys/statfs.h>
#endif
#ifdef HAVE_SYS_STATVFS_H
#    include <sys/statvfs.h>
#endif
#ifdef HAVE_MNTENT_H
#    include <mntent.h>
#endif
#ifdef HAVE_PATHS_H
#    include <paths.h>
#endif
#ifdef HAVE_FCNTL_H
#    include <fcntl.h>
#endif

#ifdef _PATH_MOUNTED
#    define MOUNTED_FILE _PATH_MOUNTED
#else
#    define MOUNTED_FILE "/etc/mtab"
#endif

#include "src/include/pmix_globals.h"
#include "src/include/pmix_stdint.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_basename.h"
#include "src/util/pmix_os_path.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_path.h"

/*
 * Sanity check to ensure we have either statfs or statvfs
 */
#if !defined(HAVE_STATFS) && !defined(HAVE_STATVFS)
#    error Must have either statfs() or statvfs()
#endif

/*
 * Note that some OS's (e.g., NetBSD and Solaris) have statfs(), but
 * no struct statfs (!).  So check to make sure we have struct statfs
 * before allowing the use of statfs().
 */
#if defined(HAVE_STATFS) \
    && (defined(HAVE_STRUCT_STATFS_F_FSTYPENAME) || defined(HAVE_STRUCT_STATFS_F_TYPE))
#    define USE_STATFS 1
#else
#    define USE_STATFS 0
#endif

static void path_env_load(char *path, int *pargc, char ***pargv);
static char *list_env_get(char *var, char **list);

bool pmix_path_is_absolute(const char *path)
{
    if (NULL == path) {
        return false;
    }
    if (PMIX_PATH_SEP[0] == *path) {
        return true;
    }
    return false;
}

/**
 *  Locates a file with certain permissions
 */
char *pmix_path_find(char *fname, char **pathv, int mode, char **envv)
{
    char *fullpath;
    char *delimit;
    char *env;
    char *pfix;
    char *varname;
    size_t namelen;
    int i;

    if (NULL == fname || NULL == pathv) {
        return NULL;
    }

    /* If absolute path is given, return it without searching. */
    if (pmix_path_is_absolute(fname)) {
        return pmix_path_access(fname, NULL, mode);
    }

    /* Initialize. */

    fullpath = NULL;
    i = 0;

    /* Consider each directory until the file is found.  Thus, the
       order of directories is important. */

    while (pathv[i] && NULL == fullpath) {

        /* Replace environment variable at the head of the string. */
        if ('$' == *pathv[i]) {
            delimit = strchr(pathv[i], PMIX_PATH_SEP[0]);
            /* copy the variable name out rather than punching a
             * temporary NUL into the caller's string and putting it
             * back: pathv belongs to the caller, its elements may be
             * string literals, and another thread may be reading the
             * same array */
            namelen = (NULL == delimit) ? strlen(pathv[i] + 1)
                                        : (size_t)(delimit - (pathv[i] + 1));
            varname = (char *) malloc(namelen + 1);
            if (NULL == varname) {
                return NULL;
            }
            memcpy(varname, pathv[i] + 1, namelen);
            varname[namelen] = '\0';
            env = list_env_get(varname, envv);
            free(varname);
            if (NULL != env) {
                if (!delimit) {
                    fullpath = pmix_path_access(fname, env, mode);
                } else {
                    pfix = (char *) calloc((strlen(env) + strlen(delimit) + 1), sizeof(char));
                    if (NULL == pfix) {
                        return NULL;
                    }
                    strcpy(pfix, env);
                    strcat(pfix, delimit);
                    fullpath = pmix_path_access(fname, pfix, mode);
                    free(pfix);
                }
            }
        } else {
            fullpath = pmix_path_access(fname, pathv[i], mode);
        }
        i++;
    }
    return pmix_make_filename_os_friendly(fullpath);
}

/*
 * Locates a file with certain permissions from a list of search paths
 */
char *pmix_path_findv(char *fname, int mode, char **envv, char *wrkdir)
{
    char **dirv;
    char *fullpath;
    char *path;
    int dirc;
    int i;
    bool found_dot = false;

    /* Set the local search paths. */

    dirc = 0;
    dirv = NULL;

    if (NULL != (path = list_env_get("PATH", envv))) {
        path_env_load(path, &dirc, &dirv);
    }

    /* Replace the "." path by the working directory. */

    if (NULL != wrkdir) {
        for (i = 0; i < dirc; ++i) {
            if (0 == strcmp(dirv[i], ".")) {
                found_dot = true;
                free(dirv[i]);
                dirv[i] = strdup(wrkdir);
                if (NULL == dirv[i]) {
                    /* free the partially-built path vector rather than
                     * leaking it on this OOM path */
                    PMIx_Argv_free(dirv);
                    return NULL;
                }
            }
        }
    }

    /* If we didn't find "." in the path and we have a wrkdir, append
       the wrkdir to the end of the path */

    if (!found_dot && NULL != wrkdir) {
        pmix_argv_append(&dirc, &dirv, wrkdir);
    }

    if (NULL == dirv) {
        return NULL;
    }
    fullpath = pmix_path_find(fname, dirv, mode, envv);
    PMIx_Argv_free(dirv);
    return fullpath;
}

/**
 *  Forms a complete pathname and checks it for existence and
 *  permissions
 *
 *  Accepts:
 *      -fname File name
 *      -path  Path prefix
 *      -mode  Target permissions which must be satisfied
 *
 *  Returns:
 *      -Full pathname of located file Success
 *      -NULL Failure
 */
char *pmix_path_access(char *fname, char *path, int mode)
{
    char *fullpath = NULL;

    if (NULL == fname) {
        return NULL;
    }

    /* Allocate space for the full pathname. */
    if (NULL == path) {
        fullpath = pmix_os_path(false, fname, NULL);
    } else {
        fullpath = pmix_os_path(false, path, fname, NULL);
    }
    if (NULL == fullpath)
        return NULL;

    if (0 != access(fullpath, mode)) {
        free(fullpath);
        return NULL;
    }

    /* must have met all criteria! */
    return fullpath;
}

/**
 *
 *  Loads argument array with $PATH env var.
 *
 *  Accepts
 *      -path String containing the $PATH
 *      -argc Pointer to argc
 *      -argv Pointer to list of argv
 */
static void path_env_load(char *path, int *pargc, char ***pargv)
{
    char **elements;
    int i;

    if (NULL == path) {
        *pargc = 0;
        return;
    }

    /* Split the paths (delimited by PATHENVSEP) and add each one to
     * argv. This used to walk the string in place, punching a temporary
     * NUL in at each delimiter and putting it back - but the string it
     * is handed is usually the value getenv() returned, which POSIX
     * says must not be modified, and any other thread reading the same
     * variable in that window sees a truncated PATH. Split a copy.
     * PMIx_Argv_split() drops empty fields, which is what the walk did
     * with a repeated or leading delimiter. */
    elements = PMIx_Argv_split(path, PMIX_ENV_SEP);
    for (i = 0; NULL != elements && NULL != elements[i]; ++i) {
        pmix_argv_append(pargc, pargv, elements[i]);
    }
    PMIx_Argv_free(elements);
}

/**
 *  Gets value of variable in list or environment. Looks in the list first
 *
 *  Accepts:
 *      -var  String variable
 *      -list Pointer to environment list
 *
 *  Returns:
 *      -List Pointer to environment list Success
 *      -NULL Failure
 */
static char *list_env_get(char *var, char **list)
{
    size_t n;

    if (NULL != list) {
        n = strlen(var);

        while (NULL != *list) {
            if ((0 == strncmp(var, *list, n)) && ('=' == (*list)[n])) {
                return (*list + n + 1);
            }
            ++list;
        }
    }
    return getenv(var);
}

/**
 * Try to figure out the absolute path based on the application name
 * (usually argv[0]). If the path is already absolute return a copy, if
 * it start with . look into the current directory, if not dig into
 * the $PATH.
 * In case of error or if executable was not found (as an example if
 * the application did a cwd between the start and this call), the
 * function will return NULL. Otherwise, an newly allocated string
 * will be returned.
 */
char *pmix_find_absolute_path(char *app_name)
{
    char *abs_app_name;
    char cwd[PMIX_PATH_MAX], *pcwd;

    if (NULL == app_name) {
        return NULL;
    }

    if (pmix_path_is_absolute(app_name)) { /* already absolute path */
        abs_app_name = app_name;
    } else if ('.' == app_name[0] || NULL != strchr(app_name, PMIX_PATH_SEP[0])) {
        /* the app is in the current directory or below it */
        pcwd = getcwd(cwd, PMIX_PATH_MAX);
        if (NULL == pcwd) {
            /* too bad there is no way we can get the app absolute name */
            return NULL;
        }
        abs_app_name = pmix_os_path(false, pcwd, app_name, NULL);
    } else {
        /* Otherwise try to search for the application in the PATH ... */
        abs_app_name = pmix_path_findv(app_name, X_OK, NULL, NULL);
    }

    if (NULL != abs_app_name) {
        char *resolved_path = (char *) malloc(PMIX_PATH_MAX);
        if (NULL == resolved_path) {
            /* realpath() would allocate a buffer of its own for a NULL
             * argument, and we would then return this NULL and leak it */
            if (abs_app_name != app_name) {
                free(abs_app_name);
            }
            return NULL;
        }
        if (NULL == realpath(abs_app_name, resolved_path)) {
            free(resolved_path);
            /* only free abs_app_name if we allocated it - in the
             * already-absolute case it aliases the caller's app_name */
            if (abs_app_name != app_name) {
                free(abs_app_name);
            }
            return NULL;
        }
        if (abs_app_name != app_name) {
            free(abs_app_name);
        }
        return resolved_path;
    }
    return NULL;
}

/**
 * @brief Figure out whether fname is on network file system
 *
 * Try to figure out whether the file name specified through fname is
 * on any network file system (currently NFS, Lustre, Panasas and GPFS).
 *
 * @fname[in]          File name to check
 * @fstype[out]        File system type if retval is true
 *
 * @retval true       If fname is on NFS, Lustre, Panasas or GPFS
 * @retval false      otherwise
 *
 */
bool pmix_path_nfs(char *fname, char **fstype)
{
#ifdef HAVE_MNTENT_H
    struct stat s;
    struct mntent mnt;
    FILE *fp;
    dev_t dev;
    char buf[1024];
    int fd, n;
    char* fs_types[] = {
        "lustre",
        "nfs",
        "autofs",
        "panfs",
        "gpfs",
        "pvfs2",
        NULL
    };
    char *parent;

    fd = open(fname, O_RDONLY);
    if (0 > fd) {
        // try the parent
        parent = pmix_dirname(fname);
        fd = open(parent, O_RDONLY);
        free(parent);
        if (0 > fd) {
            // give up
            return false;
        }
    }
    if (fstat(fd, &s) != 0) {
        close(fd);
        return false;
    }
    close(fd);

    // retain the inode of the file
    dev = s.st_dev;

    // try a couple of possible locations for the
    // mount table
    if ((fp = setmntent("/proc/mounts", "r")) == NULL) {
        if ((fp = setmntent("/etc/mtab", "r")) == NULL) {
            return false;
        }
    }

    // search the mount table for an entry with
    // matching inode
    while (getmntent_r(fp, &mnt, buf, sizeof(buf))) {
        fd = open(mnt.mnt_dir, O_RDONLY);
        if (0 > fd) {
            // probably lack permissions
            continue;
        }
        if (fstat(fd, &s) != 0) {
            close(fd);
            continue;
        }

        if (s.st_dev == dev) {
            close(fd);
            endmntent(fp);
            // check if this is a file system of concern
            for (n=0; NULL != fs_types[n]; n++) {
                if (0 == strcmp(fs_types[n], mnt.mnt_type)) {
                    // yep, this is a shared file system. Per the contract,
                    // *fstype is only valid (and owned by the caller) on a
                    // true return - so only set it here, and not on any of
                    // the false paths where it would leak.
                    if (NULL != fstype) {
                        *fstype = strdup(mnt.mnt_type);
                        if (NULL == *fstype) {
                            /* the contract is that *fstype is valid on a
                             * true return, so we cannot answer true
                             * without it */
                            return false;
                        }
                    }
                    return true;
                }
            }
            // if we get here, then this is not a file
            // system of concern
            return false;
        }
        close(fd);
    }

    endmntent(fp);

    // Should never reach here.
    return false;
#else
    // cannot do anything
    PMIX_HIDE_UNUSED_PARAMS(fname, fstype);
    return false;
#endif
}

int pmix_path_df(const char *path, uint64_t *out_avail)
{
    int rc = -1;
    int trials = 5;
    int err = 0;
    int64_t avail;
#if USE_STATFS
    struct statfs buf;
#elif defined(HAVE_STATVFS)
    struct statvfs buf;
#endif

    if (NULL == path || NULL == out_avail) {
        return PMIX_ERROR;
    }
    *out_avail = 0;

    do {
#if USE_STATFS
        rc = statfs(path, &buf);
#elif defined(HAVE_STATVFS)
        rc = statvfs(path, &buf);
#endif
        err = errno;
    } while (-1 == rc && ESTALE == err && (--trials > 0));

    if (-1 == rc) {
        pmix_output_verbose(10, 0,
                             "pmix_path_df: stat(v)fs on "
                             "path: %s failed with errno: %d (%s)\n",
                             path, err, strerror(err));
        return PMIX_ERROR;
    }

    /* now set the amount of free space available on path.
     *
     * The two interfaces do not count in the same units. statfs()
     * reports f_bavail in f_bsize blocks, but POSIX statvfs() reports it
     * in f_frsize ones - and f_bsize there is the *preferred I/O size*,
     * which on several filesystems is many times f_frsize. Multiplying
     * by the wrong one told the caller it had far more room than it has.
     *
     * f_bavail is signed on the BSDs, where it can be negative, and
     * unsigned elsewhere - so the sign test has to be made through a
     * variable of a signed type. Written as a cast in the comparison
     * itself it is -Werror=type-limits on every platform where the
     * field is unsigned, which is why the statvfs arm of this function
     * has never compiled. */
    avail = (int64_t) buf.f_bavail;
    if (0 > avail) {
        avail = 0;
    }
#if USE_STATFS
    *out_avail = (uint64_t) buf.f_bsize * (uint64_t) avail;
#else
    *out_avail = (uint64_t) buf.f_frsize * (uint64_t) avail;
#endif

    pmix_output_verbose(10, 0,
                         "pmix_path_df: stat(v)fs states "
                         "path: %s has %" PRIu64 " B of free space.",
                         path, *out_avail);

    return PMIX_SUCCESS;
}
