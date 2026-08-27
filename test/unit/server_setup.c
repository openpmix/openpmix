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
 *
 *   role completeness - the three setup calls a launcher makes fan out to
 *     pnet and pgpu side by side, and PMIx_tool_init opened only pnet. A
 *     base function whose framework never opened walks an actives list
 *     that was never constructed, so PMIx_server_setup_fork took a
 *     launcher down; setup_application and setup_local_support survived
 *     only because their pgpu counterparts happen to open with a list-size
 *     guard, which meant the whole pgpu path was silently dead for a
 *     launcher instead. The launcher cases run in a forked child for the
 *     same reason as the rest: this process becomes a server below.
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "include/pmix_server.h"
#include "include/pmix_tool.h"

#include "src/class/pmix_list.h"
#include "src/include/pmix_globals.h"
#include "src/server/pmix_server_ops.h"
#include "src/util/pmix_argv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
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
/* retraction: a deregistration reaching data already handed out        */
/* ------------------------------------------------------------------ */

/* The global cache is copied into a namespace's datastore once, when
 * that namespace is first registered, and nothing re-reads it. So
 * removing an entry from the cache used to govern only the namespaces
 * registered afterwards, while a running job kept its copy - the open
 * decision docs/todo.rst recorded. Deregistration now takes the key back
 * from the namespaces that already have it.
 *
 * Job-level values live under PMIX_RANK_WILDCARD, which is what the get
 * below asks for. */
static void test_retraction(void)
{
    pmix_info_t info;
    pmix_status_t rc;
    pmix_proc_t wildcard;
    pmix_value_t *val;
    pmix_nspace_t ns;

    fprintf(stdout, "\n-- deregistration retracts from a registered namespace --\n");

    PMIX_LOAD_NSPACE(ns, "sut-retract");

    /* register the key BEFORE the namespace, so the namespace is seeded
     * from the cache - that is the only time the copy is made */
    PMIX_INFO_LOAD(&info, SUT_KEY, "handed-out", PMIX_STRING);
    rc = PMIx_server_register_resources(&info, 1, NULL, NULL);
    PMIX_INFO_DESTRUCT(&info);
    if (!ok(rc)) {
        report("retraction precondition: the key registers", false);
        return;
    }

    {
        pmix_info_t jinfo[2];
        uint32_t nprocs = 1;

        PMIX_INFO_LOAD(&jinfo[0], PMIX_JOB_SIZE, &nprocs, PMIX_UINT32);
        PMIX_INFO_LOAD(&jinfo[1], PMIX_UNIV_SIZE, &nprocs, PMIX_UINT32);
        rc = PMIx_server_register_nspace(ns, 1, jinfo, 2, NULL, NULL);
        PMIX_INFO_DESTRUCT(&jinfo[0]);
        PMIX_INFO_DESTRUCT(&jinfo[1]);
    }
    if (!ok(rc)) {
        report("retraction precondition: the namespace registers", false);
        return;
    }

    PMIX_LOAD_PROCID(&wildcard, ns, PMIX_RANK_WILDCARD);
    val = NULL;
    rc = PMIx_Get(&wildcard, SUT_KEY, NULL, 0, &val);
    report("the namespace was seeded with the key",
           PMIX_SUCCESS == rc && NULL != val);
    if (NULL != val) {
        PMIX_VALUE_RELEASE(val);
    }

    /* now take it back */
    PMIX_INFO_LOAD(&info, SUT_KEY, NULL, PMIX_UNDEF);
    rc = PMIx_server_deregister_resources(&info, 1, NULL, NULL);
    PMIX_INFO_DESTRUCT(&info);
    report("deregister_resources accepts the retraction", ok(rc));

    val = NULL;
    rc = PMIx_Get(&wildcard, SUT_KEY, NULL, 0, &val);
    report("the namespace no longer has the key",
           PMIX_SUCCESS != rc || NULL == val);
    if (NULL != val) {
        PMIX_VALUE_RELEASE(val);
    }
}

/* ------------------------------------------------------------------ */
/* qualified deregistration                                            */
/* ------------------------------------------------------------------ */

