/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit tests for what the ptl framework's open/register code and its class
 * destructors promise.
 *
 *  - A pmix_ptl_recv_t owns its payload until pmix_ptl_base_process_msg
 *    loads it into a buffer. Every other way a message ends - no recv
 *    posted for its tag, or a connection lost part-way through reading it
 *    - releases the object with the payload still attached, and the
 *    destructor used to leave it behind: up to ptl_base_max_msg_size per
 *    message.
 *
 *  - ptl_base_max_msg_size is given in megabytes and converted to bytes.
 *    A value too large for that conversion wrapped instead of meaning "no
 *    limit" - 2^44 MB wraps to exactly zero on a 64-bit size_t, and a zero
 *    limit rejects every message that carries a payload.
 *
 *  - A port list the parser cannot read, and the "-1" wildcard, both fall
 *    back to the ephemeral port. This runs as two init/finalize cycles, so
 *    the second registration of the framework is exercised as well.
 *
 *  - The listener's role flags come from the directives handed to server
 *    init, and the listener only ever sets the ones it is given. They
 *    used to survive finalize, so a second server init in the same
 *    process inherited the first one's - remote connections and tool
 *    support included. The first cycle here refuses foreign tools and
 *    asks for session support; the second asks for neither.
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "include/pmix_server.h"
#include "src/include/pmix_globals.h"
#include "src/mca/ptl/base/base.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__APPLE__)
#    include <malloc/malloc.h>
#elif defined(__GLIBC__)
#    include <malloc.h>
#endif

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

/* bytes the allocator currently has handed out, where the platform will
 * say - false means the payload case cannot be measured here */
static bool heap_in_use(size_t *out)
{
#if defined(__APPLE__)
    malloc_statistics_t st;
    malloc_zone_statistics(NULL, &st);
    *out = st.size_in_use;
    return true;
#elif defined(__GLIBC__)
#    if __GLIBC_PREREQ(2, 33)
    /* a payload this size is served by mmap, which uordblks leaves out */
    struct mallinfo2 mi = mallinfo2();
    *out = mi.uordblks + mi.hblkhd;
    return true;
#    else
    *out = 0;
    return false;
#    endif
#else
    *out = 0;
    return false;
#endif
}

#define PAYLOAD_SIZE (1024 * 1024)
#define NMSGS        64

static void test_recv_payload_released(void)
{
    size_t before, after;
    char detail[128];
    int i;

    if (!heap_in_use(&before)) {
        fprintf(stdout, "  SKIP: recv payload released (no allocator statistics here)\n");
        return;
    }
    for (i = 0; i < NMSGS; i++) {
        pmix_ptl_recv_t *msg = PMIX_NEW(pmix_ptl_recv_t);
        msg->data = (char *) malloc(PAYLOAD_SIZE);
        memset(msg->data, 1, PAYLOAD_SIZE);
        msg->hdr.nbytes = PAYLOAD_SIZE;
        PMIX_RELEASE(msg);
    }
    heap_in_use(&after);
    /* all of it stranded is NMSGS MB; allow a small fraction of one
     * payload for whatever else the allocator is doing */
    snprintf(detail, sizeof(detail), "%ld KB still in use after %d releases",
             ((long) after - (long) before) / 1024, NMSGS);
    report("recv payload released", after < before + PAYLOAD_SIZE / 2, detail);
}

static void test_max_msg_size_no_wrap(void)
{
    char detail[128];

    snprintf(detail, sizeof(detail), "max_msg_size is %lu",
             (unsigned long) pmix_ptl_base.max_msg_size);
    report("oversized max_msg_size means no limit",
           PMIX_TAINT_SIZE_LIMIT == pmix_ptl_base.max_msg_size, detail);
}

static void test_ports_fallback(const char *spec)
{
    char name[128], detail[128];
    char **ports = pmix_ptl_base.ipv4_ports;

    snprintf(name, sizeof(name), "ipv4_ports \"%s\" falls back to the ephemeral port", spec);
    snprintf(detail, sizeof(detail), "got %d entries, first \"%s\"",
             PMIx_Argv_count(ports), (NULL == ports || NULL == ports[0]) ? "(none)" : ports[0]);
    report(name, 1 == PMIx_Argv_count(ports) && 0 == strcmp(ports[0], "0"), detail);
}

static void test_role_flags_default(void)
{
    char detail[128];

    snprintf(detail, sizeof(detail), "allow_foreign_tools %d, session_tool %d",
             (int) pmix_ptl_base.allow_foreign_tools, (int) pmix_ptl_base.session_tool);
    report("listener role flags start from their defaults",
           pmix_ptl_base.allow_foreign_tools && !pmix_ptl_base.session_tool, detail);
}

static int cycle(const char *spec, bool first)
{
    static pmix_server_module_t mymodule = {0};
    pmix_info_t info[2];
    pmix_status_t rc;
    bool no = false, yes = true;

    setenv("PMIX_MCA_ptl_base_ipv4_ports", spec, 1);
    if (first) {
        PMIX_INFO_LOAD(&info[0], PMIX_SERVER_ALLOW_FOREIGN_TOOLS, &no, PMIX_BOOL);
        PMIX_INFO_LOAD(&info[1], PMIX_SERVER_SESSION_SUPPORT, &yes, PMIX_BOOL);
        rc = PMIx_server_init(&mymodule, info, 2);
    } else {
        rc = PMIx_server_init(&mymodule, NULL, 0);
    }
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }
    test_ports_fallback(spec);
    test_max_msg_size_no_wrap();
    if (first) {
        test_recv_payload_released();
    } else {
        test_role_flags_default();
    }
    PMIx_server_finalize();
    return 0;
}

int main(int argc, char **argv)
{
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    /* 2^44 MB is 2^64 bytes */
    setenv("PMIX_MCA_ptl_base_max_msg_size", "17592186044416", 1);

    fprintf(stdout, "\n=== ptl framework open and class unit tests ===\n\n");

    if (0 != cycle("abc", true)) {
        return 1;
    }
    if (0 != cycle("-1", false)) {
        return 1;
    }

    fprintf(stdout, "\n%d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
