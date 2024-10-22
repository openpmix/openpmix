#file: pmix.pyx
#
# Copyright (c) 2022      Nanook Consulting. All rights reserved

from libc.string cimport memset, strncpy, strcpy, strlen, strdup

from libc.stdlib cimport malloc, free
from libc.string cimport memcpy
from libc.stdio cimport printf
from ctypes import addressof, c_int
from cython.operator import address
import signal, time, sys
import threading, ctypes
import queue
import array
import os
from typing import Callable
from threading import Timer

# pull in all the constant definitions - we
# store them in a separate file for neatness
include "pmix_constants.pxi"
include "pmix.pxi"

active = myLock()
myhdlrs = []
myname = {}


cdef struct icbd: 
    pmix_info_t *info
    size_t ninfo
ctypedef icbd pypmix_info_cbdata_t

cdef struct mcbd: 
    char *data
    size_t size
ctypedef mcbd pypmix_modex_cbdata_t

# Function to release pypmix_info_cbdata_t structure
cdef void op_release(pmix_status_t status, void *cbdata) noexcept nogil:
    if NULL == cbdata:
        return
    cdef pypmix_info_cbdata_t *icbd = <pypmix_info_cbdata_t *> cbdata
    if NULL != icbd.info and icbd.ninfo > 0:
        PMIx_Info_free(icbd.info, icbd.ninfo)
    free(icbd)

# Function to release pypmix_info_cbdata_t structure
cdef void info_release(void *cbdata) noexcept nogil:
    if NULL == cbdata:
        return
    cdef pypmix_info_cbdata_t *icbd = <pypmix_info_cbdata_t *> cbdata
    if NULL != icbd.info and icbd.ninfo > 0:
        PMIx_Info_free(icbd.info, icbd.ninfo)
    free(icbd)

# Function to release pypmix_modex_cbdata_t structure
cdef void modex_release(void *cbdata) noexcept nogil:
    if NULL == cbdata:
        return
    cdef pypmix_modex_cbdata_t *mcbd = <pypmix_modex_cbdata_t *> cbdata
    if NULL != mcbd.data and mcbd.size > 0:
        free(mcbd.data)
    free(mcbd)



###########################
# Server Callback Functions
###########################
pypmix_op_cbfunc_t              =   Callable[[int, dict], int]
pypmix_info_cbfunc_t            =   Callable[[int, list, dict], int]
pypmix_modex_cbfunc_t           =   Callable[[int, bytearray, dict], int]
pypmix_lookup_cbfunc_t          =   Callable[[int, list, dict], int]
pypmix_spawn_cbfunc_t           =   Callable[[int, str, dict], int]
pypmix_tool_connection_cbfunc_t =   Callable[[int, dict, dict], None]
pypmix_credential_cbfunc_t      =   Callable[[int, dict, list, dict], None]
pypmix_validation_cbfunc_t      =   Callable[[int, list], None]


def pypmix_op_cbfunc(int rc, dict cbdata_dict):
    cdef pmix_op_cbfunc_t cbfunc
    cdef void* cbdata
    cbfunc = <pmix_op_cbfunc_t> <uintptr_t> cbdata_dict['cbfunc']
    cbdata = <void*> <uintptr_t> cbdata_dict['cbdata']
    
    if NULL == cbfunc:
        return

    with nogil:
        cbfunc(rc, cbdata)

def pypmix_info_cbfunc(int rc, list refarginfo, dict cbdata_dict):
    cdef pmix_info_cbfunc_t cbfunc = <pmix_info_cbfunc_t> <uintptr_t> cbdata_dict['cbfunc']
    cdef void* cbdata = <void*> <uintptr_t> cbdata_dict['cbdata']
    cdef pmix_info_t *info
    cdef pmix_info_t **info_ptr
    cdef pypmix_info_cbdata_t *icbd
    cdef size_t ninfo = 0
    info              = NULL
    info_ptr          = &info

    if NULL == cbfunc:
        return

    prc = pmix_alloc_info(info_ptr, &ninfo, refarginfo)
    if PMIX_SUCCESS != prc:
        print("Error transferring info to C:", prc)
        return

    icbd = <pypmix_info_cbdata_t *> malloc(sizeof(pypmix_info_cbdata_t))
    if icbd == NULL:
        print("Error allocating pypmix_info_cbdata_t")
        return
    icbd.info = info
    icbd.ninfo = ninfo

    with nogil:
        cbfunc(rc, info, ninfo, cbdata, info_release, <void*> icbd) 

def pypmix_modex_cbfunc(int status, bytearray ret_data, dict cbdata_dict):    
    cdef char *data
    cdef size_t size = len(ret_data)
    cdef pmix_modex_cbfunc_t cbfunc = <pmix_modex_cbfunc_t> <uintptr_t> cbdata_dict['cbfunc']
    cdef void* cbdata = <void*> <uintptr_t> cbdata_dict['cbdata']

    if NULL == cbfunc:
        return

    data = strdup(ret_data)
    mcbd = <pypmix_modex_cbdata_t *> malloc(sizeof(pypmix_modex_cbdata_t))
    if mcbd == NULL:
        print("Error allocating pypmix_modex_cbdata_t")
        return
    mcbd.data = data
    mcbd.size = size

    with nogil:
        cbfunc(status, data, size, cbdata, modex_release, mcbd)   

def pypmix_lookup_cbfunc(int rc, list pdata, dict cbdata_dict):
    cdef pmix_lookup_cbfunc_t cbfunc = <pmix_lookup_cbfunc_t> <uintptr_t> cbdata_dict['cbfunc']
    cdef void* cbdata = <void*> <uintptr_t> cbdata_dict['cbdata']
    cdef pmix_pdata_t *pd;
    cdef size_t ndata = 0;
    cdef pypmix_modex_cbdata_t *pcbd = NULL

    if NULL == cbfunc:
        return

    if pdata is not None:
        ndata = len(pdata)
        if 0 < ndata:
            pd = <pmix_pdata_t*> malloc(ndata * sizeof(pmix_pdata_t))
            if not pdata:
                return
            prc = pmix_load_pdata(pd, pdata)
            if PMIX_SUCCESS != prc:
                pmix_free_pdata(pd, ndata)
                return
        else:
            pd = NULL
    else:
        pd = NULL
    
    with nogil:
        cbfunc(rc, pd, ndata, cbdata)

    PyMem_Free(pd)

def pypmix_spawn_cbfunc(int rc, str nspace, dict cbdata_dict):
    cdef pmix_spawn_cbfunc_t cbfunc = <pmix_spawn_cbfunc_t> <uintptr_t> cbdata_dict['cbfunc']
    cdef void* cbdata = <void*> <uintptr_t> cbdata_dict['cbdata']
    cdef pmix_nspace_t c_nspace

    if NULL == cbfunc:
        return

    if PMIX_SUCCESS == rc:
        strcpy(c_nspace, nspace)
    
    with nogil:
        cbfunc(rc, c_nspace, cbdata)


def pypmix_tool_connection_cbfunc(int rc, dict proc, dict cbdata_dict):
    cdef pmix_tool_connection_cbfunc_t cbfunc = <pmix_tool_connection_cbfunc_t> <uintptr_t> cbdata_dict['cbfunc']
    cdef void* cbdata = <void*> <uintptr_t> cbdata_dict['cbdata']
    cdef pmix_proc_t c_proc

    if NULL == cbfunc:
        return

    if PMIX_SUCCESS == rc:
        pmix_copy_nspace(c_proc.nspace, proc['nspace'])
        c_proc.rank = proc['rank']
    
    with nogil:
        cbfunc(rc, &c_proc, cbdata)

def pypmix_credential_cbfunc(int rc, dict byteobject, list info, dict cbdata_dict):
    cdef pmix_credential_cbfunc_t cbfunc = <pmix_credential_cbfunc_t> <uintptr_t> cbdata_dict['cbfunc']
    cdef void* cbdata = <void*> <uintptr_t> cbdata_dict['cbdata']
    cdef int size = 0
    cdef size_t ninfo = 0
    cdef pmix_byte_object_t c_byteobject
    cdef pmix_info_t *c_info = NULL

    if NULL == cbfunc:
        return

    if PMIX_SUCCESS == rc:
        c_byteobject.size = byteobject['size']
        c_byteobject.bytes = <char *> malloc(byteobject['size'])
        memcpy(c_byteobject.bytes, <void *> byteobject['bytes'], byteobject['size'])

        prc = pmix_alloc_info(&c_info, &ninfo, info)
        if PMIX_SUCCESS != prc:
            print("Error transferring info to C:", prc)
            return prc
    
    with nogil:
        cbfunc(rc, &c_byteobject, c_info, ninfo, cbdata)

    if 0 < size:
        free(c_byteobject.bytes)
    if 0 < ninfo:
        pmix_free_info(c_info, ninfo)


def pypmix_validation_cbfunc(int rc, dict byteobject, list info, dict cbdata_dict):
    cdef pmix_validation_cbfunc_t cbfunc = <pmix_validation_cbfunc_t> <uintptr_t> cbdata_dict['cbfunc']
    cdef void* cbdata = <void*> <uintptr_t> cbdata_dict['cbdata']
    cdef size_t ninfo = 0
    cdef pmix_info_t *c_info = NULL

    if NULL == cbfunc:
        return

    if PMIX_SUCCESS == rc:

        prc = pmix_alloc_info(&c_info, &ninfo, info)
        if PMIX_SUCCESS != prc:
            print("Error transferring info to C:", prc)
            return prc
    
    with nogil:
        cbfunc(rc, c_info, ninfo, cbdata)

    if 0 < ninfo:
        pmix_free_info(c_info, ninfo)


cdef void dmodx_cbfunc(pmix_status_t status,
                       char *data, size_t sz,
                       void *cbdata) noexcept:
    global active
    if PMIX_SUCCESS == status:
        active.cache_data(data, sz)
    active.set(status)
    return

cdef void setupapp_cbfunc(pmix_status_t status,
                          pmix_info_t info[], size_t ninfo,
                          void *provided_cbdata,
                          pmix_op_cbfunc_t cbfunc, void *cbdata) noexcept with gil:
    global active
    if PMIX_SUCCESS == status:
        ilist = []
        rc = pmix_unload_info(info, ninfo, ilist)
        active.cache_info(ilist)
        status = rc
    active.set(status)
    if (NULL != cbfunc):
        cbfunc(PMIX_SUCCESS, cbdata)
    return

cdef void collectinventory_cbfunc(pmix_status_t status, pmix_info_t info[],
                                  size_t ninfo, void *cbdata,
                                  pmix_release_cbfunc_t release_fn,
                                  void *release_cbdata) noexcept with gil:
    global active
    if PMIX_SUCCESS == status:
        ilist = []
        rc = pmix_unload_info(info, ninfo, ilist)
        active.cache_info(ilist)
        status = rc
    active.set(status)
    if (NULL != release_fn):
        release_fn(release_cbdata)
    return

cdef void pyiofhandler(size_t iofhdlr_id, pmix_iof_channel_t channel,
                       pmix_proc_t *source, pmix_byte_object_t *payload,
                       pmix_info_t info[], size_t ninfo) noexcept with gil:
    cdef char* kystr
    pychannel = int(channel)
    pyiof_id  = int(iofhdlr_id)

    # convert the source to python
    pysource = {}
    kystr = strdup(source[0].nspace)
    myns = kystr.decode('ascii')
    free(kystr)
    pysource = {'nspace': myns, 'rank': source[0].rank}

    # convert the inbound info to python
    pyinfo = []
    pmix_unload_info(info, ninfo, pyinfo)

    # convert payload to python byteobject
    pybytes = {}
    if NULL != payload:
        pybytes['bytes'] = payload[0].bytes
        pybytes['size']  = payload[0].size


    # find the handler being called
    found = False
    rc = PMIX_ERR_NOT_FOUND
    for h in myhdlrs:
        try:
            if iofhdlr_id == h['refid']:
                found = True
                # call user iof python handler
                h['hdlr'](pyiof_id, pychannel, pysource, pybytes, pyinfo)
        except:
            pass

    # if we didn't find the handler, cache this event in a timeshift
    # and try it again
    if not found:
        mycaddy    = <pmix_pyshift_t*> PyMem_Malloc(sizeof(pmix_pyshift_t))
        mycaddy.op = strdup("iofhdlr_cache")
        mycaddy.idx                 = iofhdlr_id
        mycaddy.channel             = channel
        memset(mycaddy.source.nspace, 0, PMIX_MAX_NSLEN+1)
        memcpy(mycaddy.source.nspace, source[0].nspace, PMIX_MAX_NSLEN)
        mycaddy.source.rank         = source[0].rank
        if payload != NULL:
            mycaddy.payload.bytes       = <char *>malloc(payload[0].size)
            memset(mycaddy.payload.bytes, 0, payload[0].size)
            memcpy(mycaddy.payload.bytes, payload[0].bytes, payload[0].size)
            mycaddy.payload.size        = payload[0].size
        else:
            mycaddy.payload.bytes   = <char *>NULL
            mycaddy.payload.size    = 0
        mycaddy.info                = info
        mycaddy.ndata               = ninfo
        cb = PyCapsule_New(mycaddy, "iofhdlr_cache", NULL)
        threading.Timer(0.001, iofhdlr_cache, [cb, rc]).start()
    return

