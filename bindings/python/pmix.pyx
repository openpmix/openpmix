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
import traceback
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

###############################################
# Client-side non-blocking (async) operations
#
# A non-blocking PMIx API returns as soon as the request has been handed
# to the library, and reports its result later by executing a callback
# from the library's progress thread. Two things must therefore survive
# the Python method that started the operation:
#
#   * the C input arrays. PMIx does not copy them - the caller must keep
#     them valid until the callback fires - so they cannot be freed when
#     the method returns the way the blocking methods free theirs. They
#     are parked in the caddy below and released by the trampoline.
#   * the caller's Python callback and cbdata object. Nothing else holds
#     a reference to them, so they are stashed in a module-global
#     registry (the same technique the bindings use to keep event
#     handlers alive in 'myhdlrs') and the caddy carries only the
#     integer key. No PyObject pointer ever crosses into C.
#
# See docs/how-things-work/python_nonblocking.rst for the full picture.
cdef struct nbcbd:
    char *grp             # strdup'd group identifier
    char *ky              # strdup'd key (get_nb)
    char *blk             # strdup'd resource block name
    char **keys           # NULL-terminated argv (lookup_nb, unpublish_nb)
    pmix_proc_t *procs    # PyMem_Malloc'd by pmix_load_procs
    size_t nprocs
    pmix_info_t *info     # malloc'd by pmix_alloc_info
    size_t ninfo
    pmix_info_t *dirs     # malloc'd by pmix_alloc_info - the second info
    size_t ndirs          #   array carried by log/job_control/monitor
    pmix_app_t *apps      # PyMem_Malloc'd, loaded by pmix_load_apps
    size_t napps
    pmix_query_t *queries # PyMem_Malloc'd
    size_t nqueries
    pmix_resource_unit_t *units  # PyMem_Malloc'd by pmix_alloc_units
    size_t nunits
    pmix_byte_object_t *cred     # PyMem_Malloc'd, as is its payload
    pmix_cpuset_t cpuset  # constructed only when havecpuset is set
    int havecpuset
    size_t idx            # key into the pynbcbs registry
ctypedef nbcbd pypmix_nb_cbdata_t

# Registry of in-flight non-blocking operations. Guarded by pynblock as
# entries are added by the calling thread and removed by the progress
# thread.
pynbcbs = {}
pynbidx = 0
pynblock = threading.Lock()

def pypmix_nb_register(cbfunc, cbdata, obj=None):
    # Record a Python callback pair and return its registry key. 'obj' is
    # never read - it is held solely to keep an object alive for the
    # duration of the operation, which the calls that hand the library a
    # pointer into the class (the fabric object, the topology) rely on
    global pynbidx
    with pynblock:
        pynbidx += 1
        idx = pynbidx
        pynbcbs[idx] = {'cbfunc': cbfunc, 'cbdata': cbdata, 'obj': obj}
    return idx

def pypmix_nb_take(idx):
    # Remove and return a registry entry, or None if it is already gone
    with pynblock:
        return pynbcbs.pop(idx, None)

# Allocate a caddy for a non-blocking operation. Returns NULL on failure
cdef pypmix_nb_cbdata_t* pypmix_nb_cbdata_new():
    cdef pypmix_nb_cbdata_t *cd
    cd = <pypmix_nb_cbdata_t *> malloc(sizeof(pypmix_nb_cbdata_t))
    if NULL == cd:
        return NULL
    memset(cd, 0, sizeof(pypmix_nb_cbdata_t))
    return cd

# Release a caddy and everything it owns. The buffers it carries come
# from several different allocators - strdup/malloc for the strings and
# the info arrays, PyMem_Malloc for the arrays the bindings both build
# and release - so each must go back to its own
cdef void pypmix_nb_cbdata_free(pypmix_nb_cbdata_t *cd):
    if NULL == cd:
        return
    if NULL != cd.grp:
        free(cd.grp)
    if NULL != cd.ky:
        free(cd.ky)
    if NULL != cd.blk:
        free(cd.blk)
    if NULL != cd.keys:
        pmix_free_argv(cd.keys)
    if NULL != cd.procs:
        pmix_free_procs(cd.procs, cd.nprocs)
    if NULL != cd.info and 0 < cd.ninfo:
        pmix_free_info(cd.info, cd.ninfo)
    if NULL != cd.dirs and 0 < cd.ndirs:
        pmix_free_info(cd.dirs, cd.ndirs)
    if NULL != cd.apps:
        pmix_free_apps(cd.apps, cd.napps)
    if NULL != cd.queries:
        pmix_free_queries(cd.queries, cd.nqueries)
    if NULL != cd.units:
        pmix_free_units(cd.units, cd.nunits)
    if NULL != cd.cred:
        if NULL != cd.cred.bytes:
            PyMem_Free(cd.cred.bytes)
        PyMem_Free(cd.cred)
    if 0 != cd.havecpuset:
        pmix_destruct_cpuset(&cd.cpuset)
    free(cd)

# The registry entry doubles as the ownership token for the caddy:
# whoever pops it is responsible for releasing the caddy, and a pop that
# comes up empty means someone else already did. That keeps the starting
# method's error path and the trampoline from ever racing to free it.
#
# Trampoline for non-blocking APIs that take a pmix_op_cbfunc_t. This
# fires on the library's progress thread, which is not a Python thread,
# so the GIL must be acquired
cdef void pypmix_client_op_cbfunc(pmix_status_t status,
                                  void *cbdata) noexcept with gil:
    cdef pypmix_nb_cbdata_t *cd
    if NULL == cbdata:
        return
    cd = <pypmix_nb_cbdata_t *> cbdata
    entry = pypmix_nb_take(cd.idx)
    if entry is None:
        return
    pypmix_nb_cbdata_free(cd)
    # a callback that raises must not unwind into the library
    try:
        entry['cbfunc'](status, entry['cbdata'])
    except:
        traceback.print_exc()

# Trampoline for non-blocking APIs that take a pmix_info_cbfunc_t
cdef void pypmix_client_info_cbfunc(pmix_status_t status,
                                    pmix_info_t *info, size_t ninfo,
                                    void *cbdata,
                                    pmix_release_cbfunc_t release_fn,
                                    void *release_cbdata) noexcept with gil:
    cdef pypmix_nb_cbdata_t *cd
    # the results belong to the library, so convert them to Python before
    # telling it that we are done with them
    pyresults = []
    prc = PMIX_SUCCESS
    if NULL != info and 0 < ninfo:
        prc = pmix_unload_info(info, ninfo, pyresults)
    if NULL != release_fn:
        release_fn(release_cbdata)
    if PMIX_SUCCESS != prc and PMIX_SUCCESS == status:
        status = prc
    if NULL == cbdata:
        return
    cd = <pypmix_nb_cbdata_t *> cbdata
    entry = pypmix_nb_take(cd.idx)
    if entry is None:
        return
    pypmix_nb_cbdata_free(cd)
    # a callback that raises must not unwind into the library
    try:
        entry['cbfunc'](status, pyresults, entry['cbdata'])
    except:
        traceback.print_exc()

# Trampoline for non-blocking APIs that take a pmix_value_cbfunc_t. The
# value belongs to the library and is released as soon as we return, so
# convert it here rather than handing the pointer onward
cdef void pypmix_client_value_cbfunc(pmix_status_t status,
                                     pmix_value_t *kv,
                                     void *cbdata) noexcept with gil:
    cdef pypmix_nb_cbdata_t *cd
    pyval = None
    if NULL != kv:
        pyval = pmix_unload_value(kv)
    if NULL == cbdata:
        return
    cd = <pypmix_nb_cbdata_t *> cbdata
    entry = pypmix_nb_take(cd.idx)
    if entry is None:
        return
    pypmix_nb_cbdata_free(cd)
    # a callback that raises must not unwind into the library
    try:
        entry['cbfunc'](status, pyval, entry['cbdata'])
    except:
        traceback.print_exc()

# Trampoline for non-blocking APIs that take a pmix_lookup_cbfunc_t. As
# above, the returned pdata array is the library's
cdef void pypmix_client_lookup_cbfunc(pmix_status_t status,
                                      pmix_pdata_t data[], size_t ndata,
                                      void *cbdata) noexcept with gil:
    cdef pypmix_nb_cbdata_t *cd
    pyresults = []
    prc = PMIX_SUCCESS
    if NULL != data and 0 < ndata:
        prc = pmix_unload_pdata(data, ndata, pyresults)
    if PMIX_SUCCESS != prc and PMIX_SUCCESS == status:
        status = prc
    if NULL == cbdata:
        return
    cd = <pypmix_nb_cbdata_t *> cbdata
    entry = pypmix_nb_take(cd.idx)
    if entry is None:
        return
    pypmix_nb_cbdata_free(cd)
    # a callback that raises must not unwind into the library
    try:
        entry['cbfunc'](status, pyresults, entry['cbdata'])
    except:
        traceback.print_exc()

# Trampoline for non-blocking APIs that take a pmix_spawn_cbfunc_t
cdef void pypmix_client_spawn_cbfunc(pmix_status_t status,
                                     pmix_nspace_t nspace,
                                     void *cbdata) noexcept with gil:
    cdef pypmix_nb_cbdata_t *cd
    # the nspace is released by the library upon our return, so decode it
    # into a Python string now. A failed spawn has no namespace to report,
    # and the parameter is a bare array pointer that may be NULL
    pyns = None
    if PMIX_SUCCESS == status and NULL != nspace:
        pyns = nspace.decode('ascii')
    if NULL == cbdata:
        return
    cd = <pypmix_nb_cbdata_t *> cbdata
    entry = pypmix_nb_take(cd.idx)
    if entry is None:
        return
    pypmix_nb_cbdata_free(cd)
    # a callback that raises must not unwind into the library
    try:
        entry['cbfunc'](status, pyns, entry['cbdata'])
    except:
        traceback.print_exc()

# Trampoline for non-blocking APIs that take a pmix_credential_cbfunc_t
cdef void pypmix_client_credential_cbfunc(pmix_status_t status,
                                          pmix_byte_object_t *credential,
                                          pmix_info_t info[], size_t ninfo,
                                          void *cbdata) noexcept with gil:
    cdef pypmix_nb_cbdata_t *cd
    # both the credential and the info array belong to the library
    cred = {}
    if NULL != credential and NULL != credential.bytes and 0 < credential.size:
        blist = []
        pmix_unload_bytes(credential.bytes, credential.size, blist)
        cred['bytes'] = bytearray(blist)
        cred['size'] = credential.size
    pyresults = []
    prc = PMIX_SUCCESS
    if NULL != info and 0 < ninfo:
        prc = pmix_unload_info(info, ninfo, pyresults)
    if PMIX_SUCCESS != prc and PMIX_SUCCESS == status:
        status = prc
    if NULL == cbdata:
        return
    cd = <pypmix_nb_cbdata_t *> cbdata
    entry = pypmix_nb_take(cd.idx)
    if entry is None:
        return
    pypmix_nb_cbdata_free(cd)
    # a callback that raises must not unwind into the library
    try:
        entry['cbfunc'](status, cred, pyresults, entry['cbdata'])
    except:
        traceback.print_exc()

# Trampoline for non-blocking APIs that take a pmix_validation_cbfunc_t.
# Same shape as the info trampoline, but without a release function -
# the library reclaims the array itself
cdef void pypmix_client_validation_cbfunc(pmix_status_t status,
                                          pmix_info_t info[], size_t ninfo,
                                          void *cbdata) noexcept with gil:
    cdef pypmix_nb_cbdata_t *cd
    pyresults = []
    prc = PMIX_SUCCESS
    if NULL != info and 0 < ninfo:
        prc = pmix_unload_info(info, ninfo, pyresults)
    if PMIX_SUCCESS != prc and PMIX_SUCCESS == status:
        status = prc
    if NULL == cbdata:
        return
    cd = <pypmix_nb_cbdata_t *> cbdata
    entry = pypmix_nb_take(cd.idx)
    if entry is None:
        return
    pypmix_nb_cbdata_free(cd)
    # a callback that raises must not unwind into the library
    try:
        entry['cbfunc'](status, pyresults, entry['cbdata'])
    except:
        traceback.print_exc()

# Trampoline for non-blocking APIs that take a pmix_device_dist_cbfunc_t
cdef void pypmix_client_devdist_cbfunc(pmix_status_t status,
                                       pmix_device_distance_t *dist,
                                       size_t ndist,
                                       void *cbdata,
                                       pmix_release_cbfunc_t release_fn,
                                       void *release_cbdata) noexcept with gil:
    cdef pypmix_nb_cbdata_t *cd
    cdef size_t n
    # convert the distances before telling the library we are done with them
    pyresults = []
    n = 0
    while n < ndist:
        pydist = {}
        if NULL == dist[n].uuid:
            pydist['uuid'] = None
        else:
            pydist['uuid'] = dist[n].uuid.decode('ascii')
        if NULL == dist[n].osname:
            pydist['osname'] = None
        else:
            pydist['osname'] = dist[n].osname.decode('ascii')
        pydist['type'] = dist[n].type
        pydist['mindist'] = dist[n].mindist
        pydist['maxdist'] = dist[n].maxdist
        pyresults.append(pydist)
        n += 1
    if NULL != release_fn:
        release_fn(release_cbdata)
    if NULL == cbdata:
        return
    cd = <pypmix_nb_cbdata_t *> cbdata
    entry = pypmix_nb_take(cd.idx)
    if entry is None:
        return
    pypmix_nb_cbdata_free(cd)
    # a callback that raises must not unwind into the library
    try:
        entry['cbfunc'](status, pyresults, entry['cbdata'])
    except:
        traceback.print_exc()

# Build the caddy for a non-blocking operation, taking ownership of the
# info array. Returns NULL on failure, with the reason stored in rcptr
cdef pypmix_nb_cbdata_t* pypmix_nb_setup(pyinfo, int *rcptr):
    cdef pypmix_nb_cbdata_t *cd
    cdef pmix_info_t **info_ptr

    cd = pypmix_nb_cbdata_new()
    if NULL == cd:
        rcptr[0] = PMIX_ERR_NOMEM
        return NULL
    info_ptr = &cd.info
    rc = pmix_alloc_info(info_ptr, &cd.ninfo, pyinfo)
    if PMIX_SUCCESS != rc:
        pypmix_nb_cbdata_free(cd)
        rcptr[0] = rc
        return NULL
    rcptr[0] = PMIX_SUCCESS
    return cd

