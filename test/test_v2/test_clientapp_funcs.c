/*
 * Copyright (c) 2013-2017 Intel, Inc. All rights reserved.
 * Copyright (c) 2015      Artem Y. Polyakov <artpol84@gmail.com>.
 *                         All rights reserved.
 * Copyright (c) 2015-2018 Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2020      Triad National Security, LLC
 *                         All rights reserved.
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

/* This file includes all functions called directly by client apps, along with
 * their callees. */

#include <pmix_common.h>
#include <src/include/pmix_config.h>

#include "test_common.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>

extern int pmix_test_verbose;
extern test_params params;

extern FILE *pmixt_outfile;

// presently a placeholder - to be developed into a client-only command parser,
// separating the processing logic for client command line options from that for
// other callers (test app or server), unlike how parse_cmd() presently works.
void parse_cmd_client(int argc, char **argv, test_params *params, validation_params *v_params)
{
    ;
}

void pmixt_pre_init(int argc, char **argv, test_params *params, validation_params *v_params)
{

    ssize_t v_size = -1;

    default_params(params, v_params);
    parse_cmd(argc, argv, params, v_params);
    TEST_OUTPUT(("v_params->pmix_rank: %d", v_params->pmix_rank));

    /* set filename if available in params */
    if (NULL != params->prefix && -1 != params->ns_id) {
        PMIXT_SET_FILE(params->prefix, params->ns_id, v_params->pmix_rank);
    } else {
        pmixt_outfile = stdout;
    }
}

void pmixt_fix_rank_and_ns(pmix_proc_t *this_proc, test_params *params, validation_params *v_params)
{
    // first check for bugs
    if (this_proc->rank != v_params->pmix_rank) {
        TEST_ERROR_EXIT(("Client ns: v_params %s; this_proc: %s, Rank returned in PMIx_Init: "
                         "%d does not match validation rank: %d.",
                         v_params->pmix_nspace, this_proc->nspace, this_proc->rank,
                         v_params->pmix_rank));
    }
    // Fix rank if running under RM
    if (PMIX_RANK_UNDEF == v_params->pmix_rank) {
        char *ranklist = getenv("SLURM_GTIDS");
        char *rankno = getenv("SLURM_LOCALID");
        // Fix rank if running under SLURM
        if (NULL != ranklist && NULL != rankno) {
            char **argv = pmix_argv_split(ranklist, ',');
            int count = pmix_argv_count(argv);
            int rankidx = strtoul(rankno, NULL, 10);
            if (rankidx >= count) {
                TEST_ERROR_EXIT(("It feels like we are running under SLURM:\n\t"
                                 "SLURM_GTIDS=%s, SLURM_LOCALID=%s\nbut env vars are conflicting",
                                 ranklist, rankno));
            }
            v_params->pmix_rank = strtoul(argv[rankidx], NULL, 10);
            pmix_argv_free(argv);
        } else if (NULL == getenv("PMIX_RANK")) { /* we must not be running under SLURM */
            // **Call separate function for your own resource manager here**
            // It should likely be wrapped in condition that checks for existence of an RM env var
            // Function name should be of form: fix_rank_and_nspace_rm_*
            // and should check/fix both rank and nspace
            // e.g: fix_rank_and_ns_rm_pbs(), fix_rank_and_ns_rm_torque(), etc.
        } else { /* unknown situation - PMIX_RANK is not null but SLURM env vars are */
            TEST_ERROR_EXIT(("It feels like we are running under SLURM:\n\t"
                             "PMIX_RANK=%s\nbut SLURM env vars are null\n"
                             "v_params.pmix_rank = %d, this_proc.rank = %d, pmix_local_peers = %s",
                             getenv("PMIX_RANK"), v_params->pmix_rank, this_proc->rank,
                             v_params->pmix_local_peers));
        }
    }

    // Fix namespace if running under RM
    if (NULL == v_params->pmix_nspace) {
        char *nspace = getenv("PMIX_NAMESPACE");
        if (NULL != nspace) {
            strcpy(v_params->pmix_nspace, nspace);
            free(nspace);
        } else { /* If we aren't running under SLURM, you should have set nspace
                    in your custom fix_rank_and_ns_rm_* function! */
            TEST_ERROR_EXIT(("nspace not set and no value for PMIX_NAMESPACE env variable. "
                             "Is the fix_rank_and_ns_rm_* function for this resource manager "
                             "failing to set it?"));
        }
    }
}

void pmixt_post_init(pmix_proc_t *this_proc, test_params *params, validation_params *val_params)
{
    pmixt_fix_rank_and_ns(this_proc, params, val_params);
    TEST_VERBOSE(
        (" Client/PMIX ns %s rank %d: PMIx_Init success", this_proc->nspace, this_proc->rank));
}

void pmixt_post_finalize(pmix_proc_t *this_proc, test_params *params, validation_params *v_params)
{
    free_params(params, v_params);
    TEST_VERBOSE(
        ("Client ns %s rank %d: PMIx_Finalize success", this_proc->nspace, this_proc->rank));
    TEST_VERBOSE(("Before fclose (after file closed, no more output can be printed)"));
    pmixt_exit(EXIT_SUCCESS);
}

/* This function is used to validate values returned from calls to PMIx_Get against
   side channel validation data */
