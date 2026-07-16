/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2006-2013 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2009-2012 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2011      Oak Ridge National Labs.  All rights reserved.
 * Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2016      IBM Corporation.  All rights reserved.
 * Copyright (c) 2023-2024 Triad National Security, LLC. All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#include "src/include/pmix_config.h"
#include "include/pmix_server.h"
#include "src/include/pmix_globals.h"
#include "src/include/pmix_types.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "src/class/pmix_list.h"
#include "src/mca/preg/preg.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_environ.h"
#include "src/util/pmix_printf.h"


/* Unused when PMIX_TESTBUILD short-circuits main() before server init. */
static pmix_server_module_t mymodule __pmix_attribute_unused__ = {
    .client_connected = NULL,
    .client_finalized = NULL,
    .abort = NULL,
    .fence_nb = NULL,
    .direct_modex = NULL,
    .publish = NULL,
    .lookup = NULL,
    .unpublish = NULL,
    .spawn = NULL,
    .connect = NULL,
    .disconnect = NULL,
    .register_events = NULL,
    .deregister_events = NULL,
    .notify_event = NULL,
    .query = NULL,
    .tool_connected = NULL,
    .log = NULL,
    .allocate = NULL,
    .job_control = NULL,
    .monitor = NULL,
    .group = NULL
};


int main(int argc, char **argv)
{
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

#if PMIX_TESTBUILD
    /* This test drives real (de)compression end-to-end, but the pcompress
     * components are non-functional shims in a --enable-test-build (see the
     * top-level AGENTS.md): deflate/inflate are no-ops, so the round-trips
     * cannot reproduce their input. Report the automake "skip" status (77)
     * rather than a spurious failure. */
    fprintf(stdout, "SKIP: compression is stubbed in --enable-test-build\n");
    return 77;
#else
    char **nodes = NULL, **nodesout;
    char **procs = NULL, **procsout;
    char *tmp, *regex, *ppn, *t1;
    int n, rk;
    int errors = 0;
    pmix_status_t rc;

    /* setup the server library */
    if (PMIX_SUCCESS != (rc = PMIx_server_init(&mymodule, NULL, 0))) {
        fprintf(stderr, "Init failed with error %s\n", PMIx_Error_string(rc));
        return rc;
    }

    for (n=0; n < 10000; n++) {
        pmix_asprintf(&tmp, "node%04d", n);
        PMIx_Argv_append_nosize(&nodes, tmp);
        free(tmp);
    }
    tmp = PMIx_Argv_join(nodes, ',');
    PMIx_Argv_free(nodes);
    PMIx_generate_regex(tmp, &regex);

    // reverse it
    rc = pmix_preg.parse_nodes(regex, &nodesout);
    fprintf(stderr, "PARSE NODES %s\n", PMIx_Error_string(rc));
    t1 = PMIx_Argv_join(nodesout, ',');
    PMIx_Argv_free(nodesout);
    if (0 == strcmp(t1, tmp)) {
        fprintf(stderr, "NODES MATCH\n");
    } else {
        fprintf(stderr, "NODES ERROR\n");
        errors++;
    }
    free(tmp);
    free(t1);

    rk = 0;
    for (n = 0; n < 10000; n++) {
        // just do 2ppn
        pmix_asprintf(&tmp, "%d,%d", rk, rk+1);
        PMIx_Argv_append_nosize(&procs, tmp);
        free(tmp);
        rk += 2;
    }
    tmp = PMIx_Argv_join(procs, ',');
    PMIx_Argv_free(procs);
    PMIx_generate_ppn(tmp, &ppn);

    rc = pmix_preg.parse_procs(ppn, &procsout);
    fprintf(stderr, "PARSE PROCS %s\n", PMIx_Error_string(rc));
    t1 = PMIx_Argv_join(procsout, ',');
    PMIx_Argv_free(procsout);
    if (0 == strcmp(t1, tmp)) {
        fprintf(stderr, "PROCS MATCH\n");
    } else {
        fprintf(stderr, "PROCS ERROR\n");
        errors++;
    }
    free(tmp);
    free(t1);

    /* finalize the server library */
    if (PMIX_SUCCESS != (rc = PMIx_server_finalize())) {
        fprintf(stderr, "Finalize failed with error %d\n", rc);
    }

    return errors;
#endif
}