# Add the second info array carried by the operations that take one -
# the directives that accompany the data being logged, the targets being
# controlled, or the monitor being requested
cdef int pypmix_nb_add_dirs(pypmix_nb_cbdata_t *cd, pydirs):
    cdef pmix_info_t **dirs_ptr
    dirs_ptr = &cd.dirs
    return pmix_alloc_info(dirs_ptr, &cd.ndirs, pydirs)

# Park the operation's target procs in the caddy. As in the blocking
# forms, an absent or empty list means "this proc's entire job"
cdef int pypmix_nb_add_procs(pypmix_nb_cbdata_t *cd, peers, nspace):
    if peers is not None and 0 < len(peers):
        cd.nprocs = len(peers)
        cd.procs = <pmix_proc_t*> PyMem_Malloc(cd.nprocs * sizeof(pmix_proc_t))
        if not cd.procs:
            cd.nprocs = 0
            return PMIX_ERR_NOMEM
        return pmix_load_procs(cd.procs, peers)
    cd.nprocs = 1
    cd.procs = <pmix_proc_t*> PyMem_Malloc(sizeof(pmix_proc_t))
    if not cd.procs:
        cd.nprocs = 0
        return PMIX_ERR_NOMEM
    pmix_copy_nspace(cd.procs[0].nspace, nspace)
    cd.procs[0].rank = PMIX_RANK_WILDCARD
    return PMIX_SUCCESS

# Park a NULL-terminated key array in the caddy. A None/empty list is
# left as NULL, which the APIs that take one read as "all keys"
cdef int pypmix_nb_add_keys(pypmix_nb_cbdata_t *cd, pykeys):
    cdef size_t nstrings
    if pykeys is None or 0 == len(pykeys):
        return PMIX_SUCCESS
    nstrings = len(pykeys)
    cd.keys = <char **> malloc((nstrings + 1) * sizeof(char*))
    if NULL == cd.keys:
        return PMIX_ERR_NOMEM
    memset(cd.keys, 0, (nstrings + 1) * sizeof(char*))
    return pmix_load_argv(cd.keys, pykeys)