cdef void pyeventhandler(size_t evhdlr_registration_id,
                         pmix_status_t status,
                         const pmix_proc_t *source,
                         pmix_info_t info[], size_t ninfo,
                         pmix_info_t *results, size_t nresults,
                         pmix_event_notification_cbfunc_fn_t cbfunc,
                         void *cbdata) noexcept with gil:
    cdef pmix_info_t *myresults
    cdef pmix_info_t **myresults_ptr
    cdef size_t nmyresults
    cdef char* kystr
    cdef pmix_nspace_t srcnspace
    cdef pmix_status_t ret_status
    cdef pypmix_info_cbdata_t *icbd

    # convert the source to python
    pysource = {}
    memset(srcnspace, 0, PMIX_MAX_NSLEN+1)
    memcpy(srcnspace, source[0].nspace, PMIX_MAX_NSLEN)
    kystr = strdup(srcnspace)
    myns = kystr.decode('ascii')
    free(kystr)
    srcrank = int(source[0].rank)
    pysource = {'nspace': myns, 'rank': srcrank}
    pyev_id  = int(evhdlr_registration_id)

    # convert the inbound info to python
    pyinfo = []
    if 0 < ninfo:
        rc = pmix_unload_info(info, ninfo, pyinfo)
        if PMIX_SUCCESS != rc:
            print("Unable to unload info structs")
            return

    # convert the inbound results from prior handlers
    # that serviced this event to python
    pyresults = []
    if 0 < nresults:
        rc = pmix_unload_info(results, nresults, pyresults)
        if PMIX_SUCCESS != rc:
            print("Unable to unload prior results")
            return

    # find the handler being called
    found = False
    rc = PMIX_ERR_NOT_FOUND
    for h in myhdlrs:
        try:
            if evhdlr_registration_id == h['refid']:
                found = True
                # execute their handler
                ret_status, pymyresults = h['hdlr'](pyev_id, status, pysource, pyinfo, pyresults)
                # allocate and load pmix info structs from python list of dictionaries
                myresults_ptr = &myresults
                prc = pmix_alloc_info(myresults_ptr, &nmyresults, pymyresults)
                if PMIX_SUCCESS != prc:
                    print("Unable to load new results")
                icbd = <pypmix_info_cbdata_t *> malloc(sizeof(pypmix_info_cbdata_t))
                if icbd == NULL:
                    print("Error allocating pypmix_info_cbdata_t")
                icbd.info = myresults
                icbd.ninfo = nmyresults
                with nogil:
                    cbfunc(ret_status, myresults, nmyresults, op_release, <void *>icbd, cbdata)
        except:
            pass

    # if we didn't find the handler, delay a little and try again
    if not found:
        mycaddy    = <pmix_pyshift_t*> PyMem_Malloc(sizeof(pmix_pyshift_t))
        mycaddy.op = strdup("event_handler")
        mycaddy.idx                 = evhdlr_registration_id
        mycaddy.status              = status
        memset(mycaddy.source.nspace, 0, PMIX_MAX_NSLEN+1)
        memcpy(mycaddy.source.nspace, source[0].nspace, PMIX_MAX_NSLEN)
        mycaddy.source.rank         = source[0].rank
        mycaddy.info                = info
        mycaddy.ndata               = ninfo
        mycaddy.results             = results
        mycaddy.nresults            = nresults
        mycaddy.op_cbfunc           = NULL
        mycaddy.cbdata              = NULL
        mycaddy.notification_cbdata = cbdata
        mycaddy.event_handler       = cbfunc
        cb = PyCapsule_New(mycaddy, "event_handler", NULL)
        threading.Timer(0.001, event_cache_cb, [cb, rc]).start()
    return

