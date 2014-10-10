/*
 * PMIx copyrights:
 * Copyright (c) 2013      Intel, Inc. All rights reserved
 */
/*****************************************************************************\
 *  FOR BACKWARD COMPATIBILITY, SOME PORTIONS OF THE FOLLOWING DEFINITIONS ARE
 *  TAKEN FROM THE PMI and PMI-2 HEADERS AS DEFINED UNDER THE FOLLOWING COPYRIGHT:
 *
 *  COPYRIGHT
 *
 *  The following is a notice of limited availability of the code, and
 *  disclaimer which must be included in the prologue of the code and in all
 *  source listings of the code.
 *
 *  Copyright Notice + 2002 University of Chicago
 *
 *  Permission is hereby granted to use, reproduce, prepare derivative
 *  works, and to redistribute to others. This software was authored by:
 *
 *  Argonne National Laboratory Group
 *  W. Gropp: (630) 252-4318; FAX: (630) 252-5986; e-mail: gropp@mcs.anl.gov
 *  E. Lusk: (630) 252-7852; FAX: (630) 252-5986; e-mail: lusk@mcs.anl.gov
 *  Mathematics and Computer Science Division Argonne National Laboratory,
 *  Argonne IL 60439
 *
 *  GOVERNMENT LICENSE
 *
 *  Portions of this material resulted from work developed under a U.S.
 *  Government Contract and are subject to the following license: the
 *  Government is granted for itself and others acting on its behalf a
 *  paid-up, nonexclusive, irrevocable worldwide license in this computer
 *  software to reproduce, prepare derivative works, and perform publicly
 *  and display publicly.
 *
 *  DISCLAIMER
 *
 *  This computer code material was prepared, in part, as an account of work
 *  sponsored by an agency of the United States Government. Neither the
 *  United States, nor the University of Chicago, nor any of their
 *  employees, makes any warranty express or implied, or assumes any legal
 *  liability or responsibility for the accuracy, completeness, or
 *  usefulness of any information, apparatus, product, or process disclosed,
 *  or represents that its use would not infringe privately owned rights.
 *
 *  MCS Division <http://www.mcs.anl.gov>        Argonne National Laboratory
 *  <http://www.anl.gov>     University of Chicago <http://www.uchicago.edu>
\*****************************************************************************/

#ifndef PMIx_H
#define PMIx_H

BEGIN_C_DECLS

/*D
PMI_CONSTANTS - PMI definitions

Error Codes:
+ PMI_SUCCESS - operation completed successfully
. PMI_FAIL - operation failed
. PMI_ERR_NOMEM - input buffer not large enough
. PMI_ERR_INIT - PMI not initialized
. PMI_ERR_INVALID_ARG - invalid argument
. PMI_ERR_INVALID_KEY - invalid key argument
. PMI_ERR_INVALID_KEY_LENGTH - invalid key length argument
. PMI_ERR_INVALID_VAL - invalid val argument
. PMI_ERR_INVALID_VAL_LENGTH - invalid val length argument
. PMI_ERR_INVALID_LENGTH - invalid length argument
. PMI_ERR_INVALID_NUM_ARGS - invalid number of arguments
. PMI_ERR_INVALID_ARGS - invalid args argument
. PMI_ERR_INVALID_NUM_PARSED - invalid num_parsed length argument
. PMI_ERR_INVALID_KEYVALP - invalid keyvalp argument
- PMI_ERR_INVALID_SIZE - invalid size argument

Booleans:
+ PMI_TRUE - true
- PMI_FALSE - false

D*/
#define PMI_SUCCESS                  0
#define PMI_FAIL                    -1
#define PMI_ERR_INIT                 1
#define PMI_ERR_NOMEM                2
#define PMI_ERR_INVALID_ARG          3
#define PMI_ERR_INVALID_KEY          4
#define PMI_ERR_INVALID_KEY_LENGTH   5
#define PMI_ERR_INVALID_VAL          6
#define PMI_ERR_INVALID_VAL_LENGTH   7
#define PMI_ERR_INVALID_LENGTH       8
#define PMI_ERR_INVALID_NUM_ARGS     9
#define PMI_ERR_INVALID_ARGS        10
#define PMI_ERR_INVALID_NUM_PARSED  11
#define PMI_ERR_INVALID_KEYVALP     12
#define PMI_ERR_INVALID_SIZE        13
#define PMI_ERR_INVALID_KVS         14

/* PMI2 definitions are mirrors of the above */
#define PMI2_SUCCESS                PMI_SUCCESS
#define PMI2_FAIL                   PMI_FAIL
#define PMI2_ERR_INIT               PMI_ERR_INIT
#define PMI2_ERR_NOMEM              PMI_ERR_NOMEM
#define PMI2_ERR_INVALID_ARG        PMI_ERR_INVALID_ARG
#define PMI2_ERR_INVALID_KEY        PMI_ERR_INVALID_KEY
#define PMI2_ERR_INVALID_KEY_LENGTH PMI_ERR_INVALID_KEY_LENGTH
#define PMI2_ERR_INVALID_VAL        PMI_ERR_INVALID_VAL
#define PMI2_ERR_INVALID_VAL_LENGTH PMI_ERR_INVALID_VAL_LENGTH
#define PMI2_ERR_INVALID_LENGTH     PMI_ERR_INVALID_LENGTH
#define PMI2_ERR_INVALID_NUM_ARGS   PMI_ERR_INVALID_NUM_ARGS
#define PMI2_ERR_INVALID_ARGS       PMI_ERR_INVALID_ARGS
#define PMI2_ERR_INVALID_NUM_PARSED PMI_ERR_INVALID_NUM_PARSED
#define PMI2_ERR_INVALID_KEYVALP    PMI_ERR_INVALID_KEYVALP
#define PMI2_ERR_INVALID_SIZE       PMI_ERR_INVALID_SIZE
#define PMI2_ERR_OTHER              PMI_ERR_INVALID_KVS

