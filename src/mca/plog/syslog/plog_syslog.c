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
#include "src/util/pmix_string_copy.h"

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

    /* both of these are MCA parameters this component publishes, so
     * they have to actually reach openlog - LOG_CONS in particular is
     * what the "console" parameter is asking for, and forcing it on
     * writes to the system console for every message the logger cannot
     * take, on every node */
    opts = LOG_PID;
    if (pmix_mca_plog_syslog_component.console) {
        opts |= LOG_CONS;
    }
    openlog("PMIx Log Report:", opts,
            pmix_mca_plog_syslog_component.facility);

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
    size_t n, nhandled = 0;
    int pri = pmix_mca_plog_syslog_component.level;
    pmix_status_t rc;
    time_t timestamp = 0;
    /* completion is tracked in the caller's array - see the plog
     * AGENTS.md on why that state is shared and mutable */
    pmix_info_t *dt = (pmix_info_t *) data;
    char *msg;

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
                /* the type is the caller's to choose, so read the union
                 * member the caller actually filled in */
                if (PMIX_SUCCESS != PMIx_Value_get_number(&directives[n].value,
                                                          &pri, PMIX_INT)) {
                    PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
                    pri = pmix_mca_plog_syslog_component.level;
                }
            } else if (0 == strncmp(directives[n].key, PMIX_LOG_TIMESTAMP, PMIX_MAX_KEYLEN)) {
                if (PMIX_TIME == directives[n].value.type) {
                    timestamp = directives[n].value.data.time;
                }
            }
        }
    }

    /* check to see if there are any syslog entries */
    for (n = 0; n < ndata; n++) {
        /* another module may already have serviced this entry - the
         * marks are there so we don't log it a second time */
        if (PMIX_INFO_OP_IS_COMPLETE(&data[n])) {
            continue;
        }
        if (!PMIX_CHECK_KEY(&data[n], PMIX_LOG_SYSLOG) &&
            !PMIX_CHECK_KEY(&data[n], PMIX_LOG_LOCAL_SYSLOG) &&
            !PMIX_CHECK_KEY(&data[n], PMIX_LOG_GLOBAL_SYSLOG)) {
            continue;
        }
        /* the value's type is the caller's to set, and for a client's
         * PMIx_Log that caller is on the far end of a socket - confirm
         * it is a string before syslog() renders it with %s */
        if (PMIX_STRING != data[n].value.type) {
            PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
            PMIX_INFO_OP_COMPLETED(&dt[n]);
            continue;
        }
        msg = data[n].value.data.string;

        if (PMIX_CHECK_KEY(&data[n], PMIX_LOG_GLOBAL_SYSLOG) &&
            !PMIX_PEER_IS_GATEWAY(pmix_globals.mypeer)) {
            /* only a gateway can reach the global syslog. Leave the
             * entry unmarked and uncounted so the framework can tell
             * the caller we did not service it - a client that hears
             * "success" here would never fall back to anyone who can */
            pmix_output_verbose(2, pmix_plog_base_framework.framework_output,
                                "%s: plog:syslog global syslog, but not gateway",
                                PMIX_NAME_PRINT(&pmix_globals.myid));
            continue;
        }

        pmix_output_verbose(2, pmix_plog_base_framework.framework_output,
                            "%s: plog:syslog delivering to %s syslog",
                            PMIX_NAME_PRINT(&pmix_globals.myid),
                            PMIX_CHECK_KEY(&data[n], PMIX_LOG_GLOBAL_SYSLOG) ? "global" : "local");
        rc = write_local(source, timestamp, pri, msg);
        if (PMIX_SUCCESS != rc) {
            return rc;
        }
        PMIX_INFO_OP_COMPLETED(&dt[n]);
        ++nhandled;
    }

    /* saying "success" for a request we did not actually service would
     * swallow a PMIX_LOG_ONCE and starve the channel the caller wanted */
    if (0 == nhandled) {
        return PMIX_ERR_TAKE_NEXT_OPTION;
    }
    return PMIX_SUCCESS;
}

static const char *sev2str(int severity)
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
    size_t len;

    pmix_output_verbose(5, pmix_plog_base_framework.framework_output,
                        "plog:syslog:mylog function called with severity %d", severity);

    if (0 < timestamp) {
        /* ctime_r is allowed to decline a time it cannot represent, and
         * it leaves the buffer untouched when it does - so the result
         * has to be checked before it is indexed into */
        if (NULL == ctime_r(&timestamp, tod)) {
            pmix_strncpy(tod, "unknown", sizeof(tod) - 1);
        } else {
            /* trim the newline */
            len = strlen(tod);
            if (0 < len && '\n' == tod[len - 1]) {
                tod[len - 1] = '\0';
            }
        }
    } else {
        pmix_strncpy(tod, "N/A", sizeof(tod) - 1);
    }

    syslog(severity, "%s %s:%s PROC %s REPORTS: %s", tod, PMIX_NAME_PRINT(&pmix_globals.myid),
           sev2str(severity), PMIX_NAME_PRINT(source),
           (NULL == msg) ? "<N/A>" : msg);

    return PMIX_SUCCESS;
}
