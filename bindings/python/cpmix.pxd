from posix.types cimport pid_t, uid_t, gid_t
from posix.time cimport timeval

cdef extern from "time.h" nogil:
    ctypedef int time_t
    time_t time(time_t*)

cdef extern from "<stdint.h>" nogil:

    # 7.18.1 Integer types
    # 7.18.1.1 Exact-width integer types
    ctypedef   signed char  int8_t
    ctypedef   signed short int16_t
    ctypedef   signed int   int32_t
    ctypedef   signed long  int64_t
    ctypedef unsigned char  uint8_t
    ctypedef unsigned short uint16_t
    ctypedef unsigned int   uint32_t
    ctypedef unsigned long long uint64_t
    # 7.18.1.4 Integer types capable of holding object pointers
    ctypedef ssize_t intptr_t
    ctypedef  size_t uintptr_t

    # 7.18.2 Limits of specified-width integer types
    # 7.18.2.1 Limits of exact-width integer types
    int8_t   INT8_MIN
    int16_t  INT16_MIN
    int32_t  INT32_MIN
    int64_t  INT64_MIN
    int8_t   INT8_MAX
    int16_t  INT16_MAX
    int32_t  INT32_MAX
    int64_t  INT64_MAX
    uint8_t  UINT8_MAX
    uint16_t UINT16_MAX
    uint32_t UINT32_MAX
    uint64_t UINT64_MAX
    # size_t
    size_t SIZE_MAX

#file: cpmix.pxd

