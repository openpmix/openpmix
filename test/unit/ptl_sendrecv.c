/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit tests for the ptl's steady-state receive path and its handling of
 * a lost connection.
 *
 * A tool can be connected to more than one server and switch which one is
 * its primary, so the posted recvs waiting on sendrecv replies can belong
 * to several peers at once. The cases here stand two fake server peers up
 * on socketpairs, arm the real pmix_ptl_base_recv_handler on each, and
 * drive it by writing to - or closing - the other end:
 *
 *  - losing the primary used to complete EVERY outstanding sendrecv with
 *    an empty buffer, including those waiting on the other server, and to
 *    leave them all posted. That other server's real reply then ran the
 *    callback a second time, on a caddy the first run had released.
 *
 *  - losing a server that was not the primary completed nothing, so every
 *    request outstanding on it waited forever.
 *
 *  - a header that arrived in two pieces lost its first piece: the handler
 *    read it into a local copy that each call started afresh, so the rest
 *    of the stream was read from the wrong offset.
 *
 *  - a sendrecv reaching a peer whose socket had already closed was
 *    dropped without its callback, so a caller blocked on it never woke.
 *    The same goes for a request a tool sends itself: it has no server
 *    half to read it, so it is answered empty rather than looped back to
 *    wait forever. (A server's request to itself is ptl_loopback.c.)
 *
 *  - a buffer too large for the header's 32-bit length was framed anyway,
 *    with the length truncated. It is now refused before it is queued;
 *    the case only claims the size, it does not allocate it.
 *
 *  - no single writev may exceed pmix_ptl_base.max_write, since macOS
 *    refuses one totalling more than INT_MAX. Reproducing that needs a
 *    2 GB message, so the case lowers the cap to a few bytes instead and
 *    checks that a message split across many writes arrives intact -
 *    which is what the chunking arithmetic has to get right.
 *
 *  - pmix_ptl_base_flush_sends, draining a peer that is not reading, put
 *    its socket in an fd_set whatever the descriptor's number. Past
 *    FD_SETSIZE that writes beyond the set, on the stack. This case runs
 *    last, since against the unfixed library it can take the process down
 *    rather than fail.
 *
 * Each case fails against the library without its fix.
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "include/pmix_tool.h"
#include "src/client/pmix_client_ops.h"
#include "src/include/pmix_globals.h"
#include "src/mca/ptl/base/base.h"
#include "src/threads/pmix_threads.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

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

/* one posted recv under test: how often its callback ran, and what the
 * last run was handed */
typedef struct {
    pmix_ptl_posted_recv_t *rcv;
    volatile int calls;
    size_t nbytes;
    char data[16];
} probe_t;

static void probe_cb(struct pmix_peer_t *peer, pmix_ptl_hdr_t *hdr, pmix_buffer_t *buf,
                     void *cbdata)
{
    probe_t *p = (probe_t *) cbdata;
    PMIX_HIDE_UNUSED_PARAMS(peer, hdr);

    p->nbytes = buf->bytes_used;
    memset(p->data, 0, sizeof(p->data));
    if (0 < buf->bytes_used && buf->bytes_used < sizeof(p->data)) {
        memcpy(p->data, buf->base_ptr, buf->bytes_used);
    }
    ++p->calls;
}

typedef struct {
    pmix_object_t super;
    pmix_event_t ev;
    pmix_lock_t lock;
    void (*fn)(void *arg);
    void *arg;
} shift_t;
static PMIX_CLASS_INSTANCE(shift_t, pmix_object_t, NULL, NULL);

static void shift_hdlr(int sd, short args, void *cbdata)
{
    shift_t *s = (shift_t *) cbdata;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);
    s->fn(s->arg);
    PMIX_WAKEUP_THREAD(&s->lock);
}

/* run fn on the progress thread, which owns the peers and the recv list */
static void on_progress_thread(void (*fn)(void *arg), void *arg)
{
    shift_t *s = PMIX_NEW(shift_t);
    PMIX_CONSTRUCT_LOCK(&s->lock);
    s->fn = fn;
    s->arg = arg;
    PMIX_THREADSHIFT(s, shift_hdlr);
    PMIX_WAIT_THREAD(&s->lock);
    PMIX_DESTRUCT_LOCK(&s->lock);
    PMIX_RELEASE(s);
}

