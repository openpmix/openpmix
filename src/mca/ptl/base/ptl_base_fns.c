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
#include <limits.h>
#include <signal.h>
#include <time.h>

#include "src/include/pmix_globals.h"
#include "src/include/pmix_socket_errno.h"
#include "src/runtime/pmix_progress_threads.h"
#include "src/client/pmix_client_ops.h"
#include "src/common/pmix_idenv.h"
#include "src/mca/bfrops/base/base.h"
#include "src/mca/gds/base/base.h"
#include "src/mca/psec/base/base.h"
#include "src/util/pmix_getid.h"
#include "src/util/pmix_idname.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_fd.h"
#include "src/util/pmix_name_fns.h"
#include "src/util/pmix_net.h"
#include "src/util/pmix_os_path.h"
#include "src/util/pmix_if.h"
#include "src/util/pmix_printf.h"
#include "src/util/pmix_show_help.h"
#include "src/util/pmix_string_copy.h"
#include "src/util/pmix_strnlen.h"

#include "src/mca/ptl/base/base.h"
#include "src/mca/ptl/base/ptl_base_handshake.h"

/* the credential's bytes are zeroed before they are freed */
static void wipe_cred(pmix_byte_object_t *cred)
{
    if (NULL != cred->bytes) {
        pmix_ptl_base_wipe(cred->bytes, cred->size);
    }
    PMIX_BYTE_OBJECT_DESTRUCT(cred);
}

/****    SUPPORTING FUNCTIONS    ****/
static void timeout(int sd, short args, void *cbdata);
static void retry_wait(const struct timeval *tv);
static pmix_status_t build_offer(pmix_peer_t *peer, char **msgout, size_t *sz);
static pmix_status_t apply_choice(pmix_peer_t *peer, char *msg, size_t len);
static pmix_status_t build_credential(pmix_peer_t *peer, pmix_info_t *iptr, size_t niptr,
                                      char **msgout, size_t *sz);
static pmix_status_t construct_message(pmix_peer_t *peer, char **msgout, size_t *sz,
                                       pmix_info_t *iptr, size_t niptr);
static pmix_status_t parse_conn_file(char *filename, bool optional, bool found_by_search,
                                     bool regular_only, pmix_list_t *connections);
static bool search_candidate(const char *path, bool *isdir);
static bool local_addr(const struct sockaddr_storage *addr);

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
        /* from a directive, not the environment: the PMIX_VERSION there
         * is the release of the server that started us, which need not be
         * this one. The variable's name gives this one's */
        vrs = NULL;
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
        /* the server that started us named itself in our environment, as
         * it does for a client: it is the one named for us */
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

/* Read one component of a version string at *pp: decimal digits, as a
 * value no larger than PMIX_MAJOR_WILDCARD - 1. Leaves *pp at the first
 * character after the digits. False when there are no digits or the value
 * is larger - the wildcard is never a component a peer can declare */
static bool version_component(const char **pp, uint8_t *out)
{
    const char *p = *pp;
    unsigned long val = 0;

    if (!isdigit((unsigned char) *p)) {
        return false;
    }
    while (isdigit((unsigned char) *p)) {
        if (val < PMIX_MAJOR_WILDCARD) {
            val = val * 10 + (unsigned long) (*p - '0');
        }
        ++p;
    }
    if (PMIX_MAJOR_WILDCARD <= val) {
        return false;
    }
    *out = (uint8_t) val;
    *pp = p;
    return true;
}

/* Split a "major.minor.release" version string into its components.
 *
 * The strings handed to this come off the wire, so they need not carry
 * all three components. A component the string does not carry is zero.
 * Components are separated by '.', and anything else after one ends the
 * version - a release-candidate suffix such as "7.0.0a1". Each component
 * the string does carry must be a number from 0 to 254: a string whose
 * first component, or one following a '.', is not a number, or one
 * with a component larger than 254, is PMIX_ERR_BAD_PARAM, and every
 * component is then reported as zero. */
pmix_status_t pmix_ptl_base_parse_version(const char *vers, uint8_t *major,
                                          uint8_t *minor, uint8_t *release)
{
    uint8_t *parts[3] = {major, minor, release};
    const char *p = vers;
    int n;

    *major = 0;
    *minor = 0;
    *release = 0;
    if (NULL == vers) {
        return PMIX_ERR_BAD_PARAM;
    }
    for (n = 0; n < 3; n++) {
        if (!version_component(&p, parts[n])) {
            *major = 0;
            *minor = 0;
            *release = 0;
            return PMIX_ERR_BAD_PARAM;
        }
        /* a separator with nothing after it ends the string */
        if ('.' != p[0] || '\0' == p[1]) {
            break;
        }
        ++p;
    }
    return PMIX_SUCCESS;
}

#ifndef O_NOCTTY
#    define O_NOCTTY 0
#endif

FILE *pmix_ptl_base_open_regular(const char *filename, off_t max)
{
    int fd, flags, save;
    struct stat st;
    FILE *fp;

    if (NULL == filename) {
        errno = EINVAL;
        return NULL;
    }
    /* O_NONBLOCK: a FIFO or a device at the name does not hold the open.
     * O_NOFOLLOW: nor is a symbolic link at the name followed */
    fd = open(filename, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_NOCTTY | PMIX_O_CLOEXEC);
    if (0 > fd) {
        return NULL;
    }
    if (0 == PMIX_O_CLOEXEC) {
        (void) pmix_fd_set_cloexec(fd);
    }
    /* the descriptor is what is checked - the file actually opened */
    if (0 != fstat(fd, &st) || !S_ISREG(st.st_mode)) {
        close(fd);
        errno = EINVAL;
        return NULL;
    }
    if (0 < max && max < st.st_size) {
        close(fd);
        errno = EFBIG;
        return NULL;
    }
    /* a regular file never blocks a read, but stdio is given the
     * descriptor as an ordinary one */
    flags = fcntl(fd, F_GETFL);
    if (0 <= flags) {
        (void) fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
    }
    fp = fdopen(fd, "r");
    if (NULL == fp) {
        save = errno;
        close(fd);
        errno = save;
    }
    return fp;
}

/* Open a connection file for reading - a regular file at the name, by
 * pmix_ptl_base_open_regular. A file found by walking a directory, or at
 * one of the fixed rendezvous names, is also kept only if it is no larger
 * than any server writes; one the caller named is read only as far as
 * that (pmix_ptl_base_getline's budget). */
static FILE *open_conn_file(const char *filename, bool regular_only)
{
    return pmix_ptl_base_open_regular(filename, regular_only ? PMIX_PTL_MAX_CONN_FILE : 0);
}

