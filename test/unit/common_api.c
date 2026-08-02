/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Regression coverage for defects found reviewing src/common.
 *
 * Like test/unit/client_api.c, everything here runs as a singleton - no
 * server, no launcher - so the whole file is safe under "make check".
 * That bounds what can be covered to the paths src/common resolves
 * locally or rejects up front, which is where this round's defects were:
 *
 *   1. PMIx_Log_nb read the value of a PMIX_LOG_SOURCE directive through
 *      the "proc" member of the union without ever checking that the
 *      caller had actually stored a proc there. Any other type - a
 *      string, an int - was dereferenced as a pointer.
 *
 *   2. PMIx_Query_info_nb accepted a query carrying no keys. The parser
 *      that runs on the progress thread walks keys[] to its NULL
 *      terminator, so a NULL array was dereferenced immediately.
 *
 *   3. The same parser read the PMIX_PROCID / PMIX_NSPACE / PMIX_RANK
 *      qualifiers through the matching member of the value union with no
 *      type check - the same wild dereference as (1).
 *
 *   4. PMIx_Data_copy_payload checked its source for NULL but not its
 *      destination, then embedded the destination buffer regardless.
 *
 *   5. PMIx_Info_directives_string never learned about
 *      PMIX_INFO_PERSISTENT, so a persistent info printed as though the
 *      flag were not set at all.
 *
 * The locally-resolved queries are repeated enough times that a caddy
 * leak or a reference-count error shows up as growth or a crash rather
 * than passing quietly - the query caddy is released through several
 * different paths depending on whether the request was answered locally,
 * and one of them (a NULL cbfunc) used to release it through none.
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "src/common/pmix_iof.h"
#include "src/include/pmix_globals.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define REPEAT 200

static int nfail = 0;

static void check(int ok, const char *what)
{
    if (ok) {
        fprintf(stdout, "  ok    : %s\n", what);
    } else {
        fprintf(stdout, "  FAILED: %s\n", what);
        nfail++;
    }
}

static void opcb(pmix_status_t status, void *cbdata)
{
    (void) status;
    (void) cbdata;
}

static void infocb(pmix_status_t status, pmix_info_t *info, size_t ninfo, void *cbdata,
                   pmix_release_cbfunc_t release_fn, void *release_cbdata)
{
    (void) status;
    (void) info;
    (void) ninfo;
    (void) cbdata;
    if (NULL != release_fn) {
        release_fn(release_cbdata);
    }
}

/* ------------------------------------------------------------------ */
/* PMIx_Log: a PMIX_LOG_SOURCE whose value is not a proc               */
/* ------------------------------------------------------------------ */
static void test_log_bad_source(void)
{
    pmix_status_t rc;
    pmix_info_t data, dir;

    fprintf(stdout, "\n-- PMIx_Log_nb, malformed PMIX_LOG_SOURCE --\n");

    PMIX_INFO_LOAD(&data, PMIX_LOG_STDERR, "regression", PMIX_STRING);

    /* a string where a proc belongs: the old code memcpy'd sizeof(proc)
     * bytes from the string pointer */
    PMIX_INFO_LOAD(&dir, PMIX_LOG_SOURCE, "not-a-proc", PMIX_STRING);
    rc = PMIx_Log_nb(&data, 1, &dir, 1, opcb, NULL);
    check(PMIX_ERR_BAD_PARAM == rc, "string-valued PMIX_LOG_SOURCE rejected");
    PMIX_INFO_DESTRUCT(&dir);

    /* an int is just as wrong, and lands on a small non-pointer value */
    PMIX_INFO_LOAD(&dir, PMIX_LOG_SOURCE, NULL, PMIX_BOOL);
    rc = PMIx_Log_nb(&data, 1, &dir, 1, opcb, NULL);
    check(PMIX_ERR_BAD_PARAM == rc, "bool-valued PMIX_LOG_SOURCE rejected");
    PMIX_INFO_DESTRUCT(&dir);

    /* a well-formed source must still be accepted - the check must not
     * have made the good case unreachable */
    PMIX_INFO_DESTRUCT(&data);
    PMIX_INFO_LOAD(&data, PMIX_LOG_STDERR, "regression", PMIX_STRING);
    rc = PMIx_Log_nb(&data, 1, NULL, 0, opcb, NULL);
    check(PMIX_ERR_BAD_PARAM != rc, "log with no directives still accepted");
    PMIX_INFO_DESTRUCT(&data);

    /* an empty data array is a bad parameter, and must be caught before
     * anything is allocated on its behalf */
    rc = PMIx_Log_nb(NULL, 0, NULL, 0, opcb, NULL);
    check(PMIX_ERR_BAD_PARAM == rc, "PMIx_Log_nb(NULL data) rejected");
}

