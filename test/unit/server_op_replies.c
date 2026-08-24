/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * White-box unit tests for src/server/pmix_server_op_replies.c - the host
 * callbacks that complete a client operation and carry its payload back.
 *
 * The case here is the lookup reply. pmix_lookup_cbfunc_t carries no
 * release function, so nothing the host hands us survives the return of
 * the call and pmix_server_lookup_cbfunc has to copy every element into
 * its own array. That copy runs through the *requesting peer's* bfrops
 * module, which is exactly why it can fail without an allocation failure:
 * an older peer's module need not know every type a newer one published.
 *
 * A failed copy leaves the element carrying the PMIX_UNDEF its
 * constructor gave it, and _lkupcbfunc packs the array as it stands - so
 * with the status left at PMIX_SUCCESS the requestor was told a key it
 * asked for had been found, and handed nothing. PMIX_ERR_PARTIAL_SUCCESS
 * is the status the client side already understands: lookup_cbfunc in
 * src/client/pmix_client_pub.c transfers what it was given under that
 * status and under PMIX_SUCCESS, and discards the whole array - including
 * the elements that did copy - under any other error.
 *
 * The replies are read back off the peer rather than out of a stub's
 * arguments, per the idiom in test/unit/server_control.c: PMIX_SERVER_QUEUE_REPLY
 * never arms the send event for a peer whose sd is negative, so the
 * message comes to rest on peer->send_msg and a single process can unpack
 * what the server actually packed.
 *
 * Ordering is deterministic rather than timed. pmix_server_lookup_cbfunc
 * thread-shifts, and PMIx_Store_internal is a blocking call onto that same
 * progress thread - so when it returns, everything queued ahead of it has
 * already run.
 *
 * Two fixes are in play in the second case, and re-breaking them one at a
 * time is how they were told apart. value_xfer copies the source type
 * across before it discovers it cannot copy the payload, so a failed
 * element was left claiming a type this build cannot pack; _lkupcbfunc
 * then abandoned the whole reply on that pack failure, and the client sat
 * in PMIx_Lookup forever (the case reports PMIX_ERR_NOT_FOUND here,
 * meaning nothing was queued at all). With only the second fix in place
 * the case reports PMIX_ERROR - a status-only reply, which is the client
 * being told rather than left waiting. Both together give
 * PMIX_ERR_PARTIAL_SUCCESS and the elements that did copy.
 *
 * That is also why there is no case for the status-only fallback on its
 * own: with the type reset in place the array always packs, so reaching
 * that arm now needs a genuine allocation failure.
 *
 * Test cases:
 *
 *   lookup reply, all values copied     -> PMIX_SUCCESS, both keys present
 *   one value the peer cannot copy      -> PMIX_ERR_PARTIAL_SUCCESS
 *   and the element that did copy       -> still in the reply
 *   host reported an error, no data     -> that status, reply carries no data
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "include/pmix_server.h"

#include "src/include/pmix_globals.h"
#include "src/mca/bfrops/bfrops.h"
#include "src/server/pmix_server_ops.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define OPUT_KEY1 "server-op-replies-ut.one"
#define OPUT_KEY2 "server-op-replies-ut.two"
/* a type no bfrops module implements, so value_xfer refuses it */
#define OPUT_BOGUS_TYPE ((pmix_data_type_t) 0x7000)

static int npass = 0;
static int nfail = 0;

static void report(const char *name, int passed, const char *detail)
{
    if (passed) {
        fprintf(stdout, "  PASS: %s\n", name);
        ++npass;
    } else {
        fprintf(stdout, "  FAIL: %s (%s)\n", name, NULL == detail ? "" : detail);
        ++nfail;
    }
}

/* Block until everything already queued on the progress thread has run.
 * PMIx_Store_internal thread-shifts and waits, so its return is the
 * barrier - no sleeps. */
static void settle(void)
{
    pmix_value_t val;

    PMIX_VALUE_LOAD(&val, "settle", PMIX_STRING);
    (void) PMIx_Store_internal(&pmix_globals.myid, "server-op-replies-ut.settle", &val);
    PMIX_VALUE_DESTRUCT(&val);
}

/* Take the reply the server queued to a peer that has no socket. */
static pmix_buffer_t *take_queued_reply(void)
{
    pmix_peer_t *peer = pmix_globals.mypeer;
    pmix_ptl_send_t *snd = peer->send_msg;
    pmix_buffer_t *buf;

    if (NULL == snd) {
        return NULL;
    }
    peer->send_msg = NULL;
    buf = snd->data;
    snd->data = NULL;
    PMIX_RELEASE(snd);
    return buf;
}

/* Drive one lookup completion. "bogus" says which element (or -1 for
 * none) carries a type the peer's bfrops module cannot transfer. */