cdef class PMIxClient:
    cdef pmix_proc_t myproc;
    cdef pmix_fabric_t myfabric;
    cdef int fabric_set;
    cdef pmix_topology_t topo

    def __cinit__(self):
        memset(self.myproc.nspace, 0, sizeof(self.myproc.nspace))
        self.myproc.rank = PMIX_RANK_UNDEF
        memset(&self.myfabric, 0, sizeof(self.myfabric))
        self.fabric_set = 0
        self.topo.source = NULL
        self.topo.topology = NULL

    def __init__(self):
        global myhdlrs, myname
        memset(self.myproc.nspace, 0, sizeof(self.myproc.nspace))
        self.myproc.rank = <uint32_t>PMIX_RANK_UNDEF
        myhdlrs = []
        myname = {}

    def initialized(self):
        return PMIx_Initialized()

    def get_version(self):
        return PMIx_Get_version()

    # Initialize the PMIx client library, connecting
    # us to the local PMIx server
    #
    # @dicts [INPUT]
    #          - a list of dictionaries, where each
    #            dictionary has a key, value, and val_type
    #            defined as such:
    #            [{key:y, value:val, val_type:ty}, … ]
    #
    def init(self, dicts:list):
        cdef size_t klen
        global myname
        cdef pmix_info_t *info
        cdef pmix_info_t **info_ptr
        myname = {}

        # allocate and load pmix info structs from python list of dictionaries
        info_ptr = &info
        rc = pmix_alloc_info(info_ptr, &klen, dicts)
        rc = PMIx_Init(&self.myproc, info, klen)
        if 0 < klen:
            pmix_free_info(info, klen)
        if PMIX_SUCCESS == rc:
            # convert the returned name
            myname = {'nspace': (<bytes>self.myproc.nspace).decode('UTF-8'), 'rank': self.myproc.rank}
        return rc, myname

    # Finalize the client library
    def finalize(self, dicts:list):
        cdef size_t klen
        cdef pmix_info_t *info
        cdef pmix_info_t **info_ptr

        # allocate and load pmix info structs from python list of dictionaries
        info_ptr = &info
        rc = pmix_alloc_info(info_ptr, &klen, dicts)
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
    #        - list of proc nspace,rank dicts
    def abort(self, status, msg, peers:list):
        cdef pmix_proc_t *procs
        cdef size_t sz
        # convert list of procs to array of pmix_proc_t's
        if peers is not None:
            sz = len(peers)
            if 0 < sz:
                procs = <pmix_proc_t*> PyMem_Malloc(sz * sizeof(pmix_proc_t))
                if not procs:
                    return PMIX_ERR_NOMEM
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
                    return PMIX_ERR_NOMEM
                pmix_copy_nspace(procs[0].nspace, self.myproc.nspace)
                procs[0].rank = PMIX_RANK_WILDCARD
        else:
            # if they didn't give us a set of procs,
            # then we default to our entire job
            sz = 1
            procs = <pmix_proc_t*> PyMem_Malloc(sz * sizeof(pmix_proc_t))
            if not procs:
                return PMIX_ERR_NOMEM
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

    # Store some data locally for retrieval by other areas of the
    # proc. This is data that has only internal scope - it will
    # never be "pushed" externally
    #
    # @proc [INPUT]
    #       - namespace and rank of the client (dict)
    #
    # @key [INPUT]
    #      - the key to be stored
    #
    # @value [INPUT]
    #        - a dict to be stored with keys (value, val_type)
    def store_internal(self, pyproc:dict, pykey:str, pyval:dict):
        cdef pmix_key_t key
        cdef pmix_proc_t proc
        cdef pmix_value_t value

        # convert pyproc to pmix_proc_t
        if pyproc is None:
            pmix_copy_nspace(proc.nspace, self.myproc.nspace)
            proc.rank = self.myproc.rank
        else:
            pmix_copy_nspace(proc.nspace, pyproc['nspace'])
            proc.rank = pyproc['rank']

        # convert key,val to pmix_value_t and pmix_key_t
        pmix_copy_key(key, pykey)

        # convert the dict to a pmix_value_t
        rc = pmix_load_value(&value, pyval)

        # call API
        rc = PMIx_Store_internal(&proc, key, &value)
        if rc == PMIX_SUCCESS:
            pmix_free_value(self, &value)
        return rc

    # put a value into the keystore
    #
    # @scope [INPUT]
    #        - the scope of the data
    #
    # @key [INPUT]
    #      - the key to be stored
    #
    # @value [INPUT]
    #        - a dict to be stored with keys (value, val_type)
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

    def fence(self, peers:list, dicts:list):
        cdef pmix_proc_t *procs
        cdef pmix_info_t *info
        cdef pmix_info_t **info_ptr
        cdef size_t ninfo, nprocs
        nprocs = 0
        ninfo = 0
        # convert list of procs to array of pmix_proc_t's
        if peers is not None:
            nprocs = len(peers)
            if 0 < nprocs:
                procs = <pmix_proc_t*> PyMem_Malloc(nprocs * sizeof(pmix_proc_t))
                if not procs:
                    return PMIX_ERR_NOMEM
                rc = pmix_load_procs(procs, peers)
                if PMIX_SUCCESS != rc:
                    pmix_free_procs(procs, nprocs)
                    return rc
            else:
                nprocs = 1
                procs = <pmix_proc_t*> PyMem_Malloc(nprocs * sizeof(pmix_proc_t))
                if not procs:
                    return PMIX_ERR_NOMEM
                pmix_copy_nspace(procs[0].nspace, self.myproc.nspace)
                procs[0].rank = PMIX_RANK_WILDCARD
        else:
            nprocs = 1
            procs = <pmix_proc_t*> PyMem_Malloc(nprocs * sizeof(pmix_proc_t))
            if not procs:
                return PMIX_ERR_NOMEM
            pmix_copy_nspace(procs[0].nspace, self.myproc.nspace)
            procs[0].rank = PMIX_RANK_WILDCARD

        # allocate and load pmix info structs from python list of dictionaries
        info_ptr = &info
        rc = pmix_alloc_info(info_ptr, &ninfo, dicts)
        if PMIX_SUCCESS != rc:
            pmix_free_procs(procs, nprocs)
            return rc

        # pass it into the fence API
        rc = PMIx_Fence(procs, nprocs, info, ninfo)
        if 0 < nprocs:
            pmix_free_procs(procs, nprocs)
        if 0 < ninfo:
            pmix_free_info(info, ninfo)
        return rc

    # retrieve a value from the keystore
    #
    # @proc [INPUT]
    #       - namespace and rank of the client (dict)
    #
    # @key [INPUT]
    #      - the key to be retrieved
    #
    # @dicts [INPUT]
    #          - a list of dictionaries, where each
    #            dictionary has a key, value, and val_type
    #            defined as such:
    #            [{key:y, value:val, val_type:ty}, … ]
    def get(self, proc:dict, ky, dicts:list):
        cdef pmix_info_t *info;
        cdef pmix_info_t **info_ptr;
        cdef size_t ninfo;
        cdef pmix_key_t key;
        cdef pmix_value_t *val_ptr;
        cdef pmix_proc_t p;

        ninfo   = 0
        val_ptr = NULL

        # convert proc to pmix_proc_t
        if proc is None:
            pmix_copy_nspace(p.nspace, self.myproc.nspace)
            p.rank = self.myproc.rank
        else:
            pmix_copy_nspace(p.nspace, proc['nspace'])
            p.rank = proc['rank']

        # convert key,val to pmix_value_t and pmix_key_t
        pmix_copy_key(key, ky)

        # allocate and load pmix info structs from python list of dictionaries
        info_ptr = &info
        rc = pmix_alloc_info(info_ptr, &ninfo, dicts)

        val = None

        # pass it into the get API
        with nogil:
            rc = PMIx_Get(&p, key, info, ninfo, &val_ptr)
        if PMIX_SUCCESS == rc:
            val = pmix_unload_value(val_ptr)
            pmix_free_value(self, val_ptr)
        if 0 < ninfo:
            pmix_free_info(info, ninfo)
        return rc, val

    # Publish the data in the info array for lookup
    #
    # @dicts [INPUT]
    #          - a list of dictionaries, where
    #            a key, flags, value, and val_type
    #            can be defined as keys
    def publish(self, dicts:list):
        cdef pmix_info_t *info;
        cdef pmix_info_t **info_ptr;
        cdef size_t ninfo;
        ninfo = 0

        # allocate and load pmix info structs from python list of dictionaries
        info_ptr = &info
        rc = pmix_alloc_info(info_ptr, &ninfo, dicts)

        # pass it into the publish API
        rc = PMIx_Publish(info, ninfo)
        if 0 < ninfo:
            pmix_free_info(info, ninfo)
        return rc

    # unpublish the data in the data store
    #
    # @dicts [INPUT]
    #          - a list of dictionaries, where
    #            a key, flags, value, and val_type
    #            can be defined as keys
    # @pykeys [INPUT]
    #          - list of python info key strings
    def unpublish(self, pykeys:list, dicts:list):
        cdef pmix_info_t *info;
        cdef pmix_info_t **info_ptr;
        cdef size_t ninfo;
        cdef size_t nstrings;
        cdef char **keys;
        keys     = NULL
        ninfo    = 0
        nstrings = 0

        # load pykeys into char **keys
        if pykeys is not None:
            nstrings = len(pykeys)
            if 0 < nstrings:
                keys = <char **> PyMem_Malloc(nstrings * sizeof(char*))
                if not keys:
                    PMIX_ERR_NOMEM
            rc = pmix_load_argv(keys, pykeys)
            if PMIX_SUCCESS != rc:
                n = 0
                while keys[n] != NULL:
                    PyMem_Free(keys[n])
                    n += 1
                return rc
        else:
            keys = NULL

        # allocate and load pmix info structs from python list of dictionaries
        if dicts is not None:
            info_ptr = &info
            rc = pmix_alloc_info(info_ptr, &ninfo, dicts)
        else:
            info = NULL

        # pass it into the unpublish API
        rc = PMIx_Unpublish(keys, info, ninfo)
        if 0 < ninfo:
            pmix_free_info(info, ninfo)
        return rc

    # lookup info published by this or another process
    # @pdata [INPUT]
    #          - a list of dictionaries, where key is
    #            recorded in the pdata dictionary and
    #            passed to PMIx_Lookup
    # pdata = {‘proc’: {‘nspace’: mynspace, ‘rank’: myrank}, ‘key’: ky,
    # ‘value’: v, ‘val_type’: t}
    # @dicts [INPUT]
    #          - a list of dictionaries, where
    #            a key, flags, value, and val_type
    #            can be defined as keys
    def lookup(self, data:list, dicts:list):
        cdef pmix_pdata_t *pdata;
        cdef pmix_info_t  *info;
        cdef pmix_info_t  **info_ptr;
        cdef size_t npdata;
        cdef size_t ninfo;

        npdata  = 0
        ninfo   = 0

        # allocate and load pmix info structs from python list of dictionaries
        info_ptr = &info
        rc = pmix_alloc_info(info_ptr, &ninfo, dicts)

        # convert the list of dictionaries to array of
        # pmix_pdata_t structs
        if data is not None:
            npdata = len(data)
            if 0 < npdata:
                pdata = <pmix_pdata_t*> PyMem_Malloc(npdata * sizeof(pmix_pdata_t))
                if not pdata:
                    return PMIX_ERR_NOMEM
                n = 0
                for d in data:
                    pykey = d['key']
                    pmix_copy_key(pdata[n].key, pykey)
                    rc = 0
                    n += 1
                if PMIX_SUCCESS != rc:
                    pmix_free_pdata(pdata, npdata)
                    return rc
            else:
                pdata = NULL
        else:
            pdata = NULL

        # pass it into the lookup API
        rc = PMIx_Lookup(pdata, npdata, info, ninfo)
        if PMIX_SUCCESS == rc:
            rc = pmix_unload_pdata(pdata, npdata, data)
            # remove the first element, which is just the key
            data.pop(0)
            pmix_free_info(info, ninfo)
            pmix_free_pdata(pdata, npdata)
        return rc, data

    # Spawn a new job
    #
    #
    def spawn(self, jobInfo:list, pyapps:list):
        cdef pmix_info_t *jinfo;
        cdef pmix_info_t **jinfo_ptr;
        cdef pmix_app_t *apps;
        cdef size_t ninfo
        cdef size_t napps;
        cdef pmix_nspace_t nspace;

        # protect against bad input
        if pyapps is None or len(pyapps) == 0:
            return PMIX_ERR_BAD_PARAM, None

        # allocate and load pmix info structs from python list of dictionaries
        if jobInfo is not None:
            jinfo_ptr = &jinfo
            rc = pmix_alloc_info(jinfo_ptr, &ninfo, jobInfo)
        else:
            jinfo = NULL
            ninfo = 0

        # convert the list of apps to an array of pmix_app_t
        napps = len(pyapps)
        apps = <pmix_app_t*> PyMem_Malloc(napps * sizeof(pmix_app_t))
        if not apps:
            pmix_free_info(jinfo, ninfo)
            return PMIX_ERR_NOMEM, None

        rc = pmix_load_apps(apps, pyapps)
        if PMIX_SUCCESS != rc:
            pmix_free_apps(apps, napps)
            if 0 < ninfo:
                pmix_free_info(jinfo, ninfo)
            return rc, None

        with nogil:
            rc = PMIx_Spawn(jinfo, ninfo, apps, napps, nspace)
        pmix_free_apps(apps, napps)

        if 0 < ninfo:
            pmix_free_info(jinfo, ninfo)
        if PMIX_SUCCESS != rc:
            pyns = None
        else:
            pyns = nspace.decode('ascii')
        return rc, pyns

    def connect(self, peers:list, pyinfo:list):
        cdef pmix_proc_t *procs
        cdef pmix_info_t *info
        cdef pmix_info_t **info_ptr
        cdef size_t ninfo
        cdef size_t nprocs
        nprocs = 0
        ninfo = 0

        # convert list of procs to array of pmix_proc_t's
        if peers is not None:
            nprocs = len(peers)
            if 0 < nprocs:
                procs = <pmix_proc_t*> PyMem_Malloc(nprocs * sizeof(pmix_proc_t))
                if not procs:
                    return PMIX_ERR_NOMEM
                rc = pmix_load_procs(procs, peers)
                if PMIX_SUCCESS != rc:
                    pmix_free_procs(procs, nprocs)
                    return rc
            else:
                nprocs = 1
                procs = <pmix_proc_t*> PyMem_Malloc(nprocs * sizeof(pmix_proc_t))
                if not procs:
                    return PMIX_ERR_NOMEM
                pmix_copy_nspace(procs[0].nspace, self.myproc.nspace)
                procs[0].rank = PMIX_RANK_WILDCARD
        else:
            nprocs = 1
            procs = <pmix_proc_t*> PyMem_Malloc(nprocs * sizeof(pmix_proc_t))
            if not procs:
                return PMIX_ERR_NOMEM
            pmix_copy_nspace(procs[0].nspace, self.myproc.nspace)
            procs[0].rank = PMIX_RANK_WILDCARD

        # allocate and load pmix info structs from python list of dictionaries
        info_ptr = &info
        rc = pmix_alloc_info(info_ptr, &ninfo, pyinfo)

        # Call the library
        rc = PMIx_Connect(procs, nprocs, info, ninfo)
        if 0 < nprocs:
            pmix_free_procs(procs, nprocs)
        if 0 < ninfo:
            pmix_free_info(info, ninfo)
        return rc

    def disconnect(self, peers:list, pyinfo:list):
        cdef pmix_proc_t *procs
        cdef pmix_info_t *info
        cdef pmix_info_t **info_ptr
        cdef size_t ninfo
        cdef size_t nprocs
        nprocs = 0
        ninfo = 0

        # convert list of procs to array of pmix_proc_t's
        if peers is not None:
            nprocs = len(peers)
            if 0 < nprocs:
                procs = <pmix_proc_t*> PyMem_Malloc(nprocs * sizeof(pmix_proc_t))
                if not procs:
                    return PMIX_ERR_NOMEM
                rc = pmix_load_procs(procs, peers)
                if PMIX_SUCCESS != rc:
                    pmix_free_procs(procs, nprocs)
                    return rc
            else:
                nprocs = 1
                procs = <pmix_proc_t*> PyMem_Malloc(nprocs * sizeof(pmix_proc_t))
                if not procs:
                    return PMIX_ERR_NOMEM
                pmix_copy_nspace(procs[0].nspace, self.myproc.nspace)
                procs[0].rank = PMIX_RANK_WILDCARD
        else:
            nprocs = 1
            procs = <pmix_proc_t*> PyMem_Malloc(nprocs * sizeof(pmix_proc_t))
            if not procs:
                return PMIX_ERR_NOMEM
            pmix_copy_nspace(procs[0].nspace, self.myproc.nspace)
            procs[0].rank = PMIX_RANK_WILDCARD

        # allocate and load pmix info structs from python list of dictionaries
        info_ptr = &info
        rc = pmix_alloc_info(info_ptr, &ninfo, pyinfo)

        # Call the library
        rc = PMIx_Disconnect(procs, nprocs, info, ninfo)
        if 0 < nprocs:
            pmix_free_procs(procs, nprocs)
        if 0 < ninfo:
            pmix_free_info(info, ninfo)
        return rc

    def resolve_peers(self, pynode:str, pyns:str):
        cdef pmix_nspace_t nspace
        cdef char *nodename
        cdef pmix_proc_t *procs
        cdef size_t nprocs
        peers = []

        nodename = NULL
        memset(nspace, 0, sizeof(nspace))
        procs = NULL
        if pynode is not None:
            pyn = pynode.encode('ascii')
            nodename = strdup(pyn)
        if pyns is not None:
            pmix_copy_nspace(nspace, pyns)
        rc = PMIx_Resolve_peers(nodename, nspace, &procs, &nprocs)
        if PMIX_SUCCESS == rc and 0 < nprocs:
            rc = pmix_unload_procs(procs, nprocs, peers)
            pmix_free_procs(procs, nprocs)
        return rc, peers

    def resolve_nodes(self, pyns:str):
        cdef pmix_nspace_t nspace
        cdef char *nodelist

        nodelist = NULL
        memset(nspace, 0, sizeof(nspace))
        if pyns is not None:
            pmix_copy_nspace(nspace, pyns)
        rc = PMIx_Resolve_nodes(nspace, &nodelist)
        if PMIX_SUCCESS == rc:
            pyn = nodelist
            pynodes = pyn.decode('ascii')
            PyMem_Free(nodelist)
        return rc, pynodes

    def query(self, pyq:list):
        cdef pmix_query_t *queries
        cdef size_t nqueries
        cdef pmix_info_t *results
        cdef pmix_info_t **results_ptr
        cdef size_t nresults
        cdef pmix_info_t **qual_ptr
        cdef pmix_status_t _rc;
        nqueries   = 0
        nresults   = 0
        queries    = NULL
        qual_ptr   = NULL

        pyresults = []
        if pyq is not None:
            nqueries = len(pyq)
            if 0 < nqueries:
                queries = <pmix_query_t*> PyMem_Malloc(nqueries * sizeof(pmix_query_t))
                if not queries:
                    return PMIX_ERR_NOMEM,pyresults
                n = 0
                for q in pyq:
                    queries[n].keys       = NULL
                    queries[n].qualifiers = NULL
                    nstrings = len(q['keys'])
                    if 0 < nstrings:
                        queries[n].keys = <char **> PyMem_Malloc((nstrings+1) * sizeof(char*))
                        if not queries[n].keys:
                            pmix_free_queries(queries, nqueries)
                            return PMIX_ERR_NOMEM,pyresults
                        rc = pmix_load_argv(queries[n].keys, q['keys'])
                        if PMIX_SUCCESS != rc:
                            pmix_free_queries(queries, nqueries)
                            return rc,pyresults
                    # allocate and load pmix info structs from python list of dictionaries
                    queries[n].nqual = 0
                    qual_ptr         = &(queries[n].qualifiers)
                    rc               = pmix_alloc_info(qual_ptr, &(queries[n].nqual), q['qualifiers'])
                    n += 1
            else:
                nqueries = 0
        else:
            nqueries = 0

        # pass it into the query_info API
        with nogil:
            _rc = PMIx_Query_info(queries, nqueries, &results, &nresults)
        rc = _rc
        if PMIX_SUCCESS == rc:
            rc = pmix_unload_info(results, nresults,  pyresults)
            # free results info structs
            pmix_free_info(results, nresults)
        # free memory for query structs
        pmix_free_queries(queries, nqueries)
        return rc, pyresults

    def log(self, pydata:list, pydirs:list):
        cdef pmix_info_t *data
        cdef pmix_info_t **data_ptr
        cdef pmix_info_t *directives
        cdef pmix_info_t **directives_ptr
        cdef size_t ndata
        cdef size_t ndirs

        # allocate and load pmix info structs from python list of dictionaries
        data_ptr = &data
        directives_ptr = &directives
        rc = pmix_alloc_info(data_ptr, &ndata, pydata)
        rc = pmix_alloc_info(directives_ptr, &ndirs, pydirs)

        # call the API
        rc = PMIx_Log(data, ndata, directives, ndirs)
        pmix_free_info(data, ndata)
        if 0 < ndirs:
            pmix_free_info(directives, ndirs)
        return rc

    def allocation_request(self, directive, pyinfo:list):
        cdef pmix_info_t *info
        cdef pmix_info_t **info_ptr
        cdef pmix_info_t *results
        cdef size_t ninfo
        cdef size_t nresults

        results = NULL
        nresults = 0
        pyres = []

        # allocate and load pmix info structs from python list of dictionaries
        info_ptr = &info
        rc = pmix_alloc_info(info_ptr, &ninfo, pyinfo)

        # call the API
        rc = PMIx_Allocation_request(directive, info, ninfo, &results, &nresults)
        if 0 < ninfo:
            pmix_free_info(info, ninfo)
        if PMIX_SUCCESS == rc and 0 < nresults:
            # convert the results
            rc = pmix_unload_info(results, nresults, pyres)
            pmix_free_info(results, nresults)
        return rc, pyres

    def job_control(self, pytargets:list, pydirs:list):
        cdef pmix_proc_t *targets
        cdef pmix_info_t *directives
        cdef pmix_info_t **directives_ptr
        cdef pmix_info_t *results
        cdef size_t ntargets
        cdef size_t ndirs
        cdef size_t nresults

        results = NULL
        nresults = 0
        pyres = []
        # convert list of procs to array of pmix_proc_t's
        if pytargets is not None:
            ntargets = len(pytargets)
            if 0 < ntargets:
                targets = <pmix_proc_t*> PyMem_Malloc(ntargets * sizeof(pmix_proc_t))
                if not targets:
                    return PMIX_ERR_NOMEM, pyres
                rc = pmix_load_procs(targets, pytargets)
                if PMIX_SUCCESS != rc:
                    pmix_free_procs(targets, ntargets)
                    return rc, pyres
            else:
                ntargets = 1
                targets = <pmix_proc_t*> PyMem_Malloc(ntargets * sizeof(pmix_proc_t))
                if not targets:
                    return PMIX_ERR_NOMEM, pyres
                pmix_copy_nspace(targets[0].nspace, self.myproc.nspace)
                targets[0].rank = PMIX_RANK_WILDCARD
        else:
            ntargets = 1
            targets = <pmix_proc_t*> PyMem_Malloc(ntargets * sizeof(pmix_proc_t))
            if not targets:
                return PMIX_ERR_NOMEM, pyres
            pmix_copy_nspace(targets[0].nspace, self.myproc.nspace)
            targets[0].rank = PMIX_RANK_WILDCARD

        # allocate and load pmix info structs from python list of dictionaries
        directives_ptr = &directives
        rc = pmix_alloc_info(directives_ptr, &ndirs, pydirs)
        if PMIX_SUCCESS != rc:
            if 0 < ntargets:
                pmix_free_procs(targets, ntargets)
            return rc

        # call the API
        rc = PMIx_Job_control(targets, ntargets, directives, ndirs, &results, &nresults)
        if 0 < ndirs:
            pmix_free_info(directives, ndirs)
        if 0 < ntargets:
            pmix_free_procs(targets, ntargets)
        if PMIX_SUCCESS == rc and 0 < nresults:
            # convert the results
            rc = pmix_unload_info(results, nresults, pyres)
            pmix_free_info(results, nresults)
        return rc, pyres

    def monitor(self, pymonitor_info:list, code:int, pydirs:list):
        cdef pmix_info_t *monitor_info
        cdef pmix_info_t **monitor_info_ptr
        cdef pmix_info_t *directives
        cdef pmix_info_t **directives_ptr
        cdef pmix_info_t *results
        cdef size_t nmonitor
        cdef size_t ndirs
        cdef size_t nresults

        results = NULL
        nresults = 0
        pyres = []

        # convert list of info to array of pmix_info_t's
        monitor_info_ptr = &monitor_info
        rc = pmix_alloc_info(monitor_info_ptr, &nmonitor, pymonitor_info)
        if PMIX_SUCCESS != rc:
            if 0 < nmonitor:
                pmix_free_info(monitor_info, nmonitor)
            return rc

        # allocate and load pmix info structs from python list of dictionaries
        directives_ptr = &directives
        rc = pmix_alloc_info(directives_ptr, &ndirs, pydirs)
        if PMIX_SUCCESS != rc:
            if 0 < ndirs:
                pmix_free_info(directives, ndirs)
            return rc

        # call the API
        rc = PMIx_Process_monitor(monitor_info, code, directives, ndirs, &results, &nresults)
        if 0 < ndirs:
            pmix_free_info(directives, ndirs)
        if 0 < nmonitor:
            pmix_free_info(monitor_info, nmonitor)
        if PMIX_SUCCESS == rc and 0 < nresults:
            # convert the results
            rc = pmix_unload_info(results, nresults, pyres)
            pmix_free_info(results, nresults)
        return rc, pyres

    def get_credential(self, pyinfo:list):
        cdef pmix_info_t *info
        cdef pmix_info_t **info_ptr
        cdef pmix_byte_object_t bo
        cdef pmix_byte_object_t *boptr
        cdef size_t ninfo

        # allocate and load pmix info structs from python list of dictionaries
        info_ptr = &info
        rc = pmix_alloc_info(info_ptr, &ninfo, pyinfo)
        if PMIX_SUCCESS != rc:
            if 0 < ninfo:
                pmix_free_info(info, ninfo)
            return rc

        # call the API
        boptr = &bo
        rc = PMIx_Get_credential(info, ninfo, boptr)
        if 0 < ninfo:
            pmix_free_info(info, ninfo)
        blist = []
        cred = {}
        if PMIX_SUCCESS == rc and 0 < bo.size:
            # convert the results
            pmix_unload_bytes(bo.bytes, bo.size, blist)
            barray = bytearray(blist)
            cred['bytes'] = barray
            cred['size'] = bo.size
        return rc, cred

    def validate_credential(self, pycred:dict, pyinfo:list):
        cdef pmix_info_t *info
        cdef pmix_info_t **info_ptr
        cdef pmix_byte_object_t *bo
        cdef size_t ninfo
        cdef pmix_info_t *results
        cdef size_t nresults

        results = NULL
        nresults = 0
        pyres = []

        # convert pycred to pmix_byte_object_t
        bo = <pmix_byte_object_t*>PyMem_Malloc(sizeof(pmix_byte_object_t))
        if not bo:
            return PMIX_ERR_NOMEM
        cred = bytes(pycred['bytes'], 'ascii')
        bo.size = sizeof(cred)
        bo.bytes = <char*> PyMem_Malloc(bo.size)
        if not bo.bytes:
            return PMIX_ERR_NOMEM
        pyptr = <const char*>cred
        memcpy(bo.bytes, pyptr, bo.size)

        # allocate and load pmix info structs from python list of dictionaries
        info_ptr = &info
        rc = pmix_alloc_info(info_ptr, &ninfo, pyinfo)
        if PMIX_SUCCESS != rc:
            if 0 < ninfo:
                pmix_free_info(info, ninfo)
            return rc

        # call the API
        rc = PMIx_Validate_credential(bo, info, ninfo, &results, &nresults)
        if 0 < ninfo:
            pmix_free_info(info, ninfo)
        if PMIX_SUCCESS == rc and 0 < nresults:
            # convert the results
            rc = pmix_unload_info(results, nresults, pyres)
            pmix_free_info(results, nresults)
        return rc, pyres

    def group_construct(self, group:str, peers:list, pyinfo:list):
        cdef pmix_proc_t *procs
        cdef pmix_info_t *info
        cdef pmix_info_t **info_ptr
        cdef pmix_info_t *results
        cdef size_t ninfo
        cdef size_t nprocs
        cdef size_t nresults
        nprocs = 0
        ninfo = 0

        # convert group name
        pygrp = group.encode('ascii')
        # convert list of procs to array of pmix_proc_t's
        if peers is not None:
            nprocs = len(peers)
            procs = <pmix_proc_t*> PyMem_Malloc(nprocs * sizeof(pmix_proc_t))
            if not procs:
                return PMIX_ERR_NOMEM
            rc = pmix_load_procs(procs, peers)
            if PMIX_SUCCESS != rc:
                pmix_free_procs(procs, nprocs)
                return rc
        else:
            nprocs = 1
            procs = <pmix_proc_t*> PyMem_Malloc(nprocs * sizeof(pmix_proc_t))
            if not procs:
                return PMIX_ERR_NOMEM
            pmix_copy_nspace(procs[0].nspace, self.myproc.nspace)
            procs[0].rank = PMIX_RANK_WILDCARD

        # allocate and load pmix info structs from python list of dictionaries
        info_ptr = &info
        rc = pmix_alloc_info(info_ptr, &ninfo, pyinfo)

        # Call the library
        rc = PMIx_Group_construct(pygrp, procs, nprocs, info, ninfo, &results, &nresults)
        if 0 < nprocs:
            pmix_free_procs(procs, nprocs)
        if 0 < ninfo:
            pmix_free_info(info, ninfo)
        pyres = []
        if 0 < nresults:
            # convert results
            pmix_unload_info(results, nresults, pyres)
            pmix_free_info(results, nresults)
        return rc, pyres

    def group_invite(self, group:str, peers:list, pyinfo:list):
        cdef pmix_proc_t *procs
        cdef pmix_info_t *info
        cdef pmix_info_t **info_ptr
        cdef pmix_info_t *results
        cdef size_t ninfo
        cdef size_t nprocs
        cdef size_t nresults
        nprocs = 0
        ninfo = 0

        # convert group name
        pygrp = group.encode('ascii')
        # convert list of procs to array of pmix_proc_t's
        if peers is not None:
            nprocs = len(peers)
            procs = <pmix_proc_t*> PyMem_Malloc(nprocs * sizeof(pmix_proc_t))
            if not procs:
                return PMIX_ERR_NOMEM
            rc = pmix_load_procs(procs, peers)
            if PMIX_SUCCESS != rc:
                pmix_free_procs(procs, nprocs)
                return rc
        else:
            nprocs = 1
            procs = <pmix_proc_t*> PyMem_Malloc(nprocs * sizeof(pmix_proc_t))
            if not procs:
                return PMIX_ERR_NOMEM
            pmix_copy_nspace(procs[0].nspace, self.myproc.nspace)
            procs[0].rank = PMIX_RANK_WILDCARD

        # allocate and load pmix info structs from python list of dictionaries
        info_ptr = &info
        rc = pmix_alloc_info(info_ptr, &ninfo, pyinfo)

        # Call the library
        rc = PMIx_Group_invite(pygrp, procs, nprocs, info, ninfo, &results, &nresults)
        if 0 < nprocs:
            pmix_free_procs(procs, nprocs)
        if 0 < ninfo:
            pmix_free_info(info, ninfo)
        pyres = []
        if 0 < nresults:
            # convert results
            pmix_unload_info(results, nresults, pyres)
            pmix_free_info(results, nresults)
        return rc, pyres

    def group_join(self, group:str, leader:dict, opt:int, pyinfo:list):
        cdef pmix_proc_t proc
        cdef pmix_info_t *info
        cdef pmix_info_t **info_ptr
        cdef pmix_info_t *results
        cdef size_t ninfo
        cdef size_t nprocs
        cdef size_t nresults
        ninfo = 0

        # convert group name
        pygrp = group.encode('ascii')
        # convert leader to proc
        if leader is not None:
            pmix_copy_nspace(proc.nspace, leader['nspace'])
            proc.rank = leader['rank']
        else:
            pmix_copy_nspace(proc.nspace, self.myproc.nspace)
            proc.rank = self.myproc.rank

        # allocate and load pmix info structs from python list of dictionaries
        info_ptr = &info
        rc = pmix_alloc_info(info_ptr, &ninfo, pyinfo)

        # Call the library
        rc = PMIx_Group_join(pygrp, &proc, opt, info, ninfo, &results, &nresults)
        if 0 < ninfo:
            pmix_free_info(info, ninfo)
        pyres = []
        if 0 < nresults:
            # convert results
            pmix_unload_info(results, nresults, pyres)
            pmix_free_info(results, nresults)
        return rc, pyres

    def group_leave(self, group:str, pyinfo:list):
        cdef pmix_info_t *info
        cdef pmix_info_t **info_ptr
        cdef size_t ninfo
        ninfo = 0

        # convert group name
        pygrp = group.encode('ascii')

        # allocate and load pmix info structs from python list of dictionaries
        info_ptr = &info
        rc = pmix_alloc_info(info_ptr, &ninfo, pyinfo)

        # Call the library
        rc = PMIx_Group_leave(pygrp, info, ninfo)
        if 0 < ninfo:
            pmix_free_info(info, ninfo)
        return rc

    def group_destruct(self, group:str, pyinfo:list):
        cdef pmix_info_t *info
        cdef pmix_info_t **info_ptr
        cdef size_t ninfo
        ninfo = 0

        # convert group name
        pygrp = group.encode('ascii')

        # allocate and load pmix info structs from python list of dictionaries
        info_ptr = &info
        rc = pmix_alloc_info(info_ptr, &ninfo, pyinfo)

        # Call the library
        rc = PMIx_Group_destruct(pygrp, info, ninfo)
        if 0 < ninfo:
            pmix_free_info(info, ninfo)
        return rc

    def register_event_handler(self, pycodes:list, pyinfo:list, hdlr):
        cdef pmix_status_t *codes
        cdef size_t ncodes
        cdef pmix_info_t *info
        cdef pmix_info_t **info_ptr
        cdef size_t ninfo

        # convert the codes to an array of ints
        if pycodes is not None:
            ncodes = len(pycodes)
            codes = <int*> PyMem_Malloc(ncodes * sizeof(int))
            if not codes:
                return PMIX_ERR_NOMEM
            n = 0
            for c in pycodes:
                codes[n] = c
                n += 1
        else:
            codes = NULL
            ncodes = 0
        # allocate and load pmix info structs from python list of dictionaries
        if pyinfo is not None:
            info_ptr = &info
            rc = pmix_alloc_info(info_ptr, &ninfo, pyinfo)
            if PMIX_SUCCESS != rc:
                print("Error converting info array:", self.error_string(rc))
                return rc, -1
        else:
            info = NULL
            ninfo = 0

        # pass our hdlr switchyard to the API
        with nogil:
             rc = PMIx_Register_event_handler(codes, ncodes, info, ninfo, pyeventhandler, NULL, NULL)

        # cleanup
        if 0 < ninfo:
            pmix_free_info(info, ninfo)
        if 0 < ncodes:
            PyMem_Free(codes)

        # if rc < 0, then there was an error
        if 0 > rc:
            return rc, -1

        # otherwise, this is our ref ID for this hdlr
        myhdlrs.append({'refid': rc, 'hdlr': hdlr})
        return PMIX_SUCCESS, rc

    def deregister_event_handler(self, ref:int):
        rc = PMIx_Deregister_event_handler(ref, NULL, NULL)
        return rc

    def notify_event(self, status:int, pysrc:dict, range, pyinfo:list):
        cdef pmix_proc_t proc
        cdef pmix_info_t *info
        cdef pmix_info_t **info_ptr
        cdef size_t ninfo
        cdef pmix_data_range_t crange = range
        cdef pmix_status_t cstatus = status

        # convert the proc
        pmix_copy_nspace(proc.nspace, pysrc['nspace'])
        proc.rank = pysrc['rank']

        # allocate and load pmix info structs from python list of dictionaries
        info_ptr = &info
        rc = pmix_alloc_info(info_ptr, &ninfo, pyinfo)

        # call the library
        with nogil:
            rc = PMIx_Notify_event(cstatus, &proc, crange, info, ninfo, NULL, NULL)
        if 0 < ninfo:
            pmix_free_info(info, ninfo)
        return rc

    def error_string(self, pystat:int):
        cdef char *string

        string = <char*>PMIx_Error_string(pystat)
        pystr = string
        val = pystr.decode('ascii')
        return val

    def proc_state_string(self, pystat:int):
        cdef char *string

        string = <char*>PMIx_Proc_state_string(pystat)
        pystr = string
        return pystr.decode('ascii')

    def scope_string(self, pystat:int):
        cdef char *string

        string = <char*>PMIx_Scope_string(pystat)
        pystr = string
        return pystr.decode('ascii')

    def persistence_string(self, pystat:int):
        cdef char *string

        string = <char*>PMIx_Persistence_string(pystat)
        pystr = string
        return pystr.decode('ascii')

    def data_range_string(self, pystat:int):
        cdef char *string

        string = <char*>PMIx_Data_range_string(pystat)
        pystr = string
        return pystr.decode('ascii')

    def info_directives_string(self, pystat:int):
        cdef char *string

        string = <char*>PMIx_Info_directives_string(pystat)
        pystr = string
        return pystr.decode('ascii')

    def data_type_string(self, pystat:int):
        cdef char *string

        string = <char*>PMIx_Data_type_string(pystat)
        pystr = string
        return pystr.decode('ascii')

    def alloc_directive_string(self, pystat:int):
        cdef char *string

        string = <char*>PMIx_Alloc_directive_string(pystat)
        pystr = string
        return pystr.decode('ascii')

    def iof_channel_string(self, pystat:int):
        cdef char *string

        string = <char*>PMIx_IOF_channel_string(pystat)
        pystr = string
        return pystr.decode('ascii')

    def job_state_string(self, pystat:int):
        cdef char *string

        string = <char*>PMIx_Job_state_string(pystat)
        pystr = string
        return pystr.decode('ascii')

    def get_attribute_string(self, rep:str):
        cdef char *string
        pyrep = rep.encode('ascii')
        string = <char*>PMIx_Get_attribute_string(pyrep)
        pystr = string
        return pystr.decode('ascii')

    def get_attribute_name(self, rep:str):
        cdef char *string
        pyrep = rep.encode('ascii')
        string = <char*>PMIx_Get_attribute_name(pyrep)
        pystr = string
        return pystr.decode('ascii')

    def link_state_string(self, pystat:int):
        cdef char *string

        string = <char*>PMIx_Link_state_string(pystat)
        pystr = string
        return pystr.decode('ascii')

    def device_type_string(self, pystat:int):
        cdef char *string

        string = <char*>PMIx_Device_type_string(pystat)
        pystr = string
        return pystr.decode('ascii')

    def fabric_register(self, dicts:list):
        cdef pmix_info_t *info
        cdef pmix_info_t **info_ptr
        cdef size_t sz
        fabricinfo = []
        if 1 == self.fabric_set:
            return (PMIX_ERR_RESOURCE_BUSY, None)

        # allocate and load pmix info structs from python list of dictionaries
        info_ptr = &info
        rc = pmix_alloc_info(info_ptr, &sz, dicts)

        if sz > 0:
            rc = PMIx_Fabric_register(&self.myfabric, info, sz)
            pmix_free_info(info, sz)
        else:
            rc = PMIx_Fabric_register(&self.myfabric, NULL, 0)
        if PMIX_SUCCESS == rc:
            self.fabric_set = 1
            # convert the fabric info array for return
            if 0 < self.myfabric.ninfo:
                pmix_unload_info(self.myfabric.info, self.myfabric.ninfo, fabricinfo)
        return (rc, fabricinfo)

    def fabric_update(self):
        fabricinfo = []
        if 0 == self.fabric_set:
            return (PMIX_ERR_INIT, None)
        rc = PMIx_Fabric_update(&self.myfabric)
        # convert the fabric info array for return
        if 0 < self.myfabric.ninfo:
            pmix_unload_info(self.myfabric.info, self.myfabric.ninfo, fabricinfo)
        return (rc, fabricinfo)

    def fabric_deregister(self):
        if 0 == self.fabric_set:
            return PMIX_ERR_INIT
        rc = PMIx_Fabric_deregister(&self.myfabric)
        self.fabric_set = 0
        return rc;

    def load_topology(self):
        rc = PMIx_Load_topology(&self.topo)
        return rc

    def get_relative_locality(self, loc1:str, loc2:str):
        cdef char *string
        cdef pmix_locality_t locality
        pyl1 = loc1.encode('ascii')
        pyl2 = loc2.encode('ascii')
        pyloc = []
        rc = PMIx_Get_relative_locality(pyl1, pyl2, &locality)
        if PMIX_SUCCESS == rc:
            pmix_convert_locality(locality, pyloc)
        return (rc, pyloc)

    def parse_cpuset_string(self, csetstr:str):
        return (PMIX_ERR_NOT_SUPPORTED, None)

    def get_cpuset(self, ref:int):
        cdef pmix_cpuset_t cpuset
        cdef char* csetstr
        pycpus = {}
        rc = PMIx_Get_cpuset(&cpuset, ref)
        if PMIX_SUCCESS == rc:
            rc = PMIx_server_generate_cpuset_string(&cpuset, &csetstr)
            if PMIX_SUCCESS == rc:
                pycpus['source'] = strdup(cpuset.source)
                txt = csetstr.decode('ascii')
                pycpus['cpus'] = txt.split(",")
        return (rc, pycpus)

    def compute_distances(self, pycpus:dict, dicts:list):
        cdef pmix_cpuset_t cpuset
        cdef pmix_info_t *info
        cdef pmix_info_t **info_ptr
        cdef size_t sz
        cdef pmix_device_distance_t *distances
        cdef size_t ndist

        results = []

        # check that we loaded our topology
        if NULL == self.topo.topology:
            rc = self.load_topology()
            if PMIX_SUCCESS != rc:
                return (rc, results)

        # allocate and load pmix info structs from python list of dictionaries
        info_ptr = &info
        rc = pmix_alloc_info(info_ptr, &sz, dicts)
        if PMIX_SUCCESS != rc:
            return (rc, results)

        # convert the cpuset
        csetstr = pycpus['cpus'].encode('ascii')
        rc = PMIx_Parse_cpuset_string(csetstr, &cpuset);
        if PMIX_SUCCESS != rc:
            return (rc, results)

        # compute distances
        rc = PMIx_Compute_distances(&self.topo, &cpuset, info, sz, &distances, &ndist)
        if PMIX_SUCCESS != rc:
            return (rc, results)

        # convert to Python
        n = 0
        while n < ndist:
            pydist = {}
            pydist['uuid'] = strdup(distances[n].uuid)
            pydist['osname'] = strdup(distances[n].osname)
            pydist['mindist'] = distances[n].mindist
            pydist['maxdist'] = distances[n].maxdist
            results.append(pydist)
            n += 1

        # free the memory
        PyMem_Free(distances)

        # return result
        return (rc, results)

    def progress(self):
        PMIx_Progress()
        return

