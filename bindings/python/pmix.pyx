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
    permitted = ['clientconnected']
    if k not in permitted:
        raise KeyError
    if not k in pmixservermodule:
        print("SETTING MODULE FN FOR ", k)
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
        cdef size_t ninfo = 0
        # Convert any provided dictionary to an array of pmix_info_t
        if keyvals is not None and 0 < len(keyvals):
            ninfo = len(keyvals)
            info = <pmix_info_t *>malloc(ninfo * sizeof(pmix_info_t))
            if not info:
                raise MemoryError()
         #   pyinfoarray_to_pmix(keyvals, info, ninfo)
            for n in range(ninfo):
                print("INFO[" + n + "] " + info[n].key + "\n")
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
        return PMIx_server_init(&self.myserver, NULL, 0)

    def finalize(self):
        return PMIx_server_finalize()

    def register_nspace(self, args):
        # convert the args into the necessary C-arguments
        pass

cdef void client_connected(pmix_proc_t *proc, void *server_object,
                           pmix_op_cbfunc_t cbfunc, void *cbdata) with gil:
    keys = pmixservermodule.keys()
    if 'clientconnected' in keys:
    #    args = pmixproc_to_py(proc)
        args = {}
        rc = pmixservermodule['clientconnected'](args)
    else:
        rc = PMIX_ERR_NOT_SUPPORTED
    cbfunc(rc, cbdata)
    return

