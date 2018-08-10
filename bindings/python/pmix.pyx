#file: pmix.pyx

from libc.string cimport memset,strncpy, strdup
from libc.stdlib cimport malloc, free
from libc.string cimport memcpy
from ctypes import addressof, c_int
from cython.operator import address

# pull in all the constant definitions - we
# store them in a separate file for neatness
include "pmix_constants.pxi"
include "pmix.pxi"


cdef class PMIxClient:
    cdef pmix_proc_t myproc;
    def __init__(self):
        memset(self.myproc.nspace, 0, sizeof(self.myproc.nspace))
        self.myproc.rank = <uint32_t>PMIX_RANK_UNDEF

    def initialized(self):
        return PMIx_Initialized()

    def get_version(self):
        return PMIx_Get_version()

    # Initialize the PMIx client library, connecting
    # us to the local PMIx server
    #
    # @keyvals [INPUT]
    #          - a dictionary of key-value pairs
    def init(self, keyvals):
        cdef bytes pykey
        cdef pmix_info_t info
        cdef size_t ninfo
        cdef size_t klen
        cdef const char *pykeyptr
        # Convert any provided dictionary to an array of pmix_info_t
        kvkeys = list(keyvals.keys())
        klen = len(kvkeys)
        try:
            inarray = PMIxInfoArray(klen)
        except:
            print("Unable to create info array")
            return -1
        inarray.load(keyvals)
        print(keyvals)
        return PMIx_Init(&self.myproc, inarray.array, klen)

    # Finalize the client library
    def finalize(self, keyvals):
        cdef pmix_info_t info
        cdef size_t ninfo = 0
        return PMIx_Finalize(NULL, ninfo)

    # private function to convert key-value tuple
    # to pmix_info_t
    def __convert_value(self, char* dest, char* src):
        strncpy(dest, src, 511)

pmixservermodule = {}
def setmodulefn(k, f):
    global pmixservermodule
    permitted = ['clientconnected', 'clientfinalized', 'abort',
                 'fencenb', 'directmodex', 'publish', 'lookup', 'unpublish',
                 'spawn', 'connect', 'disconnect', 'registerevents',
                 'deregisterevents', 'listener', 'notify_event', 'query',
                 'toolconnected', 'log', 'allocate', 'jobcontrol',
                 'monitor', 'getcredential', 'validatecredential',
                 'iofpull', 'pushstdin']
    if k not in permitted:
        raise KeyError
    if not k in pmixservermodule:
        print("SETTING MODULE FN FOR ", k)
        pmixservermodule[k] = f

