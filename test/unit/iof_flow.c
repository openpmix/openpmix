/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * IOF stdin flow control.
 *
 * A PMIx server reads stdin and hands it to its host through the
 * push_stdin upcall. Before flow control existed, that read was
 * re-armed unconditionally and the host's status was discarded, so a
 * host falling behind had no way to slow the producer - only to drop
 * bytes or queue without bound.
 *
 * This drives the real path in one process: a pipe stands in for the
 * server's stdin, and the host module here decides what each push is
 * answered with. It asserts the three things flow control has to get
 * right - that an XOFF actually stops the reading, that an XON restarts
 * it, and that a suspended stream loses nothing.
 */

#include "src/include/pmix_config.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "include/pmix_server.h"
#include "include/pmix_tool.h"

#define TEST_CHUNK 4000
#define WAIT_USEC  20000
#define WAIT_TRIES 250

static int failures = 0;

static void report(const char *what, bool pass)
{
    fprintf(stdout, "%-62s : %s\n", what, pass ? "PASS" : "FAIL");
    if (!pass) {
        ++failures;
    }
}

/* everything the host module records, read by the main thread */
static pthread_mutex_t hostlock = PTHREAD_MUTEX_INITIALIZER;
static unsigned char *received = NULL;
static size_t nreceived = 0;
static size_t received_size = 0;
static size_t npushes = 0;
static bool xoff_next = false;  /* answer the next push with XOFF */
static bool xoff_sent = false;  /* we have done so */

static size_t bytes_received(void)
{
    size_t n;
    pthread_mutex_lock(&hostlock);
    n = nreceived;
    pthread_mutex_unlock(&hostlock);
    return n;
}

static bool xoff_was_sent(void)
{
    bool b;
    pthread_mutex_lock(&hostlock);
    b = xoff_sent;
    pthread_mutex_unlock(&hostlock);
    return b;
}

static void arm_xoff(void)
{
    pthread_mutex_lock(&hostlock);
    xoff_next = true;
    xoff_sent = false;
    pthread_mutex_unlock(&hostlock);
}

/* the host: accumulate what we are handed, and answer either "keep
 * going" or "I have taken this, now stop" */
static pmix_status_t my_push_stdin(const pmix_proc_t *source,
                                   const pmix_proc_t targets[], size_t ntargets,
                                   const pmix_info_t directives[], size_t ndirs,
                                   const pmix_byte_object_t *bo,
                                   pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    pmix_status_t ret = PMIX_SUCCESS;
    (void) source;
    (void) targets;
    (void) ntargets;
    (void) directives;
    (void) ndirs;

    pthread_mutex_lock(&hostlock);
    if (NULL != bo && 0 < bo->size) {
        if (nreceived + bo->size <= received_size) {
            memcpy(&received[nreceived], bo->bytes, bo->size);
            nreceived += bo->size;
        }
        ++npushes;
        if (xoff_next) {
            xoff_next = false;
            xoff_sent = true;
            ret = PMIX_ERR_IOF_XOFF;
        }
    }
    pthread_mutex_unlock(&hostlock);

    /* we always answer through the callback, so report success to say
     * the callback is ours to make */
    if (NULL != cbfunc) {
        cbfunc(ret, cbdata);
    }
    return PMIX_SUCCESS;
}

static pmix_server_module_t mymodule = {
    .push_stdin = my_push_stdin
};

/* fill buf with a position-dependent pattern so the receiving end can
 * tell a lost chunk from a duplicated or reordered one */
static void pattern(unsigned char *buf, size_t offset, size_t len)
{
    size_t n;
    for (n = 0; n < len; n++) {
        buf[n] = (unsigned char) ((offset + n) % 251);
    }
}

static bool pattern_ok(size_t len)
{
    size_t n;
    bool ok = true;

    pthread_mutex_lock(&hostlock);
    for (n = 0; n < len && n < nreceived; n++) {
        if (received[n] != (unsigned char) (n % 251)) {
            ok = false;
            break;
        }
    }
    pthread_mutex_unlock(&hostlock);
    return ok;
}

/* wait until at least "target" bytes have arrived, or we run out of
 * patience. Returns what actually arrived */
static size_t wait_for(size_t target)
{
    int i;
    size_t got = bytes_received();

    for (i = 0; i < WAIT_TRIES && got < target; i++) {
        usleep(WAIT_USEC);
        got = bytes_received();
    }
    return got;
}

static bool wait_for_xoff(void)
{
    int i;

    for (i = 0; i < WAIT_TRIES; i++) {
        if (xoff_was_sent()) {
            return true;
        }
        usleep(WAIT_USEC);
    }
    return false;
}

/* let the progress thread have a good run at whatever is in the pipe -
 * used where the assertion is that it does NOT read it */
static void settle(void)
{
    int i;
    for (i = 0; i < 25; i++) {
        usleep(WAIT_USEC);
    }
}

static int wfd = -1;
static size_t written = 0;