/* for compatibility, provide same names for PMIx */
#define PMIx_SUCCESS                PMI_SUCCESS
#define PMIx_FAIL                   PMI_FAIL
#define PMIx_ERR_INIT               PMI_ERR_INIT
#define PMIx_ERR_NOMEM              PMI_ERR_NOMEM
#define PMIx_ERR_INVALID_ARG        PMI_ERR_INVALID_ARG
#define PMIx_ERR_INVALID_KEY        PMI_ERR_INVALID_KEY
#define PMIx_ERR_INVALID_KEY_LENGTH PMI_ERR_INVALID_KEY_LENGTH
#define PMIx_ERR_INVALID_VAL        PMI_ERR_INVALID_VAL
#define PMIx_ERR_INVALID_VAL_LENGTH PMI_ERR_INVALID_VAL_LENGTH
#define PMIx_ERR_INVALID_LENGTH     PMI_ERR_INVALID_LENGTH
#define PMIx_ERR_INVALID_NUM_ARGS   PMI_ERR_INVALID_NUM_ARGS
#define PMIx_ERR_INVALID_ARGS       PMI_ERR_INVALID_ARGS
#define PMIx_ERR_INVALID_NUM_PARSED PMI_ERR_INVALID_NUM_PARSED
#define PMIx_ERR_INVALID_KEYVALP    PMI_ERR_INVALID_KEYVALP
#define PMIx_ERR_INVALID_SIZE       PMI_ERR_INVALID_SIZE
#define PMIx_ERR_OTHER              PMI_ERR_INVALID_KVS

typedef int PMI_BOOL;
#define PMI_TRUE     1
#define PMI_FALSE    0

/* define some maximum sizes */
#define PMIX_MAX_VALLEN   1024
#define PMIX_MAX_INFO_KEY  255
#define PMIX_MAX_INFO_VAL 1024

/* define an INFO object corresponding to
 * the MPI_Info structure */
typedef struct {
    char key[PMIX_MAX_INFO_KEY];
    char value[PMIX_MAX_INFO_VAL];
} pmix_info_t;

/* define a scope for data "put" by PMI per the following:
 *
 * PMI_LOCAL - the data is intended only for other application
 *             processes on the same node. Data marked in this way
 *             will not be included in data packages sent to remote requestors
 * PMI_REMOTE - the data is intended solely for applications processes on
 *              remote nodes. Data marked in this way will not be shared with
 *              other processes on the same node
 * PMI_GLOBAL - the data is to be shared with all other requesting processes,
 *              regardless of location
 */
typedef uint8_t pmix_scope_t;
#define PMIX_SCOPE_T PMIX_UINT8
#define PMIX_SCOPE_UNDEF  0
#define PMIX_INTERNAL     1  // data used internally only
#define PMIX_LOCAL        2  // share to procs also on this node
#define PMIX_REMOTE       3  // share with procs not on this node
#define PMIX_GLOBAL       4  // share with all procs (local + remote)

/* callback function for non-blocking operations */
typedef void (*pmix_cbfunc_t)(int status, pmix_value_t *kv, void *cbdata);

/* flags to indicate if the modex value being pushed into
 * the PMIx server comes from an element that is ready to
 * support async modex operations, or from one that requires
 * synchronous modex (i.e., blocking modex operation) */
#define PMIX_SYNC_REQD  true
#define PMIX_ASYNC_RDY  false

/* define a set of "standard" PMIx attributes that can
 * be queried. Implementations (and users) are free to extend as
 * desired, so the get_attr functions need to be capable
 * of handling the "not found" condition. Note that these
 * are attributes of the system and the job as opposed to
 * values the application (or underlying MPI library)
 * might choose to expose - i.e., they are values provided
 * by the resource manager as opposed to the application */
#define PMIX_ATTR_UNDEF      NULL