pmixservermodule = {}
def setmodulefn(k, f):
    global pmixservermodule
    permitted = ['clientconnected', 'clientfinalized', 'abort',
                 'fencenb', 'directmodex', 'publish', 'lookup', 'unpublish',
                 'spawn', 'connect', 'disconnect', 'registerevents',
                 'deregisterevents', 'listener', 'notify_event', 'query',
                 'toolconnected', 'log', 'allocate', 'jobcontrol',
                 'monitor', 'getcredential', 'validatecredential',
                 'iofpull', 'pushstdin', 'group', 'fabric', 'sessioncontrol']
    if k not in permitted:
        return PMIX_ERR_BAD_PARAM
    if not k in pmixservermodule:
        pmixservermodule[k] = f

cdef class PMIxServer(PMIxClient):
    cdef pmix_server_module_t myserver

    def __cinit__(self):
        self.fabric_set = 0
        memset(self.myproc.nspace, 0, sizeof(self.myproc.nspace))
        self.myproc.rank = PMIX_RANK_UNDEF

    # Initialize the PMIx server library
    #
    # @dicts [INPUT]
    #          - a list of dictionaries, where each
    #            dictionary has a key, value, and val_type
    #            defined as such:
    #            [{key:y, value:val, val_type:ty}, … ]
    #
    # @map [INPUT]
    #          - a dictionary of key-function pairs that map
    #            server module callback functions to provided
    #            implementations
    def init(self, dicts:list, map:dict):
        cdef pmix_info_t *info
        cdef pmix_info_t **info_ptr
        cdef size_t sz

        # setup server module
        self.server_module_init()
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

        # allocate and load pmix info structs from python list of dictionaries
        info_ptr = &info
        rc = pmix_alloc_info(info_ptr, &sz, dicts)
        if sz > 0:
            rc = PMIx_server_init(&self.myserver, info, sz)
        else:
            rc = PMIx_server_init(&self.myserver, NULL, 0)
        return rc

    def server_module_init(self):
        # v1.x interfaces
        self.myserver.client_connected2 = <pmix_server_client_connected2_fn_t>clientconnected
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
        self.myserver.fabric = <pmix_server_fabric_fn_t>fabric
        # v5.x interfaces
        self.myserver.session_control = <pmix_server_session_control_fn_t>sessioncontrol

    # Allow a tool to set server module callback functions
    # when it needs to also act as a server
    def set_server_module(self, map:dict):
        # setup server module
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
        return PMIX_SUCCESS

    def finalize(self):
        # finalize
        return PMIx_server_finalize()

    def generate_regex(self, hosts:list):
        cdef char *regex;
        mycomma = ","
        myhosts = mycomma.join(hosts)
        pyhosts = myhosts.encode('ascii')
        rc = PMIx_generate_regex(pyhosts, &regex)
        # load regex appropriately into python bytearray
        ba = pmix_convert_regex(regex)
        return (rc, ba)

    def generate_ppn(self, procs:list):
        cdef char *ppn;
        mysemi = ";"
        myprocs = mysemi.join(procs)
        pyprocs = myprocs.encode('ascii')
        rc = PMIx_generate_ppn(pyprocs, &ppn)
        if "pmix" == ppn[:4].decode("ascii"):
            if b'\x00' in ppn:
                ppn.replace(b'\x00', '')
            ba = bytearray(ppn)
        elif "blob" == ppn[:4].decode("ascii"):
            sz_str    = len(ppn)
            sz_prefix = 5
            # extract length of bytearray
            ppn.split(b'\x00')
            len_bytearray = ppn[1]
            length = len(len_bytearray) + sz_prefix + sz_str
            ba = bytearray(length)
            index = 0
            pyppn = <bytes> ppn[:length]
            while index < length:
                ba[index] = pyppn[index]
                index += 1
        else:
            # last case with no ':' in string
            ba = bytearray(ppn)
        return (rc, ba)

    def generate_cpuset_string(self, cpuset:dict):
        return (PMIX_ERR_NOT_SUPPORTED, None)

    def generate_locality_string(self, cpuset:dict):
        return (PMIX_ERR_NOT_SUPPORTED, None)

    # Register a namespace
    #
    # @ns [INPUT]
    #     - Namespace of job (string)
    #
    # @nlocalprocs [INPUT]
    #              - number of local procs for this job (int)
    #
    # @dicts [INPUT]
    #          - a list of dictionaries, where each
    #            dictionary has a key, value, and val_type
    #            defined as such:
    #            [{key:y, value:val, val_type:ty}, … ]
    #
    def register_nspace(self, ns:str, nlocalprocs:int, dicts:list):
        cdef pmix_nspace_t nspace
        cdef pmix_info_t *info
        cdef pmix_info_t **info_ptr
        cdef size_t sz
        global active
        # convert the args into the necessary C-arguments
        pmix_copy_nspace(nspace, ns)

        # allocate and load pmix info structs from python list of dictionaries
        info_ptr = &info
        rc = pmix_alloc_info(info_ptr, &sz, dicts)

        if sz > 0:
            rc = PMIx_server_register_nspace(nspace, nlocalprocs, info, sz, NULL, NULL)
        else:
            rc = PMIx_server_register_nspace(nspace, nlocalprocs, NULL, 0, NULL, NULL)
        return rc

    # Deregister a namespace
    #
    # @ns [INPUT]
    #     - Namespace of job (string)
    #
    def deregister_nspace(self, ns:str):
        cdef pmix_nspace_t nspace
        global active
        # convert the args into the necessary C-arguments
        pmix_copy_nspace(nspace, ns)
        PMIx_server_deregister_nspace(nspace, NULL, NULL)
        return

    # Register resources
    def register_resources(directives:list):
        cdef pmix_info_t *info
        cdef pmix_info_t **info_ptr
        cdef size_t sz

        # allocate and load pmix info structs from python list of dictionaries
        info_ptr = &info
        rc = pmix_alloc_info(info_ptr, &sz, directives)
        if PMIX_SUCCESS != rc:
            return rc

        rc = PMIx_server_register_resources(info, sz, NULL, NULL)
        return rc

    # Deregister resources
    def deregister_resources(directives:list):
        cdef pmix_info_t *info
        cdef pmix_info_t **info_ptr
        cdef size_t sz

        # allocate and load pmix info structs from python list of dictionaries
        info_ptr = &info
        rc = pmix_alloc_info(info_ptr, &sz, directives)
        if PMIX_SUCCESS != rc:
            return rc

        rc = PMIx_server_deregister_resources(info, sz, NULL, NULL)
        return rc

    # Register a client process
    #
    # @proc [INPUT]
    #       - namespace and rank of the client (dict)
    #
    # @uid [INPUT]
    #      - User ID (uid) of the client (int)
    #
    # @gid [INPUT]
    #      - Group ID (gid) of the client (int)
    #
    def register_client(self, proc:dict, uid:int, gid:int):
        global active
        cdef pmix_proc_t p;
        pmix_copy_nspace(p.nspace, proc['nspace'])
        p.rank = proc['rank']
        rc = PMIx_server_register_client(&p, uid, gid, NULL, NULL, NULL)
        return rc

    # Deregister a client process
    #
    # @proc [INPUT]
    #       - namespace and rank of the client (dict)
    #
    def deregister_client(self, proc:dict):
        global active
        cdef pmix_proc_t p;
        pmix_copy_nspace(p.nspace, proc['nspace'])
        p.rank = proc['rank']
        rc = PMIx_server_deregister_client(&p, NULL, NULL)
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
    def setup_fork(self, proc:dict, envin:dict):
        cdef pmix_proc_t p;
        cdef char **penv = NULL;
        cdef unicode pstring
        pmix_copy_nspace(p.nspace, proc['nspace'])
        p.rank = proc['rank']
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
        pmix_copy_nspace(p.nspace, proc['nspace'])
        p.rank = proc['rank']
        active.clear()
        pybo = (None, 0)
        rc = PMIx_server_dmodex_request(&p, dmodx_cbfunc, NULL);
        if PMIX_SUCCESS == rc:
            active.wait()
            # transfer the data to the dictionary
            (data, sz) = active.fetch_data()
            pybo = (data, sz)
        return rc, pybo

    def setup_application(self, ns:str, dicts:list):
        global active
        cdef pmix_nspace_t nspace;
        cdef pmix_info_t *info
        cdef pmix_info_t **info_ptr
        cdef size_t sz
        dataout = []
        pmix_copy_nspace(nspace, ns)

        # allocate and load pmix info structs from python list of dictionaries
        info_ptr = &info
        rc = pmix_alloc_info(info_ptr, &sz, dicts)

        active.clear()
        rc = PMIx_server_setup_application(nspace, info, sz, setupapp_cbfunc, NULL);
        if PMIX_SUCCESS == rc:
            active.wait()
            # transfer the data to the dictionary
            active.fetch_info(dataout)
        return (rc, dataout)

    def register_attributes(function:str, attrs:list):
        cdef size_t nattrs
        cdef char *func
        cdef char **attarray
        nattrs    = 0
        func      = strdup(function)

        if attrs is not None:
            nattrs = len(attrs)
            if 0 < nattrs:
                # allocate and load list of strings into regattrs struct
                attarray = <char **> PyMem_Malloc((nattrs+1) * sizeof(char*))
                if not attarray:
                    return PMIX_ERR_NOMEM
                rc = pmix_load_argv(attarray, attrs)
                if PMIX_SUCCESS != rc:
                    PyMem_Free(attarray)
                    return rc
            else:
                return PMIX_SUCCESS
        else:
            return PMIX_SUCCESS

        # call Server API
        rc = PMIx_Register_attributes(func, attarray)

        if 0 < nattrs:
            PyMem_Free(attarray)
        if func != NULL:
            PyMem_Free(func)
        return PMIX_SUCCESS

    def collect_inventory(pydirs:list):
        cdef pmix_info_t *directives
        cdef pmix_info_t **directives_ptr
        cdef size_t ndirs
        ndirs   = 0
        dataout = []

        # allocate and load pmix info structs from python list of dictionaries
        directives_ptr = &directives
        rc = pmix_alloc_info(directives_ptr, &ndirs, pydirs)

        # call the API
        active.clear()
        rc = PMIx_server_collect_inventory(directives, ndirs,
                                           collectinventory_cbfunc, NULL)
        if PMIX_SUCCESS == rc:
            active.wait()
            # transfer the data to the dictionary
            active.fetch_info(dataout)
        return (rc, dataout)

    def deliver_inventory(pyinfo:list, pydirs:list):
        cdef pmix_info_t *directives
        cdef pmix_info_t **directives_ptr
        cdef pmix_info_t *info
        cdef pmix_info_t **info_ptr
        cdef size_t ndirs
        cdef size_t ninfo
        ndirs   = 0
        ninfo   = 0

        # allocate and load pmix info structs from python list of dictionaries
        directives_ptr = &directives
        rc = pmix_alloc_info(directives_ptr, &ndirs, pydirs)
        info_ptr = &info
        rc = pmix_alloc_info(info_ptr, &ninfo, pyinfo)

        # call the API
        rc = PMIx_server_deliver_inventory(info, ninfo, directives, ndirs,
                                           NULL, NULL)
        return rc

    def setup_local_support(self, ns:str, ilist:list):
        global active
        cdef pmix_nspace_t nspace;
        cdef pmix_info_t *info
        cdef pmix_info_t **info_ptr
        cdef size_t sz
        pmix_copy_nspace(nspace, ns)
        # convert the info list
        info_ptr = &info
        rc = pmix_alloc_info(info_ptr, &sz, ilist)
        if PMIX_SUCCESS != rc:
            return rc
        if sz > 0:
            rc = PMIx_server_setup_local_support(nspace, info, sz, NULL, NULL);
        else:
            rc = PMIx_server_setup_local_support(nspace, NULL, 0, NULL, NULL);
        if PMIX_SUCCESS == rc:
            active.wait()
        return rc

    def iof_deliver(pysrc:dict, pychannel:int, pydata:dict, pydirs:list):
        cdef pmix_proc_t *source
        cdef pmix_iof_channel_t channel
        cdef pmix_byte_object_t *bo
        cdef pmix_info_t *directives
        cdef pmix_info_t **directives_ptr
        cdef size_t ndirs
        source  = NULL
        ndirs   = 0
        channel = pychannel

        # convert pysrc to pmix_proc_t
        pmix_copy_nspace(source[0].nspace, pysrc['nspace'])
        source[0].rank = pysrc['rank']

        # convert pydata to pmix_byte_object_t
        bo = <pmix_byte_object_t*>PyMem_Malloc(sizeof(pmix_byte_object_t))
        if not bo:
            return PMIX_ERR_NOMEM
        data = bytes(pydata['bytes'], 'ascii')
        bo.size = sizeof(data)
        bo.bytes = <char*> PyMem_Malloc(bo.size)
        if not bo.bytes:
            return PMIX_ERR_NOMEM
        pyptr = <const char*>data
        memcpy(bo.bytes, pyptr, bo.size)

        # allocate and load pmix info structs from python list of dictionaries
        directives_ptr = &directives
        rc = pmix_alloc_info(directives_ptr, &ndirs, pydirs)

        # call API
        rc = PMIx_server_IOF_deliver(source, channel, bo, directives, ndirs,
                                     NULL, NULL)
        return rc

    def define_process_set(members:list, name:str):
        cdef pmix_proc_t *procs
        cdef size_t nprocs
        nprocs = 0

        # convert set name
        pyset = name.encode('ascii')
        # convert list of procs to array of pmix_proc_t's
        if members is None:
            return PMIX_ERR_BAD_PARAM
        nprocs = len(members)
        procs = <pmix_proc_t*> PyMem_Malloc(nprocs * sizeof(pmix_proc_t))
        if not procs:
            return PMIX_ERR_NOMEM
        rc = pmix_load_procs(procs, members)
        if PMIX_SUCCESS != rc:
            pmix_free_procs(procs, nprocs)
            return rc
        # define the set
        rc = PMIx_server_define_process_set(procs, nprocs, pyset)
        pmix_free_procs(procs, nprocs)
        return rc

    def delete_process_set(name:str):

        # convert set name
        pyset = name.encode('ascii')
        # delete the set
        rc = PMIx_server_delete_process_set(pyset)
        return rc

    def session_control(sessionID:int, ilist:list):
        cdef pmix_info_t *info
        cdef pmix_info_t **info_ptr
        cdef size_t sz

        # allocate and load pmix info structs from python list of dictionaries
        info_ptr = &info
        rc = pmix_alloc_info(info_ptr, &sz, ilist)
        if PMIX_SUCCESS != rc:
            return rc

         # call the API
        if 0 < sz:
            rc = PMIx_Session_control(sessionID, info, sz, NULL, NULL)
            pmix_free_info(info, sz)
        else:
            rc = PMIx_Session_control(sessionID, NULL, 0, NULL, NULL)
        return rc

