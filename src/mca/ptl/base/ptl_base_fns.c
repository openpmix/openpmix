/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2006 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "src/include/pmix_config.h"

#include <stdio.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif
#ifdef HAVE_SYS_TYPES_H
#    include <sys/types.h>
#endif
#ifdef HAVE_SYS_STAT_H
#    include <sys/stat.h>
#endif
#ifdef HAVE_DIRENT_H
#    include <dirent.h>
#endif
#ifdef HAVE_FCNTL_H
#    include <fcntl.h>
#endif
#ifdef HAVE_IFADDRS_H
#    include <ifaddrs.h>
#endif
#ifdef HAVE_NET_IF_H
#    include <net/if.h>
#endif
#include <ctype.h>
#include <sys/file.h>
#include <strings.h>
#include <errno.h>
#include <time.h>

#include "src/include/pmix_globals.h"
#include "src/include/pmix_socket_errno.h"
#include "src/runtime/pmix_progress_threads.h"
#include "src/client/pmix_client_ops.h"
#include "src/common/pmix_idenv.h"
#include "src/mca/bfrops/base/base.h"
#include "src/mca/psec/base/base.h"
#include "src/util/pmix_getid.h"
#include "src/util/pmix_idname.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_name_fns.h"
#include "src/util/pmix_net.h"
#include "src/util/pmix_random_bytes.h"
#include "src/util/pmix_os_path.h"
#include "src/util/pmix_if.h"
#include "src/util/pmix_printf.h"
#include "src/util/pmix_show_help.h"
#include "src/util/pmix_string_copy.h"

#include "src/mca/ptl/base/base.h"
#include "src/mca/ptl/base/ptl_base_handshake.h"

/****    SUPPORTING FUNCTIONS    ****/
static void timeout(int sd, short args, void *cbdata);
static void retry_wait(const struct timeval *tv);
static pmix_status_t construct_message(pmix_peer_t *peer, char **msgout, size_t *sz,
                                       pmix_info_t *iptr, size_t niptr,
                                       const pmix_byte_object_t *bind, uid_t to);
static pmix_status_t parse_conn_file(char *filename, bool optional, bool found_by_search,
                                     bool regular_only, pmix_list_t *connections);
static bool search_candidate(const char *path, bool *isdir);

pmix_status_t pmix_ptl_base_set_peer(pmix_peer_t *peer, char **evar)
{
    pmix_status_t rc;
    const char *vrs;
    pmix_bfrops_base_active_module_t *mod;
    char *ptr, *tmp, check[2], *eval;
    int major, minor;
    bool evalgiven;

    vrs = pmix_idenv_get("PMIX_VERSION");

    if (NULL == evar) {
        return PMIX_ERR_BAD_PARAM;
    }

    if (NULL == *evar) {
        eval = NULL;
        evalgiven = false;
    } else {
        // they are passing in the bfrops module to use
        eval = *evar;
        evalgiven = true;
    }

    PMIX_LIST_FOREACH(mod, &pmix_bfrops_globals.actives, pmix_bfrops_base_active_module_t) {
        // these are in priority order, so take the highest priority that we find
        /* every bfrops component is named for the wire version it
         * speaks - "v41", "v21" - so the digits follow the last 'v'.
         * A component that does not follow that convention cannot be
         * matched to a PMIX_SERVER_URIvNN variable at all */
        ptr = strrchr(mod->component->base.pmix_mca_component_name, 'v');
        if (NULL == ptr) {
            continue;
        }
        ++ptr;
        if (0 > pmix_asprintf(&tmp, "PMIX_SERVER_URI%s", ptr)) {
            return PMIX_ERR_NOMEM;
        }
        if (evalgiven) {
            if (0 != strcmp(tmp, eval)) {
                free(tmp);
                continue;
            }
        } else {
            if (NULL == (eval = (char *) pmix_idenv_get(tmp))) {
                free(tmp);
                continue;
            }
        }
        free(tmp);

        /* must use the v<ptr> bfrops module */
        if (0 > pmix_asprintf(&tmp, "v%s", ptr)) {
            return PMIX_ERR_NOMEM;
        }
        PMIX_BFROPS_SET_MODULE(rc, pmix_globals.mypeer, peer, tmp);
        free(tmp);
        if (PMIX_SUCCESS != rc) {
            continue;
        }

        check[1] = '\0';
        // must be at least one number in the version
        check[0] = ptr[0];
        major = strtoul(check, NULL, 10);
        // might not be a second - e.g., v4
        if (2 == strlen(ptr)) {
            check[0] = ptr[1];
            minor = strtoul(check, NULL, 10);
        } else {
            minor = 0;
        }
        /* we are talking to a vmajor.minor server */
        PMIX_SET_PEER_TYPE(peer, PMIX_PROC_SERVER);
        PMIX_SET_PEER_VERSION(peer, vrs, major, minor);
        /* the server that started us said so */
        pmix_ptl_base.server_release_vouched = true;

        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "Version %s SERVER DETECTED - BFROPS %s",
                            vrs, mod->component->base.pmix_mca_component_name);

        if (!evalgiven) {
            // return the uri
            *evar = eval;
        }
        return PMIX_SUCCESS;
    }

    return PMIX_ERR_UNREACH;
}

pmix_status_t pmix_ptl_base_setup_fork(const pmix_proc_t *proc, char ***env)
{
    pmix_status_t rc;

    /* a child that does not get these still starts, and then searches
     * the default tmpdir for its server - where it may find a different
     * one - so a failure to set them has to fail the fork setup */
    rc = PMIx_Setenv("PMIX_SERVER_TMPDIR", pmix_ptl_base.session_tmpdir, true, env);
    if (PMIX_SUCCESS == rc) {
        rc = PMIx_Setenv("PMIX_SYSTEM_TMPDIR", pmix_ptl_base.system_tmpdir, true, env);
    }
    PMIX_HIDE_UNUSED_PARAMS(proc);
    return rc;
}

pmix_status_t pmix_ptl_base_parse_uri(const char *evar, char **nspace, pmix_rank_t *rank,
                                      char **suri)
{
    char **uri;
    char *p;

    uri = PMIx_Argv_split(evar, ';');
    if (2 != PMIx_Argv_count(uri)) {
        PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
        PMIx_Argv_free(uri);
        return PMIX_ERR_NOT_SUPPORTED;
    }

    /* set the server nspace - the rank is appended
     * to the end with a '.' separator. NOTE: we
     * cannot search from the FRONT as that would
     * stop on any FQDN or IPv4 separations */
    if (NULL == (p = strrchr(uri[0], '.'))) {
        PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
        PMIx_Argv_free(uri);
        return PMIX_ERR_NOT_SUPPORTED;
    }
    *p = '\0';
    ++p;
    *nspace = strdup(uri[0]);
    if (NULL == *nspace) {
        PMIx_Argv_free(uri);
        return PMIX_ERR_NOMEM;
    }
    /* set the server rank. This is deliberately lenient: v3.2 servers
     * print the rank with "%d", so a wildcard or invalid rank arrives as
     * a negative number, and strtoull() wraps it back onto the same
     * pmix_rank_t. A digits-only parse would refuse those servers */
    *rank = strtoull(p, NULL, 10);
    if (NULL != suri) {
        *suri = strdup(uri[1]);
        if (NULL == *suri) {
            free(*nspace);
            *nspace = NULL;
            PMIx_Argv_free(uri);
            return PMIX_ERR_NOMEM;
        }
    }

    PMIx_Argv_free(uri);
    return PMIX_SUCCESS;
}

/* Split a "major.minor.release" version string into its components.
 *
 * The strings handed to this come off the wire, so they need not carry
 * all three components - or any of them. Every component we cannot read
 * is reported as zero, and we step over a separator only after
 * confirming the string has not already ended. */
void pmix_ptl_base_parse_version(const char *vers, uint8_t *major,
                                 uint8_t *minor, uint8_t *release)
{
    char *p;

    *major = 0;
    *minor = 0;
    *release = 0;
    if (NULL == vers) {
        return;
    }
    *major = (uint8_t) strtoul(vers, &p, 10);
    if ('\0' == *p) {
        return;
    }
    *minor = (uint8_t) strtoul(&p[1], &p, 10);
    if ('\0' == *p) {
        return;
    }
    *release = (uint8_t) strtoul(&p[1], NULL, 10);
}

/* Open a connection file for reading.
 *
 * A file the caller named is opened with fopen(). A file found by walking
 * a directory, or at one of the fixed rendezvous names, is opened
 * non-blocking and kept only if the open descriptor is a regular file no
 * larger than any server writes. */
static FILE *open_conn_file(const char *filename, bool regular_only)
{
    int fd, flags;
    struct stat st;
    FILE *fp;

    if (!regular_only) {
        return fopen(filename, "r");
    }
    fd = open(filename, O_RDONLY | O_NONBLOCK);
    if (0 > fd) {
        return NULL;
    }
    if (0 != fstat(fd, &st) || !S_ISREG(st.st_mode) || PMIX_PTL_MAX_CONN_FILE < st.st_size) {
        close(fd);
        errno = ENOENT;
        return NULL;
    }
    flags = fcntl(fd, F_GETFL);
    if (0 <= flags) {
        (void) fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
    }
    fp = fdopen(fd, "r");
    if (NULL == fp) {
        close(fd);
    }
    return fp;
}

char *pmix_ptl_base_getline(FILE *fp, size_t *budget)
{
    char *line, *grown;
    size_t len = 0, size = 128;
    int c;

    if (0 == *budget || NULL == (line = (char *) malloc(size))) {
        return NULL;
    }
    while (0 < *budget && EOF != (c = getc(fp))) {
        --*budget;
        if ('\n' == c) {
            line[len] = '\0';
            return line;
        }
        if ('\0' == c) {
            /* handed back as a C string, the rest would silently vanish */
            free(line);
            return NULL;
        }
        if (len + 1 == size) {
            size *= 2;
            if (NULL == (grown = (char *) realloc(line, size))) {
                free(line);
                return NULL;
            }
            line = grown;
        }
        line[len++] = (char) c;
    }
    /* a final line with no newline is still a line - one the budget cut
     * short, or a read failed partway thru, is not */
    if (0 == len || ferror(fp) || (0 == *budget && !feof(fp))) {
        free(line);
        return NULL;
    }
    line[len] = '\0';
    return line;
}

char *pmix_ptl_base_lock_name(const char *name)
{
    const char *base = strrchr(name, PMIX_PATH_SEP[0]);
    char *lname = NULL;
    int dlen;

    /* a '.' before the file's own name, in the same directory */
    base = (NULL == base) ? name : base + 1;
    dlen = (int) (base - name);
    if (0 > pmix_asprintf(&lname, "%.*s.%s%s", dlen, name, base, PMIX_PTL_LOCK_SUFFIX)) {
        return NULL;
    }
    return lname;
}

#ifdef O_CLOEXEC
#    define PTL_O_CLOEXEC O_CLOEXEC
#else
#    define PTL_O_CLOEXEC 0
#endif

int pmix_ptl_base_take_lock(int dirfd, const char *name)
{
    struct stat st, now;
    struct timespec wait = {0, 10000000};
    char *lname;
    int fd, rc, tries, busy = 0;

    if (NULL == (lname = pmix_ptl_base_lock_name(name))) {
        return -1;
    }
    for (tries = 0; tries < 2; tries++) {
        fd = openat(dirfd, lname, O_RDWR | O_CREAT | O_NOFOLLOW | O_NONBLOCK | PTL_O_CLOEXEC,
                    S_IRUSR | S_IWUSR);
        if (0 > fd) {
            break;
        }
        (void) fcntl(fd, F_SETFD, FD_CLOEXEC);
        /* ours alone - one another user left at the name is not */
        if (0 != fstat(fd, &st) || !S_ISREG(st.st_mode) || geteuid() != st.st_uid ||
            1 != st.st_nlink || 0 != (st.st_mode & (S_IRWXG | S_IRWXO))) {
            close(fd);
            fd = -1;
            break;
        }
        /* and held by no other server - a reader asking whether one runs
         * holds it for a moment, and is waited out */
        rc = flock(fd, LOCK_EX | LOCK_NB);
        while (0 != rc && EWOULDBLOCK == errno && busy < 20) {
            ++busy;
            (void) nanosleep(&wait, NULL);
            rc = flock(fd, LOCK_EX | LOCK_NB);
        }
        if (0 != rc) {
            busy = (EWOULDBLOCK == errno);
            close(fd);
            fd = busy ? PMIX_PTL_LOCK_BUSY : -1;
            break;
        }
        /* and still the file at the name: a server finalizing removes
         * its own, and a lock on a file no longer there says nothing to
         * anyone - take the one now there */
        if (0 == fstatat(dirfd, lname, &now, AT_SYMLINK_NOFOLLOW) && now.st_dev == st.st_dev &&
            now.st_ino == st.st_ino) {
            break;
        }
        close(fd);
        fd = -1;
    }
    free(lname);
    return fd;
}

bool pmix_ptl_base_server_gone(int dirfd, const char *name, uid_t owner, const char *lock)
{
    return PMIX_PTL_LOCK_GONE == pmix_ptl_base_lock_state(dirfd, name, owner, lock);
}

pmix_ptl_lock_state_t pmix_ptl_base_lock_state(int dirfd, const char *name, uid_t owner,
                                               const char *lock)
{
    pmix_ptl_lock_state_t state = PMIX_PTL_LOCK_UNKNOWN;
    struct stat lst, fst, ours;
    char *lname;
    size_t n;
    int fd;

    if (NULL == lock || 0 != strcmp(lock, PMIX_PTL_LOCK_FLOCK) ||
        NULL == (lname = pmix_ptl_base_lock_name(name))) {
        return PMIX_PTL_LOCK_UNKNOWN;
    }
    /* the server's own: one name, that only its owner can open - no
     * other file linked to the name, nor one another can lock */
    if (0 != fstatat(dirfd, lname, &lst, AT_SYMLINK_NOFOLLOW) || !S_ISREG(lst.st_mode) ||
        owner != lst.st_uid || 1 != lst.st_nlink || 0 != (lst.st_mode & (S_IRWXG | S_IRWXO))) {
        free(lname);
        return PMIX_PTL_LOCK_UNKNOWN;
    }
    /* one we hold is our own server's: never opened here, as closing a
     * descriptor on it lets go of our lock where flock is done with
     * fcntl's locks, as over NFS */
    for (n = 0; n < pmix_ptl_base.nrndz_lockfd; n++) {
        if (0 == fstat(pmix_ptl_base.rndz_lockfd[n], &ours) && ours.st_dev == lst.st_dev &&
            ours.st_ino == lst.st_ino) {
            free(lname);
            return PMIX_PTL_LOCK_UNKNOWN;
        }
    }
    fd = openat(dirfd, lname, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | PTL_O_CLOEXEC);
    free(lname);
    if (0 > fd) {
        return PMIX_PTL_LOCK_UNKNOWN;
    }
    if (0 == fstat(fd, &fst) && fst.st_dev == lst.st_dev && fst.st_ino == lst.st_ino) {
        if (0 == flock(fd, LOCK_SH | LOCK_NB)) {
            state = PMIX_PTL_LOCK_GONE;
            (void) flock(fd, LOCK_UN);
        } else if (EWOULDBLOCK == errno) {
            /* held by its server - any other failure says nothing */
            state = PMIX_PTL_LOCK_HELD;
        }
    }
    close(fd);
    return state;
}

/* Decide what a directory-walk entry is. Only a real directory is
 * descended into: a symbolic link is never followed to one, because a
 * link back up the tree ("ln -s . a") makes the walk revisit everything
 * beneath it at every level, and two such links make it exponential -
 * again, in a directory anyone can write to. A link to a regular file is
 * still read. Returns false for anything that is neither. */
static bool search_candidate(const char *path, bool *isdir)
{
    struct stat st;

    *isdir = false;
    if (0 != lstat(path, &st)) {
        return false;
    }
    if (S_ISDIR(st.st_mode)) {
        *isdir = true;
        return true;
    }
    if (S_ISLNK(st.st_mode) && 0 != stat(path, &st)) {
        return false;
    }
    return S_ISREG(st.st_mode);
}

/* Our highest wire format named for a release no later than major.minor -
 * the guess for a server whose file does not say which it accepts. A
 * component is named for the release that introduced it ("v41"), so this
 * assumes the server still runs every one up to its own, which the
 * server's own bfrops selection may not. NULL when we have none that old */