#define PMIX_CPUSET          "pmix.cpuset"      // (char*) hwloc bitmap applied to proc upon launch
#define PMIX_CREDENTIAL      "pmix.cred"        // (opal_byte_object*) security credential assigned to proc
#define PMIX_HOSTNAME        "pmix.hname"       // (char*) name of the host this proc is on
/* scratch directory locations for use by applications */
#define PMIX_TMPDIR          "pmix.tmpdir"      // (char*) top-level tmp dir assigned to session
/* information about relative ranks as assigned */
#define PMIX_JOBID           "pmix.jobid"       // (char*) jobid assigned by scheduler
#define PMIX_APPNUM          "pmix.appnum"      // (uint32_t) app number within the job
#define PMIX_RANK            "pmix.rank"        // (uint32_t) process rank within the job
#define PMIX_GLOBAL_RANK     "pmix.grank"       // (uint32_t) rank spanning across all jobs in this session
#define PMIX_APP_RANK        "pmix.apprank"     // (uint32_t) rank within this app
#define PMIX_NPROC_OFFSET    "pmix.offset"      // (uint32_t) starting global rank of this job
#define PMIX_LOCAL_RANK      "pmix.lrank"       // (uint16_t) rank on this node within this job
#define PMIX_NODE_RANK       "pmix.nrank"       // (uint16_t) rank on this node spanning all jobs
#define PMIX_LOCALLDR        "pmix.lldr"        // (uint64_t) opal_identifier of lowest rank on this node within this job
#define PMIX_APPLDR          "pmix.aldr"        // (uint32_t) lowest rank in this app within this job
/* proc location-related info */
#define PMIX_PROC_MAP        "pmix.map"         // (byte_object) packed map of proc locations within this job
#define PMIX_LOCAL_PEERS     "pmix.lpeers"      // (char*) comma-delimited string of ranks on this node within this job
#define PMIX_LOCAL_CPUSETS   "pmix.lcpus"       // (byte_object) packed names and cpusets of local peers
/* size info */
#define PMIX_UNIV_SIZE       "pmix.univ.size"   // (uint32_t) #procs in this namespace
#define PMIX_JOB_SIZE        "pmix.job.size"    // (uint32_t) #procs in this job
#define PMIX_LOCAL_SIZE      "pmix.local.size"  // (uint32_t) #procs in this job on this node
#define PMIX_NODE_SIZE       "pmix.node.size"   // (uint32_t) #procs across all jobs on this node
#define PMIX_MAX_PROCS       "pmix.max.size"    // (uint32_t) max #procs for this job
/* topology info */
#define PMIX_NET_TOPO        "pmix.ntopo"       // (byte_object) network topology
#define PMIX_LOCAL_TOPO      "pmix.ltopo"       // (hwloc topo) local node topology

/**
 * Provide a simplified macro for sending data via modex
 * to other processes. The macro requires four arguments:
 *
 * r - the integer return status from the modex op
 * f - whether this modex requires sync or is async ready
 * sc - the PMIX scope of the data
 * s - the key to tag the data being posted
 * d - the data object being posted
 * sz - the number of bytes in the data object
 */
#define OPAL_MODEX_SEND_STRING(r, f, sc, s, d, sz)              \
    do {                                                        \
        opal_value_t kv;                                        \
        if (PMIX_SYNC_REQD == (f)) {                            \
            opal_pmix_use_collective = true;                    \
        }                                                       \
        OBJ_CONSTRUCT(&kv, opal_value_t);                       \
        kv.key = (s);                                           \
        kv.type = OPAL_BYTE_OBJECT;                             \
        kv.data.bo.bytes = (uint8_t*)(d);                       \
        kv.data.bo.size = (sz);                                 \
        if (OPAL_SUCCESS != ((r) = opal_pmix.put(sc, &kv))) {   \
            OPAL_ERROR_LOG((r));                                \
        }                                                       \
        kv.data.bo.bytes = NULL; /* protect the data */         \
        kv.key = NULL;  /* protect the key */                   \
        OBJ_DESTRUCT(&kv);                                      \
    } while(0);

/**
 * Provide a simplified macro for sending data via modex
 * to other processes. The macro requires four arguments:
 *
 * r - the integer return status from the modex op
 * f - whether this modex requires sync or is async ready
 * sc - the PMIX scope of the data
 * s - the MCA component that is posting the data
 * d - the data object being posted
 * sz - the number of bytes in the data object
 */
#define OPAL_MODEX_SEND(r, f, sc, s, d, sz)                     \
    do {                                                        \
        char *key;                                              \
        if (PMIX_SYNC_REQD == (f)) {                            \
            opal_pmix_use_collective = true;                    \
        }                                                       \
        key = mca_base_component_to_string((s));                \
        OPAL_MODEX_SEND_STRING((r), (f), (sc), key, (d), (sz)); \
        free(key);                                              \
    } while(0);

/**
 * Provide a simplified macro for retrieving modex data
 * from another process:
 *
 * r - the integer return status from the modex op (int)
 * s - string key (char*)
 * p - pointer to the opal_proc_t of the proc that posted
 *     the data (opal_proc_t*)
 * d - pointer to a location wherein the data object
 *     it to be returned
 * t - the expected data type
 */
#define OPAL_MODEX_RECV_VALUE(r, s, p, d, t)                            \
    do {                                                                \
        opal_value_t *kv;                                               \
        if (OPAL_SUCCESS != ((r) = opal_pmix.get(&(p)->proc_name,       \
                                                 (s), &kv))) {          \
            *(d) = NULL;                                                \
        } else {                                                        \
            (r) = opal_value_unload(kv, (void**)(d), (t));              \
            OBJ_RELEASE(kv);                                            \
        }                                                               \
    } while(0);

/**
 * Provide a simplified macro for retrieving modex data
 * from another process:
 *
 * r - the integer return status from the modex op (int)
 * s - string key (char*)
 * p - pointer to the opal_proc_t of the proc that posted
 *     the data (opal_proc_t*)
 * d - pointer to a location wherein the data object
 *     it to be returned (char**)
 * sz - pointer to a location wherein the number of bytes
 *     in the data object can be returned (size_t)
 */
#define OPAL_MODEX_RECV_STRING(r, s, p, d, sz)                          \
    do {                                                                \
        opal_value_t *kv;                                               \
        if (OPAL_SUCCESS == ((r) = opal_pmix.get(&(p)->proc_name,       \
                                                 (s), &kv)) &&          \
            NULL != kv) {                                               \
            *(d) = kv->data.bo.bytes;                                   \
            *(sz) = kv->data.bo.size;                                   \
            kv->data.bo.bytes = NULL; /* protect the data */            \
            OBJ_RELEASE(kv);                                            \
        } else {                                                        \
            *(d) = NULL;                                                \
            *(sz) = 0;                                                  \
        }                                                               \
    } while(0);

