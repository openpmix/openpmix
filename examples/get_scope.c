/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Which of a peer's values PMIx_Get returns, for each scope a get can
 * name, depending on where the peer is.
 *
 * Every rank publishes four keys:
 *
 *   getscope.dual         twice: "L<rank>" at PMIX_LOCAL, "R<rank>" at
 *                         PMIX_REMOTE
 *   getscope.remote_only  "R<rank>" at PMIX_REMOTE
 *   getscope.local_only   "L<rank>" at PMIX_LOCAL
 *   getscope.glob         "G<rank>" at PMIX_GLOBAL
 *   getscope.cpuset       a PMIX_PROC_CPUSET at PMIX_LOCAL - a value whose
 *                         payload a copy keeps in the copier's own heap,
 *                         so it cannot be answered out of shared memory
 *                         and has to reach a same-node reader intact
 *                         another way
 *
 * then commits and runs a fence that collects data.
 *
 * Where the target is relative to the requester decides what the
 * requester may see (see PMIx_Put(3) and PMIx_Get(3)):
 *
 *   - a target on the requester's node: its PMIX_LOCAL and PMIX_GLOBAL
 *     data, never its PMIX_REMOTE data;
 *   - a target on another node: its PMIX_REMOTE and PMIX_GLOBAL data,
 *     never its PMIX_LOCAL data.
 *
 * A PMIX_DATA_SCOPE qualifier names where the target is, not one of its
 * tables: PMIX_LOCAL means "a target on my node", PMIX_REMOTE "a target on
 * another node", PMIX_GLOBAL either. A qualifier that contradicts where
 * the target is finds nothing, PMIX_GLOBAL data included. Data that is not
 * visible is PMIX_ERR_NOT_FOUND, at once.
 *
 * Every rank checks every peer. With no argument each peer is asked every
 * combination of key and scope in turn, which also exercises whatever the
 * library cached from the earlier answers. With a combination number
 * (0..19) only that one is asked, so each peer is asked once and the
 * answer cannot have come from a cache.
 *
 * With no argument, five more checks follow the table:
 *
 *   - a NULL-key get of every peer ("everything it posted") returns
 *     exactly the visible copies of the keys above, once each;
 *   - the legacy form: rank 0 also posts getscope.legacy.{global,local,
 *     remote} at those scopes, and every rank reads them naming no rank -
 *     a NULL proc, and PMIX_RANK_UNDEF with and without a scope - which
 *     is how a PMI-1/PMI-2 style key-value space is read;
 *   - PMIX_RANK_WILDCARD never finds a posted key: it reads job-level data;
 *   - an overwrite between fences: every rank posts getscope.local_only
 *     again with a new value, and a same-node reader must get the new one
 *     once it asks with PMIX_GET_REFRESH_CACHE, then from a plain get,
 *     and again after the next fence;
 *   - many put-commit-fence rounds in a row, each read back from a peer,
 *     the way an MPI library's start-up broadcasts run.
 *
 * test/unit/run_getscope.pl drives this under test/simple/simptest, where
 * every proc is on one node; contrib/dockerswarm/run-gds-tests.sh drives
 * it across nodes. Each rank prints one "getscope: ... PASS" line when
 * every answer was the expected one, and exits non-zero otherwise.
 */

#define _GNU_SOURCE
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "examples.h"
#include <pmix.h>

#define NKEYS   5
#define NSCOPES 4

static pmix_proc_t myproc;

static const char *keys[NKEYS] = {
    "getscope.dual",
    "getscope.remote_only",
    "getscope.local_only",
    "getscope.glob",
    "getscope.cpuset",
};

/* -1 means "no PMIX_DATA_SCOPE qualifier at all" */
static const int scopes[NSCOPES] = {-1, PMIX_LOCAL, PMIX_REMOTE, PMIX_GLOBAL};

static const char *scope_name(int scope)
{
    switch (scope) {
    case PMIX_LOCAL:
        return "LOCAL";
    case PMIX_REMOTE:
        return "REMOTE";
    case PMIX_GLOBAL:
        return "GLOBAL";
    default:
        return "none";
    }
}

/* The first letter of the value expected for (key, scope) from a peer -
 * the peer put "<letter><rank>" - or 0 when the answer must be
 * PMIX_ERR_NOT_FOUND. See the rules at the top of this file. */
