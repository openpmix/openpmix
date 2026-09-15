/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit tests for the ptl listener: what it does with the directives a host
 * hands PMIx_server_init, and how it stops when accept() fails for good.
 *
 *  - accept() failing with EMFILE closed the listening descriptor and left
 *    the rest standing: the event registered, the listener marked active,
 *    and pmix_ptl_base.listener.socket naming a number the kernel was free
 *    to reuse for a client socket or a pipe - which finalize then closed.
 *
 *  - PMIX_TCP_IF_INCLUDE and its string siblings were read out of the
 *    value union as a pointer whatever type the value carried.
 *
 *  - PMIX_TCP_IPV4_PORT given as a PMIX_INT larger than a port was
 *    narrowed to 16 bits and bound: 70000 listened on 4464.
 *
 *  - an empty PMIX_TCP_REPORT_URI was read as pipe descriptor 0, so the
 *    listener wrote its URI to stdin and closed it.
 *
 *  - a PMIX_TCP_REPORT_URI naming a file was written but not removed at
 *    finalize, because the name finalize removes was taken from the MCA
 *    parameter before the directive could replace it.
 *
 *  - with remote connections accepted, the listener bound only the first
 *    public interface, so a remote tool on any other network could not
 *    reach the server at all. It now listens on each of them and records
 *    the others after everything a released reader takes from the
 *    rendezvous and report files, where no older reader looks.
 *
 * Each case runs in a forked child: PMIx_server_init can only be called
 * once in a process, and against the unfixed library several of these die
 * on a signal rather than fail.
 */

#include "src/include/pmix_config.h"

#include "include/pmix_server.h"
#include "src/include/pmix_globals.h"
#include "src/mca/ptl/base/base.h"
#include "src/threads/pmix_threads.h"
#include "src/util/pmix_if.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHILD_PASS 0
#define CHILD_FAIL 1
#define CHILD_SKIP 77

static int npass = 0;
static int nfail = 0;
static int nskip = 0;
static char tmpdir[PMIX_PATH_MAX];
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
}

static void pause_ms(long ms)
{
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

/* run fn in a child and report its outcome under name */
static void run_case(const char *name, void (*fn)(void))
{
    pid_t pid;
    int status;
    char detail[64];

    fflush(stdout);
    fflush(stderr);
    pid = fork();
    if (0 > pid) {
        report(name, 0, "fork failed");
        return;
    }
    if (0 == pid) {
        alarm(30);
        fn();
        _exit(CHILD_FAIL);
    }
    if (pid != waitpid(pid, &status, 0)) {
        report(name, 0, "waitpid failed");
    } else if (WIFSIGNALED(status)) {
        snprintf(detail, sizeof(detail), "child died on signal %d", WTERMSIG(status));
        report(name, 0, detail);
    } else if (CHILD_SKIP == WEXITSTATUS(status)) {
        fprintf(stdout, "  SKIP: %s\n", name);
        ++nskip;
    } else {
        report(name, CHILD_PASS == WEXITSTATUS(status), "child reported failure");
    }
}

/* ---- accept() out of descriptors ---------------------------------- */

typedef struct {
    pmix_object_t super;
    pmix_event_t ev;
    pmix_lock_t lock;
    bool active;
    int socket;
} lstate_t;
static PMIX_CLASS_INSTANCE(lstate_t, pmix_object_t, NULL, NULL);

static void read_listener(int sd, short args, void *cbdata)
{
    lstate_t *ls = (lstate_t *) cbdata;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);
    ls->active = pmix_ptl_base.listener.active;
    ls->socket = pmix_ptl_base.listener.socket;
    PMIX_WAKEUP_THREAD(&ls->lock);
}

