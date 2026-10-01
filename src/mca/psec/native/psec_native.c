/*
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2016      IBM Corporation.  All rights reserved.
 * Copyright (c) 2017      Research Organization for Information Science
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
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifdef HAVE_SYS_TYPES_H
#    include <sys/types.h>
#endif
#include <sys/stat.h>
#ifdef HAVE_SYS_SOCKET_H
#    include <sys/socket.h>
#endif
#ifdef HAVE_SYS_UN_H
#    include <sys/un.h>
#endif
#if OAC_HAVE_APPLE && defined(HAVE_SYS_SYSCTL_H)
#    include <sys/sysctl.h>
#endif

#include "pmix_common.h"

#include "src/include/pmix_globals.h"
#include "src/include/pmix_socket_errno.h"
#include "src/mca/ptl/base/base.h"
#include "src/server/pmix_server_ops.h"
#include "src/util/pmix_environ.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_fd.h"
#include "src/util/pmix_getid.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_printf.h"
#include "src/util/pmix_show_help.h"

#include "psec_native.h"
#include "src/mca/psec/base/base.h"

static pmix_status_t native_init(void);
static void native_finalize(void);
static pmix_status_t create_cred(struct pmix_peer_t *peer, const pmix_info_t directives[],
                                 size_t ndirs, pmix_info_t **info, size_t *ninfo,
                                 pmix_byte_object_t *cred);
static pmix_status_t client_handshake(struct pmix_peer_t *peer, int sd);
static pmix_status_t validate_cred(struct pmix_peer_t *peer, const pmix_info_t directives[],
                                   size_t ndirs, pmix_info_t **info, size_t *ninfo,
                                   const pmix_byte_object_t *cred);
static pmix_status_t server_handshake(struct pmix_peer_t *peer, int sd);

pmix_psec_module_t pmix_native_module = {.name = "native",
                                         .init = native_init,
                                         .finalize = native_finalize,
                                         .create_cred = create_cred,
                                         .client_handshake = client_handshake,
                                         .validate_cred = validate_cred,
                                         .server_handshake = server_handshake};

/* The credential is the peer's effective uid and gid, followed - from the
 * release that added the local-socket check - by a trailer:
 *
 *     uid_t euid | gid_t egid | "PNX1" | uint8 idlen | idlen bytes
 *
 * The trailer says the peer can run the local-socket handshake, and its
 * bytes identify the kernel the peer runs on. Every earlier server reads
 * the uid and gid and ignores what follows, and every earlier client
 * sends no trailer - which is how a server tells the two apart. A later
 * release may append more after the kernel id. */
#define NATIVE_CRED_MAGIC     "PNX1"
#define NATIVE_CRED_MAGIC_LEN 4
#define NATIVE_KERNEL_ID_MAX  64

/* the local-socket handshake's limits on what it reads off the wire */
#define NATIVE_NONCE_LEN      32
#define NATIVE_MAX_NAME_LEN   256

/* this host's kernel, as create_cred reports it and validate_cred
 * compares against it. Set once, in init, before any credential is made */
static char kernel_id[NATIVE_KERNEL_ID_MAX + 1];
static size_t kernel_idlen = 0;

/* the directory this server made for its handshake sockets - created on
 * first use, removed at finalize. Only the progress thread touches these */
static char *sockdir_base = NULL;
static char *sockdir_rel = NULL;
static unsigned long sock_counter = 0;

/* Something that names the running kernel - the same for every process
 * on this host, containers included, and different on any other host
 * or after a reboot. It is not secret, and nothing trusts it: it only
 * says whether the local-socket handshake can possibly succeed, which the
 * handshake itself then decides. */
static void find_kernel_id(void)
{
    size_t n;
#if defined(__linux__)
    int fd;
    ssize_t rd;

    fd = open("/proc/sys/kernel/random/boot_id", O_RDONLY | O_CLOEXEC);
    if (0 <= fd) {
        rd = read(fd, kernel_id, NATIVE_KERNEL_ID_MAX);
        close(fd);
        if (0 < rd) {
            kernel_idlen = (size_t) rd;
        }
    }
#elif OAC_HAVE_APPLE && defined(HAVE_SYS_SYSCTL_H)
    size_t len = NATIVE_KERNEL_ID_MAX;

    if (0 == sysctlbyname("kern.bootsessionuuid", kernel_id, &len, NULL, 0)) {
        kernel_idlen = len;
    }
#endif
    /* keep only the printable part - a trailing newline or NUL is not
     * part of the name */
    for (n = 0; n < kernel_idlen; n++) {
        if (kernel_id[n] <= ' ' || kernel_id[n] > '~') {
            break;
        }
    }
    kernel_idlen = n;
    kernel_id[kernel_idlen] = '\0';
}

