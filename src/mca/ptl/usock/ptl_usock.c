/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/* The usock transport: an AF_UNIX socket for the processes on this host,
 * whose far end the kernel identifies - its uid, gid and pid. */

#include "src/include/pmix_config.h"
#include "src/include/pmix_globals.h"
#include "src/include/pmix_socket_errno.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif
#ifdef HAVE_SYS_SOCKET_H
#    include <sys/socket.h>
#endif
#include <sys/stat.h>
#include <sys/un.h>

#include "src/util/pmix_error.h"
#include "src/util/pmix_fd.h"
#include "src/util/pmix_getid.h"
#include "src/util/pmix_idname.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_printf.h"
#include "src/util/pmix_show_help.h"

#include "src/mca/ptl/base/base.h"
#include "src/mca/ptl/usock/ptl_usock.h"

static pmix_status_t usock_setup_listener(pmix_info_t info[], size_t ninfo,
                                          pmix_listener_t *lt);
static pmix_status_t uri_to_addr(const char *uri, struct sockaddr_storage *connection,
                                 size_t *len);
static pmix_status_t peer_identity(int sd, uid_t *uid, gid_t *gid, pid_t *pid);
static void usock_close(void);

PMIX_EXPORT pmix_ptl_module_t pmix_ptl_usock_module = {
    .name = "usock",
    .local = true,
    .setup_listener = usock_setup_listener,
    .uri_to_addr = uri_to_addr,
    .peer_identity = peer_identity,
    .close = usock_close
};

/* The socket's name within its directory */
static const char *socket_name(const char *path)
{
    const char *slash = strrchr(path, '/');

    return (NULL == slash) ? path : slash + 1;
}

/* Remove the socket and its directory, if this process made them */
static void usock_close(void)
{
    if (0 <= pmix_mca_ptl_usock_component.dfd) {
        if (NULL != pmix_mca_ptl_usock_component.path) {
            (void) unlinkat(pmix_mca_ptl_usock_component.dfd,
                            socket_name(pmix_mca_ptl_usock_component.path), 0);
        }
        close(pmix_mca_ptl_usock_component.dfd);
        pmix_mca_ptl_usock_component.dfd = -1;
    }
    free(pmix_mca_ptl_usock_component.path);
    pmix_mca_ptl_usock_component.path = NULL;
    if (NULL != pmix_mca_ptl_usock_component.dir) {
        (void) rmdir(pmix_mca_ptl_usock_component.dir);
        free(pmix_mca_ptl_usock_component.dir);
        pmix_mca_ptl_usock_component.dir = NULL;
    }
}

/* A directive naming the socket's mode: a uint32 */
static bool mode_directive(const pmix_info_t *info, uint32_t *val, pmix_status_t *rc)
{
    if (PMIX_UINT32 != info->value.type) {
        pmix_output(0, "ptl:usock:setup_listener: directive %s must be a PMIX_UINT32, not %s",
                    info->key, PMIx_Data_type_string(info->value.type));
        *rc = PMIX_ERR_BAD_PARAM;
        return false;
    }
    *val = info->value.data.uint32;
    return true;
}

/* A directive naming the socket's owner or group: a number, or a name */
static bool id_directive(const pmix_info_t *info, bool user, uint32_t *val, pmix_status_t *rc)
{
    *rc = user ? pmix_util_uid_from_value(&info->value, val)
               : pmix_util_gid_from_value(&info->value, val);
    if (PMIX_SUCCESS != *rc) {
        pmix_output(0, "ptl:usock:setup_listener: directive %s names no %s: %s", info->key,
                    user ? "user" : "group", PMIx_Error_string(*rc));
        return false;
    }
    return true;
}

/* Open the listening socket, in a directory of its own beneath the
 * session tmpdir that only this process's user can enter, and owner-only
 * itself - made that way, so that no other process can reach it at any
 * moment. Only the host widens it: PMIX_USERID and PMIX_GRPID name the
 * socket's owner and group, and PMIX_SOCKET_MODE its mode; the directory
 * then lets in whom the mode lets in, and stays this process's. The
 * directory is held open from the moment it is made, and every change in
 * it is made through that descriptor: its name is in the tmpdir, which
 * others may be able to write. A socket that cannot be made is an error -
 * every process of this release on this host connects here */
