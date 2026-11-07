/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * A server in a user namespace of its own, and peers outside it. The
 * kernel reports to the server a peer's ids as its namespace numbers
 * them, and an id the namespace does not map as the overflow id (65534).
 *
 * Where the namespace maps only the server's own user and group, the
 * overflow id names no process of it: a peer the kernel reports with the
 * overflow uid, or the overflow gid, is refused with PMIX_ERR_INVALID_CRED
 * - on the local socket at its first message, and over TCP (where legacy
 * clients are built) when it presents a credential for a rank the host
 * registered as that uid. A peer of the server's own user and group is
 * answered as usual.
 *
 * Where the namespace maps the overflow id itself - the server runs as
 * it, as an image that runs as "nobody" does in a container of its own -
 * a peer of that user is taken.
 *
 * Run as root on Linux. Anywhere else, or where user namespaces cannot be
 * made (a seccomp policy, say), the cases are skipped.
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "include/pmix_server.h"
#include "include/pmix_version.h"
#include "src/include/pmix_globals.h"
#include "src/mca/bfrops/bfrops_types.h"
#include "src/mca/ptl/base/base.h"
#include "src/mca/ptl/base/ptl_base_handshake.h"
#include "src/util/pmix_getid.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <netinet/in.h>
#if defined(__linux__)
#    include <sched.h>
#    include <sys/prctl.h>
#endif
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#define SKIP   77
#define NSPACE "userns.peer"

/* what the server child reports once it is up */
typedef struct {
    int status;        // 0 up, SKIP no namespace here, else failed
    char local[256];   // the local socket's path
    int tcp_port;      // the TCP listener's port, or 0
} server_info_t;

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

#if defined(__linux__) && defined(CLONE_NEWUSER)
/* write one line of a map file */
static bool put_map(const char *file, const char *line)
{
    int fd = open(file, O_WRONLY);
    bool ok;

    if (0 > fd) {
        return false;
    }
    ok = ((ssize_t) strlen(line) == write(fd, line, strlen(line)));
    close(fd);
    return ok;
}
#endif

/* The server: become `uid`:`gid`, enter a user namespace mapping only
 * them, as `inside` there, and serve until `donefd` closes */
static int run_server(uid_t uid, gid_t gid, uid_t inside, const char *dir, int infofd,
                      int donefd)
{
    server_info_t si;
    char map[64], *p, c;
    pmix_info_t info[3];
    pmix_proc_t proc;
    pmix_nspace_t ns;
    pmix_status_t rc;
    uint32_t one = 1;
    size_t ninfo = 1;
    bool legacy = true;

    memset(&si, 0, sizeof(si));
    si.status = SKIP;
#if defined(__linux__) && defined(CLONE_NEWUSER)
    /* A process that changed its uid is not dumpable, which leaves its
     * /proc/self files root's - and the maps below unwritable */
    if (0 != setgroups(0, NULL) || 0 != setgid(gid) || 0 != setuid(uid) ||
        0 != prctl(PR_SET_DUMPABLE, 1, 0, 0, 0) || 0 != unshare(CLONE_NEWUSER) ||
        !put_map("/proc/self/setgroups", "deny")) {
        (void) !write(infofd, &si, sizeof(si));
        return 0;
    }
    snprintf(map, sizeof(map), "%lu %lu 1", (unsigned long) inside, (unsigned long) uid);
    if (!put_map("/proc/self/uid_map", map)) {
        (void) !write(infofd, &si, sizeof(si));
        return 0;
    }
    snprintf(map, sizeof(map), "%lu %lu 1", (unsigned long) inside, (unsigned long) gid);
    if (!put_map("/proc/self/gid_map", map)) {
        (void) !write(infofd, &si, sizeof(si));
        return 0;
    }
#else
    (void) map;
    (void) uid;
    (void) gid;
    (void) inside;
    (void) !write(infofd, &si, sizeof(si));
    return 0;
#endif
    si.status = 1;
    setenv("TMPDIR", dir, 1);
    PMIX_INFO_LOAD(&info[0], PMIX_SERVER_TMPDIR, dir, PMIX_STRING);
#if PMIX_ENABLE_LEGACY_CLIENTS
    PMIX_INFO_LOAD(&info[1], PMIX_SERVER_LEGACY_PEERS, &legacy, PMIX_BOOL);
    ninfo = 2;
#else
    (void) legacy;
#endif
    rc = PMIx_server_init(&mymodule, info, ninfo);
    if (PMIX_SUCCESS != rc) {
        fprintf(stdout, "    server init: %s\n", PMIx_Error_string(rc));
        (void) !write(infofd, &si, sizeof(si));
        return 1;
    }
    /* a job whose one process the host registered as the overflow ids */
    PMIX_INFO_LOAD(&info[0], PMIX_JOB_SIZE, &one, PMIX_UINT32);
    PMIX_LOAD_NSPACE(ns, NSPACE);
    PMIX_LOAD_PROCID(&proc, NSPACE, 0);
    rc = PMIx_server_register_nspace(ns, 1, info, 1, NULL, NULL);
    if (PMIX_SUCCESS == rc || PMIX_OPERATION_SUCCEEDED == rc) {
        rc = PMIx_server_register_client(&proc, (uid_t) pmix_util_overflow_uid(),
                                         (gid_t) pmix_util_overflow_gid(), NULL, NULL, NULL);
    }
    if (PMIX_SUCCESS != rc && PMIX_OPERATION_SUCCEEDED != rc) {
        fprintf(stdout, "    registration: %s\n", PMIx_Error_string(rc));
        (void) !write(infofd, &si, sizeof(si));
        PMIx_server_finalize();
        return 1;
    }
    if (NULL == pmix_ptl_base.local_listener.uri ||
        NULL == (p = strstr(pmix_ptl_base.local_listener.uri, "usock://"))) {
        (void) !write(infofd, &si, sizeof(si));
        PMIx_server_finalize();
        return 1;
    }
    snprintf(si.local, sizeof(si.local), "%s", p + strlen("usock://"));
    if (NULL != pmix_ptl_base.listener.uri &&
        NULL != strstr(pmix_ptl_base.listener.uri, "tcp4://") &&
        NULL != (p = strrchr(pmix_ptl_base.listener.uri, ':'))) {
        si.tcp_port = atoi(p + 1);
    }
    si.status = 0;
    (void) !write(infofd, &si, sizeof(si));
    /* serve until the peers are done */
    while (0 < read(donefd, &c, 1)) {
    }
    PMIx_server_finalize();
    return 0;
}

