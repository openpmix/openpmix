/*
 * PMIx copyrights:
 * Copyright (c) 2013-2014 Intel, Inc. All rights reserved
 */

#ifndef PMIx_H
#define PMIx_H

#include "src/class/pmix_list.h"
#include "src/buffer_ops/types.h"

BEGIN_C_DECLS

/* PMIx errors are always negative, with 0 reserved for success */
#define PMIX_ERROR_BASE  0

#define PMIX_SUCCESS                               (PMIX_ERROR_BASE)           // operation completed successfully    
#define PMIX_ERROR                                 (PMIX_ERROR_BASE -  1)      // generalized error
#define PMIX_ERR_INIT                              (PMIX_ERROR_BASE -  2)      // PMIx not initialized
#define PMIX_ERR_NOMEM                             (PMIX_ERROR_BASE -  3)      // 
#define PMIX_ERR_INVALID_ARG                       (PMIX_ERROR_BASE -  4)      // invalid argument
#define PMIX_ERR_INVALID_KEY                       (PMIX_ERROR_BASE -  5)      // invalid key
#define PMIX_ERR_INVALID_KEY_LENGTH                (PMIX_ERROR_BASE -  6)      // invalid key length argument
#define PMIX_ERR_INVALID_VAL                       (PMIX_ERROR_BASE -  7)
#define PMIX_ERR_INVALID_VAL_LENGTH                (PMIX_ERROR_BASE -  8)
#define PMIX_ERR_INVALID_LENGTH                    (PMIX_ERROR_BASE -  9)
#define PMIX_ERR_INVALID_NUM_ARGS                  (PMIX_ERROR_BASE - 10)
#define PMIX_ERR_INVALID_ARGS                      (PMIX_ERROR_BASE - 11)
#define PMIX_ERR_INVALID_NUM_PARSED                (PMIX_ERROR_BASE - 12)
#define PMIX_ERR_INVALID_KEYVALP                   (PMIX_ERROR_BASE - 13)
#define PMIX_ERR_INVALID_SIZE                      (PMIX_ERROR_BASE - 14)
#define PMIX_ERR_INVALID_NAMESPACE                 (PMIX_ERROR_BASE - 15)
#define PMIX_ERR_SERVER_NOT_AVAIL                  (PMIX_ERROR_BASE - 16)
#define PMIX_ERR_NOT_FOUND                         (PMIX_ERROR_BASE - 17)
#define PMIX_ERR_NOT_SUPPORTED                     (PMIX_ERROR_BASE - 18)
#define PMIX_ERR_NOT_IMPLEMENTED                   (PMIX_ERROR_BASE - 19)
#define PMIX_ERR_COMM_FAILURE                      (PMIX_ERROR_BASE - 20)
#define PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER    (PMIX_ERROR_BASE - 21)

/* define some maximum sizes */
#define PMIX_MAX_VALLEN   1024
#define PMIX_MAX_INFO_KEY  255
#define PMIX_MAX_INFO_VAL 1024

/* define an INFO object corresponding to
 * the MPI_Info structure */
typedef struct {
    pmix_list_item_t super;
    char key[PMIX_MAX_INFO_KEY];
    char value[PMIX_MAX_INFO_VAL];
} pmix_info_t;
OBJ_CLASS_DECLARATION(pmix_info_t);

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


/* miscellaneous common keys - used in functions and internally, but
 * not attributes */
#define PMIX_PORT     "pmix.port"


/* callback handler for errors */
typedef void (*pmix_errhandler_fn_t)(int error);

/* callback function for non-blocking operations */
typedef void (*pmix_cbfunc_t)(int status, pmix_value_t *kv, void *cbdata);

/* NOTE: calls to these APIs must be thread-protected as there
 * currently is NO internal thread safety. */

/* Init */
int PMIx_Init(char **namespace, int *rank);

/* Finalize */
int PMIx_Finalize(void);

/* Initialized */
bool PMIx_Initialized(void);

/* Abort */
int PMIx_Abort(int status, const char msg[]);

/* Fence */
int PMIx_Fence(pmix_list_t *ranges);

/* Fence_nb */
int PMIx_Fence_nb(pmix_list_t *ranges, bool barrier,
                  pmix_cbfunc_t cbfunc, void *cbdata);

/* Put */
int PMIx_Put(pmix_scope_t scope, pmix_value_t *kv);

/* Get */
int PMIx_Get(const char *namespace, int rank,
             const char *key, pmix_value_t **kv);

/* Get_nb */
void PMIx_Get_nb(const char *namespace, int rank,
                 const char *key,
                 pmix_cbfunc_t cbfunc, void *cbdata);

/* Publish - the "info" parameter
 * consists of an array of pmix_info_t objects */
int PMIx_Publish(const char service_name[],
                 pmix_list_t *info);

/* Lookup - the "info" parameter consists of a list of
 * pmix_info_t objects that specify the requested
 * information. Info will be returned in the objects.
 * The namespace param will contain the namespace of
 * the process that published the service_name */
int PMIx_Lookup(const char service_name[],
                pmix_list_t *info,
                char **namespace);

/* Unpublish - the "info" parameter
 * consists of a list of pmix_info_t objects */
int PMIx_Unpublish(const char service_name[], 
                   pmix_list_t *info);

/* Get attribute */
bool PMIx_Get_attr(const char *namespace, int rank,
                   const char *attr, pmix_value_t **kv);

/* Get attribute (non-blocking) */
int PMIx_Get_attr_nb(const char *namespace, int rank,
                     const char *attr,
                     pmix_cbfunc_t cbfunc, void *cbdata);

/* register an errhandler to report loss of connection to the server */
void PMIx_Register_errhandler(pmix_errhandler_fn_t errhandler);

/* deregister the errhandler */
void PMIx_Deregister_errhandler(void);

/* connect */
int PMIx_Connect(pmix_list_t *ranges);

/* disconnect */
int PMIx_Disconnect(pmix_list_t *ranges);

END_C_DECLS

#endif
