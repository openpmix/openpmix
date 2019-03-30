from libc.string cimport memset, strncpy, strcpy, strlen, strdup
from libc.stdlib cimport malloc, realloc, free
from libc.string cimport memcpy
from cpython.mem cimport PyMem_Malloc, PyMem_Realloc, PyMem_Free

# pull in all the constant definitions - we
# store them in a separate file for neatness
include "pmix_constants.pxi"

# provide a lock class for catching information
# returned in callback functions
class myLock(threading.Event):
    def __init__(self):
        threading.Event.__init__(self)
        self.event = threading.Event()
        self.status = PMIX_ERR_NOT_SUPPORTED
        self.sz = 0

    def set(self, status):
        self.status = status
        self.event.set()

    def clear(self):
        self.event.clear()

    def wait(self):
        self.event.wait()

    def get_status(self):
        return self.status

    def cache_data(self, data, sz):
        self.data = array.array('B', data[0])
        # need to copy the data bytes as the
        # PMIx server will free it upon return
        n = 1
        while n < sz:
            self.data.append(data[n])
            n += 1
        self.sz = sz

    def fetch_data(self):
        return (self.data, self.sz)

# provide conversion programs that translate incoming
# PMIx structures into Python dictionaries, and incoming
# arrays into Python lists of objects

def pmix_bool_convert(f):
    if isinstance(f, str):
        if f.startswith('t'):
            return 1
        elif f.startswith('f'):
            return 0
        else:
            raise ValueError("Incorrect boolean value provided")
    else:
        return f

pmix_int_types = (int, long)

# provide a safe way to copy a Python nspace into
# the pmix_nspace_t structure that guarantees the
# array is NULL-terminated
cdef void pmix_copy_nspace(pmix_nspace_t nspace, ns):
    nslen = len(ns)
    if PMIX_MAX_NSLEN < nslen:
        nslen = PMIX_MAX_NSLEN
    if isinstance(ns, str):
        pyns = ns.encode('ascii')
    else:
        pyns = ns
    pynsptr = <const char *>(pyns)
    memset(nspace, 0, PMIX_MAX_NSLEN+1)
    memcpy(nspace, pynsptr, nslen)

# provide a safe way to copy a Python key into
# the pmix_key_t structure that guarantees the
# array is NULL-terminated
cdef void pmix_copy_key(pmix_key_t key, ky):
    klen = len(ky)
    if PMIX_MAX_KEYLEN < klen:
        klen = PMIX_MAX_KEYLEN
    if isinstance(ky, str):
        pykey = ky.encode('ascii')
    else:
        pykey = ky
    pykeyptr = <const char *>(pykey)
    memset(key, 0, PMIX_MAX_KEYLEN+1)
    memcpy(key, pykeyptr, klen)