/* What the user is told of a connection file that could not be opened */
static const char *open_failure(int err)
{
    if (EACCES == err) {
        return "access denied";
    }
    if (ENOENT == err) {
        return "could not be found";
    }
    return "is not a regular file (a symbolic link, FIFO, device or directory is not read)";
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
    bool created;

    if (NULL == (lname = pmix_ptl_base_lock_name(name))) {
        return -1;
    }
    for (tries = 0; tries < 2; tries++) {
        /* knowing whether we made it, so as not to leave one behind that
         * we could not lock */
        created = true;
        fd = openat(dirfd, lname,
                    O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_NONBLOCK | PTL_O_CLOEXEC,
                    S_IRUSR | S_IWUSR);
        if (0 > fd && EEXIST == errno) {
            created = false;
            fd = openat(dirfd, lname, O_RDWR | O_NOFOLLOW | O_NONBLOCK | PTL_O_CLOEXEC);
            if (0 > fd && ENOENT == errno) {
                /* gone between the two: make it after all */
                continue;
            }
        }
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
        do {
            rc = flock(fd, LOCK_EX | LOCK_NB);
        } while (0 != rc && EINTR == errno);
        while (0 != rc && EWOULDBLOCK == errno && busy < 20) {
            ++busy;
            (void) nanosleep(&wait, NULL);
            do {
                rc = flock(fd, LOCK_EX | LOCK_NB);
            } while (0 != rc && EINTR == errno);
        }
        if (0 != rc) {
            busy = (EWOULDBLOCK == errno);
            /* A file system that does not lock: a lock file there is no
             * use to anyone - the one we made, or one of ours another
             * run left - and would keep a directory we made from being
             * empty at finalize */
            if (!busy && (created || ENOLCK == errno || EOPNOTSUPP == errno) &&
                0 == fstatat(dirfd, lname, &now, AT_SYMLINK_NOFOLLOW) &&
                now.st_dev == st.st_dev && now.st_ino == st.st_ino) {
                (void) unlinkat(dirfd, lname, 0);
            }
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

bool pmix_ptl_base_rndz_pid(const char *line, pid_t *pid)
{
    unsigned long val;
    char *endptr = NULL;

    if (NULL == line) {
        return false;
    }
    errno = 0;
    val = strtoul(line, &endptr, 10);
    if (0 != errno || NULL == endptr || endptr == line || 0 == val ||
        (unsigned long) INT_MAX < val || val != (unsigned long) (pid_t) val) {
        return false;
    }
    *pid = (pid_t) val;
    return true;
}

bool pmix_ptl_base_pid_gone(pid_t pid)
{
    if (pid == pmix_globals.pid) {
        return true;
    }
    return (0 != kill(pid, 0) && EPERM != errno);
}

bool pmix_ptl_base_server_gone(int dirfd, const char *name, uid_t owner, const char *lock,
                               pid_t pid)
{
    /* A file without the lock line was written by an earlier release,
     * which held no lock - and keeping every such file made one left
     * behind by a server long gone stop the search for any other on this
     * node with too-many-conns, however well the files of current servers
     * said they were running. Its pid is what such a file has to say for
     * itself, and what a server starting at its name already goes by. A
     * file WITH the line is not judged by its pid: from another pid
     * namespace sharing the directory, the pid is another process or none */
    if (NULL == lock) {
        return (0 < pid && pmix_ptl_base_pid_gone(pid));
    }
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
     * if we have it, else native */
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

bool pmix_ptl_base_rndz_file_sound(const char *filename, const struct stat *st)
{
    struct stat lst;

    return (NULL != st && 0 == lstat(filename, &lst) && S_ISREG(lst.st_mode) &&
            lst.st_dev == st->st_dev && lst.st_ino == st->st_ino && 1 == st->st_nlink &&
            0 == (st->st_mode & (S_IWGRP | S_IWOTH)));
}

/* Whether the kernel's answer names an id we can make sense of: one
 * mapped into our user namespace. A map that cannot be read - no user
 * namespaces, as off Linux - maps every id */
static bool id_named(const char *mapfile, uint32_t id)
{
    uint32_t out;

    return (PMIX_ERR_NOT_FOUND != pmix_util_id_to_parent(mapfile, id, &out));
}

bool pmix_ptl_base_named_file_vouches(const char *filename, const struct stat *st,
                                      const char *uidmap)
{
    size_t n;
    bool ok;

    if (!pmix_ptl_base_rndz_file_sound(filename, st)) {
        ok = false;
    } else {
        ok = ((pmix_ptl_base.server_expect_given &&
               pmix_ptl_base.server_expect_uid == st->st_uid) ||
              geteuid() == st->st_uid || 0 == st->st_uid);
        for (n = 0; !ok && n < pmix_ptl_base.naccess_uids; n++) {
            ok = (pmix_ptl_base.access_uids[n] == st->st_uid);
        }
        if (!ok) {
            ok = !id_named(uidmap, (uint32_t) st->st_uid);
        }
    }
    if (!ok) {
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "ptl:base: %s does not vouch for its server", filename);
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
    char *line, *local = NULL, *lns = NULL, *luri = NULL, *lhost;
    pmix_rank_t lrank;
    char *bfrops = NULL, *psec = NULL, **dest;
    const char *tag;
    size_t taglen;
    char *lock = NULL;
    bool gone, owned, believed;
    pid_t pid = 0;
    int nlines = 0;
    size_t budget = PMIX_PTL_MAX_CONN_FILE;
    struct stat st;

    /* if we cannot open the file, then the server must not
     * be configured to support tool connections, or this
     * user isn't authorized to access it - or it may just
     * not exist yet! Check for existence */
    /* coverity[TOCTOU] */
    if (0 != access(filename, R_OK)) {
        if (ENOENT == errno && !optional && !found_by_search &&
            PMIX_PTL_FILE_DEFER == pmix_ptl_base.file_wait) {
            /* the caller looks again from a timer, rather than this
             * thread - the progress thread - waiting here */
            return PMIX_ERR_TEMP_UNAVAILABLE;
        }
        if (ENOENT == errno && !optional && !found_by_search &&
            PMIX_PTL_FILE_WAIT == pmix_ptl_base.file_wait) {
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
        /* a candidate a search found that cannot be read is passed over,
         * as one that cannot be parsed is */
        if (!optional && !found_by_search) {
            pmix_show_help("help-ptl-base.txt", "file-not-found", true,
                           filename, open_failure(errno));
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
    /* and the wire formats and security mechanisms it accepts, its local
     * transport and its lock. They follow every line
     * a released reader takes by position, and are found by their tag
     * rather than by where they fall. The first of each counts */
    if (NULL != p) {
        while (nlines++ < PMIX_PTL_MAX_TAG_LINES && NULL != (line = pmix_ptl_base_getline(fp, &budget))) {
            /* the line after the version is its writer's pid */
            if (1 == nlines && pmix_ptl_base_rndz_pid(line, &pid)) {
                free(line);
                continue;
            }
            if (0 == strncmp(line, PMIX_PTL_LOCAL_URI_TAG,
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
           pmix_ptl_base_server_gone(AT_FDCWD, filename, st.st_uid, lock, pid);
    /* a file the caller named vouches for its server; one found by a
     * search, or at a fixed name, never does */
    believed = !gone && !regular_only &&
               pmix_ptl_base_named_file_vouches(filename, owned ? &st : NULL,
                                                "/proc/self/uid_map");
    free(lock);
    fclose(fp);
    /* A file we searched for, or found at a fixed name, says whose server
     * it is by its owner - so it must be the file at the name, with one
     * name, that only its owner can write. Any other is passed over */
    if (!gone && (found_by_search || regular_only) &&
        !pmix_ptl_base_rndz_file_sound(filename, owned ? &st : NULL)) {
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "pmix:tool: passing over %s - not a file only its owner can write, "
                            "with one name", filename);
        free(srvr);
        free(p);
        free(local);
        free(bfrops);
        free(psec);
        return PMIX_ERR_NOT_FOUND;
    }
    if (gone) {
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "pmix:tool: passing over %s - the server that wrote it is gone",
                            filename);
        free(srvr);
        free(p);
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
            free(local);
            free(bfrops);
            free(psec);
            return PMIX_ERR_NOMEM;
        }
        cn->nspace = nspace;
        cn->rank = rank;
        cn->uri = uri;
        cn->version = p;
        cn->vouched = believed;
        cn->owner_known = owned;
        cn->owner = owned ? st.st_uid : (uid_t) -1;
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

pmix_status_t pmix_ptl_base_df_search_depth(char *dirname, char *prefix, bool exact,
                                            pmix_info_t info[], size_t ninfo, bool optional,
                                            pmix_list_t *connections, int depth)
{
    return df_search(dirname, prefix, exact, info, ninfo, optional, connections, depth);
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

/* Whether `id` is the overflow id of a user namespace that does not map
 * every id to itself - where the kernel gives that one id for any id the
 * namespace does not map */
static bool overflow_unnamed(const char *mapfile, uint32_t id, uint32_t overflow)
{
    return (id == overflow && !pmix_util_id_map_is_identity(mapfile));
}

pmix_ptl_judgement_t pmix_ptl_base_judge_server(const char *uidmap, const char *gidmap,
                                                bool answered, uid_t kuid, bool havegid,
                                                gid_t kgid, bool given, uid_t uid, gid_t gid,
                                                bool vouched)
{
    uint32_t puid, pgid;
    size_t n;
    bool ok;

    /* An answer that names nobody we know - no answer, or an id from
     * outside our user namespace, which the kernel shows as the overflow
     * id - says nothing of who runs the server. Then only where its
     * address came from does. In a user namespace that does not map every
     * id to itself, the overflow id is how the kernel shows every id from
     * outside it, so it is no id even where the map has an entry for it */
    if (!answered || !id_named(uidmap, (uint32_t) kuid) ||
        (havegid && !id_named(gidmap, (uint32_t) kgid)) ||
        overflow_unnamed(uidmap, (uint32_t) kuid, pmix_util_overflow_uid()) ||
        (havegid && overflow_unnamed(gidmap, (uint32_t) kgid, pmix_util_overflow_gid()))) {
        return vouched ? PMIX_PTL_SERVER_OK_VOUCHED : PMIX_PTL_SERVER_UNNAMED;
    }
    if (given) {
        /* the server that started us said who it is - in its own user
         * namespace, which is the parent of ours if we run in one: the
         * kernel reports the server's ids translated into ours */
        ok = (kuid == uid && (!havegid || kgid == gid));
        if (!ok) {
            ok = (PMIX_SUCCESS == pmix_util_id_to_parent(uidmap, (uint32_t) kuid, &puid) &&
                  puid == (uint32_t) uid);
            if (ok && havegid) {
                ok = (PMIX_SUCCESS == pmix_util_id_to_parent(gidmap, (uint32_t) kgid, &pgid) &&
                      pgid == (uint32_t) gid);
            }
        }
        return ok ? PMIX_PTL_SERVER_OK : PMIX_PTL_SERVER_WRONG_USER;
    }
    ok = (kuid == geteuid() || 0 == kuid);
    for (n = 0; !ok && n < pmix_ptl_base.naccess_uids; n++) {
        ok = (kuid == pmix_ptl_base.access_uids[n]);
    }
    for (n = 0; !ok && havegid && n < pmix_ptl_base.naccess_gids; n++) {
        ok = (kgid == pmix_ptl_base.access_gids[n]);
    }
    return ok ? PMIX_PTL_SERVER_OK : PMIX_PTL_SERVER_WRONG_USER;
}

/* How check_peer waits for a TCP connection the kernel's table does not
 * list yet - one the server has yet to accept: CHECK_WAIT asks again,
 * sleeping between, for as long as the connect's deadline runs -
 * ptl_base_handshake_wait_time - or with no deadline CHECK_TRIES times,
 * and then refuses the server as unidentified (the blocking connect, on its
 * caller's thread); CHECK_ONCE asks once and returns PMIX_ERR_NOT_FOUND,
 * saying nothing, for the caller to ask again later (the event-driven
 * connect, which must not hold the progress thread, and asks again from a
 * timer on the same terms); CHECK_LAST asks once more and takes no answer
 * for an answer */
typedef enum {
    CHECK_WAIT,
    CHECK_ONCE,
    CHECK_LAST
} check_wait_t;

/* how many times to look again with no deadline, and the delay before the
 * n'th look again: doubling from a millisecond, up to 64 */
#define CHECK_TRIES 10
#define CHECK_DELAY_USEC(n) (1000L << (((n) < 6) ? (n) : 6))

/* Has the server closed its end - or the connection failed - while we
 * waited for its kernel's table to list it? A server sends nothing before
 * our request, so anything readable is the close */
static bool peer_closed(int sd)
{
    char c;
    ssize_t n = recv(sd, &c, 1, MSG_PEEK | MSG_DONTWAIT);

    return (0 == n || (0 > n && EAGAIN != errno && EWOULDBLOCK != errno && EINTR != errno));
}

/* The time now plus `secs` - a connect's deadline */
static void deadline_in(int secs, struct timeval *when)
{
    gettimeofday(when, NULL);
    when->tv_sec += secs;
}

/* Wait, blocking, while the kernel's table does not list the TCP
 * connection on `sd`: until it does (PMIX_SUCCESS, with the uid it
 * names), the server closes its end (PMIX_ERR_UNREACH), or the deadline
 * passes - with no deadline, CHECK_TRIES more looks - without finding it
 * (PMIX_ERR_NOT_FOUND). Any other answer of the table's is returned as it
 * is */
static pmix_status_t wait_listed(int sd, uid_t *kuid, const struct timeval *deadline)
{
    struct timeval tv, now;
    long left;
    pmix_status_t rc = PMIX_ERR_NOT_FOUND;
    int n;

    for (n = 0; PMIX_ERR_NOT_FOUND == rc; n++) {
        if (peer_closed(sd)) {
            pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                                "ptl:base:connect: the server closed the connection before "
                                "its kernel listed it");
            return PMIX_ERR_UNREACH;
        }
        tv.tv_sec = 0;
        tv.tv_usec = CHECK_DELAY_USEC(n);
        if (NULL == deadline) {
            if (CHECK_TRIES <= n) {
                return PMIX_ERR_NOT_FOUND;
            }
        } else {
            gettimeofday(&now, NULL);
            left = (deadline->tv_sec - now.tv_sec) * 1000000L +
                   (deadline->tv_usec - now.tv_usec);
            if (0 >= left) {
                pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                                    "ptl:base:connect: the server's kernel did not list the "
                                    "connection within %d seconds",
                                    pmix_ptl_base_connect_wait());
                return PMIX_ERR_NOT_FOUND;
            }
            if (left < tv.tv_usec) {
                tv.tv_usec = left;
            }
        }
        retry_wait(&tv);
        rc = pmix_util_getid_tcp(sd, kuid);
    }
    return rc;
}

/* Who a rendezvous file we searched for, or found at a fixed name, says
 * runs its server - see pmix_ptl_base.rndz_owner_given */
typedef struct {
    bool owner_given;
    uid_t owner;
    pid_t pid;
} rndz_claim_t;

static pmix_status_t check_peer(pmix_peer_t *peer, bool given, uid_t uid, gid_t gid,
                                bool vouched, const rndz_claim_t *claim, check_wait_t wait);

pmix_status_t pmix_ptl_base_check_server(pmix_peer_t *peer, bool given, uid_t uid, gid_t gid,
                                         bool vouched)
{
    rndz_claim_t claim = {.owner_given = pmix_ptl_base.rndz_owner_given,
                          .owner = pmix_ptl_base.rndz_owner,
                          .pid = pmix_ptl_base.rndz_pid};

    return check_peer(peer, given, uid, gid, vouched, &claim, CHECK_WAIT);
}

static pmix_status_t check_peer(pmix_peer_t *peer, bool given, uid_t uid, gid_t gid,
                                bool vouched, const rndz_claim_t *claim, check_wait_t wait)
{
    uid_t kuid = 0;
    gid_t kgid = 0;
    pid_t kpid = 0;
    bool tcp = false, answered = true;
    char want[128];
    struct sockaddr_storage sa;
    pmix_socklen_t salen = sizeof(sa);
    pmix_status_t rc;
    pmix_ptl_judgement_t verdict;

    /* a client's server is the one named for it - by the environment it
     * was started with, or by its caller */
    if (PMIX_PEER_IS_CLIENT(pmix_globals.mypeer) && !PMIX_PEER_IS_TOOL(pmix_globals.mypeer) &&
        !PMIX_PEER_IS_SINGLETON(pmix_globals.mypeer)) {
        vouched = true;
    }

    memset(&sa, 0, sizeof(sa));
    if (NULL == peer->transport || NULL == peer->transport->peer_identity) {
        /* Over TCP, the kernel names the owner of the server's end of the
         * connection, from its table of TCP connections - a user, but no
         * group */
        tcp = true;
        rc = pmix_util_getid_tcp(peer->sd, &kuid);
        if (PMIX_SUCCESS != rc) {
            /* Only a server on this host is talked to: one at any address
             * that is not this host's is on another host, or in another
             * network namespace */
            if (0 != getpeername(peer->sd, (struct sockaddr *) &sa, &salen)) {
                /* the server has closed the connection already */
                return PMIX_ERR_UNREACH;
            }
            if (!local_addr(&sa)) {
                pmix_show_help("help-ptl-base.txt", "server-not-local", true,
                               pmix_globals.hostname, pmix_net_get_hostname((struct sockaddr *) &sa),
                               pmix_net_get_port((struct sockaddr *) &sa));
                return PMIX_ERR_UNREACH;
            }
            /* One whose address is this host's has its end in that table,
             * as an established connection - but not always yet. Our end
             * is connected once the server's SYN-ACK arrives, and the
             * server's is established only when its kernel has taken our
             * ACK, which on loopback it may get to a moment later: until
             * then macOS holds it in SYN_RECEIVED, Linux as a request in
             * SYN_RECV, and FreeBSD in its syncache, outside the table.
             * Give it that moment */
            if (PMIX_ERR_NOT_FOUND == rc && CHECK_ONCE == wait) {
                return PMIX_ERR_NOT_FOUND;
            }
            if (PMIX_ERR_NOT_FOUND == rc && CHECK_WAIT == wait) {
                struct timeval deadline;

                if (0 < pmix_ptl_base_connect_wait()) {
                    deadline_in(pmix_ptl_base_connect_wait(), &deadline);
                }
                rc = wait_listed(peer->sd, &kuid,
                                 (0 < pmix_ptl_base.handshake_wait_time) ? &deadline : NULL);
            }
            if (PMIX_ERR_NOMEM == rc || PMIX_ERR_UNREACH == rc) {
                return rc;
            }
            if (PMIX_ERR_NOT_SUPPORTED == rc) {
                /* no table to ask, or one that cannot be read */
                answered = false;
            } else if (PMIX_SUCCESS != rc) {
                /* any other failure to read the table is no answer */
                pmix_show_help("help-ptl-base.txt", "server-unidentified", true,
                               pmix_globals.hostname);
                return PMIX_ERR_INVALID_CRED;
            }
        }
    } else if (PMIX_SUCCESS != peer->transport->peer_identity(peer->sd, &kuid, &kgid, &kpid)) {
        pmix_show_help("help-ptl-base.txt", "server-unidentified", true, pmix_globals.hostname);
        return PMIX_ERR_INVALID_CRED;
    }

    verdict = pmix_ptl_base_judge_server("/proc/self/uid_map", "/proc/self/gid_map", answered,
                                         kuid, !tcp, kgid, given, uid, gid, vouched);
    switch (verdict) {
    case PMIX_PTL_SERVER_OK:
        /* A server found through its file is the file's: the kernel must
         * say it runs as the file's owner and, for a file by pid, that it
         * is that process - where the kernel names a pid, on the local
         * socket. Anyone else answering at the address it gives is not the
         * server the file was written by */
        if (NULL != claim && claim->owner_given && kuid != claim->owner) {
            pmix_show_help("help-ptl-base.txt", "server-not-file-owner", true,
                           pmix_globals.hostname, (unsigned long) kuid,
                           (unsigned long) claim->owner);
            return PMIX_ERR_INVALID_CRED;
        }
        if (NULL != claim && 0 != claim->pid && !tcp && 0 != kpid && kpid != claim->pid) {
            pmix_show_help("help-ptl-base.txt", "server-not-file-pid", true,
                           pmix_globals.hostname, (unsigned long) kpid,
                           (unsigned long) claim->pid);
            return PMIX_ERR_INVALID_CRED;
        }
        return PMIX_SUCCESS;
    case PMIX_PTL_SERVER_OK_VOUCHED:
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "ptl:base: the kernel names no user for the server, whose "
                            "address came from a source that vouches for it - connecting");
        return PMIX_SUCCESS;
    case PMIX_PTL_SERVER_UNNAMED:
        if (!answered) {
            if (0 == sa.ss_family) {
                (void) getpeername(peer->sd, (struct sockaddr *) &sa, &salen);
            }
            pmix_show_help("help-ptl-base.txt", "server-unnamed", true, pmix_globals.hostname,
                           pmix_net_get_hostname((struct sockaddr *) &sa),
                           pmix_net_get_port((struct sockaddr *) &sa));
        } else {
            pmix_show_help("help-ptl-base.txt", "server-unmapped", true, pmix_globals.hostname,
                           (unsigned long) kuid);
        }
        return PMIX_ERR_INVALID_CRED;
    default:
        break;
    }
    if (given) {
        if (tcp) {
            snprintf(want, sizeof(want), "uid %lu, as the server that started this process said",
                     (unsigned long) uid);
        } else {
            snprintf(want, sizeof(want), "uid %lu gid %lu, as the server that started this "
                     "process said", (unsigned long) uid, (unsigned long) gid);
        }
    } else if (tcp) {
        snprintf(want, sizeof(want), "this process's own user, root, or a user "
                 "pmix_ptl_base_server_access names");
    } else {
        snprintf(want, sizeof(want), "this process's own user, root, or one "
                 "pmix_ptl_base_server_access allows");
    }
    if (tcp) {
        pmix_show_help("help-ptl-base.txt", "server-identity-tcp", true,
                       pmix_globals.hostname, (unsigned long) kuid, want);
    } else {
        pmix_show_help("help-ptl-base.txt", "server-identity", true, pmix_globals.hostname,
                       (unsigned long) kuid, (unsigned long) kgid, want);
    }
    return PMIX_ERR_INVALID_CRED;
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

static pmix_status_t send_connect_ack(pmix_peer_t *peer, pmix_info_t iptr[], size_t niptr)
{
    char *msg;
    size_t sdsize = 0;
    pmix_status_t rc;

    pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                        "pmix:ptl SEND CONNECT ACK");

    /* set our ID flag and compute the required handshake size */
    peer->proc_type.flag = pmix_ptl_base_set_flag(&sdsize);

    /* construct the contact message */
    rc = construct_message(peer, &msg, &sdsize, iptr, niptr);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    /* send the entire message across */
    /* the message carries our token, if we have one: it goes no further */
    if (PMIX_SUCCESS != pmix_ptl_base_send_blocking(peer->sd, msg, sdsize)) {
        pmix_ptl_base_wipe_released(msg, sdsize);
        free(msg);
        return PMIX_ERR_UNREACH;
    }
    pmix_ptl_base_wipe_released(msg, sdsize);
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
static pmix_status_t server_status(uint32_t u32);

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
    reply = server_status(ntohl(u32));

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

/* Whether the connection is on the local socket, whose far end the kernel
 * identifies - and whose handshake is the offer, the server's choice, and
 * the credential */
static bool local_transport(pmix_peer_t *peer)
{
    return NULL != peer->transport && NULL != peer->transport->peer_identity;
}

/* The local socket's handshake up to the server's final answer: send the
 * offer, read the server's choice and take it, then send the credential
 * made with it. recv_connect_ack reads the answer to that */
static pmix_status_t local_handshake(pmix_peer_t *peer, pmix_info_t iptr[], size_t niptr)
{
    char *msg = NULL;
    char choice[PMIX_PTL_CHOICE_MAX + 1];
    size_t sz = 0;
    struct timeval save;
    pmix_socklen_t tsz = sizeof(save);
    bool sockopt = true;
    uint32_t u32;
    pmix_status_t rc;

    pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                        "pmix:ptl SEND OFFER");
    peer->proc_type.flag = pmix_ptl_base_set_flag(&sz);
    rc = build_offer(peer, &msg, &sz);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    rc = pmix_ptl_base_send_blocking(peer->sd, msg, sz);
    free(msg);
    msg = NULL;
    if (PMIX_SUCCESS != rc) {
        return PMIX_ERR_UNREACH;
    }

    rc = pmix_ptl_base_set_timeout(peer, &save, &tsz, &sockopt);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    rc = pmix_ptl_base_recv_blocking(peer->sd, (char *) &u32, sizeof(uint32_t));
    if (PMIX_SUCCESS == rc) {
        rc = server_status(ntohl(u32));
    }
    if (PMIX_SUCCESS == rc) {
        rc = pmix_ptl_base_recv_blocking(peer->sd, (char *) &u32, sizeof(uint32_t));
    }
    if (PMIX_SUCCESS == rc) {
        u32 = ntohl(u32);
        if (0 == u32 || PMIX_PTL_CHOICE_MAX < u32) {
            rc = PMIX_ERR_BAD_PARAM;
        }
    }
    if (PMIX_SUCCESS == rc) {
        memset(choice, 0, sizeof(choice));
        rc = pmix_ptl_base_recv_blocking(peer->sd, choice, u32);
    }
    if (sockopt) {
        if (0 != setsockopt(peer->sd, SOL_SOCKET, SO_RCVTIMEO, &save, tsz)) {
            pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                                "pmix: could not reset setsockopt SO_RCVTIMEO");
        }
    }
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    rc = apply_choice(peer, choice, u32);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }

    pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                        "pmix:ptl SEND CREDENTIAL");
    rc = build_credential(peer, iptr, niptr, &msg, &sz);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    /* the message carries our token, if we have one: it goes no further */
    rc = pmix_ptl_base_send_blocking(peer->sd, msg, sz);
    pmix_ptl_base_wipe_released(msg, sz);
    free(msg);
    if (PMIX_SUCCESS != rc) {
        return PMIX_ERR_UNREACH;
    }
    return PMIX_SUCCESS;
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

/* Whether a server's address is this host's, for a process deciding
 * whether to talk to it: one on the interface list
 * (pmix_ptl_base_addr_on_host), or one an interface holds by the
 * kernel's own list - which names some the interface list made at init
 * leaves out: an address added since, a secondary address on Linux, a
 * point-to-point or link-local one on BSD. An address the kernel holds
 * may still lead elsewhere, as a Kubernetes service address does - the
 * kernel's table of TCP connections then says nothing of the server, and
 * pmix_ptl_base_check_server refuses it */
static bool local_addr(const struct sockaddr_storage *addr)
{
#if defined(HAVE_IFADDRS_H) && defined(HAVE_NET_IF_H)
    struct sockaddr_storage peer;
    const struct sockaddr_in6 *p6 = (const struct sockaddr_in6 *) &peer;
    const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *) addr;
    struct sockaddr_in *in4;
    struct in6_addr a6;
    struct ifaddrs *ifs, *ifa;
    bool held = false;

    if (pmix_ptl_base_addr_on_host(addr)) {
        return true;
    }
    memcpy(&peer, addr, sizeof(peer));
    /* an IPv4 address may arrive v4-mapped */
    if (AF_INET6 == addr->ss_family && IN6_IS_ADDR_V4MAPPED(&in6->sin6_addr)) {
        memset(&peer, 0, sizeof(peer));
        in4 = (struct sockaddr_in *) &peer;
        in4->sin_family = AF_INET;
        memcpy(&in4->sin_addr, &in6->sin6_addr.s6_addr[12], sizeof(in4->sin_addr));
    }
    if (AF_INET != peer.ss_family && AF_INET6 != peer.ss_family) {
        return false;
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
             * bytes 2 and 3, which an address given to connect to does
             * not carry - and it is ours only on the link it is held on */
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
    return pmix_ptl_base_addr_on_host(addr);
#endif
}

/* Only a server on this host is talked to. Over TCP, one at an address
 * that is not this host's is on another host - or in another network
 * namespace - and is not dialled: PMIX_ERR_UNREACH, saying so. The local
 * transport reaches only this host */
pmix_status_t pmix_ptl_base_server_on_host(const struct sockaddr_storage *addr,
                                           const pmix_ptl_module_t *transport)
{
    if ((NULL != transport && NULL != transport->peer_identity) || local_addr(addr)) {
        return PMIX_SUCCESS;
    }
    pmix_show_help("help-ptl-base.txt", "server-not-local", true, pmix_globals.hostname,
                   pmix_net_get_hostname((struct sockaddr *) addr),
                   pmix_net_get_port((struct sockaddr *) addr));
    return PMIX_ERR_UNREACH;
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

pmix_status_t pmix_ptl_base_make_connection(pmix_peer_t *peer, char *suri,
                                            pmix_info_t *iptr, size_t niptr)
{
    struct sockaddr_storage myconnection;
    pmix_ptl_module_t *transport = NULL;
    pmix_status_t rc;
    size_t len;
    bool renamed = false;

    rc = refuse_outdated(peer);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }

    /* setup the connection - to a server on this host */
    rc = pmix_ptl_base_uri_to_addr(suri, &myconnection, &len, &transport);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    rc = pmix_ptl_base_server_on_host(&myconnection, transport);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    /* do not error log - might just be a stale connection point */
    rc = pmix_ptl_base_connect(&myconnection, len, &peer->sd);
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
                                    pmix_ptl_base.server_release_vouched);
    if (PMIX_SUCCESS != rc) {
        CLOSE_THE_SOCKET(peer->sd);
        return rc;
    }

    /* send our identity and any authentication credentials to the server */
    if (local_transport(peer)) {
        rc = local_handshake(peer, iptr, niptr);
    } else {
        rc = send_connect_ack(peer, iptr, niptr);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
        }
    }
    if (PMIX_SUCCESS != rc) {
        CLOSE_THE_SOCKET(peer->sd);
        return renamed ? PMIX_ERR_EXISTS : rc;
    }

    /* do whatever handshake is required */
    if (PMIX_SUCCESS != (rc = recv_connect_ack(peer))) {
        CLOSE_THE_SOCKET(peer->sd);
        if (PMIX_ERR_EXISTS == rc && !renamed && take_new_identity()) {
            renamed = true;
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
    PMIX_CNCT_OFFERING,
    PMIX_CNCT_CHOICE_STATUS,
    PMIX_CNCT_CHOICE_LEN,
    PMIX_CNCT_CHOICE,
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
    char *suri;                 // the address being tried
    pmix_info_t *iptr;          // the info blob for the connect-ack
    size_t niptr;
    struct sockaddr_storage addr;
    size_t addrlen;
    pmix_ptl_module_t *transport; // whose address addr is
    bool expect_given;            // the server's identity, if we were told it
    bool vouched;                 // its file says it may be connected to unnamed
    rndz_claim_t claim;           // who the file it was found by says runs it
    uid_t expect_uid;
    gid_t expect_gid;
    int sd;
    int connect_tries;
    pmix_cnct_state_t state;
    char *msg;                  // the connect-ack being sent
    size_t msglen;
    size_t sent;
    char field[PMIX_MAX_NSLEN + 1]; // the reply field being read - and the server's choice
    size_t fieldlen;
    size_t got;
    pmix_event_t ev;
    bool ev_active;
    pmix_event_t timer;
    bool timer_active;
    int check_tries;              // looks again at a server the table does not list yet
    int wait;                     // the connect's bound in seconds, 0 for none
    pmix_ptl_connect_nb_cbfunc_t cbfunc;
    void *cbdata;
} pmix_ptl_connect_op_t;

/* the field buffer also holds the server's choice */
typedef char pmix_ptl_choice_fits_t[(PMIX_PTL_CHOICE_MAX <= PMIX_MAX_NSLEN + 1) ? 1 : -1];

static void cnopcon(pmix_ptl_connect_op_t *p)
{
    p->peer = NULL;
    p->nspace = NULL;
    p->rank = PMIX_RANK_UNDEF;
    p->suri = NULL;
    p->iptr = NULL;
    p->niptr = 0;
    memset(&p->addr, 0, sizeof(p->addr));
    p->addrlen = 0;
    p->transport = NULL;
    p->expect_given = false;
    p->vouched = false;
    memset(&p->claim, 0, sizeof(p->claim));
    p->expect_uid = (uid_t) -1;
    p->expect_gid = (gid_t) -1;
    p->sd = -1;
    p->connect_tries = 0;
    p->state = PMIX_CNCT_CONNECTING;
    p->msg = NULL;
    p->msglen = 0;
    p->sent = 0;
    memset(p->field, 0, sizeof(p->field));
    p->fieldlen = 0;
    p->got = 0;
    memset(&p->ev, 0, sizeof(p->ev));
    p->ev_active = false;
    memset(&p->timer, 0, sizeof(p->timer));
    p->timer_active = false;
    p->check_tries = 0;
    p->wait = 0;
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
    if (NULL != p->iptr) {
        pmix_ptl_base_wipe_token_info(p->iptr, p->niptr);
        PMIX_INFO_FREE(p->iptr, p->niptr);
    }
    pmix_ptl_base_wipe_released(p->msg, p->msglen);
    free(p->msg);
}
static PMIX_CLASS_INSTANCE(pmix_ptl_connect_op_t,
                           pmix_list_item_t,
                           cnopcon, cnopdes);

static void cnct_begin(int sd, short args, void *cbdata);
static void cnct_finish(pmix_ptl_connect_op_t *op, pmix_status_t rc);
static void cnct_connected(int sd, short args, void *cbdata);
static void cnct_check(int sd, short args, void *cbdata);
static void cnct_send(int sd, short args, void *cbdata);
static void cnct_recv(int sd, short args, void *cbdata);

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
                        (NULL == op->suri) ? "server" : op->suri, op->wait);
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
    op->fieldlen = len;
    op->got = 0;
    memset(op->field, 0, sizeof(op->field));
    return cnct_wait(op, EV_READ, cnct_recv);
}

void pmix_ptl_base_connect_backoff(int tries, struct timeval *tv)
{
    long ms = 10;

    while (1 < tries-- && 1000 > ms) {
        ms *= 2;
    }
    if (1000 < ms) {
        ms = 1000;
    }
    tv->tv_sec = ms / 1000;
    tv->tv_usec = (ms % 1000) * 1000;
}

/* another attempt, after a delay when the last one found the server busy -
 * or the end of them */
static void cnct_retry(pmix_ptl_connect_op_t *op, int err)
{
    struct timeval tv = {0, 0};

    if (PMIX_MAX_RETRIES < op->connect_tries) {
        cnct_finish(op, PMIX_ERR_UNREACH);
        return;
    }
    if (PMIX_PTL_CONNECT_BUSY(err)) {
        pmix_ptl_base_connect_backoff(op->connect_tries, &tv);
    }
    pmix_event_evtimer_set(pmix_globals.evbase, &op->ev, cnct_begin, op);
    op->ev_active = true;
    PMIX_POST_OBJECT(op);
    pmix_event_evtimer_add(&op->ev, &tv);
}

static void cnct_begin(int sd, short args, void *cbdata)
{
    pmix_ptl_connect_op_t *op = (pmix_ptl_connect_op_t *) cbdata;
    int err;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_ACQUIRE_OBJECT(op);
    op->ev_active = false;

    /* one of the attempts pmix_ptl_base_connect makes, without waiting */
    op->connect_tries++;
    op->sd = socket(op->addr.ss_family, SOCK_STREAM | PMIX_SOCK_CLOEXEC, 0);
    if (0 > op->sd) {
        cnct_retry(op, pmix_socket_errno);
        return;
    }
    pmix_ptl_base_own_socket(op->sd);
    pmix_ptl_base_set_nonblocking(op->sd);
    if (0 == connect(op->sd, (struct sockaddr *) &op->addr, op->addrlen)) {
        cnct_connected(op->sd, EV_WRITE, op);
        return;
    }
    err = pmix_socket_errno;
    if (EINPROGRESS == err) {
        if (PMIX_SUCCESS == cnct_wait(op, EV_WRITE, cnct_connected)) {
            return;
        }
        cnct_finish(op, PMIX_ERROR);
        return;
    }
    pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                        "ptl:base:connect: connect failed: %s (%d)", strerror(err), err);
    CLOSE_THE_SOCKET(op->sd);
    op->sd = -1;
    cnct_retry(op, err);
}

static void cnct_connected(int sd, short args, void *cbdata)
{
    pmix_ptl_connect_op_t *op = (pmix_ptl_connect_op_t *) cbdata;
    int err = 0;
    pmix_socklen_t errlen = sizeof(err);
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_ACQUIRE_OBJECT(op);
    op->ev_active = false;

    if (0 != getsockopt(op->sd, SOL_SOCKET, SO_ERROR, (char *) &err, &errlen) || 0 != err) {
        if (0 == err) {
            err = pmix_socket_errno;
        }
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "ptl:base:connect: connect failed: %s (%d)", strerror(err), err);
        CLOSE_THE_SOCKET(op->sd);
        op->sd = -1;
        cnct_retry(op, err);
        return;
    }

    op->peer->sd = op->sd;
    op->peer->transport = op->transport;
    op->check_tries = 0;
    cnct_check(-1, 0, op);
}

/* Connected - to a server we may talk to, by what the kernel says of it?
 * A TCP connection the server has yet to accept is not in the kernel's
 * table yet; look again from a timer rather than sleep, as the blocking
 * connect does, on the progress thread - and for as long as the connect's
 * own deadline allows, so a server slow to accept is still reached, and
 * one that never does ends as any silent server does, PMIX_ERR_TIMEOUT.
 * With no deadline, as long as the blocking connect would. Then build the
 * same connect-ack send_connect_ack sends */
static void cnct_check(int sd, short args, void *cbdata)
{
    pmix_ptl_connect_op_t *op = (pmix_ptl_connect_op_t *) cbdata;
    pmix_status_t rc;
    size_t sdsize = 0;
    struct timeval tv;
    check_wait_t mode;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_ACQUIRE_OBJECT(op);
    op->ev_active = false;
    mode = (op->timer_active || CHECK_TRIES > op->check_tries) ? CHECK_ONCE : CHECK_LAST;
    rc = check_peer(op->peer, op->expect_given, op->expect_uid, op->expect_gid, op->vouched,
                    &op->claim, mode);
    if (PMIX_ERR_NOT_FOUND == rc && CHECK_ONCE == mode && peer_closed(op->sd)) {
        /* it will not be listed now - and there is nobody to talk to */
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "ptl:base:connect: the server closed the connection before "
                            "its kernel listed it");
        rc = PMIX_ERR_UNREACH;
    } else if (PMIX_ERR_NOT_FOUND == rc && CHECK_ONCE == mode) {
        tv.tv_sec = 0;
        tv.tv_usec = CHECK_DELAY_USEC(op->check_tries);
        ++op->check_tries;
        pmix_event_assign(&op->ev, pmix_globals.evbase, -1, 0, cnct_check, op);
        PMIX_POST_OBJECT(op);
        if (0 == pmix_event_add(&op->ev, &tv)) {
            op->ev_active = true;
            return;
        }
        rc = PMIX_ERROR;
    }
    if (PMIX_SUCCESS != rc) {
        cnct_finish(op, rc);
        return;
    }
    op->peer->proc_type.flag = pmix_ptl_base_set_flag(&sdsize);
    if (local_transport(op->peer)) {
        /* the local socket starts with the offer */
        rc = build_offer(op->peer, &op->msg, &sdsize);
        op->state = PMIX_CNCT_OFFERING;
    } else {
        rc = construct_message(op->peer, &op->msg, &sdsize, op->iptr, op->niptr);
        op->state = PMIX_CNCT_SENDING;
    }
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        cnct_finish(op, rc);
        return;
    }
    op->msglen = sdsize;
    op->sent = 0;
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
                            "ptl:base:connect: send of connect-ack failed: %s (%d)",
                            strerror(pmix_socket_errno), pmix_socket_errno);
        cnct_finish(op, PMIX_ERR_UNREACH);
        return;
    }
    /* the message carries our token, if we have one: it goes no further */
    pmix_ptl_base_wipe_released(op->msg, op->msglen);
    free(op->msg);
    op->msg = NULL;
    op->msglen = 0;

    /* every connector's answer starts with a status - to the offer, the
     * status that comes before the server's choice */
    if (PMIX_SUCCESS != cnct_expect(op, (PMIX_CNCT_OFFERING == op->state)
                                            ? PMIX_CNCT_CHOICE_STATUS : PMIX_CNCT_STATUS,
                                    sizeof(uint32_t))) {
        cnct_finish(op, PMIX_ERROR);
    }
}

