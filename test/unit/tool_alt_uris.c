/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * A tool reaches a server at one of its alternate addresses when the
 * address in its URI cannot be reached.
 *
 * A server accepting remote tool connections on a multi-homed host listens
 * on every public interface, but a URI carries only one address - every
 * released parser refuses a URI with more than one, and a client refused
 * that way quietly runs as a singleton. So the server lists its other
 * addresses where no older reader looks: a tagged line after the five a
 * rendezvous file has always held, and the PMIX_SERVER_ALT_URIS directive.
 * A tool that cannot reach the URI's address tries each of those in turn.
 *
 * A single host has no second network to be unreachable from, so each case
 * stands a dead address - a port just released on the server's own
 * loopback, which refuses at once - in for the one the tool cannot reach,
 * and names the server's real address as the alternate. Each case is a
 * tool in its own child, forked before any PMIx call, with an alarm armed.
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "include/pmix_server.h"
#include "include/pmix_tool.h"
#include "src/client/pmix_client_ops.h"
#include "src/include/pmix_globals.h"
#include "src/mca/ptl/base/base.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define SERVER_NSPACE "alt.uri.server"
#define TOOL_NSPACE "alt-uri-tool"

typedef enum {
    CASE_INIT_DIRECTIVE,  // blocking init: URI unreachable, PMIX_SERVER_ALT_URIS names the server
    CASE_ATTACH_FILE,     // attachment file: dead first line, alternates on the tagged line
    CASE_FILE_GARBLED,    // ...with an unparseable alternate ahead of the real one
    CASE_ATTACH_NB,       // the event-driven attach, same directives
    CASE_NO_ALTERNATE,    // an unreachable URI with nothing else still fails
    CASE_BAD_DIRECTIVE,   // an alternate that is not an address is refused up front
    NCASES
} tcase_t;

static const char *case_names[NCASES] = {
    "blocking init falls back to PMIX_SERVER_ALT_URIS",
    "an attachment file's alternates are tried after its URI",
    "an unparseable alternate in a file is passed over",
    "the event-driven attach falls back to an alternate",
    "an unreachable URI with no alternates still fails",
    "a PMIX_SERVER_ALT_URIS that is not an address is refused",
};

static int npass = 0;
static int nfail = 0;

static void report(const char *name, int passed, const char *detail)
{
    if (passed) {
        fprintf(stdout, "  PASS: %s\n", name);
        ++npass;
    } else {
        fprintf(stdout, "  FAIL: %s (%s)\n", name, detail);
        ++nfail;
    }
}

static void tool_connected_fn(pmix_info_t *info, size_t ninfo,
                              pmix_tool_connection_cbfunc_t cbfunc, void *cbdata)
{
    static int next = 0;
    pmix_proc_t proc;
    PMIX_HIDE_UNUSED_PARAMS(info, ninfo);

    PMIX_LOAD_PROCID(&proc, TOOL_NSPACE, next++);
    cbfunc(PMIX_SUCCESS, &proc, cbdata);
}

static pmix_server_module_t mymodule = {
    .tool_connected = tool_connected_fn
};

/* what the server tells each tool: its URI's identity, a dead address in
 * the server's family, its real address, and a file to attach thru */
typedef struct {
    char ident[256];    // "nspace.rank"
    char dead[128];
    char real[128];
    char file[PMIX_PATH_MAX + 32];
} contact_t;

static bool recorded_server(void)
{
    return NULL != pmix_client_globals.myserver &&
           NULL != pmix_client_globals.myserver->info &&
           NULL != pmix_client_globals.myserver->info->pname.nspace &&
           0 == strcmp(pmix_client_globals.myserver->info->pname.nspace, SERVER_NSPACE);
}

