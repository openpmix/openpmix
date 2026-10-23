/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Connections from tools on other hosts.
 *
 * A server takes them only in a build configured --enable-remote-tools,
 * and only when its host asks with PMIX_SERVER_REMOTE_CONNECTIONS. In a
 * build without them, a host that asks fails its init, and the TCP
 * listener is on a loopback interface or nowhere: a server whose
 * directives leave it no loopback interface fails its init rather than
 * taking a public one. In a build with them, a host that asks fails its
 * init when no security mechanism can authenticate across hosts.
 *
 * Which peers are on this host: a loopback address, and the address of
 * one of this host's interfaces, either way an IPv4 address arrives. Any
 * other address is not, and neither is a local socket's.
 *
 * Which mechanisms authenticate across hosts: munge and ssl do, native
 * does not.
 *
 * Each case runs in a child forked before any PMIx call.
 */

#include "src/include/pmix_config.h"

#include "include/pmix_server.h"
#include "src/include/pmix_globals.h"
#include "src/mca/psec/base/base.h"
#include "src/mca/ptl/base/base.h"
#include "src/util/pmix_if.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHILD_PASS 0
#define CHILD_FAIL 1
#define CHILD_SKIP 77

static int npass = 0;
static int nfail = 0;

static void report(const char *name, int passed, const char *detail)
{
    if (passed) {
        fprintf(stdout, "  PASS: %s\n", name);
        ++npass;
    } else {
        fprintf(stdout, "  FAIL: %s (%s)\n", name, detail);
        ++nfail;
    }
}

static pmix_server_module_t mymodule = {0};

/* a check made inside a child: print it, and fold it into the exit status */
static int check(const char *what, bool ok)
{
    fprintf(stdout, "    %s: %s\n", what, ok ? "yes" : "NO");
    return ok ? CHILD_PASS : CHILD_FAIL;
}

static void load4(struct sockaddr_storage *ss, const char *addr)
{
    struct sockaddr_in *in = (struct sockaddr_in *) ss;

    memset(ss, 0, sizeof(*ss));
    in->sin_family = AF_INET;
    inet_pton(AF_INET, addr, &in->sin_addr);
}

static void load6(struct sockaddr_storage *ss, const char *addr)
{
    struct sockaddr_in6 *in = (struct sockaddr_in6 *) ss;

    memset(ss, 0, sizeof(*ss));
    in->sin6_family = AF_INET6;
    inet_pton(AF_INET6, addr, &in->sin6_addr);
}

/* the IPv4 address of an interface of this host that is not loopback, and
 * its name, if there is one */
static bool public_if(char *addr, size_t alen, char *name, size_t nlen)
{
    struct ifaddrs *ifs, *ifa;
    bool found = false;

    if (0 != getifaddrs(&ifs)) {
        return false;
    }
    for (ifa = ifs; NULL != ifa && !found; ifa = ifa->ifa_next) {
        if (NULL == ifa->ifa_addr || AF_INET != ifa->ifa_addr->sa_family ||
            (ifa->ifa_flags & IFF_LOOPBACK) || !(ifa->ifa_flags & IFF_UP)) {
            continue;
        }
        inet_ntop(AF_INET, &((struct sockaddr_in *) ifa->ifa_addr)->sin_addr, addr, alen);
        snprintf(name, nlen, "%s", ifa->ifa_name);
        found = true;
    }
    freeifaddrs(ifs);
    return found;
}

/* every address an interface of this host holds now, by the kernel's
 * list - which includes some the interface list made at init leaves out,
 * such as a link-local address on BSD - as a peer's arrives: a
 * link-local one with its interface as the scope, and none of the scope
 * BSD writes into the address itself */
static bool all_held_on_host(char *missed, size_t len)
{
    struct sockaddr_storage ss;
    struct ifaddrs *ifs, *ifa;
    bool ok = true;

    if (0 != getifaddrs(&ifs)) {
        return true;
    }
    for (ifa = ifs; ok && NULL != ifa; ifa = ifa->ifa_next) {
        if (NULL == ifa->ifa_addr ||
            (AF_INET != ifa->ifa_addr->sa_family && AF_INET6 != ifa->ifa_addr->sa_family)) {
            continue;
        }
        memset(&ss, 0, sizeof(ss));
        memcpy(&ss, ifa->ifa_addr, (AF_INET == ifa->ifa_addr->sa_family)
                                       ? sizeof(struct sockaddr_in)
                                       : sizeof(struct sockaddr_in6));
        if (AF_INET6 == ss.ss_family &&
            IN6_IS_ADDR_LINKLOCAL(&((struct sockaddr_in6 *) &ss)->sin6_addr)) {
            ((struct sockaddr_in6 *) &ss)->sin6_addr.s6_addr[2] = 0;
            ((struct sockaddr_in6 *) &ss)->sin6_addr.s6_addr[3] = 0;
            ((struct sockaddr_in6 *) &ss)->sin6_scope_id = if_nametoindex(ifa->ifa_name);
        }
        if (!pmix_ptl_base_addr_held(&ss)) {
            ok = false;
            inet_ntop(ss.ss_family,
                      (AF_INET == ss.ss_family)
                          ? (void *) &((struct sockaddr_in *) &ss)->sin_addr
                          : (void *) &((struct sockaddr_in6 *) &ss)->sin6_addr,
                      missed, (pmix_socklen_t) len);
        }
    }
    freeifaddrs(ifs);
    return ok;
}

