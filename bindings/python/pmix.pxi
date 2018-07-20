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
            if (isinstance(keyvals[key], str)):
                self.array[n].value.type = PMIX_STRING
                if isinstance(keyvals[key], str):
                    pykey = keyvals[key].encode('ascii')
                else:
                    pykey = keyvals[key]
                self.array[n].value.data.string = strdup(pykey)
            n += 1

    def unload(self, infoarray:list):
        for n in range(self.ninfo):
            pyin = {}
            pyin['key'] = self.array[n].key
            pyin['value'] = "foo"
            infoarray.append(pyin)

    def __dealloc__(self):
        PyMem_Free(self.array)
