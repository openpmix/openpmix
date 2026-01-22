/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2014 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2006      Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2008-2022 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2009      Oak Ridge National Labs.  All rights reserved.
 * Copyright (c) 2010-2014 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2014      Hochschule Esslingen.  All rights reserved.
 * Copyright (c) 2015-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2015-2018 Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2016-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2020      IBM Corporation.  All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "pmix_config.h"

#include "src/client/pmix_client_ops.h"
#include "src/hwloc/pmix_hwloc.h"
#include "src/mca/base/pmix_mca_base_var.h"
#include "src/runtime/pmix_rte.h"
#include "src/server/pmix_server_ops.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_printf.h"
#include "src/util/pmix_show_help.h"
#include "src/util/pmix_timings.h"

#if PMIX_ENABLE_TIMING
char *pmix_timing_output = NULL;
bool pmix_timing_overhead = true;
#endif

static bool pmix_register_done = false;
char *pmix_net_private_ipv4 = NULL;
int pmix_event_caching_window = 1;
char *pmix_progress_thread_cpus = NULL;
bool pmix_bind_progress_thread_reqd = false;
int pmix_maxfd = 1024;
int pmix_server_client_fintime;
bool pmix_keep_fqdn_hostnames = false;

char *pmix_var_dump_color[PMIX_VAR_DUMP_COLOR_KEY_COUNT] = {NULL};
static char *pmix_var_dump_color_string = NULL;

static char *pmix_var_dump_color_keys[PMIX_VAR_DUMP_COLOR_KEY_COUNT+1] = {
    [PMIX_VAR_DUMP_COLOR_VAR_NAME] = "name",
    [PMIX_VAR_DUMP_COLOR_VAR_VALUE] = "value",
    [PMIX_VAR_DUMP_COLOR_VALID_VALUES] = "valid_values",
    [PMIX_VAR_DUMP_COLOR_KEY_COUNT] = NULL
};

static int parse_color_string(char *color_string, char **key_names,
                              int key_count, char **values_out);