static pmix_bfrops_module_t *bfrops_by_version(unsigned major, unsigned minor)
{
    pmix_bfrops_base_active_module_t *active;
    const char *name, *ptr;
    unsigned cmaj, cmin;

    PMIX_LIST_FOREACH (active, &pmix_bfrops_globals.actives, pmix_bfrops_base_active_module_t) {
        name = active->component->base.pmix_mca_component_name;
        ptr = strrchr(name, 'v');
        if (NULL == ptr || !isdigit((unsigned char) ptr[1])) {
            continue;
        }
        cmaj = (unsigned) (ptr[1] - '0');
        cmin = isdigit((unsigned char) ptr[2]) ? (unsigned) (ptr[2] - '0') : 0;
        if (cmaj < major || (cmaj == major && cmin <= minor)) {
            return active->component->assign_module();
        }
    }
    return NULL;
}

pmix_status_t pmix_ptl_base_select_compat(struct pmix_peer_t *pr, pmix_connection_t *cn)
{
    pmix_peer_t *peer = (pmix_peer_t *) pr;
    pmix_bfrops_module_t *bfrops = NULL;
    pmix_psec_module_t *psec = NULL, *cur;
    char *ours, **list;
    bool primary;
    uint8_t major, minor;
    int n;

    if (NULL == peer || NULL == peer->nptr || NULL == cn) {
        return PMIX_ERR_BAD_PARAM;
    }
    primary = (peer == pmix_client_globals.myserver);
    peer->nptr->compat.rpsec = NULL;

    /* the wire format: the highest of ours the server lists */
    if (NULL != cn->bfrops) {
        bfrops = pmix_bfrops_base_assign_module(cn->bfrops);
        if (NULL == bfrops) {
            ours = pmix_bfrops_base_get_available_modules();
            pmix_show_help("help-ptl-base.txt", "no-common-module", true, cn->uri,
                           "wire format (bfrops component)", cn->bfrops,
                           (NULL == ours) ? "none" : ours, "wire formats", "bfrops");
            free(ours);
            return PMIX_ERR_NOT_SUPPORTED;
        }
    } else {
        /* an older server says only its version - go by that, if it is
         * older than we are */
        major = PMIX_PEER_MAJOR_VERSION(peer);
        minor = PMIX_PEER_MINOR_VERSION(peer);
        // compare major.minor as one value - comparing the minors on
        // their own is a constant false in any x.0 release
        if (0 != major && PMIX_MAJOR_WILDCARD != major &&
            (((unsigned) major << 8) | minor) <
            (((unsigned) PMIX_VERSION_MAJOR << 8) | (unsigned) PMIX_VERSION_MINOR)) {
            bfrops = bfrops_by_version(major, minor);
        }
    }
    if (NULL != bfrops) {
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "ptl:base: speaking %s to server %s (server lists %s)",
                            bfrops->version, cn->uri, (NULL == cn->bfrops) ? "none" : cn->bfrops);
        peer->nptr->compat.bfrops = bfrops;
        if (primary) {
            pmix_globals.mypeer->nptr->compat.bfrops = bfrops;
        }
    }

    /* the security mechanism: ours if the server lists it, otherwise the
     * highest of ours that it does. An older server's list is unknown, so
     * take the mechanisms every release serves a local peer with - munge
     * if we have it, else native. Never ssl, which no older server has */
    if (NULL == cn->psec) {
        psec = pmix_psec_base_assign_module("munge,native");
        if (NULL == psec) {
            ours = pmix_psec_base_get_available_modules();
            pmix_show_help("help-ptl-base.txt", "no-common-module", true, cn->uri,
                           "security mechanism (psec component)",
                           "(none listed - an older server takes munge or native)",
                           (NULL == ours) ? "none" : ours, "security mechanisms", "psec");
            free(ours);
            return PMIX_ERR_NOT_SUPPORTED;
        }
        peer->nptr->compat.psec = psec;
        if (primary) {
            pmix_globals.mypeer->nptr->compat.psec = psec;
        }
    } else {
        cur = (NULL != peer->nptr->compat.psec) ? peer->nptr->compat.psec
                                                : pmix_globals.mypeer->nptr->compat.psec;
        list = PMIx_Argv_split(cn->psec, ',');
        for (n = 0; NULL != cur && NULL != list && NULL != list[n]; n++) {
            if (0 == strcmp(list[n], cur->name)) {
                psec = cur;
                break;
            }
        }
        PMIx_Argv_free(list);
        if (NULL == psec) {
            psec = pmix_psec_base_assign_module(cn->psec);
        }
        if (NULL == psec) {
            ours = pmix_psec_base_get_available_modules();
            pmix_show_help("help-ptl-base.txt", "no-common-module", true, cn->uri,
                           "security mechanism (psec component)", cn->psec,
                           (NULL == ours) ? "none" : ours, "security mechanisms", "psec");
            free(ours);
            return PMIX_ERR_NOT_SUPPORTED;
        }
        peer->nptr->compat.psec = psec;
        /* and the one to prove who either end is with, should the server
         * have to */
        peer->nptr->compat.rpsec = pmix_psec_base_remote_module_among(cn->psec);
        if (primary) {
            pmix_globals.mypeer->nptr->compat.psec = psec;
        }
    }
    return PMIX_SUCCESS;
}

pmix_status_t pmix_ptl_base_parse_uri_file(char *filename,
                                           bool optional,
                                           pmix_list_t *connections)
{
    return parse_conn_file(filename, optional, false, false, connections);
}

pmix_status_t pmix_ptl_base_parse_rndz_file(char *filename,
                                            bool optional,
                                            pmix_list_t *connections)
{
    return parse_conn_file(filename, optional, false, true, connections);
}

/* A server's file says which release it is. That it is a release before
 * this one's - which a tool on this host connects to as before, where its
 * kernel cannot name the server's user (prove_unidentified) - is believed
 * only of a file the caller named (`named`), not one found in a directory
 * anyone can write to or at a fixed name there, whose server may be long
 * gone and its port another's; and only of one a server we may talk to
 * wrote: the file at the name, not a link to one or one of several names
 * for it, that only its owner can write, owned by the user we were told
 * to expect, or else by our own user, root, or a user
 * ptl_base_server_access names. The version itself is kept as the file
 * says, whatever is believed of it: it chooses the wire format */
static bool vouched_earlier(const char *version, bool named, const struct stat *st,
                            const char *filename)
{
    uint8_t major, minor, release;
    struct stat lst;
    size_t n;
    bool ok;

    if (NULL == version) {
        return false;
    }
    pmix_ptl_base_parse_version(('v' == version[0]) ? version + 1 : version, &major, &minor,
                                &release);
    if (PMIX_PTL_LOCAL_MAJOR <= major) {
        return false;
    }
    ok = (named && NULL != st && 0 == lstat(filename, &lst) && S_ISREG(lst.st_mode) &&
          lst.st_dev == st->st_dev && lst.st_ino == st->st_ino && 1 == st->st_nlink &&
          0 == (st->st_mode & (S_IWGRP | S_IWOTH)));
    if (ok && pmix_ptl_base.server_expect_given) {
        ok = (pmix_ptl_base.server_expect_uid == st->st_uid);
    } else if (ok) {
        ok = (geteuid() == st->st_uid || 0 == st->st_uid);
        for (n = 0; !ok && n < pmix_ptl_base.naccess_uids; n++) {
            ok = (pmix_ptl_base.access_uids[n] == st->st_uid);
        }
    }
    if (!ok) {
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "ptl:base: a server's file says it is of an earlier release - not "
                            "believed of this one");
    }
    return ok;
}

static pmix_status_t parse_conn_file(char *filename, bool optional, bool found_by_search,
                                     bool regular_only, pmix_list_t *connections)
{
    FILE *fp;
    char *srvr, *p = NULL;
    struct timeval tv;
    int retries;
    pmix_status_t rc;
    pmix_connection_t *cn;
    char *nspace = NULL;
    pmix_rank_t rank;
    char *uri = NULL;
    char *alt = NULL, *line, *local = NULL, *lns = NULL, *luri = NULL, *lhost;
    pmix_rank_t lrank;
    char *bfrops = NULL, *psec = NULL, **dest;
    const char *tag;
    size_t taglen;
    char *lock = NULL;
    bool gone, owned;
    int nlines = 0;
    size_t budget = PMIX_PTL_MAX_CONN_FILE;
    struct stat st;

    /* if we cannot open the file, then the server must not
     * be configured to support tool connections, or this
     * user isn't authorized to access it - or it may just
     * not exist yet! Check for existence */
    /* coverity[TOCTOU] */
    if (0 != access(filename, R_OK)) {
        if (ENOENT == errno && !optional && !found_by_search) {
            /* the file does not exist, so give it
             * a little time to see if the server
             * is still starting up. Not for a file a directory walk
             * just listed - that one is not late, it is gone */
            retries = 0;
            do {
                ++retries;
                pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                                    "WAITING FOR CONNECTION FILE %s", filename);
                if (0 < pmix_ptl_base.wait_to_connect) {
                    tv.tv_sec = pmix_ptl_base.wait_to_connect;
                    tv.tv_usec = 0;
                } else {
                    tv.tv_sec = 0;
                    tv.tv_usec = 10000; // use 0.01 sec as default
                }
                retry_wait(&tv);
                /* coverity[TOCTOU] */
                if (0 == access(filename, R_OK)) {
                    goto process;
                }
            } while (retries < pmix_ptl_base.max_retries);
            /* otherwise, mark it as unreachable */
        }
        if (!optional) {
            if (EACCES == errno) {
                pmix_show_help("help-ptl-base.txt", "file-not-found", true,
                               filename, "access denied");
            } else {
                pmix_show_help("help-ptl-base.txt", "file-not-found", true,
                               filename, "could not be found");
            }
        }
        return PMIX_ERR_UNREACH;
    }

process:
    fp = open_conn_file(filename, regular_only);
    if (NULL == fp) {
        if (!optional) {
            if (EACCES == errno) {
                pmix_show_help("help-ptl-base.txt", "file-not-found", true,
                               filename, "access denied");
            } else {
                pmix_show_help("help-ptl-base.txt", "file-not-found", true,
                               filename, "could not be found");
            }
        }
        return PMIX_ERR_UNREACH;
    }
    /* get the URI - might seem crazy, but there is actually
     * a race condition here where the server may have created
     * the file but not yet finished writing into it. So give
     * us a chance to get the required info */
    for (retries = 0; retries < 3; retries++) {
        srvr = pmix_ptl_base_getline(fp, &budget);
        if (NULL != srvr) {
            break;
        }
        fclose(fp);
        tv.tv_sec = 0;
        tv.tv_usec = 10000; // use 0.01 sec as default
        retry_wait(&tv);
        fp = open_conn_file(filename, regular_only);
        if (NULL == fp) {
            return PMIX_ERR_UNREACH;
        }
    }
    if (NULL == srvr) {
        if (!optional) {
            pmix_show_help("help-ptl-base.txt", "file-not-found", true,
                           filename, "could not be read");
        }
        fclose(fp);
        return PMIX_ERR_UNREACH;
    }

    /* see if this file contains the server's version */
    p = pmix_ptl_base_getline(fp, &budget);
    /* and whether it lists other addresses the server listens on, and the
     * wire formats and security mechanisms it accepts. They follow every
     * line a released reader takes by position, and are found by their
     * tag rather than by where they fall. The first of each counts */
    alt = NULL;
    if (NULL != p) {
        while (nlines++ < PMIX_PTL_MAX_TAG_LINES && NULL != (line = pmix_ptl_base_getline(fp, &budget))) {
            if (0 == strncmp(line, PMIX_PTL_ALT_URIS_TAG, strlen(PMIX_PTL_ALT_URIS_TAG))) {
                tag = PMIX_PTL_ALT_URIS_TAG;
                dest = &alt;
            } else if (0 == strncmp(line, PMIX_PTL_LOCAL_URI_TAG,
                                    strlen(PMIX_PTL_LOCAL_URI_TAG))) {
                tag = PMIX_PTL_LOCAL_URI_TAG;
                dest = &local;
            } else if (0 == strncmp(line, PMIX_PTL_BFROPS_TAG, strlen(PMIX_PTL_BFROPS_TAG))) {
                tag = PMIX_PTL_BFROPS_TAG;
                dest = &bfrops;
            } else if (0 == strncmp(line, PMIX_PTL_PSEC_TAG, strlen(PMIX_PTL_PSEC_TAG))) {
                tag = PMIX_PTL_PSEC_TAG;
                dest = &psec;
            } else if (0 == strncmp(line, PMIX_PTL_LOCK_TAG, strlen(PMIX_PTL_LOCK_TAG))) {
                tag = PMIX_PTL_LOCK_TAG;
                dest = &lock;
            } else {
                free(line);
                continue;
            }
            taglen = strlen(tag);
            if (NULL == *dest && '\0' != line[taglen]) {
                *dest = strdup(line + taglen);
            }
            free(line);
        }
    }
    /* whose file it is - and, for a search, whether the server that
     * wrote it is gone: it passes over such a file, so that the file of
     * one still running is the one found */
    owned = (0 == fstat(fileno(fp), &st));
    gone = found_by_search && owned &&
           pmix_ptl_base_server_gone(AT_FDCWD, filename, st.st_uid, lock);
    free(lock);
    fclose(fp);
    if (gone) {
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "pmix:tool: passing over %s - the server that wrote it is gone",
                            filename);
        free(srvr);
        free(p);
        free(alt);
        free(local);
        free(bfrops);
        free(psec);
        return PMIX_ERR_NOT_FOUND;
    }

    /* parse the URI */
    rc = pmix_ptl_base_parse_uri(srvr, &nspace, &rank, &uri);
    free(srvr);
    if (PMIX_SUCCESS == rc) {
        cn = PMIX_NEW(pmix_connection_t);
        if (NULL == cn) {
            free(nspace);
            free(uri);
            if (NULL != p) {
                free(p);
            }
            free(alt);
            free(local);
            free(bfrops);
            free(psec);
            return PMIX_ERR_NOMEM;
        }
        cn->nspace = nspace;
        cn->rank = rank;
        cn->uri = uri;
        cn->version = p;
        cn->vouched = vouched_earlier(p, !regular_only, owned ? &st : NULL, filename);
        cn->alt_uris = alt;
        cn->bfrops = bfrops;
        cn->psec = psec;
        /* the local address is "<host>;<uri>": used only on the server's
         * host - a report file may be read on another - and only if it is
         * the same server's */
        lhost = (NULL == local) ? NULL : strchr(local, ';');
        if (NULL != lhost && (size_t) (lhost - local) == strlen(pmix_globals.hostname) &&
            0 == strncmp(local, pmix_globals.hostname, (size_t) (lhost - local)) &&
            PMIX_SUCCESS == pmix_ptl_base_parse_uri(lhost + 1, &lns, &lrank, &luri) &&
            0 == strcmp(lns, nspace) && lrank == rank) {
            cn->local_uri = luri;
            luri = NULL;
        }
        free(lns);
        free(luri);
        free(local);
        pmix_list_append(connections, &cn->super);
    } else {
        free(local);
        free(alt);
        free(bfrops);
        free(psec);
        if (NULL != nspace) {
            free(nspace);
        }
        if (NULL != uri) {
            free(uri);
        }
        if (NULL != p) {
            free(p);
        }
    }
    return rc;
}

/* depth is how many more directories below this one may be entered. The
 * walk holds a directory stream open for every level it is in, and runs
 * wherever its caller does - for a server's query, the progress thread -
 * so a tree deeper than any server places its files is not followed. */
