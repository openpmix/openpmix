/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * Unit tests for the pstat framework's request-parsing helpers
 * (src/mca/pstat/base/pstat_base_fns.c).
 *
 * These four helpers translate the caller's "which statistics do you
 * want" info array into the framework's spec structs, and the disk/net
 * pair additionally collect PMIX_DISK_ID / PMIX_NETWORK_ID values into an
 * argv of devices to report.
 *
 * The array they walk is the data array carried inside the monitor value,
 * and for a request that arrived from a client it came straight off the
 * wire - nothing between the unpack and these helpers validates it. So
 * the central case here is a malformed one: an ID entry whose declared
 * type is not PMIX_STRING. Reading value.data.string out of such an entry
 * reinterprets whatever the sender put in the union as a pointer and
 * hands it to strdup, which is a wild read driven entirely by a remote
 * peer. The helpers must reject the entry instead.
 *
 * The helpers touch no library state - they only read the caller's array
 * and write the caller's structs - so these tests run without
 * initializing PMIx.
 *
 * The rejection cases deliberately drive PMIX_ERROR_LOG, so a passing run
 * still prints a few "PMIX ERROR: ... BAD-PARAM" lines. That output is
 * expected; the exit status is what reports the verdict.
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "src/include/pmix_globals.h"
#include "src/mca/pstat/base/base.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* Build entries by hand rather than with PMIX_INFO_LOAD: the point of
 * several of these is to carry a type that does not match the value, which
 * a loader would not let us express. Strings are borrowed literals, so no
 * entry here is ever destructed - the helpers only read. */
static void set_str(pmix_info_t *ip, const char *key, const char *val)
{
    memset(ip, 0, sizeof(*ip));
    PMIx_Load_key(ip->key, key);
    ip->value.type = PMIX_STRING;
    ip->value.data.string = (char *) val;
}

static void set_flag(pmix_info_t *ip, const char *key)
{
    memset(ip, 0, sizeof(*ip));
    PMIx_Load_key(ip->key, key);
    ip->value.type = PMIX_BOOL;
    ip->value.data.flag = true;
}

/* an entry that claims a key whose published type is char*, but declares
 * an integer type - the union then holds a value that is not a pointer */
static void set_wrong_type(pmix_info_t *ip, const char *key, size_t bits)
{
    memset(ip, 0, sizeof(*ip));
    PMIx_Load_key(ip->key, key);
    ip->value.type = PMIX_SIZE;
    ip->value.data.size = bits;
}

static size_t argv_len(char **argv)
{
    size_t n = 0;

    if (NULL == argv) {
        return 0;
    }
    while (NULL != argv[n]) {
        ++n;
    }
    return n;
}

/* a value that is certain to fault if it is ever treated as a pointer */
#define BOGUS_PTR_BITS ((size_t) 0x0badc0de1)

