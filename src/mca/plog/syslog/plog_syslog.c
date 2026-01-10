/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2007      Sun Microsystems, Inc.  All rights reserved.
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "pmix_config.h"
#include "pmix_common.h"

#include <string.h>
#ifdef HAVE_TIME_H
#    include <time.h>
#endif
#ifdef HAVE_SYS_TIME_H
#    include <sys/time.h>
#endif /* HAVE_SYS_TIME_H */
#ifdef HAVE_SYSLOG_H
#    include <syslog.h>
#endif
#include <stdarg.h>

#include "src/mca/bfrops/bfrops.h"
#include "src/server/pmix_server_ops.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_name_fns.h"
#include "src/util/pmix_show_help.h"

#include "plog_syslog.h"
#include "src/mca/plog/base/base.h"

/* Static API's */
static pmix_status_t init(void);
static void finalize(void);
static pmix_status_t mylog(const pmix_proc_t *source, const pmix_info_t data[], size_t ndata,
                           const pmix_info_t directives[], size_t ndirs);

/* Module def */
pmix_plog_module_t pmix_plog_syslog_module = {
    .name = "syslog",
    .init = init,
    .finalize = finalize,
    .log = mylog
};

static pmix_status_t init(void)
{
    int opts;
    char *mychannels = "lsys,gsys,syslog,local_syslog,global_syslog";

    pmix_plog_syslog_module.channels = PMIx_Argv_split(mychannels, ',');

    opts = LOG_CONS | LOG_PID;
    openlog("PMIx Log Report:", opts, LOG_USER);

    return PMIX_SUCCESS;
}

static void finalize(void)
{
    closelog();
    PMIx_Argv_free(pmix_plog_syslog_module.channels);
}

static pmix_status_t write_local(const pmix_proc_t *source, time_t timestamp,
                                 int severity, char *msg);


static pmix_status_t mylog(const pmix_proc_t *source, const pmix_info_t data[], size_t ndata,
                           const pmix_info_t directives[], size_t ndirs)
{
    size_t n;
    int pri = pmix_mca_plog_syslog_component.level;
    pmix_status_t rc;
    time_t timestamp = 0;

    /* if there is no data, then we don't handle it */
    if (NULL == data || 0 == ndata) {
        return PMIX_ERR_NOT_AVAILABLE;
    }

    pmix_output_verbose(2, pmix_plog_base_framework.framework_output,
                        "%s: plog:syslog called",
                        PMIX_NAME_PRINT(&pmix_globals.myid));

    /* check directives */
    if (NULL != directives) {
        for (n = 0; n < ndirs; n++) {
            if (0 == strncmp(directives[n].key, PMIX_LOG_SYSLOG_PRI, PMIX_MAX_KEYLEN)) {
                pri = directives[n].value.data.integer;
            } else if (0 == strncmp(directives[n].key, PMIX_LOG_TIMESTAMP, PMIX_MAX_KEYLEN)) {
                timestamp = directives[n].value.data.time;
            }
        }
    }

    /* check to see if there are any syslog entries */
    for (n = 0; n < ndata; n++) {
        if (PMIX_CHECK_KEY(&data[n], PMIX_LOG_SYSLOG)) {
            pmix_output_verbose(2, pmix_plog_base_framework.framework_output,
                                "%s: plog:syslog delivering to local syslog",
                                PMIX_NAME_PRINT(&pmix_globals.myid));
            /* we default to using the local syslog */
            rc = write_local(source, timestamp, pri,
                             data[n].value.data.string);
            if (PMIX_SUCCESS != rc) {
                return rc;
            }
        } else if (PMIX_CHECK_KEY(&data[n], PMIX_LOG_LOCAL_SYSLOG)) {
            pmix_output_verbose(2, pmix_plog_base_framework.framework_output,
                                "%s: plog:syslog delivering to local syslog",
                                PMIX_NAME_PRINT(&pmix_globals.myid));
            rc = write_local(source, timestamp, pri,
                             data[n].value.data.string);
            if (PMIX_SUCCESS != rc) {
                return rc;
            }
        } else if (PMIX_CHECK_KEY(&data[n], PMIX_LOG_GLOBAL_SYSLOG)) {
            /* only do this if we are a gateway server */
            if (PMIX_PEER_IS_GATEWAY(pmix_globals.mypeer)) {
                pmix_output_verbose(2, pmix_plog_base_framework.framework_output,
                                    "%s: plog:syslog delivering to global syslog",
                                    PMIX_NAME_PRINT(&pmix_globals.myid));
                rc = write_local(source, timestamp, pri,
                                 data[n].value.data.string);
                if (PMIX_SUCCESS != rc) {
                    return rc;
                }
            } else {
                pmix_output_verbose(2, pmix_plog_base_framework.framework_output,
                                    "%s: plog:syslog global syslog, but not gateway",
                                    PMIX_NAME_PRINT(&pmix_globals.myid));
            }
        }
    }

    return PMIX_SUCCESS;
}

static char *sev2str(int severity)
{
    switch (severity) {
    case LOG_EMERG:
        return "EMERGENCY";
    case LOG_ALERT:
        return "ALERT";
    case LOG_CRIT:
        return "CRITICAL";
    case LOG_ERR:
        return "ERROR";
    case LOG_WARNING:
        return "WARNING";
    case LOG_NOTICE:
        return "NOTICE";
    case LOG_INFO:
        return "INFO";
    case LOG_DEBUG:
        return "DEBUG";
    default:
        return "UNKNOWN SEVERITY";
    }
}

static pmix_status_t write_local(const pmix_proc_t *source, time_t timestamp,
                                 int severity, char *msg)
{
    char tod[48];

    pmix_output_verbose(5, pmix_plog_base_framework.framework_output,
                        "plog:syslog:mylog function called with severity %d", severity);

    if (0 < timestamp) {
        /* If there was a message, output it */
        (void) ctime_r(&timestamp, tod);
        /* trim the newline */
        tod[strlen(tod)-1] = '\0';
    } else {
        strcpy(tod, "N/A");
    }

    syslog(severity, "%s %s:%s PROC %s REPORTS: %s", tod, PMIX_NAME_PRINT(&pmix_globals.myid),
           sev2str(severity), PMIX_NAME_PRINT(source),
           (NULL == msg) ? "<N/A>" : msg);

    return PMIX_SUCCESS;
}
