/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * Unit tests for the pnet simptest component's map handling.
 *
 * simptest is the tree's working example of a fabric component that
 * assigns endpoints from a topology description, and everything it does
 * hangs off two arrays it parses out of the host's directives: the
 * PMIX_NODE_MAP and the PMIX_PROC_MAP.  It walks them in lockstep -
 * procs[n] is read for every nodes[n] - which makes their relationship
 * the thing worth pinning down, because nothing in the parser enforces
 * it: the two are separate strings, parsed by separate calls, and
 * PMIx_Argv_split collapses empty entries, so a proc map with a node
 * that hosts nothing comes back shorter than the node map it belongs
 * to.  Reading past the end of the shorter one is a wild pointer handed
 * straight to a string splitter.
 *
 * Driven through PMIx_server_setup_application, which is what actually
 * reaches the module.  The call is non-blocking, so each case waits on
 * its own lock rather than sleeping.
 *
 * What each case pins down:
 *
 *   mismatched maps - three nodes and two proc entries used to walk off
 *     the end of the proc array.  gds/hash's store_map has always
 *     refused this pair; simptest now refuses it the same way.
 *
 *   map encodings - a host may send either map as a plain PMIX_STRING,
 *     as an encoded PMIX_REGEX, or as a PMIX_REGEX2.  The component read
 *     value.data.string whatever it was told: that aliases correctly for
 *     the first two and hands the parser a pmix_regex2_t* cast to char*
 *     for the third.
 *
 *   a node the topology file does not describe - the far more likely
 *     spelling mistake than the one the other direction.  It used to be
 *     PMIX_ERR_NOT_FOUND, logged as "should be impossible", which aborts
 *     the framework's allocate fan-out for every other component too.
 *     It is a configuration error, so it declines and says so.
 *
 *   a topology file whose last line has no trailing newline - the line
 *     reader removed the last character unconditionally, so that node's
 *     final coordinate (or its name, on a single-token line) was
 *     silently truncated and it stopped matching the node map.
 *
 * The component is opt-in at configure time (--with-simptest /
 * --enable-test-build), so the test asks the MCA whether it was built
 * and exits 77 - automake's "skip" - when it was not.
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "include/pmix_server.h"

#include "src/class/pmix_list.h"
#include "src/include/pmix_globals.h"
#include "src/mca/base/pmix_mca_base_var.h"
#include "src/mca/bfrops/bfrops.h"
#include "src/threads/pmix_threads.h"
#include "src/util/pmix_argv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SIMPTEST_BLOB "pmix-pnet-simptest-blob"
#define NODE_A        "unit-node-a"
#define NODE_B        "unit-node-b"
#define NODE_C        "unit-node-c"
#define NODE_D        "unit-node-d"

static int npass = 0;
static int nfail = 0;
static char topofile[512];

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

typedef struct {
    pmix_lock_t lock;
    pmix_status_t status;
    bool gotblob;
    /* dimensions of the coordinate the blob carries for NODE_B, whose
     * topology line is the file's last and carries no newline */
    int bdims;
} sim_result_t;

/* pull NODE_B's coordinate out of the blob. The topology file gives it
 * two dimensions, so anything else means the line was not read whole */
static void examine(sim_result_t *res, pmix_byte_object_t *bo)
{
    pmix_buffer_t bkt;
    pmix_kval_t *kv;
    pmix_status_t rc;
    pmix_info_t *iptr;
    pmix_data_array_t *d;
    pmix_coord_t *cptr;
    int32_t cnt;
    size_t j;
    bool isb;

    PMIX_LOAD_BUFFER_NON_DESTRUCT(pmix_globals.mypeer, &bkt, bo->bytes, bo->size);
    kv = PMIX_NEW(pmix_kval_t);
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, pmix_globals.mypeer, &bkt, kv, &cnt, PMIX_KVAL);
    while (PMIX_SUCCESS == rc) {
        if (PMIX_CHECK_KEY(kv, PMIX_NODE_INFO_ARRAY) && PMIX_DATA_ARRAY == kv->value->type
            && NULL != kv->value->data.darray) {
            d = kv->value->data.darray;
            iptr = (pmix_info_t *) d->array;
            isb = false;
            for (j = 0; j < d->size; j++) {
                if (PMIX_CHECK_KEY(&iptr[j], PMIX_HOSTNAME) && PMIX_STRING == iptr[j].value.type
                    && NULL != iptr[j].value.data.string
                    && 0 == strcmp(iptr[j].value.data.string, NODE_B)) {
                    isb = true;
                }
            }
            if (isb) {
                for (j = 0; j < d->size; j++) {
                    if (PMIX_CHECK_KEY(&iptr[j], PMIX_FABRIC_COORDINATES)
                        && PMIX_DATA_ARRAY == iptr[j].value.type
                        && NULL != iptr[j].value.data.darray
                        && 0 < iptr[j].value.data.darray->size) {
                        cptr = (pmix_coord_t *) iptr[j].value.data.darray->array;
                        res->bdims = (int) cptr[0].dims;
                    }
                }
            }
        }
        PMIX_RELEASE(kv);
        kv = PMIX_NEW(pmix_kval_t);
        cnt = 1;
        PMIX_BFROPS_UNPACK(rc, pmix_globals.mypeer, &bkt, kv, &cnt, PMIX_KVAL);
    }
    PMIX_RELEASE(kv);
}