static pmix_status_t native_init(void)
{
    pmix_output_verbose(2, pmix_psec_base_framework.framework_output, "psec: native init");
    find_kernel_id();
    pmix_output_verbose(2, pmix_psec_base_framework.framework_output,
                        "psec: native kernel id \"%s\"", kernel_id);
    return PMIX_SUCCESS;
}

static void native_finalize(void)
{
    char *dir;

    pmix_output_verbose(2, pmix_psec_base_framework.framework_output, "psec: native finalize");
    if (NULL != sockdir_rel) {
        /* every socket is unlinked as its handshake ends, so the
         * directory is empty unless a handshake was cut short */
        if (0 <= pmix_asprintf(&dir, "%s/%s", sockdir_base, sockdir_rel)) {
            (void) rmdir(dir);
            free(dir);
        }
        free(sockdir_rel);
        sockdir_rel = NULL;
    }
    free(sockdir_base);
    sockdir_base = NULL;
}

static pmix_status_t create_cred(struct pmix_peer_t *peer, const pmix_info_t directives[],
                                 size_t ndirs, pmix_info_t **info, size_t *ninfo,
                                 pmix_byte_object_t *cred)
{
    pmix_peer_t *pr = (pmix_peer_t *) peer;
    uid_t euid;
    gid_t egid;
    char *tmp, *ptr;
    size_t len;

    /* ensure initialization */
    PMIX_BYTE_OBJECT_CONSTRUCT(cred);

    /* we may be responding to a local request for a credential, so
     * see if they specified a mechanism */
    if (!pmix_psec_base_check_directives("native", directives, ndirs)) {
        PMIX_ERROR_LOG(PMIX_ERR_NOT_SUPPORTED);
        return PMIX_ERR_NOT_SUPPORTED;
    }

    if (PMIX_PROTOCOL_V2 == pr->protocol) {
        /* tcp protocol - need to provide our effective
         * uid and gid for validation on remote end, and the
         * trailer that says we can run the local-socket check */
        len = sizeof(uid_t) + sizeof(gid_t) + NATIVE_CRED_MAGIC_LEN + 1 + kernel_idlen;
        tmp = (char *) malloc(len);
        if (NULL == tmp) {
            return PMIX_ERR_NOMEM;
        }
        euid = geteuid();
        memcpy(tmp, &euid, sizeof(uid_t));
        ptr = tmp + sizeof(uid_t);
        egid = getegid();
        memcpy(ptr, &egid, sizeof(gid_t));
        ptr += sizeof(gid_t);
        memcpy(ptr, NATIVE_CRED_MAGIC, NATIVE_CRED_MAGIC_LEN);
        ptr += NATIVE_CRED_MAGIC_LEN;
        *ptr = (char) kernel_idlen;
        ++ptr;
        if (0 < kernel_idlen) {
            memcpy(ptr, kernel_id, kernel_idlen);
        }
        cred->bytes = tmp;
        cred->size = len;
        goto complete;
    } else {
        /* unrecognized protocol */
        PMIX_ERROR_LOG(PMIX_ERR_NOT_SUPPORTED);
        return PMIX_ERR_NOT_SUPPORTED;
    }

complete:
    if (NULL != info) {
        /* mark that this came from us */
        PMIX_INFO_CREATE(*info, 1);
        if (NULL == *info) {
            /* our caller only reclaims the credential when we report
             * success, so release it here rather than stranding it */
            PMIX_BYTE_OBJECT_DESTRUCT(cred);
            return PMIX_ERR_NOMEM;
        }
        *ninfo = 1;
        PMIX_INFO_LOAD(&(*info)[0], PMIX_CRED_TYPE, "native", PMIX_STRING);
    }
    return PMIX_SUCCESS;
}