cdef int clientconnected(pmix_proc_t *proc, void *server_object,
                         pmix_op_cbfunc_t cbfunc, void *cbdata) with gil:
    keys = pmixservermodule.keys()
    if 'clientconnected' in keys:
        if not proc:
            return PMIX_ERR_BAD_PARAM
        myproc = []
        pmix_unload_procs(proc, 1, myproc)
        cbdata_dict = {'cbdata' : <uintptr_t> cbdata, 'cbfunc' : <uintptr_t> cbfunc}
        return pmixservermodule['clientconnected'](myproc[0], pypmix_op_cbfunc, cbdata_dict)
    
    return PMIX_ERR_NOT_FOUND

cdef int clientfinalized(pmix_proc_t *proc, void *server_object,
                         pmix_op_cbfunc_t cbfunc, void *cbdata) with gil:
    keys = pmixservermodule.keys()
    if 'clientfinalized' in keys:
        if not proc:
            return PMIX_ERR_BAD_PARAM
        myproc = []
        pmix_unload_procs(proc, 1, myproc)
        cbdata_dict = {'cbdata' : <uintptr_t> cbdata, 'cbfunc' : <uintptr_t> cbfunc}
        return pmixservermodule['clientfinalized'](myproc[0], pypmix_op_cbfunc, cbdata_dict)

    return PMIX_ERR_NOT_SUPPORTED

