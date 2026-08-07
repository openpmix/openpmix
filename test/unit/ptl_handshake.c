/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit tests for the PTL connection handshake (de)serialization macros.
 *
 * The handshake is the first thing that crosses a new socket, and it is
 * a frozen wire format: a server built from one PMIx release has to read
 * one written by another. The PMIX_PTL_PUT_* macros in ptl_base_fns.c
 * build it and the PMIX_PTL_GET_* macros in ptl_base_connection_hdlr.c
 * take it apart, and the two are only correct as a pair. These tests
 * drive that pair directly - building a message field by field, then
 * parsing it back - so a change to either half that is not mirrored in
 * the other shows up here rather than as an unexplained connect failure
 * against a peer of a different vintage.
 *
 * The second half of the file feeds the parser malformed messages. This
 * is the one place in PMIx where a completely unauthenticated remote
 * party gets to hand us a length and a buffer: the connection handler
 * runs before the credential has been validated, so anything that can
 * open a TCP connection to the listener can drive these macros. Each
 * "truncated" case below is a message whose declared field extends past
 * the bytes actually received; the parser has to stop, not read what
 * follows the allocation.
 *
 * The macros deliberately reference variables from their surroundings
 * (msg/csize when building, mg/cnt when parsing, plus an "error" label),
 * so each function here declares them under exactly those names.
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "include/pmix_server.h"
#include "src/include/pmix_globals.h"
#include "src/mca/ptl/base/base.h"
#include "src/mca/ptl/base/ptl_base_handshake.h"
#include "src/util/pmix_strnlen.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* what the tests build and read back */
#define TEST_PSEC    "native"
#define TEST_VERSION "5.1.2"
#define TEST_BFROPS  "v41"
#define TEST_GDS     "hash"
#define TEST_NSPACE  "handshake.ns"
#define TEST_RANK    7
#define TEST_UID     1001
#define TEST_GID     2002
#define TEST_CRED    "credential-bytes"

/* Build the handshake a PMIX_TOOL_GIVEN_ID connector sends, exactly as
 * construct_message() lays it out. Returns the allocated message and its
 * length; the caller frees it. */
static char *build_message(size_t *sz, uint32_t credlen, const char *cred)
{
    char *msg;
    size_t csize, sdsize;
    uint8_t flag = PMIX_TOOL_GIVEN_ID;
    uint8_t bftype = PMIX_BFROP_BUFFER_FULLY_DESC;
    uint32_t u32;
    pmix_proc_t proc;

    PMIX_LOAD_PROCID(&proc, TEST_NSPACE, TEST_RANK);

    sdsize = strlen(TEST_PSEC) + 1;              /* psec module name */
    sdsize += sizeof(uint32_t);                  /* credential length */
    sdsize += credlen;                           /* credential */
    sdsize += 1;                                 /* connector flag */
    sdsize += 2 * sizeof(uint32_t);              /* uid, gid */
    sdsize += strlen(TEST_NSPACE) + 1;           /* our nspace */
    sdsize += sizeof(uint32_t);                  /* our rank */
    sdsize += strlen(TEST_VERSION) + 1;          /* version */
    sdsize += strlen(TEST_BFROPS) + 1;           /* bfrops module name */
    sdsize += sizeof(bftype);                    /* buffer type */
    sdsize += strlen(TEST_GDS) + 1;              /* gds module name */

    msg = (char *) malloc(sdsize);
    if (NULL == msg) {
        return NULL;
    }
    /* the PUT_STRING macro relies on the buffer being zeroed to
     * terminate each string it copies */
    memset(msg, 0, sdsize);

    csize = 0;
    PMIX_PTL_PUT_STRING(TEST_PSEC);
    PMIX_PTL_PUT_U32(credlen);
    if (0 < credlen && NULL != cred) {
        PMIX_PTL_PUT_BLOB(cred, credlen);
    }
    PMIX_PTL_PUT_U8(flag);
    u32 = TEST_UID;
    PMIX_PTL_PUT_U32(u32);
    u32 = TEST_GID;
    PMIX_PTL_PUT_U32(u32);
    PMIX_PTL_PUT_PROCID(proc);
    PMIX_PTL_PUT_STRING(TEST_VERSION);
    PMIX_PTL_PUT_STRING(TEST_BFROPS);
    PMIX_PTL_PUT_U8(bftype);
    PMIX_PTL_PUT_STRING(TEST_GDS);

    *sz = sdsize;
    return msg;
}

/* ---- round trip -------------------------------------------------- */