static void emfile_child(void)
{
    struct rlimit rl, saved;
    struct sockaddr_storage addr;
    lstate_t *ls;
    int c, probe;
    bool ok;

    if (PMIX_SUCCESS != PMIx_server_init(&mymodule, NULL, 0)) {
        _exit(CHILD_FAIL);
    }
    memcpy(&addr, pmix_ptl_base.connection, sizeof(addr));
    c = socket(addr.ss_family, SOCK_STREAM, 0);
    /* leave no descriptor free for the accept */
    probe = dup(0);
    close(probe);
    getrlimit(RLIMIT_NOFILE, &saved);
    rl = saved;
    rl.rlim_cur = probe;
    if (0 > c || 0 != setrlimit(RLIMIT_NOFILE, &rl)) {
        _exit(CHILD_FAIL);
    }
    if (0 != connect(c, (struct sockaddr *) &addr,
                     (AF_INET == addr.ss_family) ? sizeof(struct sockaddr_in)
                                                 : sizeof(struct sockaddr_in6))) {
        _exit(CHILD_FAIL);
    }
    pause_ms(300);
    setrlimit(RLIMIT_NOFILE, &saved);

    ls = PMIX_NEW(lstate_t);
    PMIX_CONSTRUCT_LOCK(&ls->lock);
    PMIX_THREADSHIFT(ls, read_listener);
    PMIX_WAIT_THREAD(&ls->lock);
    ok = !ls->active && 0 > ls->socket;
    if (!ok) {
        fprintf(stderr, "listener after EMFILE: active %d socket %d\n", (int) ls->active,
                ls->socket);
    }
    /* no finalize: against the unfixed library it closes whatever now
     * holds the stale descriptor number */
    _exit(ok ? CHILD_PASS : CHILD_FAIL);
}

/* ---- directives ---------------------------------------------------- */

static void bool_if_include_child(void)
{
    pmix_info_t info;
    bool yes = true;

    PMIX_INFO_LOAD(&info, PMIX_TCP_IF_INCLUDE, &yes, PMIX_BOOL);
    /* refused, not crashed on */
    _exit(PMIX_SUCCESS != PMIx_server_init(&mymodule, &info, 1) ? CHILD_PASS : CHILD_FAIL);
}

static void big_port_child(void)
{
    pmix_info_t info;
    int port = 70000;

    PMIX_INFO_LOAD(&info, PMIX_TCP_IPV4_PORT, &port, PMIX_INT);
    _exit(PMIX_SUCCESS != PMIx_server_init(&mymodule, &info, 1) ? CHILD_PASS : CHILD_FAIL);
}

static void empty_report_uri_child(void)
{
    pmix_info_t info;
    int fds[2];

    /* give the child a stdin we know is open */
    if (0 != pipe(fds) || 0 > dup2(fds[0], 0)) {
        _exit(CHILD_FAIL);
    }
    PMIX_INFO_LOAD(&info, PMIX_TCP_REPORT_URI, "", PMIX_STRING);
    if (PMIX_SUCCESS != PMIx_server_init(&mymodule, &info, 1)) {
        _exit(CHILD_FAIL);
    }
    _exit(-1 != fcntl(0, F_GETFD) ? CHILD_PASS : CHILD_FAIL);
}

static void report_uri_file_child(void)
{
    pmix_info_t info;
    char path[PMIX_PATH_MAX + 16];
    struct stat st;

    snprintf(path, sizeof(path), "%s/uri.txt", tmpdir);
    unlink(path);
    PMIX_INFO_LOAD(&info, PMIX_TCP_REPORT_URI, path, PMIX_STRING);
    if (PMIX_SUCCESS != PMIx_server_init(&mymodule, &info, 1)) {
        _exit(CHILD_FAIL);
    }
    if (0 != stat(path, &st)) {
        fprintf(stderr, "report file %s was not written\n", path);
        _exit(CHILD_FAIL);
    }
    PMIx_server_finalize();
    if (0 == stat(path, &st)) {
        fprintf(stderr, "report file %s left behind at finalize\n", path);
        unlink(path);
        _exit(CHILD_FAIL);
    }
    _exit(CHILD_PASS);
}

/* ---- listening on every public interface ------------------------- */

/* how many addresses the listener could take when remote connections are
 * accepted: public, in an enabled family, and not a virtual interface */