/* the tool half - runs in a child that has made no PMIx call */
static int run_tool(tcase_t tc, int fd)
{
    contact_t ct;
    pmix_proc_t myproc, server;
    pmix_info_t info[2];
    char uri[512], alts[512];
    pmix_status_t rc;
    size_t ninfo = 0;
    bool flag = true;
    int result = 0;

    alarm(60);
    if (sizeof(ct) != read(fd, &ct, sizeof(ct))) {
        return 10;
    }
    snprintf(uri, sizeof(uri), "%s;%s", ct.ident, ct.dead);

    switch (tc) {
    case CASE_INIT_DIRECTIVE:
    case CASE_NO_ALTERNATE:
    case CASE_BAD_DIRECTIVE:
        if (CASE_BAD_DIRECTIVE == tc) {
            /* the URI is fine here: only the alternate is wrong */
            snprintf(uri, sizeof(uri), "%s;%s", ct.ident, ct.real);
        }
        PMIX_INFO_LOAD(&info[ninfo], PMIX_SERVER_URI, uri, PMIX_STRING);
        ++ninfo;
        if (CASE_NO_ALTERNATE != tc) {
            snprintf(alts, sizeof(alts), "%s",
                     (CASE_BAD_DIRECTIVE == tc) ? "not-an-address" : ct.real);
            PMIX_INFO_LOAD(&info[ninfo], PMIX_SERVER_ALT_URIS, alts, PMIX_STRING);
            ++ninfo;
        }
        rc = PMIx_tool_init(&myproc, info, ninfo);
        if (CASE_INIT_DIRECTIVE == tc) {
            result = (PMIX_SUCCESS == rc && recorded_server()) ? 0 : 1;
        } else if (CASE_NO_ALTERNATE == tc) {
            result = (PMIX_SUCCESS != rc) ? 0 : 1;
        } else {
            result = (PMIX_ERR_BAD_PARAM == rc) ? 0 : 1;
        }
        if (0 != result) {
            fprintf(stderr, "  tool[%d]: PMIx_tool_init returned %s\n", (int) tc,
                    PMIx_Error_string(rc));
        }
        if (PMIX_SUCCESS == rc) {
            PMIx_tool_finalize();
        }
        break;

    case CASE_ATTACH_FILE:
    case CASE_FILE_GARBLED:
        PMIX_INFO_LOAD(&info[0], PMIX_TOOL_ATTACHMENT_FILE, ct.file, PMIX_STRING);
        rc = PMIx_tool_init(&myproc, info, 1);
        result = (PMIX_SUCCESS == rc && recorded_server()) ? 0 : 1;
        if (0 != result) {
            fprintf(stderr, "  tool[%d]: PMIx_tool_init returned %s\n", (int) tc,
                    PMIx_Error_string(rc));
        }
        if (PMIX_SUCCESS == rc) {
            PMIx_tool_finalize();
        }
        break;

    case CASE_ATTACH_NB:
        PMIX_INFO_LOAD(&info[0], PMIX_TOOL_DO_NOT_CONNECT, &flag, PMIX_BOOL);
        rc = PMIx_tool_init(&myproc, info, 1);
        if (PMIX_SUCCESS != rc) {
            fprintf(stderr, "  tool[%d]: PMIx_tool_init returned %s\n", (int) tc,
                    PMIx_Error_string(rc));
            return 1;
        }
        PMIX_INFO_LOAD(&info[0], PMIX_SERVER_URI, uri, PMIX_STRING);
        PMIX_INFO_LOAD(&info[1], PMIX_SERVER_ALT_URIS, ct.real, PMIX_STRING);
        rc = PMIx_tool_attach_to_server(NULL, &server, info, 2);
        result = (PMIX_SUCCESS == rc && 0 == strcmp(server.nspace, SERVER_NSPACE)) ? 0 : 1;
        if (0 != result) {
            fprintf(stderr, "  tool[%d]: PMIx_tool_attach_to_server returned %s\n", (int) tc,
                    PMIx_Error_string(rc));
        }
        PMIx_tool_finalize();
        break;

    default:
        return 10;
    }
    return result;
}

/* a port on this address that nothing is listening on - bound and released,
 * so a connect to it is refused at once */
static bool dead_address(const char *real, char *out, size_t len)
{
    struct sockaddr_storage ss;
    pmix_socklen_t sslen;
    size_t alen;
    char host[INET6_ADDRSTRLEN];
    int sd;
    bool ok = false;

    if (PMIX_SUCCESS != pmix_ptl_base_setup_connection((char *) real, &ss, &alen)) {
        return false;
    }
    sslen = (pmix_socklen_t) alen;
    if (AF_INET == ss.ss_family) {
        ((struct sockaddr_in *) &ss)->sin_port = 0;
    } else {
        ((struct sockaddr_in6 *) &ss)->sin6_port = 0;
    }
    sd = socket(ss.ss_family, SOCK_STREAM, 0);
    if (0 > sd) {
        return false;
    }
    if (0 == bind(sd, (struct sockaddr *) &ss, sslen) &&
        0 == getsockname(sd, (struct sockaddr *) &ss, &sslen)) {
        if (AF_INET == ss.ss_family) {
            inet_ntop(AF_INET, &((struct sockaddr_in *) &ss)->sin_addr, host, sizeof(host));
            snprintf(out, len, "tcp4://%s:%u", host,
                     (unsigned) ntohs(((struct sockaddr_in *) &ss)->sin_port));
        } else {
            inet_ntop(AF_INET6, &((struct sockaddr_in6 *) &ss)->sin6_addr, host, sizeof(host));
            snprintf(out, len, "tcp6://%s:%u", host,
                     (unsigned) ntohs(((struct sockaddr_in6 *) &ss)->sin6_port));
        }
        ok = true;
    }
    close(sd);
    return ok;
}

/* a contact file shaped as a server writes one: the five lines every release
 * reads by position, then the tagged alternates */
