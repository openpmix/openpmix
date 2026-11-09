/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/* Compute PMIX_ANL_MAP - the process mapping in the notation PMI-1 and
 * PMI-2 define for their PMI_process_mapping attribute - from the job
 * data this server already holds, for a host that did not supply it.
 *
 * The notation is
 *
 *     (vector,(X,Y,Z),(X,Y,Z),...)
 *
 * where each (X,Y,Z) block says "Y nodes, with ids X through X+Y-1, each
 * given the next Z ranks in turn". A reader expands the blocks in order,
 * and if ranks remain when it reaches the end it starts over at the first
 * block - so a cyclic placement of any length is one block. Only that
 * form is produced: the bracketed repeat syntax some readers also accept
 * is a later extension that older PMI-2 consumers do not parse.
 *
 * Node ids carry no meaning beyond identity - readers renumber them - so
 * nodes are numbered in the order they first appear in rank order. That
 * is what makes the common placements compress: by-node and by-slot both
 * become a single block. */

#include "src/include/pmix_config.h"

#include "src/include/pmix_stdint.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "include/pmix_server.h"
#include "src/include/pmix_globals.h"
#include "src/mca/gds/base/base.h"
#include "src/util/pmix_output.h"

#include "src/server/pmix_server_ops.h"

/* PMI-2 bounds every value at PMI2_MAX_VALLEN (1024 bytes, terminator
 * included), and the consumers of this notation copy it into a buffer of
 * that size - MPICH's PMIx support does exactly that. A longer string
 * would reach them truncated, and a truncated map does not parse, so a
 * placement that cannot be said in that space is not offered at all and
 * the reader falls back to working the map out for itself. */
#define PMIX_ANL_MAP_MAX_LEN 1024

typedef struct {
    uint32_t start; // id of the first node in the block
    uint32_t count; // how many consecutive node ids the block covers
    uint32_t ppn;   // ranks placed on each of them in turn
} anl_block_t;

/* fetch one job-level or node-level value for nspace from our own store.
 * Never asks the host: PMIX_OPTIONAL confines the search to what we hold,
 * which is the only answer this computation can be built from */
static pmix_status_t fetch_value(const char *nspace, pmix_rank_t rank, const char *key,
                                 const char *hostname, pmix_value_t **out)
{
    pmix_cb_t cb;
    pmix_proc_t proc;
    pmix_info_t info[3];
    size_t ninfo = 1, n;
    pmix_kval_t *kv;
    pmix_status_t rc;

    *out = NULL;
    PMIX_INFO_LOAD(&info[0], PMIX_OPTIONAL, NULL, PMIX_BOOL);
    if (NULL != hostname) {
        PMIX_INFO_LOAD(&info[1], PMIX_NODE_INFO, NULL, PMIX_BOOL);
        PMIX_INFO_LOAD(&info[2], PMIX_HOSTNAME, hostname, PMIX_STRING);
        ninfo = 3;
    }

    PMIX_LOAD_PROCID(&proc, nspace, rank);
    PMIX_CONSTRUCT(&cb, pmix_cb_t);
    cb.proc = &proc;
    cb.key = (char *) key;
    cb.scope = PMIX_INTERNAL;
    cb.info = info;
    cb.ninfo = ninfo;
    PMIX_GDS_FETCH_KV(rc, pmix_globals.mypeer, &cb);
    cb.info = NULL;
    cb.ninfo = 0;
    cb.key = NULL;
    if (PMIX_SUCCESS == rc) {
        kv = (pmix_kval_t *) pmix_list_remove_first(&cb.kvs);
        if (NULL == kv || NULL == kv->value) {
            rc = PMIX_ERR_NOT_FOUND;
        } else {
            /* take the value and leave the kval nothing to free */
            *out = kv->value;
            kv->value = NULL;
        }
        if (NULL != kv) {
            PMIX_RELEASE(kv);
        }
    }
    PMIX_DESTRUCT(&cb);
    for (n = 0; n < ninfo; n++) {
        PMIX_INFO_DESTRUCT(&info[n]);
    }
    return rc;
}

/* fill nodemap[rank] with the index of the node, in PMIX_NODE_LIST
 * order, that hosts the rank. Every rank must be placed exactly once:
 * a rank missing or claimed by two nodes means the data we hold does not
 * describe the job, and a map built from it would be wrong */