static pmix_status_t df_search(char *dirname, char *prefix, bool exact,
                               pmix_info_t info[], size_t ninfo,
                               bool optional, pmix_list_t *connections, int depth)
{
    char *newdir;
    DIR *cur_dirp;
    struct dirent *dir_entry;
    pmix_status_t rc;
    bool isdir;

    if (NULL == (cur_dirp = opendir(dirname))) {
        return PMIX_ERR_NOT_FOUND;
    }

    pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                        "pmix:ptl: searching directory %s", dirname);

    /* search the entries for something that starts with the provided prefix */
    while (NULL != (dir_entry = readdir(cur_dirp))) {
        /* ignore the . and .. entries */
        if (0 == strcmp(dir_entry->d_name, ".") || 0 == strcmp(dir_entry->d_name, "..")) {
            continue;
        }
        newdir = pmix_os_path(false, dirname, dir_entry->d_name, NULL);
        if (NULL == newdir) {
            /* nesting has taken the assembled name past PMIX_PATH_MAX,
             * or we are out of memory - either way there is nothing to
             * hand opendir() but a NULL */
            continue;
        }
        if (!search_candidate(newdir, &isdir)) {
            free(newdir);
            continue;
        }
        /* if it is a directory, down search - as far as we may */
        if (isdir) {
            if (0 < depth) {
                df_search(newdir, prefix, exact, info, ninfo, optional, connections, depth - 1);
            }
            free(newdir);
            continue;
        }
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "pmix:tool: checking %s vs %s", dir_entry->d_name, prefix);
        /* see if it is the file we want - by its whole name when a
         * particular server was asked for, since one name can be the start
         * of another ("tool.12" of "tool.123") */
        if (exact ? (0 == strcmp(dir_entry->d_name, prefix))
                  : (0 == strncmp(dir_entry->d_name, prefix, strlen(prefix)))) {
            /* try to read this file. One we cannot read or parse is
             * passed over rather than ending the search: a server killed
             * partway thru writing its file leaves exactly that behind,
             * and stopping at it hid any live server that readdir()
             * happened to list after it */
            pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                                "pmix:tool: reading file %s", newdir);
            rc = parse_conn_file(newdir, optional, true, true, connections);
            if (PMIX_ERR_NOMEM == rc) {
                free(newdir);
                closedir(cur_dirp);
                return rc;
            }
        }
        free(newdir);
    }
    closedir(cur_dirp);
    if (0 == pmix_list_get_size(connections)) {
        return PMIX_ERR_NOT_FOUND;
    }
    return PMIX_SUCCESS;
}

pmix_status_t pmix_ptl_base_df_search(char *dirname, char *prefix, bool exact,
                                      pmix_info_t info[], size_t ninfo,
                                      bool optional, pmix_list_t *connections)
{
    return df_search(dirname, prefix, exact, info, ninfo, optional, connections,
                     pmix_ptl_base.search_depth);
}

/* The address a URI names, from the transport whose URI it is - and
 * that transport, if asked. A URI no transport speaks is not supported */
pmix_status_t pmix_ptl_base_uri_to_addr(const char *uri, struct sockaddr_storage *connection,
                                        size_t *len, pmix_ptl_module_t **transport)
{
    pmix_ptl_base_active_t *active;
    pmix_status_t rc;

    PMIX_LIST_FOREACH (active, &pmix_ptl_base.actives, pmix_ptl_base_active_t) {
        rc = active->module->uri_to_addr(uri, connection, len);
        if (PMIX_ERR_NOT_SUPPORTED != rc) {
            if (PMIX_SUCCESS == rc && NULL != transport) {
                *transport = active->module;
            }
            return rc;
        }
    }
    PMIX_ERROR_LOG(PMIX_ERR_NOT_SUPPORTED);
    return PMIX_ERR_NOT_SUPPORTED;
}

pmix_status_t pmix_ptl_base_setup_connection(char *uri, struct sockaddr_storage *connection,
                                             size_t *len)
{
    return pmix_ptl_base_uri_to_addr(uri, connection, len, NULL);
}

bool pmix_ptl_base_addr_held(const struct sockaddr_storage *addr)
{
#if defined(HAVE_IFADDRS_H) && defined(HAVE_NET_IF_H)
    struct sockaddr_storage peer;
    const struct sockaddr_in6 *p6 = (const struct sockaddr_in6 *) &peer;
    const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *) addr;
    struct sockaddr_in *in4;
    struct in6_addr a6;
    struct ifaddrs *ifs, *ifa;
    bool held = false;

    memcpy(&peer, addr, sizeof(peer));
    /* an IPv4 peer of a dual-stack listener arrives v4-mapped */
    if (AF_INET6 == addr->ss_family && IN6_IS_ADDR_V4MAPPED(&in6->sin6_addr)) {
        memset(&peer, 0, sizeof(peer));
        in4 = (struct sockaddr_in *) &peer;
        in4->sin_family = AF_INET;
        memcpy(&in4->sin_addr, &in6->sin6_addr.s6_addr[12], sizeof(in4->sin_addr));
    }
    if (AF_INET != peer.ss_family && AF_INET6 != peer.ss_family) {
        return false;
    }
    if (pmix_net_islocalhost((struct sockaddr *) &peer)) {
        return true;
    }
    if (0 != getifaddrs(&ifs)) {
        return false;
    }
    for (ifa = ifs; !held && NULL != ifa; ifa = ifa->ifa_next) {
        if (NULL == ifa->ifa_addr || ifa->ifa_addr->sa_family != peer.ss_family) {
            continue;
        }
        if (AF_INET == peer.ss_family) {
            held = (0 == memcmp(&((struct sockaddr_in *) ifa->ifa_addr)->sin_addr,
                                &((struct sockaddr_in *) &peer)->sin_addr,
                                sizeof(struct in_addr)));
            continue;
        }
        memcpy(&a6, &((struct sockaddr_in6 *) ifa->ifa_addr)->sin6_addr, sizeof(a6));
        if (IN6_IS_ADDR_LINKLOCAL(&a6)) {
            /* BSD lists a link-local address with its scope written into
             * bytes 2 and 3, which a peer's address does not carry - and
             * it is ours only on the link it is held on */
            a6.s6_addr[2] = 0;
            a6.s6_addr[3] = 0;
            held = (0 == memcmp(&a6, &p6->sin6_addr, sizeof(a6))) &&
                   p6->sin6_scope_id == if_nametoindex(ifa->ifa_name);
        } else {
            held = (0 == memcmp(&a6, &p6->sin6_addr, sizeof(a6)));
        }
    }
    freeifaddrs(ifs);
    return held;
#else
    PMIX_HIDE_UNUSED_PARAMS(addr);
    return false;
#endif
}

bool pmix_ptl_base_addr_on_host(const struct sockaddr_storage *addr)
{
    struct sockaddr_storage peer, ifaddr;
    const struct sockaddr_in6 *in6;
    struct sockaddr_in *in4;
    int i;

    memcpy(&peer, addr, sizeof(peer));
    /* an IPv4 peer of a dual-stack listener arrives v4-mapped */
    in6 = (const struct sockaddr_in6 *) addr;
    if (AF_INET6 == addr->ss_family && IN6_IS_ADDR_V4MAPPED(&in6->sin6_addr)) {
        memset(&peer, 0, sizeof(peer));
        in4 = (struct sockaddr_in *) &peer;
        in4->sin_family = AF_INET;
        memcpy(&in4->sin_addr, &in6->sin6_addr.s6_addr[12], sizeof(in4->sin_addr));
    }
    if (AF_INET != peer.ss_family && AF_INET6 != peer.ss_family) {
        return false;
    }
    if (pmix_net_islocalhost((struct sockaddr *) &peer)) {
        return true;
    }
    for (i = pmix_ifbegin(); 0 <= i; i = pmix_ifnext(i)) {
        if (PMIX_SUCCESS != pmix_ifindextoaddr(i, (struct sockaddr *) &ifaddr, sizeof(ifaddr)) ||
            ifaddr.ss_family != peer.ss_family) {
            continue;
        }
        if (AF_INET == peer.ss_family &&
            0 == memcmp(&((struct sockaddr_in *) &ifaddr)->sin_addr,
                        &((struct sockaddr_in *) &peer)->sin_addr, sizeof(struct in_addr))) {
            return true;
        }
        if (AF_INET6 == peer.ss_family &&
            0 == memcmp(&((struct sockaddr_in6 *) &ifaddr)->sin6_addr,
                        &((struct sockaddr_in6 *) &peer)->sin6_addr, sizeof(struct in6_addr))) {
            /* a link-local address is this host's only on the link it is
             * ours on: another host on another link may hold the same one */
            if (IN6_IS_ADDR_LINKLOCAL(&((struct sockaddr_in6 *) &peer)->sin6_addr) &&
                (int) ((struct sockaddr_in6 *) &peer)->sin6_scope_id != pmix_ifindextokindex(i)) {
                continue;
            }
            return true;
        }
    }
    return false;
}

pmix_status_t pmix_ptl_base_parse_local_uri(const char *value, char **uri, uid_t *uid,
                                            gid_t *gid)
{
    const char *id;
    char *end;
    unsigned long u, g;
    size_t n;

    /* "<nspace>.<rank>;<address>;<uid>:<gid>" - the identity is after the
     * last ';', which no address of ours holds */
    if (NULL == value || NULL == (id = strrchr(value, ';')) || id == value) {
        return PMIX_ERR_BAD_PARAM;
    }
    errno = 0;
    u = strtoul(id + 1, &end, 10);
    if (0 != errno || end == id + 1 || ':' != *end || !isdigit((unsigned char) end[1])) {
        return PMIX_ERR_BAD_PARAM;
    }
    id = end + 1;
    g = strtoul(id, &end, 10);
    if (0 != errno || '\0' != *end || (unsigned long) (uid_t) u != u ||
        (unsigned long) (gid_t) g != g) {
        return PMIX_ERR_BAD_PARAM;
    }
    n = (size_t) (strrchr(value, ';') - value);
    *uri = (char *) malloc(n + 1);
    if (NULL == *uri) {
        return PMIX_ERR_NOMEM;
    }
    memcpy(*uri, value, n);
    (*uri)[n] = '\0';
    *uid = (uid_t) u;
    *gid = (gid_t) g;
    return PMIX_SUCCESS;
}

pmix_status_t pmix_ptl_base_use_local_env(const char *server, char **uri)
{
    const char *value, *a, *b;
    uid_t uid;
    gid_t gid;
    pmix_status_t rc;

    value = pmix_idenv_get(PMIX_PTL_LOCAL_URI_ENV);
    if (NULL == value) {
        return PMIX_ERR_NOT_FOUND;
    }
    rc = pmix_ptl_base_parse_local_uri(value, uri, &uid, &gid);
    if (PMIX_SUCCESS != rc) {
        if (PMIX_ERR_BAD_PARAM == rc) {
            pmix_show_help("help-ptl-base.txt", "local-uri-bad", true, PMIX_PTL_LOCAL_URI_ENV,
                           value);
            rc = PMIX_ERR_SILENT;
        }
        return rc;
    }
    /* It must name the server the rest of the environment does: a server
     * of an earlier release, started under one of this release, sets the
     * other variables but leaves this one as it found it */
    a = strchr(server, ';');
    b = strchr(*uri, ';');
    if (NULL == a || NULL == b || (a - server) != (b - *uri) ||
        0 != strncmp(server, *uri, (size_t) (a - server))) {
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "ptl:base: %s names another server than %s - not using it",
                            PMIX_PTL_LOCAL_URI_ENV, server);
        free(*uri);
        *uri = NULL;
        return PMIX_ERR_NOT_FOUND;
    }
    pmix_ptl_base.server_expect_given = true;
    pmix_ptl_base.server_expect_uid = uid;
    pmix_ptl_base.server_expect_gid = gid;
    return PMIX_SUCCESS;
}

/* A TCP server on this host that the kernel cannot name the user of, as
 * on a platform with no table of TCP connections: a process has it prove
 * who it is, as a server on another host does. A server known to be of an
 * earlier release has no such exchange, and is talked to as before. A
 * client is not asked to: its server is the one named for it, by the
 * environment it was started with or by its caller */
static pmix_status_t prove_unidentified(pmix_peer_t *peer, const struct sockaddr_storage *sa,
                                        bool vouched, bool *prove)
{
    uint8_t major = PMIX_PEER_MAJOR_VERSION(peer);
    pmix_psec_module_t *cur;
    char *ours, where[128];

    if (PMIX_PEER_IS_CLIENT(pmix_globals.mypeer) && !PMIX_PEER_IS_TOOL(pmix_globals.mypeer) &&
        !PMIX_PEER_IS_SINGLETON(pmix_globals.mypeer)) {
        return PMIX_SUCCESS;
    }
    snprintf(where, sizeof(where), "%s:%d", pmix_net_get_hostname((struct sockaddr *) sa),
             pmix_net_get_port((struct sockaddr *) sa));
    if (vouched && 0 != major && PMIX_MAJOR_WILDCARD != major && PMIX_PTL_LOCAL_MAJOR > major) {
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "ptl:base: the kernel names no user for the server at %s, of "
                            "release %u, which cannot prove who it is - connecting as before",
                            where, (unsigned) major);
        return PMIX_SUCCESS;
    }
    /* the mechanism it proves itself with is chosen once the exchange
     * starts - see pmix_ptl_base_auth_module. Here, only whether there is
     * one */
    cur = (NULL != peer->nptr && NULL != peer->nptr->compat.psec)
              ? peer->nptr->compat.psec
              : pmix_globals.mypeer->nptr->compat.psec;
    if (NULL == pmix_psec_base_remote_module(cur)) {
        ours = pmix_psec_base_get_available_modules();
        pmix_show_help("help-ptl-base.txt", "local-no-psec-tool", true, pmix_globals.hostname,
                       where, (NULL == ours) ? "none" : ours);
        free(ours);
        return PMIX_ERR_NOT_SUPPORTED;
    }
    *prove = true;
    return PMIX_SUCCESS;
}

pmix_status_t pmix_ptl_base_check_server(pmix_peer_t *peer, bool given, uid_t uid, gid_t gid,
                                         bool vouched, bool *prove)
{
    uid_t kuid;
    gid_t kgid = 0;
    pid_t kpid;
    size_t n;
    bool ok, tcp = false;
    char want[128];
    struct sockaddr_storage sa;
    pmix_socklen_t salen = sizeof(sa);
    pmix_status_t rc;

    *prove = false;
    if (NULL == peer->transport || NULL == peer->transport->peer_identity) {
        /* Over TCP, the kernel names the owner of the server's end of the
         * connection when the server is on this host, from its table of
         * TCP connections - a user, but no group */
        if (pmix_ptl_base_test_no_tcp_table) {
            rc = PMIX_ERR_NOT_SUPPORTED;
        } else {
            rc = pmix_util_getid_tcp(peer->sd, &kuid);
        }
        if (PMIX_SUCCESS != rc) {
            /* One at any address that is not this host's is on another
             * host, or in another network namespace, and the kernel says
             * nothing of it - see pmix_ptl_base_server_remote */
            memset(&sa, 0, sizeof(sa));
            if (0 != getpeername(peer->sd, (struct sockaddr *) &sa, &salen)) {
                return PMIX_SUCCESS;
            }
            /* With no table to ask, one at an address an interface of
             * this host holds - by the kernel's list, which names some
             * the interface list leaves out - proves who it is on this
             * connection alone (prove_unidentified) */
            if (PMIX_ERR_NOT_SUPPORTED == rc) {
                if (!pmix_ptl_base_tcp_peer_held(&sa)) {
                    return PMIX_SUCCESS;
                }
                return prove_unidentified(peer, &sa, vouched, prove);
            }
            if (pmix_ptl_base_tcp_peer_remote(&sa)) {
                return PMIX_SUCCESS;
            }
            /* One whose address is this host's has its end in that table,
             * as an established connection - but not always yet. Our end
             * is connected once the server's SYN-ACK arrives, and the
             * server's is established only when its kernel has taken our
             * ACK, which on loopback it may get to a moment later: until
             * then macOS holds it in SYN_RECEIVED, Linux as a request in
             * SYN_RECV, and FreeBSD in its syncache, outside the table.
             * Give it that moment */
            if (PMIX_ERR_NOT_FOUND == rc) {
                struct timeval tv = {0, 1000};

                for (n = 0; PMIX_ERR_NOT_FOUND == rc && n < 10; n++) {
                    retry_wait(&tv);
                    tv.tv_usec *= 2;
                    rc = pmix_util_getid_tcp(peer->sd, &kuid);
                }
            }
            if (PMIX_ERR_NOMEM == rc) {
                return rc;
            }
            /* Any other failure to read the table is no answer */
            if (PMIX_SUCCESS != rc) {
                pmix_show_help("help-ptl-base.txt", "server-unidentified", true,
                               pmix_globals.hostname);
                return PMIX_ERR_INVALID_CRED;
            }
        }
        tcp = true;
    } else if (PMIX_SUCCESS != peer->transport->peer_identity(peer->sd, &kuid, &kgid, &kpid)) {
        pmix_show_help("help-ptl-base.txt", "server-unidentified", true, pmix_globals.hostname);
        return PMIX_ERR_INVALID_CRED;
    }
    if (tcp) {
        /* a user, and no group to check */
        if (given) {
            ok = (kuid == uid);
            snprintf(want, sizeof(want), "uid %lu, as the server that started this process said",
                     (unsigned long) uid);
        } else {
            ok = (kuid == geteuid() || 0 == kuid);
            for (n = 0; !ok && n < pmix_ptl_base.naccess_uids; n++) {
                ok = (kuid == pmix_ptl_base.access_uids[n]);
            }
            snprintf(want, sizeof(want), "this process's own user, root, or a user "
                     "pmix_ptl_base_server_access names");
        }
        if (!ok) {
            pmix_show_help("help-ptl-base.txt", "server-identity-tcp", true,
                           pmix_globals.hostname, (unsigned long) kuid, want);
            return PMIX_ERR_INVALID_CRED;
        }
        return PMIX_SUCCESS;
    }
    return pmix_ptl_base_check_ids(kuid, kgid, given, uid, gid, false);
}

