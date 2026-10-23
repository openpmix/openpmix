/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/* A tool and a server on different hosts authenticate each other, the
 * server first - see "Between hosts" in base.h. This file holds the
 * pieces both ends share and the tool's half; the server's answer is
 * made in ptl_base_connection_hdlr.c, where the connection it belongs to
 * is handled.
 *
 * The tool's two connect paths - the blocking one in
 * pmix_ptl_base_make_connection_alts and the event-driven one - send the
 * same opening message (pmix_ptl_base_auth_request) and check the same
 * answer (pmix_ptl_base_auth_reply). Only the blocking path reads that
 * answer here (pmix_ptl_base_auth_server). */

#include "src/include/pmix_config.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#ifdef HAVE_SYS_SOCKET_H
#    include <sys/socket.h>
#endif
#ifdef HAVE_ARPA_INET_H
#    include <arpa/inet.h>
#endif

#include "src/include/pmix_globals.h"
#include "src/mca/psec/base/base.h"
#include "src/util/pmix_net.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_random_bytes.h"
#include "src/util/pmix_show_help.h"

#include "src/mca/ptl/base/base.h"

bool pmix_ptl_base_test_remote = false;
bool pmix_ptl_base_test_no_tcp_table = false;

/* what a bound credential carries: a label, whose it is, the tool's
 * nonce and the server's */
#define AUTH_LABEL     "pmix-ra1"
#define AUTH_LABEL_LEN 8
#define AUTH_BIND_LEN  (AUTH_LABEL_LEN + 1 + 2 * PMIX_PTL_AUTH_NONCE_LEN)

pmix_status_t pmix_ptl_base_auth_bind(bool server, const unsigned char *tool,
                                      const unsigned char *srv, const char *conn,
                                      pmix_byte_object_t *bind)
{
    unsigned char *b;
    size_t clen = (NULL == conn) ? 0 : strlen(conn);

    PMIX_BYTE_OBJECT_CONSTRUCT(bind);
    b = (unsigned char *) malloc(AUTH_BIND_LEN + clen);
    if (NULL == b) {
        return PMIX_ERR_NOMEM;
    }
    memcpy(b, AUTH_LABEL, AUTH_LABEL_LEN);
    b[AUTH_LABEL_LEN] = server ? 'S' : 'T';
    memcpy(b + AUTH_LABEL_LEN + 1, tool, PMIX_PTL_AUTH_NONCE_LEN);
    memcpy(b + AUTH_LABEL_LEN + 1 + PMIX_PTL_AUTH_NONCE_LEN, srv, PMIX_PTL_AUTH_NONCE_LEN);
    if (0 < clen) {
        memcpy(b + AUTH_BIND_LEN, conn, clen);
    }
    bind->bytes = (char *) b;
    bind->size = AUTH_BIND_LEN + clen;
    return PMIX_SUCCESS;
}

/* one end of a TCP connection as "addr,port" */
static bool end_text(const struct sockaddr_storage *sa, char *buf, size_t len)
{
    const struct sockaddr_in *in4 = (const struct sockaddr_in *) sa;
    const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *) sa;
    char addr[INET6_ADDRSTRLEN];
    struct in_addr v4;
    unsigned port;
    int n;

    if (AF_INET == sa->ss_family) {
        if (NULL == inet_ntop(AF_INET, &in4->sin_addr, addr, sizeof(addr))) {
            return false;
        }
        port = ntohs(in4->sin_port);
    } else if (AF_INET6 == sa->ss_family) {
        if (IN6_IS_ADDR_V4MAPPED(&in6->sin6_addr)) {
            memcpy(&v4, &in6->sin6_addr.s6_addr[12], sizeof(v4));
            if (NULL == inet_ntop(AF_INET, &v4, addr, sizeof(addr))) {
                return false;
            }
        } else if (NULL == inet_ntop(AF_INET6, &in6->sin6_addr, addr, sizeof(addr))) {
            return false;
        }
        port = ntohs(in6->sin6_port);
    } else {
        return false;
    }
    n = snprintf(buf, len, "%s,%u", addr, port);
    return 0 < n && (size_t) n < len;
}

