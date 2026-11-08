/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * A tool and the servers other than its primary.
 *
 * A tool can be attached to several servers, each with its own wire
 * format (bfrops module and buffer type), chosen when its connection was
 * made. A tool with no primary at all - PMIX_TOOL_DO_NOT_CONNECT - has
 * itself as its primary.
 *
 * Events: the tool comes up with PMIX_TOOL_DO_NOT_CONNECT and puts an
 * event through its own receive path (PMIX_PTL_TAG_NOTIFY) as though a
 * server it connected to had sent it. First from a server whose wire
 * format is the primary's, then from one whose buffer type differs (and
 * whose bfrops module does too, where an earlier one is built). The
 * tool's handler must see both, with the info the event carried.
 *
 * Attaching: the same tool then attaches to a real server (the parent
 * process) without asking for it as its primary. It must still report
 * itself not connected, since its primary is still itself; once it makes
 * that server its primary with PMIx_tool_set_server, it is connected.
 *
 * The tool runs in a forked child, which reports its own cases and exits
 * with the number that failed.
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "include/pmix_server.h"
#include "include/pmix_tool.h"
#include "src/client/pmix_client_ops.h"
#include "src/include/pmix_globals.h"
#include "src/mca/bfrops/bfrops.h"
#include "src/mca/ptl/base/base.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define SERVER_NSPACE "tool-servers-server"
#define OTHER_NSPACE "tool-servers-other"
#define EV_KEY       "tool.servers.key"
#define CODE_SAME    (PMIX_EXTERNAL_ERR_BASE - 31)
#define CODE_OTHER   (PMIX_EXTERNAL_ERR_BASE - 32)

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

/* ---------- putting a message on our own receive path ---------- */

typedef struct {
    pmix_object_t super;
    pmix_event_t ev;
    pmix_lock_t lock;
} barrier_t;
static PMIX_CLASS_INSTANCE(barrier_t, pmix_object_t, NULL, NULL);

static void barrier_cb(int sd, short args, void *cbdata)
{
    barrier_t *b = (barrier_t *) cbdata;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);
    PMIX_WAKEUP_THREAD(&b->lock);
}

/* everything already posted to the progress thread has run */
static void progress_barrier(void)
{
    barrier_t *b = PMIX_NEW(barrier_t);

    PMIX_CONSTRUCT_LOCK(&b->lock);
    PMIX_THREADSHIFT(b, barrier_cb);
    PMIX_WAIT_THREAD(&b->lock);
    PMIX_DESTRUCT_LOCK(&b->lock);
    PMIX_RELEASE(b);
}

/* hand "payload" to our receive path as a message "peer" sent on "tag" -
 * where a message read off that peer's socket goes */
static void deliver(pmix_peer_t *peer, pmix_ptl_tag_t tag, pmix_buffer_t *payload)
{
    pmix_ptl_recv_t *msg;
    size_t nbytes = 0;

    msg = PMIX_NEW(pmix_ptl_recv_t);
    PMIX_RETAIN(peer);
    msg->peer = peer;
    msg->hdr.tag = tag;
    PMIX_UNLOAD_BUFFER(payload, msg->data, nbytes);
    msg->hdr.nbytes = (uint32_t) nbytes;
    PMIX_RELEASE(payload);
    PMIX_THREADSHIFT(msg, pmix_ptl_base_process_msg);
    progress_barrier();
}

/* a server we connected to, speaking the given wire format */
static pmix_peer_t *server_peer(const char *nspace, pmix_bfrops_module_t *bfrops,
                                pmix_bfrop_buffer_type_t type)
{
    pmix_peer_t *p = PMIX_NEW(pmix_peer_t);
    pmix_namespace_t *me = pmix_globals.mypeer->nptr;

    p->nptr = PMIX_NEW(pmix_namespace_t);
    p->nptr->nspace = strdup(nspace);
    p->nptr->compat.bfrops = bfrops;
    p->nptr->compat.type = type;
    p->nptr->compat.psec = me->compat.psec;
    p->nptr->compat.gds = me->compat.gds;
    p->gds = me->compat.gds;
    p->info = PMIX_NEW(pmix_rank_info_t);
    p->info->pname.nspace = strdup(nspace);
    p->info->pname.rank = 0;
    PMIX_SET_PEER_TYPE(p, PMIX_PROC_SERVER);
    p->upstream = true;
    return p;
}