/* Decide the peer's identity, given the uid the kernel attests for it
 * and its group - attested too when `gid_attested`, else the group it
 * claimed.
 *
 * - An identity the host registered (a client) is what the peer must
 *   be: the attested uid, and the group, must match it.
 * - Any other identity (a tool, or a process that connected before the
 *   host registered it) is only what the peer claimed. The attested
 *   uid replaces it - the claim may be the uid inside a user namespace,
 *   which is not the uid outside it. The claimed group stands if it is
 *   the attested one or a group the user holds; otherwise an attested
 *   group replaces it, and a group nobody attested is refused.
 *
 * On success the peer's info carries the identity decided on. A refusal
 * for the group alone - the uid was right - is reported in *group_only,
 * since an attested group may yet settle it. */
static pmix_status_t settle_identity(pmix_peer_t *pr, uid_t uid, gid_t gid, bool gid_attested,
                                     bool *group_only)
{
    gid_t claimed = pr->info->gid;

    *group_only = false;

    if (pr->info->host_registered) {
        if (uid != pr->info->uid) {
            pmix_output_verbose(2, pmix_psec_base_framework.framework_output,
                                "psec: native peer is uid %lu - required uid %lu",
                                (unsigned long) uid, (unsigned long) pr->info->uid);
            return PMIX_ERR_INVALID_CRED;
        }
        if (gid != pr->info->gid) {
            pmix_output_verbose(2, pmix_psec_base_framework.framework_output,
                                "psec: native peer is gid %lu - required gid %lu",
                                (unsigned long) gid, (unsigned long) pr->info->gid);
            *group_only = true;
            return PMIX_ERR_INVALID_CRED;
        }
        return PMIX_SUCCESS;
    }

    if (gid_attested) {
        if (claimed != gid && !pmix_psec_base_gid_held(uid, claimed)) {
            claimed = gid;
        }
    } else if (!(uid == geteuid() && claimed == getegid()) &&
               !pmix_psec_base_gid_held(uid, claimed)) {
        *group_only = true;
        return PMIX_ERR_INVALID_CRED;
    }
    pr->info->uid = uid;
    pr->info->gid = claimed;
    return PMIX_SUCCESS;
}