# Build the caddy for a non-blocking group operation, taking ownership of
# the group identifier and the info array. Returns NULL on failure, with
# the reason stored in rcptr
cdef pypmix_nb_cbdata_t* pypmix_nb_group_setup(pygrp, pyinfo, int *rcptr):
    cdef pypmix_nb_cbdata_t *cd

    cd = pypmix_nb_setup(pyinfo, rcptr)
    if NULL == cd:
        return NULL
    cd.grp = strdup(pygrp)
    if NULL == cd.grp:
        pypmix_nb_cbdata_free(cd)
        rcptr[0] = PMIX_ERR_NOMEM
        return NULL
    return cd

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
        size = byteobject['size']
        c_byteobject.size = size
        c_byteobject.bytes = <char *> malloc(size)
        memcpy(c_byteobject.bytes, <void *> byteobject['bytes'], size)

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
        cdef const char *v = PMIx_Get_version()
        if NULL == v:
            return None
        return v.decode('ascii')

    # Initialize the PMIx client library, connecting
    # us to the local PMIx server
    #
    # @dicts [INPUT]
    #          - a list of dictionaries, where each
    #            dictionary has a key, value, and val_type
    #            defined as such:
    #            [{key:y, value:val, val_type:ty}, … ]
    #
    def init(self, dicts):
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
    def finalize(self, dicts):
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
    def abort(self, status, msg, peers):
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
    def store_internal(self, pyproc, pykey:str, pyval:dict):
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

    def fence(self, peers, dicts):
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
    def get(self, proc, ky, dicts):
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

        # allocate and load pmix info structs from python list of dictionaries
        info_ptr = &info
        rc = pmix_alloc_info(info_ptr, &ninfo, dicts)

        val = None

        # None key - pass NULL to return all data
        if ky is None:
            with nogil:
                rc = PMIx_Get(&p, NULL, info, ninfo, &val_ptr)
        else:
            # convert key,val to pmix_value_t and pmix_key_t
            pmix_copy_key(key, ky)
            with nogil:
                rc = PMIx_Get(&p, key, info, ninfo, &val_ptr)
        if PMIX_SUCCESS == rc:
            val = pmix_unload_value(val_ptr)
            # val_ptr was allocated by the PMIx library (PMIX_VALUE_CREATE),
            # so it must be released with the library's own allocator rather
            # than PyMem_Free.
            PMIx_Value_free(val_ptr, 1)
        if 0 < ninfo:
            pmix_free_info(info, ninfo)
        return rc, val

    # Publish the data in the info array for lookup
    #
    # @dicts [INPUT]
    #          - a list of dictionaries, where
    #            a key, flags, value, and val_type
    #            can be defined as keys
    def publish(self, dicts):
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
    def unpublish(self, pykeys, dicts):
        cdef pmix_info_t *info;
        cdef pmix_info_t **info_ptr;
        cdef size_t ninfo;
        cdef size_t nstrings;
        cdef char **keys;
        keys     = NULL
        ninfo    = 0
        nstrings = 0

        # load pykeys into char **keys - the entries are strdup'd, so the
        # array must come from the C allocator as well
        if pykeys is not None:
            nstrings = len(pykeys)
            if 0 < nstrings:
                keys = <char **> malloc((nstrings + 1) * sizeof(char*))
                if not keys:
                    return PMIX_ERR_NOMEM
                memset(keys, 0, (nstrings + 1) * sizeof(char*))
                rc = pmix_load_argv(keys, pykeys)
                if PMIX_SUCCESS != rc:
                    pmix_free_argv(keys)
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
        if NULL != keys:
            pmix_free_argv(keys)
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
    def lookup(self, data, dicts):
        cdef pmix_pdata_t *pdata;
        cdef pmix_info_t  *info;
        cdef pmix_info_t  **info_ptr;
        cdef size_t npdata;
        cdef size_t ninfo;

        npdata  = 0
        ninfo   = 0

        # allocate and load pmix info structs from python list of dictionaries
        if dicts is not None:
            info_ptr = &info
            rc = pmix_alloc_info(info_ptr, &ninfo, dicts)
            if PMIX_SUCCESS != rc:
                return rc
        else:
            info = NULL

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
                    n += 1
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
    def spawn(self, jobInfo, pyapps):
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

    def connect(self, peers, pyinfo):
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

    def disconnect(self, peers, pyinfo):
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

    def query(self, pyq):
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
                # zero it so a failure partway through leaves the untouched
                # entries safe for pmix_free_queries to walk
                memset(queries, 0, nqueries * sizeof(pmix_query_t))
                n = 0
                for q in pyq:
                    queries[n].keys       = NULL
                    queries[n].qualifiers = NULL
                    nstrings = len(q['keys'])
                    if 0 < nstrings:
                        # the entries are strdup'd, so the array must come
                        # from the C allocator as well
                        queries[n].keys = <char **> malloc((nstrings+1) * sizeof(char*))
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

    def log(self, pydata, pydirs):
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

    def allocation_request(self, directive, pyinfo):
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

    # Define, extend, reduce or delete a block of resources. The request is
    # relayed to the scheduler, so it is the caller's server (not the caller
    # itself) that must be the scheduler - a process running as the
    # scheduler gets PMIX_ERR_NOT_SUPPORTED as there is no one to ask.
    #
    # @directive [INPUT]
    #            - one of the PMIX_RESOURCE_BLOCK_ values (int)
    #
    # @block [INPUT]
    #        - name of the resource block, unique within the requestor's
    #          session (string)
    #
    # @pyunits [INPUT]
    #          - a list of resource unit dictionaries, each of the form
    #            {'type': PMIX_DEVTYPE_x, 'count': n}
    #
    # @pyinfo [INPUT]
    #         - a list of dictionaries, where each
    #           dictionary has a key, value, and val_type
    #           defined as such:
    #           [{key:y, value:val, val_type:ty}, … ]
    def resource_block(self, directive:int, block, pyunits, pyinfo):
        cdef pmix_resource_unit_t *units
        cdef size_t nunits
        cdef pmix_info_t *info
        cdef pmix_info_t **info_ptr
        cdef size_t ninfo
        cdef char *blk
        cdef pmix_resource_block_directive_t drctv

        if block is None:
            return PMIX_ERR_BAD_PARAM
        if isinstance(block, str):
            pyblock = block.encode('ascii')
        else:
            pyblock = block
        blk = <char*>pyblock
        drctv = directive

        # allocate and load pmix info structs from python list of dictionaries
        info_ptr = &info
        rc = pmix_alloc_info(info_ptr, &ninfo, pyinfo)
        if PMIX_SUCCESS != rc:
            return rc

        # convert the resource units
        rc = pmix_alloc_units(&units, &nunits, pyunits)
        if PMIX_SUCCESS != rc:
            if 0 < ninfo:
                pmix_free_info(info, ninfo)
            return rc

        # call the API - it blocks until the scheduler responds, and the
        # response may have to pass through a server module written in
        # Python, so the GIL cannot be held across it
        with nogil:
            rc = PMIx_Resource_block(drctv, blk, units, nunits, info, ninfo)
        pmix_free_units(units, nunits)
        if 0 < ninfo:
            pmix_free_info(info, ninfo)
        return rc

    def job_control(self, pytargets, pydirs):
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

    def monitor(self, pymonitor_info, code:int, pydirs):
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

    # Send a heartbeat to our server, feeding whatever process health
    # monitor was established with the PMIX_MONITOR_HEARTBEAT attribute
    # (see monitor() above). The heartbeat is fire-and-forget: the C API
    # returns nothing, so this reports PMIX_SUCCESS once the message has
    # been handed to the library.
    #
    # @dicts [INPUT]
    #        - accepted for future directives; the C API defines none
    #          today, so anything provided here is ignored
    def heartbeat(self, dicts=None):
        PMIx_Heartbeat()
        return PMIX_SUCCESS

    def get_credential(self, pyinfo):
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

    def validate_credential(self, pycred:dict, pyinfo):
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

    # Note: 'peers' and 'pyinfo' are deliberately left unannotated. Cython
    # treats a parameter annotation as a type declaration that rejects
    # None, which would make the "no peers given" default below - and the
    # documented ability to pass None for the attributes - unreachable.
    def group_construct(self, group:str, peers, pyinfo):
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

    def group_invite(self, group:str, peers, pyinfo):
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

    def group_join(self, group:str, leader, opt:int, pyinfo):
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

    def group_leave(self, group:str, pyinfo):
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

    def group_destruct(self, group:str, pyinfo):
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

    # Non-blocking forms of the group operations. Each returns only the
    # integer status of the request itself - the result of the operation
    # is delivered later by executing 'cbfunc' on the library's progress
    # thread. The callback is passed the opaque 'cbdata' object handed in
    # here, unmodified, and takes the form
    #
    #    cbfunc(status:int, results:list, cbdata)   # construct/invite/join
    #    cbfunc(status:int, cbdata)                 # leave/destruct
    #
    # A callback is executed if and only if the method returns
    # PMIX_SUCCESS. Note that it runs on the progress thread, so it must
    # not call a blocking PMIx operation - hand the work to another
    # thread instead.
    def group_construct_nb(self, group:str, peers, pyinfo,
                           cbfunc, cbdata=None):
        cdef pypmix_nb_cbdata_t *cd
        cdef pmix_status_t rc
        cdef int prc

        if not callable(cbfunc):
            return PMIX_ERR_BAD_PARAM

        # convert group name and the info array into a caddy that will
        # outlive this call - the library does not copy them, so they
        # must remain valid until the callback fires
        pygrp = group.encode('ascii')
        cd = pypmix_nb_group_setup(pygrp, pyinfo, &prc)
        if NULL == cd:
            return prc

        # convert list of procs to array of pmix_proc_t's
        prc = pypmix_nb_add_procs(cd, peers, self.myproc.nspace)
        if PMIX_SUCCESS != prc:
            pypmix_nb_cbdata_free(cd)
            return prc

        # record the callback so it stays alive, then call the library
        cd.idx = pypmix_nb_register(cbfunc, cbdata)
        with nogil:
            rc = PMIx_Group_construct_nb(cd.grp, cd.procs, cd.nprocs,
                                         cd.info, cd.ninfo,
                                         pypmix_client_info_cbfunc, <void *> cd)
        if PMIX_SUCCESS != rc:
            # no callback will be executed, so reclaim the operation here
            if pypmix_nb_take(cd.idx) is not None:
                pypmix_nb_cbdata_free(cd)
        return rc

    def group_invite_nb(self, group:str, peers, pyinfo,
                        cbfunc, cbdata=None):
        cdef pypmix_nb_cbdata_t *cd
        cdef pmix_status_t rc
        cdef int prc

        if not callable(cbfunc):
            return PMIX_ERR_BAD_PARAM

        # convert group name and the info array into a caddy that will
        # outlive this call
        pygrp = group.encode('ascii')
        cd = pypmix_nb_group_setup(pygrp, pyinfo, &prc)
        if NULL == cd:
            return prc

        # convert list of procs to array of pmix_proc_t's
        prc = pypmix_nb_add_procs(cd, peers, self.myproc.nspace)
        if PMIX_SUCCESS != prc:
            pypmix_nb_cbdata_free(cd)
            return prc

        # record the callback so it stays alive, then call the library
        cd.idx = pypmix_nb_register(cbfunc, cbdata)
        with nogil:
            rc = PMIx_Group_invite_nb(cd.grp, cd.procs, cd.nprocs,
                                      cd.info, cd.ninfo,
                                      pypmix_client_info_cbfunc, <void *> cd)
        if PMIX_SUCCESS != rc:
            # no callback will be executed, so reclaim the operation here
            if pypmix_nb_take(cd.idx) is not None:
                pypmix_nb_cbdata_free(cd)
        return rc

    def group_join_nb(self, group:str, leader, opt:int, pyinfo,
                      cbfunc, cbdata=None):
        cdef pypmix_nb_cbdata_t *cd
        cdef pmix_status_t rc
        cdef int prc
        cdef pmix_group_opt_t copt

        if not callable(cbfunc):
            return PMIX_ERR_BAD_PARAM
        copt = opt

        # convert group name and the info array into a caddy that will
        # outlive this call
        pygrp = group.encode('ascii')
        cd = pypmix_nb_group_setup(pygrp, pyinfo, &prc)
        if NULL == cd:
            return prc

        # the leader must outlive this call as well, so park it in the
        # caddy as a single-element proc array
        cd.nprocs = 1
        cd.procs = <pmix_proc_t*> PyMem_Malloc(sizeof(pmix_proc_t))
        if not cd.procs:
            cd.nprocs = 0
            pypmix_nb_cbdata_free(cd)
            return PMIX_ERR_NOMEM
        if leader is not None:
            pmix_copy_nspace(cd.procs[0].nspace, leader['nspace'])
            cd.procs[0].rank = leader['rank']
        else:
            pmix_copy_nspace(cd.procs[0].nspace, self.myproc.nspace)
            cd.procs[0].rank = self.myproc.rank

        # record the callback so it stays alive, then call the library
        cd.idx = pypmix_nb_register(cbfunc, cbdata)
        with nogil:
            rc = PMIx_Group_join_nb(cd.grp, &cd.procs[0], copt,
                                    cd.info, cd.ninfo,
                                    pypmix_client_info_cbfunc, <void *> cd)
        if PMIX_SUCCESS != rc:
            # no callback will be executed, so reclaim the operation here
            if pypmix_nb_take(cd.idx) is not None:
                pypmix_nb_cbdata_free(cd)
        return rc

    def group_leave_nb(self, group:str, pyinfo, cbfunc, cbdata=None):
        cdef pypmix_nb_cbdata_t *cd
        cdef pmix_status_t rc
        cdef int prc

        if not callable(cbfunc):
            return PMIX_ERR_BAD_PARAM

        # convert group name and the info array into a caddy that will
        # outlive this call
        pygrp = group.encode('ascii')
        cd = pypmix_nb_group_setup(pygrp, pyinfo, &prc)
        if NULL == cd:
            return prc

        # record the callback so it stays alive, then call the library
        cd.idx = pypmix_nb_register(cbfunc, cbdata)
        with nogil:
            rc = PMIx_Group_leave_nb(cd.grp, cd.info, cd.ninfo,
                                     pypmix_client_op_cbfunc, <void *> cd)
        if PMIX_SUCCESS != rc:
            # no callback will be executed, so reclaim the operation here
            if pypmix_nb_take(cd.idx) is not None:
                pypmix_nb_cbdata_free(cd)
        return rc

    def group_destruct_nb(self, group:str, pyinfo, cbfunc, cbdata=None):
        cdef pypmix_nb_cbdata_t *cd
        cdef pmix_status_t rc
        cdef int prc

        if not callable(cbfunc):
            return PMIX_ERR_BAD_PARAM

        # convert group name and the info array into a caddy that will
        # outlive this call
        pygrp = group.encode('ascii')
        cd = pypmix_nb_group_setup(pygrp, pyinfo, &prc)
        if NULL == cd:
            return prc

        # record the callback so it stays alive, then call the library
        cd.idx = pypmix_nb_register(cbfunc, cbdata)
        with nogil:
            rc = PMIx_Group_destruct_nb(cd.grp, cd.info, cd.ninfo,
                                        pypmix_client_op_cbfunc, <void *> cd)
        if PMIX_SUCCESS != rc:
            # no callback will be executed, so reclaim the operation here
            if pypmix_nb_take(cd.idx) is not None:
                pypmix_nb_cbdata_free(cd)
        return rc

    # Non-blocking forms of the remaining client operations. They follow
    # the same contract as the group operations above: the method returns
    # only the status of the request itself, the result is delivered later
    # by executing 'cbfunc' on the library's progress thread, and a
    # callback is executed if and only if the method returned
    # PMIX_SUCCESS. The 'cbdata' object is handed back to the callback
    # unmodified. Each callback signature is given with its method; all of
    # them run on the progress thread, so none may call a blocking PMIx
    # operation - record the result and wake another thread instead.

    # cbfunc(status:int, cbdata)
    def fence_nb(self, peers, dicts, cbfunc, cbdata=None):
        cdef pypmix_nb_cbdata_t *cd
        cdef pmix_status_t rc
        cdef int prc

        if not callable(cbfunc):
            return PMIX_ERR_BAD_PARAM

        # the library does not copy its input, so everything it will hold
        # goes into a caddy that outlives this call
        cd = pypmix_nb_setup(dicts, &prc)
        if NULL == cd:
            return prc
        prc = pypmix_nb_add_procs(cd, peers, self.myproc.nspace)
        if PMIX_SUCCESS != prc:
            pypmix_nb_cbdata_free(cd)
            return prc

        # record the callback so it stays alive, then call the library
        cd.idx = pypmix_nb_register(cbfunc, cbdata)
        with nogil:
            rc = PMIx_Fence_nb(cd.procs, cd.nprocs, cd.info, cd.ninfo,
                               pypmix_client_op_cbfunc, <void *> cd)
        if PMIX_SUCCESS != rc:
            # no callback will be executed, so reclaim the operation here
            if pypmix_nb_take(cd.idx) is not None:
                pypmix_nb_cbdata_free(cd)
        return rc

    # cbfunc(status:int, value:dict, cbdata) - 'value' is None if the key
    # was not found. A None key requests all data for the given proc
    def get_nb(self, proc, ky, dicts, cbfunc, cbdata=None):
        cdef pypmix_nb_cbdata_t *cd
        cdef pmix_status_t rc
        cdef int prc

        if not callable(cbfunc):
            return PMIX_ERR_BAD_PARAM

        cd = pypmix_nb_setup(dicts, &prc)
        if NULL == cd:
            return prc

        # the proc and the key must outlive this call as well - a
        # single-element proc array covers the former
        cd.nprocs = 1
        cd.procs = <pmix_proc_t*> PyMem_Malloc(sizeof(pmix_proc_t))
        if not cd.procs:
            cd.nprocs = 0
            pypmix_nb_cbdata_free(cd)
            return PMIX_ERR_NOMEM
        if proc is None:
            pmix_copy_nspace(cd.procs[0].nspace, self.myproc.nspace)
            cd.procs[0].rank = self.myproc.rank
        else:
            pmix_copy_nspace(cd.procs[0].nspace, proc['nspace'])
            cd.procs[0].rank = proc['rank']
        if ky is not None:
            if isinstance(ky, str):
                pyky = ky.encode('ascii')
            else:
                pyky = ky
            cd.ky = strdup(pyky)
            if NULL == cd.ky:
                pypmix_nb_cbdata_free(cd)
                return PMIX_ERR_NOMEM

        cd.idx = pypmix_nb_register(cbfunc, cbdata)
        with nogil:
            rc = PMIx_Get_nb(&cd.procs[0], cd.ky, cd.info, cd.ninfo,
                             pypmix_client_value_cbfunc, <void *> cd)
        if PMIX_SUCCESS != rc:
            if pypmix_nb_take(cd.idx) is not None:
                pypmix_nb_cbdata_free(cd)
        return rc

    # cbfunc(status:int, cbdata)
    def publish_nb(self, dicts, cbfunc, cbdata=None):
        cdef pypmix_nb_cbdata_t *cd
        cdef pmix_status_t rc
        cdef int prc

        if not callable(cbfunc):
            return PMIX_ERR_BAD_PARAM

        cd = pypmix_nb_setup(dicts, &prc)
        if NULL == cd:
            return prc

        cd.idx = pypmix_nb_register(cbfunc, cbdata)
        with nogil:
            rc = PMIx_Publish_nb(cd.info, cd.ninfo,
                                 pypmix_client_op_cbfunc, <void *> cd)
        if PMIX_SUCCESS != rc:
            if pypmix_nb_take(cd.idx) is not None:
                pypmix_nb_cbdata_free(cd)
        return rc

    # cbfunc(status:int, pdata:list, cbdata) - each entry of 'pdata' is a
    # dict of proc/key/value/val_type. Note that the non-blocking form
    # takes a list of key strings, not the pdata list its blocking
    # counterpart takes, as that is what the C API accepts
    def lookup_nb(self, pykeys, dicts, cbfunc, cbdata=None):
        cdef pypmix_nb_cbdata_t *cd
        cdef pmix_status_t rc
        cdef int prc

        if not callable(cbfunc):
            return PMIX_ERR_BAD_PARAM

        cd = pypmix_nb_setup(dicts, &prc)
        if NULL == cd:
            return prc
        prc = pypmix_nb_add_keys(cd, pykeys)
        if PMIX_SUCCESS != prc:
            pypmix_nb_cbdata_free(cd)
            return prc

        cd.idx = pypmix_nb_register(cbfunc, cbdata)
        with nogil:
            rc = PMIx_Lookup_nb(cd.keys, cd.info, cd.ninfo,
                                pypmix_client_lookup_cbfunc, <void *> cd)
        if PMIX_SUCCESS != rc:
            if pypmix_nb_take(cd.idx) is not None:
                pypmix_nb_cbdata_free(cd)
        return rc

    # cbfunc(status:int, cbdata) - a None/empty key list asks the server
    # to remove everything this process published
    def unpublish_nb(self, pykeys, dicts, cbfunc, cbdata=None):
        cdef pypmix_nb_cbdata_t *cd
        cdef pmix_status_t rc
        cdef int prc

        if not callable(cbfunc):
            return PMIX_ERR_BAD_PARAM

        cd = pypmix_nb_setup(dicts, &prc)
        if NULL == cd:
            return prc
        prc = pypmix_nb_add_keys(cd, pykeys)
        if PMIX_SUCCESS != prc:
            pypmix_nb_cbdata_free(cd)
            return prc

        cd.idx = pypmix_nb_register(cbfunc, cbdata)
        with nogil:
            rc = PMIx_Unpublish_nb(cd.keys, cd.info, cd.ninfo,
                                   pypmix_client_op_cbfunc, <void *> cd)
        if PMIX_SUCCESS != rc:
            if pypmix_nb_take(cd.idx) is not None:
                pypmix_nb_cbdata_free(cd)
        return rc

    # cbfunc(status:int, nspace:str, cbdata) - 'nspace' is the namespace
    # assigned to the new job, or None if the spawn failed
    def spawn_nb(self, jobInfo, pyapps, cbfunc, cbdata=None):
        cdef pypmix_nb_cbdata_t *cd
        cdef pmix_status_t rc
        cdef int prc

        if not callable(cbfunc):
            return PMIX_ERR_BAD_PARAM
        # protect against bad input
        if pyapps is None or 0 == len(pyapps):
            return PMIX_ERR_BAD_PARAM

        cd = pypmix_nb_setup(jobInfo, &prc)
        if NULL == cd:
            return prc

        # convert the list of apps to an array of pmix_app_t
        cd.napps = len(pyapps)
        cd.apps = <pmix_app_t*> PyMem_Malloc(cd.napps * sizeof(pmix_app_t))
        if not cd.apps:
            cd.napps = 0
            pypmix_nb_cbdata_free(cd)
            return PMIX_ERR_NOMEM
        prc = pmix_load_apps(cd.apps, pyapps)
        if PMIX_SUCCESS != prc:
            pypmix_nb_cbdata_free(cd)
            return prc

        cd.idx = pypmix_nb_register(cbfunc, cbdata)
        with nogil:
            rc = PMIx_Spawn_nb(cd.info, cd.ninfo, cd.apps, cd.napps,
                               pypmix_client_spawn_cbfunc, <void *> cd)
        if PMIX_SUCCESS != rc:
            if pypmix_nb_take(cd.idx) is not None:
                pypmix_nb_cbdata_free(cd)
        return rc

    # cbfunc(status:int, cbdata)
    def connect_nb(self, peers, pyinfo, cbfunc, cbdata=None):
        cdef pypmix_nb_cbdata_t *cd
        cdef pmix_status_t rc
        cdef int prc

        if not callable(cbfunc):
            return PMIX_ERR_BAD_PARAM

        cd = pypmix_nb_setup(pyinfo, &prc)
        if NULL == cd:
            return prc
        prc = pypmix_nb_add_procs(cd, peers, self.myproc.nspace)
        if PMIX_SUCCESS != prc:
            pypmix_nb_cbdata_free(cd)
            return prc

        cd.idx = pypmix_nb_register(cbfunc, cbdata)
        with nogil:
            rc = PMIx_Connect_nb(cd.procs, cd.nprocs, cd.info, cd.ninfo,
                                 pypmix_client_op_cbfunc, <void *> cd)
        if PMIX_SUCCESS != rc:
            if pypmix_nb_take(cd.idx) is not None:
                pypmix_nb_cbdata_free(cd)
        return rc

    # cbfunc(status:int, cbdata)
    def disconnect_nb(self, peers, pyinfo, cbfunc, cbdata=None):
        cdef pypmix_nb_cbdata_t *cd
        cdef pmix_status_t rc
        cdef int prc

        if not callable(cbfunc):
            return PMIX_ERR_BAD_PARAM

        cd = pypmix_nb_setup(pyinfo, &prc)
        if NULL == cd:
            return prc
        prc = pypmix_nb_add_procs(cd, peers, self.myproc.nspace)
        if PMIX_SUCCESS != prc:
            pypmix_nb_cbdata_free(cd)
            return prc

        cd.idx = pypmix_nb_register(cbfunc, cbdata)
        with nogil:
            rc = PMIx_Disconnect_nb(cd.procs, cd.nprocs, cd.info, cd.ninfo,
                                    pypmix_client_op_cbfunc, <void *> cd)
        if PMIX_SUCCESS != rc:
            if pypmix_nb_take(cd.idx) is not None:
                pypmix_nb_cbdata_free(cd)
        return rc

    # cbfunc(status:int, results:list, cbdata) - 'results' is a list of
    # info dicts. 'pyq' has the same shape as for query()
    def query_nb(self, pyq, cbfunc, cbdata=None):
        cdef pypmix_nb_cbdata_t *cd
        cdef pmix_status_t rc
        cdef int prc
        cdef size_t nstrings
        cdef size_t n

        if not callable(cbfunc):
            return PMIX_ERR_BAD_PARAM

        # this operation carries no info array of its own - the
        # qualifiers ride along inside each query
        cd = pypmix_nb_setup(None, &prc)
        if NULL == cd:
            return prc

        if pyq is not None and 0 < len(pyq):
            cd.nqueries = len(pyq)
            cd.queries = <pmix_query_t*> PyMem_Malloc(cd.nqueries * sizeof(pmix_query_t))
            if not cd.queries:
                cd.nqueries = 0
                pypmix_nb_cbdata_free(cd)
                return PMIX_ERR_NOMEM
            # zero it so a failure partway through leaves the untouched
            # entries safe to release
            memset(cd.queries, 0, cd.nqueries * sizeof(pmix_query_t))
            n = 0
            for q in pyq:
                qkeys = q.get('keys')
                if qkeys is not None and 0 < len(qkeys):
                    nstrings = len(qkeys)
                    cd.queries[n].keys = <char **> malloc((nstrings+1) * sizeof(char*))
                    if not cd.queries[n].keys:
                        pypmix_nb_cbdata_free(cd)
                        return PMIX_ERR_NOMEM
                    memset(cd.queries[n].keys, 0, (nstrings+1) * sizeof(char*))
                    prc = pmix_load_argv(cd.queries[n].keys, qkeys)
                    if PMIX_SUCCESS != prc:
                        pypmix_nb_cbdata_free(cd)
                        return prc
                prc = pmix_alloc_info(&(cd.queries[n].qualifiers),
                                      &(cd.queries[n].nqual),
                                      q.get('qualifiers'))
                if PMIX_SUCCESS != prc:
                    pypmix_nb_cbdata_free(cd)
                    return prc
                n += 1

        cd.idx = pypmix_nb_register(cbfunc, cbdata)
        with nogil:
            rc = PMIx_Query_info_nb(cd.queries, cd.nqueries,
                                    pypmix_client_info_cbfunc, <void *> cd)
        if PMIX_SUCCESS != rc:
            if pypmix_nb_take(cd.idx) is not None:
                pypmix_nb_cbdata_free(cd)
        return rc

    # cbfunc(status:int, cbdata)
    def log_nb(self, pydata, pydirs, cbfunc, cbdata=None):
        cdef pypmix_nb_cbdata_t *cd
        cdef pmix_status_t rc
        cdef int prc

        if not callable(cbfunc):
            return PMIX_ERR_BAD_PARAM

        cd = pypmix_nb_setup(pydata, &prc)
        if NULL == cd:
            return prc
        prc = pypmix_nb_add_dirs(cd, pydirs)
        if PMIX_SUCCESS != prc:
            pypmix_nb_cbdata_free(cd)
            return prc

        cd.idx = pypmix_nb_register(cbfunc, cbdata)
        with nogil:
            rc = PMIx_Log_nb(cd.info, cd.ninfo, cd.dirs, cd.ndirs,
                             pypmix_client_op_cbfunc, <void *> cd)
        if PMIX_SUCCESS != rc:
            if pypmix_nb_take(cd.idx) is not None:
                pypmix_nb_cbdata_free(cd)
        return rc

    # cbfunc(status:int, results:list, cbdata)
    def allocation_request_nb(self, directive, pyinfo, cbfunc, cbdata=None):
        cdef pypmix_nb_cbdata_t *cd
        cdef pmix_status_t rc
        cdef int prc
        cdef pmix_alloc_directive_t drctv

        if not callable(cbfunc):
            return PMIX_ERR_BAD_PARAM
        drctv = directive

        cd = pypmix_nb_setup(pyinfo, &prc)
        if NULL == cd:
            return prc

        cd.idx = pypmix_nb_register(cbfunc, cbdata)
        with nogil:
            rc = PMIx_Allocation_request_nb(drctv, cd.info, cd.ninfo,
                                            pypmix_client_info_cbfunc,
                                            <void *> cd)
        if PMIX_SUCCESS != rc:
            if pypmix_nb_take(cd.idx) is not None:
                pypmix_nb_cbdata_free(cd)
        return rc

    # cbfunc(status:int, results:list, cbdata)
    def job_control_nb(self, pytargets, pydirs, cbfunc, cbdata=None):
        cdef pypmix_nb_cbdata_t *cd
        cdef pmix_status_t rc
        cdef int prc

        if not callable(cbfunc):
            return PMIX_ERR_BAD_PARAM

        # the directives are this operation's info array
        cd = pypmix_nb_setup(pydirs, &prc)
        if NULL == cd:
            return prc
        prc = pypmix_nb_add_procs(cd, pytargets, self.myproc.nspace)
        if PMIX_SUCCESS != prc:
            pypmix_nb_cbdata_free(cd)
            return prc

        cd.idx = pypmix_nb_register(cbfunc, cbdata)
        with nogil:
            rc = PMIx_Job_control_nb(cd.procs, cd.nprocs, cd.info, cd.ninfo,
                                     pypmix_client_info_cbfunc, <void *> cd)
        if PMIX_SUCCESS != rc:
            if pypmix_nb_take(cd.idx) is not None:
                pypmix_nb_cbdata_free(cd)
        return rc

    # cbfunc(status:int, results:list, cbdata). As in the blocking form,
    # the monitor request is given as a list whose first element is the
    # monitoring attribute the C API takes
    def monitor_nb(self, pymonitor_info, code:int, pydirs, cbfunc, cbdata=None):
        cdef pypmix_nb_cbdata_t *cd
        cdef pmix_status_t rc
        cdef pmix_status_t ccode
        cdef int prc

        if not callable(cbfunc):
            return PMIX_ERR_BAD_PARAM
        ccode = code

        cd = pypmix_nb_setup(pymonitor_info, &prc)
        if NULL == cd:
            return prc
        prc = pypmix_nb_add_dirs(cd, pydirs)
        if PMIX_SUCCESS != prc:
            pypmix_nb_cbdata_free(cd)
            return prc

        cd.idx = pypmix_nb_register(cbfunc, cbdata)
        with nogil:
            rc = PMIx_Process_monitor_nb(cd.info, ccode, cd.dirs, cd.ndirs,
                                         pypmix_client_info_cbfunc, <void *> cd)
        if PMIX_SUCCESS != rc:
            if pypmix_nb_take(cd.idx) is not None:
                pypmix_nb_cbdata_free(cd)
        return rc

    # cbfunc(status:int, credential:dict, results:list, cbdata) - the
    # credential is the usual byte-object dict of 'bytes' and 'size'
    def get_credential_nb(self, pyinfo, cbfunc, cbdata=None):
        cdef pypmix_nb_cbdata_t *cd
        cdef pmix_status_t rc
        cdef int prc

        if not callable(cbfunc):
            return PMIX_ERR_BAD_PARAM

        cd = pypmix_nb_setup(pyinfo, &prc)
        if NULL == cd:
            return prc

        cd.idx = pypmix_nb_register(cbfunc, cbdata)
        with nogil:
            rc = PMIx_Get_credential_nb(cd.info, cd.ninfo,
                                        pypmix_client_credential_cbfunc,
                                        <void *> cd)
        if PMIX_SUCCESS != rc:
            if pypmix_nb_take(cd.idx) is not None:
                pypmix_nb_cbdata_free(cd)
        return rc

    # cbfunc(status:int, results:list, cbdata) - a PMIX_SUCCESS status
    # means the credential was accepted
    def validate_credential_nb(self, pycred, pyinfo, cbfunc, cbdata=None):
        cdef pypmix_nb_cbdata_t *cd
        cdef pmix_status_t rc
        cdef int prc
        cdef const char *credptr

        if not callable(cbfunc):
            return PMIX_ERR_BAD_PARAM
        if pycred is None:
            return PMIX_ERR_BAD_PARAM

        cd = pypmix_nb_setup(pyinfo, &prc)
        if NULL == cd:
            return prc

        # the credential must outlive this call, so copy it into the caddy
        cd.cred = <pmix_byte_object_t*> PyMem_Malloc(sizeof(pmix_byte_object_t))
        if not cd.cred:
            pypmix_nb_cbdata_free(cd)
            return PMIX_ERR_NOMEM
        cd.cred.bytes = NULL
        cd.cred.size = 0
        pybytes = pycred.get('bytes')
        if pybytes is None:
            pypmix_nb_cbdata_free(cd)
            return PMIX_ERR_BAD_PARAM
        if isinstance(pybytes, str):
            pybytes = pybytes.encode('ascii')
        else:
            pybytes = bytes(pybytes)
        if 0 < len(pybytes):
            cd.cred.size = len(pybytes)
            cd.cred.bytes = <char*> PyMem_Malloc(cd.cred.size)
            if not cd.cred.bytes:
                cd.cred.size = 0
                pypmix_nb_cbdata_free(cd)
                return PMIX_ERR_NOMEM
            credptr = <const char*>pybytes
            memcpy(cd.cred.bytes, credptr, cd.cred.size)

        cd.idx = pypmix_nb_register(cbfunc, cbdata)
        with nogil:
            rc = PMIx_Validate_credential_nb(cd.cred, cd.info, cd.ninfo,
                                             pypmix_client_validation_cbfunc,
                                             <void *> cd)
        if PMIX_SUCCESS != rc:
            if pypmix_nb_take(cd.idx) is not None:
                pypmix_nb_cbdata_free(cd)
        return rc

    # cbfunc(status:int, cbdata). The fabric object lives in this class,
    # so the registry holds a reference to us until the callback fires,
    # and the "registered" flag is only set once the library says so
    def fabric_register_nb(self, dicts, cbfunc, cbdata=None):
        cdef pypmix_nb_cbdata_t *cd
        cdef pmix_status_t rc
        cdef int prc
        cdef pmix_fabric_t *fab

        if not callable(cbfunc):
            return PMIX_ERR_BAD_PARAM
        if 1 == self.fabric_set:
            return PMIX_ERR_RESOURCE_BUSY

        cd = pypmix_nb_setup(dicts, &prc)
        if NULL == cd:
            return prc

        def regdone(status, ud):
            if PMIX_SUCCESS == status:
                self.fabric_set = 1
            cbfunc(status, ud)

        fab = &self.myfabric
        cd.idx = pypmix_nb_register(regdone, cbdata, self)
        with nogil:
            rc = PMIx_Fabric_register_nb(fab, cd.info, cd.ninfo,
                                         pypmix_client_op_cbfunc, <void *> cd)
        if PMIX_SUCCESS != rc:
            if pypmix_nb_take(cd.idx) is not None:
                pypmix_nb_cbdata_free(cd)
        return rc

    # cbfunc(status:int, cbdata) - the updated fabric information is read
    # back through fabric_update()'s companion accessors once the
    # callback reports success
    def fabric_update_nb(self, cbfunc, cbdata=None):
        cdef pypmix_nb_cbdata_t *cd
        cdef pmix_status_t rc
        cdef int prc
        cdef pmix_fabric_t *fab

        if not callable(cbfunc):
            return PMIX_ERR_BAD_PARAM
        if 0 == self.fabric_set:
            return PMIX_ERR_INIT

        cd = pypmix_nb_setup(None, &prc)
        if NULL == cd:
            return prc

        fab = &self.myfabric
        cd.idx = pypmix_nb_register(cbfunc, cbdata, self)
        with nogil:
            rc = PMIx_Fabric_update_nb(fab, pypmix_client_op_cbfunc,
                                       <void *> cd)
        if PMIX_SUCCESS != rc:
            if pypmix_nb_take(cd.idx) is not None:
                pypmix_nb_cbdata_free(cd)
        return rc

    # cbfunc(status:int, cbdata)
    def fabric_deregister_nb(self, cbfunc, cbdata=None):
        cdef pypmix_nb_cbdata_t *cd
        cdef pmix_status_t rc
        cdef int prc
        cdef pmix_fabric_t *fab

        if not callable(cbfunc):
            return PMIX_ERR_BAD_PARAM
        if 0 == self.fabric_set:
            return PMIX_ERR_INIT

        cd = pypmix_nb_setup(None, &prc)
        if NULL == cd:
            return prc

        def deregdone(status, ud):
            if PMIX_SUCCESS == status:
                self.fabric_set = 0
            cbfunc(status, ud)

        fab = &self.myfabric
        cd.idx = pypmix_nb_register(deregdone, cbdata, self)
        with nogil:
            rc = PMIx_Fabric_deregister_nb(fab, pypmix_client_op_cbfunc,
                                           <void *> cd)
        if PMIX_SUCCESS != rc:
            if pypmix_nb_take(cd.idx) is not None:
                pypmix_nb_cbdata_free(cd)
        return rc

    # cbfunc(status:int, distances:list, cbdata) - each entry is a dict of
    # uuid/osname/type/mindist/maxdist, as compute_distances() returns
    def compute_distances_nb(self, pycpus, dicts, cbfunc, cbdata=None):
        cdef pypmix_nb_cbdata_t *cd
        cdef pmix_status_t rc
        cdef int prc
        cdef pmix_topology_t *topo

        if not callable(cbfunc):
            return PMIX_ERR_BAD_PARAM

        # check that we loaded our topology
        if NULL == self.topo.topology:
            prc = self.load_topology()
            if PMIX_SUCCESS != prc:
                return prc

        cd = pypmix_nb_setup(dicts, &prc)
        if NULL == cd:
            return prc

        # the cpuset must outlive this call. Its loader constructs before
        # it parses, so mark it for destruction regardless of the outcome
        prc = pmix_load_cpuset(&cd.cpuset, pycpus)
        cd.havecpuset = 1
        if PMIX_SUCCESS != prc:
            pypmix_nb_cbdata_free(cd)
            return prc

        topo = &self.topo
        cd.idx = pypmix_nb_register(cbfunc, cbdata, self)
        with nogil:
            rc = PMIx_Compute_distances_nb(topo, &cd.cpuset,
                                           cd.info, cd.ninfo,
                                           pypmix_client_devdist_cbfunc,
                                           <void *> cd)
        if PMIX_SUCCESS != rc:
            if pypmix_nb_take(cd.idx) is not None:
                pypmix_nb_cbdata_free(cd)
        return rc

    # cbfunc(status:int, cbdata). See resource_block() for the meaning of
    # the directive, block name and resource unit list
    def resource_block_nb(self, directive:int, block, pyunits, pyinfo,
                          cbfunc, cbdata=None):
        cdef pypmix_nb_cbdata_t *cd
        cdef pmix_status_t rc
        cdef int prc
        cdef pmix_resource_block_directive_t drctv

        if not callable(cbfunc):
            return PMIX_ERR_BAD_PARAM
        if block is None:
            return PMIX_ERR_BAD_PARAM
        drctv = directive

        cd = pypmix_nb_setup(pyinfo, &prc)
        if NULL == cd:
            return prc

        # the block name and the units must outlive this call
        if isinstance(block, str):
            pyblock = block.encode('ascii')
        else:
            pyblock = block
        cd.blk = strdup(pyblock)
        if NULL == cd.blk:
            pypmix_nb_cbdata_free(cd)
            return PMIX_ERR_NOMEM
        prc = pmix_alloc_units(&cd.units, &cd.nunits, pyunits)
        if PMIX_SUCCESS != prc:
            pypmix_nb_cbdata_free(cd)
            return prc

        cd.idx = pypmix_nb_register(cbfunc, cbdata)
        with nogil:
            rc = PMIx_Resource_block_nb(drctv, cd.blk, cd.units, cd.nunits,
                                        cd.info, cd.ninfo,
                                        pypmix_client_op_cbfunc, <void *> cd)
        if PMIX_SUCCESS != rc:
            if pypmix_nb_take(cd.idx) is not None:
                pypmix_nb_cbdata_free(cd)
        return rc

    def register_event_handler(self, pycodes, pyinfo, hdlr):
        # pycodes/pyinfo are intentionally untyped: the body accepts either a
        # list or None (None => register a default handler / no directives).
        # A `:list` annotation would make Cython reject None at the boundary.
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

    def notify_event(self, status:int, pysrc:dict, range, pyinfo):
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

    def value_comparison_string(self, pystat:int):
        cdef char *string

        string = <char*>PMIx_Value_comparison_string(pystat)
        pystr = string
        return pystr.decode('ascii')

    def group_operation_string(self, pystat:int):
        cdef char *string

        string = <char*>PMIx_Group_operation_string(pystat)
        pystr = string
        return pystr.decode('ascii')

    def resource_block_directive_string(self, pystat:int):
        cdef char *string

        string = <char*>PMIx_Resource_block_directive_string(pystat)
        pystr = string
        return pystr.decode('ascii')

    def alloc_inheritance_string(self, pystat:int):
        cdef char *string

        string = <char*>PMIx_Alloc_inheritance_string(pystat)
        pystr = string
        return pystr.decode('ascii')

    # The inverse of error_string: map the name of a PMIx status back to
    # its numeric value. Returns PMIX_ERR_NOT_FOUND for an unknown name
    #
    # @errname [INPUT]
    #          - the symbolic name of a status, e.g. "PMIX_ERR_TIMEOUT"
    def error_code(self, errname):
        if errname is None:
            return PMIX_ERR_BAD_PARAM
        if isinstance(errname, str):
            pyname = errname.encode('ascii')
        else:
            pyname = errname
        return PMIx_Error_code(pyname)

    # Render a value or an info into the library's printable form. This is
    # the library's own pretty-printer, so what comes back is exactly what
    # PMIx debug output shows for the same data.
    #
    # @prefix [INPUT]
    #         - string to prepend to the output, or None
    #
    # @pysrc [INPUT]
    #        - a value dict ({'value':val, 'val_type':ty}) or an info dict
    #          (the same plus a 'key')
    #
    # @data_type [INPUT]
    #            - PMIX_VALUE or PMIX_INFO. Defaults to whichever shape
    #              pysrc has. The C API accepts any registered data type,
    #              but a Python caller can only hand us a value or an info
    #              - every other type is reachable as the value of one
    #
    # Returns (rc, string)
    def data_print(self, prefix, pysrc, data_type=None):
        cdef pmix_value_t value
        cdef pmix_info_t info
        cdef char *output = NULL
        cdef char *pfx = NULL

        if not isinstance(pysrc, dict):
            return (PMIX_ERR_BAD_PARAM, None)
        if data_type is None:
            if 'key' in pysrc:
                data_type = PMIX_INFO
            else:
                data_type = PMIX_VALUE
        if prefix is not None:
            if isinstance(prefix, str):
                pypfx = prefix.encode('ascii')
            else:
                pypfx = prefix
            pfx = <char*>pypfx

        if PMIX_VALUE == data_type:
            rc = pmix_load_value(&value, pysrc)
            if PMIX_SUCCESS != rc:
                pmix_destruct_value(&value)
                return (rc, None)
            rc = PMIx_Data_print(&output, pfx, <void*>&value, PMIX_VALUE)
            pmix_destruct_value(&value)
        elif PMIX_INFO == data_type:
            if 'key' not in pysrc:
                return (PMIX_ERR_BAD_PARAM, None)
            memset(&info, 0, sizeof(pmix_info_t))
            rc = pmix_load_info(&info, [pysrc])
            if PMIX_SUCCESS != rc:
                pmix_destruct_info(&info)
                return (rc, None)
            rc = PMIx_Data_print(&output, pfx, <void*>&info, PMIX_INFO)
            pmix_destruct_info(&info)
        else:
            return (PMIX_ERR_NOT_SUPPORTED, None)

        # the library allocated the string, so decode and hand it back
        if PMIX_SUCCESS != rc or NULL == output:
            return (rc, None)
        txt = output.decode('ascii')
        free(output)
        return (rc, txt)

    # Render one of the PMIx structs as a printable string. Unlike the
    # *_string converters above, which translate a single enumerated
    # value, these take a whole struct and are the C analogue of printing
    # the Python dict - they are bound so a Python program can produce
    # exactly the text the C library would.
    #
    # Each returns (rc, str). The library allocates the string, so each
    # decodes it and hands the storage back.

    # @pyinfo [INPUT] - a single info dict
    def info_string(self, pyinfo):
        cdef pmix_info_t info
        cdef char *output

        if not isinstance(pyinfo, dict) or 'key' not in pyinfo:
            return (PMIX_ERR_BAD_PARAM, None)
        memset(&info, 0, sizeof(pmix_info_t))
        rc = pmix_load_info(&info, [pyinfo])
        if PMIX_SUCCESS != rc:
            pmix_destruct_info(&info)
            return (rc, None)
        output = PMIx_Info_string(&info)
        pmix_destruct_info(&info)
        if NULL == output:
            return (PMIX_ERROR, None)
        txt = output.decode('ascii')
        free(output)
        return (PMIX_SUCCESS, txt)

    # @pyval [INPUT] - a single value dict
    def value_string(self, pyval):
        cdef pmix_value_t value
        cdef char *output

        if not isinstance(pyval, dict):
            return (PMIX_ERR_BAD_PARAM, None)
        rc = pmix_load_value(&value, pyval)
        if PMIX_SUCCESS != rc:
            pmix_destruct_value(&value)
            return (rc, None)
        output = PMIx_Value_string(&value)
        pmix_destruct_value(&value)
        if NULL == output:
            return (PMIX_ERROR, None)
        txt = output.decode('ascii')
        free(output)
        return (PMIX_SUCCESS, txt)

    # @pyproc [INPUT] - a proc dict of nspace and rank
    def proc_string(self, pyproc):
        cdef pmix_proc_t proc
        cdef char *output

        if not isinstance(pyproc, dict):
            return (PMIX_ERR_BAD_PARAM, None)
        memset(&proc, 0, sizeof(pmix_proc_t))
        rc = pmix_load_procs(&proc, [pyproc])
        if PMIX_SUCCESS != rc:
            return (rc, None)
        output = PMIx_Proc_string(&proc)
        if NULL == output:
            return (PMIX_ERROR, None)
        txt = output.decode('ascii')
        free(output)
        return (PMIX_SUCCESS, txt)

    # @pyapp [INPUT] - a single app dict, as spawn() takes
    def app_string(self, pyapp):
        cdef pmix_app_t app
        cdef char *output

        if not isinstance(pyapp, dict):
            return (PMIX_ERR_BAD_PARAM, None)
        memset(&app, 0, sizeof(pmix_app_t))
        rc = pmix_load_apps(&app, [pyapp])
        if PMIX_SUCCESS != rc:
            pmix_destruct_app(&app)
            return (rc, None)
        output = PMIx_App_string(&app)
        pmix_destruct_app(&app)
        if NULL == output:
            return (PMIX_ERROR, None)
        txt = output.decode('ascii')
        free(output)
        return (PMIX_SUCCESS, txt)

    # @pyunit [INPUT] - a resource unit dict of type and count
    def resource_unit_string(self, pyunit):
        cdef pmix_resource_unit_t unit
        cdef char *output

        if not isinstance(pyunit, dict):
            return (PMIX_ERR_BAD_PARAM, None)
        memset(&unit, 0, sizeof(pmix_resource_unit_t))
        rc = pmix_load_units(&unit, [pyunit])
        if PMIX_SUCCESS != rc:
            return (rc, None)
        output = PMIx_Resource_unit_string(&unit)
        if NULL == output:
            return (PMIX_ERROR, None)
        txt = output.decode('ascii')
        free(output)
        return (PMIX_SUCCESS, txt)

    # Serialization. A Python data buffer is the dict
    #
    #     {'bytes': b'...', 'bytes_used': n, 'bytes_unpacked': m}
    #
    # mirroring the fields of a pmix_data_buffer_t that mean anything on
    # this side of the boundary - the payload, and how far the unpack
    # cursor has advanced through it. The three raw pointers the struct
    # also carries are rebuilt from those offsets on each call, so a
    # buffer can be carried across calls, stored, or sent somewhere as
    # ordinary bytes. There is nothing to create or release: the dict is
    # the buffer, and the C storage lives only for the duration of a call.
    #
    # As with data_print, the payload a Python caller can build is either
    # a value dict or an info dict, deduced from the presence of a 'key';
    # every other data type is reachable as the value of one of those.

    # Pack a value into a data buffer, appending to whatever it holds
    #
    # @pybuf [INPUT/OUTPUT]
    #        - the buffer dict to append to; None starts a new one
    #
    # @pysrc [INPUT]
    #        - the value or info dict to pack
    #
    # @data_type [INPUT]
    #            - PMIX_VALUE or PMIX_INFO; deduced when omitted
    #
    # @target [INPUT]
    #         - proc dict naming the peer this buffer is destined for,
    #           which selects the wire format. None means our own job
    #
    # Returns (rc, buffer dict) - the same dict, updated in place
    def data_pack(self, pybuf, pysrc, data_type=None, target=None):
        cdef pmix_data_buffer_t buf
        cdef pmix_value_t value
        cdef pmix_info_t info
        cdef pmix_proc_t proc
        cdef pmix_proc_t *tgt

        if not isinstance(pysrc, dict):
            return (PMIX_ERR_BAD_PARAM, pybuf)
        if data_type is None:
            if 'key' in pysrc:
                data_type = PMIX_INFO
            else:
                data_type = PMIX_VALUE
        if PMIX_VALUE != data_type and PMIX_INFO != data_type:
            return (PMIX_ERR_NOT_SUPPORTED, pybuf)
        if pybuf is None:
            pybuf = {}
        elif not isinstance(pybuf, dict):
            return (PMIX_ERR_BAD_PARAM, pybuf)

        tgt = NULL
        if target is not None:
            memset(&proc, 0, sizeof(pmix_proc_t))
            rc = pmix_load_procs(&proc, [target])
            if PMIX_SUCCESS != rc:
                return (rc, pybuf)
            tgt = &proc

        rc = pmix_load_dbuf(&buf, pybuf)
        if PMIX_SUCCESS != rc:
            pmix_destruct_dbuf(&buf)
            return (rc, pybuf)

        if PMIX_VALUE == data_type:
            rc = pmix_load_value(&value, pysrc)
            if PMIX_SUCCESS == rc:
                rc = PMIx_Data_pack(tgt, &buf, <void*>&value, 1, PMIX_VALUE)
            pmix_destruct_value(&value)
        else:
            if 'key' not in pysrc:
                pmix_destruct_dbuf(&buf)
                return (PMIX_ERR_BAD_PARAM, pybuf)
            memset(&info, 0, sizeof(pmix_info_t))
            rc = pmix_load_info(&info, [pysrc])
            if PMIX_SUCCESS == rc:
                rc = PMIx_Data_pack(tgt, &buf, <void*>&info, 1, PMIX_INFO)
            pmix_destruct_info(&info)

        if PMIX_SUCCESS == rc:
            pmix_unload_dbuf(&buf, pybuf)
        pmix_destruct_dbuf(&buf)
        return (rc, pybuf)

    # Unpack the next value from a data buffer. The buffer's unpack
    # cursor advances, so successive calls walk the payload
    #
    # @pybuf [INPUT/OUTPUT]
    #        - the buffer dict to read from, updated in place
    #
    # @data_type [INPUT]
    #            - PMIX_VALUE or PMIX_INFO, whichever was packed
    #
    # @target [INPUT]
    #         - proc dict naming the peer that packed the buffer
    #
    # Returns (rc, value or info dict)
    def data_unpack(self, pybuf, data_type=None, target=None):
        cdef pmix_data_buffer_t buf
        cdef pmix_value_t value
        cdef pmix_info_t info
        cdef pmix_proc_t proc
        cdef pmix_proc_t *tgt
        cdef int32_t cnt

        if not isinstance(pybuf, dict):
            return (PMIX_ERR_BAD_PARAM, None)
        if data_type is None:
            data_type = PMIX_VALUE
        if PMIX_VALUE != data_type and PMIX_INFO != data_type:
            return (PMIX_ERR_NOT_SUPPORTED, None)

        tgt = NULL
        if target is not None:
            memset(&proc, 0, sizeof(pmix_proc_t))
            rc = pmix_load_procs(&proc, [target])
            if PMIX_SUCCESS != rc:
                return (rc, None)
            tgt = &proc

        rc = pmix_load_dbuf(&buf, pybuf)
        if PMIX_SUCCESS != rc:
            pmix_destruct_dbuf(&buf)
            return (rc, None)

        cnt = 1
        pyout = None
        if PMIX_VALUE == data_type:
            memset(&value, 0, sizeof(pmix_value_t))
            rc = PMIx_Data_unpack(tgt, &buf, <void*>&value, &cnt, PMIX_VALUE)
            if PMIX_SUCCESS == rc:
                pyout = pmix_unload_value(&value)
            pmix_destruct_value(&value)
        else:
            memset(&info, 0, sizeof(pmix_info_t))
            rc = PMIx_Data_unpack(tgt, &buf, <void*>&info, &cnt, PMIX_INFO)
            if PMIX_SUCCESS == rc:
                ilist = []
                rc = pmix_unload_info(&info, 1, ilist)
                if PMIX_SUCCESS == rc and 0 < len(ilist):
                    pyout = ilist[0]
            pmix_destruct_info(&info)

        # record how far the cursor moved, whatever the outcome, so a
        # caller that stops on an error can see where it stopped
        pmix_unload_dbuf(&buf, pybuf)
        pmix_destruct_dbuf(&buf)
        return (rc, pyout)

    # Copy a value or info through the library's own copy function
    #
    # Returns (rc, dict)
    def data_copy(self, pysrc, data_type=None):
        cdef pmix_value_t value
        cdef pmix_info_t info
        cdef pmix_value_t *valdest
        cdef pmix_info_t *infodest
        cdef void **dest

        if not isinstance(pysrc, dict):
            return (PMIX_ERR_BAD_PARAM, None)
        if data_type is None:
            if 'key' in pysrc:
                data_type = PMIX_INFO
            else:
                data_type = PMIX_VALUE

        if PMIX_VALUE == data_type:
            rc = pmix_load_value(&value, pysrc)
            if PMIX_SUCCESS != rc:
                pmix_destruct_value(&value)
                return (rc, None)
            valdest = NULL
            dest = <void**>&valdest
            rc = PMIx_Data_copy(dest, <void*>&value, PMIX_VALUE)
            pmix_destruct_value(&value)
            if PMIX_SUCCESS != rc or NULL == valdest:
                return (rc, None)
            pyout = pmix_unload_value(valdest)
            # the copy came from the library, so it goes back to it
            PMIx_Value_free(valdest, 1)
            return (rc, pyout)
        elif PMIX_INFO == data_type:
            if 'key' not in pysrc:
                return (PMIX_ERR_BAD_PARAM, None)
            memset(&info, 0, sizeof(pmix_info_t))
            rc = pmix_load_info(&info, [pysrc])
            if PMIX_SUCCESS != rc:
                pmix_destruct_info(&info)
                return (rc, None)
            infodest = NULL
            dest = <void**>&infodest
            rc = PMIx_Data_copy(dest, <void*>&info, PMIX_INFO)
            pmix_destruct_info(&info)
            if PMIX_SUCCESS != rc or NULL == infodest:
                return (rc, None)
            ilist = []
            rc = pmix_unload_info(infodest, 1, ilist)
            PMIx_Info_free(infodest, 1)
            if PMIX_SUCCESS != rc or 0 == len(ilist):
                return (rc, None)
            return (rc, ilist[0])
        return (PMIX_ERR_NOT_SUPPORTED, None)

    # Append the unread portion of one buffer to another
    #
    # Returns (rc, destination buffer dict)
    def data_copy_payload(self, pydest, pysrc):
        cdef pmix_data_buffer_t dest
        cdef pmix_data_buffer_t src

        if pydest is None:
            pydest = {}
        elif not isinstance(pydest, dict):
            return (PMIX_ERR_BAD_PARAM, pydest)

        rc = pmix_load_dbuf(&dest, pydest)
        if PMIX_SUCCESS != rc:
            pmix_destruct_dbuf(&dest)
            return (rc, pydest)
        rc = pmix_load_dbuf(&src, pysrc)
        if PMIX_SUCCESS != rc:
            pmix_destruct_dbuf(&src)
            pmix_destruct_dbuf(&dest)
            return (rc, pydest)

        rc = PMIx_Data_copy_payload(&dest, &src)
        if PMIX_SUCCESS == rc:
            pmix_unload_dbuf(&dest, pydest)
        pmix_destruct_dbuf(&src)
        pmix_destruct_dbuf(&dest)
        return (rc, pydest)

    # Extract a buffer's payload as a byte object, emptying the buffer.
    # Note that the library returns only the portion that has not been
    # unpacked yet
    #
    # Returns (rc, {'bytes': bytes, 'size': int})
    def data_unload(self, pybuf):
        cdef pmix_data_buffer_t buf
        cdef pmix_byte_object_t bo

        if not isinstance(pybuf, dict):
            return (PMIX_ERR_BAD_PARAM, None)
        rc = pmix_load_dbuf(&buf, pybuf)
        if PMIX_SUCCESS != rc:
            pmix_destruct_dbuf(&buf)
            return (rc, None)

        bo.bytes = NULL
        bo.size = 0
        rc = PMIx_Data_unload(&buf, &bo)
        pyout = None
        if PMIX_SUCCESS == rc:
            pyout = {}
            pmix_unload_bo(&bo, pyout)
            # the payload was handed to us by the library
            if NULL != bo.bytes:
                free(bo.bytes)
            # unload consumes the buffer, so report it empty
            pmix_unload_dbuf(&buf, pybuf)
        pmix_destruct_dbuf(&buf)
        return (rc, pyout)

    # Replace a buffer's payload with the provided byte object
    #
    # Returns (rc, buffer dict)
    def data_load(self, pybuf, payload):
        cdef pmix_data_buffer_t buf
        cdef pmix_byte_object_t bo

        if pybuf is None:
            pybuf = {}
        elif not isinstance(pybuf, dict):
            return (PMIX_ERR_BAD_PARAM, pybuf)
        rc = pmix_load_dbuf(&buf, pybuf)
        if PMIX_SUCCESS != rc:
            pmix_destruct_dbuf(&buf)
            return (rc, pybuf)
        # the library takes ownership of the payload it is given, which
        # is why pmix_load_bo malloc's it
        rc = pmix_load_bo(&bo, payload)
        if PMIX_SUCCESS != rc:
            pmix_destruct_dbuf(&buf)
            return (rc, pybuf)
        rc = PMIx_Data_load(&buf, &bo)
        if PMIX_SUCCESS == rc:
            pmix_unload_dbuf(&buf, pybuf)
        elif NULL != bo.bytes:
            # it was refused, so the payload is still ours
            free(bo.bytes)
        pmix_destruct_dbuf(&buf)
        return (rc, pybuf)

    # Embed a payload in a buffer. Identical to data_load except that the
    # library copies the payload rather than taking ownership of it
    #
    # Returns (rc, buffer dict)
    def data_embed(self, pybuf, payload):
        cdef pmix_data_buffer_t buf
        cdef pmix_byte_object_t bo

        if pybuf is None:
            pybuf = {}
        elif not isinstance(pybuf, dict):
            return (PMIX_ERR_BAD_PARAM, pybuf)
        rc = pmix_load_dbuf(&buf, pybuf)
        if PMIX_SUCCESS != rc:
            pmix_destruct_dbuf(&buf)
            return (rc, pybuf)
        rc = pmix_load_bo(&bo, payload)
        if PMIX_SUCCESS != rc:
            pmix_destruct_dbuf(&buf)
            return (rc, pybuf)
        rc = PMIx_Data_embed(&buf, &bo)
        if PMIX_SUCCESS == rc:
            pmix_unload_dbuf(&buf, pybuf)
        # embed copies, so the payload remains ours either way
        if NULL != bo.bytes:
            free(bo.bytes)
        pmix_destruct_dbuf(&buf)
        return (rc, pybuf)

    # Compress a block of bytes with the library's loss-less compressor
    #
    # The C API answers a bool - whether it compressed the block at all,
    # which it declines to do when no compression component is available
    # or the block is below the threshold where compression would pay.
    # That answer is reported here as PMIX_SUCCESS or
    # PMIX_ERR_NOT_AVAILABLE, so the return follows the same (rc, data)
    # shape as every other method
    def data_compress(self, pybytes):
        cdef uint8_t *inbytes
        cdef uint8_t *outbytes
        cdef size_t nbytes

        if pybytes is None:
            return (PMIX_ERR_BAD_PARAM, None)
        if isinstance(pybytes, str):
            pyin = pybytes.encode('ascii')
        else:
            pyin = bytes(pybytes)
        if 0 == len(pyin):
            return (PMIX_ERR_BAD_PARAM, None)
        inbytes = <uint8_t*><const char*>pyin
        outbytes = NULL
        nbytes = 0
        if not PMIx_Data_compress(inbytes, len(pyin), &outbytes, &nbytes):
            return (PMIX_ERR_NOT_AVAILABLE, None)
        if NULL == outbytes or 0 == nbytes:
            return (PMIX_ERR_NOT_AVAILABLE, None)
        blist = []
        pmix_unload_bytes(<char*>outbytes, nbytes, blist)
        free(outbytes)
        return (PMIX_SUCCESS, bytes(bytearray(blist)))

    # Decompress a block produced by data_compress. See that method for
    # the meaning of the returned status
    def data_decompress(self, pybytes):
        cdef uint8_t *inbytes
        cdef uint8_t *outbytes
        cdef size_t nbytes

        if pybytes is None:
            return (PMIX_ERR_BAD_PARAM, None)
        if isinstance(pybytes, str):
            pyin = pybytes.encode('ascii')
        else:
            pyin = bytes(pybytes)
        if 0 == len(pyin):
            return (PMIX_ERR_BAD_PARAM, None)
        inbytes = <uint8_t*><const char*>pyin
        outbytes = NULL
        nbytes = 0
        if not PMIx_Data_decompress(inbytes, len(pyin), &outbytes, &nbytes):
            return (PMIX_ERR_NOT_AVAILABLE, None)
        if NULL == outbytes or 0 == nbytes:
            return (PMIX_ERR_NOT_AVAILABLE, None)
        blist = []
        pmix_unload_bytes(<char*>outbytes, nbytes, blist)
        free(outbytes)
        return (PMIX_SUCCESS, bytes(bytearray(blist)))

    def fabric_register(self, dicts):
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

    # Parse a cpuset string of the form "<source>:<range-list>" (e.g.
    # "hwloc:0-3,8") into the Python cpuset dict described in pmix.pxi.
    #
    # @csetstr [INPUT]
    #          - the cpuset string (string)
    #
    # Returns (rc, {'source': str, 'cpus': [int, ...]})
    #
    # NOTE: csetstr is deliberately left unannotated - a ":str" annotation
    # makes Cython reject None at the call boundary before the body's own
    # check can report PMIX_ERR_BAD_PARAM.
    def parse_cpuset_string(self, csetstr):
        cdef pmix_cpuset_t cpuset

        pycpus = {}
        if csetstr is None:
            return (PMIX_ERR_BAD_PARAM, pycpus)
        if isinstance(csetstr, str):
            pycset = csetstr.encode('ascii')
        else:
            pycset = csetstr
        # let the library do the parsing so we honor whatever providers
        # it supports, then render the result back through our converter.
        # the parser can allocate before it detects a malformed range, so
        # destruct unconditionally
        PMIx_Cpuset_construct(&cpuset)
        rc = PMIx_Parse_cpuset_string(pycset, &cpuset)
        if PMIX_SUCCESS == rc:
            rc = pmix_unload_cpuset(&cpuset, pycpus)
        pmix_destruct_cpuset(&cpuset)
        return (rc, pycpus)

    def get_cpuset(self, ref:int):
        cdef pmix_cpuset_t cpuset

        pycpus = {}
        PMIx_Cpuset_construct(&cpuset)
        rc = PMIx_Get_cpuset(&cpuset, ref)
        if PMIX_SUCCESS == rc:
            rc = pmix_unload_cpuset(&cpuset, pycpus)
        pmix_destruct_cpuset(&cpuset)
        return (rc, pycpus)

    def compute_distances(self, pycpus, dicts):
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
        rc = pmix_load_cpuset(&cpuset, pycpus)
        if PMIX_SUCCESS != rc:
            pmix_destruct_cpuset(&cpuset)
            if 0 < sz:
                pmix_free_info(info, sz)
            return (rc, results)

        # compute distances
        rc = PMIx_Compute_distances(&self.topo, &cpuset, info, sz, &distances, &ndist)

        # the library is done with our inputs
        pmix_destruct_cpuset(&cpuset)
        if 0 < sz:
            pmix_free_info(info, sz)

        if PMIX_SUCCESS != rc:
            return (rc, results)

        # convert to Python
        n = 0
        while n < ndist:
            pydist = {}
            if NULL == distances[n].uuid:
                pydist['uuid'] = None
            else:
                pydist['uuid'] = distances[n].uuid.decode('ascii')
            if NULL == distances[n].osname:
                pydist['osname'] = None
            else:
                pydist['osname'] = distances[n].osname.decode('ascii')
            pydist['type'] = distances[n].type
            pydist['mindist'] = distances[n].mindist
            pydist['maxdist'] = distances[n].maxdist
            results.append(pydist)
            n += 1

        # the distance array was allocated by the library, so it must be
        # released by the library
        PMIx_Device_distance_free(distances, ndist)

        # return result
        return (rc, results)

    def progress(self):
        PMIx_Progress()
        return

    # Stop the library's internal progress thread. Once stopped, the
    # library only makes progress when progress() is called, so this is
    # meant for callers that want to drive the event loop themselves.
    #
    # @dicts [INPUT]
    #        - a list of dictionaries, where each
    #          dictionary has a key, value, and val_type
    #          defined as such:
    #          [{key:y, value:val, val_type:ty}, … ]
    #          e.g. PMIX_PROGRESS_THREAD_FLUSH to complete all pending
    #          events before stopping, or PMIX_PROGRESS_THREAD_NAME to
    #          stop one named thread instead of the shared one
    #
    # The C API returns nothing, so PMIX_SUCCESS is reported once the
    # thread has been stopped; a directive that could not be converted is
    # reported instead.
    def progress_thread_stop(self, dicts=None):
        cdef pmix_info_t *info
        cdef pmix_info_t **info_ptr
        cdef size_t ninfo

        # allocate and load pmix info structs from python list of dictionaries
        info_ptr = &info
        rc = pmix_alloc_info(info_ptr, &ninfo, dicts)
        if PMIX_SUCCESS != rc:
            return rc

        # this joins the progress thread, so the GIL must be released -
        # the thread may need it to complete a pending Python callback
        with nogil:
            PMIx_Progress_thread_stop(info, ninfo)
        if 0 < ninfo:
            pmix_free_info(info, ninfo)
        return PMIX_SUCCESS