int main(int argc, char **argv)
{
    pmix_info_t info[6];
    pmix_dkstats_t dk;
    pmix_netstats_t net;
    pmix_procstats_t proc;
    pmix_ndstats_t nd;
    pmix_dkstats_t zerodk;
    char **ids;

    (void) argc;
    (void) argv;

    memset(&zerodk, 0, sizeof(zerodk));

    /* ---- the regression case: a wrongly-typed device ID ------------- */

    /* PMIX_DISK_ID carrying an integer must be rejected outright, not
     * strdup'd as if the integer were a char* */
    set_wrong_type(&info[0], PMIX_DISK_ID, BOGUS_PTR_BITS);
    ids = NULL;
    pmix_pstat_parse_dkstats(&ids, &dk, info, 1);
    report("a wrongly-typed PMIX_DISK_ID is rejected", NULL == ids);
    PMIx_Argv_free(ids);

    set_wrong_type(&info[0], PMIX_NETWORK_ID, BOGUS_PTR_BITS);
    ids = NULL;
    pmix_pstat_parse_netstats(&ids, &net, info, 1);
    report("a wrongly-typed PMIX_NETWORK_ID is rejected", NULL == ids);
    PMIx_Argv_free(ids);

    /* a correctly-typed entry whose string is NULL is equally unusable */
    set_str(&info[0], PMIX_DISK_ID, NULL);
    ids = NULL;
    pmix_pstat_parse_dkstats(&ids, &dk, info, 1);
    report("a NULL PMIX_DISK_ID string is rejected", NULL == ids);
    PMIx_Argv_free(ids);

    /* one bad entry must not cost the good ones in the same array */
    set_str(&info[0], PMIX_DISK_ID, "sda");
    set_wrong_type(&info[1], PMIX_DISK_ID, BOGUS_PTR_BITS);
    set_str(&info[2], PMIX_DISK_ID, "sdb");
    ids = NULL;
    pmix_pstat_parse_dkstats(&ids, &dk, info, 3);
    report("a bad entry does not discard the good ones in the same array",
           2 == argv_len(ids) && NULL != ids && 0 == strcmp(ids[0], "sda")
               && 0 == strcmp(ids[1], "sdb"));
    PMIx_Argv_free(ids);

    /* ---- the ID list is a set --------------------------------------- */

    set_str(&info[0], PMIX_DISK_ID, "sda");
    set_str(&info[1], PMIX_DISK_ID, "sdb");
    set_str(&info[2], PMIX_DISK_ID, "sda");
    ids = NULL;
    pmix_pstat_parse_dkstats(&ids, &dk, info, 3);
    report("a repeated disk ID is collected once", 2 == argv_len(ids));
    PMIx_Argv_free(ids);

    set_str(&info[0], PMIX_NETWORK_ID, "eth0");
    set_str(&info[1], PMIX_NETWORK_ID, "eth0");
    ids = NULL;
    pmix_pstat_parse_netstats(&ids, &net, info, 2);
    report("a repeated network ID is collected once", 1 == argv_len(ids));
    PMIx_Argv_free(ids);

    /* ---- field selection -------------------------------------------- */

    /* exactly the named fields come on, and nothing else */
    set_flag(&info[0], PMIX_DISK_READ_COMPLETED);
    set_flag(&info[1], PMIX_DISK_IO_WEIGHTED);
    ids = NULL;
    pmix_pstat_parse_dkstats(&ids, &dk, info, 2);
    report("named disk fields come on and no others",
           dk.rdcompleted && dk.ioweight && !dk.rdmerged && !dk.wrtcompleted
               && !dk.ioms && !dk.ioprog);
    report("field keys do not add device IDs", NULL == ids);
    PMIx_Argv_free(ids);

    /* a key belonging to another category is simply not recognized */
    set_flag(&info[0], PMIX_NODE_LOAD_AVG);
    ids = NULL;
    pmix_pstat_parse_dkstats(&ids, &dk, info, 1);
    report("a key from another category selects no disk field",
           0 == memcmp(&dk, &zerodk, sizeof(dk)));
    PMIx_Argv_free(ids);

    set_flag(&info[0], PMIX_PROC_RSS);
    set_flag(&info[1], PMIX_PROC_NUM_THREADS);
    pmix_pstat_parse_procstats(&proc, info, 2);
    report("named proc fields come on and no others",
           proc.rss && proc.nthreads && !proc.vsize && !proc.pctcpu && !proc.state
               && !proc.cmdline);

    /* every field the components can emit must be reachable by naming it.
     * PMIX_CMD_LINE is the one that is easy to leave out of the parser,
     * because the struct member is spelled differently from the key and
     * because the "select everything" path turns it on regardless - so a
     * caller asking for just the command line got silence rather than an
     * error. */
    set_flag(&info[0], PMIX_CMD_LINE);
    pmix_pstat_parse_procstats(&proc, info, 1);
    report("PMIX_CMD_LINE can be requested on its own",
           proc.cmdline && !proc.rss && !proc.state);

    set_flag(&info[0], PMIX_NODE_MEM_FREE);
    pmix_pstat_parse_ndstats(&nd, info, 1);
    report("named node fields come on and no others",
           nd.mfree && !nd.mtot && !nd.la && !nd.la15);

    set_flag(&info[0], PMIX_NET_SENT_BYTES);
    ids = NULL;
    pmix_pstat_parse_netstats(&ids, &net, info, 1);
    report("named network fields come on and no others",
           net.sntb && !net.rcvdb && !net.snte && !net.sntp);
    PMIx_Argv_free(ids);

    /* ---- the two "nothing specified" cases are not the same ---------- */

    /* a NULL array means "everything" ... */
    ids = NULL;
    pmix_pstat_parse_dkstats(&ids, &dk, NULL, 0);
    report("a NULL info array selects every disk field",
           dk.rdcompleted && dk.rdmerged && dk.rdsectors && dk.rdms && dk.wrtcompleted
               && dk.wrtmerged && dk.wrtsectors && dk.wrtms && dk.ioprog && dk.ioms
               && dk.ioweight);
    report("a NULL info array names no specific device", NULL == ids);
    PMIx_Argv_free(ids);

    pmix_pstat_parse_procstats(&proc, NULL, 0);
    report("a NULL info array selects every proc field",
           proc.cmdline && proc.pctcpu && proc.state && proc.time && proc.pri
               && proc.nthreads && proc.cpu && proc.vsize && proc.pkvsize && proc.rss
               && proc.pss);

    pmix_pstat_parse_ndstats(&nd, NULL, 0);
    report("a NULL info array selects every node field",
           nd.la && nd.la5 && nd.la15 && nd.mtot && nd.mfree && nd.mbuf && nd.mcached
               && nd.mswapcached && nd.mswaptot && nd.mswapfree && nd.mmap);

    ids = NULL;
    pmix_pstat_parse_netstats(&ids, &net, NULL, 0);
    report("a NULL info array selects every network field",
           net.rcvdb && net.rcvdp && net.rcvde && net.sntb && net.sntp && net.snte);
    PMIx_Argv_free(ids);

    /* ... while a present-but-empty array means "nothing". The asymmetry
     * is deliberate: a component hands these helpers darray->array and
     * darray->size, so an empty data array must not turn into a request
     * for every statistic the node can produce. */
    ids = NULL;
    pmix_pstat_parse_dkstats(&ids, &dk, info, 0);
    report("an empty info array selects no disk field",
           0 == memcmp(&dk, &zerodk, sizeof(dk)));
    report("an empty info array names no specific device", NULL == ids);
    PMIx_Argv_free(ids);

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);

    return (nfail > 0) ? 1 : 0;
}
