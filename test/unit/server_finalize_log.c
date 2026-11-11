/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Output a server logs just before it finalizes reaches its stderr.
 *
 * A server's log to stderr - a show_help message, or PMIx_Log of
 * PMIX_LOG_STDERR - is thread-shifted, and plog/stdfd then hands it to
 * the IOF for writing. PMIx_server_finalize clears the initialized flag
 * and only then drains the progress thread, so a log still queued at
 * that point ran with the flag down. stdfd handed it to
 * PMIx_server_IOF_deliver, the public entry point, which answers
 * PMIX_ERR_INIT when the flag is down: the message was dropped and a
 * PMIX ERROR line logged in its place.
 *
 * Test cases:
 *
 *   PMIx_Log_nb of a stderr line, queued behind a busy progress thread,
 *   then PMIx_server_finalize      -> the line is on stderr, with no
 *                                     PMIX ERROR line, and the log's
 *                                     callback reports success
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "include/pmix_server.h"

#include "src/include/pmix_globals.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define FL_MARKER "server-finalize-log-marker"

static pmix_event_t busy_ev;
static volatile int log_called = 0;
static volatile pmix_status_t log_status = PMIX_ERR_TIMEOUT;

/* hold the progress thread long enough that the log queued behind this
 * runs only once finalize has begun */
static void busy_cb(int fd, short args, void *cbdata)
{
    PMIX_HIDE_UNUSED_PARAMS(fd, args, cbdata);
    usleep(300000);
}

static void log_cb(pmix_status_t status, void *cbdata)
{
    PMIX_HIDE_UNUSED_PARAMS(cbdata);
    log_status = status;
    log_called = 1;
}

int main(int argc, char **argv)
{
    static pmix_server_module_t mymodule = {0};
    const char *tmp = getenv("TMPDIR");
    char path[512], buf[8192];
    pmix_info_t data;
    pmix_status_t rc;
    int fd, saved, ok = 1;
    ssize_t n;
    size_t len = 0;

    (void) argc;
    (void) argv;
    setvbuf(stdout, NULL, _IONBF, 0);
    fprintf(stdout, "server_finalize_log: a log in flight at server finalize\n");

    snprintf(path, sizeof(path), "%s/sfinlog.XXXXXX", (NULL == tmp) ? "/tmp" : tmp);
    fd = mkstemp(path);
    if (0 > fd) {
        fprintf(stderr, "mkstemp failed: %s\n", strerror(errno));
        return 1;
    }
    saved = dup(STDERR_FILENO);
    if (0 > saved || 0 > dup2(fd, STDERR_FILENO)) {
        fprintf(stderr, "dup2 failed: %s\n", strerror(errno));
        unlink(path);
        return 1;
    }

    rc = PMIx_server_init(&mymodule, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        dup2(saved, STDERR_FILENO);
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        unlink(path);
        return 1;
    }

    pmix_event_assign(&busy_ev, pmix_globals.evbase, -1, EV_WRITE, busy_cb, NULL);
    pmix_event_active(&busy_ev, EV_WRITE, 1);

    PMIX_INFO_LOAD(&data, PMIX_LOG_STDERR, FL_MARKER "\n", PMIX_STRING);
    rc = PMIx_Log_nb(&data, 1, NULL, 0, log_cb, NULL);
    if (PMIX_SUCCESS != rc) {
        dup2(saved, STDERR_FILENO);
        fprintf(stderr, "PMIx_Log_nb failed: %s\n", PMIx_Error_string(rc));
        ok = 0;
    }

    rc = PMIx_server_finalize();
    PMIX_INFO_DESTRUCT(&data);
    fflush(stderr);
    dup2(saved, STDERR_FILENO);
    close(saved);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_finalize failed: %s\n", PMIx_Error_string(rc));
        ok = 0;
    }

    if (0 > lseek(fd, 0, SEEK_SET)) {
        fprintf(stderr, "lseek failed: %s\n", strerror(errno));
        ok = 0;
    }
    while (len < sizeof(buf) - 1 && 0 < (n = read(fd, buf + len, sizeof(buf) - 1 - len))) {
        len += (size_t) n;
    }
    buf[len] = '\0';
    close(fd);
    unlink(path);

    if (NULL == strstr(buf, FL_MARKER)) {
        fprintf(stdout, "  FAIL: the logged line did not reach stderr\n");
        ok = 0;
    } else {
        fprintf(stdout, "  PASS: the logged line reached stderr\n");
    }
    if (NULL != strstr(buf, "PMIX ERROR")) {
        fprintf(stdout, "  FAIL: an error was logged in its place\n");
        ok = 0;
    } else {
        fprintf(stdout, "  PASS: no error was logged\n");
    }
    if (!log_called || PMIX_SUCCESS != log_status) {
        fprintf(stdout, "  FAIL: the log's callback %s\n",
                log_called ? PMIx_Error_string(log_status) : "never ran");
        ok = 0;
    } else {
        fprintf(stdout, "  PASS: the log's callback reported success\n");
    }
    if (!ok) {
        fprintf(stdout, "--- captured stderr ---\n%s--- end ---\n", buf);
    }
    return ok ? 0 : 1;
}
