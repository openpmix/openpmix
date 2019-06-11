#file: pmix.pyx

from libc.string cimport memset, strncpy, strcpy, strlen, strdup
from libc.stdlib cimport malloc, free
from libc.string cimport memcpy
from ctypes import addressof, c_int
from cython.operator import address
import signal, time
import threading
import array

# pull in all the constant definitions - we
# store them in a separate file for neatness
include "pmix_constants.pxi"
include "pmix.pxi"

active = myLock()

cdef void pmix_opcbfunc(pmix_status_t status, void *cbdata) with gil:
    global active
    active.set(status)
    return

cdef void dmodx_cbfunc(pmix_status_t status,
                       char *data, size_t sz,
                       void *cbdata) with gil:
    global active
    if PMIX_SUCCESS == status:
        active.cache_data(data, sz)
    active.set(status)
    return

cdef void setupapp_cbfunc(pmix_status_t status,
                          pmix_info_t info[], size_t ninfo,
                          void *provided_cbdata,
                          pmix_op_cbfunc_t cbfunc, void *cbdata) with gil:
    global active
    if PMIX_SUCCESS == status:
        ilist = []
        rc = pmix_unload_info(info, ninfo, ilist)
        active.cache_info(ilist)
        status = rc
    active.set(status)
    cbfunc(PMIX_SUCCESS, cbdata)
    return

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
    def init(self, keyvals:dict):
        cdef pmix_info_t *info
        cdef size_t klen
        # Convert any provided dictionary to an array of pmix_info_t
        if keyvals is not None:
            kvkeys = list(keyvals.keys())
            klen = len(kvkeys)
            if 0 < klen:
                info = <pmix_info_t*> PyMem_Malloc(klen * sizeof(pmix_info_t))
                if not info:
                    raise MemoryError()
                pmix_load_info(info, keyvals)
            else:
                info = NULL
                klen = 0
        else:
            info = NULL
            klen = 0
        rc = PMIx_Init(&self.myproc, info, klen)
        if 0 < klen:
            pmix_free_info(info, klen)
        return rc

    # Finalize the client library
    def finalize(self, keyvals:dict):
        cdef pmix_info_t *info
        cdef size_t klen
        if keyvals is not None:
            kvkeys = list(keyvals.keys())
            klen = len(kvkeys)
            if 0 < klen:
                info = <pmix_info_t*> PyMem_Malloc(klen * sizeof(pmix_info_t))
                if not info:
                    raise MemoryError()
                pmix_load_info(info, keyvals)
            else:
                info = NULL
                klen = 0
        else:
            info = NULL
            klen = 0
        rc = PMIx_Finalize(info, klen)
        if 0 < klen:
            pmix_free_info(info, klen)
        return rc

    def initialized(self):
        return PMIx_Initialized()

    # Request that the provided array of procs be aborted, returning the
    # provided _status_ and printing the provided message.
    #
    # @status [INPUT]
    #         - PMIx status to be returned on exit
    #
    # @msg [INPUT]
    #        - string message to be printed
    #
    # @procs [INPUT]
    #        - list of proc nspace,rank tuples
    def abort(self, status, msg, peers:list):
        cdef pmix_proc_t *procs
        cdef size_t sz
        # convert list of procs to array of pmix_proc_t's
        if peers is not None:
            sz = len(peers)
            if 0 < sz:
                procs = <pmix_proc_t*> PyMem_Malloc(sz * sizeof(pmix_proc_t))
                if not procs:
                    raise MemoryError()
                rc = pmix_load_procs(procs, peers)
                if PMIX_SUCCESS != rc:
                    pmix_free_procs(procs, sz)
                    return rc
            else:
                # if they didn't give us a set of procs,
                # then we default to our entire job
                sz = 1
                procs = <pmix_proc_t*> PyMem_Malloc(sz * sizeof(pmix_proc_t))
                if not procs:
                    raise MemoryError()
                pmix_copy_nspace(procs[0].nspace, self.myproc.nspace)
                procs[0].rank = PMIX_RANK_WILDCARD
        else:
            # if they didn't give us a set of procs,
            # then we default to our entire job
            sz = 1
            procs = <pmix_proc_t*> PyMem_Malloc(sz * sizeof(pmix_proc_t))
            if not procs:
                raise MemoryError()
            pmix_copy_nspace(procs[0].nspace, self.myproc.nspace)
            procs[0].rank = PMIX_RANK_WILDCARD
        if isinstance(msg, str):
            pymsg = msg.encode('ascii')
        else:
            pymsg = msg
        # pass into PMIx_Abort
        rc = PMIx_Abort(status, pymsg, procs, sz)
        if 0 < sz:
            pmix_free_procs(procs, sz)
        return rc

    def get_version(self):
        return PMIx_Get_version()

    # put a value into the keystore
    #
    # @scope [INPUT]
    #        - the scope of the data
    #
    # @key [INPUT]
    #      - the key to be stored
    #
    # @value [INPUT]
    #        - a value tuple to be stored (value, type)
    def put(self, scope, ky, val):
        cdef pmix_key_t key
        cdef pmix_value_t value
        # convert the keyval tuple to a pmix_info_t
        pmix_copy_key(key, ky)
        pmix_load_value(&value, val)
        # pass it into the PMIx_Put function
        rc = PMIx_Put(scope, key, &value)
        pmix_destruct_value(&value)
        return rc

    def commit(self):
        rc = PMIx_Commit()
        return rc

    def fence(self, peers:list, keyvals:dict):
        cdef pmix_proc_t *procs
        cdef pmix_info_t *info
        cdef size_t ninfo, nprocs
        nprocs = 0
        ninfo = 0
        # convert list of procs to array of pmix_proc_t's
        if peers is not None:
            nprocs = len(peers)
            if 0 < nprocs:
                procs = <pmix_proc_t*> PyMem_Malloc(nprocs * sizeof(pmix_proc_t))
                if not procs:
                    raise MemoryError()
                rc = pmix_load_procs(procs, peers)
                if PMIX_SUCCESS != rc:
                    pmix_free_procs(procs, nprocs)
                    return rc
            else:
                nprocs = 1
                procs = <pmix_proc_t*> PyMem_Malloc(nprocs * sizeof(pmix_proc_t))
                if not procs:
                    raise MemoryError()
                pmix_copy_nspace(procs[0].nspace, self.myproc.nspace)
                procs[0].rank = PMIX_RANK_WILDCARD
        else:
            nprocs = 1
            procs = <pmix_proc_t*> PyMem_Malloc(nprocs * sizeof(pmix_proc_t))
            if not procs:
                raise MemoryError()
            pmix_copy_nspace(procs[0].nspace, self.myproc.nspace)
            procs[0].rank = PMIX_RANK_WILDCARD
        # convert the keyval dictionary to array of
        # pmix_info_t structs
        if keyvals is not None:
            kvkeys = list(keyvals.keys())
            ninfo = len(kvkeys)
            if 0 < ninfo:
                info = <pmix_info_t*> PyMem_Malloc(ninfo * sizeof(pmix_info_t))
                if not info:
                    raise MemoryError()
                rc = pmix_load_info(info, keyvals)
                if PMIX_SUCCESS != rc:
                    pmix_free_procs(procs, nprocs)
                    pmix_free_info(info, ninfo)
                    return rc
            else:
                info = NULL
        else:
            info = NULL
        # pass it into the fence API
        print("FENCE", nprocs, ninfo)
        rc = PMIx_Fence(procs, nprocs, info, ninfo)
        if 0 < nprocs:
            pmix_free_procs(procs, nprocs)
        if 0 < ninfo:
            pmix_free_info(info, ninfo)
        return rc