static pmix_status_t validate_cred(struct pmix_peer_t *peer, const pmix_info_t directives[],
                                   size_t ndirs, pmix_info_t **info, size_t *ninfo,
                                   const pmix_byte_object_t *cred)
{
    static bool warned_remote = false, warned_legacy = false;
    pmix_peer_t *pr = (pmix_peer_t *) peer;
    uid_t euid = (uid_t) -1;
    gid_t egid = (gid_t) -1;
    uid_t owner = (uid_t) -1;
    char *ptr = NULL;
    size_t ln = 0, idlen = 0;
    bool capable = false;
    pmix_status_t rc;
    uint32_t u32;
    bool group_only;

    pmix_output_verbose(2, pmix_psec_base_framework.framework_output,
                        "psec: native validate_cred %s", (NULL == cred) ? "NULL" : "NON-NULL");

    /* if we are responding to a local request to validate a credential,
     * then see if they specified a mechanism. Settle "is this even our
     * job" before we start interpreting bytes that may not be ours -
     * the other modules screen in this order too */
    if (!pmix_psec_base_check_directives("native", directives, ndirs)) {
        return PMIX_ERR_NOT_SUPPORTED;
    }

    if (PMIX_PROTOCOL_V2 == pr->protocol) {
        /* this is a tcp protocol, so the cred is actually the uid/gid
         * passed upwards from the client */
        if (NULL == cred) {
            /* not allowed */
            return PMIX_ERR_INVALID_CRED;
        }
        ln = cred->size;
        if (sizeof(uid_t) + sizeof(gid_t) > ln) {
            return PMIX_ERR_INVALID_CRED;
        }
        memcpy(&euid, cred->bytes, sizeof(uid_t));
        memcpy(&egid, cred->bytes + sizeof(uid_t), sizeof(gid_t));
        ln -= sizeof(uid_t) + sizeof(gid_t);
        ptr = cred->bytes + sizeof(uid_t) + sizeof(gid_t);
        if (0 < ln) {
            /* a trailer must be ours, and hold what it says it holds */
            if (NATIVE_CRED_MAGIC_LEN + 1 > ln ||
                0 != memcmp(ptr, NATIVE_CRED_MAGIC, NATIVE_CRED_MAGIC_LEN)) {
                return PMIX_ERR_INVALID_CRED;
            }
            idlen = (uint8_t) ptr[NATIVE_CRED_MAGIC_LEN];
            if (NATIVE_CRED_MAGIC_LEN + 1 + idlen > ln) {
                return PMIX_ERR_INVALID_CRED;
            }
            ptr += NATIVE_CRED_MAGIC_LEN + 1;
            capable = true;
        }
    } else if (PMIX_PROTOCOL_UNDEF == pr->protocol) {
        /* we have no credential format we can trust, so there is
         * nothing here we can validate. Say so explicitly rather than
         * relying on the (uid_t)-1 initializers above to fail the
         * comparison below - a peer whose recorded uid/gid happened to
         * match those sentinels would otherwise be accepted */
        return PMIX_ERR_INVALID_CRED;
    } else {
        /* don't recognize the protocol */
        return PMIX_ERR_NOT_SUPPORTED;
    }

    if (0 > pr->sd) {
        /* not a connection - a credential passed to
         * PMIx_Validate_credential, with no connection to check it
         * against */
        pmix_output_verbose(2, pmix_psec_base_framework.framework_output,
                            "psec: native credential for uid %lu has no "
                            "connection to check against",
                            (unsigned long) euid);
        return PMIX_ERR_INVALID_CRED;
    }

    /* ask the kernel who owns the peer's end of the connection */
    if (pmix_psec_native_params.force_handshake && capable) {
        rc = PMIX_ERR_NOT_FOUND;
    } else {
        rc = pmix_util_getid_tcp(pr->sd, &owner);
    }
    if (PMIX_SUCCESS == rc) {
        /* the kernel records no group for a socket, so the group is
         * the one the peer claims - or, where that does not hold up and
         * the peer can run the local-socket check, the one that check
         * attests. Inside a user namespace the claim is the gid there */
        rc = settle_identity(pr, owner, egid, false, &group_only);
        if (PMIX_SUCCESS != rc) {
            if (group_only && capable) {
                pmix_output_verbose(2, pmix_psec_base_framework.framework_output,
                                    "psec: native cannot confirm the group of uid %lu - "
                                    "confirming it over a local socket",
                                    (unsigned long) owner);
                return PMIX_ERR_READY_FOR_HANDSHAKE;
            }
            if (group_only && !pr->info->host_registered) {
                pmix_show_help("help-psec-native.txt", "foreign-group", true,
                               (unsigned long) owner, (unsigned long) pr->info->gid);
            }
            return rc;
        }

    } else if (capable && PMIX_ERR_BAD_PARAM != rc) {
        /* The peer can be asked to prove itself over a local socket -
         * which reaches it only if it runs on this host's kernel. A peer
         * that names a different kernel is on another host, where
         * native cannot vouch for anyone */
        if (0 < kernel_idlen && 0 < idlen &&
            (idlen != kernel_idlen || 0 != memcmp(ptr, kernel_id, idlen))) {
            if (!warned_remote) {
                pmix_show_help("help-psec-native.txt", "remote-peer", true,
                               pmix_fd_get_peer_name(pr->sd), (unsigned long) euid);
                warned_remote = true;
            }
            return PMIX_ERR_INVALID_CRED;
        }
        pmix_output_verbose(2, pmix_psec_base_framework.framework_output,
                            "psec: native cannot see the owner of the connection from %s - "
                            "confirming it over a local socket",
                            pmix_fd_get_peer_name(pr->sd));
        return PMIX_ERR_READY_FOR_HANDSHAKE;

    } else if (pmix_psec_native_params.legacy_auth) {
        /* a release that predates the local-socket check, and nothing
         * the kernel can tell us - take it at its word, as those
         * releases always were */
        if (euid != pr->info->uid) {
            pmix_output_verbose(2, pmix_psec_base_framework.framework_output,
                                "psec: socket cred contains invalid uid %u - required uid %u",
                                euid, pr->info->uid);
            return PMIX_ERR_INVALID_CRED;
        }
        if (egid != pr->info->gid) {
            pmix_output_verbose(2, pmix_psec_base_framework.framework_output,
                                "psec: socket cred contains invalid gid %u - required gid %u",
                                egid, pr->info->gid);
            return PMIX_ERR_INVALID_CRED;
        }
        pmix_output_verbose(2, pmix_psec_base_framework.framework_output,
                            "psec: native accepting the claimed identity of an older peer "
                            "at %s", pmix_fd_get_peer_name(pr->sd));

    } else {
        if (!warned_legacy) {
            pmix_show_help("help-psec-native.txt", "legacy-peer", true,
                           pmix_fd_get_peer_name(pr->sd), (unsigned long) euid);
            warned_legacy = true;
        }
        return PMIX_ERR_INVALID_CRED;
    }

    /* validated - mark that we did it */
    if (NULL != info) {
        PMIX_INFO_CREATE(*info, 3);
        if (NULL == *info) {
            return PMIX_ERR_NOMEM;
        }
        *ninfo = 3;
        /* mark that this came from us */
        PMIX_INFO_LOAD(&(*info)[0], PMIX_CRED_TYPE, "native", PMIX_STRING);
        /* provide the identity it was validated as */
        u32 = pr->info->uid;
        PMIX_INFO_LOAD(&(*info)[1], PMIX_USERID, &u32, PMIX_UINT32);
        u32 = pr->info->gid;
        PMIX_INFO_LOAD(&(*info)[2], PMIX_GRPID, &u32, PMIX_UINT32);
    }
    return PMIX_SUCCESS;
}

