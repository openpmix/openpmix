/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * White-box unit tests for the server side of the two "resolve" convenience
 * APIs - pmix_server_resolve_peers() / pmix_server_resolve_node() and the
 * progress-thread handlers they defer to, in src/server/pmix_server_resolve.c.
 *
 * The process comes up as a PMIx server with a stub host module that
 * deliberately provides no "query" entry point. That is what puts these
 * tests on the code path they are about: with no host to relay to, both
 * handlers thread-shift and answer out of the local datastore, which is the
 * half of the file a single process can reach at all.
 *
 * Two behaviors are pinned down.
 *
 *   the aggregate peer walk and its two passes
 *      A request naming no namespace walks every namespace we know, builds
 *      one "<nspace>:<peerlist>" string per namespace, and counts the peers
 *      as it goes. It then allocates a proc array of that size and walks the
 *      strings a *second* time to fill it, splitting each back apart at the
 *      delimiter. The two passes have to agree, and they did not: the split
 *      took the *first* colon, while a namespace is an arbitrary string the
 *      host chose and may contain one of its own. For a namespace such as
 *      "res:ut,x" the second pass therefore found three comma-separated
 *      fields where the first pass had counted two peers, and wrote past the
 *      end of the array that count had sized.
 *
 *      RESUT_NSPACE_ODD below is exactly that shape. Against an unfixed
 *      library this case reports the wrong peer count and the wrong
 *      namespaces, and the out-of-bounds write may well take the process
 *      down before it gets that far - the same bargain test/unit/iof_output.c
 *      makes with its wrap case.
 *
 *   the node-resolution status contract
 *      docs/how-things-work/resolve.rst is explicit: a namespace that is
 *      known but has no nodes assigned to it right now is a *successfully
 *      executed* request answered with a NULL node list, not a failure.
 *      PMIx_Resolve_nodes says exactly that when it resolves the request out
 *      of the client's own store; the server handed the raw fetch status
 *      back instead, so the same server answered the same question two
 *      different ways depending on which side computed it.
 *
 * Test cases:
 *
 *   aggregate resolve_peers over two nspaces -> SUCCESS, every registered
 *                                               rank, correct namespaces
 *   resolve_peers naming one nspace          -> SUCCESS, that nspace's ranks
 *   resolve_node naming a mapped nspace      -> SUCCESS, our hostname
 *   resolve_node naming an unmapped nspace   -> SUCCESS, NULL node list
 *   resolve_node naming an unknown nspace    -> PMIX_ERR_INVALID_NAMESPACE
 *
 * Replies are read back off the peer's send queue rather than out of a host
 * stub: PMIX_SERVER_QUEUE_REPLY never arms the send event for a peer whose
 * sd is negative, so the message comes to rest on send_msg and a single
 * process can unpack exactly what the server packed. That idiom is
 * documented in test/unit/server_control.c and shared with
 * test/unit/server_fence.c.
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "include/pmix_server.h"

#include "src/include/pmix_globals.h"
#include "src/mca/bfrops/bfrops.h"
#include "src/mca/ptl/base/base.h"
#include "src/server/pmix_server_ops.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* a perfectly ordinary namespace */
#define RESUT_NSPACE_PLAIN "resolve-ut"
/* a namespace carrying the delimiter the aggregate walk inserts, followed
 * by a comma - the shape the two-pass split used to disagree about */
#define RESUT_NSPACE_ODD   "res:ut,x"
/* registered with a job size but no node map, so it is known to us and has
 * no node list */
#define RESUT_NSPACE_BARE  "resolve-ut-bare"

#define RESUT_NPROCS 2

static int npass = 0;
static int nfail = 0;

static void report(const char *name, int passed)
{
    if (passed) {
        fprintf(stdout, "  PASS: %s\n", name);
        ++npass;
    } else {
        fprintf(stdout, "  FAIL: %s\n", name);
        ++nfail;
    }
}

/* Take the reply the handler queued. The peer has no socket, so the send
 * event is never armed: the first lands on send_msg and the rest accumulate
 * on send_queue. */
static pmix_buffer_t *take_queued_reply(void)
{
    pmix_peer_t *peer = pmix_globals.mypeer;
    pmix_ptl_send_t *snd = peer->send_msg;
    pmix_buffer_t *buf;

    if (NULL != snd) {
        peer->send_msg = NULL;
    } else {
        snd = (pmix_ptl_send_t *) pmix_list_remove_first(&peer->send_queue);
        if (NULL == snd) {
            return NULL;
        }
    }
    buf = snd->data;
    /* the buffer is ours now - the send's destructor would release it */
    snd->data = NULL;
    PMIX_RELEASE(snd);
    return buf;
}