pmixservermodule = {}
def setmodulefn(k, f):
    global pmixservermodule
    permitted = ['clientconnected', 'clientfinalized', 'abort',
                 'fencenb', 'directmodex', 'publish', 'lookup', 'unpublish',
                 'spawn', 'connect', 'disconnect', 'registerevents',
                 'deregisterevents', 'listener', 'notify_event', 'query',
                 'toolconnected', 'log', 'allocate', 'jobcontrol',
                 'monitor', 'getcredential', 'validatecredential',
                 'iofpull', 'pushstdin', 'group']
    if k not in permitted:
        raise KeyError
    if not k in pmixservermodule:
        pmixservermodule[k] = f

cdef class PMIxServer(PMIxClient):
    cdef pmix_server_module_t myserver;
    cdef pmix_fabric_t fabric;
    cdef int fabric_set;
    def __init__(self):
        self.fabric_set = 0
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
        # v4.x interfaces
        self.myserver.group = <pmix_server_grp_fn_t>group

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
    def init(self, keyvals:dict, map:dict):
        cdef pmix_info_t *info
        cdef size_t sz

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
        if keyvals is not None:
            # Convert any provided dictionary to an array of pmix_info_t
            kvkeys = list(keyvals.keys())
            sz = len(kvkeys)
            info = <pmix_info_t*> PyMem_Malloc(sz * sizeof(pmix_info_t))
            if not info:
                raise MemoryError()
            pmix_load_info(info, keyvals)
            rc = PMIx_server_init(&self.myserver, info, sz)
            pmix_free_info(info, sz)
        else:
            rc = PMIx_server_init(&self.myserver, NULL, 0)
        return rc

    def finalize(self):
        return PMIx_server_finalize()

    # Register a namespace
    #
    # @ns [INPUT]
    #     - Namespace of job (string)
    #
    # @nlocalprocs [INPUT]
    #              - number of local procs for this job (int)
    #
    # @keyvals [INPUT]
    #          - key-value pairs containing job-level info (dict)
    #
    def register_nspace(self, ns, nlocalprocs, keyvals:dict):
        cdef pmix_nspace_t nspace
        cdef pmix_info_t *info
        cdef size_t sz
        global active
        # convert the args into the necessary C-arguments
        pmix_copy_nspace(nspace, ns)
        active.clear()
        if keyvals is not None:
            # Convert any provided dictionary to an array of pmix_info_t
            kvkeys = list(keyvals.keys())
            sz = len(kvkeys)
            info = <pmix_info_t*> PyMem_Malloc(sz * sizeof(pmix_info_t))
            if not info:
                raise MemoryError()
            pmix_load_info(info, keyvals)
            rc = PMIx_server_register_nspace(nspace, nlocalprocs, info, sz, pmix_opcbfunc, NULL)
        else:
            rc = PMIx_server_register_nspace(nspace, nlocalprocs, NULL, 0, pmix_opcbfunc, NULL)
        if PMIX_SUCCESS == rc:
            active.wait()
            rc = active.get_status()
        active.clear()
        return rc

    # Deregister a namespace
    #
    # @ns [INPUT]
    #     - Namespace of job (string)
    #
    def deregister_nspace(self, ns):
        cdef pmix_nspace_t nspace
        global active
        # convert the args into the necessary C-arguments
        pmix_copy_nspace(nspace, ns)
        active.clear()
        PMIx_server_deregister_nspace(nspace, pmix_opcbfunc, NULL)
        active.wait()
        active.clear()
        return

    # Register a client process
    #
    # @proc [INPUT]
    #       - namespace and rank of the client (tuple)
    #
    # @uid [INPUT]
    #      - User ID (uid) of the client (int)
    #
    # @gid [INPUT]
    #      - Group ID (gid) of the client (int)
    #
    def register_client(self, proc:tuple, uid, gid):
        global active
        cdef pmix_proc_t p;
        pmix_copy_nspace(p.nspace, proc[0])
        p.rank = proc[1]
        active.clear()
        rc = PMIx_server_register_client(&p, uid, gid, NULL, pmix_opcbfunc, NULL)
        if PMIX_SUCCESS == rc:
            active.wait()
            rc = active.get_status()
        return rc

    # Deregister a client process
    #
    # @proc [INPUT]
    #       - namespace and rank of the client (tuple)
    #
    def deregister_client(self, proc:tuple):
        global active
        cdef pmix_proc_t p;
        pmix_copy_nspace(p.nspace, proc[0])
        p.rank = proc[1]
        active.clear()
        rc = PMIx_server_deregister_client(&p, pmix_opcbfunc, NULL)
        if PMIX_SUCCESS == rc:
            active.wait()
            rc = active.get_status()
        return rc

    # Setup the environment of a child process that is to be forked
    # by the host
    #
    # @proc [INPUT]
    #       - namespace,rank of client process (tuple)
    #
    # @envin [INPUT/OUTPUT]
    #        - environ of client proc that will be updated
    #          with PMIx envars (dict)
    #
    def setup_fork(self, proc:tuple, envin:dict):
        cdef pmix_proc_t p;
        cdef char **penv = NULL;
        cdef unicode pstring
        pmix_copy_nspace(p.nspace, proc[0])
        p.rank = proc[1]
        # convert the incoming dictionary to an array
        # of strings
        rc = PMIx_server_setup_fork(&p, &penv)
        if PMIX_SUCCESS == rc:
            # update the incoming dictionary
            n = 0
            while NULL != penv[n]:
                ln = strlen(penv[n])
                pstring = penv[n].decode('ascii')
                kv = pstring.split('=')
                envin[kv[0]] = kv[1]
                free(penv[n])
                n += 1
            free(penv)
        return rc

    def dmodex_request(self, proc, dataout:dict):
        global active
        cdef pmix_proc_t p;
        pmix_copy_nspace(p.nspace, proc[0])
        p.rank = proc[1]
        active.clear()
        rc = PMIx_server_dmodex_request(&p, dmodx_cbfunc, NULL);
        if PMIX_SUCCESS == rc:
            active.wait()
            # transfer the data to the dictionary
            (data, sz) = active.fetch_data()
            dataout["dmodx"] = (data, sz)
        return rc

    def setup_application(self, ns, keyvals:dict):
        global active
        cdef pmix_nspace_t nspace;
        cdef pmix_info_t *info
        cdef size_t sz
        dataout = []
        pmix_copy_nspace(nspace, ns)
        if keyvals is not None:
            # Convert any provided dictionary to an array of pmix_info_t
            kvkeys = list(keyvals.keys())
            sz = len(kvkeys)
            info = <pmix_info_t*> PyMem_Malloc(sz * sizeof(pmix_info_t))
            if not info:
                raise MemoryError()
            pmix_load_info(info, keyvals)
        else:
            info = NULL
            sz = 0
        active.clear()
        rc = PMIx_server_setup_application(nspace, info, sz, setupapp_cbfunc, NULL);
        if PMIX_SUCCESS == rc:
            active.wait()
            # transfer the data to the dictionary
            active.fetch_info(dataout)
        return (rc, dataout)

    def setup_local_support(self, ns, ilist:list):
        global active
        cdef pmix_nspace_t nspace;
        cdef pmix_info_t *info
        cdef size_t sz
        pmix_copy_nspace(nspace, ns)
        if ilist is not None:
            sz = len(ilist)
            info = <pmix_info_t*> PyMem_Malloc(sz * sizeof(pmix_info_t))
            if not info:
                raise MemoryError()
            n = 0
            for iptr in ilist:
                keys = list(iptr.keys())
                for key in keys:
                    pykey = str(key)
                    pmix_copy_key(info[n].key, pykey)
                    # the value also needs to be transferred
                    pmix_load_value(&info[n].value, iptr[key])
                    n += 1
                    break
        else:
            info = NULL
            sz = 0
        rc = PMIx_server_setup_local_support(nspace, info, sz, pmix_opcbfunc, NULL);
        if PMIX_SUCCESS == rc:
            active.wait()
        return rc

    def register_fabric(self, keyvals:dict):
        cdef pmix_info_t *info
        cdef size_t sz
        if 1 == self.fabric_set:
            return _PMIX_ERR_RESOURCE_BUSY
        if keyvals is not None:
            # Convert any provided dictionary to an array of pmix_info_t
            kvkeys = list(keyvals.keys())
            sz = len(kvkeys)
            info = <pmix_info_t*> PyMem_Malloc(sz * sizeof(pmix_info_t))
            if not info:
                raise MemoryError()
            pmix_load_info(info, keyvals)
            rc = PMIx_server_register_fabric(&self.fabric, info, sz)
            pmix_free_info(info, sz)
        else:
            rc = PMIx_server_register_fabric(&self.fabric, NULL, 0)
        if PMIX_SUCCESS == rc:
            self.fabric_set = 1
        return rc

    def deregister_fabric(self):
        if 0 == self.fabric_set:
            return PMIX_ERR_INIT
        rc = PMIx_server_deregister_fabric(&self.fabric)
        self.fabric_set = 0
        return rc;

    def get_num_vertices(self):
        cdef uint32_t nverts;
        rc = PMIx_server_get_num_vertices(&self.fabric, &nverts)
        if PMIX_SUCCESS == rc:
            return nverts
        else:
            return rc

    def get_comm_cost(self, src, dest):
        cdef uint16_t cost;
        rc = PMIx_server_get_comm_cost(&self.fabric, src, dest, &cost);
        if PMIX_SUCCESS == rc:
            return cost
        else:
            return rc

    def get_vertex_info(self, i):
        cdef pmix_value_t vertex;
        cdef char *nodename;
        rc = PMIx_server_get_vertex_info(&self.fabric, i, &vertex, &nodename)
        if PMIX_SUCCESS == rc:
            # convert the vertex to a tuple
            pyvertex = pmix_unload_value(&vertex)
            # convert the nodename to a Python string
            pyb = nodename
            pystr = pyb.decode("ascii")
            # return it as a tuple
            return (rc, pyvertex, pystr)
        else:
            return (rc, None, None)

    def get_index(self, pyvertex:tuple):
        cdef pmix_value_t vertex;
        cdef uint32_t i;
        cdef char *nodename;
        # convert the tuple to a pmix_value_t
        rc = pmix_load_value(&vertex, pyvertex)
        if PMIX_SUCCESS != rc:
            return (rc, -1, None)
        rc = PMIx_server_get_index(&self.fabric, &vertex, &i, &nodename)
        if PMIX_SUCCESS != rc:
            return (rc, -1, None)
        # convert the nodename to a Python string
        pyb = nodename
        pystr = pyb.decode("ascii")
        # return it as a tuple
        return (rc, i, pystr)

    def generate_regex(self, hosts):
        cdef char *regex;
        if isinstance(hosts, str):
            pyhosts = hosts.encode('ascii')
        else:
            pyhosts = hosts
        rc = PMIx_generate_regex(pyhosts, &regex)
        pyreg = regex
        pystr = pyreg.decode("ascii")
        return (rc, pystr)

    def generate_ppn(self, procs):
        cdef char *ppn;
        if isinstance(procs, str):
            pyprocs = procs.encode('ascii')
        else:
            pyprocs = procs
        rc = PMIx_generate_ppn(pyprocs, &ppn)
        pyppn = ppn
        pystr = pyppn.decode("ascii")
        return (rc, pystr)