static char expected_prefix(int k, int scope, bool local)
{
    /* a qualifier that contradicts where the target is finds nothing */
    if ((local && PMIX_REMOTE == scope) || (!local && PMIX_LOCAL == scope)) {
        return 0;
    }
    switch (k) {
    case 0: /* dual */
        return local ? 'L' : 'R';
    case 1: /* remote_only */
        return local ? 0 : 'R';
    case 2: /* local_only */
        return local ? 'L' : 0;
    case 3: /* glob */
        return 'G';
    default: /* cpuset, put at PMIX_LOCAL */
        return local ? 'C' : 0;
    }
}

static void put(const char *key, pmix_scope_t scope, char prefix)
{
    char buf[64];
    pmix_value_t val;
    pmix_status_t rc;

    snprintf(buf, sizeof(buf), "%c%u", prefix, myproc.rank);
    PMIX_VALUE_LOAD(&val, buf, PMIX_STRING);
    rc = PMIx_Put(scope, key, &val);
    PMIX_VALUE_DESTRUCT(&val);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "getscope: rank %u PMIx_Put %s failed: %s\n", myproc.rank, key,
                PMIx_Error_string(rc));
        exit(1);
    }
}

/* the ranks that share this proc's node, as PMIX_LOCAL_PEERS reports them */
static bool *local_ranks(uint32_t nprocs)
{
    pmix_proc_t wild;
    pmix_value_t *val = NULL;
    pmix_status_t rc;
    bool *local;
    char **peers;
    size_t n;

    local = calloc(nprocs, sizeof(bool));
    if (NULL == local) {
        return NULL;
    }
    PMIX_LOAD_PROCID(&wild, myproc.nspace, PMIX_RANK_WILDCARD);
    rc = PMIx_Get(&wild, PMIX_LOCAL_PEERS, NULL, 0, &val);
    if (PMIX_SUCCESS != rc || NULL == val || PMIX_STRING != val->type ||
        NULL == val->data.string) {
        fprintf(stderr, "getscope: rank %u cannot get PMIX_LOCAL_PEERS: %s\n", myproc.rank,
                PMIx_Error_string(rc));
        free(local);
        if (NULL != val) {
            PMIX_VALUE_RELEASE(val);
        }
        return NULL;
    }
    peers = PMIx_Argv_split(val->data.string, ',');
    for (n = 0; NULL != peers && NULL != peers[n]; n++) {
        unsigned long r = strtoul(peers[n], NULL, 10);
        if (r < nprocs) {
            local[r] = true;
        }
    }
    PMIx_Argv_free(peers);
    PMIX_VALUE_RELEASE(val);
    return local;
}