/**
 * Provide a simplified macro for retrieving modex data
 * from another process:
 *
 * r - the integer return status from the modex op (int)
 * s - the MCA component that posted the data (mca_base_component_t*)
 * p - pointer to the opal_proc_t of the proc that posted
 *     the data (opal_proc_t*)
 * d - pointer to a location wherein the data object
 *     it to be returned (char**)
 * sz - pointer to a location wherein the number of bytes
 *     in the data object can be returned (size_t)
 */
#define OPAL_MODEX_RECV(r, s, p, d, sz)                                 \
    do {                                                                \
        char *key;                                                      \
        key = mca_base_component_to_string((s));                        \
        if (NULL == key) {                                              \
            OPAL_ERROR_LOG(OPAL_ERR_OUT_OF_RESOURCE);                   \
            (r) = OPAL_ERR_OUT_OF_RESOURCE;                             \
        } else {                                                        \
            OPAL_MODEX_RECV_STRING((r), key, (p), (d), (sz));           \
            free(key);                                                  \
        }                                                               \
    } while(0);


/**
 * Provide a simplified macro for calling the fence function
 * that takes into account directives and availability of
 * non-blocking operations
 */
#define OPAL_FENCE(p, s, cf, cd)                                        \
    do {                                                                \
        if (opal_pmix_use_collective || NULL == opal_pmix.fence_nb) {   \
            opal_pmix.fence((p), (s));                                  \
        } else {                                                        \
            opal_pmix.fence_nb((p), (s), (cf), (cd));                   \
        }                                                               \
    } while(0);

/* callback handler for errors */
typedef void (*pmix_errhandler_fn_t)(int error);

/****    DEFINE THE PUBLIC API'S                          ****
 ****    NOTE THAT WE DO NOT HAVE A 1:1 MAPPING OF APIs   ****
 ****    HERE TO THOSE CURRENTLY DEFINED BY PMI AS WE     ****
 ****    DON'T USE SOME OF THOSE FUNCTIONS AND THIS ISN'T ****
 ****    A GENERAL LIBRARY                                ****/

/*****  APIs CURRENTLY USED IN THE OMPI/ORTE CODE BASE   ****/
/* NOTE: calls to these APIs must be thread-protected as there
 * currently is NO internal thread safety. */

/* Init */
int PMIx_Init(PMI_BOOL *spawned);

/* Finalize */
int PMIx_Finalize(void);

/* Initialized */
PMIx_BOOL PMIx_Initialized(void);

/* Abort */
int PMIx_Abort(int flag, const char msg[]);

/* Fence - note that this call is required to commit any
 * data "put" to the system since the last call to "fence"
 * prior to (or as part of) executing the barrier. Serves both PMI2
 * and PMI1 "barrier" purposes */
int PMIx_Fence(pmix_identifier_t *procs, size_t nprocs);

/* Fence_nb - not included in the current PMI standard. This is a non-blocking
 * version of the standard "fence" call. All subsequent "get" calls will block
 * pending completion of this operation. Non-blocking "get" calls will still
 * complete as data becomes available */
int PMIx_Fence_nb(pmix_identifier_t *procs, size_t nprocs,
                  pmix_cbfunc_t cbfunc, void *cbdata);

/* Put - note that this API has been modified from the current PMI standard to
 * reflect the proposed PMIx extensions. */
int PMIx_Put(pmix_scope_t scope, pmix_value_t *kv);

/* Get - note that this API has been modified from the current PMI standard to
 * reflect the proposed PMIx extensions, and to include the process identifier so
 * we can form the PMI key within the active component instead of sprinkling that
 * code all over the code base. */
int PMIx_Get(const pmix_identifier_t *id,
             const char *key,
             pmix_value_t **kv);

/* Get_nb - not included in the current PMI standard. This is a non-blocking
 * version of the standard "get" call. Retrieved value will be provided as
 * opal_value_t object in the callback. We include the process identifier so
 * we can form the PMI key within the active component instead of sprinkling that
 * code all over the code base. */
void PMIx_Get_nb(const pmix_identifier_t *id,
                 const char *key,
                 pmix_cbfunc_t cbfunc,
                 void *cbdata);

/* Publish - the "info" parameter
 * consists of a list of pmix_info_t objects */
int PMIx_Publish(const char service_name[],
                 pmix_list_t *info,
                 const char port[]);

/* Lookup - the "info" parameter
 * consists of a list of pmix_info_t objects */
int PMIx_Lookup(const char service_name[],
                pmix_list_t *info,
                char port[], int portLen);

/* Unpublish - the "info" parameter
 * consists of a list of pmix_info_t objects */
int PMIx_Unpublish(const char service_name[], 
                   pmix_list_t *info);

/* Get attribute
 * Query the server for the specified attribute, returning it in the
 * provided opal_value_t. The function will return "true" if the attribute
 * is found, and "false" if not.
 * Attributes are provided by the PMIx server, so there is no corresponding
 * "put" function. */
PMIx_BOOL PMIx_Get_attr(const char *attr, pmix_value_t **kv);

/* Get attribute (non-blocking)
 * Query the server for the specified attribute..
 * Attributes are provided by the PMIx server, so there is no corresponding "put"
 * function. The call will be executed as non-blocking, returning immediately,
 * with data resulting from the call returned in the callback function. A returned
 * NULL opal_value_t* indicates that the attribute was not found. The returned
 * pointer is "owned" by the PMIx module and must not be released by the
 * callback function */