cdef int clientconnected(pmix_proc_t *proc, void *server_object,
                         pmix_op_cbfunc_t cbfunc, void *cbdata) with gil:
    keys = pmixservermodule.keys()
    if 'clientconnected' in keys:
        if not proc:
            return PMIX_ERR_BAD_PARAM
        myproc = []
        pmix_unload_procs(proc, 1, myproc)
        rc = pmixservermodule['clientconnected'](myproc[0])
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
        if not proc:
            return PMIX_ERR_BAD_PARAM
        myproc = []
        pmix_unload_procs(proc, 1, myproc)
        rc = pmixservermodule['clientfinalized'](myproc[0])
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
        args = {}
        myproc = []
        myprocs = []
        keyvals = {}
        # convert the caller's name
        pmix_unload_procs(proc, 1, myproc)
        args['caller'] = myproc[0]
        # record the status
        args['status'] = status
        # record the msg, if given
        if NULL != msg:
            args['msg'] = str(msg)
        # convert any provided array of procs to be aborted
        if NULL != procs:
            pmix_unload_procs(procs, nprocs, myprocs)
            args['targets'] = myprocs
        # upcall it
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
        args = {}
        myprocs = []
        blist = []
        ilist = []
        pmix_unload_procs(procs, nprocs, myprocs)
        args['procs'] = myprocs
        if NULL != info:
            rc = pmix_unload_info(info, ninfo, ilist)
            if PMIX_SUCCESS != rc:
                return rc
            args['info'] = ilist
        if NULL != data:
            pmix_unload_bytes(data, ndata, blist)
            barray = bytearray(blist)
            args['data'] = barray
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