/****    THE LOCAL-SOCKET HANDSHAKE    ****
 *
 * When the kernel cannot say who owns the peer's end of a TCP connection
 * - the peer is in a container with a network of its own, so its socket
 * is not in our table - the peer proves its identity over an AF_UNIX
 * socket instead, where the kernel reports the uid and gid of the process
 * that connected. TCP stays the connection; the AF_UNIX socket exists for
 * this exchange only:
 *
 *   server -> peer (TCP):  nonce, the base directory, and the socket's
 *                          path beneath it
 *   peer -> server (unix): the nonce
 *   server -> peer (TCP):  the outcome
 *
 * The nonce went to whoever holds the TCP connection, so the process that
 * returns it on the unix socket speaks for that connection. Each
 * handshake gets a socket of its own, removed when it ends. The peer
 * reaches it at the same path beneath the base directory, which it may
 * see somewhere else (psec_native_socket_dir) when a container mounts it
 * elsewhere.
 *
 * Every length on the wire is a uint32 in network order. */

static pmix_status_t send_string(int sd, const char *str)
{
    uint32_t u32;
    size_t len = strlen(str) + 1;
    pmix_status_t rc;

    u32 = htonl((uint32_t) len);
    rc = pmix_ptl_base_send_blocking(sd, (char *) &u32, sizeof(uint32_t));
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    return pmix_ptl_base_send_blocking(sd, (char *) str, len);
}

/* read a NUL-terminated string of at most `max` bytes, terminator
 * included. The caller frees it */
static pmix_status_t recv_string(int sd, char **str, size_t max)
{
    uint32_t u32;
    size_t len;
    char *s;
    pmix_status_t rc;

    rc = pmix_ptl_base_recv_blocking(sd, (char *) &u32, sizeof(uint32_t));
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    len = ntohl(u32);
    if (0 == len || max < len) {
        return PMIX_ERR_BAD_PARAM;
    }
    s = (char *) malloc(len);
    if (NULL == s) {
        return PMIX_ERR_NOMEM;
    }
    rc = pmix_ptl_base_recv_blocking(sd, s, len);
    if (PMIX_SUCCESS != rc) {
        free(s);
        return rc;
    }
    if ('\0' != s[len - 1]) {
        free(s);
        return PMIX_ERR_BAD_PARAM;
    }
    *str = s;
    return PMIX_SUCCESS;
}

static pmix_status_t get_nonce(unsigned char *nonce)
{
    int fd;
    size_t got = 0;
    ssize_t rd;

    fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (0 > fd) {
        return PMIX_ERR_NOT_AVAILABLE;
    }
    while (got < NATIVE_NONCE_LEN) {
        rd = read(fd, nonce + got, NATIVE_NONCE_LEN - got);
        if (0 > rd && EINTR == errno) {
            continue;
        }
        if (0 >= rd) {
            close(fd);
            return PMIX_ERR_NOT_AVAILABLE;
        }
        got += (size_t) rd;
    }
    close(fd);
    return PMIX_SUCCESS;
}

/* make this server's own directory for its sockets, the first time it
 * needs one. Several servers may share the base directory, so each gets
 * a uniquely named one beneath it. It must let any user reach a socket in
 * it (0711), since a peer may be any user - but not list it */