static pmix_status_t build_nodemap(const char *nspace, uint32_t nprocs, uint32_t *nodemap,
                                   uint32_t *nnodes)
{
    pmix_value_t *val = NULL;
    char **nodes = NULL, **peers;
    char *end;
    unsigned long r;
    uint32_t n, nplaced = 0;
    size_t m;
    pmix_status_t rc;

    rc = fetch_value(nspace, PMIX_RANK_WILDCARD, PMIX_NODE_LIST, NULL, &val);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    if (PMIX_STRING != val->type || NULL == val->data.string) {
        PMIX_VALUE_RELEASE(val);
        return PMIX_ERR_NOT_FOUND;
    }
    nodes = PMIx_Argv_split(val->data.string, ',');
    PMIX_VALUE_RELEASE(val);
    if (NULL == nodes) {
        return PMIX_ERR_NOT_FOUND;
    }
    if ((size_t) UINT32_MAX <= (size_t) PMIx_Argv_count(nodes)) {
        PMIx_Argv_free(nodes);
        return PMIX_ERR_NOT_SUPPORTED;
    }

    for (r = 0; r < nprocs; r++) {
        nodemap[r] = UINT32_MAX;
    }

    for (n = 0; NULL != nodes[n]; n++) {
        /* rank UNDEF first, then the job-level table, as the local
         * resolve does - an older host records local peers there */
        rc = fetch_value(nspace, PMIX_RANK_UNDEF, PMIX_LOCAL_PEERS, nodes[n], &val);
        if (PMIX_SUCCESS != rc) {
            rc = fetch_value(nspace, PMIX_RANK_WILDCARD, PMIX_LOCAL_PEERS, nodes[n], &val);
        }
        if (PMIX_SUCCESS != rc) {
            /* nothing held for this node. If it hosts any rank, the
             * completeness check below catches it */
            continue;
        }
        if (PMIX_STRING != val->type) {
            PMIX_VALUE_RELEASE(val);
            PMIx_Argv_free(nodes);
            return PMIX_ERR_INVALID_VAL;
        }
        /* a NULL or empty list is a node in the map with no ranks of
         * this job on it - PMIx_Argv_split returns NULL for both */
        peers = (NULL == val->data.string) ? NULL : PMIx_Argv_split(val->data.string, ',');
        PMIX_VALUE_RELEASE(val);
        if (NULL == peers) {
            continue;
        }
        for (m = 0; NULL != peers[m]; m++) {
            errno = 0;
            r = strtoul(peers[m], &end, 10);
            if (0 != errno || end == peers[m] || '\0' != *end ||
                r >= nprocs || UINT32_MAX != nodemap[r]) {
                PMIx_Argv_free(peers);
                PMIx_Argv_free(nodes);
                return PMIX_ERR_INVALID_VAL;
            }
            nodemap[r] = n;
            ++nplaced;
        }
        PMIx_Argv_free(peers);
    }
    PMIx_Argv_free(nodes);

    /* each placement was checked against a distinct, in-range rank, so
     * this count is the whole of the completeness check */
    if (nplaced != nprocs) {
        return PMIX_ERR_NOT_FOUND;
    }
    *nnodes = n;
    return PMIX_SUCCESS;
}

/* does cycling through blocks[0..nb-1] reproduce nodemap exactly? */
static bool blocks_reproduce(const anl_block_t *blocks, size_t nb, const uint32_t *nodemap,
                             uint32_t nprocs)
{
    uint32_t r = 0, i, j;
    size_t b;

    /* every block places at least one rank, so each pass advances */
    while (true) {
        for (b = 0; b < nb; b++) {
            for (i = 0; i < blocks[b].count; i++) {
                for (j = 0; j < blocks[b].ppn; j++) {
                    if (nodemap[r] != blocks[b].start + i) {
                        return false;
                    }
                    if (++r == nprocs) {
                        return true;
                    }
                }
            }
        }
    }
}

static bool same_block(const anl_block_t *a, const anl_block_t *b)
{
    return (a->start == b->start && a->count == b->count && a->ppn == b->ppn);
}

/* the shortest prefix of blocks[] that, cycled, reproduces nodemap.
 * The candidate is the smallest period of every block but the last - the
 * last may be cut short where the ranks ran out - and it is verified
 * rather than trusted. The whole list always reproduces the map, so it is
 * the answer when the candidate does not */
static size_t shortest_cycle(const anl_block_t *blocks, size_t nblocks, const uint32_t *nodemap,
                             uint32_t nprocs)
{
    size_t *fail, len, i, k, period;

    if (2 > nblocks) {
        return nblocks;
    }
    len = nblocks - 1;
    fail = (size_t *) malloc(len * sizeof(size_t));
    if (NULL == fail) {
        return nblocks;
    }
    /* KMP failure function: fail[i] is the length of the longest proper
     * prefix of blocks[0..i] that is also a suffix of it */
    fail[0] = 0;
    k = 0;
    for (i = 1; i < len; i++) {
        while (0 < k && !same_block(&blocks[i], &blocks[k])) {
            k = fail[k - 1];
        }
        if (same_block(&blocks[i], &blocks[k])) {
            ++k;
        }
        fail[i] = k;
    }
    period = len - fail[len - 1];
    free(fail);

    if (period < nblocks && blocks_reproduce(blocks, period, nodemap, nprocs)) {
        return period;
    }
    return nblocks;
}

/* encode nodemap - already renumbered in order of first appearance -
 * into ANL notation */