cdef class PMIxServer(PMIxClient):
    cdef pmix_server_module_t myserver;
    def __init__(self):
        memset(self.myproc.nspace, 0, sizeof(self.myproc.nspace))
        self.myproc.rank = PMIX_RANK_UNDEF
        # v1.x interfaces
        self.myserver.client_connected = <pmix_server_client_connected_fn_t>clientconnected
        self.myserver.client_finalized = <pmix_server_client_finalized_fn_t>clientfinalized
        self.myserver.abort = <pmix_server_abort_fn_t>clientaborted
        self.myserver.fence_nb = <pmix_server_fencenb_fn_t>fencenb
        self.myserver.direct_modex = <pmix_server_dmodex_req_fn_t>directmodex
        self.myserver.publish = <pmix_server_publish_fn_t>publish
        self.myserver.lookup = <pmix_server_lookup_fn_t>lookup
        self.myserver.unpublish = <pmix_server_unpublish_fn_t>unpublish
        self.myserver.spawn = <pmix_server_spawn_fn_t>spawn
        self.myserver.connect = <pmix_server_connect_fn_t>connect
        self.myserver.disconnect = <pmix_server_disconnect_fn_t>disconnect
        self.myserver.register_events = <pmix_server_register_events_fn_t>registerevents
        self.myserver.deregister_events = <pmix_server_deregister_events_fn_t>deregisterevents
        # skip the listener entry as Python servers will never
        # provide their own socket listener thread
        #
        # v2.x interfaces
        self.myserver.notify_event = <pmix_server_notify_event_fn_t>notifyevent
        self.myserver.query = <pmix_server_query_fn_t>query
        self.myserver.tool_connected = <pmix_server_tool_connection_fn_t>toolconnected
        self.myserver.log = <pmix_server_log_fn_t>log
        self.myserver.allocate = <pmix_server_alloc_fn_t>allocate
        self.myserver.job_control = <pmix_server_job_control_fn_t>jobcontrol
        self.myserver.monitor = <pmix_server_monitor_fn_t>monitor
        # v3.x interfaces
        self.myserver.get_credential = <pmix_server_get_cred_fn_t>getcredential
        self.myserver.validate_credential = <pmix_server_validate_cred_fn_t>validatecredential
        self.myserver.iof_pull = <pmix_server_iof_fn_t>iofpull
        self.myserver.push_stdin = <pmix_server_stdin_fn_t>pushstdin

    def initialized(self):
        return PMIx_Initialized()

    def get_version(self):
        return PMIx_Get_version()

    # Initialize the PMIx server library
    #
    # @keyvals [INPUT]
    #          - a dictionary of key-value pairs to be passed
    #            as pmix_info_t to PMIx_server_init
    #
    # @map [INPUT]
    #          - a dictionary of key-function pairs that map
    #            server module callback functions to provided
    #            implementations
    def init(self, keyvals, map):
        if map is None or 0 == len(map):
            print("SERVER REQUIRES AT LEAST ONE MODULE FUNCTION TO OPERATE")
            return PMIX_ERR_INIT
        kvkeys = list(map.keys())
        for key in kvkeys:
            try:
                setmodulefn(key, map[key])
            except KeyError:
                print("SERVER MODULE FUNCTION ", key, " IS NOT RECOGNIZED")
                return PMIX_ERR_INIT
        # Convert any provided dictionary to an array of pmix_info_t
        if keyvals is not None:
            kvkeys = list(keyvals.keys())
            klen = len(kvkeys)
            try:
                inarray = PMIxInfoArray(klen)
            except:
                print("Unable to create info array")
                return -1
            inarray.load(keyvals)
            print(keyvals)
            rc = PMIx_server_init(&self.myserver, inarray.array, klen)
        else:
            rc = PMIx_server_init(&self.myserver, NULL, 0)
        return rc

    def finalize(self):
        return PMIx_server_finalize()

    def register_nspace(self, args):
        # convert the args into the necessary C-arguments
        pass

cdef int clientconnected(pmix_proc_t *proc, void *server_object,
                         pmix_op_cbfunc_t cbfunc, void *cbdata) with gil:
    keys = pmixservermodule.keys()
    if 'clientconnected' in keys:
    #    args = pmixproc_to_py(proc)
        args = {}
        rc = pmixservermodule['clientconnected'](args)
    else:
        return PMIX_ERR_NOT_SUPPORTED
    # we cannot execute a callback function here as
    # that would cause PMIx to lockup. Likewise, the
    # Python function we called can't do it as it
    # would require them to call a C-function. So
    # if they succeeded in processing this request,
    # we return a PMIX_OPERATION_SUCCEEDED status
    # that let's the underlying PMIx library know
    # the situation so it can generate its own
    # callback
    if PMIX_SUCCESS == rc:
        rc = PMIX_OPERATION_SUCCEEDED
    return rc

cdef int clientfinalized(pmix_proc_t *proc, void *server_object,
                         pmix_op_cbfunc_t cbfunc, void *cbdata) with gil:
    keys = pmixservermodule.keys()
    if 'clientfinalized' in keys:
    #    args = pmixproc_to_py(proc)
        args = {}
        rc = pmixservermodule['clientfinalized'](args)
    else:
        return PMIX_ERR_NOT_SUPPORTED
    # we cannot execute a callback function here as
    # that would cause PMIx to lockup. Likewise, the
    # Python function we called can't do it as it
    # would require them to call a C-function. So
    # if they succeeded in processing this request,
    # we return a PMIX_OPERATION_SUCCEEDED status
    # that let's the underlying PMIx library know
    # the situation so it can generate its own
    # callback
    if PMIX_SUCCESS == rc:
        rc = PMIX_OPERATION_SUCCEEDED
    return rc