static void setupcb(pmix_status_t status, pmix_info_t info[], size_t ninfo, void *provided_cbdata,
                    pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    sim_result_t *res = (sim_result_t *) provided_cbdata;
    size_t n;

    res->status = status;
    for (n = 0; n < ninfo; n++) {
        if (0 == strncmp(info[n].key, SIMPTEST_BLOB, PMIX_MAX_KEYLEN)
            && PMIX_BYTE_OBJECT == info[n].value.type) {
            res->gotblob = true;
            examine(res, &info[n].value.data.bo);
        }
    }
    if (NULL != cbfunc) {
        cbfunc(PMIX_SUCCESS, cbdata);
    }
    PMIX_WAKEUP_THREAD(&res->lock);
}

/* the three shapes a host may use for either map */
typedef enum { MAP_STRING, MAP_REGEX, MAP_REGEX2 } mapform_t;

/* nodemap/procmap are the plain comma- and semicolon-delimited forms;
 * form says how to hand them over - as the plain strings a simple host
 * sends, as the deprecated encoded PMIX_REGEX, or as the PMIX_REGEX2
 * that PMIx_generate_regex2 produces and that new hosts should use */
static void ask(const char *nspace, const char *nodemap, const char *procmap, mapform_t form,
                sim_result_t *res)
{
    pmix_info_t directives[2];
    pmix_nspace_t ns;
    pmix_status_t rc;
    char *nreg = NULL, *preg = NULL;
    pmix_regex2_t nr2, pr2;

    memset(res, 0, sizeof(*res));
    PMIX_CONSTRUCT_LOCK(&res->lock);
    res->status = PMIX_ERR_NOT_SUPPORTED;

    if (MAP_REGEX == form) {
        /* the deprecated encoded form, which arrives as PMIX_REGEX */
        if (PMIX_SUCCESS != PMIx_generate_regex(nodemap, &nreg)
            || PMIX_SUCCESS != PMIx_generate_ppn(procmap, &preg)) {
            res->status = PMIX_ERROR;
            PMIX_DESTRUCT_LOCK(&res->lock);
            if (NULL != nreg) {
                free(nreg);
            }
            if (NULL != preg) {
                free(preg);
            }
            return;
        }
        PMIX_INFO_LOAD(&directives[0], PMIX_NODE_MAP, nreg, PMIX_REGEX);
        PMIX_INFO_LOAD(&directives[1], PMIX_PROC_MAP, preg, PMIX_REGEX);
        free(nreg);
        free(preg);
    } else if (MAP_REGEX2 == form) {
        /* the form PMIx_generate_regex2 produces - the replacement for
         * the deprecated one above, and the one whose payload does not
         * live where value.data.string points */
        PMIx_Regex2_construct(&nr2);
        PMIx_Regex2_construct(&pr2);
        if (PMIX_SUCCESS != PMIx_generate_regex2(nodemap, NULL, 0, &nr2)
            || PMIX_SUCCESS != PMIx_generate_regex2(procmap, NULL, 0, &pr2)) {
            res->status = PMIX_ERROR;
            PMIX_DESTRUCT_LOCK(&res->lock);
            PMIx_Regex2_destruct(&nr2);
            PMIx_Regex2_destruct(&pr2);
            return;
        }
        PMIX_INFO_LOAD(&directives[0], PMIX_NODE_MAP, &nr2, PMIX_REGEX2);
        PMIX_INFO_LOAD(&directives[1], PMIX_PROC_MAP, &pr2, PMIX_REGEX2);
        PMIx_Regex2_destruct(&nr2);
        PMIx_Regex2_destruct(&pr2);
    } else {
        PMIX_INFO_LOAD(&directives[0], PMIX_NODE_MAP, nodemap, PMIX_STRING);
        PMIX_INFO_LOAD(&directives[1], PMIX_PROC_MAP, procmap, PMIX_STRING);
    }

    PMIX_LOAD_NSPACE(ns, nspace);
    rc = PMIx_server_setup_application(ns, directives, 2, setupcb, res);
    if (PMIX_SUCCESS != rc) {
        res->status = rc;
    } else {
        PMIX_WAIT_THREAD(&res->lock);
    }
    PMIX_INFO_DESTRUCT(&directives[0]);
    PMIX_INFO_DESTRUCT(&directives[1]);
    PMIX_DESTRUCT_LOCK(&res->lock);
}