cdef int clientaborted(const pmix_proc_t *proc, void *server_object,
                       int status, const char msg[],
                       pmix_proc_t procs[], size_t nprocs,
                       pmix_op_cbfunc_t cbfunc, void *cbdata) with gil:
    keys = pmixservermodule.keys()
    if 'abort' in keys:
        args = {}
        myproc = []
        myprocs = []
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
        cbdata_dict = {'cbdata' : <uintptr_t> cbdata, 'cbfunc' : <uintptr_t> cbfunc}
        # upcall it
        return pmixservermodule['abort'](args, pypmix_op_cbfunc, cbdata_dict)
    
    return PMIX_ERR_NOT_SUPPORTED

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
        barray = None

        if NULL == procs:
            myprocs.append({'nspace': myname.nspace, 'rank': PMIX_RANK_WILDCARD})
        else:
            pmix_unload_procs(procs, nprocs, myprocs)
        args['procs'] = myprocs
        if NULL != info:
            rc = pmix_unload_info(info, ninfo, ilist)
            if PMIX_SUCCESS != rc:
                return rc
            args['directives'] = ilist
        if NULL != data:
            pmix_unload_bytes(data, ndata, blist)
            barray = bytearray(blist)
            args['data'] = barray
        cbdata_dict = {'cbdata' : <uintptr_t> cbdata, 'cbfunc' : <uintptr_t> cbfunc}
        return pmixservermodule['fencenb'](args, pypmix_modex_cbfunc, cbdata_dict)
    
    return PMIX_ERR_NOT_SUPPORTED



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
            args['directives'] = ilist
        cbdata_dict = {'cbdata' : <uintptr_t> cbdata, 'cbfunc' : <uintptr_t> cbfunc}
        return pmixservermodule['directmodex'](args, pypmix_modex_cbfunc, cbdata_dict)

    return PMIX_ERR_NOT_SUPPORTED

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
        args['directives'] = ilist
        cbdata_dict = {'cbdata' : <uintptr_t> cbdata, 'cbfunc' : <uintptr_t> cbfunc}
        return pmixservermodule['publish'](args, pypmix_op_cbfunc, cbdata_dict)

    return PMIX_ERR_NOT_SUPPORTED

