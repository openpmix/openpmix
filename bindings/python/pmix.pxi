#file: pmix.pxi
#
# Copyright (c) 2022      Nanook Consulting. All rights reserved
#

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
        self.data = bytearray(0)

    def set(self, status):
        self.status = status
        self.event.set()

    # Arm for a new request. What the previous one left is dropped here,
    # so a request that fails without caching anything can never hand
    # back the last one's results
    def clear(self):
        self.status = PMIX_ERR_NOT_SUPPORTED
        self.sz = 0
        self.info = []
        self.data = bytearray(0)
        self.event.clear()

    def wait(self):
        self.event.wait()

    def get_status(self):
        return self.status

    def cache_data(self, data, sz):
        # 'data' arrives already converted to a Python bytes/bytearray by
        # the caller, which must copy it: the storage behind it belongs to
        # the PMIx server and is released as soon as the callback returns.
        #
        # The version this replaces tried to build the copy with
        # array.array('B', data[0]) - in Python 3 an index into a bytes
        # object is an int, and array() rejects one - so a direct modex
        # request raised TypeError inside the C callback every time.
        if data is None:
            self.data = bytearray(0)
        else:
            self.data = bytearray(data)
        self.sz = len(self.data)

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

# Find the Python handler the library means by "refid".
#
# A handler can be called before the registration that creates it has
# returned its refid: registering replays any matching cached event, and
# pulling IOF delivers any output already held, on the progress thread
# and ahead of the acknowledgement the caller is waiting on. So while a
# registration is in flight ("pending" is its record), a refid we hold no
# entry for is that registration's, and it is recorded there and then -
# the handler sees the event at once rather than after a timed retry that
# could run before the caller had recorded anything.
#
# "status" is the event's code, or None for IOF. A pending event handler
# is matched only on a code it asked for, so an event still on its way
# to a handler deregistered a moment ago is not handed to the new one.
def _hdlr_find(hdlrs:list, pending, refid, status=None):
    for h in hdlrs:
        if refid == h['refid']:
            return h['hdlr']
    if pending is None or pending['refid'] is not None:
        return None
    if status is not None and pending['codes'] is not None and \
       status not in pending['codes']:
        return None
    pending['refid'] = refid
    hdlrs.append({'refid': refid, 'hdlr': pending['hdlr']})
    return pending['hdlr']

# Record the outcome of a registration once the library has returned it:
# an entry for "rc", unless the registration failed - and none for any
# refid _hdlr_find took on its behalf that the library did not give it
def _hdlr_settle(hdlrs:list, pending, rc):
    if pending['refid'] is not None and pending['refid'] != rc:
        _hdlr_drop(hdlrs, pending['refid'])
        pending['refid'] = None
    if 0 > rc or pending['refid'] is not None:
        return
    hdlrs.append({'refid': rc, 'hdlr': pending['hdlr']})

def _hdlr_drop(hdlrs:list, refid):
    for n, h in enumerate(hdlrs):
        if refid == h['refid']:
            del hdlrs[n]
            return

cdef void pmix_convert_locality(pmix_locality_t loc, pyloc:list):
    if PMIX_LOCALITY_NONLOCAL & loc:
        pyloc.append(PMIX_LOCALITY_NONLOCAL)
    if PMIX_LOCALITY_SHARE_HWTHREAD & loc:
        pyloc.append(PMIX_LOCALITY_SHARE_HWTHREAD)
    if PMIX_LOCALITY_SHARE_CORE & loc:
        pyloc.append(PMIX_LOCALITY_SHARE_CORE)
    if PMIX_LOCALITY_SHARE_L1CACHE & loc:
        pyloc.append(PMIX_LOCALITY_SHARE_L1CACHE)
    if PMIX_LOCALITY_SHARE_L2CACHE & loc:
        pyloc.append(PMIX_LOCALITY_SHARE_L2CACHE)
    if PMIX_LOCALITY_SHARE_L3CACHE & loc:
        pyloc.append(PMIX_LOCALITY_SHARE_L3CACHE)
    if PMIX_LOCALITY_SHARE_PACKAGE & loc:
        pyloc.append(PMIX_LOCALITY_SHARE_PACKAGE)
    if PMIX_LOCALITY_SHARE_NUMA & loc:
        pyloc.append(PMIX_LOCALITY_SHARE_NUMA)
    if PMIX_LOCALITY_SHARE_NODE & loc:
        pyloc.append(PMIX_LOCALITY_SHARE_NODE)
    if 0 == len(pyloc):
        pyloc.append(PMIX_LOCALITY_NONLOCAL)
    return

# The Python representation of a pmix_cpuset_t is a dict of the form
#
#     {'source': str, 'cpus': [int, int, ...]}
#
# where 'source' names the provider that generated the bitmap (the library
# currently only supports "hwloc") and 'cpus' is the list of CPU indices in
# the set. The bitmap itself is an opaque provider object that Python cannot
# construct, so every crossing goes through the library's string form,
# "<source>:<range-list>" - e.g. "hwloc:0-3,8". The two converters below
# translate between that string and the dict. They are deliberately plain
# module-level functions rather than cdef helpers so the range expansion,
# which is where the interesting logic lives, can be unit tested without a
# server.

# Expand a cpuset string into the Python cpuset dict. Returns None if the
# string is not of the expected "<source>:<range-list>" form.
def pmix_cpuset_from_string(csetstr):
    if isinstance(csetstr, bytes):
        csetstr = csetstr.decode('ascii')
    if not isinstance(csetstr, str):
        return None
    # the source is everything ahead of the first ':' delimiter
    source, sep, ranges = csetstr.partition(':')
    if '' == sep:
        # no delimiter - not a cpuset string
        return None
    cpus = []
    for token in ranges.split(','):
        token = token.strip()
        if '' == token:
            continue
        if '-' in token:
            lo, _, hi = token.partition('-')
            try:
                start = int(lo)
                end = int(hi)
            except ValueError:
                return None
            if end < start:
                return None
            cpus.extend(range(start, end + 1))
        else:
            try:
                cpus.append(int(token))
            except ValueError:
                return None
    return {'source': source, 'cpus': cpus}

# Render a Python cpuset dict into the library's cpuset string. The 'cpus'
# entry may be a list of indices, a list of range tokens ('0-3'), or an
# already-formatted range list string - callers have used all three. A
# missing 'source' defaults to the only provider the library implements.
# Returns None if the dict cannot be interpreted.
def pmix_cpuset_to_string(pycpus):
    if not isinstance(pycpus, dict):
        return None
    source = pycpus.get('source', 'hwloc')
    if isinstance(source, bytes):
        source = source.decode('ascii')
    if not isinstance(source, str) or '' == source:
        return None
    cpus = pycpus.get('cpus', [])
    if isinstance(cpus, bytes):
        cpus = cpus.decode('ascii')
    if isinstance(cpus, str):
        # already a formatted range list
        ranges = cpus
    else:
        try:
            tokens = [x.decode('ascii') if isinstance(x, bytes) else str(x) for x in cpus]
        except TypeError:
            return None
        ranges = ','.join(tokens)
    return source + ':' + ranges

# Build a pmix_cpuset_t from the Python dict. The caller owns the result
# and must release it with pmix_destruct_cpuset once the library is done
# with it. On any failure the cpuset is left empty and safe to destruct.
cdef int pmix_load_cpuset(pmix_cpuset_t *cpuset, pycpus):
    PMIx_Cpuset_construct(cpuset)
    csetstr = pmix_cpuset_to_string(pycpus)
    if csetstr is None:
        return PMIX_ERR_BAD_PARAM
    pycset = csetstr.encode('ascii')
    return PMIx_Parse_cpuset_string(pycset, cpuset)

# Convert a pmix_cpuset_t into the Python dict, filling in pycpus. The
# library renders the bitmap for us and hands back a malloc'd string that
# we own.
cdef int pmix_unload_cpuset(const pmix_cpuset_t *cpuset, pycpus:dict):
    cdef char *csetstr = NULL

    # the library's renderer reads the source without checking it, so do
    # not hand it a cpuset that carries a bitmap but no provider name
    if NULL == cpuset[0].source:
        pycpus['source'] = ''
        pycpus['cpus'] = []
        return PMIX_SUCCESS
    rc = PMIx_server_generate_cpuset_string(cpuset, &csetstr)
    if PMIX_SUCCESS != rc:
        return rc
    if NULL == csetstr:
        # an unbound process has no cpus in its set
        pycpus['source'] = ''
        pycpus['cpus'] = []
        return PMIX_SUCCESS
    txt = csetstr.decode('ascii')
    free(csetstr)
    converted = pmix_cpuset_from_string(txt)
    if converted is None:
        return PMIX_ERR_BAD_PARAM
    pycpus['source'] = converted['source']
    pycpus['cpus'] = converted['cpus']
    return PMIX_SUCCESS

# Release the contents of a cpuset built by pmix_load_cpuset or returned
# by the library. Both the source string and the provider bitmap belong to
# the library, so hand them back to it rather than freeing them here. The
# library's destructor tolerates NULL fields, so this is safe to call on a
# cpuset whose construction failed partway through.
cdef void pmix_destruct_cpuset(pmix_cpuset_t *cpuset):
    PMIx_Cpuset_destruct(cpuset)

cdef void pmix_unload_argv(char **keys, argv:list):
    n = 0
    while NULL != keys[n]:
        argv.append(pmix_decode_str(keys[n]))
        n += 1

cdef int pmix_load_argv(char **keys, size_t nmax, argv):
    cdef size_t n
    # the caller allocated nmax+1 entries; zero them all, so a failure
    # partway leaves a well-formed, shorter argv that pmix_free_argv can
    # walk
    memset(keys, 0, (nmax + 1) * sizeof(char*))
    seq = pmix_snapshot(argv, nmax)
    if seq is None:
        return PMIX_ERR_BAD_PARAM
    n = 0
    for a in seq:
        if a is None:
            # an argv ends at its first NULL, so None cannot be an entry
            keys[n] = NULL
            return PMIX_ERR_BAD_PARAM
        rc = pmix_load_cstr(&keys[n], a)
        if PMIX_SUCCESS != rc:
            keys[n] = NULL
            return rc
        n += 1
    keys[n] = NULL
    return PMIX_SUCCESS

# Release a NULL-terminated argv array built by pmix_load_argv. Its
# entries were strdup'd, so both they and the array itself belong to the
# C allocator - callers must therefore malloc the array, not PyMem_Malloc
# it
cdef void pmix_free_argv(char **argv):
    if NULL == argv:
        return
    n = 0
    while NULL != argv[n]:
        free(argv[n])
        n += 1
    free(argv)

# A private copy of a list or tuple the caller is about to load into an
# array it allocated for nmax entries - or None if it is not a list or
# tuple, or does not hold exactly that many.
#
# Every array loader sizes the C array from the length of what it is
# given and then fills it by iterating, and those are two separate
# questions to put to a Python object: a list subclass can answer them
# differently, and a list can grow while it is being walked (the loop
# runs Python code - a comparison, a dict lookup, an encode - that may
# reach it). Either way the loop would write past the end of the block.
# A plain list that only the loader holds can do neither, so each loader
# walks one of these, checked against the count the block was sized for
cdef object pmix_snapshot(obj, size_t nmax):
    if not isinstance(obj, (list, tuple)):
        return None
    try:
        seq = list(obj)
    except Exception:
        traceback.print_exc()
        return None
    if <size_t> len(seq) != nmax:
        print("A list changed length while it was being converted")
        return None
    return seq

# A private copy of a list or tuple whose length the caller has not
# taken yet, or None if it is not one
cdef object pmix_listcopy(obj):
    if not isinstance(obj, (list, tuple)):
        return None
    try:
        return list(obj)
    except Exception:
        traceback.print_exc()
        return None

# The bytes of a payload - a byte object, a regex, a coordinate vector, a
# data buffer - or None if obj is not one. bytes(), which this replaced,
# also takes an integer and answers that many NUL bytes
cdef object pmix_payload(obj):
    if isinstance(obj, str):
        try:
            return obj.encode('UTF-8')
        except UnicodeError:
            return None
    if isinstance(obj, (bytes, bytearray, memoryview)):
        return bytes(obj)
    return None

# Allocate the block behind a data array. Every arm of pmix_load_darray
# needs the same two guards, and getting either wrong is invisible until
# it bites: malloc(0) is allowed to answer NULL, which the "did the
# allocation fail?" test below would then report as PMIX_ERR_NOMEM on a
# perfectly legal empty array, and an array the loader may abandon partway
# has to be safe for the caller to destruct. So an empty array stays NULL
# (its size is zero, so nothing reads it) and every block is zeroed.
cdef void* pmix_darray_alloc(size_t nmemb, size_t esize):
    cdef void *ptr
    if 0 == nmemb or 0 == esize:
        return NULL
    ptr = malloc(nmemb * esize)
    if NULL != ptr:
        memset(ptr, 0, nmemb * esize)
    return ptr

