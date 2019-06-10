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
        self.info = []

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

    def cache_info(self, info:list):
        # need to copy the info array as the
        # PMIx server will free it upon execing
        # the callback function
        self.info = []
        for x in info:
            self.info.append(x)

    def fetch_info(self, info:list):
        for x in self.info:
            info.append(x)

cdef void pmix_load_argv(char **keys, argv:list):
    n = 0
    while NULL != keys[n]:
        mykey = str(keys[n])
        argv.append(mykey)
        n += 1

# TODO: implement support for PMIX_BOOL and PMIX_BYTE
cdef int pmix_load_darray(pmix_data_array_t *array, mytype, mylist:list):
    cdef pmix_info_t *infoptr;
    mysize = len(mylist)
    n = 0
    if PMIX_INFO == mytype:
        array[0].array = PyMem_Malloc(mysize * sizeof(pmix_info_t))
        if not array[0].array:
            raise MemoryError()
        n = 0
        infoptr = <pmix_info_t*>array[0].array
        for item in mylist:
            keylist = item.keys()  # will be only one
            for key in keylist:
                pykey = str(key)
                pmix_copy_key(infoptr[n].key, pykey)
                pmix_load_value(&infoptr[n].value, item[key])
                break
            n += 1
    elif PMIX_STRING == mytype:
        array[0].array = PyMem_Malloc(mysize * sizeof(char*))
        if not array[0].array:
            raise MemoryError()
        n = 0
        strptr = <char**> array[0].array
        for item in mylist:
            strptr[n] = strdup(item)
            n += 1
    elif PMIX_SIZE == mytype:
        array[0].array = PyMem_Malloc(mysize * sizeof(size_t))
        if not array[0].array:
            raise MemoryError()
        n = 0
        sptr = <size_t*> array[0].array
        for item in mylist:
            if not isinstance(item, pmix_int_types):
                raise TypeError("size_t value declared but non-integer provided")
            sptr[n] = int(item)
            n += 1
    elif PMIX_PID == mytype:
        array[0].array = PyMem_Malloc(mysize * sizeof(pid_t))
        if not array[0].array:
            raise MemoryError()
        n = 0
        pidptr = <pid_t*> array[0].array
        for item in mylist:
            if not isinstance(item, pmix_int_types):
                raise TypeError("pid_t value declared but non-integer provided")
            pidptr[n] = int(item)
            n += 1
    elif PMIX_INT == mytype or PMIX_UINT == mytype:
        array[0].array = PyMem_Malloc(mysize * sizeof(int))
        if not array[0].array:
            raise MemoryError()
        n = 0
        iptr = <int*> array[0].array
        for item in mylist:
            if not isinstance(item, pmix_int_types):
                raise TypeError("int value declared but non-integer provided")
            iptr[n] = int(item)
            n += 1
    elif PMIX_INT8 == mytype or PMIX_UINT8 == mytype:
        array[0].array = PyMem_Malloc(mysize * sizeof(int8_t))
        if not array[0].array:
            raise MemoryError()
        n = 0
        i8ptr = <int8_t*> array[0].array
        for item in mylist:
            if not isinstance(item, pmix_int_types):
                raise TypeError("8-bit int value declared but non-integer provided")
            i8ptr[n] = int(item)
            n += 1
    elif PMIX_INT16 == mytype or PMIX_UINT16 == mytype:
        array[0].array = PyMem_Malloc(mysize * sizeof(int16_t))
        if not array[0].array:
            raise MemoryError()
        n = 0
        i16ptr = <int16_t*> array[0].array
        for item in mylist:
            if not isinstance(item, pmix_int_types):
                raise TypeError("16-bit int value declared but non-integer provided")
            i16ptr[n] = int(item)
            n += 1
    elif PMIX_INT32 == mytype or PMIX_UINT32 == mytype:
        array[0].array = PyMem_Malloc(mysize * sizeof(int32_t))
        if not array[0].array:
            raise MemoryError()
        n = 0
        i32ptr = <int32_t*> array[0].array
        for item in mylist:
            if not isinstance(item, pmix_int_types):
                raise TypeError("32-bit int value declared but non-integer provided")
            i32ptr[n] = int(item)
            n += 1
    elif PMIX_INT64 == mytype or PMIX_UINT64 == mytype:
        array[0].array = PyMem_Malloc(mysize * sizeof(int64_t))
        if not array[0].array:
            raise MemoryError()
        n = 0
        i64ptr = <int64_t*> array[0].array
        for item in mylist:
            if not isinstance(item, pmix_int_types):
                raise TypeError("64-bit int value declared but non-integer provided")
            i64ptr[n] = int(item)
            n += 1
    elif PMIX_FLOAT == mytype:
        array[0].array = PyMem_Malloc(mysize * sizeof(float))
        if not array[0].array:
            raise MemoryError()
        n = 0
        fptr = <float*> array[0].array
        for item in mylist:
            fptr[n] = float(item)
            n += 1
    elif PMIX_DOUBLE == mytype:
        array[0].array = PyMem_Malloc(mysize * sizeof(double))
        if not array[0].array:
            raise MemoryError()
        n = 0
        dptr = <double*> array[0].array
        for item in mylist:
            dptr[n] = float(item)
            n += 1
            n += 1
    elif PMIX_TIMEVAL == mytype:
        # TODO: Not clear that "timeval" has the same size as
        # "struct timeval"
        array[0].array = PyMem_Malloc(mysize * sizeof(timeval))
        if not array[0].array:
            raise MemoryError()
        n = 0
        tvptr = <timeval*> array[0].array
        for item in mylist:
            if isinstance(item, tuple):
                tvptr[n].tv_sec = item[0]
                tvptr[n].tv_usec = item[1]
            else:
                tvptr[n].tv_sec = item
                tvptr[n].tv_usec = 0
            n += 1
    elif PMIX_TIME == mytype:
        array[0].array = PyMem_Malloc(mysize * sizeof(time_t))
        if not array[0].array:
            raise MemoryError()
        n = 0
        tmptr = <time_t*> array[0].array
        for item in mylist:
            tmptr[n] = item
            n += 1
    elif PMIX_STATUS == mytype:
        array[0].array = PyMem_Malloc(mysize * sizeof(int))
        if not array[0].array:
            raise MemoryError()
        n = 0
        stptr = <int*> array[0].array
        for item in mylist:
            stptr[n] = item
            n += 1
    elif PMIX_PROC_RANK == mytype:
        array[0].array = PyMem_Malloc(mysize * sizeof(pmix_rank_t))
        if not array[0].array:
            raise MemoryError()
        n = 0
        rkptr = <pmix_rank_t*> array[0].array
        for item in mylist:
            rkptr[n] = item
            n += 1
    elif PMIX_PROC == mytype:
        array[0].array = PyMem_Malloc(mysize * sizeof(pmix_proc_t))
        if not array[0].array:
            raise MemoryError()
        n = 0
        prcptr = <pmix_proc_t*> array[0].array
        for item in mylist:
            pmix_copy_nspace(prcptr[n].nspace, item[0])
            prcptr[n].rank = item[1]
            n += 1
    elif PMIX_BYTE_OBJECT == mytype:
        array[0].array = PyMem_Malloc(mysize * sizeof(pmix_byte_object_t))
        if not array[0].array:
            raise MemoryError()
        n = 0
        boptr = <pmix_byte_object_t*>array[0].array
        for item in mylist:
            boptr[n].size = item[0]
            boptr[n].bytes = <char*> PyMem_Malloc(item[0].data.bo.size)
            if not boptr[n].bytes:
                raise MemoryError()
            pyarr = bytes(item[1])
            pyptr = <const char*> pyarr
            memcpy(boptr[n].bytes, pyptr, boptr[n].size)
            n += 1
    elif PMIX_PERSISTENCE == mytype:
        array[0].array = PyMem_Malloc(mysize * sizeof(pmix_persistence_t))
        if not array[0].array:
            raise MemoryError()
        n = 0
        perptr = <pmix_persistence_t*> array[0].array
        for item in mylist:
            perptr[n] = item
            n += 1
    elif PMIX_SCOPE == mytype:
        array[0].array = PyMem_Malloc(mysize * sizeof(pmix_scope_t))
        if not array[0].array:
            raise MemoryError()
        n = 0
        scptr = <pmix_scope_t*> array[0].array
        for item in mylist:
            scptr[n] = item
            n += 1
    elif PMIX_RANGE == mytype:
        array[0].array = PyMem_Malloc(mysize * sizeof(pmix_data_range_t))
        if not array[0].array:
            raise MemoryError()
        n = 0
        rgptr = <pmix_data_range_t*> array[0].array
        for item in mylist:
            rgptr[n] = item
            n += 1
    elif PMIX_PROC_STATE == mytype:
        array[0].array = PyMem_Malloc(mysize * sizeof(pmix_proc_state_t))
        if not array[0].array:
            raise MemoryError()
        n = 0
        psptr = <pmix_proc_state_t*> array[0].array
        for item in mylist:
            psptr[n] = item
            n += 1
    elif PMIX_PROC_INFO == mytype:
        array[0].array = PyMem_Malloc(mysize * sizeof(pmix_proc_info_t))
        if not array[0].array:
            raise MemoryError()
        n = 0
        piptr = <pmix_proc_info_t*> array[0].array
        for item in mylist:
            pmix_copy_nspace(piptr[n].proc.nspace, item['proc'][0])
            piptr[n].proc.rank = item['proc'][1]
            piptr[n].hostname = strdup(item['hostname'])
            piptr[n].pid = item['pid']
            piptr[n].exit_code = item['exitcode']
            piptr[n].state = item['state']
            n += 1
    elif PMIX_DATA_ARRAY == mytype:
        array[0].array = <pmix_data_array_t*> PyMem_Malloc(sizeof(pmix_data_array_t))
        if not array[0].array:
            raise MemoryError()
        daptr = <pmix_data_array_t*>array[0].array
        n = 0
        for item in mylist:
            daptr[n].type = item[0]
            daptr[n].size = len(item[1])
            daptr[n].array = <pmix_data_array_t*> PyMem_Malloc(sizeof(pmix_data_array_t))
            if not daptr[n].array:
                raise MemoryError()
            mydaptr = <pmix_data_array_t*>daptr[n].array
            try:
                return pmix_load_darray(mydaptr, daptr[n].type, item[1])
            except:
                return PMIX_ERR_NOT_SUPPORTED
    elif PMIX_ALLOC_DIRECTIVE == mytype:
        array[0].array = PyMem_Malloc(mysize * sizeof(pmix_alloc_directive_t))
        if not array[0].array:
            raise MemoryError()
        n = 0
        aldptr = <pmix_alloc_directive_t*> array[0].array
        for item in mylist:
            aldptr[n] = item
            n += 1
    elif PMIX_ENVAR == mytype:
        array[0].array = PyMem_Malloc(mysize * sizeof(pmix_envar_t))
        if not array[0].array:
            raise MemoryError()
        n = 0
        envptr = <pmix_envar_t*> array[0].array
        for item in mylist:
            envptr[n].envar = strdup(item['envar'])
            envptr[n].value = strdup(item['value'])
            envptr[n].separator = item['separator']
            n += 1
    else:
        print("UNRECOGNIZED DATA TYPE IN ARRAY")
        return PMIX_ERR_NOT_SUPPORTED
    return PMIX_SUCCESS


