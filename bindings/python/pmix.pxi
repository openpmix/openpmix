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

cdef int pmix_load_value(pmix_value_t *value, val:tuple):
    if val[1] == 'bool':
        value[0].type = PMIX_BOOL
        value[0].data.flag = pmix_bool_convert(val[0])
    elif val[1] == 'byte':
        value[0].type = PMIX_BYTE
        value[0].data.byte = val[0]
    elif val[1] == 'string':
        value[0].type = PMIX_STRING
        if isinstance(val[0], str):
            pykey = val[0].encode('ascii')
        else:
            pykey = val[0]
        try:
            value[0].data.string = strdup(pykey)
        except:
            raise ValueError("String value declared but non-string provided")
    elif val[1] == 'size':
        value[0].type = PMIX_SIZE
        if not isinstance(val[0], pmix_int_types):
            raise TypeError("size_t value declared but non-integer provided")
        value[0].data.size = val[0]
    elif val[1] == 'pid':
        value[0].type = PMIX_PID
        if not isinstance(val[0], pmix_int_types):
            raise TypeError("pid value declared but non-integer provided")
        if val[0] < 0:
            raise ValueError("pid value is negative")
        value[0].data.pid = val[0]
    elif val[1] == 'int':
        value[0].type = PMIX_INT
        if not isinstance(val[0], pmix_int_types):
            raise TypeError("integer value declared but non-integer provided")
        value[0].data.integer = val[0]
    elif val[1] == 'int8':
        value[0].type = PMIX_INT8
        if not isinstance(val[0], pmix_int_types):
            raise TypeError("int8 value declared but non-integer provided")
        if val[0] > 127 or val[0] < -128:
            raise ValueError("int8 value is out of bounds")
        value[0].data.int8 = val[0]
    elif val[1] == 'int16':
        value[0].type = PMIX_INT16
        if not isinstance(val[0], pmix_int_types):
            raise TypeError("int16 value declared but non-integer provided")
        if val[0] > 32767 or val[0] < -32768:
            raise ValueError("int16 value is out of bounds")
        value[0].data.int16 = val[0]
    elif val[1] == 'int32':
        value[0].type = PMIX_INT32
        if not isinstance(val[0], pmix_int_types):
            raise TypeError("int32 value declared but non-integer provided")
        if val[0] > 2147483647 or val[0] < -2147483648:
            raise ValueError("int32 value is out of bounds")
        value[0].data.int32 = val[0]
    elif val[1] == 'int64':
        value[0].type = PMIX_INT64
        if not isinstance(val[0], pmix_int_types):
            raise TypeError("int64 value declared but non-integer provided")
        if val[0] > (2147483647*2147483647) or val[0] < -(2147483648*2147483648):
            raise ValueError("int64 value is out of bounds")
        value[0].data.int64 = val[0]
    elif val[1] == 'uint':
        value[0].type = PMIX_UINT
        if not isinstance(val[0], pmix_int_types):
            raise TypeError("integer value declared but non-integer provided")
        value[0].data.uint = val[0]
    elif val[1] == 'uint8':
        value[0].type = PMIX_UINT8
        if not isinstance(val[0], pmix_int_types):
            raise TypeError("uint8 value declared but non-integer provided")
        if val[0] > 255:
            raise ValueError("uint8 value is out of bounds")
        value[0].data.uint8 = val[0]
    elif val[1] == 'uint16':
        value[0].type = PMIX_UINT16
        if not isinstance(val[0], pmix_int_types):
            raise TypeError("uint16 value declared but non-integer provided")
        if val[0] > 65536:
            raise ValueError("uint16 value is out of bounds")
        value[0].data.uint16 = val[0]
    elif val[1] == 'uint32':
        value[0].type = PMIX_UINT32
        if not isinstance(val[0], pmix_int_types):
            raise TypeError("uint32 value declared but non-integer provided")
        if val[0] > (65536*65536):
            raise ValueError("uint32 value is out of bounds")
        value[0].data.uint32 = val[0]
    elif val[1] == 'uint64':
        value[0].type = PMIX_UINT64
        if not isinstance(val[0], pmix_int_types):
            raise TypeError("int64 value declared but non-integer provided")
        if val[0] > (2147483648*2147483648):
            raise ValueError("uint64 value is out of bounds")
        value[0].data.uint64 = val[0]
    else:
        print("UNRECOGNIZED INFO VALUE")
        return PMIX_ERR_NOT_SUPPORTED
    return PMIX_SUCCESS