cdef int clientaborted(const pmix_proc_t *proc, void *server_object,
                       int status, const char msg[],
                       pmix_proc_t procs[], size_t nprocs,
                       pmix_op_cbfunc_t cbfunc, void *cbdata) with gil:
    keys = pmixservermodule.keys()
    if 'abort' in keys:
    #    args = pmixproc_to_py(proc)
        args = {}
        rc = pmixservermodule['abort'](args)
    else:
        return PMIX_ERR_NOT_SUPPORTED
    # we cannot execute a callback function here as
    # that would cause PMIx to lockup. Likewise, the
    # Python function we called can't do it as it
    # would require them to call a C-function. So
    # if they succeeded in processing this request,
    # we return a PMIX_OPERATION_SUCCEEDED status
    # that let's the underlying PMIx library know
    # the situation so it can generate its own
    # callback
    if PMIX_SUCCESS == rc:
        rc = PMIX_OPERATION_SUCCEEDED
    return rc

cdef int fencenb(const pmix_proc_t procs[], size_t nprocs,
                 const pmix_info_t info[], size_t ninfo,
                 char *data, size_t ndata,
                 pmix_modex_cbfunc_t cbfunc, void *cbdata) with gil:
    keys = pmixservermodule.keys()
    if 'fencenb' in keys:
    #    args = pmixproc_to_py(proc)
        args = {}
        rc = pmixservermodule['fencenb'](args)
    else:
        return PMIX_ERR_NOT_SUPPORTED
    # we cannot execute a callback function here as
    # that would cause PMIx to lockup. Likewise, the
    # Python function we called can't do it as it
    # would require them to call a C-function. So
    # if they succeeded in processing this request,
    # we return a PMIX_OPERATION_SUCCEEDED status
    # that let's the underlying PMIx library know
    # the situation so it can generate its own
    # callback
    if PMIX_SUCCESS == rc:
        rc = PMIX_OPERATION_SUCCEEDED
    return rc

cdef int directmodex(const pmix_proc_t *proc,
                     const pmix_info_t info[], size_t ninfo,
                     pmix_modex_cbfunc_t cbfunc, void *cbdata) with gil:
    keys = pmixservermodule.keys()
    if 'directmodex' in keys:
    #    args = pmixproc_to_py(proc)
        args = {}
        rc = pmixservermodule['directmodex'](args)
    else:
        return PMIX_ERR_NOT_SUPPORTED
    # we cannot execute a callback function here as
    # that would cause PMIx to lockup. Likewise, the
    # Python function we called can't do it as it
    # would require them to call a C-function. So
    # if they succeeded in processing this request,
    # we return a PMIX_OPERATION_SUCCEEDED status
    # that let's the underlying PMIx library know
    # the situation so it can generate its own
    # callback
    if PMIX_SUCCESS == rc:
        rc = PMIX_OPERATION_SUCCEEDED
    return rc

cdef int publish(const pmix_proc_t *proc,
                 const pmix_info_t info[], size_t ninfo,
                 pmix_op_cbfunc_t cbfunc, void *cbdata) with gil:
    keys = pmixservermodule.keys()
    if 'publish' in keys:
    #    args = pmixproc_to_py(proc)
        args = {}
        rc = pmixservermodule['publish'](args)
    else:
        return PMIX_ERR_NOT_SUPPORTED
    # we cannot execute a callback function here as
    # that would cause PMIx to lockup. Likewise, the
    # Python function we called can't do it as it
    # would require them to call a C-function. So
    # if they succeeded in processing this request,
    # we return a PMIX_OPERATION_SUCCEEDED status
    # that let's the underlying PMIx library know
    # the situation so it can generate its own
    # callback
    if PMIX_SUCCESS == rc:
        rc = PMIX_OPERATION_SUCCEEDED
    return rc

cdef int lookup(const pmix_proc_t *proc, char **keys,
                const pmix_info_t info[], size_t ninfo,
                pmix_lookup_cbfunc_t cbfunc, void *cbdata) with gil:
    srvkeys = pmixservermodule.keys()
    if 'lookup' in srvkeys:
    #    args = pmixproc_to_py(proc)
        args = {}
        rc = pmixservermodule['lookup'](args)
    else:
        return PMIX_ERR_NOT_SUPPORTED
    # we cannot execute a callback function here as
    # that would cause PMIx to lockup. Likewise, the
    # Python function we called can't do it as it
    # would require them to call a C-function. So
    # if they succeeded in processing this request,
    # we return a PMIX_OPERATION_SUCCEEDED status
    # that let's the underlying PMIx library know
    # the situation so it can generate its own
    # callback
    if PMIX_SUCCESS == rc:
        rc = PMIX_OPERATION_SUCCEEDED
    return rc