/* ask one peer for one (key, scope); true if the answer was the expected one */
static bool check(pmix_rank_t peer, int k, int s, bool local)
{
    pmix_proc_t proc;
    pmix_info_t info[2];
    size_t ninfo = 0;
    pmix_value_t *val = NULL;
    pmix_status_t rc;
    pmix_scope_t scope = (pmix_scope_t) scopes[s];
    int timeout = 5;
    char want[64], got[128];
    char prefix;
    bool ok;

    /* bound the get, so a library that waits for data the requester can
     * never see fails this check rather than hanging the run */
    PMIX_INFO_LOAD(&info[ninfo], PMIX_TIMEOUT, &timeout, PMIX_INT);
    ninfo++;
    if (0 <= scopes[s]) {
        PMIX_INFO_LOAD(&info[ninfo], PMIX_DATA_SCOPE, &scope, PMIX_SCOPE);
        ninfo++;
    }
    PMIX_LOAD_PROCID(&proc, myproc.nspace, peer);
    rc = PMIx_Get(&proc, keys[k], info, ninfo, &val);
    PMIX_INFO_DESTRUCT(&info[0]);
    if (1 < ninfo) {
        PMIX_INFO_DESTRUCT(&info[1]);
    }

    prefix = expected_prefix(k, scopes[s], local);
    if (0 == prefix) {
        snprintf(want, sizeof(want), "PMIX_ERR_NOT_FOUND");
    } else if ('C' == prefix) {
        snprintf(want, sizeof(want), "a cpuset");
    } else {
        snprintf(want, sizeof(want), "\"%c%u\"", prefix, peer);
    }
    if ('C' == prefix && PMIX_SUCCESS == rc && NULL != val) {
        /* a cpuset that survived the trip has its own bitmap and source;
         * reading them is the point - a copy that kept another process's
         * pointers faults here */
        if (PMIX_PROC_CPUSET == val->type && NULL != val->data.cpuset &&
            NULL != val->data.cpuset->bitmap && NULL != val->data.cpuset->source &&
            0 == strncmp(val->data.cpuset->source, "hwloc", 5)) {
            snprintf(got, sizeof(got), "a cpuset");
            ok = true;
        } else {
            snprintf(got, sizeof(got), "a malformed %s", PMIx_Data_type_string(val->type));
            ok = false;
        }
    } else if (PMIX_SUCCESS != rc) {
        snprintf(got, sizeof(got), "%s", PMIx_Error_string(rc));
        ok = (0 == prefix && PMIX_ERR_NOT_FOUND == rc);
    } else if (NULL == val) {
        snprintf(got, sizeof(got), "success with no value");
        ok = false;
    } else if (PMIX_STRING != val->type) {
        /* two copies of one key come back as a data array */
        snprintf(got, sizeof(got), "a %s", PMIx_Data_type_string(val->type));
        ok = false;
    } else {
        snprintf(got, sizeof(got), "\"%s\"", val->data.string);
        ok = (0 == strcmp(got, want));
    }
    if (NULL != val) {
        PMIX_VALUE_RELEASE(val);
    }
    if (!ok) {
        fprintf(stderr,
                "getscope: rank %u peer %u (%s node) key %s scope %s: got %s, expected %s: FAIL\n",
                myproc.rank, peer, local ? "same" : "other", keys[k], scope_name(scopes[s]), got,
                want);
    }
    return ok;
}

#define LEGACY_GLOBAL "getscope.legacy.global"
#define LEGACY_LOCAL  "getscope.legacy.local"
#define LEGACY_REMOTE "getscope.legacy.remote"
#define NROUNDS       20

/* One get, with optional scope and directives; "want" NULL means the
 * answer must be PMIX_ERR_NOT_FOUND. Reports and returns false on a
 * wrong answer. */
static bool expect(const char *what, const pmix_proc_t *proc, const char *key, int scope,
                   bool immediate, bool refresh, const char *want)
{
    pmix_info_t info[4];
    size_t ninfo = 0, n;
    pmix_value_t *val = NULL;
    pmix_status_t rc;
    pmix_scope_t sc = (pmix_scope_t) scope;
    int timeout = 5;
    char got[128];
    bool ok;

    PMIX_INFO_LOAD(&info[ninfo], PMIX_TIMEOUT, &timeout, PMIX_INT);
    ninfo++;
    if (0 <= scope) {
        PMIX_INFO_LOAD(&info[ninfo], PMIX_DATA_SCOPE, &sc, PMIX_SCOPE);
        ninfo++;
    }
    if (immediate) {
        PMIX_INFO_LOAD(&info[ninfo], PMIX_IMMEDIATE, NULL, PMIX_BOOL);
        ninfo++;
    }
    if (refresh) {
        PMIX_INFO_LOAD(&info[ninfo], PMIX_GET_REFRESH_CACHE, NULL, PMIX_BOOL);
        ninfo++;
    }
    rc = PMIx_Get(proc, key, info, ninfo, &val);
    for (n = 0; n < ninfo; n++) {
        PMIX_INFO_DESTRUCT(&info[n]);
    }
    if (PMIX_SUCCESS != rc) {
        snprintf(got, sizeof(got), "%s", PMIx_Error_string(rc));
        ok = (NULL == want && PMIX_ERR_NOT_FOUND == rc);
    } else if (NULL == val || PMIX_STRING != val->type) {
        snprintf(got, sizeof(got), "a %s", (NULL == val) ? "NULL value"
                                                         : PMIx_Data_type_string(val->type));
        ok = false;
    } else {
        snprintf(got, sizeof(got), "\"%s\"", val->data.string);
        ok = (NULL != want && 0 == strcmp(val->data.string, want));
    }
    if (NULL != val) {
        PMIX_VALUE_RELEASE(val);
    }
    if (!ok) {
        fprintf(stderr, "getscope: rank %u %s key %s scope %s: got %s, expected %s%s%s: FAIL\n",
                myproc.rank, what, key, scope_name(scope), got, (NULL == want) ? "" : "\"",
                (NULL == want) ? "PMIX_ERR_NOT_FOUND" : want, (NULL == want) ? "" : "\"");
    }
    return ok;
}

