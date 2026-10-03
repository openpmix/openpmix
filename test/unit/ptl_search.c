/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit tests for the rendezvous-file directory walks and the fixed
 * rendezvous names.
 *
 * A tool that is not told where its server is searches for one: it walks
 * the system tmpdir for "pmix.*" contact files, and so does a
 * PMIX_QUERY_AVAIL_SERVERS query. Both walks:
 *
 *  - keep an entry only if it is a regular file, so a FIFO named like a
 *    contact file is skipped rather than opened with fopen();
 *  - never follow a symbolic link to a directory, so a link back up the
 *    tree ("ln -s . a") is not descended into;
 *  - skip a contact file that cannot be read or parsed and carry on, so a
 *    valid file readdir() lists after it is still found.
 *
 * The directory built here holds all three next to one valid contact
 * file, and both walks must come back promptly having found that one
 * file. A watchdog turns a hang into a failure rather than a stalled
 * "make check". There are thirty unreadable files so that a readdir()
 * order listing the valid file first is unlikely.
 *
 * A search for one particular server - by pid or by namespace - matches
 * the file's whole name, so asking for "tool.12" does not find "tool.123".
 *
 * The fixed rendezvous names (pmix.sys.<host> and friends) are read with
 * pmix_ptl_base_parse_rndz_file(), which applies the same regular-file
 * rule: a FIFO, or a link to a character device, is refused promptly.
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "include/pmix_server.h"
#include "src/include/pmix_globals.h"
#include "src/mca/bfrops/base/base.h"
#include "src/mca/psec/base/base.h"
#include "src/mca/ptl/base/base.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define NJUNK 30
#define WATCHDOG_SECS 60

static int npass = 0;
static int nfail = 0;
static char searchdir[PMIX_PATH_MAX];
static char sessdir[PMIX_PATH_MAX];

static pmix_server_module_t mymodule = {0};

static void report(const char *name, int passed, const char *detail)
{
    if (passed) {
        fprintf(stdout, "  PASS: %s\n", name);
        ++npass;
    } else {
        fprintf(stdout, "  FAIL: %s (%s)\n", name, detail);
        ++nfail;
    }
    fflush(stdout);
}

static void watchdog(int sig)
{
    static const char msg[] = "  FAIL: a walk or rendezvous read did not return - hung\n";
    PMIX_HIDE_UNUSED_PARAMS(sig);
    /* async-signal-safe only */
    (void) !write(STDOUT_FILENO, msg, sizeof(msg) - 1);
    _exit(1);
}

static int write_file(const char *name, const char *content)
{
    char path[PMIX_PATH_MAX + 128];
    FILE *fp;

    snprintf(path, sizeof(path), "%s/%s", searchdir, name);
    fp = fopen(path, "w");
    if (NULL == fp) {
        return -1;
    }
    fputs(content, fp);
    fclose(fp);
    return 0;
}

static int build_searchdir(void)
{
    char path[PMIX_PATH_MAX + 128], name[64];
    int n;

    /* a FIFO nobody will ever write to, named like a contact file */
    snprintf(path, sizeof(path), "%s/pmix.test.fifo", searchdir);
    if (0 != mkfifo(path, 0600)) {
        return -1;
    }
    /* two links back to the directory itself */
    snprintf(path, sizeof(path), "%s/loopA", searchdir);
    if (0 != symlink(".", path)) {
        return -1;
    }
    snprintf(path, sizeof(path), "%s/loopB", searchdir);
    if (0 != symlink(".", path)) {
        return -1;
    }
    /* contact files that cannot be parsed: an empty one, as a server
     * killed before writing leaves, and ones holding no URI */
    for (n = 0; n < NJUNK; n++) {
        snprintf(name, sizeof(name), "pmix.test.junk%02d", n);
        if (0 != write_file(name, (0 == n) ? "" : "not a uri\n")) {
            return -1;
        }
    }
    /* and the one a search should find */
    if (0 != write_file("pmix.test.good", "goodns.0;tcp4://127.0.0.1:4242\n4.2.0\n")) {
        return -1;
    }
    return 0;
}

static void test_df_search(void)
{
    pmix_list_t connections;
    pmix_connection_t *cn;
    pmix_status_t rc;

    PMIX_CONSTRUCT(&connections, pmix_list_t);
    rc = pmix_ptl_base_df_search(searchdir, "pmix.test.", false, NULL, 0, true, &connections);
    report("df_search: returns success past a FIFO, link loops and unreadable files",
           PMIX_SUCCESS == rc, PMIx_Error_string(rc));
    report("df_search: finds exactly the one valid contact file",
           1 == pmix_list_get_size(&connections), "wrong number of connections");
    if (1 == pmix_list_get_size(&connections)) {
        cn = (pmix_connection_t *) pmix_list_get_first(&connections);
        report("df_search: the connection found is the valid one",
               NULL != cn->nspace && 0 == strcmp(cn->nspace, "goodns"),
               (NULL == cn->nspace) ? "NULL" : cn->nspace);
    }
    PMIX_LIST_DESTRUCT(&connections);
}