/* ------------------------------------------------------------------ */
/* PMIx_Query_info: malformed queries                                  */
/* ------------------------------------------------------------------ */
static void test_query_bad_params(void)
{
    pmix_status_t rc;
    pmix_query_t query;
    pmix_info_t *results = NULL;
    size_t nresults = 0;
    char *nokeys[] = {NULL};

    fprintf(stdout, "\n-- PMIx_Query_info, malformed queries --\n");

    /* a query with no keys array at all */
    PMIX_QUERY_CONSTRUCT(&query);
    rc = PMIx_Query_info(&query, 1, &results, &nresults);
    check(PMIX_ERR_BAD_PARAM == rc, "query with NULL keys rejected");
    PMIX_QUERY_DESTRUCT(&query);

    /* a keys array that is present but empty */
    PMIX_QUERY_CONSTRUCT(&query);
    query.keys = nokeys;
    rc = PMIx_Query_info(&query, 1, &results, &nresults);
    check(PMIX_ERR_BAD_PARAM == rc, "query with empty keys rejected");
    query.keys = NULL;
    PMIX_QUERY_DESTRUCT(&query);

    /* PMIX_PROCID carrying something that is not a proc */
    PMIX_QUERY_CONSTRUCT(&query);
    PMIx_Argv_append_nosize(&query.keys, PMIX_QUERY_STABLE_ABI_VERSION);
    query.nqual = 1;
    PMIX_INFO_CREATE(query.qualifiers, 1);
    PMIX_INFO_LOAD(&query.qualifiers[0], PMIX_PROCID, "not-a-proc", PMIX_STRING);
    rc = PMIx_Query_info(&query, 1, &results, &nresults);
    check(PMIX_ERR_BAD_PARAM == rc, "string-valued PMIX_PROCID qualifier rejected");
    PMIX_QUERY_DESTRUCT(&query);

    /* PMIX_NSPACE carrying something that is not a string */
    PMIX_QUERY_CONSTRUCT(&query);
    PMIx_Argv_append_nosize(&query.keys, PMIX_QUERY_STABLE_ABI_VERSION);
    query.nqual = 1;
    PMIX_INFO_CREATE(query.qualifiers, 1);
    PMIX_INFO_LOAD(&query.qualifiers[0], PMIX_NSPACE, NULL, PMIX_BOOL);
    rc = PMIx_Query_info(&query, 1, &results, &nresults);
    check(PMIX_ERR_BAD_PARAM == rc, "bool-valued PMIX_NSPACE qualifier rejected");
    PMIX_QUERY_DESTRUCT(&query);

    /* PMIX_RANK carrying something that is not a rank */
    PMIX_QUERY_CONSTRUCT(&query);
    PMIx_Argv_append_nosize(&query.keys, PMIX_QUERY_STABLE_ABI_VERSION);
    query.nqual = 1;
    PMIX_INFO_CREATE(query.qualifiers, 1);
    PMIX_INFO_LOAD(&query.qualifiers[0], PMIX_RANK, "not-a-rank", PMIX_STRING);
    rc = PMIx_Query_info(&query, 1, &results, &nresults);
    check(PMIX_ERR_BAD_PARAM == rc, "string-valued PMIX_RANK qualifier rejected");
    PMIX_QUERY_DESTRUCT(&query);

    /* zero queries is still a bad parameter */
    rc = PMIx_Query_info(NULL, 0, &results, &nresults);
    check(PMIX_ERR_BAD_PARAM == rc, "PMIx_Query_info(NULL queries) rejected");
}