pmixservermodule = {}
def setmodulefn(k, f):
    global pmixservermodule
    permitted = ['clientconnected', 'clientfinalized', 'abort',
                 'fencenb', 'directmodex', 'publish', 'lookup', 'unpublish',
                 'spawn', 'connect', 'disconnect', 'registerevents',
                 'deregisterevents', 'listener', 'notifyevent', 'query',
                 'toolconnected', 'log', 'allocate', 'jobcontrol',
                 'monitor', 'getcredential', 'validatecredential',
                 'iofpull', 'pushstdin', 'group', 'fabric', 'clientconnected2',
                 'toolconnected2', 'log2', 'sessioncontrol', 'resourceblock']
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
    def init(self, dicts, map):
        cdef pmix_info_t *info
        cdef pmix_info_t **info_ptr
        cdef size_t sz

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

        # setup server module functions
        self.server_module_init(kvkeys)

        # allocate and load pmix info structs from python list of dictionaries
        info_ptr = &info
        rc = pmix_alloc_info(info_ptr, &sz, dicts)
        if sz > 0:
            rc = PMIx_server_init(&self.myserver, info, sz)
        else:
            rc = PMIx_server_init(&self.myserver, NULL, 0)
        return rc

    def server_module_init(self, kvkeys:list):
        # v1.x interfaces
        if 'clientconnected' in kvkeys:
            self.myserver.client_connected = <pmix_server_client_connected_fn_t>clientconnected
        if 'clientfinalized' in kvkeys:
            self.myserver.client_finalized = <pmix_server_client_finalized_fn_t>clientfinalized
        if 'abort' in kvkeys:
            self.myserver.abort = <pmix_server_abort_fn_t>clientaborted
        if 'fencenb' in kvkeys:
            self.myserver.fence_nb = <pmix_server_fencenb_fn_t>fencenb
        if 'directmodex' in kvkeys:
            self.myserver.direct_modex = <pmix_server_dmodex_req_fn_t>directmodex
        if 'publish' in kvkeys:
            self.myserver.publish = <pmix_server_publish_fn_t>publish
        if 'lookup' in kvkeys:
            self.myserver.lookup = <pmix_server_lookup_fn_t>lookup
        if 'unpublish' in kvkeys:
            self.myserver.unpublish = <pmix_server_unpublish_fn_t>unpublish
        if 'spawn' in kvkeys:
            self.myserver.spawn = <pmix_server_spawn_fn_t>spawn
        if 'connect' in kvkeys:
            self.myserver.connect = <pmix_server_connect_fn_t>connect
        if 'disconnect' in kvkeys:
            self.myserver.disconnect = <pmix_server_disconnect_fn_t>disconnect
        if 'registerevents' in kvkeys:
            self.myserver.register_events = <pmix_server_register_events_fn_t>registerevents
        if 'deregisterevents' in kvkeys:
            self.myserver.deregister_events = <pmix_server_deregister_events_fn_t>deregisterevents
        # skip the listener entry as Python servers will never
        # provide their own socket listener thread
        #
        # v2.x interfaces
        if 'notifyevent' in kvkeys:
            self.myserver.notify_event = <pmix_server_notify_event_fn_t>notifyevent
        if 'query' in kvkeys:
            self.myserver.query = <pmix_server_query_fn_t>query
        if 'toolconnected' in kvkeys:
            self.myserver.tool_connected = <pmix_server_tool_connection_fn_t>toolconnected
        if 'log' in kvkeys:
            self.myserver.log = <pmix_server_log_fn_t>log
        if 'allocate' in kvkeys:
            self.myserver.allocate = <pmix_server_alloc_fn_t>allocate
        if 'jobcontrol' in kvkeys:
            self.myserver.job_control = <pmix_server_job_control_fn_t>jobcontrol
        if 'monitor' in kvkeys:
            self.myserver.monitor = <pmix_server_monitor_fn_t>monitor
        # v3.x interfaces
        if 'getcredential' in kvkeys:
            self.myserver.get_credential = <pmix_server_get_cred_fn_t>getcredential
        if 'validatecredential' in kvkeys:
            self.myserver.validate_credential = <pmix_server_validate_cred_fn_t>validatecredential
        if 'iofpull' in kvkeys:
            self.myserver.iof_pull = <pmix_server_iof_fn_t>iofpull
        if 'pushstdin' in kvkeys:
            self.myserver.push_stdin = <pmix_server_stdin_fn_t>pushstdin
        # v4.x interfaces
        if 'group' in kvkeys:
            self.myserver.group = <pmix_server_grp_fn_t>group
        if 'fabric' in kvkeys:
            self.myserver.fabric = <pmix_server_fabric_fn_t>fabric
        # v6.x interfaces
        if 'clientconnected2' in kvkeys:
            self.myserver.client_connected2 = <pmix_server_client_connected2_fn_t>clientconnected2
        if 'toolconnected2' in kvkeys:
            self.myserver.tool_connected2 = <pmix_server_tool_connection2_fn_t>toolconnected2
        if 'log2' in kvkeys:
            self.myserver.log2 = <pmix_server_log2_fn_t>log2
        # pending interfaces
        if 'sessioncontrol' in kvkeys:
            self.myserver.session_control = <pmix_server_session_control_fn_t>sessioncontrol
        if 'resourceblock' in kvkeys:
            self.myserver.resource_block = <pmix_server_resource_block_fn_t>resourceblock

    def finalize(self):
        # finalize
        return PMIx_server_finalize()

    def generate_regex(self, hosts:list):
        cdef char *regex = NULL
        mycomma = ","
        myhosts = mycomma.join(hosts)
        pyhosts = myhosts.encode('ascii')
        rc = PMIx_generate_regex(pyhosts, &regex)
        # the library leaves the output pointer untouched when it fails,
        # so there is nothing to convert
        if PMIX_SUCCESS != rc or NULL == regex:
            return (rc, bytearray(0))
        # load regex appropriately into python bytearray
        ba = pmix_convert_regex(regex)
        return (rc, ba)

    # Compress a list of node names into the current regular expression
    # form, a pmix_regex2_t. This is the generator to use in new code -
    # generate_regex above is the deprecated interface, which produces a
    # bare string and cannot say which component encoded it.
    #
    # @hosts [INPUT]
    #        - a list of node names, or an already comma-delimited string
    #
    # @dicts [INPUT]
    #        - a list of dictionaries, where each
    #          dictionary has a key, value, and val_type
    #          defined as such:
    #          [{key:y, value:val, val_type:ty}, … ]
    #
    # Returns (rc, {'type': str, 'bytes': bytes, 'len': int}), the dict
    # being what parse_regex2 takes back
    def generate_regex2(self, hosts, dicts=None):
        cdef pmix_regex2_t regex
        cdef pmix_info_t *info
        cdef pmix_info_t **info_ptr
        cdef size_t ninfo

        if hosts is None:
            return (PMIX_ERR_BAD_PARAM, None)
        if isinstance(hosts, str):
            myhosts = hosts
        elif isinstance(hosts, bytes):
            myhosts = hosts.decode('ascii')
        else:
            mycomma = ","
            myhosts = mycomma.join(hosts)
        pyhosts = myhosts.encode('ascii')

        # allocate and load pmix info structs from python list of dictionaries
        info_ptr = &info
        rc = pmix_alloc_info(info_ptr, &ninfo, dicts)
        if PMIX_SUCCESS != rc:
            return (rc, None)

        PMIx_Regex2_construct(&regex)
        rc = PMIx_generate_regex2(pyhosts, info, ninfo, &regex)
        if 0 < ninfo:
            pmix_free_info(info, ninfo)
        if PMIX_SUCCESS != rc:
            # a component can fail after setting a field, so release
            # whatever was built before converting nothing
            PMIx_Regex2_destruct(&regex)
            return (rc, None)
        pyregex = pmix_unload_regex2(&regex)
        PMIx_Regex2_destruct(&regex)
        return (rc, pyregex)

    # Expand a pmix_regex2_t produced by generate_regex2 back into the
    # list of values it encodes, preserving their original order.
    #
    # @pyregex [INPUT]
    #          - regex dict: {'type': str, 'bytes': bytes, 'len': int}
    #
    # @dicts [INPUT]
    #        - a list of dictionaries, where each
    #          dictionary has a key, value, and val_type
    #          defined as such:
    #          [{key:y, value:val, val_type:ty}, … ]
    #
    # Returns (rc, [name, name, ...])
    def parse_regex2(self, pyregex, dicts=None):
        cdef pmix_regex2_t regex
        cdef pmix_info_t *info
        cdef pmix_info_t **info_ptr
        cdef size_t ninfo
        cdef char *output = NULL

        names = []
        # the loader constructs the regex first, so it is safe to destruct
        # on every path from here on
        rc = pmix_load_regex2(&regex, pyregex)
        if PMIX_SUCCESS != rc:
            PMIx_Regex2_destruct(&regex)
            return (rc, names)

        # allocate and load pmix info structs from python list of dictionaries
        info_ptr = &info
        rc = pmix_alloc_info(info_ptr, &ninfo, dicts)
        if PMIX_SUCCESS != rc:
            PMIx_Regex2_destruct(&regex)
            return (rc, names)

        rc = PMIx_parse_regex2(&regex, info, ninfo, &output)
        PMIx_Regex2_destruct(&regex)
        if 0 < ninfo:
            pmix_free_info(info, ninfo)
        if PMIX_SUCCESS != rc or NULL == output:
            return (rc, names)
        # the parser hands back a comma-delimited string it allocated
        txt = output.decode('ascii')
        free(output)
        if 0 < len(txt):
            names = txt.split(',')
        return (rc, names)

    def generate_ppn(self, procs:list):
        cdef char *ppn = NULL
        mysemi = ";"
        myprocs = mysemi.join(procs)
        pyprocs = myprocs.encode('ascii')
        rc = PMIx_generate_ppn(pyprocs, &ppn)
        # the library leaves the output pointer untouched when it fails
        if PMIX_SUCCESS != rc or NULL == ppn:
            return (rc, bytearray(0))
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

    # Render a Python cpuset dict into the library's cpuset string, the
    # form in which a cpuset is passed between servers and stored as the
    # PMIX_CPUSET attribute of a process.
    #
    # @pycpus [INPUT]
    #         - cpuset dict: {'source': str, 'cpus': [int, ...]}
    #
    # Returns (rc, cpuset_string)
    def generate_cpuset_string(self, pycpus):
        cdef pmix_cpuset_t cpuset
        cdef char *csetstr = NULL

        rc = pmix_load_cpuset(&cpuset, pycpus)
        if PMIX_SUCCESS != rc:
            pmix_destruct_cpuset(&cpuset)
            return (rc, None)
        rc = PMIx_server_generate_cpuset_string(&cpuset, &csetstr)
        pmix_destruct_cpuset(&cpuset)
        if PMIX_SUCCESS != rc or NULL == csetstr:
            return (rc, None)
        txt = csetstr.decode('ascii')
        free(csetstr)
        return (rc, txt)

    # The inverse of generate_cpuset_string: convert the library's cpuset
    # string form into the Python cpuset dict. This is the server-side
    # entry point; PMIxClient.parse_cpuset_string performs the same
    # conversion through the client entry point.
    #
    # @csetstr [INPUT]
    #          - cpuset string of the form "<source>:<range-list>",
    #            e.g. "hwloc:0-3,8"
    #
    # Returns (rc, {'source': str, 'cpus': [int, ...]})
    def generate_cpuset(self, csetstr):
        cdef pmix_cpuset_t cpuset

        pycpus = {}
        if csetstr is None:
            return (PMIX_ERR_BAD_PARAM, pycpus)
        if isinstance(csetstr, str):
            pycset = csetstr.encode('ascii')
        else:
            pycset = csetstr
        # the parser can allocate before it detects a malformed range, so
        # construct up front and destruct on every path
        PMIx_Cpuset_construct(&cpuset)
        rc = PMIx_server_generate_cpuset(pycset, &cpuset)
        if PMIX_SUCCESS == rc:
            rc = pmix_unload_cpuset(&cpuset, pycpus)
        pmix_destruct_cpuset(&cpuset)
        return (rc, pycpus)

    # Compute the locality string for a process bound to the given cpuset.
    # This is the string a host environment passes as PMIX_LOCALITY_STRING
    # so peers can compute their relative locality.
    #
    # @pycpus [INPUT]
    #         - cpuset dict: {'source': str, 'cpus': [int, ...]}
    #
    # Returns (rc, locality_string). An unbound process has no locality,
    # in which case the string is None and rc is PMIX_SUCCESS.
    def generate_locality_string(self, pycpus):
        cdef pmix_cpuset_t cpuset
        cdef char *locality = NULL

        # the locality computation walks the topology, so be sure we
        # have one loaded before asking for it
        if NULL == self.topo.topology:
            rc = self.load_topology()
            if PMIX_SUCCESS != rc:
                return (rc, None)

        rc = pmix_load_cpuset(&cpuset, pycpus)
        if PMIX_SUCCESS != rc:
            pmix_destruct_cpuset(&cpuset)
            return (rc, None)
        rc = PMIx_server_generate_locality_string(&cpuset, &locality)
        pmix_destruct_cpuset(&cpuset)
        if PMIX_SUCCESS != rc or NULL == locality:
            return (rc, None)
        txt = locality.decode('ascii')
        free(locality)
        return (rc, txt)

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
    def register_nspace(self, ns:str, nlocalprocs:int, dicts):
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
    def register_resources(self, directives):
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
    def deregister_resources(self, directives):
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

    def setup_application(self, ns:str, dicts):
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

    def register_attributes(self, function:str, attrs):
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

    def collect_inventory(self, pydirs):
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

    def deliver_inventory(self, pyinfo, pydirs):
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

    def setup_local_support(self, ns:str, ilist):
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

    def iof_deliver(self, pysrc:dict, pychannel:int, pydata:dict, pydirs):
        cdef pmix_proc_t source
        cdef pmix_iof_channel_t channel
        cdef pmix_byte_object_t bo
        cdef pmix_info_t *directives
        cdef pmix_info_t **directives_ptr
        cdef size_t ndirs
        ndirs   = 0
        channel = pychannel

        # convert pysrc to pmix_proc_t
        pmix_copy_nspace(source.nspace, pysrc['nspace'])
        source.rank = pysrc['rank']

        # convert pydata to pmix_byte_object_t
        data = bytes(pydata['bytes'], 'ascii')
        bo.size = len(data)
        bo.bytes = <char*> PyMem_Malloc(bo.size)
        if not bo.bytes:
            return PMIX_ERR_NOMEM
        pyptr = <const char*>data
        memcpy(bo.bytes, pyptr, bo.size)

        # allocate and load pmix info structs from python list of dictionaries
        directives_ptr = &directives
        rc = pmix_alloc_info(directives_ptr, &ndirs, pydirs)

        # call API
        rc = PMIx_server_IOF_deliver(&source, channel, &bo, directives, ndirs,
                                     NULL, NULL)
        PyMem_Free(bo.bytes)
        if 0 < ndirs:
            pmix_free_info(directives, ndirs)
        return rc

    def define_process_set(self, members, name:str):
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

    def delete_process_set(self, name:str):

        # convert set name
        pyset = name.encode('ascii')
        # delete the set
        rc = PMIx_server_delete_process_set(pyset)
        return rc

    # Collect the job-level information this server holds for the jobs
    # the given procs belong to, packaged as an opaque blob. A host
    # environment forwards the blob to a remote PMIx server, which feeds
    # it back into the library so its own clients can see that job data.
    #
    # @peers [INPUT]
    #        - a list of proc dicts ({'nspace': str, 'rank': int}). Only
    #          the namespaces matter; the ranks are ignored. The list
    #          cannot be empty - there would be no job to collect
    #
    # Returns (rc, {'bytes': bytes, 'size': int}) - the byte-object form
    # every other blob in these bindings uses
    def collect_job_info(self, peers):
        cdef pmix_proc_t *procs
        cdef size_t nprocs
        cdef pmix_data_buffer_t dbuf
        cdef char *blob = NULL
        cdef size_t nblob

        pyblob = {'bytes': b'', 'size': 0}
        if peers is None or 0 == len(peers):
            return (PMIX_ERR_BAD_PARAM, pyblob)

        # convert list of procs to array of pmix_proc_t's
        nprocs = len(peers)
        procs = <pmix_proc_t*> PyMem_Malloc(nprocs * sizeof(pmix_proc_t))
        if not procs:
            return (PMIX_ERR_NOMEM, pyblob)
        rc = pmix_load_procs(procs, peers)
        if PMIX_SUCCESS != rc:
            pmix_free_procs(procs, nprocs)
            return (rc, pyblob)

        # the collection is performed on the progress thread and this
        # call blocks until it completes, so release the GIL
        PMIx_Data_buffer_construct(&dbuf)
        with nogil:
            rc = PMIx_server_collect_job_info(procs, nprocs, &dbuf)
        pmix_free_procs(procs, nprocs)

        if PMIX_SUCCESS == rc:
            # unload transfers ownership of the payload to us
            nblob = 0
            PMIx_Data_buffer_unload(&dbuf, &blob, &nblob)
            if NULL != blob:
                if 0 < nblob:
                    pyblob['bytes'] = blob[:nblob]
                    pyblob['size'] = nblob
                free(blob)
        PMIx_Data_buffer_destruct(&dbuf)
        return (rc, pyblob)

    def session_control(self, sessionID:int, ilist):
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

