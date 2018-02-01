#file: pmix.pyx

from cpmix cimport *
from libc.string cimport memset,strncpy
from libc.stdlib cimport malloc, free

cdef class PMIxProc:
    cdef pmix_proc_t proc
    def __init__(self, n, r):
        strncpy(self.proc.nspace, n, PMIX_MAX_NSLEN)
        self.proc.rank = r

    # define properties in the normal Python way
    @property
    def nspace(self):
        return self.proc.nspace

    @nspace.setter
    def nspace(self,n):
        strncpy(self.proc.nspace, n, PMIX_MAX_NSLEN)

    @property
    def rank(self):
        return self.proc.rank

    @rank.setter
    def rank(self,r):
        self.proc.rank = r

cdef class pyOpCbfunc:
    cdef pmix_op_cbfunc_t cbfunc
    cdef void* cbdata
    def __cinit__(self):
        self.cbfunc = NULL
        self.cbdata = NULL

    cdef setFn(self, pmix_op_cbfunc_t fn):
        self.cbfunc = fn
        return

    cdef setCbd(self, void *cbd):
        self.cbdata = cbd
        return

def convert_value(val):
    pass

cdef class PMIxClient:
    cdef pmix_proc_t myproc;
    def __init__(self):
        memset(self.myproc.nspace, 0, sizeof(self.myproc.nspace))
        self.myproc.rank = PMIX_RANK_UNDEF

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
        cdef pmix_info_t info
        cdef size_t ninfo
        kvkeys = list(keyvals.keys())
        for key in kvkeys:
            strncpy(info.key, str.encode(key), PMIX_MAX_KEYLEN)
    #        self.__convert_value(keyvals[key])
            print "INFO ", info.key
        ninfo = 1
        return PMIx_Init(&self.myproc, &info, ninfo)
    # private function to convert key-value tuple
    # to pmix_info_t
    def __convert_value(self, inval):
        pass

pmixservermodule = {}
def setmodulefn(k, f):
    global pmixservermodule
    permitted = ['clientconnected']
    if k not in permitted:
        raise KeyError
    if not k in pmixservermodule:
        print "SETTING MODULE FN FOR ", k
        pmixservermodule[k] = f

cdef class PMIxServer:
    cdef pmix_proc_t myproc;
    cdef pmix_server_module_t myserver;
    def __init__(self):
        memset(self.myproc.nspace, 0, sizeof(self.myproc.nspace))
        self.myproc.rank = PMIX_RANK_UNDEF
        self.myserver.client_connected = <pmix_server_client_connected_fn_t>client_connected

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
        cdef pmix_info_t *info = NULL
        if keyvals is not None and 0 < len(keyvals):
            info = <pmix_info_t *>malloc(len(keyvals) * sizeof(pmix_info_t))
            if not info:
                raise MemoryError()
            kvkeys = list(keyvals.keys())
            for key in kvkeys:
                strncpy(info.key, str.encode(key), PMIX_MAX_KEYLEN)
        #        self.__convert_value(keyvals[key])
                print "INFO ", info.key
        if map is None or 0 == len(map):
            print "SERVER REQUIRES AT LEAST ONE MODULE FUNCTION TO OPERATE"
            return PMIX_ERR_INIT
        kvkeys = list(map.keys())
        for key in kvkeys:
            try:
                setmodulefn(key, map[key])
            except KeyError:
                print "SERVER MODULE FUNCTION ", key, " IS NOT RECOGNIZED"
                return PMIX_ERR_INIT
        return PMIx_server_init(&self.myserver, NULL, 0)

    def finalize(self):
        return PMIx_server_finalize()

    def register_nspace(self, args):
        # convert the args into the necessary C-arguments
        pass

    # private function to convert key-value tuple
    # to pmix_info_t
    def __convert_value(self, inval):
        pass

cdef void client_connected(pmix_proc_t *proc, void *server_object,
                           pmix_op_cbfunc_t cbfunc, void *cbdata):
    print "CLIENT CONNECTED", proc.nspace, proc.rank
    keys = pmixservermodule.keys()
    if 'clientconnected' in keys:
        p = pyOpCbfunc()
        p.setFn(cbfunc)
        p.setCbd(cbdata)
        pname = PMIxProc(proc.nspace, proc.rank)
        args = {}
        args['proc'] = pname
        args['callback'] = p
        pmixservermodule['clientconnected'](args)
    return