/* Both handlers thread-shift, so let anything queued behind us run before
 * we look at the reply. PMIx_Store_internal blocks on the progress thread,
 * so everything queued ahead of it has run by the time it returns. */
static void progress_barrier(void)
{
    pmix_value_t v;

    PMIX_VALUE_LOAD(&v, "barrier", PMIX_STRING);
    PMIx_Store_internal(&pmix_globals.myid, "resolve-ut.barrier", &v);
    PMIX_VALUE_DESTRUCT(&v);
}

/* Build a caddy shaped the way the switchyard builds one. */
static pmix_server_caddy_t *make_caddy(void)
{
    pmix_server_caddy_t *cd;

    cd = PMIX_NEW(pmix_server_caddy_t);
    if (NULL == cd) {
        return NULL;
    }
    PMIX_RETAIN(pmix_globals.mypeer);
    cd->peer = pmix_globals.mypeer;
    cd->hdr.tag = 0;
    return cd;
}

/* Drive one resolve_peers request and unpack whatever it queued. The proc
 * array, when there is one, is handed back through the two OUT
 * parameters. */
static pmix_status_t do_resolve_peers(const char *nodename, const char *nspace,
                                      pmix_proc_t **procs, size_t *nprocs)
{
    pmix_server_caddy_t *cd;
    pmix_buffer_t *buf, *reply;
    pmix_status_t rc, ret;
    char *cptr;
    int32_t cnt;

    *procs = NULL;
    *nprocs = 0;

    buf = PMIX_NEW(pmix_buffer_t);
    if (NULL == buf) {
        return PMIX_ERR_NOMEM;
    }
    cptr = (char *) nodename;
    PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &cptr, 1, PMIX_STRING);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(buf);
        return rc;
    }
    cptr = (char *) nspace;
    PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &cptr, 1, PMIX_STRING);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(buf);
        return rc;
    }

    cd = make_caddy();
    if (NULL == cd) {
        PMIX_RELEASE(buf);
        return PMIX_ERR_NOMEM;
    }
    rc = pmix_server_resolve_peers(cd, buf, pmix_server_respeers_cbfunc);
    PMIX_RELEASE(buf);
    if (PMIX_SUCCESS != rc) {
        /* the switchyard owns the caddy on a non-success return */
        PMIX_RELEASE(cd);
        return rc;
    }
    progress_barrier();

    reply = take_queued_reply();
    if (NULL == reply) {
        return PMIX_ERR_NOT_FOUND;
    }
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, pmix_globals.mypeer, reply, &ret, &cnt, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(reply);
        return rc;
    }
    if (PMIX_SUCCESS == ret) {
        cnt = 1;
        PMIX_BFROPS_UNPACK(rc, pmix_globals.mypeer, reply, nprocs, &cnt, PMIX_SIZE);
        if (PMIX_SUCCESS != rc) {
            PMIX_RELEASE(reply);
            return rc;
        }
        if (0 < *nprocs) {
            PMIX_PROC_CREATE(*procs, *nprocs);
            cnt = (int32_t) *nprocs;
            PMIX_BFROPS_UNPACK(rc, pmix_globals.mypeer, reply, *procs, &cnt, PMIX_PROC);
            if (PMIX_SUCCESS != rc) {
                PMIX_PROC_FREE(*procs, *nprocs);
                *procs = NULL;
                *nprocs = 0;
                PMIX_RELEASE(reply);
                return rc;
            }
        }
    }
    PMIX_RELEASE(reply);
    return ret;
}

/* Drive one resolve_node request. The node list, when there is one, is
 * handed back through *nodelist - which stays NULL when the server answered
 * success with nothing, which is a legal answer. */
static pmix_status_t do_resolve_node(const char *nspace, char **nodelist)
{
    pmix_server_caddy_t *cd;
    pmix_buffer_t *buf, *reply;
    pmix_status_t rc, ret;
    char *cptr;
    int32_t cnt;

    *nodelist = NULL;

    buf = PMIX_NEW(pmix_buffer_t);
    if (NULL == buf) {
        return PMIX_ERR_NOMEM;
    }
    cptr = (char *) nspace;
    PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, buf, &cptr, 1, PMIX_STRING);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(buf);
        return rc;
    }

    cd = make_caddy();
    if (NULL == cd) {
        PMIX_RELEASE(buf);
        return PMIX_ERR_NOMEM;
    }
    rc = pmix_server_resolve_node(cd, buf, pmix_server_resnodes_cbfunc);
    PMIX_RELEASE(buf);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(cd);
        return rc;
    }
    progress_barrier();

    reply = take_queued_reply();
    if (NULL == reply) {
        return PMIX_ERR_NOT_FOUND;
    }
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, pmix_globals.mypeer, reply, &ret, &cnt, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(reply);
        return rc;
    }
    if (PMIX_SUCCESS == ret) {
        cnt = 1;
        PMIX_BFROPS_UNPACK(rc, pmix_globals.mypeer, reply, nodelist, &cnt, PMIX_STRING);
        if (PMIX_SUCCESS != rc) {
            PMIX_RELEASE(reply);
            return rc;
        }
    }
    PMIX_RELEASE(reply);
    return ret;
}

