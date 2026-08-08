/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Exercise the tool library's multi-server model against servers that are
 * really on different nodes.
 *
 * A tool is the only PMIx role that can hold connections to several
 * servers at once and choose which of them is "primary" - the one that
 * services queries, spawns and event notifications that are not directed
 * anywhere in particular. That machinery lives in src/tool
 * (pmix_tool_retry_attach, pmix_tool_retry_set, disc, getsrvrs), and each
 * of those repoints pmix_client_globals.myserver while the peer objects
 * are also held by pmix_server_globals.clients, so their reference
 * accounting has to balance exactly against PMIx_tool_finalize.
 *
 * Two servers on ONE host will exercise the bookkeeping, but not much
 * else: both connections are loopback, both peers are reachable for as
 * long as the process lives, and a query answered by "the other server"
 * never leaves the machine. Pointing this at daemons on two different
 * nodes is what makes the primary-server choice observable - the answer
 * to PMIX_QUERY_NAMESPACES, or to a request for the server's own info,
 * comes back over a different socket to a different host depending on
 * which server is primary at the time.
 *
 * Usage:
 *
 *   toolswitch -u <uri> [-n <cycles>] [-q]
 *
 *     -u <uri>     URI of a SECOND server, normally one on another node.
 *                  Required: without it there is only one server and
 *                  nothing to switch between.
 *     -n <cycles>  how many attach/switch/disconnect cycles to run
 *                  (default 5). Each cycle inits and finalizes the tool
 *                  library, so the count is also a re-init stress.
 *     -q           skip the query stage (useful when the servers in play
 *                  do not answer queries).
 *
 * The tool's FIRST server is whatever PMIx_tool_init finds on its own -
 * normally the daemon on this node, discovered through its rendezvous
 * file. So run this on a node that has one.
 *
 * Exits 0 if every cycle completed, non-zero (with a message on stderr)
 * on the first failure.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "examples.h"
#include <pmix_tool.h>

#define DEFAULT_CYCLES 5
/* a typo on the command line must not turn into a run that never ends */
#define MAX_CYCLES     10000

static char hostname[1024];

static void querycb(pmix_status_t status, pmix_info_t *info, size_t ninfo, void *cbdata,
                    pmix_release_cbfunc_t release_fn, void *release_cbdata)
{
    myquery_data_t *mq = (myquery_data_t *) cbdata;
    EXAMPLES_HIDE_UNUSED_PARAMS(info, ninfo);

    mq->lock.status = status;
    if (NULL != release_fn) {
        release_fn(release_cbdata);
    }
    DEBUG_WAKEUP_THREAD(&mq->lock);
}

/* Ask the current primary server something only a server can answer.
 * The point is not the answer but the round trip: it proves the primary
 * we just selected is the peer the request actually went to. A server
 * that does not implement the query interface says so, and that is not a
 * failure of the switch. */
static int ask_primary(const char *who)
{
    pmix_query_t *query;
    myquery_data_t mydata;
    pmix_status_t rc;

    PMIX_QUERY_CREATE(query, 1);
    PMIX_ARGV_APPEND(rc, query[0].keys, PMIX_QUERY_NAMESPACES);
    if (PMIX_SUCCESS != rc) {
        PMIX_QUERY_FREE(query, 1);
        fprintf(stderr, "toolswitch: could not build the query\n");
        return 1;
    }

    DEBUG_CONSTRUCT_LOCK(&mydata.lock);
    mydata.info = NULL;
    mydata.ninfo = 0;
    rc = PMIx_Query_info_nb(query, 1, querycb, (void *) &mydata);
    if (PMIX_SUCCESS != rc) {
        DEBUG_DESTRUCT_LOCK(&mydata.lock);
        PMIX_QUERY_FREE(query, 1);
        /* the request never left, which IS a switch-path failure */
        fprintf(stderr, "toolswitch: query to %s could not be sent: %s\n", who,
                PMIx_Error_string(rc));
        return 1;
    }
    DEBUG_WAIT_THREAD(&mydata.lock);
    rc = mydata.lock.status;
    DEBUG_DESTRUCT_LOCK(&mydata.lock);
    PMIX_QUERY_FREE(query, 1);
    if (NULL != mydata.info) {
        PMIX_INFO_FREE(mydata.info, mydata.ninfo);
    }

    /* PMIX_ERR_NOT_SUPPORTED means we reached a server that does not
     * answer this query - the round trip still happened */
    if (PMIX_SUCCESS != rc && PMIX_ERR_NOT_SUPPORTED != rc && PMIX_ERR_NOT_FOUND != rc) {
        fprintf(stderr, "toolswitch: query to %s failed: %s\n", who, PMIx_Error_string(rc));
        return 1;
    }
    printf("[%s] query answered by %s: %s\n", hostname, who, PMIx_Error_string(rc));
    return 0;
}