cdef int unpublish(const pmix_proc_t *proc, char **keys,
                   const pmix_info_t info[], size_t ninfo,
                   pmix_op_cbfunc_t cbfunc, void *cbdata) with gil:
    srvkeys = pmixservermodule.keys()
    if 'unpublish' in srvkeys:
    #    args = pmixproc_to_py(proc)
        args = {}
        rc = pmixservermodule['unpublish'](args)
    else:
        return PMIX_ERR_NOT_SUPPORTED
    # we cannot execute a callback function here as
    # that would cause PMIx to lockup. Likewise, the
    # Python function we called can't do it as it
    # would require them to call a C-function. So
    # if they succeeded in processing this request,
    # we return a PMIX_OPERATION_SUCCEEDED status
    # that let's the underlying PMIx library know
    # the situation so it can generate its own
    # callback
    if PMIX_SUCCESS == rc:
        rc = PMIX_OPERATION_SUCCEEDED
    return rc

cdef int spawn(const pmix_proc_t *proc,
               const pmix_info_t job_info[], size_t ninfo,
               const pmix_app_t apps[], size_t napps,
               pmix_spawn_cbfunc_t cbfunc, void *cbdata) with gil:
    keys = pmixservermodule.keys()
    if 'spawn' in keys:
    #    args = pmixproc_to_py(proc)
        args = {}
        rc = pmixservermodule['spawn'](args)
    else:
        return PMIX_ERR_NOT_SUPPORTED
    # we cannot execute a callback function here as
    # that would cause PMIx to lockup. Likewise, the
    # Python function we called can't do it as it
    # would require them to call a C-function. So
    # if they succeeded in processing this request,
    # we return a PMIX_OPERATION_SUCCEEDED status
    # that let's the underlying PMIx library know
    # the situation so it can generate its own
    # callback
    if PMIX_SUCCESS == rc:
        rc = PMIX_OPERATION_SUCCEEDED
    return rc

cdef int connect(const pmix_proc_t procs[], size_t nprocs,
                 const pmix_info_t info[], size_t ninfo,
                 pmix_op_cbfunc_t cbfunc, void *cbdata) with gil:
    keys = pmixservermodule.keys()
    if 'connect' in keys:
    #    args = pmixproc_to_py(proc)
        args = {}
        rc = pmixservermodule['connect'](args)
    else:
        return PMIX_ERR_NOT_SUPPORTED
    # we cannot execute a callback function here as
    # that would cause PMIx to lockup. Likewise, the
    # Python function we called can't do it as it
    # would require them to call a C-function. So
    # if they succeeded in processing this request,
    # we return a PMIX_OPERATION_SUCCEEDED status
    # that let's the underlying PMIx library know
    # the situation so it can generate its own
    # callback
    if PMIX_SUCCESS == rc:
        rc = PMIX_OPERATION_SUCCEEDED
    return rc

cdef int disconnect(const pmix_proc_t procs[], size_t nprocs,
                    const pmix_info_t info[], size_t ninfo,
                    pmix_op_cbfunc_t cbfunc, void *cbdata) with gil:
    keys = pmixservermodule.keys()
    if 'disconnect' in keys:
    #    args = pmixproc_to_py(proc)
        args = {}
        rc = pmixservermodule['disconnect'](args)
    else:
        return PMIX_ERR_NOT_SUPPORTED
    # we cannot execute a callback function here as
    # that would cause PMIx to lockup. Likewise, the
    # Python function we called can't do it as it
    # would require them to call a C-function. So
    # if they succeeded in processing this request,
    # we return a PMIX_OPERATION_SUCCEEDED status
    # that let's the underlying PMIx library know
    # the situation so it can generate its own
    # callback
    if PMIX_SUCCESS == rc:
        rc = PMIX_OPERATION_SUCCEEDED
    return rc