/* A search for a particular server takes only a file of exactly that
 * name: one whose name merely begins with it belongs to another server. */
static void test_df_search_exact(void)
{
    char dir[PMIX_PATH_MAX + 16], path[PMIX_PATH_MAX + 160];
    pmix_list_t connections;
    pmix_connection_t *cn;
    pmix_status_t rc;
    FILE *fp;

    snprintf(dir, sizeof(dir), "%s.exact", searchdir);
    if (0 != mkdir(dir, 0700)) {
        report("df_search exact: setup", 0, "mkdir failed");
        return;
    }
    snprintf(path, sizeof(path), "%s/pmix.x.tool.123", dir);
    if (NULL != (fp = fopen(path, "w"))) {
        fputs("otherns.0;tcp4://127.0.0.1:4243\n4.2.0\n", fp);
        fclose(fp);
    }
    PMIX_CONSTRUCT(&connections, pmix_list_t);
    rc = pmix_ptl_base_df_search(dir, "pmix.x.tool.12", true, NULL, 0, true, &connections);
    report("df_search exact: a longer name is not taken for the one asked for",
           PMIX_SUCCESS != rc && 0 == pmix_list_get_size(&connections),
           "matched by prefix");
    PMIX_LIST_DESTRUCT(&connections);

    snprintf(path, sizeof(path), "%s/pmix.x.tool.12", dir);
    if (NULL != (fp = fopen(path, "w"))) {
        fputs("wantns.0;tcp4://127.0.0.1:4244\n4.2.0\n", fp);
        fclose(fp);
    }
    PMIX_CONSTRUCT(&connections, pmix_list_t);
    rc = pmix_ptl_base_df_search(dir, "pmix.x.tool.12", true, NULL, 0, true, &connections);
    cn = (1 == pmix_list_get_size(&connections))
             ? (pmix_connection_t *) pmix_list_get_first(&connections) : NULL;
    report("df_search exact: the file of exactly that name is found",
           PMIX_SUCCESS == rc && NULL != cn && NULL != cn->nspace &&
               0 == strcmp(cn->nspace, "wantns"),
           "wrong or missing connection");
    PMIX_LIST_DESTRUCT(&connections);

    unlink(path);
    snprintf(path, sizeof(path), "%s/pmix.x.tool.123", dir);
    unlink(path);
    rmdir(dir);
}

static void test_rndz_file(void)
{
    char path[PMIX_PATH_MAX + 128];
    pmix_list_t connections;
    pmix_status_t rc;

    /* a FIFO at a rendezvous name is refused, not waited on */
    snprintf(path, sizeof(path), "%s/pmix.test.fifo", searchdir);
    PMIX_CONSTRUCT(&connections, pmix_list_t);
    rc = pmix_ptl_base_parse_rndz_file(path, true, &connections);
    report("rndz file: a FIFO is refused", PMIX_SUCCESS != rc, "accepted");
    PMIX_LIST_DESTRUCT(&connections);

    /* so is a link to a device that never reaches end of file */
    snprintf(path, sizeof(path), "%s/pmix.test.zero", searchdir);
    if (0 == symlink("/dev/zero", path)) {
        PMIX_CONSTRUCT(&connections, pmix_list_t);
        rc = pmix_ptl_base_parse_rndz_file(path, true, &connections);
        report("rndz file: a link to /dev/zero is refused", PMIX_SUCCESS != rc, "accepted");
        PMIX_LIST_DESTRUCT(&connections);
    }

    /* and a regular file is read as before */
    snprintf(path, sizeof(path), "%s/pmix.test.good", searchdir);
    PMIX_CONSTRUCT(&connections, pmix_list_t);
    rc = pmix_ptl_base_parse_rndz_file(path, true, &connections);
    report("rndz file: a regular contact file is read",
           PMIX_SUCCESS == rc && 1 == pmix_list_get_size(&connections),
           PMIx_Error_string(rc));
    PMIX_LIST_DESTRUCT(&connections);
}

/* A server's file names the wire formats and security mechanisms it
 * accepts; a process that finds it there takes the highest of its own the
 * server lists, refuses to try when there is none, and - for a server too
 * old to list them - goes by the server's version */
