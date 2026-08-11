/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * White-box unit tests for src/server/pmix_server_setup.c - the calls a
 * host makes to prepare a job before its processes run, plus the small
 * helper APIs it uses to build that information.
 *
 * Everything here is driven through the *public* entry points in their
 * blocking form, so the ordering is deterministic: a blocking
 * PMIx_server_* call does not return until its handler has run on the
 * progress thread and woken us. No sleeps, no polling.
 *
 * What each case pins down:
 *
 *   argument screens - the helper APIs hand their arguments straight to
 *     a preg component or to hwloc, both of which take them at face value
 *     (preg/raw strncmp's the input and writes through the output pointer
 *     without looking; the hwloc string generators report a bad cpuset by
 *     writing NULL through the output pointer, so they cannot be the ones
 *     to screen it). A NULL argument used to take the library down.
 *
 *   job info - PMIX_GROUP_JOB_INFO carries a packed blob, which the
 *     handler walks until the buffer runs dry. Two things were wrong with
 *     that. The loop ends on PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER by
 *     design, and that status was handed back as the result, so every
 *     registration carrying job info told the host it had failed. And the
 *     blob was loaded with PMIX_LOAD_BUFFER, which does not copy - it
 *     points the buffer at the payload and NULLs the source - so the
 *     handler emptied the caller's own byte object and then leaked the
 *     blob, since the buffer is never destructed. The blob here is built
 *     by PMIx_server_collect_job_info, which is what produces this format
 *     in the first place.
 *
 *   deregistration - the man page says "each matching entry" is deleted.
 *     The cache legitimately holds more than one entry per key, because
 *     registration appends without checking, and the gds walk stores them
 *     in order so the *later* one is the one in effect. Stopping at the
 *     first match removed the shadowed entry and left the live one, so
 *     the deregistration silently did nothing.
 *
 *   mistyped group arrays - a key does not make the union an array. The
 *     PMIX_GROUP_INFO and PMIX_GROUP_ENDPT_DATA arms read
 *     value.data.darray (and, inside the endpt array, value.data.proc) on
 *     the strength of the key alone, so a host that got the type wrong
 *     had the server dereference whatever it had put there. Each is
 *     paired with a well-formed request so the screens are held to
 *     accepting what they should.
 *
 * Three groups of case here kill an unfixed library rather than failing
 * it - the argument screens and both mistyped arrays - which is the same
 * bargain test/unit/iof_output.c makes, and the point of it: every one of
 * them is reachable from an ordinary host, not from a hostile one. That
 * was verified rather than assumed, by reverting each screen in turn and
 * re-running: SIGSEGV (exit 139) each time, with no output at all, since
 * stdout is block-buffered when the harness captures it. So a regression
 * here looks like an empty log and a signal, not like a FAIL line.
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "include/pmix_server.h"

#include "src/class/pmix_list.h"
#include "src/include/pmix_globals.h"
#include "src/server/pmix_server_ops.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SUT_NSPACE "server-setup-ut"
#define SUT_NPROCS 2
#define SUT_KEY    "pmix.test.setup.key"

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

/* a blocking registration reports success as PMIX_OPERATION_SUCCEEDED */
static bool ok(pmix_status_t rc)
{
    return (PMIX_SUCCESS == rc || PMIX_OPERATION_SUCCEEDED == rc);
}

static size_t ncached(void)
{
    return pmix_list_get_size(&pmix_server_globals.gdata);
}

static pmix_status_t register_job(void)
{
    pmix_info_t info[2];
    pmix_nspace_t ns;
    pmix_status_t rc;
    uint32_t nprocs = SUT_NPROCS;

    PMIX_INFO_LOAD(&info[0], PMIX_JOB_SIZE, &nprocs, PMIX_UINT32);
    PMIX_INFO_LOAD(&info[1], PMIX_UNIV_SIZE, &nprocs, PMIX_UINT32);

    PMIX_LOAD_NSPACE(ns, SUT_NSPACE);
    rc = PMIx_server_register_nspace(ns, SUT_NPROCS, info, 2, NULL, NULL);

    PMIX_INFO_DESTRUCT(&info[0]);
    PMIX_INFO_DESTRUCT(&info[1]);
    return rc;
}