cdef int registerevents(pmix_status_t *codes, size_t ncodes,
                        const pmix_info_t info[], size_t ninfo,
                        pmix_op_cbfunc_t cbfunc, void *cbdata) with gil:
    keys = pmixservermodule.keys()
    if 'registerevents' in keys:
    #    args = pmixproc_to_py(proc)
        args = {}
        rc = pmixservermodule['registerevents'](args)
    else:
        return PMIX_ERR_NOT_SUPPORTED
    # we cannot execute a callback function here as
    # that would cause PMIx to lockup. Likewise, the
    # Python function we called can't do it as it
    # would require them to call a C-function. So
    # if they succeeded in processing this request,
    # we return a PMIX_OPERATION_SUCCEEDED status
    # that let's the underlying PMIx library know
    # the situation so it can generate its own
    # callback
    if PMIX_SUCCESS == rc:
        rc = PMIX_OPERATION_SUCCEEDED
    return rc

cdef int deregisterevents(pmix_status_t *codes, size_t ncodes,
                          pmix_op_cbfunc_t cbfunc, void *cbdata) with gil:
    keys = pmixservermodule.keys()
    if 'deregisterevents' in keys:
    #    args = pmixproc_to_py(proc)
        args = {}
        rc = pmixservermodule['deregisterevents'](args)
    else:
        return PMIX_ERR_NOT_SUPPORTED
    # we cannot execute a callback function here as
    # that would cause PMIx to lockup. Likewise, the
    # Python function we called can't do it as it
    # would require them to call a C-function. So
    # if they succeeded in processing this request,
    # we return a PMIX_OPERATION_SUCCEEDED status
    # that let's the underlying PMIx library know
    # the situation so it can generate its own
    # callback
    if PMIX_SUCCESS == rc:
        rc = PMIX_OPERATION_SUCCEEDED
    return rc

cdef int notifyevent(pmix_status_t code,
                     const pmix_proc_t *source,
                     pmix_data_range_t range,
                     pmix_info_t info[], size_t ninfo,
                     pmix_op_cbfunc_t cbfunc, void *cbdata) with gil:
    keys = pmixservermodule.keys()
    if 'notifyevent' in keys:
    #    args = pmixproc_to_py(proc)
        args = {}
        rc = pmixservermodule['notifyevent'](args)
    else:
        return PMIX_ERR_NOT_SUPPORTED
    # we cannot execute a callback function here as
    # that would cause PMIx to lockup. Likewise, the
    # Python function we called can't do it as it
    # would require them to call a C-function. So
    # if they succeeded in processing this request,
    # we return a PMIX_OPERATION_SUCCEEDED status
    # that let's the underlying PMIx library know
    # the situation so it can generate its own
    # callback
    if PMIX_SUCCESS == rc:
        rc = PMIX_OPERATION_SUCCEEDED
    return rc

cdef int query(pmix_proc_t *proct,
               pmix_query_t *queries, size_t nqueries,
               pmix_info_cbfunc_t cbfunc,
               void *cbdata) with gil:
    keys = pmixservermodule.keys()
    if 'query' in keys:
    #    args = pmixproc_to_py(proc)
        args = {}
        rc = pmixservermodule['query'](args)
    else:
        return PMIX_ERR_NOT_SUPPORTED
    # we cannot execute a callback function here as
    # that would cause PMIx to lockup. Likewise, the
    # Python function we called can't do it as it
    # would require them to call a C-function. So
    # if they succeeded in processing this request,
    # we return a PMIX_OPERATION_SUCCEEDED status
    # that let's the underlying PMIx library know
    # the situation so it can generate its own
    # callback
    if PMIX_SUCCESS == rc:
        rc = PMIX_OPERATION_SUCCEEDED
    return rc