static int count_public_addresses(void)
{
    struct sockaddr_storage ss;
    char name[32];
    int i, n = 0;

    for (i = pmix_ifbegin(); i >= 0; i = pmix_ifnext(i)) {
        if (PMIX_SUCCESS != pmix_ifindextoaddr(i, (struct sockaddr *) &ss, sizeof(ss)) ||
            pmix_ifisloopback(i)) {
            continue;
        }
        pmix_ifindextoname(i, name, sizeof(name));
        if (0 == strncmp(name, "vir", 3)) {
            continue;
        }
        if (AF_INET == ss.ss_family && !pmix_ptl_base.disable_ipv4_family) {
            ++n;
        }
#if PMIX_ENABLE_IPV6
        if (AF_INET6 == ss.ss_family && !pmix_ptl_base.disable_ipv6_family) {
            ++n;
        }
#endif
    }
    return n;
}

/* does a connect() to this "tcp4://host:port"/"tcp6://host:port" succeed? */
static bool can_connect(const char *uri)
{
    struct sockaddr_storage ss;
    size_t len;
    int sd;
    bool ok;

    if (PMIX_SUCCESS != pmix_ptl_base_setup_connection((char *) uri, &ss, &len)) {
        return false;
    }
    sd = socket(ss.ss_family, SOCK_STREAM, 0);
    if (0 > sd) {
        return false;
    }
    ok = (0 == connect(sd, (struct sockaddr *) &ss, (pmix_socklen_t) len));
    close(sd);
    return ok;
}

/* the line after the first five that carries the given tag, or NULL */
static char *tagged_line(const char *path, const char *tag, int *lineno)
{
    FILE *fp;
    char line[4096], *found = NULL;
    int n = 0;

    *lineno = 0;
    if (NULL == (fp = fopen(path, "r"))) {
        return NULL;
    }
    while (NULL != fgets(line, sizeof(line), fp)) {
        ++n;
        if (0 == strncmp(line, tag, strlen(tag))) {
            line[strcspn(line, "\n")] = '\0';
            found = strdup(line + strlen(tag));
            *lineno = n;
            break;
        }
    }
    fclose(fp);
    return found;
}

static void alternates_child(void)
{
    pmix_info_t info[3];
    pmix_listener_t *alt;
    pmix_value_t *val = NULL;
    char path[PMIX_PATH_MAX + 16], *line;
    int nalt = 0, lineno;
    bool tool = true, remote = true;

    snprintf(path, sizeof(path), "%s/alturi.txt", tmpdir);
    unlink(path);
    PMIX_INFO_LOAD(&info[0], PMIX_SERVER_REMOTE_CONNECTIONS, &remote, PMIX_BOOL);
    PMIX_INFO_LOAD(&info[1], PMIX_SERVER_TOOL_SUPPORT, &tool, PMIX_BOOL);
    PMIX_INFO_LOAD(&info[2], PMIX_TCP_REPORT_URI, path, PMIX_STRING);
    if (PMIX_SUCCESS != PMIx_server_init(&mymodule, info, 3)) {
        /* a host with no public interface cannot accept remote tools */
        _exit(0 == count_public_addresses() ? CHILD_SKIP : CHILD_FAIL);
    }
    if (2 > count_public_addresses()) {
        PMIx_server_finalize();
        _exit(CHILD_SKIP);
    }
    if (NULL == pmix_ptl_base.alt_uris) {
        fprintf(stderr, "%d public addresses but no alternate listeners\n",
                count_public_addresses());
        _exit(CHILD_FAIL);
    }
    PMIX_LIST_FOREACH (alt, &pmix_ptl_base.alt_listeners, pmix_listener_t) {
        ++nalt;
        if (NULL == strstr(pmix_ptl_base.alt_uris, alt->uri)) {
            fprintf(stderr, "alternate %s missing from %s\n", alt->uri, pmix_ptl_base.alt_uris);
            _exit(CHILD_FAIL);
        }
        if (!can_connect(alt->uri)) {
            fprintf(stderr, "alternate %s does not accept connections\n", alt->uri);
            _exit(CHILD_FAIL);
        }
    }
    if (NULL != strstr(pmix_ptl_base.listener.uri, ",")) {
        fprintf(stderr, "primary URI %s carries more than one address\n",
                pmix_ptl_base.listener.uri);
        _exit(CHILD_FAIL);
    }

    /* the report file: URI, version, then the tagged alternates */
    line = tagged_line(path, PMIX_PTL_ALT_URIS_TAG, &lineno);
    if (NULL == line || 0 != strcmp(line, pmix_ptl_base.alt_uris) || 3 > lineno) {
        fprintf(stderr, "report file alternates wrong: %s at line %d\n",
                (NULL == line) ? "none" : line, lineno);
        _exit(CHILD_FAIL);
    }
    free(line);

    /* the rendezvous file: the five lines every release reads, then ours */
    line = tagged_line(pmix_ptl_base.pid_filename, PMIX_PTL_ALT_URIS_TAG, &lineno);
    if (NULL == line || 0 != strcmp(line, pmix_ptl_base.alt_uris) || 6 > lineno) {
        fprintf(stderr, "rendezvous file alternates wrong: %s at line %d\n",
                (NULL == line) ? "none" : line, lineno);
        _exit(CHILD_FAIL);
    }
    free(line);

    /* and the value a host can ask for */
    if (PMIX_SUCCESS != PMIx_Get(&pmix_globals.myid, PMIX_MYSERVER_ALT_URIS, NULL, 0, &val) ||
        PMIX_STRING != val->type || 0 != strcmp(val->data.string, pmix_ptl_base.alt_uris)) {
        fprintf(stderr, "PMIX_MYSERVER_ALT_URIS not stored\n");
        _exit(CHILD_FAIL);
    }
    PMIX_VALUE_RELEASE(val);

    /* finalize closes them: the first is enough to show it */
    line = strdup(pmix_ptl_base.alt_uris);
    if (NULL != line) {
        line[strcspn(line, ",")] = '\0';
    }
    PMIx_server_finalize();
    if (NULL != line && can_connect(line)) {
        fprintf(stderr, "alternate %s still accepting after finalize\n", line);
        _exit(CHILD_FAIL);
    }
    free(line);
    fprintf(stderr, "%d alternate listener(s) checked\n", nalt);
    _exit(CHILD_PASS);
}