pmix_status_t pmix_ptl_base_check_ids(uid_t kuid, gid_t kgid, bool given, uid_t uid, gid_t gid,
                                      bool remote)
{
    size_t n;
    bool ok;
    char want[128];

    if (given) {
        /* the server that started us said who it is - in its own user
         * namespace, which is the parent of ours if we run in one: the
         * kernel reports the server's ids translated into ours. A server on
         * another host proved its ids there, and nothing here maps them */
        ok = (kuid == uid && kgid == gid);
        if (!ok && !remote) {
            uint32_t puid, pgid;

            ok = (PMIX_SUCCESS == pmix_util_id_to_parent("/proc/self/uid_map", (uint32_t) kuid,
                                                         &puid) &&
                  PMIX_SUCCESS == pmix_util_id_to_parent("/proc/self/gid_map", (uint32_t) kgid,
                                                         &pgid) &&
                  puid == (uint32_t) uid && pgid == (uint32_t) gid);
        }
        snprintf(want, sizeof(want), "uid %lu gid %lu, as the server that started this "
                 "process said", (unsigned long) uid, (unsigned long) gid);
    } else {
        ok = (kuid == geteuid() || 0 == kuid);
        for (n = 0; !ok && n < pmix_ptl_base.naccess_uids; n++) {
            ok = (kuid == pmix_ptl_base.access_uids[n]);
        }
        for (n = 0; !ok && n < pmix_ptl_base.naccess_gids; n++) {
            ok = (kgid == pmix_ptl_base.access_gids[n]);
        }
        snprintf(want, sizeof(want), "this process's own user, root, or one "
                 "pmix_ptl_base_server_access allows");
    }
    if (!ok) {
        if (remote) {
            pmix_show_help("help-ptl-base.txt", "server-identity-remote", true,
                           pmix_globals.hostname, (unsigned long) kuid, (unsigned long) kgid,
                           want);
        } else {
            pmix_show_help("help-ptl-base.txt", "server-identity", true, pmix_globals.hostname,
                           (unsigned long) kuid, (unsigned long) kgid, want);
        }
        return PMIX_ERR_INVALID_CRED;
    }
    return PMIX_SUCCESS;
}

/* One name or number of a ptl_base_server_access list, as an id */
static bool access_id(const char *item, bool user, unsigned long *id)
{
    uint32_t val;
    pmix_status_t rc;

    rc = user ? pmix_util_uid_from_string(item, &val) : pmix_util_gid_from_string(item, &val);
    if (PMIX_SUCCESS != rc) {
        return false;
    }
    *id = (unsigned long) val;
    return true;
}

pmix_status_t pmix_ptl_base_parse_server_access(const char *spec)
{
    char **dirs = NULL, **items = NULL, *val;
    bool users_seen = false, groups_seen = false, user;
    unsigned long id;
    size_t n, m, cnt;
    pmix_status_t rc = PMIX_SUCCESS;

    pmix_ptl_base.access_uids = NULL;
    pmix_ptl_base.naccess_uids = 0;
    pmix_ptl_base.access_gids = NULL;
    pmix_ptl_base.naccess_gids = 0;
    if (NULL == spec || '\0' == spec[0]) {
        return PMIX_SUCCESS;
    }
    dirs = PMIx_Argv_split(spec, ',');
    for (n = 0; NULL != dirs && NULL != dirs[n]; n++) {
        if (0 == strncasecmp(dirs[n], "users=", 6)) {
            if (users_seen) {
                rc = PMIX_ERR_BAD_PARAM;
                break;
            }
            users_seen = true;
            user = true;
            val = dirs[n] + 6;
        } else if (0 == strncasecmp(dirs[n], "groups=", 7)) {
            if (groups_seen) {
                rc = PMIX_ERR_BAD_PARAM;
                break;
            }
            groups_seen = true;
            user = false;
            val = dirs[n] + 7;
        } else {
            rc = PMIX_ERR_BAD_PARAM;
            break;
        }
        items = PMIx_Argv_split(val, ':');
        cnt = PMIx_Argv_count(items);
        if (0 == cnt) {
            rc = PMIX_ERR_BAD_PARAM;
            break;
        }
        for (m = 0; m < cnt; m++) {
            if (!access_id(items[m], user, &id)) {
                rc = PMIX_ERR_BAD_PARAM;
                break;
            }
            if (user) {
                uid_t *tmp = (uid_t *) realloc(pmix_ptl_base.access_uids,
                                               (pmix_ptl_base.naccess_uids + 1) * sizeof(uid_t));
                if (NULL == tmp) {
                    rc = PMIX_ERR_NOMEM;
                    break;
                }
                pmix_ptl_base.access_uids = tmp;
                pmix_ptl_base.access_uids[pmix_ptl_base.naccess_uids++] = (uid_t) id;
            } else {
                gid_t *tmp = (gid_t *) realloc(pmix_ptl_base.access_gids,
                                               (pmix_ptl_base.naccess_gids + 1) * sizeof(gid_t));
                if (NULL == tmp) {
                    rc = PMIX_ERR_NOMEM;
                    break;
                }
                pmix_ptl_base.access_gids = tmp;
                pmix_ptl_base.access_gids[pmix_ptl_base.naccess_gids++] = (gid_t) id;
            }
        }
        PMIx_Argv_free(items);
        items = NULL;
        if (PMIX_SUCCESS != rc) {
            break;
        }
    }
    PMIx_Argv_free(items);
    PMIx_Argv_free(dirs);
    if (PMIX_SUCCESS != rc) {
        if (PMIX_ERR_BAD_PARAM == rc) {
            pmix_show_help("help-ptl-base.txt", "server-access-bad", true, spec);
            rc = PMIX_ERR_SILENT;
        }
        free(pmix_ptl_base.access_uids);
        pmix_ptl_base.access_uids = NULL;
        pmix_ptl_base.naccess_uids = 0;
        free(pmix_ptl_base.access_gids);
        pmix_ptl_base.access_gids = NULL;
        pmix_ptl_base.naccess_gids = 0;
    }
    return rc;
}

static pmix_status_t send_connect_ack(pmix_peer_t *peer, pmix_info_t iptr[], size_t niptr,
                                      const pmix_byte_object_t *bind, uid_t to)
{
    char *msg;
    size_t sdsize = 0;
    pmix_status_t rc;

    pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                        "pmix:ptl SEND CONNECT ACK");

    /* set our ID flag and compute the required handshake size */
    peer->proc_type.flag = pmix_ptl_base_set_flag(&sdsize);

    /* construct the contact message */
    rc = construct_message(peer, &msg, &sdsize, iptr, niptr, bind, to);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    /* send the entire message across */
    /* the message carries our token, if we have one: it goes no further */
    if (PMIX_SUCCESS != pmix_ptl_base_send_blocking(peer->sd, msg, sdsize)) {
        pmix_ptl_base_wipe(msg, sdsize);
        free(msg);
        return PMIX_ERR_UNREACH;
    }
    pmix_ptl_base_wipe(msg, sdsize);
    free(msg);
    return PMIX_SUCCESS;
}

/* A server that does not have the wire format or security mechanism we
 * named closes the connection without a word. When we were handed only
 * its address there was nothing to choose from - no version, no list of
 * what it accepts - so say how to give us that: its file. A server whose
 * version we know was found through a file or the environment already */
static void hint_closed_no_reply(pmix_peer_t *peer, const char *uri)
{
    static bool warned = false;
    uint8_t major = PMIX_PEER_MAJOR_VERSION(peer);

    if (warned || (0 != major && PMIX_MAJOR_WILDCARD != major)) {
        return;
    }
    warned = true;
    pmix_show_help("help-ptl-base.txt", "server-closed-no-reply", true,
                   (NULL == uri) ? "(the URI given)" : uri);
}

/* we receive a connection acknowledgment from the server,
 * consisting of nothing more than a status report. If success,
 * then we initiate authentication method */

static pmix_status_t recv_connect_ack(pmix_peer_t *peer)
{
    pmix_status_t reply;
    pmix_status_t rc;
    struct timeval save;
    pmix_socklen_t sz = sizeof(save);
    bool sockopt = true;
    uint32_t u32;

    pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                        "pmix: RECV CONNECT ACK FROM SERVER");

    /* set the socket timeout so we don't hang on blocking recv */
    rc = pmix_ptl_base_set_timeout(peer, &save, &sz, &sockopt);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }

    /* receive the status reply */
    rc = pmix_ptl_base_recv_blocking(peer->sd, (char *) &u32, sizeof(uint32_t));
    if (PMIX_SUCCESS != rc) {
        if (PMIX_ERR_UNREACH == rc) {
            hint_closed_no_reply(peer, pmix_ptl_base.uri);
        }
        if (sockopt) {
            /* return the socket to normal */
            if (0 != setsockopt(peer->sd, SOL_SOCKET, SO_RCVTIMEO, &save, sz)) {
                pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                                    "pmix: could not reset setsockopt SO_RCVTIMEO");
            }
        }
        return rc;
    }
    reply = pmix_ptl_base_server_status(ntohl(u32));

    if ((PMIX_PEER_IS_CLIENT(pmix_globals.mypeer) ||
         PMIX_PEER_IS_SINGLETON(pmix_globals.mypeer)) &&
        !PMIX_PEER_IS_TOOL(pmix_globals.mypeer)) {
        rc = pmix_ptl_base_client_handshake(peer, reply);
    } else { // we are a tool
        rc = pmix_ptl_base_tool_handshake(peer, reply);
    }

    if (sockopt) {
        /* return the socket to normal */
        if (0 != setsockopt(peer->sd, SOL_SOCKET, SO_RCVTIMEO, &save, sz)) {
            pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                                "pmix: could not reset setsockopt SO_RCVTIMEO");
        }
    }

    return rc;
}

/* The oldest server we will talk to is v3.2. Its version is known by
 * now on every path that can know it - the PMIX_SERVER_URI variable and
 * PMIX_VERSION for a client, or the version line of a rendezvous file,
 * whose absence marks a v2.0 server. A server reached through a URI handed
 * to us directly has none recorded, and is not refused. Refuse before
 * connecting: an older server accepts the connection and then leaves us
 * waiting on answers it never sends. */
static pmix_status_t refuse_outdated(pmix_peer_t *peer)
{
    uint8_t major = PMIX_PEER_MAJOR_VERSION(peer);
    uint8_t minor = PMIX_PEER_MINOR_VERSION(peer);

    if (0 != major && PMIX_MAJOR_WILDCARD != major &&
        (3 > major || (3 == major && PMIX_MINOR_WILDCARD != minor && 2 > minor))) {
        pmix_show_help("help-ptl-base.txt", "unsupported-server-version", true,
                       (int) major, (int) minor);
        return PMIX_ERR_OUTDATED;
    }
    return PMIX_SUCCESS;
}

pmix_status_t pmix_ptl_base_make_connection(pmix_peer_t *peer, char *suri,
                                            pmix_info_t *iptr, size_t niptr)
{
    pmix_status_t rc;
    char *copy = suri;

    rc = pmix_ptl_base_make_connection_alts(peer, &copy, NULL, iptr, niptr);
    /* with no alternates the address cannot have changed */
    return rc;
}

/* Connect a socket to the first of the server's addresses that answers.
 *
 * Only a failure to connect moves on to the next address: once a socket is
 * up, whatever the handshake says is the server's answer, and asking again
 * at another of its addresses would only get the same one. The primary
 * address must parse - it is the one every discovery route validated - but
 * an alternate that does not is passed over, since it came from a file or a
 * directive an older or foreign writer may have garbled. */
static pmix_status_t connect_any(char **suri, const char *alt_uris,
                                 struct sockaddr_storage *addr, size_t *len, int *sd,
                                 pmix_ptl_module_t **transport)
{
    char **alts = NULL, *used;
    pmix_status_t rc;
    int n;

    rc = pmix_ptl_base_uri_to_addr(*suri, addr, len, transport);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    rc = pmix_ptl_base_connect(addr, *len, sd);
    if (PMIX_SUCCESS == rc || NULL == alt_uris) {
        /* do not error log - might just be a stale connection point */
        return rc;
    }

    alts = PMIx_Argv_split(alt_uris, ',');
    for (n = 0; NULL != alts && NULL != alts[n]; n++) {
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "ptl:base: %s unreachable - trying alternate %s", *suri, alts[n]);
        if (PMIX_SUCCESS != pmix_ptl_base_uri_to_addr(alts[n], addr, len, transport)) {
            continue;
        }
        rc = pmix_ptl_base_connect(addr, *len, sd);
        if (PMIX_SUCCESS == rc) {
            used = strdup(alts[n]);
            if (NULL == used) {
                CLOSE_THE_SOCKET(*sd);
                PMIx_Argv_free(alts);
                return PMIX_ERR_NOMEM;
            }
            free(*suri);
            *suri = used;
            break;
        }
    }
    PMIx_Argv_free(alts);
    return rc;
}

/* A tool a server started takes the namespace and rank in the environment
 * it inherited - a tool run from a job script takes the script's. If that
 * rank already has a connection, the server refuses the tool with
 * PMIX_ERR_EXISTS: several tools a script starts cannot all be the
 * script's rank. Such a tool then connects again as a tool that needs an
 * identity, and the host gives it a namespace of its own. Only an identity
 * the tool took from its environment: one its caller gave it is the one
 * the caller asked for. Only while the tool is initializing: once
 * PMIx_tool_init has returned, the caller has been told who it is.
 * Returns true if the tool is to connect again */
static bool take_new_identity(void)
{
    const char *ns, *rk;
    char *end;
    unsigned long rank;

    if (pmix_atomic_check_bool(&pmix_globals.initialized) ||
        !PMIX_PEER_IS_TOOL(pmix_globals.mypeer) ||
        !PMIX_PEER_IS_CLIENT(pmix_globals.mypeer)) {
        return false;
    }
    ns = pmix_idenv_get("PMIX_NAMESPACE");
    rk = pmix_idenv_get("PMIX_RANK");
    if (NULL == ns || NULL == rk || !PMIx_Check_nspace(ns, pmix_globals.myid.nspace)) {
        return false;
    }
    errno = 0;
    rank = strtoul(rk, &end, 10);
    if (0 != errno || end == rk || '\0' != *end || rank != (unsigned long) pmix_globals.myid.rank) {
        return false;
    }
    pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                        "pmix: %s:%u already has a connection - connecting again as a tool "
                        "that needs an identity",
                        pmix_globals.myid.nspace, pmix_globals.myid.rank);
    pmix_globals.mypeer->proc_type.type &= ~(PMIX_PROC_CLIENT | PMIX_PROC_CLIENT_TOOL_ACT);
    memset(pmix_globals.myid.nspace, 0, sizeof(pmix_globals.myid.nspace));
    pmix_globals.myid.rank = PMIX_RANK_INVALID;
    if (NULL != pmix_globals.mypeer->info) {
        free(pmix_globals.mypeer->info->pname.nspace);
        pmix_globals.mypeer->info->pname.nspace = NULL;
        pmix_globals.mypeer->info->pname.rank = PMIX_RANK_INVALID;
    }
    return true;
}

