/*
 * Copyright (c) 2013-2017 Intel, Inc. All rights reserved.
 * Copyright (c) 2015      Artem Y. Polyakov <artpol84@gmail.com>.
 *                         All rights reserved.
 * Copyright (c) 2015-2018 Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2020      Triad National Security, LLC
 *                         All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

/* This file includes all functions called directly by client apps, along with
 * their callees. */

#include <src/include/pmix_config.h>
#include <pmix_common.h>

#include "test_common.h"
#include <stdarg.h>
#include <stdio.h>
#include <ctype.h>

extern int pmix_test_verbose;
extern test_params params;

extern FILE *file;

// presently a placeholder - to be developed into a client-only command parser,
// separating the processing logic for client command line options from that for
// other callers (test app or server), unlike how parse_cmd() presently works.
void parse_cmd_client(int argc, char **argv, test_params *params, validation_params *v_params)
{
    ;
}

void pmixt_pre_init(int argc, char **argv, test_params *params, validation_params *v_params) {

    ssize_t v_size = -1;

    default_params(params, v_params);
    parse_cmd(argc, argv, params, v_params);
    // separate out parse_cmd_server and parse_cmd_client to simplify understanding
    //parse_cmd_client(argc, argv, params, v_params);

    if (v_params->validate_params) {
        v_size = pmixt_decode(v_params_ascii_str, v_params, sizeof(*v_params));
        if (v_size != sizeof(*v_params)) {
            assert(v_size == sizeof(*v_params));
            exit(1);
        }
        // immediately parse out rank placement if it was provided in validation params
        if (v_params->custom_rank_placement){
            TEST_VERBOSE(("v_params->pmix_num_nodes: %d being passed into init_nodes", v_params->pmix_num_nodes));
            init_nodes(v_params->pmix_num_nodes);
            char *local_rank_placement_string = NULL;
            TEST_VERBOSE(("Before parse_rank_placement_string, string: %s", v_params->rank_placement_string));
            local_rank_placement_string = strdup(v_params->rank_placement_string);
            parse_rank_placement_string(local_rank_placement_string, v_params->pmix_num_nodes);
            free(local_rank_placement_string);
        }
    }
    else {
        // we're not doing any parameter validation - is this reasonable?
        TEST_VERBOSE(("Parameter validation disabled\n"));
    }

    /* set filename if available in params */
    if ( NULL != params->prefix && -1 != params->ns_id ) {
	char *fname = malloc( strlen(params->prefix) + MAX_DIGIT_LEN + 2 );
        sprintf(fname, "%s.%d.%d", params->prefix, params->ns_id, v_params->pmix_rank);
        file = fopen(fname, "w");
        free(fname);
        if( NULL == file ){
            fprintf(stderr, "Cannot open file %s for writing!", fname);
            exit(1);
        }
    }
    else {
        file = stdout;
    }
}

void pmixt_fix_rank_and_ns(pmix_proc_t *this_proc, test_params *params, validation_params *v_params) {
    // first check for bugs
    if (this_proc->rank != v_params->pmix_rank) {
        TEST_ERROR(("Client ns: v_params %s; this_proc: %s, Rank returned in PMIx_Init: %d does not match validation rank: %d.",
	    v_params->pmix_nspace, this_proc->nspace, this_proc->rank, v_params->pmix_rank));
        if( (stdout != file) && (stderr != file) ) {
            fclose(file);
        }
        exit(1);
    }
    // Fix rank if running under RM
    if( PMIX_RANK_UNDEF == v_params->pmix_rank ){
        char *ranklist = getenv("SLURM_GTIDS");
        char *rankno = getenv("SLURM_LOCALID");
        // Fix rank if running under SLURM
        if( NULL != ranklist && NULL != rankno ){
            char **argv = pmix_argv_split(ranklist, ',');
            int count = pmix_argv_count(argv);
            int rankidx = strtoul(rankno, NULL, 10);
            if( rankidx >= count ){
                fprintf(stderr, "It feels like we are running under SLURM:\n\t"
                        "SLURM_GTIDS=%s, SLURM_LOCALID=%s\nbut env vars are conflicting\n",
                        ranklist, rankno);
                if( (stdout != file) && (stderr != file) ) {
                    fclose(file);
                }
                exit(1);
            }
            v_params->pmix_rank = strtoul(argv[rankidx], NULL, 10);
            pmix_argv_free(argv);
        }
        else if ( NULL == getenv("PMIX_RANK") ) { /* we must not be running under SLURM */
            // **Call separate function for your own resource manager here**
            // It should likely be wrapped in condition that checks for existence of an RM env var
            // Function name should be of form: fix_rank_and_nspace_rm_*
            // and should check/fix both rank and nspace
            // e.g: fix_rank_and_ns_rm_pbs(), fix_rank_and_ns_rm_torque(), etc.
        }
        else { /* unknown situation - PMIX_RANK is not null but SLURM env vars are */
            fprintf(stderr, "It feels like we are running under SLURM:\n\t"
                    "PMIX_RANK=%s\nbut SLURM env vars are null\n"
                    "val_params.pmix_rank = %d, this_proc.rank = %d, custom_rank_placement = %d pmix_local_peers = %s\n",
                    getenv("PMIX_RANK"), v_params->pmix_rank, this_proc->rank, v_params->custom_rank_placement, v_params->pmix_local_peers);
            if( (stdout != file) && (stderr != file) ) {
                  fclose(file);
            }
            exit(1);
        }
    }

    // Fix namespace if running under RM
    if( NULL == params->nspace ){
        char *nspace = getenv("PMIX_NAMESPACE");
        if( NULL != nspace ){
            params->nspace = strdup(nspace);
        }
	else { /* If we aren't running under SLURM, you should have set nspace
	          in your custom fix_rank_and_ns_rm_* function! */
            fprintf(stderr, "nspace not set. Is the fix_rank_and_ns_rm_*"
	            " function for this resource manager failing to set it?\n");
            if( (stdout != file) && (stderr != file) ) {
                fclose(file);
            }
            exit(1);
        }
    }
}