cdef tuple pmix_unload_value(pmix_value_t *value):
    if value[0].type == PMIX_BOOL:
        if value[0].data.flag:
            return (True, 'bool')
        else:
            return (False, 'bool')
    elif value[0].type == PMIX_BYTE:
        return (value[0].data.byte, 'byte')
    elif value[0].type == PMIX_STRING:
        pyb = value[0].data.string
        pystr = pyb.decode("ascii")
        return (pystr, 'string')
    elif value[0].type == PMIX_SIZE:
        return (value[0].data.size, 'size')
    elif value[0].type == PMIX_PID:
        return (value[0].data.pid, 'pid')
    elif value[0].type == PMIX_INT:
        return (value[0].data.integer, 'int')
    elif value[0].type == PMIX_INT8:
        return (value[0].data.int8, 'int8')
    elif value[0].type == PMIX_INT16:
        return (value[0].data.int16, 'int16')
    elif value[0].type == PMIX_INT32:
        return (value[0].data.int32, 'int32')
    elif value[0].type == PMIX_INT64:
        return (value[0].data.int64, 'int64')
    elif value[0].type == PMIX_UINT:
        return (value[0].data.uint, 'uint')
    elif value[0].type == PMIX_UINT8:
        return (value[0].data.uint8, 'uint8')
    elif value[0].type == PMIX_UINT16:
        return (value[0].data.uint16, 'uint16')
    elif value[0].type == PMIX_UINT32:
        return (value[0].data.uint32, 'uint32')
    elif value[0].type == PMIX_UINT64:
        return (value[0].data.uint64, 'uint64')
    else:
        return (PMIX_ERR_NOT_SUPPORTED, 'error')


cdef void pmix_destruct_value(pmix_value_t *value):
    if value[0].type == PMIX_STRING:
        free(value[0].data.string);

cdef void pmix_free_value(self, pmix_value_t *value):
    pmix_destruct_value(value);
    PyMem_Free(value)


# Convert a dictionary of key-value pairs into an
# array of pmix_info_t structs
#
# @array [INPUT]
#        - malloc'd array of pmix_info_t structs
#
# @keyvals [INPUT]
#          - dictionary of key-value pairs {key: (value, type)}
#
cdef int pmix_load_info(pmix_info_t *array, keyvals:dict):
    kvkeys = list(keyvals.keys())
    n = 0
    for key in kvkeys:
        pmix_copy_key(array[n].key, key)
        # the value also needs to be transferred
        pmix_load_value(&array[n].value, keyvals[key])
        print("ARRAY[", n, "]: ", array[n].key, array[n].value.type)
        n += 1
    return PMIX_SUCCESS

cdef int pmix_unload_info(pmix_info_t *info, keyval:dict):
    cdef char* kystr
    val = pmix_unload_value(&info[0].value)
    if val[1] == 'error':
        return PMIX_ERR_NOT_SUPPORTED
    kystr = strdup(info[0].key)
    pykey = kystr.decode("ascii")
    free(kystr)
    keyval[pykey] = val
    return PMIX_SUCCESS

cdef void pmix_destruct_info(pmix_info_t *info):
    pmix_destruct_value(&info[0].value)

# Free a malloc'd array of pmix_info_t structures
#
# @array [INPUT]
#        - array of pmix_info_t to be free'd
#
# @sz [INPUT]
#     - number of elements in array
cdef void pmix_free_info(pmix_info_t *array, size_t sz):
    n = 0
    while n < sz:
        pmix_destruct_info(&array[n])
        n += 1
    PyMem_Free(array)

# Convert a list of (nspace, rank) tuples into an
# array of pmix_proc_t structs
#
# @proc [INPUT]
#       - malloc'd array of pmix_proc_t structs
#
# @peers [INPUT]
#       - list of (nspace,rank) tuples
#
cdef int pmix_load_proc(pmix_proc_t *proc, peers:list):
    n = 0
    for p in peers:
        pmix_copy_nspace(proc[n].nspace, p[0])
        proc[n].rank = p[1]
        n += 1
    return PMIX_SUCCESS

# Free a malloc'd array of pmix_proc_t structures
#
# @array [INPUT]
#        - array of pmix_proc_t to be free'd
#
# @sz [INPUT]
#     - number of elements in array
#
cdef void pmix_free_procs(pmix_proc_t *array, size_t sz):
    PyMem_Free(array)

