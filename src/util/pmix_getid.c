/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2013 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2007      Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * Recover the credentials of the process on the far end of a socket.
 */

#include "src/include/pmix_config.h"
#include "pmix_common.h"
#include "src/include/pmix_socket_errno.h"

#include <string.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif
#ifdef HAVE_SYS_TYPES_H
#    include <sys/types.h>
#endif
/* SO_PEERCRED is what the route below is selected on, so the header
 * that spells it has to be in scope here and not merely reachable
 * through some other include's includes: were it to go missing, this
 * file would quietly compile the getpeereid() route on a platform that
 * wanted the other one, rather than say anything about it */
#include <sys/socket.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef HAVE_NETINET_IN_H
#    include <netinet/in.h>
#endif

/* The kernel interfaces pmix_util_getid_tcp() asks. Each platform has
 * its own, and none of them is portable: Linux answers through the
 * socket-diagnostics netlink family, with the text table in
 * /proc/net/tcp{,6} to fall back on, and macOS through the pcblist64
 * sysctl that netstat reads. */
#if defined(__linux__) && defined(HAVE_LINUX_NETLINK_H) && \
    defined(HAVE_LINUX_SOCK_DIAG_H) && defined(HAVE_LINUX_INET_DIAG_H)
#    define PMIX_HAVE_SOCK_DIAG 1
#    include <netinet/tcp.h>
#    include <linux/netlink.h>
#    include <linux/sock_diag.h>
#    include <linux/inet_diag.h>
#else
#    define PMIX_HAVE_SOCK_DIAG 0
#endif
#if defined(__linux__)
#    define PMIX_HAVE_PROC_NET_TCP 1
#else
#    define PMIX_HAVE_PROC_NET_TCP 0
#endif
#if defined(__APPLE__) && defined(HAVE_SYS_SYSCTL_H)
#    define PMIX_HAVE_PCBLIST64 1
#    include <sys/socketvar.h>
#    include <sys/sysctl.h>
#    include <netinet/in_pcb.h>
#    include <netinet/tcp.h>
#    include <netinet/tcp_timer.h>
#    include <netinet/tcp_var.h>
#    include <netinet/tcp_fsm.h>
#else
#    define PMIX_HAVE_PCBLIST64 0
#endif

#include "src/include/pmix_globals.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_output.h"

#include "src/util/pmix_getid.h"

/* Which shape of peer credential this platform offers is decided once,
 * here, because SO_PEERCRED on its own does not answer it: the struct
 * getsockopt fills is `struct sockpeercred` on OpenBSD and `struct
 * ucred` elsewhere, and the latter spells its members either uid/gid or
 * cr_uid/cr_gid. Only a build in which configure recognized one of those
 * three shapes can take the getsockopt route.
 *
 * Deciding it in one place is the point. The declarations used to sit
 * behind a bare SO_PEERCRED test while the code reading them sat behind
 * the full condition, so a platform that defines SO_PEERCRED but whose
 * member probe did not fire - a cross-compile, a sysroot, any configure
 * test that failed for reasons of its own - did not fall back to
 * getpeereid() at all. It failed to build: on an incomplete struct
 * ucred, and on two variables nothing went on to use. */
#if defined(SO_PEERCRED) && defined(HAVE_STRUCT_SOCKPEERCRED_UID)
#    define PMIX_HAVE_PEERCRED 1
#    define PMIX_PEERCRED_T    struct sockpeercred
#    define PMIX_PEERCRED_UID  uid
#    define PMIX_PEERCRED_GID  gid
#elif defined(SO_PEERCRED) && defined(HAVE_STRUCT_UCRED_UID)
#    define PMIX_HAVE_PEERCRED 1
#    define PMIX_PEERCRED_T    struct ucred
#    define PMIX_PEERCRED_UID  uid
#    define PMIX_PEERCRED_GID  gid
#elif defined(SO_PEERCRED) && defined(HAVE_STRUCT_UCRED_CR_UID)
#    define PMIX_HAVE_PEERCRED 1
#    define PMIX_PEERCRED_T    struct ucred
#    define PMIX_PEERCRED_UID  cr_uid
#    define PMIX_PEERCRED_GID  cr_gid
#else
#    define PMIX_HAVE_PEERCRED 0
#endif