int PMIx_Get_attr_nb(const char *attr,
                     pmix_cbfunc_t cbfunc,
                     void *cbdata);

/* register an errhandler to report loss of connection to the server */
void PMIx_Register_errhandler(pmix_errhandler_fn_t errhandler);

/* deregister the errhandler */
void PMIx_Deregister_errhandler(void);

/*****************************************************************************\
 *            PMI  FUNCTIONS
\*****************************************************************************/

/*@
PMI_Init - initialize the Process Manager Interface

Output Parameter:
. spawned - spawned flag

Return values:
+ PMI_SUCCESS - initialization completed successfully
. PMI_ERR_INVALID_ARG - invalid argument
- PMI_FAIL - initialization failed

Notes:
Initialize PMI for this process group. The value of spawned indicates whether
this process was created by 'PMI_Spawn_multiple'.  'spawned' will be 'PMI_TRUE' if
this process group has a parent and 'PMI_FALSE' if it does not.

@*/
int PMI_Init( int *spawned );

/*@
PMI_Initialized - check if PMI has been initialized

Output Parameter:
. initialized - boolean value

Return values:
+ PMI_SUCCESS - initialized successfully set
. PMI_ERR_INVALID_ARG - invalid argument
- PMI_FAIL - unable to set the variable

Notes:
On successful output, initialized will either be 'PMI_TRUE' or 'PMI_FALSE'.

+ PMI_TRUE - initialize has been called.
- PMI_FALSE - initialize has not been called or previously failed.

@*/
int PMI_Initialized( PMI_BOOL *initialized );

/*@
PMI_Finalize - finalize the Process Manager Interface

Return values:
+ PMI_SUCCESS - finalization completed successfully
- PMI_FAIL - finalization failed

Notes:
 Finalize PMI for this process group.

@*/
int PMI_Finalize( void );

/*@
PMI_Get_size - obtain the size of the process group

Output Parameters:
. size - pointer to an integer that receives the size of the process group

Return values:
+ PMI_SUCCESS - size successfully obtained
. PMI_ERR_INVALID_ARG - invalid argument
- PMI_FAIL - unable to return the size

Notes:
This function returns the size of the process group to which the local process
belongs.

@*/
int PMI_Get_size( int *size );

/*@
PMI_Get_rank - obtain the rank of the local process in the process group

Output Parameters:
. rank - pointer to an integer that receives the rank in the process group

Return values:
+ PMI_SUCCESS - rank successfully obtained
. PMI_ERR_INVALID_ARG - invalid argument
- PMI_FAIL - unable to return the rank

Notes:
This function returns the rank of the local process in its process group.

@*/
int PMI_Get_rank( int *rank );

/*@
PMI_Get_universe_size - obtain the universe size

Output Parameters:
. size - pointer to an integer that receives the size

Return values:
+ PMI_SUCCESS - size successfully obtained
. PMI_ERR_INVALID_ARG - invalid argument
- PMI_FAIL - unable to return the size


@*/
int PMI_Get_universe_size( int *size );

/*@
PMI_Get_appnum - obtain the application number

Output parameters:
. appnum - pointer to an integer that receives the appnum

Return values:
+ PMI_SUCCESS - appnum successfully obtained
. PMI_ERR_INVALID_ARG - invalid argument
- PMI_FAIL - unable to return the size


@*/
int PMI_Get_appnum( int *appnum );

/*@
PMI_Publish_name - publish a name 

Input parameters:
. service_name - string representing the service being published
. port - string representing the port on which to contact the service

Return values:
+ PMI_SUCCESS - port for service successfully published
. PMI_ERR_INVALID_ARG - invalid argument
- PMI_FAIL - unable to publish service


@*/
int PMI_Publish_name( const char service_name[], const char port[] );

/*@
PMI_Unpublish_name - unpublish a name

Input parameters:
. service_name - string representing the service being unpublished

Return values:
+ PMI_SUCCESS - port for service successfully published
. PMI_ERR_INVALID_ARG - invalid argument
- PMI_FAIL - unable to unpublish service


@*/
int PMI_Unpublish_name( const char service_name[] );

/*@
PMI_Lookup_name - lookup a service by name

Input parameters:
. service_name - string representing the service being published

Output parameters:
. port - string representing the port on which to contact the service

Return values:
+ PMI_SUCCESS - port for service successfully obtained
. PMI_ERR_INVALID_ARG - invalid argument
- PMI_FAIL - unable to lookup service


@*/
int PMI_Lookup_name( const char service_name[], char port[] );

/*@
PMI_Get_id - obtain the id of the process group

Input Parameter:
. length - length of the id_str character array

Output Parameter:
. id_str - character array that receives the id of the process group

Return values:
+ PMI_SUCCESS - id successfully obtained
. PMI_ERR_INVALID_ARG - invalid rank argument
. PMI_ERR_INVALID_LENGTH - invalid length argument
- PMI_FAIL - unable to return the id

Notes:
This function returns a string that uniquely identifies the process group
that the local process belongs to.  The string passed in must be at least
as long as the number returned by 'PMI_Get_id_length_max()'.

@*/
int PMI_Get_id( char id_str[], int length );

/*@
PMI_Get_kvs_domain_id - obtain the id of the PMI domain

Input Parameter:
. length - length of id_str character array

Output Parameter:
. id_str - character array that receives the id of the PMI domain

Return values:
+ PMI_SUCCESS - id successfully obtained
. PMI_ERR_INVALID_ARG - invalid argument
. PMI_ERR_INVALID_LENGTH - invalid length argument
- PMI_FAIL - unable to return the id

Notes:
This function returns a string that uniquely identifies the PMI domain
where keyval spaces can be shared.  The string passed in must be at least
as long as the number returned by 'PMI_Get_id_length_max()'.

@*/
int PMI_Get_kvs_domain_id( char id_str[], int length );