cdef extern from "pmix_common.h":
    cdef int PMIX_MAX_KEYLEN "PMIX_MAX_KEYLEN"
    cdef int PMIX_MAX_NSLEN "PMIX_MAX_NSLEN"
    const int CPMIX_MAX_NSLEN = 256
    const int CPMIX_MAX_KEYLEN = 512

    ctypedef unsigned int pmix_rank_t

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
    cdef const char* PMIX_SERVER_ENABLE_MONITORING "PMIX_SERVER_ENABLE_MONITORING"
    cdef const char* PMIX_SERVER_NSPACE "PMIX_SERVER_NSPACE"
    cdef const char* PMIX_SERVER_RANK "PMIX_SERVER_RANK"
    cdef const char* PMIX_SERVER_GATEWAY "PMIX_SERVER_GATEWAY"

    cdef const char* PMIX_TOOL_NSPACE "PMIX_TOOL_NSPACE"
    cdef const char* PMIX_TOOL_RANK "PMIX_TOOL_RANK"
    cdef const char* PMIX_SERVER_PIDINFO "PMIX_SERVER_PIDINFO"
    cdef const char* PMIX_CONNECT_TO_SYSTEM "PMIX_CONNECT_TO_SYSTEM"
    cdef const char* PMIX_CONNECT_SYSTEM_FIRST "PMIX_CONNECT_SYSTEM_FIRST"
    cdef const char* PMIX_SERVER_URI "PMIX_SERVER_URI"
    cdef const char* PMIX_SERVER_HOSTNAME "PMIX_SERVER_HOSTNAME"
    cdef const char* PMIX_CONNECT_MAX_RETRIES "PMIX_CONNECT_MAX_RETRIES"
    cdef const char* PMIX_CONNECT_RETRY_DELAY "PMIX_CONNECT_RETRY_DELAY"
    cdef const char* PMIX_TOOL_DO_NOT_CONNECT "PMIX_TOOL_DO_NOT_CONNECT"
    cdef const char* PMIX_RECONNECT_SERVER "PMIX_RECONNECT_SERVER"
    cdef const char* PMIX_LAUNCHER "PMIX_LAUNCHER"

    cdef const char* PMIX_USERID "PMIX_USERID"
    cdef const char* PMIX_GRPID "PMIX_GRPID"
    cdef const char* PMIX_DSTPATH "PMIX_DSTPATH"
    cdef const char* PMIX_VERSION_INFO "PMIX_VERSION_INFO"
    cdef const char* PMIX_REQUESTOR_IS_TOOL "PMIX_REQUESTOR_IS_TOOL"
    cdef const char* PMIX_REQUESTOR_IS_CLIENT "PMIX_REQUESTOR_IS_CLIENT"

    cdef const char* PMIX_PROGRAMMING_MODEL "PMIX_PROGRAMMING_MODEL"
    cdef const char* PMIX_MODEL_LIBRARY_NAME "PMIX_MODEL_LIBRARY_NAME"
    cdef const char* PMIX_MODEL_LIBRARY_VERSION "PMIX_MODEL_LIBRARY_VERSION"
    cdef const char* PMIX_THREADING_MODEL "PMIX_THREADING_MODEL"
    cdef const char* PMIX_MODEL_NUM_THREADS "PMIX_MODEL_NUM_THREADS"
    cdef const char* PMIX_MODEL_NUM_CPUS "PMIX_MODEL_NUM_CPUS"
    cdef const char* PMIX_MODEL_CPU_TYPE "PMIX_MODEL_CPU_TYPE"
    cdef const char* PMIX_MODEL_PHASE_NAME "PMIX_MODEL_PHASE_NAME"
    cdef const char* PMIX_MODEL_PHASE_TYPE "PMIX_MODEL_PHASE_TYPE"
    cdef const char* PMIX_MODEL_AFFINITY_POLICY "PMIX_MODEL_AFFINITY_POLICY"

    cdef const char* PMIX_USOCK_DISABLE "PMIX_USOCK_DISABLE"
    cdef const char* PMIX_SOCKET_MODE "PMIX_SOCKET_MODE"
    cdef const char* PMIX_SINGLE_LISTENER "PMIX_SINGLE_LISTENER"

    cdef const char* PMIX_TCP_REPORT_URI "PMIX_TCP_REPORT_URI"
    cdef const char* PMIX_TCP_URI "PMIX_TCP_URI"
    cdef const char* PMIX_TCP_IF_INCLUDE "PMIX_TCP_IF_INCLUDE"
    cdef const char* PMIX_TCP_IF_EXCLUDE "PMIX_TCP_IF_EXCLUDE"
    cdef const char* PMIX_TCP_IPV4_PORT "PMIX_TCP_IPV4_PORT"
    cdef const char* PMIX_TCP_IPV6_PORT "PMIX_TCP_IPV6_PORT"
    cdef const char* PMIX_TCP_DISABLE_IPV4 "PMIX_TCP_DISABLE_IPV4"
    cdef const char* PMIX_TCP_DISABLE_IPV6 "PMIX_TCP_DISABLE_IPV6"

    cdef const char* PMIX_GDS_MODULE "PMIX_GDS_MODULE"

    cdef const char* PMIX_CPUSET "PMIX_CPUSET"
    cdef const char* PMIX_CREDENTIAL "PMIX_CREDENTIAL"
    cdef const char* PMIX_SPAWNED "PMIX_SPAWNED"
    cdef const char* PMIX_ARCH "PMIX_ARCH"

    cdef const char* PMIX_TMPDIR "PMIX_TMPDIR"
    cdef const char* PMIX_NSDIR "PMIX_NSDIR"
    cdef const char* PMIX_PROCDIR "PMIX_PROCDIR"
    cdef const char* PMIX_TDIR_RMCLEAN "PMIX_TDIR_RMCLEAN"

    cdef const char* PMIX_CLUSTER_ID "PMIX_CLUSTER_ID"
    cdef const char* PMIX_PROCID "PMIX_PROCID"
    cdef const char* PMIX_NSPACE "PMIX_NSPACE"
    cdef const char* PMIX_JOBID "PMIX_JOBID"
    cdef const char* PMIX_APPNUM "PMIX_APPNUM"
    cdef const char* PMIX_RANK "PMIX_RANK"
    cdef const char* PMIX_GLOBAL_RANK "PMIX_GLOBAL_RANK"
    cdef const char* PMIX_APP_RANK "PMIX_APP_RANK"
    cdef const char* PMIX_NPROC_OFFSET "PMIX_NPROC_OFFSET"
    cdef const char* PMIX_LOCAL_RANK "PMIX_LOCAL_RANK"
    cdef const char* PMIX_NODE_RANK "PMIX_NODE_RANK"
    cdef const char* PMIX_LOCALLDR "PMIX_LOCALLDR"
    cdef const char* PMIX_APPLDR "PMIX_APPLDR"
    cdef const char* PMIX_PROC_PID "PMIX_PROC_PID"
    cdef const char* PMIX_SESSION_ID "PMIX_SESSION_ID"
    cdef const char* PMIX_NODE_LIST "PMIX_NODE_LIST"
    cdef const char* PMIX_ALLOCATED_NODELIST "PMIX_ALLOCATED_NODELIST"
    cdef const char* PMIX_HOSTNAME "PMIX_HOSTNAME"
    cdef const char* PMIX_NODEID "PMIX_NODEID"
    cdef const char* PMIX_LOCAL_PEERS "PMIX_LOCAL_PEERS"
    cdef const char* PMIX_LOCAL_PROCS "PMIX_LOCAL_PROCS"
    cdef const char* PMIX_LOCAL_CPUSETS "PMIX_LOCAL_CPUSETS"
    cdef const char* PMIX_PROC_URI "PMIX_PROC_URI"
    cdef const char* PMIX_LOCALITY "PMIX_LOCALITY"
    cdef const char* PMIX_PARENT_ID "PMIX_PARENT_ID"
    cdef const char* PMIX_EXIT_CODE "PMIX_EXIT_CODE"

    cdef const char* PMIX_UNIV_SIZE "PMIX_UNIV_SIZE"
    cdef const char* PMIX_JOB_SIZE "PMIX_JOB_SIZE"
    cdef const char* PMIX_JOB_NUM_APPS "PMIX_JOB_NUM_APPS"
    cdef const char* PMIX_APP_SIZE "PMIX_APP_SIZE"
    cdef const char* PMIX_LOCAL_SIZE "PMIX_LOCAL_SIZE"
    cdef const char* PMIX_NODE_SIZE "PMIX_NODE_SIZE"
    cdef const char* PMIX_MAX_PROCS "PMIX_MAX_PROCS"
    cdef const char* PMIX_NUM_NODES "PMIX_NUM_NODES"

    cdef const char* PMIX_AVAIL_PHYS_MEMORY "PMIX_AVAIL_PHYS_MEMORY"
    cdef const char* PMIX_DAEMON_MEMORY "PMIX_DAEMON_MEMORY"
    cdef const char* PMIX_CLIENT_AVG_MEMORY "PMIX_CLIENT_AVG_MEMORY"

    cdef const char* PMIX_NET_TOPO "PMIX_NET_TOPO"
    cdef const char* PMIX_LOCAL_TOPO "PMIX_LOCAL_TOPO"
    cdef const char* PMIX_TOPOLOGY "PMIX_TOPOLOGY"
    cdef const char* PMIX_TOPOLOGY_XML "PMIX_TOPOLOGY_XML"
    cdef const char* PMIX_TOPOLOGY_FILE "PMIX_TOPOLOGY_FILE"
    cdef const char* PMIX_TOPOLOGY_SIGNATURE "PMIX_TOPOLOGY_SIGNATURE"
    cdef const char* PMIX_LOCALITY_STRING "PMIX_LOCALITY_STRING"
    cdef const char* PMIX_HWLOC_SHMEM_ADDR "PMIX_HWLOC_SHMEM_ADDR"
    cdef const char* PMIX_HWLOC_SHMEM_SIZE "PMIX_HWLOC_SHMEM_SIZE"
    cdef const char* PMIX_HWLOC_SHMEM_FILE "PMIX_HWLOC_SHMEM_FILE"
    cdef const char* PMIX_HWLOC_XML_V1 "PMIX_HWLOC_XML_V1"
    cdef const char* PMIX_HWLOC_XML_V2 "PMIX_HWLOC_XML_V2"
    cdef const char* PMIX_HWLOC_SHARE_TOPO "PMIX_HWLOC_SHARE_TOPO"
    cdef const char* PMIX_HWLOC_HOLE_KIND "PMIX_HWLOC_HOLE_KIND"

    cdef const char* PMIX_COLLECT_DATA "PMIX_COLLECT_DATA"
    cdef const char* PMIX_TIMEOUT "PMIX_TIMEOUT"
    cdef const char* PMIX_IMMEDIATE "PMIX_IMMEDIATE"
    cdef const char* PMIX_WAIT "PMIX_WAIT"
    cdef const char* PMIX_COLLECTIVE_ALGO "PMIX_COLLECTIVE_ALGO"
    cdef const char* PMIX_COLLECTIVE_ALGO_REQD "PMIX_COLLECTIVE_ALGO_REQD"
    cdef const char* PMIX_NOTIFY_COMPLETION "PMIX_NOTIFY_COMPLETION"
    cdef const char* PMIX_RANGE "PMIX_RANGE"
    cdef const char* PMIX_PERSISTENCE "PMIX_PERSISTENCE"
    cdef const char* PMIX_DATA_SCOPE "PMIX_DATA_SCOPE"
    cdef const char* PMIX_OPTIONAL "PMIX_OPTIONAL"
    cdef const char* PMIX_EMBED_BARRIER "PMIX_EMBED_BARRIER"
    cdef const char* PMIX_JOB_TERM_STATUS "PMIX_JOB_TERM_STATUS"
    cdef const char* PMIX_PROC_STATE_STATUS "PMIX_PROC_STATE_STATUS"

    # internal attributes
    cdef const char* PMIX_REGISTER_NODATA "PMIX_REGISTER_NODATA"
    cdef const char* PMIX_PROC_DATA "PMIX_PROC_DATA"
    cdef const char* PMIX_NODE_MAP "PMIX_NODE_MAP"
    cdef const char* PMIX_PROC_MAP "PMIX_PROC_MAP"
    cdef const char* PMIX_ANL_MAP "PMIX_ANL_MAP"
    cdef const char* PMIX_APP_MAP_TYPE "PMIX_APP_MAP_TYPE"
    cdef const char* PMIX_APP_MAP_REGEX "PMIX_APP_MAP_REGEX"
    cdef const char* PMIX_PROC_BLOB "PMIX_PROC_BLOB"
    cdef const char* PMIX_MAP_BLOB "PMIX_MAP_BLOB"

    # event handler registration and notification
    cdef const char* PMIX_EVENT_HDLR_NAME "PMIX_EVENT_HDLR_NAME"
    cdef const char* PMIX_EVENT_JOB_LEVEL "PMIX_EVENT_JOB_LEVEL"
    cdef const char* PMIX_EVENT_ENVIRO_LEVEL "PMIX_EVENT_ENVIRO_LEVEL"
    cdef const char* PMIX_EVENT_HDLR_FIRST "PMIX_EVENT_HDLR_FIRST"
    cdef const char* PMIX_EVENT_HDLR_LAST "PMIX_EVENT_HDLR_LAST"
    cdef const char* PMIX_EVENT_HDLR_FIRST_IN_CATEGORY "PMIX_EVENT_HDLR_FIRST_IN_CATEGORY"
    cdef const char* PMIX_EVENT_HDLR_LAST_IN_CATEGORY "PMIX_EVENT_HDLR_LAST_IN_CATEGORY"
    cdef const char* PMIX_EVENT_HDLR_BEFORE "PMIX_EVENT_HDLR_BEFORE"
    cdef const char* PMIX_EVENT_HDLR_AFTER "PMIX_EVENT_HDLR_AFTER"
    cdef const char* PMIX_EVENT_HDLR_PREPEND "PMIX_EVENT_HDLR_PREPEND"
    cdef const char* PMIX_EVENT_HDLR_APPEND "PMIX_EVENT_HDLR_APPEND"
    cdef const char* PMIX_EVENT_CUSTOM_RANGE "PMIX_EVENT_CUSTOM_RANGE"
    cdef const char* PMIX_EVENT_AFFECTED_PROC "PMIX_EVENT_AFFECTED_PROC"
    cdef const char* PMIX_EVENT_AFFECTED_PROCS "PMIX_EVENT_AFFECTED_PROCS"
    cdef const char* PMIX_EVENT_NON_DEFAULT "PMIX_EVENT_NON_DEFAULT"
    cdef const char* PMIX_EVENT_RETURN_OBJECT "PMIX_EVENT_RETURN_OBJECT"
    cdef const char* PMIX_EVENT_DO_NOT_CACHE "PMIX_EVENT_DO_NOT_CACHE"
    cdef const char* PMIX_EVENT_SILENT_TERMINATION "PMIX_EVENT_SILENT_TERMINATION"

    # fault tolerance events
    cdef const char* PMIX_EVENT_TERMINATE_SESSION "PMIX_EVENT_TERMINATE_SESSION"
    cdef const char* PMIX_EVENT_TERMINATE_JOB "PMIX_EVENT_TERMINATE_JOB"
    cdef const char* PMIX_EVENT_TERMINATE_NODE "PMIX_EVENT_TERMINATE_NODE"
    cdef const char* PMIX_EVENT_TERMINATE_PROC "PMIX_EVENT_TERMINATE_PROC"
    cdef const char* PMIX_EVENT_ACTION_TIMEOUT "PMIX_EVENT_ACTION_TIMEOUT"
    cdef const char* PMIX_EVENT_NO_TERMINATION "PMIX_EVENT_NO_TERMINATION"
    cdef const char* PMIX_EVENT_WANT_TERMINATION "PMIX_EVENT_WANT_TERMINATION"

    # attributes used to describe "spawn" directives
    cdef const char* PMIX_PERSONALITY "PMIX_PERSONALITY"
    cdef const char* PMIX_HOST "PMIX_HOST"
    cdef const char* PMIX_HOSTFILE "PMIX_HOSTFILE"
    cdef const char* PMIX_ADD_HOST "PMIX_ADD_HOST"
    cdef const char* PMIX_ADD_HOSTFILE "PMIX_ADD_HOSTFILE"
    cdef const char* PMIX_PREFIX "PMIX_PREFIX"
    cdef const char* PMIX_WDIR "PMIX_WDIR"
    cdef const char* PMIX_MAPPER "PMIX_MAPPER"
    cdef const char* PMIX_DISPLAY_MAP "PMIX_DISPLAY_MAP"
    cdef const char* PMIX_PPR "PMIX_PPR"
    cdef const char* PMIX_MAPBY "PMIX_MAPBY"
    cdef const char* PMIX_RANKBY "PMIX_RANKBY"
    cdef const char* PMIX_BINDTO "PMIX_BINDTO"
    cdef const char* PMIX_PRELOAD_BIN "PMIX_PRELOAD_BIN"
    cdef const char* PMIX_PRELOAD_FILES "PMIX_PRELOAD_FILES"
    cdef const char* PMIX_NON_PMI "PMIX_NON_PMI"
    cdef const char* PMIX_STDIN_TGT "PMIX_STDIN_TGT"
    cdef const char* PMIX_DEBUGGER_DAEMONS "PMIX_DEBUGGER_DAEMONS"
    cdef const char* PMIX_COSPAWN_APP "PMIX_COSPAWN_APP"
    cdef const char* PMIX_SET_SESSION_CWD "PMIX_SET_SESSION_CWD"
    cdef const char* PMIX_TAG_OUTPUT "PMIX_TAG_OUTPUT"
    cdef const char* PMIX_TIMESTAMP_OUTPUT "PMIX_TIMESTAMP_OUTPUT"
    cdef const char* PMIX_MERGE_STDERR_STDOUT "PMIX_MERGE_STDERR_STDOUT"
    cdef const char* PMIX_OUTPUT_TO_FILE "PMIX_OUTPUT_TO_FILE"
    cdef const char* PMIX_INDEX_ARGV "PMIX_INDEX_ARGV"
    cdef const char* PMIX_CPUS_PER_PROC "PMIX_CPUS_PER_PROC"
    cdef const char* PMIX_NO_PROCS_ON_HEAD "PMIX_NO_PROCS_ON_HEAD"
    cdef const char* PMIX_NO_OVERSUBSCRIBE "PMIX_NO_OVERSUBSCRIBE"
    cdef const char* PMIX_REPORT_BINDINGS "PMIX_REPORT_BINDINGS"
    cdef const char* PMIX_CPU_LIST "PMIX_CPU_LIST"
    cdef const char* PMIX_JOB_RECOVERABLE "PMIX_JOB_RECOVERABLE"
    cdef const char* PMIX_MAX_RESTARTS "PMIX_MAX_RESTARTS"
    cdef const char* PMIX_FWD_STDIN "PMIX_FWD_STDIN"
    cdef const char* PMIX_FWD_STDOUT "PMIX_FWD_STDOUT"
    cdef const char* PMIX_FWD_STDERR "PMIX_FWD_STDERR"
    cdef const char* PMIX_FWD_STDDIAG "PMIX_FWD_STDDIAG"

    # query attributes
    cdef const char* PMIX_QUERY_NAMESPACES "PMIX_QUERY_NAMESPACES"
    cdef const char* PMIX_QUERY_JOB_STATUS "PMIX_QUERY_JOB_STATUS"
    cdef const char* PMIX_QUERY_QUEUE_LIST "PMIX_QUERY_QUEUE_LIST"
    cdef const char* PMIX_QUERY_QUEUE_STATUS "PMIX_QUERY_QUEUE_STATUS"
    cdef const char* PMIX_QUERY_PROC_TABLE "PMIX_QUERY_PROC_TABLE"
    cdef const char* PMIX_QUERY_LOCAL_PROC_TABLE "PMIX_QUERY_LOCAL_PROC_TABLE"
    cdef const char* PMIX_QUERY_AUTHORIZATIONS "PMIX_QUERY_AUTHORIZATIONS"
    cdef const char* PMIX_QUERY_SPAWN_SUPPORT "PMIX_QUERY_SPAWN_SUPPORT"
    cdef const char* PMIX_QUERY_DEBUG_SUPPORT "PMIX_QUERY_DEBUG_SUPPORT"
    cdef const char* PMIX_QUERY_MEMORY_USAGE "PMIX_QUERY_MEMORY_USAGE"
    cdef const char* PMIX_QUERY_LOCAL_ONLY "PMIX_QUERY_LOCAL_ONLY"
    cdef const char* PMIX_QUERY_REPORT_AVG "PMIX_QUERY_REPORT_AVG"
    cdef const char* PMIX_QUERY_REPORT_MINMAX "PMIX_QUERY_REPORT_MINMAX"
    cdef const char* PMIX_QUERY_ALLOC_STATUS "PMIX_QUERY_ALLOC_STATUS"
    cdef const char* PMIX_TIME_REMAINING "PMIX_TIME_REMAINING"


    # log attributes
    cdef const char* PMIX_LOG_SOURCE "PMIX_LOG_SOURCE"
    cdef const char* PMIX_LOG_STDERR "PMIX_LOG_STDERR"
    cdef const char* PMIX_LOG_STDOUT "PMIX_LOG_STDOUT"
    cdef const char* PMIX_LOG_SYSLOG "PMIX_LOG_SYSLOG"
    cdef const char* PMIX_LOG_LOCAL_SYSLOG "PMIX_LOG_LOCAL_SYSLOG"
    cdef const char* PMIX_LOG_GLOBAL_SYSLOG "PMIX_LOG_GLOBAL_SYSLOG"
    cdef const char* PMIX_LOG_SYSLOG_PRI "PMIX_LOG_SYSLOG_PRI"
    cdef const char* PMIX_LOG_TIMESTAMP "PMIX_LOG_TIMESTAMP"
    cdef const char* PMIX_LOG_GENERATE_TIMESTAMP "PMIX_LOG_GENERATE_TIMESTAMP"
    cdef const char* PMIX_LOG_TAG_OUTPUT "PMIX_LOG_TAG_OUTPUT"
    cdef const char* PMIX_LOG_TIMESTAMP_OUTPUT "PMIX_LOG_TIMESTAMP_OUTPUT"
    cdef const char* PMIX_LOG_XML_OUTPUT "PMIX_LOG_XML_OUTPUT"
    cdef const char* PMIX_LOG_ONCE "PMIX_LOG_ONCE"
    cdef const char* PMIX_LOG_MSG "PMIX_LOG_MSG"
    cdef const char* PMIX_LOG_EMAIL "PMIX_LOG_EMAIL"
    cdef const char* PMIX_LOG_EMAIL_ADDR "PMIX_LOG_EMAIL_ADDR"
    cdef const char* PMIX_LOG_EMAIL_SENDER_ADDR "PMIX_LOG_EMAIL_SENDER_ADDR"
    cdef const char* PMIX_LOG_EMAIL_SUBJECT "PMIX_LOG_EMAIL_SUBJECT"
    cdef const char* PMIX_LOG_EMAIL_MSG "PMIX_LOG_EMAIL_MSG"
    cdef const char* PMIX_LOG_EMAIL_SERVER "PMIX_LOG_EMAIL_SERVER"
    cdef const char* PMIX_LOG_EMAIL_SRVR_PORT "PMIX_LOG_EMAIL_SRVR_PORT"
    cdef const char* PMIX_LOG_GLOBAL_DATASTORE "PMIX_LOG_GLOBAL_DATASTORE"
    cdef const char* PMIX_LOG_JOB_RECORD "PMIX_LOG_JOB_RECORD"

    # debugger attributes
    cdef const char* PMIX_DEBUG_STOP_ON_EXEC "PMIX_DEBUG_STOP_ON_EXEC"
    cdef const char* PMIX_DEBUG_STOP_IN_INIT "PMIX_DEBUG_STOP_IN_INIT"
    cdef const char* PMIX_DEBUG_WAIT_FOR_NOTIFY "PMIX_DEBUG_WAIT_FOR_NOTIFY"
    cdef const char* PMIX_DEBUG_JOB "PMIX_DEBUG_JOB"
    cdef const char* PMIX_DEBUG_WAITING_FOR_NOTIFY "PMIX_DEBUG_WAITING_FOR_NOTIFY"
    cdef const char* PMIX_DEBUG_JOB_DIRECTIVES "PMIX_DEBUG_JOB_DIRECTIVES"
    cdef const char* PMIX_DEBUG_APP_DIRECTIVES "PMIX_DEBUG_APP_DIRECTIVES"

    # Resource manager identification
    cdef const char* PMIX_RM_NAME "PMIX_RM_NAME"
    cdef const char* PMIX_RM_VERSION "PMIX_RM_VERSION"

    # Environmental variable operations
    cdef const char* PMIX_SET_ENVAR "PMIX_SET_ENVAR"
    cdef const char* PMIX_ADD_ENVAR "PMIX_ADD_ENVAR"
    cdef const char* PMIX_UNSET_ENVAR "PMIX_UNSET_ENVAR"
    cdef const char* PMIX_PREPEND_ENVAR "PMIX_PREPEND_ENVAR"
    cdef const char* PMIX_APPEND_ENVAR "PMIX_APPEND_ENVAR"

    # Allocation attributes
    cdef const char* PMIX_ALLOC_ID "PMIX_ALLOC_ID"
    cdef const char* PMIX_ALLOC_NUM_NODES "PMIX_ALLOC_NUM_NODES"
    cdef const char* PMIX_ALLOC_NODE_LIST "PMIX_ALLOC_NODE_LIST"
    cdef const char* PMIX_ALLOC_NUM_CPUS "PMIX_ALLOC_NUM_CPUS"
    cdef const char* PMIX_ALLOC_NUM_CPU_LIST "PMIX_ALLOC_NUM_CPU_LIST"
    cdef const char* PMIX_ALLOC_CPU_LIST "PMIX_ALLOC_CPU_LIST"
    cdef const char* PMIX_ALLOC_MEM_SIZE "PMIX_ALLOC_MEM_SIZE"
    cdef const char* PMIX_ALLOC_NETWORK "PMIX_ALLOC_NETWORK"
    cdef const char* PMIX_ALLOC_NETWORK_ID "PMIX_ALLOC_NETWORK_ID"
    cdef const char* PMIX_ALLOC_BANDWIDTH "PMIX_ALLOC_BANDWIDTH"
    cdef const char* PMIX_ALLOC_NETWORK_QOS "PMIX_ALLOC_NETWORK_QOS"
    cdef const char* PMIX_ALLOC_TIME "PMIX_ALLOC_TIME"
    cdef const char* PMIX_ALLOC_NETWORK_TYPE "PMIX_ALLOC_NETWORK_TYPE"
    cdef const char* PMIX_ALLOC_NETWORK_PLANE "PMIX_ALLOC_NETWORK_PLANE"
    cdef const char* PMIX_ALLOC_NETWORK_ENDPTS "PMIX_ALLOC_NETWORK_ENDPTS"
    cdef const char* PMIX_ALLOC_NETWORK_ENDPTS_NODE "PMIX_ALLOC_NETWORK_ENDPTS_NODE"
    cdef const char* PMIX_ALLOC_NETWORK_SEC_KEY "PMIX_ALLOC_NETWORK_SEC_KEY"

    # Job control attributes
    cdef const char* PMIX_JOB_CTRL_ID "PMIX_JOB_CTRL_ID"
    cdef const char* PMIX_JOB_CTRL_PAUSE "PMIX_JOB_CTRL_PAUSE"
    cdef const char* PMIX_JOB_CTRL_RESUME "PMIX_JOB_CTRL_RESUME"
    cdef const char* PMIX_JOB_CTRL_CANCEL "PMIX_JOB_CTRL_CANCEL"
    cdef const char* PMIX_JOB_CTRL_KILL "PMIX_JOB_CTRL_KILL"
    cdef const char* PMIX_JOB_CTRL_RESTART "PMIX_JOB_CTRL_RESTART"
    cdef const char* PMIX_JOB_CTRL_CHECKPOINT "PMIX_JOB_CTRL_CHECKPOINT"
    cdef const char* PMIX_JOB_CTRL_CHECKPOINT_EVENT "PMIX_JOB_CTRL_CHECKPOINT_EVENT"
    cdef const char* PMIX_JOB_CTRL_CHECKPOINT_SIGNAL "PMIX_JOB_CTRL_CHECKPOINT_SIGNAL"
    cdef const char* PMIX_JOB_CTRL_CHECKPOINT_TIMEOUT "PMIX_JOB_CTRL_CHECKPOINT_TIMEOUT"
    cdef const char* PMIX_JOB_CTRL_CHECKPOINT_METHOD "PMIX_JOB_CTRL_CHECKPOINT_METHOD"
    cdef const char* PMIX_JOB_CTRL_SIGNAL "PMIX_JOB_CTRL_SIGNAL"
    cdef const char* PMIX_JOB_CTRL_PROVISION "PMIX_JOB_CTRL_PROVISION"
    cdef const char* PMIX_JOB_CTRL_PROVISION_IMAGE "PMIX_JOB_CTRL_PROVISION_IMAGE"
    cdef const char* PMIX_JOB_CTRL_PREEMPTIBLE "PMIX_JOB_CTRL_PREEMPTIBLE"
    cdef const char* PMIX_JOB_CTRL_TERMINATE "PMIX_JOB_CTRL_TERMINATE"
    cdef const char* PMIX_REGISTER_CLEANUP "PMIX_REGISTER_CLEANUP"
    cdef const char* PMIX_REGISTER_CLEANUP_DIR "PMIX_REGISTER_CLEANUP_DIR"
    cdef const char* PMIX_CLEANUP_RECURSIVE "PMIX_CLEANUP_RECURSIVE"
    cdef const char* PMIX_CLEANUP_EMPTY "PMIX_CLEANUP_EMPTY"
    cdef const char* PMIX_CLEANUP_IGNORE "PMIX_CLEANUP_IGNORE"
    cdef const char* PMIX_CLEANUP_LEAVE_TOPDIR "PMIX_CLEANUP_LEAVE_TOPDIR"

    # Monitoring attributes
    cdef const char* PMIX_MONITOR_ID "PMIX_MONITOR_ID"
    cdef const char* PMIX_MONITOR_CANCEL "PMIX_MONITOR_CANCEL"
    cdef const char* PMIX_MONITOR_APP_CONTROL "PMIX_MONITOR_APP_CONTROL"
    cdef const char* PMIX_MONITOR_HEARTBEAT "PMIX_MONITOR_HEARTBEAT"
    cdef const char* PMIX_SEND_HEARTBEAT "PMIX_SEND_HEARTBEAT"
    cdef const char* PMIX_MONITOR_HEARTBEAT_TIME "PMIX_MONITOR_HEARTBEAT_TIME"
    cdef const char* PMIX_MONITOR_HEARTBEAT_DROPS "PMIX_MONITOR_HEARTBEAT_DROPS"
    cdef const char* PMIX_MONITOR_FILE "PMIX_MONITOR_FILE"
    cdef const char* PMIX_MONITOR_FILE_SIZE "PMIX_MONITOR_FILE_SIZE"
    cdef const char* PMIX_MONITOR_FILE_ACCESS "PMIX_MONITOR_FILE_ACCESS"
    cdef const char* PMIX_MONITOR_FILE_MODIFY "PMIX_MONITOR_FILE_MODIFY"
    cdef const char* PMIX_MONITOR_FILE_CHECK_TIME "PMIX_MONITOR_FILE_CHECK_TIME"
    cdef const char* PMIX_MONITOR_FILE_DROPS "PMIX_MONITOR_FILE_DROPS"

    # Security attributes
    cdef const char* PMIX_CRED_TYPE "PMIX_CRED_TYPE"
    cdef const char* PMIX_CRYPTO_KEY "PMIX_CRYPTO_KEY"

    # IO Forwarding attributes
    cdef const char* PMIX_IOF_CACHE_SIZE "PMIX_IOF_CACHE_SIZE"
    cdef const char* PMIX_IOF_DROP_OLDEST "PMIX_IOF_DROP_OLDEST"
    cdef const char* PMIX_IOF_DROP_NEWEST "PMIX_IOF_DROP_NEWEST"
    cdef const char* PMIX_IOF_BUFFERING_SIZE "PMIX_IOF_BUFFERING_SIZE"
    cdef const char* PMIX_IOF_BUFFERING_TIME "PMIX_IOF_BUFFERING_TIME"
    cdef const char* PMIX_IOF_COMPLETE "PMIX_IOF_COMPLETE"
    cdef const char* PMIX_IOF_PUSH_STDIN "PMIX_IOF_PUSH_STDIN"
    cdef const char* PMIX_IOF_TAG_OUTPUT "PMIX_IOF_TAG_OUTPUT"
    cdef const char* PMIX_IOF_TIMESTAMP_OUTPUT "PMIX_IOF_TIMESTAMP_OUTPUT"
    cdef const char* PMIX_IOF_XML_OUTPUT "PMIX_IOF_XML_OUTPUT"

    # Attributes for application setup data
    cdef const char* PMIX_SETUP_APP_ENVARS "PMIX_SETUP_APP_ENVARS"
    cdef const char* PMIX_SETUP_APP_NONENVARS "PMIX_SETUP_APP_NONENVARS"
    cdef const char* PMIX_SETUP_APP_ALL "PMIX_SETUP_APP_ALL"


    # Process state definitions
    ctypedef uint8_t pmix_proc_state_t
    cdef pmix_proc_state_t PMIX_PROC_STATE_UNDEF "PMIX_PROC_STATE_UNDEF"
    cdef pmix_proc_state_t PMIX_PROC_STATE_PREPPED "PMIX_PROC_STATE_PREPPED"
    cdef pmix_proc_state_t PMIX_PROC_STATE_LAUNCH_UNDERWAY "PMIX_PROC_STATE_LAUNCH_UNDERWAY"
    cdef pmix_proc_state_t PMIX_PROC_STATE_RESTART "PMIX_PROC_STATE_RESTART"
    cdef pmix_proc_state_t PMIX_PROC_STATE_TERMINATE "PMIX_PROC_STATE_TERMINATE"
    cdef pmix_proc_state_t PMIX_PROC_STATE_RUNNING "PMIX_PROC_STATE_RUNNING"
    cdef pmix_proc_state_t PMIX_PROC_STATE_CONNECTED "PMIX_PROC_STATE_CONNECTED"
    cdef pmix_proc_state_t PMIX_PROC_STATE_UNTERMINATED "PMIX_PROC_STATE_UNTERMINATED"
    cdef pmix_proc_state_t PMIX_PROC_STATE_TERMINATED "PMIX_PROC_STATE_TERMINATED"
    cdef pmix_proc_state_t PMIX_PROC_STATE_ERROR "PMIX_PROC_STATE_ERROR"
    cdef pmix_proc_state_t PMIX_PROC_STATE_KILLED_BY_CMD "PMIX_PROC_STATE_KILLED_BY_CMD"
    cdef pmix_proc_state_t PMIX_PROC_STATE_ABORTED "PMIX_PROC_STATE_ABORTED"
    cdef pmix_proc_state_t PMIX_PROC_STATE_FAILED_TO_START "PMIX_PROC_STATE_FAILED_TO_START"
    cdef pmix_proc_state_t PMIX_PROC_STATE_ABORTED_BY_SIG "PMIX_PROC_STATE_ABORTED_BY_SIG"
    cdef pmix_proc_state_t PMIX_PROC_STATE_TERM_WO_SYNC "PMIX_PROC_STATE_TERM_WO_SYNC"
    cdef pmix_proc_state_t PMIX_PROC_STATE_COMM_FAILED "PMIX_PROC_STATE_COMM_FAILED"
    cdef pmix_proc_state_t PMIX_PROC_STATE_SENSOR_BOUND_EXCEEDED "PMIX_PROC_STATE_SENSOR_BOUND_EXCEEDED"
    cdef pmix_proc_state_t PMIX_PROC_STATE_CALLED_ABORT "PMIX_PROC_STATE_CALLED_ABORT"
    cdef pmix_proc_state_t PMIX_PROC_STATE_HEARTBEAT_FAILED "PMIX_PROC_STATE_HEARTBEAT_FAILED"
    cdef pmix_proc_state_t PMIX_PROC_STATE_MIGRATING "PMIX_PROC_STATE_MIGRATING"
    cdef pmix_proc_state_t PMIX_PROC_STATE_CANNOT_RESTART "PMIX_PROC_STATE_CANNOT_RESTART"
    cdef pmix_proc_state_t PMIX_PROC_STATE_TERM_NON_ZERO "PMIX_PROC_STATE_TERM_NON_ZERO"
    cdef pmix_proc_state_t PMIX_PROC_STATE_FAILED_TO_LAUNCH "PMIX_PROC_STATE_FAILED_TO_LAUNCH"

    ctypedef int pmix_status_t
    # Error values
    cdef pmix_status_t PMIX_ERR_BASE "PMIX_ERR_BASE"
    # generic values
    cdef pmix_status_t PMIX_SUCCESS "PMIX_SUCCESS"
    cdef pmix_status_t PMIX_ERROR "PMIX_ERROR"
    cdef pmix_status_t PMIX_ERR_SILENT "PMIX_ERR_SILENT"
    # debugger release
    cdef pmix_status_t PMIX_ERR_DEBUGGER_RELEASE "PMIX_ERR_DEBUGGER_RELEASE"
    # fault tolerance
    cdef pmix_status_t PMIX_ERR_PROC_RESTART "PMIX_ERR_PROC_RESTART"
    cdef pmix_status_t PMIX_ERR_PROC_CHECKPOINT "PMIX_ERR_PROC_CHECKPOINT"
    cdef pmix_status_t PMIX_ERR_PROC_MIGRATE "PMIX_ERR_PROC_MIGRATE"
    # abort
    cdef pmix_status_t PMIX_ERR_PROC_ABORTED "PMIX_ERR_PROC_ABORTED"
    cdef pmix_status_t PMIX_ERR_PROC_REQUESTED_ABORT "PMIX_ERR_PROC_REQUESTED_ABORT"
    cdef pmix_status_t PMIX_ERR_PROC_ABORTING "PMIX_ERR_PROC_ABORTING"
    # comm failures
    cdef pmix_status_t PMIX_ERR_SERVER_FAILED_REQUEST "PMIX_ERR_SERVER_FAILED_REQUEST"
    cdef pmix_status_t PMIX_EXISTS "PMIX_EXISTS"
    cdef pmix_status_t PMIX_ERR_INVALID_CRED "PMIX_ERR_INVALID_CRED"
    cdef pmix_status_t PMIX_ERR_HANDSHAKE_FAILED "PMIX_ERR_HANDSHAKE_FAILED"
    cdef pmix_status_t PMIX_ERR_READY_FOR_HANDSHAKE "PMIX_ERR_READY_FOR_HANDSHAKE"
    cdef pmix_status_t PMIX_ERR_WOULD_BLOCK "PMIX_ERR_WOULD_BLOCK"
    cdef pmix_status_t PMIX_ERR_UNKNOWN_DATA_TYPE "PMIX_ERR_UNKNOWN_DATA_TYPE"
    cdef pmix_status_t PMIX_ERR_PROC_ENTRY_NOT_FOUND "PMIX_ERR_PROC_ENTRY_NOT_FOUND"
    cdef pmix_status_t PMIX_ERR_TYPE_MISMATCH "PMIX_ERR_TYPE_MISMATCH"
    cdef pmix_status_t PMIX_ERR_UNPACK_INADEQUATE_SPACE "PMIX_ERR_UNPACK_INADEQUATE_SPACE"
    cdef pmix_status_t PMIX_ERR_UNPACK_FAILURE "PMIX_ERR_UNPACK_FAILURE"
    cdef pmix_status_t PMIX_ERR_PACK_FAILURE "PMIX_ERR_PACK_FAILURE"
    cdef pmix_status_t PMIX_ERR_PACK_MISMATCH "PMIX_ERR_PACK_MISMATCH"
    cdef pmix_status_t PMIX_ERR_NO_PERMISSIONS "PMIX_ERR_NO_PERMISSIONS"
    cdef pmix_status_t PMIX_ERR_TIMEOUT "PMIX_ERR_TIMEOUT"
    cdef pmix_status_t PMIX_ERR_UNREACH "PMIX_ERR_UNREACH"
    cdef pmix_status_t PMIX_ERR_IN_ERRNO "PMIX_ERR_IN_ERRNO"
    cdef pmix_status_t PMIX_ERR_BAD_PARAM "PMIX_ERR_BAD_PARAM"
    cdef pmix_status_t PMIX_ERR_RESOURCE_BUSY "PMIX_ERR_RESOURCE_BUSY"
    cdef pmix_status_t PMIX_ERR_OUT_OF_RESOURCE "PMIX_ERR_OUT_OF_RESOURCE"
    cdef pmix_status_t PMIX_ERR_DATA_VALUE_NOT_FOUND "PMIX_ERR_DATA_VALUE_NOT_FOUND"
    cdef pmix_status_t PMIX_ERR_INIT "PMIX_ERR_INIT"
    cdef pmix_status_t PMIX_ERR_NOMEM "PMIX_ERR_NOMEM"
    cdef pmix_status_t PMIX_ERR_INVALID_ARG "PMIX_ERR_INVALID_ARG"
    cdef pmix_status_t PMIX_ERR_INVALID_KEY "PMIX_ERR_INVALID_KEY"
    cdef pmix_status_t PMIX_ERR_INVALID_KEY_LENGTH "PMIX_ERR_INVALID_KEY_LENGTH"
    cdef pmix_status_t PMIX_ERR_INVALID_VAL "PMIX_ERR_INVALID_VAL"
    cdef pmix_status_t PMIX_ERR_INVALID_VAL_LENGTH "PMIX_ERR_INVALID_VAL_LENGTH"
    cdef pmix_status_t PMIX_ERR_INVALID_LENGTH "PMIX_ERR_INVALID_LENGTH"
    cdef pmix_status_t PMIX_ERR_INVALID_NUM_ARGS "PMIX_ERR_INVALID_NUM_ARGS"
    cdef pmix_status_t PMIX_ERR_INVALID_ARGS "PMIX_ERR_INVALID_ARGS"
    cdef pmix_status_t PMIX_ERR_INVALID_NUM_PARSED "PMIX_ERR_INVALID_NUM_PARSED"
    cdef pmix_status_t PMIX_ERR_INVALID_KEYVALP "PMIX_ERR_INVALID_KEYVALP"
    cdef pmix_status_t PMIX_ERR_INVALID_SIZE "PMIX_ERR_INVALID_SIZE"
    cdef pmix_status_t PMIX_ERR_INVALID_NAMESPACE "PMIX_ERR_INVALID_NAMESPACE"
    cdef pmix_status_t PMIX_ERR_SERVER_NOT_AVAIL "PMIX_ERR_SERVER_NOT_AVAIL"
    cdef pmix_status_t PMIX_ERR_NOT_FOUND "PMIX_ERR_NOT_FOUND"
    cdef pmix_status_t PMIX_ERR_NOT_SUPPORTED "PMIX_ERR_NOT_SUPPORTED"
    cdef pmix_status_t PMIX_ERR_NOT_IMPLEMENTED "PMIX_ERR_NOT_IMPLEMENTED"
    cdef pmix_status_t PMIX_ERR_COMM_FAILURE "PMIX_ERR_COMM_FAILURE"
    cdef pmix_status_t PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER "PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER"
    cdef pmix_status_t PMIX_ERR_CONFLICTING_CLEANUP_DIRECTIVES "PMIX_ERR_CONFLICTING_CLEANUP_DIRECTIVES"

    cdef pmix_status_t PMIX_ERR_V2X_BASE "PMIX_ERR_V2X_BASE"
    cdef pmix_status_t PMIX_ERR_LOST_CONNECTION_TO_SERVER "PMIX_ERR_LOST_CONNECTION_TO_SERVER"
    cdef pmix_status_t PMIX_ERR_LOST_PEER_CONNECTION "PMIX_ERR_LOST_PEER_CONNECTION"
    cdef pmix_status_t PMIX_ERR_LOST_CONNECTION_TO_CLIENT "PMIX_ERR_LOST_CONNECTION_TO_CLIENT"
    cdef pmix_status_t PMIX_QUERY_PARTIAL_SUCCESS "PMIX_QUERY_PARTIAL_SUCCESS"
    cdef pmix_status_t PMIX_NOTIFY_ALLOC_COMPLETE "PMIX_NOTIFY_ALLOC_COMPLETE"
    cdef pmix_status_t PMIX_JCTRL_CHECKPOINT "PMIX_JCTRL_CHECKPOINT"
    cdef pmix_status_t PMIX_JCTRL_CHECKPOINT_COMPLETE "PMIX_JCTRL_CHECKPOINT_COMPLETE"
    cdef pmix_status_t PMIX_JCTRL_PREEMPT_ALERT "PMIX_JCTRL_PREEMPT_ALERT"
    cdef pmix_status_t PMIX_MONITOR_HEARTBEAT_ALERT "PMIX_MONITOR_HEARTBEAT_ALERT"
    cdef pmix_status_t PMIX_MONITOR_FILE_ALERT "PMIX_MONITOR_FILE_ALERT"

    cdef pmix_status_t PMIX_ERR_OP_BASE "PMIX_ERR_OP_BASE"
    cdef pmix_status_t PMIX_ERR_EVENT_REGISTRATION "PMIX_ERR_EVENT_REGISTRATION"
    cdef pmix_status_t PMIX_ERR_JOB_TERMINATED "PMIX_ERR_JOB_TERMINATED"
    cdef pmix_status_t PMIX_ERR_UPDATE_ENDPOINTS "PMIX_ERR_UPDATE_ENDPOINTS"
    cdef pmix_status_t PMIX_MODEL_DECLARED "PMIX_MODEL_DECLARED"
    cdef pmix_status_t PMIX_GDS_ACTION_COMPLETE "PMIX_GDS_ACTION_COMPLETE"
    cdef pmix_status_t PMIX_PROC_HAS_CONNECTED "PMIX_PROC_HAS_CONNECTED"
    cdef pmix_status_t PMIX_CONNECT_REQUESTED "PMIX_CONNECT_REQUESTED"
    cdef pmix_status_t PMIX_MODEL_RESOURCES "PMIX_MODEL_RESOURCES"
    cdef pmix_status_t PMIX_OPENMP_PARALLEL_ENTERED "PMIX_OPENMP_PARALLEL_ENTERED"
    cdef pmix_status_t PMIX_OPENMP_PARALLEL_EXITED "PMIX_OPENMP_PARALLEL_EXITED"
    cdef pmix_status_t PMIX_LAUNCH_DIRECTIVE "PMIX_LAUNCH_DIRECTIVE"
    cdef pmix_status_t PMIX_LAUNCHER_READY "PMIX_LAUNCHER_READY"
    cdef pmix_status_t PMIX_OPERATION_IN_PROGRESS "PMIX_OPERATION_IN_PROGRESS"

    cdef pmix_status_t PMIX_ERR_SYS_BASE "PMIX_ERR_SYS_BASE"
    cdef pmix_status_t PMIX_ERR_NODE_DOWN "PMIX_ERR_NODE_DOWN"
    cdef pmix_status_t PMIX_ERR_NODE_OFFLINE "PMIX_ERR_NODE_OFFLINE"

    cdef pmix_status_t PMIX_ERR_EVHDLR_BASE "PMIX_ERR_EVHDLR_BASE"
    cdef pmix_status_t PMIX_EVENT_NO_ACTION_TAKEN "PMIX_EVENT_NO_ACTION_TAKEN"
    cdef pmix_status_t PMIX_EVENT_PARTIAL_ACTION_TAKEN "PMIX_EVENT_PARTIAL_ACTION_TAKEN"
    cdef pmix_status_t PMIX_EVENT_ACTION_DEFERRED "PMIX_EVENT_ACTION_DEFERRED"
    cdef pmix_status_t PMIX_EVENT_ACTION_COMPLETE "PMIX_EVENT_ACTION_COMPLETE"

    cdef pmix_status_t PMIX_INTERNAL_ERR_BASE "PMIX_INTERNAL_ERR_BASE"
    cdef pmix_status_t PMIX_EXTERNAL_ERR_BASE "PMIX_EXTERNAL_ERR_BASE"


    # Data type definitions
    ctypedef uint16_t pmix_data_type_t
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
    cdef pmix_data_type_t PMIX_DATA_TYPE_MAX "PMIX_DATA_TYPE_MAX"


    # Scope
    ctypedef uint8_t pmix_scope_t
    cdef pmix_scope_t PMIX_SCOPE_UNDEF "PMIX_SCOPE_UNDEF"
    cdef pmix_scope_t PMIX_LOCAL "PMIX_LOCAL"
    cdef pmix_scope_t PMIX_REMOTE "PMIX_REMOTE"
    cdef pmix_scope_t PMIX_GLOBAL "PMIX_GLOBAL"
    cdef pmix_scope_t PMIX_INTERNAL "PMIX_INTERNAL"

    # Data range
    ctypedef uint8_t pmix_data_range_t
    cdef pmix_data_range_t PMIX_RANGE_UNDEF "PMIX_RANGE_UNDEF"
    cdef pmix_data_range_t PMIX_RANGE_RM "PMIX_RANGE_RM"
    cdef pmix_data_range_t PMIX_RANGE_LOCAL "PMIX_RANGE_LOCAL"
    cdef pmix_data_range_t PMIX_RANGE_NAMESPACE "PMIX_RANGE_NAMESPACE"
    cdef pmix_data_range_t PMIX_RANGE_SESSION "PMIX_RANGE_SESSION"
    cdef pmix_data_range_t PMIX_RANGE_GLOBAL "PMIX_RANGE_GLOBAL"
    cdef pmix_data_range_t PMIX_RANGE_CUSTOM "PMIX_RANGE_CUSTOM"
    cdef pmix_data_range_t PMIX_RANGE_PROC_LOCAL "PMIX_RANGE_PROC_LOCAL"
    cdef pmix_data_range_t PMIX_RANGE_INVALID "PMIX_RANGE_INVALID"

    # Persistence
    ctypedef uint8_t pmix_persistence_t
    cdef pmix_persistence_t PMIX_PERSIST_INDEF "PMIX_PERSIST_INDEF"
    cdef pmix_persistence_t PMIX_PERSIST_FIRST_READ "PMIX_PERSIST_FIRST_READ"
    cdef pmix_persistence_t PMIX_PERSIST_PROC "PMIX_PERSIST_PROC"
    cdef pmix_persistence_t PMIX_PERSIST_APP "PMIX_PERSIST_APP"
    cdef pmix_persistence_t PMIX_PERSIST_SESSION "PMIX_PERSIST_SESSION"
    cdef pmix_persistence_t PMIX_PERSIST_INVALID "PMIX_PERSIST_INVALID"

    # Info directives
    ctypedef uint32_t pmix_info_directives_t
    cdef pmix_info_directives_t PMIX_INFO_REQD "PMIX_INFO_REQD"
    cdef pmix_info_directives_t PMIX_INFO_DIR_RESERVED "PMIX_INFO_DIR_RESERVED"

    # Allocation directives
    ctypedef uint8_t pmix_alloc_directive_t
    cdef pmix_alloc_directive_t PMIX_ALLOC_NEW "PMIX_ALLOC_NEW"
    cdef pmix_alloc_directive_t PMIX_ALLOC_EXTEND "PMIX_ALLOC_EXTEND"
    cdef pmix_alloc_directive_t PMIX_ALLOC_RELEASE "PMIX_ALLOC_RELEASE"
    cdef pmix_alloc_directive_t PMIX_ALLOC_REAQUIRE "PMIX_ALLOC_REAQUIRE"
    cdef pmix_alloc_directive_t PMIX_ALLOC_EXTERNAL "PMIX_ALLOC_EXTERNAL"

    # IOF channel definitions
    ctypedef uint16_t pmix_iof_channel_t
    cdef pmix_iof_channel_t PMIX_FWD_NO_CHANNELS "PMIX_FWD_NO_CHANNELS"
    cdef pmix_iof_channel_t PMIX_FWD_STDIN_CHANNEL "PMIX_FWD_STDIN_CHANNEL"
    cdef pmix_iof_channel_t PMIX_FWD_STDOUT_CHANNEL "PMIX_FWD_STDOUT_CHANNEL"
    cdef pmix_iof_channel_t PMIX_FWD_STDERR_CHANNEL "PMIX_FWD_STDERR_CHANNEL"
    cdef pmix_iof_channel_t PMIX_FWD_STDDIAG_CHANNEL "PMIX_FWD_STDDIAG_CHANNEL"
    cdef pmix_iof_channel_t PMIX_FWD_ALL_CHANNELS "PMIX_FWD_ALL_CHANNELS"

    # PMIx data structure definitions
    ctypedef struct pmix_byte_object_t:
        char *bytes
        size_t size

    ctypedef struct pmix_envar_t:
        char *envar
        char *value
        char separator

    ctypedef struct pmix_data_buffer_t:
        char *base_ptr
        char *pack_ptr
        char *unpack_ptr
        size_t bytes_allocated
        size_t bytes_used

    ctypedef struct pmix_proc_t:
        char nspace[CPMIX_MAX_NSLEN]
        pmix_rank_t rank

    ctypedef struct pmix_proc_info_t:
        pmix_proc_t proc
        char *hostname
        pid_t pid
        int exit_code
        pmix_proc_state_t state

    ctypedef struct pmix_data_array_t:
        pmix_data_type_t type
        size_t size
        void *array

    ctypedef struct pmix_info_array_t:
        size_t size
        pmix_info_t *array

    ctypedef struct pmix_value_t:
        pmix_data_type_t type
        bint flag
        uint8_t byte
        char *string
        size_t size
        pid_t pid
        int integer
        int8_t int8
        int16_t int16
        int32_t int32
        int64_t int64
        unsigned int uint
        uint8_t uint8
        uint16_t uint16
        uint32_t uint32
        uint64_t uint64
        float fval
        double dval
        timeval tv
        time_t time
        pmix_status_t status
        pmix_rank_t rank
        pmix_proc_t *proc
        pmix_byte_object_t bo
        pmix_persistence_t persist
        pmix_scope_t scope
        pmix_data_range_t range
        pmix_proc_state_t state
        pmix_proc_info_t *pinfo
        pmix_data_array_t *darray
        pmix_alloc_directive_t adir
        pmix_envar_t envar
        # DEPRECATED
        pmix_info_array_t *array

    ctypedef struct pmix_info_t:
        char key[CPMIX_MAX_KEYLEN]
        pmix_info_directives_t flags
        pmix_value_t value

    ctypedef struct pmix_pdata_t:
        pmix_proc_t proc
        char key[CPMIX_MAX_KEYLEN]
        pmix_value_t value

    ctypedef struct pmix_app_t:
        char *cmd
        char **argv
        char **env
        char *cwd
        int maxprocs
        pmix_info_t *info
        size_t ninfo

    ctypedef struct pmix_query_t:
        char **keys
        pmix_info_t *qualifiers
        size_t nqual

    ctypedef struct pmix_modex_data:
        char nspace[CPMIX_MAX_NSLEN]
        int rank
        uint8_t *blob
        size_t size

    # Callback function definitions
    ctypedef void (*pmix_release_cbfunc_t)(void *cbdata)
    ctypedef void (*pmix_modex_cbfunc_t)(pmix_status_t status,
                                         const char *data, size_t ndata,
                                         void *cbdata,
                                         pmix_release_cbfunc_t release_fn,
                                         void *release_cbdata)
    ctypedef void (*pmix_spawn_cbfunc_t)(pmix_status_t status,
                                         char nspace[], void *cbdata)
    ctypedef void (*pmix_op_cbfunc_t)(pmix_status_t status, void *cbdata)
    ctypedef void (*pmix_lookup_cbfunc_t)(pmix_status_t status,
                                          pmix_pdata_t data[], size_t ndata,
                                          void *cbdata)
    ctypedef void (*pmix_event_notification_cbfunc_fn_t)(pmix_status_t status,
                                                         pmix_info_t *results, size_t nresults,
                                                         pmix_op_cbfunc_t cbfunc, void *thiscbdata,
                                                         void *notification_cbdata)
    ctypedef void (*pmix_notification_fn_t)(size_t evhdlr_registration_id,
                                            pmix_status_t status,
                                            const pmix_proc_t *source,
                                            pmix_info_t info[], size_t ninfo,
                                            pmix_info_t *results, size_t nresults,
                                            pmix_event_notification_cbfunc_fn_t cbfunc,
                                            void *cbdata)
    ctypedef void (*pmix_hdlr_reg_cbfunc_t)(pmix_status_t status,
                                            size_t refid,
                                            void *cbdata)
    ctypedef void (*pmix_value_cbfunc_t)(pmix_status_t status,
                                         pmix_value_t *kv, void *cbdata)
    ctypedef void (*pmix_info_cbfunc_t)(pmix_status_t status,
                                        pmix_info_t *info, size_t ninfo,
                                        void *cbdata,
                                        pmix_release_cbfunc_t release_fn,
                                        void *release_cbdata)
    ctypedef void (*pmix_credential_cbfunc_t)(pmix_status_t status,
                                              pmix_byte_object_t *credential,
                                              pmix_info_t info[], size_t ninfo,
                                              void *cbdata)
    ctypedef void (*pmix_validation_cbfunc_t)(pmix_status_t status,
                                              pmix_info_t info[], size_t ninfo,
                                              void *cbdata)

    # PMIx Common functions
    void PMIx_Register_event_handler(pmix_status_t codes[], size_t ncodes,
                                     pmix_info_t info[], size_t ninfo,
                                     pmix_notification_fn_t evhdlr,
                                     pmix_hdlr_reg_cbfunc_t cbfunc,
                                     void *cbdata)
    void PMIx_Deregister_event_handler(size_t evhdlr_ref,
                                       pmix_op_cbfunc_t cbfunc,
                                       void *cbdata)
    pmix_status_t PMIx_Notify_event(pmix_status_t status,
                                    const pmix_proc_t *source,
                                    pmix_data_range_t range,
                                    pmix_info_t info[], size_t ninfo,
                                    pmix_op_cbfunc_t cbfunc, void *cbdata)
    const char* PMIx_Error_string(pmix_status_t status)
    const char* PMIx_Proc_state_string(pmix_proc_state_t state)
    const char* PMIx_Scope_string(pmix_scope_t scope)
    const char* PMIx_Persistence_string(pmix_persistence_t persist)
    const char* PMIx_Data_range_string(pmix_data_range_t range)
    const char* PMIx_Info_directives_string(pmix_info_directives_t directives)
    const char* PMIx_Data_type_string(pmix_data_type_t type)
    const char* PMIx_Alloc_directive_string(pmix_alloc_directive_t directive)
    const char* PMIx_IOF_channel_string(pmix_iof_channel_t channel)

    const char* PMIx_Get_version()
    pmix_status_t PMIx_Store_internal(const pmix_proc_t *proc,
                                      const char *key, pmix_value_t *val)

    pmix_status_t PMIx_Data_pack(const pmix_proc_t *target,
                                 pmix_data_buffer_t *buffer,
                                 void *src, int32_t num_vals,
                                 pmix_data_type_t type)
    pmix_status_t PMIx_Data_unpack(const pmix_proc_t *source,
                                   pmix_data_buffer_t *buffer, void *dest,
                                   int32_t *max_num_values,
                                   pmix_data_type_t type)
    pmix_status_t PMIx_Data_copy(void **dest, void *src,
                                 pmix_data_type_t type)
    pmix_status_t PMIx_Data_print(char **output, char *prefix,
                                  void *src, pmix_data_type_t type)
    pmix_status_t PMIx_Data_copy_payload(pmix_data_buffer_t *dest,
                                         pmix_data_buffer_t *src)