pmix_status_t pmix_util_getid(int sd, uid_t *uid, gid_t *gid)
{
#if PMIX_HAVE_PEERCRED
    PMIX_PEERCRED_T cred;
    socklen_t crlen = sizeof(cred);

    /* Ignore any credential the peer sent us and ask the kernel about
     * the socket instead. */
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "getid: checking getsockopt for peer credentials");
    if (getsockopt(sd, SOL_SOCKET, SO_PEERCRED, &cred, &crlen) < 0) {
        pmix_output_verbose(2, pmix_globals.debug_output,
                            "getid: getsockopt SO_PEERCRED failed: %s",
                            strerror(pmix_socket_errno));
        return PMIX_ERR_INVALID_CRED;
    }
    /* getsockopt reports back how much it actually wrote. Anything short
     * of the whole struct leaves the rest of it holding whatever was on
     * the stack, and these two fields are what a caller decides an
     * identity on - so refuse the answer rather than hand back a uid
     * that was never written. */
    if (crlen < sizeof(cred)) {
        pmix_output_verbose(2, pmix_globals.debug_output,
                            "getid: getsockopt SO_PEERCRED returned %u bytes, expected %u",
                            (unsigned) crlen, (unsigned) sizeof(cred));
        return PMIX_ERR_INVALID_CRED;
    }
    *uid = cred.PMIX_PEERCRED_UID;
    *gid = cred.PMIX_PEERCRED_GID;

#elif defined(HAVE_GETPEEREID)
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "getid: checking getpeereid for peer credentials");
    if (0 != getpeereid(sd, uid, gid)) {
        pmix_output_verbose(2, pmix_globals.debug_output, "getid: getpeereid failed: %s",
                            strerror(pmix_socket_errno));
        return PMIX_ERR_INVALID_CRED;
    }
#else
    PMIX_HIDE_UNUSED_PARAMS(sd, uid, gid);
    return PMIX_ERR_NOT_SUPPORTED;
#endif

    return PMIX_SUCCESS;
}

/* ------------------------------------------------------------------ *
 * pmix_util_getid_tcp()
 *
 * A TCP socket carries no credentials of its own - SO_PEERCRED on one
 * answers with success and an overflow uid on Linux, and getpeereid()
 * refuses it outright - so the peer's identity has to be looked up
 * instead. The kernel records the uid that created every socket, and
 * the peer's socket is findable by the four values that identify the
 * connection: its own address and port, which are our peer address, and
 * its remote address and port, which are ours.
 *
 * Two rules apply to the entry that is used:
 *
 * - It must be ESTABLISHED. An entry in TIME_WAIT or FIN_WAIT carries no
 *   owner, and the kernel reports uid 0 for it.
 * - It must match all four values. Linux's exact lookup falls back to a
 *   listening socket when no connection matches.
 * ------------------------------------------------------------------ */

typedef struct {
    int family;                     /* AF_INET or AF_INET6, after unmapping */
    struct in_addr peer4, local4;   /* valid for AF_INET */
    struct in6_addr peer6, local6;  /* always valid: v4-mapped for AF_INET */
    uint16_t peer_port, local_port; /* network byte order */
} tcp_tuple_t;

static void map_v4(struct in6_addr *dst, const struct in_addr *src)
{
    memset(dst, 0, sizeof(*dst));
    dst->s6_addr[10] = 0xff;
    dst->s6_addr[11] = 0xff;
    memcpy(&dst->s6_addr[12], src, sizeof(*src));
}

/* the connection's four values, with an IPv4-mapped IPv6 pair - what a
 * dual-stack listener reports for an IPv4 peer - unwrapped to IPv4 */