/* A status a server sent. The codes the connect uses to say how it is
 * going - done, or under way - never come from a server, and one that does
 * is not a reply. A server asking for a security handshake is refused:
 * this library runs none */
static pmix_status_t server_status(uint32_t u32)
{
    pmix_status_t reply = (pmix_status_t) u32;

    if (PMIX_ERR_LOCAL_URI_REQUIRED == reply) {
        /* say where the address came from, and what to use instead. A
         * file that gave us only TCP is a hand-made one, or one read on
         * another host than the server's: the server's own report file
         * names its local socket too */
        if (NULL != pmix_ptl_base.source_file) {
            pmix_show_help("help-ptl-base.txt", "local-uri-required-file", true,
                           pmix_ptl_base.source_file);
        } else if (NULL != pmix_ptl_base.source_uri) {
            pmix_show_help("help-ptl-base.txt", "local-uri-required-uri", true,
                           pmix_ptl_base.source_uri);
        } else if (NULL != pmix_ptl_base.uri && 0 != strncmp(pmix_ptl_base.uri, "file:", 5)) {
            pmix_show_help("help-ptl-base.txt", "local-uri-required-uri", true,
                           pmix_ptl_base.uri);
        } else {
            pmix_show_help("help-ptl-base.txt", "local-uri-required-uri", true,
                           "(the address given)");
        }
        return reply;
    }

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

    if (sizeof(uint32_t) == op->fieldlen) {
        memcpy(&u32, op->field, sizeof(uint32_t));
        u32 = ntohl(u32);
    }
    client = (PMIX_PEER_IS_CLIENT(pmix_globals.mypeer) ||
              PMIX_PEER_IS_SINGLETON(pmix_globals.mypeer)) &&
             !PMIX_PEER_IS_TOOL(pmix_globals.mypeer);

    switch (op->state) {
    case PMIX_CNCT_CHOICE_STATUS:
        reply = server_status(u32);
        if (PMIX_SUCCESS != reply) {
            return reply;
        }
        return cnct_expect(op, PMIX_CNCT_CHOICE_LEN, sizeof(uint32_t));

    case PMIX_CNCT_CHOICE_LEN:
        if (0 == u32 || PMIX_PTL_CHOICE_MAX < u32) {
            return PMIX_ERR_BAD_PARAM;
        }
        return cnct_expect(op, PMIX_CNCT_CHOICE, u32);

    case PMIX_CNCT_CHOICE:
        rc = apply_choice(op->peer, op->field, op->fieldlen);
        if (PMIX_SUCCESS != rc) {
            return rc;
        }
        rc = build_credential(op->peer, op->iptr, op->niptr, &op->msg, &op->msglen);
        if (PMIX_SUCCESS != rc) {
            return rc;
        }
        op->sent = 0;
        op->state = PMIX_CNCT_SENDING;
        /* the send finishes the operation itself if it fails */
        cnct_send(op->sd, EV_WRITE, op);
        return PMIX_OPERATION_IN_PROGRESS;

    case PMIX_CNCT_STATUS:
        reply = server_status(u32);
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
        reply = server_status(u32);
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
        n = recv(op->sd, op->field + op->got, op->fieldlen - op->got, 0);
        if (0 < n) {
            op->got += (size_t) n;
            continue;
        }
        if (0 == n) {
            pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                                "ptl:base:connect: server closed the connection");
            if (PMIX_CNCT_STATUS == op->state) {
                hint_closed_no_reply(op->peer, op->suri);
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
    op->claim.owner_given = pmix_ptl_base.rndz_owner_given;
    op->claim.owner = pmix_ptl_base.rndz_owner;
    op->claim.pid = pmix_ptl_base.rndz_pid;
    op->expect_uid = pmix_ptl_base.server_expect_uid;
    op->expect_gid = pmix_ptl_base.server_expect_gid;
    rc = pmix_ptl_base_uri_to_addr(suri, &op->addr, &op->addrlen, &op->transport);
    if (PMIX_SUCCESS == rc) {
        rc = pmix_ptl_base_server_on_host(&op->addr, op->transport);
    }
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(op);
        return rc;
    }
    op->peer = peer;
    op->cbfunc = cbfunc;
    op->cbdata = cbdata;
    pmix_list_append(&pmix_ptl_base.connecting, &op->super);

    op->wait = pmix_ptl_base_connect_wait();
    if (0 < op->wait) {
        tv.tv_sec = op->wait;
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

/* The uid of the server at the far end of `peer`'s connection, for the
 * credential we send it (PMIX_PSEC_SERVER_UID) - where the kernel names
 * it: on the local socket, as pmix_ptl_base_check_server read it. Over
 * TCP there is none to give. In a user namespace the uid is given only
 * where it is the same number in the parent namespace, so it names the
 * same user to a munged on either side of it; an id from outside our
 * namespace (the overflow id) is not given */
static bool server_uid(pmix_peer_t *peer, uint32_t *uid)
{
    uid_t kuid;
    gid_t kgid;
    pid_t kpid;
    uint32_t puid;
    pmix_status_t rc;

    if (NULL == peer->transport || NULL == peer->transport->peer_identity ||
        PMIX_SUCCESS != peer->transport->peer_identity(peer->sd, &kuid, &kgid, &kpid)) {
        return false;
    }
    rc = pmix_util_id_to_parent("/proc/self/uid_map", (uint32_t) kuid, &puid);
    if (PMIX_ERR_NOT_SUPPORTED != rc && (PMIX_SUCCESS != rc || puid != (uint32_t) kuid)) {
        return false;
    }
    *uid = (uint32_t) kuid;
    return true;
}

/* The packed info of a connect-ack carries the connection token, when
 * there is one: zero the buffer before it is released */
static void release_packed(pmix_buffer_t *buf)
{
    pmix_ptl_base_wipe_released(buf->base_ptr, buf->bytes_allocated);
    PMIX_DESTRUCT(buf);
}

/* Add a name to an offer's list, once, while the list has room */
static pmix_status_t offer_add(char ***list, const char *name)
{
    int i, n = PMIx_Argv_count(*list);

    if (NULL == name || '\0' == name[0] || PMIX_PTL_OFFER_MAX_NAMELEN < strlen(name) ||
        PMIX_PTL_OFFER_MAX_NAMES <= n) {
        return PMIX_SUCCESS;
    }
    for (i = 0; i < n; i++) {
        if (0 == strcmp((*list)[i], name)) {
            return PMIX_SUCCESS;
        }
    }
    return PMIx_Argv_append_nosize(list, name);
}

/* A list for the offer: the preferred name first, then those in avail -
 * a comma-separated list, in priority order, which is freed */
static pmix_status_t offer_names(char ***list, const char *preferred, char *avail)
{
    char **names = NULL;
    pmix_status_t rc = PMIX_SUCCESS;
    int i;

    if (NULL != preferred) {
        rc = offer_add(list, preferred);
    }
    if (NULL != avail) {
        names = PMIx_Argv_split(avail, ',');
        free(avail);
    }
    for (i = 0; PMIX_SUCCESS == rc && NULL != names && NULL != names[i]; i++) {
        rc = offer_add(list, names[i]);
    }
    PMIx_Argv_free(names);
    return rc;
}

static size_t list_size(char **list)
{
    size_t sz = 1;
    int i;

    for (i = 0; NULL != list && NULL != list[i]; i++) {
        sz += strlen(list[i]) + 1;
    }
    return sz;
}

#define PMIX_PTL_PUT_LIST(l)                                \
    do {                                                    \
        uint8_t _n = (uint8_t) PMIx_Argv_count((l));        \
        int _i;                                             \
        PMIX_PTL_PUT_U8(_n);                                \
        for (_i = 0; _i < (int) _n; _i++) {                 \
            PMIX_PTL_PUT_STRING((l)[_i]);                   \
        }                                                   \
    } while (0)

/* The local socket's offer: our version, type and identifier, and the
 * security, wire-format and datastore components we have, with the wire
 * format and datastore chosen for this server first */
static pmix_status_t build_offer(pmix_peer_t *peer, char **msgout, size_t *sz)
{
    char **psecs = NULL, **bfrops = NULL, **gdss = NULL;
    pmix_bfrops_module_t *bfmod;
    pmix_ptl_hdr_t hdr;
    uint8_t bftype;
    char *msg;
    size_t sdsize, csize;
    bool named;
    pmix_status_t rc;

    if (NULL == peer->nptr || NULL == peer->nptr->compat.gds) {
        return PMIX_ERR_INIT;
    }
    bfmod = (NULL != peer->nptr->compat.bfrops)
                ? peer->nptr->compat.bfrops : pmix_globals.mypeer->nptr->compat.bfrops;
    bftype = pmix_globals.mypeer->nptr->compat.type;

    rc = offer_names(&psecs, NULL, pmix_psec_base_get_available_modules());
    if (PMIX_SUCCESS == rc) {
        rc = offer_names(&bfrops, bfmod->version, pmix_bfrops_base_get_available_modules());
    }
    if (PMIX_SUCCESS == rc) {
        rc = offer_names(&gdss, peer->nptr->compat.gds->name,
                         pmix_gds_base_get_available_modules());
    }
    if (PMIX_SUCCESS != rc) {
        goto done;
    }

    switch (peer->proc_type.flag) {
    case PMIX_TOOL_NEEDS_ID:
    case PMIX_LAUNCHER_NEEDS_ID:
        named = false;
        break;
    case PMIX_SIMPLE_CLIENT:
    case PMIX_TOOL_GIVEN_ID:
    case PMIX_LAUNCHER_GIVEN_ID:
    case PMIX_SCHEDULER_WITH_ID:
    case PMIX_SINGLETON_CLIENT:
    case PMIX_TOOL_CLIENT:
    case PMIX_LAUNCHER_CLIENT:
        named = true;
        break;
    default:
        rc = PMIX_ERR_NOT_SUPPORTED;
        goto done;
    }

    sdsize = strlen(PMIX_VERSION) + 1 + 1;
    if (named) {
        sdsize += strlen(pmix_globals.myid.nspace) + 1 + sizeof(uint32_t);
    }
    sdsize += list_size(psecs) + list_size(bfrops) + 1 + list_size(gdss);

    memset(&hdr, 0, sizeof(pmix_ptl_hdr_t));
    hdr.pindex = -1;
    hdr.tag = UINT32_MAX;
    hdr.nbytes = sdsize;
    sdsize += sizeof(hdr);
    msg = (char *) calloc(1, sdsize);
    if (NULL == msg) {
        rc = PMIX_ERR_NOMEM;
        goto done;
    }
    memcpy(msg, &hdr, sizeof(hdr));
    csize = sizeof(hdr);
    PMIX_PTL_PUT_STRING(PMIX_VERSION);
    PMIX_PTL_PUT_U8(peer->proc_type.flag);
    if (named) {
        PMIX_PTL_PUT_PROCID(pmix_globals.myid);
    }
    PMIX_PTL_PUT_LIST(psecs);
    PMIX_PTL_PUT_LIST(bfrops);
    PMIX_PTL_PUT_U8(bftype);
    PMIX_PTL_PUT_LIST(gdss);
    *msgout = msg;
    *sz = sdsize;

done:
    PMIx_Argv_free(psecs);
    PMIx_Argv_free(bfrops);
    PMIx_Argv_free(gdss);
    return rc;
}

/* Take the server's choice for this connection - each must be one we
 * offered, which is one we have */
static pmix_status_t apply_choice(pmix_peer_t *peer, char *msg, size_t len)
{
    char *mg = msg, *sec = NULL, *bfrops = NULL, *gds = NULL;
    size_t cnt = len;
    uint8_t bt = PMIX_BFROP_BUFFER_UNDEF;
    pmix_psec_module_t *psec;
    pmix_bfrops_module_t *bfmod;
    pmix_gds_base_module_t *gmod;
    pmix_info_t ginfo;
    pmix_status_t rc = PMIX_ERR_BAD_PARAM;

    PMIX_PTL_GET_STRING(sec);
    PMIX_PTL_GET_STRING(bfrops);
    PMIX_PTL_GET_U8(bt);
    PMIX_PTL_GET_STRING(gds);
    if (0 != cnt) {
        goto error;
    }

    psec = pmix_psec_base_assign_module(sec);
    bfmod = pmix_bfrops_base_assign_module(bfrops);
    PMIX_INFO_LOAD(&ginfo, PMIX_GDS_MODULE, gds, PMIX_STRING);
    gmod = pmix_gds_base_assign_module(&ginfo, 1);
    PMIX_INFO_DESTRUCT(&ginfo);
    if (NULL == psec || 0 != strcmp(psec->name, sec) || NULL == bfmod || NULL == gmod ||
        0 != strcmp(gmod->name, gds) ||
        (PMIX_BFROP_BUFFER_NON_DESC != bt && PMIX_BFROP_BUFFER_FULLY_DESC != bt)) {
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "pmix: the server chose %s/%s/%u/%s, which is not what we offered",
                            sec, bfrops, (unsigned) bt, gds);
        rc = PMIX_ERR_NOT_SUPPORTED;
        goto error;
    }
    pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                        "pmix: the server chose security %s, wire format %s, datastore %s",
                        sec, bfrops, gds);
    peer->nptr->compat.psec = psec;
    peer->nptr->compat.bfrops = bfmod;
    peer->nptr->compat.type = bt;
    peer->nptr->compat.gds = gmod;
    peer->gds = gmod;
    rc = PMIX_SUCCESS;

error:
    free(sec);
    free(bfrops);
    free(gds);
    return rc;
}

/* The local socket's credential message: the credential, made with the
 * chosen security component, and the info, packed in the chosen wire
 * format */
static pmix_status_t build_credential(pmix_peer_t *peer, pmix_info_t *iptr, size_t niptr,
                                      char **msgout, size_t *sz)
{
    pmix_psec_module_t *psec = peer->nptr->compat.psec;
    pmix_bfrops_module_t *bfmod = peer->nptr->compat.bfrops;
    pmix_byte_object_t cred;
    pmix_buffer_t buf;
    pmix_ptl_hdr_t hdr;
    pmix_info_t dir;
    size_t ndirs = 0, sdsize, csize;
    uint32_t suid;
    char *msg;
    pmix_status_t rc;

    PMIX_BYTE_OBJECT_CONSTRUCT(&cred);
    if (server_uid(peer, &suid)) {
        PMIX_INFO_LOAD(&dir, PMIX_PSEC_SERVER_UID, &suid, PMIX_UINT32);
        ndirs = 1;
    }
    rc = psec->create_cred((struct pmix_peer_t *) pmix_globals.mypeer,
                           (0 < ndirs) ? &dir : NULL, ndirs, NULL, NULL, &cred);
    if (0 < ndirs) {
        PMIX_INFO_DESTRUCT(&dir);
    }
    if (PMIX_SUCCESS != rc) {
        wipe_cred(&cred);
        return rc;
    }

    PMIX_CONSTRUCT(&buf, pmix_buffer_t);
    if (NULL != iptr) {
        buf.type = peer->nptr->compat.type;
        rc = bfmod->pack(&buf, &niptr, 1, PMIX_SIZE);
        if (PMIX_SUCCESS == rc) {
            rc = bfmod->pack(&buf, iptr, niptr, PMIX_INFO);
        }
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            wipe_cred(&cred);
            release_packed(&buf);
            return rc;
        }
    }

    memset(&hdr, 0, sizeof(pmix_ptl_hdr_t));
    hdr.pindex = -1;
    hdr.tag = UINT32_MAX;
    hdr.nbytes = sizeof(uint32_t) + cred.size + buf.bytes_used;
    sdsize = sizeof(hdr) + hdr.nbytes;
    msg = (char *) calloc(1, sdsize);
    if (NULL == msg) {
        wipe_cred(&cred);
        release_packed(&buf);
        return PMIX_ERR_NOMEM;
    }
    memcpy(msg, &hdr, sizeof(hdr));
    csize = sizeof(hdr);
    PMIX_PTL_PUT_U32(cred.size);
    PMIX_PTL_PUT_BLOB(cred.bytes, cred.size);
    wipe_cred(&cred);
    PMIX_PTL_PUT_BLOB(buf.base_ptr, buf.bytes_used);
    release_packed(&buf);
    *msgout = msg;
    *sz = sdsize;
    return PMIX_SUCCESS;
}