static pmix_buffer_t *event_msg(pmix_peer_t *server, pmix_status_t code, const char *value)
{
    pmix_buffer_t *msg = PMIX_NEW(pmix_buffer_t);
    pmix_cmd_t cmd = PMIX_NOTIFY_CMD;
    pmix_proc_t source;
    pmix_data_range_t range = PMIX_RANGE_LOCAL;
    pmix_info_t info;
    size_t ninfo = 1;
    pmix_status_t rc;

    PMIX_LOAD_PROCID(&source, server->info->pname.nspace, 0);
    PMIX_INFO_LOAD(&info, EV_KEY, value, PMIX_STRING);
    PMIX_BFROPS_PACK(rc, server, msg, &cmd, 1, PMIX_COMMAND);
    if (PMIX_SUCCESS == rc) {
        PMIX_BFROPS_PACK(rc, server, msg, &code, 1, PMIX_STATUS);
    }
    if (PMIX_SUCCESS == rc) {
        PMIX_BFROPS_PACK(rc, server, msg, &source, 1, PMIX_PROC);
    }
    if (PMIX_SUCCESS == rc) {
        PMIX_BFROPS_PACK(rc, server, msg, &ninfo, 1, PMIX_SIZE);
    }
    if (PMIX_SUCCESS == rc) {
        PMIX_BFROPS_PACK(rc, server, msg, &info, 1, PMIX_INFO);
    }
    if (PMIX_SUCCESS == rc) {
        PMIX_BFROPS_PACK(rc, server, msg, &range, 1, PMIX_DATA_RANGE);
    }
    PMIX_INFO_DESTRUCT(&info);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(msg);
        return NULL;
    }
    return msg;
}

/* ---------- the tool's handler ---------- */

static volatile int got_same = 0;
static volatile int got_other = 0;
static char seen_same[64];
static char seen_other[64];

static void record(char *dst, pmix_info_t info[], size_t ninfo)
{
    size_t n;

    dst[0] = '\0';
    for (n = 0; n < ninfo; n++) {
        if (PMIX_CHECK_KEY(&info[n], EV_KEY) && PMIX_STRING == info[n].value.type &&
            NULL != info[n].value.data.string) {
            snprintf(dst, 64, "%s", info[n].value.data.string);
        }
    }
}

static void count_events(size_t evhdlr_registration_id, pmix_status_t status,
                         const pmix_proc_t *source, pmix_info_t info[], size_t ninfo,
                         pmix_info_t results[], size_t nresults,
                         pmix_event_notification_cbfunc_fn_t cbfunc, void *cbdata)
{
    PMIX_HIDE_UNUSED_PARAMS(evhdlr_registration_id, source, results, nresults);

    if (CODE_SAME == status) {
        record(seen_same, info, ninfo);
        ++got_same;
    } else if (CODE_OTHER == status) {
        record(seen_other, info, ninfo);
        ++got_other;
    }
    if (NULL != cbfunc) {
        cbfunc(PMIX_EVENT_ACTION_COMPLETE, NULL, 0, NULL, NULL, cbdata);
    }
}

static int wait_for(volatile int *count)
{
    int i;

    for (i = 0; i < 300 && 0 == *count; i++) {
        progress_barrier();
        usleep(10000);
    }
    return *count;
}

/* ---------- the cases ---------- */

static void check_event_formats(void)
{
    pmix_status_t codes[2] = {CODE_SAME, CODE_OTHER};
    pmix_namespace_t *me = pmix_globals.mypeer->nptr;
    pmix_bfrops_module_t *other_bfrops = NULL;
    pmix_bfrop_buffer_type_t other_type;
    pmix_peer_t *same, *other;
    pmix_buffer_t *msg;
    pmix_status_t rc;
    char detail[320];

    rc = PMIx_Register_event_handler(codes, 2, NULL, 0, count_events, NULL, NULL);
    if (0 > rc) {
        report("register the event handler", 0, PMIx_Error_string(rc));
        return;
    }

    /* a server speaking what the primary does */
    same = server_peer(OTHER_NSPACE "-same", me->compat.bfrops, me->compat.type);
    msg = event_msg(same, CODE_SAME, "same");
    if (NULL == msg) {
        report("an event from a server of the primary's format", 0, "could not pack it");
    } else {
        deliver(same, PMIX_PTL_TAG_NOTIFY, msg);
        snprintf(detail, sizeof(detail), "handler ran %d times, info \"%s\"",
                 wait_for(&got_same), seen_same);
        report("an event from a server of the primary's format",
               1 == got_same && 0 == strcmp(seen_same, "same"), detail);
    }
    PMIX_RELEASE(same);

    /* a server speaking another: the other buffer type, and an earlier
     * bfrops module where one is built */
#if PMIX_ENABLE_LEGACY_SUPPORT
    other_bfrops = pmix_bfrops_base_assign_module("v41");
#endif
    if (NULL == other_bfrops) {
        other_bfrops = me->compat.bfrops;
    }
    other_type = (PMIX_BFROP_BUFFER_FULLY_DESC == me->compat.type) ? PMIX_BFROP_BUFFER_NON_DESC
                                                                   : PMIX_BFROP_BUFFER_FULLY_DESC;
    other = server_peer(OTHER_NSPACE, other_bfrops, other_type);
    msg = event_msg(other, CODE_OTHER, "other");
    if (NULL == msg) {
        report("an event from a server of another format", 0, "could not pack it");
    } else {
        deliver(other, PMIX_PTL_TAG_NOTIFY, msg);
        snprintf(detail, sizeof(detail), "handler ran %d times, info \"%s\" (%s)",
                 wait_for(&got_other), seen_other, other_bfrops->version);
        report("an event from a server of another format",
               1 == got_other && 0 == strcmp(seen_other, "other"), detail);
    }
    PMIX_RELEASE(other);
}