/* A peer's address as a connection on a link-local address of this host
 * gives it: 1 if the kernel says this host holds it, 0 if not, -1 when
 * there is no link-local address to connect on */
static int link_local_peer_held(char *where, size_t len)
{
    struct sockaddr_storage peer;
    struct sockaddr_in6 sa;
    struct ifaddrs *ifs, *ifa;
    pmix_socklen_t plen = sizeof(peer), slen = sizeof(sa);
    int lsd = -1, csd = -1, asd = -1, rc = -1;
    bool found = false;

    if (0 != getifaddrs(&ifs)) {
        return -1;
    }
    for (ifa = ifs; !found && NULL != ifa; ifa = ifa->ifa_next) {
        if (NULL == ifa->ifa_addr || AF_INET6 != ifa->ifa_addr->sa_family ||
            !IN6_IS_ADDR_LINKLOCAL(&((struct sockaddr_in6 *) ifa->ifa_addr)->sin6_addr)) {
            continue;
        }
        memcpy(&sa, ifa->ifa_addr, sizeof(sa));
        /* as one names it to connect: the scope apart from the address */
        sa.sin6_addr.s6_addr[2] = 0;
        sa.sin6_addr.s6_addr[3] = 0;
        sa.sin6_scope_id = if_nametoindex(ifa->ifa_name);
        sa.sin6_port = 0;
        snprintf(where, len, "%s", ifa->ifa_name);
        found = true;
    }
    freeifaddrs(ifs);
    if (!found) {
        return -1;
    }
    lsd = socket(AF_INET6, SOCK_STREAM, 0);
    if (0 > lsd || 0 != bind(lsd, (struct sockaddr *) &sa, sizeof(sa)) || 0 != listen(lsd, 1) ||
        0 != getsockname(lsd, (struct sockaddr *) &sa, &slen)) {
        goto done;
    }
    csd = socket(AF_INET6, SOCK_STREAM, 0);
    if (0 > csd || 0 != connect(csd, (struct sockaddr *) &sa, sizeof(sa))) {
        goto done;
    }
    asd = accept(lsd, NULL, NULL);
    memset(&peer, 0, sizeof(peer));
    if (0 > asd || 0 != getpeername(asd, (struct sockaddr *) &peer, &plen)) {
        goto done;
    }
    rc = pmix_ptl_base_addr_held(&peer) ? 1 : 0;

done:
    if (0 <= asd) {
        close(asd);
    }
    if (0 <= csd) {
        close(csd);
    }
    if (0 <= lsd) {
        close(lsd);
    }
    return rc;
}

static int case_on_host(void)
{
    struct sockaddr_storage ss;
    struct sockaddr_un *un;
    char addr[INET_ADDRSTRLEN], name[IF_NAMESIZE + 1], mapped[64], missed[INET6_ADDRSTRLEN];
    int rc = CHILD_PASS, held;

    if (PMIX_SUCCESS != PMIx_server_init(&mymodule, NULL, 0)) {
        return CHILD_FAIL;
    }
    load4(&ss, "127.0.0.1");
    rc |= check("127.0.0.1 is on this host", pmix_ptl_base_addr_on_host(&ss));
    load4(&ss, "127.1.2.3");
    rc |= check("127.1.2.3 is on this host", pmix_ptl_base_addr_on_host(&ss));
    load6(&ss, "::1");
    rc |= check("::1 is on this host", pmix_ptl_base_addr_on_host(&ss));
    load6(&ss, "::ffff:127.0.0.1");
    rc |= check("::ffff:127.0.0.1 is on this host", pmix_ptl_base_addr_on_host(&ss));
    if (public_if(addr, sizeof(addr), name, sizeof(name))) {
        load4(&ss, addr);
        rc |= check("this host's interface address is on this host",
                    pmix_ptl_base_addr_on_host(&ss));
        snprintf(mapped, sizeof(mapped), "::ffff:%s", addr);
        load6(&ss, mapped);
        rc |= check("...and arriving v4-mapped", pmix_ptl_base_addr_on_host(&ss));
    }
    missed[0] = '\0';
    rc |= check("every address an interface of this host holds is one the kernel says it holds",
                all_held_on_host(missed, sizeof(missed)));
    load4(&ss, "192.0.2.1");
    rc |= check("...and 192.0.2.1 is not", !pmix_ptl_base_addr_held(&ss));
    held = link_local_peer_held(name, sizeof(name));
    if (0 <= held) {
        rc |= check("a peer on a link-local address of this host is one it holds", 1 == held);
    } else {
        fprintf(stdout, "    (no link-local address to connect on)\n");
    }
    if ('\0' != missed[0]) {
        fprintf(stdout, "    not: %s\n", missed);
    }
    load4(&ss, "192.0.2.1");
    rc |= check("192.0.2.1 is not on this host", !pmix_ptl_base_addr_on_host(&ss));
    load6(&ss, "2001:db8::1");
    rc |= check("2001:db8::1 is not on this host", !pmix_ptl_base_addr_on_host(&ss));
    load6(&ss, "::ffff:192.0.2.1");
    rc |= check("::ffff:192.0.2.1 is not on this host", !pmix_ptl_base_addr_on_host(&ss));
    memset(&ss, 0, sizeof(ss));
    un = (struct sockaddr_un *) &ss;
    un->sun_family = AF_UNIX;
    rc |= check("a local socket's address is not", !pmix_ptl_base_addr_on_host(&ss));
    PMIx_server_finalize();
    return rc;
}