static void no_alternates_child(void)
{
    pmix_info_t info;
    char *line;
    int lineno;
    bool tool = true;

    PMIX_INFO_LOAD(&info, PMIX_SERVER_TOOL_SUPPORT, &tool, PMIX_BOOL);
    if (PMIX_SUCCESS != PMIx_server_init(&mymodule, &info, 1)) {
        _exit(CHILD_FAIL);
    }
    if (NULL != pmix_ptl_base.alt_uris ||
        0 != pmix_list_get_size(&pmix_ptl_base.alt_listeners)) {
        fprintf(stderr, "alternates opened without remote connections\n");
        _exit(CHILD_FAIL);
    }
    line = tagged_line(pmix_ptl_base.pid_filename, PMIX_PTL_ALT_URIS_TAG, &lineno);
    if (NULL != line) {
        fprintf(stderr, "rendezvous file carries alternates: %s\n", line);
        _exit(CHILD_FAIL);
    }
    PMIx_server_finalize();
    _exit(CHILD_PASS);
}

int main(int argc, char **argv)
{
    const char *base;
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    base = getenv("TMPDIR");
    if (NULL == base) {
        base = "/tmp";
    }
    snprintf(tmpdir, sizeof(tmpdir), "%s/pmix-lsn-XXXXXX", base);
    if (NULL == mkdtemp(tmpdir)) {
        fprintf(stderr, "could not create a temporary directory\n");
        return 1;
    }
    setenv("PMIX_SYSTEM_TMPDIR", tmpdir, 1);
    setenv("PMIX_SERVER_TMPDIR", tmpdir, 1);

    fprintf(stdout, "\n=== ptl listener unit tests ===\n\n");

    run_case("accept out of descriptors stops the listener cleanly", emfile_child);
    run_case("a non-string PMIX_TCP_IF_INCLUDE is refused", bool_if_include_child);
    run_case("a PMIX_TCP_IPV4_PORT past 65535 is refused", big_port_child);
    run_case("an empty PMIX_TCP_REPORT_URI leaves stdin alone", empty_report_uri_child);
    run_case("a report file named by directive is removed at finalize", report_uri_file_child);
    run_case("remote connections listen on, and advertise, every public interface",
             alternates_child);
    run_case("without remote connections there are no alternates", no_alternates_child);

    rmdir(tmpdir);
    fprintf(stdout, "\n%d passed, %d failed, %d skipped\n", npass, nfail, nskip);
    return (0 == nfail) ? 0 : 1;
}
