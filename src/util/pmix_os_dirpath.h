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
 * Copyright (c) 2019-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/** @file:
 * Creates a directory tree set to the specified permissions.
 *
 * The pmix_os_dirpath_create() function creates a directory
 * tree, with each directory that is created in the tree having the specified
 * access permissions. Existing directories within the tree are left
 * untouched - however, if they do not permit the user to create a directory
 * within them, the function will return an error condition.
 *
 * If the specified full path name already exists, the
 * pmix_os_dirpath_create() function will check to ensure that
 * the final directory in the tree has at least the specified access permission. In other
 * words, if the directory has read-write-execute for all, and the user
 * has requested read-write access for just the user, then the function
 * will consider the directory acceptable. If the minimal permissions are
 * not currently provided, the function will attempt to change the
 * access permissions of the directory to add the specified
 * permissions. The function will return PMIX_ERROR if this cannot
 * be done.
 **/

#ifndef PMIX_OS_DIRPATH_CREATE_H
#define PMIX_OS_DIRPATH_CREATE_H

#include "src/include/pmix_config.h"
#ifdef HAVE_SYS_STAT_H
#    include <sys/stat.h>
#endif

BEGIN_C_DECLS

/**
 * @param path A pointer to a string that contains the path name to be built.
 * @param mode A mode_t bit mask that specifies the access permissions for the
 * directories being constructed.
 * @retval PMIX_SUCCESS If the directory tree has been successfully created with
 * the specified access permissions.
 * @retval PMIX_ERR_EXISTS If the final directory was already there. It carries
 * at least the requested permissions, so this is a usable directory and most
 * callers treat it exactly as they treat PMIX_SUCCESS - but they have to test
 * for it, since it is not PMIX_SUCCESS. The distinction is what tells a caller
 * whether it is the one that must clean the directory up afterwards.
 * @retval PMIX_ERR_BAD_PARAM If path is NULL or empty.
 * @retval PMIX_ERR_OUT_OF_RESOURCE If memory ran out while building the tree.
 * @retval PMIX_ERR_SILENT If the tree could not be created - because something
 * that is not a directory occupies the name, or the OS refused. A message has
 * already been displayed to the user; the caller must not display another.
 */

PMIX_EXPORT int pmix_os_dirpath_create(const char *path, const mode_t mode);

/**
 * Build a directory tree beneath a trusted root, refusing a symlink at
 * every component of the part PMIx is inventing.
 *
 * The split is the whole point. `root` is what somebody handed PMIx -
 * $TMPDIR, PMIX_NSDIR, a user-named output directory - and is trusted by
 * construction: if the caller supplies an untrustworthy starting point,
 * that is theirs to answer for. It is resolved the ordinary way, which
 * it has to be, since /tmp and /var are themselves symlinks on macOS.
 * `tail` is what PMIx composes underneath it out of its own naming
 * scheme, and every component of it is created with mkdirat() and opened
 * with O_NOFOLLOW against the descriptor of its parent, so the directory
 * each one lands in is the one just checked.
 *
 * That covers what a single O_NOFOLLOW cannot: O_NOFOLLOW applies to the
 * last component of a name and nothing else, so a symlink at one of the
 * directories PMIx is building through would otherwise send everything
 * created below it somewhere the caller did not name.
 *
 * @param root A directory that already exists. Trusted; not walked.
 * @param tail The relative path to build inside it. Walked.
 * @param mode Access permissions for the directories created.
 * @retval PMIX_SUCCESS     The tree was created.
 * @retval PMIX_ERR_EXISTS  The final directory was already there, and
 *                          carries at least the requested mode. Callers
 *                          normally treat this exactly as SUCCESS.
 * @retval PMIX_ERR_BAD_PARAM  A NULL root or an empty tail.
 * @retval PMIX_ERR_SILENT  It could not be built - the user has already
 *                          been shown why, and must not be shown again.
 */
PMIX_EXPORT int pmix_os_dirpath_create_under(const char *root, const char *tail,
                                             const mode_t mode);

/**
 * Open (or create) a file beneath a trusted root, declining a symlink at
 * every component of the part PMIx composes - the file included.
 *
 * The companion to pmix_os_dirpath_create_under(); see it for what the
 * root/tail split means and why it exists.
 *
 * @param root  A directory that already exists. Trusted; not walked.
 * @param tail  The relative path of the file inside it. Walked.
 * @param flags open(2) flags. O_NOFOLLOW is added.
 * @param mode  open(2) mode, used only when creating.
 * @retval >=0  A descriptor on the file. The caller closes it.
 * @retval -1   errno says why.
 */
PMIX_EXPORT int pmix_os_dirpath_open_file_under(const char *root, const char *tail,
                                                int flags, mode_t mode);

/**
 * Open (or create) a file, declining a symlink at the file itself.
 *
 * Adding O_NOFOLLOW to a plain open() is only half of what is wanted, and
 * this supplies exactly that half. The leaf is a name PMIx composes, so a
 * symlink there is not something this library put in place and following
 * it would land the operation on an unrelated file. The directory above
 * it comes from the system or the admin - and on macOS /tmp, /var and
 * /etc are all root-owned symlinks into /private - so it is resolved the
 * ordinary way. Where PMIx composes directory components of its own, use
 * pmix_os_dirpath_open_file_under() instead.
 *
 * @param path  The file to open.
 * @param flags open(2) flags. O_NOFOLLOW is added. Pass O_EXCL alongside
 *              O_CREAT where the name is one this process creates fresh:
 *              together they refuse anything already at the name rather
 *              than only a symlink.
 * @param mode  open(2) mode, used only when creating.
 * @retval >=0  A descriptor on the file. The caller closes it.
 * @retval -1   errno says why.
 */