/* build a PMIX_NODE_INFO_ARRAY naming a host, optionally with a nodeid
 * and a fabric device. PMIx_Info_load routes a PMIX_DATA_ARRAY through
 * copy_darray, so the info owns a deep copy and the source is ours to
 * destruct */
static void load_node_array(pmix_info_t *dest, const char *host,
                            bool have_id, uint32_t nodeid, const char *device)
{
    pmix_data_array_t darray;
    pmix_info_t *iptr;
    size_t n = 0, cnt = 1;

    if (have_id) {
        ++cnt;
    }
    if (NULL != device) {
        ++cnt;
    }
    PMIX_DATA_ARRAY_CONSTRUCT(&darray, cnt, PMIX_INFO);
    iptr = (pmix_info_t *) darray.array;
    PMIX_INFO_LOAD(&iptr[n], PMIX_HOSTNAME, host, PMIX_STRING);
    ++n;
    if (have_id) {
        PMIX_INFO_LOAD(&iptr[n], PMIX_NODEID, &nodeid, PMIX_UINT32);
        ++n;
    }
    if (NULL != device) {
        PMIX_INFO_LOAD(&iptr[n], PMIX_FABRIC_DEVICE_NAME, device, PMIX_STRING);
        ++n;
    }
    PMIX_INFO_LOAD(dest, PMIX_NODE_INFO_ARRAY, &darray, PMIX_DATA_ARRAY);
    PMIX_DATA_ARRAY_DESTRUCT(&darray);
}

/* the same, carrying a nodeid and nothing else */
static void load_nodeid_array(pmix_info_t *dest, uint32_t nodeid)
{
    pmix_data_array_t darray;
    pmix_info_t *iptr;

    PMIX_DATA_ARRAY_CONSTRUCT(&darray, 1, PMIX_INFO);
    iptr = (pmix_info_t *) darray.array;
    PMIX_INFO_LOAD(&iptr[0], PMIX_NODEID, &nodeid, PMIX_UINT32);
    PMIX_INFO_LOAD(dest, PMIX_NODE_INFO_ARRAY, &darray, PMIX_DATA_ARRAY);
    PMIX_DATA_ARRAY_DESTRUCT(&darray);
}

/* a node array carrying two fabric devices, each in its own sub-array -
 * the shape PMIX_FABRIC_DEVICE describes, and the one a qualifier naming
 * a device by name has to reach into */
static void load_device_node(pmix_info_t *dest, const char *host, uint32_t nodeid,
                             const char *dev1, const char *dev2)
{
    pmix_data_array_t darray, d1, d2;
    pmix_info_t *iptr;

    PMIX_DATA_ARRAY_CONSTRUCT(&d1, 1, PMIX_INFO);
    PMIX_INFO_LOAD(&((pmix_info_t *) d1.array)[0], PMIX_FABRIC_DEVICE_NAME, dev1,
                   PMIX_STRING);
    PMIX_DATA_ARRAY_CONSTRUCT(&d2, 1, PMIX_INFO);
    PMIX_INFO_LOAD(&((pmix_info_t *) d2.array)[0], PMIX_FABRIC_DEVICE_NAME, dev2,
                   PMIX_STRING);

    PMIX_DATA_ARRAY_CONSTRUCT(&darray, 4, PMIX_INFO);
    iptr = (pmix_info_t *) darray.array;
    PMIX_INFO_LOAD(&iptr[0], PMIX_HOSTNAME, host, PMIX_STRING);
    PMIX_INFO_LOAD(&iptr[1], PMIX_NODEID, &nodeid, PMIX_UINT32);
    PMIX_INFO_LOAD(&iptr[2], PMIX_FABRIC_DEVICE, &d1, PMIX_DATA_ARRAY);
    PMIX_INFO_LOAD(&iptr[3], PMIX_FABRIC_DEVICE, &d2, PMIX_DATA_ARRAY);
    PMIX_DATA_ARRAY_DESTRUCT(&d1);
    PMIX_DATA_ARRAY_DESTRUCT(&d2);

    PMIX_INFO_LOAD(dest, PMIX_NODE_INFO_ARRAY, &darray, PMIX_DATA_ARRAY);
    PMIX_DATA_ARRAY_DESTRUCT(&darray);
}