/* Register a namespace with RESUT_NPROCS ranks, all of them on this node.
 * The node map is what makes the gds derive the PMIX_LOCAL_PEERS entry the
 * peer walk reads back. */
static pmix_status_t register_mapped(const char *nsname)
{
    pmix_info_t info[4];
    pmix_nspace_t ns;
    pmix_status_t rc;
    char *noderegex = NULL, *ppnregex = NULL;
    uint32_t nprocs = RESUT_NPROCS;

    PMIx_generate_regex(pmix_globals.hostname, &noderegex);
    PMIx_generate_ppn("0,1", &ppnregex);

    PMIX_INFO_LOAD(&info[0], PMIX_NODE_MAP, noderegex, PMIX_REGEX);
    PMIX_INFO_LOAD(&info[1], PMIX_PROC_MAP, ppnregex, PMIX_REGEX);
    PMIX_INFO_LOAD(&info[2], PMIX_JOB_SIZE, &nprocs, PMIX_UINT32);
    PMIX_INFO_LOAD(&info[3], PMIX_UNIV_SIZE, &nprocs, PMIX_UINT32);

    PMIX_LOAD_NSPACE(ns, nsname);
    rc = PMIx_server_register_nspace(ns, RESUT_NPROCS, info, 4, NULL, NULL);
    if (PMIX_OPERATION_SUCCEEDED == rc) {
        rc = PMIX_SUCCESS;
    }

    PMIX_INFO_DESTRUCT(&info[0]);
    PMIX_INFO_DESTRUCT(&info[1]);
    PMIX_INFO_DESTRUCT(&info[2]);
    PMIX_INFO_DESTRUCT(&info[3]);
    free(noderegex);
    free(ppnregex);
    return rc;
}

/* Register a namespace with no node map at all: known to us, with no node
 * list to answer a resolve_node with. */
static pmix_status_t register_bare(const char *nsname)
{
    pmix_info_t info[2];
    pmix_nspace_t ns;
    pmix_status_t rc;
    uint32_t nprocs = RESUT_NPROCS;

    PMIX_INFO_LOAD(&info[0], PMIX_JOB_SIZE, &nprocs, PMIX_UINT32);
    PMIX_INFO_LOAD(&info[1], PMIX_UNIV_SIZE, &nprocs, PMIX_UINT32);

    PMIX_LOAD_NSPACE(ns, nsname);
    rc = PMIx_server_register_nspace(ns, 0, info, 2, NULL, NULL);
    if (PMIX_OPERATION_SUCCEEDED == rc) {
        rc = PMIX_SUCCESS;
    }

    PMIX_INFO_DESTRUCT(&info[0]);
    PMIX_INFO_DESTRUCT(&info[1]);
    return rc;
}

/* how many of the returned procs name this nspace, and are their ranks the
 * ones that were registered for it? */
static size_t count_ranks(pmix_proc_t *procs, size_t nprocs, const char *nsname,
                          bool *ranks_ok)
{
    size_t n, found = 0;
    bool seen[RESUT_NPROCS];

    for (n = 0; n < RESUT_NPROCS; n++) {
        seen[n] = false;
    }
    for (n = 0; n < nprocs; n++) {
        if (0 != strncmp(procs[n].nspace, nsname, PMIX_MAX_NSLEN)) {
            continue;
        }
        ++found;
        if (procs[n].rank < RESUT_NPROCS) {
            seen[procs[n].rank] = true;
        }
    }
    *ranks_ok = true;
    for (n = 0; n < RESUT_NPROCS; n++) {
        if (!seen[n]) {
            *ranks_ok = false;
        }
    }
    return found;
}