PMIX_EXPORT int pmix_os_dirpath_open_file(const char *path, int flags, mode_t mode);

/**
 * Decide whether a file already sitting at a name this process meant to
 * create may be removed, and remove it.
 *
 * Called by pmix_os_dirpath_create_file() when the name is taken.
 *
 * @param path   The name that was found occupied.
 * @param cbdata The caller's pointer, passed through untouched.
 * @retval true  The name is free now - the callback unlinked it (or found
 *               it already gone) - so the create is worth one more try.
 * @retval false Leave it be. The create fails with errno EEXIST; anything
 *               the caller needs to tell those cases apart travels back
 *               through cbdata.
 */
typedef bool (*pmix_os_dirpath_reclaim_fn_t)(const char *path, void *cbdata);

/**
 * Create a file fresh at a name PMIx composes, reclaiming a leftover once.
 *
 * The names that go through here - a segment backing file, hwloc.sm, a
 * rendezvous file - are built from PMIx's own naming scheme, usually with
 * a pid in them, so anything already at the name is either left over from
 * an earlier run whose pid has come round again or something this code
 * did not put there. The create is therefore O_CREAT | O_EXCL through
 * pmix_os_dirpath_open_file(), which declines both, a symlink included.
 *
 * A leftover is then offered to `reclaim`, and if it is removed the create
 * is tried exactly once more. Once more and no further: a name that is
 * occupied again after being cleared belongs to some other process that
 * is creating it right now, and removing that one too would pull the file
 * out from under it.
 *
 * @param path    The file to create. Its directory is resolved normally;
 *                see pmix_os_dirpath_open_file().
 * @param flags   Further open(2) flags - the access mode, typically.
 *                O_CREAT, O_EXCL and O_NOFOLLOW are added.
 * @param mode    The new file's mode.
 * @param reclaim Decides about a file already at the name. NULL removes
 *                it unconditionally with unlink(), which acts on the name
 *                itself and never follows it onward.
 * @param cbdata  Passed through to reclaim.
 * @retval >=0  A descriptor on the new file. The caller closes it.
 * @retval -1   errno says why: EEXIST if the name was still occupied.
 */
PMIX_EXPORT int pmix_os_dirpath_create_file(const char *path, int flags, mode_t mode,
                                            pmix_os_dirpath_reclaim_fn_t reclaim,
                                            void *cbdata);

/**
 * Check to see if a directory is empty
 *
 * A directory that cannot be opened as a directory - it does not exist, it is
 * a plain file, it is a symlink, or it is unreadable - answers false, on the
 * grounds that the answer is normally used to decide whether to remove the
 * thing, and none of those may be removed on the strength of this call. A NULL
 * path answers true, there being nothing in it. Trailing separators are
 * ignored, so "link/" is still a symlink rather than the directory it names.
 *
 * @param path A pointer to a string that contains the path name to be checked.
 *
 * @retval true If the directory is empty, or path is NULL
 * @retval false If the directory is not empty, or could not be read
 */
PMIX_EXPORT bool pmix_os_dirpath_is_empty(const char *path);

/**
 * Stale function left for PRRTE backward compatibility. It is a no-op that
 * always answers PMIX_SUCCESS, and it must stay one: asking whether a
 * directory is writable and then acting on the answer is a time-of-check /
 * time-of-use race that no spelling of access()/faccessat() can close.
 * Whether files can be placed in a directory is settled by placing one.
 */
PMIX_EXPORT int pmix_os_dirpath_access(const char *path, const mode_t mode);

/**
 * Callback for pmix_os_dirpath_destroy(). Call for every file/directory before
 * taking action to remove/unlink it.
 *
 * @param root A pointer to a string that contains the base path name (e.g., /tmp/foo from
 * /tmp/foo/bar)
 * @param path A pointer to a string that contains the file or directory (e.g., bar from
 * /tmp/foo/bar)
 *
 * @retval true  Allow the program to remove the file/directory
 * @retval false Do not allow the program to remove the file/directory
 */
typedef bool (*pmix_os_dirpath_destroy_callback_fn_t)(const char *root, const char *path);

/**
 * Destroy a directory
 *
 * The directory is opened relative to its parent and never through a symlink -
 * at the final component or anywhere inside the tree, and trailing separators
 * are ignored so that "link/" cannot make the kernel follow one. It is removed
 * at the end only if its parent still holds the directory that was emptied: one
 * renamed away and replaced during the walk leaves the replacement alone.
 *
 * @param path A pointer to a string that contains the path name to be destroyed
 * @param recursive Recursively descend the directory removing all files and directories.
 *                  if set to 'false' then the directory must be empty to succeed.
 * @param cbfunc A function that will be called before removing a file or directory.
 *               If NULL, then assume all remove.
 *
 * @retval PMIX_SUCCESS If the directory was successfully removed or removed to the
 *                      specification of the user (i.e., obeyed the callback function).
 *                      A directory left non-empty counts as obeying the callback only
 *                      when there is one.
 * @retval PMIX_ERR_NOT_FOUND If directory does not exist.
 * @retval PMIX_ERR_BAD_PARAM If path has no final component to remove - it is empty
 *                    or the root - or its final component is "." or "..".
 * @retval PMIX_ERR_OUT_OF_RESOURCE If memory ran out.
 * @retval PMIX_ERROR If the directory cannot be removed, accessed properly, or contains
 *                    anything that could not be classified or removed - including an
 *                    emptied subdirectory or the directory itself - or was replaced
 *                    while it was being emptied.
 */
PMIX_EXPORT int pmix_os_dirpath_destroy(const char *path, bool recursive,
                                        pmix_os_dirpath_destroy_callback_fn_t cbfunc);

END_C_DECLS

#endif