cdef int clientconnected2(pmix_proc_t *proc, void *server_object,
                          pmix_info_t info[], size_t ninfo,
                          pmix_op_cbfunc_t cbfunc, void *cbdata) with gil:
    keys = pmixservermodule.keys()
    if 'clientconnected2' in keys:
        if not proc:
            return PMIX_ERR_BAD_PARAM
        args = {}
        myproc = []
        pmix_unload_procs(proc, 1, myproc)
        args['proc'] = myproc[0]
        ilist = []
        if NULL != info:
            rc = pmix_unload_info(info, ninfo, ilist)
            if PMIX_SUCCESS != rc:
                return rc
            args['directives'] = ilist
        cbdata_dict = {'cbdata' : <uintptr_t> cbdata, 'cbfunc' : <uintptr_t> cbfunc}
        return pmixservermodule['clientconnected2'](args, pypmix_op_cbfunc, cbdata_dict)

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
                        void *cbdata) noexcept with gil:
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
              pmix_op_cbfunc_t cbfunc, void *cbdata) noexcept with gil:
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
        mydirs = []
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
        mymon = []
        myproc = []
        mydirs = []
        blist = []
        if NULL == monitor:
            return PMIX_ERR_BAD_PARAM
        if NULL != requestor:
            pmix_unload_procs(requestor, 1, myproc)
            args['source'] = myproc[0]
        pmix_unload_info(monitor, 1, mymon)
        args['monitor'] = mymon[0]
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
        mydirs = []
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
        mydirs = []
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
        mydirs = []
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
        mydirs = []
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