/* ------------------------------------------------------------------ */
static void test_plain_strings(void)
{
    sim_result_t res;

    ask("sim-plain", NODE_A "," NODE_B, "0,1;2,3", MAP_STRING, &res);
    report("a two-node job assigns endpoints from plain string maps",
           PMIX_SUCCESS == res.status && res.gotblob);
}

static void test_regex_maps(void)
{
    sim_result_t res;

    ask("sim-regex", NODE_A "," NODE_B, "0,1;2,3", MAP_REGEX, &res);
    report("the same maps work in the encoded form a launcher sends",
           PMIX_SUCCESS == res.status && res.gotblob);
}

static void test_regex2_maps(void)
{
    sim_result_t res;

    ask("sim-regex2", NODE_A "," NODE_B, "0,1;2,3", MAP_REGEX2, &res);
    report("the same maps work as the PMIX_REGEX2 that replaces it",
           PMIX_SUCCESS == res.status && res.gotblob);
}

static void test_mismatched_maps(void)
{
    sim_result_t res;

    /* four nodes, two proc entries. The third node reads the proc
     * array's NULL terminator and is skipped, but the fourth reads
     * past the end of the allocation entirely and hands whatever it
     * finds to a string splitter */
    ask("sim-mismatch", NODE_A "," NODE_B "," NODE_C "," NODE_D, "0,1;2,3", MAP_STRING, &res);
    report("a node map longer than its proc map is refused",
           PMIX_SUCCESS != res.status && !res.gotblob);
}

static void test_unknown_node(void)
{
    sim_result_t res;

    /* NODE_C is in the job but not in the topology file */
    ask("sim-unknown", NODE_A "," NODE_C, "0,1;2,3", MAP_STRING, &res);
    report("a node the topology file omits is declined, not failed",
           PMIX_SUCCESS == res.status && !res.gotblob);
}

/* the topology file's last line deliberately carries no trailing
 * newline - if the reader eats its last character, NODE_B stops
 * matching the node map and this job gets nothing */
static void test_unterminated_last_line(void)
{
    sim_result_t res;

    ask("sim-noeol", NODE_A "," NODE_B, "0,1;2,3", MAP_STRING, &res);
    report("a topology file with no trailing newline still resolves",
           PMIX_SUCCESS == res.status && res.gotblob);
    /* the file gives NODE_B two dimensions. Chopping the last character
     * off that line unconditionally leaves it with one */
    report("the last line's final coordinate survives", 2 == res.bdims);
}

static int write_topology(void)
{
    FILE *fp;

    (void) snprintf(topofile, sizeof(topofile), "%s/pmix-simptest-topo-%d.txt",
                    (NULL == getenv("TMPDIR")) ? "/tmp" : getenv("TMPDIR"), (int) getpid());
    fp = fopen(topofile, "w");
    if (NULL == fp) {
        return -1;
    }
    fprintf(fp, "# unit-test fabric topology\n");
    fprintf(fp, "%s 0 0\n", NODE_A);
    /* deliberately no trailing newline on the final line */
    fprintf(fp, "%s 0 1", NODE_B);
    fclose(fp);
    return 0;
}

int main(int argc, char **argv)
{
    static pmix_server_module_t mymodule = {0};
    pmix_info_t sinfo;
    pmix_status_t rc;
    bool flag = true;

    (void) argc;
    (void) argv;

    fprintf(stdout, "pnet_simptest_map: node/proc map unit tests\n");

    if (0 != write_topology()) {
        fprintf(stderr, "could not write the topology file\n");
        return 1;
    }

    setenv("PMIX_MCA_pnet", "simptest", 1);
    setenv("PMIX_MCA_pnet_simptest_config_file", topofile, 1);

    /* the endpoint assigner only runs for the scheduler role */
    PMIX_INFO_LOAD(&sinfo, PMIX_SERVER_SCHEDULER, &flag, PMIX_BOOL);
    rc = PMIx_server_init(&mymodule, &sinfo, 1);
    PMIX_INFO_DESTRUCT(&sinfo);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        unlink(topofile);
        return 1;
    }

    /* was the component built at all? Its parameters are registered for
     * any built component, so this answers "built", not "selected" */
    if (0 > pmix_mca_base_var_find("pmix", "pnet", "simptest", "config_file")) {
        fprintf(stdout, "pnet/simptest was not built (--with-simptest) - skipping\n");
        PMIx_server_finalize();
        unlink(topofile);
        return 77;
    }

    test_plain_strings();
    test_regex_maps();
    test_regex2_maps();
    test_mismatched_maps();
    test_unknown_node();
    test_unterminated_last_line();

    PMIx_server_finalize();
    unlink(topofile);

    fprintf(stdout, "pnet_simptest_map: %d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