pmix_status_t pmix_ptl_base_auth_conn(int sd, bool at_server, char conn[PMIX_PTL_AUTH_CONN_LEN])
{
    struct sockaddr_storage mine, theirs;
    pmix_socklen_t mlen = sizeof(mine), tlen = sizeof(theirs);
    char a[INET6_ADDRSTRLEN + 8], b[INET6_ADDRSTRLEN + 8];
    int n;

    memset(&mine, 0, sizeof(mine));
    memset(&theirs, 0, sizeof(theirs));
    if (0 != getsockname(sd, (struct sockaddr *) &mine, &mlen) ||
        0 != getpeername(sd, (struct sockaddr *) &theirs, &tlen) ||
        !end_text(&mine, a, sizeof(a)) || !end_text(&theirs, b, sizeof(b))) {
        return PMIX_ERR_UNREACH;
    }
    n = snprintf(conn, PMIX_PTL_AUTH_CONN_LEN, "%s;%s", at_server ? b : a, at_server ? a : b);
    if (0 > n || PMIX_PTL_AUTH_CONN_LEN <= n) {
        return PMIX_ERR_BAD_PARAM;
    }
    return PMIX_SUCCESS;
}

bool pmix_ptl_base_tcp_peer_remote(const struct sockaddr_storage *addr)
{
    return pmix_ptl_base_test_remote || !pmix_ptl_base_addr_on_host(addr);
}

bool pmix_ptl_base_tcp_peer_held(const struct sockaddr_storage *addr)
{
    return !pmix_ptl_base_test_remote &&
           (pmix_ptl_base_addr_on_host(addr) || pmix_ptl_base_addr_held(addr));
}

bool pmix_ptl_base_server_remote(pmix_peer_t *peer)
{
    struct sockaddr_storage sa;
    pmix_socklen_t len = sizeof(sa);

    /* the local transport reaches only a server on this host */
    if (NULL != peer->transport && NULL != peer->transport->peer_identity) {
        return false;
    }
    memset(&sa, 0, sizeof(sa));
    if (0 != getpeername(peer->sd, (struct sockaddr *) &sa, &len)) {
        /* a connected socket that cannot say where it goes is not taken to
         * be on this host */
        return true;
    }
    return pmix_ptl_base_tcp_peer_remote(&sa);
}

/* where the server at the far end of this socket is, for a message */
static void server_addr(int sd, char *buf, size_t len)
{
    struct sockaddr_storage sa;
    pmix_socklen_t salen = sizeof(sa);

    memset(&sa, 0, sizeof(sa));
    if (0 == getpeername(sd, (struct sockaddr *) &sa, &salen)) {
        snprintf(buf, len, "%s:%d", pmix_net_get_hostname((struct sockaddr *) &sa),
                 pmix_net_get_port((struct sockaddr *) &sa));
    } else {
        snprintf(buf, len, "(unknown address)");
    }
}

pmix_psec_module_t *pmix_ptl_base_auth_module(pmix_peer_t *peer)
{
    pmix_psec_module_t *cur, *mod;
    char *ours, where[128];

    if (NULL == peer->nptr) {
        return NULL;
    }
    cur = (NULL != peer->nptr->compat.psec) ? peer->nptr->compat.psec
                                            : pmix_globals.mypeer->nptr->compat.psec;
    /* the one chosen for the server, if it can - else, of those the
     * server lists, the highest of ours that can */
    if (!pmix_psec_base_remote_capable(cur) && NULL != peer->nptr->compat.rpsec) {
        cur = peer->nptr->compat.rpsec;
    }
    mod = pmix_psec_base_remote_module(cur);
    if (NULL == mod) {
        server_addr(peer->sd, where, sizeof(where));
        ours = pmix_psec_base_get_available_modules();
        pmix_show_help("help-ptl-base.txt", "remote-no-psec-tool", true, pmix_globals.hostname,
                       where, (NULL == ours) ? "none" : ours);
        free(ours);
        return NULL;
    }
    peer->nptr->compat.psec = mod;
    return mod;
}

