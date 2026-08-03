/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Push one value of every interesting PMIx data type through a real
 * server, and check that a rank behind a *different* server gets the
 * same value back.
 *
 * test/unit/bfrops_darray.c and test/unit/bfrops_malformed.c pack and
 * unpack in a single process, which covers the encoders but not the
 * thing they exist for. Two facts only become true once there is a
 * socket in the middle:
 *
 *   - the module doing the encoding is the *peer's*, picked by
 *     pmix_bfrops_base_assign_module() from the version string the ptl
 *     handshake exchanged, rather than this process's own newest one;
 *   - the buffer type (described vs. non-described) is whatever the two
 *     ends negotiated, so the pack and unpack drivers take their tagged
 *     branch or their untagged one depending on how the peer was built.
 *
 * A single-process round trip is always self-consistent under both, and
 * so cannot fail on either. Spreading the ranks over separate nodes -
 * hence separate PMIx servers - is what makes a value asked for by rank
 * N actually travel: it is not in the local datastore, so the get takes
 * the full pack/send/unpack/store path through two daemons.
 *
 * Every rank publishes the same values under its own keys, then verifies
 * every *other* rank's copy. Exits non-zero on the first mismatch and
 * prints "datatypes: PASS" only when every type checked out.
 */

#define _GNU_SOURCE
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "examples.h"
#include <pmix.h>

static pmix_proc_t myproc;
static int nerrs = 0;

#define CHECK(cond, what)                                                     \
    do {                                                                      \
        if (!(cond)) {                                                        \
            fprintf(stderr, "datatypes: rank %u: %s: MISMATCH\n",              \
                    (unsigned) myproc.rank, (what));                          \
            nerrs++;                                                          \
        }                                                                     \
    } while (0)

/* the scalar payloads, identical on every rank so any rank can check any
 * other rank's copy without knowing who wrote it */
#define V_INT8   ((int8_t) -0x7F)
#define V_UINT8  ((uint8_t) 0xFE)
#define V_INT16  ((int16_t) -0x7FFE)
#define V_UINT16 ((uint16_t) 0xFFFE)
#define V_INT32  ((int32_t) -2000000000)
#define V_UINT32 ((uint32_t) 4000000000U)
#define V_INT64  ((int64_t) -9000000000000000000LL)
#define V_UINT64 ((uint64_t) 18000000000000000000ULL)
#define V_SIZE   ((size_t) 0x0123456789ABULL)
#define V_STRING "the quick brown fox jumps over the lazy dog"

/* Load the value for slot n. Keeping the writer and the checker driven
 * off one table is the point: a type that is written one way and checked
 * another proves nothing. */
typedef struct {
    const char *key;
    pmix_data_type_t type;
} dtype_t;

static dtype_t dtypes[] = {
    {"dt-bool", PMIX_BOOL},
    {"dt-byte", PMIX_BYTE},
    {"dt-string", PMIX_STRING},
    {"dt-size", PMIX_SIZE},
    {"dt-int", PMIX_INT},
    {"dt-int8", PMIX_INT8},
    {"dt-int16", PMIX_INT16},
    {"dt-int32", PMIX_INT32},
    {"dt-int64", PMIX_INT64},
    {"dt-uint", PMIX_UINT},
    {"dt-uint8", PMIX_UINT8},
    {"dt-uint16", PMIX_UINT16},
    {"dt-uint32", PMIX_UINT32},
    {"dt-uint64", PMIX_UINT64},
    {"dt-float", PMIX_FLOAT},
    {"dt-double", PMIX_DOUBLE},
    {"dt-status", PMIX_STATUS},
    {"dt-rank", PMIX_PROC_RANK},
    {"dt-bo", PMIX_BYTE_OBJECT},
    {"dt-proc", PMIX_PROC},
    {"dt-envar", PMIX_ENVAR},
    {"dt-darray-int32", PMIX_DATA_ARRAY},
    {"dt-darray-string", PMIX_DATA_ARRAY},
    {"dt-darray-info", PMIX_DATA_ARRAY},
    {"dt-darray-nested", PMIX_DATA_ARRAY},
    {"dt-darray-empty", PMIX_DATA_ARRAY},
    /* deliberately AFTER the empty array: if the packer and the unpacker
     * disagree about how a typeless array is spelled, the leftover bytes
     * land in front of whatever is read next, and this is what notices */
    {"dt-after-empty", PMIX_STRING}
};
#define NDTYPES (sizeof(dtypes) / sizeof(dtypes[0]))