# TODO: This function requires that the server execute the
# provided callback function to return retrieved data, and
# it is not allowed to do so until _after_ it returns from
# this upcall. We'll need to figure out a way to 'save' the
# cbfunc until the server calls us back, possibly by passing
# an appropriate caddy object in 'cbdata'
cdef int directmodex(const pmix_proc_t *proc,
                     const pmix_info_t info[], size_t ninfo,
                     pmix_modex_cbfunc_t cbfunc, void *cbdata) with gil:
    keys = pmixservermodule.keys()
    if 'directmodex' in keys:
        args = {}
        myprocs = []
        ilist = []
        pmix_unload_procs(proc, 1, myprocs)
        args['proc'] = myprocs[0]
        if NULL != info:
            pmix_unload_info(info, ninfo, ilist)
            args['info'] = ilist
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
        args = {}
        myprocs = []
        ilist = []
        pmix_unload_procs(proc, 1, myprocs)
        args['proc'] = myprocs[0]
        if NULL != info:
            pmix_unload_info(info, ninfo, ilist)
            args['info'] = ilist
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

# TODO: This function requires that the server execute the
# provided callback function to return retrieved data, and
# it is not allowed to do so until _after_ it returns from
# this upcall. We'll need to figure out a way to 'save' the
# cbfunc until the server calls us back, possibly by passing
# an appropriate caddy object in 'cbdata'
cdef int lookup(const pmix_proc_t *proc, char **keys,
                const pmix_info_t info[], size_t ninfo,
                pmix_lookup_cbfunc_t cbfunc, void *cbdata) with gil:
    srvkeys = pmixservermodule.keys()
    if 'lookup' in srvkeys:
        args = {}
        myprocs = []
        ilist = []
        pykeys = []
        n = 0
        while NULL != keys[n]:
            pykeys.append(keys[n])
            n += 1
        args['keys'] = pykeys
        pmix_unload_procs(proc, 1, myprocs)
        args['proc'] = myprocs[0]
        if NULL != info:
            pmix_unload_info(info, ninfo, ilist)
            args['info'] = ilist
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
        args = {}
        myprocs = []
        ilist = []
        pykeys = []
        if NULL != keys:
            pmix_load_argv(keys, pykeys)
            args['keys'] = pykeys
        pmix_unload_procs(proc, 1, myprocs)
        args['proc'] = myprocs[0]
        if NULL != info:
            pmix_unload_info(info, ninfo, ilist)
            args['info'] = ilist
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