/* ------------------------------------------------------------------ */
/* PMIx_Query_info: the locally-resolved path, repeatedly              */
/* ------------------------------------------------------------------ */
static void test_query_local(void)
{
    pmix_status_t rc;
    pmix_query_t query;
    pmix_info_t *results;
    size_t nresults;
    int i, good = 0;

    fprintf(stdout, "\n-- PMIx_Query_info, locally resolved --\n");

    for (i = 0; i < REPEAT; i++) {
        PMIX_QUERY_CONSTRUCT(&query);
        PMIx_Argv_append_nosize(&query.keys, PMIX_QUERY_STABLE_ABI_VERSION);
        results = NULL;
        nresults = 0;
        rc = PMIx_Query_info(&query, 1, &results, &nresults);
        if (PMIX_SUCCESS == rc && 0 < nresults && NULL != results) {
            good++;
        }
        if (NULL != results) {
            PMIX_INFO_FREE(results, nresults);
        }
        PMIX_QUERY_DESTRUCT(&query);
    }
    check(REPEAT == good, "ABI-version query answered locally every time");

    /* the same request with no callback at all: the caddy and everything
     * it built have to be released by the handler itself, since the
     * release function only ever runs by way of a callback */
    for (i = 0; i < REPEAT; i++) {
        PMIX_QUERY_CONSTRUCT(&query);
        PMIx_Argv_append_nosize(&query.keys, PMIX_QUERY_PROVISIONAL_ABI_VERSION);
        rc = PMIx_Query_info_nb(&query, 1, NULL, NULL);
        PMIX_QUERY_DESTRUCT(&query);
        if (PMIX_SUCCESS != rc) {
            break;
        }
    }
    check(PMIX_SUCCESS == rc, "locally-resolved query with a NULL cbfunc accepted");
    /* let the progress thread drain those before we tear down */
    usleep(200000);

    /* and with a callback, to keep the ordinary path exercised too */
    PMIX_QUERY_CONSTRUCT(&query);
    PMIx_Argv_append_nosize(&query.keys, PMIX_QUERY_STABLE_ABI_VERSION);
    rc = PMIx_Query_info_nb(&query, 1, infocb, NULL);
    check(PMIX_SUCCESS == rc, "locally-resolved query with a cbfunc accepted");
    PMIX_QUERY_DESTRUCT(&query);
    usleep(200000);
}

/* ------------------------------------------------------------------ */
/* PMIx_Data_copy_payload: a NULL destination                          */
/* ------------------------------------------------------------------ */
static void test_data_bad_params(void)
{
    pmix_status_t rc;
    pmix_data_buffer_t src, dst;
    int32_t val = 42;

    fprintf(stdout, "\n-- PMIx_Data_copy_payload, missing buffers --\n");

    PMIX_DATA_BUFFER_CONSTRUCT(&src);
    rc = PMIx_Data_pack(NULL, &src, &val, 1, PMIX_INT32);
    check(PMIX_SUCCESS == rc, "packed a value for our own peer");

    /* a NULL source is defined to be a no-op */
    rc = PMIx_Data_copy_payload(&src, NULL);
    check(PMIX_SUCCESS == rc, "NULL source is a no-op");

    /* a NULL destination used to be embedded anyway */
    rc = PMIx_Data_copy_payload(NULL, &src);
    check(PMIX_ERR_BAD_PARAM == rc, "NULL destination rejected");

    /* and the real thing still works */
    PMIX_DATA_BUFFER_CONSTRUCT(&dst);
    rc = PMIx_Data_copy_payload(&dst, &src);
    check(PMIX_SUCCESS == rc, "payload copied between two buffers");
    PMIX_DATA_BUFFER_DESTRUCT(&dst);
    PMIX_DATA_BUFFER_DESTRUCT(&src);
}

/* ------------------------------------------------------------------ */
/* PMIx_Info_directives_string: every flag it is asked to render       */
/* ------------------------------------------------------------------ */
static void test_directives_string(void)
{
    char *str;

    fprintf(stdout, "\n-- PMIx_Info_directives_string --\n");

    str = PMIx_Info_directives_string(PMIX_INFO_REQD);
    check(NULL != str && NULL != strstr(str, "REQUIRED"), "REQD renders as REQUIRED");
    free(str);

    str = PMIx_Info_directives_string(PMIX_INFO_QUALIFIER);
    check(NULL != str && NULL != strstr(str, "QUALIFIER"), "QUALIFIER renders");
    free(str);

    /* PMIX_INFO_PERSISTENT was added to the header without being added
     * here, so a persistent info printed as a plain optional one */
    str = PMIx_Info_directives_string(PMIX_INFO_PERSISTENT);
    check(NULL != str && NULL != strstr(str, "PERSISTENT"), "PERSISTENT renders");
    free(str);

    str = PMIx_Info_directives_string(PMIX_INFO_REQD | PMIX_INFO_PERSISTENT);
    check(NULL != str && NULL != strstr(str, "REQUIRED") && NULL != strstr(str, "PERSISTENT"),
          "REQD|PERSISTENT renders both");
    free(str);
}