static pmix_status_t make_sockdir(void)
{
    const char *base;
    char *tmpl;
    char *slash;

    if (NULL != sockdir_rel) {
        return PMIX_SUCCESS;
    }
    base = pmix_psec_native_params.socket_dir;
    if (NULL == base) {
        base = pmix_server_globals.tmpdir;
    }
    if (NULL == base) {
        base = pmix_tmp_directory();
    }
    if (0 > pmix_asprintf(&tmpl, "%s/pmix-native.XXXXXX", base)) {
        return PMIX_ERR_NOMEM;
    }
    if (NULL == mkdtemp(tmpl)) {
        pmix_output_verbose(2, pmix_psec_base_framework.framework_output,
                            "psec: native cannot create a socket directory under %s: %s",
                            base, strerror(errno));
        free(tmpl);
        return PMIX_ERR_NOT_AVAILABLE;
    }
    if (0 != chmod(tmpl, S_IRWXU | S_IXGRP | S_IXOTH)) {
        (void) rmdir(tmpl);
        free(tmpl);
        return PMIX_ERR_NOT_AVAILABLE;
    }
    sockdir_base = strdup(base);
    slash = strrchr(tmpl, '/');
    sockdir_rel = strdup(slash + 1);
    free(tmpl);
    if (NULL == sockdir_base || NULL == sockdir_rel) {
        free(sockdir_base);
        sockdir_base = NULL;
        free(sockdir_rel);
        sockdir_rel = NULL;
        return PMIX_ERR_NOMEM;
    }
    return PMIX_SUCCESS;
}

/* The server's half of the handshake runs on its progress thread, so the
 * whole of it - however many waits the peer puts it through - shares one
 * deadline: the time a server gives a security handshake
 * (ptl_base_connect_ack_timeout). A per-wait bound is not one: a peer
 * could restart it with every byte it sends. -1 means no limit */
static int64_t now_ms(void)
{
    struct timespec tp;

    (void) clock_gettime(CLOCK_MONOTONIC, &tp);
    return (int64_t) tp.tv_sec * 1000 + tp.tv_nsec / 1000000;
}

static int64_t handshake_deadline(void)
{
    if (0 >= pmix_ptl_base.connect_ack_timeout) {
        return -1;
    }
    return now_ms() + (int64_t) pmix_ptl_base.connect_ack_timeout * 1000;
}

/* wait until `fd` is readable, or until the TCP connection `sd` says
 * something (the peer gave up, or went away), or the deadline passes */
static pmix_status_t wait_readable(int fd, int sd, int64_t deadline)
{
    struct pollfd pfd[2];
    int64_t left;
    int timeout, rc;

    pfd[0].fd = fd;
    pfd[0].events = POLLIN;
    pfd[1].fd = sd;
    pfd[1].events = POLLIN;
    while (1) {
        if (0 > deadline) {
            timeout = -1;
        } else {
            left = deadline - now_ms();
            if (0 >= left) {
                return PMIX_ERR_TIMEOUT;
            }
            timeout = (int) left;
        }
        pfd[0].revents = 0;
        pfd[1].revents = 0;
        rc = poll(pfd, 2, timeout);
        if (0 > rc && EINTR == errno) {
            continue;
        }
        if (0 > rc) {
            return PMIX_ERROR;
        }
        if (0 == rc) {
            return PMIX_ERR_TIMEOUT;
        }
        if (0 != pfd[1].revents) {
            return PMIX_ERR_UNREACH;
        }
        return PMIX_SUCCESS;
    }
}

/* read exactly `size` bytes from the non-blocking socket `fd`, against
 * the same deadline */
static pmix_status_t read_by(int fd, int sd, char *data, size_t size, int64_t deadline)
{
    size_t cnt = 0;
    ssize_t rd;
    pmix_status_t rc;

    while (cnt < size) {
        rc = wait_readable(fd, sd, deadline);
        if (PMIX_SUCCESS != rc) {
            return rc;
        }
        rd = recv(fd, data + cnt, size - cnt, 0);
        if (0 == rd) {
            return PMIX_ERR_UNREACH;
        }
        if (0 > rd) {
            if (EAGAIN == pmix_socket_errno || EWOULDBLOCK == pmix_socket_errno ||
                EINTR == pmix_socket_errno) {
                continue;
            }
            return PMIX_ERR_UNREACH;
        }
        cnt += (size_t) rd;
    }
    return PMIX_SUCCESS;
}