/* ------------------------------------------------------------------ */
/* the helper APIs screen what they are handed                         */
/* ------------------------------------------------------------------ */
static void test_arg_screens(void)
{
    pmix_cpuset_t cpuset;
    pmix_status_t rc;
    char *out = NULL;

    rc = PMIx_generate_regex(NULL, &out);
    report("generate_regex rejects a NULL input", PMIX_ERR_BAD_PARAM == rc);
    rc = PMIx_generate_regex("nodeA,nodeB", NULL);
    report("generate_regex rejects a NULL output", PMIX_ERR_BAD_PARAM == rc);

    rc = PMIx_generate_ppn(NULL, &out);
    report("generate_ppn rejects a NULL input", PMIX_ERR_BAD_PARAM == rc);
    rc = PMIx_generate_ppn("0,1;2,3", NULL);
    report("generate_ppn rejects a NULL output", PMIX_ERR_BAD_PARAM == rc);

    rc = PMIx_generate_regex2(NULL, NULL, 0, NULL);
    report("generate_regex2 rejects NULL arguments", PMIX_ERR_BAD_PARAM == rc);

    /* the cpuset is never looked at - the output pointer is written
     * through before anything else happens */
    PMIX_CPUSET_CONSTRUCT(&cpuset);
    rc = PMIx_server_generate_locality_string(&cpuset, NULL);
    report("generate_locality_string rejects a NULL output", PMIX_ERR_BAD_PARAM == rc);
    rc = PMIx_server_generate_cpuset_string(&cpuset, NULL);
    report("generate_cpuset_string rejects a NULL output", PMIX_ERR_BAD_PARAM == rc);

    /* and the two resource entry points walk the array ninfo times */
    rc = PMIx_server_register_resources(NULL, 2, NULL, NULL);
    report("register_resources rejects a NULL array", PMIX_ERR_BAD_PARAM == rc);
    rc = PMIx_server_deregister_resources(NULL, 2, NULL, NULL);
    report("deregister_resources rejects a NULL array", PMIX_ERR_BAD_PARAM == rc);

    /* a well-formed call still works */
    rc = PMIx_generate_regex("nodeA,nodeB", &out);
    report("generate_regex still answers a good request",
           PMIX_SUCCESS == rc && NULL != out);
    if (NULL != out) {
        free(out);
    }
}

/* ------------------------------------------------------------------ */
/* register/deregister of ordinary keys                                */
/* ------------------------------------------------------------------ */
static void test_cache(void)
{
    pmix_info_t info;
    pmix_status_t rc;
    size_t base;

    base = ncached();

    PMIX_INFO_LOAD(&info, SUT_KEY, "first", PMIX_STRING);
    rc = PMIx_server_register_resources(&info, 1, NULL, NULL);
    report("register_resources accepts an ordinary key", ok(rc));
    PMIX_INFO_DESTRUCT(&info);

    report("the key is cached", base + 1 == ncached());

    /* re-register the same key with a new value: nothing dedups, so the
     * cache now holds two entries and the later one is the one the gds
     * walk leaves in effect */
    PMIX_INFO_LOAD(&info, SUT_KEY, "second", PMIX_STRING);
    rc = PMIx_server_register_resources(&info, 1, NULL, NULL);
    report("re-registering the key is accepted", ok(rc));
    PMIX_INFO_DESTRUCT(&info);

    report("the cache holds both entries", base + 2 == ncached());

    /* one deregistration must clear every entry carrying that key */
    PMIX_INFO_LOAD(&info, SUT_KEY, NULL, PMIX_UNDEF);
    rc = PMIx_server_deregister_resources(&info, 1, NULL, NULL);
    report("deregister_resources accepts the key", ok(rc));
    PMIX_INFO_DESTRUCT(&info);

    report("deregistration cleared every matching entry", base == ncached());
}