static pmix_status_t get_tuple(int sd, tcp_tuple_t *t)
{
    struct sockaddr_storage local, peer;
    socklen_t llen = sizeof(local), plen = sizeof(peer);
    const struct sockaddr_in6 *l6, *p6;
    const struct sockaddr_in *l4, *p4;

    memset(t, 0, sizeof(*t));
    if (0 > sd) {
        return PMIX_ERR_BAD_PARAM;
    }
    if (0 != getsockname(sd, (struct sockaddr *) &local, &llen) ||
        0 != getpeername(sd, (struct sockaddr *) &peer, &plen) ||
        local.ss_family != peer.ss_family) {
        return PMIX_ERR_BAD_PARAM;
    }

    if (AF_INET == local.ss_family) {
        l4 = (const struct sockaddr_in *) &local;
        p4 = (const struct sockaddr_in *) &peer;
        t->family = AF_INET;
        t->local4 = l4->sin_addr;
        t->peer4 = p4->sin_addr;
        t->local_port = l4->sin_port;
        t->peer_port = p4->sin_port;
        map_v4(&t->local6, &t->local4);
        map_v4(&t->peer6, &t->peer4);
        return PMIX_SUCCESS;
    }

    if (AF_INET6 == local.ss_family) {
        l6 = (const struct sockaddr_in6 *) &local;
        p6 = (const struct sockaddr_in6 *) &peer;
        t->local6 = l6->sin6_addr;
        t->peer6 = p6->sin6_addr;
        t->local_port = l6->sin6_port;
        t->peer_port = p6->sin6_port;
        if (IN6_IS_ADDR_V4MAPPED(&t->local6) && IN6_IS_ADDR_V4MAPPED(&t->peer6)) {
            t->family = AF_INET;
            memcpy(&t->local4, &t->local6.s6_addr[12], sizeof(t->local4));
            memcpy(&t->peer4, &t->peer6.s6_addr[12], sizeof(t->peer4));
        } else {
            t->family = AF_INET6;
        }
        return PMIX_SUCCESS;
    }

    /* AF_UNIX and anything else is not ours to answer */
    return PMIX_ERR_BAD_PARAM;
}

#if PMIX_HAVE_SOCK_DIAG
/* compare a 16-byte address field of a diag message against one of
 * ours: an AF_INET entry carries its address in the first four bytes, an
 * AF_INET6 one - including an IPv6 socket connected over IPv4, which is
 * reported with mapped addresses - in all sixteen */
static bool diag_addr_matches(int family, const __be32 *field, const tcp_tuple_t *t,
                              bool peer)
{
    if (AF_INET == family) {
        return AF_INET == t->family &&
               0 == memcmp(field, peer ? &t->peer4 : &t->local4, sizeof(struct in_addr));
    }
    return 0 == memcmp(field, peer ? &t->peer6 : &t->local6, sizeof(struct in6_addr));
}

