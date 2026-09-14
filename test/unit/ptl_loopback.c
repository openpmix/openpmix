/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit tests for a server sending a request to itself.
 *
 * A server's active server is its own peer, and a sendrecv to that peer
 * never touches a socket: the request is posted straight to the matching
 * code and must reach the server's own command switchyard, and the
 * switchyard's answer must come back to the recv waiting for it. None of
 * that worked:
 *
 *  - the send found the server's peer with no socket and never got as far
 *    as its loopback branch, so the request was never delivered;
 *
 *  - had it been, it would have matched the reply recv posted for it on
 *    the same tag, ahead of the switchyard's wildcard, and been handed to
 *    the caller as its own answer;
 *
 *  - and the switchyard's reply was queued on a peer with no socket to
 *    send it on, where it sat for the life of the process.
 *
 * The last case pins the guard that keeps a loopback reply nobody is
 * waiting for from being read as a command: without it the error that
 * draws is itself a reply to ourselves, and the two go round forever.
 */

#include "src/include/pmix_config.h"

#include "include/pmix_server.h"
#include "src/include/pmix_globals.h"
#include "src/mca/ptl/base/base.h"
#include "src/threads/pmix_threads.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int npass = 0;
static int nfail = 0;
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

/* what a reply callback saw */
typedef struct {
    volatile int calls;
    size_t nbytes;
    bool have_status;
    pmix_status_t status;
} probe_t;

static void probe_cb(struct pmix_peer_t *peer, pmix_ptl_hdr_t *hdr, pmix_buffer_t *buf,
                     void *cbdata)
{
    probe_t *p = (probe_t *) cbdata;
    pmix_status_t rc;
    int32_t cnt = 1;
    PMIX_HIDE_UNUSED_PARAMS(peer, hdr);

    p->nbytes = buf->bytes_used;
    if (0 < buf->bytes_used) {
        PMIX_BFROPS_UNPACK(rc, pmix_globals.mypeer, buf, &p->status, &cnt, PMIX_STATUS);
        p->have_status = (PMIX_SUCCESS == rc);
    }
    ++p->calls;
}

static int wait_calls(probe_t *p)
{
    int i;
    for (i = 0; i < 200 && 0 == p->calls; i++) {
        pause_ms(10);
    }
    /* and a moment more, so a second delivery would be counted */
    pause_ms(100);
    return p->calls;
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

static void check_nothing_queued(void *arg)
{
    bool *empty = (bool *) arg;
    *empty = (NULL == pmix_globals.mypeer->send_msg &&
              0 == pmix_list_get_size(&pmix_globals.mypeer->send_queue));
}

/* send one command to ourselves and report what came back */
static void self_request(const char *name, pmix_cmd_t cmd, pmix_status_t expect)
{
    pmix_buffer_t *buf;
    pmix_status_t rc;
    probe_t probe;
    bool empty = false;
    char detail[128];

    memset(&probe, 0, sizeof(probe));
    buf = PMIX_NEW(pmix_buffer_t);
    PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &cmd, 1, PMIX_COMMAND);
    PMIX_PTL_SEND_RECV(rc, pmix_globals.mypeer, buf, probe_cb, &probe);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(buf);
        report(name, 0, PMIx_Error_string(rc));
        return;
    }
    wait_calls(&probe);
    on_progress_thread(check_nothing_queued, &empty);
    snprintf(detail, sizeof(detail), "%d calls, %lu bytes, status %s, queued reply %s",
             probe.calls, (unsigned long) probe.nbytes,
             probe.have_status ? PMIx_Error_string(probe.status) : "(none)",
             empty ? "no" : "yes");
    report(name,
           1 == probe.calls && probe.have_status && expect == probe.status && empty,
           detail);
}

/* ---- a loopback reply nobody waits for is not a command ---- */

typedef struct {
    pmix_ptl_posted_recv_t *rcv;
    probe_t *probe;
} intercept_t;

static void post_intercept(void *arg)
{
    intercept_t *i = (intercept_t *) arg;

    /* a wildcard recv ahead of the switchyard's - anything that would
     * reach the switchyard reaches this first */
    i->rcv = PMIX_NEW(pmix_ptl_posted_recv_t);
    i->rcv->tag = UINT32_MAX;
    i->rcv->cbfunc = probe_cb;
    i->rcv->cbdata = i->probe;
    pmix_list_prepend(&pmix_ptl_base.posted_recvs, &i->rcv->super);
}

static void queue_unwanted_reply(void *arg)
{
    pmix_buffer_t *reply = PMIX_NEW(pmix_buffer_t);
    pmix_status_t rc, st = PMIX_SUCCESS;
    PMIX_HIDE_UNUSED_PARAMS(arg);

    PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, reply, &st, 1, PMIX_STATUS);
    PMIX_SERVER_QUEUE_REPLY(rc, pmix_globals.mypeer, PMIX_PTL_TAG_DYNAMIC + 4242, reply);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(reply);
    }
}

static void remove_intercept(void *arg)
{
    intercept_t *i = (intercept_t *) arg;
    pmix_list_remove_item(&pmix_ptl_base.posted_recvs, &i->rcv->super);
    PMIX_RELEASE(i->rcv);
}

static void test_unwanted_reply(void)
{
    probe_t probe;
    intercept_t icpt = {NULL, &probe};
    bool empty = false;
    char detail[64];

    memset(&probe, 0, sizeof(probe));
    on_progress_thread(post_intercept, &icpt);
    on_progress_thread(queue_unwanted_reply, NULL);
    pause_ms(300);
    on_progress_thread(remove_intercept, &icpt);
    on_progress_thread(check_nothing_queued, &empty);
    snprintf(detail, sizeof(detail), "wildcard saw it %d times, queued %s", probe.calls,
             empty ? "no" : "yes");
    report("a loopback reply nobody waits for is not read as a command",
           0 == probe.calls && empty, detail);
}

int main(int argc, char **argv)
{
    pmix_status_t rc;
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    rc = PMIx_server_init(&mymodule, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    fprintf(stdout, "\n=== ptl loopback (server to itself) unit tests ===\n\n");

    /* no switchyard arm knows this one, so the reply is the status-only
     * error pmix_server_message_handler sends */
    self_request("a request to ourselves reaches the switchyard and is answered once",
                 (pmix_cmd_t) 255, PMIX_ERR_NOT_SUPPORTED);
    test_unwanted_reply();

    fprintf(stdout, "\n%d passed, %d failed\n", npass, nfail);
    PMIx_server_finalize();
    return (0 == nfail) ? 0 : 1;
}