static void test_compat(void)
{
    char path[PMIX_PATH_MAX + 128];
    pmix_list_t connections;
    pmix_connection_t *cn, scratch;
    pmix_peer_t *peer;
    pmix_psec_module_t *native;
    pmix_status_t rc;

    snprintf(path, sizeof(path), "%s/pmix.test.compat", searchdir);
    write_file("pmix.test.compat", "pmix-server.1;tcp4://127.0.0.1:1\n7.0.0\n1\n0:0\nnow\n"
                     "bfrops:v41,v21\npsec:native\n");
    PMIX_CONSTRUCT(&connections, pmix_list_t);
    rc = pmix_ptl_base_parse_rndz_file(path, true, &connections);
    cn = (pmix_connection_t *) pmix_list_get_first(&connections);
    report("compat: the server's wire formats and mechanisms are read",
           PMIX_SUCCESS == rc && 1 == pmix_list_get_size(&connections) &&
               NULL != cn->bfrops && 0 == strcmp(cn->bfrops, "v41,v21") &&
               NULL != cn->psec && 0 == strcmp(cn->psec, "native"),
           PMIx_Error_string(rc));
    PMIX_LIST_DESTRUCT(&connections);
    unlink(path);

    snprintf(path, sizeof(path), "%s/pmix.test.old", searchdir);
    write_file("pmix.test.old", "pmix-server.1;tcp4://127.0.0.1:1\n5.0.9\n1\n0:0\nnow\n");
    PMIX_CONSTRUCT(&connections, pmix_list_t);
    rc = pmix_ptl_base_parse_rndz_file(path, true, &connections);
    cn = (pmix_connection_t *) pmix_list_get_first(&connections);
    report("compat: an older server's file has none to read",
           PMIX_SUCCESS == rc && NULL == cn->bfrops && NULL == cn->psec, PMIx_Error_string(rc));
    PMIX_LIST_DESTRUCT(&connections);
    unlink(path);

    /* a server peer that is not our primary - our own modules stay put */
    native = pmix_psec_base_assign_module("native");
    peer = PMIX_NEW(pmix_peer_t);
    peer->nptr = PMIX_NEW(pmix_namespace_t);
    PMIX_CONSTRUCT(&scratch, pmix_connection_t);
    scratch.uri = strdup("tcp4://127.0.0.1:1");

#define RESET()                                                                 \
    do {                                                                        \
        peer->nptr->compat.bfrops = pmix_bfrops_base_assign_module(NULL);      \
        peer->nptr->compat.psec = native;                                      \
        free(scratch.bfrops);                                                   \
        scratch.bfrops = NULL;                                                  \
        free(scratch.psec);                                                     \
        scratch.psec = NULL;                                                    \
        PMIX_SET_PEER_VERSION(peer, "7.0.0", 2, 0);                             \
    } while (0)

    RESET();
    scratch.bfrops = strdup("v99,v41,v21");
    rc = pmix_ptl_base_select_compat((struct pmix_peer_t *) peer, &scratch);
    report("compat: the highest wire format both have is taken",
           PMIX_SUCCESS == rc && 0 == strcmp(peer->nptr->compat.bfrops->version, "v41"),
           PMIx_Error_string(rc));
    report("compat: a server that is not the primary leaves our own format alone",
           pmix_globals.mypeer->nptr->compat.bfrops != peer->nptr->compat.bfrops, "changed");

    RESET();
    scratch.bfrops = strdup("v98,v99");
    rc = pmix_ptl_base_select_compat((struct pmix_peer_t *) peer, &scratch);
    report("compat: no wire format in common - the connection is not attempted",
           PMIX_ERR_NOT_SUPPORTED == rc, PMIx_Error_string(rc));

    RESET();
    PMIX_SET_PEER_VERSION(peer, "5.0.9", 2, 0);
    rc = pmix_ptl_base_select_compat((struct pmix_peer_t *) peer, &scratch);
    report("compat: an older server that lists none is matched by its version",
           PMIX_SUCCESS == rc && 0 == strcmp(peer->nptr->compat.bfrops->version, "v41"),
           PMIx_Error_string(rc));

    RESET();
    rc = pmix_ptl_base_select_compat((struct pmix_peer_t *) peer, &scratch);
    report("compat: a server as new as us that lists none keeps our newest",
           PMIX_SUCCESS == rc &&
               peer->nptr->compat.bfrops == pmix_bfrops_base_assign_module(NULL),
           PMIx_Error_string(rc));

    RESET();
    scratch.psec = strdup("no-such-mechanism,native");
    rc = pmix_ptl_base_select_compat((struct pmix_peer_t *) peer, &scratch);
    report("compat: our security mechanism is kept when the server lists it",
           PMIX_SUCCESS == rc && native == peer->nptr->compat.psec, PMIx_Error_string(rc));

    /* an older server lists no mechanism: munge if we have it, else
     * native - whatever we would have picked for ourselves */
    RESET();
    peer->nptr->compat.psec = NULL;
    rc = pmix_ptl_base_select_compat((struct pmix_peer_t *) peer, &scratch);
    report("compat: an older server that lists no mechanism gets munge or native",
           PMIX_SUCCESS == rc && NULL != peer->nptr->compat.psec &&
               (0 == strcmp(peer->nptr->compat.psec->name, "munge") ||
                0 == strcmp(peer->nptr->compat.psec->name, "native")) &&
               peer->nptr->compat.psec == pmix_psec_base_assign_module("munge,native"),
           PMIx_Error_string(rc));

    RESET();
    scratch.psec = strdup("no-such-mechanism");
    rc = pmix_ptl_base_select_compat((struct pmix_peer_t *) peer, &scratch);
    report("compat: no security mechanism in common - the connection is not attempted",
           PMIX_ERR_NOT_SUPPORTED == rc, PMIx_Error_string(rc));
#undef RESET

    PMIX_DESTRUCT(&scratch);
    peer->nptr->compat.psec = NULL;
    PMIX_RELEASE(peer);
}

