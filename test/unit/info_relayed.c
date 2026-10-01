/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * PMIX_INFO_RELAYED - the mark a server puts on a PMIX_USERID or
 * PMIX_GRPID it is relaying for the process that made a request.
 *
 * Only a server relays, so only a server's library sends the mark: any
 * other process's library clears it when it packs an info, so a process
 * that is not a server speaks only for itself whatever flag it sets. This
 * test process is a tool, not a server.
 *
 * Test cases:
 *
 *   PMIx_Info_relayed / PMIx_Info_is_relayed    -> set and seen
 *   the directives string                       -> names it
 *   packed by a process that is not a server    -> the mark is cleared,
 *                                                  other directives kept
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "include/pmix_tool.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int npass = 0;
static int nfail = 0;

static void report(const char *name, int passed)
{
    if (passed) {
        fprintf(stdout, "  PASS: %s\n", name);
        npass++;
    } else {
        fprintf(stdout, "  FAIL: %s\n", name);
        nfail++;
    }
}

int main(int argc, char **argv)
{
    pmix_info_t info, out, tinfo;
    pmix_data_buffer_t buf;
    pmix_proc_t me;
    pmix_status_t rc;
    uint32_t uid = 4242;
    int32_t cnt = 1;
    char *str;

    (void) argc;
    (void) argv;
    fprintf(stdout, "info_relayed: the relayed-identity mark\n");

    PMIX_INFO_LOAD(&tinfo, PMIX_TOOL_DO_NOT_CONNECT, NULL, PMIX_BOOL);
    rc = PMIx_tool_init(&me, &tinfo, 1);
    PMIX_INFO_DESTRUCT(&tinfo);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_tool_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    PMIX_INFO_LOAD(&info, PMIX_USERID, &uid, PMIX_UINT32);
    report("an info starts unmarked", !PMIx_Info_is_relayed(&info));
    PMIx_Info_relayed(&info);
    PMIx_Info_required(&info);
    report("the mark is set and seen", PMIx_Info_is_relayed(&info));
    str = PMIx_Info_directives_string(info.flags);
    report("the directives string names it", NULL != str && NULL != strstr(str, "RELAYED"));
    free(str);

    PMIX_DATA_BUFFER_CONSTRUCT(&buf);
    rc = PMIx_Data_pack(NULL, &buf, &info, 1, PMIX_INFO);
    PMIX_INFO_CONSTRUCT(&out);
    if (PMIX_SUCCESS == rc) {
        rc = PMIx_Data_unpack(NULL, &buf, &out, &cnt, PMIX_INFO);
    }
    report("packed by a process that is not a server, the mark is cleared",
           PMIX_SUCCESS == rc && PMIx_Check_key(out.key, PMIX_USERID) &&
           uid == out.value.data.uint32 && !PMIx_Info_is_relayed(&out));
    report("and the other directives are kept", PMIX_SUCCESS == rc && PMIx_Info_is_required(&out));
    report("the caller's own info is left as it was", PMIx_Info_is_relayed(&info));
    PMIX_INFO_DESTRUCT(&out);
    PMIX_DATA_BUFFER_DESTRUCT(&buf);
    PMIX_INFO_DESTRUCT(&info);

    (void) PMIx_tool_finalize();
    fprintf(stdout, "info_relayed: %d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