pmix_status_t pmix_ptl_base_make_connection_alts(pmix_peer_t *peer, char **suri,
                                                 const char *alt_uris,
                                                 pmix_info_t *iptr, size_t niptr)
{
    struct sockaddr_storage myconnection;
    pmix_ptl_module_t *transport = NULL;
    pmix_byte_object_t bind = PMIX_BYTE_OBJECT_STATIC_INIT;
    pmix_status_t rc;
    size_t len;
    int retries = 0;
    bool renamed = false, prove = false, proved, unidentified;
    uid_t srvuid = (uid_t) -1;

    rc = refuse_outdated(peer);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }

    /* connect to whichever of the server's addresses answers first */
    rc = connect_any(suri, alt_uris, &myconnection, &len, &peer->sd, &transport);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    peer->transport = transport;
    goto connected;

retry:
    /* the server asked us to try the handshake again - at the address that
     * answered */
    if (PMIX_SUCCESS != (rc = pmix_ptl_base_connect(&myconnection, len, &peer->sd))) {
        return renamed ? PMIX_ERR_EXISTS : rc;
    }

connected:

    /* the server must be one we may talk to, by what the kernel says of it */
    rc = pmix_ptl_base_check_server(peer, pmix_ptl_base.server_expect_given,
                                    pmix_ptl_base.server_expect_uid,
                                    pmix_ptl_base.server_expect_gid,
                                    pmix_ptl_base.server_release_vouched, &unidentified);
    if (PMIX_SUCCESS != rc) {
        CLOSE_THE_SOCKET(peer->sd);
        return rc;
    }

    /* a server on another host proves who it is before we send anything
     * of ours, and our credential is then made for this connection - as
     * does one that told us to, which sees us as on another host, and one
     * on this host the kernel cannot name */
    proved = prove || unidentified || pmix_ptl_base_server_remote(peer);
    if (proved) {
        rc = pmix_ptl_base_auth_server(peer, pmix_ptl_base.server_expect_given,
                                       pmix_ptl_base.server_expect_uid,
                                       pmix_ptl_base.server_expect_gid, unidentified, &bind,
                                       &srvuid);
        if (PMIX_SUCCESS != rc) {
            CLOSE_THE_SOCKET(peer->sd);
            return rc;
        }
    }

    /* send our identity and any authentication credentials to the server */
    rc = send_connect_ack(peer, iptr, niptr, (NULL == bind.bytes) ? NULL : &bind, srvuid);
    PMIX_BYTE_OBJECT_DESTRUCT(&bind);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        CLOSE_THE_SOCKET(peer->sd);
        return renamed ? PMIX_ERR_EXISTS : rc;
    }

    /* do whatever handshake is required */
    if (PMIX_SUCCESS != (rc = recv_connect_ack(peer))) {
        CLOSE_THE_SOCKET(peer->sd);
        if (PMIX_ERR_TEMP_UNAVAILABLE == rc) {
            ++retries;
            if (retries < pmix_ptl_base.handshake_max_retries) {
                goto retry;
            }
        }
        if (PMIX_ERR_EXISTS == rc && !renamed && take_new_identity()) {
            renamed = true;
            goto retry;
        }
        if (PMIX_ERR_PROVE_FIRST == rc && !proved) {
            /* it sees us as on another host - a port forward, say */
            prove = true;
            goto retry;
        }
        /* a tool that could not come up under a namespace of its own is
         * told why it needed one: the identity it was started with is
         * taken */
        return renamed ? PMIX_ERR_EXISTS : rc;
    }
    /* Assign the lower half of the tag space for sendrecvs */
    peer->dyn_tags_start    = PMIX_PTL_TAG_DYNAMIC;
    peer->dyn_tags_current  = PMIX_PTL_TAG_DYNAMIC;
    peer->dyn_tags_end      = PMIX_PTL_TAG_DYNAMIC + (UINT32_MAX - PMIX_PTL_TAG_DYNAMIC)/2;

    return PMIX_SUCCESS;
}

/* Record the identity of the server a peer object stands for. Both copies
 * of the name are made before either is installed, so a failure leaves
 * the peer's previous identity - if it had one - intact */
static pmix_status_t set_server_id(pmix_peer_t *peer, const char *nspace, pmix_rank_t rank)
{
    char *ns1, *ns2;

    if (NULL == peer->info) {
        peer->info = PMIX_NEW(pmix_rank_info_t);
        if (NULL == peer->info) {
            return PMIX_ERR_NOMEM;
        }
    }
    if (NULL == peer->nptr) {
        peer->nptr = PMIX_NEW(pmix_namespace_t);
        if (NULL == peer->nptr) {
            return PMIX_ERR_NOMEM;
        }
    }
    ns1 = strdup(nspace);
    ns2 = strdup(nspace);
    if (NULL == ns1 || NULL == ns2) {
        free(ns1);
        free(ns2);
        return PMIX_ERR_NOMEM;
    }
    if (NULL != peer->nptr->nspace) {
        free(peer->nptr->nspace);
    }
    peer->nptr->nspace = ns1;
    if (NULL != peer->info->pname.nspace) {
        free(peer->info->pname.nspace);
    }
    peer->info->pname.nspace = ns2;
    peer->info->pname.rank = rank;
    return PMIX_SUCCESS;
}

/* Everything that can fail is done before the process is marked
 * connected and the socket's events are armed. On failure the socket the
 * handshake just completed on is closed: nothing will ever service it */
pmix_status_t pmix_ptl_base_complete_connection(pmix_peer_t *peer, char *nspace,
                                                pmix_rank_t rank)
{
    pmix_status_t rc;

    /* setup the server info */
    rc = set_server_id(peer, nspace, rank);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        CLOSE_THE_SOCKET(peer->sd);
        return rc;
    }

    pmix_atomic_set_bool(&pmix_globals.connected);
    peer->upstream = true;

    pmix_ptl_base_set_nonblocking(peer->sd);

    /* setup recv event */
    pmix_event_assign(&peer->recv_event, pmix_globals.evbase, peer->sd, EV_READ | EV_PERSIST,
                      pmix_ptl_base_recv_handler, peer);
    peer->recv_ev_active = true;
    PMIX_POST_OBJECT(peer);
    pmix_event_add(&peer->recv_event, 0);

    /* setup send event */
    pmix_event_assign(&peer->send_event, pmix_globals.evbase, peer->sd, EV_WRITE | EV_PERSIST,
                      pmix_ptl_base_send_handler, peer);
    peer->send_ev_active = false;
    /* A send may have been queued while this event had no base - see
     * pmix_ptl_base_send(). It could not be activated then; activate it
     * now, or it waits for a send that may never come. */
    if (NULL != peer->send_msg) {
        if (0 == pmix_event_add(&peer->send_event, 0)) {
            peer->send_ev_active = true;
        }
    }
    return PMIX_SUCCESS;
}

/****    EVENT-DRIVEN CONNECT    ****
 *
 * pmix_ptl_base_make_connection() and the handshake after it wait on the
 * server with blocking calls. That is fine for a caller whose own thread
 * it is - PMIx_Init, PMIx_tool_init - but a tool attaching to a server
 * does it from a thread-shift handler, and then the whole progress thread
 * waits: every other connection, event and callback in the process, for
 * as long as the server takes, or forever if it accepts and never answers.
 *
 * This is the same exchange driven from socket events instead. It sends
 * the identical connect-ack and reads the identical replies - nothing on
 * the wire changes - but never waits for bytes that have not arrived: each
 * reply field is collected as it comes in, the operation keeps its place,
 * and the progress thread goes back to its other work in between. One
 * timer, ptl_base_handshake_wait_time, bounds the whole connect. */

typedef enum {
    PMIX_CNCT_CONNECTING,
    /* a server on another host proving who it is - see "Between hosts" in
     * base.h */
    PMIX_CNCT_AUTH_SENDING,
    PMIX_CNCT_AUTH_STATUS,
    PMIX_CNCT_AUTH_NONCE,
    PMIX_CNCT_AUTH_LEN,
    PMIX_CNCT_AUTH_CRED,
    PMIX_CNCT_SENDING,
    PMIX_CNCT_STATUS,
    PMIX_CNCT_MY_NSPACE,
    PMIX_CNCT_MY_RANK,
    PMIX_CNCT_SRV_NSPACE,
    PMIX_CNCT_SRV_RANK,
    PMIX_CNCT_SEC_STATUS,
    PMIX_CNCT_PINDEX
} pmix_cnct_state_t;

typedef struct {
    pmix_list_item_t super;
    pmix_peer_t *peer;          // borrowed - the caller owns it until cbfunc
    char *nspace;               // the server's identity from locating it
    pmix_rank_t rank;
    char *suri;                 // the address being tried - the one connected, in the end
    char **alts;                // the server's other addresses, still to try
    int nextalt;
    pmix_info_t *iptr;          // the info blob for the connect-ack
    size_t niptr;
    struct sockaddr_storage addr;
    size_t addrlen;
    pmix_ptl_module_t *transport; // whose address addr is
    bool expect_given;            // the server's identity, if we were told it
    bool vouched;                 // an earlier release its version names may be believed
    uid_t expect_uid;
    gid_t expect_gid;
    int sd;
    int connect_tries;
    int handshake_tries;
    pmix_cnct_state_t state;
    char *msg;                  // the connect-ack being sent
    size_t msglen;
    size_t sent;
    char field[PMIX_MAX_NSLEN + 1]; // the reply field being read
    char *rbuf;                 // where it is read to: field, or cred.bytes
    size_t fieldlen;
    size_t got;
    /* a server on another host: our nonce and its, the credential it
     * proves itself with, and the module we use with it */
    unsigned char auth_tool[PMIX_PTL_AUTH_NONCE_LEN];
    unsigned char auth_srv[PMIX_PTL_AUTH_NONCE_LEN];
    pmix_byte_object_t cred;
    pmix_psec_module_t *auth_psec;
    bool prove;                  // the server told us to have it prove who it is
    bool auth_local;             // it is on this host, and the kernel cannot name it
    pmix_event_t ev;
    bool ev_active;
    pmix_event_t timer;
    bool timer_active;
    pmix_ptl_connect_nb_cbfunc_t cbfunc;
    void *cbdata;
} pmix_ptl_connect_op_t;

static void cnopcon(pmix_ptl_connect_op_t *p)
{
    p->peer = NULL;
    p->nspace = NULL;
    p->rank = PMIX_RANK_UNDEF;
    p->suri = NULL;
    p->alts = NULL;
    p->nextalt = 0;
    p->iptr = NULL;
    p->niptr = 0;
    memset(&p->addr, 0, sizeof(p->addr));
    p->addrlen = 0;
    p->transport = NULL;
    p->expect_given = false;
    p->vouched = false;
    p->expect_uid = (uid_t) -1;
    p->expect_gid = (gid_t) -1;
    p->sd = -1;
    p->connect_tries = 0;
    p->handshake_tries = 0;
    p->state = PMIX_CNCT_CONNECTING;
    p->msg = NULL;
    p->msglen = 0;
    p->sent = 0;
    memset(p->field, 0, sizeof(p->field));
    p->rbuf = p->field;
    p->fieldlen = 0;
    p->got = 0;
    memset(p->auth_tool, 0, sizeof(p->auth_tool));
    memset(p->auth_srv, 0, sizeof(p->auth_srv));
    PMIX_BYTE_OBJECT_CONSTRUCT(&p->cred);
    p->auth_psec = NULL;
    p->prove = false;
    p->auth_local = false;
    memset(&p->ev, 0, sizeof(p->ev));
    p->ev_active = false;
    memset(&p->timer, 0, sizeof(p->timer));
    p->timer_active = false;
    p->cbfunc = NULL;
    p->cbdata = NULL;
}
static void cnopdes(pmix_ptl_connect_op_t *p)
{
    if (p->ev_active) {
        pmix_event_del(&p->ev);
    }
    if (p->timer_active) {
        pmix_event_evtimer_del(&p->timer);
    }
    if (0 <= p->sd) {
        CLOSE_THE_SOCKET(p->sd);
    }
    free(p->nspace);
    free(p->suri);
    PMIx_Argv_free(p->alts);
    if (NULL != p->iptr) {
        PMIX_INFO_FREE(p->iptr, p->niptr);
    }
    free(p->msg);
    PMIX_BYTE_OBJECT_DESTRUCT(&p->cred);
}
static PMIX_CLASS_INSTANCE(pmix_ptl_connect_op_t,
                           pmix_list_item_t,
                           cnopcon, cnopdes);

static void cnct_begin(int sd, short args, void *cbdata);
static void cnct_connected(int sd, short args, void *cbdata);
static void cnct_send(int sd, short args, void *cbdata);
static void cnct_recv(int sd, short args, void *cbdata);
static void cnct_send_ack(pmix_ptl_connect_op_t *op, const pmix_byte_object_t *bind, uid_t to);

/* Report the outcome and retire the operation. Everything it holds but the
 * URI - which goes to the callback - is released here, and on failure the
 * socket with it */
static void cnct_finish(pmix_ptl_connect_op_t *op, pmix_status_t status)
{
    char *suri;

    if (op->ev_active) {
        pmix_event_del(&op->ev);
        op->ev_active = false;
    }
    if (op->timer_active) {
        pmix_event_evtimer_del(&op->timer);
        op->timer_active = false;
    }
    pmix_list_remove_item(&pmix_ptl_base.connecting, &op->super);
    if (PMIX_SUCCESS == status) {
        /* the socket is the peer's now */
        op->sd = -1;
    } else if (NULL != op->peer) {
        op->peer->sd = -1;
    }
    suri = op->suri;
    op->suri = NULL;
    op->cbfunc(status, (struct pmix_peer_t *) op->peer, suri, op->cbdata);
    PMIX_RELEASE(op);
}

static void cnct_expired(int sd, short args, void *cbdata)
{
    pmix_ptl_connect_op_t *op = (pmix_ptl_connect_op_t *) cbdata;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_ACQUIRE_OBJECT(op);
    op->timer_active = false;
    pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                        "ptl:base:connect: no answer from %s within %d seconds",
                        (NULL == op->suri) ? "server" : op->suri,
                        pmix_ptl_base.handshake_wait_time);
    if (PMIX_CNCT_AUTH_STATUS == op->state && NULL != op->auth_psec) {
        pmix_ptl_base_auth_refused(op->peer, op->auth_psec->name, op->auth_local,
                                   PMIX_ERR_TIMEOUT);
    }
    cnct_finish(op, PMIX_ERR_TIMEOUT);
}

/* wait for the socket to become readable or writable, then run fn */
static pmix_status_t cnct_wait(pmix_ptl_connect_op_t *op, short what,
                               event_callback_fn fn)
{
    pmix_event_assign(&op->ev, pmix_globals.evbase, op->sd, what, fn, op);
    PMIX_POST_OBJECT(op);
    if (0 != pmix_event_add(&op->ev, NULL)) {
        return PMIX_ERROR;
    }
    op->ev_active = true;
    return PMIX_SUCCESS;
}

/* expect a reply field of this many bytes next */
static pmix_status_t cnct_expect(pmix_ptl_connect_op_t *op, pmix_cnct_state_t state,
                                 size_t len)
{
    op->state = state;
    op->rbuf = op->field;
    op->fieldlen = len;
    op->got = 0;
    memset(op->field, 0, sizeof(op->field));
    return cnct_wait(op, EV_READ, cnct_recv);
}

/* Start over with a fresh socket - a failed attempt to connect, or a
 * server that asked us to retry the handshake */
static void cnct_restart(pmix_ptl_connect_op_t *op)
{
    if (0 <= op->sd) {
        CLOSE_THE_SOCKET(op->sd);
        op->sd = -1;
    }
    op->peer->sd = -1;
    free(op->msg);
    op->msg = NULL;
    PMIX_BYTE_OBJECT_DESTRUCT(&op->cred);
    cnct_begin(-1, 0, op);
}