/* does any cached node array still carry this device? */
static bool cached_device(const char *name)
{
    pmix_kval_t *kv;
    pmix_info_t *iptr, *sub;
    size_t n, m;

    PMIX_LIST_FOREACH (kv, &pmix_server_globals.gdata, pmix_kval_t) {
        if (!PMIX_CHECK_KEY(kv, PMIX_NODE_INFO_ARRAY) || NULL == kv->value ||
            PMIX_DATA_ARRAY != kv->value->type || NULL == kv->value->data.darray ||
            NULL == kv->value->data.darray->array) {
            continue;
        }
        iptr = (pmix_info_t *) kv->value->data.darray->array;
        for (n = 0; n < kv->value->data.darray->size; n++) {
            if (PMIX_DATA_ARRAY != iptr[n].value.type ||
                NULL == iptr[n].value.data.darray ||
                NULL == iptr[n].value.data.darray->array) {
                continue;
            }
            sub = (pmix_info_t *) iptr[n].value.data.darray->array;
            for (m = 0; m < iptr[n].value.data.darray->size; m++) {
                if (PMIX_CHECK_KEY(&sub[m], PMIX_FABRIC_DEVICE_NAME) &&
                    PMIX_STRING == sub[m].value.type &&
                    NULL != sub[m].value.data.string &&
                    0 == strcmp(sub[m].value.data.string, name)) {
                    return true;
                }
            }
        }
    }
    return false;
}

/* does the cache still hold a node array naming this host? */
static bool cached_node(const char *host)
{
    pmix_kval_t *kv;
    pmix_info_t *iptr;
    size_t n;

    PMIX_LIST_FOREACH (kv, &pmix_server_globals.gdata, pmix_kval_t) {
        if (!PMIX_CHECK_KEY(kv, PMIX_NODE_INFO_ARRAY) || NULL == kv->value ||
            PMIX_DATA_ARRAY != kv->value->type || NULL == kv->value->data.darray ||
            NULL == kv->value->data.darray->array) {
            continue;
        }
        iptr = (pmix_info_t *) kv->value->data.darray->array;
        for (n = 0; n < kv->value->data.darray->size; n++) {
            if (PMIX_CHECK_KEY(&iptr[n], PMIX_HOSTNAME) &&
                PMIX_STRING == iptr[n].value.type &&
                NULL != iptr[n].value.data.string &&
                0 == strcmp(iptr[n].value.data.string, host)) {
                return true;
            }
        }
    }
    return false;
}