static void fill(pmix_value_t *v, size_t n)
{
    PMIX_VALUE_CONSTRUCT(v);
    v->type = dtypes[n].type;

    switch (n) {
    case 0: v->data.flag = true; break;
    case 1: v->data.byte = 0xA5; break;
    case 2: v->data.string = strdup(V_STRING); break;
    case 3: v->data.size = V_SIZE; break;
    case 4: v->data.integer = -123456; break;
    case 5: v->data.int8 = V_INT8; break;
    case 6: v->data.int16 = V_INT16; break;
    case 7: v->data.int32 = V_INT32; break;
    case 8: v->data.int64 = V_INT64; break;
    case 9: v->data.uint = 654321; break;
    case 10: v->data.uint8 = V_UINT8; break;
    case 11: v->data.uint16 = V_UINT16; break;
    case 12: v->data.uint32 = V_UINT32; break;
    case 13: v->data.uint64 = V_UINT64; break;
    case 14: v->data.fval = 3.5f; break;
    case 15: v->data.dval = -2.25; break;
    case 16: v->data.status = PMIX_ERR_NOT_FOUND; break;
    case 17: v->data.rank = 7; break;
    case 18:
        v->data.bo.size = 5;
        v->data.bo.bytes = (char *) malloc(5);
        memcpy(v->data.bo.bytes, "\x00\x01\xfe\xff\x7f", 5);
        break;
    case 19:
        PMIX_PROC_CREATE(v->data.proc, 1);
        PMIX_LOAD_PROCID(v->data.proc, "some-nspace", 3);
        break;
    case 20:
        v->data.envar.envar = strdup("PMIX_TEST_VAR");
        v->data.envar.value = strdup("some-value");
        v->data.envar.separator = ':';
        break;
    case 21: {
        int32_t *p;
        size_t k;
        v->data.darray = PMIx_Data_array_create(8, PMIX_INT32);
        p = (int32_t *) v->data.darray->array;
        for (k = 0; k < 8; k++) {
            p[k] = (int32_t) (k * 100000) - 400000;
        }
        break;
    }
    case 22: {
        char **p;
        v->data.darray = PMIx_Data_array_create(3, PMIX_STRING);
        p = (char **) v->data.darray->array;
        p[0] = strdup("alpha");
        p[1] = strdup("beta");
        p[2] = strdup("gamma");
        break;
    }
    case 23: {
        pmix_info_t *p;
        v->data.darray = PMIx_Data_array_create(2, PMIX_INFO);
        p = (pmix_info_t *) v->data.darray->array;
        PMIX_INFO_LOAD(&p[0], "inner-one", "value-one", PMIX_STRING);
        PMIX_INFO_LOAD(&p[1], "inner-two", &(uint32_t){42}, PMIX_UINT32);
        break;
    }
    case 24: {
        /* an array of arrays: the recursion in pack/unpack/copy */
        pmix_data_array_t *outer, *inner;
        size_t k;
        outer = PMIx_Data_array_create(2, PMIX_DATA_ARRAY);
        inner = (pmix_data_array_t *) outer->array;
        for (k = 0; k < 2; k++) {
            uint64_t *q;
            size_t j;
            PMIX_DATA_ARRAY_CONSTRUCT(&inner[k], 4, PMIX_UINT64);
            q = (uint64_t *) inner[k].array;
            for (j = 0; j < 4; j++) {
                q[j] = (uint64_t) 1 << (8 * (k + 1) + j);
            }
        }
        v->data.darray = outer;
        break;
    }
    case 25:
        /* an array descriptor with no element type: the marker the packer
         * and the unpacker must spell identically, or everything after it
         * in the message is misread */
        v->data.darray = (pmix_data_array_t *) calloc(1, sizeof(pmix_data_array_t));
        break;
    case 26:
        v->data.string = strdup("still-in-step");
        break;
    default:
        break;
    }
}