static void cnct_begin(int sd, short args, void *cbdata)
{
    pmix_ptl_connect_op_t *op = (pmix_ptl_connect_op_t *) cbdata;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_ACQUIRE_OBJECT(op);
    op->ev_active = false;

    /* the same attempts pmix_ptl_base_connect makes, without waiting */
    while (PMIX_MAX_RETRIES >= op->connect_tries++) {
        op->sd = socket(op->addr.ss_family, SOCK_STREAM | PMIX_SOCK_CLOEXEC, 0);
        if (0 > op->sd) {
            continue;
        }
        pmix_ptl_base_own_socket(op->sd);
        pmix_ptl_base_set_nonblocking(op->sd);
        if (0 == connect(op->sd, (struct sockaddr *) &op->addr, op->addrlen)) {
            cnct_connected(op->sd, EV_WRITE, op);
            return;
        }
        if (EINPROGRESS == pmix_socket_errno) {
            if (PMIX_SUCCESS == cnct_wait(op, EV_WRITE, cnct_connected)) {
                return;
            }
            cnct_finish(op, PMIX_ERROR);
            return;
        }
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "ptl:base:connect: connect failed: %s (%d)",
                            strerror(pmix_socket_errno), pmix_socket_errno);
        CLOSE_THE_SOCKET(op->sd);
        op->sd = -1;
    }

    /* this address never answered - move on to the server's next one. Only
     * a failure to connect does this; see connect_any */
    while (NULL != op->alts && NULL != op->alts[op->nextalt]) {
        const char *next = op->alts[op->nextalt++];
        char *copy;

        if (PMIX_SUCCESS != pmix_ptl_base_uri_to_addr(next, &op->addr, &op->addrlen,
                                                      &op->transport)) {
            continue;
        }
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "ptl:base:connect: %s unreachable - trying alternate %s",
                            (NULL == op->suri) ? "server" : op->suri, next);
        copy = strdup(next);
        if (NULL == copy) {
            cnct_finish(op, PMIX_ERR_NOMEM);
            return;
        }
        free(op->suri);
        op->suri = copy;
        op->connect_tries = 0;
        cnct_begin(-1, 0, op);
        return;
    }
    cnct_finish(op, PMIX_ERR_UNREACH);
}

static void cnct_connected(int sd, short args, void *cbdata)
{
    pmix_ptl_connect_op_t *op = (pmix_ptl_connect_op_t *) cbdata;
    pmix_status_t rc;
    int err = 0;
    pmix_socklen_t errlen = sizeof(err);
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_ACQUIRE_OBJECT(op);
    op->ev_active = false;

    if (0 != getsockopt(op->sd, SOL_SOCKET, SO_ERROR, (char *) &err, &errlen) || 0 != err) {
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "ptl:base:connect: connect failed: %s (%d)",
                            strerror(0 != err ? err : pmix_socket_errno),
                            0 != err ? err : pmix_socket_errno);
        CLOSE_THE_SOCKET(op->sd);
        op->sd = -1;
        cnct_begin(-1, 0, op);
        return;
    }

    /* connected - to a server we may talk to, by what the kernel says of
     * it - and then build the same connect-ack send_connect_ack sends */
    op->peer->sd = op->sd;
    op->peer->transport = op->transport;
    rc = pmix_ptl_base_check_server(op->peer, op->expect_given, op->expect_uid,
                                    op->expect_gid, op->vouched, &op->auth_local);
    if (PMIX_SUCCESS != rc) {
        cnct_finish(op, rc);
        return;
    }

    /* a server on another host proves who it is before we send anything
     * of ours - see pmix_ptl_base_auth_server, which does the same */
    if (op->prove || op->auth_local || pmix_ptl_base_server_remote(op->peer)) {
        op->auth_psec = pmix_ptl_base_auth_module(op->peer);
        if (NULL == op->auth_psec) {
            cnct_finish(op, (NULL == op->peer->nptr) ? PMIX_ERR_INIT : PMIX_ERR_NOT_SUPPORTED);
            return;
        }
        rc = pmix_util_random_bytes(op->auth_tool, sizeof(op->auth_tool));
        if (PMIX_SUCCESS == rc) {
            rc = pmix_ptl_base_auth_request(op->auth_psec->name, op->auth_tool, op->auth_local,
                                            &op->msg, &op->msglen);
        }
        if (PMIX_SUCCESS != rc) {
            cnct_finish(op, rc);
            return;
        }
        op->sent = 0;
        op->state = PMIX_CNCT_AUTH_SENDING;
        cnct_send(op->sd, EV_WRITE, op);
        return;
    }
    cnct_send_ack(op, NULL, (uid_t) -1);
}

/* build the connect-ack - its credential bound to `bind`, for the user
 * `to`, for a server on another host - and send it */
static void cnct_send_ack(pmix_ptl_connect_op_t *op, const pmix_byte_object_t *bind, uid_t to)
{
    size_t sdsize = 0;
    pmix_status_t rc;

    op->peer->proc_type.flag = pmix_ptl_base_set_flag(&sdsize);
    rc = construct_message(op->peer, &op->msg, &sdsize, op->iptr, op->niptr, bind, to);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        cnct_finish(op, rc);
        return;
    }
    op->msglen = sdsize;
    op->sent = 0;
    op->state = PMIX_CNCT_SENDING;
    cnct_send(op->sd, EV_WRITE, op);
}

static void cnct_send(int sd, short args, void *cbdata)
{
    pmix_ptl_connect_op_t *op = (pmix_ptl_connect_op_t *) cbdata;
    ssize_t n;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_ACQUIRE_OBJECT(op);
    op->ev_active = false;

    while (op->sent < op->msglen) {
        n = send(op->sd, op->msg + op->sent, op->msglen - op->sent, PMIX_PTL_NOSIGNAL);
        if (0 < n) {
            op->sent += (size_t) n;
            continue;
        }
        if (0 > n && EINTR == pmix_socket_errno) {
            continue;
        }
        if (0 > n && (EAGAIN == pmix_socket_errno || EWOULDBLOCK == pmix_socket_errno)) {
            if (PMIX_SUCCESS != cnct_wait(op, EV_WRITE, cnct_send)) {
                cnct_finish(op, PMIX_ERROR);
            }
            return;
        }
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "ptl:base:connect: send to the server failed: %s (%d)",
                            strerror(pmix_socket_errno), pmix_socket_errno);
        cnct_finish(op, PMIX_ERR_UNREACH);
        return;
    }
    free(op->msg);
    op->msg = NULL;

    /* every connector's answer starts with a status - the answer to our
     * opening, for a server on another host, or to our connect-ack */
    if (PMIX_SUCCESS != cnct_expect(op, (PMIX_CNCT_AUTH_SENDING == op->state)
                                            ? PMIX_CNCT_AUTH_STATUS : PMIX_CNCT_STATUS,
                                    sizeof(uint32_t))) {
        cnct_finish(op, PMIX_ERROR);
    }
}

/* A status a server sent. The codes the connect uses to say how it is
 * going - done, or under way - never come from a server, and one that does
 * is not a reply. A server asking for a security handshake is refused:
 * this library runs none */
pmix_status_t pmix_ptl_base_server_status(uint32_t u32)
{
    pmix_status_t reply = (pmix_status_t) u32;

    if (PMIX_ERR_READY_FOR_HANDSHAKE == reply) {
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "pmix: the server asked for a security handshake, which this "
                            "library does not run");
        return PMIX_ERR_NOT_SUPPORTED;
    }

    if (PMIX_OPERATION_SUCCEEDED == reply || PMIX_OPERATION_IN_PROGRESS == reply) {
        return PMIX_ERR_BAD_PARAM;
    }
    return reply;
}

/* The field just read is complete - act on it and say what comes next.
 * Returns PMIX_SUCCESS when there is more to read, PMIX_OPERATION_SUCCEEDED
 * when the handshake is done, or the failure. The order below is the one
 * pmix_ptl_base_client_handshake and pmix_ptl_base_tool_handshake read in. */
static pmix_status_t cnct_field(pmix_ptl_connect_op_t *op)
{
    pmix_status_t rc, reply;
    uint32_t u32 = 0;
    bool client;
    pmix_byte_object_t bind;
    uid_t srvuid = (uid_t) -1;

    if (sizeof(uint32_t) == op->fieldlen) {
        memcpy(&u32, op->field, sizeof(uint32_t));
        u32 = ntohl(u32);
    }
    client = (PMIX_PEER_IS_CLIENT(pmix_globals.mypeer) ||
              PMIX_PEER_IS_SINGLETON(pmix_globals.mypeer)) &&
             !PMIX_PEER_IS_TOOL(pmix_globals.mypeer);

    switch (op->state) {
    case PMIX_CNCT_AUTH_STATUS:
        reply = pmix_ptl_base_server_status(u32);
        if (PMIX_SUCCESS != reply) {
            pmix_ptl_base_auth_refused(op->peer, op->auth_psec->name, op->auth_local, reply);
            return reply;
        }
        return cnct_expect(op, PMIX_CNCT_AUTH_NONCE, PMIX_PTL_AUTH_NONCE_LEN);

    case PMIX_CNCT_AUTH_NONCE:
        memcpy(op->auth_srv, op->field, PMIX_PTL_AUTH_NONCE_LEN);
        return cnct_expect(op, PMIX_CNCT_AUTH_LEN, sizeof(uint32_t));

    case PMIX_CNCT_AUTH_LEN:
        if (0 == u32 || PMIX_MAX_CRED_SIZE < u32) {
            return PMIX_ERR_BAD_PARAM;
        }
        PMIX_BYTE_OBJECT_DESTRUCT(&op->cred);
        op->cred.bytes = (char *) malloc(u32);
        if (NULL == op->cred.bytes) {
            return PMIX_ERR_NOMEM;
        }
        op->cred.size = u32;
        op->state = PMIX_CNCT_AUTH_CRED;
        op->rbuf = op->cred.bytes;
        op->fieldlen = u32;
        op->got = 0;
        return cnct_wait(op, EV_READ, cnct_recv);

    case PMIX_CNCT_AUTH_CRED:
        rc = pmix_ptl_base_auth_reply(op->peer, op->expect_given, op->expect_uid,
                                      op->expect_gid, op->auth_local, op->auth_tool,
                                      op->auth_srv, &op->cred, &bind, &srvuid);
        PMIX_BYTE_OBJECT_DESTRUCT(&op->cred);
        if (PMIX_SUCCESS != rc) {
            return rc;
        }
        /* the server is who it should be - now our connect-ack. This may
         * finish the operation, so nothing here touches it afterwards */
        cnct_send_ack(op, &bind, srvuid);
        PMIX_BYTE_OBJECT_DESTRUCT(&bind);
        return PMIX_OPERATION_IN_PROGRESS;

    case PMIX_CNCT_STATUS:
        reply = pmix_ptl_base_server_status(u32);
        if (PMIX_ERR_PROVE_FIRST == reply && NULL == op->auth_psec) {
            /* it sees us as on another host - a port forward, say - so it
             * proves who it is first, on a connection of its own */
            op->prove = true;
            op->connect_tries = 0;
            cnct_restart(op);
            return PMIX_OPERATION_IN_PROGRESS;
        }
        if (PMIX_ERR_TEMP_UNAVAILABLE == reply &&
            ++op->handshake_tries < pmix_ptl_base.handshake_max_retries) {
            /* the server asked us to try again - make_connection does so
             * from the connect on, and so do we */
            op->connect_tries = 0;
            cnct_restart(op);
            return PMIX_OPERATION_IN_PROGRESS;
        }
        if (PMIX_SUCCESS != reply) {
            return reply;
        }
        if (client) {
            return cnct_expect(op, PMIX_CNCT_PINDEX, sizeof(uint32_t));
        }
        if (PMIX_TOOL_NEEDS_ID == op->peer->proc_type.flag ||
            PMIX_LAUNCHER_NEEDS_ID == op->peer->proc_type.flag) {
            return cnct_expect(op, PMIX_CNCT_MY_NSPACE, PMIX_MAX_NSLEN + 1);
        }
        return cnct_expect(op, PMIX_CNCT_SRV_NSPACE, PMIX_MAX_NSLEN + 1);

    case PMIX_CNCT_MY_NSPACE:
        op->field[PMIX_MAX_NSLEN] = '\0';
        PMIX_LOAD_NSPACE(pmix_globals.myid.nspace, op->field);
        return cnct_expect(op, PMIX_CNCT_MY_RANK, sizeof(uint32_t));

    case PMIX_CNCT_MY_RANK:
        pmix_globals.myid.rank = u32;
        return cnct_expect(op, PMIX_CNCT_SRV_NSPACE, PMIX_MAX_NSLEN + 1);

    case PMIX_CNCT_SRV_NSPACE:
        op->field[PMIX_MAX_NSLEN] = '\0';
        free(op->nspace);
        op->nspace = strdup(op->field);
        if (NULL == op->nspace) {
            return PMIX_ERR_NOMEM;
        }
        return cnct_expect(op, PMIX_CNCT_SRV_RANK, sizeof(uint32_t));

    case PMIX_CNCT_SRV_RANK:
        op->rank = u32;
        rc = set_server_id(op->peer, op->nspace, op->rank);
        if (PMIX_SUCCESS != rc) {
            return rc;
        }
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "pmix: RECV CONNECT CONFIRMATION FOR TOOL %s:%d FROM SERVER %s:%d",
                            pmix_globals.myid.nspace, pmix_globals.myid.rank,
                            op->peer->info->pname.nspace, op->peer->info->pname.rank);
        return cnct_expect(op, PMIX_CNCT_SEC_STATUS, sizeof(uint32_t));

    case PMIX_CNCT_SEC_STATUS:
        /* the outcome of our request */
        reply = pmix_ptl_base_server_status(u32);
        if (PMIX_SUCCESS != reply) {
            return reply;
        }
        return PMIX_OPERATION_SUCCEEDED;

    case PMIX_CNCT_PINDEX:
        pmix_globals.pindex = u32;
        return PMIX_OPERATION_SUCCEEDED;

    default:
        return PMIX_ERR_BAD_PARAM;
    }
}

static void cnct_recv(int sd, short args, void *cbdata)
{
    pmix_ptl_connect_op_t *op = (pmix_ptl_connect_op_t *) cbdata;
    pmix_status_t rc;
    ssize_t n;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_ACQUIRE_OBJECT(op);
    op->ev_active = false;

    while (op->got < op->fieldlen) {
        n = recv(op->sd, op->rbuf + op->got, op->fieldlen - op->got, 0);
        if (0 < n) {
            op->got += (size_t) n;
            continue;
        }
        if (0 == n) {
            pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                                "ptl:base:connect: server closed the connection");
            if (PMIX_CNCT_STATUS == op->state) {
                hint_closed_no_reply(op->peer, op->suri);
            } else if (PMIX_CNCT_AUTH_STATUS == op->state) {
                pmix_ptl_base_auth_refused(op->peer, op->auth_psec->name, op->auth_local,
                                           PMIX_ERR_UNREACH);
            }
            cnct_finish(op, PMIX_ERR_UNREACH);
            return;
        }
        if (EINTR == pmix_socket_errno) {
            continue;
        }
        if (EAGAIN == pmix_socket_errno || EWOULDBLOCK == pmix_socket_errno) {
            if (PMIX_SUCCESS != cnct_wait(op, EV_READ, cnct_recv)) {
                cnct_finish(op, PMIX_ERROR);
            }
            return;
        }
        if (PMIX_CNCT_AUTH_STATUS == op->state) {
            pmix_ptl_base_auth_refused(op->peer, op->auth_psec->name, op->auth_local,
                                       PMIX_ERR_UNREACH);
        }
        cnct_finish(op, PMIX_ERR_UNREACH);
        return;
    }

    rc = cnct_field(op);
    if (PMIX_SUCCESS == rc || PMIX_OPERATION_IN_PROGRESS == rc) {
        /* waiting on the next field, or started over */
        return;
    }
    if (PMIX_OPERATION_SUCCEEDED != rc) {
        cnct_finish(op, rc);
        return;
    }

    /* the handshake is complete - finish exactly as make_connection and
     * connect_to_peer do */
    op->peer->dyn_tags_start = PMIX_PTL_TAG_DYNAMIC;
    op->peer->dyn_tags_current = PMIX_PTL_TAG_DYNAMIC;
    op->peer->dyn_tags_end = PMIX_PTL_TAG_DYNAMIC + (UINT32_MAX - PMIX_PTL_TAG_DYNAMIC) / 2;
    rc = pmix_ptl_base_complete_connection(op->peer, op->nspace, op->rank);
    if (PMIX_SUCCESS != rc) {
        /* complete_connection has already closed the socket */
        op->sd = -1;
    }
    cnct_finish(op, rc);
}