# TODO: This function requires that the server execute the
# provided callback function to return the spawned nspace, and
# it is not allowed to do so until _after_ it returns from
# this upcall. We'll need to figure out a way to 'save' the
# cbfunc until the server calls us back, possibly by passing
# an appropriate caddy object in 'cbdata'
cdef int spawn(const pmix_proc_t *proc,
               const pmix_info_t job_info[], size_t ninfo,
               const pmix_app_t apps[], size_t napps,
               pmix_spawn_cbfunc_t cbfunc, void *cbdata) with gil:
    keys = pmixservermodule.keys()
    if 'spawn' in keys:
        args = {}
        myprocs = []
        ilist = []
        pyapps = []
        pmix_unload_procs(proc, 1, myprocs)
        args['proc'] = myprocs[0]
        if NULL != job_info:
            pmix_unload_info(job_info, ninfo, ilist)
            args['jobinfo'] = ilist
        pmix_unload_apps(apps, napps, pyapps)
        args['apps'] = pyapps
        rc = pmixservermodule['spawn'](args)
    else:
        rc = PMIX_ERR_NOT_SUPPORTED
    return rc

cdef int connect(const pmix_proc_t procs[], size_t nprocs,
                 const pmix_info_t info[], size_t ninfo,
                 pmix_op_cbfunc_t cbfunc, void *cbdata) with gil:
    keys = pmixservermodule.keys()
    if 'connect' in keys:
        args = {}
        myprocs = []
        ilist = []
        if NULL != procs:
            pmix_unload_procs(procs, nprocs, myprocs)
            args['procs'] = myprocs
        if NULL != info:
            pmix_unload_info(info, ninfo, ilist)
            args['info'] = ilist
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
        args = {}
        myprocs = []
        ilist = []
        if NULL != procs:
            pmix_unload_procs(procs, nprocs, myprocs)
            args['procs'] = myprocs
        if NULL != info:
            pmix_unload_info(info, ninfo, ilist)
            args['info'] = ilist
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
        args = {}
        mycodes = []
        ilist = []
        if NULL != codes:
            n = 0
            while n < ncodes:
                mycodes.append(codes[n])
                n += 1
            args['codes'] = mycodes
        if NULL != info:
            pmix_unload_info(info, ninfo, ilist)
            args['info'] = ilist
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
        args = {}
        mycodes = []
        if NULL != codes:
            n = 0
            while n < ncodes:
                mycodes.append(codes[n])
                n += 1
            args['codes'] = mycodes
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
                     pmix_data_range_t drange,
                     pmix_info_t info[], size_t ninfo,
                     pmix_op_cbfunc_t cbfunc, void *cbdata) with gil:
    keys = pmixservermodule.keys()
    if 'notifyevent' in keys:
        args = {}
        ilist = []
        myproc = []
        args['code'] = code
        pmix_unload_procs(source, 1, myproc)
        args['source'] = myproc[0]
        args['range'] = drange
        if NULL != info:
            pmix_unload_info(info, ninfo, ilist)
            args['info'] = ilist
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