static pmix_status_t lookup_sock_diag(const tcp_tuple_t *t, uid_t *uid)
{
    struct sockaddr_nl sa;
    struct {
        struct nlmsghdr nlh;
        struct inet_diag_req_v2 req;
    } msg;
    union {
        struct nlmsghdr nlh;
        char buf[8192];
    } rsp;
    struct nlmsghdr *nlh;
    struct nlmsgerr *err;
    struct inet_diag_msg *r;
    ssize_t n;
    size_t off;
    int nl;
    pmix_status_t rc = PMIX_ERR_NOT_FOUND;

    nl = socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC, NETLINK_SOCK_DIAG);
    if (0 > nl) {
        return PMIX_ERR_NOT_SUPPORTED;
    }
    memset(&sa, 0, sizeof(sa));
    sa.nl_family = AF_NETLINK;

    /* an exact lookup, not a dump: the kernel finds the one socket whose
     * own end is the peer's address and port and whose far end is ours */
    memset(&msg, 0, sizeof(msg));
    msg.nlh.nlmsg_len = sizeof(msg);
    msg.nlh.nlmsg_type = SOCK_DIAG_BY_FAMILY;
    msg.nlh.nlmsg_flags = NLM_F_REQUEST;
    msg.nlh.nlmsg_seq = 1;
    msg.req.sdiag_family = t->family;
    msg.req.sdiag_protocol = IPPROTO_TCP;
    msg.req.idiag_states = ~0U;
    msg.req.id.idiag_sport = t->peer_port;
    msg.req.id.idiag_dport = t->local_port;
    if (AF_INET == t->family) {
        memcpy(msg.req.id.idiag_src, &t->peer4, sizeof(t->peer4));
        memcpy(msg.req.id.idiag_dst, &t->local4, sizeof(t->local4));
    } else {
        memcpy(msg.req.id.idiag_src, &t->peer6, sizeof(t->peer6));
        memcpy(msg.req.id.idiag_dst, &t->local6, sizeof(t->local6));
    }
    msg.req.id.idiag_cookie[0] = INET_DIAG_NOCOOKIE;
    msg.req.id.idiag_cookie[1] = INET_DIAG_NOCOOKIE;

    if ((ssize_t) sizeof(msg) != sendto(nl, &msg, sizeof(msg), 0,
                                        (struct sockaddr *) &sa, sizeof(sa))) {
        close(nl);
        return PMIX_ERR_NOT_SUPPORTED;
    }
    /* the kernel answers a request to itself before sendto() returns, so
     * the reply is already queued - never wait for one that is not */
    n = recv(nl, &rsp, sizeof(rsp), MSG_DONTWAIT);
    close(nl);
    if (0 >= n) {
        return PMIX_ERR_NOT_SUPPORTED;
    }

    /* walked by hand rather than with NLMSG_OK/NLMSG_NEXT, whose mix of
     * signed and unsigned lengths does not survive -Wsign-compare */
    for (off = 0; off + sizeof(struct nlmsghdr) <= (size_t) n;
         off += NLMSG_ALIGN(nlh->nlmsg_len)) {
        nlh = (struct nlmsghdr *) (rsp.buf + off);
        if (nlh->nlmsg_len < sizeof(struct nlmsghdr) || off + nlh->nlmsg_len > (size_t) n) {
            break;
        }
        if (1 != nlh->nlmsg_seq) {
            continue;
        }
        if (NLMSG_ERROR == nlh->nlmsg_type) {
            if (nlh->nlmsg_len < NLMSG_LENGTH(sizeof(*err))) {
                return PMIX_ERR_NOT_SUPPORTED;
            }
            err = (struct nlmsgerr *) NLMSG_DATA(nlh);
            /* ENOENT is an answer: there is no such connection here. Any
             * other error - the diag module absent, the request filtered
             * by a seccomp policy - says only that this route is closed */
            return (-ENOENT == err->error) ? PMIX_ERR_NOT_FOUND : PMIX_ERR_NOT_SUPPORTED;
        }
        if (SOCK_DIAG_BY_FAMILY != nlh->nlmsg_type ||
            nlh->nlmsg_len < NLMSG_LENGTH(sizeof(*r))) {
            continue;
        }
        r = (struct inet_diag_msg *) NLMSG_DATA(nlh);
        if (TCP_ESTABLISHED != r->idiag_state ||
            r->id.idiag_sport != t->peer_port ||
            r->id.idiag_dport != t->local_port ||
            !diag_addr_matches(r->idiag_family, r->id.idiag_src, t, true) ||
            !diag_addr_matches(r->idiag_family, r->id.idiag_dst, t, false)) {
            continue;
        }
        *uid = (uid_t) r->idiag_uid;
        rc = PMIX_SUCCESS;
        break;
    }
    return rc;
}
#endif /* PMIX_HAVE_SOCK_DIAG */

#if PMIX_HAVE_PROC_NET_TCP
/* /proc/net/tcp prints each address as the raw network-order word(s) read
 * as host integers, "%08X" apiece, and each port in host order */
static bool proc_addr_matches(const char *hex, const void *addr, size_t len)
{
    uint32_t words[4];
    char chunk[9];
    size_t i, nwords = len / sizeof(uint32_t);

    if (strlen(hex) != 8 * nwords) {
        return false;
    }
    for (i = 0; i < nwords; i++) {
        memcpy(chunk, hex + 8 * i, 8);
        chunk[8] = '\0';
        words[i] = (uint32_t) strtoul(chunk, NULL, 16);
    }
    return 0 == memcmp(words, addr, len);
}