static pmix_status_t encode(const uint32_t *nodemap, uint32_t nprocs, char **out)
{
    anl_block_t *blocks;
    size_t nblocks = 0, nb, b;
    uint32_t r, runlen;
    char str[PMIX_ANL_MAP_MAX_LEN];
    int pos, len;

    /* one block per run of ranks on a node is the worst case */
    blocks = (anl_block_t *) malloc(nprocs * sizeof(anl_block_t));
    if (NULL == blocks) {
        return PMIX_ERR_NOMEM;
    }

    /* split the ranks into maximal runs on one node, and fold each run
     * into the block before it when it is the next node with the same
     * number of ranks */
    r = 0;
    while (r < nprocs) {
        runlen = 1;
        while (r + runlen < nprocs && nodemap[r + runlen] == nodemap[r]) {
            ++runlen;
        }
        if (0 < nblocks && blocks[nblocks - 1].ppn == runlen &&
            blocks[nblocks - 1].start + blocks[nblocks - 1].count == nodemap[r]) {
            blocks[nblocks - 1].count++;
        } else {
            blocks[nblocks].start = nodemap[r];
            blocks[nblocks].count = 1;
            blocks[nblocks].ppn = runlen;
            ++nblocks;
        }
        r += runlen;
    }

    nb = shortest_cycle(blocks, nblocks, nodemap, nprocs);

    pos = snprintf(str, sizeof(str), "(vector");
    for (b = 0; b < nb; b++) {
        len = snprintf(str + pos, sizeof(str) - pos, ",(%u,%u,%u)", blocks[b].start,
                       blocks[b].count, blocks[b].ppn);
        if (0 > len || (size_t) len >= sizeof(str) - pos) {
            free(blocks);
            return PMIX_ERR_NOT_SUPPORTED;
        }
        pos += len;
    }
    free(blocks);
    len = snprintf(str + pos, sizeof(str) - pos, ")");
    if (0 > len || (size_t) len >= sizeof(str) - pos) {
        return PMIX_ERR_NOT_SUPPORTED;
    }

    *out = strdup(str);
    if (NULL == *out) {
        return PMIX_ERR_NOMEM;
    }
    return PMIX_SUCCESS;
}

pmix_status_t pmix_server_compute_anl_map(pmix_namespace_t *nptr)
{
    pmix_value_t *val = NULL;
    uint32_t nprocs, nnodes = 0, r, next = 0;
    uint32_t *nodemap = NULL, *ids = NULL;
    char *map = NULL;
    pmix_kval_t *kv;
    pmix_proc_t proc;
    pmix_status_t rc;

    rc = fetch_value(nptr->nspace, PMIX_RANK_WILDCARD, PMIX_JOB_SIZE, NULL, &val);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }
    if (PMIX_UINT32 != val->type) {
        PMIX_VALUE_RELEASE(val);
        return PMIX_ERR_INVALID_VAL;
    }
    nprocs = val->data.uint32;
    PMIX_VALUE_RELEASE(val);
    /* a reader holds node ids and counts in an int */
    if (0 == nprocs || INT32_MAX < nprocs) {
        return PMIX_ERR_NOT_SUPPORTED;
    }

    nodemap = (uint32_t *) malloc(nprocs * sizeof(uint32_t));
    if (NULL == nodemap) {
        return PMIX_ERR_NOMEM;
    }
    rc = build_nodemap(nptr->nspace, nprocs, nodemap, &nnodes);
    if (PMIX_SUCCESS != rc) {
        goto done;
    }

    /* renumber the nodes in order of first appearance */
    ids = (uint32_t *) malloc(nnodes * sizeof(uint32_t));
    if (NULL == ids) {
        rc = PMIX_ERR_NOMEM;
        goto done;
    }
    for (r = 0; r < nnodes; r++) {
        ids[r] = UINT32_MAX;
    }
    for (r = 0; r < nprocs; r++) {
        if (UINT32_MAX == ids[nodemap[r]]) {
            ids[nodemap[r]] = next++;
        }
        nodemap[r] = ids[nodemap[r]];
    }

    rc = encode(nodemap, nprocs, &map);
    if (PMIX_SUCCESS != rc) {
        goto done;
    }

    /* keep it as job-level data, so the next local proc to ask is
     * answered from the store like any key the host supplied */
    kv = PMIX_NEW(pmix_kval_t);
    if (NULL == kv) {
        free(map);
        rc = PMIX_ERR_NOMEM;
        goto done;
    }
    kv->key = strdup(PMIX_ANL_MAP);
    PMIX_VALUE_CREATE(kv->value, 1);
    if (NULL == kv->key || NULL == kv->value) {
        free(map);
        PMIX_RELEASE(kv);
        rc = PMIX_ERR_NOMEM;
        goto done;
    }
    kv->value->type = PMIX_STRING;
    kv->value->data.string = map;
    PMIX_LOAD_PROCID(&proc, nptr->nspace, PMIX_RANK_WILDCARD);
    PMIX_GDS_STORE_KV(rc, pmix_globals.mypeer, &proc, PMIX_INTERNAL, kv);
    PMIX_RELEASE(kv);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto done;
    }
    pmix_output_verbose(2, pmix_server_globals.get_output,
                        "%s COMPUTED %s FOR %s",
                        PMIX_NAME_PRINT(&pmix_globals.myid), PMIX_ANL_MAP, nptr->nspace);

done:
    free(ids);
    free(nodemap);
    return rc;
}