# The server library requires that the host complete a group operation by
# executing the provided callback function, and the host is not required to
# do so before returning from this upcall. A Python handler therefore has
# two ways to respond:
#
#   * return PMIX_OPERATION_SUCCEEDED to decline the callback entirely - the
#     server library will complete the operation itself with no results, or
#   * return PMIX_SUCCESS and invoke the supplied callback exactly once with
#     the results, either from within the handler or later from another
#     thread. The 'cbdata_dict' passed to the handler holds only integers,
#     so it may be saved and used after the handler returns, and the server
#     library thread-shifts the callback, so any thread may drive it.
cdef int group(pmix_group_operation_t op, char grp[],
               const pmix_proc_t procs[], size_t nprocs,
               const pmix_info_t directives[], size_t ndirs,
               pmix_info_cbfunc_t cbfunc, void *cbdata) with gil:
    keys = pmixservermodule.keys()
    if 'group' in keys:
        args = {}
        myprocs = []
        mydirs = []
        args['op'] = op
        if NULL == grp:
            return PMIX_ERR_BAD_PARAM
        args['group'] = grp.decode('ascii')
        if NULL != procs:
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
        mydirs = []
        args['op'] = op
        if NULL != directives:
            pmix_unload_info(directives, ndirs, mydirs)
            args['directives'] = mydirs
        cbdata_dict = {'cbdata' : <uintptr_t> cbdata, 'cbfunc' : <uintptr_t> cbfunc}
        return pmixservermodule['fabric'](args, pypmix_info_cbfunc, cbdata_dict)

    return PMIX_ERR_NOT_SUPPORTED

