from libc.string cimport memset,strncpy, strdup
from libc.stdlib cimport malloc, free
from libc.string cimport memcpy
from cpython.mem cimport PyMem_Malloc, PyMem_Realloc, PyMem_Free

# pull in all the constant definitions - we
# store them in a separate file for neatness
include "pmix_constants.pxi"

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

cdef class PMIxInfoArray:
    cdef pmix_info_t* array
    cdef size_t ninfo

    def __cinit__(self, size_t number):
        # allocate some memory (uninitialised, may contain arbitrary data)
        self.array = <pmix_info_t*> PyMem_Malloc(number * sizeof(pmix_info_t))
        if not self.array:
            raise MemoryError()
        self.ninfo = number

    def load(self, keyvals:dict):
        kvkeys = list(keyvals.keys())
        if len(kvkeys) > <int>self.ninfo:
            # this should never happen except in development
            # so raise an error to let them know
            raise IndexError()
        n = 0
        for key in kvkeys:
            klen = len(key)
            if isinstance(key, str):
                pykey = key.encode('ascii')
            else:
                pykey = key
            pykeyptr = <const char *>(pykey)
            memset(self.array[n].key, 0, PMIX_MAX_KEYLEN+1)
            memcpy(self.array[n].key, pykeyptr, klen)
            # the value also needs to be transferred
            if keyvals[key][1] == 'bool':
                self.array[n].value.type = PMIX_BOOL
                self.array[n].value.data.flag = pmix_bool_convert(keyvals[key][0])
            elif keyvals[key][1] == 'byte':
                self.array[n].value.type = PMIX_BYTE
                self.array[n].value.data.byte = keyvals[key][0]
            elif keyvals[key][1] == 'string':
                self.array[n].value.type = PMIX_STRING
                if isinstance(keyvals[key][0], str):
                    pykey = keyvals[key][0].encode('ascii')
                else:
                    pykey = keyvals[key][0]
                try:
                    self.array[n].value.data.string = strdup(pykey)
                except:
                    raise ValueError("String value declared but non-string provided")
            elif keyvals[key][1] == 'size':
                self.array[n].value.type = PMIX_SIZE
                if not isinstance(keyvals[key][0], pmix_int_types):
                    raise TypeError("size_t value declared but non-integer provided")
                self.array[n].value.data.size = keyvals[key][0]
            elif keyvals[key][1] == 'pid':
                self.array[n].value.type = PMIX_PID
                if not isinstance(keyvals[key][0], pmix_int_types):
                    raise TypeError("pid value declared but non-integer provided")
                if keyvals[key][0] < 0:
                    raise ValueError("pid value is negative")
                self.array[n].value.data.pid = keyvals[key][0]
            elif keyvals[key][1] == 'int':
                self.array[n].value.type = PMIX_INT
                if not isinstance(keyvals[key][0], pmix_int_types):
                    raise TypeError("integer value declared but non-integer provided")
                self.array[n].value.data.integer = keyvals[key][0]
            elif keyvals[key][1] == 'int8':
                self.array[n].value.type = PMIX_INT8
                if not isinstance(keyvals[key][0], pmix_int_types):
                    raise TypeError("int8 value declared but non-integer provided")
                if keyvals[key][0] > 127 or keyvals[key][0] < -128:
                    raise ValueError("int8 value is out of bounds")
                self.array[n].value.data.int8 = keyvals[key][0]
            elif keyvals[key][1] == 'int16':
                self.array[n].value.type = PMIX_INT16
                if not isinstance(keyvals[key][0], pmix_int_types):
                    raise TypeError("int16 value declared but non-integer provided")
                if keyvals[key][0] > 32767 or keyvals[key][0] < -32768:
                    raise ValueError("int16 value is out of bounds")
                self.array[n].value.data.int16 = keyvals[key][0]
            elif keyvals[key][1] == 'int32':
                self.array[n].value.type = PMIX_INT32
                if not isinstance(keyvals[key][0], pmix_int_types):
                    raise TypeError("int32 value declared but non-integer provided")
                if keyvals[key][0] > 2147483647 or keyvals[key][0] < -2147483648:
                    raise ValueError("int16 value is out of bounds")
                self.array[n].value.data.int32 = keyvals[key][0]
            else:
                print("UNRECOGNIZED INFO VALUE TYPE FOR KEY", key)
            n += 1

    def unload(self, infoarray:list):
        for n in range(self.ninfo):
            pyin = {}
            pyin['key'] = self.array[n].key
            pyin['value'] = "foo"
            infoarray.append(pyin)

    def __dealloc__(self):
        PyMem_Free(self.array)