int main(int argc, char **argv)
{
    static pmix_server_module_t mymodule = {0};
    pmix_status_t rc;
    pmix_proc_t *procs = NULL;
    size_t nprocs = 0, nplain, nodd;
    char *nodelist = NULL;
    bool ranks_ok = false, odd_ok = false;

    (void) argc;
    (void) argv;

    fprintf(stdout, "server_resolve: server-side resolve_peers/resolve_node unit tests\n");

    rc = PMIx_server_init(&mymodule, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }
    /* the stub module has no query entry point, which is the configuration
     * these tests need - assert it rather than assume it */
    if (NULL != pmix_host_server.query) {
        fprintf(stderr, "test setup error: host module advertises query\n");
        PMIx_server_finalize();
        return 1;
    }

    rc = register_mapped(RESUT_NSPACE_PLAIN);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "register %s failed: %s\n", RESUT_NSPACE_PLAIN, PMIx_Error_string(rc));
        PMIx_server_finalize();
        return 1;
    }
    rc = register_mapped(RESUT_NSPACE_ODD);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "register %s failed: %s\n", RESUT_NSPACE_ODD, PMIx_Error_string(rc));
        PMIx_server_finalize();
        return 1;
    }
    rc = register_bare(RESUT_NSPACE_BARE);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "register %s failed: %s\n", RESUT_NSPACE_BARE, PMIx_Error_string(rc));
        PMIx_server_finalize();
        return 1;
    }

    /* --- one namespace, our node --------------------------------------- */
    rc = do_resolve_peers(NULL, RESUT_NSPACE_PLAIN, &procs, &nprocs);
    report("resolve_peers for a mapped nspace succeeds", PMIX_SUCCESS == rc);
    nplain = count_ranks(procs, nprocs, RESUT_NSPACE_PLAIN, &ranks_ok);
    report("resolve_peers returns both of its ranks",
           RESUT_NPROCS == nprocs && RESUT_NPROCS == nplain && ranks_ok);
    if (NULL != procs) {
        PMIX_PROC_FREE(procs, nprocs);
        procs = NULL;
    }

    /* --- the same, for the namespace carrying a colon ------------------- */
    rc = do_resolve_peers(NULL, RESUT_NSPACE_ODD, &procs, &nprocs);
    report("resolve_peers for a colon-bearing nspace succeeds", PMIX_SUCCESS == rc);
    nodd = count_ranks(procs, nprocs, RESUT_NSPACE_ODD, &ranks_ok);
    report("resolve_peers returns both of its ranks under its real name",
           RESUT_NPROCS == nprocs && RESUT_NPROCS == nodd && ranks_ok);
    if (NULL != procs) {
        PMIX_PROC_FREE(procs, nprocs);
        procs = NULL;
    }

    /* --- the aggregate walk over every namespace ------------------------
     * This is the case the two-pass split used to get wrong. The counting
     * pass credits RESUT_NSPACE_ODD with two peers; a split on the *first*
     * colon finds three fields in "res:ut,x:0,1" and writes a third entry
     * past the end of the array those counts sized. */
    rc = do_resolve_peers(NULL, NULL, &procs, &nprocs);
    report("aggregate resolve_peers succeeds", PMIX_SUCCESS == rc);
    report("aggregate resolve_peers returns exactly the registered peers",
           (2 * RESUT_NPROCS) == nprocs);
    nplain = count_ranks(procs, nprocs, RESUT_NSPACE_PLAIN, &ranks_ok);
    report("aggregate resolve_peers attributes the plain nspace correctly",
           RESUT_NPROCS == nplain && ranks_ok);
    nodd = count_ranks(procs, nprocs, RESUT_NSPACE_ODD, &odd_ok);
    report("aggregate resolve_peers attributes the colon-bearing nspace correctly",
           RESUT_NPROCS == nodd && odd_ok);
    if (NULL != procs) {
        PMIX_PROC_FREE(procs, nprocs);
        procs = NULL;
    }

    /* --- resolve_node against a mapped namespace ------------------------ */
    rc = do_resolve_node(RESUT_NSPACE_PLAIN, &nodelist);
    report("resolve_node for a mapped nspace succeeds", PMIX_SUCCESS == rc);
    report("resolve_node names this host",
           NULL != nodelist && NULL != strstr(nodelist, pmix_globals.hostname));
    if (NULL != nodelist) {
        free(nodelist);
        nodelist = NULL;
    }

    /* --- resolve_node against a namespace with no nodes assigned --------
     * docs/how-things-work/resolve.rst calls this a successfully executed
     * request answered with a NULL node list. The server used to hand back
     * the raw fetch status instead. */
    rc = do_resolve_node(RESUT_NSPACE_BARE, &nodelist);
    report("resolve_node for an nspace with no nodes reports success",
           PMIX_SUCCESS == rc);
    report("resolve_node for an nspace with no nodes returns no list",
           NULL == nodelist);
    if (NULL != nodelist) {
        free(nodelist);
        nodelist = NULL;
    }

    /* --- resolve_node against a namespace we never heard of -------------
     * that one really is an error, and a different one */
    rc = do_resolve_node("no-such-nspace-at-all", &nodelist);
    report("resolve_node for an unknown nspace reports an invalid namespace",
           PMIX_ERR_INVALID_NAMESPACE == rc);
    if (NULL != nodelist) {
        free(nodelist);
        nodelist = NULL;
    }

    PMIx_server_finalize();

    fprintf(stdout, "server_resolve: %d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