cdef void toolconnected(pmix_info_t *info, size_t ninfo,
                        pmix_tool_connection_cbfunc_t cbfunc,
                        void *cbdata) with gil:
    keys = pmixservermodule.keys()
    if 'toolconnected' in keys:
    #    args = pmixproc_to_py(proc)
        args = {}
        rc = pmixservermodule['toolconnected'](args)
    else:
        rc = PMIX_ERR_NOT_SUPPORTED
    # we cannot execute a callback function here as
    # that would cause PMIx to lockup. Likewise, the
    # Python function we called can't do it as it
    # would require them to call a C-function. So
    # if they succeeded in processing this request,
    # we return a PMIX_OPERATION_SUCCEEDED status
    # that let's the underlying PMIx library know
    # the situation so it can generate its own
    # callback
    if PMIX_SUCCESS == rc:
        rc = PMIX_OPERATION_SUCCEEDED
    return

cdef void log(const pmix_proc_t *client,
              const pmix_info_t data[], size_t ndata,
              const pmix_info_t directives[], size_t ndirs,
              pmix_op_cbfunc_t cbfunc, void *cbdata) with gil:
    keys = pmixservermodule.keys()
    if 'log' in keys:
    #    args = pmixproc_to_py(proc)
        args = {}
        rc = pmixservermodule['log'](args)
    else:
        rc = PMIX_ERR_NOT_SUPPORTED
    # we cannot execute a callback function here as
    # that would cause PMIx to lockup. Likewise, the
    # Python function we called can't do it as it
    # would require them to call a C-function. So
    # if they succeeded in processing this request,
    # we return a PMIX_OPERATION_SUCCEEDED status
    # that let's the underlying PMIx library know
    # the situation so it can generate its own
    # callback
    if PMIX_SUCCESS == rc:
        rc = PMIX_OPERATION_SUCCEEDED
    return

cdef int allocate(const pmix_proc_t *client,
                  pmix_alloc_directive_t directive,
                  const pmix_info_t data[], size_t ndata,
                  pmix_info_cbfunc_t cbfunc, void *cbdata) with gil:
    keys = pmixservermodule.keys()
    if 'allocate' in keys:
    #    args = pmixproc_to_py(proc)
        args = {}
        rc = pmixservermodule['allocate'](args)
    else:
        rc = PMIX_ERR_NOT_SUPPORTED
    # we cannot execute a callback function here as
    # that would cause PMIx to lockup. Likewise, the
    # Python function we called can't do it as it
    # would require them to call a C-function. So
    # if they succeeded in processing this request,
    # we return a PMIX_OPERATION_SUCCEEDED status
    # that let's the underlying PMIx library know
    # the situation so it can generate its own
    # callback
    if PMIX_SUCCESS == rc:
        rc = PMIX_OPERATION_SUCCEEDED
    return rc

cdef int jobcontrol(const pmix_proc_t *requestor,
                    const pmix_proc_t targets[], size_t ntargets,
                    const pmix_info_t directives[], size_t ndirs,
                    pmix_info_cbfunc_t cbfunc, void *cbdata) with gil:
    keys = pmixservermodule.keys()
    if 'jobcontrol' in keys:
    #    args = pmixproc_to_py(proc)
        args = {}
        rc = pmixservermodule['jobcontrol'](args)
    else:
        rc = PMIX_ERR_NOT_SUPPORTED
    # we cannot execute a callback function here as
    # that would cause PMIx to lockup. Likewise, the
    # Python function we called can't do it as it
    # would require them to call a C-function. So
    # if they succeeded in processing this request,
    # we return a PMIX_OPERATION_SUCCEEDED status
    # that let's the underlying PMIx library know
    # the situation so it can generate its own
    # callback
    if PMIX_SUCCESS == rc:
        rc = PMIX_OPERATION_SUCCEEDED
    return rc

cdef int monitor(const pmix_proc_t *requestor,
                 const pmix_info_t *monitor, pmix_status_t error,
                 const pmix_info_t directives[], size_t ndirs,
                 pmix_info_cbfunc_t cbfunc, void *cbdata) with gil:
    keys = pmixservermodule.keys()
    if 'monitor' in keys:
    #    args = pmixproc_to_py(proc)
        args = {}
        rc = pmixservermodule['monitor'](args)
    else:
        rc = PMIX_ERR_NOT_SUPPORTED
    # we cannot execute a callback function here as
    # that would cause PMIx to lockup. Likewise, the
    # Python function we called can't do it as it
    # would require them to call a C-function. So
    # if they succeeded in processing this request,
    # we return a PMIX_OPERATION_SUCCEEDED status
    # that let's the underlying PMIx library know
    # the situation so it can generate its own
    # callback
    if PMIX_SUCCESS == rc:
        rc = PMIX_OPERATION_SUCCEEDED
    return rc

