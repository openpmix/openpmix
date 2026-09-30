/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * A server that has written a job's output to a file finalizes cleanly,
 * and what it was still holding reaches the file.
 *
 * A job's output sink flushes the partial line it holds - output with no
 * newline yet, kept on pmix_server_globals.iof_residuals - into its file
 * when it closes. The sinks hang off the job's namespace, and the
 * namespaces are released only in rte_finalize, after
 * PMIx_server_finalize has destructed that list. So every server that had
 * written a job's output to a file walked a destructed list on its way
 * out, and crashed.
 *
 * Test cases:
 *
 *   a job's output to a directory, ending in a partial line,
 *   then finalize                         -> finalize returns, and the
 *                                            partial line is in the file
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

#define FS_NSPACE "iof-fin-ut"

static void progress_barrier(void)
{
    pmix_value_t v;

    PMIX_VALUE_LOAD(&v, "barrier", PMIX_STRING);
    PMIx_Store_internal(&pmix_globals.myid, "iof-fin-ut.barrier", &v);
    PMIX_VALUE_DESTRUCT(&v);
}

int main(int argc, char **argv)
{
    static pmix_server_module_t mymodule = {0};
    const char *tmp = getenv("TMPDIR");
    char base[512], file[1024], buf[64] = {0}, cmd[600];
    pmix_info_t info[2];
    pmix_byte_object_t bo;
    pmix_nspace_t ns;
    pmix_proc_t src;
    pmix_status_t rc;
    uint32_t one = 1;
    int fd, ok;

    (void) argc;
    (void) argv;
    setvbuf(stdout, NULL, _IONBF, 0);
    fprintf(stdout, "iof_finalize_sinks: output sinks at server finalize\n");

    snprintf(base, sizeof(base), "%s/ioffin.XXXXXX", (NULL == tmp) ? "/tmp" : tmp);
    if (NULL == mkdtemp(base)) {
        fprintf(stderr, "mkdtemp failed: %s\n", strerror(errno));
        return 1;
    }
    rc = PMIx_server_init(&mymodule, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    PMIX_INFO_LOAD(&info[0], PMIX_JOB_SIZE, &one, PMIX_UINT32);
    PMIX_INFO_LOAD(&info[1], PMIX_IOF_OUTPUT_TO_DIRECTORY, base, PMIX_STRING);
    PMIX_LOAD_NSPACE(ns, FS_NSPACE);
    rc = PMIx_server_register_nspace(ns, 1, info, 2, NULL, NULL);
    PMIX_INFO_DESTRUCT(&info[0]);
    PMIX_INFO_DESTRUCT(&info[1]);
    if (PMIX_SUCCESS != rc && PMIX_OPERATION_SUCCEEDED != rc) {
        fprintf(stderr, "register failed: %s\n", PMIx_Error_string(rc));
        PMIx_server_finalize();
        return 1;
    }
    progress_barrier();

    /* a whole line, then one with no newline - held until the sink closes */
    PMIX_LOAD_PROCID(&src, FS_NSPACE, 0);
    bo.bytes = (char *) "whole\npartial";
    bo.size = strlen(bo.bytes);
    (void) PMIx_server_IOF_deliver(&src, PMIX_FWD_STDOUT_CHANNEL, &bo, NULL, 0, NULL, NULL);
    progress_barrier();
    usleep(200000);
    progress_barrier();

    PMIx_server_finalize();
    fprintf(stdout, "  PASS: finalize returns\n");

    snprintf(file, sizeof(file), "%s/%s/rank.0/stdout", base, FS_NSPACE);
    fd = open(file, O_RDONLY);
    if (0 <= fd) {
        if (0 > read(fd, buf, sizeof(buf) - 1)) {
            buf[0] = '\0';
        }
        close(fd);
    }
    ok = (NULL != strstr(buf, "whole\n") && NULL != strstr(buf, "partial"));
    fprintf(stdout, "  %s: the partial line reaches the file\n", ok ? "PASS" : "FAIL");

    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", base);
    if (0 != system(cmd)) {
        fprintf(stderr, "could not remove %s\n", base);
    }
    return ok ? 0 : 1;
}