pmix_status_t pmix_ptl_base_auth_request(const char *psec, const unsigned char *nonce,
                                         bool bound, char **msgout, size_t *len)
{
    pmix_ptl_hdr_t hdr;
    char *msg;
    size_t csize, sdsize;
    uint8_t flag = PMIX_REMOTE_AUTH;

    /* the mechanism, an empty credential, the flag, our version, our
     * nonce, our user, and whether we bind the connection's two ends too
     * - in the connect-ack's framing, so the server reads it the way it
     * reads one */
    sdsize = strlen(psec) + 1 + sizeof(uint32_t) + 1 + strlen(PMIX_VERSION) + 1 +
             sizeof(uint32_t) + PMIX_PTL_AUTH_NONCE_LEN + sizeof(uint32_t) + 1;
    memset(&hdr, 0, sizeof(hdr));
    hdr.pindex = -1;
    hdr.tag = UINT32_MAX;
    hdr.nbytes = sdsize;
    msg = (char *) calloc(1, sizeof(hdr) + sdsize);
    if (NULL == msg) {
        return PMIX_ERR_NOMEM;
    }
    memcpy(msg, &hdr, sizeof(hdr));
    csize = sizeof(hdr);
    PMIX_PTL_PUT_STRING(psec);
    PMIX_PTL_PUT_U32(0);
    PMIX_PTL_PUT_U8(flag);
    PMIX_PTL_PUT_STRING(PMIX_VERSION);
    PMIX_PTL_PUT_U32(PMIX_PTL_AUTH_NONCE_LEN);
    PMIX_PTL_PUT_BLOB(nonce, PMIX_PTL_AUTH_NONCE_LEN);
    PMIX_PTL_PUT_U32(geteuid());
    flag = bound ? 1 : 0;
    PMIX_PTL_PUT_U8(flag);
    *msgout = msg;
    *len = csize;
    return PMIX_SUCCESS;
}

pmix_status_t pmix_ptl_base_auth_reply(pmix_peer_t *peer, bool given, uid_t uid, gid_t gid,
                                       bool local, const unsigned char *tool,
                                       const unsigned char *srv,
                                       const pmix_byte_object_t *cred, pmix_byte_object_t *bind,
                                       uid_t *srvuid)
{
    pmix_psec_module_t *psec;
    pmix_byte_object_t sbind;
    uid_t kuid;
    gid_t kgid;
    char where[128], conn[PMIX_PTL_AUTH_CONN_LEN];
    pmix_status_t rc;

    PMIX_BYTE_OBJECT_CONSTRUCT(bind);
    psec = (NULL == peer->nptr) ? NULL : peer->nptr->compat.psec;
    if (!pmix_psec_base_remote_capable(psec)) {
        return PMIX_ERR_NOT_SUPPORTED;
    }
    /* a server on this host binds the connection's two ends as well */
    if (local) {
        rc = pmix_ptl_base_auth_conn(peer->sd, false, conn);
        if (PMIX_SUCCESS != rc) {
            return rc;
        }
    }

    /* the server's credential, made as a server for our nonce and its */
    rc = pmix_ptl_base_auth_bind(true, tool, srv, local ? conn : NULL, &sbind);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    rc = psec->validate_bound(&sbind, true, cred, &kuid, &kgid);
    PMIX_BYTE_OBJECT_DESTRUCT(&sbind);
    if (PMIX_SUCCESS != rc) {
        server_addr(peer->sd, where, sizeof(where));
        pmix_show_help("help-ptl-base.txt", "remote-server-unproven", true,
                       pmix_globals.hostname, where, psec->name, PMIx_Error_string(rc));
        return PMIX_ERR_INVALID_CRED;
    }

    /* and it must be a server we may talk to */
    rc = pmix_ptl_base_check_ids(kuid, kgid, given, uid, gid, true);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                        "ptl:base: server at the far end of socket %d proved it is uid %lu "
                        "gid %lu with %s",
                        peer->sd, (unsigned long) kuid, (unsigned long) kgid, psec->name);

    /* our credential is bound to the same two nonces, as a tool's, and is
     * for that server's user */
    *srvuid = kuid;
    return pmix_ptl_base_auth_bind(false, tool, srv, local ? conn : NULL, bind);
}