static void check(pmix_value_t *v, size_t n, unsigned peer)
{
    char what[256];

    snprintf(what, sizeof(what), "%s from rank %u", dtypes[n].key, peer);

    if (v->type != dtypes[n].type) {
        fprintf(stderr, "datatypes: rank %u: %s: type %d, expected %d\n",
                (unsigned) myproc.rank, what, (int) v->type, (int) dtypes[n].type);
        nerrs++;
        return;
    }

    switch (n) {
    case 0: CHECK(true == v->data.flag, what); break;
    case 1: CHECK(0xA5 == (unsigned char) v->data.byte, what); break;
    case 2: CHECK(NULL != v->data.string
                  && 0 == strcmp(v->data.string, V_STRING), what); break;
    case 3: CHECK(V_SIZE == v->data.size, what); break;
    case 4: CHECK(-123456 == v->data.integer, what); break;
    case 5: CHECK(V_INT8 == v->data.int8, what); break;
    case 6: CHECK(V_INT16 == v->data.int16, what); break;
    case 7: CHECK(V_INT32 == v->data.int32, what); break;
    case 8: CHECK(V_INT64 == v->data.int64, what); break;
    case 9: CHECK(654321 == v->data.uint, what); break;
    case 10: CHECK(V_UINT8 == v->data.uint8, what); break;
    case 11: CHECK(V_UINT16 == v->data.uint16, what); break;
    case 12: CHECK(V_UINT32 == v->data.uint32, what); break;
    case 13: CHECK(V_UINT64 == v->data.uint64, what); break;
    case 14: CHECK(3.5f == v->data.fval, what); break;
    case 15: CHECK(-2.25 == v->data.dval, what); break;
    case 16: CHECK(PMIX_ERR_NOT_FOUND == v->data.status, what); break;
    case 17: CHECK(7 == v->data.rank, what); break;
    case 18: CHECK(5 == v->data.bo.size && NULL != v->data.bo.bytes
                   && 0 == memcmp(v->data.bo.bytes, "\x00\x01\xfe\xff\x7f", 5),
                   what); break;
    case 19: CHECK(NULL != v->data.proc
                   && 0 == strcmp(v->data.proc->nspace, "some-nspace")
                   && 3 == v->data.proc->rank, what); break;
    case 20: CHECK(NULL != v->data.envar.envar
                   && 0 == strcmp(v->data.envar.envar, "PMIX_TEST_VAR")
                   && NULL != v->data.envar.value
                   && 0 == strcmp(v->data.envar.value, "some-value")
                   && ':' == v->data.envar.separator, what); break;
    case 21: {
        int ok = (NULL != v->data.darray) && (PMIX_INT32 == v->data.darray->type)
                 && (8 == v->data.darray->size) && (NULL != v->data.darray->array);
        if (ok) {
            int32_t *p = (int32_t *) v->data.darray->array;
            size_t k;
            for (k = 0; k < 8; k++) {
                if (p[k] != (int32_t) (k * 100000) - 400000) {
                    ok = 0;
                    break;
                }
            }
        }
        CHECK(ok, what);
        break;
    }
    case 22: {
        int ok = (NULL != v->data.darray) && (PMIX_STRING == v->data.darray->type)
                 && (3 == v->data.darray->size) && (NULL != v->data.darray->array);
        if (ok) {
            char **p = (char **) v->data.darray->array;
            ok = (NULL != p[0] && 0 == strcmp(p[0], "alpha"))
                 && (NULL != p[1] && 0 == strcmp(p[1], "beta"))
                 && (NULL != p[2] && 0 == strcmp(p[2], "gamma"));
        }
        CHECK(ok, what);
        break;
    }
    case 23: {
        int ok = (NULL != v->data.darray) && (PMIX_INFO == v->data.darray->type)
                 && (2 == v->data.darray->size) && (NULL != v->data.darray->array);
        if (ok) {
            pmix_info_t *p = (pmix_info_t *) v->data.darray->array;
            ok = (0 == strcmp(p[0].key, "inner-one"))
                 && (PMIX_STRING == p[0].value.type)
                 && (NULL != p[0].value.data.string)
                 && (0 == strcmp(p[0].value.data.string, "value-one"))
                 && (0 == strcmp(p[1].key, "inner-two"))
                 && (PMIX_UINT32 == p[1].value.type)
                 && (42 == p[1].value.data.uint32);
        }
        CHECK(ok, what);
        break;
    }
    case 24: {
        int ok = (NULL != v->data.darray) && (PMIX_DATA_ARRAY == v->data.darray->type)
                 && (2 == v->data.darray->size) && (NULL != v->data.darray->array);
        if (ok) {
            pmix_data_array_t *inner = (pmix_data_array_t *) v->data.darray->array;
            size_t k, j;
            for (k = 0; k < 2 && ok; k++) {
                uint64_t *q = (uint64_t *) inner[k].array;
                if (PMIX_UINT64 != inner[k].type || 4 != inner[k].size || NULL == q) {
                    ok = 0;
                    break;
                }
                for (j = 0; j < 4; j++) {
                    if (q[j] != ((uint64_t) 1 << (8 * (k + 1) + j))) {
                        ok = 0;
                        break;
                    }
                }
            }
        }
        CHECK(ok, what);
        break;
    }
    case 25:
        /* the only requirement is that it comes back as an empty array
         * rather than as garbage - and, far more importantly, that
         * everything published after it still checks out, which the
         * cases above and below this one are what verify */
        CHECK(NULL == v->data.darray || 0 == v->data.darray->size, what);
        break;
    case 26:
        CHECK(NULL != v->data.string
              && 0 == strcmp(v->data.string, "still-in-step"), what);
        break;
    default:
        break;
    }
}