/* "Everything the peer posted": each of our keys must appear exactly as
 * often as it is visible from here - once or not at all - and with the
 * copy a get of it alone would return. */
static bool check_all(pmix_rank_t peer, bool local)
{
    pmix_proc_t proc;
    pmix_info_t tinfo;
    pmix_value_t *val = NULL;
    pmix_status_t rc;
    pmix_info_t *iptr;
    size_t n, ninfo;
    int timeout = 5, k, seen[NKEYS] = {0};
    bool ok = true;
    char prefix, want[32];

    PMIX_INFO_LOAD(&tinfo, PMIX_TIMEOUT, &timeout, PMIX_INT);
    PMIX_LOAD_PROCID(&proc, myproc.nspace, peer);
    rc = PMIx_Get(&proc, NULL, &tinfo, 1, &val);
    PMIX_INFO_DESTRUCT(&tinfo);
    if (PMIX_SUCCESS != rc || NULL == val || PMIX_DATA_ARRAY != val->type ||
        NULL == val->data.darray || PMIX_INFO != val->data.darray->type) {
        fprintf(stderr, "getscope: rank %u peer %u NULL-key get: %s: FAIL\n", myproc.rank, peer,
                (PMIX_SUCCESS != rc) ? PMIx_Error_string(rc) : "not an info array");
        if (NULL != val) {
            PMIX_VALUE_RELEASE(val);
        }
        return false;
    }
    iptr = (pmix_info_t *) val->data.darray->array;
    ninfo = val->data.darray->size;
    for (n = 0; n < ninfo; n++) {
        for (k = 0; k < NKEYS; k++) {
            if (0 != strncmp(iptr[n].key, keys[k], PMIX_MAX_KEYLEN)) {
                continue;
            }
            seen[k]++;
            prefix = expected_prefix(k, -1, local);
            if ('C' == prefix) {
                if (PMIX_PROC_CPUSET != iptr[n].value.type || NULL == iptr[n].value.data.cpuset ||
                    NULL == iptr[n].value.data.cpuset->bitmap) {
                    fprintf(stderr, "getscope: rank %u peer %u NULL-key get: %s malformed: FAIL\n",
                            myproc.rank, peer, keys[k]);
                    ok = false;
                }
            } else if (0 != prefix) {
                snprintf(want, sizeof(want), "%c%u", prefix, peer);
                if (PMIX_STRING != iptr[n].value.type ||
                    0 != strcmp(iptr[n].value.data.string, want)) {
                    fprintf(stderr,
                            "getscope: rank %u peer %u NULL-key get: %s is not \"%s\": FAIL\n",
                            myproc.rank, peer, keys[k], want);
                    ok = false;
                }
            }
        }
    }
    for (k = 0; k < NKEYS; k++) {
        int wanted = (0 == expected_prefix(k, -1, local)) ? 0 : 1;
        if (seen[k] != wanted) {
            fprintf(stderr,
                    "getscope: rank %u peer %u (%s node) NULL-key get: %s appears %d times, "
                    "expected %d: FAIL\n",
                    myproc.rank, peer, local ? "same" : "other", keys[k], seen[k], wanted);
            ok = false;
        }
    }
    PMIX_VALUE_RELEASE(val);
    return ok;
}

/* "Everything I posted": a process sees all of its own keys, whatever
 * their scope, each once - beside whatever the host said about it */