static void test_qualified_dereg(void)
{
    pmix_info_t info;
    pmix_status_t rc;
    size_t base;

    base = ncached();

    /* two nodes registered under the same key */
    load_node_array(&info, "nodeA", true, 0, NULL);
    rc = PMIx_server_register_resources(&info, 1, NULL, NULL);
    report("register_resources accepts a node array", ok(rc));
    PMIX_INFO_DESTRUCT(&info);

    load_node_array(&info, "nodeB", true, 1, NULL);
    rc = PMIx_server_register_resources(&info, 1, NULL, NULL);
    report("register_resources accepts a second node array", ok(rc));
    PMIX_INFO_DESTRUCT(&info);

    report("both nodes are cached",
           base + 2 == ncached() && cached_node("nodeA") && cached_node("nodeB"));

    /* a qualifier naming one node by hostname removes that node only.
     * Matching on the key alone - which is what this used to do - takes
     * both, and nothing here can put the other one back */
    load_node_array(&info, "nodeA", false, 0, NULL);
    rc = PMIx_server_deregister_resources(&info, 1, NULL, NULL);
    report("a hostname-qualified removal is accepted", ok(rc));
    PMIX_INFO_DESTRUCT(&info);

    report("the qualified removal took only the named node",
           base + 1 == ncached() && !cached_node("nodeA") && cached_node("nodeB"));

    /* the same, by nodeid alone - the qualifier carries no hostname, so
     * only the nodeid can match it to the stored entry */
    load_nodeid_array(&info, 1);
    rc = PMIx_server_deregister_resources(&info, 1, NULL, NULL);
    report("a nodeid-qualified removal is accepted", ok(rc));
    PMIX_INFO_DESTRUCT(&info);

    report("the nodeid-qualified removal took the second node",
           base == ncached() && !cached_node("nodeB"));

    /* a qualifier naming something inside the stored array takes that
     * element out and leaves the rest of the node alone */
    load_device_node(&info, "nodeC", 2, "mlx5_0", "mlx5_1");
    rc = PMIx_server_register_resources(&info, 1, NULL, NULL);
    report("register_resources accepts a node array carrying devices", ok(rc));
    PMIX_INFO_DESTRUCT(&info);

    load_node_array(&info, "nodeC", false, 0, "mlx5_0");
    rc = PMIx_server_deregister_resources(&info, 1, NULL, NULL);
    report("an element-level qualifier is accepted", ok(rc));
    PMIX_INFO_DESTRUCT(&info);

    report("the named device is gone and the node is not",
           base + 1 == ncached() && cached_node("nodeC") &&
           !cached_device("mlx5_0") && cached_device("mlx5_1"));

    /* taking the last thing the entry described takes the entry: what is
     * left names a node and nothing else, which is what an empty
     * registration would have produced */
    load_node_array(&info, "nodeC", false, 0, "mlx5_1");
    rc = PMIx_server_deregister_resources(&info, 1, NULL, NULL);
    report("removing the last device is accepted", ok(rc));
    PMIX_INFO_DESTRUCT(&info);

    report("the emptied entry was dropped",
           base == ncached() && !cached_node("nodeC"));

    /* re-register a plain node so the unqualified case below has
     * something to clear */
    load_node_array(&info, "nodeC", true, 2, NULL);
    rc = PMIx_server_register_resources(&info, 1, NULL, NULL);
    report("register_resources accepts a third node array", ok(rc));
    PMIX_INFO_DESTRUCT(&info);

    /* an unqualified request still selects by key alone */
    PMIX_INFO_LOAD(&info, PMIX_NODE_INFO_ARRAY, NULL, PMIX_UNDEF);
    rc = PMIx_server_deregister_resources(&info, 1, NULL, NULL);
    report("an unqualified removal is accepted", ok(rc));
    PMIX_INFO_DESTRUCT(&info);

    report("the unqualified removal cleared the key", base == ncached());
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

/* PMIx_server_setup_fork is blocking, has no callback, and reads library
 * state the progress thread writes - the pnet and pgpu per-namespace envar
 * caches, the local datastore, pmix_server_globals.genvars. It therefore
 * runs its body on that thread. The wrinkle is a host that calls it from
 * inside a PMIx callback: such a host is *already* on the progress thread,
 * and posting an event to wait for there would be waiting for itself.
 *
 * Both entries have to work, and a regression in either shows up as a
 * hang rather than a wrong answer, so the whole case is fenced with an
 * alarm - dying beats blocking a CI run forever. */
typedef struct {
    pmix_event_t ev;
    pmix_lock_t lock;
    pmix_status_t status;
    int nvars;
} fork_probe_t;

static void probe_setup_fork(int sd, short args, void *cbdata)
{
    fork_probe_t *pb = (fork_probe_t *) cbdata;
    pmix_proc_t proc;
    char **env = NULL;

    (void) sd;
    (void) args;

    PMIX_LOAD_PROCID(&proc, SUT_NSPACE, 0);
    pb->status = PMIx_server_setup_fork(&proc, &env);
    pb->nvars = PMIx_Argv_count(env);
    PMIx_Argv_free(env);

    PMIX_WAKEUP_THREAD(&pb->lock);
}

static void test_setup_fork_reentry(void)
{
    fork_probe_t pb;
    pmix_proc_t proc;
    char **env = NULL;
    pmix_status_t rc;
    int direct;

    alarm(60);

    /* the ordinary entry: from the host's own thread, which shifts */
    PMIX_LOAD_PROCID(&proc, SUT_NSPACE, 0);
    rc = PMIx_server_setup_fork(&proc, &env);
    direct = PMIx_Argv_count(env);
    PMIx_Argv_free(env);
    report("setup_fork from the host's thread", ok(rc) && 0 < direct);

    /* and from the progress thread, where it must run inline */
    pb.status = PMIX_ERR_NOT_SUPPORTED;
    pb.nvars = 0;
    PMIX_CONSTRUCT_LOCK(&pb.lock);
    PMIX_THREADSHIFT(&pb, probe_setup_fork);
    PMIX_WAIT_THREAD(&pb.lock);
    PMIX_DESTRUCT_LOCK(&pb.lock);
    report("setup_fork from the progress thread does not wait on itself",
           ok(pb.status));

    /* and says the same thing from either side */
    report("both entries produce the same environment", direct == pb.nvars);

    alarm(0);
}

/* ------------------------------------------------------------------
 * All four job-preparation entry points must screen the SERVER
 * library's flag
 *
 * Their documented PMIX_ERR_INIT means "the PMIx server library has not
 * been initialized", and pmix_globals.initialized does not answer that:
 * PMIx_Init and PMIx_tool_init set it just as readily as
 * PMIx_server_init does.
 *
 * A CLIENT is the case that bites the two resource calls. PMIx_Init
 * constructs only the two IOF lists in pmix_server_globals, so gdata is
 * still PMIX_LIST_STATIC_INIT, whose sentinel carries NULL next and prev
 * pointers: the registration path writes through the NULL prev inside
 * pmix_list_append and the deregistration path walks off the NULL next.
 * Both are a SIGSEGV on the progress thread, taken after the entry point
 * has already answered PMIX_SUCCESS - which is why these cases run in a
 * forked child. A tool escapes the crash, PMIx_tool_init having called
 * pmix_server_initialize(), but is owed the same refusal.
 *
 * The two SETUP calls are deliberately NOT screened that way, and the
 * last two cases here are what keeps anyone from "fixing" that. They fan
 * out to pnet, pgpu and pmdl, and PMIx_tool_init opens pmdl
 * unconditionally and pnet for a launcher - saying so at the site, "we
 * might need them if we are asking a server to launch something for us".
 * PRRTE's prun is exactly that caller: it reaches
 * PMIx_server_setup_application through prun_common.c after
 * PMIx_tool_init and never calls PMIx_server_init at all, so screening
 * the server library's flag turned every prun launch into
 * PMIX_ERR_INIT. A plain client stands in for that here, since the API
 * cannot tell the two apart: the call must be accepted and its callback
 * must fire.
 *
 * The non-blocking form is used deliberately. The blocking form would
 * have an unfixed library hang the child on a lock the dead progress
 * thread can no longer wake, and a hang is a worse failure report than a
 * signal.
 * ------------------------------------------------------------------ */

/* none of these may fire: the entry point must refuse before it shifts */
/* May or may not be called: what an entry point does for a role with no
 * server library behind it is not a contract. */
static void tolerant_op(pmix_status_t status, void *cbdata)
{
    (void) status;
    (void) cbdata;
}

/* the setup pair must reach their callback, so these record that it
 * happened rather than refusing to be called */
static int setup_fired = 0;
static pmix_status_t setup_status = PMIX_ERR_NOT_SUPPORTED;

static void accepted_setup(pmix_status_t status, pmix_info_t info[], size_t ninfo,
                           void *provided_cbdata, pmix_op_cbfunc_t cbfunc, void *cbdata)
{
    (void) info;
    (void) ninfo;
    (void) provided_cbdata;

    setup_fired = 1;
    setup_status = status;
    /* the library owns whatever came back and frees it when we call this */
    if (NULL != cbfunc) {
        cbfunc(PMIX_SUCCESS, cbdata);
    }
}

static void accepted_op(pmix_status_t status, void *cbdata)
{
    (void) cbdata;

    setup_fired = 1;
    setup_status = status;
}

static int setup_client_child(int which)
{
    pmix_proc_t myproc;
    pmix_info_t info;
    pmix_status_t rc;
    uint32_t one = 1;

    /* a singleton reports PMIX_ERR_UNREACH from init - it is fully
     * initialized, it just has no server */
    rc = PMIx_Init(&myproc, NULL, 0);
    if (PMIX_SUCCESS != rc && PMIX_ERR_UNREACH != rc) {
        return 3;
    }

    PMIX_INFO_LOAD(&info, SUT_KEY, &one, PMIX_UINT32);
    if (0 == which || 1 == which) {
        /* the two a client has no business calling. What it is answered
         * is not asserted - the library does not try to diagnose that
         * mistake - only that the process is still alive afterwards. */
        if (0 == which) {
            rc = PMIx_server_register_resources(&info, 1, tolerant_op, NULL);
        } else {
            rc = PMIx_server_deregister_resources(&info, 1, tolerant_op, NULL);
        }
        PMIX_INFO_DESTRUCT(&info);
        /* give the progress thread a turn: the shifted handler is what
         * used to crash, and it has not run yet */
        (void) PMIx_Get(&myproc, PMIX_UNIV_SIZE, NULL, 0, NULL);
        PMIx_Finalize(NULL, 0);
        (void) rc;
        return 0;
    }

    /* the two that must be ACCEPTED - a launcher reaches these with no
     * PMIx_server_init behind it */
    if (2 == which) {
        rc = PMIx_server_setup_application(myproc.nspace, &info, 1,
                                           accepted_setup, NULL);
    } else {
        rc = PMIx_server_setup_local_support(myproc.nspace, &info, 1,
                                             accepted_op, NULL);
    }
    PMIX_INFO_DESTRUCT(&info);
    if (PMIX_SUCCESS != rc) {
        PMIx_Finalize(NULL, 0);
        return 1;
    }
    /* the callback lands on the progress thread; give it turns to run
     * rather than a sleep, and fail rather than hang if it never does */
    for (int i = 0; i < 200 && !setup_fired; ++i) {
        pmix_proc_t self;
        PMIX_LOAD_PROCID(&self, myproc.nspace, myproc.rank);
        (void) PMIx_Get(&self, PMIX_UNIV_SIZE, NULL, 0, NULL);
    }
    PMIx_Finalize(NULL, 0);
    if (!setup_fired) {
        return 4;
    }
    /* the STATUS the callback carries is deliberately not asserted. What
     * this case is about is the entry point accepting the request; what
     * comes back depends on which frameworks the caller's role opened,
     * and a plain client gets PMIX_ERR_INIT out of
     * pmix_pmdl_base_harvest_envars, which is the framework answering
     * honestly for a role that never opened it. A launcher, which is the
     * caller this protects, has pmdl open and gets a real answer. */
    (void) setup_status;
    return 0;
}

/* The launcher case itself, rather than the client standing in for it:
 * come up through PMIx_tool_init - which is the state prun is in when it
 * calls PMIx_server_setup_application - and require the call to be
 * accepted. A tool has pmdl open, so this one gets a real answer and the
 * status IS asserted. */
static int setup_tool_child(int which)
{
    pmix_proc_t myproc;
    pmix_info_t tinfo, info;
    pmix_status_t rc;
    uint32_t one = 1;

    PMIX_INFO_LOAD(&tinfo, PMIX_TOOL_DO_NOT_CONNECT, NULL, PMIX_BOOL);
    rc = PMIx_tool_init(&myproc, &tinfo, 1);
    PMIX_INFO_DESTRUCT(&tinfo);
    if (PMIX_SUCCESS != rc) {
        return 3;
    }

    PMIX_INFO_LOAD(&info, SUT_KEY, &one, PMIX_UINT32);
    if (0 == which) {
        rc = PMIx_server_setup_application(myproc.nspace, &info, 1,
                                           accepted_setup, NULL);
    } else {
        rc = PMIx_server_setup_local_support(myproc.nspace, &info, 1,
                                             accepted_op, NULL);
    }
    PMIX_INFO_DESTRUCT(&info);
    if (PMIX_SUCCESS != rc) {
        PMIx_tool_finalize();
        return 1;
    }
    for (int i = 0; i < 200 && !setup_fired; ++i) {
        (void) PMIx_Get(&myproc, PMIX_UNIV_SIZE, NULL, 0, NULL);
    }
    PMIx_tool_finalize();
    if (!setup_fired) {
        return 4;
    }
    return (PMIX_SUCCESS == setup_status) ? 0 : 5;
}

/* The launcher shape, which is not the same as the tool shape above.
 * PMIx_tool_init opens pnet and pgpu only for a launcher or a scheduler,
 * and the three server-side setup calls a launcher makes fan out to both
 * of those frameworks side by side. A launcher that came up without one
 * of them reaches a base function whose actives list was never
 * constructed - the walk starts at a NULL sentinel - so this comes up as
 * prun does and requires all three calls to be answered and the process
 * to still be alive afterwards.
 *
 * PMIx_server_setup_fork is the one that failed: pmix_pgpu_base_setup_fork
 * walks its actives list with no size guard, and the call is blocking, so
 * the child died before the API returned anything at all. */
static int setup_launcher_child(int which)
{
    pmix_proc_t myproc, child;
    pmix_info_t tinfo[2], info;
    pmix_status_t rc;
    char **env = NULL;
    uint32_t one = 1;

    PMIX_INFO_LOAD(&tinfo[0], PMIX_TOOL_DO_NOT_CONNECT, NULL, PMIX_BOOL);
    PMIX_INFO_LOAD(&tinfo[1], PMIX_LAUNCHER, NULL, PMIX_BOOL);
    rc = PMIx_tool_init(&myproc, tinfo, 2);
    PMIX_INFO_DESTRUCT(&tinfo[0]);
    PMIX_INFO_DESTRUCT(&tinfo[1]);
    if (PMIX_SUCCESS != rc) {
        return 3;
    }

    if (2 == which) {
        /* the crash case - blocking, so surviving it IS the assertion */
        PMIX_LOAD_PROCID(&child, myproc.nspace, 0);
        rc = PMIx_server_setup_fork(&child, &env);
        PMIx_Argv_free(env);
        PMIx_tool_finalize();
        return (PMIX_SUCCESS == rc) ? 0 : 1;
    }

    PMIX_INFO_LOAD(&info, SUT_KEY, &one, PMIX_UINT32);
    if (0 == which) {
        rc = PMIx_server_setup_application(myproc.nspace, &info, 1,
                                           accepted_setup, NULL);
    } else {
        rc = PMIx_server_setup_local_support(myproc.nspace, &info, 1,
                                             accepted_op, NULL);
    }
    PMIX_INFO_DESTRUCT(&info);
    if (PMIX_SUCCESS != rc) {
        PMIx_tool_finalize();
        return 1;
    }
    for (int i = 0; i < 200 && !setup_fired; ++i) {
        (void) PMIx_Get(&myproc, PMIX_UNIV_SIZE, NULL, 0, NULL);
    }
    PMIx_tool_finalize();
    if (!setup_fired) {
        return 4;
    }
    return (PMIX_SUCCESS == setup_status) ? 0 : 5;
}

static void check_client_refusal(const char *name, int which)
{
    pid_t child;
    int status = 0;

    fflush(stdout);
    child = fork();
    if (0 == child) {
        if (20 <= which) {
            _exit(setup_launcher_child(which - 20));
        }
        _exit((which < 10) ? setup_client_child(which) : setup_tool_child(which - 10));
    }
    if (0 > child) {
        report(name, 0);
        return;
    }
    if (0 > waitpid(child, &status, 0)) {
        report(name, 0);
        return;
    }
    /* a child that died on a signal is the unfixed library crashing on
     * the progress thread, which is exactly what the screen prevents */
    report(name, WIFEXITED(status) && 0 == WEXITSTATUS(status));
}

int main(int argc, char **argv)
{
    static pmix_server_module_t mymodule = {0};
    pmix_status_t rc;

    (void) argc;
    (void) argv;

    fprintf(stdout, "server_setup: job-preparation unit tests\n");

    /* these fork, so they run before this process becomes a server */
    check_client_refusal("register_resources from a client is not fatal", 0);
    check_client_refusal("deregister_resources from a client is not fatal", 1);
    check_client_refusal("setup_application is accepted without a server library", 2);
    check_client_refusal("setup_local_support is accepted without a server library", 3);
    /* and the case that actually regressed: a launcher, as prun is */
    check_client_refusal("setup_application works for a tool, as prun needs", 10);
    check_client_refusal("setup_local_support works for a tool", 11);
    /* and the launcher, which is the role that actually opens pnet and
     * pgpu - the third of these took the process down */
    check_client_refusal("setup_application works for a launcher", 20);
    check_client_refusal("setup_local_support works for a launcher", 21);
    check_client_refusal("setup_fork works for a launcher", 22);

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
    test_qualified_dereg();
    test_retraction();
    test_job_info();
    test_group_arrays();
    test_setup_fork_reentry();

    PMIx_server_finalize();

    fprintf(stdout, "server_setup: %d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