# TODO: This function requires that the server execute the
# provided callback function to return retrieved data, and
# it is not allowed to do so until _after_ it returns from
# this upcall. We'll need to figure out a way to 'save' the
# cbfunc until the server calls us back, possibly by passing
# an appropriate caddy object in 'cbdata'
cdef int query(pmix_proc_t *source,
               pmix_query_t *queries, size_t nqueries,
               pmix_info_cbfunc_t cbfunc,
               void *cbdata) with gil:
    keys = pmixservermodule.keys()
    if 'query' in keys:
        args = {}
        myproc = []
        if NULL != source:
            pmix_unload_procs(source, 1, myproc)
            args['source'] = myproc[0]
        rc = pmixservermodule['query'](args)
    else:
        rc = PMIX_ERR_NOT_SUPPORTED
    return rc

# TODO: This function requires that the server execute the
# provided callback function to return an assigned ID, and
# it is not allowed to do so until _after_ it returns from
# this upcall. We'll need to figure out a way to 'save' the
# cbfunc until the server calls us back, possibly by passing
# an appropriate caddy object in 'cbdata'
cdef void toolconnected(pmix_info_t *info, size_t ninfo,
                        pmix_tool_connection_cbfunc_t cbfunc,
                        void *cbdata) with gil:
    keys = pmixservermodule.keys()
    if 'toolconnected' in keys:
        args = {}
        ilist = {}
        if NULL != info:
            pmix_unload_info(info, ninfo, ilist)
            args['info'] = ilist
        pmixservermodule['toolconnected'](args)
    return