static void test_round_trip(void)
{
    char *msg, *mg;
    size_t sz, cnt;
    char *psec = NULL, *version = NULL, *bfrops = NULL, *gds = NULL, *cred = NULL;
    uint32_t credlen = 0, uid = 0, gid = 0;
    uint8_t flag = 0, bftype = 0;
    pmix_proc_t proc;
    uint8_t major, minor, release;
    bool parsed = false;

    msg = build_message(&sz, strlen(TEST_CRED), TEST_CRED);
    if (NULL == msg) {
        report("round trip: message built", 0, "out of memory");
        return;
    }

    mg = msg;
    cnt = sz;

    PMIX_PTL_GET_STRING(psec);
    PMIX_PTL_GET_U32(credlen);
    PMIX_PTL_GET_BLOB(cred, credlen);
    PMIX_PTL_GET_U8(flag);
    PMIX_PTL_GET_U32(uid);
    PMIX_PTL_GET_U32(gid);
    PMIX_PTL_GET_PROCID(proc);
    PMIX_PTL_GET_STRING(version);
    PMIX_PTL_GET_STRING(bfrops);
    PMIX_PTL_GET_U8(bftype);
    PMIX_PTL_GET_STRING(gds);
    parsed = true;

error:
    report("round trip: message parsed to completion", parsed, "parser bailed out");
    if (parsed) {
        report("round trip: psec module name", NULL != psec && 0 == strcmp(psec, TEST_PSEC),
               (NULL == psec) ? "NULL" : psec);
        report("round trip: credential length", strlen(TEST_CRED) == credlen, "wrong length");
        report("round trip: credential bytes",
               NULL != cred && 0 == memcmp(cred, TEST_CRED, strlen(TEST_CRED)), "wrong bytes");
        report("round trip: connector flag", PMIX_TOOL_GIVEN_ID == flag, "wrong flag");
        report("round trip: uid", TEST_UID == uid, "wrong uid");
        report("round trip: gid", TEST_GID == gid, "wrong gid");
        report("round trip: nspace", 0 == strcmp(proc.nspace, TEST_NSPACE), proc.nspace);
        report("round trip: rank", TEST_RANK == proc.rank, "wrong rank");
        report("round trip: version", NULL != version && 0 == strcmp(version, TEST_VERSION),
               (NULL == version) ? "NULL" : version);
        report("round trip: bfrops module name", NULL != bfrops && 0 == strcmp(bfrops, TEST_BFROPS),
               (NULL == bfrops) ? "NULL" : bfrops);
        report("round trip: buffer type", PMIX_BFROP_BUFFER_FULLY_DESC == bftype, "wrong type");
        report("round trip: gds module name", NULL != gds && 0 == strcmp(gds, TEST_GDS),
               (NULL == gds) ? "NULL" : gds);
        /* the whole message must have been consumed - a mismatch here
         * means the two halves of the format have drifted apart */
        report("round trip: no bytes left over", 0 == cnt, "trailing bytes");

        /* and the version string the handler goes on to interpret */
        pmix_ptl_base_parse_version(version, &major, &minor, &release);
        report("round trip: version parsed to a triplet", 5 == major && 1 == minor && 2 == release,
               "wrong triplet");
    }

    free(msg);
    if (NULL != psec) {
        free(psec);
    }
    if (NULL != cred) {
        free(cred);
    }
    if (NULL != version) {
        free(version);
    }
    if (NULL != bfrops) {
        free(bfrops);
    }
    if (NULL != gds) {
        free(gds);
    }
}

/* A message with no credential at all - the common case, since not every
 * psec module produces one. The zero-length blob must be skipped rather
 * than turned into a zero-byte allocation. */
static void test_no_credential(void)
{
    char *msg, *mg;
    size_t sz, cnt;
    char *psec = NULL, *cred = NULL;
    uint32_t credlen = 1;
    bool parsed = false;

    msg = build_message(&sz, 0, NULL);
    if (NULL == msg) {
        report("no credential: message built", 0, "out of memory");
        return;
    }
    mg = msg;
    cnt = sz;

    PMIX_PTL_GET_STRING(psec);
    PMIX_PTL_GET_U32(credlen);
    PMIX_PTL_GET_BLOB(cred, credlen);
    parsed = true;

error:
    report("no credential: parsed", parsed, "parser bailed out");
    report("no credential: length is zero", 0 == credlen, "non-zero length");
    report("no credential: nothing allocated", NULL == cred, "allocated a blob");
    free(msg);
    if (NULL != psec) {
        free(psec);
    }
    if (NULL != cred) {
        free(cred);
    }
}

/* ---- malformed input -------------------------------------------- */

/* A peer declares a credential far larger than the message it sent.
 *
 * The connection handler's only other guard on this field is a taint
 * ceiling of UINT_MAX-2, while the message itself is capped at
 * PMIX_MAX_CRED_SIZE - so without a check against the bytes remaining,
 * the copy runs off the end of the receive buffer and the subsequent
 * "cnt -= l" underflows the remaining count to a huge value, letting
 * every later field read out of bounds too. */