/* ------------------------------------------------------------------ */
/* IOF: sources, flags, and the XML escaper                            */
/* ------------------------------------------------------------------ */
static void iofcb(size_t iofhdlr, pmix_iof_channel_t channel, pmix_proc_t *source,
                  pmix_byte_object_t *payload, pmix_info_t info[], size_t ninfo)
{
    (void) iofhdlr;
    (void) channel;
    (void) source;
    (void) payload;
    (void) info;
    (void) ninfo;
}

static void test_iof_bad_params(void)
{
    pmix_status_t rc;
    pmix_proc_t proc;

    fprintf(stdout, "\n-- PMIx_IOF_pull, malformed sources --\n");

    /* the sources are copied wholesale into the request, so a count
     * with no array behind it used to be a memcpy from NULL */
    rc = PMIx_IOF_pull(NULL, 1, NULL, 0, PMIX_FWD_STDOUT_CHANNEL, iofcb, NULL, NULL);
    check(PMIX_ERR_BAD_PARAM == rc, "PMIx_IOF_pull(NULL sources, 1) rejected");

    rc = PMIx_IOF_pull(NULL, 0, NULL, 0, PMIX_FWD_STDOUT_CHANNEL, iofcb, NULL, NULL);
    check(PMIX_ERR_BAD_PARAM == rc, "PMIx_IOF_pull(no sources) rejected");

    /* stdin is still refused for its own reason, ahead of the above */
    PMIX_LOAD_PROCID(&proc, "nspace", PMIX_RANK_WILDCARD);
    rc = PMIx_IOF_pull(&proc, 1, NULL, 0, PMIX_FWD_STDIN_CHANNEL, iofcb, NULL, NULL);
    check(PMIX_ERR_NOT_SUPPORTED == rc, "PMIx_IOF_pull(stdin) still refused");
}

static void test_iof_flags(void)
{
    pmix_iof_flags_t flags;
    pmix_info_t info;

    fprintf(stdout, "\n-- pmix_iof_check_flags, malformed output names --\n");

    /* a name that is not a string used to be strdup'd from whatever
     * else shared the value union */
    pmix_iof_init_flags(&flags);
    PMIX_INFO_LOAD(&info, PMIX_IOF_OUTPUT_TO_FILE, NULL, PMIX_BOOL);
    pmix_iof_check_flags(&info, &flags);
    check(NULL == flags.file, "bool-valued PMIX_IOF_OUTPUT_TO_FILE ignored");
    PMIX_INFO_DESTRUCT(&info);

    pmix_iof_init_flags(&flags);
    PMIX_INFO_LOAD(&info, PMIX_IOF_OUTPUT_TO_DIRECTORY, NULL, PMIX_BOOL);
    pmix_iof_check_flags(&info, &flags);
    check(NULL == flags.directory, "bool-valued PMIX_IOF_OUTPUT_TO_DIRECTORY ignored");
    PMIX_INFO_DESTRUCT(&info);

    /* and a well-formed one is still taken */
    pmix_iof_init_flags(&flags);
    PMIX_INFO_LOAD(&info, PMIX_IOF_OUTPUT_TO_FILE, "out", PMIX_STRING);
    pmix_iof_check_flags(&info, &flags);
    check(NULL != flags.file && 0 == strcmp(flags.file, "out"),
          "string-valued PMIX_IOF_OUTPUT_TO_FILE taken");
    if (NULL != flags.file) {
        free(flags.file);
    }
    PMIX_INFO_DESTRUCT(&info);
}