static pmix_buffer_t *drive_lookup(pmix_status_t status, int nelements, int bogus)
{
    pmix_server_caddy_t *cd;
    pmix_pdata_t *pdata = NULL;
    int n;

    if (0 < nelements) {
        PMIX_PDATA_CREATE(pdata, (size_t) nelements);
        if (NULL == pdata) {
            return NULL;
        }
        for (n = 0; n < nelements; n++) {
            PMIX_LOAD_PROCID(&pdata[n].proc, pmix_globals.myid.nspace, (pmix_rank_t) n);
            pmix_strncpy(pdata[n].key, (0 == n) ? OPUT_KEY1 : OPUT_KEY2, PMIX_MAX_KEYLEN);
            if (n == bogus) {
                pdata[n].value.type = OPUT_BOGUS_TYPE;
            } else {
                PMIX_VALUE_LOAD(&pdata[n].value, "a-value", PMIX_STRING);
            }
        }
    }

    cd = PMIX_NEW(pmix_server_caddy_t);
    if (NULL == cd) {
        PMIX_PDATA_FREE(pdata, (size_t) nelements);
        return NULL;
    }
    PMIX_RETAIN(pmix_globals.mypeer);
    cd->peer = pmix_globals.mypeer;
    cd->hdr.tag = 0;

    pmix_server_lookup_cbfunc(status, pdata, (size_t) (0 < nelements ? nelements : 0), cd);

    /* the host owns what it handed down - free it exactly as a host would
     * the moment the call returns, which is what makes the copy mandatory */
    if (NULL != pdata) {
        for (n = 0; n < nelements; n++) {
            if (n == bogus) {
                /* not a real value - do not let the destructor read it */
                pdata[n].value.type = PMIX_UNDEF;
            }
        }
        PMIX_PDATA_FREE(pdata, (size_t) nelements);
    }

    settle();
    return take_queued_reply();
}

/* Unpack a lookup reply: status, then (if any) the count and the pdata. */
static pmix_status_t read_reply(pmix_buffer_t *reply, pmix_pdata_t **pdata, size_t *ndata)
{
    pmix_status_t st = PMIX_ERR_NOT_FOUND, rc;
    int32_t cnt = 1;

    *pdata = NULL;
    *ndata = 0;
    if (NULL == reply) {
        return PMIX_ERR_NOT_FOUND;
    }
    PMIX_BFROPS_UNPACK(rc, pmix_globals.mypeer, reply, &st, &cnt, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        return PMIX_ERR_UNPACK_FAILURE;
    }
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, pmix_globals.mypeer, reply, ndata, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc || 0 == *ndata) {
        *ndata = 0;
        return st;
    }
    PMIX_PDATA_CREATE(*pdata, *ndata);
    cnt = (int32_t) *ndata;
    PMIX_BFROPS_UNPACK(rc, pmix_globals.mypeer, reply, *pdata, &cnt, PMIX_PDATA);
    if (PMIX_SUCCESS != rc) {
        PMIX_PDATA_FREE(*pdata, *ndata);
        *pdata = NULL;
        *ndata = 0;
    }
    return st;
}

int main(int argc, char **argv)
{
    static pmix_server_module_t mymodule = {0};
    pmix_status_t rc, st;
    pmix_buffer_t *reply;
    pmix_pdata_t *got = NULL;
    size_t ngot = 0;

    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    fprintf(stdout, "server_op_replies: operation-reply unit tests\n");

    rc = PMIx_server_init(&mymodule, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    /* --- everything copied ------------------------------------------- */
    reply = drive_lookup(PMIX_SUCCESS, 2, -1);
    st = read_reply(reply, &got, &ngot);
    report("a clean lookup reports success", PMIX_SUCCESS == st, PMIx_Error_string(st));
    report("and carries both elements", 2 == ngot, "wrong element count");
    if (2 == ngot) {
        report("with the keys the host supplied",
               0 == strcmp(got[0].key, OPUT_KEY1) && 0 == strcmp(got[1].key, OPUT_KEY2),
               "keys do not match");
        report("and the values copied out of the host's array",
               PMIX_STRING == got[0].value.type && NULL != got[0].value.data.string
                   && 0 == strcmp(got[0].value.data.string, "a-value"),
               "value did not survive the copy");
    }
    if (NULL != got) {
        PMIX_PDATA_FREE(got, ngot);
        got = NULL;
    }
    if (NULL != reply) {
        PMIX_RELEASE(reply);
    }

    /* --- one element the peer's module cannot copy -------------------- */
    fprintf(stdout, "  (the next case deliberately provokes an "
                    "\"UNSUPPORTED TYPE\" diagnostic)\n");
    reply = drive_lookup(PMIX_SUCCESS, 2, 1);
    st = read_reply(reply, &got, &ngot);
    report("a value that will not copy is not reported as success",
           PMIX_ERR_PARTIAL_SUCCESS == st, PMIx_Error_string(st));
    report("and the element that did copy is still in the reply",
           2 == ngot && NULL != got && PMIX_STRING == got[0].value.type,
           "the good element was lost");
    if (NULL != got) {
        PMIX_PDATA_FREE(got, ngot);
        got = NULL;
    }
    if (NULL != reply) {
        PMIX_RELEASE(reply);
    }

    /* --- the host found nothing -------------------------------------- */
    reply = drive_lookup(PMIX_ERR_NOT_FOUND, 0, -1);
    st = read_reply(reply, &got, &ngot);
    report("a host that found nothing has its status passed through",
           PMIX_ERR_NOT_FOUND == st, PMIx_Error_string(st));
    report("and no data is packed behind it", 0 == ngot, "data was packed");
    if (NULL != got) {
        PMIX_PDATA_FREE(got, ngot);
    }
    if (NULL != reply) {
        PMIX_RELEASE(reply);
    }

    PMIx_server_finalize();

    fprintf(stdout, "server_op_replies: %d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