# True when pmix_darray_alloc failed rather than declining to allocate an
# empty array
cdef int pmix_darray_nomem(void *ptr, size_t nmemb):
    if NULL == ptr and 0 < nmemb:
        return 1
    return 0

# Build a pmix_coord_t from the Python coord dict,
#
#     {'view': int, 'coord': bytes, 'dims': int}
#
# whose 'coord' entry is the raw uint32 vector as bytes, exactly as
# pmix_unload_value hands it back. 'dims' counts coordinates, not bytes,
# so the payload must be four times as long - a dims larger than the bytes
# actually provided used to be taken at face value and read off the end of
# the Python object
cdef int pmix_load_coord(pmix_coord_t *coord, pycoord):
    cdef const char *ptr
    cdef size_t nbytes

    coord[0].view = PMIX_COORD_VIEW_UNDEF
    coord[0].coord = NULL
    coord[0].dims = 0
    if not isinstance(pycoord, dict):
        return PMIX_ERR_BAD_PARAM
    view = pycoord.get('view', PMIX_COORD_VIEW_UNDEF)
    rc = pmix_check_int(view, 0, 255, "coordinate view")
    if PMIX_SUCCESS != rc:
        return rc
    coord[0].view = view
    pybytes = pycoord.get('coord')
    if pybytes is None:
        return PMIX_SUCCESS
    pybytes = pmix_payload(pybytes)
    if pybytes is None:
        return PMIX_ERR_TYPE_MISMATCH
    dims = pycoord.get('dims', len(pybytes) // sizeof(uint32_t))
    if not pmix_is_int(dims) or 0 > dims:
        return PMIX_ERR_BAD_PARAM
    nbytes = dims * sizeof(uint32_t)
    # a length is never negative, so the comparison can be made unsigned
    if <size_t> len(pybytes) < nbytes:
        return PMIX_ERR_BAD_PARAM
    if 0 == nbytes:
        return PMIX_SUCCESS
    coord[0].coord = <uint32_t*> malloc(nbytes)
    if NULL == coord[0].coord:
        return PMIX_ERR_NOMEM
    ptr = <const char*>pybytes
    memcpy(coord[0].coord, ptr, nbytes)
    coord[0].dims = dims
    return PMIX_SUCCESS

# Render a pmix_coord_t back into its Python dict. The vector is handed
# back as raw bytes, which is what pmix_load_coord takes, so a coord that
# comes out of here can be passed straight back in. A NULL vector is legal
cdef dict pmix_unload_coord(const pmix_coord_t *coord):
    cdef size_t nbytes
    if NULL == coord:
        return {'view': PMIX_COORD_VIEW_UNDEF, 'coord': b'', 'dims': 0}
    if NULL == coord[0].coord or 0 == coord[0].dims:
        return {'view': coord[0].view, 'coord': b'', 'dims': 0}
    nbytes = coord[0].dims * sizeof(uint32_t)
    return {'view': coord[0].view,
            'coord': (<char*>coord[0].coord)[:nbytes],
            'dims': coord[0].dims}

# Allocate and zero one struct for a pmix_value_t to point at. The loaders
# below fill these in field by field and can fail partway - an unconvertible
# member is enough - and the caller then hands the whole value to the
# library's destructor, which frees every pointer it finds. Zeroing first is
# what makes those untouched members NULL rather than stack garbage.
cdef void* pmix_value_alloc(size_t sz):
    cdef void *ptr
    ptr = malloc(sz)
    if NULL != ptr:
        memset(ptr, 0, sz)
    return ptr

###########################################################################
# Value conversion
#
# A value crosses as {'value': <python>, 'val_type': PMIX_*}, and the shape
# of <python> for each type is fixed: it is what the unloader below hands
# back, and what the loader takes, so anything that comes out of one can
# be passed straight into the other. See section 3 of the bindings guide.
#
# Four rules hold for every arm, because each has been broken before:
#
#   * an integer is checked against both ends of the C type it lands in,
#     and a failure is a status, never an exception: a mistyped value is
#     PMIX_ERR_TYPE_MISMATCH and an out-of-range one PMIX_ERR_BAD_PARAM;
#   * None is the Python spelling of a NULL - for every string member, and
#     for every type whose union member is a pointer. The library both
#     produces and accepts a NULL there, so both directions must too;
#   * strings are UTF-8 (ASCII is a subset), and an unload that cannot
#     decode one hands back the bytes rather than failing;
#   * everything the loader builds comes from malloc, because the
#     library's destructor is what releases it.
#
# The data-array arms reuse the scalar ones rather than restating them -
# each element is loaded as a value and moved into its slot - so a type
# is supported in an array exactly when it is supported on its own.
###########################################################################

# The bounds of a C integer type of the given width. The arithmetic is
# done on Python ints on purpose: as C arithmetic, 1 << 64 overflows
cdef object pmix_umax(size_t nbytes):
    cdef object bits = 8 * <object> nbytes
    return (1 << bits) - 1

cdef object pmix_smax(size_t nbytes):
    cdef object bits = 8 * <object> nbytes
    return (1 << (bits - 1)) - 1

cdef object pmix_smin(size_t nbytes):
    cdef object bits = 8 * <object> nbytes
    return -(1 << (bits - 1))

# A struct's dict must name every member the struct has: a member that is
# absent is refused, and only an explicit None is taken for a NULL. A
# misspelt key would otherwise load as an empty member and succeed
cdef int pmix_need(v, keys):
    if not isinstance(v, dict):
        return PMIX_ERR_TYPE_MISMATCH
    for k in keys:
        if k not in v:
            print("Value dict is missing its '%s' member" % k)
            return PMIX_ERR_BAD_PARAM
    return PMIX_SUCCESS

# Is v an integer? A bool is an int to Python, but True handed over as a
# rank or a count is a mistake, not a 1
cdef int pmix_is_int(v):
    return isinstance(v, pmix_int_types) and not isinstance(v, bool)

# Check that v is an integer in [lo, hi]. Returns a PMIx status
cdef int pmix_check_int(v, lo, hi, what):
    if not pmix_is_int(v):
        print(what + " value declared but non-integer provided")
        return PMIX_ERR_TYPE_MISMATCH
    if v < lo or v > hi:
        print(what + " value is out of bounds")
        return PMIX_ERR_BAD_PARAM
    return PMIX_SUCCESS

# Copy a Python string into a newly allocated C string. None is NULL
cdef int pmix_load_cstr(char **dest, obj):
    dest[0] = NULL
    if obj is None:
        return PMIX_SUCCESS
    if isinstance(obj, str):
        try:
            pyb = obj.encode('UTF-8')
        except UnicodeError:
            print("String value cannot be encoded as UTF-8")
            return PMIX_ERR_BAD_PARAM
    elif isinstance(obj, (bytes, bytearray)):
        pyb = bytes(obj)
    else:
        print("String value declared but non-string provided")
        return PMIX_ERR_TYPE_MISMATCH
    # a C string ends at its first NUL; one inside would silently
    # truncate what the caller handed us
    if b'\0' in pyb:
        print("String value contains a NUL byte")
        return PMIX_ERR_BAD_PARAM
    dest[0] = strdup(<const char*> pyb)
    if NULL == dest[0]:
        return PMIX_ERR_NOMEM
    return PMIX_SUCCESS

# Decode a C string that the library is allowed to leave NULL. Casting a
# NULL char* to <bytes> walks address zero, which is how several unload
# arms used to crash on a struct whose optional name was simply not set
cdef object pmix_decode_str(const char *ptr):
    if NULL == ptr:
        return None
    pyb = <bytes> ptr
    try:
        return pyb.decode('UTF-8')
    except UnicodeDecodeError:
        return pyb

# A proc dict {'nspace': str, 'rank': int} into a pmix_proc_t
cdef int pmix_load_proc(pmix_proc_t *proc, pyproc):
    rc = pmix_need(pyproc, ('nspace', 'rank'))
    if PMIX_SUCCESS != rc:
        return rc
    rc = pmix_check_int(pyproc.get('rank'), 0, 4294967295, "rank")
    if PMIX_SUCCESS != rc:
        return rc
    ns = pyproc.get('nspace')
    rc = pmix_name_ok(ns, PMIX_MAX_NSLEN, "nspace")
    if PMIX_SUCCESS != rc:
        return rc
    pmix_copy_nspace(proc[0].nspace, ns)
    proc[0].rank = pyproc['rank']
    return PMIX_SUCCESS

cdef dict pmix_unload_proc(const pmix_proc_t *proc):
    return {'nspace': pmix_decode_str(proc[0].nspace), 'rank': proc[0].rank}

# The types whose member of the value union is a pointer to a struct the
# value owns. A data array holds those structs inline, one per slot
cdef int pmix_type_is_boxed(pmix_data_type_t t):
    if t in (PMIX_PROC_NSPACE, PMIX_PROC, PMIX_PROC_INFO, PMIX_DATA_ARRAY,
             PMIX_COORD, PMIX_GEOMETRY, PMIX_DEVICE, PMIX_DEVICE_DIST,
             PMIX_ENDPOINT, PMIX_DATA_BUFFER, PMIX_RESOURCE_UNIT,
             PMIX_NODE_PID, PMIX_REGEX2, PMIX_REGATTR):
        return 1
    return 0

# The size of one element of a data array of type t, as the library's
# pmix_bfrops_base_tma_data_array_construct allocates it - or 0 for a type
# the bindings cannot convert as an element. PMIX_REGEX is absent on
# purpose: the library does not agree with itself on its element width
# (see bfrops/base/AGENTS.md), and so does not construct one either
cdef size_t pmix_darray_esize(pmix_data_type_t t):
    if PMIX_BOOL == t:
        return sizeof(c_bool)
    if t in (PMIX_BYTE, PMIX_INT8, PMIX_UINT8, PMIX_PERSIST, PMIX_SCOPE,
             PMIX_DATA_RANGE, PMIX_PROC_STATE, PMIX_ALLOC_DIRECTIVE,
             PMIX_RESBLOCK_DIRECTIVE, PMIX_ALLOC_INHERIT, PMIX_LINK_STATE,
             PMIX_JOB_STATE):
        return sizeof(uint8_t)
    if t in (PMIX_INT16, PMIX_UINT16, PMIX_LOCTYPE, PMIX_STOR_ACCESS_TYPE,
             PMIX_DATA_TYPE, PMIX_IOF_CHANNEL):
        return sizeof(uint16_t)
    if t in (PMIX_INT32, PMIX_UINT32, PMIX_PROC_RANK, PMIX_INFO_DIRECTIVES):
        return sizeof(uint32_t)
    if t in (PMIX_INT64, PMIX_UINT64, PMIX_DEVTYPE, PMIX_STOR_MEDIUM,
             PMIX_STOR_ACCESS, PMIX_STOR_PERSIST):
        return sizeof(uint64_t)
    if t in (PMIX_INT, PMIX_UINT, PMIX_STATUS):
        return sizeof(int)
    if PMIX_STRING == t:
        return sizeof(char*)
    if PMIX_SIZE == t:
        return sizeof(size_t)
    if PMIX_PID == t:
        return sizeof(pid_t)
    if PMIX_FLOAT == t:
        return sizeof(float)
    if PMIX_DOUBLE == t:
        return sizeof(double)
    if PMIX_TIMEVAL == t:
        return sizeof(timeval)
    if PMIX_TIME == t:
        return sizeof(time_t)
    if PMIX_PROC_NSPACE == t:
        return sizeof(pmix_nspace_t)
    if PMIX_PROC == t:
        return sizeof(pmix_proc_t)
    if t in (PMIX_BYTE_OBJECT, PMIX_COMPRESSED_BYTE_OBJECT,
             PMIX_COMPRESSED_STRING):
        return sizeof(pmix_byte_object_t)
    if PMIX_TOPO == t:
        return sizeof(pmix_topology_t)
    if PMIX_PROC_CPUSET == t:
        return sizeof(pmix_cpuset_t)
    if PMIX_PROC_INFO == t:
        return sizeof(pmix_proc_info_t)
    if PMIX_DATA_ARRAY == t:
        return sizeof(pmix_data_array_t)
    if PMIX_ENVAR == t:
        return sizeof(pmix_envar_t)
    if PMIX_COORD == t:
        return sizeof(pmix_coord_t)
    if PMIX_GEOMETRY == t:
        return sizeof(pmix_geometry_t)
    if PMIX_DEVICE == t:
        return sizeof(pmix_device_t)
    if PMIX_DEVICE_DIST == t:
        return sizeof(pmix_device_distance_t)
    if PMIX_ENDPOINT == t:
        return sizeof(pmix_endpoint_t)
    if PMIX_DATA_BUFFER == t:
        return sizeof(pmix_data_buffer_t)
    if PMIX_RESOURCE_UNIT == t:
        return sizeof(pmix_resource_unit_t)
    if PMIX_NODE_PID == t:
        return sizeof(pmix_node_pid_t)
    if PMIX_REGEX2 == t:
        return sizeof(pmix_regex2_t)
    if PMIX_REGATTR == t:
        return sizeof(pmix_regattr_t)
    return 0

# Build a data array of type 'mytype' from a Python list.
#
# The array is described as it is filled - type first, then the size of
# what is already in the block - so a load that fails partway leaves the
# caller holding something the library's destructor releases correctly:
# every slot is zeroed, and the size never covers a slot not yet written.
cdef int pmix_load_darray(pmix_data_array_t *array, mytype, mylist):
    cdef pmix_value_t tmp
    cdef size_t mysize
    cdef size_t esize
    cdef size_t n
    cdef char *slot

    array[0].array = NULL
    array[0].size = 0
    rc = pmix_check_int(mytype, 0, 65535, "data array type")
    if PMIX_SUCCESS != rc:
        return rc
    array[0].type = mytype
    if mylist is None:
        return PMIX_SUCCESS
    # walk a private copy: see pmix_snapshot
    mylist = pmix_listcopy(mylist)
    if mylist is None:
        return PMIX_ERR_TYPE_MISMATCH
    mysize = len(mylist)

    # the three element types that have no scalar form
    if PMIX_INFO == mytype:
        array[0].array = pmix_darray_alloc(mysize, sizeof(pmix_info_t))
        if pmix_darray_nomem(array[0].array, mysize):
            return PMIX_ERR_NOMEM
        array[0].size = mysize
        return pmix_load_info(<pmix_info_t*> array[0].array, mysize, mylist)
    if PMIX_VALUE == mytype:
        array[0].array = pmix_darray_alloc(mysize, sizeof(pmix_value_t))
        if pmix_darray_nomem(array[0].array, mysize):
            return PMIX_ERR_NOMEM
        array[0].size = mysize
        n = 0
        for item in mylist:
            rc = pmix_load_value(&(<pmix_value_t*> array[0].array)[n], item)
            if PMIX_SUCCESS != rc:
                return rc
            n += 1
        return PMIX_SUCCESS
    if PMIX_APP == mytype:
        array[0].array = pmix_darray_alloc(mysize, sizeof(pmix_app_t))
        if pmix_darray_nomem(array[0].array, mysize):
            return PMIX_ERR_NOMEM
        array[0].size = mysize
        return pmix_load_apps(<pmix_app_t*> array[0].array, mysize, mylist)

    esize = pmix_darray_esize(mytype)
    if 0 == esize or PMIX_TOPO == mytype or PMIX_PROC_CPUSET == mytype:
        # a topology or a cpuset has no Python form to load an element
        # from (see the scalar arm); they unload, as None, but only that
        print("UNRECOGNIZED DATA TYPE IN ARRAY "+str(mytype))
        return PMIX_ERR_NOT_SUPPORTED
    array[0].array = pmix_darray_alloc(mysize, esize)
    if pmix_darray_nomem(array[0].array, mysize):
        return PMIX_ERR_NOMEM
    # describe the whole block at once. It is zeroed, so a slot not yet
    # written is an empty element the library's destructor releases as
    # such - and some of its destructors release the block only when the
    # count is non-zero, so a size grown slot by slot leaked the block of
    # an array whose first element failed
    array[0].size = mysize
    n = 0
    for item in mylist:
        # a slot holds its struct inline, so it has no way to be NULL: a
        # None here would silently become an empty element instead
        if item is None and pmix_type_is_boxed(mytype):
            print("A data array of type", mytype, "cannot hold None")
            return PMIX_ERR_BAD_PARAM
        memset(&tmp, 0, sizeof(pmix_value_t))
        rc = pmix_load_value(&tmp, {'value': item, 'val_type': mytype})
        if PMIX_SUCCESS != rc:
            pmix_destruct_value(&tmp)
            return rc
        # move what the value owns into the slot. A boxed type's struct is
        # copied out of its box and the box alone is freed - the members
        # it points at now belong to the slot. Anything else is the union
        # member itself, which starts at the front of the union
        slot = (<char*> array[0].array) + n * esize
        if pmix_type_is_boxed(mytype):
            if NULL != tmp.data.ptr:
                memcpy(slot, tmp.data.ptr, esize)
                free(tmp.data.ptr)
        else:
            memcpy(slot, &tmp.data, esize)
        n += 1
    return PMIX_SUCCESS

# Convert a pmix_data_array_t into a Python dict of the form
# {'type': <type>, 'array': [...]}, or None if an element cannot be
# converted.
#
# This only reads the array - it never releases any part of it. Ownership
# stays with whoever allocated it: the library frees the arrays it hands
# to an upcall or returns from PMIx_Get, and pmix_destruct_value frees the
# ones the bindings built.
cdef object pmix_unload_darray(const pmix_data_array_t *array):
    cdef pmix_value_t tmp
    cdef size_t esize
    cdef size_t n
    cdef char *slot

    if NULL == array:
        return None
    # an empty array legitimately carries no block at all
    if NULL == array[0].array or 0 == array[0].size:
        return {'type': array[0].type, 'array': []}

    pylist = []
    if PMIX_INFO == array[0].type:
        rc = pmix_unload_info(<pmix_info_t*> array[0].array, array[0].size, pylist)
        if PMIX_SUCCESS != rc:
            return None
        return {'type': array[0].type, 'array': pylist}
    if PMIX_VALUE == array[0].type:
        n = 0
        while n < array[0].size:
            d = pmix_unload_value(&(<pmix_value_t*> array[0].array)[n])
            if d is None:
                return None
            pylist.append(d)
            n += 1
        return {'type': array[0].type, 'array': pylist}
    if PMIX_APP == array[0].type:
        pmix_unload_apps(<pmix_app_t*> array[0].array, array[0].size, pylist)
        return {'type': array[0].type, 'array': pylist}

    esize = pmix_darray_esize(array[0].type)
    if 0 == esize:
        print("UNRECOGNIZED DATA TYPE IN ARRAY: "+str(array[0].type))
        return None
    n = 0
    while n < array[0].size:
        # lend the slot to a value and unload that: the value is never
        # destructed, so nothing here takes ownership of the element
        slot = (<char*> array[0].array) + n * esize
        memset(&tmp, 0, sizeof(pmix_value_t))
        tmp.type = array[0].type
        if pmix_type_is_boxed(array[0].type) or \
           PMIX_TOPO == array[0].type or PMIX_PROC_CPUSET == array[0].type:
            tmp.data.ptr = slot
        else:
            memcpy(&tmp.data, slot, esize)
        d = pmix_unload_value(&tmp)
        if d is None:
            return None
        pylist.append(d['value'])
        n += 1
    return {'type': array[0].type, 'array': pylist}


# provide a function for transferring a Python 'value'
# object (a dict with value and val_type as keys)
# to a pmix_value_t.
#
# The value must be zeroed before it is loaded, so that a load which fails
# partway leaves the untouched members NULL rather than stack garbage: the
# caller releases it with pmix_destruct_value whatever the result.
cdef int pmix_load_value(pmix_value_t *value, val):
    try:
        return pmix_load_value_arm(value, val)
    except Exception:
        # a malformed dict - a missing key, a member of the wrong type -
        # is the caller's mistake, and is reported as one
        traceback.print_exc()
        return PMIX_ERR_BAD_PARAM

cdef int pmix_load_value_arm(pmix_value_t *value, val) except? -1:
    if not isinstance(val, dict):
        return PMIX_ERR_BAD_PARAM
    t = val.get('val_type')
    if not pmix_is_int(t) or t < 0 or t > 65535:
        return PMIX_ERR_BAD_PARAM
    v = val.get('value')
    value[0].type = t

    if PMIX_UNDEF == t:
        return PMIX_SUCCESS

    if PMIX_BOOL == t:
        int_bool = pmix_bool_convert(v)
        if int_bool != 0 and int_bool != 1:
            return PMIX_ERR_BAD_PARAM
        value[0].data.flag = int_bool
        return PMIX_SUCCESS

    # the integers, each checked against its own C type
    if PMIX_BYTE == t:
        rc = pmix_check_int(v, 0, 255, "byte")
        if PMIX_SUCCESS == rc:
            value[0].data.byte = v
        return rc
    if PMIX_SIZE == t:
        rc = pmix_check_int(v, 0, pmix_umax(sizeof(size_t)), "size_t")
        if PMIX_SUCCESS == rc:
            value[0].data.size = v
        return rc
    if PMIX_PID == t:
        rc = pmix_check_int(v, 0, pmix_smax(sizeof(pid_t)), "pid")
        if PMIX_SUCCESS == rc:
            value[0].data.pid = v
        return rc
    if PMIX_INT == t:
        rc = pmix_check_int(v, pmix_smin(sizeof(int)), pmix_smax(sizeof(int)), "int")
        if PMIX_SUCCESS == rc:
            value[0].data.integer = v
        return rc
    if PMIX_INT8 == t:
        rc = pmix_check_int(v, -128, 127, "int8")
        if PMIX_SUCCESS == rc:
            value[0].data.int8 = v
        return rc
    if PMIX_INT16 == t:
        rc = pmix_check_int(v, -32768, 32767, "int16")
        if PMIX_SUCCESS == rc:
            value[0].data.int16 = v
        return rc
    if PMIX_INT32 == t:
        rc = pmix_check_int(v, -2147483648, 2147483647, "int32")
        if PMIX_SUCCESS == rc:
            value[0].data.int32 = v
        return rc
    if PMIX_INT64 == t:
        rc = pmix_check_int(v, -9223372036854775808, 9223372036854775807, "int64")
        if PMIX_SUCCESS == rc:
            value[0].data.int64 = v
        return rc
    if PMIX_UINT == t:
        rc = pmix_check_int(v, 0, pmix_umax(sizeof(unsigned int)), "uint")
        if PMIX_SUCCESS == rc:
            value[0].data.uint = v
        return rc
    if PMIX_UINT8 == t:
        rc = pmix_check_int(v, 0, 255, "uint8")
        if PMIX_SUCCESS == rc:
            value[0].data.uint8 = v
        return rc
    if PMIX_UINT16 == t:
        rc = pmix_check_int(v, 0, 65535, "uint16")
        if PMIX_SUCCESS == rc:
            value[0].data.uint16 = v
        return rc
    if PMIX_UINT32 == t:
        rc = pmix_check_int(v, 0, 4294967295, "uint32")
        if PMIX_SUCCESS == rc:
            value[0].data.uint32 = v
        return rc
    if PMIX_UINT64 == t:
        rc = pmix_check_int(v, 0, 18446744073709551615, "uint64")
        if PMIX_SUCCESS == rc:
            value[0].data.uint64 = v
        return rc
    if PMIX_STATUS == t:
        rc = pmix_check_int(v, pmix_smin(sizeof(pmix_status_t)),
                            pmix_smax(sizeof(pmix_status_t)), "status")
        if PMIX_SUCCESS == rc:
            value[0].data.status = v
        return rc
    if PMIX_PROC_RANK == t:
        rc = pmix_check_int(v, 0, 4294967295, "rank")
        if PMIX_SUCCESS == rc:
            value[0].data.rank = v
        return rc
    if PMIX_TIME == t:
        rc = pmix_check_int(v, pmix_smin(sizeof(time_t)), pmix_smax(sizeof(time_t)), "time")
        if PMIX_SUCCESS == rc:
            value[0].data.time = v
        return rc
    if PMIX_PERSIST == t:
        rc = pmix_check_int(v, 0, 255, "persistence")
        if PMIX_SUCCESS == rc:
            value[0].data.persist = v
        return rc
    if PMIX_SCOPE == t:
        rc = pmix_check_int(v, 0, 255, "scope")
        if PMIX_SUCCESS == rc:
            value[0].data.scope = v
        return rc
    if PMIX_DATA_RANGE == t:
        rc = pmix_check_int(v, 0, 255, "range")
        if PMIX_SUCCESS == rc:
            value[0].data.range = v
        return rc
    if PMIX_PROC_STATE == t:
        rc = pmix_check_int(v, 0, 255, "proc state")
        if PMIX_SUCCESS == rc:
            value[0].data.state = v
        return rc
    if PMIX_ALLOC_DIRECTIVE == t:
        rc = pmix_check_int(v, 0, 255, "alloc directive")
        if PMIX_SUCCESS == rc:
            value[0].data.adir = v
        return rc
    if PMIX_RESBLOCK_DIRECTIVE == t:
        rc = pmix_check_int(v, 0, 255, "resource block directive")
        if PMIX_SUCCESS == rc:
            value[0].data.rbdir = v
        return rc
    if PMIX_ALLOC_INHERIT == t:
        rc = pmix_check_int(v, 0, 255, "alloc inheritance")
        if PMIX_SUCCESS == rc:
            value[0].data.inheritance = v
        return rc
    if PMIX_LINK_STATE == t:
        rc = pmix_check_int(v, 0, 255, "link state")
        if PMIX_SUCCESS == rc:
            value[0].data.linkstate = v
        return rc
    if PMIX_JOB_STATE == t:
        rc = pmix_check_int(v, 0, 255, "job state")
        if PMIX_SUCCESS == rc:
            value[0].data.jstate = v
        return rc
    if PMIX_LOCTYPE == t:
        rc = pmix_check_int(v, 0, 65535, "locality")
        if PMIX_SUCCESS == rc:
            value[0].data.locality = v
        return rc
    if PMIX_DEVTYPE == t:
        rc = pmix_check_int(v, 0, 18446744073709551615, "device type")
        if PMIX_SUCCESS == rc:
            value[0].data.devtype = v
        return rc
    if PMIX_INFO_DIRECTIVES == t:
        rc = pmix_check_int(v, 0, 4294967295, "info directives")
        if PMIX_SUCCESS == rc:
            value[0].data.infodirs = v
        return rc
    if PMIX_DATA_TYPE == t:
        rc = pmix_check_int(v, 0, 65535, "data type")
        if PMIX_SUCCESS == rc:
            value[0].data.dtype = v
        return rc
    if PMIX_IOF_CHANNEL == t:
        rc = pmix_check_int(v, 0, 65535, "IOF channel")
        if PMIX_SUCCESS == rc:
            value[0].data.channel = v
        return rc
    # the storage types have no member of their own: the library keeps
    # them in the unsigned member of their width
    if t in (PMIX_STOR_MEDIUM, PMIX_STOR_ACCESS, PMIX_STOR_PERSIST):
        rc = pmix_check_int(v, 0, 18446744073709551615, "storage")
        if PMIX_SUCCESS == rc:
            value[0].data.uint64 = v
        return rc
    if PMIX_STOR_ACCESS_TYPE == t:
        rc = pmix_check_int(v, 0, 65535, "storage access type")
        if PMIX_SUCCESS == rc:
            value[0].data.uint16 = v
        return rc

    if PMIX_FLOAT == t or PMIX_DOUBLE == t:
        # anything float() takes - a number, or a numeric string - but
        # not a bool, which float() would take as 0 or 1
        try:
            if isinstance(v, bool):
                raise TypeError
            fv = float(v)
        except (TypeError, ValueError):
            print("floating point value declared but non-number provided")
            return PMIX_ERR_TYPE_MISMATCH
        if PMIX_FLOAT == t:
            value[0].data.fval = fv
        else:
            value[0].data.dval = fv
        return PMIX_SUCCESS

    if PMIX_TIMEVAL == t:
        rc = pmix_need(v, ('sec', 'usec'))
        if PMIX_SUCCESS != rc:
            return rc
        # whole seconds and microseconds; a float is truncated, as it
        # always was
        sec = v['sec']
        usec = v['usec']
        if isinstance(sec, float):
            sec = int(sec)
        if isinstance(usec, float):
            usec = int(usec)
        rc = pmix_check_int(sec, pmix_smin(sizeof(time_t)),
                            pmix_smax(sizeof(time_t)), "timeval seconds")
        if PMIX_SUCCESS != rc:
            return rc
        rc = pmix_check_int(usec, 0, 999999, "timeval microseconds")
        if PMIX_SUCCESS != rc:
            return rc
        value[0].data.tv.tv_sec = sec
        value[0].data.tv.tv_usec = usec
        return PMIX_SUCCESS

    if PMIX_STRING == t:
        return pmix_load_cstr(&value[0].data.string, v)

    if t in (PMIX_BYTE_OBJECT, PMIX_COMPRESSED_BYTE_OBJECT,
             PMIX_COMPRESSED_STRING):
        # the payload is the authority for how much there is to copy, not
        # a 'size' the caller declared
        return pmix_load_bo(&value[0].data.bo, v)

    # a regex is carried in a byte object; the value may be given either
    # as the raw bytes or as a byte-object dict
    if PMIX_REGEX == t:
        if isinstance(v, dict):
            return pmix_load_regex(&value[0].data.bo, v.get('bytes'))
        return pmix_load_regex(&value[0].data.bo, v)

    if PMIX_ENVAR == t:
        rc = pmix_need(v, ('envar', 'value', 'separator'))
        if PMIX_SUCCESS != rc:
            return rc
        rc = pmix_load_cstr(&value[0].data.envar.envar, v.get('envar'))
        if PMIX_SUCCESS != rc:
            return rc
        rc = pmix_load_cstr(&value[0].data.envar.value, v.get('value'))
        if PMIX_SUCCESS != rc:
            return rc
        sep = v.get('separator')
        if sep is None:
            value[0].data.envar.separator = 0
        elif isinstance(sep, str) and 1 == len(sep) and ord(sep) < 256:
            value[0].data.envar.separator = ord(sep)
        else:
            rc = pmix_check_int(sep, 0, 255, "separator")
            if PMIX_SUCCESS != rc:
                return rc
            value[0].data.envar.separator = sep
        return PMIX_SUCCESS

    if PMIX_TOPO == t or PMIX_PROC_CPUSET == t:
        # an opaque provider object has no Python form, so only its
        # absence - None, which the library packs as "no topology" or
        # "not bound" - can be loaded. A cpuset crosses as its string
        # form instead (section 8a of the bindings guide)
        if v is None:
            value[0].data.ptr = NULL
            return PMIX_SUCCESS
        return PMIX_ERR_NOT_SUPPORTED

    if PMIX_POINTER == t:
        # a pointer into some process's memory
        return PMIX_ERR_NOT_SUPPORTED

    if not pmix_type_is_boxed(t):
        print("UNRECOGNIZED VALUE TYPE: "+str(t))
        return PMIX_ERR_NOT_SUPPORTED

    # everything below points at a struct of its own. None is the NULL
    # the library allows there
    if v is None:
        value[0].data.ptr = NULL
        return PMIX_SUCCESS

    if PMIX_PROC_NSPACE == t:
        rc = pmix_name_ok(v, PMIX_MAX_NSLEN, "nspace")
        if PMIX_SUCCESS != rc:
            return rc
        value[0].data.nspace = <pmix_nspace_t*> pmix_value_alloc(sizeof(pmix_nspace_t))
        if NULL == value[0].data.nspace:
            return PMIX_ERR_NOMEM
        pmix_copy_nspace(value[0].data.nspace[0], v)
        return PMIX_SUCCESS

    if PMIX_PROC == t:
        value[0].data.proc = <pmix_proc_t*> pmix_value_alloc(sizeof(pmix_proc_t))
        if NULL == value[0].data.proc:
            return PMIX_ERR_NOMEM
        return pmix_load_proc(value[0].data.proc, v)

    if PMIX_PROC_INFO == t:
        rc = pmix_need(v, ('proc', 'hostname', 'executable', 'pid', 'exitcode', 'state'))
        if PMIX_SUCCESS != rc:
            return rc
        value[0].data.pinfo = <pmix_proc_info_t*> pmix_value_alloc(sizeof(pmix_proc_info_t))
        if NULL == value[0].data.pinfo:
            return PMIX_ERR_NOMEM
        rc = pmix_load_proc(&value[0].data.pinfo[0].proc, v.get('proc'))
        if PMIX_SUCCESS != rc:
            return rc
        rc = pmix_load_cstr(&value[0].data.pinfo[0].hostname, v.get('hostname'))
        if PMIX_SUCCESS != rc:
            return rc
        rc = pmix_load_cstr(&value[0].data.pinfo[0].executable_name, v.get('executable'))
        if PMIX_SUCCESS != rc:
            return rc
        rc = pmix_check_int(v.get('pid'), 0, pmix_smax(sizeof(pid_t)), "pid")
        if PMIX_SUCCESS != rc:
            return rc
        value[0].data.pinfo[0].pid = v['pid']
        rc = pmix_check_int(v.get('exitcode'), pmix_smin(sizeof(int)), pmix_smax(sizeof(int)), "exit code")
        if PMIX_SUCCESS != rc:
            return rc
        value[0].data.pinfo[0].exit_code = v['exitcode']
        rc = pmix_check_int(v.get('state'), 0, 255, "proc state")
        if PMIX_SUCCESS != rc:
            return rc
        value[0].data.pinfo[0].state = v['state']
        return PMIX_SUCCESS

    if PMIX_DATA_ARRAY == t:
        if not isinstance(v, dict) or 'type' not in v or 'array' not in v:
            return PMIX_ERR_TYPE_MISMATCH
        value[0].data.darray = <pmix_data_array_t*> pmix_value_alloc(sizeof(pmix_data_array_t))
        if NULL == value[0].data.darray:
            return PMIX_ERR_NOMEM
        return pmix_load_darray(value[0].data.darray, v.get('type'), v.get('array'))

    if PMIX_COORD == t:
        value[0].data.coord = <pmix_coord_t*> pmix_value_alloc(sizeof(pmix_coord_t))
        if NULL == value[0].data.coord:
            return PMIX_ERR_NOMEM
        return pmix_load_coord(value[0].data.coord, v)

    if PMIX_GEOMETRY == t:
        rc = pmix_need(v, ('fabric', 'uuid', 'osname'))
        if PMIX_SUCCESS != rc:
            return rc
        value[0].data.geometry = <pmix_geometry_t*> pmix_value_alloc(sizeof(pmix_geometry_t))
        if NULL == value[0].data.geometry:
            return PMIX_ERR_NOMEM
        rc = pmix_check_int(v.get('fabric'), 0, pmix_umax(sizeof(size_t)), "fabric index")
        if PMIX_SUCCESS != rc:
            return rc
        value[0].data.geometry[0].fabric = v['fabric']
        rc = pmix_load_cstr(&value[0].data.geometry[0].uuid, v.get('uuid'))
        if PMIX_SUCCESS != rc:
            return rc
        rc = pmix_load_cstr(&value[0].data.geometry[0].osname, v.get('osname'))
        if PMIX_SUCCESS != rc:
            return rc
        # ncoords must be set as each coordinate is built, not after the
        # loop: the library's destructor walks exactly that many entries
        pycoords = v.get('coords')
        if pycoords is None:
            pycoords = []
        # walk a private copy: see pmix_snapshot
        pycoords = pmix_listcopy(pycoords)
        if pycoords is None:
            return PMIX_ERR_TYPE_MISMATCH
        ncoords = len(pycoords)
        value[0].data.geometry[0].coordinates = <pmix_coord_t*> pmix_darray_alloc(ncoords, sizeof(pmix_coord_t))
        if pmix_darray_nomem(value[0].data.geometry[0].coordinates, ncoords):
            return PMIX_ERR_NOMEM
        n = 0
        for k in pycoords:
            value[0].data.geometry[0].ncoords = n + 1
            rc = pmix_load_coord(&value[0].data.geometry[0].coordinates[n], k)
            if PMIX_SUCCESS != rc:
                return rc
            n = n + 1
        value[0].data.geometry[0].ncoords = ncoords
        return PMIX_SUCCESS

    if PMIX_DEVICE == t:
        rc = pmix_need(v, ('uuid', 'osname', 'type'))
        if PMIX_SUCCESS != rc:
            return rc
        value[0].data.device = <pmix_device_t*> pmix_value_alloc(sizeof(pmix_device_t))
        if NULL == value[0].data.device:
            return PMIX_ERR_NOMEM
        rc = pmix_load_cstr(&value[0].data.device[0].uuid, v.get('uuid'))
        if PMIX_SUCCESS != rc:
            return rc
        rc = pmix_load_cstr(&value[0].data.device[0].osname, v.get('osname'))
        if PMIX_SUCCESS != rc:
            return rc
        rc = pmix_check_int(v.get('type'), 0, 18446744073709551615, "device type")
        if PMIX_SUCCESS != rc:
            return rc
        value[0].data.device[0].type = v['type']
        return PMIX_SUCCESS

    if PMIX_DEVICE_DIST == t:
        # 'type' was not always part of it, so it alone may be left out
        rc = pmix_need(v, ('uuid', 'osname', 'mindist', 'maxdist'))
        if PMIX_SUCCESS != rc:
            return rc
        value[0].data.devdist = <pmix_device_distance_t*> pmix_value_alloc(sizeof(pmix_device_distance_t))
        if NULL == value[0].data.devdist:
            return PMIX_ERR_NOMEM
        return pmix_load_devdist(value[0].data.devdist, v)

    if PMIX_ENDPOINT == t:
        rc = pmix_need(v, ('uuid', 'osname', 'endpt'))
        if PMIX_SUCCESS != rc:
            return rc
        value[0].data.endpoint = <pmix_endpoint_t*> pmix_value_alloc(sizeof(pmix_endpoint_t))
        if NULL == value[0].data.endpoint:
            return PMIX_ERR_NOMEM
        rc = pmix_load_cstr(&value[0].data.endpoint[0].uuid, v.get('uuid'))
        if PMIX_SUCCESS != rc:
            return rc
        rc = pmix_load_cstr(&value[0].data.endpoint[0].osname, v.get('osname'))
        if PMIX_SUCCESS != rc:
            return rc
        return pmix_load_bo(&value[0].data.endpoint[0].endpt, v.get('endpt'))

    if PMIX_DATA_BUFFER == t:
        value[0].data.dbuf = <pmix_data_buffer_t*> pmix_value_alloc(sizeof(pmix_data_buffer_t))
        if NULL == value[0].data.dbuf:
            return PMIX_ERR_NOMEM
        return pmix_load_dbuf(value[0].data.dbuf, v)

    if PMIX_RESOURCE_UNIT == t:
        rc = pmix_need(v, ('type', 'count'))
        if PMIX_SUCCESS != rc:
            return rc
        value[0].data.resunit = <pmix_resource_unit_t*> pmix_value_alloc(sizeof(pmix_resource_unit_t))
        if NULL == value[0].data.resunit:
            return PMIX_ERR_NOMEM
        rc = pmix_check_int(v.get('type'), 0, 18446744073709551615, "device type")
        if PMIX_SUCCESS != rc:
            return rc
        value[0].data.resunit[0].type = v['type']
        rc = pmix_check_int(v.get('count'), 0, pmix_umax(sizeof(size_t)), "count")
        if PMIX_SUCCESS != rc:
            return rc
        value[0].data.resunit[0].count = v['count']
        return PMIX_SUCCESS

    if PMIX_NODE_PID == t:
        rc = pmix_need(v, ('hostname', 'nodeid', 'pid'))
        if PMIX_SUCCESS != rc:
            return rc
        value[0].data.nodepid = <pmix_node_pid_t*> pmix_value_alloc(sizeof(pmix_node_pid_t))
        if NULL == value[0].data.nodepid:
            return PMIX_ERR_NOMEM
        rc = pmix_load_cstr(&value[0].data.nodepid[0].hostname, v.get('hostname'))
        if PMIX_SUCCESS != rc:
            return rc
        rc = pmix_check_int(v.get('nodeid'), 0, 4294967295, "nodeid")
        if PMIX_SUCCESS != rc:
            return rc
        value[0].data.nodepid[0].nodeid = v['nodeid']
        rc = pmix_check_int(v.get('pid'), 0, pmix_smax(sizeof(pid_t)), "pid")
        if PMIX_SUCCESS != rc:
            return rc
        value[0].data.nodepid[0].pid = v['pid']
        return PMIX_SUCCESS

    if PMIX_REGEX2 == t:
        value[0].data.regex2 = <pmix_regex2_t*> pmix_value_alloc(sizeof(pmix_regex2_t))
        if NULL == value[0].data.regex2:
            return PMIX_ERR_NOMEM
        return pmix_load_regex2(value[0].data.regex2, v)

    if PMIX_REGATTR == t:
        return pmix_load_regattr(<pmix_regattr_t**> &value[0].data.ptr, v)

    print("UNRECOGNIZED VALUE TYPE: "+str(t))
    return PMIX_ERR_NOT_SUPPORTED

# A device distance dict {'uuid', 'osname', 'type', 'mindist', 'maxdist'}.
# 'type' was not always part of it, so a dict without one loads as type 0
cdef int pmix_load_devdist(pmix_device_distance_t *d, v):
    rc = pmix_load_cstr(&d[0].uuid, v.get('uuid'))
    if PMIX_SUCCESS != rc:
        return rc
    rc = pmix_load_cstr(&d[0].osname, v.get('osname'))
    if PMIX_SUCCESS != rc:
        return rc
    rc = pmix_check_int(v.get('type', 0), 0, 18446744073709551615, "device type")
    if PMIX_SUCCESS != rc:
        return rc
    d[0].type = v.get('type', 0)
    rc = pmix_check_int(v.get('mindist'), 0, 65535, "mindist")
    if PMIX_SUCCESS != rc:
        return rc
    d[0].mindist = v['mindist']
    rc = pmix_check_int(v.get('maxdist'), 0, 65535, "maxdist")
    if PMIX_SUCCESS != rc:
        return rc
    d[0].maxdist = v['maxdist']
    return PMIX_SUCCESS

cdef dict pmix_unload_devdist(const pmix_device_distance_t *d):
    return {'uuid': pmix_decode_str(d[0].uuid),
            'osname': pmix_decode_str(d[0].osname),
            'type': d[0].type,
            'mindist': d[0].mindist,
            'maxdist': d[0].maxdist}

# An array of device distances, as compute_distances and its non-blocking
# form hand them back
cdef void pmix_unload_devdists(const pmix_device_distance_t *dist, size_t ndist,
                               pylist:list):
    cdef size_t n = 0
    while n < ndist:
        pylist.append(pmix_unload_devdist(&dist[n]))
        n += 1

# Convert a pmix_value_t into its Python dict, or None if it cannot be
# converted. None is deliberately not a PMIX_UNDEF dict: an undefined
# value is a legitimate thing to be handed (a lookup reports a key it did
# not find that way), and the two must not be confused
cdef object pmix_unload_value(const pmix_value_t *value):
    try:
        return pmix_unload_value_arm(value)
    except Exception:
        traceback.print_exc()
        return None

cdef object pmix_unload_value_arm(const pmix_value_t *value):
    cdef size_t nbytes
    t = value[0].type

    if PMIX_UNDEF == t:
        return {'value': None, 'val_type': t}
    if PMIX_BOOL == t:
        return {'value': True if value[0].data.flag else False, 'val_type': t}
    if PMIX_BYTE == t:
        return {'value': value[0].data.byte, 'val_type': t}
    if PMIX_SIZE == t:
        return {'value': value[0].data.size, 'val_type': t}
    if PMIX_PID == t:
        return {'value': value[0].data.pid, 'val_type': t}
    if PMIX_INT == t:
        return {'value': value[0].data.integer, 'val_type': t}
    if PMIX_INT8 == t:
        return {'value': value[0].data.int8, 'val_type': t}
    if PMIX_INT16 == t:
        return {'value': value[0].data.int16, 'val_type': t}
    if PMIX_INT32 == t:
        return {'value': value[0].data.int32, 'val_type': t}
    if PMIX_INT64 == t:
        return {'value': value[0].data.int64, 'val_type': t}
    if PMIX_UINT == t:
        return {'value': value[0].data.uint, 'val_type': t}
    if PMIX_UINT8 == t:
        return {'value': value[0].data.uint8, 'val_type': t}
    if PMIX_UINT16 == t:
        return {'value': value[0].data.uint16, 'val_type': t}
    if PMIX_UINT32 == t:
        return {'value': value[0].data.uint32, 'val_type': t}
    if PMIX_UINT64 == t:
        return {'value': value[0].data.uint64, 'val_type': t}
    if PMIX_FLOAT == t:
        return {'value': value[0].data.fval, 'val_type': t}
    if PMIX_DOUBLE == t:
        return {'value': value[0].data.dval, 'val_type': t}
    if PMIX_TIMEVAL == t:
        return {'value': {'sec': value[0].data.tv.tv_sec, 'usec': value[0].data.tv.tv_usec},
                'val_type': t}
    if PMIX_TIME == t:
        return {'value': value[0].data.time, 'val_type': t}
    if PMIX_STATUS == t:
        return {'value': value[0].data.status, 'val_type': t}
    if PMIX_PROC_RANK == t:
        return {'value': value[0].data.rank, 'val_type': t}
    if PMIX_PERSIST == t:
        return {'value': value[0].data.persist, 'val_type': t}
    if PMIX_SCOPE == t:
        return {'value': value[0].data.scope, 'val_type': t}
    if PMIX_DATA_RANGE == t:
        return {'value': value[0].data.range, 'val_type': t}
    if PMIX_PROC_STATE == t:
        return {'value': value[0].data.state, 'val_type': t}
    if PMIX_ALLOC_DIRECTIVE == t:
        return {'value': value[0].data.adir, 'val_type': t}
    if PMIX_RESBLOCK_DIRECTIVE == t:
        return {'value': value[0].data.rbdir, 'val_type': t}
    if PMIX_ALLOC_INHERIT == t:
        return {'value': value[0].data.inheritance, 'val_type': t}
    if PMIX_LINK_STATE == t:
        return {'value': value[0].data.linkstate, 'val_type': t}
    if PMIX_JOB_STATE == t:
        return {'value': value[0].data.jstate, 'val_type': t}
    if PMIX_LOCTYPE == t:
        return {'value': value[0].data.locality, 'val_type': t}
    if PMIX_DEVTYPE == t:
        return {'value': value[0].data.devtype, 'val_type': t}
    if PMIX_INFO_DIRECTIVES == t:
        return {'value': value[0].data.infodirs, 'val_type': t}
    if PMIX_DATA_TYPE == t:
        return {'value': value[0].data.dtype, 'val_type': t}
    if PMIX_IOF_CHANNEL == t:
        return {'value': value[0].data.channel, 'val_type': t}
    if t in (PMIX_STOR_MEDIUM, PMIX_STOR_ACCESS, PMIX_STOR_PERSIST):
        return {'value': value[0].data.uint64, 'val_type': t}
    if PMIX_STOR_ACCESS_TYPE == t:
        return {'value': value[0].data.uint16, 'val_type': t}

    if PMIX_STRING == t:
        return {'value': pmix_decode_str(value[0].data.string), 'val_type': t}

    if t in (PMIX_BYTE_OBJECT, PMIX_COMPRESSED_BYTE_OBJECT,
             PMIX_COMPRESSED_STRING):
        pybo = {}
        pmix_unload_bo(&value[0].data.bo, pybo)
        return {'value': pybo, 'val_type': t}

    if PMIX_REGEX == t:
        # a regex is carried in a byte object and a compressed one is
        # binary, so it gets the same length-respecting treatment. The
        # NUL that ends one in memory is the library's, not part of the
        # regex - see pmix_load_regex
        if NULL == value[0].data.bo.bytes:
            mybytes = b''
        else:
            mybytes = value[0].data.bo.bytes[:value[0].data.bo.size]
            if mybytes.endswith(b'\0'):
                mybytes = mybytes[:-1]
        return {'value': mybytes, 'val_type': t}

    if PMIX_ENVAR == t:
        # either string may be NULL, and the separator goes back as the
        # one-character string the loader expects rather than its ordinal
        return {'value': {'envar': pmix_decode_str(value[0].data.envar.envar),
                          'value': pmix_decode_str(value[0].data.envar.value),
                          'separator': None if 0 == value[0].data.envar.separator
                                       else chr(<unsigned char> value[0].data.envar.separator)},
                'val_type': t}

    if PMIX_TOPO == t or PMIX_PROC_CPUSET == t:
        # opaque provider objects - see the loader
        return {'value': None, 'val_type': t}

    if not pmix_type_is_boxed(t):
        print("Unload_value: provided type is unknown", t)
        return None

    # everything below points at a struct, and the library allows that
    # pointer to be NULL
    if NULL == value[0].data.ptr:
        return {'value': None, 'val_type': t}

    if PMIX_PROC_NSPACE == t:
        return {'value': pmix_decode_str(value[0].data.nspace[0]), 'val_type': t}
    if PMIX_PROC == t:
        return {'value': pmix_unload_proc(value[0].data.proc), 'val_type': t}
    if PMIX_PROC_INFO == t:
        return {'value': {'proc': pmix_unload_proc(&value[0].data.pinfo[0].proc),
                          'hostname': pmix_decode_str(value[0].data.pinfo[0].hostname),
                          'executable': pmix_decode_str(value[0].data.pinfo[0].executable_name),
                          'pid': value[0].data.pinfo[0].pid,
                          'exitcode': value[0].data.pinfo[0].exit_code,
                          'state': value[0].data.pinfo[0].state},
                'val_type': t}
    if PMIX_DATA_ARRAY == t:
        darray = pmix_unload_darray(value[0].data.darray)
        if darray is None:
            return None
        return {'value': darray, 'val_type': t}
    if PMIX_COORD == t:
        return {'value': pmix_unload_coord(value[0].data.coord), 'val_type': t}
    if PMIX_GEOMETRY == t:
        pylist = []
        if NULL != value[0].data.geometry[0].coordinates:
            for n in range(value[0].data.geometry[0].ncoords):
                pylist.append(pmix_unload_coord(&value[0].data.geometry[0].coordinates[n]))
        return {'value': {'fabric': value[0].data.geometry[0].fabric,
                          'uuid': pmix_decode_str(value[0].data.geometry[0].uuid),
                          'osname': pmix_decode_str(value[0].data.geometry[0].osname),
                          'ncoords': len(pylist), 'coords': pylist},
                'val_type': t}
    if PMIX_DEVICE == t:
        return {'value': {'uuid': pmix_decode_str(value[0].data.device[0].uuid),
                          'osname': pmix_decode_str(value[0].data.device[0].osname),
                          'type': value[0].data.device[0].type},
                'val_type': t}
    if PMIX_DEVICE_DIST == t:
        return {'value': pmix_unload_devdist(value[0].data.devdist), 'val_type': t}
    if PMIX_ENDPOINT == t:
        pybo = {}
        pmix_unload_bo(&value[0].data.endpoint[0].endpt, pybo)
        return {'value': {'uuid': pmix_decode_str(value[0].data.endpoint[0].uuid),
                          'osname': pmix_decode_str(value[0].data.endpoint[0].osname),
                          'endpt': pybo},
                'val_type': t}
    if PMIX_DATA_BUFFER == t:
        pybuf = {}
        pmix_unload_dbuf(value[0].data.dbuf, pybuf)
        return {'value': pybuf, 'val_type': t}
    if PMIX_RESOURCE_UNIT == t:
        return {'value': {'type': value[0].data.resunit[0].type,
                          'count': value[0].data.resunit[0].count},
                'val_type': t}
    if PMIX_NODE_PID == t:
        return {'value': {'hostname': pmix_decode_str(value[0].data.nodepid[0].hostname),
                          'nodeid': value[0].data.nodepid[0].nodeid,
                          'pid': value[0].data.nodepid[0].pid},
                'val_type': t}
    if PMIX_REGEX2 == t:
        return {'value': pmix_unload_regex2(value[0].data.regex2), 'val_type': t}
    if PMIX_REGATTR == t:
        return {'value': pmix_unload_regattr(<pmix_regattr_t*> value[0].data.ptr),
                'val_type': t}

    print("Unload_value: provided type is unknown", t)
    return None

# A registered attribute - what an attribute-support query answers with -
# as {'name': str, 'key': str, 'type': PMIX_*, 'description': [str]}
cdef int pmix_load_regattr(pmix_regattr_t **dest, v):
    cdef pmix_regattr_t *ra
    rc = pmix_need(v, ('name', 'key', 'type', 'description'))
    if PMIX_SUCCESS != rc:
        return rc
    ra = <pmix_regattr_t*> pmix_value_alloc(sizeof(pmix_regattr_t))
    if NULL == ra:
        return PMIX_ERR_NOMEM
    dest[0] = ra
    rc = pmix_load_cstr(&ra[0].name, v['name'])
    if PMIX_SUCCESS != rc:
        return rc
    if v['key'] is not None:
        rc = pmix_name_ok(v['key'], PMIX_MAX_KEYLEN, "key")
        if PMIX_SUCCESS != rc:
            return rc
        pmix_copy_key(ra[0].string, v['key'])
    rc = pmix_check_int(v['type'], 0, 65535, "data type")
    if PMIX_SUCCESS != rc:
        return rc
    ra[0].type = v['type']
    return pmix_build_argv(&ra[0].description, v['description'])

cdef dict pmix_unload_regattr(const pmix_regattr_t *ra):
    desc = []
    if NULL != ra[0].description:
        pmix_unload_argv(ra[0].description, desc)
    return {'name': pmix_decode_str(ra[0].name),
            'key': pmix_decode_str(ra[0].string),
            'type': ra[0].type,
            'description': desc}

# provide conversion programs that translate incoming
# PMIx structures into Python dictionaries, and incoming
# arrays into Python lists of objects

# A boolean as 1 or 0, or PMIX_ERR_BAD_PARAM. A string must say which it
# is - "true"/"false" or "t"/"f", in any case: matching on the first
# letter alone read "tomato" as true
def pmix_bool_convert(f):
    if isinstance(f, bool):
        return 1 if f else 0
    if isinstance(f, str):
        word = f.strip().lower()
        if word in ('true', 't'):
            return 1
        if word in ('false', 'f'):
            return 0
        print("Incorrect boolean value provided")
        return PMIX_ERR_BAD_PARAM
    if isinstance(f, (int, float)):
        if 1 == f:
            return 1
        if 0 == f:
            return 0
        return PMIX_ERR_BAD_PARAM
    print("Unrecognized boolean value type provided")
    return PMIX_ERR_BAD_PARAM

# Tuple of integer types.
#  Python 2.x distinguished int/long, but in
#  Python 3.x dropped long and just uses int or width specific variants.
pmix_int_types = (int,)

# A namespace or a key as the bytes that go into its fixed-size C array,
# or None if it cannot go there whole: not a string, not encodable, longer
# than the array holds, or carrying a NUL. Each of those used to be cut
# short in silence, and a cut name is a different name - two long keys
# that differ only past the limit became the same key
cdef object pmix_name_bytes(obj, size_t maxlen):
    if isinstance(obj, str):
        try:
            pyb = obj.encode('UTF-8')
        except UnicodeError:
            return None
    elif isinstance(obj, (bytes, bytearray)):
        pyb = bytes(obj)
    else:
        return None
    if <size_t> len(pyb) > maxlen or b'\0' in pyb:
        return None
    return pyb

# The status for a name the loaders are about to copy
cdef int pmix_name_ok(obj, size_t maxlen, what):
    if not isinstance(obj, (str, bytes, bytearray)):
        print(what, "must be a string, not", repr(obj))
        return PMIX_ERR_TYPE_MISMATCH
    if pmix_name_bytes(obj, maxlen) is None:
        print(what, repr(obj), "is longer than", maxlen,
              "bytes, carries a NUL, or is not UTF-8")
        return PMIX_ERR_BAD_PARAM
    return PMIX_SUCCESS

# Copy a Python nspace into a pmix_nspace_t, NUL-terminated. One that does
# not fit is refused with ValueError rather than shortened; the loaders
# check with pmix_name_ok first, so for them this cannot raise
cdef int pmix_copy_nspace(pmix_nspace_t nspace, ns) except -1:
    cdef const char *pynsptr
    pyns = pmix_name_bytes(ns, PMIX_MAX_NSLEN)
    if pyns is None:
        raise ValueError("invalid nspace %r" % (ns,))
    pynsptr = <const char *>(pyns)
    memset(nspace, 0, PMIX_MAX_NSLEN+1)
    memcpy(nspace, pynsptr, len(pyns))
    return 0

# The same, for a key
cdef int pmix_copy_key(pmix_key_t key, ky) except -1:
    cdef const char *pykeyptr
    pykey = pmix_name_bytes(ky, PMIX_MAX_KEYLEN)
    if pykey is None:
        raise ValueError("invalid key %r" % (ky,))
    pykeyptr = <const char *>(pykey)
    memset(key, 0, PMIX_MAX_KEYLEN+1)
    memcpy(key, pykeyptr, len(pykey))
    return 0

# Convert a regular expression the library handed back into a Python
# bytearray.
#
# The deprecated PMIx_generate_regex/PMIx_generate_ppn interface answers a
# NUL-terminated string in every case the library actually produces:
# "pmix[...]" from the native generator and "blob:<len>:<payload>" from the
# compressing one, whose payload is base64 and therefore also free of NUL
# bytes. So the whole string is the value, and the length is strlen.
#
# (The version this replaced tried to parse the blob header by indexing a
# bytes object - regex[1] is an int in Python 3, and len() of an int raises
# TypeError - and computed a length from that plus the string's own length,
# which would have read past the allocation had it ever got that far. It
# also called .replace()/.split() and discarded the results.)
cdef bytearray pmix_convert_regex(char *regex):
    cdef size_t length
    if NULL == regex:
        return bytearray(0)
    length = strlen(regex)
    return bytearray(regex[:length])

# The pmix_regex2_t crosses to Python as a dict carrying the three fields
# of the struct:
#
#   {'type': str, 'bytes': bytes, 'len': int}
#
# 'type' names the encoding method (the preg component that produced it -
# e.g. "raw" or "compress"); it is what tells the parser which component
# can decode the blob. 'bytes' is the encoded representation and is NOT
# necessarily NUL-terminated, so it is carried as a Python bytes object
# sliced to 'len' rather than as a string.

# Convert a pmix_regex2_t into its Python dict form
cdef dict pmix_unload_regex2(const pmix_regex2_t *regex):
    d = {}
    if NULL == regex[0].type:
        d['type'] = None
    else:
        d['type'] = pmix_decode_str(regex[0].type)
    if NULL == regex[0].bytes or 0 == regex[0].len:
        d['bytes'] = b''
        d['len']   = 0
    else:
        d['bytes'] = (<char*>regex[0].bytes)[:regex[0].len]
        d['len']   = regex[0].len
    return d

# Build a pmix_regex2_t from the Python dict. The struct is constructed
# first, so the caller can release it with PMIx_Regex2_destruct on every
# path - including a failure that left it partly built
cdef int pmix_load_regex2(pmix_regex2_t *regex, pyregex):
    cdef const char *byteptr

    PMIx_Regex2_construct(regex)
    if not isinstance(pyregex, dict):
        return PMIX_ERR_BAD_PARAM
    # the encoding's name, which must be given. The library leaves it
    # NULL on a regex no component produced, and the unloader hands that
    # back as None, so an explicit None loads as that NULL
    if 'type' not in pyregex:
        return PMIX_ERR_BAD_PARAM
    rc = pmix_load_cstr(&regex[0].type, pyregex['type'])
    if PMIX_SUCCESS != rc:
        return rc
    mybytes = pyregex.get('bytes')
    if mybytes is None:
        return PMIX_ERR_BAD_PARAM
    mybytes = pmix_payload(mybytes)
    if mybytes is None:
        return PMIX_ERR_TYPE_MISMATCH
    # honor an explicitly provided length, but never read past the end of
    # the buffer we were actually handed
    mylen = pyregex.get('len', len(mybytes))
    if not pmix_is_int(mylen) or 0 > mylen or len(mybytes) < mylen:
        mylen = len(mybytes)
    if 0 == mylen:
        regex[0].len = 0
        return PMIX_SUCCESS
    regex[0].bytes = <uint8_t*> malloc(mylen)
    if NULL == regex[0].bytes:
        return PMIX_ERR_NOMEM
    byteptr = <const char *>mybytes
    memcpy(regex[0].bytes, byteptr, mylen)
    regex[0].len = mylen
    return PMIX_SUCCESS

# Release everything a loaded value owns.
#
# pmix_load_value deliberately allocates with malloc (see the bindings
# AGENTS.md) precisely so the result can be handed to the library, which
# means the library's own destructor is the right one to release it. It
# covers every type - the byte objects, envars, procs, proc_infos and data
# arrays that a string-only destruct silently leaked on every put(),
# store_internal() and data_*() call.
#
# The value must be zeroed before it is loaded, so that a load which fails
# partway leaves the untouched members NULL rather than stack garbage.
#
cdef void pmix_destruct_value(pmix_value_t *value):
    PMIx_Value_destruct(value)

# Convert a dictionary of key-value pairs into an
# array of pmix_info_t structs
#
# @array [INPUT]
#        - malloc'd array of pmix_info_t structs
#
# @dicts [INPUT]
#          - a list of dictionaries, where each
#            dictionary has a key, value, and val_type
#            defined as such:
#            [{key:y, value:val, val_type:ty}, … ]
#
#
# The caller allocated 'array' for nmax entries, and 'dicts' must hold
# exactly that many - see pmix_snapshot
cdef int pmix_load_info(pmix_info_t *array, size_t nmax, dicts):
    # zero the array first. This function can fail partway through - an
    # unconvertible value type is enough - and the caller then releases
    # the whole array, so every entry it never reached has to be safe to
    # destruct
    memset(array, 0, nmax * sizeof(pmix_info_t))
    seq = pmix_snapshot(dicts, nmax)
    if seq is None:
        return PMIX_ERR_BAD_PARAM
    try:
        return pmix_load_info_list(array, seq)
    except Exception:
        traceback.print_exc()
        return PMIX_ERR_BAD_PARAM

cdef int pmix_load_info_list(pmix_info_t *array, list dicts) except? -1:
    n = 0
    for d in dicts:
        # an info is a key and a value - a dict missing either, or
        # carrying flags that are not a directive bitmask, is refused
        # rather than raised out of here
        if not isinstance(d, dict) or 'key' not in d or 'val_type' not in d:
            print("Info dict needs a key and a val_type:", d)
            return PMIX_ERR_BAD_PARAM
        rc = pmix_name_ok(d['key'], PMIX_MAX_KEYLEN, "key")
        if PMIX_SUCCESS != rc:
            return rc
        pmix_copy_key(array[n].key, d['key'])
        flags = d.get('flags', 0)
        if flags is None:
            flags = 0
        rc = pmix_check_int(flags, 0, 4294967295, "info flags")
        if PMIX_SUCCESS != rc:
            return rc
        # PMIX_INFO_PERSISTENT says "the library must not release this
        # value" - never true of a value the bindings built, which is
        # always a copy of its own, and honored it would leak that copy
        # wherever the library takes ownership of the array
        array[n].flags = flags & ~PMIX_INFO_PERSISTENT
        val = {'value': d.get('value'), 'val_type': d['val_type']}
        rc = pmix_load_value(&array[n].value, val)
        if PMIX_SUCCESS != rc:
            return rc
        n += 1
    return PMIX_SUCCESS

# Allocate memory and load pmix info structs
#
# @array [INPUT]
#          - array of pmix_info_t structs
#
# @ninfo [INPUT]
#          - length of the list of dictionaries
#
# @dicts [INPUT]
#          - a list of dictionaries, where each
#            dictionary has a key, value, and val_type
#            defined as such:
#            [{key:y, value:val, val_type:ty}, … ]
#
# Note: 'dicts' is deliberately left unannotated. Cython treats a
# parameter annotation as a type declaration that rejects None, and
# callers are explicitly allowed to pass None to mean "no attributes" -
# the branch below depends on it
cdef int pmix_alloc_info(pmix_info_t **info_ptr, size_t *ninfo, dicts):
    # Start from an empty array, and leave it that way on every failure
    # path. Callers routinely pass the result straight into a PMIx call,
    # and a failure that left the freed pointer and its old count in place
    # handed the library memory it had just released
    cdef size_t n
    info_ptr[0] = NULL
    ninfo[0] = 0
    # Convert any provided dictionary to an array of pmix_info_t
    if dicts is None:
        return PMIX_SUCCESS
    dicts = pmix_listcopy(dicts)
    if dicts is None:
        return PMIX_ERR_BAD_PARAM
    n = len(dicts)
    if 0 == n:
        return PMIX_SUCCESS
    info_ptr[0] = <pmix_info_t*>malloc(n * sizeof(pmix_info_t))
    if not info_ptr[0]:
        return PMIX_ERR_NOMEM
    rc = pmix_load_info(info_ptr[0], n, dicts)
    if PMIX_SUCCESS != rc:
        pmix_free_info(info_ptr[0], n)
        info_ptr[0] = NULL
        return rc
    ninfo[0] = n
    return PMIX_SUCCESS

cdef int pmix_unload_info(const pmix_info_t *info, size_t ninfo, ilist:list):
    cdef char* kystr
    cdef size_t n
    n = 0
    while n < ninfo:
        # pmix_unload_value returns a python dict of val, val_type - or
        # None for a value it cannot convert. PMIX_UNDEF is a value like
        # any other (a flag carried by its key alone)
        val = pmix_unload_value(&info[n].value)
        if val is None:
            return PMIX_ERR_NOT_SUPPORTED
        d     = {}
        d['key']      = pmix_decode_str(info[n].key)
        # TODO: don't know how to decide if flags was defined or not??
        d['flags']    = info[n].flags
        d['value']    = val['value']
        d['val_type'] = val['val_type']
        ilist.append(d)
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
    free(array)


# Convert array of resource units into a python list of
# dictionaries
cdef int pmix_unload_units(const pmix_resource_unit_t *units, size_t nunits, ilist:list):
    cdef size_t n
    n = 0
    while n < nunits:
        d     = {}
        d['type']      = units[n].type
        d['count']    = units[n].count
        ilist.append(d)
        n += 1
    return PMIX_SUCCESS


# Convert a python list of resource unit dicts into an array of
# pmix_resource_unit_t structs
#
# @array [INPUT]
#        - allocated array of pmix_resource_unit_t structs
#
# @units [INPUT]
#        - a list of dictionaries, each with a 'type' (a PMIX_DEVTYPE_
#          value) and a 'count'
#
# 'array' was allocated for nmax units, and 'units' must hold that many
cdef int pmix_load_units(pmix_resource_unit_t *array, size_t nmax, units):
    seq = pmix_snapshot(units, nmax)
    if seq is None:
        return PMIX_ERR_BAD_PARAM
    try:
        n = 0
        for d in seq:
            if not isinstance(d, dict):
                return PMIX_ERR_BAD_PARAM
            # both members, as for a unit given as a value: a misspelt
            # 'count' used to ask for zero of the type and succeed
            rc = pmix_need(d, ('type', 'count'))
            if PMIX_SUCCESS != rc:
                return rc
            mytype = d['type']
            mycount = d['count']
            rc = pmix_check_int(mytype, 0, 18446744073709551615, "device type")
            if PMIX_SUCCESS != rc:
                return rc
            rc = pmix_check_int(mycount, 0, pmix_umax(sizeof(size_t)), "count")
            if PMIX_SUCCESS != rc:
                return rc
            array[n].type = mytype
            array[n].count = mycount
            n += 1
    except Exception:
        traceback.print_exc()
        return PMIX_ERR_BAD_PARAM
    return PMIX_SUCCESS

# Allocate and load an array of pmix_resource_unit_t from a python list -
# the resource-unit counterpart of pmix_alloc_info. A None or empty list
# yields a NULL array of length zero, which the APIs accept
cdef int pmix_alloc_units(pmix_resource_unit_t **units_ptr, size_t *nunits, units):
    cdef size_t n
    nunits[0] = 0
    units_ptr[0] = NULL
    if units is None:
        return PMIX_SUCCESS
    units = pmix_listcopy(units)
    if units is None:
        return PMIX_ERR_BAD_PARAM
    n = len(units)
    if 0 == n:
        return PMIX_SUCCESS
    units_ptr[0] = <pmix_resource_unit_t*> PyMem_Malloc(n * sizeof(pmix_resource_unit_t))
    if not units_ptr[0]:
        return PMIX_ERR_NOMEM
    rc = pmix_load_units(units_ptr[0], n, units)
    if PMIX_SUCCESS != rc:
        pmix_free_units(units_ptr[0], n)
        units_ptr[0] = NULL
        return rc
    nunits[0] = n
    return PMIX_SUCCESS

# Free an array of pmix_resource_unit_t. The struct carries no allocated
# members, so the array itself is all there is to release - and as the
# bindings both allocate and release it (the APIs that take one do not
# retain it), it stays on the Python allocator
cdef void pmix_free_units(pmix_resource_unit_t *array, size_t sz):
    PyMem_Free(array)


# Convert list of python pmix_pdata_t dicts into an
# array of pmix_pdata_t structs
#
# @array [INPUT]
#        - malloc'd array of pmix_pdata_t structs
#
# @pdata [INPUT]
#          - a list of dictionaries, where each
#            dictionary has a key, value, val_type,
#            and proc keys
cdef int pmix_load_pdata(pmix_pdata_t *array, size_t nmax, data):
    # zero first, for the same reason pmix_load_info does: this can fail
    # partway through and the caller then releases the whole array
    memset(array, 0, nmax * sizeof(pmix_pdata_t))
    seq = pmix_snapshot(data, nmax)
    if seq is None:
        return PMIX_ERR_BAD_PARAM
    try:
        return pmix_load_pdata_list(array, seq)
    except Exception:
        traceback.print_exc()
        return PMIX_ERR_BAD_PARAM

cdef int pmix_load_pdata_list(pmix_pdata_t *array, list data) except? -1:
    n = 0
    for d in data:
        rc = pmix_need(d, ('key', 'value', 'val_type', 'proc'))
        if PMIX_SUCCESS != rc:
            return rc
        rc = pmix_name_ok(d['key'], PMIX_MAX_KEYLEN, "key")
        if PMIX_SUCCESS != rc:
            return rc
        pmix_copy_key(array[n].key, d['key'])
        rc = pmix_load_proc(&array[n].proc, d['proc'])
        if PMIX_SUCCESS != rc:
            return rc
        val = {'value':d['value'], 'val_type':d['val_type']}
        rc = pmix_load_value(&array[n].value, val)
        if PMIX_SUCCESS != rc:
            return rc
        n += 1
    return PMIX_SUCCESS

cdef int pmix_unload_pdata(const pmix_pdata_t *pdata, size_t npdata, ilist:list):
    cdef char* kystr
    cdef size_t n = 0
    while n < npdata:
        # a key the lookup did not find comes back PMIX_UNDEF, and is
        # reported that way rather than ending the conversion
        val = pmix_unload_value(&pdata[n].value)
        if val is None:
            return PMIX_ERR_NOT_SUPPORTED
        d     = {}
        d['key']      = pmix_decode_str(pdata[n].key)
        d['proc']     = pmix_unload_proc(&pdata[n].proc)
        d['value']    = val['value']
        d['val_type'] = val['val_type']
        ilist.append(d)
        n += 1
    return PMIX_SUCCESS

cdef void pmix_destruct_pdata(pmix_pdata_t *pdata):
    pmix_destruct_value(&pdata[0].value)

# Free a malloc'd array of pmix_pdata_t structures
#
# @array [INPUT]
#        - array of pmix_pdata_t to be free'd
#
# @sz [INPUT]
#     - number of elements in array
cdef void pmix_free_pdata(pmix_pdata_t *array, size_t sz):
    n = 0
    while n < sz:
        pmix_destruct_pdata(&array[n])
        n += 1
    PyMem_Free(array)

cdef int pmix_unload_queries(const pmix_query_t *queries, size_t nqueries, ilist:list):
    cdef char* kystr
    cdef size_t n = 0
    while n < nqueries:
        keylist = []
        qualist = []
        query = {}
        if NULL != queries[n].keys:
            pmix_unload_argv(queries[n].keys, keylist)
        if NULL != queries[n].qualifiers:
            pmix_unload_info(queries[n].qualifiers, queries[n].nqual, qualist)
        query['keys']       = keylist
        query['qualifiers'] = qualist
        ilist.append(query)
        n += 1
    return PMIX_SUCCESS


# Free a malloc'd array of pmix_query_t structs to free
#
# @array [INPUT]
#        - pmix_query_t queries to be free'd
#
# @sz [INPUT]
#     - number of elements in array
cdef void pmix_free_queries(pmix_query_t *queries, size_t sz):
    n = 0
    while n < sz:
        if queries[n].keys != NULL:
            pmix_free_argv(queries[n].keys)
        if queries[n].qualifiers != NULL:
            pmix_free_info(queries[n].qualifiers, queries[n].nqual)
        n += 1
    if queries != NULL:
        PyMem_Free(queries)

# Convert a list of (nspace, rank) tuples into an
# array of pmix_proc_t structs
#
# @proc [INPUT]
#       - malloc'd array of pmix_proc_t structs
#
# @peers [INPUT]
#       - list of (nspace,rank) tuples
#
# Every loader that walks a Python list takes a list or a tuple, and
# reports anything else - or anything it cannot convert - as a status. A
# loader that raised instead left its caller holding the array it had
# allocated for the result, which the caller's failure path never got to
# release
#
# 'proc' was allocated for nmax procs, and 'peers' must hold that many
cdef int pmix_load_procs(pmix_proc_t *proc, size_t nmax, peers):
    seq = pmix_snapshot(peers, nmax)
    if seq is None:
        return PMIX_ERR_BAD_PARAM
    try:
        n = 0
        for p in seq:
            rc = pmix_load_proc(&proc[n], p)
            if PMIX_SUCCESS != rc:
                return rc
            n += 1
    except Exception:
        traceback.print_exc()
        return PMIX_ERR_BAD_PARAM
    return PMIX_SUCCESS

cdef int pmix_unload_procs(const pmix_proc_t *procs, size_t nprocs, peers:list):
    cdef char* kystr
    n = 0
    while n < nprocs:
        peers.append(pmix_unload_proc(&procs[n]))
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

# Convert a raw byte buffer into a list of integers suitable for
# bytearray(). Read it through an unsigned pointer: 'char' is signed on
# most platforms, so any byte at or above 0x80 would otherwise arrive as
# a negative number and bytearray() would reject the whole list with
# "byte must be in range(0, 256)". Every payload that is not plain ASCII
# hits this - credentials, IOF data, a packed buffer
cdef void pmix_unload_bytes(char *data, size_t ndata, blist:list):
    cdef size_t n = 0
    cdef unsigned char *udata = <unsigned char*>data
    while n < ndata:
        blist.append(udata[n])
        n += 1

# Convert a Python byte-object dict, {'bytes': b'...', 'size': n}, into a
# pmix_byte_object_t. The payload is malloc'd, so it can be handed to a
# library call that takes ownership of it - callers that retain ownership
# must free it themselves
cdef int pmix_load_bo(pmix_byte_object_t *bo, pybo):
    cdef const char *ptr

    bo[0].bytes = NULL
    bo[0].size = 0
    if pybo is None:
        return PMIX_SUCCESS
    if not isinstance(pybo, dict):
        return PMIX_ERR_BAD_PARAM
    pybytes = pybo.get('bytes')
    if pybytes is None:
        return PMIX_SUCCESS
    pybytes = pmix_payload(pybytes)
    if pybytes is None:
        return PMIX_ERR_TYPE_MISMATCH
    if 0 == len(pybytes):
        return PMIX_SUCCESS
    bo[0].size = len(pybytes)
    bo[0].bytes = <char*> malloc(bo[0].size)
    if NULL == bo[0].bytes:
        bo[0].size = 0
        return PMIX_ERR_NOMEM
    ptr = <const char*>pybytes
    memcpy(bo[0].bytes, ptr, bo[0].size)
    return PMIX_SUCCESS

# Load a PMIX_REGEX. The library keeps one in a byte object, but reads it
# as a C string: every consumer - pack, copy, parse - finds its end with
# strlen, or reads its length out of the framing of the encoded form. The
# library's own loader therefore stores the terminating NUL with it, and
# a payload stored without one was read past the end of its allocation
# by the first pack. So a NUL always follows the payload here, and the
# unloader takes it off again; an encoded regex carries its own length,
# and a NUL after it changes nothing
cdef int pmix_load_regex(pmix_byte_object_t *bo, pyregex):
    cdef const char *ptr
    cdef size_t sz

    bo[0].bytes = NULL
    bo[0].size = 0
    if pyregex is None:
        return PMIX_SUCCESS
    pybytes = pmix_payload(pyregex)
    if pybytes is None:
        return PMIX_ERR_TYPE_MISMATCH
    sz = len(pybytes)
    bo[0].bytes = <char*> malloc(sz + 1)
    if NULL == bo[0].bytes:
        return PMIX_ERR_NOMEM
    ptr = <const char*>pybytes
    memcpy(bo[0].bytes, ptr, sz)
    bo[0].bytes[sz] = 0
    bo[0].size = sz + 1
    return PMIX_SUCCESS

# Convert a pmix_byte_object_t into the Python dict, filling in pybo
cdef void pmix_unload_bo(const pmix_byte_object_t *bo, pybo:dict):
    blist = []
    if NULL != bo[0].bytes and 0 < bo[0].size:
        pmix_unload_bytes(bo[0].bytes, bo[0].size, blist)
    pybo['bytes'] = bytes(bytearray(blist))
    pybo['size'] = len(pybo['bytes'])

# A pmix_data_buffer_t holds three raw pointers into one allocation plus
# two sizes. Only two of those five fields mean anything on the Python
# side - the payload itself, and how far the unpack cursor has advanced
# through it - so a Python data buffer is the dict
#
#     {'bytes': b'...', 'bytes_used': n, 'bytes_unpacked': m}
#
# and the pointers are rebuilt from those offsets on every crossing.
# 'bytes_used' is redundant with len(bytes) and is carried only because
# the struct has it; the loader trusts the payload, not the count.
#
# Note that the library's own PMIx_Data_buffer_load cannot be used here:
# it routes through PMIx_Data_load, which refuses to run before
# PMIx_Init and discards its status, so a pre-init caller would silently
# get an empty buffer
cdef int pmix_load_dbuf(pmix_data_buffer_t *buf, pybuf):
    cdef const char *ptr
    cdef size_t sz
    cdef size_t unpacked

    PMIx_Data_buffer_construct(buf)
    if pybuf is None:
        return PMIX_SUCCESS
    if not isinstance(pybuf, dict):
        return PMIX_ERR_BAD_PARAM
    pybytes = pybuf.get('bytes')
    if pybytes is None:
        return PMIX_SUCCESS
    pybytes = pmix_payload(pybytes)
    if pybytes is None:
        return PMIX_ERR_TYPE_MISMATCH
    # the unpack cursor, so successive unpacks resume where the last one
    # stopped. It is checked before anything is allocated
    pyunpacked = pybuf.get('bytes_unpacked')
    if pyunpacked is None:
        pyunpacked = 0
    rc = pmix_check_int(pyunpacked, 0, len(pybytes), "bytes_unpacked")
    if PMIX_SUCCESS != rc:
        return rc
    unpacked = pyunpacked
    sz = len(pybytes)
    if 0 == sz:
        return PMIX_SUCCESS
    # the buffer owns its payload and releases it with the C allocator
    buf[0].base_ptr = <char*> malloc(sz)
    if NULL == buf[0].base_ptr:
        return PMIX_ERR_NOMEM
    ptr = <const char*>pybytes
    memcpy(buf[0].base_ptr, ptr, sz)
    buf[0].bytes_allocated = sz
    buf[0].bytes_used = sz
    buf[0].pack_ptr = buf[0].base_ptr + sz
    buf[0].unpack_ptr = buf[0].base_ptr + unpacked
    return PMIX_SUCCESS

# Render a pmix_data_buffer_t back into the Python dict, filling in pybuf
cdef void pmix_unload_dbuf(const pmix_data_buffer_t *buf, pybuf:dict):
    blist = []
    if NULL != buf[0].base_ptr and 0 < buf[0].bytes_used:
        pmix_unload_bytes(buf[0].base_ptr, buf[0].bytes_used, blist)
    pybuf['bytes'] = bytes(bytearray(blist))
    pybuf['bytes_used'] = len(pybuf['bytes'])
    if NULL != buf[0].base_ptr and NULL != buf[0].unpack_ptr:
        pybuf['bytes_unpacked'] = buf[0].unpack_ptr - buf[0].base_ptr
    else:
        pybuf['bytes_unpacked'] = 0

# Release a data buffer built by pmix_load_dbuf. The library's destructor
# frees the payload and tolerates a buffer that was never loaded
cdef void pmix_destruct_dbuf(pmix_data_buffer_t *buf):
    PMIx_Data_buffer_destruct(buf)

# Release the allocated members of a single app struct, leaving the
# struct itself alone - the counterpart of pmix_destruct_info, and the
# right thing to call on an app that lives on the stack rather than in a
# heap array
cdef void pmix_destruct_app(pmix_app_t *app):
    cdef size_t n
    if NULL != app[0].cmd:
        free(app[0].cmd)
        app[0].cmd = NULL
    if NULL != app[0].argv:
        n = 0
        while NULL != app[0].argv[n]:
            free(app[0].argv[n])
            n += 1
        free(app[0].argv)
        app[0].argv = NULL
    if NULL != app[0].env:
        n = 0
        while NULL != app[0].env[n]:
            free(app[0].env[n])
            n += 1
        free(app[0].env)
        app[0].env = NULL
    if NULL != app[0].cwd:
        free(app[0].cwd)
        app[0].cwd = NULL
    if NULL != app[0].info and 0 < app[0].ninfo:
        pmix_free_info(app[0].info, app[0].ninfo)
        app[0].info = NULL
        app[0].ninfo = 0

cdef void pmix_free_apps(pmix_app_t *array, size_t sz):
    if NULL == array:
        return
    n = 0
    while n < sz:
        free(array[n].cmd)
        pmix_free_argv(array[n].argv)
        pmix_free_argv(array[n].env)
        free(array[n].cwd)
        if 0 < array[n].ninfo:
            pmix_free_info(array[n].info, array[n].ninfo)
        n += 1
    PyMem_Free(array)

# Render an array of pmix_app_t into the list of app dicts that spawn()
# takes, so a server's 'spawn' handler sees the apps in exactly the shape
# a Python caller would have built them
cdef void pmix_unload_apps(const pmix_app_t *apps, size_t napps, pyapps:list):
    cdef size_t n = 0
    if NULL == apps:
        return
    while n < napps:
        myapp = {}
        # decode - str() of a C string yields the repr of a bytes object,
        # "b'/bin/true'", which is not a command anything can run
        myapp['cmd'] = pmix_decode_str(apps[n].cmd)
        myargv = []
        if NULL != apps[n].argv:
            pmix_unload_argv(apps[n].argv, myargv)
        myapp['argv'] = myargv
        myenv = []
        if NULL != apps[n].env:
            pmix_unload_argv(apps[n].env, myenv)
        myapp['env'] = myenv
        myapp['cwd'] = pmix_decode_str(apps[n].cwd)
        myapp['maxprocs'] = apps[n].maxprocs
        # the info array unloads into a LIST of info dicts - handing
        # pmix_unload_info a dict raised AttributeError on its first append
        # and took the whole upcall with it
        keyvals = []
        if NULL != apps[n].info and 0 < apps[n].ninfo:
            pmix_unload_info(apps[n].info, apps[n].ninfo, keyvals)
        myapp['info'] = keyvals
        pyapps.append(myapp)
        n += 1

# Build a NULL-terminated argv from a Python list of strings. None or an
# empty list leaves it NULL
cdef int pmix_build_argv(char ***dest, pylist):
    cdef size_t m
    cdef char **argv
    dest[0] = NULL
    if pylist is None:
        return PMIX_SUCCESS
    # walk a private copy: see pmix_snapshot
    pylist = pmix_listcopy(pylist)
    if pylist is None:
        return PMIX_ERR_TYPE_MISMATCH
    if 0 == len(pylist):
        return PMIX_SUCCESS
    m = len(pylist) + 1
    argv = <char**> malloc(m * sizeof(char*))
    if NULL == argv:
        return PMIX_ERR_NOMEM
    memset(argv, 0, m * sizeof(char*))
    # hand the array over before filling it, so a failure partway leaves
    # a shorter, well-formed argv for the caller to release
    dest[0] = argv
    n = 0
    for a in pylist:
        if a is None:
            return PMIX_ERR_BAD_PARAM
        rc = pmix_load_cstr(&argv[n], a)
        if PMIX_SUCCESS != rc:
            return rc
        n += 1
    return PMIX_SUCCESS

# Load a list of app dicts into a zeroed array of pmix_app_t.
#
#   {'cmd': str, 'argv': [str], 'env': [str], 'cwd': str,
#    'maxprocs': int, 'info': [info dicts]}
#
# Only 'cmd' (or a non-empty 'argv') is required. A missing argv is the
# command alone, a missing cwd is the current directory, and a missing
# maxprocs is 1. Anything that IS given but cannot be converted is an
# error: a spawn that quietly drops an argument, an environment entry or
# a directive starts something other than what was asked for.
#
# 'apps' was allocated for nmax apps, and 'pyapps' must hold that many
cdef int pmix_load_apps(pmix_app_t *apps, size_t nmax, pyapps):
    # zero the array first - this function can fail partway through, and
    # the caller will then hand the whole array to pmix_free_apps
    memset(apps, 0, nmax * sizeof(pmix_app_t))
    seq = pmix_snapshot(pyapps, nmax)
    if seq is None:
        return PMIX_ERR_BAD_PARAM
    try:
        return pmix_load_apps_list(apps, seq)
    except Exception:
        traceback.print_exc()
        return PMIX_ERR_BAD_PARAM

cdef int pmix_load_apps_list(pmix_app_t *apps, list pyapps) except? -1:
    cdef size_t ninfo
    n = 0
    for p in pyapps:
        if not isinstance(p, dict):
            return PMIX_ERR_BAD_PARAM
        rc = pmix_load_cstr(&apps[n].cmd, p.get('cmd'))
        if PMIX_SUCCESS != rc:
            return rc
        pyargv = p.get('argv')
        if pyargv is None or 0 == len(pyargv):
            if p.get('cmd') is None:
                print("An app needs a cmd or an argv")
                return PMIX_ERR_BAD_PARAM
            pyargv = [p['cmd']]
        rc = pmix_build_argv(&apps[n].argv, pyargv)
        if PMIX_SUCCESS != rc:
            return rc
        rc = pmix_build_argv(&apps[n].env, p.get('env'))
        if PMIX_SUCCESS != rc:
            return rc
        mycwd = p.get('cwd')
        if mycwd is None:
            # no cwd given - the process should start where we are
            mycwd = os.getcwd()
        rc = pmix_load_cstr(&apps[n].cwd, mycwd)
        if PMIX_SUCCESS != rc:
            return rc
        mymax = p.get('maxprocs', 1)
        rc = pmix_check_int(mymax, 0, pmix_smax(sizeof(int)), "maxprocs")
        if PMIX_SUCCESS != rc:
            return rc
        apps[n].maxprocs = mymax
        # an empty list must leave the array NULL: a non-NULL info with
        # ninfo == 0 tells the library the array is terminated by an
        # "end" marker, and it will walk off the allocation looking for
        # one. The count is set only once the array exists, so a failed
        # allocation leaves nothing for pmix_free_apps to walk
        pyinfo = p.get('info')
        if pyinfo is not None:
            pyinfo = pmix_listcopy(pyinfo)
            if pyinfo is None:
                return PMIX_ERR_BAD_PARAM
        if pyinfo is not None and 0 < len(pyinfo):
            ninfo = len(pyinfo)
            apps[n].info = <pmix_info_t*> malloc(ninfo * sizeof(pmix_info_t))
            if NULL == apps[n].info:
                return PMIX_ERR_NOMEM
            apps[n].ninfo = ninfo
            rc = pmix_load_info(apps[n].info, ninfo, pyinfo)
            if PMIX_SUCCESS != rc:
                return rc
        n += 1
    return PMIX_SUCCESS