cdef void log(const pmix_proc_t *client,
              const pmix_info_t data[], size_t ndata,
              const pmix_info_t directives[], size_t ndirs,
              pmix_op_cbfunc_t cbfunc, void *cbdata) with gil:
    keys = pmixservermodule.keys()
    if 'log' in keys:
        args = {}
        ilist = []
        myproc = []
        mydirs = []
        pmix_unload_procs(client, 1, myproc)
        args['client'] = myproc[0]
        if NULL != data:
            pmix_unload_info(data, ndata, ilist)
            args['data'] = ilist
        if NULL != directives:
            pmix_unload_info(directives, ndirs, mydirs)
            args['directives'] = mydirs
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

# TODO: This function requires that the server execute the
# provided callback function to return the allocation, and
# it is not allowed to do so until _after_ it returns from
# this upcall. We'll need to figure out a way to 'save' the
# cbfunc until the server calls us back, possibly by passing
# an appropriate caddy object in 'cbdata'
cdef int allocate(const pmix_proc_t *client,
                  pmix_alloc_directive_t directive,
                  const pmix_info_t data[], size_t ndata,
                  pmix_info_cbfunc_t cbfunc, void *cbdata) with gil:
    keys = pmixservermodule.keys()
    if 'allocate' in keys:
        args = {}
        myproc = []
        mydirs = {}
        keyvals = {}
        if NULL != client:
            pmix_unload_procs(client, 1, myproc)
            args['client'] = myproc[0]
        args['directive'] = directive
        if NULL != data:
            pmix_unload_info(data, ndata, keyvals)
            args['data'] = keyvals
        rc = pmixservermodule['allocate'](args)
    else:
        rc = PMIX_ERR_NOT_SUPPORTED
    return rc

# TODO: This function requires that the server execute the
# provided callback function to return the outcome of the op, and
# it is not allowed to do so until _after_ it returns from
# this upcall. We'll need to figure out a way to 'save' the
# cbfunc until the server calls us back, possibly by passing
# an appropriate caddy object in 'cbdata'
cdef int jobcontrol(const pmix_proc_t *requestor,
                    const pmix_proc_t targets[], size_t ntargets,
                    const pmix_info_t directives[], size_t ndirs,
                    pmix_info_cbfunc_t cbfunc, void *cbdata) with gil:
    keys = pmixservermodule.keys()
    if 'jobcontrol' in keys:
        args = {}
        myproc = []
        mytargets = []
        mydirs = {}
        if NULL != requestor:
            pmix_unload_procs(requestor, 1, myproc)
            args['requestor'] = myproc[0]
        if NULL != targets:
            pmix_unload_procs(targets, ntargets, mytargets)
            args['targets'] = mytargets
        if NULL != directives:
            pmix_unload_info(directives, ndirs, mydirs)
            args['directives'] = mydirs
        rc = pmixservermodule['jobcontrol'](args)
    else:
        rc = PMIX_ERR_NOT_SUPPORTED
    return rc

# TODO: This function requires that the server execute the
# provided callback function to return the monitoring response, and
# it is not allowed to do so until _after_ it returns from
# this upcall. We'll need to figure out a way to 'save' the
# cbfunc until the server calls us back, possibly by passing
# an appropriate caddy object in 'cbdata'
cdef int monitor(const pmix_proc_t *requestor,
                 const pmix_info_t *monitor, pmix_status_t error,
                 const pmix_info_t directives[], size_t ndirs,
                 pmix_info_cbfunc_t cbfunc, void *cbdata) with gil:
    keys = pmixservermodule.keys()
    if 'monitor' in keys:
        args = {}
        mymon = {}
        myproc = []
        mydirs = {}
        blist = []
        if NULL != requestor:
            pmix_unload_procs(requestor, 1, myproc)
            args['requestor'] = myproc[0]
        if NULL != monitor:
            pmix_unload_info(monitor, 1, mymon)
            args['monitor'] = mymon
        args['error'] = error
        if NULL != directives:
            pmix_unload_info(directives, ndirs, mydirs)
            args['directives'] = mydirs
        rc = pmixservermodule['monitor'](args)
    else:
        rc = PMIX_ERR_NOT_SUPPORTED
    return rc