static bool feed(void)
{
    unsigned char buf[TEST_CHUNK];
    ssize_t n;

    pattern(buf, written, sizeof(buf));
    n = write(wfd, buf, sizeof(buf));
    if (n != (ssize_t) sizeof(buf)) {
        fprintf(stderr, "short write to pipe: %d (%s)\n", (int) n, strerror(errno));
        return false;
    }
    written += sizeof(buf);
    return true;
}

int main(int argc, char **argv)
{
    pmix_status_t rc;
    pmix_proc_t target;
    pmix_info_t dir;
    int pipefd[2];
    size_t got;
    (void) argc;
    (void) argv;

    fprintf(stdout, "\n=== PMIx IOF stdin flow control unit test ===\n\n");

    received_size = 16 * TEST_CHUNK;
    received = (unsigned char *) malloc(received_size);
    if (NULL == received) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }

    /* stand a pipe up in place of our stdin before the library goes
     * looking for it - the read event is built on fileno(stdin) */
    if (0 != pipe(pipefd)) {
        fprintf(stderr, "pipe failed: %s\n", strerror(errno));
        return 1;
    }
    if (0 > dup2(pipefd[0], fileno(stdin))) {
        fprintf(stderr, "dup2 failed: %s\n", strerror(errno));
        return 1;
    }
    close(pipefd[0]);
    wfd = pipefd[1];

    rc = PMIx_server_init(&mymodule, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    /* the channel check comes before anything else, and does not need a
     * stream to be running */
    rc = PMIx_server_IOF_flow_control(NULL, PMIX_FWD_STDOUT_CHANNEL, true, NULL, 0, NULL, NULL);
    report("flow control on a non-stdin channel is refused", PMIX_ERR_NOT_SUPPORTED == rc);

    /* start collecting our stdin */
    PMIX_LOAD_PROCID(&target, "flowtest", 0);
    PMIX_INFO_LOAD(&dir, PMIX_IOF_PUSH_STDIN, NULL, PMIX_BOOL);
    rc = PMIx_IOF_push(&target, 1, NULL, &dir, 1, NULL, NULL);
    PMIX_INFO_DESTRUCT(&dir);
    if (PMIX_SUCCESS != rc && PMIX_OPERATION_SUCCEEDED != rc) {
        fprintf(stderr, "PMIx_IOF_push(PUSH_STDIN) failed: %s\n", PMIx_Error_string(rc));
        PMIx_server_finalize();
        return 1;
    }

    /* ---- baseline: an accepting host gets everything ---- */
    if (!feed()) {
        PMIx_server_finalize();
        return 1;
    }
    got = wait_for(TEST_CHUNK);
    report("stdin reaches the host when nothing is throttled", TEST_CHUNK <= got);

    /* ---- the host answers a push with XOFF ---- */
    arm_xoff();
    if (!feed()) {
        PMIx_server_finalize();
        return 1;
    }
    report("host XOFF is delivered to the library", wait_for_xoff());
    got = wait_for(2 * TEST_CHUNK);
    report("the chunk that drew the XOFF is still delivered", 2 * TEST_CHUNK <= got);

    /* ---- and the reading stops: this is the whole point ---- */
    got = bytes_received();
    if (!feed()) {
        PMIx_server_finalize();
        return 1;
    }
    settle();
    report("no stdin is read while the stream is suspended", got == bytes_received());

    /* ---- an XON restarts it, and nothing was lost ---- */
    rc = PMIx_server_IOF_flow_control(NULL, PMIX_FWD_STDIN_CHANNEL, false, NULL, 0, NULL, NULL);
    report("XON is accepted", PMIX_SUCCESS == rc || PMIX_OPERATION_SUCCEEDED == rc);
    got = wait_for(3 * TEST_CHUNK);
    report("suspended stdin is delivered after the XON", 3 * TEST_CHUNK <= got);
    report("nothing was lost, duplicated or reordered", pattern_ok(3 * TEST_CHUNK));

    /* ---- the host can also assert XOFF on its own, with no push in
     * flight to answer ---- */
    rc = PMIx_server_IOF_flow_control(NULL, PMIX_FWD_STDIN_CHANNEL, true, NULL, 0, NULL, NULL);
    report("unsolicited XOFF is accepted", PMIX_SUCCESS == rc || PMIX_OPERATION_SUCCEEDED == rc);
    got = bytes_received();
    if (!feed()) {
        PMIx_server_finalize();
        return 1;
    }
    settle();
    report("unsolicited XOFF stops the reading too", got == bytes_received());

    rc = PMIx_server_IOF_flow_control(NULL, PMIX_FWD_STDIN_CHANNEL, false, NULL, 0, NULL, NULL);
    got = wait_for(4 * TEST_CHUNK);
    report("the stream resumes again", 4 * TEST_CHUNK <= got);
    report("still nothing lost across two suspensions", pattern_ok(4 * TEST_CHUNK));

    close(wfd);
    PMIx_server_finalize();
    free(received);

    fprintf(stdout, "\n%s\n", (0 == failures) ? "ALL PASSED" : "FAILURES DETECTED");
    return (0 == failures) ? 0 : 1;
}