/* find the entry in the list that is NOT "skip" */
static int find_other(pmix_proc_t *list, size_t n, pmix_proc_t *skip, pmix_proc_t *found)
{
    size_t i;

    for (i = 0; i < n; i++) {
        if (!PMIX_CHECK_PROCID(&list[i], skip)) {
            PMIX_LOAD_PROCID(found, list[i].nspace, list[i].rank);
            return 0;
        }
    }
    return -1;
}

static int one_cycle(long cyc, const char *uri, bool doquery)
{
    pmix_proc_t myproc, local, remote, other;
    pmix_proc_t *servers = NULL;
    size_t nservers = 0;
    pmix_info_t info[3];
    pmix_status_t rc;
    int itmo = 5;

    /* Each cycle must come up with a fresh identity. A previous cycle's
     * attach can leave identity variables in our environment, and an
     * nspace without a rank (or the reverse) is a hard error in init. */
    unsetenv("PMIX_NAMESPACE");
    unsetenv("PMIX_RANK");

    rc = PMIx_tool_init(&myproc, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "cycle %ld: PMIx_tool_init failed: %s\n", cyc, PMIx_Error_string(rc));
        return 1;
    }
    if (!PMIx_tool_is_connected()) {
        fprintf(stderr, "cycle %ld: init did not connect us to a server\n", cyc);
        goto err;
    }

    /* whatever init found is our local server */
    rc = PMIx_tool_get_servers(&servers, &nservers);
    if (PMIX_SUCCESS != rc || 1 != nservers) {
        fprintf(stderr, "cycle %ld: get_servers after init: rc=%s n=%zu (want 1)\n", cyc,
                PMIx_Error_string(rc), nservers);
        goto err;
    }
    PMIX_LOAD_PROCID(&local, servers[0].nspace, servers[0].rank);
    PMIX_PROC_FREE(servers, nservers);
    servers = NULL;
    printf("[%s] cycle %ld: local server is %s:%u\n", hostname, cyc, local.nspace, local.rank);

    /* attach to the remote server and make it primary - this is
     * pmix_tool_retry_attach's switch branch, over a real socket to
     * another host */
    PMIX_INFO_LOAD(&info[0], PMIX_SERVER_URI, uri, PMIX_STRING);
    PMIX_INFO_LOAD(&info[1], PMIX_PRIMARY_SERVER, NULL, PMIX_BOOL);
    PMIX_INFO_LOAD(&info[2], PMIX_TIMEOUT, &itmo, PMIX_INT);
    rc = PMIx_tool_attach_to_server(&myproc, &remote, info, 3);
    PMIX_INFO_DESTRUCT(&info[0]);
    PMIX_INFO_DESTRUCT(&info[1]);
    PMIX_INFO_DESTRUCT(&info[2]);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "cycle %ld: attach to remote server failed: %s\n", cyc,
                PMIx_Error_string(rc));
        goto err;
    }
    printf("[%s] cycle %ld: remote server is %s:%u\n", hostname, cyc, remote.nspace,
           remote.rank);

    /* the two must be DIFFERENT servers, or the rest of this cycle is
     * only testing that we can talk to one server twice */
    if (PMIX_CHECK_PROCID(&local, &remote)) {
        fprintf(stderr, "cycle %ld: the remote URI resolved to our own local server "
                "(%s:%u) - point -u at a daemon on another node\n",
                cyc, local.nspace, local.rank);
        goto err;
    }

    rc = PMIx_tool_get_servers(&servers, &nservers);
    if (PMIX_SUCCESS != rc || 2 != nservers) {
        fprintf(stderr, "cycle %ld: get_servers after attach: rc=%s n=%zu (want 2)\n", cyc,
                PMIx_Error_string(rc), nservers);
        goto err;
    }
    /* the primary is documented to be first */
    if (!PMIX_CHECK_PROCID(&servers[0], &remote)) {
        fprintf(stderr, "cycle %ld: the new primary is not first in the server list\n", cyc);
        goto err;
    }
    if (0 != find_other(servers, nservers, &remote, &other) ||
        !PMIX_CHECK_PROCID(&other, &local)) {
        fprintf(stderr, "cycle %ld: the server list does not hold both servers\n", cyc);
        goto err;
    }
    PMIX_PROC_FREE(servers, nservers);
    servers = NULL;

    if (doquery && 0 != ask_primary("the remote server")) {
        goto err;
    }

    /* switch the primary back to the local server and ask again - the
     * same call now has to travel to a different host */
    rc = PMIx_tool_set_server(&local, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "cycle %ld: set_server(local) failed: %s\n", cyc,
                PMIx_Error_string(rc));
        goto err;
    }
    if (doquery && 0 != ask_primary("the local server")) {
        goto err;
    }

    /* switch to ourselves: we then have no server at all, and the
     * library must say so */
    rc = PMIx_tool_set_server(&myproc, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "cycle %ld: set_server(self) failed: %s\n", cyc,
                PMIx_Error_string(rc));
        goto err;
    }
    if (PMIx_tool_is_connected()) {
        fprintf(stderr, "cycle %ld: still 'connected' after pointing at ourselves\n", cyc);
        goto err;
    }

    /* and back out to the remote one, which must still be attached */
    rc = PMIx_tool_set_server(&remote, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "cycle %ld: set_server(remote) failed: %s\n", cyc,
                PMIx_Error_string(rc));
        goto err;
    }
    if (!PMIx_tool_is_connected()) {
        fprintf(stderr, "cycle %ld: not connected after returning to the remote server\n", cyc);
        goto err;
    }

    /* drop the non-primary first, then the primary: disc's two branches */
    rc = PMIx_tool_disconnect(&local);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "cycle %ld: disconnect(local) failed: %s\n", cyc,
                PMIx_Error_string(rc));
        goto err;
    }
    rc = PMIx_tool_disconnect(&remote);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "cycle %ld: disconnect(remote) failed: %s\n", cyc,
                PMIx_Error_string(rc));
        goto err;
    }
    if (PMIx_tool_is_connected()) {
        fprintf(stderr, "cycle %ld: still connected after dropping every server\n", cyc);
        goto err;
    }
    rc = PMIx_tool_get_servers(&servers, &nservers);
    if (PMIX_ERR_UNREACH != rc || 0 != nservers) {
        fprintf(stderr, "cycle %ld: get_servers with none left: rc=%s n=%zu "
                "(want PMIX_ERR_UNREACH/0)\n", cyc, PMIx_Error_string(rc), nservers);
        goto err;
    }

    rc = PMIx_tool_finalize();
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "cycle %ld: PMIx_tool_finalize failed: %s\n", cyc,
                PMIx_Error_string(rc));
        return 1;
    }
    return 0;