# provide conversion programs that translate incoming
# PMIx structures into Python dictionaries, and incoming
# arrays into Python lists of objects

def pmix_bool_convert(f):
    if isinstance(f, str):
        if f.startswith('t') or f.startswith('T'):
            return 1
        elif f.startswith('f') or f.startswith('F'):
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
    if 'b' == ky[0]:
        memcpy(key, &pykeyptr[2], klen-3)
    else:
        memcpy(key, pykeyptr, klen)

# provide a function for transferring a Python 'value'
# object (a tuple containing the value and its type)
# to a pmix_value_t
cdef int pmix_load_value(pmix_value_t *value, val:tuple):
    value[0].type = val[1]
    if val[1] == PMIX_BOOL:
        value[0].data.flag = pmix_bool_convert(val[0])
    elif val[1] == PMIX_BYTE:
        value[0].data.byte = val[0]
    elif val[1] == PMIX_STRING:
        if isinstance(val[0], str):
            pykey = val[0].encode('ascii')
        else:
            pykey = val[0]
        try:
            value[0].data.string = strdup(pykey)
        except:
            raise ValueError("String value declared but non-string provided")
    elif val[1] == PMIX_SIZE:
        if not isinstance(val[0], pmix_int_types):
            raise TypeError("size_t value declared but non-integer provided")
        value[0].data.size = val[0]
    elif val[1] == PMIX_PID:
        if not isinstance(val[0], pmix_int_types):
            raise TypeError("pid value declared but non-integer provided")
        if val[0] < 0:
            raise ValueError("pid value is negative")
        value[0].data.pid = val[0]
    elif val[1] == PMIX_INT:
        if not isinstance(val[0], pmix_int_types):
            raise TypeError("integer value declared but non-integer provided")
        value[0].data.integer = val[0]
    elif val[1] == PMIX_INT8:
        if not isinstance(val[0], pmix_int_types):
            raise TypeError("int8 value declared but non-integer provided")
        if val[0] > 127 or val[0] < -128:
            raise ValueError("int8 value is out of bounds")
        value[0].data.int8 = val[0]
    elif val[1] == PMIX_INT16:
        if not isinstance(val[0], pmix_int_types):
            raise TypeError("int16 value declared but non-integer provided")
        if val[0] > 32767 or val[0] < -32768:
            raise ValueError("int16 value is out of bounds")
        value[0].data.int16 = val[0]
    elif val[1] == PMIX_INT32:
        if not isinstance(val[0], pmix_int_types):
            raise TypeError("int32 value declared but non-integer provided")
        if val[0] > 2147483647 or val[0] < -2147483648:
            raise ValueError("int32 value is out of bounds")
        value[0].data.int32 = val[0]
    elif val[1] == PMIX_INT64:
        if not isinstance(val[0], pmix_int_types):
            raise TypeError("int64 value declared but non-integer provided")
        if val[0] > (2147483647*2147483647) or val[0] < -(2147483648*2147483648):
            raise ValueError("int64 value is out of bounds")
        value[0].data.int64 = val[0]
    elif val[1] == PMIX_UINT:
        if not isinstance(val[0], pmix_int_types):
            raise TypeError("integer value declared but non-integer provided")
        value[0].data.uint = val[0]
    elif val[1] == PMIX_UINT8:
        if not isinstance(val[0], pmix_int_types):
            raise TypeError("uint8 value declared but non-integer provided")
        if val[0] > 255:
            raise ValueError("uint8 value is out of bounds")
        value[0].data.uint8 = val[0]
    elif val[1] == PMIX_UINT16:
        if not isinstance(val[0], pmix_int_types):
            raise TypeError("uint16 value declared but non-integer provided")
        if val[0] > 65536:
            raise ValueError("uint16 value is out of bounds")
        value[0].data.uint16 = val[0]
    elif val[1] == PMIX_UINT32:
        if not isinstance(val[0], pmix_int_types):
            raise TypeError("uint32 value declared but non-integer provided")
        if val[0] > (65536*65536):
            raise ValueError("uint32 value is out of bounds")
        value[0].data.uint32 = val[0]
    elif val[1] == PMIX_UINT64:
        if not isinstance(val[0], pmix_int_types):
            raise TypeError("int64 value declared but non-integer provided")
        if val[0] > (2147483648*2147483648):
            raise ValueError("uint64 value is out of bounds")
        value[0].data.uint64 = val[0]
    elif val[1] == PMIX_FLOAT:
        value[0].data.fval = float(val[0])
    elif val[1] == PMIX_DOUBLE:
        value[0].data.dval = float(val[0])
    elif val[1] == PMIX_TIMEVAL:
        if isinstance(val[0], tuple):
            value[0].data.tv.tv_sec = val[0][0]
            value[0].data.tv.tv_usec = val[0][1]
        else:
            value[0].data.tv.tv_sec = val[0]
            value[0].data.tv.tv_usec = 0
    elif val[1] == PMIX_TIME:
        value[0].data.time = val[1]
    elif val[1] == PMIX_STATUS:
        value[0].data.status = val[1]
    elif val[1] == PMIX_PROC_RANK:
        value[0].data.rank = val[0]
    elif val[1] == PMIX_PROC:
        value[0].data.proc = <pmix_proc_t*> PyMem_Malloc(sizeof(pmix_proc_t))
        if not value[0].data.proc:
            raise MemoryError()
        pmix_copy_nspace(value[0].data.proc[0].nspace, val[0][0])
        value[0].data.proc[0].rank = val[0][1]
    elif val[1] == PMIX_BYTE_OBJECT:
        if 'b' == val[0][0][0]:
            value[0].data.bo.size = val[0][1] - 3
            offset = 2
        else:
            value[0].data.bo.size = val[0][1]
            offset = 0
        value[0].data.bo.bytes = <char*> PyMem_Malloc(value[0].data.bo.size)
        if not value[0].data.bo.bytes:
            raise MemoryError()
        pyarr = bytes(val[0][0][offset])
        pyptr = <const char*> pyarr
        memcpy(value[0].data.bo.bytes, pyptr, value[0].data.bo.size)
    elif val[1] == PMIX_PERSISTENCE:
        value[0].data.persist = val[0]
    elif val[1] == PMIX_SCOPE:
        value[0].data.scope = val[0]
    elif val[1] == PMIX_RANGE:
        value[0].data.range = val[0]
    elif val[1] == PMIX_PROC_STATE:
        value[0].data.state = val[0]
    elif val[1] == PMIX_PROC_INFO:
        value[0].data.pinfo = <pmix_proc_info_t*> PyMem_Malloc(sizeof(pmix_proc_info_t))
        if not value[0].data.pinfo:
            raise MemoryError()
        pmix_copy_nspace(value[0].data.pinfo[0].proc.nspace, val[0]['proc'][0])
        value[0].data.pinfo[0].proc.rank = val[0]['proc'][1]
        value[0].data.pinfo[0].hostname = strdup(val[0]['hostname'])
        value[0].data.pinfo[0].pid = val[0]['pid']
        value[0].data.pinfo[0].exit_code = val[0]['exitcode']
        value[0].data.pinfo[0].state = val[0]['state']
    elif val[1] == PMIX_DATA_ARRAY:
        value[0].data.darray = <pmix_data_array_t*> PyMem_Malloc(sizeof(pmix_data_array_t))
        if not value[0].data.darray:
            raise MemoryError()
        value[0].data.darray[0].type = val[0][0]
        value[0].data.darray[0].size = len(val[0][1])
        try:
            pmix_load_darray(value[0].data.darray, value[0].data.darray[0].type, val[0][1])
        except:
            return PMIX_ERR_NOT_SUPPORTED
    elif val[1] == PMIX_ALLOC_DIRECTIVE:
        if not isinstance(val[0], pmix_int_types):
            raise TypeError("allocdirective value declared but non-integer provided")
        if val[0] > 255:
            raise ValueError("allocdirective value is out of bounds")
        value[0].data.adir = val[0]
    elif val[1] == PMIX_ENVAR:
        enval = val[0]['envar']
        if isinstance(enval, str):
            pyns = enval.encode('ascii')
        else:
            pyns = enval
        pynsptr = <const char *>(pyns)
        value[0].data.envar.envar = strdup(pynsptr)
        enval = val[0]['value']
        if isinstance(enval, str):
            pyns = enval.encode('ascii')
        else:
            pyns = enval
        pynsptr = <const char *>(pyns)
        value[0].data.envar.value = strdup(pynsptr)
        value[0].data.envar.separator = val[0]['separator']
    else:
        print("UNRECOGNIZED VALUE TYPE")
        return PMIX_ERR_NOT_SUPPORTED
    return PMIX_SUCCESS

