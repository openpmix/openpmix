/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit tests for the ptl listener: what it does with the directives a host
 * hands PMIx_server_init, and what it does when accept() fails.
 *
 *  - accept() failing with EMFILE stopped the listener for good, so a
 *    server that once ran out of descriptors never accepted another
 *    connection, however many were freed. It now pauses and accepts again
 *    a second later. Linux leaves the connection that failed in the
 *    backlog, and it is the one accepted; macOS closes it, so the case
 *    connects again and that one must be accepted. Before
 *    that, the stop itself closed the listening descriptor and left the
 *    rest standing: the event registered, the listener marked active, and
 *    pmix_ptl_base.listener.socket naming a number the kernel was free to
 *    reuse - which finalize then closed. The finalize case holds that.
 *
 *  - an accept() error that does not clear on its own is retried once a
 *    second for a minute, and only then is the listener stopped; running
 *    out of descriptors or memory is retried for as long as it lasts, and
 *    a descriptor that is no longer a socket stops the listener at once.
 *    These drive pmix_ptl_base_accept_failed directly, since no portable
 *    way exists to make accept() report a chosen error.
 *
 *  - PMIX_TCP_IF_INCLUDE and its string siblings were read out of the
 *    value union as a pointer whatever type the value carried.
 *
 *  - PMIX_TCP_IPV4_PORT given as a PMIX_INT larger than a port was
 *    narrowed to 16 bits and bound: 70000 listened on 4464.
 *
 *  - an empty PMIX_TCP_REPORT_URI was read as pipe descriptor 0, so the
 *    listener wrote its URI to stdin and closed it.
 *
 *  - a PMIX_TCP_REPORT_URI naming a file was written but not removed at
 *    finalize, because the name finalize removes was taken from the MCA
 *    parameter before the directive could replace it.
 *
 *  - a PMIX_TCP_REPORT_URI is written only as a regular file with a single
 *    name: a symlink at the name is not followed and a second hard link is
 *    not written through, while an existing regular file is replaced.
 *
 * Each case runs in a forked child: PMIx_server_init can only be called
 * once in a process, and against the unfixed library several of these die
 * on a signal rather than fail.
 */

#include "src/include/pmix_config.h"

#include "include/pmix_server.h"
#include "src/include/pmix_globals.h"
#include "src/mca/ptl/base/base.h"
#include "src/mca/ptl/tcp/ptl_tcp.h"
#include "src/threads/pmix_threads.h"
#include "src/util/pmix_if.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHILD_PASS 0
#define CHILD_FAIL 1
#define CHILD_SKIP 77

static int npass = 0;
static int nfail = 0;
static int nskip = 0;
static char tmpdir[PMIX_PATH_MAX];
static pmix_server_module_t mymodule = {0};

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