static void test_credential_longer_than_message(void)
{
    char *msg, *mg;
    size_t sz, cnt;
    char *psec = NULL, *cred = NULL;
    uint32_t credlen = 0;
    bool parsed = false;

    /* declare 4096 bytes of credential but send only 16 */
    msg = build_message(&sz, strlen(TEST_CRED), TEST_CRED);
    if (NULL == msg) {
        report("overlong credential: message built", 0, "out of memory");
        return;
    }
    /* rewrite the declared length in place, immediately after the psec
     * name, leaving the message the size it actually is */
    {
        uint32_t bogus = htonl(4096);
        memcpy(msg + strlen(TEST_PSEC) + 1, &bogus, sizeof(uint32_t));
    }

    mg = msg;
    cnt = sz;
    PMIX_PTL_GET_STRING(psec);
    PMIX_PTL_GET_U32(credlen);
    PMIX_PTL_GET_BLOB(cred, credlen);
    parsed = true;

error:
    report("overlong credential: parser refused it", !parsed, "read past the message");
    report("overlong credential: nothing copied out", NULL == cred, "allocated and copied");
    free(msg);
    if (NULL != psec) {
        free(psec);
    }
    if (NULL != cred) {
        free(cred);
    }
}

/* A string field that carries no terminator inside the bytes we
 * received. GET_STRING must not strdup past the end. */
static void test_unterminated_string(void)
{
    char msg[8];
    char *mg;
    size_t cnt;
    char *psec = NULL;
    bool parsed = false;

    memset(msg, 'a', sizeof(msg)); /* no NUL anywhere */
    mg = msg;
    cnt = sizeof(msg);

    PMIX_PTL_GET_STRING(psec);
    parsed = true;

error:
    report("unterminated string: parser refused it", !parsed, "read past the message");
    report("unterminated string: nothing copied out", NULL == psec, "copied a string");
    if (NULL != psec) {
        free(psec);
    }
}

/* A message that ends in the middle of a four-byte integer. */
static void test_truncated_u32(void)
{
    char msg[2] = {0, 0};
    char *mg;
    size_t cnt;
    uint32_t val = 0xdeadbeef;
    bool parsed = false;

    mg = msg;
    cnt = sizeof(msg);

    PMIX_PTL_GET_U32(val);
    parsed = true;

error:
    report("truncated u32: parser refused it", !parsed, "read past the message");
    report("truncated u32: target left alone", 0xdeadbeef == val, "wrote a partial value");
}

/* A message that ends before its single-byte flag. */
static void test_truncated_u8(void)
{
    char msg[1];
    char *mg;
    size_t cnt;
    uint8_t val = 0xab;
    bool parsed = false;

    mg = msg;
    cnt = 0; /* nothing left */

    PMIX_PTL_GET_U8(val);
    parsed = true;

error:
    report("truncated u8: parser refused it", !parsed, "read past the message");
    report("truncated u8: target left alone", 0xab == val, "wrote a value");
}

/* A procid whose nspace consumes the message, leaving nothing for the
 * rank that must follow it. GET_PROCID has to release the nspace it
 * already copied before it gives up. */
static void test_truncated_procid(void)
{
    char msg[5] = {'a', 'b', 'c', 'd', '\0'};
    char *mg;
    size_t cnt;
    pmix_proc_t proc;
    bool parsed = false;

    PMIX_LOAD_PROCID(&proc, NULL, PMIX_RANK_UNDEF);
    mg = msg;
    cnt = sizeof(msg);

    PMIX_PTL_GET_PROCID(proc);
    parsed = true;

error:
    report("truncated procid: parser refused it", !parsed, "read past the message");
    report("truncated procid: procid left undefined", PMIX_RANK_UNDEF == proc.rank,
           "loaded a rank");
}

int main(int argc, char **argv)
{
    pmix_status_t rc;
    static pmix_server_module_t mymodule = {0};
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    /* the GET macros log through PMIX_ERROR_LOG, so the library has to
     * be up before we drive them */
    rc = PMIx_server_init(&mymodule, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    fprintf(stdout, "\n=== ptl handshake serialization unit tests ===\n\n");

    test_round_trip();
    test_no_credential();
    fprintf(stdout, "\n  (the following cases log the errors they are checking for)\n\n");
    test_credential_longer_than_message();
    test_unterminated_string();
    test_truncated_u32();
    test_truncated_u8();
    test_truncated_procid();

    fprintf(stdout, "\n%d passed, %d failed\n", npass, nfail);

    PMIx_server_finalize();

    return (0 == nfail) ? 0 : 1;
}