static bool check_self(void)
{
    pmix_info_t tinfo;
    pmix_value_t *val = NULL;
    pmix_status_t rc;
    pmix_info_t *iptr;
    size_t n;
    int timeout = 5, k, seen[NKEYS] = {0};
    bool ok = true;

    PMIX_INFO_LOAD(&tinfo, PMIX_TIMEOUT, &timeout, PMIX_INT);
    rc = PMIx_Get(&myproc, NULL, &tinfo, 1, &val);
    PMIX_INFO_DESTRUCT(&tinfo);
    if (PMIX_SUCCESS != rc || NULL == val || PMIX_DATA_ARRAY != val->type ||
        NULL == val->data.darray || PMIX_INFO != val->data.darray->type) {
        fprintf(stderr, "getscope: rank %u NULL-key get of itself: %s: FAIL\n", myproc.rank,
                (PMIX_SUCCESS != rc) ? PMIx_Error_string(rc) : "not an info array");
        if (NULL != val) {
            PMIX_VALUE_RELEASE(val);
        }
        return false;
    }
    iptr = (pmix_info_t *) val->data.darray->array;
    for (n = 0; n < val->data.darray->size; n++) {
        for (k = 0; k < NKEYS; k++) {
            if (0 == strncmp(iptr[n].key, keys[k], PMIX_MAX_KEYLEN)) {
                seen[k]++;
            }
        }
    }
    for (k = 0; k < NKEYS; k++) {
        if (1 != seen[k]) {
            fprintf(stderr, "getscope: rank %u NULL-key get of itself: %s appears %d times, "
                            "expected 1: FAIL\n", myproc.rank, keys[k], seen[k]);
            ok = false;
        }
    }
    PMIX_VALUE_RELEASE(val);
    return ok;
}

static void fence(bool collect, const char *why)
{
    pmix_proc_t wild;
    pmix_info_t info;
    pmix_status_t rc;

    PMIX_LOAD_PROCID(&wild, myproc.nspace, PMIX_RANK_WILDCARD);
    PMIX_INFO_LOAD(&info, PMIX_COLLECT_DATA, &collect, PMIX_BOOL);
    rc = PMIx_Fence(&wild, 1, &info, 1);
    PMIX_INFO_DESTRUCT(&info);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "getscope: rank %u %s PMIx_Fence failed: %s\n", myproc.rank, why,
                PMIx_Error_string(rc));
        exit(1);
    }
}

static void commit(void)
{
    pmix_status_t rc = PMIx_Commit();
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "getscope: rank %u PMIx_Commit failed: %s\n", myproc.rank,
                PMIx_Error_string(rc));
        exit(1);
    }
}

/* the legacy form - keys that name no process (see "Legacy" in
 * docs/how-things-work/data_scope.rst). Every answer is asked for with
 * PMIX_IMMEDIATE: after a collecting fence everything this reader may
 * see is already at its server, and a key it may not see never will be */
static int check_legacy(bool local0)
{
    pmix_proc_t undef;
    char g0[] = "G0", l0[] = "L0", r0[] = "R0";
    int nbad = 0;
    bool me = (0 == myproc.rank);

    PMIX_LOAD_PROCID(&undef, myproc.nspace, PMIX_RANK_UNDEF);
    /* a NULL proc and PMIX_RANK_UNDEF are the same question */
    nbad += !expect("NULL proc", NULL, LEGACY_GLOBAL, -1, true, false, g0);
    nbad += !expect("NULL proc", NULL, LEGACY_LOCAL, -1, true, false, (me || local0) ? l0 : NULL);
    nbad += !expect("NULL proc", NULL, LEGACY_REMOTE, -1, true, false, (me || !local0) ? r0 : NULL);
    nbad += !expect("any rank", &undef, LEGACY_GLOBAL, -1, true, false, g0);
    nbad += !expect("any rank", &undef, LEGACY_LOCAL, -1, true, false, (me || local0) ? l0 : NULL);
    nbad += !expect("any rank", &undef, LEGACY_REMOTE, -1, true, false,
                    (me || !local0) ? r0 : NULL);
    if (!me) {
        /* a scope narrows the ranks searched to one side of the node boundary */
        nbad += !expect("any rank", &undef, LEGACY_GLOBAL, PMIX_LOCAL, true, false,
                        local0 ? g0 : NULL);
        nbad += !expect("any rank", &undef, LEGACY_GLOBAL, PMIX_REMOTE, true, false,
                        local0 ? NULL : g0);
    }
    return nbad;
}

