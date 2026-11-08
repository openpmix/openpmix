/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit test: a client or tool takes only a choice it offered.
 *
 * On the local socket the server answers a connecting process's offer
 * with its choice of security mechanism, wire format, buffer type and
 * datastore, and the process then sends its credential, made and packed
 * with them. A choice of anything the process did not offer ends the
 * connection there, with PMIX_ERR_NOT_SUPPORTED.
 *
 * A stand-in server answers a tool's offer with a wire format of
 * "v41,v70" - a list, which names no one module the tool offered. The
 * tool must close the connection without sending its credential. Then,
 * as a check on the stand-in, the same with "v70", which the tool must
 * take.
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "include/pmix_tool.h"
#include "src/include/pmix_globals.h"
#include "src/mca/ptl/ptl_types.h"

#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

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

static bool read_all(int sd, void *buf, size_t len)
{
    size_t got = 0;
    ssize_t n;

    while (got < len) {
        n = read(sd, (char *) buf + got, len - got);
        if (0 >= n) {
            return false;
        }
        got += (size_t) n;
    }
    return true;
}

/* the tool: its init's status, as an exit code - 0 for PMIX_SUCCESS, 1
 * for PMIX_ERR_NOT_SUPPORTED, 2 for anything else */
static int run_tool(const char *uri)
{
    pmix_proc_t me;
    pmix_info_t info;
    pmix_status_t rc;

    alarm(30);
    PMIX_INFO_LOAD(&info, PMIX_SERVER_URI, uri, PMIX_STRING);
    rc = PMIx_tool_init(&me, &info, 1);
    if (PMIX_SUCCESS == rc) {
        return 0;
    }
    return (PMIX_ERR_NOT_SUPPORTED == rc) ? 1 : 2;
}

/* Answer a tool's offer with `bfrops` as the wire format, and say whether
 * a credential followed. *status is the tool's exit status */
static bool offer_answered(const char *bfrops, bool *cred_sent, int *status)
{
    char dir[] = "/tmp/pmix-choice.XXXXXX";
    char path[256], uri[512], choice[256];
    struct sockaddr_un sa;
    struct pollfd pfd;
    pmix_ptl_hdr_t hdr;
    char *offer = NULL, c;
    int lsd = -1, sd = -1;
    size_t len;
    uint32_t u32;
    pid_t child = -1;
    bool ok = false;

    *cred_sent = false;
    *status = -1;
    if (NULL == mkdtemp(dir)) {
        return false;
    }
    snprintf(path, sizeof(path), "%s/s", dir);
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(sa.sun_path)) {
        goto cleanup;
    }
    memcpy(sa.sun_path, path, strlen(path) + 1);
    lsd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (0 > lsd || 0 != bind(lsd, (struct sockaddr *) &sa, sizeof(sa)) || 0 != listen(lsd, 4)) {
        goto cleanup;
    }
    snprintf(uri, sizeof(uri), "ptl.choice.0;usock://%s", path);

    child = fork();
    if (0 > child) {
        goto cleanup;
    }
    if (0 == child) {
        close(lsd);
        setenv("PMIX_MCA_ptl_base_handshake_wait_time", "5", 1);
        setenv("PMIX_BFROP_BUFFER_TYPE", "PMIX_BFROP_BUFFER_FULLY_DESC", 1);
        _exit(run_tool(uri));
    }

    sd = accept(lsd, NULL, NULL);
    if (0 > sd || !read_all(sd, &hdr, sizeof(hdr)) || 0 == hdr.nbytes ||
        65536 < hdr.nbytes || NULL == (offer = malloc(hdr.nbytes)) ||
        !read_all(sd, offer, hdr.nbytes)) {
        goto cleanup;
    }
    /* the choice: native, `bfrops`, a fully described buffer, hash */
    len = 0;
    memcpy(choice + 8 + len, "native", 7);
    len += 7;
    memcpy(choice + 8 + len, bfrops, strlen(bfrops) + 1);
    len += strlen(bfrops) + 1;
    choice[8 + len] = (char) PMIX_BFROP_BUFFER_FULLY_DESC;
    len += 1;
    memcpy(choice + 8 + len, "hash", 5);
    len += 5;
    u32 = htonl((uint32_t) PMIX_SUCCESS);
    memcpy(choice, &u32, 4);
    u32 = htonl((uint32_t) len);
    memcpy(choice + 4, &u32, 4);
    if ((ssize_t) (8 + len) != write(sd, choice, 8 + len)) {
        goto cleanup;
    }
    /* anything at all after the choice is the credential */
    pfd.fd = sd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    if (0 < poll(&pfd, 1, 10000) && 1 == read(sd, &c, 1)) {
        *cred_sent = true;
    }
    ok = true;

cleanup:
    if (0 <= sd) {
        close(sd);
    }
    if (0 <= lsd) {
        close(lsd);
    }
    if (0 < child) {
        waitpid(child, status, 0);
    }
    free(offer);
    unlink(path);
    rmdir(dir);
    return ok;
}

int main(int argc, char **argv)
{
    bool cred;
    int status;
    char detail[128];
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGPIPE, SIG_IGN);
    fprintf(stdout, "\n=== ptl: a client or tool takes only a choice it offered ===\n\n");

    if (!offer_answered("v41,v70", &cred, &status)) {
        report("a choice naming no module offered", 0, "the stand-in server could not run");
    } else {
        snprintf(detail, sizeof(detail), "%s; the tool %s",
                 cred ? "a credential was sent" : "no credential",
                 (WIFEXITED(status) && 1 == WEXITSTATUS(status)) ? "was refused"
                 : (WIFEXITED(status) && 0 == WEXITSTATUS(status)) ? "connected"
                                                                   : "failed otherwise");
        report("a choice naming no module offered ends the connect with PMIX_ERR_NOT_SUPPORTED",
               !cred && WIFEXITED(status) && 1 == WEXITSTATUS(status), detail);
    }

    if (!offer_answered("v70", &cred, &status)) {
        report("a choice the tool offered", 0, "the stand-in server could not run");
    } else {
        report("a choice the tool offered is taken, and its credential sent", cred,
               "no credential");
    }

    fprintf(stdout, "\n%d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
