/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit tests for the rendezvous-file directory walks.
 *
 * A tool that is not told where its server is searches for one: it walks
 * the system tmpdir for "pmix.*" contact files, and so does a
 * PMIX_QUERY_AVAIL_SERVERS query. That directory defaults to $TMPDIR or
 * /tmp - somewhere any local user can write - and both walks used to
 * trust what they found there:
 *
 *  - a FIFO named like a contact file was opened with fopen(), which
 *    blocks until something opens the other end, so the walk never
 *    returned;
 *  - a symbolic link to a directory was descended into, so a link back
 *    up the tree ("ln -s . a") made the walk revisit everything beneath
 *    it at every level until the path length ran out, and two such links
 *    made that exponential;
 *  - the first contact file that could not be read or parsed ended the
 *    search with an error, hiding any valid file readdir() listed after
 *    it. A server killed partway thru writing its file leaves exactly
 *    that behind.
 *
 * The directory built here holds all three next to one valid contact
 * file, and both walks must come back promptly having found that one
 * file. A watchdog turns a hang into a failure rather than a stalled
 * "make check". There are thirty unreadable files so that a readdir()
 * order listing the valid file first - which would let the old abort
 * pass unnoticed - is unlikely.
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "include/pmix_server.h"
#include "src/include/pmix_globals.h"
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
    static const char msg[] = "  FAIL: directory walk did not return - hung\n";
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
    rc = pmix_ptl_base_df_search(searchdir, "pmix.test.", NULL, 0, true, &connections);
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
    test_query_servers();
    alarm(0);

    fprintf(stdout, "\n%d passed, %d failed\n", npass, nfail);

    PMIx_server_finalize();
    cleanup();

    return (0 == nfail) ? 0 : 1;
}