static void test_iof_xml_escaping(void)
{
    pmix_iof_flags_t flags;
    pmix_byte_object_t bo, *out;
    pmix_proc_t name;
    char payload[6];
    char *text;
    int ok;

    fprintf(stdout, "\n-- pmix_iof_prep_output, XML escaping --\n");

    /* '<', '&', '>' are escaped by name; the two high-bit bytes are not
     * printable and become numeric character references. Reading them
     * through a signed char - which is what the escaper used to do -
     * is undefined for isprint() and renders "&#-01;" style references
     * that are not valid XML at all */
    payload[0] = '<';
    payload[1] = '&';
    payload[2] = '>';
    payload[3] = (char) 0x80;
    payload[4] = (char) 0xFF;
    payload[5] = '\n';
    bo.bytes = payload;
    bo.size = sizeof(payload);

    PMIX_LOAD_PROCID(&name, "nspace", 0);
    pmix_iof_init_flags(&flags);
    flags.set = true;
    flags.xml = true;

    out = pmix_iof_prep_output(&name, &flags, PMIX_FWD_STDOUT_CHANNEL, &bo);
    check(NULL != out && NULL != out->bytes, "prep_output produced XML output");
    if (NULL == out || NULL == out->bytes) {
        return;
    }
    /* prep_output returns a counted buffer, not a C string */
    text = (char *) malloc(out->size + 1);
    memcpy(text, out->bytes, out->size);
    text[out->size] = '\0';

    check(NULL != strstr(text, "&lt;"), "'<' escaped as &lt;");
    check(NULL != strstr(text, "&amp;"), "'&' escaped as &amp;");
    check(NULL != strstr(text, "&gt;"), "'>' escaped as &gt;");
    check(NULL != strstr(text, "&#128;"), "0x80 escaped as &#128;");
    check(NULL != strstr(text, "&#255;"), "0xFF escaped as &#255;");
    ok = (NULL == strstr(text, "&#-"));
    check(ok, "no negative character references emitted");

    free(text);
    PMIx_Byte_object_free(out, 1);
}

/* ------------------------------------------------------------------ */
/* PMIx_Register_attributes: a function name that is not there         */
/* ------------------------------------------------------------------ */
static void test_register_attributes(void)
{
    pmix_status_t rc;
    char *attrs[] = {"PMIX_TESTATTR", NULL};

    fprintf(stdout, "\n-- PMIx_Register_attributes --\n");

    /* the name is compared against and then duplicated on the progress
     * thread, so a NULL used to reach strdup() */
    rc = PMIx_Register_attributes(NULL, attrs);
    check(PMIX_ERR_BAD_PARAM == rc, "NULL function name rejected");

    /* a real registration still works, and is still refused twice */
    rc = PMIx_Register_attributes("common_api_test_fn", attrs);
    check(PMIX_SUCCESS == rc, "named function registered");
    rc = PMIx_Register_attributes("common_api_test_fn", attrs);
    check(PMIX_ERR_REPEAT_ATTR_REGISTRATION == rc, "duplicate registration refused");
}

int main(int argc, char **argv)
{
    pmix_proc_t myproc;
    pmix_status_t rc;
    (void) argc;
    (void) argv;

    /* force the singleton path - nothing to connect to, so each API call
     * either resolves locally or is rejected up front */
    unsetenv("PMIX_NAMESPACE");
    unsetenv("PMIX_RANK");
    unsetenv("PMIX_SERVER_URI");
    unsetenv("PMIX_SERVER_URI2");
    unsetenv("PMIX_SERVER_URI3");
    unsetenv("PMIX_SERVER_URI21");
    unsetenv("PMIX_SERVER_URI41");
    unsetenv("PMIX_SERVER_URI51");

    fprintf(stdout, "\n=== src/common API regression test ===\n");

    /* a singleton reports PMIX_ERR_UNREACH from init - it is fully
     * initialized, it just has no server */
    rc = PMIx_Init(&myproc, NULL, 0);
    if (PMIX_SUCCESS != rc && PMIX_ERR_UNREACH != rc) {
        fprintf(stderr, "PMIx_Init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    test_log_bad_source();
    test_query_bad_params();
    test_query_local();
    test_data_bad_params();
    test_directives_string();
    test_iof_bad_params();
    test_iof_flags();
    test_iof_xml_escaping();
    test_register_attributes();

    rc = PMIx_Finalize(NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_Finalize failed: %s\n", PMIx_Error_string(rc));
        nfail++;
    }

    if (0 == nfail) {
        fprintf(stdout, "\nsrc/common API regression test: PASS\n\n");
    } else {
        fprintf(stdout, "\nsrc/common API regression test: FAIL (%d)\n\n", nfail);
    }
    return (0 == nfail) ? 0 : 1;
}