int main(int argc, char **argv)
{
    pmix_status_t rc;
    pmix_proc_t wild;
    pmix_value_t *val = NULL;
    pmix_info_t info;
    bool collect = true;
    bool *local;
    uint32_t nprocs = 0;
    int only = -1;
    int nbad = 0, nchecked = 0;
    pmix_rank_t peer;
    int c, k, s;

    if (1 < argc) {
        only = atoi(argv[1]);
        if (0 > only || NKEYS * NSCOPES <= only) {
            fprintf(stderr, "getscope: combination must be 0..%d\n", NKEYS * NSCOPES - 1);
            exit(1);
        }
    }

    if (PMIX_SUCCESS != (rc = PMIx_Init(&myproc, NULL, 0))) {
        fprintf(stderr, "getscope: PMIx_Init failed: %s\n", PMIx_Error_string(rc));
        exit(1);
    }
    PMIX_LOAD_PROCID(&wild, myproc.nspace, PMIX_RANK_WILDCARD);
    rc = PMIx_Get(&wild, PMIX_JOB_SIZE, NULL, 0, &val);
    if (PMIX_SUCCESS != rc || NULL == val) {
        fprintf(stderr, "getscope: rank %u cannot get the job size: %s\n", myproc.rank,
                PMIx_Error_string(rc));
        exit(1);
    }
    PMIX_VALUE_GET_NUMBER(rc, val, nprocs, uint32_t);
    PMIX_VALUE_RELEASE(val);
    if (PMIX_SUCCESS != rc || 2 > nprocs) {
        fprintf(stderr, "getscope: needs at least 2 procs\n");
        exit(1);
    }
    local = local_ranks(nprocs);
    if (NULL == local) {
        exit(1);
    }

    put(keys[0], PMIX_LOCAL, 'L');
    put(keys[0], PMIX_REMOTE, 'R');
    put(keys[1], PMIX_REMOTE, 'R');
    put(keys[2], PMIX_LOCAL, 'L');
    put(keys[3], PMIX_GLOBAL, 'G');
    if (0 == myproc.rank) {
        put(LEGACY_GLOBAL, PMIX_GLOBAL, 'G');
        put(LEGACY_LOCAL, PMIX_LOCAL, 'L');
        put(LEGACY_REMOTE, PMIX_REMOTE, 'R');
    }
    {
        pmix_cpuset_t cpuset;
        pmix_value_t cval;

        PMIx_Cpuset_construct(&cpuset);
        rc = PMIx_Parse_cpuset_string("hwloc:0", &cpuset);
        if (PMIX_SUCCESS != rc) {
            fprintf(stderr, "getscope: rank %u cannot build a cpuset: %s\n", myproc.rank,
                    PMIx_Error_string(rc));
            exit(1);
        }
        PMIX_VALUE_LOAD(&cval, &cpuset, PMIX_PROC_CPUSET);
        rc = PMIx_Put(PMIX_LOCAL, keys[4], &cval);
        PMIX_VALUE_DESTRUCT(&cval);
        PMIx_Cpuset_destruct(&cpuset);
        if (PMIX_SUCCESS != rc) {
            fprintf(stderr, "getscope: rank %u PMIx_Put %s failed: %s\n", myproc.rank, keys[4],
                    PMIx_Error_string(rc));
            exit(1);
        }
    }
    if (PMIX_SUCCESS != (rc = PMIx_Commit())) {
        fprintf(stderr, "getscope: rank %u PMIx_Commit failed: %s\n", myproc.rank,
                PMIx_Error_string(rc));
        exit(1);
    }
    /* GETSCOPE_COLLECT=0 fences without collecting, so every get of a
     * peer on another node has to be answered by that peer's server on
     * demand (direct modex) instead of out of a modex already here */
    if (NULL != getenv("GETSCOPE_COLLECT") && 0 == strcmp(getenv("GETSCOPE_COLLECT"), "0")) {
        collect = false;
    }
    PMIX_INFO_LOAD(&info, PMIX_COLLECT_DATA, &collect, PMIX_BOOL);
    rc = PMIx_Fence(&wild, 1, &info, 1);
    PMIX_INFO_DESTRUCT(&info);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "getscope: rank %u PMIx_Fence failed: %s\n", myproc.rank,
                PMIx_Error_string(rc));
        exit(1);
    }

    for (peer = 0; peer < nprocs; peer++) {
        if (peer == myproc.rank) {
            continue;
        }
        for (c = 0; c < NKEYS * NSCOPES; c++) {
            if (0 <= only && c != only) {
                continue;
            }
            k = c / NSCOPES;
            s = c % NSCOPES;
            if (!check(peer, k, s, local[peer])) {
                nbad++;
            }
            nchecked++;
        }
    }

    if (0 > only) {
        pmix_proc_t proc;
        char want[32];
        int i;
        bool anylocal = false;

        /* everything this process, and each peer, posted */
        nbad += !check_self();
        nchecked++;
        for (peer = 0; peer < nprocs; peer++) {
            if (peer != myproc.rank) {
                nbad += !check_all(peer, local[peer]);
                nchecked++;
                anylocal |= local[peer];
            }
        }

        /* keys that name no process - only once the fence has collected
         * them: a get naming no rank has nobody to ask on demand */
        if (collect) {
            nbad += check_legacy(local[0]);
            nchecked += 8;
            /* job-level data only, never a posted key */
            nbad += !expect("wildcard rank", &wild, LEGACY_GLOBAL, -1, true, false, NULL);
            nchecked++;
        }

        /* everyone is done reading the first values before anyone
         * replaces one - otherwise a peer's read races its overwrite */
        fence(collect, "pre-overwrite");

        /* An overwrite between fences. Every rank posts local_only again;
         * a same-node reader asks with PMIX_GET_REFRESH_CACHE until the
         * new value shows (the writer may not have committed yet), and
         * then a plain get must give it too - no older copy, in a cache
         * or in shared memory, may answer instead. */
        put(keys[2], PMIX_LOCAL, 'M');
        commit();
        for (peer = 0; peer < nprocs; peer++) {
            pmix_value_t *v = NULL;
            bool fresh = false;

            if (peer == myproc.rank || !local[peer]) {
                continue;
            }
            PMIX_LOAD_PROCID(&proc, myproc.nspace, peer);
            snprintf(want, sizeof(want), "M%u", peer);
            for (i = 0; i < 100 && !fresh; i++) {
                pmix_info_t rinfo;

                PMIX_INFO_LOAD(&rinfo, PMIX_GET_REFRESH_CACHE, NULL, PMIX_BOOL);
                if (PMIX_SUCCESS == PMIx_Get(&proc, keys[2], &rinfo, 1, &v) && NULL != v) {
                    fresh = (PMIX_STRING == v->type && 0 == strcmp(v->data.string, want));
                    PMIX_VALUE_RELEASE(v);
                    v = NULL;
                }
                PMIX_INFO_DESTRUCT(&rinfo);
                if (!fresh) {
                    usleep(100000);
                }
            }
            if (!fresh) {
                fprintf(stderr, "getscope: rank %u peer %u overwrite never seen after refresh: "
                                "FAIL\n", myproc.rank, peer);
                nbad++;
            }
            nbad += !expect("after refresh", &proc, keys[2], -1, false, false, want);
            nchecked += 2;
        }
        fence(collect, "post-overwrite");
        for (peer = 0; peer < nprocs; peer++) {
            if (peer == myproc.rank || !local[peer]) {
                continue;
            }
            PMIX_LOAD_PROCID(&proc, myproc.nspace, peer);
            snprintf(want, sizeof(want), "M%u", peer);
            nbad += !expect("after the next fence", &proc, keys[2], -1, false, false, want);
            nchecked++;
        }
        (void) anylocal;

        /* many rounds of put, commit, fence, read - each one adds to the
         * data a fence collects, and on a shared-memory datastore each one
         * is a new generation */
        for (i = 0; i < NROUNDS; i++) {
            char key[64];

            snprintf(key, sizeof(key), "getscope.round.%d", i);
            put(key, PMIX_GLOBAL, 'N');
            commit();
            fence(collect, "round");
            peer = (myproc.rank + 1) % nprocs;
            PMIX_LOAD_PROCID(&proc, myproc.nspace, peer);
            snprintf(want, sizeof(want), "N%u", peer);
            nbad += !expect("round", &proc, key, -1, false, false, want);
            nchecked++;
        }
    }
    free(local);

    /* nobody leaves while a peer may still be asking it for data */
    rc = PMIx_Fence(&wild, 1, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "getscope: rank %u final PMIx_Fence failed: %s\n", myproc.rank,
                PMIx_Error_string(rc));
        nbad++;
    }
    if (0 == nbad) {
        fprintf(stderr, "getscope: rank %u checked %d gets: PASS\n", myproc.rank, nchecked);
    } else {
        fprintf(stderr, "getscope: rank %u %d of %d gets wrong: FAIL\n", myproc.rank, nbad,
                nchecked);
    }
    PMIx_Finalize(NULL, 0);
    return (0 == nbad) ? 0 : 1;
}