cdef tuple pmix_unload_value(const pmix_value_t *value):
    if PMIX_BOOL == value[0].type:
        if value[0].data.flag:
            return (True, PMIX_BOOL)
        else:
            return (False, PMIX_BOOL)
    elif PMIX_BYTE == value[0].type:
        return (value[0].data.byte, PMIX_BYTE)
    elif PMIX_STRING == value[0].type:
        pyb = value[0].data.string
        pystr = pyb.decode("ascii")
        return (pystr, PMIX_STRING)
    elif PMIX_SIZE == value[0].type:
        return (value[0].data.size, PMIX_SIZE)
    elif PMIX_PID == value[0].type:
        return (value[0].data.pid, PMIX_PID)
    elif PMIX_INT == value[0].type:
        return (value[0].data.integer, PMIX_INT)
    elif PMIX_INT8 == value[0].type:
        return (value[0].data.int8, PMIX_INT8)
    elif PMIX_INT16 == value[0].type:
        return (value[0].data.int16, PMIX_INT16)
    elif PMIX_INT32 == value[0].type:
        return (value[0].data.int32, PMIX_INT32)
    elif PMIX_INT64 == value[0].type:
        return (value[0].data.int64, PMIX_INT64)
    elif PMIX_UINT == value[0].type:
        return (value[0].data.uint, PMIX_UINT)
    elif PMIX_UINT8 == value[0].type:
        return (value[0].data.uint8, PMIX_UINT8)
    elif PMIX_UINT16 == value[0].type:
        return (value[0].data.uint16, PMIX_UINT16)
    elif PMIX_UINT32 == value[0].type:
        return (value[0].data.uint32, PMIX_UINT32)
    elif PMIX_UINT64 == value[0].type:
        return (value[0].data.uint64, PMIX_UINT64)
    elif PMIX_FLOAT == value[0].type:
        return (value[0].data.fval, PMIX_FLOAT)
    elif PMIX_DOUBLE == value[0].type:
        return (value[0].data.dval, PMIX_DOUBLE)
    elif PMIX_TIMEVAL == value[0].type:
        return ((value[0].data.tv.tv_sec, value[0].data.tv.tv_used), PMIX_TIMEVAL)
    elif PMIX_TIME == value[0].type:
        return (value[0].data.time, PMIX_TIME)
    elif PMIX_STATUS == value[0].type:
        return (value[0].data.status, PMIX_STATUS)
    elif PMIX_PROC_RANK == value[0].type:
        return (value[0].data.rank, PMIX_PROC_RANK)
    elif PMIX_PROC == value[0].type:
        pyns = str(value[0].data.proc[0].nspace)
        return ((pyns, value[0].data.proc[0].rank), PMIX_PROC)
    elif PMIX_BYTE_OBJECT == value[0].type:
        mybytes = <char*> PyMem_Malloc(value[0].data.bo.size)
        if not mybytes:
            raise MemoryError()
        memcpy(mybytes, value[0].data.bo.bytes, value[0].data.bo.size)
        return ((mybytes, value[0].data.bo.size), PMIX_BYTE_OBJECT)
    elif PMIX_PERSISTENCE == value[0].type:
        return (value[0].data.persist, PMIX_PERSISTENCE)
    elif PMIX_SCOPE == value[0].type:
        return (value[0].data.scope, PMIX_SCOPE)
    elif PMIX_RANGE == value[0].type:
        return (value[0].data.range, PMIX_RANGE)
    elif PMIX_PROC_STATE == value[0].type:
        return (value[0].data.state, PMIX_PROC_STATE)
    elif PMIX_PROC_INFO == value[0].type:
        pins = str(value[0].data.pinfo[0].proc.nspace)
        pirk = value[0].data.pinfo[0].proc.rank
        pihost = str(value[0].data.pinfo[0].hostname)
        pipid = value[0].data.pinfo[0].pid
        piex = value[0].data.pinfo[0].exit_code
        pist = value[0].data.pinfo[0].state
        pians = {'proc': (pins, pirk), 'hostname': pihost, 'pid': pipid, 'exitcode': piex, 'state': pist}
        return (pians, PMIX_PROC_INFO)
    elif PMIX_DATA_ARRAY == value[0].type:
        raise ValueError("Unload_value: data array not supported")
    elif PMIX_ALLOC_DIRECTIVE == value[0].type:
        return (value[0].data.adir, PMIX_ALLOC_DIRECTIVE)
    elif PMIX_ENVAR:
        pyenv = str(value[0].data.envar.envar)
        pyval = str(value[0].data.envar.value)
        pysep = value[0].data.envar.separator
        pyenvans = {'envar': pyenv, 'value': pyval, 'separator': pysep}
        return (pyenvans, PMIX_ENVAR)
    else:
        raise ValueError("Unload_value: provided type is unknown")


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
        pykey = str(key)
        pmix_copy_key(array[n].key, pykey)
        # the value also needs to be transferred
        rc = pmix_load_value(&array[n].value, keyvals[key])
        if PMIX_SUCCESS != rc:
            return rc
        n += 1
    return PMIX_SUCCESS