static void pause_ms(long ms)
{
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

/* run fn in a child and report its outcome under name */
static void run_case(const char *name, void (*fn)(void))
{
    pid_t pid;
    int status;
    char detail[64];

    fflush(stdout);
    fflush(stderr);
    pid = fork();
    if (0 > pid) {
        report(name, 0, "fork failed");
        return;
    }
    if (0 == pid) {
        alarm(30);
        fn();
        _exit(CHILD_FAIL);
    }
    if (pid != waitpid(pid, &status, 0)) {
        report(name, 0, "waitpid failed");
    } else if (WIFSIGNALED(status)) {
        snprintf(detail, sizeof(detail), "child died on signal %d", WTERMSIG(status));
        report(name, 0, detail);
    } else if (CHILD_SKIP == WEXITSTATUS(status)) {
        fprintf(stdout, "  SKIP: %s\n", name);
        ++nskip;
    } else {
        report(name, CHILD_PASS == WEXITSTATUS(status), "child reported failure");
    }
}

/* ---- accept() failures -------------------------------------------- */

typedef struct {
    pmix_object_t super;
    pmix_event_t ev;
    pmix_lock_t lock;
    int err;      // fed to pmix_ptl_base_accept_failed nerr times first
    int nerr;
    bool active;
    bool paused;
    int nretries;
    int socket;
    size_t npending;
} lstate_t;
static PMIX_CLASS_INSTANCE(lstate_t, pmix_object_t, NULL, NULL);

static void read_listener(int sd, short args, void *cbdata)
{
    lstate_t *ls = (lstate_t *) cbdata;
    pmix_listener_t *lt = &pmix_ptl_base.listener;
    int n;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    for (n = 0; n < ls->nerr; n++) {
        pmix_ptl_base_accept_failed(lt, ls->err);
    }
    ls->active = lt->active;
    ls->paused = lt->paused;
    ls->nretries = lt->nretries;
    ls->socket = lt->socket;
    ls->npending = pmix_list_get_size(&pmix_ptl_base.pending_connections);
    PMIX_WAKEUP_THREAD(&ls->lock);
}

/* the listener's state, read on the progress thread after it has been
 * told nerr times that accept() failed with err */
static void listener_state(lstate_t *out, int err, int nerr)
{
    lstate_t *ls;

    ls = PMIX_NEW(lstate_t);
    PMIX_CONSTRUCT_LOCK(&ls->lock);
    ls->err = err;
    ls->nerr = nerr;
    PMIX_THREADSHIFT(ls, read_listener);
    PMIX_WAIT_THREAD(&ls->lock);
    PMIX_DESTRUCT_LOCK(&ls->lock);
    out->active = ls->active;
    out->paused = ls->paused;
    out->nretries = ls->nretries;
    out->socket = ls->socket;
    out->npending = ls->npending;
    PMIX_RELEASE(ls);
}

static void print_state(const char *when, const lstate_t *ls)
{
    fprintf(stderr, "listener %s: active %d paused %d retries %d socket %d pending %lu\n",
            when, (int) ls->active, (int) ls->paused, ls->nretries, ls->socket,
            (unsigned long) ls->npending);
}

static int dial_listener(void)
{
    struct sockaddr_storage addr;
    int c;

    memcpy(&addr, &pmix_mca_ptl_tcp_component.connection, sizeof(addr));
    c = socket(addr.ss_family, SOCK_STREAM, 0);
    if (0 > c) {
        return -1;
    }
    if (0 != connect(c, (struct sockaddr *) &addr,
                     (AF_INET == addr.ss_family) ? sizeof(struct sockaddr_in)
                                                 : sizeof(struct sockaddr_in6))) {
        close(c);
        return -1;
    }
    return c;
}

/* Connect while the server has no descriptor to accept with, and leave
 * the limit lowered long enough for the accept to fail. Returns the
 * client's socket, with the limit restored */
static int connect_out_of_descriptors(void)
{
    struct rlimit rl, saved;
    struct sockaddr_storage addr;
    int c, probe;

    memcpy(&addr, &pmix_mca_ptl_tcp_component.connection, sizeof(addr));
    c = socket(addr.ss_family, SOCK_STREAM, 0);
    /* leave no descriptor free for the accept */
    probe = dup(0);
    close(probe);
    getrlimit(RLIMIT_NOFILE, &saved);
    rl = saved;
    rl.rlim_cur = probe;
    if (0 > c || 0 != setrlimit(RLIMIT_NOFILE, &rl)) {
        _exit(CHILD_FAIL);
    }
    if (0 != connect(c, (struct sockaddr *) &addr,
                     (AF_INET == addr.ss_family) ? sizeof(struct sockaddr_in)
                                                 : sizeof(struct sockaddr_in6))) {
        _exit(CHILD_FAIL);
    }
    pause_ms(300);
    setrlimit(RLIMIT_NOFILE, &saved);
    return c;
}

/* true if the far end of c has closed it within msec */
static bool closed_by_peer(int c, int msec)
{
    struct pollfd pfd;
    char ch;

    pfd.fd = c;
    pfd.events = POLLIN;
    pfd.revents = 0;
    if (0 >= poll(&pfd, 1, msec)) {
        return false;
    }
    return 0 >= recv(c, &ch, 1, 0);
}

/* out of descriptors: the listener pauses with its socket open, and once
 * descriptors are free it accepts again - the connection that was waiting
 * if the kernel kept it, or a new one if the kernel closed it */
static void emfile_child(void)
{
    lstate_t ls;
    int c, sock;
    bool ok;

    if (PMIX_SUCCESS != PMIx_server_init(&mymodule, NULL, 0)) {
        _exit(CHILD_FAIL);
    }
    c = connect_out_of_descriptors();
    listener_state(&ls, 0, 0);
    ok = !ls.active && ls.paused && 0 <= ls.socket && 0 == ls.npending;
    if (!ok) {
        print_state("after EMFILE", &ls);
        _exit(CHILD_FAIL);
    }
    sock = ls.socket;
    /* the retry is due a second after the failure */
    pause_ms(1500);
    if (closed_by_peer(c, 0)) {
        close(c);
        if (0 > (c = dial_listener())) {
            _exit(CHILD_FAIL);
        }
        pause_ms(300);
    }
    listener_state(&ls, 0, 0);
    ok = ls.active && !ls.paused && sock == ls.socket && 1 == ls.npending;
    if (!ok) {
        print_state("a second later", &ls);
        _exit(CHILD_FAIL);
    }
    close(c);
    PMIx_server_finalize();
    _exit(CHILD_PASS);
}

/* finalize while paused: the listening socket is closed, and a second
 * init comes up listening rather than paused */
static void emfile_finalize_child(void)
{
    lstate_t ls;
    int c, sock;

    if (PMIX_SUCCESS != PMIx_server_init(&mymodule, NULL, 0)) {
        _exit(CHILD_FAIL);
    }
    c = connect_out_of_descriptors();
    listener_state(&ls, 0, 0);
    if (!ls.paused || 0 > ls.socket) {
        print_state("after EMFILE", &ls);
        _exit(CHILD_FAIL);
    }
    sock = ls.socket;
    PMIx_server_finalize();
    if (0 <= fcntl(sock, F_GETFD) || EBADF != errno) {
        fprintf(stderr, "listening descriptor %d still open after finalize\n", sock);
        _exit(CHILD_FAIL);
    }
    /* a second cycle must come up listening, with no retry left over */
    if (PMIX_SUCCESS != PMIx_server_init(&mymodule, NULL, 0)) {
        _exit(CHILD_FAIL);
    }
    pause_ms(1500);
    listener_state(&ls, 0, 0);
    if (!ls.active || ls.paused || 0 > ls.socket) {
        print_state("in the second cycle", &ls);
        _exit(CHILD_FAIL);
    }
    PMIx_server_finalize();
    close(c);
    _exit(CHILD_PASS);
}

/* running out of descriptors or memory is retried for as long as it lasts */
static void resource_retry_child(void)
{
    lstate_t ls;

    if (PMIX_SUCCESS != PMIx_server_init(&mymodule, NULL, 0)) {
        _exit(CHILD_FAIL);
    }
    listener_state(&ls, EMFILE, 200);
    if (!ls.paused || 0 > ls.socket || 0 != ls.nretries) {
        print_state("after 200 EMFILE", &ls);
        _exit(CHILD_FAIL);
    }
    listener_state(&ls, ENOBUFS, 200);
    if (!ls.paused || 0 > ls.socket || 0 != ls.nretries) {
        print_state("after 200 ENOBUFS", &ls);
        _exit(CHILD_FAIL);
    }
    PMIx_server_finalize();
    _exit(CHILD_PASS);
}

/* An error that does not clear on its own is retried 60 times; the next
 * one stops the listener. A connection accepted in between starts the
 * count again */
static void unknown_retry_child(void)
{
    lstate_t ls;
    int c;

    if (PMIX_SUCCESS != PMIx_server_init(&mymodule, NULL, 0)) {
        _exit(CHILD_FAIL);
    }
    listener_state(&ls, EIO, 60);
    if (!ls.paused || 0 > ls.socket || 60 != ls.nretries) {
        print_state("after 60 EIO", &ls);
        _exit(CHILD_FAIL);
    }
    /* resumed a second later, and a connection resets the count */
    pause_ms(1500);
    if (0 > (c = dial_listener())) {
        _exit(CHILD_FAIL);
    }
    pause_ms(300);
    listener_state(&ls, 0, 0);
    if (!ls.active || ls.paused || 0 != ls.nretries || 1 != ls.npending) {
        print_state("after a connection", &ls);
        _exit(CHILD_FAIL);
    }
    listener_state(&ls, EIO, 60);
    if (!ls.paused || 0 > ls.socket) {
        print_state("after 60 more EIO", &ls);
        _exit(CHILD_FAIL);
    }
    listener_state(&ls, EIO, 1);
    if (ls.active || ls.paused || 0 <= ls.socket) {
        print_state("after the 61st EIO", &ls);
        _exit(CHILD_FAIL);
    }
    close(c);
    PMIx_server_finalize();
    _exit(CHILD_PASS);
}

/* a descriptor that is no longer a socket stops the listener at once */
static void notsock_child(void)
{
    lstate_t ls;

    if (PMIX_SUCCESS != PMIx_server_init(&mymodule, NULL, 0)) {
        _exit(CHILD_FAIL);
    }
    listener_state(&ls, ENOTSOCK, 1);
    if (ls.active || ls.paused || 0 <= ls.socket) {
        print_state("after ENOTSOCK", &ls);
        _exit(CHILD_FAIL);
    }
    PMIx_server_finalize();
    _exit(CHILD_PASS);
}

/* ---- directives ---------------------------------------------------- */

/* An include list naming only a subnet this host is not on, or an address
 * that is not a subnet, is refused - it used to read as no list at all,
 * and leave the listener where it was told not to be */
static void include_child(const char *spec)
{
    pmix_info_t info;

    PMIX_INFO_LOAD(&info, PMIX_TCP_IF_INCLUDE, spec, PMIX_STRING);
    if (PMIX_SUCCESS == PMIx_server_init(&mymodule, &info, 1)) {
        fprintf(stderr, "listening at %s\n", (NULL == pmix_ptl_base.listener.uri)
                                                   ? "(none)" : pmix_ptl_base.listener.uri);
        _exit(CHILD_FAIL);
    }
    _exit(CHILD_PASS);
}
static void include_nomatch_child(void)
{
    include_child("203.0.113.0/24");
}
static void include_noprefix_child(void)
{
    include_child("192.168.77.5");
}

/* A help message is printed in a second init cycle too. The first cycle
 * left help routed through the server's output forwarding, which the next
 * init could not use while it was starting - so a server that failed its
 * second init said nothing of why */
static void help_after_cycle_child(void)
{
    pmix_info_t info;
    char path[] = "/tmp/pmix_help_cycle.XXXXXX", buf[8192];
    int fd, saved;
    ssize_t n;
    bool said;

    if (PMIX_SUCCESS != PMIx_server_init(&mymodule, NULL, 0)) {
        _exit(CHILD_FAIL);
    }
    PMIx_server_finalize();

    fd = mkstemp(path);
    if (0 > fd) {
        _exit(CHILD_FAIL);
    }
    unlink(path);
    fflush(stderr);
    saved = dup(STDERR_FILENO);
    dup2(fd, STDERR_FILENO);
    PMIX_INFO_LOAD(&info, PMIX_TCP_IF_INCLUDE, "192.168.77.5", PMIX_STRING);
    if (PMIX_SUCCESS == PMIx_server_init(&mymodule, &info, 1)) {
        dup2(saved, STDERR_FILENO);
        fprintf(stderr, "the second init succeeded\n");
        _exit(CHILD_FAIL);
    }
    fflush(stderr);
    dup2(saved, STDERR_FILENO);
    lseek(fd, 0, SEEK_SET);
    n = read(fd, buf, sizeof(buf) - 1);
    buf[(0 < n) ? n : 0] = '\0';
    said = (NULL != strstr(buf, "pmix_ptl_base_if_include"));
    if (!said) {
        fprintf(stderr, "the second init said: \"%s\"\n", buf);
    }
    _exit(said ? CHILD_PASS : CHILD_FAIL);
}

static void bool_if_include_child(void)
{
    pmix_info_t info;
    bool yes = true;

    PMIX_INFO_LOAD(&info, PMIX_TCP_IF_INCLUDE, &yes, PMIX_BOOL);
    /* refused, not crashed on */
    _exit(PMIX_SUCCESS != PMIx_server_init(&mymodule, &info, 1) ? CHILD_PASS : CHILD_FAIL);
}

static void big_port_child(void)
{
    pmix_info_t info;
    int port = 70000;

    PMIX_INFO_LOAD(&info, PMIX_TCP_IPV4_PORT, &port, PMIX_INT);
    _exit(PMIX_SUCCESS != PMIx_server_init(&mymodule, &info, 1) ? CHILD_PASS : CHILD_FAIL);
}

static void empty_report_uri_child(void)
{
    pmix_info_t info;
    int fds[2];
    bool ok;

    /* give the child a stdin we know is open */
    if (0 != pipe(fds) || 0 > dup2(fds[0], 0)) {
        _exit(CHILD_FAIL);
    }
    PMIX_INFO_LOAD(&info, PMIX_TCP_REPORT_URI, "", PMIX_STRING);
    if (PMIX_SUCCESS != PMIx_server_init(&mymodule, &info, 1)) {
        _exit(CHILD_FAIL);
    }
    ok = (-1 != fcntl(0, F_GETFD));
    /* and leaves nothing behind in the shared tmpdir */
    PMIx_server_finalize();
    _exit(ok ? CHILD_PASS : CHILD_FAIL);
}

static void report_uri_file_child(void)
{
    pmix_info_t info;
    char path[PMIX_PATH_MAX + 16];
    struct stat st;

    snprintf(path, sizeof(path), "%s/uri.txt", tmpdir);
    unlink(path);
    /* so that the mode is the one the server asks for */
    umask(0);
    PMIX_INFO_LOAD(&info, PMIX_TCP_REPORT_URI, path, PMIX_STRING);
    if (PMIX_SUCCESS != PMIx_server_init(&mymodule, &info, 1)) {
        _exit(CHILD_FAIL);
    }
    if (0 != stat(path, &st)) {
        fprintf(stderr, "report file %s was not written\n", path);
        _exit(CHILD_FAIL);
    }
    /* anyone may read it; only its server writes it */
    if (0 != (st.st_mode & (S_IWGRP | S_IWOTH))) {
        fprintf(stderr, "report file %s is mode %o\n", path, (unsigned) (st.st_mode & 07777));
        _exit(CHILD_FAIL);
    }
    PMIx_server_finalize();
    if (0 == stat(path, &st)) {
        fprintf(stderr, "report file %s left behind at finalize\n", path);
        unlink(path);
        _exit(CHILD_FAIL);
    }
    _exit(CHILD_PASS);
}

/* Write a known line to path. */
static int put_marker(const char *path)
{
    FILE *fp = fopen(path, "w");

    if (NULL == fp) {
        return -1;
    }
    fprintf(fp, "marker\n");
    fclose(fp);
    return 0;
}

/* Does path still hold exactly the marker? */
static bool has_marker(const char *path)
{
    char line[64] = {0};
    FILE *fp = fopen(path, "r");
    bool ok;

    if (NULL == fp) {
        return false;
    }
    ok = (NULL != fgets(line, sizeof(line), fp) && 0 == strcmp(line, "marker\n") &&
          NULL == fgets(line, sizeof(line), fp));
    fclose(fp);
    return ok;
}

static void report_uri_symlink_child(void)
{
    pmix_info_t info;
    char target[PMIX_PATH_MAX + 16], lnk[PMIX_PATH_MAX + 16];
    bool ok;

    snprintf(target, sizeof(target), "%s/target.txt", tmpdir);
    snprintf(lnk, sizeof(lnk), "%s/link.txt", tmpdir);
    unlink(target);
    unlink(lnk);
    if (0 != put_marker(target) || 0 != symlink(target, lnk)) {
        _exit(CHILD_FAIL);
    }
    PMIX_INFO_LOAD(&info, PMIX_TCP_REPORT_URI, lnk, PMIX_STRING);
    (void) PMIx_server_init(&mymodule, &info, 1);
    ok = has_marker(target);
    if (!ok) {
        fprintf(stderr, "the report was written through a symlink\n");
    }
    unlink(lnk);
    unlink(target);
    _exit(ok ? CHILD_PASS : CHILD_FAIL);
}

static void report_uri_hardlink_child(void)
{
    pmix_info_t info;
    char target[PMIX_PATH_MAX + 16], lnk[PMIX_PATH_MAX + 16];
    bool ok;

    snprintf(target, sizeof(target), "%s/target.txt", tmpdir);
    snprintf(lnk, sizeof(lnk), "%s/link.txt", tmpdir);
    unlink(target);
    unlink(lnk);
    if (0 != put_marker(target) || 0 != link(target, lnk)) {
        _exit(CHILD_FAIL);
    }
    PMIX_INFO_LOAD(&info, PMIX_TCP_REPORT_URI, lnk, PMIX_STRING);
    (void) PMIx_server_init(&mymodule, &info, 1);
    ok = has_marker(target);
    if (!ok) {
        fprintf(stderr, "the report was written through a second hard link\n");
    }
    unlink(lnk);
    unlink(target);
    _exit(ok ? CHILD_PASS : CHILD_FAIL);
}

static void report_uri_replace_child(void)
{
    pmix_info_t info;
    char path[PMIX_PATH_MAX + 16];
    char line[256] = {0};
    FILE *fp;
    bool ok;

    snprintf(path, sizeof(path), "%s/uri.txt", tmpdir);
    unlink(path);
    if (0 != put_marker(path)) {
        _exit(CHILD_FAIL);
    }
    PMIX_INFO_LOAD(&info, PMIX_TCP_REPORT_URI, path, PMIX_STRING);
    if (PMIX_SUCCESS != PMIx_server_init(&mymodule, &info, 1)) {
        unlink(path);
        _exit(CHILD_FAIL);
    }
    /* the URI, and nothing left of what was there before */
    fp = fopen(path, "r");
    ok = (NULL != fp && NULL != fgets(line, sizeof(line), fp) &&
          0 != strcmp(line, "marker\n") && NULL != strstr(line, ";"));
    if (NULL != fp) {
        fclose(fp);
    }
    PMIx_server_finalize();
    unlink(path);
    _exit(ok ? CHILD_PASS : CHILD_FAIL);
}

/* ---- the report file ------------------------------------------------ */

/* the line after the first five that carries the given tag, or NULL */
static char *tagged_line(const char *path, const char *tag, int *lineno)
{
    FILE *fp;
    char line[4096], *found = NULL;
    int n = 0;

    *lineno = 0;
    if (NULL == (fp = fopen(path, "r"))) {
        return NULL;
    }
    while (NULL != fgets(line, sizeof(line), fp)) {
        ++n;
        if (0 == strncmp(line, tag, strlen(tag))) {
            line[strcspn(line, "\n")] = '\0';
            found = strdup(line + strlen(tag));
            *lineno = n;
            break;
        }
    }
    fclose(fp);
    return found;
}

/* The report file tells a process that finds us through it which wire
 * formats and security mechanisms we accept - exactly the ones this
 * server runs, after whatever restriction its MCA parameters put on
 * them. That restriction is the case a guess from our version gets
 * wrong */
static void compat_lines_child(void)
{
    pmix_info_t info;
    char path[PMIX_PATH_MAX + 16];
    char *bf, *ps;
    int line;
    bool ok;

    setenv("PMIX_MCA_bfrops", "v41,v21", 1);
    snprintf(path, sizeof(path), "%s/compat.txt", tmpdir);
    unlink(path);
    PMIX_INFO_LOAD(&info, PMIX_TCP_REPORT_URI, path, PMIX_STRING);
    if (PMIX_SUCCESS != PMIx_server_init(&mymodule, &info, 1)) {
        _exit(CHILD_FAIL);
    }
    bf = tagged_line(path, "bfrops:", &line);
    ps = tagged_line(path, "psec:", &line);
    ok = (NULL != bf && 0 == strcmp(bf, "v41,v21"));
    if (!ok) {
        fprintf(stderr, "bfrops line is \"%s\", expected \"v41,v21\"\n",
                (NULL == bf) ? "(missing)" : bf);
    }
    if (NULL == ps || NULL == strstr(ps, "native")) {
        fprintf(stderr, "psec line is \"%s\", expected one naming native\n",
                (NULL == ps) ? "(missing)" : ps);
        ok = false;
    }
    free(bf);
    free(ps);
    PMIx_server_finalize();
    _exit(ok ? CHILD_PASS : CHILD_FAIL);
}

int main(int argc, char **argv)
{
    const char *base;
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);
#if PMIX_ENABLE_LEGACY_PEERS
    /* these cases are about the TCP listener, which a server opens for
     * clients and tools of earlier releases */
    setenv("PMIX_MCA_ptl_base_legacy_peers", "1", 1);
#else
    fprintf(stdout, "this PMIx takes no clients or tools of earlier releases, so a "
                    "server has no TCP listener for these cases\n");
    return 77;
#endif

    base = getenv("TMPDIR");
    if (NULL == base) {
        base = "/tmp";
    }
    snprintf(tmpdir, sizeof(tmpdir), "%s/pmix-lsn-XXXXXX", base);
    if (NULL == mkdtemp(tmpdir)) {
        fprintf(stderr, "could not create a temporary directory\n");
        return 1;
    }
    setenv("PMIX_SYSTEM_TMPDIR", tmpdir, 1);
    setenv("PMIX_SERVER_TMPDIR", tmpdir, 1);

    fprintf(stdout, "\n=== ptl listener unit tests ===\n\n");

    run_case("accept out of descriptors pauses, then accepts once descriptors are free",
             emfile_child);
    run_case("finalize while paused closes the listener, and the next cycle listens",
             emfile_finalize_child);
    run_case("running out of descriptors or memory is retried for as long as it lasts",
             resource_retry_child);
    run_case("an unknown accept error is retried 60 times, then the listener stops",
             unknown_retry_child);
    run_case("a listening descriptor that is not a socket stops the listener at once",
             notsock_child);
    run_case("a non-string PMIX_TCP_IF_INCLUDE is refused", bool_if_include_child);
    run_case("an include list matching no interface here is refused", include_nomatch_child);
    run_case("a help message is printed in a second init cycle", help_after_cycle_child);
    run_case("an include list holding an address with no prefix is refused",
             include_noprefix_child);
    run_case("a PMIX_TCP_IPV4_PORT past 65535 is refused", big_port_child);
    run_case("an empty PMIX_TCP_REPORT_URI leaves stdin alone", empty_report_uri_child);
    run_case("a report file named by directive is written by its server alone, and removed at "
             "finalize",
             report_uri_file_child);
    run_case("a report file is not written through a symlink", report_uri_symlink_child);
    run_case("a report file is not written through a second hard link",
             report_uri_hardlink_child);
    run_case("an existing report file is replaced", report_uri_replace_child);
    run_case("the report file lists the wire formats and security mechanisms this server runs",
             compat_lines_child);

    rmdir(tmpdir);
    fprintf(stdout, "\n%d passed, %d failed, %d skipped\n", npass, nfail, nskip);
    return (0 == nfail) ? 0 : 1;
}