/*@
PMI_Get_id_length_max - obtain the maximum length of an id string

Output Parameters:
. length - the maximum length of an id string

Return values:
+ PMI_SUCCESS - length successfully set
. PMI_ERR_INVALID_ARG - invalid argument
- PMI_FAIL - unable to return the maximum length

Notes:
This function returns the maximum length of a process group id string.

@*/
int PMI_Get_id_length_max( int *length );

/*@
PMI_Barrier - barrier across the process group

Return values:
+ PMI_SUCCESS - barrier successfully finished
- PMI_FAIL - barrier failed

Notes:
This function is a collective call across all processes in the process group
the local process belongs to.  It will not return until all the processes
have called 'PMI_Barrier()'.

@*/
int PMI_Barrier( void );

/*@
PMI_Get_clique_size - obtain the number of processes on the local node

Output Parameters:
. size - pointer to an integer that receives the size of the clique

Return values:
+ PMI_SUCCESS - size successfully obtained
. PMI_ERR_INVALID_ARG - invalid argument
- PMI_FAIL - unable to return the clique size

Notes:
This function returns the number of processes in the local process group that
are on the local node along with the local process.  This is a simple topology
function to distinguish between processes that can communicate through IPC
mechanisms (e.g., shared memory) and other network mechanisms.

@*/
int PMI_Get_clique_size( int *size );

/*@
PMI_Get_clique_ranks - get the ranks of the local processes in the process group

Input Parameters:
. length - length of the ranks array

Output Parameters:
. ranks - pointer to an array of integers that receive the local ranks

Return values:
+ PMI_SUCCESS - ranks successfully obtained
. PMI_ERR_INVALID_ARG - invalid argument
. PMI_ERR_INVALID_LENGTH - invalid length argument
- PMI_FAIL - unable to return the ranks

Notes:
This function returns the ranks of the processes on the local node.  The array
must be at least as large as the size returned by 'PMI_Get_clique_size()'.  This
is a simple topology function to distinguish between processes that can
communicate through IPC mechanisms (e.g., shared memory) and other network
mechanisms.

@*/
int PMI_Get_clique_ranks( int ranks[], int length);

/*@
PMI_Abort - abort the process group associated with this process

Input Parameters:
+ exit_code - exit code to be returned by this process
- error_msg - error message to be printed

Return values:
. none - this function should not return
@*/
int PMI_Abort(int exit_code, const char error_msg[]);

/* PMI Keymap functions */
/*@
PMI_KVS_Get_my_name - obtain the name of the keyval space the local process group has access to

Input Parameters:
. length - length of the kvsname character array

Output Parameters:
. kvsname - a string that receives the keyval space name

Return values:
+ PMI_SUCCESS - kvsname successfully obtained
. PMI_ERR_INVALID_ARG - invalid argument
. PMI_ERR_INVALID_LENGTH - invalid length argument
- PMI_FAIL - unable to return the kvsname

Notes:
This function returns the name of the keyval space that this process and all
other processes in the process group have access to.  The output parameter,
kvsname, must be at least as long as the value returned by
'PMI_KVS_Get_name_length_max()'.

@*/
int PMI_KVS_Get_my_name( char kvsname[], int length );

/*@
PMI_KVS_Get_name_length_max - obtain the length necessary to store a kvsname

Output Parameter:
. length - maximum length required to hold a keyval space name

Return values:
+ PMI_SUCCESS - length successfully set
. PMI_ERR_INVALID_ARG - invalid argument
- PMI_FAIL - unable to set the length

Notes:
This function returns the string length required to store a keyval space name.

A routine is used rather than setting a maximum value in 'pmi.h' to allow
different implementations of PMI to be used with the same executable.  These
different implementations may allow different maximum lengths; by using a 
routine here, we can interface with a variety of implementations of PMI.

@*/
int PMI_KVS_Get_name_length_max( int *length );

/*@
PMI_KVS_Get_key_length_max - obtain the length necessary to store a key

Output Parameter:
. length - maximum length required to hold a key string.

Return values:
+ PMI_SUCCESS - length successfully set
. PMI_ERR_INVALID_ARG - invalid argument
- PMI_FAIL - unable to set the length

Notes:
This function returns the string length required to store a key.

@*/
int PMI_KVS_Get_key_length_max( int *length );

/*@
PMI_KVS_Get_value_length_max - obtain the length necessary to store a value

Output Parameter:
. length - maximum length required to hold a keyval space value

Return values:
+ PMI_SUCCESS - length successfully set
. PMI_ERR_INVALID_ARG - invalid argument
- PMI_FAIL - unable to set the length

Notes:
This function returns the string length required to store a value from a
keyval space.

@*/
int PMI_KVS_Get_value_length_max( int *length );

/*@
PMI_KVS_Create - create a new keyval space

Input Parameter:
. length - length of the kvsname character array

Output Parameters:
. kvsname - a string that receives the keyval space name

Return values:
+ PMI_SUCCESS - keyval space successfully created
. PMI_ERR_INVALID_ARG - invalid argument
. PMI_ERR_INVALID_LENGTH - invalid length argument
- PMI_FAIL - unable to create a new keyval space

Notes:
This function creates a new keyval space.  Everyone in the same process group
can access this keyval space by the name returned by this function.  The
function is not collective.  Only one process calls this function.  The output
parameter, kvsname, must be at least as long as the value returned by
'PMI_KVS_Get_name_length_max()'.

@*/
int PMI_KVS_Create( char kvsname[], int length );