/* which mechanisms authenticate across hosts */
static int case_psec_remote(void)
{
    pmix_psec_module_t *mod;
    int rc = CHILD_PASS;

    if (PMIX_SUCCESS != PMIx_server_init(&mymodule, NULL, 0)) {
        return CHILD_FAIL;
    }
    mod = pmix_psec_base_assign_module("native");
    rc |= check("native is available", NULL != mod);
    if (NULL != mod) {
        rc |= check("native does not authenticate across hosts",
                    NULL == mod->create_bound && NULL == mod->validate_bound &&
                        !pmix_psec_base_remote_capable(mod));
    }
    mod = pmix_psec_base_assign_module("munge");
    if (NULL != mod) {
        rc |= check("munge authenticates across hosts", pmix_psec_base_remote_capable(mod));
    }
    mod = pmix_psec_base_assign_module("ssl");
    if (NULL != mod) {
        rc |= check("ssl authenticates across hosts",
                    NULL != mod->create_bound && NULL != mod->validate_bound);
    }
    PMIx_server_finalize();
    return rc;
}

/* whether what the server wrote to stderr, kept in `path`, says `what` */
static bool said(const char *path, const char *what)
{
    char buf[8192];
    size_t n;
    FILE *fp;

    fp = fopen(path, "r");
    if (NULL == fp) {
        return false;
    }
    n = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    buf[n] = '\0';
    return NULL != strstr(buf, what);
}

/* send our stderr to a file of our own, named in path; saved keeps the
 * old one */
static bool capture(char *path, size_t len, int *saved)
{
    const char *tmp = getenv("TMPDIR");
    int fd;

    snprintf(path, len, "%s/ptl_remote.err.%d", (NULL == tmp) ? "/tmp" : tmp, (int) getpid());
    fflush(stderr);
    *saved = dup(2);
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (0 > fd || 0 > *saved) {
        return false;
    }
    dup2(fd, 2);
    close(fd);
    return true;
}

static void release(int saved)
{
    fflush(stderr);
    if (0 <= saved) {
        dup2(saved, 2);
        close(saved);
    }
}

/* a host asking for remote connections, and one saying not to. The
 * refusal is told apart by what the server says: in a build without
 * remote tools the directive itself is refused; in one with them, only
 * native is left here, which cannot validate a tool on another host */
static int case_remote_directive(bool want)
{
    pmix_info_t info;
    pmix_status_t prc;
    char path[1024];
    const char *reason;
    int saved;

    if (!capture(path, sizeof(path), &saved)) {
        return CHILD_FAIL;
    }
    PMIX_INFO_LOAD(&info, PMIX_SERVER_REMOTE_CONNECTIONS, &want, PMIX_BOOL);
    prc = PMIx_server_init(&mymodule, &info, 1);
    PMIX_INFO_DESTRUCT(&info);
    release(saved);
    fprintf(stdout, "    PMIx_server_init with remote connections %s: %s\n",
            want ? "on" : "off", PMIx_Error_string(prc));
    if (PMIX_SUCCESS == prc) {
        PMIx_server_finalize();
    }
    if (!want) {
        unlink(path);
        return (PMIX_SUCCESS == prc) ? CHILD_PASS : CHILD_FAIL;
    }
#if PMIX_ENABLE_REMOTE_TOOLS
    reason = "neither is available";
#else
    reason = "Rebuild PMIx with --enable-remote-tools";
#endif
    fprintf(stdout, "    the server said \"%s\": %s\n", reason, said(path, reason) ? "yes" : "NO");
    if (!said(path, reason)) {
        unlink(path);
        return CHILD_FAIL;
    }
    unlink(path);
    return (PMIX_SUCCESS != prc) ? CHILD_PASS : CHILD_FAIL;
}