static pmix_status_t server_handshake(struct pmix_peer_t *peer, int sd)
{
    static bool warned = false;
    pmix_peer_t *pr = (pmix_peer_t *) peer;
    unsigned char nonce[NATIVE_NONCE_LEN], got[NATIVE_NONCE_LEN];
    struct sockaddr_un addr;
    char *rel = NULL, *path = NULL;
    int lsd = -1, csd = -1, n;
    unsigned char diff;
    uid_t uid;
    gid_t gid;
    uint32_t u32;
    int64_t deadline;
    pmix_status_t rc, ret;
    bool group_only;

    if (NULL == pr || NULL == pr->info || 0 > sd) {
        return PMIX_ERR_BAD_PARAM;
    }
    deadline = handshake_deadline();

    rc = get_nonce(nonce);
    if (PMIX_SUCCESS != rc) {
        goto report;
    }
    rc = make_sockdir();
    if (PMIX_SUCCESS != rc) {
        goto report;
    }
    if (0 > pmix_asprintf(&rel, "%s/auth.%lu", sockdir_rel, sock_counter++) ||
        0 > pmix_asprintf(&path, "%s/%s", sockdir_base, rel)) {
        rc = PMIX_ERR_NOMEM;
        goto report;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (sizeof(addr.sun_path) <= strlen(path)) {
        pmix_output_verbose(2, pmix_psec_base_framework.framework_output,
                            "psec: native socket path %s is too long", path);
        rc = PMIX_ERR_BAD_PARAM;
        goto report;
    }
    pmix_strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    lsd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (0 > lsd) {
        rc = PMIX_ERR_NOT_AVAILABLE;
        goto report;
    }
    (void) unlink(path);
    if (0 != bind(lsd, (struct sockaddr *) &addr, sizeof(addr)) ||
        0 != chmod(path, S_IRWXU | S_IRWXG | S_IRWXO) || 0 != listen(lsd, 1)) {
        pmix_output_verbose(2, pmix_psec_base_framework.framework_output,
                            "psec: native cannot listen on %s: %s", path,
                            strerror(pmix_socket_errno));
        rc = PMIX_ERR_NOT_AVAILABLE;
        goto report;
    }
    pmix_ptl_base_set_nonblocking(lsd);

    /* tell the peer where to go and what to bring */
    u32 = htonl(NATIVE_NONCE_LEN);
    if (PMIX_SUCCESS != (rc = pmix_ptl_base_send_blocking(sd, (char *) &u32, sizeof(uint32_t))) ||
        PMIX_SUCCESS != (rc = pmix_ptl_base_send_blocking(sd, (char *) nonce, NATIVE_NONCE_LEN)) ||
        PMIX_SUCCESS != (rc = send_string(sd, sockdir_base)) ||
        PMIX_SUCCESS != (rc = send_string(sd, rel))) {
        goto cleanup;
    }

    /* the peer connects - or gives up and says so on the TCP connection */
    while (1) {
        rc = wait_readable(lsd, sd, deadline);
        if (PMIX_SUCCESS != rc) {
            goto fail;
        }
        csd = accept(lsd, NULL, NULL);
        if (0 <= csd) {
            break;
        }
        if (EAGAIN != pmix_socket_errno && EWOULDBLOCK != pmix_socket_errno &&
            EINTR != pmix_socket_errno && ECONNABORTED != pmix_socket_errno) {
            rc = PMIX_ERR_UNREACH;
            goto fail;
        }
    }
    /* one connection is all this socket is for */
    close(lsd);
    lsd = -1;
    (void) unlink(path);

    pmix_ptl_base_set_nonblocking(csd);
    rc = read_by(csd, sd, (char *) got, NATIVE_NONCE_LEN, deadline);
    if (PMIX_SUCCESS != rc) {
        goto fail;
    }
    diff = 0;
    for (n = 0; n < NATIVE_NONCE_LEN; n++) {
        diff |= (unsigned char) (nonce[n] ^ got[n]);
    }
    if (0 != diff) {
        pmix_output_verbose(2, pmix_psec_base_framework.framework_output,
                            "psec: native local socket returned the wrong nonce");
        rc = PMIX_ERR_INVALID_CRED;
        goto cleanup;
    }
    rc = pmix_util_getid(csd, &uid, &gid);
    if (PMIX_SUCCESS != rc) {
        rc = PMIX_ERR_INVALID_CRED;
        goto cleanup;
    }
    pmix_output_verbose(2, pmix_psec_base_framework.framework_output,
                        "psec: native local socket peer is uid %lu gid %lu",
                        (unsigned long) uid, (unsigned long) gid);
    rc = settle_identity(pr, uid, gid, true, &group_only);
    goto cleanup;

fail:
    /* the peer never reached us over the local socket - say why once */
    if (!warned) {
        pmix_show_help("help-psec-native.txt", "handshake-failed", true,
                       pmix_fd_get_peer_name(sd), path,
                       (PMIX_ERR_TIMEOUT == rc) ? "the peer did not connect in time"
                                                : "the peer could not reach the socket");
        warned = true;
    }
    rc = PMIX_ERR_INVALID_CRED;
    goto cleanup;

report:
    pmix_output_verbose(2, pmix_psec_base_framework.framework_output,
                        "psec: native cannot set up the local socket check: %s",
                        PMIx_Error_string(rc));
    rc = PMIX_ERR_INVALID_CRED;

cleanup:
    if (0 <= csd) {
        close(csd);
    }
    if (0 <= lsd) {
        close(lsd);
    }
    if (NULL != path) {
        (void) unlink(path);
        free(path);
    }
    free(rel);
    /* the outcome - unless the connection is already gone */
    u32 = htonl((uint32_t) rc);
    ret = pmix_ptl_base_send_blocking(sd, (char *) &u32, sizeof(uint32_t));
    if (PMIX_SUCCESS == rc && PMIX_SUCCESS != ret) {
        rc = ret;
    }
    return rc;
}

static pmix_status_t client_handshake(struct pmix_peer_t *peer, int sd)
{
    unsigned char nonce[NATIVE_NONCE_LEN];
    struct sockaddr_un addr;
    char *base = NULL, *rel = NULL, *path = NULL;
    const char *dir;
    int usd = -1;
    uint32_t u32;
    pmix_status_t rc;
    PMIX_HIDE_UNUSED_PARAMS(peer);

    rc = pmix_ptl_base_recv_blocking(sd, (char *) &u32, sizeof(uint32_t));
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    if (NATIVE_NONCE_LEN != ntohl(u32)) {
        /* a server that could not set the check up sends its outcome
         * in place of the nonce */
        rc = (pmix_status_t) ntohl(u32);
        return (PMIX_SUCCESS == rc || 0 < rc) ? PMIX_ERR_BAD_PARAM : rc;
    }
    rc = pmix_ptl_base_recv_blocking(sd, (char *) nonce, NATIVE_NONCE_LEN);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    if (PMIX_SUCCESS != (rc = recv_string(sd, &base, PMIX_PATH_MAX)) ||
        PMIX_SUCCESS != (rc = recv_string(sd, &rel, NATIVE_MAX_NAME_LEN))) {
        goto done;
    }
    /* the socket is beneath the base directory, which we may see at a
     * place of our own */
    if ('/' == rel[0] || NULL != strstr(rel, "..")) {
        rc = PMIX_ERR_BAD_PARAM;
        goto done;
    }
    dir = (NULL != pmix_psec_native_params.socket_dir) ? pmix_psec_native_params.socket_dir : base;
    if (0 > pmix_asprintf(&path, "%s/%s", dir, rel)) {
        rc = PMIX_ERR_NOMEM;
        goto done;
    }
    pmix_output_verbose(2, pmix_psec_base_framework.framework_output,
                        "psec: native confirming identity over %s", path);

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (sizeof(addr.sun_path) <= strlen(path)) {
        pmix_show_help("help-psec-native.txt", "socket-unreachable", true, path,
                       "the path is too long for a local socket", base);
        rc = PMIX_ERR_UNREACH;
        goto done;
    }
    pmix_strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    usd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (0 > usd) {
        rc = PMIX_ERR_UNREACH;
        goto done;
    }
    if (0 != connect(usd, (struct sockaddr *) &addr, sizeof(addr))) {
        pmix_show_help("help-psec-native.txt", "socket-unreachable", true, path,
                       strerror(pmix_socket_errno), base);
        rc = PMIX_ERR_UNREACH;
        goto done;
    }
    rc = pmix_ptl_base_send_blocking(usd, (char *) nonce, NATIVE_NONCE_LEN);
    if (PMIX_SUCCESS != rc) {
        goto done;
    }
    /* hold the local socket open until the server has its answer */
    rc = pmix_ptl_base_recv_blocking(sd, (char *) &u32, sizeof(uint32_t));
    if (PMIX_SUCCESS == rc) {
        rc = (pmix_status_t) ntohl(u32);
    }

done:
    if (0 <= usd) {
        close(usd);
    }
    free(base);
    free(rel);
    free(path);
    return rc;
}