/*@
PMI_KVS_Destroy - destroy keyval space

Input Parameters:
. kvsname - keyval space name

Return values:
+ PMI_SUCCESS - keyval space successfully destroyed
. PMI_ERR_INVALID_ARG - invalid argument
- PMI_FAIL - unable to destroy the keyval space

Notes:
This function destroys a keyval space created by 'PMI_KVS_Create()'.

@*/
int PMI_KVS_Destroy( const char kvsname[] );

/*@
PMI_KVS_Put - put a key/value pair in a keyval space

Input Parameters:
+ kvsname - keyval space name
. key - key
- value - value

Return values:
+ PMI_SUCCESS - keyval pair successfully put in keyval space
. PMI_ERR_INVALID_KVS - invalid kvsname argument
. PMI_ERR_INVALID_KEY - invalid key argument
. PMI_ERR_INVALID_VAL - invalid val argument
- PMI_FAIL - put failed

Notes:
This function puts the key/value pair in the specified keyval space.  The
value is not visible to other processes until 'PMI_KVS_Commit()' is called.  
The function may complete locally.  After 'PMI_KVS_Commit()' is called, the
value may be retrieved by calling 'PMI_KVS_Get()'.  All keys put to a keyval
space must be unique to the keyval space.  You may not put more than once
with the same key.

@*/
int PMI_KVS_Put( const char kvsname[], const char key[], const char value[]);

/*@
PMI_KVS_Commit - commit all previous puts to the keyval space

Input Parameters:
. kvsname - keyval space name

Return values:
+ PMI_SUCCESS - commit succeeded
. PMI_ERR_INVALID_ARG - invalid argument
- PMI_FAIL - commit failed

Notes:
This function commits all previous puts since the last 'PMI_KVS_Commit()' into
the specified keyval space. It is a process local operation.

@*/
int PMI_KVS_Commit( const char kvsname[] );

/*@
PMI_KVS_Get - get a key/value pair from a keyval space

Input Parameters:
+ kvsname - keyval space name
. key - key
- length - length of value character array

Output Parameters:
. value - value

Return values:
+ PMI_SUCCESS - get succeeded
. PMI_ERR_INVALID_KVS - invalid kvsname argument
. PMI_ERR_INVALID_KEY - invalid key argument
. PMI_ERR_INVALID_VAL - invalid val argument
. PMI_ERR_INVALID_LENGTH - invalid length argument
- PMI_FAIL - get failed

Notes:
This function gets the value of the specified key in the keyval space.

@*/
int PMI_KVS_Get( const char kvsname[], const char key[], char value[], int length);

/*@
PMI_KVS_Iter_first - initialize the iterator and get the first value

Input Parameters:
+ kvsname - keyval space name
. key_len - length of key character array
- val_len - length of val character array

Output Parameters:
+ key - key
- value - value

Return values:
+ PMI_SUCCESS - keyval pair successfully retrieved from the keyval space
. PMI_ERR_INVALID_KVS - invalid kvsname argument
. PMI_ERR_INVALID_KEY - invalid key argument
. PMI_ERR_INVALID_KEY_LENGTH - invalid key length argument
. PMI_ERR_INVALID_VAL - invalid val argument
. PMI_ERR_INVALID_VAL_LENGTH - invalid val length argument
- PMI_FAIL - failed to initialize the iterator and get the first keyval pair

Notes:
This function initializes the iterator for the specified keyval space and
retrieves the first key/val pair.  The end of the keyval space is specified
by returning an empty key string.  key and val must be at least as long as
the values returned by 'PMI_KVS_Get_key_length_max()' and
'PMI_KVS_Get_value_length_max()'.

@*/
int PMI_KVS_Iter_first(const char kvsname[], char key[], int key_len, char val[], int val_len);

/*@
PMI_KVS_Iter_next - get the next keyval pair from the keyval space

Input Parameters:
+ kvsname - keyval space name
. key_len - length of key character array
- val_len - length of val character array

Output Parameters:
+ key - key
- value - value

Return values:
+ PMI_SUCCESS - keyval pair successfully retrieved from the keyval space
. PMI_ERR_INVALID_KVS - invalid kvsname argument
. PMI_ERR_INVALID_KEY - invalid key argument
. PMI_ERR_INVALID_KEY_LENGTH - invalid key length argument
. PMI_ERR_INVALID_VAL - invalid val argument
. PMI_ERR_INVALID_VAL_LENGTH - invalid val length argument
- PMI_FAIL - failed to get the next keyval pair

Notes:
This function retrieves the next keyval pair from the specified keyval space.  
'PMI_KVS_Iter_first()' must have been previously called.  The end of the keyval
space is specified by returning an empty key string.  The output parameters,
key and val, must be at least as long as the values returned by
'PMI_KVS_Get_key_length_max()' and 'PMI_KVS_Get_value_length_max()'.

@*/
int PMI_KVS_Iter_next(const char kvsname[], char key[], int key_len, char val[], int val_len);

/* PMI Process Creation functions */

/*S
PMI_keyval_t - keyval structure used by PMI_Spawn_mulitiple

Fields:
+ key - name of the key
- val - value of the key

S*/
typedef struct PMI_keyval_t
{
    char * key;
    char * val;
} PMI_keyval_t;