/* ------------------------------------------------------------------ */
/* PMIX_GROUP_JOB_INFO                                                 */
/* ------------------------------------------------------------------ */
static void test_job_info(void)
{
    pmix_data_buffer_t dbuf;
    pmix_byte_object_t bo;
    pmix_proc_t proc;
    pmix_info_t info;
    pmix_status_t rc;
    size_t original;

    PMIX_DATA_BUFFER_CONSTRUCT(&dbuf);
    PMIX_LOAD_PROCID(&proc, SUT_NSPACE, PMIX_RANK_WILDCARD);
    rc = PMIx_server_collect_job_info(&proc, 1, &dbuf);
    if (PMIX_SUCCESS != rc || 0 == dbuf.bytes_used) {
        report("collect_job_info produced a blob", false);
        PMIX_DATA_BUFFER_DESTRUCT(&dbuf);
        return;
    }
    report("collect_job_info produced a blob", true);

    /* hand it over exactly as a host would - PMIX_INFO_LOAD copies, so
     * what we hold afterwards is our own byte object, and the library
     * has no business emptying it */
    PMIX_BYTE_OBJECT_CONSTRUCT(&bo);
    bo.bytes = dbuf.base_ptr;
    bo.size = dbuf.bytes_used;
    PMIX_INFO_LOAD(&info, PMIX_GROUP_JOB_INFO, &bo, PMIX_BYTE_OBJECT);
    original = info.value.data.bo.size;

    rc = PMIx_server_register_resources(&info, 1, NULL, NULL);
    report("registering job info reports success, not end-of-buffer", ok(rc));
    report("the caller's byte object is left intact",
           NULL != info.value.data.bo.bytes && original == info.value.data.bo.size);

    PMIX_INFO_DESTRUCT(&info);
    PMIX_DATA_BUFFER_DESTRUCT(&dbuf);
}