static void check_attach(int urifd)
{
    pmix_proc_t myproc, server;
    pmix_info_t info;
    pmix_status_t rc;
    char uri[2048];
    char detail[320];
    ssize_t n;

    n = read(urifd, uri, sizeof(uri) - 1);
    if (0 >= n) {
        report("read the server's URI", 0, "nothing arrived");
        return;
    }
    uri[n] = '\0';

    PMIX_INFO_LOAD(&info, PMIX_SERVER_URI, uri, PMIX_STRING);
    rc = PMIx_tool_attach_to_server(&myproc, &server, &info, 1);
    PMIX_INFO_DESTRUCT(&info);
    if (PMIX_SUCCESS != rc) {
        report("attach to a server that is not our primary", 0, PMIx_Error_string(rc));
        return;
    }
    snprintf(detail, sizeof(detail), "attached to %s:%u", server.nspace, server.rank);
    report("attach to a server that is not our primary",
           0 == strcmp(server.nspace, SERVER_NSPACE), detail);

    report("not connected after attaching to a server that is not our primary",
           !PMIx_tool_is_connected(), "PMIx_tool_is_connected answered true");

    rc = PMIx_tool_set_server(&server, NULL, 0);
    report("make the attached server our primary", PMIX_SUCCESS == rc, PMIx_Error_string(rc));
    report("connected once the attached server is our primary", PMIx_tool_is_connected(),
           "PMIx_tool_is_connected answered false");
}

static int run_tool(int urifd)
{
    pmix_proc_t myproc;
    pmix_info_t tinfo;
    pmix_status_t rc;

    PMIX_INFO_LOAD(&tinfo, PMIX_TOOL_DO_NOT_CONNECT, NULL, PMIX_BOOL);
    rc = PMIx_tool_init(&myproc, &tinfo, 1);
    PMIX_INFO_DESTRUCT(&tinfo);
    if (PMIX_SUCCESS != rc) {
        report("tool init", 0, PMIx_Error_string(rc));
        return nfail;
    }

    check_event_formats();
    check_attach(urifd);

    rc = PMIx_tool_finalize();
    if (PMIX_SUCCESS != rc) {
        report("tool finalize", 0, PMIx_Error_string(rc));
    }
    return nfail;
}

/* ---------- the server ---------- */

static void tool_connected_fn(pmix_info_t *info, size_t ninfo,
                              pmix_tool_connection_cbfunc_t cbfunc, void *cbdata)
{
    pmix_proc_t proc;
    PMIX_HIDE_UNUSED_PARAMS(info, ninfo);

    PMIX_LOAD_PROCID(&proc, "tool-servers-tool", 0);
    cbfunc(PMIX_SUCCESS, &proc, cbdata);
}

static pmix_server_module_t mymodule = {
    .tool_connected = tool_connected_fn
};

int main(int argc, char **argv)
{
    pmix_info_t sinfo[2];
    pmix_status_t rc;
    int uripipe[2];
    pid_t child;
    int status = 0;
    bool flag = true;
    char *uri;
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    setvbuf(stdout, NULL, _IONBF, 0);
    fprintf(stdout, "tool_servers:\n");

    if (0 != pipe(uripipe)) {
        fprintf(stdout, "  FAIL: pipe\n");
        return 1;
    }
    child = fork();
    if (0 > child) {
        fprintf(stdout, "  FAIL: fork\n");
        return 1;
    }
    if (0 == child) {
        close(uripipe[1]);
        _exit(run_tool(uripipe[0]));
    }
    close(uripipe[0]);

    PMIX_INFO_LOAD(&sinfo[0], PMIX_SERVER_TOOL_SUPPORT, &flag, PMIX_BOOL);
    PMIX_INFO_LOAD(&sinfo[1], PMIX_SERVER_NSPACE, SERVER_NSPACE, PMIX_STRING);
    rc = PMIx_server_init(&mymodule, sinfo, 2);
    PMIX_INFO_DESTRUCT(&sinfo[0]);
    PMIX_INFO_DESTRUCT(&sinfo[1]);
    if (PMIX_SUCCESS != rc) {
        report("server init", 0, PMIx_Error_string(rc));
        close(uripipe[1]);
        waitpid(child, &status, 0);
        goto done;
    }
    uri = (NULL != pmix_ptl_base.listener.uri) ? pmix_ptl_base.listener.uri
                                               : pmix_ptl_base.local_listener.uri;
    if (NULL == uri || (ssize_t) strlen(uri) != write(uripipe[1], uri, strlen(uri))) {
        report("hand the tool the server's URI", 0, "no URI, or a short write");
    }
    close(uripipe[1]);

    waitpid(child, &status, 0);
    PMIx_server_finalize();

done:
    if (!WIFEXITED(status)) {
        report("the tool ran to completion", 0, "it died on a signal");
    } else {
        nfail += WEXITSTATUS(status);
    }
    fprintf(stdout, "tool_servers: %d failed\n", nfail);
    return (0 == nfail) ? 0 : 1;
}