static void test_query_servers(void)
{
    pmix_query_t query;
    pmix_info_t *results = NULL, *sdata;
    size_t nresults = 0, n, m, nservers = 0, ndata;
    pmix_status_t rc;
    bool found = false;

    PMIX_QUERY_CONSTRUCT(&query);
    PMIx_Argv_append_nosize(&query.keys, PMIX_QUERY_AVAIL_SERVERS);
    rc = PMIx_Query_info(&query, 1, &results, &nresults);
    report("query avail servers: returns success past a FIFO and link loops",
           PMIX_SUCCESS == rc, PMIx_Error_string(rc));
    for (n = 0; n < nresults; n++) {
        if (!PMIX_CHECK_KEY(&results[n], PMIX_SERVER_INFO_ARRAY) ||
            PMIX_DATA_ARRAY != results[n].value.type) {
            continue;
        }
        ++nservers;
        sdata = (pmix_info_t *) results[n].value.data.darray->array;
        ndata = results[n].value.data.darray->size;
        for (m = 0; m < ndata; m++) {
            if (PMIX_CHECK_KEY(&sdata[m], PMIX_SERVER_NSPACE) &&
                0 == strcmp(sdata[m].value.data.string, "goodns")) {
                found = true;
            }
        }
    }
    report("query avail servers: reports exactly one server", 1 == nservers,
           "wrong number of servers");
    report("query avail servers: the server reported is the valid one", found, "not found");
    if (NULL != results) {
        PMIX_INFO_FREE(results, nresults);
    }
    PMIX_QUERY_DESTRUCT(&query);
}

static void cleanup(void)
{
    char cmd[3 * PMIX_PATH_MAX];

    /* the tree holds links and a FIFO - let rm deal with them */
    snprintf(cmd, sizeof(cmd), "rm -rf '%s' '%s'", searchdir, sessdir);
    if (0 != system(cmd)) {
        fprintf(stderr, "could not remove %s\n", searchdir);
    }
}

int main(int argc, char **argv)
{
    pmix_info_t info[2];
    pmix_status_t rc;
    const char *tdir;
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    tdir = getenv("TMPDIR");
    if (NULL == tdir) {
        tdir = "/tmp";
    }
    snprintf(searchdir, sizeof(searchdir), "%s/ptlsearch.XXXXXX", tdir);
    snprintf(sessdir, sizeof(sessdir), "%s/ptlsess.XXXXXX", tdir);
    if (NULL == mkdtemp(searchdir) || NULL == mkdtemp(sessdir)) {
        fprintf(stderr, "mkdtemp failed\n");
        return 1;
    }
    if (0 != build_searchdir()) {
        fprintf(stderr, "could not build the search directory\n");
        cleanup();
        return 1;
    }

    /* the search directory is the server's system tmpdir, which is what
     * PMIX_QUERY_AVAIL_SERVERS walks; keep the server's own session files
     * out of it */
    PMIX_INFO_LOAD(&info[0], PMIX_SYSTEM_TMPDIR, searchdir, PMIX_STRING);
    PMIX_INFO_LOAD(&info[1], PMIX_SERVER_TMPDIR, sessdir, PMIX_STRING);
    rc = PMIx_server_init(&mymodule, info, 2);
    PMIX_INFO_DESTRUCT(&info[0]);
    PMIX_INFO_DESTRUCT(&info[1]);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        cleanup();
        return 1;
    }

    fprintf(stdout, "\n=== ptl rendezvous directory walk unit tests ===\n\n");

    signal(SIGALRM, watchdog);
    alarm(WATCHDOG_SECS);
    test_df_search();
    test_df_search_exact();
    test_query_servers();
    test_rndz_file();
    test_compat();
    alarm(0);

    fprintf(stdout, "\n%d passed, %d failed\n", npass, nfail);

    PMIx_server_finalize();
    cleanup();

    return (0 == nfail) ? 0 : 1;
}