static int run_remote_on(void)
{
#if PMIX_ENABLE_REMOTE_TOOLS
    setenv("PMIX_MCA_psec", "native", 1);
#endif
    return case_remote_directive(true);
}

static int run_remote_off(void)
{
    return case_remote_directive(false);
}

/* directives that leave no loopback interface: a build without remote
 * tools takes no public one instead */
static int run_no_loopback(void)
{
#if PMIX_ENABLE_LEGACY_PEERS
    char addr[INET_ADDRSTRLEN], name[IF_NAMESIZE + 1], path[1024];
    pmix_info_t info[2];
    pmix_status_t prc;
    bool legacy = true, errpath;
    int saved;

    if (!public_if(addr, sizeof(addr), name, sizeof(name))) {
        fprintf(stdout, "    this host has no interface but loopback\n");
        return CHILD_SKIP;
    }
    /* a TCP listener, for clients and tools of earlier releases */
    PMIX_INFO_LOAD(&info[0], PMIX_SERVER_LEGACY_PEERS, &legacy, PMIX_BOOL);
    PMIX_INFO_LOAD(&info[1], PMIX_TCP_IF_INCLUDE, name, PMIX_STRING);
    errpath = capture(path, sizeof(path), &saved);
    prc = PMIx_server_init(&mymodule, info, 2);
    release(saved);
    PMIX_INFO_DESTRUCT(&info[0]);
    PMIX_INFO_DESTRUCT(&info[1]);
    fprintf(stdout, "    PMIx_server_init listening only on %s: %s (%s)\n", name,
            PMIx_Error_string(prc),
            (PMIX_SUCCESS == prc && NULL != pmix_ptl_base.listener.uri)
                ? pmix_ptl_base.listener.uri : "no TCP URI");
    if (PMIX_SUCCESS == prc) {
        PMIx_server_finalize();
    }
#    if PMIX_ENABLE_REMOTE_TOOLS
    /* the host did not say, so a public interface is taken */
    PMIX_HIDE_UNUSED_PARAMS(errpath);
    unlink(path);
    return (PMIX_SUCCESS == prc) ? CHILD_PASS : CHILD_FAIL;
#    else
    /* and for want of a loopback interface - not something else */
    fprintf(stdout, "    the server said it found no loopback interface: %s\n",
            (errpath && said(path, "no loopback interfaces were found")) ? "yes" : "NO");
    if (!errpath || !said(path, "no loopback interfaces were found")) {
        unlink(path);
        return CHILD_FAIL;
    }
    unlink(path);
    return (PMIX_SUCCESS != prc) ? CHILD_PASS : CHILD_FAIL;
#    endif
#else
    /* nothing else opens a TCP listener without remote connections */
    fprintf(stdout, "    this build takes no clients or tools of earlier releases\n");
    return CHILD_SKIP;
#endif
}

static void run_case(const char *name, int (*fn)(void))
{
    pid_t pid;
    int status;
    char detail[64];

    fflush(stdout);
    pid = fork();
    if (0 == pid) {
        exit(fn());
    }
    if (0 > pid || pid != waitpid(pid, &status, 0)) {
        report(name, 0, "fork or wait failed");
        return;
    }
    if (WIFEXITED(status) && CHILD_SKIP == WEXITSTATUS(status)) {
        fprintf(stdout, "  SKIP: %s\n", name);
        return;
    }
    snprintf(detail, sizeof(detail), "child status 0x%x", status);
    report(name, WIFEXITED(status) && CHILD_PASS == WEXITSTATUS(status), detail);
}

int main(void)
{
    fprintf(stdout, "Remote tools (this build %s them):\n",
            PMIX_ENABLE_REMOTE_TOOLS ? "supports" : "does not support");
    run_case("which peer addresses are on this host", case_on_host);
    run_case("which mechanisms authenticate across hosts", case_psec_remote);
    run_case("a host saying no remote connections initializes", run_remote_off);
#if PMIX_ENABLE_REMOTE_TOOLS
    run_case("a host asking for remote connections with only native fails its init",
             run_remote_on);
    run_case("with no loopback interface left, a public one is taken", run_no_loopback);
#else
    run_case("a host asking for remote connections fails its init", run_remote_on);
    run_case("with no loopback interface left, init fails rather than take a public one",
             run_no_loopback);
#endif

    fprintf(stdout, "\nResults: %d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