static pmix_status_t construct_message(pmix_peer_t *peer, char **msgout, size_t *sz,
                                       pmix_info_t *iptr, size_t niptr)
{
    char *msg;
    char *sec, *bfrops, *gds;
    pmix_bfrop_buffer_type_t bftype;
    uid_t euid;
    gid_t egid;
    pmix_buffer_t buf;
    pmix_status_t rc;
    pmix_ptl_hdr_t hdr;
    size_t sdsize, csize, ndirs;
    pmix_byte_object_t cred;
    pmix_psec_module_t *psec;
    pmix_bfrops_module_t *bfmod;
    pmix_info_t dir;
    uint32_t suid;

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
     * get a credential, if the security system provides one. Not
     * every psec module will do so, thus we must first check */
    PMIX_BYTE_OBJECT_CONSTRUCT(&cred);
    ndirs = 0;
    if (server_uid(peer, &suid)) {
        PMIX_INFO_LOAD(&dir, PMIX_PSEC_SERVER_UID, &suid, PMIX_UINT32);
        ndirs = 1;
    }
    rc = psec->create_cred((struct pmix_peer_t *) pmix_globals.mypeer,
                           (0 < ndirs) ? &dir : NULL, ndirs, NULL, NULL, &cred);
    if (0 < ndirs) {
        PMIX_INFO_DESTRUCT(&dir);
    }
    if (PMIX_SUCCESS != rc) {
        wipe_cred(&cred);
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
            wipe_cred(&cred);
            release_packed(&buf);
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
        wipe_cred(&cred);
        if (NULL != iptr) {
            release_packed(&buf);
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
    wipe_cred(&cred);

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
            release_packed(&buf);
        }
        pmix_ptl_base_wipe_released(msg, sdsize);
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
        release_packed(&buf);
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
        tv.tv_sec = pmix_ptl_base_connect_wait();
        tv.tv_usec = 0;
        if (0 != setsockopt(peer->sd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv))) {
            *sockopt = false;
        }
    }
    if (!*sockopt) {
        /* the replies are then read with no bound - say so */
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "ptl:base: could not bound the receives on socket %d: %s",
                            peer->sd, strerror(errno));
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
    pmix_ptl_base.server_named = true;

    pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                        "pmix: RECV CONNECT CONFIRMATION FOR TOOL %s:%d FROM SERVER %s:%d",
                        pmix_globals.myid.nspace, pmix_globals.myid.rank, peer->info->pname.nspace,
                        peer->info->pname.rank);

    /* get the outcome of our request */
    PMIX_PTL_RECV_U32(peer->sd, reply);
    reply = server_status((uint32_t) reply);
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
    pid_t pid = 0;
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
    if (!pmix_ptl_base_rndz_pid(p2, &pid)) {
        pid = 0;
    }
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
            pmix_ptl_base_server_gone(AT_FDCWD, filename, st.st_uid, lock, pid));
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
    size_t n, len;
    pmix_infolist_t *iptr;
    pmix_status_t rc;
    char **dirs, **more;
    bool legacy;

    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_CONSTRUCT(&servers, pmix_list_t);

    /* the directories servers of this release write their files in -
     * each looked in, not below - and, only with legacy peers on, the
     * whole system tmpdir, where servers of earlier releases write theirs
     * (it holds the pmix-user directories too, so they are not looked in
     * twice) */
    dirs = pmix_ptl_base_rndz_dirs(true);
    more = pmix_ptl_base_rndz_dirs(false);
    for (n = 0; NULL != more && NULL != more[n]; n++) {
        PMIx_Argv_append_unique_nosize(&dirs, more[n]);
    }
    PMIx_Argv_free(more);
    legacy = pmix_ptl_base_legacy_walk();
    if (legacy) {
        query_servers(NULL, &servers, pmix_ptl_base.search_depth);
    }
    len = strlen(pmix_ptl_base.system_tmpdir);
    for (n = 0; NULL != dirs && NULL != dirs[n]; n++) {
        if (legacy && 0 == strncmp(dirs[n], pmix_ptl_base.system_tmpdir, len) &&
            ('/' == dirs[n][len] || '\0' == dirs[n][len])) {
            continue;
        }
        query_servers(dirs[n], &servers, 0);
    }
    PMIx_Argv_free(dirs);

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
