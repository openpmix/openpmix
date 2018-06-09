#file: cpmix.pxd

cdef extern from "pmix_common.h":
    cdef int PMIX_MAX_KEYLEN "PMIX_MAX_KEYLEN"
    cdef int PMIX_MAX_NSLEN "PMIX_MAX_NSLEN"
    const int CPMIX_MAX_NSLEN = 256
    const int CPMIX_MAX_KEYLEN = 512

    ctypedef int pmix_status_t
    ctypedef unsigned int pmix_rank_t
    ctypedef int uid_t
    ctypedef int gid_t

    cdef pmix_rank_t PMIX_RANK_UNDEF "PMIX_RANK_UNDEF"
    cdef pmix_rank_t PMIX_RANK_WILDCARD "PMIX_RANK_WILDCARD"
    cdef pmix_rank_t PMIX_RANK_LOCAL_NODE "PMIX_RANK_LOCAL_NODE"
    cdef pmix_rank_t PMIX_RANK_INVALID "PMIX_RANK_INVALID"

    # Attributes
    cdef const char* PMIX_EVENT_BASE "PMIX_EVENT_BASE"
    cdef const char* PMIX_SERVER_TOOL_SUPPORT "PMIX_SERVER_TOOL_SUPPORT"
    cdef const char* PMIX_SERVER_REMOTE_CONNECTIONS "PMIX_SERVER_REMOTE_CONNECTIONS"
    cdef const char* PMIX_SERVER_SYSTEM_SUPPORT "PMIX_SERVER_SYSTEM_SUPPORT"
    cdef const char* PMIX_SERVER_TMPDIR "PMIX_SERVER_TMPDIR"
    cdef const char* PMIX_SYSTEM_TMPDIR "PMIX_SYSTEM_TMPDIR"
    cdef const char* PMIX_REGISTER_NODATA "PMIX_REGISTER_NODATA"

    ctypedef struct pmix_proc_t:
        char nspace[CPMIX_MAX_NSLEN]
        pmix_rank_t rank
    ctypedef unsigned short pmix_data_type_t
    ctypedef unsigned int pmix_info_directives_t
    ctypedef struct pmix_info_t:
        char key[CPMIX_MAX_KEYLEN]
        pmix_info_directives_t flags

    cdef pmix_info_directives_t PMIX_INFO_REQD "PMIX_INFO_REQD"
    cdef pmix_info_directives_t PMIX_INFO_DIR_RESERVED "PMIX_INFO_DIR_RESERVED"

    cdef pmix_data_type_t PMIX_UNDEF "PMIX_UNDEF"
    cdef pmix_data_type_t PMIX_BOOL "PMIX_BOOL"
    cdef pmix_data_type_t PMIX_BYTE "PMIX_BYTE"
    cdef pmix_data_type_t PMIX_STRING "PMIX_STRING"
    cdef pmix_data_type_t PMIX_SIZE "PMIX_SIZE"
    cdef pmix_data_type_t PMIX_PID "PMIX_PID"
    cdef pmix_data_type_t PMIX_INT "PMIX_INT"
    cdef pmix_data_type_t PMIX_INT8 "PMIX_INT8"
    cdef pmix_data_type_t PMIX_INT16 "PMIX_INT16"
    cdef pmix_data_type_t PMIX_INT32 "PMIX_INT32"
    cdef pmix_data_type_t PMIX_INT64 "PMIX_INT64"
    cdef pmix_data_type_t PMIX_UINT "PMIX_UINT"
    cdef pmix_data_type_t PMIX_UINT8 "PMIX_UINT8"
    cdef pmix_data_type_t PMIX_UINT16 "PMIX_UINT16"
    cdef pmix_data_type_t PMIX_UINT32 "PMIX_UINT32"
    cdef pmix_data_type_t PMIX_UINT64 "PMIX_UINT64"
    cdef pmix_data_type_t PMIX_FLOAT "PMIX_FLOAT"
    cdef pmix_data_type_t PMIX_DOUBLE "PMIX_DOUBLE"
    cdef pmix_data_type_t PMIX_TIMEVAL "PMIX_TIMEVAL"
    cdef pmix_data_type_t PMIX_TIME "PMIX_TIME"
    cdef pmix_data_type_t PMIX_STATUS "PMIX_STATUS"
    cdef pmix_data_type_t PMIX_VALUE "PMIX_VALUE"
    cdef pmix_data_type_t PMIX_PROC "PMIX_PROC"
    cdef pmix_data_type_t PMIX_APP "PMIX_APP"
    cdef pmix_data_type_t PMIX_INFO "PMIX_INFO"
    cdef pmix_data_type_t PMIX_PDATA "PMIX_PDATA"
    cdef pmix_data_type_t PMIX_BUFFER "PMIX_BUFFER"
    cdef pmix_data_type_t PMIX_BYTE_OBJECT "PMIX_BYTE_OBJECT"
    cdef pmix_data_type_t PMIX_KVAL "PMIX_KVAL"
    cdef pmix_data_type_t PMIX_MODEX "PMIX_MODEX"
    cdef pmix_data_type_t PMIX_PERSIST "PMIX_PERSIST"
    cdef pmix_data_type_t PMIX_POINTER "PMIX_POINTER"
    cdef pmix_data_type_t PMIX_SCOPE "PMIX_SCOPE"
    cdef pmix_data_type_t PMIX_DATA_RANGE "PMIX_DATA_RANGE"
    cdef pmix_data_type_t PMIX_COMMAND "PMIX_COMMAND"
    cdef pmix_data_type_t PMIX_INFO_DIRECTIVES "PMIX_INFO_DIRECTIVES"
    cdef pmix_data_type_t PMIX_DATA_TYPE "PMIX_DATA_TYPE"
    cdef pmix_data_type_t PMIX_PROC_STATE "PMIX_PROC_STATE"
    cdef pmix_data_type_t PMIX_PROC_INFO "PMIX_PROC_INFO"
    cdef pmix_data_type_t PMIX_DATA_ARRAY "PMIX_DATA_ARRAY"
    cdef pmix_data_type_t PMIX_PROC_RANK "PMIX_PROC_RANK"
    cdef pmix_data_type_t PMIX_QUERY "PMIX_QUERY"
    cdef pmix_data_type_t PMIX_COMPRESSED_STRING "PMIX_COMPRESSED_STRING"
    cdef pmix_data_type_t PMIX_ALLOC_DIRECTIVE "PMIX_ALLOC_DIRECTIVE"
    cdef pmix_data_type_t PMIX_INFO_ARRAY "PMIX_INFO_ARRAY"
    cdef pmix_data_type_t PMIX_IOF_CHANNEL "PMIX_IOF_CHANNEL"
    cdef pmix_data_type_t PMIX_ENVAR "PMIX_ENVAR"

    # Error values
    cdef pmix_status_t PMIX_ERR_INIT "PMIX_ERR_INIT"

    ctypedef void (*pmix_op_cbfunc_t)(pmix_status_t status, void *cbdata)

