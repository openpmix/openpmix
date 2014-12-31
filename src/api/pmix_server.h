/*
 * Copyright (c) 2013-2014 Intel, Inc. All rights reserved
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */

#ifndef PMIx_H
#define PMIx_H

#include <stdint.h>
#include <string.h>
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h> /* for struct timeval */
#endif

#if defined(c_plusplus) || defined(__cplusplus)
extern "C" {
#endif

/****  PMIX CONSTANTS    ****/

/* define maximum value and key sizes */
#define PMIX_MAX_VALLEN   1024
#define PMIX_MAX_KEYLEN    512

/* define a set of "standard" PMIx attributes that can
 * be queried. Implementations (and users) are free to extend as
 * desired, so the get functions need to be capable
 * of handling the "not found" condition. Note that these
 * are attributes of the system and the job as opposed to
 * values the application (or underlying MPI library)
 * might choose to expose - i.e., they are values provided
 * by the resource manager as opposed to the application. Thus,
 * these keys are RESERVED */
#define PMIX_ATTR_UNDEF      NULL

#define PMIX_CPUSET          "pmix.cpuset"      // (char*) hwloc bitmap applied to proc upon launch
#define PMIX_CREDENTIAL      "pmix.cred"        // (opal_byte_object*) security credential assigned to proc
#define PMIX_HOSTNAME        "pmix.hname"       // (char*) name of the host this proc is on
#define PMIX_SPAWNED         "pmix.spawned"     // (bool) true if this proc resulted from a call to PMIx_Spawn
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


/****    PMIX ERROR CONSTANTS    ****/
/* PMIx errors are always negative, with 0 reserved for success */
#define PMIX_ERROR_MIN  -38  // set equal to number of non-zero entries in enum

typedef enum {
    PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER = PMIX_ERROR_MIN,
    PMIX_ERR_COMM_FAILURE,
    PMIX_ERR_NOT_IMPLEMENTED,
    PMIX_ERR_NOT_SUPPORTED,
    PMIX_ERR_NOT_FOUND,
    PMIX_ERR_SERVER_NOT_AVAIL,
    PMIX_ERR_INVALID_NAMESPACE,
    PMIX_ERR_INVALID_SIZE,
    PMIX_ERR_INVALID_KEYVALP,
    PMIX_ERR_INVALID_NUM_PARSED,

    PMIX_ERR_INVALID_ARGS,
    PMIX_ERR_INVALID_NUM_ARGS,
    PMIX_ERR_INVALID_LENGTH,
    PMIX_ERR_INVALID_VAL_LENGTH,
    PMIX_ERR_INVALID_VAL,
    PMIX_ERR_INVALID_KEY_LENGTH,
    PMIX_ERR_INVALID_KEY,
    PMIX_ERR_INVALID_ARG,
    PMIX_ERR_NOMEM,
    PMIX_ERR_INIT,

    PMIX_ERR_DATA_VALUE_NOT_FOUND,
    PMIX_ERR_OUT_OF_RESOURCE,
    PMIX_ERR_RESOURCE_BUSY,
    PMIX_ERR_BAD_PARAM,
    PMIX_ERR_IN_ERRNO,
    PMIX_ERR_UNREACH,
    PMIX_ERR_TIMEOUT,
    PMIX_ERR_NO_PERMISSIONS,
    PMIX_ERR_PACK_MISMATCH,
    PMIX_ERR_PACK_FAILURE,
    
    PMIX_ERR_UNPACK_FAILURE,
    PMIX_ERR_UNPACK_INADEQUATE_SPACE,
    PMIX_ERR_TYPE_MISMATCH,
    PMIX_ERR_PROC_ENTRY_NOT_FOUND,
    PMIX_ERR_UNKNOWN_DATA_TYPE,
    PMIX_ERR_WOULD_BLOCK,
    PMIX_EXISTS,
    PMIX_ERROR,

    PMIX_SUCCESS
} pmix_status_t;


/****    PMIX DATA TYPES    ****/
typedef enum {
    PMIX_UNDEF = 0,
    PMIX_BYTE,           // a byte of data
    PMIX_BOOL,           // boolean
    PMIX_STRING,         // NULL-terminated string
    PMIX_SIZE,           // size_t
    PMIX_PID,            // OS-pid

    PMIX_INT,
    PMIX_INT8,
    PMIX_INT16,
    PMIX_INT32,
    PMIX_INT64,

    PMIX_UINT,
    PMIX_UINT8,
    PMIX_UINT16,
    PMIX_UINT32,
    PMIX_UINT64,

    PMIX_FLOAT,
    PMIX_DOUBLE,

    PMIX_TIMEVAL,
    PMIX_TIME,

    PMIX_HWLOC_TOPO,
    PMIX_VALUE,
    PMIX_ARRAY,
    PMIX_RANGE,
    PMIX_APP,
    PMIX_INFO,
    PMIX_BUFFER,
    PMIX_KVAL
} pmix_data_type_t;

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
#define PMIX_SCOPE PMIX_UINT32
typedef enum {
    PMIX_SCOPE_UNDEF = 0,
    PMIX_INTERNAL,        // data used internally only
    PMIX_LOCAL,           // share to procs also on this node
    PMIX_REMOTE,          // share with procs not on this node
    PMIX_GLOBAL,          // share with all procs (local + remote)
    PMIX_NAMESPACE,       // used by Publish to indicate data is available to namespace only
    PMIX_UNIVERSAL,       // used by Publish to indicate data available to all
    PMIX_USER             // used by Publish to indicate data available to all user-owned namespaces
} pmix_scope_t; 


/****    PMIX STRUCTS    ****/
typedef struct {
    pmix_data_type_t type;
    size_t size;
    void *array;
} pmix_array_t;
/* NOTE: PMIX_Put can push a collection of values under
 * a single key by passing a pmix_value_t containing an
 * array of type PMIX_VALUE, with each array element
 * containing its own pmix_value_t object */

typedef struct {
    pmix_data_type_t type;
    union {
        bool flag;
        uint8_t byte;
        char *string;
        size_t size;
        pid_t pid;
        int integer;
        int8_t int8;
        int16_t int16;
        int32_t int32;
        int64_t int64;
        unsigned int uint;
        uint8_t uint8;
        uint16_t uint16;
        uint32_t uint32;
        uint64_t uint64;
        float fval;
        double dval;
        struct timeval tv;
        pmix_array_t array;
    } data;
} pmix_value_t;
/* utility function to free data contained in a pmix_value_t */
void PMIx_free_value_data(pmix_value_t *val);

typedef struct {
    char namespace[PMIX_MAX_VALLEN];
    int *ranks;
    size_t nranks;
} pmix_range_t;

typedef struct {
    char key[PMIX_MAX_KEYLEN];
    pmix_value_t *value;
} pmix_info_t;

typedef struct {
    char *cmd;
    int argc;
    char **argv;
    char **env;
    int maxprocs;
    pmix_info_t *info;
    size_t ninfo;
} pmix_app_t;

/* Key-Value pair management macros */
// TODO: add all possible types/fields here.

#define PMIX_VAL_FIELD_int(x) ((x)->data.integer)
#define PMIX_VAL_FIELD_uint32(x) ((x)->data.uint32)
#define PMIX_VAL_FIELD_uint16(x) ((x)->data.uint16)
#define PMIX_VAL_FIELD_string(x) ((x)->data.string)
#define PMIX_VAL_FIELD_float(x) ((x)->data.fval)

#define PMIX_VAL_TYPE_int    PMIX_INT
#define PMIX_VAL_TYPE_uint32 PMIX_UINT32
#define PMIX_VAL_TYPE_uint16 PMIX_UINT16
#define PMIX_VAL_TYPE_string PMIX_STRING
#define PMIX_VAL_TYPE_float  PMIX_FLOAT

#define PMIX_VAL_set_assign(_kp, _field, _val, _rc, __eext )         \
    do {                                                             \
        (_kp)->type = PMIX_VAL_TYPE_ ## _field;                      \
        PMIX_VAL_FIELD_ ## _field((_kp)) = _val;                     \
    } while(0);

#define PMIX_VAL_set_strdup(_kp, _field, _val, _rc, __eext )            \
    do {                                                                \
        (_kp)->type = PMIX_VAL_TYPE_ ## _field;                         \
        PMIX_VAL_FIELD_ ## _field((_kp)) = strdup(_val);                \
    } while(0);

#define PMIX_VAL_SET_int     PMIX_VAL_set_assign
#define PMIX_VAL_SET_uint32  PMIX_VAL_set_assign
#define PMIX_VAL_SET_uint16  PMIX_VAL_set_assign
#define PMIX_VAL_SET_string  PMIX_VAL_set_strdup
#define PMIX_VAL_SET_float   PMIX_VAL_set_assign

#define PMIX_VAL_SET(_kp, _field, _val, _rc, __eext )           \
    PMIX_VAL_SET_ ## _field(_kp, _field, _val, _rc, __eext)

#if defined(c_plusplus) || defined(__cplusplus)
}
#endif

#endif