static bool write_contact_file(const char *path, const contact_t *ct, const char *alts)
{
    FILE *fp;

    if (NULL == (fp = fopen(path, "w"))) {
        return false;
    }
    fprintf(fp, "%s;%s\n%s\n%lu\n%lu:%lu\n%s\n\n%s%s\n", ct->ident, ct->dead, PMIX_VERSION,
            (unsigned long) getpid(), (unsigned long) getuid(), (unsigned long) getgid(),
            "Thu Jan  1 00:00:00 1970", PMIX_PTL_ALT_URIS_TAG, alts);
    fclose(fp);
    return true;
}

int main(int argc, char **argv)
{
    int pipes[NCASES][2];
    pid_t child[NCASES];
    contact_t ct;
    pmix_info_t sinfo[2];
    pmix_status_t rc;
    const char *base, *p;
    char dir[PMIX_PATH_MAX], path[PMIX_PATH_MAX + 32], garbled[512];
    int status, i;
    bool flag = true;
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    fprintf(stdout, "\n=== tool alternate server address unit test ===\n\n");

    base = getenv("TMPDIR");
    snprintf(dir, sizeof(dir), "%s/pmix-alturi-XXXXXX", (NULL == base) ? "/tmp" : base);
    if (NULL == mkdtemp(dir)) {
        fprintf(stderr, "could not create a temporary directory\n");
        return 1;
    }
    setenv("PMIX_SYSTEM_TMPDIR", dir, 1);
    setenv("PMIX_SERVER_TMPDIR", dir, 1);

    /* fork before touching PMIx so no tool inherits an initialized library */
    for (i = 0; i < NCASES; i++) {
        if (0 != pipe(pipes[i])) {
            fprintf(stderr, "pipe() failed\n");
            return 1;
        }
        child[i] = fork();
        if (0 > child[i]) {
            fprintf(stderr, "fork() failed\n");
            return 1;
        }
        if (0 == child[i]) {
            close(pipes[i][1]);
            _exit(run_tool((tcase_t) i, pipes[i][0]));
        }
        close(pipes[i][0]);
    }

    PMIX_INFO_LOAD(&sinfo[0], PMIX_SERVER_TOOL_SUPPORT, &flag, PMIX_BOOL);
    PMIX_INFO_LOAD(&sinfo[1], PMIX_SERVER_NSPACE, SERVER_NSPACE, PMIX_STRING);
    rc = PMIx_server_init(&mymodule, sinfo, 2);
    PMIX_INFO_DESTRUCT(&sinfo[0]);
    PMIX_INFO_DESTRUCT(&sinfo[1]);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    memset(&ct, 0, sizeof(ct));
    p = (NULL == pmix_ptl_base.listener.uri) ? NULL : strchr(pmix_ptl_base.listener.uri, ';');
    if (NULL == p) {
        report("server published a URI", 0, "no listener URI");
        return 1;
    }
    snprintf(ct.ident, sizeof(ct.ident), "%.*s", (int) (p - pmix_ptl_base.listener.uri),
             pmix_ptl_base.listener.uri);
    snprintf(ct.real, sizeof(ct.real), "%s", p + 1);
    if (!dead_address(ct.real, ct.dead, sizeof(ct.dead))) {
        report("found an address nothing listens on", 0, "could not bind one");
        return 1;
    }

    for (i = 0; i < NCASES; i++) {
        contact_t mine = ct;

        if (CASE_ATTACH_FILE == i || CASE_FILE_GARBLED == i) {
            snprintf(path, sizeof(path), "%s/contact.%d", dir, i);
            snprintf(garbled, sizeof(garbled), "%s%s",
                     (CASE_FILE_GARBLED == i) ? "tcp4://no-such-host:1," : "", ct.real);
            if (!write_contact_file(path, &mine, garbled)) {
                report(case_names[i], 0, "could not write the contact file");
            }
            snprintf(mine.file, sizeof(mine.file), "%s", path);
        }
        if (sizeof(mine) != write(pipes[i][1], &mine, sizeof(mine))) {
            report(case_names[i], 0, "could not hand the tool its contact info");
        }
        close(pipes[i][1]);
    }

    for (i = 0; i < NCASES; i++) {
        if (child[i] != waitpid(child[i], &status, 0)) {
            report(case_names[i], 0, "waitpid failed");
        } else if (WIFSIGNALED(status)) {
            report(case_names[i], 0, "tool died on a signal (hung and alarmed, or crashed)");
        } else {
            report(case_names[i], 0 == WEXITSTATUS(status), "tool reported failure");
        }
    }

    PMIx_server_finalize();
    for (i = 0; i < NCASES; i++) {
        snprintf(path, sizeof(path), "%s/contact.%d", dir, i);
        unlink(path);
    }
    rmdir(dir);

    fprintf(stdout, "\n%d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