cdef int toolconnected2(pmix_info_t *info, size_t ninfo,
                        pmix_tool_connection_cbfunc_t cbfunc,
                        void *cbdata) noexcept with gil:
    keys = pmixservermodule.keys()
    ret_proc = {'nspace': "UNDEF", 'rank': PMIX_RANK_UNDEF}
    if 'toolconnected2' in keys:
        args = {}
        ilist = []
        if NULL != info:
            pmix_unload_info(info, ninfo, ilist)
            args['directives'] = ilist
        cbdata_dict = {'cbdata' : <uintptr_t> cbdata, 'cbfunc' : <uintptr_t> cbfunc}
        return pmixservermodule['toolconnected2'](args, pypmix_tool_connection_cbfunc, cbdata_dict)

    return PMIX_ERR_NOT_SUPPORTED

cdef int log2(const pmix_proc_t *client,
              const pmix_info_t data[], size_t ndata,
              const pmix_info_t directives[], size_t ndirs,
              pmix_op_cbfunc_t cbfunc, void *cbdata) noexcept with gil:
    keys = pmixservermodule.keys()
    if 'log2' in keys:
        args = {}
        ilist = []
        myproc = []
        mydirs = []
        if NULL == client:
            return PMIX_ERR_BAD_PARAM
        pmix_unload_procs(client, 1, myproc)
        args['source'] = myproc[0]
        if NULL != data:
            pmix_unload_info(data, ndata, ilist)
            args['data'] = ilist
        if NULL != directives:
            pmix_unload_info(directives, ndirs, mydirs)
            args['directives'] = mydirs
        cbdata_dict = {'cbdata' : <uintptr_t> cbdata, 'cbfunc' : <uintptr_t> cbfunc}
        return pmixservermodule['log2'](args, pypmix_op_cbfunc, cbdata_dict)

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