/*@
PMI_Spawn_multiple - spawn a new set of processes

Input Parameters:
+ count - count of commands
. cmds - array of command strings
. argvs - array of argv arrays for each command string
. maxprocs - array of maximum processes to spawn for each command string
. info_keyval_sizes - array giving the number of elements in each of the 
  'info_keyval_vectors'
. info_keyval_vectors - array of keyval vector arrays
. preput_keyval_size - Number of elements in 'preput_keyval_vector'
- preput_keyval_vector - array of keyvals to be pre-put in the spawned keyval space

Output Parameter:
. errors - array of errors for each command

Return values:
+ PMI_SUCCESS - spawn successful
. PMI_ERR_INVALID_ARG - invalid argument
- PMI_FAIL - spawn failed

Notes:
This function spawns a set of processes into a new process group.  The 'count'
field refers to the size of the array parameters - 'cmd', 'argvs', 'maxprocs',
'info_keyval_sizes' and 'info_keyval_vectors'.  The 'preput_keyval_size' refers
to the size of the 'preput_keyval_vector' array.  The 'preput_keyval_vector'
contains keyval pairs that will be put in the keyval space of the newly
created process group before the processes are started.  The 'maxprocs' array
specifies the desired number of processes to create for each 'cmd' string.  
The actual number of processes may be less than the numbers specified in
maxprocs.  The acceptable number of processes spawned may be controlled by
``soft'' keyvals in the info arrays.  The ``soft'' option is specified by
mpiexec in the MPI-2 standard.  Environment variables may be passed to the
spawned processes through PMI implementation specific 'info_keyval' parameters.
@*/
int PMI_Spawn_multiple(int count,
                       const char * cmds[],
                       const char ** argvs[],
                       const int maxprocs[],
                       const int info_keyval_sizesp[],
                       const PMI_keyval_t * info_keyval_vectors[],
                       int preput_keyval_size,
                       const PMI_keyval_t preput_keyval_vector[],
                       int errors[]);


/*@
PMI_Parse_option - create keyval structures from a single command line argument

Input Parameters:
+ num_args - length of args array
- args - array of command line arguments starting with the argument to be parsed

Output Parameters:
+ num_parsed - number of elements of the argument array parsed
. keyvalp - pointer to an array of keyvals
- size - size of the allocated array

Return values:
+ PMI_SUCCESS - success
. PMI_ERR_INVALID_NUM_ARGS - invalid number of arguments
. PMI_ERR_INVALID_ARGS - invalid args argument
. PMI_ERR_INVALID_NUM_PARSED - invalid num_parsed length argument
. PMI_ERR_INVALID_KEYVALP - invalid keyvalp argument
. PMI_ERR_INVALID_SIZE - invalid size argument
- PMI_FAIL - fail

Notes:
This function removes one PMI specific argument from the command line and
creates the corresponding 'PMI_keyval_t' structure for it.  It returns
an array and size to the caller.  The array must be freed by 'PMI_Free_keyvals()'.
If the first element of the args array is not a PMI specific argument, the function
returns success and sets num_parsed to zero.  If there are multiple PMI specific
arguments in the args array, this function may parse more than one argument as long
as the options are contiguous in the args array.

@*/
int PMI_Parse_option(int num_args, char *args[], int *num_parsed, PMI_keyval_t **keyvalp, int *size);

/*@
PMI_Args_to_keyval - create keyval structures from command line arguments

Input Parameters:
+ argcp - pointer to argc
- argvp - pointer to argv

Output Parameters:
+ keyvalp - pointer to an array of keyvals
- size - size of the allocated array

Return values:
+ PMI_SUCCESS - success
. PMI_ERR_INVALID_ARG - invalid argument
- PMI_FAIL - fail

Notes:
This function removes PMI specific arguments from the command line and
creates the corresponding 'PMI_keyval_t' structures for them.  It returns
an array and size to the caller that can then be passed to 'PMI_Spawn_multiple()'.
The array can be freed by 'PMI_Free_keyvals()'.  The routine 'free()' should 
not be used to free this array as there is no requirement that the array be
allocated with 'malloc()'.

@*/
int PMI_Args_to_keyval(int *argcp, char *((*argvp)[]), PMI_keyval_t **keyvalp, int *size);

/*@
PMI_Free_keyvals - free the keyval structures created by PMI_Args_to_keyval

Input Parameters:
+ keyvalp - array of keyvals
- size - size of the array

Return values:
+ PMI_SUCCESS - success
. PMI_ERR_INVALID_ARG - invalid argument
- PMI_FAIL - fail

Notes:
 This function frees the data returned by 'PMI_Args_to_keyval' and 'PMI_Parse_option'.
 Using this routine instead of 'free' allows the PMI package to track 
 allocation of storage or to use interal storage as it sees fit.
@*/
int PMI_Free_keyvals(PMI_keyval_t keyvalp[], int size);

/*@
PMI_Get_options - get a string of command line argument descriptions that may be printed to the user

Input Parameters:
. length - length of str

Output Parameters:
+ str - description string
- length - length of string or necessary length if input is not large enough

Return values:
+ PMI_SUCCESS - success
. PMI_ERR_INVALID_ARG - invalid argument
. PMI_ERR_INVALID_LENGTH - invalid length argument
. PMI_ERR_NOMEM - input length too small
- PMI_FAIL - fail

Notes:
 This function returns the command line options specific to the pmi implementation
@*/
int PMI_Get_options(char *str, int *length);

#if defined(__cplusplus)
}
#endif

#endif