static pmix_status_t scan_proc_net(const char *path, bool v6, const tcp_tuple_t *t, uid_t *uid)
{
    char line[1024], laddr[33], raddr[33];
    unsigned int lport, rport, st;
    unsigned long owner;
    const void *pa, *la;
    size_t alen;
    FILE *fp;
    int fd;
    pmix_status_t rc = PMIX_ERR_NOT_FOUND;

    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (0 > fd) {
        return PMIX_ERR_NOT_SUPPORTED;
    }
    fp = fdopen(fd, "r");
    if (NULL == fp) {
        close(fd);
        return PMIX_ERR_NOT_SUPPORTED;
    }
    if (v6) {
        pa = &t->peer6;
        la = &t->local6;
        alen = sizeof(struct in6_addr);
    } else {
        pa = &t->peer4;
        la = &t->local4;
        alen = sizeof(struct in_addr);
    }

    while (NULL != fgets(line, sizeof(line), fp)) {
        if (NULL == strchr(line, '\n') && !feof(fp)) {
            /* longer than any real entry - skip the rest of it so the
             * next read starts on a line boundary */
            int c;
            while (EOF != (c = fgetc(fp)) && '\n' != c) {
                continue;
            }
            continue;
        }
        if (6 != sscanf(line, " %*u: %32[0-9A-Fa-f]:%x %32[0-9A-Fa-f]:%x %x %*x:%*x %*x:%*x %*x %lu",
                        laddr, &lport, raddr, &rport, &st, &owner)) {
            continue; /* the header, or something we do not recognize */
        }
        if (0x01 != st ||                           /* TCP_ESTABLISHED */
            lport != ntohs(t->peer_port) || rport != ntohs(t->local_port) ||
            !proc_addr_matches(laddr, pa, alen) || !proc_addr_matches(raddr, la, alen)) {
            continue;
        }
        *uid = (uid_t) owner;
        rc = PMIX_SUCCESS;
        break;
    }
    fclose(fp);
    return rc;
}

static pmix_status_t lookup_proc_net(const tcp_tuple_t *t, uid_t *uid)
{
    pmix_status_t rc;
    bool answered = false;

    /* an IPv4 connection may be held by an AF_INET socket, listed in
     * tcp, or by an AF_INET6 one using mapped addresses, listed in tcp6 */
    if (AF_INET == t->family) {
        rc = scan_proc_net("/proc/net/tcp", false, t, uid);
        if (PMIX_SUCCESS == rc) {
            return rc;
        }
        answered = (PMIX_ERR_NOT_FOUND == rc);
    }
    rc = scan_proc_net("/proc/net/tcp6", true, t, uid);
    if (PMIX_SUCCESS == rc) {
        return rc;
    }
    /* a table that was read and did not list the connection is an
     * answer; a table that could not be read is not */
    if (answered || PMIX_ERR_NOT_FOUND == rc) {
        return PMIX_ERR_NOT_FOUND;
    }
    return PMIX_ERR_NOT_SUPPORTED;
}
#endif /* PMIX_HAVE_PROC_NET_TCP */

#if PMIX_HAVE_PCBLIST64
static bool pcb_matches(const struct xtcpcb64 *tp, const tcp_tuple_t *t)
{
    const struct xinpcb64 *inp = &tp->xt_inpcb;

    if (IPPROTO_TCP != inp->xi_socket.xso_protocol ||
        TCPS_ESTABLISHED != tp->t_state ||
        inp->inp_lport != t->peer_port || inp->inp_fport != t->local_port) {
        return false;
    }
    /* an IPv6 socket connected over IPv4 is flagged IPv4, and keeps the
     * addresses where an IPv4 one does */
    if (INP_IPV4 & inp->inp_vflag) {
        return AF_INET == t->family &&
               inp->inp_dependladdr.inp46_local.ia46_addr4.s_addr == t->peer4.s_addr &&
               inp->inp_dependfaddr.inp46_foreign.ia46_addr4.s_addr == t->local4.s_addr;
    }
    if (INP_IPV6 & inp->inp_vflag) {
        return 0 == memcmp(&inp->inp_dependladdr.inp6_local, &t->peer6, sizeof(t->peer6)) &&
               0 == memcmp(&inp->inp_dependfaddr.inp6_foreign, &t->local6, sizeof(t->local6));
    }
    return false;
}