cdef int lookup(const pmix_proc_t *proc, char **keys,
                const pmix_info_t info[], size_t ninfo,
                pmix_lookup_cbfunc_t cbfunc, void *cbdata) with gil:
    srvkeys = pmixservermodule.keys()
    if 'lookup' in srvkeys:
        args = {}
        pdata   = []
        myprocs = []
        ilist = []
        pykeys = []
        pmix_unload_procs(proc, 1, myprocs)
        args['proc'] = myprocs[0]
        n = 0
        while NULL != keys[n]:
            pykeys.append(keys[n])
            n += 1
        args['keys'] = pykeys
        if NULL != info:
            pmix_unload_info(info, ninfo, ilist)
            args['directives'] = ilist
        cbdata_dict = {'cbdata' : <uintptr_t> cbdata, 'cbfunc' : <uintptr_t> cbfunc}
        return pmixservermodule['lookup'](args, pypmix_lookup_cbfunc, cbdata_dict)
        
    return PMIX_ERR_NOT_SUPPORTED

cdef int unpublish(const pmix_proc_t *proc, char **keys,
                   const pmix_info_t info[], size_t ninfo,
                   pmix_op_cbfunc_t cbfunc, void *cbdata) with gil:
    srvkeys = pmixservermodule.keys()
    if 'unpublish' in srvkeys:
        args = {}
        myprocs = []
        ilist = []
        pykeys = []
        pmix_unload_procs(proc, 1, myprocs)
        args['proc'] = myprocs[0]
        if NULL != keys:
            pmix_unload_argv(keys, pykeys)
            args['keys'] = pykeys
        if NULL != info:
            pmix_unload_info(info, ninfo, ilist)
            args['directives'] = ilist
        cbdata_dict = {'cbdata' : <uintptr_t> cbdata, 'cbfunc' : <uintptr_t> cbfunc}
        return pmixservermodule['unpublish'](args, pypmix_op_cbfunc, cbdata_dict)
    
    return PMIX_ERR_NOT_SUPPORTED

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
        cbdata_dict = {'cbdata' : <uintptr_t> cbdata, 'cbfunc' : <uintptr_t> cbfunc}
        return pmixservermodule['spawn'](args, pypmix_spawn_cbfunc, cbdata_dict)

    return PMIX_ERR_NOT_SUPPORTED


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
            args['directives'] = ilist
        cbdata_dict = {'cbdata' : <uintptr_t> cbdata, 'cbfunc' : <uintptr_t> cbfunc}
        return pmixservermodule['connect'](args, pypmix_op_cbfunc, cbdata_dict)

    return PMIX_ERR_NOT_SUPPORTED

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
            args['directives'] = ilist
        cbdata_dict = {'cbdata' : <uintptr_t> cbdata, 'cbfunc' : <uintptr_t> cbfunc}
        return pmixservermodule['disconnect'](args, pypmix_op_cbfunc, cbdata_dict)

    return PMIX_ERR_NOT_SUPPORTED

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
            args['directives'] = ilist
        cbdata_dict = {'cbdata' : <uintptr_t> cbdata, 'cbfunc' : <uintptr_t> cbfunc}
        return pmixservermodule['registerevents'](args, pypmix_op_cbfunc, cbdata_dict)

    return PMIX_ERR_NOT_SUPPORTED

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
        cbdata_dict = {'cbdata' : <uintptr_t> cbdata, 'cbfunc' : <uintptr_t> cbfunc}
        return pmixservermodule['deregisterevents'](args, pypmix_op_cbfunc, cbdata_dict)

    return PMIX_ERR_NOT_SUPPORTED


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
            args['directives'] = ilist
        cbdata_dict = {'cbdata' : <uintptr_t> cbdata, 'cbfunc' : <uintptr_t> cbfunc}
        return pmixservermodule['notifyevent'](args, pypmix_op_cbfunc, cbdata_dict)
    
    return PMIX_ERR_NOT_SUPPORTED


cdef int query(pmix_proc_t *source,
               pmix_query_t *queries, size_t nqueries,
               pmix_info_cbfunc_t cbfunc,
               void *cbdata) with gil:
    pyqueries = []
    keys = pmixservermodule.keys()
    if 'query' in keys:
        args = {}
        myproc = []
        if NULL == queries or NULL == source:
            return PMIX_ERR_BAD_PARAM
        pmix_unload_queries(queries, nqueries, pyqueries)
        args['queries'] = pyqueries
        pmix_unload_procs(source, 1, myproc)
        args['source'] = myproc[0]
        cbdata_dict = {'cbdata' : <uintptr_t> cbdata, 'cbfunc' : <uintptr_t> cbfunc}
        return pmixservermodule['query'](args, pypmix_info_cbfunc, cbdata_dict)

    return PMIX_ERR_NOT_SUPPORTED

cdef void toolconnected(pmix_info_t *info, size_t ninfo,
                        pmix_tool_connection_cbfunc_t cbfunc,
                        void *cbdata) with gil:
    keys = pmixservermodule.keys()
    ret_proc = {'nspace': "UNDEF", 'rank': PMIX_RANK_UNDEF}
    if 'toolconnected' in keys:
        args = {}
        ilist = []
        if NULL != info:
            pmix_unload_info(info, ninfo, ilist)
            args['directives'] = ilist
        cbdata_dict = {'cbdata' : <uintptr_t> cbdata, 'cbfunc' : <uintptr_t> cbfunc}
        pmixservermodule['toolconnected'](args, pypmix_tool_connection_cbfunc, cbdata_dict)

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
        if NULL == client:
            return
        pmix_unload_procs(client, 1, myproc)
        args['source'] = myproc[0]
        if NULL != data:
            pmix_unload_info(data, ndata, ilist)
            args['data'] = ilist
        if NULL != directives:
            pmix_unload_info(directives, ndirs, mydirs)
            args['directives'] = mydirs
        cbdata_dict = {'cbdata' : <uintptr_t> cbdata, 'cbfunc' : <uintptr_t> cbfunc}
        pmixservermodule['log'](args, pypmix_op_cbfunc, cbdata_dict)

cdef int allocate(const pmix_proc_t *client,
                  pmix_alloc_directive_t action,
                  const pmix_info_t directives[], size_t ndirs,
                  pmix_info_cbfunc_t cbfunc, void *cbdata) with gil:
    keys = pmixservermodule.keys()
    if 'allocate' in keys:
        args = {}
        myproc = []
        mydirs = {}
        keyvals = []
        if NULL == client:
            return PMIX_ERR_BAD_PARAM
        pmix_unload_procs(client, 1, myproc)
        args['source'] = myproc[0]
        args['action'] = action
        if NULL != directives:
            pmix_unload_info(directives, ndirs, keyvals)
            args['directives'] = keyvals
        cbdata_dict = {'cbdata' : <uintptr_t> cbdata, 'cbfunc' : <uintptr_t> cbfunc}
        return pmixservermodule['allocate'](args, pypmix_info_cbfunc, cbdata_dict)
    
    return PMIX_ERR_NOT_SUPPORTED


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
            args['source'] = myproc[0]
        if NULL != targets:
            pmix_unload_procs(targets, ntargets, mytargets)
            args['targets'] = mytargets
        if NULL != directives:
            pmix_unload_info(directives, ndirs, mydirs)
            args['directives'] = mydirs
        cbdata_dict = {'cbdata' : <uintptr_t> cbdata, 'cbfunc' : <uintptr_t> cbfunc}
        return pmixservermodule['jobcontrol'](args, pypmix_info_cbfunc, cbdata_dict)

    return PMIX_ERR_NOT_SUPPORTED

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
        if NULL == monitor:
            return PMIX_ERR_BAD_PARAM
        if NULL != requestor:
            pmix_unload_procs(requestor, 1, myproc)
            args['source'] = myproc[0]
        pmix_unload_info(monitor, 1, mymon)
        args['monitor'] = mymon
        args['error'] = error
        if NULL != directives:
            pmix_unload_info(directives, ndirs, mydirs)
            args['directives'] = mydirs
        cbdata_dict = {'cbdata' : <uintptr_t> cbdata, 'cbfunc' : <uintptr_t> cbfunc}
        return pmixservermodule['monitor'](args, pypmix_info_cbfunc, cbdata_dict)

    return PMIX_ERR_NOT_SUPPORTED

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
            args['source'] = myproc[0]
        if NULL != directives:
            pmix_unload_info(directives, ndirs, mydirs)
            args['directives'] = mydirs
        cbdata_dict = {'cbdata' : <uintptr_t> cbdata, 'cbfunc' : <uintptr_t> cbfunc}
        return pmixservermodule['getcredential'](args, pypmix_credential_cbfunc, cbdata_dict)

    return PMIX_ERR_NOT_SUPPORTED
   
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
        pycred = {}
        if NULL != proc:
            pmix_unload_procs(proc, 1, myproc)
            args['source'] = myproc[0]
        if NULL != cred:
            pmix_unload_bytes(cred[0].bytes, cred[0].size, blist)
            barray = bytearray(blist)
            pycred['bytes'] = barray
            pycred['size'] = cred[0].size
            args['credential'] = pycred
        if NULL != directives:
            pmix_unload_info(directives, ndirs, mydirs)
            args['directives'] = mydirs
        cbdata_dict = {'cbdata' : <uintptr_t> cbdata, 'cbfunc' : <uintptr_t> cbfunc}
        return pmixservermodule['validatecredential'](args, pypmix_validation_cbfunc, cbdata_dict)

    return PMIX_ERR_NOT_SUPPORTED       

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
        pychannels = int(channels)
        args['channels'] = channels
        if NULL != procs:
            pmix_unload_procs(procs, nprocs, myprocs)
            args['sources'] = myprocs
        if NULL != directives:
            pmix_unload_info(directives, ndirs, mydirs)
            args['directives'] = mydirs
        cbdata_dict = {'cbdata' : <uintptr_t> cbdata, 'cbfunc' : <uintptr_t> cbfunc}
        return pmixservermodule['iofpull'](args, pypmix_op_cbfunc, cbdata_dict)

    return PMIX_ERR_NOT_SUPPORTED

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
        blist = []
        pyload = {}
        if NULL != source:
            pmix_unload_procs(source, 1, myproc)
            args['source'] = myproc[0]
        if NULL != targets:
            pmix_unload_procs(targets, ntargets, mytargets)
            args['targets'] = mytargets
        if NULL != directives:
            pmix_unload_info(directives, ndirs, mydirs)
            args['directives'] = mydirs
        if NULL != bo:
            pmix_unload_bytes(bo[0].bytes, bo[0].size, blist)
            barray = bytearray(blist)
            pyload['bytes'] = barray
            pyload['size'] = bo[0].size
            args['payload'] = pyload
        cbdata_dict = {'cbdata' : <uintptr_t> cbdata, 'cbfunc' : <uintptr_t> cbfunc}
        return pmixservermodule['pushstdin'](args, pypmix_op_cbfunc, cbdata_dict)

    return PMIX_ERR_NOT_SUPPORTED
    

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
        args['group'] = str(grp)
        pmix_unload_procs(procs, nprocs, myprocs)
        args['procs'] = myprocs
        if NULL != directives:
            pmix_unload_info(directives, ndirs, mydirs)
            args['directives'] = mydirs
        cbdata_dict = {'cbdata' : <uintptr_t> cbdata, 'cbfunc' : <uintptr_t> cbfunc}
        return pmixservermodule['group'](args, pypmix_info_cbfunc, cbdata_dict)

    return PMIX_ERR_NOT_SUPPORTED