# TODO: This function requires that the server execute the
# provided callback function to return the credential, and
# it is not allowed to do so until _after_ it returns from
# this upcall. We'll need to figure out a way to 'save' the
# cbfunc until the server calls us back, possibly by passing
# an appropriate caddy object in 'cbdata'
cdef int getcredential(const pmix_proc_t *proc,
                       const pmix_info_t directives[], size_t ndirs,
                       pmix_credential_cbfunc_t cbfunc, void *cbdata) with gil:
    keys = pmixservermodule.keys()
    if 'getcredential' in keys:
        args = {}
        myproc = []
        mydirs = {}
        if NULL != proc:
            pmix_unload_procs(proc, 1, myproc)
            args['proc'] = myproc[0]
        if NULL != directives:
            pmix_unload_info(directives, ndirs, mydirs)
            args['directives'] = mydirs
        rc = pmixservermodule['getcredential'](args)
    else:
        rc = PMIX_ERR_NOT_SUPPORTED
    return rc

# TODO: This function requires that the server execute the
# provided callback function to return the validation, and
# it is not allowed to do so until _after_ it returns from
# this upcall. We'll need to figure out a way to 'save' the
# cbfunc until the server calls us back, possibly by passing
# an appropriate caddy object in 'cbdata'
cdef int validatecredential(const pmix_proc_t *proc,
                            const pmix_byte_object_t *cred,
                            const pmix_info_t directives[], size_t ndirs,
                            pmix_validation_cbfunc_t cbfunc, void *cbdata) with gil:
    keys = pmixservermodule.keys()
    if 'validatecredential' in keys:
        args = {}
        keyvals = {}
        myproc = []
        mydirs = {}
        blist = []
        if NULL != proc:
            pmix_unload_procs(proc, 1, myproc)
            args['proc'] = myproc[0]
        if NULL != cred:
            pmix_unload_bytes(cred[0].bytes, cred[0].size, blist)
            args['cred'] = blist
        if NULL != directives:
            pmix_unload_info(directives, ndirs, mydirs)
            args['directives'] = mydirs
        rc = pmixservermodule['validatecredential'](args)
    else:
        rc = PMIX_ERR_NOT_SUPPORTED
    return rc

cdef int iofpull(const pmix_proc_t procs[], size_t nprocs,
                 const pmix_info_t directives[], size_t ndirs,
                 pmix_iof_channel_t channels,
                 pmix_op_cbfunc_t cbfunc, void *cbdata) with gil:
    keys = pmixservermodule.keys()
    if 'iofpull' in keys:
        args = {}
        keyvals = {}
        myprocs = []
        mydirs = {}
        if NULL != procs:
            pmix_unload_procs(procs, nprocs, myprocs)
            args['procs'] = myprocs
        if NULL != directives:
            pmix_unload_info(directives, ndirs, mydirs)
            args['directives'] = mydirs
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
        args = {}
        keyvals = {}
        myproc = []
        mytargets = []
        mydirs = {}
        if NULL != source:
            pmix_unload_procs(source, 1, myproc)
            args['source'] = myproc[0]
        if NULL != targets:
            pmix_unload_procs(targets, ntargets, mytargets)
            args['targets'] = mytargets
        if NULL != directives:
            pmix_unload_info(directives, ndirs, mydirs)
            args['directives'] = mydirs
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

# TODO: This function requires that the server execute the
# provided callback function to return the group info, and
# it is not allowed to do so until _after_ it returns from
# this upcall. We'll need to figure out a way to 'save' the
# cbfunc until the server calls us back, possibly by passing
# an appropriate caddy object in 'cbdata'
cdef int group(pmix_group_operation_t op, char grp[],
               const pmix_proc_t procs[], size_t nprocs,
               const pmix_info_t directives[], size_t ndirs,
               pmix_info_cbfunc_t cbfunc, void *cbdata) with gil:
    keys = pmixservermodule.keys()
    if 'group' in keys:
        args = {}
        keyvals = {}
        myprocs = []
        mydirs = {}
        args['op'] = op
        args['grp'] = str(grp)
        pmix_unload_procs(procs, nprocs, myprocs)
        args['procs'] = myprocs
        if NULL != directives:
            pmix_unload_info(directives, ndirs, mydirs)
            args['directives'] = mydirs
        rc = pmixservermodule['group'](args)
    else:
        rc = PMIX_ERR_NOT_SUPPORTED
    return rc