cdef int getcredential(const pmix_proc_t *proc,
                       const pmix_info_t directives[], size_t ndirs,
                       pmix_credential_cbfunc_t cbfunc, void *cbdata) with gil:
    keys = pmixservermodule.keys()
    if 'getcredential' in keys:
    #    args = pmixproc_to_py(proc)
        args = {}
        rc = pmixservermodule['getcredential'](args)
    else:
        rc = PMIX_ERR_NOT_SUPPORTED
    # we cannot execute a callback function here as
    # that would cause PMIx to lockup. Likewise, the
    # Python function we called can't do it as it
    # would require them to call a C-function. So
    # if they succeeded in processing this request,
    # we return a PMIX_OPERATION_SUCCEEDED status
    # that let's the underlying PMIx library know
    # the situation so it can generate its own
    # callback
    if PMIX_SUCCESS == rc:
        rc = PMIX_OPERATION_SUCCEEDED
    return rc

cdef int validatecredential(const pmix_proc_t *proc,
                            const pmix_byte_object_t *cred,
                            const pmix_info_t directives[], size_t ndirs,
                            pmix_validation_cbfunc_t cbfunc, void *cbdata) with gil:
    keys = pmixservermodule.keys()
    if 'validatecredential' in keys:
    #    args = pmixproc_to_py(proc)
        args = {}
        rc = pmixservermodule['validatecredential'](args)
    else:
        rc = PMIX_ERR_NOT_SUPPORTED
    # we cannot execute a callback function here as
    # that would cause PMIx to lockup. Likewise, the
    # Python function we called can't do it as it
    # would require them to call a C-function. So
    # if they succeeded in processing this request,
    # we return a PMIX_OPERATION_SUCCEEDED status
    # that let's the underlying PMIx library know
    # the situation so it can generate its own
    # callback
    if PMIX_SUCCESS == rc:
        rc = PMIX_OPERATION_SUCCEEDED
    return rc

cdef int iofpull(const pmix_proc_t procs[], size_t nprocs,
                 const pmix_info_t directives[], size_t ndirs,
                 pmix_iof_channel_t channels,
                 pmix_op_cbfunc_t cbfunc, void *cbdata) with gil:
    keys = pmixservermodule.keys()
    if 'iofpull' in keys:
    #    args = pmixproc_to_py(proc)
        args = {}
        rc = pmixservermodule['iofpull'](args)
    else:
        rc = PMIX_ERR_NOT_SUPPORTED
    # we cannot execute a callback function here as
    # that would cause PMIx to lockup. Likewise, the
    # Python function we called can't do it as it
    # would require them to call a C-function. So
    # if they succeeded in processing this request,
    # we return a PMIX_OPERATION_SUCCEEDED status
    # that let's the underlying PMIx library know
    # the situation so it can generate its own
    # callback
    if PMIX_SUCCESS == rc:
        rc = PMIX_OPERATION_SUCCEEDED
    return rc

cdef int pushstdin(const pmix_proc_t *source,
                   const pmix_proc_t targets[], size_t ntargets,
                   const pmix_info_t directives[], size_t ndirs,
                   const pmix_byte_object_t *bo,
                   pmix_op_cbfunc_t cbfunc, void *cbdata) with gil:
    keys = pmixservermodule.keys()
    if 'pushstdin' in keys:
    #    args = pmixproc_to_py(proc)
        args = {}
        rc = pmixservermodule['pushstdin'](args)
    else:
        rc = PMIX_ERR_NOT_SUPPORTED
    # we cannot execute a callback function here as
    # that would cause PMIx to lockup. Likewise, the
    # Python function we called can't do it as it
    # would require them to call a C-function. So
    # if they succeeded in processing this request,
    # we return a PMIX_OPERATION_SUCCEEDED status
    # that let's the underlying PMIx library know
    # the situation so it can generate its own
    # callback
    if PMIX_SUCCESS == rc:
        rc = PMIX_OPERATION_SUCCEEDED
    return rc