void pmixt_validate_predefined(pmix_proc_t *myproc, const pmix_key_t key, pmix_value_t *value,
                               const pmix_data_type_t expected_type, validation_params *val_params)
{
    pmix_status_t rc = PMIX_ERROR;
    uint16_t local_rank = -1, node_rank = -1;
    uint32_t local_size = -1, nodeid = -1;
    char l_hostname[PMIX_MAX_KEYLEN];
    bool rank_found = false;
    int i, j;

    if (!val_params->validate_params) {
        TEST_VERBOSE(("All validation disabled, will not validate: %s", key));
        return;
    }

    if (expected_type != value->type) {
        TEST_ERROR_EXIT(("Type mismatch for key. Key: %s Type: %u Expected type: %u", key,
                         value->type, expected_type));
    }
    // if we're not validating our own process information, we are validating a peer's info,
    // so we need to examine the nodes array to find the relevant validation info.
    // For simplicity, we just do this for all cases in which we are validating node-local info.
    if (PMIX_RANK_WILDCARD != myproc->rank) {
        for (i = 0; i < val_params->pmix_num_nodes; i++) {
            for (j = 0; j < nodes[i].pmix_local_size; j++) {
                if (nodes[i].pmix_rank[j] == myproc->rank) {
                    local_rank = j;
                    // WARNING: if we test a multi-job case later, node_rank will not equal
                    // local_rank and the assignment below will have to be changed
                    node_rank = j;
                    local_size = nodes[i].pmix_local_size;
                    nodeid = nodes[i].pmix_nodeid;
                    strcpy(l_hostname, nodes[i].pmix_hostname);
                    rank_found = true;
                    break;
                }
            }
            if (rank_found) {
                break;
            }
        }
    }
    switch (value->type) {
    case PMIX_UINT16: {
        uint16_t uint16data;
        PMIX_VALUE_GET_NUMBER(rc, value, uint16data, uint16_t);
        if (PMIX_SUCCESS != rc) {
            TEST_ERROR_EXIT(
                ("Failed to retrieve value correctly. Key: %s Type: %u", key, value->type));
        }
        PMIXT_TEST_NUM_KEY(myproc, key, PMIX_LOCAL_RANK, uint16data, local_rank);
        PMIXT_TEST_NUM_KEY(myproc, key, PMIX_NODE_RANK, uint16data, node_rank);
        TEST_ERROR_EXIT(("Check input for case PMIX_UINT16: key: %s", key));
    }
    case PMIX_UINT32: {
        uint32_t uint32data;
        PMIX_VALUE_GET_NUMBER(rc, value, uint32data, uint32_t);
        if (PMIX_SUCCESS != rc) {
            TEST_ERROR_EXIT(
                ("Failed to retrieve value correctly. Key: %s Type: %u", key, value->type));
        }
        PMIXT_TEST_NUM_KEY(myproc, key, PMIX_JOB_SIZE, uint32data, val_params->pmix_job_size);
        PMIXT_TEST_NUM_KEY(myproc, key, PMIX_UNIV_SIZE, uint32data, val_params->pmix_univ_size);
        PMIXT_TEST_NUM_KEY(myproc, key, PMIX_LOCAL_SIZE, uint32data, local_size);
        PMIXT_TEST_NUM_KEY(myproc, key, PMIX_NODEID, uint32data, nodeid);
        TEST_ERROR_EXIT(("Check input for case PMIX_UINT32: key: %s", key));
    }
    case PMIX_PROC_RANK: {
        pmix_rank_t rankdata;
        PMIX_VALUE_GET_NUMBER(rc, value, rankdata, pmix_rank_t);
        if (PMIX_SUCCESS != rc) {
            TEST_ERROR(("Failed to retrieve value correctly. Key: %s Type: %u", key, value->type));
            pmixt_exit(1);
        }
        PMIXT_TEST_NUM_KEY(myproc, key, PMIX_RANK, rankdata, val_params->pmix_rank);
        TEST_ERROR_EXIT(("Check input for case PMIX_PROC_RANK: key: %s", key));
    }
    case PMIX_STRING: {
        void *stringdata;
        size_t lsize;
        PMIX_VALUE_UNLOAD(rc, value, &stringdata, &lsize);
        if (PMIX_SUCCESS != rc) {
            TEST_ERROR_EXIT(
                ("Failed to retrieve value correctly. Key: %s Type: %u", key, value->type));
        }
        PMIXT_TEST_STR_KEY(myproc, key, PMIX_NSPACE, stringdata, val_params->pmix_nspace);
        PMIXT_TEST_STR_KEY(myproc, key, PMIX_JOBID, stringdata, val_params->pmix_jobid);
        PMIXT_TEST_STR_KEY(myproc, key, PMIX_LOCAL_PEERS, stringdata, val_params->pmix_local_peers);
        PMIXT_TEST_STR_KEY(myproc, key, PMIX_HOSTNAME, stringdata, l_hostname);
        TEST_ERROR_EXIT(
            ("Check input for case PMIX_STRING: key: %s stringdata: %s", key, stringdata));
    }
    default: {
        TEST_ERROR_EXIT(("No test logic for type: %d, key: %s", value->type, key));
    }
    }
}