static void pause_ms(long ms)
{
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

/* wait up to two seconds for a callback count to reach n */
static int wait_calls(probe_t *p, int n)
{
    int i;
    for (i = 0; i < 200 && p->calls < n; i++) {
        pause_ms(10);
    }
    return p->calls;
}

typedef struct {
    pmix_peer_t *peer;
    int far;    // the end the test writes to or closes
    const char *name;
} fake_t;

static void make_peer(void *arg)
{
    fake_t *f = (fake_t *) arg;
    pmix_peer_t *peer;
    int sv[2];

    if (0 != socketpair(AF_UNIX, SOCK_STREAM, 0, sv)) {
        perror("socketpair");
        exit(1);
    }
    peer = PMIX_NEW(pmix_peer_t);
    peer->nptr = PMIX_NEW(pmix_namespace_t);
    peer->nptr->nspace = strdup(f->name);
    peer->nptr->compat.type = pmix_globals.mypeer->nptr->compat.type;
    peer->nptr->compat.bfrops = pmix_globals.mypeer->nptr->compat.bfrops;
    peer->info = PMIX_NEW(pmix_rank_info_t);
    peer->info->pname.nspace = strdup(f->name);
    peer->info->pname.rank = 0;
    peer->sd = sv[0];
    pmix_ptl_base_set_nonblocking(peer->sd);
    pmix_event_assign(&peer->recv_event, pmix_globals.evbase, peer->sd, EV_READ | EV_PERSIST,
                      pmix_ptl_base_recv_handler, peer);
    pmix_event_add(&peer->recv_event, 0);
    peer->recv_ev_active = true;
    f->peer = peer;
    f->far = sv[1];
}

typedef struct {
    probe_t *probe;
    pmix_peer_t *peer;
    pmix_ptl_tag_t tag;
} post_t;

static void post_recv(void *arg)
{
    post_t *p = (post_t *) arg;
    pmix_ptl_posted_recv_t *rcv = PMIX_NEW(pmix_ptl_posted_recv_t);

    rcv->peer = p->peer;
    rcv->tag = p->tag;
    rcv->cbfunc = probe_cb;
    rcv->cbdata = p->probe;
    p->probe->rcv = rcv;
    pmix_list_prepend(&pmix_ptl_base.posted_recvs, &rcv->super);
}

static void post(probe_t *probe, pmix_peer_t *peer, pmix_ptl_tag_t tag)
{
    post_t p = {probe, peer, tag};
    memset(probe, 0, sizeof(*probe));
    on_progress_thread(post_recv, &p);
}

typedef struct {
    probe_t *probe;
    bool posted;
} find_t;

static void find_recv(void *arg)
{
    find_t *f = (find_t *) arg;
    pmix_ptl_posted_recv_t *rcv;

    f->posted = false;
    PMIX_LIST_FOREACH (rcv, &pmix_ptl_base.posted_recvs, pmix_ptl_posted_recv_t) {
        if (rcv == f->probe->rcv) {
            f->posted = true;
        }
    }
}

static bool still_posted(probe_t *probe)
{
    find_t f = {probe, false};
    on_progress_thread(find_recv, &f);
    return f.posted;
}

static void set_primary(void *arg)
{
    pmix_client_globals.myserver = (pmix_peer_t *) arg;
}

/* a steady-state header, in network order, as send_msg writes it */
static void write_msg(int fd, pmix_ptl_tag_t tag, const char *payload, size_t split)
{
    pmix_ptl_hdr_t hdr;
    char wire[64];
    size_t len = strlen(payload), total = sizeof(hdr) + len;

    memset(&hdr, 0, sizeof(hdr));
    hdr.pindex = htonl(0);
    hdr.tag = htonl(tag);
    hdr.nbytes = htonl((uint32_t) len);
    memcpy(wire, &hdr, sizeof(hdr));
    memcpy(wire + sizeof(hdr), payload, len);
    if (0 < split) {
        if ((ssize_t) split != write(fd, wire, split)) {
            perror("write");
        }
        /* long enough for the handler to read the piece and run dry */
        pause_ms(200);
    }
    if ((ssize_t) (total - split) != write(fd, wire + split, total - split)) {
        perror("write");
    }
}

typedef struct {
    pmix_peer_t *peer;
    probe_t *probe;
    pmix_status_t rc;
} sr_t;

static void do_sendrecv(void *arg)
{
    sr_t *s = (sr_t *) arg;
    pmix_buffer_t *buf = PMIX_NEW(pmix_buffer_t);

    PMIX_PTL_SEND_RECV(s->rc, s->peer, buf, probe_cb, s->probe);
    if (PMIX_SUCCESS != s->rc) {
        PMIX_RELEASE(buf);
    }
}

typedef struct {
    pmix_peer_t *peer;
    pmix_buffer_t *buf;
    pmix_ptl_tag_t tag;
    size_t cap;
    pmix_status_t rc;
} oneway_t;

static void arm_send(void *arg)
{
    pmix_peer_t *peer = (pmix_peer_t *) arg;
    pmix_event_assign(&peer->send_event, pmix_globals.evbase, peer->sd, EV_WRITE | EV_PERSIST,
                      pmix_ptl_base_send_handler, peer);
}

static void do_oneway(void *arg)
{
    oneway_t *o = (oneway_t *) arg;
    pmix_ptl_base.max_write = o->cap;
    PMIX_PTL_SEND_ONEWAY(o->rc, o->peer, o->buf, o->tag);
}

/* Release a fake peer on the progress thread, before its far end is
 * closed: the peer's recv event is armed, so a far end closed first makes
 * the progress thread tear the peer down while this thread frees it. */
static void release_peer(void *arg)
{
    fake_t *f = (fake_t *) arg;
    PMIX_RELEASE(f->peer);
}

static void reset_cap(void *arg)
{
    PMIX_HIDE_UNUSED_PARAMS(arg);
    pmix_ptl_base.max_write = INT_MAX;
}

/* read exactly len bytes, or give up after the socket's receive timeout */
static size_t read_all(int fd, char *ptr, size_t len)
{
    size_t got = 0;
    ssize_t rc;

    while (got < len) {
        rc = read(fd, ptr + got, len - got);
        if (0 >= rc) {
            break;
        }
        got += (size_t) rc;
    }
    return got;
}

#define CHUNKED_PAYLOAD 5000

static void test_chunked_write(void)
{
    fake_t c = {NULL, -1, "server.c"};
    oneway_t o;
    pmix_ptl_hdr_t hdr;
    char *payload, *got;
    struct timeval tv = {5, 0};
    size_t i, n;
    bool intact, hdr_ok;
    char detail[128];

    on_progress_thread(make_peer, &c);
    on_progress_thread(arm_send, c.peer);
    setsockopt(c.far, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    payload = (char *) malloc(CHUNKED_PAYLOAD);
    got = (char *) malloc(CHUNKED_PAYLOAD);
    for (i = 0; i < CHUNKED_PAYLOAD; i++) {
        payload[i] = (char) (i % 251);
    }
    o.peer = c.peer;
    o.buf = PMIX_NEW(pmix_buffer_t);
    o.buf->base_ptr = payload;
    o.buf->bytes_allocated = CHUNKED_PAYLOAD;
    o.buf->bytes_used = CHUNKED_PAYLOAD;
    o.tag = PMIX_PTL_TAG_DYNAMIC + 7;
    /* smaller than the header, so every region is split */
    o.cap = 7;
    on_progress_thread(do_oneway, &o);

    n = read_all(c.far, (char *) &hdr, sizeof(hdr));
    hdr_ok = (sizeof(hdr) == n && PMIX_PTL_TAG_DYNAMIC + 7 == ntohl(hdr.tag) &&
              CHUNKED_PAYLOAD == ntohl(hdr.nbytes));
    intact = hdr_ok;
    n = 0;
    i = 0;
    if (hdr_ok) {
        n = read_all(c.far, got, CHUNKED_PAYLOAD);
        for (i = 0; i < CHUNKED_PAYLOAD && i < n; i++) {
            if (got[i] != (char) (i % 251)) {
                break;
            }
        }
        intact = (CHUNKED_PAYLOAD == n && CHUNKED_PAYLOAD == i);
    }
    snprintf(detail, sizeof(detail), "rc %s, header %s, %lu payload bytes, first bad byte %lu",
             PMIx_Error_string(o.rc), hdr_ok ? "ok" : "wrong", (unsigned long) n,
             (unsigned long) i);
    report("a message split across capped writes arrives intact",
           PMIX_SUCCESS == o.rc && intact, detail);
    on_progress_thread(reset_cap, NULL);

    free(got);
    on_progress_thread(release_peer, &c);
    close(c.far);
}

#if SIZEOF_SIZE_T > 4
static void test_too_big(void)
{
    fake_t d = {NULL, -1, "server.d"};
    probe_t big;
    sr_t sr;
    pmix_buffer_t *buf;
    struct timeval tv = {0, 300000};
    char byte;
    ssize_t n;
    char detail[128];

    on_progress_thread(make_peer, &d);
    on_progress_thread(arm_send, d.peer);
    setsockopt(d.far, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    memset(&big, 0, sizeof(big));
    buf = PMIX_NEW(pmix_buffer_t);
    /* claimed, never allocated - the send must refuse it before it looks */
    buf->bytes_used = (size_t) UINT32_MAX + 1;
    PMIX_RETAIN(d.peer);
    sr.peer = d.peer;
    sr.probe = &big;
    PMIX_PTL_SEND_RECV(sr.rc, d.peer, buf, probe_cb, &big);
    PMIX_RELEASE(d.peer);
    wait_calls(&big, 1);
    n = read(d.far, &byte, 1);
    snprintf(detail, sizeof(detail), "rc %s, %d calls, %s on the wire", PMIx_Error_string(sr.rc),
             big.calls, (0 < n) ? "something" : "nothing");
    report("a buffer too large to frame is refused and its caller answered",
           PMIX_SUCCESS == sr.rc && 1 == big.calls && 0 >= n, detail);

    on_progress_thread(release_peer, &d);
    close(d.far);
}
#endif

#define HIGH_FD 4000

typedef struct {
    pmix_peer_t *peer;
    bool drained;
} flush_t;

static void do_flush(void *arg)
{
    flush_t *f = (flush_t *) arg;
    pmix_ptl_base_flush_sends(f->peer);
    f->drained = (NULL == f->peer->send_msg);
}

static void test_flush_high_fd(void)
{
    struct rlimit rl;
    int sv[2];
    char chunk[4096];
    pmix_ptl_send_t *snd;
    pmix_buffer_t *buf;
    flush_t f;
    const size_t payload = 1024 * 1024;

    if (0 != getrlimit(RLIMIT_NOFILE, &rl)) {
        fprintf(stdout, "  SKIP: flush_sends with a descriptor past FD_SETSIZE (getrlimit)\n");
        return;
    }
    if (rl.rlim_cur <= HIGH_FD) {
        rl.rlim_cur = HIGH_FD + 1;
        if ((RLIM_INFINITY != rl.rlim_max && rl.rlim_max <= HIGH_FD) ||
            0 != setrlimit(RLIMIT_NOFILE, &rl)) {
            fprintf(stdout, "  SKIP: flush_sends with a descriptor past FD_SETSIZE "
                            "(cannot raise the descriptor limit)\n");
            return;
        }
    }
    if (HIGH_FD < FD_SETSIZE) {
        fprintf(stdout, "  SKIP: flush_sends with a descriptor past FD_SETSIZE (FD_SETSIZE %d)\n",
                (int) FD_SETSIZE);
        return;
    }
    if (0 != socketpair(AF_UNIX, SOCK_STREAM, 0, sv) || HIGH_FD != dup2(sv[0], HIGH_FD)) {
        perror("socketpair/dup2");
        ++nfail;
        return;
    }
    close(sv[0]);
    /* fill the socket so the flush has to wait - nobody reads sv[1] */
    fcntl(HIGH_FD, F_SETFL, fcntl(HIGH_FD, F_GETFL) | O_NONBLOCK);
    memset(chunk, 0, sizeof(chunk));
    while (0 < write(HIGH_FD, chunk, sizeof(chunk))) {
    }

    f.peer = PMIX_NEW(pmix_peer_t);
    f.peer->sd = HIGH_FD;
    buf = PMIX_NEW(pmix_buffer_t);
    buf->base_ptr = (char *) calloc(1, payload);
    buf->bytes_allocated = payload;
    buf->bytes_used = payload;
    snd = PMIX_NEW(pmix_ptl_send_t);
    snd->hdr.tag = htonl(PMIX_PTL_TAG_DYNAMIC);
    snd->hdr.nbytes = htonl((uint32_t) payload);
    snd->data = buf;
    snd->sdptr = (char *) &snd->hdr;
    snd->sdbytes = sizeof(pmix_ptl_hdr_t);
    f.peer->send_msg = snd;
    f.drained = false;

    on_progress_thread(do_flush, &f);
    report("flush_sends gives up on a stalled descriptor past FD_SETSIZE", f.drained,
           "message still queued");

    PMIX_RELEASE(f.peer);   // closes HIGH_FD
    close(sv[1]);
}

int main(int argc, char **argv)
{
    pmix_proc_t myproc;
    pmix_info_t info;
    pmix_status_t rc;
    fake_t a = {NULL, -1, "server.a"}, b = {NULL, -1, "server.b"};
    pmix_peer_t *primary;
    probe_t pa, pb, split, onb, late;
    sr_t sr;
    char detail[128];
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    PMIX_INFO_LOAD(&info, PMIX_TOOL_DO_NOT_CONNECT, NULL, PMIX_BOOL);
    rc = PMIx_tool_init(&myproc, &info, 1);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_tool_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    fprintf(stdout, "\n=== ptl steady-state receive unit tests ===\n\n");

    on_progress_thread(make_peer, &a);
    on_progress_thread(make_peer, &b);
    primary = pmix_client_globals.myserver;

    /* both servers have a sendrecv outstanding on the same tag - each
     * connection has a tag space of its own */
    post(&pa, a.peer, PMIX_PTL_TAG_DYNAMIC + 1);
    post(&pb, b.peer, PMIX_PTL_TAG_DYNAMIC + 1);

    /* ---- losing the primary completes only the primary's recvs ---- */
    on_progress_thread(set_primary, a.peer);
    close(a.far);
    wait_calls(&pa, 1);
    snprintf(detail, sizeof(detail), "%d calls", pa.calls);
    report("lost primary's outstanding sendrecv is completed", 1 == pa.calls, detail);
    report("lost primary's completed recv is taken off the list", !still_posted(&pa),
           "still posted");
    pause_ms(100);
    snprintf(detail, sizeof(detail), "%d calls", pb.calls);
    report("the other server's outstanding sendrecv is left alone", 0 == pb.calls, detail);
    on_progress_thread(set_primary, primary);

    /* ---- a header that arrives in two pieces ---- */
    post(&split, b.peer, PMIX_PTL_TAG_DYNAMIC + 2);
    write_msg(b.far, PMIX_PTL_TAG_DYNAMIC + 2, "hello", 5);
    wait_calls(&split, 1);
    snprintf(detail, sizeof(detail), "%d calls, %lu bytes \"%s\"", split.calls,
             (unsigned long) split.nbytes, split.data);
    report("a split header still delivers its message",
           1 == split.calls && 5 == split.nbytes && 0 == strcmp(split.data, "hello"), detail);

    /* ---- the other server's real reply now arrives exactly once ---- */
    write_msg(b.far, PMIX_PTL_TAG_DYNAMIC + 1, "reply", 0);
    wait_calls(&pb, 1);
    pause_ms(100);
    snprintf(detail, sizeof(detail), "%d calls, %lu bytes", pb.calls, (unsigned long) pb.nbytes);
    report("the other server's reply runs its callback once, with the data",
           1 == pb.calls && 5 == pb.nbytes, detail);

    /* ---- losing a server that is not the primary ---- */
    post(&onb, b.peer, PMIX_PTL_TAG_DYNAMIC + 3);
    close(b.far);
    wait_calls(&onb, 1);
    snprintf(detail, sizeof(detail), "%d calls", onb.calls);
    report("lost non-primary server's outstanding sendrecv is completed", 1 == onb.calls,
           detail);

    /* ---- a sendrecv to a peer that is already gone is still answered ---- */
    memset(&late, 0, sizeof(late));
    sr.peer = a.peer;
    sr.probe = &late;
    on_progress_thread(do_sendrecv, &sr);
    wait_calls(&late, 1);
    snprintf(detail, sizeof(detail), "rc %s, %d calls", PMIx_Error_string(sr.rc), late.calls);
    report("a sendrecv to a closed peer is answered", PMIX_SUCCESS == sr.rc && 1 == late.calls,
           detail);

    /* ---- a tool's request to itself has nobody to answer it ---- */
    memset(&late, 0, sizeof(late));
    sr.peer = pmix_globals.mypeer;
    sr.probe = &late;
    on_progress_thread(do_sendrecv, &sr);
    wait_calls(&late, 1);
    snprintf(detail, sizeof(detail), "rc %s, %d calls", PMIx_Error_string(sr.rc), late.calls);
    report("a tool's sendrecv to itself is answered", PMIX_SUCCESS == sr.rc && 1 == late.calls,
           detail);

    PMIX_RELEASE(a.peer);
    PMIX_RELEASE(b.peer);

    test_chunked_write();
#if SIZEOF_SIZE_T > 4
    test_too_big();
#endif
    test_flush_high_fd();

    fprintf(stdout, "\n%d passed, %d failed\n", npass, nfail);
    PMIx_tool_finalize();
    return (0 == nfail) ? 0 : 1;
}