pmix_status_t pmix_register_params(void)
{
    int ret;
    char *string, *tmp;

    if (pmix_register_done) {
        return PMIX_SUCCESS;
    }

    pmix_register_done = true;

#if PMIX_ENABLE_TIMING
    pmix_timing_output = NULL;
    (void) pmix_mca_base_var_register(
        "pmix", "pmix", NULL, "timing_output",
        "The name of output file for timing information. If this parameter is not set then output "
        "will be directed into PMIX debug channel.",
        PMIX_MCA_BASE_VAR_TYPE_STRING,
        &pmix_timing_output);

    pmix_timing_overhead = true;
    (void) pmix_mca_base_var_register(
        "pmix", "pmix", NULL, "timing_overhead",
        "Timing framework introduce additional overhead (malloc's mostly)."
        " The time spend in such costly routines is measured and may be accounted"
        " (subtracted from timestamps). 'true' means consider overhead, 'false' - ignore (default: "
        "true).",
        PMIX_MCA_BASE_VAR_TYPE_BOOL,
        &pmix_timing_overhead);
#endif

    /* RFC1918 defines
       - 10.0.0./8
       - 172.16.0.0/12
       - 192.168.0.0/16

       RFC3330 also mentions
       - 169.254.0.0/16 for DHCP onlink iff there's no DHCP server
    */
    pmix_net_private_ipv4 = "10.0.0.0/8;172.16.0.0/12;192.168.0.0/16;169.254.0.0/16";
    ret = pmix_mca_base_var_register(
        "pmix", "pmix", "net", "private_ipv4",
        "Semicolon-delimited list of CIDR notation entries specifying what networks are considered "
        "\"private\" (default value based on RFC1918 and RFC3330)",
        PMIX_MCA_BASE_VAR_TYPE_STRING,
        &pmix_net_private_ipv4);
    if (0 > ret) {
        return ret;
    }

    (void) pmix_mca_base_var_register(
        "pmix", "pmix", NULL, "event_caching_window",
        "Time (in seconds) to aggregate events before reporting them - this "
        "suppresses event cascades when processes abnormally terminate",
        PMIX_MCA_BASE_VAR_TYPE_INT,
        &pmix_event_caching_window);

    /****   CLIENT: VERBOSE OUTPUT PARAMS   ****/
    (void) pmix_mca_base_var_register("pmix", "pmix", "client", "get_verbose",
                                      "Verbosity for client get operations",
                                      PMIX_MCA_BASE_VAR_TYPE_INT,
                                      &pmix_client_globals.get_verbose);

    (void) pmix_mca_base_var_register("pmix", "pmix", "client", "connect_verbose",
                                      "Verbosity for client connect operations",
                                      PMIX_MCA_BASE_VAR_TYPE_INT,
                                      &pmix_client_globals.connect_verbose);

    (void) pmix_mca_base_var_register("pmix", "pmix", "client", "fence_verbose",
                                      "Verbosity for client fence operations",
                                      PMIX_MCA_BASE_VAR_TYPE_INT,
                                      &pmix_client_globals.fence_verbose);

    (void)
        pmix_mca_base_var_register("pmix", "pmix", "client", "pub_verbose",
                                   "Verbosity for client publish, lookup, and unpublish operations",
                                   PMIX_MCA_BASE_VAR_TYPE_INT,
                                   &pmix_client_globals.pub_verbose);

    (void) pmix_mca_base_var_register("pmix", "pmix", "client", "spawn_verbose",
                                      "Verbosity for client spawn operations",
                                      PMIX_MCA_BASE_VAR_TYPE_INT,
                                      &pmix_client_globals.spawn_verbose);

    (void) pmix_mca_base_var_register("pmix", "pmix", "client", "event_verbose",
                                      "Verbosity for client event notifications",
                                      PMIX_MCA_BASE_VAR_TYPE_INT,
                                      &pmix_client_globals.event_verbose);

    (void) pmix_mca_base_var_register("pmix", "pmix", "client", "iof_verbose",
                                      "Verbosity for client iof operations",
                                      PMIX_MCA_BASE_VAR_TYPE_INT,
                                      &pmix_client_globals.iof_verbose);

    (void) pmix_mca_base_var_register("pmix", "pmix", "client", "base_verbose",
                                      "Verbosity for basic client operations",
                                      PMIX_MCA_BASE_VAR_TYPE_INT,
                                      &pmix_client_globals.base_verbose);

    (void) pmix_mca_base_var_register("pmix", "pmix", "client", "group_verbose",
                                      "Verbosity for server group operations",
                                      PMIX_MCA_BASE_VAR_TYPE_INT,
                                      &pmix_client_globals.group_verbose);

    /****   SERVER: VERBOSE OUTPUT PARAMS   ****/
    (void) pmix_mca_base_var_register("pmix", "pmix", "server", "get_verbose",
                                      "Verbosity for server get operations",
                                      PMIX_MCA_BASE_VAR_TYPE_INT,
                                      &pmix_server_globals.get_verbose);

    (void) pmix_mca_base_var_register("pmix", "pmix", "server", "connect_verbose",
                                      "Verbosity for server connect operations",
                                      PMIX_MCA_BASE_VAR_TYPE_INT,
                                      &pmix_server_globals.connect_verbose);

    (void) pmix_mca_base_var_register("pmix", "pmix", "server", "fence_verbose",
                                      "Verbosity for server fence operations",
                                      PMIX_MCA_BASE_VAR_TYPE_INT,
                                      &pmix_server_globals.fence_verbose);

    (void)
        pmix_mca_base_var_register("pmix", "pmix", "server", "pub_verbose",
                                   "Verbosity for server publish, lookup, and unpublish operations",
                                   PMIX_MCA_BASE_VAR_TYPE_INT,
                                   &pmix_server_globals.pub_verbose);

    (void) pmix_mca_base_var_register("pmix", "pmix", "server", "spawn_verbose",
                                      "Verbosity for server spawn operations",
                                      PMIX_MCA_BASE_VAR_TYPE_INT,
                                      &pmix_server_globals.spawn_verbose);

    (void) pmix_mca_base_var_register("pmix", "pmix", "server", "event_verbose",
                                      "Verbosity for server event operations",
                                      PMIX_MCA_BASE_VAR_TYPE_INT,
                                      &pmix_server_globals.event_verbose);

    (void) pmix_mca_base_var_register("pmix", "pmix", "server", "iof_verbose",
                                      "Verbosity for server iof operations",
                                      PMIX_MCA_BASE_VAR_TYPE_INT,
                                      &pmix_server_globals.iof_verbose);

    (void) pmix_mca_base_var_register("pmix", "pmix", "server", "base_verbose",
                                      "Verbosity for basic server operations",
                                      PMIX_MCA_BASE_VAR_TYPE_INT,
                                      &pmix_server_globals.base_verbose);

    (void) pmix_mca_base_var_register("pmix", "pmix", "server", "group_verbose",
                                      "Verbosity for server group operations",
                                      PMIX_MCA_BASE_VAR_TYPE_INT,
                                      &pmix_server_globals.group_verbose);

    pmix_server_globals.fence_localonly_opt = true;
    (void) pmix_mca_base_var_register(
        "pmix", "pmix", "server", "fence_localonly_opt",
        "Optimize local-only fence opteration by eliminating the upcall to the RM (default: true)",
        PMIX_MCA_BASE_VAR_TYPE_BOOL,
        &pmix_server_globals.fence_localonly_opt);

    /* check for maximum number of pending output messages */
    pmix_globals.output_limit = (size_t) INT_MAX;
    (void) pmix_mca_base_var_register("pmix", "iof", NULL, "output_limit",
                                      "Maximum backlog of output messages [default: unlimited]",
                                      PMIX_MCA_BASE_VAR_TYPE_SIZE_T,
                                      &pmix_globals.output_limit);

    pmix_globals.xml_output = false;
    (void) pmix_mca_base_var_register("pmix", "iof", NULL, "xml_output",
                                      "Display all output in XML format (default: false)",
                                      PMIX_MCA_BASE_VAR_TYPE_BOOL,
                                      &pmix_globals.xml_output);

    /* whether to tag output */
    /* if we requested xml output, be sure to tag the output as well */
    pmix_globals.tag_output = pmix_globals.xml_output;
    (void) pmix_mca_base_var_register("pmix", "iof", NULL, "tag_output",
                                      "Tag all output with [job,rank] (default: false)",
                                      PMIX_MCA_BASE_VAR_TYPE_BOOL,
                                      &pmix_globals.tag_output);
    if (pmix_globals.xml_output) {
        pmix_globals.tag_output = true;
    }

    /* whether to timestamp output */
    pmix_globals.timestamp_output = false;
    (void) pmix_mca_base_var_register("pmix", "iof", NULL, "timestamp_output",
                                      "Timestamp all application process output (default: false)",
                                      PMIX_MCA_BASE_VAR_TYPE_BOOL,
                                      &pmix_globals.timestamp_output);

    /* max size of the notification hotel */
    pmix_globals.max_events = 512;
    (void) pmix_mca_base_var_register("pmix", "pmix", "max", "events",
                                      "Maximum number of event notifications to cache",
                                      PMIX_MCA_BASE_VAR_TYPE_INT,
                                      &pmix_globals.max_events);

    /* how long to cache an event */
    pmix_globals.event_eviction_time = 120;
    (void) pmix_mca_base_var_register("pmix", "pmix", "event", "eviction_time",
                                      "Maximum number of seconds to cache an event",
                                      PMIX_MCA_BASE_VAR_TYPE_INT,
                                      &pmix_globals.event_eviction_time);

    /* max number of IOF messages to cache */
    pmix_server_globals.max_iof_cache = 1024 * 1024;
    (void) pmix_mca_base_var_register("pmix", "pmix", "max", "iof_cache",
                                      "Maximum number of IOF messages to cache",
                                      PMIX_MCA_BASE_VAR_TYPE_INT,
                                      &pmix_server_globals.max_iof_cache);

    (void) pmix_mca_base_var_register("pmix", "pmix", NULL, "progress_thread_cpus",
                                      "Comma-delimited list of ranges of CPUs to which"
                                      "the internal PMIx progress thread is to be bound",
                                      PMIX_MCA_BASE_VAR_TYPE_STRING,
                                      &pmix_progress_thread_cpus);

    (void) pmix_mca_base_var_register("pmix", "pmix", NULL, "bind_progress_thread_reqd",
                                      "Whether binding of internal PMIx progress thread is required",
                                      PMIX_MCA_BASE_VAR_TYPE_BOOL,
                                      &pmix_bind_progress_thread_reqd);

    (void) pmix_mca_base_var_register("pmix", "pmix", NULL, "maxfd",
                                      "In non-Linux environments, use this value as a maximum number of file descriptors to close when forking a new child process",
                                      PMIX_MCA_BASE_VAR_TYPE_INT,
                                      &pmix_maxfd);

    pmix_server_client_fintime = 10;
    (void) pmix_mca_base_var_register("pmix", "pmix", NULL, "finalize_timeout",
                                      "Time in seconds to wait for server to ack client finalize request",
                                      PMIX_MCA_BASE_VAR_TYPE_INT,
                                      &pmix_server_client_fintime);

    pmix_keep_fqdn_hostnames = false;
    (void) pmix_mca_base_var_register("pmix", "pmix", NULL, "keep_fqdn_hostnames",
                                      "Whether or not to keep FQDN hostnames [default: no]",
                                      PMIX_MCA_BASE_VAR_TYPE_BOOL,
                                      &pmix_keep_fqdn_hostnames);

    /* Var-dump color */

    string = pmix_argv_join_range(pmix_var_dump_color_keys, 0, PMIX_VAR_DUMP_COLOR_KEY_COUNT, ',');
    if (NULL == string) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    }

    ret = pmix_asprintf(&tmp, "The colors to use when dumping MCA vars with color "
        "(e.g. pmix_info). The format is a comma-delimited key=value list, where the "
        "key is the attribute whose color to adjust, and the value is the ANSI color "
        "code (see the ANSI X3.64 CSI SGR sequence). Available keys: %s.", string);
    free(string);
    string = tmp;
    if (0 > ret) {
        return PMIX_ERR_OUT_OF_RESOURCE;
    }

    /* Basic color options: 30=black, 31=red, 32=green,
     * 33=yellow, 34=blue, 35=magenta, 36=cyan, 37=white
     * https://en.wikipedia.org/wiki/ANSI_escape_code#Colors */
    pmix_var_dump_color_string = "name=34,value=32,valid_values=36";
    ret = pmix_mca_base_var_register("pmix", "pmix", NULL, "var_dump_color", string,
                                     PMIX_MCA_BASE_VAR_TYPE_STRING,
                                     &pmix_var_dump_color_string);
    free(string);
    if (0 > ret) {
        return ret;
    }

    ret = parse_color_string(pmix_var_dump_color_string, pmix_var_dump_color_keys,
                             PMIX_VAR_DUMP_COLOR_KEY_COUNT, pmix_var_dump_color);
    if (PMIX_SUCCESS != ret) {
        return ret;
    }


    pmix_hwloc_register();
    return PMIX_SUCCESS;
}