static pmix_status_t usock_setup_listener(pmix_info_t info[], size_t ninfo,
                                          pmix_listener_t *lt)
{
    struct sockaddr_un *addr;
    struct sockaddr_storage ss;
    struct stat st;
    uid_t owner = (uid_t) -1;
    gid_t group = (gid_t) -1;
    mode_t mode = S_IRUSR | S_IWUSR, dmode = S_IRWXU;
    uint32_t val;
    char *dir = NULL, *path = NULL;
    const char *why = NULL;
    pmix_status_t rc = PMIX_SUCCESS;
    size_t n;
    int sd = -1, dfd = -1, flags;

    pmix_output_verbose(2, pmix_ptl_base_framework.framework_output, "ptl:usock:setup_listener");

    for (n = 0; n < ninfo; n++) {
        if (PMIX_CHECK_KEY(&info[n], PMIX_USERID)) {
            if (!id_directive(&info[n], true, &val, &rc)) {
                return rc;
            }
            owner = (uid_t) val;
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_GRPID)) {
            if (!id_directive(&info[n], false, &val, &rc)) {
                return rc;
            }
            group = (gid_t) val;
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_SOCKET_MODE)) {
            if (!mode_directive(&info[n], &val, &rc)) {
                return rc;
            }
            if (0 != (val & ~(uint32_t) 0777)) {
                pmix_output(0, "ptl:usock:setup_listener: %s value %o is not a mode",
                            info[n].key, (unsigned) val);
                return PMIX_ERR_BAD_PARAM;
            }
            mode = (mode_t) val;
        }
    }
    /* the socket's group is the one named, else ours - not the one a new
     * file inherits from its directory, as on the BSDs */
    if ((gid_t) -1 == group) {
        group = getegid();
    }
    /* whom the mode lets connect may enter the directory - and nobody but
     * this process's user may list it. A socket given to another user is
     * reached by that user as one of the others */
    if (0 != (mode & S_IRWXG)) {
        dmode |= S_IXGRP;
    }
    if (0 != (mode & S_IRWXO) || ((uid_t) -1 != owner && geteuid() != owner)) {
        dmode |= S_IXOTH;
    }

    if (NULL == pmix_ptl_base.session_tmpdir) {
        return PMIX_ERR_NOT_AVAILABLE;
    }
    if (0 > pmix_asprintf(&dir, "%s/pmix-XXXXXX", pmix_ptl_base.session_tmpdir)) {
        return PMIX_ERR_NOMEM;
    }
    /* mkdtemp makes it 0700, owned by us */
    if (NULL == mkdtemp(dir)) {
        why = strerror(errno);
        path = dir;
        dir = NULL;
        goto fail;
    }
    /* hold it - and be sure what is held is the directory just made */
    dfd = open(dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (0 > dfd || 0 != fstat(dfd, &st) || !S_ISDIR(st.st_mode) || geteuid() != st.st_uid ||
        S_IRWXU != (st.st_mode & 07777)) {
        why = "the directory made for it was not the one found at its name";
        goto fail;
    }
    if (0 > pmix_asprintf(&path, "%s/pmix-%lu", dir, (unsigned long) pmix_globals.pid)) {
        rc = PMIX_ERR_NOMEM;
        goto cleanup;
    }
    memset(&ss, 0, sizeof(ss));
    addr = (struct sockaddr_un *) &ss;
    if (sizeof(addr->sun_path) <= strlen(path)) {
        why = "the path is longer than a socket's path can be";
        goto fail;
    }
    /* the URI carries the path after its ';', and nothing may follow */
    if (NULL != strchr(path, ';')) {
        why = "the path holds a ';', which a URI cannot carry";
        goto fail;
    }
    addr->sun_family = AF_UNIX;
    memcpy(addr->sun_path, path, strlen(path) + 1);

    sd = socket(AF_UNIX, SOCK_STREAM | PMIX_SOCK_CLOEXEC, 0);
    if (0 > sd) {
        why = strerror(pmix_socket_errno);
        goto fail;
    }
    if (PMIX_SUCCESS != pmix_fd_set_cloexec(sd)) {
        why = "it could not be made close-on-exec";
        goto fail;
    }
    if (0 != bind(sd, (struct sockaddr *) addr, sizeof(*addr))) {
        why = strerror(pmix_socket_errno);
        goto fail;
    }
    /* bind takes a path: the socket must be in the directory held, which
     * only this process's user can write */
    if (0 != fstatat(dfd, socket_name(path), &st, AT_SYMLINK_NOFOLLOW) ||
        !S_ISSOCK(st.st_mode)) {
        why = "the socket was not made in the directory made for it";
        goto fail;
    }
    /* the directory is still ours alone, so nothing reaches the socket
     * while it takes its mode and owner */
    if (0 != fchmodat(dfd, socket_name(path), mode, 0) ||
        0 != fchownat(dfd, socket_name(path), owner, group, AT_SYMLINK_NOFOLLOW)) {
        why = strerror(errno);
        goto fail;
    }
    if (0 != fchown(dfd, (uid_t) -1, group) || (S_IRWXU != dmode && 0 != fchmod(dfd, dmode))) {
        why = strerror(errno);
        goto fail;
    }
    if (0 != listen(sd, SOMAXCONN)) {
        why = strerror(pmix_socket_errno);
        goto fail;
    }
    if (0 > (flags = fcntl(sd, F_GETFL, 0)) || 0 > fcntl(sd, F_SETFL, flags | O_NONBLOCK)) {
        why = strerror(errno);
        goto fail;
    }

    if (0 > pmix_asprintf(&lt->uri, "%s.%u;%s%s", pmix_globals.myid.nspace,
                          pmix_globals.myid.rank, PMIX_PTL_USOCK_SCHEME, path)) {
        rc = PMIX_ERR_NOMEM;
        goto cleanup;
    }
    lt->socket = sd;
    pmix_mca_ptl_usock_component.dir = dir;
    pmix_mca_ptl_usock_component.path = path;
    pmix_mca_ptl_usock_component.dfd = dfd;
    pmix_output_verbose(2, pmix_ptl_base_framework.framework_output, "ptl:usock URI %s",
                        lt->uri);
    return PMIX_SUCCESS;

fail:
    pmix_show_help("help-ptl-base.txt", "usock-listener", true, pmix_globals.hostname,
                   (NULL == path) ? "(none)" : path, why);
    rc = PMIX_ERR_SILENT;
cleanup:
    if (0 <= sd) {
        CLOSE_THE_SOCKET(sd);
    }
    if (0 <= dfd) {
        if (NULL != path) {
            (void) unlinkat(dfd, socket_name(path), 0);
        }
        close(dfd);
    }
    if (NULL != dir) {
        (void) rmdir(dir);
        free(dir);
    }
    free(path);
    return rc;
}