void pmix_ptl_base_auth_refused(pmix_peer_t *peer, const char *psec, bool local,
                                pmix_status_t why)
{
    char where[128];

    server_addr(peer->sd, where, sizeof(where));
    if (local) {
        pmix_show_help("help-ptl-base.txt", "local-auth-refused", true, pmix_globals.hostname,
                       where, psec, PMIx_Error_string(why));
    } else {
        pmix_show_help("help-ptl-base.txt", "remote-auth-refused", true, pmix_globals.hostname,
                       where, psec, PMIx_Error_string(why));
    }
}

pmix_status_t pmix_ptl_base_auth_server(pmix_peer_t *peer, bool given, uid_t uid, gid_t gid,
                                        bool local, pmix_byte_object_t *bind, uid_t *srvuid)
{
    unsigned char tool[PMIX_PTL_AUTH_NONCE_LEN], srv[PMIX_PTL_AUTH_NONCE_LEN];
    pmix_psec_module_t *psec;
    pmix_byte_object_t cred;
    struct timeval save;
    pmix_socklen_t sz = sizeof(save);
    bool sockopt = true;
    char *msg = NULL;
    size_t len;
    uint32_t u32;
    pmix_status_t rc, reply;

    PMIX_BYTE_OBJECT_CONSTRUCT(bind);
    PMIX_BYTE_OBJECT_CONSTRUCT(&cred);
    if (NULL == peer->nptr) {
        return PMIX_ERR_INIT;
    }
    psec = pmix_ptl_base_auth_module(peer);
    if (NULL == psec) {
        return PMIX_ERR_NOT_SUPPORTED;
    }
    rc = pmix_util_random_bytes(tool, sizeof(tool));
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    rc = pmix_ptl_base_auth_request(psec->name, tool, local, &msg, &len);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    rc = pmix_ptl_base_send_blocking(peer->sd, msg, len);
    free(msg);
    if (PMIX_SUCCESS != rc) {
        return PMIX_ERR_UNREACH;
    }

    /* its answer: a status, then its nonce and its credential */
    rc = pmix_ptl_base_set_timeout(peer, &save, &sz, &sockopt);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    rc = pmix_ptl_base_recv_blocking(peer->sd, (char *) &u32, sizeof(u32));
    if (PMIX_SUCCESS == rc) {
        reply = pmix_ptl_base_server_status(ntohl(u32));
        if (PMIX_SUCCESS != reply) {
            pmix_ptl_base_auth_refused(peer, psec->name, local, reply);
            rc = reply;
        }
    } else {
        /* closed, or silent past the handshake wait */
        pmix_ptl_base_auth_refused(peer, psec->name, local, rc);
    }
    if (PMIX_SUCCESS == rc) {
        rc = pmix_ptl_base_recv_blocking(peer->sd, (char *) srv, sizeof(srv));
    }
    if (PMIX_SUCCESS == rc) {
        rc = pmix_ptl_base_recv_blocking(peer->sd, (char *) &u32, sizeof(u32));
    }
    if (PMIX_SUCCESS == rc) {
        u32 = ntohl(u32);
        if (0 == u32 || PMIX_MAX_CRED_SIZE < u32) {
            rc = PMIX_ERR_BAD_PARAM;
        } else if (NULL == (cred.bytes = (char *) malloc(u32))) {
            rc = PMIX_ERR_NOMEM;
        } else {
            cred.size = u32;
            rc = pmix_ptl_base_recv_blocking(peer->sd, cred.bytes, cred.size);
        }
    }
    if (sockopt && 0 != setsockopt(peer->sd, SOL_SOCKET, SO_RCVTIMEO, &save, sz)) {
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "pmix: could not reset setsockopt SO_RCVTIMEO");
    }
    if (PMIX_SUCCESS == rc) {
        rc = pmix_ptl_base_auth_reply(peer, given, uid, gid, local, tool, srv, &cred, bind,
                                      srvuid);
    }
    PMIX_BYTE_OBJECT_DESTRUCT(&cred);
    return rc;
}