cdef extern from "pmix.h":
    int PMIx_Initialized()
    const char* PMIx_Get_version()
    pmix_status_t PMIx_Init(pmix_proc_t* myproc, pmix_info_t* info, size_t ninfo)


cdef extern from "pmix_server.h":
    # Callback function definitions
    ctypedef void (*pmix_connection_cbfunc_t)(int incoming_sd, void *cbdata);
    ctypedef void (*pmix_tool_connection_cbfunc_t)(pmix_status_t status,
                                                   pmix_proc_t *proc, void *cbdata);
    ctypedef void (*pmix_dmodex_response_fn_t)(pmix_status_t status,
                                               char *data, size_t sz,
                                               void *cbdata)
    ctypedef void (*pmix_setup_application_cbfunc_t)(pmix_status_t status,
                                                     pmix_info_t info[], size_t ninfo,
                                                     void *provided_cbdata,
                                                     pmix_op_cbfunc_t cbfunc, void *cbdata)

    # Server module functions
    ctypedef pmix_status_t (*pmix_server_client_connected_fn_t)(pmix_proc_t *proc, void *server_object,
                                                                pmix_op_cbfunc_t cbfunc, void *cbdata)
    ctypedef pmix_status_t (*pmix_server_client_finalized_fn_t)(const pmix_proc_t *proc, void* server_object,
                                                                pmix_op_cbfunc_t cbfunc, void *cbdata)
    ctypedef pmix_status_t (*pmix_server_abort_fn_t)(const pmix_proc_t *proc, void *server_object,
                                                     int status, const char msg[],
                                                     pmix_proc_t procs[], size_t nprocs,
                                                     pmix_op_cbfunc_t cbfunc, void *cbdata)
    ctypedef pmix_status_t (*pmix_server_fencenb_fn_t)(const pmix_proc_t procs[], size_t nprocs,
                                                       const pmix_info_t info[], size_t ninfo,
                                                       char *data, size_t ndata,
                                                       pmix_modex_cbfunc_t cbfunc, void *cbdata)
    ctypedef pmix_status_t (*pmix_server_dmodex_req_fn_t)(const pmix_proc_t *proc,
                                                          const pmix_info_t info[], size_t ninfo,
                                                          pmix_modex_cbfunc_t cbfunc, void *cbdata)
    ctypedef pmix_status_t (*pmix_server_publish_fn_t)(const pmix_proc_t *proc,
                                                       const pmix_info_t info[], size_t ninfo,
                                                       pmix_op_cbfunc_t cbfunc, void *cbdata)
    ctypedef pmix_status_t (*pmix_server_lookup_fn_t)(const pmix_proc_t *proc, char **keys,
                                                      const pmix_info_t info[], size_t ninfo,
                                                      pmix_lookup_cbfunc_t cbfunc, void *cbdata)
    ctypedef pmix_status_t (*pmix_server_unpublish_fn_t)(const pmix_proc_t *proc, char **keys,
                                                         const pmix_info_t info[], size_t ninfo,
                                                         pmix_op_cbfunc_t cbfunc, void *cbdata)
    ctypedef pmix_status_t (*pmix_server_spawn_fn_t)(const pmix_proc_t *proc,
                                                     const pmix_info_t job_info[], size_t ninfo,
                                                     const pmix_app_t apps[], size_t napps,
                                                     pmix_spawn_cbfunc_t cbfunc, void *cbdata)
    ctypedef pmix_status_t (*pmix_server_connect_fn_t)(const pmix_proc_t procs[], size_t nprocs,
                                                       const pmix_info_t info[], size_t ninfo,
                                                       pmix_op_cbfunc_t cbfunc, void *cbdata)
    ctypedef pmix_status_t (*pmix_server_disconnect_fn_t)(const pmix_proc_t procs[], size_t nprocs,
                                                          const pmix_info_t info[], size_t ninfo,
                                                          pmix_op_cbfunc_t cbfunc, void *cbdata)
    ctypedef pmix_status_t (*pmix_server_register_events_fn_t)(pmix_status_t *codes, size_t ncodes,
                                                               const pmix_info_t info[], size_t ninfo,
                                                               pmix_op_cbfunc_t cbfunc, void *cbdata)
    ctypedef pmix_status_t (*pmix_server_deregister_events_fn_t)(pmix_status_t *codes, size_t ncodes,
                                                                 pmix_op_cbfunc_t cbfunc, void *cbdata)
    ctypedef pmix_status_t (*pmix_server_notify_event_fn_t)(pmix_status_t code,
                                                            const pmix_proc_t *source,
                                                            pmix_data_range_t range,
                                                            pmix_info_t info[], size_t ninfo,
                                                            pmix_op_cbfunc_t cbfunc, void *cbdata)
    ctypedef pmix_status_t (*pmix_server_listener_fn_t)(int listening_sd,
                                                        pmix_connection_cbfunc_t cbfunc,
                                                        void *cbdata)
    ctypedef pmix_status_t (*pmix_server_query_fn_t)(pmix_proc_t *proct,
                                                     pmix_query_t *queries, size_t nqueries,
                                                     pmix_info_cbfunc_t cbfunc,
                                                     void *cbdata)
    ctypedef void (*pmix_server_tool_connection_fn_t)(pmix_info_t *info, size_t ninfo,
                                                      pmix_tool_connection_cbfunc_t cbfunc,
                                                      void *cbdata)
    ctypedef void (*pmix_server_log_fn_t)(const pmix_proc_t *client,
                                          const pmix_info_t data[], size_t ndata,
                                          const pmix_info_t directives[], size_t ndirs,
                                          pmix_op_cbfunc_t cbfunc, void *cbdata)
    ctypedef pmix_status_t (*pmix_server_alloc_fn_t)(const pmix_proc_t *client,
                                                     pmix_alloc_directive_t directive,
                                                     const pmix_info_t data[], size_t ndata,
                                                     pmix_info_cbfunc_t cbfunc, void *cbdata)
    ctypedef pmix_status_t (*pmix_server_job_control_fn_t)(const pmix_proc_t *requestor,
                                                           const pmix_proc_t targets[], size_t ntargets,
                                                           const pmix_info_t directives[], size_t ndirs,
                                                           pmix_info_cbfunc_t cbfunc, void *cbdata)
    ctypedef pmix_status_t (*pmix_server_monitor_fn_t)(const pmix_proc_t *requestor,
                                                       const pmix_info_t *monitor, pmix_status_t error,
                                                       const pmix_info_t directives[], size_t ndirs,
                                                       pmix_info_cbfunc_t cbfunc, void *cbdata)
    ctypedef pmix_status_t (*pmix_server_get_cred_fn_t)(const pmix_proc_t *proc,
                                                        const pmix_info_t directives[], size_t ndirs,
                                                        pmix_credential_cbfunc_t cbfunc, void *cbdata)
    ctypedef pmix_status_t (*pmix_server_validate_cred_fn_t)(const pmix_proc_t *proc,
                                                             const pmix_byte_object_t *cred,
                                                             const pmix_info_t directives[], size_t ndirs,
                                                             pmix_validation_cbfunc_t cbfunc, void *cbdata)
    ctypedef pmix_status_t (*pmix_server_iof_fn_t)(const pmix_proc_t procs[], size_t nprocs,
                                                   const pmix_info_t directives[], size_t ndirs,
                                                   pmix_iof_channel_t channels,
                                                   pmix_op_cbfunc_t cbfunc, void *cbdata)
    ctypedef pmix_status_t (*pmix_server_stdin_fn_t)(const pmix_proc_t *source,
                                                     const pmix_proc_t targets[], size_t ntargets,
                                                     const pmix_info_t directives[], size_t ndirs,
                                                     const pmix_byte_object_t *bo,
                                                     pmix_op_cbfunc_t cbfunc, void *cbdata)

    # Server module struct
    ctypedef struct pmix_server_module_t:
        pmix_server_client_connected_fn_t   client_connected
        pmix_server_client_finalized_fn_t   client_finalized
        pmix_server_abort_fn_t              abort
        pmix_server_fencenb_fn_t            fence_nb
        pmix_server_dmodex_req_fn_t         direct_modex
        pmix_server_publish_fn_t            publish
        pmix_server_lookup_fn_t             lookup
        pmix_server_unpublish_fn_t          unpublish
        pmix_server_spawn_fn_t              spawn
        pmix_server_connect_fn_t            connect
        pmix_server_disconnect_fn_t         disconnect
        pmix_server_register_events_fn_t    register_events
        pmix_server_deregister_events_fn_t  deregister_events
        pmix_server_listener_fn_t           listener
        # v2x interfaces
        pmix_server_notify_event_fn_t       notify_event
        pmix_server_query_fn_t              query
        pmix_server_tool_connection_fn_t    tool_connected
        pmix_server_log_fn_t                log
        pmix_server_alloc_fn_t              allocate
        pmix_server_job_control_fn_t        job_control
        pmix_server_monitor_fn_t            monitor
        # v3x interfaces
        pmix_server_get_cred_fn_t           get_credential
        pmix_server_validate_cred_fn_t      validate_credential
        pmix_server_iof_fn_t                iof_pull
        pmix_server_stdin_fn_t              push_stdin


    pmix_status_t PMIx_server_init(pmix_server_module_t *module,
                                   pmix_info_t info[], size_t ninfo)
    pmix_status_t PMIx_server_finalize()
    pmix_status_t PMIx_generate_regex(const char *input, char **regex)
    pmix_status_t PMIx_generate_ppn(const char *input, char **ppn)
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
    pmix_status_t PMIx_server_dmodex_request(const pmix_proc_t *proc,
                                             pmix_dmodex_response_fn_t cbfunc,
                                             void *cbdata)
    pmix_status_t PMIx_server_setup_application(const char nspace[],
                                                pmix_info_t info[], size_t ninfo,
                                                pmix_setup_application_cbfunc_t cbfunc, void *cbdata)
    pmix_status_t PMIx_server_setup_local_support(const char nspace[],
                                                  pmix_info_t info[], size_t ninfo,
                                                  pmix_op_cbfunc_t cbfunc, void *cbdata)
    pmix_status_t PMIx_server_IOF_deliver(const pmix_proc_t *source,
                                          pmix_iof_channel_t channel,
                                          const pmix_byte_object_t *bo,
                                          const pmix_info_t info[], size_t ninfo,
                                          pmix_op_cbfunc_t cbfunc, void *cbdata)
    pmix_status_t PMIx_server_collect_inventory(pmix_info_t directives[], size_t ndirs,
                                                pmix_info_cbfunc_t cbfunc, void *cbdata)
    pmix_status_t PMIx_server_deliver_inventory(pmix_info_t info[], size_t ninfo,
                                                pmix_info_t directives[], size_t ndirs,
                                                pmix_op_cbfunc_t cbfunc, void *cbdata)