/* The address a usock:// URI names. Any other scheme is not ours */
static pmix_status_t uri_to_addr(const char *uri, struct sockaddr_storage *connection,
                                 size_t *len)
{
    struct sockaddr_un *addr = (struct sockaddr_un *) connection;
    const char *path;

    if (NULL == uri || 0 != strncmp(uri, PMIX_PTL_USOCK_SCHEME, strlen(PMIX_PTL_USOCK_SCHEME))) {
        return PMIX_ERR_NOT_SUPPORTED;
    }
    path = uri + strlen(PMIX_PTL_USOCK_SCHEME);
    /* an absolute path, that fits */
    if ('/' != path[0] || sizeof(addr->sun_path) <= strlen(path)) {
        PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
        return PMIX_ERR_BAD_PARAM;
    }
    memset(connection, 0, sizeof(*connection));
    addr->sun_family = AF_UNIX;
    memcpy(addr->sun_path, path, strlen(path) + 1);
    *len = sizeof(*addr);
    return PMIX_SUCCESS;
}

static pmix_status_t peer_identity(int sd, uid_t *uid, gid_t *gid, pid_t *pid)
{
    pmix_status_t rc;

    rc = pmix_util_getid(sd, uid, gid);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    if (PMIX_SUCCESS != pmix_util_getpid(sd, pid)) {
        /* the kernel gives no pid here */
        *pid = 0;
    }
    return PMIX_SUCCESS;
}