cdef int resourceblock(const pmix_proc_t *requestor,
                       pmix_resource_block_directive_t directive,
                       const char *block,
                       const pmix_resource_unit_t *units, size_t nunits,
                       const pmix_info_t *info, size_t ninfo,
                       pmix_op_cbfunc_t cbfunc, void *cbdata) with gil:
    keys = pmixservermodule.keys()
    if 'resourceblock' in keys:
        args = {}
        myproc = []
        blist = []
        ulist = []
        ilist = []
        barray = None

        if NULL == requestor:
            return PMIX_ERR_BAD_PARAM
        if NULL == units:
            return PMIX_ERR_BAD_PARAM
        pmix_unload_procs(requestor, 1, myproc)
        args['requestor'] = myproc[0]
        args['directive'] = directive
        args['block'] = block
        rc = pmix_unload_units(units, nunits, ulist)
        if PMIX_SUCCESS != rc:
            return rc
        args['units'] = ulist
        if NULL != info:
            rc = pmix_unload_info(info, ninfo, ilist)
            if PMIX_SUCCESS != rc:
                return rc
            args['info'] = ilist
        cbdata_dict = {'cbdata' : <uintptr_t> cbdata, 'cbfunc' : <uintptr_t> cbfunc}
        return pmixservermodule['resourceblock'](args, pypmix_info_cbfunc, cbdata_dict)


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
    def init(self, dicts):
        cdef pmix_info_t *info
        cdef pmix_info_t **info_ptr
        cdef size_t sz
        global myname

        # init myname
        myname = {'nspace':'UNASSIGNED', 'rank':PMIX_RANK_UNDEF}

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
    def disconnect(self, server:dict):
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
    def attach_to_server(self, dicts):
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

    def set_server(self, server:dict, pyinfo):
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

    # Allow a tool to set server module callback functions
    # when it needs to also act as a server
    # Provide the server function pointer module by which this tool will
    # service requests from processes that connect to it
    #
    # @map [INPUT]
    #      - a dictionary of server-module key to Python handler, as
    #        PMIxServer.init takes
    def set_server_module(self, map):
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
        self.server_module_init(kvkeys)
        # wiring the trampolines into our own struct only prepares the
        # module - the library has to be given it, or none of the
        # handlers registered above is ever called
        rc = PMIx_tool_set_server_module(&self.myserver)
        return rc


    def iof_pull(self, pyprocs, iof_channel:int, pydirs, hdlr):
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

    def iof_deregister(self, regid:int, pydirs):
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
        for h in myhdlrs:
            try:
                if iofhdlr == h['refid']:
                    found = True
                    del myhdlrs[n]
                    break
                else:
                    n = n + 1
            except:
                pass
        return rc

    def iof_push(self, pytargets, data:dict, pydirs):
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

# The scheduler is a tool that owns the system's resources, so it
# inherits every client, server and tool method and adds session
# direction on top. Two operations make up the scheduler role proper,
# and both reach it through what it inherits:
#
#   * resource blocks - the scheduler does not call resource_block (a
#     process that IS the scheduler has no one to ask, and the library
#     returns PMIX_ERR_NOT_SUPPORTED). It SERVICES the requests other
#     processes make, by registering a 'resourceblock' handler in the
#     server module map it passes to set_server_module.
#   * session control - likewise serviced through the 'sessioncontrol'
#     module key, while the scheduler's own directives to a session go
#     out through the inherited session_control method.
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
    def init(self, dicts):
        cdef pmix_info_t *info
        cdef pmix_info_t **info_ptr
        cdef size_t sz
        global myname

        # init myname
        myname = {'nspace':'UNASSIGNED', 'rank':PMIX_RANK_UNDEF}

        # init server module in case the scheduler uses it (wire whatever
        # module functions were registered via set_server_module, if any)
        self.server_module_init(list(pmixservermodule.keys()))

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
        return rc, myname

    # Finalize the tool library
    def finalize(self):
        # finalize
        rc = PMIx_tool_finalize()
        return rc

    # direct the RTE to instantiate a session
    def assign_session(self, sessionID:int, allocID:str, ilist, applist:list):
        cdef pmix_info_t *info
        cdef pmix_info_t **info_ptr
        cdef size_t sz
        # convert the info list
        info_ptr = &info
        rc = pmix_alloc_info(info_ptr, &sz, ilist)
        if PMIX_SUCCESS != rc:
            return rc
        # NOTE: the PMIx library does not yet expose a C entry point that lets
        # a scheduler directly instantiate a session from an application list.
        # When one is added, this method must convert applist and invoke it.
        # Until then, release what we converted and report the gap rather than
        # silently returning None. See MISSING_BINDINGS.md.
        if 0 < sz:
            pmix_free_info(info, sz)
        return PMIX_ERR_NOT_SUPPORTED