/* read the first status the server sends */
static pmix_status_t first_status(int sd)
{
    uint32_t u32;
    size_t got = 0;
    ssize_t rd;

    while (got < sizeof(u32)) {
        rd = read(sd, (char *) &u32 + got, sizeof(u32) - got);
        if (0 >= rd) {
            return PMIX_ERR_UNREACH;
        }
        got += (size_t) rd;
    }
    return (pmix_status_t) ntohl(u32);
}

/* a client's offer on the local socket, naming NSPACE:0 */
static pmix_status_t offer(const char *path)
{
    struct sockaddr_un sa;
    struct timeval tv = {10, 0};
    pmix_ptl_hdr_t hdr;
    pmix_proc_t proc;
    char msg[1024], version[32];
    size_t csize;
    uint8_t flag = PMIX_SIMPLE_CLIENT, one = 1, bt = PMIX_BFROP_BUFFER_NON_DESC;
    pmix_status_t rc;
    int sd;

    snprintf(version, sizeof(version), "%ld.%ld.%ld", (long) PMIX_VERSION_MAJOR,
             (long) PMIX_VERSION_MINOR, (long) PMIX_VERSION_RELEASE);
    PMIX_LOAD_PROCID(&proc, NSPACE, 0);
    memset(msg, 0, sizeof(msg));
    csize = sizeof(hdr);
    PMIX_PTL_PUT_STRING(version);
    PMIX_PTL_PUT_U8(flag);
    PMIX_PTL_PUT_PROCID(proc);
    PMIX_PTL_PUT_U8(one);
    PMIX_PTL_PUT_STRING("native");
    PMIX_PTL_PUT_U8(one);
    PMIX_PTL_PUT_STRING("v70");
    PMIX_PTL_PUT_U8(bt);
    PMIX_PTL_PUT_U8(one);
    PMIX_PTL_PUT_STRING("hash");
    memset(&hdr, 0, sizeof(hdr));
    hdr.pindex = -1;
    hdr.tag = UINT32_MAX;
    hdr.nbytes = (uint32_t) (csize - sizeof(hdr));
    memcpy(msg, &hdr, sizeof(hdr));

    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", path);
    sd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (0 > sd) {
        return PMIX_ERROR;
    }
    if (0 != connect(sd, (struct sockaddr *) &sa, sizeof(sa))) {
        close(sd);
        return PMIX_ERR_UNREACH;
    }
    setsockopt(sd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    if ((ssize_t) csize != write(sd, msg, csize)) {
        close(sd);
        return PMIX_ERROR;
    }
    rc = first_status(sd);
    close(sd);
    return rc;
}

#if PMIX_ENABLE_LEGACY_CLIENTS
/* a connect-ack of an earlier release over TCP for NSPACE:0, its
 * credential claiming the overflow ids, as the host registered them */
static pmix_status_t tcp_ack(int port)
{
    struct sockaddr_in sa;
    struct timeval tv = {10, 0};
    pmix_ptl_hdr_t hdr;
    pmix_proc_t proc;
    char msg[1024], cred[sizeof(uid_t) + sizeof(gid_t)];
    uid_t cuid = (uid_t) pmix_util_overflow_uid();
    gid_t cgid = (gid_t) pmix_util_overflow_gid();
    uint8_t flag = PMIX_SIMPLE_CLIENT, bt = PMIX_BFROP_BUFFER_NON_DESC;
    uint32_t credlen = sizeof(cred);
    size_t csize;
    pmix_status_t rc;
    int sd;

    memcpy(cred, &cuid, sizeof(uid_t));
    memcpy(cred + sizeof(uid_t), &cgid, sizeof(gid_t));
    PMIX_LOAD_PROCID(&proc, NSPACE, 0);
    memset(msg, 0, sizeof(msg));
    csize = sizeof(hdr);
    PMIX_PTL_PUT_STRING("native");
    PMIX_PTL_PUT_U32(credlen);
    PMIX_PTL_PUT_BLOB(cred, credlen);
    PMIX_PTL_PUT_U8(flag);
    PMIX_PTL_PUT_PROCID(proc);
    PMIX_PTL_PUT_STRING("6.1.0");
    PMIX_PTL_PUT_STRING("v61");
    PMIX_PTL_PUT_U8(bt);
    PMIX_PTL_PUT_STRING("hash");
    memset(&hdr, 0, sizeof(hdr));
    hdr.pindex = -1;
    hdr.tag = UINT32_MAX;
    hdr.nbytes = (uint32_t) (csize - sizeof(hdr));
    memcpy(msg, &hdr, sizeof(hdr));

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t) port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sd = socket(AF_INET, SOCK_STREAM, 0);
    if (0 > sd) {
        return PMIX_ERROR;
    }
    if (0 != connect(sd, (struct sockaddr *) &sa, sizeof(sa))) {
        close(sd);
        return PMIX_ERR_UNREACH;
    }
    setsockopt(sd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    if ((ssize_t) csize != write(sd, msg, csize)) {
        close(sd);
        return PMIX_ERROR;
    }
    rc = first_status(sd);
    close(sd);
    return rc;
}
#endif

/* A peer, in a child of its own as `uid`:`gid`: what the server first
 * answers it, on the local socket or (port > 0) over TCP */
static pmix_status_t peer_status(uid_t uid, gid_t gid, const server_info_t *si, int port)
{
    pmix_status_t rc = PMIX_ERROR;
    int fds[2], status;
    pid_t pid;

    if (0 != pipe(fds)) {
        return PMIX_ERROR;
    }
    pid = fork();
    if (0 == pid) {
        close(fds[0]);
        if (0 != setgroups(0, NULL) || 0 != setgid(gid) || 0 != setuid(uid)) {
            _exit(1);
        }
#if PMIX_ENABLE_LEGACY_CLIENTS
        rc = (0 < port) ? tcp_ack(port) : offer(si->local);
#else
        (void) port;
        rc = offer(si->local);
#endif
        (void) !write(fds[1], &rc, sizeof(rc));
        _exit(0);
    }
    close(fds[1]);
    if ((ssize_t) sizeof(rc) != read(fds[0], &rc, sizeof(rc))) {
        rc = PMIX_ERROR;
    }
    close(fds[0]);
    waitpid(pid, &status, 0);
    return rc;
}

/* start a server as `uid`:`gid`, `inside` in its namespace. Returns its
 * pid, with *done the descriptor that ends it when closed */
static pid_t start_server(uid_t uid, gid_t gid, uid_t inside, char *dir, size_t dirlen,
                          server_info_t *si, int *done)
{
    const char *base = getenv("TMPDIR");
    int infop[2], donep[2];
    pid_t pid;

    memset(si, 0, sizeof(*si));
    si->status = 1;
    snprintf(dir, dirlen, "%s/pmix-uspeer-XXXXXX", (NULL == base) ? "/tmp" : base);
    if (NULL == mkdtemp(dir) || 0 != chown(dir, uid, gid) || 0 != chmod(dir, 0755)) {
        return -1;
    }
    if (0 != pipe(infop) || 0 != pipe(donep)) {
        return -1;
    }
    pid = fork();
    if (0 == pid) {
        close(infop[0]);
        close(donep[1]);
        _exit(run_server(uid, gid, inside, dir, infop[1], donep[0]));
    }
    close(infop[1]);
    close(donep[0]);
    if ((ssize_t) sizeof(*si) != read(infop[0], si, sizeof(*si))) {
        si->status = 1;
    }
    close(infop[0]);
    *done = donep[1];
    return pid;
}

static void stop_server(pid_t pid, int done, const char *dir)
{
    char cmd[512];
    int status;

    close(done);
    waitpid(pid, &status, 0);
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
    (void) !system(cmd);
}

static bool expect(const char *what, pmix_status_t got, pmix_status_t want)
{
    char detail[128];

    snprintf(detail, sizeof(detail), "read %s, expected %s", PMIx_Error_string(got),
             PMIx_Error_string(want));
    report(what, got == want, detail);
    return got == want;
}

#define SRV_UID ((uid_t) 4242)
#define SRV_GID ((gid_t) 4242)
#define NOBODY_UID ((uid_t) 70000)
#define NOBODY_GID ((gid_t) 70000)

int main(void)
{
    server_info_t si;
    char dir[256];
    pid_t pid;
    int done;

    setvbuf(stdout, NULL, _IONBF, 0);
    fprintf(stdout, "\n=== a server in a user namespace, its peers outside it ===\n\n");
#if !defined(__linux__) || !defined(CLONE_NEWUSER)
    fprintf(stdout, "no user namespaces on this platform - skipping\n");
    return SKIP;
#endif
    if (0 != geteuid()) {
        fprintf(stdout, "not running as root - skipping\n");
        return SKIP;
    }

    /* a namespace mapping only the server's own user and group */
    pid = start_server(SRV_UID, SRV_GID, SRV_UID, dir, sizeof(dir), &si, &done);
    if (0 > pid) {
        fprintf(stdout, "no scratch directory - skipping\n");
        return SKIP;
    }
    if (SKIP == si.status) {
        stop_server(pid, done, dir);
        fprintf(stdout, "could not make a user namespace here - skipping\n");
        return SKIP;
    }
    if (0 != si.status) {
        stop_server(pid, done, dir);
        fprintf(stdout, "  FAIL: the server in a user namespace did not start\n");
        return 1;
    }
    expect("a peer of the server's own user and group is answered",
           peer_status(SRV_UID, SRV_GID, &si, 0), PMIX_SUCCESS);
    expect("a peer the namespace does not map - root, outside - is refused",
           peer_status(0, 0, &si, 0), PMIX_ERR_INVALID_CRED);
    expect("a peer of the server's user with a group the namespace does not map is refused",
           peer_status(SRV_UID, 0, &si, 0), PMIX_ERR_INVALID_CRED);
#if PMIX_ENABLE_LEGACY_CLIENTS
    if (0 < si.tcp_port) {
        expect("over TCP, a peer the namespace does not map, claiming the overflow ids the "
               "host registered its rank as, is refused",
               peer_status(0, 0, &si, si.tcp_port), PMIX_ERR_INVALID_CRED);
    } else {
        report("the server has a TCP listener", 0, "none");
    }
#endif
    stop_server(pid, done, dir);

    /* a namespace mapping the overflow id itself: the server is "nobody" */
    pid = start_server(NOBODY_UID, NOBODY_GID, (uid_t) pmix_util_overflow_uid(), dir,
                       sizeof(dir), &si, &done);
    if (0 > pid || 0 != si.status) {
        if (0 <= pid) {
            stop_server(pid, done, dir);
        }
        report("a server running as the overflow id in its namespace starts", 0,
               "did not start");
    } else {
        expect("where the namespace maps the overflow id, a peer of that user is answered",
               peer_status(NOBODY_UID, NOBODY_GID, &si, 0), PMIX_SUCCESS);
#if PMIX_ENABLE_LEGACY_CLIENTS
        if (0 < si.tcp_port) {
            expect("...and over TCP is taken as the rank the host registered as it",
                   peer_status(NOBODY_UID, NOBODY_GID, &si, si.tcp_port), PMIX_SUCCESS);
        }
#endif
        stop_server(pid, done, dir);
    }

    fprintf(stdout, "\n%d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