/* ------------------------------------------------------------------ */
/* group info and endpoint arrays, well-formed and mistyped            */
/* ------------------------------------------------------------------ */
static void test_group_arrays(void)
{
    pmix_data_array_t darray;
    pmix_info_t info[2], *iptr;
    pmix_proc_t proc;
    pmix_status_t rc;
    pmix_scope_t scope = PMIX_REMOTE;
    size_t ctxid = 42;
    uint32_t junk = 7;

    PMIX_LOAD_PROCID(&proc, SUT_NSPACE, 0);

    /* --- a well-formed group info array ---------------------------- */
    PMIX_DATA_ARRAY_CONSTRUCT(&darray, 2, PMIX_INFO);
    iptr = (pmix_info_t *) darray.array;
    PMIX_INFO_LOAD(&iptr[0], PMIX_PROCID, &proc, PMIX_PROC);
    PMIX_INFO_LOAD(&iptr[1], "pmix.test.setup.grpkey", "grpval", PMIX_STRING);
    PMIX_INFO_LOAD(&info[0], PMIX_GROUP_CONTEXT_ID, &ctxid, PMIX_SIZE);
    PMIX_INFO_LOAD(&info[1], PMIX_GROUP_INFO, &darray, PMIX_DATA_ARRAY);
    PMIX_DATA_ARRAY_DESTRUCT(&darray);

    rc = PMIx_server_register_resources(info, 2, NULL, NULL);
    report("a well-formed group info array is accepted", ok(rc));
    PMIX_INFO_DESTRUCT(&info[0]);
    PMIX_INFO_DESTRUCT(&info[1]);

    /* --- the same key carrying a scalar ----------------------------- */
    PMIX_INFO_LOAD(&info[0], PMIX_GROUP_CONTEXT_ID, &ctxid, PMIX_SIZE);
    PMIX_INFO_LOAD(&info[1], PMIX_GROUP_INFO, &junk, PMIX_UINT32);

    rc = PMIx_server_register_resources(info, 2, NULL, NULL);
    report("group info that is not an array is rejected, not dereferenced",
           !ok(rc));
    PMIX_INFO_DESTRUCT(&info[0]);
    PMIX_INFO_DESTRUCT(&info[1]);

    /* --- a well-formed endpoint array ------------------------------- */
    PMIX_DATA_ARRAY_CONSTRUCT(&darray, 3, PMIX_INFO);
    iptr = (pmix_info_t *) darray.array;
    PMIX_INFO_LOAD(&iptr[0], PMIX_PROCID, &proc, PMIX_PROC);
    PMIX_INFO_LOAD(&iptr[1], PMIX_DATA_SCOPE, &scope, PMIX_SCOPE);
    PMIX_INFO_LOAD(&iptr[2], "pmix.test.setup.endpt", "endptval", PMIX_STRING);
    PMIX_INFO_LOAD(&info[0], PMIX_GROUP_ENDPT_DATA, &darray, PMIX_DATA_ARRAY);
    PMIX_DATA_ARRAY_DESTRUCT(&darray);

    rc = PMIx_server_register_resources(info, 1, NULL, NULL);
    report("a well-formed endpoint array is accepted", ok(rc));
    PMIX_INFO_DESTRUCT(&info[0]);

    /* --- the same array with a scalar where the procID belongs ------ */
    PMIX_DATA_ARRAY_CONSTRUCT(&darray, 3, PMIX_INFO);
    iptr = (pmix_info_t *) darray.array;
    PMIX_INFO_LOAD(&iptr[0], PMIX_PROCID, &junk, PMIX_UINT32);
    PMIX_INFO_LOAD(&iptr[1], PMIX_DATA_SCOPE, &scope, PMIX_SCOPE);
    PMIX_INFO_LOAD(&iptr[2], "pmix.test.setup.endpt", "endptval", PMIX_STRING);
    PMIX_INFO_LOAD(&info[0], PMIX_GROUP_ENDPT_DATA, &darray, PMIX_DATA_ARRAY);
    PMIX_DATA_ARRAY_DESTRUCT(&darray);

    rc = PMIx_server_register_resources(info, 1, NULL, NULL);
    report("an endpoint array with a mistyped procID is rejected", !ok(rc));
    PMIX_INFO_DESTRUCT(&info[0]);

    /* --- group info without the context ID it requires -------------- */
    PMIX_DATA_ARRAY_CONSTRUCT(&darray, 2, PMIX_INFO);
    iptr = (pmix_info_t *) darray.array;
    PMIX_INFO_LOAD(&iptr[0], PMIX_PROCID, &proc, PMIX_PROC);
    PMIX_INFO_LOAD(&iptr[1], "pmix.test.setup.grpkey", "grpval", PMIX_STRING);
    PMIX_INFO_LOAD(&info[0], PMIX_GROUP_INFO, &darray, PMIX_DATA_ARRAY);
    PMIX_DATA_ARRAY_DESTRUCT(&darray);

    rc = PMIx_server_register_resources(info, 1, NULL, NULL);
    report("group info without a context ID is a bad param",
           PMIX_ERR_BAD_PARAM == rc);
    PMIX_INFO_DESTRUCT(&info[0]);
}

int main(int argc, char **argv)
{
    static pmix_server_module_t mymodule = {0};
    pmix_status_t rc;

    (void) argc;
    (void) argv;

    fprintf(stdout, "server_setup: job-preparation unit tests\n");

    rc = PMIx_server_init(&mymodule, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    rc = register_job();
    if (!ok(rc)) {
        fprintf(stderr, "register_nspace failed: %s\n", PMIx_Error_string(rc));
        PMIx_server_finalize();
        return 1;
    }

    test_arg_screens();
    test_cache();
    test_job_info();
    test_group_arrays();

    PMIx_server_finalize();

    fprintf(stdout, "server_setup: %d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