void pmixt_post_init(pmix_proc_t *this_proc, test_params *params, validation_params *val_params) {
    pmixt_fix_rank_and_ns(this_proc, params, val_params);
    TEST_VERBOSE((" Client/PMIX ns %s rank %d: PMIx_Init success", this_proc->nspace, this_proc->rank));
}

void pmixt_post_finalize(pmix_proc_t *this_proc, test_params *params, validation_params *v_params) {
    free_params(params, v_params);
    TEST_VERBOSE(("Client ns %s rank %d: PMIx_Finalize success", this_proc->nspace, this_proc->rank));
    TEST_VERBOSE(("Before fclose (after file closed, no more output can be printed)"));
    if( (stdout != file) && (stderr != file) ) {
        if ( fclose(file) ) {
            exit(errno);
        }
    }
    exit(EXIT_SUCCESS);
}

/* This function is used to validate values returned from calls to PMIx_Get against
   side channel validation data */
void pmixt_validate_predefined(bool validate_self, pmix_proc_t *myproc, const pmix_key_t key,
                               pmix_value_t *value, const pmix_data_type_t expected_type,
                               validation_params *val_params)
{
    pmix_status_t rc = PMIX_ERROR;
    pmix_rank_t peer_rank = -1;
    uint16_t save_local_rank, save_node_rank;
    uint32_t save_local_size, save_nodeid;
    char save_hostname[PMIX_MAX_KEYLEN];
    bool rank_found = false;
    int i, j;

    if (val_params->validate_params) {
        if (expected_type != value->type) {
            TEST_ERROR(("Type mismatch for key. Key: %s Type: %u Expected type: %u",
                    key, value->type, expected_type));
            exit(1);
        }
        // if we're not validating our own process information, we are validating a peer's info,
        // so we need to examine the nodes array to find the relevant validation info
        if (!validate_self) {
            peer_rank = myproc->rank;
            for (i = 0; i < val_params->pmix_num_nodes; i++) {
                for (j = 0; j < nodes[i].pmix_local_size; j++) {
                    if (nodes[i].pmix_rank[j] == peer_rank) {
                        save_local_rank = val_params->pmix_local_rank;
                        save_node_rank = val_params->pmix_node_rank;
                        save_local_size = val_params->pmix_local_size;
                        save_nodeid = val_params->pmix_nodeid;
                        strcpy(save_hostname, val_params->pmix_hostname);

                        val_params->pmix_local_rank = j;
                        // if we test a multi-job case later, node_rank will not equal local_rank
                        // and the assignment below will have to be changed
                        val_params->pmix_node_rank = j;
                        val_params->pmix_local_size = nodes[i].pmix_local_size;
                        val_params->pmix_nodeid = nodes[i].pmix_nodeid;
                        strcpy(val_params->pmix_hostname, nodes[i].pmix_hostname);
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
            case PMIX_UINT16:
                //empty statement to separate label and declaration
                ;
                uint16_t uint16data;
                PMIX_VALUE_GET_NUMBER(rc, value, uint16data, uint16_t);
                if (PMIX_SUCCESS != rc) {
                    TEST_ERROR(("Failed to retrieve value correctly. Key: %s Type: %u",
                    key, value->type));
                    exit(1);
                }
                if (PMIXT_CHECK_KEY(key, PMIX_LOCAL_RANK) && val_params->check_pmix_local_rank) {
                    if (val_params->pmix_local_rank != uint16data) {
                        TEST_ERROR(("Key %s failed validation. Get value: %d Validation value: %d",
                                     key, uint16data, val_params->pmix_local_rank));
                        exit(1);
                    }
                    TEST_VERBOSE(("Namespace %s: Rank %d: Data validated: Key: %s Value: %u",
                        myproc->nspace, myproc->rank, key, uint16data));
                }
                else if (PMIXT_CHECK_KEY(key, PMIX_NODE_RANK) && val_params->check_pmix_node_rank) {
                    if (val_params->pmix_node_rank != uint16data) {
                        TEST_ERROR(("Key %s failed validation. Get value: %d Validation value: %d",
                                     key, uint16data, val_params->pmix_node_rank));
                        exit(1);
                    }
                    TEST_VERBOSE(("Namespace %s: Rank %d: Data validated: Key: %s Value: %u",
                        myproc->nspace, myproc->rank, key, uint16data));
                } // other possibilities will be added here later
                else {
                   TEST_ERROR(("Check input for case PMIX_UINT16: key: %s check_pmix_local_rank %u",
                       key, val_params->check_pmix_local_rank));
                    exit(1);
                }
                break;
            case PMIX_UINT32:
                //empty statement to separate label and declaration
                ;
                uint32_t uint32data;
                PMIX_VALUE_GET_NUMBER(rc, value, uint32data, uint32_t);
                if (PMIX_SUCCESS != rc) {
                    TEST_ERROR(("Failed to retrieve value correctly. Key: %s Type: %u",
                    key, value->type));
                    exit(1);
                }
                if (PMIXT_CHECK_KEY(key, PMIX_JOB_SIZE) && val_params->check_pmix_job_size) {
                    if (val_params->pmix_job_size != uint32data) {
                        TEST_ERROR(("Key %s failed validation. Get value: %d Validation value: %d",
                                     key, uint32data, val_params->pmix_job_size));
                        exit(1);
                    }
                    TEST_VERBOSE(("Namespace %s: Rank %d: Data validated: Key: %s Value: %u",
                        myproc->nspace, myproc->rank, key, uint32data));
                }
                else if (PMIXT_CHECK_KEY(key, PMIX_UNIV_SIZE) && val_params->check_pmix_univ_size) {
                    if (val_params->pmix_univ_size != uint32data) {
                        TEST_ERROR(("Key %s failed validation. Get value: %d Validation value: %d",
                                     key, uint32data, val_params->pmix_univ_size));
                        exit(1);
                    }
                    TEST_VERBOSE(("Namespace %s: Rank %d: Data validated: Key: %s Value: %u",
                        myproc->nspace, myproc->rank, key, uint32data));
                }
                else if (PMIXT_CHECK_KEY(key, PMIX_LOCAL_SIZE) && val_params->check_pmix_local_size) {
                    if (val_params->pmix_local_size != uint32data) {
                        TEST_ERROR(("Key %s failed validation. Get value: %d Validation value: %d",
                                     key, uint32data, val_params->pmix_local_size));
                        exit(1);
                    }
                    TEST_VERBOSE(("Namespace %s: Rank %d: Data validated: Key: %s Value: %u",
                        myproc->nspace, myproc->rank, key, uint32data));
                }
                else if (PMIXT_CHECK_KEY(key, PMIX_NODEID) && val_params->check_pmix_nodeid) {
                    if (val_params->pmix_nodeid != uint32data) {
                        TEST_ERROR(("Key %s failed validation. Get value: %d Validation value: %d",
                                     key, uint32data, val_params->pmix_nodeid));
                        exit(1);
                    }
                    TEST_VERBOSE(("Namespace %s: Rank %d: Data validated: Key: %s Value: %u",
                        myproc->nspace, myproc->rank, key, uint32data));
                } // other possibilities will be added here later
                else {
                   TEST_ERROR(("Check input for case PMIX_UINT32: key: %s check_pmix_job_size: %u check_pmix_univ_size %u check_pmix_local_size %u",
                        key, val_params->check_pmix_job_size, val_params->check_pmix_univ_size, val_params->check_pmix_local_size));
                    exit(1);
                }
                break;
            case PMIX_PROC_RANK:
                //empty statement to separate label and declaration
                ;
                pmix_rank_t rankdata;
                PMIX_VALUE_GET_NUMBER(rc, value, rankdata, pmix_rank_t);
                if (PMIX_SUCCESS != rc) {
                    TEST_ERROR(("Failed to retrieve value correctly. Key: %s Type: %u",
                    key, value->type));
                    exit(1);
                }
                if (PMIXT_CHECK_KEY(key, PMIX_RANK) && val_params->check_pmix_rank) {
                    if (val_params->pmix_rank != rankdata) {
                        TEST_ERROR(("Key %s failed validation. Get value: %d Validation value: %d",
                                     key, rankdata, val_params->pmix_rank));
                        exit(1);
                    }
                    TEST_VERBOSE(("Namespace %s: Rank %d Data validated: Key: %s Value: %u",
                        myproc->nspace, myproc->rank, key, rankdata));
                } // other possibilities will be added here later
                else {
                    TEST_ERROR(("Check input for case PMIX_PROC_RANK: key: %s check_pmix_rank: %u",
                        key, val_params->check_pmix_rank));
                    exit(1);
                }
                break;
            case PMIX_STRING:
                //empty statement to separate label and declaration
                ;
                void *stringdata;
                size_t lsize;
                PMIX_VALUE_UNLOAD(rc, value, &stringdata, &lsize);
                if (PMIX_SUCCESS != rc) {
                    TEST_ERROR(("Failed to retrieve value correctly. Key: %s Type: %u",
                    key, value->type));
                    exit(1);
                }
                if (PMIXT_CHECK_KEY(key, PMIX_NSPACE) && val_params->check_pmix_nspace) {
                    if (0 != strcmp(val_params->pmix_nspace, stringdata)) {
                        TEST_ERROR(("Key %s failed validation. Get value: %s Validation value: %s",
                                     key, stringdata, val_params->pmix_nspace));
                        exit(1);
                    }
                    TEST_VERBOSE(("Namespace %s: Rank %d Data validated: Key: %s Value: %s",
                        myproc->nspace, myproc->rank, key, stringdata));
                }
                else if (PMIXT_CHECK_KEY(key, PMIX_JOBID) && val_params->check_pmix_jobid) {
                    if (0 != strcmp(val_params->pmix_jobid, stringdata)) {
                        TEST_ERROR(("Key %s failed validation. Get value: %s Validation value: %s",
                                     key, stringdata, val_params->pmix_jobid));
                        exit(1);
                    }
                    TEST_VERBOSE(("Namespace %s: Rank %d Data validated: Key: %s Value: %s",
                        myproc->nspace, myproc->rank, key, stringdata));
                }
                else if (PMIXT_CHECK_KEY(key, PMIX_LOCAL_PEERS) && val_params->check_pmix_local_peers) {
                    if (0 != strcmp(val_params->pmix_local_peers, stringdata)) {
                        TEST_ERROR(("Key %s failed validation. Get value: %s Validation value: %s",
                                     key, stringdata, val_params->pmix_local_peers));
                        exit(1);
                    }
                    TEST_VERBOSE(("Namespace %s: Rank %d Data validated: Key: %s Value: %s",
                        myproc->nspace, myproc->rank, key, stringdata));
                }
                else if (PMIXT_CHECK_KEY(key, PMIX_HOSTNAME) && val_params->check_pmix_hostname) {
                    if (0 != strcmp(val_params->pmix_hostname, stringdata)) {
                        TEST_ERROR(("Key %s failed validation. Get value: %s Validation value: %s",
                                     key, stringdata, val_params->pmix_hostname));
                        exit(1);
                    }
                    TEST_VERBOSE(("Namespace %s: Rank %d Data validated: Key: %s Value: %s",
                        myproc->nspace, myproc->rank, key, stringdata));
                } // other possibilities will be added here later
                else {
                    TEST_ERROR(("Check input for case PMIX_STRING: key: %s stringdata: %s",
                        key, stringdata));
                    exit(1);
                }
                free(stringdata);
                break;
            default:
                TEST_ERROR(("No test logic for type: %d, key: %s", value->type, key));
                exit(1);
        }
    }
    else {
        TEST_VERBOSE(("All validation disabled, will not validate: %s", key));
    }
    if (!validate_self) {
        val_params->pmix_local_rank = save_local_rank;
        val_params->pmix_node_rank = save_node_rank;
        val_params->pmix_local_size = save_local_size;
        val_params->pmix_nodeid = save_nodeid;
        strcpy(val_params->pmix_hostname, save_hostname);
    }
}