cdef int fabric(const pmix_proc_t *requestor,
                pmix_fabric_operation_t op,
                const pmix_info_t directives[],
                size_t ndirs,
                pmix_info_cbfunc_t cbfunc,
                void *cbdata) with gil:
    keys = pmixservermodule.keys()
    if 'fabric' in keys:
        args = {}
        keyvals = {}
        myprocs = []
        mydirs = {}
        args['op'] = op
        if NULL != directives:
            pmix_unload_info(directives, ndirs, mydirs)
            args['directives'] = mydirs
        cbdata_dict = {'cbdata' : <uintptr_t> cbdata, 'cbfunc' : <uintptr_t> cbfunc}
        return pmixservermodule['fabric'](args, pypmix_info_cbfunc, cbdata_dict)

    return PMIX_ERR_NOT_SUPPORTED

cdef int sessioncontrol(const pmix_proc_t *requestor,
                        uint32_t sessionID,
                        const pmix_info_t directives[], size_t ndirs,
                        pmix_info_cbfunc_t cbfunc, void *cbdata) with gil:
    keys = pmixservermodule.keys()
    if 'sessioncontrol' in keys:
        args = {}
        myproc = []
        blist = []
        ilist = []
        barray = None

        if NULL == requestor:
            return PMIX_ERR_BAD_PARAM
        pmix_unload_procs(requestor, 1, myproc)
        args['requestor'] = myproc[0]
        args['sessionID'] = sessionID
        if NULL != directives:
            rc = pmix_unload_info(directives, ndirs, ilist)
            if PMIX_SUCCESS != rc:
                return rc
            args['directives'] = ilist
        cbdata_dict = {'cbdata' : <uintptr_t> cbdata, 'cbfunc' : <uintptr_t> cbfunc}
        return pmixservermodule['sessioncontrol'](args, pypmix_info_cbfunc, cbdata_dict)

    return PMIX_ERR_NOT_SUPPORTED


cdef class PMIxTool(PMIxServer):
    def __cinit__(self):
        memset(self.myproc.nspace, 0, sizeof(self.myproc.nspace))
        self.myproc.rank = PMIX_RANK_UNDEF

    # Initialize the PMIx tool library
    #
    # @dicts [INPUT]
    #          - a list of dictionaries, where each
    #            dictionary has a key, value, and val_type
    #            defined as such:
    #            [{key:y, value:val, val_type:ty}, … ]
    def init(self, dicts:list):
        cdef pmix_info_t *info
        cdef pmix_info_t **info_ptr
        cdef size_t sz
        global myname

        # init myname
        myname = {'nspace':'UNASSIGNED', 'rank':PMIX_RANK_UNDEF}

        # init server module in case the tool uses it
        self.server_module_init()

        # allocate and load pmix info structs from python list of dictionaries
        info_ptr = &info
        rc = pmix_alloc_info(info_ptr, &sz, dicts)
        if PMIX_SUCCESS != rc:
            return rc, myname

        if sz > 0:
            rc = PMIx_tool_init(&self.myproc, info, sz)
            pmix_free_info(info, sz)
        else:
            rc = PMIx_tool_init(&self.myproc, NULL, 0)
        if PMIX_SUCCESS == rc:
            # convert the returned name
            myname = {'nspace': (<bytes>self.myproc.nspace).decode('UTF-8'), 'rank': self.myproc.rank}
            rc = PMIx_tool_set_server_module(&self.myserver);
        return rc, myname

    # Finalize the tool library
    def finalize(self):
        # finalize
        rc = PMIx_tool_finalize()
        return rc

    # see if the tool is connected
    def is_connected(self):
        return PMIx_tool_is_connected()

    # Disconnect from a server
    def disconnect(server:dict):
        cdef pmix_proc_t srvr

        # convert the server name
        pmix_copy_nspace(srvr.nspace, server['nspace'])
        srvr.rank = server['rank']

        # perform disconnect
        rc = PMIx_tool_disconnect(&srvr);
        return rc

    # Connect to a server
    #
    # @dicts [INPUT]
    #          - a list of dictionaries, where each
    #            dictionary has a key, value, and val_type
    #            defined as such:
    #            [{key:y, value:val, val_type:ty}, … ]
    def attach_to_server(self, dicts:list):
        cdef pmix_info_t *info
        cdef pmix_info_t **info_ptr
        cdef size_t sz
        cdef pmix_proc_t srvr

        # allocate and load pmix info structs from python list of dictionaries
        info_ptr = &info
        rc = pmix_alloc_info(info_ptr, &sz, dicts)

        if sz > 0:
            rc = PMIx_tool_attach_to_server(&self.myproc, &srvr, info, sz)
            pmix_free_info(info, sz)
        else:
            rc = PMIx_tool_attach_to_server(&self.myproc, &srvr, NULL, 0)
        if PMIX_SUCCESS == rc:
            # convert the returned name
            myname = {'nspace': (<bytes>self.myproc.nspace).decode('UTF-8'), 'rank': self.myproc.rank}
            mysrvr = {'nspace': (<bytes>srvr.nspace).decode('UTF-8'), 'rank': srvr.rank}
            return PMIX_SUCCESS, myname, mysrvr
        else:
            return rc, None, None

    def get_servers(self):
        cdef pmix_proc_t *servers
        cdef size_t nservers

        pysrvrs = []
        rc = PMIx_tool_get_servers(&servers, &nservers)
        if PMIX_SUCCESS != rc:
            return rc, pysrvrs
        rc = pmix_unload_procs(servers, nservers, pysrvrs)
        PyMem_Free(servers)
        return rc, pysrvrs

    def set_server(self, server:dict, pyinfo:list):
        cdef pmix_proc_t srvr
        cdef pmix_info_t *info
        cdef pmix_info_t **info_ptr
        cdef size_t ninfo

        # convert the server name
        pmix_copy_nspace(srvr.nspace, server['nspace'])
        srvr.rank = server['rank']

        # allocate and load pmix info structs from python list of dictionaries
        info_ptr = &info
        rc = pmix_alloc_info(info_ptr, &ninfo, pyinfo)
        if PMIX_SUCCESS != rc:
            if 0 < ninfo:
                pmix_free_info(info, ninfo)
            return rc

        # perform op
        rc = PMIx_tool_set_server(&srvr, info, ninfo);
        if 0 < ninfo:
            pmix_free_info(info, ninfo)
        return rc

    def iof_pull(self, pyprocs:list, iof_channel:int, pydirs:list, hdlr):
        cdef pmix_proc_t *procs
        cdef pmix_info_t *directives
        cdef pmix_info_t **directives_ptr
        cdef pmix_iof_channel_t channel
        cdef size_t ndirs
        cdef size_t nprocs
        nprocs      = 0
        ndirs       = 0
        channel     = iof_channel
        cdef pmix_status_t pmix_rc

        # convert list of procs to array of pmix_proc_t's
        if pyprocs is not None:
            nprocs = len(pyprocs)
            procs = <pmix_proc_t*> PyMem_Malloc(nprocs * sizeof(pmix_proc_t))
            if not procs:
                return PMIX_ERR_NOMEM, -1
            rc = pmix_load_procs(procs, pyprocs)
            if PMIX_SUCCESS != rc:
                pmix_free_procs(procs, nprocs)
                return rc, -1
        else:
            nprocs = 1
            procs = <pmix_proc_t*> PyMem_Malloc(nprocs * sizeof(pmix_proc_t))
            if not procs:
                return PMIX_ERR_NOMEM, -1
            pmix_copy_nspace(procs[0].nspace, self.myproc.nspace)
            procs[0].rank = PMIX_RANK_WILDCARD

        # allocate and load pmix info structs from python list of dictionaries
        directives_ptr = &directives
        rc = pmix_alloc_info(directives_ptr, &ndirs, pydirs)

        # Call the library
        with nogil:
             pmix_rc = PMIx_IOF_pull(procs, nprocs, directives, ndirs, channel,
                                     pyiofhandler,
                                     NULL, NULL)
        rc = pmix_rc
        if 0 < nprocs:
            pmix_free_procs(procs, nprocs)
        if 0 < ndirs:
            pmix_free_info(directives, ndirs)

        # if rc < 0, then there was an error
        if 0 > rc:
            return rc, -1

        # otherwise, this is our ref ID for this hdlr
        myhdlrs.append({'refid': rc, 'hdlr': hdlr})
        refid = rc
        rc = PMIX_SUCCESS
        return rc, refid

    def iof_deregister(self, regid:int, pydirs:list):
        cdef pmix_info_t *directives
        cdef pmix_info_t **directives_ptr
        cdef size_t ndirs
        cdef size_t iofhdlr
        ndirs       = 0
        iofhdlr     = regid

        # allocate and load pmix info structs from python list of dictionaries
        directives_ptr = &directives
        rc = pmix_alloc_info(directives_ptr, &ndirs, pydirs)

        # call the library
        rc = PMIx_IOF_deregister(iofhdlr, directives, ndirs, NULL, NULL)
        if 0 < ndirs:
            pmix_free_info(directives, ndirs)
        # remove our local hdlr
        found = False
        n = 0
        for h in myhdlrs and not found:
            try:
                if iofhdlr == h['refid']:
                    found = True
                    del myhdlrs[n]
                else :
                    n = n + 1
            except:
                pass
        return rc

    def iof_push(self, pytargets:list, data:dict, pydirs:list):
        cdef pmix_info_t *directives
        cdef pmix_info_t **directives_ptr
        cdef pmix_byte_object_t *bo
        cdef size_t ndirs
        cdef pmix_proc_t *targets
        ntargets    = 0
        ndirs       = 0

        # convert data to pmix_byte_object_t
        if data:
            bo = <pmix_byte_object_t*>malloc(sizeof(pmix_byte_object_t))
            if not bo:
                return PMIX_ERR_NOMEM
            cred = bytes(data['bytes'], 'ascii')
            bo.size = len(cred)
            bo.bytes = <char*> malloc(bo.size)
            if not bo.bytes:
                return PMIX_ERR_NOMEM
            pyptr = <const char*>cred
            memcpy(bo.bytes, pyptr, bo.size)
        else:
            bo = NULL

        # convert list of proc targets to array of pmix_proc_t's
        if pytargets is not None:
            ntargets = len(pytargets)
            targets = <pmix_proc_t*>malloc(ntargets * sizeof(pmix_proc_t))
            if not targets:
                return PMIX_ERR_NOMEM
            rc = pmix_load_procs(targets, pytargets)
            if PMIX_SUCCESS != rc:
                pmix_free_procs(targets, ntargets)
                return rc
        else:
            ntargets = 1
            targets = <pmix_proc_t*>malloc(ntargets * sizeof(pmix_proc_t))
            if not targets:
                return PMIX_ERR_NOMEM
            pmix_copy_nspace(targets[0].nspace, self.myproc.nspace)
            targets[0].rank = PMIX_RANK_WILDCARD

        # allocate and load pmix info structs from python list of dictionaries
        directives_ptr = &directives
        rc = pmix_alloc_info(directives_ptr, &ndirs, pydirs)

        # Call the library
        rc = PMIx_IOF_push(targets, ntargets, bo, directives, ndirs, NULL, NULL)
        if 0 < ntargets:
            pmix_free_procs(targets, ntargets)
        if 0 < ndirs:
            pmix_free_info(directives, ndirs)
        return rc

cdef class PMIxScheduler(PMIxTool):
    def __cinit__(self):
        memset(self.myproc.nspace, 0, sizeof(self.myproc.nspace))
        self.myproc.rank = PMIX_RANK_UNDEF

    # Initialize the PMIx tool library underneath the scheduler
    #
    # @dicts [INPUT]
    #          - a list of dictionaries, where each
    #            dictionary has a key, value, and val_type
    #            defined as such:
    #            [{key:y, value:val, val_type:ty}, … ]
    def init(self, dicts:list):
        cdef pmix_info_t *info
        cdef pmix_info_t **info_ptr
        cdef size_t sz
        global myname

        # init myname
        myname = {'nspace':'UNASSIGNED', 'rank':PMIX_RANK_UNDEF}

        # init server module in case the scheduler uses it
        self.server_module_init()

        # allocate and load pmix info structs from python list of dictionaries
        info_ptr = &info
        rc = pmix_alloc_info(info_ptr, &sz, dicts)
        if PMIX_SUCCESS != rc:
            return rc, myname

        if sz > 0:
            rc = PMIx_tool_init(&self.myproc, info, sz)
            pmix_free_info(info, sz)
        else:
            rc = PMIx_tool_init(&self.myproc, NULL, 0)
        if PMIX_SUCCESS == rc:
            # convert the returned name
            myname = {'nspace': (<bytes>self.myproc.nspace).decode('UTF-8'), 'rank': self.myproc.rank}
            rc = PMIx_tool_set_server_module(&self.myserver);
        return rc, myname

    # Finalize the tool library
    def finalize(self):
        # finalize
        rc = PMIx_tool_finalize()
        return rc

    # direct the RTE to instantiate a session
    def assign_session(sessionID:int, allocID:str, ilist:list, applist:list):
        cdef pmix_info_t *info
        cdef pmix_info_t **info_ptr
        cdef size_t sz
        # convert the info list
        info_ptr = &info
        rc = pmix_alloc_info(info_ptr, &sz, ilist)
        if PMIX_SUCCESS != rc:
            return rc
        if sz == 0:
            info = NULL
        # convert the app list