cdef extern from "pmix.h":
    int PMIx_Initialized()
    const char* PMIx_Get_version()
    pmix_status_t PMIx_Init(pmix_proc_t* myproc, pmix_info_t* info, size_t ninfo)


cdef extern from "pmix_server.h":
    ctypedef pmix_status_t (*pmix_server_client_connected_fn_t)(pmix_proc_t *proc, void *server_object,
                                                                pmix_op_cbfunc_t cbfunc, void *cbdata)
    ctypedef struct pmix_server_module_t:
        pmix_server_client_connected_fn_t client_connected

    pmix_status_t PMIx_server_init(pmix_server_module_t *module,
                                   pmix_info_t info[], size_t ninfo)
    pmix_status_t PMIx_server_finalize()
    pmix_status_t PMIx_server_register_nspace(const char nspace[], int nlocalprocs,
                                              pmix_info_t info[], size_t ninfo,
                                              pmix_op_cbfunc_t cbfunc, void *cbdata)
    void PMIx_server_deregister_nspace(const char nspace[],
                                       pmix_op_cbfunc_t cbfunc, void *cbdata)
    pmix_status_t PMIx_server_register_client(const pmix_proc_t *proc,
                                              uid_t uid, gid_t gid,
                                              void *server_object,
                                              pmix_op_cbfunc_t cbfunc, void *cbdata)
    void PMIx_server_deregister_client(const pmix_proc_t *proc,
                                       pmix_op_cbfunc_t cbfunc, void *cbdata)
    pmix_status_t PMIx_server_setup_fork(const pmix_proc_t *proc, char ***env)