cdef int pmix_unload_info(const pmix_info_t *info, size_t ninfo, ilist:list):
    cdef char* kystr
    cdef size_t n = 0
    while n < ninfo:
        val = pmix_unload_value(&info[n].value)
        if val[1] == PMIX_UNDEF:
            return PMIX_ERR_NOT_SUPPORTED
        keyval = {}
        kystr = strdup(info[n].key)
        pykey = kystr.decode("ascii")
        free(kystr)
        keyval[pykey] = val
        ilist.append(keyval)
        n += 1
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
cdef int pmix_load_procs(pmix_proc_t *proc, peers:list):
    n = 0
    for p in peers:
        pmix_copy_nspace(proc[n].nspace, p[0])
        proc[n].rank = p[1]
        n += 1
    return PMIX_SUCCESS

cdef int pmix_unload_procs(const pmix_proc_t *procs, size_t nprocs, peers:list):
    n = 0
    while n < nprocs:
        myns = str(procs[n].nspace)
        peers.append((myns, procs[n].rank))
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

cdef void pmix_unload_bytes(char *data, size_t ndata, blist:list):
    cdef size_t n = 0
    while n < ndata:
        blist.append(data[n])
        n += 1

cdef void pmix_unload_apps(const pmix_app_t *apps, size_t napps, pyapps:list):
    cdef size_t n = 0
    while n < napps:
        myapp = {}
        myapp['cmd'] = str(apps[n].cmd)
        myargv = []
        if NULL != apps[n].argv:
            pmix_load_argv(apps[n].argv, myargv)
            myapp['argv'] = myargv
        myenv = []
        if NULL != apps[n].env:
            pmix_load_argv(apps[n].env, myenv)
            myapp['env'] = myenv
        myapp['maxprocs'] = apps[n].maxprocs
        keyvals = {}
        if NULL != apps[n].info:
            pmix_unload_info(apps[n].info, apps[n].ninfo, keyvals)
            myapp['info'] = keyvals
        pyapps.append(myapp)
        n += 1