pmix_status_t pmix_ptl_base_start_connection(pmix_peer_t *peer, char *nspace,
                                             pmix_rank_t rank, char *suri,
                                             char *alt_uris,
                                             pmix_info_t *iptr, size_t niptr,
                                             pmix_ptl_connect_nb_cbfunc_t cbfunc,
                                             void *cbdata)
{
    pmix_ptl_connect_op_t *op;
    pmix_status_t rc;
    struct timeval tv = {0, 0};

    rc = refuse_outdated(peer);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }

    op = PMIX_NEW(pmix_ptl_connect_op_t);
    if (NULL == op) {
        return PMIX_ERR_NOMEM;
    }
    /* the server's identity, if whoever started us named it - carried
     * with the connect, which completes after this returns */
    op->expect_given = pmix_ptl_base.server_expect_given;
    op->vouched = pmix_ptl_base.server_release_vouched;
    op->expect_uid = pmix_ptl_base.server_expect_uid;
    op->expect_gid = pmix_ptl_base.server_expect_gid;
    rc = pmix_ptl_base_uri_to_addr(suri, &op->addr, &op->addrlen, &op->transport);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(op);
        return rc;
    }
    /* a copy - the caller keeps its string whatever happens here */
    if (NULL != alt_uris) {
        op->alts = PMIx_Argv_split(alt_uris, ',');
    }
    op->peer = peer;
    op->cbfunc = cbfunc;
    op->cbdata = cbdata;
    pmix_list_append(&pmix_ptl_base.connecting, &op->super);

    if (0 < pmix_ptl_base.handshake_wait_time) {
        tv.tv_sec = pmix_ptl_base.handshake_wait_time;
        pmix_event_evtimer_set(pmix_globals.evbase, &op->timer, cnct_expired, op);
        if (0 == pmix_event_evtimer_add(&op->timer, &tv)) {
            op->timer_active = true;
        }
    }

    /* Begin on the next pass of the event loop rather than here: the
     * contract is that the callback never runs inside this call, and a
     * connect() refused outright would otherwise report it at once */
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    pmix_event_evtimer_set(pmix_globals.evbase, &op->ev, cnct_begin, op);
    if (0 != pmix_event_evtimer_add(&op->ev, &tv)) {
        pmix_list_remove_item(&pmix_ptl_base.connecting, &op->super);
        PMIX_RELEASE(op);
        return PMIX_ERROR;
    }
    op->ev_active = true;

    /* taken last, so a failure above leaves them with the caller */
    op->nspace = nspace;
    op->rank = rank;
    op->suri = suri;
    op->iptr = iptr;
    op->niptr = niptr;
    return PMIX_SUCCESS;
}

void pmix_ptl_base_abandon_connects(void)
{
    pmix_ptl_connect_op_t *op, *next;

    PMIX_LIST_FOREACH_SAFE (op, next, &pmix_ptl_base.connecting, pmix_ptl_connect_op_t) {
        cnct_finish(op, PMIX_ERR_NOT_AVAILABLE);
    }
}

pmix_rnd_flag_t pmix_ptl_base_set_flag(size_t *sz)
{
    pmix_rnd_flag_t flag;
    size_t sdsize = 0;

    /* Defined marker values:
     *
     */
    if (PMIX_PEER_IS_LAUNCHER(pmix_globals.mypeer)) {
        if (PMIX_PEER_IS_CLIENT(pmix_globals.mypeer)) {
            /* if we are both launcher and client, then we need
             * to tell the server we are both */
            flag = PMIX_LAUNCHER_CLIENT;
            /* add space for our uid/gid for ACL purposes */
            sdsize += 2 * sizeof(uint32_t);
            /* add space for our identifier */
            sdsize += strlen(pmix_globals.myid.nspace) + 1 + sizeof(uint32_t);
        } else {
            /* add space for our uid/gid for ACL purposes */
            sdsize += 2 * sizeof(uint32_t);
            /* if they gave us an identifier, we need to pass it */
            if (0 < strlen(pmix_globals.myid.nspace)
                && PMIX_RANK_INVALID != pmix_globals.myid.rank) {
                flag = PMIX_LAUNCHER_GIVEN_ID;
                sdsize += strlen(pmix_globals.myid.nspace) + 1 + sizeof(uint32_t);
            } else {
                flag = PMIX_LAUNCHER_NEEDS_ID;
            }
        }

    } else if (PMIX_PEER_IS_SCHEDULER(pmix_globals.mypeer)) {
        /* add space for our uid/gid for ACL purposes */
        sdsize += 2 * sizeof(uint32_t);
        flag = PMIX_SCHEDULER_WITH_ID;
        sdsize += strlen(pmix_globals.myid.nspace) + 1 + sizeof(uint32_t);

    } else if (PMIX_PEER_IS_CLIENT(pmix_globals.mypeer)
               && !PMIX_PEER_IS_TOOL(pmix_globals.mypeer)) {
        if (PMIX_PEER_IS_SINGLETON(pmix_globals.mypeer)) {
            flag = PMIX_SINGLETON_CLIENT;
            /* reserve space for our nspace and rank info */
            sdsize += strlen(pmix_globals.myid.nspace) + 1 + sizeof(uint32_t);
            /* add space for our uid/gid for ACL purposes */
            sdsize += 2 * sizeof(uint32_t);
        } else {
            /* we are a simple client */
            flag = PMIX_SIMPLE_CLIENT;
            /* reserve space for our nspace and rank info */
            sdsize += strlen(pmix_globals.myid.nspace) + 1 + sizeof(uint32_t);
        }
    } else { // must be a tool of some sort
        /* add space for our uid/gid for ACL purposes */
        sdsize += 2 * sizeof(uint32_t);
        if (PMIX_PEER_IS_CLIENT(pmix_globals.mypeer)) {
            /* if we are both tool and client, then we need
             * to tell the server we are both */
            flag = PMIX_TOOL_CLIENT;
            /* add space for our identifier */
            sdsize += strlen(pmix_globals.myid.nspace) + 1 + sizeof(uint32_t);
        } else if (0 < strlen(pmix_globals.myid.nspace)
                   && PMIX_RANK_INVALID != pmix_globals.myid.rank) {
            /* we were given an identifier by the caller, pass it */
            sdsize += strlen(pmix_globals.myid.nspace) + 1 + sizeof(uint32_t);
            flag = PMIX_TOOL_GIVEN_ID;
        } else {
            /* we are a self-started tool that needs an identifier */
            flag = PMIX_TOOL_NEEDS_ID;
        }
    }

    *sz += sdsize;
    return flag;
}

static pmix_status_t construct_message(pmix_peer_t *peer, char **msgout, size_t *sz,
                                       pmix_info_t *iptr, size_t niptr,
                                       const pmix_byte_object_t *bind, uid_t to)
{
    char *msg;
    char *sec, *bfrops, *gds;
    pmix_bfrop_buffer_type_t bftype;
    uid_t euid;
    gid_t egid;
    pmix_buffer_t buf;
    pmix_status_t rc;
    pmix_ptl_hdr_t hdr;
    size_t sdsize, csize;
    pmix_byte_object_t cred;
    pmix_psec_module_t *psec;
    pmix_bfrops_module_t *bfmod;

    sdsize = *sz;

    /* every connect path gives the server's peer a namespace, and the gds
     * module we will use with it, before it gets here - the message names
     * that module, so there is nothing to send without one */
    if (NULL == peer->nptr || NULL == peer->nptr->compat.gds) {
        return PMIX_ERR_INIT;
    }

    /* setup the header */
    memset(&hdr, 0, sizeof(pmix_ptl_hdr_t));
    hdr.pindex = -1;
    hdr.tag = UINT32_MAX;

    /* Speak to this server with the security mechanism and wire format
     * chosen for it - from what its file said it accepts, when we found
     * it through one (pmix_ptl_base_select_compat). For our primary
     * server those are our own; a tool attaching to another server may
     * need older ones for it alone */
    psec = (NULL != peer->nptr->compat.psec)
               ? peer->nptr->compat.psec : pmix_globals.mypeer->nptr->compat.psec;
    bfmod = (NULL != peer->nptr->compat.bfrops)
                ? peer->nptr->compat.bfrops : pmix_globals.mypeer->nptr->compat.bfrops;

    /* add the name of our active sec module */
    sec = psec->name;
    sdsize += strlen(sec) + 1;

    /* a security module was assigned to us during rte_init based
     * on a list of available security modules provided by our
     * local PMIx server, if known. Now use that module to
     * get a credential. For a server on another host, which has already
     * proved who it is, the credential is made for this one connection */
    PMIX_BYTE_OBJECT_CONSTRUCT(&cred);
    if (NULL != bind) {
        if (NULL == psec->create_bound) {
            return PMIX_ERR_NOT_SUPPORTED;
        }
        rc = psec->create_bound(bind, false, to, &cred);
    } else {
        rc = psec->create_cred((struct pmix_peer_t *) pmix_globals.mypeer, NULL, 0, NULL, NULL,
                               &cred);
    }
    if (PMIX_SUCCESS != rc) {
        PMIX_BYTE_OBJECT_DESTRUCT(&cred);
        return rc;
    }
    sdsize += sizeof(uint32_t); // need to pass the number of bytes
    sdsize += cred.size;        // account for the payload itself

    /* add our type flag */
    sdsize += 1;

    /* add our version string */
    sdsize += strlen(PMIX_VERSION) + 1;

    /* add our active bfrops module name */
    bfrops = bfmod->version;
    sdsize += strlen(bfrops) + 1;
    /* and the type of buffer we are using */
    bftype = pmix_globals.mypeer->nptr->compat.type;
    sdsize += sizeof(bftype);

    /* add our active gds module for working with the server */
    gds = (char *) peer->nptr->compat.gds->name;
    sdsize += strlen(gds) + 1;

    /* if we were given info structs to pass to the server, pack them */
    if (NULL != iptr) {
        PMIX_CONSTRUCT(&buf, pmix_buffer_t);
        /* in the wire format we name, which the server unpacks it with */
        buf.type = bftype;
        rc = bfmod->pack(&buf, &niptr, 1, PMIX_SIZE);
        if (PMIX_SUCCESS == rc) {
            rc = bfmod->pack(&buf, iptr, niptr, PMIX_INFO);
        }
        if (PMIX_SUCCESS != rc) {
            /* we cannot send a partial blob - the far end computes the
             * length of the info section from what is left in the
             * message, so a short one would be read as garbage */
            PMIX_ERROR_LOG(rc);
            PMIX_BYTE_OBJECT_DESTRUCT(&cred);
            PMIX_DESTRUCT(&buf);
            return rc;
        }
        sdsize += buf.bytes_used;
    }

    /* set the number of bytes to be read beyond the header */
    hdr.nbytes = sdsize;

    /* create a space for our message */
    sdsize = sizeof(hdr) + hdr.nbytes;
    if (NULL == (msg = (char *) malloc(sdsize))) {
        /* "sec", "bfrops" and "gds" all point into the compat modules
         * we were assigned - they are not ours to free */
        PMIX_BYTE_OBJECT_DESTRUCT(&cred);
        if (NULL != iptr) {
            PMIX_DESTRUCT(&buf);
        }
        return PMIX_ERR_OUT_OF_RESOURCE;
    }
    memset(msg, 0, sdsize);

    /* load the header */
    csize = 0;
    memcpy(msg, &hdr, sizeof(pmix_ptl_hdr_t));
    csize += sizeof(pmix_ptl_hdr_t);

    /* provide our active psec module */
    PMIX_PTL_PUT_STRING(sec);

    /* load the length of the credential */
    PMIX_PTL_PUT_U32(cred.size);

    /* load the credential */
    PMIX_PTL_PUT_BLOB(cred.bytes, cred.size);
    PMIX_BYTE_OBJECT_DESTRUCT(&cred);

    /* load our process type - this is a single byte,
     * so no worry about heterogeneity here */
    PMIX_PTL_PUT_U8(peer->proc_type.flag);

    switch (peer->proc_type.flag) {
    case PMIX_SIMPLE_CLIENT:
        /* simple client process */
        PMIX_PTL_PUT_PROCID(pmix_globals.myid);
        break;

        /* we cannot have cases 1 or 2 because those are only
         * for legacy processes */

    case PMIX_TOOL_NEEDS_ID:
    case PMIX_LAUNCHER_NEEDS_ID:
        /* self-started tool/launcher process that needs an identifier */
        euid = geteuid();
        PMIX_PTL_PUT_U32(euid);
        egid = getegid();
        PMIX_PTL_PUT_U32(egid);
        break;

    case PMIX_TOOL_GIVEN_ID:
    case PMIX_LAUNCHER_GIVEN_ID:
    case PMIX_SCHEDULER_WITH_ID:
    case PMIX_SINGLETON_CLIENT:
        /* self-started tool/launcher/singleton process that was given an identifier by caller */
        euid = geteuid();
        PMIX_PTL_PUT_U32(euid);
        egid = getegid();
        PMIX_PTL_PUT_U32(egid);
        /* add our identifier */
        PMIX_PTL_PUT_PROCID(pmix_globals.myid);
        break;

    case PMIX_TOOL_CLIENT:
    case PMIX_LAUNCHER_CLIENT:
        /* tool/launcher that was started by a PMIx server - identifier specified by server */
        euid = geteuid();
        PMIX_PTL_PUT_U32(euid);
        egid = getegid();
        PMIX_PTL_PUT_U32(egid);
        /* add our identifier */
        PMIX_PTL_PUT_PROCID(pmix_globals.myid);
        break;

    default:
        /* we don't know what they are! */
        if (NULL != iptr) {
            PMIX_DESTRUCT(&buf);
        }
        free(msg);
        return PMIX_ERR_NOT_SUPPORTED;
    }

    /* provide our version */
    PMIX_PTL_PUT_STRING(PMIX_VERSION);

    /* provide our active bfrops module */
    PMIX_PTL_PUT_STRING(bfrops);

    /* provide the bfrops type */
    PMIX_PTL_PUT_U8(bftype);

    /* provide the gds module */
    PMIX_PTL_PUT_STRING(gds);

    /* provide the info struct bytes */
    if (NULL != iptr) {
        PMIX_PTL_PUT_BLOB(buf.base_ptr, buf.bytes_used);
        PMIX_DESTRUCT(&buf);
    }

    *msgout = msg;
    *sz = sdsize;
    return PMIX_SUCCESS;
}

pmix_status_t pmix_ptl_base_set_timeout(pmix_peer_t *peer, struct timeval *save,
                                        pmix_socklen_t *sz, bool *sockopt)
{
    struct timeval tv;

    /* get the current timeout value so we can reset to it */
    if (0 != getsockopt(peer->sd, SOL_SOCKET, SO_RCVTIMEO, (void *) save, sz)) {
        *sockopt = false;
    } else {
        /* set a timeout on the blocking recv so we don't hang */
        tv.tv_sec = pmix_ptl_base.handshake_wait_time;
        tv.tv_usec = 0;
        if (0 != setsockopt(peer->sd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv))) {
            *sockopt = false;
        }
    }

    return PMIX_SUCCESS;
}

pmix_status_t pmix_ptl_base_client_handshake(pmix_peer_t *peer, pmix_status_t reply)
{
    if (PMIX_SUCCESS != reply) {
        return reply;
    }
    pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                        "pmix: RECV CONNECT CONFIRMATION");

    /* receive our index into the peer's client array */
    PMIX_PTL_RECV_U32(peer->sd, pmix_globals.pindex);
    return PMIX_SUCCESS;
}

/* the "peer" object passed into this function is that of the SERVER
 * to which the tool is connecting - it is NOT the peer of the tool itself*/