err:
    if (NULL != servers) {
        PMIX_PROC_FREE(servers, nservers);
    }
    PMIx_tool_finalize();
    return 1;
}

int main(int argc, char **argv)
{
    char *uri = NULL;
    long ncycles = DEFAULT_CYCLES, i;
    bool doquery = true;
    int n;

    gethostname(hostname, sizeof(hostname));

    for (n = 1; n < argc; n++) {
        if (0 == strcmp("-u", argv[n]) || 0 == strcmp("--url", argv[n])) {
            if (NULL == argv[n + 1]) {
                fprintf(stderr, "%s requires a URI argument\n", argv[n]);
                return 1;
            }
            uri = argv[++n];
        } else if (0 == strcmp("-n", argv[n]) || 0 == strcmp("--cycles", argv[n])) {
            if (NULL == argv[n + 1]) {
                fprintf(stderr, "%s requires a count argument\n", argv[n]);
                return 1;
            }
            ncycles = strtol(argv[++n], NULL, 10);
            if (0 >= ncycles || MAX_CYCLES < ncycles) {
                ncycles = DEFAULT_CYCLES;
            }
        } else if (0 == strcmp("-q", argv[n]) || 0 == strcmp("--no-query", argv[n])) {
            doquery = false;
        } else {
            fprintf(stderr, "usage: %s -u <uri of a second server> [-n cycles] [-q]\n", argv[0]);
            return 1;
        }
    }
    if (NULL == uri) {
        fprintf(stderr, "usage: %s -u <uri of a second server> [-n cycles] [-q]\n", argv[0]);
        return 1;
    }

    printf("[%s] toolswitch: %ld cycles against %s\n", hostname, ncycles, uri);
    for (i = 0; i < ncycles; i++) {
        if (0 != one_cycle(i, uri, doquery)) {
            fprintf(stderr, "toolswitch: FAILED on cycle %ld\n", i);
            return 1;
        }
    }
    printf("[%s] toolswitch: PASS (%ld cycles)\n", hostname, ncycles);
    return 0;
}