int main(int argc, char **argv)
{
    pmix_status_t rc;
    pmix_value_t value;
    pmix_value_t *val;
    pmix_proc_t proc;
    uint32_t nprocs = 0, n;
    size_t k;
    char key[PMIX_MAX_KEYLEN + 1];
    pmix_info_t info;

    EXAMPLES_HIDE_UNUSED_PARAMS(argc, argv);

    if (PMIX_SUCCESS != (rc = PMIx_Init(&myproc, NULL, 0))) {
        fprintf(stderr, "datatypes: PMIx_Init failed: %s\n", PMIx_Error_string(rc));
        exit(1);
    }

    PMIX_LOAD_PROCID(&proc, myproc.nspace, PMIX_RANK_WILDCARD);
    if (PMIX_SUCCESS != (rc = PMIx_Get(&proc, PMIX_JOB_SIZE, NULL, 0, &val))) {
        fprintf(stderr, "datatypes: could not get job size: %s\n", PMIx_Error_string(rc));
        goto done;
    }
    nprocs = val->data.uint32;
    PMIX_VALUE_RELEASE(val);

    /* publish one value of every type under a key that names the type
     * and the rank that wrote it */
    for (k = 0; k < NDTYPES; k++) {
        snprintf(key, sizeof(key), "%s-%u", dtypes[k].key, (unsigned) myproc.rank);
        fill(&value, k);
        rc = PMIx_Put(PMIX_GLOBAL, key, &value);
        PMIX_VALUE_DESTRUCT(&value);
        if (PMIX_SUCCESS != rc) {
            fprintf(stderr, "datatypes: rank %u: PMIx_Put %s failed: %s\n",
                    (unsigned) myproc.rank, key, PMIx_Error_string(rc));
            nerrs++;
            goto done;
        }
    }

    if (PMIX_SUCCESS != (rc = PMIx_Commit())) {
        fprintf(stderr, "datatypes: PMIx_Commit failed: %s\n", PMIx_Error_string(rc));
        nerrs++;
        goto done;
    }

    /* collect the data so a rank behind another server can be asked for
     * it; without this the gets below would go direct-modex, which is
     * also worth exercising but is a different path */
    PMIX_INFO_LOAD(&info, PMIX_COLLECT_DATA, &(bool){true}, PMIX_BOOL);
    PMIX_LOAD_PROCID(&proc, myproc.nspace, PMIX_RANK_WILDCARD);
    if (PMIX_SUCCESS != (rc = PMIx_Fence(&proc, 1, &info, 1))) {
        fprintf(stderr, "datatypes: PMIx_Fence failed: %s\n", PMIx_Error_string(rc));
        PMIX_INFO_DESTRUCT(&info);
        nerrs++;
        goto done;
    }
    PMIX_INFO_DESTRUCT(&info);

    /* now check every other rank's copy of every type */
    for (n = 0; n < nprocs; n++) {
        if (n == myproc.rank) {
            continue;
        }
        PMIX_LOAD_PROCID(&proc, myproc.nspace, n);
        for (k = 0; k < NDTYPES; k++) {
            snprintf(key, sizeof(key), "%s-%u", dtypes[k].key, (unsigned) n);
            rc = PMIx_Get(&proc, key, NULL, 0, &val);
            if (PMIX_SUCCESS != rc) {
                fprintf(stderr, "datatypes: rank %u: PMIx_Get %s failed: %s\n",
                        (unsigned) myproc.rank, key, PMIx_Error_string(rc));
                nerrs++;
                continue;
            }
            if (NULL == val) {
                fprintf(stderr, "datatypes: rank %u: PMIx_Get %s returned NULL\n",
                        (unsigned) myproc.rank, key);
                nerrs++;
                continue;
            }
            check(val, k, (unsigned) n);
            PMIX_VALUE_RELEASE(val);
        }
    }

done:
    if (0 == nerrs) {
        fprintf(stderr, "datatypes: rank %u: PASS (%u types x %u peers)\n",
                (unsigned) myproc.rank, (unsigned) NDTYPES,
                (unsigned) (nprocs > 0 ? nprocs - 1 : 0));
    } else {
        fprintf(stderr, "datatypes: rank %u: FAIL (%d mismatches)\n",
                (unsigned) myproc.rank, nerrs);
    }
    fflush(stderr);

    PMIx_Finalize(NULL, 0);
    return (0 == nerrs) ? 0 : 1;
}