pmix_status_t pmix_deregister_params(void)
{
    pmix_register_done = false;

    return PMIX_SUCCESS;
}

/* Parses a color 'string' and extracts the 'code' for each 'key'
 * The string is in the format of 'key1=code1:key2=code2:...'
 * Example: pmix_var_dump_color MCA parameter
 *
 * color_string: The string containing the key=code sequences to parse
 * key_names: The keys to extract (e.g. key1, key2)
 * key_count: The length of key_names
 * values_out: The extracted values for the keys, 1-1 with key_names
 *   ATTENTION: Make sure it's at least as large as key_names...
 */
static int parse_color_string(char *color_string, char **key_names,
                              int key_count, char **values_out) {

    char **tokens = NULL, **kv = NULL;
    int return_code = PMIX_SUCCESS;

    for (int k = 0; k < key_count; k++) {
        values_out[k] = NULL;
    }

    if (NULL != color_string) {
        tokens = PMIx_Argv_split(color_string, ',');
        if (NULL == tokens) {
            return_code = PMIX_ERR_OUT_OF_RESOURCE;
            goto end;
        }
    }

    for (int i = 0; tokens && tokens[i] != NULL; i++) {
        kv = PMIx_Argv_split(tokens[i], '=');
        if (NULL == kv) {
            return_code = PMIX_ERR_OUT_OF_RESOURCE;
            goto end;
        }

        // Expected format of token: key=code
        if (PMIx_Argv_count(kv) != 2) {
            pmix_show_help("help-pmix-runtime.txt", "var_dump_color:format-error",
                true, tokens[i]);
            goto skip_token;
        }

        bool key_found = false;

        // Look for key name and store value in respective position
        for (int k = 0; k < key_count; ++k) {
            if (strcasecmp(key_names[k], kv[0]) == 0) {
                int ret = pmix_asprintf(&values_out[k], "\033[%sm", kv[1]);
                if (ret < 0) {
                    return_code = PMIX_ERR_OUT_OF_RESOURCE;
                    goto end;
                }

                key_found = true;
                break;
            }
        }

        if (!key_found) {
            char *valid_keys = pmix_argv_join_range(key_names, 0, key_count, ',');
            if (NULL == valid_keys) {
                return_code = PMIX_ERR_OUT_OF_RESOURCE;
                goto end;
            }

            pmix_show_help("help-pmix-runtime.txt", "var_dump_color:unknown-key",
                true, kv[0], tokens[i], valid_keys);
            free(valid_keys);
        }

        skip_token:

        PMIx_Argv_free(kv);
        kv = NULL;
    }

    // Set values for keys not in the MCA parameter to ""
    for (int k = 0; k < key_count; k++) {
        if (NULL == values_out[k]) {
            values_out[k] = strdup(""); // needs to be free-able

            if (NULL == values_out[k]) {
                return_code = PMIX_ERR_OUT_OF_RESOURCE;
                goto end;
            }
        }
    }

    end:

    PMIx_Argv_free(tokens);
    PMIx_Argv_free(kv);

    if (return_code != PMIX_SUCCESS) {
        for (int k = 0; k < key_count; k++) {
            free(values_out[k]);
            values_out[k] = NULL;
        }
    }

    return return_code;
}