pmix_status_t pmix_ptl_base_tool_handshake(pmix_peer_t *peer, pmix_status_t rp)
{
    pmix_nspace_t nspace;
    pmix_rank_t rank;
    pmix_status_t reply, rc;

    /* if the status indicates an error, then we are done */
    if (PMIX_SUCCESS != rp) {
        return rp;
    }

    /* if we need an identifier, it comes next */
    if (PMIX_TOOL_NEEDS_ID == peer->proc_type.flag ||
        PMIX_LAUNCHER_NEEDS_ID == peer->proc_type.flag) {
        PMIX_PTL_RECV_NSPACE(peer->sd, pmix_globals.myid.nspace);
        PMIX_PTL_RECV_U32(peer->sd, pmix_globals.myid.rank);
    }

    /* get the server's nspace and rank so we can send to it */
    PMIX_PTL_RECV_NSPACE(peer->sd, nspace);
    PMIX_PTL_RECV_U32(peer->sd, rank);
    rc = set_server_id(peer, nspace, rank);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }

    pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                        "pmix: RECV CONNECT CONFIRMATION FOR TOOL %s:%d FROM SERVER %s:%d",
                        pmix_globals.myid.nspace, pmix_globals.myid.rank, peer->info->pname.nspace,
                        peer->info->pname.rank);

    /* get the outcome of our request */
    PMIX_PTL_RECV_U32(peer->sd, reply);
    reply = pmix_ptl_base_server_status((uint32_t) reply);
    if (PMIX_SUCCESS != reply) {
        return reply;
    }

    return PMIX_SUCCESS;
}

/* Append one attribute to a list under construction. Returns false if it
 * could not be allocated or loaded - nothing is appended in that case */
static bool add_info(pmix_list_t *list, const char *key, const void *val,
                     pmix_data_type_t type)
{
    pmix_infolist_t *ip;

    ip = PMIX_NEW(pmix_infolist_t);
    if (NULL == ip) {
        return false;
    }
    if (PMIX_SUCCESS != PMIx_Info_load(&ip->info, key, val, type)) {
        PMIX_RELEASE(ip);
        return false;
    }
    pmix_list_append(list, &ip->super);
    return true;
}

static void check_server(char *filename, pmix_list_t *servers)
{
    FILE *fp;
    char *srvr, *p, *p2;
    struct timeval tv;
    int retries;
    pmix_info_t *sdata;
    size_t ndata, n;
    pmix_infolist_t *iptr, *ians;
    char *nspace = NULL;
    pmix_rank_t rank;
    pmix_list_t mylist;
    uint32_t u32;
    pmix_status_t rc;
    bool ok, gone;
    char *lock = NULL;
    struct stat st;
    size_t budget = PMIX_PTL_MAX_CONN_FILE;

    /* this file was just listed by the directory walk, so it cannot be
     * a server that is "still starting up" - if it is gone or unreadable
     * now, its server went away in between or it is not ours to read.
     * Waiting for it here would only stall the progress thread, which is
     * where a PMIX_QUERY_AVAIL_SERVERS walk runs */
    /* coverity[TOCTOU] */
    if (0 != access(filename, R_OK)) {
        return;
    }

    fp = open_conn_file(filename, true);
    if (NULL == fp) {
        return;
    }
    /* get the URI - might seem crazy, but there is actually
     * a race condition here where the server may have created
     * the file but not yet finished writing into it. So give
     * us a chance to get the required info */
    for (retries = 0; retries < 3; retries++) {
        srvr = pmix_ptl_base_getline(fp, &budget);
        if (NULL != srvr) {
            break;
        }
        fclose(fp);
        tv.tv_sec = 0;
        tv.tv_usec = 10000; // use 0.01 sec as default
        retry_wait(&tv);
        fp = open_conn_file(filename, true);
        if (NULL == fp) {
            return;
        }
    }
    /* an empty or malformed "pmix.*" file is to be expected in a
     * directory anyone can write to - not an error worth reporting */
    if (NULL == srvr) {
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "pmix:tcp: %s is empty", filename);
        fclose(fp);
        return;
    }
    rc = pmix_ptl_base_parse_uri(srvr, &nspace, &rank, NULL);
    if (PMIX_SUCCESS != rc) {
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "pmix:tcp: %s holds no server URI", filename);
        fclose(fp);
        free(srvr);
        return;
    }

    /* see if we already have this server in our list */
    PMIX_LIST_FOREACH (iptr, servers, pmix_infolist_t) {
        /* each item contains an array starting with the server nspace */
        sdata = (pmix_info_t *) iptr->info.value.data.darray->array;
        if (0 == strcmp(sdata[0].value.data.string, nspace) && sdata[1].value.data.rank == rank) {
            /* already have this one */
            fclose(fp);
            free(srvr);
            free(nspace);
            return;
        }
    }

    /* begin collecting data for the new entry */
    PMIX_CONSTRUCT(&mylist, pmix_list_t);
    ok = add_info(&mylist, PMIX_SERVER_NSPACE, nspace, PMIX_STRING) &&
         add_info(&mylist, PMIX_SERVER_RANK, &rank, PMIX_PROC_RANK);
    free(srvr);
    free(nspace);
    if (!ok) {
        goto nomem;
    }

    /* see if this file contains the server's version - every server
     * since v2.1 writes one, so a file without it came from a v2.0
     * server, which the connection will then refuse */
    p2 = pmix_ptl_base_getline(fp, &budget);
    if (NULL == p2) {
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output, "V20 SERVER DETECTED");
        ok = add_info(&mylist, PMIX_VERSION_INFO, "v2.0", PMIX_STRING);
    } else {
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "VERSION %s SERVER DETECTED", p2);
        ok = add_info(&mylist, PMIX_VERSION_INFO, p2, PMIX_STRING);
        free(p2);
    }
    if (!ok) {
        goto nomem;
    }

    /* see if the file contains the pid - and, below, leave out a server
     * that is gone */
    p2 = pmix_ptl_base_getline(fp, &budget);
    if (NULL == p2) {
        goto complete;
    }
    u32 = strtoul(p2, NULL, 10);
    free(p2);
    if (!add_info(&mylist, PMIX_SERVER_PIDINFO, &u32, PMIX_UINT32)) {
        goto nomem;
    }

    /* check for uid:gid */
    p2 = pmix_ptl_base_getline(fp, &budget);
    if (NULL == p2) {
        goto complete;
    }
    /* find the colon */
    if (NULL == (p = strchr(p2, ':'))) {
        /* bad format */
        free(p2);
        goto complete;
    }
    *p = '\0';
    ++p;
    u32 = strtoul(p2, NULL, 10);
    ok = add_info(&mylist, PMIX_USERID, &u32, PMIX_UINT32);
    if (ok) {
        u32 = strtoul(p, NULL, 10);
        ok = add_info(&mylist, PMIX_GRPID, &u32, PMIX_UINT32);
    }
    free(p2);
    if (!ok) {
        goto nomem;
    }

    /* check for timestamp */
    p2 = pmix_ptl_base_getline(fp, &budget);
    if (NULL == p2) {
        goto complete;
    }
    ok = add_info(&mylist, PMIX_SERVER_START_TIME, p2, PMIX_STRING);
    free(p2);
    if (!ok) {
        goto nomem;
    }

complete:
    /* whether its server holds it locked is on a tagged line, after the
     * timestamp */
    for (n = 0; n < PMIX_PTL_MAX_TAG_LINES && NULL != (p2 = pmix_ptl_base_getline(fp, &budget)); n++) {
        if (NULL == lock && 0 == strncmp(p2, PMIX_PTL_LOCK_TAG, strlen(PMIX_PTL_LOCK_TAG))) {
            lock = strdup(p2 + strlen(PMIX_PTL_LOCK_TAG));
        }
        free(p2);
    }
    gone = (0 == fstat(fileno(fp), &st) &&
            pmix_ptl_base_server_gone(AT_FDCWD, filename, st.st_uid, lock));
    free(lock);
    fclose(fp);
    if (gone) {
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "pmix:tcp: %s was written by a server that is gone", filename);
        PMIX_LIST_DESTRUCT(&mylist);
        return;
    }

    /* convert the list to an array */
    ndata = pmix_list_get_size(&mylist);
    ians = PMIX_NEW(pmix_infolist_t);
    if (NULL == ians) {
        PMIX_LIST_DESTRUCT(&mylist);
        return;
    }
    PMIX_LOAD_KEY(ians->info.key, PMIX_SERVER_INFO_ARRAY);
    ians->info.value.type = PMIX_DATA_ARRAY;
    PMIX_DATA_ARRAY_CREATE(ians->info.value.data.darray, ndata, PMIX_INFO);
    if (NULL == ians->info.value.data.darray) {
        ians->info.value.type = PMIX_UNDEF;
        PMIX_RELEASE(ians);
        PMIX_LIST_DESTRUCT(&mylist);
        return;
    }
    sdata = (pmix_info_t *) ians->info.value.data.darray->array;
    n = 0;
    PMIX_LIST_FOREACH (iptr, &mylist, pmix_infolist_t) {
        PMIX_INFO_XFER(&sdata[n], &iptr->info);
        ++n;
    }
    PMIX_LIST_DESTRUCT(&mylist);
    pmix_list_append(servers, &ians->super);
    return;

nomem:
    /* this server is left out of the answer - the rest of the directory
     * walk still reports whatever else it finds */
    PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
    fclose(fp);
    PMIX_LIST_DESTRUCT(&mylist);
}

/* depth: as for df_search() above */
static void query_servers(char *dirname, pmix_list_t *servers, int depth)
{
    char *newdir, *dname;
    DIR *cur_dirp;
    struct dirent *dir_entry;
    bool isdir;

    /* search the system tmpdir directory tree for files
     * beginning with "pmix." as these can be potential
     * servers */

    if (NULL == dirname) {
        /* we first check the system tmpdir to see if a system-level
         * server is present */
        dname = pmix_ptl_base.system_tmpdir;
    } else {
        dname = dirname;
    }
    cur_dirp = opendir(dname);
    if (NULL == cur_dirp) {
        return;
    }

    pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                        "pmix:tcp: searching directory %s",
                        (NULL == dirname) ? pmix_ptl_base.system_tmpdir : dirname);

    /* search the entries for something that starts with the "pmix." prefix */
    while (NULL != (dir_entry = readdir(cur_dirp))) {
        /* ignore the . and .. entries */
        if (0 == strcmp(dir_entry->d_name, ".") || 0 == strcmp(dir_entry->d_name, "..")) {
            continue;
        }
        newdir = pmix_os_path(false, dname, dir_entry->d_name, NULL);
        if (NULL == newdir) {
            /* as in pmix_ptl_base_df_search() above */
            continue;
        }
        /* see search_candidate() for why links to directories are
         * not followed and only regular files are read */
        if (!search_candidate(newdir, &isdir)) {
            free(newdir);
            continue;
        }
        /* if it is a directory, down search - as far as we may */
        if (isdir) {
            if (0 < depth) {
                query_servers(newdir, servers, depth - 1);
            }
            free(newdir);
            continue;
        }
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "pmix:tcp: checking %s",
                            dir_entry->d_name);
        /* see if it starts with our prefix */
        if (0 == strncmp(dir_entry->d_name, "pmix.", strlen("pmix."))) {
            /* try to read this file */
            pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                                "pmix:tcp: reading file %s", newdir);
            check_server(newdir, servers);
        }
        free(newdir);
    }
    closedir(cur_dirp);
}

static void _local_relcb(void *cbdata)
{
    pmix_query_caddy_t *cd = (pmix_query_caddy_t *) cbdata;

    if (NULL != cd->info) {
        PMIX_INFO_FREE(cd->info, cd->ninfo);
    }
    PMIX_RELEASE(cd);
}

void pmix_ptl_base_query_servers(int sd, short args, void *cbdata)
{
    pmix_query_caddy_t *cd = (pmix_query_caddy_t *) cbdata;
    pmix_list_t servers;
    size_t n;
    pmix_infolist_t *iptr;
    pmix_status_t rc;

    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_CONSTRUCT(&servers, pmix_list_t);

    query_servers(NULL, &servers, pmix_ptl_base.search_depth);

    /* convert the list to an array of pmix_info_t */
    cd->ninfo = pmix_list_get_size(&servers);
    if (0 == cd->ninfo) {
        rc = PMIX_ERR_NOT_FOUND;
    } else {
        PMIX_INFO_CREATE(cd->info, cd->ninfo);
        if (NULL == cd->info) {
            cd->ninfo = 0;
            rc = PMIX_ERR_NOMEM;
        } else {
            n = 0;
            PMIX_LIST_FOREACH (iptr, &servers, pmix_infolist_t) {
                PMIX_INFO_XFER(&cd->info[n], &iptr->info);
                ++n;
            }
            rc = PMIX_SUCCESS;
        }
    }
    PMIX_LIST_DESTRUCT(&servers);

    /* execute the callback function. PMIx_Query_info_nb accepts a NULL
     * callback - every other completion path in pmix_query.c checks for
     * one - so there may be nobody to hand the answer to, in which case
     * we still have to release what we built */
    if (NULL != cd->cbfunc) {
        cd->cbfunc(rc, cd->info, cd->ninfo, cd->cbdata, _local_relcb, cd);
    } else {
        _local_relcb(cd);
    }
}

static void timeout(int sd, short args, void *cbdata)
{
    pmix_lock_t *lock = (pmix_lock_t *) cbdata;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_WAKEUP_THREAD(lock);
}

/* Pause before looking at a connection file again.
 *
 * The pause is normally an evtimer on pmix_globals.evbase with the caller
 * parked on a lock the timer releases, so the waiting thread does not
 * spin. That only works when the caller is some OTHER thread: the timer
 * can fire only when the progress thread gets back to its event loop, and
 * if the caller IS the progress thread, it never will - the wait is
 * forever. These file loops are reached on the progress thread whenever
 * pmix_ptl_base_connect_to_peer is, which includes every
 * PMIx_tool_attach_to_server (pmix_tool_retry_attach is a thread-shift
 * handler): an attachment file that did not exist yet deadlocked the
 * tool's progress thread, and with it the call waiting on the other side.
 *
 * On the progress thread, sleep for the interval instead. That blocks the
 * loop for the length of the pause - but that path already blocks it for
 * the whole TCP handshake that follows, and a bounded pause the caller
 * asked for is what the loop is there to provide. Off the progress thread
 * the timer is used as before, unless it cannot be added - then nothing
 * would ever release the lock, so that case sleeps too. */
static void retry_wait(const struct timeval *tv)
{
    pmix_lock_t lock;
    pmix_event_t ev;
    struct timeval tvc = *tv;
    struct timespec req, rem;

    if (!pmix_progress_thread_is_current()) {
        PMIX_CONSTRUCT_LOCK(&lock);
        pmix_event_evtimer_set(pmix_globals.evbase, &ev, timeout, &lock);
        PMIX_POST_OBJECT(&ev);
        if (0 == pmix_event_evtimer_add(&ev, &tvc)) {
            PMIX_WAIT_THREAD(&lock);
            PMIX_DESTRUCT_LOCK(&lock);
            return;
        }
        /* no timer will ever release the lock - sleep instead */
        PMIX_DESTRUCT_LOCK(&lock);
    }

    req.tv_sec = tv->tv_sec;
    req.tv_nsec = (long) tv->tv_usec * 1000L;
    while (0 != nanosleep(&req, &rem) && EINTR == errno) {
        req = rem;
    }
}

char *pmix_ptl_base_peer_type(pmix_peer_t *peer)
{
    char **types = NULL;
    char *ans;

    if (PMIX_PEER_IS_CLIENT(peer)) {
        PMIx_Argv_append_nosize(&types, "CLIENT");
    }
    if (PMIX_PEER_IS_SINGLETON(peer)) {
        PMIx_Argv_append_nosize(&types, "SINGLETON");
    }
    if (PMIX_PEER_IS_SERVER(peer)) {
        PMIx_Argv_append_nosize(&types, "SERVER");
    }
    if (PMIX_PEER_IS_TOOL(peer)) {
        PMIx_Argv_append_nosize(&types, "TOOL");
    }
    if (PMIX_PEER_IS_LAUNCHER(peer)) {
        PMIx_Argv_append_nosize(&types, "LAUNCHER");
    }
    if (PMIX_PEER_IS_GATEWAY(peer)) {
        PMIx_Argv_append_nosize(&types, "GATEWAY");
    }
    if (PMIX_PEER_IS_SCHEDULER(peer)) {
        PMIx_Argv_append_nosize(&types, "SCHEDULER");
    }
    if (PMIX_PEER_IS_SYS_CTRLR(peer)) {
        PMIx_Argv_append_nosize(&types, "SYSCTRLR");
    }
    ans = PMIx_Argv_join(types, ',');
    PMIx_Argv_free(types);
    return ans;
}