static pmix_status_t lookup_pcblist64(const tcp_tuple_t *t, uid_t *uid)
{
    struct xinpgen gen;
    struct xtcpcb64 tp;
    uint32_t reclen;
    size_t len = 0, off;
    char *buf = NULL;
    int tries;
    pmix_status_t rc = PMIX_ERR_NOT_FOUND;

    /* the table can grow between asking its size and reading it, so ask
     * for headroom and try again if that was not enough */
    for (tries = 0; tries < 5; tries++) {
        if (0 != sysctlbyname("net.inet.tcp.pcblist64", NULL, &len, NULL, 0)) {
            return PMIX_ERR_NOT_SUPPORTED;
        }
        len += len / 8 + 4096;
        buf = (char *) malloc(len);
        if (NULL == buf) {
            return PMIX_ERR_NOMEM;
        }
        if (0 == sysctlbyname("net.inet.tcp.pcblist64", buf, &len, NULL, 0)) {
            break;
        }
        free(buf);
        buf = NULL;
        if (ENOMEM != errno) {
            return PMIX_ERR_NOT_SUPPORTED;
        }
    }
    if (NULL == buf) {
        return PMIX_ERR_NOT_SUPPORTED;
    }

    /* a leading xinpgen, one xtcpcb64 per connection, and a trailing
     * xinpgen - each opening with its own length. Copy each record out
     * rather than cast into the buffer: nothing promises its alignment */
    if (len < sizeof(gen)) {
        free(buf);
        return PMIX_ERR_NOT_SUPPORTED;
    }
    memcpy(&gen, buf, sizeof(gen));
    for (off = gen.xig_len; off + sizeof(reclen) <= len; off += reclen) {
        memcpy(&reclen, buf + off, sizeof(reclen));
        if (reclen <= sizeof(struct xinpgen)) {
            break; /* the trailer */
        }
        if (reclen < sizeof(tp) || off + reclen > len) {
            rc = PMIX_ERR_NOT_SUPPORTED; /* not a layout we understand */
            break;
        }
        memcpy(&tp, buf + off, sizeof(tp));
        if (pcb_matches(&tp, t)) {
            *uid = tp.xt_inpcb.xi_socket.so_uid;
            rc = PMIX_SUCCESS;
            break;
        }
    }
    free(buf);
    return rc;
}
#endif /* PMIX_HAVE_PCBLIST64 */

pmix_status_t pmix_util_getid_tcp(int sd, uid_t *uid)
{
    tcp_tuple_t t;
    pmix_status_t rc;
    uid_t owner = (uid_t) -1;

    rc = get_tuple(sd, &t);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }

#if PMIX_HAVE_SOCK_DIAG
    rc = lookup_sock_diag(&t, &owner);
    if (PMIX_ERR_NOT_SUPPORTED == rc) {
        pmix_output_verbose(2, pmix_globals.debug_output,
                            "getid: sock_diag unavailable - scanning /proc/net");
        rc = lookup_proc_net(&t, &owner);
    }
#elif PMIX_HAVE_PROC_NET_TCP
    rc = lookup_proc_net(&t, &owner);
#elif PMIX_HAVE_PCBLIST64
    rc = lookup_pcblist64(&t, &owner);
#else
    rc = PMIX_ERR_NOT_SUPPORTED;
#endif

    if (PMIX_SUCCESS == rc) {
        *uid = owner;
        pmix_output_verbose(2, pmix_globals.debug_output,
                            "getid: tcp peer of socket %d is owned by uid %lu", sd,
                            (unsigned long) owner);
    } else {
        pmix_output_verbose(2, pmix_globals.debug_output,
                            "getid: owner of tcp peer of socket %d unknown: %s", sd,
                            PMIx_Error_string(rc));
    }
    return rc;
}
