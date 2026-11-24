#file: pmix.pyx
#
# Copyright (c) 2022      Nanook Consulting. All rights reserved

from libc.string cimport memset, strncpy, strcpy, strlen, strdup

from libc.stdlib cimport malloc, free, getenv
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

# The C "bool". construct.py rewrites every bool in the generated .pxd to
# Cython's bint, which is a C int - correct for a struct field, where the C
# compiler still sees the real declaration and does the store, but wrong for
# an array the bindings allocate and walk themselves. A PMIX_BOOL data array
# is bool-sized on the wire (pmix_bfrops_base_pack_bool reads a bool *), so
# the loaders need the real type to size and stride it.
cdef extern from "stdbool.h":
    ctypedef bint c_bool "bool"

# pull in all the constant definitions - we
# store them in a separate file for neatness
include "pmix_constants.pxi"
include "pmix.pxi"

active = myLock()
# Event handlers and IOF handlers, each as {'refid', 'hdlr'}. They are
# kept apart because the library numbers the two independently - both
# from zero - so one list would hand an event to an IOF handler that
# shares its refid.
myhdlrs = []
myiofhdlrs = []
# The event or IOF registration in flight, if any - see _hdlr_find.
# _reglock admits one registration at a time, so a refid nobody holds
# belongs to that one.
_pending_evreg = None
_pending_iofreg = None
_reglock = threading.Lock()

# The library's own answer to "is the calling thread the one running its
# event loop?" - the progress thread, or whoever is inside PMIx_Progress()
# when the host drives progress itself. It is not part of the public API,
# but it is exported, and these bindings are built with the library
cdef extern from *:
    """
    #include <stdbool.h>
    extern bool pmix_progress_thread_is_current(void);
    """
    bint pmix_progress_thread_is_current() nogil

# Is this the thread that runs the library's event loop - the one every
# upcall, event handler and _nb callback runs on? A blocking operation
# started there cannot complete: it waits for an event the loop can only
# service once the current one - the caller - returns. For the library's
# own blocking calls the library answers PMIX_ERR_WOULD_BLOCK; the
# methods that wait in Python (on 'active', or for _reglock, which a
# registration on another thread may be holding while it waits for this
# very thread) have to ask first.
#
# The test used to be "is this a threading._DummyThread?", which is true
# of any thread Python did not start - one from _thread, or from another
# extension - and refused those for nothing
def _on_library_thread():
    return pmix_progress_thread_is_current()
myname = {}

# PMIx_Init and PMIx_tool_init take the variables that say who this
# process is, and how it reaches its server, out of the C environment.
# os.environ is the copy Python made when it started, and the environment
# Python gives a process it starts: drop from it every PMIX_ variable the
# C environment no longer holds
def _pmix_sync_environ():
    for key in [k for k in os.environ if k.startswith('PMIX_')]:
        if NULL == getenv(key.encode('UTF-8')):
            del os.environ[key]


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
    pmix_byte_object_t *bo       # ditto - the IOF payloads, which the
                                 #   library holds until it completes
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
    if NULL != cd.bo:
        if NULL != cd.bo.bytes:
            PyMem_Free(cd.bo.bytes)
        PyMem_Free(cd.bo)
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
# so the GIL must be acquired.
#
# Every trampoline converts what the library handed it before anything
# else, because all of it is reclaimed the moment the trampoline returns.
# A conversion that raises must not end the trampoline there: the
# caller's callback would never run, the caddy would leak, and for the
# forms with a release function the library's data would never be given
# back. So each conversion is caught and turned into an error status for
# the callback, and everything after it always runs.
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

# Finish a non-blocking operation: reclaim the caddy and hand the results
# to the caller's callback
cdef void pypmix_client_finish(void *cbdata, args) noexcept with gil:
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
        entry['cbfunc'](*args, entry['cbdata'])
    except:
        traceback.print_exc()

# Trampoline for non-blocking APIs that take a pmix_info_cbfunc_t
cdef void pypmix_client_info_cbfunc(pmix_status_t status,
                                    pmix_info_t *info, size_t ninfo,
                                    void *cbdata,
                                    pmix_release_cbfunc_t release_fn,
                                    void *release_cbdata) noexcept with gil:
    # the results belong to the library, so convert them to Python before
    # telling it that we are done with them
    pyresults = []
    prc = PMIX_SUCCESS
    try:
        if NULL != info and 0 < ninfo:
            prc = pmix_unload_info(info, ninfo, pyresults)
    except:
        traceback.print_exc()
        prc = PMIX_ERROR
    if NULL != release_fn:
        release_fn(release_cbdata)
    if PMIX_SUCCESS != prc and PMIX_SUCCESS == status:
        status = prc
    pypmix_client_finish(cbdata, (status, pyresults))

# True when an info array asks PMIx_Get for a pointer into the library's
# own storage. A Python caller always gets a copy, and the copy is all it
# can keep, so both forms of get refuse the directive rather than release
# - or leak - what the library answered with. Checked on the C array the
# library will read: a bytes key, or one with something after a NUL, is
# the same key there
cdef bint pmix_asks_pointer_values(pmix_info_t *info, size_t ninfo):
    cdef size_t n = 0
    while n < ninfo:
        if PMIx_Check_key(info[n].key, _PMIX_GET_POINTER_VALUES) and \
           (PMIX_BOOL != info[n].value.type or info[n].value.data.flag):
            return True
        n += 1
    return False

# Trampoline for non-blocking APIs that take a pmix_value_cbfunc_t. The
# library hands the value over to its receiver - see PMIx_Get(3) - so it
# is converted here and then released. Only a PMIX_GET_POINTER_VALUES
# answer stays the library's, and get_nb refuses that directive
cdef void pypmix_client_value_cbfunc(pmix_status_t status,
                                     pmix_value_t *kv,
                                     void *cbdata) noexcept with gil:
    pyval = None
    try:
        if NULL != kv:
            pyval = pmix_unload_value(kv)
            if pyval is None and PMIX_SUCCESS == status:
                status = PMIX_ERR_NOT_SUPPORTED
    except:
        traceback.print_exc()
        pyval = None
        if PMIX_SUCCESS == status:
            status = PMIX_ERROR
    if NULL != kv:
        PMIx_Value_free(kv, 1)
    pypmix_client_finish(cbdata, (status, pyval))

# Trampoline for non-blocking APIs that take a pmix_lookup_cbfunc_t. As
# above, the returned pdata array is the library's
cdef void pypmix_client_lookup_cbfunc(pmix_status_t status,
                                      pmix_pdata_t data[], size_t ndata,
                                      void *cbdata) noexcept with gil:
    pyresults = []
    prc = PMIX_SUCCESS
    try:
        if NULL != data and 0 < ndata:
            prc = pmix_unload_pdata(data, ndata, pyresults)
    except:
        traceback.print_exc()
        prc = PMIX_ERROR
    if PMIX_SUCCESS != prc and PMIX_SUCCESS == status:
        status = prc
    pypmix_client_finish(cbdata, (status, pyresults))

# Trampoline for non-blocking APIs that take a pmix_spawn_cbfunc_t
cdef void pypmix_client_spawn_cbfunc(pmix_status_t status,
                                     pmix_nspace_t nspace,
                                     void *cbdata) noexcept with gil:
    # the nspace is released by the library upon our return, so decode it
    # into a Python string now. A failed spawn has no namespace to report,
    # and the parameter is a bare array pointer that may be NULL
    pyns = None
    try:
        if PMIX_SUCCESS == status and NULL != nspace:
            pyns = pmix_decode_str(nspace)
    except:
        traceback.print_exc()
        status = PMIX_ERROR
    pypmix_client_finish(cbdata, (status, pyns))

# Trampoline for non-blocking APIs that take a pmix_credential_cbfunc_t.
# The credential's payload is handed to us - its receiver - to keep, so
# it is copied out and freed here. The byte object carrying it, and the
# info array, stay the library's
cdef void pypmix_client_credential_cbfunc(pmix_status_t status,
                                          pmix_byte_object_t *credential,
                                          pmix_info_t info[], size_t ninfo,
                                          void *cbdata) noexcept with gil:
    cred = {}
    pyresults = []
    prc = PMIX_SUCCESS
    try:
        if NULL != credential and NULL != credential.bytes and 0 < credential.size:
            blist = []
            pmix_unload_bytes(credential.bytes, credential.size, blist)
            cred['bytes'] = bytearray(blist)
            cred['size'] = credential.size
        if NULL != info and 0 < ninfo:
            prc = pmix_unload_info(info, ninfo, pyresults)
    except:
        traceback.print_exc()
        prc = PMIX_ERROR
    if NULL != credential and NULL != credential.bytes:
        free(credential.bytes)
        credential.bytes = NULL
        credential.size = 0
    if PMIX_SUCCESS != prc and PMIX_SUCCESS == status:
        status = prc
    pypmix_client_finish(cbdata, (status, cred, pyresults))

# Trampoline for non-blocking APIs that take a pmix_validation_cbfunc_t.
# Same shape as the info trampoline, but without a release function -
# the library reclaims the array itself
cdef void pypmix_client_validation_cbfunc(pmix_status_t status,
                                          pmix_info_t info[], size_t ninfo,
                                          void *cbdata) noexcept with gil:
    pyresults = []
    prc = PMIX_SUCCESS
    try:
        if NULL != info and 0 < ninfo:
            prc = pmix_unload_info(info, ninfo, pyresults)
    except:
        traceback.print_exc()
        prc = PMIX_ERROR
    if PMIX_SUCCESS != prc and PMIX_SUCCESS == status:
        status = prc
    pypmix_client_finish(cbdata, (status, pyresults))

# Trampoline for non-blocking APIs that take a pmix_device_dist_cbfunc_t
cdef void pypmix_client_devdist_cbfunc(pmix_status_t status,
                                       pmix_device_distance_t *dist,
                                       size_t ndist,
                                       void *cbdata,
                                       pmix_release_cbfunc_t release_fn,
                                       void *release_cbdata) noexcept with gil:
    # convert the distances before telling the library we are done with them
    pyresults = []
    try:
        if NULL != dist:
            pmix_unload_devdists(dist, ndist, pyresults)
    except:
        traceback.print_exc()
        pyresults = []
        if PMIX_SUCCESS == status:
            status = PMIX_ERROR
    if NULL != release_fn:
        release_fn(release_cbdata)
    pypmix_client_finish(cbdata, (status, pyresults))

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
#
# These helpers run after the caddy exists, so none of them may raise: an
# exception would leave the method without freeing it. Each reports a
# failure as a status, and the method releases the caddy
cdef int pypmix_nb_add_dirs(pypmix_nb_cbdata_t *cd, pydirs):
    cdef pmix_info_t **dirs_ptr
    dirs_ptr = &cd.dirs
    try:
        return pmix_alloc_info(dirs_ptr, &cd.ndirs, pydirs)
    except Exception:
        traceback.print_exc()
        return PMIX_ERR_BAD_PARAM

# Park the operation's target procs in the caddy. As in the blocking
# forms, an absent or empty list means "this proc's entire job"
cdef int pypmix_nb_add_procs(pypmix_nb_cbdata_t *cd, peers, nspace):
    if peers is not None:
        peers = pmix_listcopy(peers)
        if peers is None:
            return PMIX_ERR_BAD_PARAM
    if peers is not None and 0 < len(peers):
        cd.nprocs = len(peers)
        cd.procs = <pmix_proc_t*> PyMem_Malloc(cd.nprocs * sizeof(pmix_proc_t))
        if not cd.procs:
            cd.nprocs = 0
            return PMIX_ERR_NOMEM
        return pmix_load_procs(cd.procs, cd.nprocs, peers)
    cd.nprocs = 1
    cd.procs = <pmix_proc_t*> PyMem_Malloc(sizeof(pmix_proc_t))
    if not cd.procs:
        cd.nprocs = 0
        return PMIX_ERR_NOMEM
    pmix_copy_nspace(cd.procs[0].nspace, nspace)
    cd.procs[0].rank = PMIX_RANK_WILDCARD
    return PMIX_SUCCESS

# Park a byte object in the caddy. The IOF calls hand the library a
# payload it holds until the operation completes, so it cannot be the
# stack copy the blocking forms use
cdef int pypmix_nb_add_bo(pypmix_nb_cbdata_t *cd, pybo):
    cdef const char *ptr
    if pybo is None:
        return PMIX_SUCCESS
    cd.bo = <pmix_byte_object_t*> PyMem_Malloc(sizeof(pmix_byte_object_t))
    if not cd.bo:
        return PMIX_ERR_NOMEM
    cd.bo.bytes = NULL
    cd.bo.size = 0
    pybytes = pybo.get('bytes') if isinstance(pybo, dict) else pybo
    if pybytes is None:
        return PMIX_SUCCESS
    pybytes = pmix_payload(pybytes)
    if pybytes is None:
        return PMIX_ERR_TYPE_MISMATCH
    if 0 == len(pybytes):
        return PMIX_SUCCESS
    cd.bo.size = len(pybytes)
    cd.bo.bytes = <char*> PyMem_Malloc(cd.bo.size)
    if not cd.bo.bytes:
        cd.bo.size = 0
        return PMIX_ERR_NOMEM
    ptr = <const char*>pybytes
    memcpy(cd.bo.bytes, ptr, cd.bo.size)
    return PMIX_SUCCESS

# Park a NULL-terminated key array in the caddy. A None/empty list is
# left as NULL, which the APIs that take one read as "all keys"
cdef int pypmix_nb_add_keys(pypmix_nb_cbdata_t *cd, pykeys):
    cdef size_t nstrings
    if pykeys is None:
        return PMIX_SUCCESS
    pykeys = pmix_listcopy(pykeys)
    if pykeys is None:
        return PMIX_ERR_BAD_PARAM
    if 0 == len(pykeys):
        return PMIX_SUCCESS
    nstrings = len(pykeys)
    cd.keys = <char **> malloc((nstrings + 1) * sizeof(char*))
    if NULL == cd.keys:
        return PMIX_ERR_NOMEM
    memset(cd.keys, 0, (nstrings + 1) * sizeof(char*))
    return pmix_load_argv(cd.keys, nstrings, pykeys)

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
pypmix_validation_cbfunc_t      =   Callable[[int, list, dict], None]

# Completing an upcall, exactly once
#
# Every server upcall hands its Python handler one of the helpers below
# plus a 'cbdata_dict' holding the library's C callback and its cbdata as
# integers. The library's contract is that the request is completed
# exactly once: either the host returns PMIX_SUCCESS and calls the
# callback (now, or later from any thread), or it returns anything else
# and never calls it, in which case the library completes the request
# itself. Both halves have to hold, because the library releases the
# request on completion - a second completion is a use-after-free, and a
# missing one leaves the requester waiting forever.
#
# The 'cbfunc' entry of the dict is the token for that. A helper pops it
# before calling the C callback, so only the first call does anything.
# The trampoline settles its own return value against the same token
# (pypmix_settle): if the handler has already completed, the library is
# told PMIX_SUCCESS whatever the handler returned; if the handler is
# returning anything else, the trampoline pops the token itself so a
# late call from a Timer the handler left behind is a no-op. dict.pop is
# atomic under the GIL, which is what lets the two race safely.
#
# pypmix_take_cbfunc answers the callback as an integer, or None when the
# token has already been taken - which is not the same as a NULL callback
def pypmix_take_cbfunc(dict cbdata_dict):
    return cbdata_dict.pop('cbfunc', None)

# The callback a helper is to call, as a C pointer value: 0 when there is
# none, whether because it was NULL or because it was already taken
def pypmix_take_cbptr(dict cbdata_dict):
    cbf = pypmix_take_cbfunc(cbdata_dict)
    if cbf is None:
        return 0
    return cbf

# The status a trampoline returns to the library for its handler
def pypmix_settle(rc, cbdata_dict):
    if cbdata_dict is None:
        # nothing was handed out, so nothing can have completed
        if isinstance(rc, int) and not isinstance(rc, bool) and \
           -2147483648 <= rc <= 2147483647:
            return rc
        return PMIX_ERROR
    # a status the library cannot be given is no status: it would only
    # fail its conversion to a C int after the token was taken, and the
    # trampoline would then report the completion it had just disarmed
    if not isinstance(rc, int) or isinstance(rc, bool) or \
       not (-2147483648 <= rc <= 2147483647):
        print("server module handler returned", repr(rc),
              "rather than a PMIx status")
        rc = PMIX_ERROR
    if PMIX_SUCCESS == rc:
        return rc
    if pypmix_take_cbfunc(cbdata_dict) is None:
        # the handler completed the request and then reported something
        # else: the completion is what the library has acted on
        return PMIX_SUCCESS
    return rc

# The same, for a handler that raised
def pypmix_settle_raise(cbdata_dict):
    traceback.print_exc()
    return pypmix_settle(PMIX_ERROR, cbdata_dict)

# The status a handler passed a helper, as a C status. A handler that
# passes something else has still asked for the request to be completed,
# and it is - as a failure - rather than raising out of the helper, which
# from a Timer thread would simply end the thread and leave the request
# unanswered
cdef pmix_status_t pypmix_status_of(rc):
    if isinstance(rc, int) and not isinstance(rc, bool):
        if -2147483648 <= rc <= 2147483647:
            return rc
    print("server module completion given", repr(rc), "rather than a PMIx status")
    return PMIX_ERROR

def pypmix_op_cbfunc(rc, dict cbdata_dict):
    cdef pmix_op_cbfunc_t cbfunc
    cdef void* cbdata
    cbfunc = <pmix_op_cbfunc_t> <uintptr_t> pypmix_take_cbptr(cbdata_dict)
    cbdata = <void*> <uintptr_t> cbdata_dict['cbdata']

    cdef pmix_status_t crc = pypmix_status_of(rc)

    if NULL == cbfunc:
        return

    with nogil:
        cbfunc(crc, cbdata)

# Every helper that carries data has to complete the request even when
# the data cannot be converted - the library's callback is the only thing
# that will ever answer the requester. So a conversion failure is reported
# through the callback, with no data, rather than by returning early
def pypmix_info_cbfunc(rc, refarginfo, dict cbdata_dict):
    cdef pmix_info_cbfunc_t cbfunc = <pmix_info_cbfunc_t> <uintptr_t> pypmix_take_cbptr(cbdata_dict)
    cdef void* cbdata = <void*> <uintptr_t> cbdata_dict['cbdata']
    cdef pmix_info_t *info
    cdef pmix_info_t **info_ptr
    cdef pypmix_info_cbdata_t *icbd
    cdef size_t ninfo = 0
    cdef pmix_status_t crc
    info              = NULL
    info_ptr          = &info
    icbd              = NULL

    if NULL == cbfunc:
        return

    try:
        prc = pmix_alloc_info(info_ptr, &ninfo, refarginfo)
    except:
        traceback.print_exc()
        prc = PMIX_ERR_BAD_PARAM
    if PMIX_SUCCESS == prc and 0 < ninfo:
        icbd = <pypmix_info_cbdata_t *> malloc(sizeof(pypmix_info_cbdata_t))
        if NULL == icbd:
            pmix_free_info(info, ninfo)
            prc = PMIX_ERR_NOMEM
        else:
            icbd.info = info
            icbd.ninfo = ninfo
    if PMIX_SUCCESS != prc:
        print("Error transferring info to C:", prc)
        crc = prc
        with nogil:
            cbfunc(crc, NULL, 0, cbdata, NULL, NULL)
        return

    crc = pypmix_status_of(rc)
    if NULL == icbd:
        with nogil:
            cbfunc(crc, NULL, 0, cbdata, NULL, NULL)
    else:
        with nogil:
            cbfunc(crc, info, ninfo, cbdata, info_release, <void*> icbd)

# A modex payload is packed binary. It may be handed over as a bytearray,
# bytes, a list of ints, or None for "no data" - a handler reporting that
# it found nothing has no payload to pass
def pypmix_modex_cbfunc(status, ret_data, dict cbdata_dict):
    cdef char *data
    cdef const char *src
    cdef size_t size
    cdef pmix_modex_cbfunc_t cbfunc = <pmix_modex_cbfunc_t> <uintptr_t> pypmix_take_cbptr(cbdata_dict)
    cdef void* cbdata = <void*> <uintptr_t> cbdata_dict['cbdata']
    cdef pypmix_modex_cbdata_t *mcbd
    cdef pmix_status_t crc

    if NULL == cbfunc:
        return

    # strdup would stop at the first NUL byte, which for anything but an
    # all-ASCII blob hands the peer a truncated - usually empty - modex
    prc = PMIX_SUCCESS
    pybytes = b''
    if ret_data is not None:
        try:
            pybytes = bytes(ret_data)
        except:
            traceback.print_exc()
            prc = PMIX_ERR_BAD_PARAM
    size = len(pybytes)
    data = NULL
    mcbd = NULL
    if PMIX_SUCCESS == prc and 0 < size:
        data = <char *> malloc(size)
        mcbd = <pypmix_modex_cbdata_t *> malloc(sizeof(pypmix_modex_cbdata_t))
        if NULL == data or NULL == mcbd:
            free(data)
            free(mcbd)
            data = NULL
            mcbd = NULL
            prc = PMIX_ERR_NOMEM
        else:
            src = <const char*>pybytes
            memcpy(data, src, size)
            mcbd.data = data
            mcbd.size = size
    if PMIX_SUCCESS != prc:
        print("Error transferring modex payload to C:", prc)
        crc = prc
        with nogil:
            cbfunc(crc, NULL, 0, cbdata, NULL, NULL)
        return

    crc = pypmix_status_of(status)
    if NULL == mcbd:
        with nogil:
            cbfunc(crc, NULL, 0, cbdata, NULL, NULL)
    else:
        with nogil:
            cbfunc(crc, data, size, cbdata, modex_release, mcbd)

def pypmix_lookup_cbfunc(rc, pdata, dict cbdata_dict):
    cdef pmix_lookup_cbfunc_t cbfunc = <pmix_lookup_cbfunc_t> <uintptr_t> pypmix_take_cbptr(cbdata_dict)
    cdef void* cbdata = <void*> <uintptr_t> cbdata_dict['cbdata']
    cdef pmix_pdata_t *pd;
    cdef size_t ndata = 0;
    cdef pmix_status_t crc

    if NULL == cbfunc:
        return

    pd = NULL
    prc = PMIX_SUCCESS
    # the token is taken, so nothing below may raise before the callback
    # has been called - an exception here would leave the requester
    # waiting for an answer nobody can now give
    try:
        if pdata is not None and not isinstance(pdata, (list, tuple)):
            prc = PMIX_ERR_BAD_PARAM
        elif pdata is not None and 0 < len(pdata):
            ndata = len(pdata)
            # pmix_free_pdata releases this with the Python allocator, so
            # it has to come from there
            pd = <pmix_pdata_t*> PyMem_Malloc(ndata * sizeof(pmix_pdata_t))
            if NULL == pd:
                prc = PMIX_ERR_NOMEM
            else:
                memset(pd, 0, ndata * sizeof(pmix_pdata_t))
                prc = pmix_load_pdata(pd, ndata, pdata)
    except:
        traceback.print_exc()
        prc = PMIX_ERR_BAD_PARAM
    if PMIX_SUCCESS != prc:
        print("Error transferring lookup results to C:", prc)
        if NULL != pd:
            pmix_free_pdata(pd, ndata)
        crc = prc
        with nogil:
            cbfunc(crc, NULL, 0, cbdata)
        return

    crc = pypmix_status_of(rc)
    with nogil:
        cbfunc(crc, pd, ndata, cbdata)

    # the pdata carry loaded values, so release them rather than just
    # freeing the block. The library copies what it needs before the
    # callback returns
    if NULL != pd:
        pmix_free_pdata(pd, ndata)

def pypmix_spawn_cbfunc(rc, nspace, dict cbdata_dict):
    cdef pmix_spawn_cbfunc_t cbfunc = <pmix_spawn_cbfunc_t> <uintptr_t> pypmix_take_cbptr(cbdata_dict)
    cdef void* cbdata = <void*> <uintptr_t> cbdata_dict['cbdata']
    cdef pmix_nspace_t c_nspace
    cdef pmix_status_t crc = pypmix_status_of(rc)

    if NULL == cbfunc:
        return

    # always start from an empty namespace: a failed spawn has none to
    # report, and the array is passed to the library either way, so it
    # must never be left holding stack garbage. pmix_copy_nspace takes a
    # str or bytes, and raises for a name that does not fit the field
    memset(c_nspace, 0, sizeof(pmix_nspace_t))
    if PMIX_SUCCESS == rc and nspace is not None:
        try:
            pmix_copy_nspace(c_nspace, nspace)
        except:
            traceback.print_exc()
            memset(c_nspace, 0, sizeof(pmix_nspace_t))
            crc = PMIX_ERR_BAD_PARAM

    with nogil:
        cbfunc(crc, c_nspace, cbdata)


def pypmix_tool_connection_cbfunc(rc, proc, dict cbdata_dict):
    cdef pmix_tool_connection_cbfunc_t cbfunc = <pmix_tool_connection_cbfunc_t> <uintptr_t> pypmix_take_cbptr(cbdata_dict)
    cdef void* cbdata = <void*> <uintptr_t> cbdata_dict['cbdata']
    cdef pmix_proc_t c_proc
    cdef pmix_proc_t *pptr = NULL
    cdef pmix_status_t crc = pypmix_status_of(rc)

    if NULL == cbfunc:
        return

    # a refusal assigns nobody, and the library accepts a NULL proc for it
    if PMIX_SUCCESS == rc:
        memset(&c_proc, 0, sizeof(pmix_proc_t))
        try:
            pmix_copy_nspace(c_proc.nspace, proc['nspace'])
            c_proc.rank = proc['rank']
            pptr = &c_proc
        except:
            traceback.print_exc()
            crc = PMIX_ERR_BAD_PARAM

    with nogil:
        cbfunc(crc, pptr, cbdata)

def pypmix_credential_cbfunc(rc, byteobject, info, dict cbdata_dict):
    cdef pmix_credential_cbfunc_t cbfunc = <pmix_credential_cbfunc_t> <uintptr_t> pypmix_take_cbptr(cbdata_dict)
    cdef void* cbdata = <void*> <uintptr_t> cbdata_dict['cbdata']
    cdef size_t ninfo = 0
    cdef pmix_byte_object_t c_byteobject
    cdef pmix_info_t *c_info = NULL
    cdef pmix_status_t crc = pypmix_status_of(rc)

    if NULL == cbfunc:
        return

    # construct unconditionally - a failed request has no credential to
    # report, and the struct is passed to the library on that path too.
    # What a host hands up through this callback is only lent: the
    # library copies the payload before it returns (cred_relay), so it is
    # released here afterwards
    c_byteobject.bytes = NULL
    c_byteobject.size = 0
    if PMIX_SUCCESS == rc:
        try:
            prc = pmix_load_bo(&c_byteobject, byteobject)
            if PMIX_SUCCESS == prc:
                prc = pmix_alloc_info(&c_info, &ninfo, info)
        except:
            traceback.print_exc()
            prc = PMIX_ERR_BAD_PARAM
        if PMIX_SUCCESS != prc:
            print("Error transferring credential to C:", prc)
            if NULL != c_byteobject.bytes:
                free(c_byteobject.bytes)
            c_byteobject.bytes = NULL
            c_byteobject.size = 0
            crc = prc

    with nogil:
        cbfunc(crc, &c_byteobject, c_info, ninfo, cbdata)

    if NULL != c_byteobject.bytes:
        free(c_byteobject.bytes)
    if 0 < ninfo:
        pmix_free_info(c_info, ninfo)

# The C callback is (status, info, ninfo, cbdata), and so is this:
# cbfunc(status, info, cbdata). An older form of this helper also took an
# unused credential ahead of the info - cbfunc(status, cred, info,
# cbdata) - and handlers written against it are still accepted
def pypmix_validation_cbfunc(rc, *args):
    cdef pmix_validation_cbfunc_t cbfunc
    cdef void* cbdata
    cdef size_t ninfo = 0
    cdef pmix_info_t *c_info = NULL
    cdef pmix_status_t crc = pypmix_status_of(rc)

    if 2 == len(args):
        info, cbdata_dict = args
    elif 3 == len(args):
        info, cbdata_dict = args[1], args[2]
    else:
        # still complete the request, as a failure, if we can find it
        print("pypmix_validation_cbfunc(status, info, cbdata) given",
              len(args) + 1, "arguments")
        info = None
        cbdata_dict = args[-1] if 0 < len(args) else None
        crc = PMIX_ERR_BAD_PARAM
        rc = crc
    if not isinstance(cbdata_dict, dict):
        raise TypeError("pypmix_validation_cbfunc: cbdata must be the dict the handler was given")
    cbfunc = <pmix_validation_cbfunc_t> <uintptr_t> pypmix_take_cbptr(cbdata_dict)
    cbdata = <void*> <uintptr_t> cbdata_dict['cbdata']

    if NULL == cbfunc:
        return

    if PMIX_SUCCESS == rc:
        try:
            prc = pmix_alloc_info(&c_info, &ninfo, info)
        except:
            traceback.print_exc()
            prc = PMIX_ERR_BAD_PARAM
        if PMIX_SUCCESS != prc:
            print("Error transferring info to C:", prc)
            crc = prc

    with nogil:
        cbfunc(crc, c_info, ninfo, cbdata)

    if 0 < ninfo:
        pmix_free_info(c_info, ninfo)


# The three callbacks below complete the blocking bridge in section 4a of
# the bindings guide: the method that started the request is waiting on
# 'active', so active.set() must run on every path or it waits forever.
# Each therefore records a result before anything that can raise, and the
# info-carrying ones always record one - an empty list on failure - so a
# failed call can never return the previous call's results.
cdef void dmodx_cbfunc(pmix_status_t status,
                       char *data, size_t sz,
                       void *cbdata) noexcept with gil:
    global active
    # the blob is packed binary and the library frees it on our return, so
    # slice it to its recorded length and copy it now. Letting Cython
    # convert the bare char* would run strlen over it
    try:
        if PMIX_SUCCESS == status and NULL != data and 0 < sz:
            active.cache_data(data[:sz], sz)
        else:
            active.cache_data(None, 0)
    except:
        traceback.print_exc()
        active.cache_data(None, 0)
        if PMIX_SUCCESS == status:
            status = PMIX_ERROR
    active.set(status)
    return

cdef void setupapp_cbfunc(pmix_status_t status,
                          pmix_info_t info[], size_t ninfo,
                          void *provided_cbdata,
                          pmix_op_cbfunc_t cbfunc, void *cbdata) noexcept with gil:
    global active
    ilist = []
    try:
        if PMIX_SUCCESS == status:
            rc = pmix_unload_info(info, ninfo, ilist)
            if PMIX_SUCCESS != rc:
                ilist = []
                status = rc
    except:
        traceback.print_exc()
        ilist = []
        status = PMIX_ERROR
    active.cache_info(ilist)
    active.set(status)
    if (NULL != cbfunc):
        cbfunc(PMIX_SUCCESS, cbdata)
    return

cdef void collectinventory_cbfunc(pmix_status_t status, pmix_info_t info[],
                                  size_t ninfo, void *cbdata,
                                  pmix_release_cbfunc_t release_fn,
                                  void *release_cbdata) noexcept with gil:
    global active
    ilist = []
    try:
        if PMIX_SUCCESS == status:
            rc = pmix_unload_info(info, ninfo, ilist)
            if PMIX_SUCCESS != rc:
                ilist = []
                status = rc
    except:
        traceback.print_exc()
        ilist = []
        status = PMIX_ERROR
    active.cache_info(ilist)
    active.set(status)
    if (NULL != release_fn):
        release_fn(release_cbdata)
    return

cdef void pyiofhandler(size_t iofhdlr_id, pmix_iof_channel_t channel,
                       pmix_proc_t *source, pmix_byte_object_t *payload,
                       pmix_info_t info[], size_t ninfo) noexcept with gil:
    # IOF delivery has no completion to owe, so a delivery that cannot be
    # converted is reported and dropped - but never allowed to unwind
    # into the library
    try:
        pychannel = int(channel)
        pyiof_id  = int(iofhdlr_id)

        # convert the source to python
        pysource = None
        if NULL != source:
            pysource = {'nspace': pmix_decode_str(source[0].nspace), 'rank': source[0].rank}

        # convert the inbound info to python
        pyinfo = []
        if NULL != info and 0 < ninfo:
            rc = pmix_unload_info(info, ninfo, pyinfo)
            if PMIX_SUCCESS != rc:
                print("Unable to unload IOF info structs")
                return

        # convert payload to python byteobject. IOF data is a raw stream,
        # not a string: letting Cython convert the bare char* runs strlen
        # over it, which truncates any payload carrying a NUL and reads
        # past the end of one that carries none
        pybytes = {'bytes': b'', 'size': 0}
        if NULL != payload:
            pmix_unload_bo(payload, pybytes)

        # find the handler being called. One we hold no record of was
        # deregistered while this output was on its way - there is nobody
        # to give it to.
        hdlr = _hdlr_find(myiofhdlrs, _pending_iofreg, pyiof_id)
        if hdlr is None:
            return
        hdlr(pyiof_id, pychannel, pysource, pybytes, pyinfo)
    except:
        traceback.print_exc()
    return

# Complete an event for a Python handler. The chain does not move on until
# this is called, so it is called exactly once on every path
cdef void pyevent_complete(pmix_event_notification_cbfunc_fn_t cbfunc,
                           void *cbdata, pmix_status_t ret_status,
                           pymyresults) noexcept with gil:
    cdef pmix_info_t *myresults = NULL
    cdef pmix_info_t **myresults_ptr
    cdef size_t nmyresults = 0
    cdef pypmix_info_cbdata_t *icbd

    if NULL == cbfunc:
        return
    # allocate and load pmix info structs from python list of dictionaries
    myresults_ptr = &myresults
    try:
        prc = pmix_alloc_info(myresults_ptr, &nmyresults, pymyresults)
    except:
        traceback.print_exc()
        prc = PMIX_ERR_BAD_PARAM
    if PMIX_SUCCESS != prc:
        print("Unable to load new results")
        myresults = NULL
        nmyresults = 0
    icbd = NULL
    if NULL != myresults:
        icbd = <pypmix_info_cbdata_t *> malloc(sizeof(pypmix_info_cbdata_t))
        if icbd == NULL:
            # complete the event without the results rather than lose it
            pmix_free_info(myresults, nmyresults)
            myresults = NULL
            nmyresults = 0
    if NULL == icbd:
        with nogil:
            cbfunc(ret_status, NULL, 0, NULL, NULL, cbdata)
        return
    icbd.info = myresults
    icbd.ninfo = nmyresults
    with nogil:
        cbfunc(ret_status, myresults, nmyresults, op_release, <void *>icbd, cbdata)

cdef void pyeventhandler(size_t evhdlr_registration_id,
                         pmix_status_t status,
                         const pmix_proc_t *source,
                         pmix_info_t info[], size_t ninfo,
                         pmix_info_t *results, size_t nresults,
                         pmix_event_notification_cbfunc_fn_t cbfunc,
                         void *cbdata) noexcept with gil:
    cdef pmix_nspace_t srcnspace

    # Everything up to the handler is conversion, and any of it can raise.
    # The event still has to be completed if it does, or the rest of its
    # chain never runs - so it is caught and reported as no action taken
    try:
        # convert the source to python
        pysource = None
        if NULL != source:
            memset(srcnspace, 0, PMIX_MAX_NSLEN+1)
            memcpy(srcnspace, source[0].nspace, PMIX_MAX_NSLEN)
            pysource = {'nspace': pmix_decode_str(srcnspace), 'rank': int(source[0].rank)}
        pyev_id  = int(evhdlr_registration_id)

        # convert the inbound info to python
        pyinfo = []
        if NULL != info and 0 < ninfo:
            rc = pmix_unload_info(info, ninfo, pyinfo)
            if PMIX_SUCCESS != rc:
                print("Unable to unload info structs")
                pyevent_complete(cbfunc, cbdata, PMIX_EVENT_NO_ACTION_TAKEN, None)
                return

        # convert the inbound results from prior handlers
        # that serviced this event to python
        pyresults = []
        if NULL != results and 0 < nresults:
            rc = pmix_unload_info(results, nresults, pyresults)
            if PMIX_SUCCESS != rc:
                print("Unable to unload prior results")
                pyevent_complete(cbfunc, cbdata, PMIX_EVENT_NO_ACTION_TAKEN, None)
                return

        # find the handler being called. One we hold no record of was
        # deregistered while this event was on its way; the event still
        # has to be completed, or the rest of its chain never runs.
        hdlr = _hdlr_find(myhdlrs, _pending_evreg, pyev_id, int(status))
    except:
        traceback.print_exc()
        pyevent_complete(cbfunc, cbdata, PMIX_EVENT_NO_ACTION_TAKEN, None)
        return
    if hdlr is None:
        pyevent_complete(cbfunc, cbdata, PMIX_EVENT_NO_ACTION_TAKEN, None)
        return

    # execute their handler. One that raises, or answers in a shape we
    # cannot use, has still been called, and the chain must still go on
    # - it just reports no action
    cdef pmix_status_t cret = PMIX_EVENT_NO_ACTION_TAKEN
    try:
        ret_status, pymyresults = hdlr(pyev_id, status, pysource, pyinfo, pyresults)
        if not isinstance(ret_status, int) or isinstance(ret_status, bool) or \
           not (-2147483648 <= ret_status <= 2147483647):
            raise TypeError("event handler status must be a PMIx status, not %r" % (ret_status,))
        if pymyresults is not None and not isinstance(pymyresults, list):
            pymyresults = list(pymyresults)
        cret = ret_status
    except:
        traceback.print_exc()
        cret = PMIX_EVENT_NO_ACTION_TAKEN
        pymyresults = None
    pyevent_complete(cbfunc, cbdata, cret, pymyresults)

# Round-trip a value dict through the C conversion layer and back.
#
# pmix_load_value and pmix_unload_value are cdef, so Python cannot reach
# them and the conversion arms - above all the PMIX_DATA_ARRAY ones, whose
# struct-member mistakes Cython cannot catch because it silently converts
# a C struct to a dict - were coverable only by a connected round trip
# through a live server. This exposes just enough of the pair for
# test/python/test_bindings.py to check every arm with no server at all:
# it loads the dict into a pmix_value_t, converts that back to a dict, and
# releases the value.
#
# Returns (rc, value dict); the dict is None if the load failed.
#
# This needs no initialized library. (A PMIX_REGEX value is released
# through the preg framework, but its release is a plain free() on a
# statically initialized module, so that holds for it too.)
def pmix_value_roundtrip(pyval):
    cdef pmix_value_t value

    if not isinstance(pyval, dict):
        return (PMIX_ERR_BAD_PARAM, None)
    if 'value' not in pyval or 'val_type' not in pyval:
        return (PMIX_ERR_BAD_PARAM, None)

    # zero it first so a loader that fails partway leaves something safe
    # to destruct
    memset(&value, 0, sizeof(pmix_value_t))
    rc = pmix_load_value(&value, pyval)
    if PMIX_SUCCESS != rc:
        pmix_destruct_value(&value)
        return (rc, None)
    pyout = pmix_unload_value(&value)
    pmix_destruct_value(&value)
    if pyout is None:
        return (PMIX_ERR_NOT_SUPPORTED, None)
    return (PMIX_SUCCESS, pyout)


# Take the results array a blocking call handed back. It is the caller's
# whatever the status - the library answers PMIX_ERR_PARTIAL_SUCCESS with
# the part it could answer, and some calls return an array alongside an
# error too - so it is converted on success or partial success and
# released in every case. Returns the status to report
cdef int pmix_take_results(int rc, pmix_info_t *results, size_t nresults, pyres):
    if NULL == results:
        return rc
    if (PMIX_SUCCESS == rc or PMIX_ERR_PARTIAL_SUCCESS == rc) and 0 < nresults:
        prc = pmix_unload_info(results, nresults, pyres)
        if PMIX_SUCCESS != prc:
            rc = prc
    PMIx_Info_free(results, nresults)
    return rc

cdef class PMIxClient:
    cdef pmix_proc_t myproc;
    cdef pmix_fabric_t myfabric;
    cdef int fabric_set;
    # a fabric request is outstanding: the library writes the fabric
    # object from the progress thread when it completes, so nothing else
    # may read or start one until then
    cdef int fabric_busy;
    cdef pmix_topology_t topo

    def __cinit__(self):
        memset(self.myproc.nspace, 0, sizeof(self.myproc.nspace))
        self.myproc.rank = PMIX_RANK_UNDEF
        memset(&self.myfabric, 0, sizeof(self.myfabric))
        self.fabric_set = 0
        self.fabric_busy = 0
        self.topo.source = NULL
        self.topo.topology = NULL

    def __init__(self):
        global myhdlrs, myiofhdlrs, myname
        memset(self.myproc.nspace, 0, sizeof(self.myproc.nspace))
        self.myproc.rank = <uint32_t>PMIX_RANK_UNDEF
        myhdlrs = []
        myiofhdlrs = []
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
        cdef pmix_status_t _gilrc
        cdef size_t klen
        global myname
        cdef pmix_info_t *info
        cdef pmix_info_t **info_ptr
        myname = {}

        # allocate and load pmix info structs from python list of dictionaries
        info_ptr = &info
        rc = pmix_alloc_info(info_ptr, &klen, dicts)
        if PMIX_SUCCESS != rc:
            return rc, myname
        with nogil:
            _gilrc = PMIx_Init(&self.myproc, info, klen)
        rc = _gilrc
        _pmix_sync_environ()
        if 0 < klen:
            pmix_free_info(info, klen)
        if PMIX_SUCCESS == rc:
            # convert the returned name
            myname = {'nspace': (<bytes>self.myproc.nspace).decode('UTF-8'), 'rank': self.myproc.rank}
        return rc, myname

    # Finalize the client library
    def finalize(self, dicts):
        cdef pmix_status_t _gilrc
        cdef size_t klen
        cdef pmix_info_t *info
        cdef pmix_info_t **info_ptr

        # allocate and load pmix info structs from python list of dictionaries
        info_ptr = &info
        rc = pmix_alloc_info(info_ptr, &klen, dicts)
        if PMIX_SUCCESS != rc:
            return rc
        with nogil:
            _gilrc = PMIx_Finalize(info, klen)
        rc = _gilrc
        if 0 < klen:
            pmix_free_info(info, klen)
        return rc

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
        cdef pmix_status_t _gilrc
        cdef pmix_status_t cstatus
        cdef const char *cmsg
        cdef pmix_proc_t *procs
        cdef size_t sz
        # convert list of procs to array of pmix_proc_t's
        if peers is not None:
            sz = len(peers)
            if 0 < sz:
                procs = <pmix_proc_t*> PyMem_Malloc(sz * sizeof(pmix_proc_t))
                if not procs:
                    return PMIX_ERR_NOMEM
                rc = pmix_load_procs(procs, sz, peers)
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
        # an immutable copy: the buffer is lent to the library while the
        # GIL is released, and another thread resizing a bytearray would
        # free it underneath the call
        if isinstance(msg, str):
            pymsg = msg.encode('UTF-8')
        elif msg is None:
            pymsg = None
        else:
            pymsg = bytes(msg)
        # pass into PMIx_Abort. None is a NULL message
        cmsg = NULL
        if pymsg is not None:
            cmsg = pymsg
        cstatus = status
        with nogil:
            _gilrc = PMIx_Abort(cstatus, cmsg, procs, sz)
        rc = _gilrc
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
        cdef pmix_status_t _gilrc
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

        # convert the dict to a pmix_value_t. Zero it first so a loader
        # that fails partway leaves something safe to destruct
        memset(&value, 0, sizeof(pmix_value_t))
        rc = pmix_load_value(&value, pyval)
        if PMIX_SUCCESS != rc:
            pmix_destruct_value(&value)
            return rc

        # call API - the library copies the value, so release our copy on
        # every path. Note that "value" lives on the stack: it must be
        # destructed, never freed
        with nogil:
            _gilrc = PMIx_Store_internal(&proc, key, &value)
        rc = _gilrc
        pmix_destruct_value(&value)
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
        cdef pmix_status_t _gilrc
        cdef pmix_scope_t cscope
        cdef pmix_key_t key
        cdef pmix_value_t value
        # convert the keyval tuple to a pmix_info_t. Zero it first so a
        # loader that fails partway leaves something safe to destruct
        pmix_copy_key(key, ky)
        memset(&value, 0, sizeof(pmix_value_t))
        rc = pmix_load_value(&value, val)
        if PMIX_SUCCESS != rc:
            pmix_destruct_value(&value)
            return rc
        # pass it into the PMIx_Put function
        cscope = scope
        with nogil:
            _gilrc = PMIx_Put(cscope, key, &value)
        rc = _gilrc
        pmix_destruct_value(&value)
        return rc

    def commit(self):
        cdef pmix_status_t _gilrc
        with nogil:
            _gilrc = PMIx_Commit()
        rc = _gilrc
        return rc

    def fence(self, peers, dicts):
        cdef pmix_status_t _rc
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
                rc = pmix_load_procs(procs, nprocs, peers)
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
        # release the GIL across the collective. These block until the
        # server completes the operation, and this process's own event
        # handlers and _nb callbacks are Python code that needs the GIL
        # to run - holding it here deadlocks the caller against itself
        with nogil:
            _rc = PMIx_Fence(procs, nprocs, info, ninfo)
        rc = _rc
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
        cdef pmix_status_t getrc
        cdef pmix_info_t *info;
        cdef pmix_info_t **info_ptr;
        cdef size_t ninfo;
        cdef pmix_key_t key;
        cdef pmix_value_t *val_ptr;
        cdef pmix_proc_t p;

        ninfo   = 0
        val_ptr = NULL

        # convert proc to pmix_proc_t - through the loader, so that a
        # malformed proc dict is a status rather than an exception
        if proc is None:
            memcpy(&p, &self.myproc, sizeof(pmix_proc_t))
        else:
            rc = pmix_load_proc(&p, proc)
            if PMIX_SUCCESS != rc:
                return rc, None

        # allocate and load pmix info structs from python list of dictionaries
        info_ptr = &info
        rc = pmix_alloc_info(info_ptr, &ninfo, dicts)
        if PMIX_SUCCESS != rc:
            return rc, None
        # see pmix_asks_pointer_values: the release below would free
        # library memory
        if pmix_asks_pointer_values(info, ninfo):
            pmix_free_info(info, ninfo)
            return PMIX_ERR_NOT_SUPPORTED, None

        val = None

        # None key - pass NULL to return all data
        if ky is None:
            with nogil:
                getrc = PMIx_Get(&p, NULL, info, ninfo, &val_ptr)
        else:
            # convert key,val to pmix_value_t and pmix_key_t. A key that
            # does not fit is refused here, where the info array can still
            # be released, rather than raised past it
            if PMIX_SUCCESS != pmix_name_ok(ky, PMIX_MAX_KEYLEN, "key"):
                if 0 < ninfo:
                    pmix_free_info(info, ninfo)
                return PMIX_ERR_BAD_PARAM, None
            pmix_copy_key(key, ky)
            with nogil:
                getrc = PMIx_Get(&p, key, info, ninfo, &val_ptr)
        rc = getrc
        if PMIX_SUCCESS == rc:
            val = pmix_unload_value(val_ptr)
            if val is None:
                rc = PMIX_ERR_NOT_SUPPORTED
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
        cdef pmix_status_t _rc
        cdef pmix_info_t *info;
        cdef pmix_info_t **info_ptr;
        cdef size_t ninfo;
        ninfo = 0

        # allocate and load pmix info structs from python list of dictionaries
        info_ptr = &info
        rc = pmix_alloc_info(info_ptr, &ninfo, dicts)
        if PMIX_SUCCESS != rc:
            return rc

        # pass it into the publish API
        # release the GIL across the collective. These block until the
        # server completes the operation, and this process's own event
        # handlers and _nb callbacks are Python code that needs the GIL
        # to run - holding it here deadlocks the caller against itself
        with nogil:
            _rc = PMIx_Publish(info, ninfo)
        rc = _rc
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
        cdef pmix_status_t _rc
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
                rc = pmix_load_argv(keys, nstrings, pykeys)
                if PMIX_SUCCESS != rc:
                    pmix_free_argv(keys)
                    return rc
        else:
            keys = NULL

        # allocate and load pmix info structs from python list of dictionaries
        info = NULL
        if dicts is not None:
            info_ptr = &info
            rc = pmix_alloc_info(info_ptr, &ninfo, dicts)
            if PMIX_SUCCESS != rc:
                if NULL != keys:
                    pmix_free_argv(keys)
                return rc

        # pass it into the unpublish API
        # release the GIL across the collective. These block until the
        # server completes the operation, and this process's own event
        # handlers and _nb callbacks are Python code that needs the GIL
        # to run - holding it here deadlocks the caller against itself
        with nogil:
            _rc = PMIx_Unpublish(keys, info, ninfo)
        rc = _rc
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
        cdef pmix_status_t _rc
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
                return rc, []
        else:
            info = NULL

        # convert the list of dictionaries to array of
        # pmix_pdata_t structs
        if data is not None:
            # a private copy, its keys checked before anything is built
            # from it - see pmix_snapshot
            data = pmix_listcopy(data)
            ok = data is not None
            if ok:
                for d in data:
                    if not isinstance(d, dict) or \
                       PMIX_SUCCESS != pmix_name_ok(d.get('key'), PMIX_MAX_KEYLEN, "key"):
                        ok = False
                        break
            if not ok:
                if 0 < ninfo:
                    pmix_free_info(info, ninfo)
                return PMIX_ERR_BAD_PARAM, []
            npdata = len(data)
            if 0 < npdata:
                pdata = <pmix_pdata_t*> PyMem_Malloc(npdata * sizeof(pmix_pdata_t))
                if not pdata:
                    if 0 < ninfo:
                        pmix_free_info(info, ninfo)
                    return PMIX_ERR_NOMEM, []
                memset(pdata, 0, npdata * sizeof(pmix_pdata_t))
                n = 0
                for d in data:
                    pmix_copy_key(pdata[n].key, d['key'])
                    n += 1
            else:
                pdata = NULL
        else:
            pdata = NULL

        # pass it into the lookup API. Unload into a fresh list rather than
        # appending to the caller's: the results are one per requested key,
        # so mixing them in with the requests leaves the two indistinguishable
        # as soon as more than one key is looked up
        results = []
        # release the GIL across the collective. These block until the
        # server completes the operation, and this process's own event
        # handlers and _nb callbacks are Python code that needs the GIL
        # to run - holding it here deadlocks the caller against itself
        with nogil:
            _rc = PMIx_Lookup(pdata, npdata, info, ninfo)
        rc = _rc
        # a lookup that found only some of the keys answers
        # PMIX_ERR_PARTIAL_SUCCESS, with the keys it did not find left
        # PMIX_UNDEF - report what was found either way
        if PMIX_SUCCESS == rc or PMIX_ERR_PARTIAL_SUCCESS == rc:
            prc = pmix_unload_pdata(pdata, npdata, results)
            if PMIX_SUCCESS != prc:
                rc = prc
        if 0 < ninfo:
            pmix_free_info(info, ninfo)
        if 0 < npdata:
            pmix_free_pdata(pdata, npdata)
        return rc, results

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
        jinfo = NULL
        ninfo = 0
        if jobInfo is not None:
            jinfo_ptr = &jinfo
            rc = pmix_alloc_info(jinfo_ptr, &ninfo, jobInfo)
            if PMIX_SUCCESS != rc:
                return rc, None

        # convert the list of apps to an array of pmix_app_t
        napps = len(pyapps)
        apps = <pmix_app_t*> PyMem_Malloc(napps * sizeof(pmix_app_t))
        if not apps:
            pmix_free_info(jinfo, ninfo)
            return PMIX_ERR_NOMEM, None

        rc = pmix_load_apps(apps, napps, pyapps)
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
            pyns = pmix_decode_str(nspace)
        return rc, pyns

    def connect(self, peers, pyinfo):
        cdef pmix_status_t _rc
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
                rc = pmix_load_procs(procs, nprocs, peers)
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
        if PMIX_SUCCESS != rc:
            pmix_free_procs(procs, nprocs)
            return rc

        # Call the library
        # release the GIL across the collective. These block until the
        # server completes the operation, and this process's own event
        # handlers and _nb callbacks are Python code that needs the GIL
        # to run - holding it here deadlocks the caller against itself
        with nogil:
            _rc = PMIx_Connect(procs, nprocs, info, ninfo)
        rc = _rc
        if 0 < nprocs:
            pmix_free_procs(procs, nprocs)
        if 0 < ninfo:
            pmix_free_info(info, ninfo)
        return rc

    def disconnect(self, peers, pyinfo):
        cdef pmix_status_t _rc
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
                rc = pmix_load_procs(procs, nprocs, peers)
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
        if PMIX_SUCCESS != rc:
            pmix_free_procs(procs, nprocs)
            return rc

        # Call the library
        # release the GIL across the collective. These block until the
        # server completes the operation, and this process's own event
        # handlers and _nb callbacks are Python code that needs the GIL
        # to run - holding it here deadlocks the caller against itself
        with nogil:
            _rc = PMIx_Disconnect(procs, nprocs, info, ninfo)
        rc = _rc
        if 0 < nprocs:
            pmix_free_procs(procs, nprocs)
        if 0 < ninfo:
            pmix_free_info(info, ninfo)
        return rc

    # NOTE: pynode and pyns are deliberately left unannotated. A ":str"
    # annotation is a Cython type declaration that rejects None at the call
    # boundary, and None is meaningful here: a NULL nodename means "any
    # node" and a NULL nspace means "every namespace this server knows".
    # The body's own checks depend on being reachable with None.
    def resolve_peers(self, pynode, pyns):
        cdef pmix_status_t _gilrc
        cdef pmix_nspace_t nspace
        cdef char *nodename
        cdef pmix_proc_t *procs
        cdef size_t nprocs
        peers = []

        nodename = NULL
        memset(nspace, 0, sizeof(nspace))
        procs = NULL
        nprocs = 0
        # the nspace first: a name that does not fit raises, and must not
        # leave the node name behind
        if pyns is not None:
            pmix_copy_nspace(nspace, pyns)
        if pynode is not None:
            if isinstance(pynode, str):
                pyn = pynode.encode('UTF-8')
            else:
                pyn = pynode
            nodename = strdup(pyn)
            if NULL == nodename:
                return PMIX_ERR_NOMEM, peers
        with nogil:
            _gilrc = PMIx_Resolve_peers(nodename, nspace, &procs, &nprocs)
        rc = _gilrc
        # the node name was ours, not the library's, and it did not retain it
        if NULL != nodename:
            free(nodename)
        if PMIX_SUCCESS == rc and 0 < nprocs:
            rc = pmix_unload_procs(procs, nprocs, peers)
            # the proc array came from the library's allocator, so it goes
            # back to the library
            PMIx_Proc_free(procs, nprocs)
        return rc, peers

    # NOTE: pyns is deliberately left unannotated - see resolve_peers above.
    # None means "every namespace this server knows about"
    def resolve_nodes(self, pyns):
        cdef pmix_status_t _gilrc
        cdef pmix_nspace_t nspace
        cdef char *nodelist

        nodelist = NULL
        # the failure paths must still have something to return
        pynodes = None
        memset(nspace, 0, sizeof(nspace))
        if pyns is not None:
            pmix_copy_nspace(nspace, pyns)
        with nogil:
            _gilrc = PMIx_Resolve_nodes(nspace, &nodelist)
        rc = _gilrc
        if PMIX_SUCCESS == rc and NULL != nodelist:
            pynodes = pmix_decode_str(nodelist)
            # the library allocated this string with the C allocator, so
            # it goes back to free() - not to Python's allocator
            free(nodelist)
        return rc, pynodes

    def query(self, pyq):
        cdef pmix_query_t *queries
        cdef size_t nqueries
        cdef pmix_info_t *results
        cdef pmix_info_t **results_ptr
        cdef size_t nresults
        cdef pmix_info_t **qual_ptr
        cdef pmix_status_t _rc;
        results = NULL
        nresults = 0
        nqueries   = 0
        nresults   = 0
        queries    = NULL
        qual_ptr   = NULL

        pyresults = []
        # a private copy, of query dicts whose key lists are lists - checked
        # before anything is allocated from it (see pmix_snapshot)
        if pyq is not None:
            pyq = pmix_listcopy(pyq)
            if pyq is None:
                return PMIX_ERR_BAD_PARAM, pyresults
            mine = []
            for q in pyq:
                if not isinstance(q, dict):
                    return PMIX_ERR_BAD_PARAM, pyresults
                qkeys = q.get('keys')
                if qkeys is not None:
                    qkeys = pmix_listcopy(qkeys)
                    if qkeys is None:
                        return PMIX_ERR_BAD_PARAM, pyresults
                # the caller's dicts are left as they were
                mine.append({'keys': qkeys, 'qualifiers': q.get('qualifiers')})
            pyq = mine
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
                    qkeys = q.get('keys')
                    nstrings = 0 if qkeys is None else len(qkeys)
                    if 0 < nstrings:
                        # the entries are strdup'd, so the array must come
                        # from the C allocator as well
                        queries[n].keys = <char **> malloc((nstrings+1) * sizeof(char*))
                        if not queries[n].keys:
                            pmix_free_queries(queries, nqueries)
                            return PMIX_ERR_NOMEM,pyresults
                        memset(queries[n].keys, 0, (nstrings+1) * sizeof(char*))
                        rc = pmix_load_argv(queries[n].keys, nstrings, qkeys)
                        if PMIX_SUCCESS != rc:
                            pmix_free_queries(queries, nqueries)
                            return rc,pyresults
                    # allocate and load pmix info structs from python list of dictionaries
                    queries[n].nqual = 0
                    qual_ptr         = &(queries[n].qualifiers)
                    rc               = pmix_alloc_info(qual_ptr, &(queries[n].nqual), q.get('qualifiers'))
                    if PMIX_SUCCESS != rc:
                        pmix_free_queries(queries, nqueries)
                        return rc, pyresults
                    n += 1
            else:
                nqueries = 0
        else:
            nqueries = 0

        # pass it into the query_info API
        with nogil:
            _rc = PMIx_Query_info(queries, nqueries, &results, &nresults)
        rc = _rc
        rc = pmix_take_results(rc, results, nresults, pyresults)
        # free memory for query structs
        pmix_free_queries(queries, nqueries)
        return rc, pyresults

    def log(self, pydata, pydirs):
        cdef pmix_status_t _gilrc
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
        if PMIX_SUCCESS != rc:
            return rc
        rc = pmix_alloc_info(directives_ptr, &ndirs, pydirs)
        if PMIX_SUCCESS != rc:
            if 0 < ndata:
                pmix_free_info(data, ndata)
            return rc

        # call the API
        with nogil:
            _gilrc = PMIx_Log(data, ndata, directives, ndirs)
        rc = _gilrc
        if 0 < ndata:
            pmix_free_info(data, ndata)
        if 0 < ndirs:
            pmix_free_info(directives, ndirs)
        return rc

    def allocation_request(self, directive, pyinfo):
        cdef pmix_status_t _rc
        cdef pmix_alloc_directive_t _directive
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
        if PMIX_SUCCESS != rc:
            return rc, pyres

        # call the API
        _directive = directive
        # release the GIL across the collective. These block until the
        # server completes the operation, and this process's own event
        # handlers and _nb callbacks are Python code that needs the GIL
        # to run - holding it here deadlocks the caller against itself
        with nogil:
            _rc = PMIx_Allocation_request(_directive, info, ninfo, &results, &nresults)
        rc = _rc
        if 0 < ninfo:
            pmix_free_info(info, ninfo)
        rc = pmix_take_results(rc, results, nresults, pyres)
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
        # an immutable copy - see abort()
        if isinstance(block, str):
            pyblock = block.encode('UTF-8')
        else:
            pyblock = bytes(block)
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
        cdef pmix_status_t _rc
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
                rc = pmix_load_procs(targets, ntargets, pytargets)
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
            return rc, pyres

        # call the API
        # release the GIL across the collective. These block until the
        # server completes the operation, and this process's own event
        # handlers and _nb callbacks are Python code that needs the GIL
        # to run - holding it here deadlocks the caller against itself
        with nogil:
            _rc = PMIx_Job_control(targets, ntargets, directives, ndirs, &results, &nresults)
        rc = _rc
        if 0 < ndirs:
            pmix_free_info(directives, ndirs)
        if 0 < ntargets:
            pmix_free_procs(targets, ntargets)
        rc = pmix_take_results(rc, results, nresults, pyres)
        return rc, pyres

    def monitor(self, pymonitor_info, code:int, pydirs):
        cdef pmix_status_t _rc
        cdef pmix_status_t _code
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
            return rc, pyres

        # allocate and load pmix info structs from python list of dictionaries
        directives_ptr = &directives
        rc = pmix_alloc_info(directives_ptr, &ndirs, pydirs)
        if PMIX_SUCCESS != rc:
            if 0 < nmonitor:
                pmix_free_info(monitor_info, nmonitor)
            return rc, pyres

        # call the API
        _code = code
        # release the GIL across the collective. These block until the
        # server completes the operation, and this process's own event
        # handlers and _nb callbacks are Python code that needs the GIL
        # to run - holding it here deadlocks the caller against itself
        with nogil:
            _rc = PMIx_Process_monitor(monitor_info, _code, directives, ndirs, &results, &nresults)
        rc = _rc
        if 0 < ndirs:
            pmix_free_info(directives, ndirs)
        if 0 < nmonitor:
            pmix_free_info(monitor_info, nmonitor)
        rc = pmix_take_results(rc, results, nresults, pyres)
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
        with nogil:
            PMIx_Heartbeat()
        return PMIX_SUCCESS

    def get_credential(self, pyinfo):
        cdef pmix_status_t _gilrc
        cdef pmix_info_t *info
        cdef pmix_info_t **info_ptr
        cdef pmix_byte_object_t bo
        cdef pmix_byte_object_t *boptr
        cdef size_t ninfo

        # allocate and load pmix info structs from python list of dictionaries
        info_ptr = &info
        rc = pmix_alloc_info(info_ptr, &ninfo, pyinfo)
        if PMIX_SUCCESS != rc:
            return rc, {}

        # call the API. The library leaves the credential untouched on an
        # early error, so it must start out empty
        bo.bytes = NULL
        bo.size = 0
        boptr = &bo
        with nogil:
            _gilrc = PMIx_Get_credential(info, ninfo, boptr)
        rc = _gilrc
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
        # the credential's payload is handed to its receiver to keep - and
        # once copied into Python, this is done with it
        if NULL != bo.bytes:
            free(bo.bytes)
        return rc, cred

    # NOTE: pycred is deliberately left unannotated so the body can report
    # PMIX_ERR_BAD_PARAM on None rather than having Cython reject it
    def validate_credential(self, pycred, pyinfo):
        cdef pmix_status_t _gilrc
        cdef pmix_info_t *info
        cdef pmix_info_t **info_ptr
        cdef pmix_byte_object_t bo
        cdef size_t ninfo
        cdef pmix_info_t *results
        cdef size_t nresults

        results = NULL
        nresults = 0
        pyres = []

        # convert pycred to pmix_byte_object_t. The credential is binary,
        # and its length is len(), not sizeof() - the latter measures the
        # Python object reference and so always said 8, truncating every
        # credential to its first eight bytes. The struct itself lives on
        # the stack; there is no reason to heap-allocate one, and the heap
        # copy the old code made was never released
        rc = pmix_load_bo(&bo, pycred)
        if PMIX_SUCCESS != rc:
            return rc, pyres

        # allocate and load pmix info structs from python list of dictionaries
        info_ptr = &info
        rc = pmix_alloc_info(info_ptr, &ninfo, pyinfo)
        if PMIX_SUCCESS != rc:
            if NULL != bo.bytes:
                free(bo.bytes)
            return rc, pyres

        # call the API
        with nogil:
            _gilrc = PMIx_Validate_credential(&bo, info, ninfo, &results, &nresults)
        rc = _gilrc
        if NULL != bo.bytes:
            free(bo.bytes)
        if 0 < ninfo:
            pmix_free_info(info, ninfo)
        rc = pmix_take_results(rc, results, nresults, pyres)
        return rc, pyres

    # Note: 'peers' and 'pyinfo' are deliberately left unannotated. Cython
    # treats a parameter annotation as a type declaration that rejects
    # None, which would make the "no peers given" default below - and the
    # documented ability to pass None for the attributes - unreachable.
    def group_construct(self, group:str, peers, pyinfo):
        cdef pmix_status_t _rc
        cdef char *_grp
        cdef pmix_proc_t *procs
        cdef pmix_info_t *info
        cdef pmix_info_t **info_ptr
        cdef pmix_info_t *results
        cdef size_t ninfo
        cdef size_t nprocs
        cdef size_t nresults
        results = NULL
        nresults = 0
        nprocs = 0
        ninfo = 0

        # convert group name
        pygrp = group.encode('ascii')
        # convert list of procs to array of pmix_proc_t's. Every exit from
        # here reports (rc, results) - a bare status on these two paths
        # broke the caller's tuple unpacking. An empty list means "my whole
        # job", as it does everywhere else, rather than a zero-length array
        if peers is not None and 0 < len(peers):
            nprocs = len(peers)
            procs = <pmix_proc_t*> PyMem_Malloc(nprocs * sizeof(pmix_proc_t))
            if not procs:
                return PMIX_ERR_NOMEM, []
            rc = pmix_load_procs(procs, nprocs, peers)
            if PMIX_SUCCESS != rc:
                pmix_free_procs(procs, nprocs)
                return rc, []
        else:
            nprocs = 1
            procs = <pmix_proc_t*> PyMem_Malloc(nprocs * sizeof(pmix_proc_t))
            if not procs:
                return PMIX_ERR_NOMEM, []
            pmix_copy_nspace(procs[0].nspace, self.myproc.nspace)
            procs[0].rank = PMIX_RANK_WILDCARD

        # allocate and load pmix info structs from python list of dictionaries
        info_ptr = &info
        rc = pmix_alloc_info(info_ptr, &ninfo, pyinfo)
        if PMIX_SUCCESS != rc:
            pmix_free_procs(procs, nprocs)
            return rc, []

        # Call the library
        # the library only sets these on success, so start them
        # empty - otherwise a failed call leaves stack garbage
        # that the conversion below would walk
        results = NULL
        nresults = 0
        _grp = <char *>pygrp
        # release the GIL across the collective. These block until the
        # server completes the operation, and this process's own event
        # handlers and _nb callbacks are Python code that needs the GIL
        # to run - holding it here deadlocks the caller against itself
        with nogil:
            _rc = PMIx_Group_construct(_grp, procs, nprocs, info, ninfo, &results, &nresults)
        rc = _rc
        if 0 < nprocs:
            pmix_free_procs(procs, nprocs)
        if 0 < ninfo:
            pmix_free_info(info, ninfo)
        pyres = []
        rc = pmix_take_results(rc, results, nresults, pyres)
        return rc, pyres

    def group_invite(self, group:str, peers, pyinfo):
        cdef pmix_status_t _rc
        cdef char *_grp
        cdef pmix_proc_t *procs
        cdef pmix_info_t *info
        cdef pmix_info_t **info_ptr
        cdef pmix_info_t *results
        cdef size_t ninfo
        cdef size_t nprocs
        cdef size_t nresults
        results = NULL
        nresults = 0
        nprocs = 0
        ninfo = 0

        # convert group name
        pygrp = group.encode('ascii')
        # convert list of procs to array of pmix_proc_t's. Every exit from
        # here reports (rc, results) - a bare status on these two paths
        # broke the caller's tuple unpacking. An empty list means "my whole
        # job", as it does everywhere else, rather than a zero-length array
        if peers is not None and 0 < len(peers):
            nprocs = len(peers)
            procs = <pmix_proc_t*> PyMem_Malloc(nprocs * sizeof(pmix_proc_t))
            if not procs:
                return PMIX_ERR_NOMEM, []
            rc = pmix_load_procs(procs, nprocs, peers)
            if PMIX_SUCCESS != rc:
                pmix_free_procs(procs, nprocs)
                return rc, []
        else:
            nprocs = 1
            procs = <pmix_proc_t*> PyMem_Malloc(nprocs * sizeof(pmix_proc_t))
            if not procs:
                return PMIX_ERR_NOMEM, []
            pmix_copy_nspace(procs[0].nspace, self.myproc.nspace)
            procs[0].rank = PMIX_RANK_WILDCARD

        # allocate and load pmix info structs from python list of dictionaries
        info_ptr = &info
        rc = pmix_alloc_info(info_ptr, &ninfo, pyinfo)
        if PMIX_SUCCESS != rc:
            pmix_free_procs(procs, nprocs)
            return rc, []

        # Call the library
        # the library only sets these on success, so start them
        # empty - otherwise a failed call leaves stack garbage
        # that the conversion below would walk
        results = NULL
        nresults = 0
        _grp = <char *>pygrp
        # release the GIL across the collective. These block until the
        # server completes the operation, and this process's own event
        # handlers and _nb callbacks are Python code that needs the GIL
        # to run - holding it here deadlocks the caller against itself
        with nogil:
            _rc = PMIx_Group_invite(_grp, procs, nprocs, info, ninfo, &results, &nresults)
        rc = _rc
        if 0 < nprocs:
            pmix_free_procs(procs, nprocs)
        if 0 < ninfo:
            pmix_free_info(info, ninfo)
        pyres = []
        rc = pmix_take_results(rc, results, nresults, pyres)
        return rc, pyres

    def group_join(self, group:str, leader, opt:int, pyinfo):
        cdef pmix_status_t _rc
        cdef char *_grp
        cdef pmix_group_opt_t _opt
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
        if PMIX_SUCCESS != rc:
            return rc, []

        # Call the library
        # the library only sets these on success, so start them
        # empty - otherwise a failed call leaves stack garbage
        # that the conversion below would walk
        results = NULL
        nresults = 0
        _grp = <char *>pygrp
        _opt = opt
        # release the GIL across the collective. These block until the
        # server completes the operation, and this process's own event
        # handlers and _nb callbacks are Python code that needs the GIL
        # to run - holding it here deadlocks the caller against itself
        with nogil:
            _rc = PMIx_Group_join(_grp, &proc, _opt, info, ninfo, &results, &nresults)
        rc = _rc
        if 0 < ninfo:
            pmix_free_info(info, ninfo)
        pyres = []
        rc = pmix_take_results(rc, results, nresults, pyres)
        return rc, pyres

    def group_leave(self, group:str, pyinfo):
        cdef pmix_status_t _rc
        cdef char *_grp
        cdef pmix_info_t *info
        cdef pmix_info_t **info_ptr
        cdef size_t ninfo
        ninfo = 0

        # convert group name
        pygrp = group.encode('ascii')

        # allocate and load pmix info structs from python list of dictionaries
        info_ptr = &info
        rc = pmix_alloc_info(info_ptr, &ninfo, pyinfo)
        if PMIX_SUCCESS != rc:
            return rc

        # Call the library
        _grp = <char *>pygrp
        # release the GIL across the collective. These block until the
        # server completes the operation, and this process's own event
        # handlers and _nb callbacks are Python code that needs the GIL
        # to run - holding it here deadlocks the caller against itself
        with nogil:
            _rc = PMIx_Group_leave(_grp, info, ninfo)
        rc = _rc
        if 0 < ninfo:
            pmix_free_info(info, ninfo)
        return rc

    def group_destruct(self, group:str, pyinfo):
        cdef pmix_status_t _rc
        cdef char *_grp
        cdef pmix_info_t *info
        cdef pmix_info_t **info_ptr
        cdef size_t ninfo
        ninfo = 0

        # convert group name
        pygrp = group.encode('ascii')

        # allocate and load pmix info structs from python list of dictionaries
        info_ptr = &info
        rc = pmix_alloc_info(info_ptr, &ninfo, pyinfo)
        if PMIX_SUCCESS != rc:
            return rc

        # Call the library
        _grp = <char *>pygrp
        # release the GIL across the collective. These block until the
        # server completes the operation, and this process's own event
        # handlers and _nb callbacks are Python code that needs the GIL
        # to run - holding it here deadlocks the caller against itself
        with nogil:
            _rc = PMIx_Group_destruct(_grp, info, ninfo)
        rc = _rc
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
        nbidx = cd.idx
        with nogil:
            rc = PMIx_Group_construct_nb(cd.grp, cd.procs, cd.nprocs,
                                         cd.info, cd.ninfo,
                                         pypmix_client_info_cbfunc, <void *> cd)
        if PMIX_SUCCESS != rc:
            # no callback will be executed, so reclaim the operation here
            if pypmix_nb_take(nbidx) is not None:
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
        nbidx = cd.idx
        with nogil:
            rc = PMIx_Group_invite_nb(cd.grp, cd.procs, cd.nprocs,
                                      cd.info, cd.ninfo,
                                      pypmix_client_info_cbfunc, <void *> cd)
        if PMIX_SUCCESS != rc:
            # no callback will be executed, so reclaim the operation here
            if pypmix_nb_take(nbidx) is not None:
                pypmix_nb_cbdata_free(cd)
        return rc

    def group_join_nb(self, group:str, leader, opt:int, pyinfo,
                      cbfunc, cbdata=None):
        cdef pypmix_nb_cbdata_t *cd
        cdef pmix_status_t rc
        cdef int prc
        cdef pmix_group_opt_t copt
        cdef pmix_proc_t ldr

        if not callable(cbfunc):
            return PMIX_ERR_BAD_PARAM
        copt = opt

        # convert the leader before the caddy exists, so that a leader
        # that cannot be converted has nothing to leak
        if leader is not None:
            prc = pmix_load_proc(&ldr, leader)
            if PMIX_SUCCESS != prc:
                return prc
        else:
            memcpy(&ldr, &self.myproc, sizeof(pmix_proc_t))

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
        memcpy(&cd.procs[0], &ldr, sizeof(pmix_proc_t))

        # record the callback so it stays alive, then call the library
        cd.idx = pypmix_nb_register(cbfunc, cbdata)
        nbidx = cd.idx
        with nogil:
            rc = PMIx_Group_join_nb(cd.grp, &cd.procs[0], copt,
                                    cd.info, cd.ninfo,
                                    pypmix_client_info_cbfunc, <void *> cd)
        if PMIX_SUCCESS != rc:
            # no callback will be executed, so reclaim the operation here
            if pypmix_nb_take(nbidx) is not None:
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
        nbidx = cd.idx
        with nogil:
            rc = PMIx_Group_leave_nb(cd.grp, cd.info, cd.ninfo,
                                     pypmix_client_op_cbfunc, <void *> cd)
        if PMIX_SUCCESS != rc:
            # no callback will be executed, so reclaim the operation here
            if pypmix_nb_take(nbidx) is not None:
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
        nbidx = cd.idx
        with nogil:
            rc = PMIx_Group_destruct_nb(cd.grp, cd.info, cd.ninfo,
                                        pypmix_client_op_cbfunc, <void *> cd)
        if PMIX_SUCCESS != rc:
            # no callback will be executed, so reclaim the operation here
            if pypmix_nb_take(nbidx) is not None:
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
        nbidx = cd.idx
        with nogil:
            rc = PMIx_Fence_nb(cd.procs, cd.nprocs, cd.info, cd.ninfo,
                               pypmix_client_op_cbfunc, <void *> cd)
        if PMIX_SUCCESS != rc:
            # no callback will be executed, so reclaim the operation here
            if pypmix_nb_take(nbidx) is not None:
                pypmix_nb_cbdata_free(cd)
        return rc

    # cbfunc(status:int, value:dict, cbdata) - 'value' is None if the key
    # was not found. A None key requests all data for the given proc
    def get_nb(self, proc, ky, dicts, cbfunc, cbdata=None):
        cdef pypmix_nb_cbdata_t *cd
        cdef pmix_status_t rc
        cdef int prc

        cdef pmix_proc_t target

        if not callable(cbfunc):
            return PMIX_ERR_BAD_PARAM

        # convert the proc and the key before the caddy exists, so that
        # one that cannot be converted has nothing to leak
        if proc is None:
            memcpy(&target, &self.myproc, sizeof(pmix_proc_t))
        else:
            prc = pmix_load_proc(&target, proc)
            if PMIX_SUCCESS != prc:
                return prc
        pyky = None
        if ky is not None:
            pyky = pmix_name_bytes(ky, PMIX_MAX_KEYLEN)
            if pyky is None:
                return PMIX_ERR_BAD_PARAM

        cd = pypmix_nb_setup(dicts, &prc)
        if NULL == cd:
            return prc
        # see pmix_asks_pointer_values: the callback releases the value
        # it is handed, which such an answer is not
        if pmix_asks_pointer_values(cd.info, cd.ninfo):
            pypmix_nb_cbdata_free(cd)
            return PMIX_ERR_NOT_SUPPORTED

        # the proc and the key must outlive this call as well - a
        # single-element proc array covers the former
        cd.nprocs = 1
        cd.procs = <pmix_proc_t*> PyMem_Malloc(sizeof(pmix_proc_t))
        if not cd.procs:
            cd.nprocs = 0
            pypmix_nb_cbdata_free(cd)
            return PMIX_ERR_NOMEM
        memcpy(&cd.procs[0], &target, sizeof(pmix_proc_t))
        if pyky is not None:
            cd.ky = strdup(pyky)
            if NULL == cd.ky:
                pypmix_nb_cbdata_free(cd)
                return PMIX_ERR_NOMEM

        cd.idx = pypmix_nb_register(cbfunc, cbdata)
        nbidx = cd.idx
        with nogil:
            rc = PMIx_Get_nb(&cd.procs[0], cd.ky, cd.info, cd.ninfo,
                             pypmix_client_value_cbfunc, <void *> cd)
        if PMIX_SUCCESS != rc:
            if pypmix_nb_take(nbidx) is not None:
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
        nbidx = cd.idx
        with nogil:
            rc = PMIx_Publish_nb(cd.info, cd.ninfo,
                                 pypmix_client_op_cbfunc, <void *> cd)
        if PMIX_SUCCESS != rc:
            if pypmix_nb_take(nbidx) is not None:
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
        nbidx = cd.idx
        with nogil:
            rc = PMIx_Lookup_nb(cd.keys, cd.info, cd.ninfo,
                                pypmix_client_lookup_cbfunc, <void *> cd)
        if PMIX_SUCCESS != rc:
            if pypmix_nb_take(nbidx) is not None:
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
        nbidx = cd.idx
        with nogil:
            rc = PMIx_Unpublish_nb(cd.keys, cd.info, cd.ninfo,
                                   pypmix_client_op_cbfunc, <void *> cd)
        if PMIX_SUCCESS != rc:
            if pypmix_nb_take(nbidx) is not None:
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
        prc = pmix_load_apps(cd.apps, cd.napps, pyapps)
        if PMIX_SUCCESS != prc:
            pypmix_nb_cbdata_free(cd)
            return prc

        cd.idx = pypmix_nb_register(cbfunc, cbdata)
        nbidx = cd.idx
        with nogil:
            rc = PMIx_Spawn_nb(cd.info, cd.ninfo, cd.apps, cd.napps,
                               pypmix_client_spawn_cbfunc, <void *> cd)
        if PMIX_SUCCESS != rc:
            if pypmix_nb_take(nbidx) is not None:
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
        nbidx = cd.idx
        with nogil:
            rc = PMIx_Connect_nb(cd.procs, cd.nprocs, cd.info, cd.ninfo,
                                 pypmix_client_op_cbfunc, <void *> cd)
        if PMIX_SUCCESS != rc:
            if pypmix_nb_take(nbidx) is not None:
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
        nbidx = cd.idx
        with nogil:
            rc = PMIx_Disconnect_nb(cd.procs, cd.nprocs, cd.info, cd.ninfo,
                                    pypmix_client_op_cbfunc, <void *> cd)
        if PMIX_SUCCESS != rc:
            if pypmix_nb_take(nbidx) is not None:
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

        # walk a private copy (see pmix_snapshot), of query dicts
        if pyq is not None:
            pyq = pmix_listcopy(pyq)
            if pyq is None:
                return PMIX_ERR_BAD_PARAM
            for q in pyq:
                if not isinstance(q, dict):
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
                if qkeys is not None:
                    qkeys = pmix_listcopy(qkeys)
                    if qkeys is None:
                        pypmix_nb_cbdata_free(cd)
                        return PMIX_ERR_BAD_PARAM
                if qkeys is not None and 0 < len(qkeys):
                    nstrings = len(qkeys)
                    cd.queries[n].keys = <char **> malloc((nstrings+1) * sizeof(char*))
                    if not cd.queries[n].keys:
                        pypmix_nb_cbdata_free(cd)
                        return PMIX_ERR_NOMEM
                    prc = pmix_load_argv(cd.queries[n].keys, nstrings, qkeys)
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
        nbidx = cd.idx
        with nogil:
            rc = PMIx_Query_info_nb(cd.queries, cd.nqueries,
                                    pypmix_client_info_cbfunc, <void *> cd)
        if PMIX_SUCCESS != rc:
            if pypmix_nb_take(nbidx) is not None:
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
        nbidx = cd.idx
        with nogil:
            rc = PMIx_Log_nb(cd.info, cd.ninfo, cd.dirs, cd.ndirs,
                             pypmix_client_op_cbfunc, <void *> cd)
        if PMIX_SUCCESS != rc:
            if pypmix_nb_take(nbidx) is not None:
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
        nbidx = cd.idx
        with nogil:
            rc = PMIx_Allocation_request_nb(drctv, cd.info, cd.ninfo,
                                            pypmix_client_info_cbfunc,
                                            <void *> cd)
        if PMIX_SUCCESS != rc:
            if pypmix_nb_take(nbidx) is not None:
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
        nbidx = cd.idx
        with nogil:
            rc = PMIx_Job_control_nb(cd.procs, cd.nprocs, cd.info, cd.ninfo,
                                     pypmix_client_info_cbfunc, <void *> cd)
        if PMIX_SUCCESS != rc:
            if pypmix_nb_take(nbidx) is not None:
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
        nbidx = cd.idx
        with nogil:
            rc = PMIx_Process_monitor_nb(cd.info, ccode, cd.dirs, cd.ndirs,
                                         pypmix_client_info_cbfunc, <void *> cd)
        if PMIX_SUCCESS != rc:
            if pypmix_nb_take(nbidx) is not None:
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
        nbidx = cd.idx
        with nogil:
            rc = PMIx_Get_credential_nb(cd.info, cd.ninfo,
                                        pypmix_client_credential_cbfunc,
                                        <void *> cd)
        if PMIX_SUCCESS != rc:
            if pypmix_nb_take(nbidx) is not None:
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
        if not isinstance(pycred, dict):
            return PMIX_ERR_BAD_PARAM
        # the payload, converted before the caddy exists
        pybytes = pycred.get('bytes')
        if pybytes is None:
            return PMIX_ERR_BAD_PARAM
        pybytes = pmix_payload(pybytes)
        if pybytes is None:
            return PMIX_ERR_TYPE_MISMATCH

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
        nbidx = cd.idx
        with nogil:
            rc = PMIx_Validate_credential_nb(cd.cred, cd.info, cd.ninfo,
                                             pypmix_client_validation_cbfunc,
                                             <void *> cd)
        if PMIX_SUCCESS != rc:
            if pypmix_nb_take(nbidx) is not None:
                pypmix_nb_cbdata_free(cd)
        return rc

    # The fabric information the library holds for the registered fabric,
    # as fabric_register() and fabric_update() return it. This is how to
    # read it after a non-blocking registration the library completed on
    # the spot (PMIX_OPERATION_SUCCEEDED, no callback).
    #
    # Returns (rc, fabricinfo)
    def fabric_info(self):
        # busy first: during a first registration nothing is set yet,
        # but the answer that matters is that one is in flight
        if 1 == self.fabric_busy:
            return (PMIX_ERR_RESOURCE_BUSY, [])
        if 0 == self.fabric_set:
            return (PMIX_ERR_INIT, [])
        finfo = self._fabric_info()
        if finfo is None:
            return (PMIX_ERR_NOT_SUPPORTED, [])
        return (PMIX_SUCCESS, finfo)

    # The fabric information in this class's fabric object, as a list of
    # info dicts - None if it cannot be converted
    def _fabric_info(self):
        finfo = []
        if NULL != self.myfabric.info and 0 < self.myfabric.ninfo:
            rc = pmix_unload_info(self.myfabric.info, self.myfabric.ninfo, finfo)
            if PMIX_SUCCESS != rc:
                return None
        return finfo

    # The fabric object lives in this class, so the registry holds a
    # reference to us until the callback fires, and the "registered" flag
    # is only changed once the library says the operation succeeded.
    #
    # Each of these may also be completed by the library on the spot - a
    # scheduler registers a fabric itself, and a deregistration never
    # leaves the process - in which case it answers
    # PMIX_OPERATION_SUCCEEDED and the callback is not called, exactly as
    # for the C API. The flag is updated before that is returned.

    # cbfunc(status:int, fabricinfo:list, cbdata) - fabricinfo is what
    # fabric_register() returns, and is empty unless status is success
    def fabric_register_nb(self, dicts, cbfunc, cbdata=None):
        cdef pypmix_nb_cbdata_t *cd
        cdef pmix_status_t rc
        cdef int prc
        cdef pmix_fabric_t *fab

        if not callable(cbfunc):
            return PMIX_ERR_BAD_PARAM
        if 1 == self.fabric_set or 1 == self.fabric_busy:
            return PMIX_ERR_RESOURCE_BUSY
        # claim the fabric before anything that can run Python code - the
        # conversion below, or the registry lock - lets another thread in
        # past the test above
        self.fabric_busy = 1

        cd = pypmix_nb_setup(dicts, &prc)
        if NULL == cd:
            self.fabric_busy = 0
            return prc

        def regdone(status, ud):
            finfo = []
            self.fabric_busy = 0
            if PMIX_SUCCESS == status:
                self.fabric_set = 1
                finfo = self._fabric_info()
                if finfo is None:
                    status = PMIX_ERR_NOT_SUPPORTED
                    finfo = []
            cbfunc(status, finfo, ud)

        fab = &self.myfabric
        cd.idx = pypmix_nb_register(regdone, cbdata, self)
        nbidx = cd.idx
        with nogil:
            rc = PMIx_Fabric_register_nb(fab, cd.info, cd.ninfo,
                                         pypmix_client_op_cbfunc, <void *> cd)
        if PMIX_SUCCESS != rc:
            if pypmix_nb_take(nbidx) is not None:
                pypmix_nb_cbdata_free(cd)
            self.fabric_busy = 0
            if PMIX_OPERATION_SUCCEEDED == rc:
                self.fabric_set = 1
        return rc

    # cbfunc(status:int, fabricinfo:list, cbdata) - the refreshed fabric
    # information, as fabric_update() returns it
    def fabric_update_nb(self, cbfunc, cbdata=None):
        cdef pypmix_nb_cbdata_t *cd
        cdef pmix_status_t rc
        cdef int prc
        cdef pmix_fabric_t *fab

        if not callable(cbfunc):
            return PMIX_ERR_BAD_PARAM
        if 0 == self.fabric_set:
            return PMIX_ERR_INIT
        if 1 == self.fabric_busy:
            return PMIX_ERR_RESOURCE_BUSY
        # claimed at once - see fabric_register_nb
        self.fabric_busy = 1

        cd = pypmix_nb_setup(None, &prc)
        if NULL == cd:
            self.fabric_busy = 0
            return prc

        def upddone(status, ud):
            finfo = []
            self.fabric_busy = 0
            if PMIX_SUCCESS == status:
                finfo = self._fabric_info()
                if finfo is None:
                    status = PMIX_ERR_NOT_SUPPORTED
                    finfo = []
            cbfunc(status, finfo, ud)

        fab = &self.myfabric
        cd.idx = pypmix_nb_register(upddone, cbdata, self)
        nbidx = cd.idx
        with nogil:
            rc = PMIx_Fabric_update_nb(fab, pypmix_client_op_cbfunc,
                                       <void *> cd)
        if PMIX_SUCCESS != rc:
            if pypmix_nb_take(nbidx) is not None:
                pypmix_nb_cbdata_free(cd)
            self.fabric_busy = 0
        return rc

    # cbfunc(status:int, cbdata). The library completes a deregistration
    # itself, so this normally answers PMIX_OPERATION_SUCCEEDED and the
    # callback is not called
    def fabric_deregister_nb(self, cbfunc, cbdata=None):
        cdef pypmix_nb_cbdata_t *cd
        cdef pmix_status_t rc
        cdef int prc
        cdef pmix_fabric_t *fab

        if not callable(cbfunc):
            return PMIX_ERR_BAD_PARAM
        if 0 == self.fabric_set:
            return PMIX_ERR_INIT
        if 1 == self.fabric_busy:
            return PMIX_ERR_RESOURCE_BUSY
        # claimed at once - see fabric_register_nb
        self.fabric_busy = 1

        cd = pypmix_nb_setup(None, &prc)
        if NULL == cd:
            self.fabric_busy = 0
            return prc

        def deregdone(status, ud):
            self.fabric_busy = 0
            if PMIX_SUCCESS == status:
                self.fabric_set = 0
            cbfunc(status, ud)

        fab = &self.myfabric
        cd.idx = pypmix_nb_register(deregdone, cbdata, self)
        nbidx = cd.idx
        with nogil:
            rc = PMIx_Fabric_deregister_nb(fab, pypmix_client_op_cbfunc,
                                           <void *> cd)
        if PMIX_SUCCESS != rc:
            if pypmix_nb_take(nbidx) is not None:
                pypmix_nb_cbdata_free(cd)
            self.fabric_busy = 0
            if PMIX_OPERATION_SUCCEEDED == rc:
                self.fabric_set = 0
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
        nbidx = cd.idx
        with nogil:
            rc = PMIx_Compute_distances_nb(topo, &cd.cpuset,
                                           cd.info, cd.ninfo,
                                           pypmix_client_devdist_cbfunc,
                                           <void *> cd)
        if PMIX_SUCCESS != rc:
            if pypmix_nb_take(nbidx) is not None:
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
        # the block name as resource_block() takes it, converted before
        # the caddy exists so a name that cannot be has nothing to leak
        if isinstance(block, str):
            try:
                pyblock = block.encode('UTF-8')
            except UnicodeError:
                return PMIX_ERR_BAD_PARAM
        elif isinstance(block, (bytes, bytearray)):
            pyblock = bytes(block)
        else:
            return PMIX_ERR_TYPE_MISMATCH
        if b'\0' in pyblock:
            return PMIX_ERR_BAD_PARAM

        cd = pypmix_nb_setup(pyinfo, &prc)
        if NULL == cd:
            return prc

        # the block name and the units must outlive this call
        cd.blk = strdup(pyblock)
        if NULL == cd.blk:
            pypmix_nb_cbdata_free(cd)
            return PMIX_ERR_NOMEM
        prc = pmix_alloc_units(&cd.units, &cd.nunits, pyunits)
        if PMIX_SUCCESS != prc:
            pypmix_nb_cbdata_free(cd)
            return prc

        cd.idx = pypmix_nb_register(cbfunc, cbdata)
        nbidx = cd.idx
        with nogil:
            rc = PMIx_Resource_block_nb(drctv, cd.blk, cd.units, cd.nunits,
                                        cd.info, cd.ninfo,
                                        pypmix_client_op_cbfunc, <void *> cd)
        if PMIX_SUCCESS != rc:
            if pypmix_nb_take(nbidx) is not None:
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
        cdef pmix_status_t regrc = PMIX_ERR_EVENT_REGISTRATION

        # see _on_library_thread: refuse before taking _reglock
        if _on_library_thread():
            return PMIX_ERR_WOULD_BLOCK, -1

        # convert the codes to an array of ints
        codes = NULL
        ncodes = 0
        info = NULL
        ninfo = 0
        # the codes are checked before anything is allocated, from a
        # private copy (see pmix_snapshot)
        if pycodes is not None:
            pycodes = pmix_listcopy(pycodes)
            if pycodes is None:
                return PMIX_ERR_BAD_PARAM, -1
            for c in pycodes:
                if not isinstance(c, int) or isinstance(c, bool) or \
                   not (-2147483648 <= c <= 2147483647):
                    return PMIX_ERR_BAD_PARAM, -1
        if pycodes is not None and 0 < len(pycodes):
            ncodes = len(pycodes)
            codes = <int*> PyMem_Malloc(ncodes * sizeof(int))
            if not codes:
                # every other exit reports (rc, refid) - a bare status here
                # broke the caller's tuple unpacking
                return PMIX_ERR_NOMEM, -1
            n = 0
            for c in pycodes:
                codes[n] = c
                n += 1
        # allocate and load pmix info structs from python list of dictionaries
        if pyinfo is not None:
            info_ptr = &info
            rc = pmix_alloc_info(info_ptr, &ninfo, pyinfo)
            if PMIX_SUCCESS != rc:
                print("Error converting info array:", self.error_string(rc))
                if 0 < ncodes:
                    PyMem_Free(codes)
                return rc, -1

        # pass our hdlr switchyard to the API. The library can call it
        # before this returns - see _hdlr_find
        global _pending_evreg
        with _reglock:
            _pending_evreg = {'hdlr': hdlr, 'refid': None,
                             'codes': None if 0 == ncodes else list(pycodes)}
            try:
                with nogil:
                     regrc = PMIx_Register_event_handler(codes, ncodes, info, ninfo, pyeventhandler, NULL, NULL)
                _hdlr_settle(myhdlrs, _pending_evreg, regrc)
            finally:
                _pending_evreg = None
        rc = regrc

        # cleanup
        if 0 < ninfo:
            pmix_free_info(info, ninfo)
        if 0 < ncodes:
            PyMem_Free(codes)

        # if rc < 0, then there was an error
        if 0 > rc:
            return rc, -1

        # otherwise, this is our ref ID for this hdlr
        return PMIX_SUCCESS, rc

    # cbfunc(status:int, cbdata) - deregistering from inside the very
    # handler being deregistered is a natural thing to do, and that
    # handler runs on the progress thread
    def deregister_event_handler(self, ref:int, cbfunc=None, cbdata=None):
        cdef pypmix_nb_cbdata_t *cd
        cdef pmix_status_t crc
        cdef size_t cref
        cdef int prc

        if cbfunc is not None:
            if not callable(cbfunc):
                return PMIX_ERR_BAD_PARAM
            cref = ref
            # nothing of ours is retained - the caddy carries only the
            # registry key
            cd = pypmix_nb_setup(None, &prc)
            if NULL == cd:
                return prc
            cd.idx = pypmix_nb_register(cbfunc, cbdata)
            nbidx = cd.idx
            with nogil:
                crc = PMIx_Deregister_event_handler(cref,
                                                    pypmix_client_op_cbfunc,
                                                    <void *> cd)
            if PMIX_SUCCESS != crc:
                if pypmix_nb_take(nbidx) is not None:
                    pypmix_nb_cbdata_free(cd)
                return crc
            # the local record goes with it, as in the blocking form - the
            # library will not call this handler again
            _hdlr_drop(myhdlrs, ref)
            return crc

        # without the GIL: an event being dispatched to one of our
        # handlers needs it, and the library finishes that dispatch
        # before it answers this
        cref = ref
        with nogil:
            crc = PMIx_Deregister_event_handler(cref, NULL, NULL)
        if PMIX_SUCCESS == crc:
            _hdlr_drop(myhdlrs, ref)
        return crc

    # cbfunc(status:int, cbdata) - reporting an event from inside an
    # event handler or a server module upcall needs the non-blocking
    # form, as both run on the progress thread
    # Generate an event
    #
    # @pysrc [INPUT]
    #        - the proc dict the event is reported as coming from, or
    #          None for this process
    #
    # With a callback this is the non-blocking form: the library holds
    # the source and the info array until the event has been delivered,
    # so both live in the caddy rather than on this method's stack - the
    # form exists to be called from an event handler, and the queued
    # event always runs after the handler has returned.
    def notify_event(self, status:int, pysrc, range, pyinfo,
                     cbfunc=None, cbdata=None):
        cdef pmix_proc_t proc
        cdef pmix_proc_t *srcptr = NULL
        cdef pmix_info_t *info
        cdef pmix_info_t **info_ptr
        cdef size_t ninfo
        cdef pmix_data_range_t crange = range
        cdef pmix_status_t cstatus = status
        cdef pypmix_nb_cbdata_t *cd
        cdef pmix_status_t crc
        cdef int prc

        if pysrc is not None and not isinstance(pysrc, dict):
            return PMIX_ERR_BAD_PARAM

        if cbfunc is not None:
            if not callable(cbfunc):
                return PMIX_ERR_BAD_PARAM
            cd = pypmix_nb_setup(pyinfo, &prc)
            if NULL == cd:
                return prc
            if pysrc is not None:
                cd.procs = <pmix_proc_t*> PyMem_Malloc(sizeof(pmix_proc_t))
                if NULL == cd.procs:
                    pypmix_nb_cbdata_free(cd)
                    return PMIX_ERR_NOMEM
                cd.nprocs = 1
                prc = pmix_load_proc(cd.procs, pysrc)
                if PMIX_SUCCESS != prc:
                    pypmix_nb_cbdata_free(cd)
                    return prc
                srcptr = cd.procs
            cd.idx = pypmix_nb_register(cbfunc, cbdata)
            nbidx = cd.idx
            with nogil:
                crc = PMIx_Notify_event(cstatus, srcptr, crange,
                                        cd.info, cd.ninfo,
                                        pypmix_client_op_cbfunc, <void *> cd)
            if PMIX_SUCCESS != crc:
                if pypmix_nb_take(nbidx) is not None:
                    pypmix_nb_cbdata_free(cd)
            return crc

        # the blocking form waits for delivery, so the stack is fine
        if pysrc is not None:
            prc = pmix_load_proc(&proc, pysrc)
            if PMIX_SUCCESS != prc:
                return prc
            srcptr = &proc

        # allocate and load pmix info structs from python list of dictionaries
        info_ptr = &info
        rc = pmix_alloc_info(info_ptr, &ninfo, pyinfo)
        if PMIX_SUCCESS != rc:
            return rc

        # call the library
        with nogil:
            crc = PMIx_Notify_event(cstatus, srcptr, crange, info, ninfo, NULL, NULL)
        if 0 < ninfo:
            pmix_free_info(info, ninfo)
        return crc

    def error_string(self, pystat:int):
        cdef char *string

        string = <char*>PMIx_Error_string(pystat)
        if NULL == string:
            return None
        pystr = string
        return pystr.decode('ascii')

    def proc_state_string(self, pystat:int):
        cdef char *string

        string = <char*>PMIx_Proc_state_string(pystat)
        if NULL == string:
            return None
        pystr = string
        return pystr.decode('ascii')

    def scope_string(self, pystat:int):
        cdef char *string

        string = <char*>PMIx_Scope_string(pystat)
        if NULL == string:
            return None
        pystr = string
        return pystr.decode('ascii')

    def persistence_string(self, pystat:int):
        cdef char *string

        string = <char*>PMIx_Persistence_string(pystat)
        if NULL == string:
            return None
        pystr = string
        return pystr.decode('ascii')

    def data_range_string(self, pystat:int):
        cdef char *string

        string = <char*>PMIx_Data_range_string(pystat)
        if NULL == string:
            return None
        pystr = string
        return pystr.decode('ascii')

    def info_directives_string(self, pystat:int):
        cdef char *string

        string = <char*>PMIx_Info_directives_string(pystat)
        if NULL == string:
            return None
        pystr = string
        return pystr.decode('ascii')

    def data_type_string(self, pystat:int):
        cdef char *string

        string = <char*>PMIx_Data_type_string(pystat)
        if NULL == string:
            return None
        pystr = string
        return pystr.decode('ascii')

    def alloc_directive_string(self, pystat:int):
        cdef char *string

        string = <char*>PMIx_Alloc_directive_string(pystat)
        if NULL == string:
            return None
        pystr = string
        return pystr.decode('ascii')

    def iof_channel_string(self, pystat:int):
        cdef char *string

        string = <char*>PMIx_IOF_channel_string(pystat)
        if NULL == string:
            return None
        pystr = string
        return pystr.decode('ascii')

    def job_state_string(self, pystat:int):
        cdef char *string

        string = <char*>PMIx_Job_state_string(pystat)
        if NULL == string:
            return None
        pystr = string
        return pystr.decode('ascii')

    def get_attribute_string(self, rep:str):
        cdef char *string
        pyrep = rep.encode('ascii')
        string = <char*>PMIx_Get_attribute_string(pyrep)
        if NULL == string:
            return None
        pystr = string
        return pystr.decode('ascii')

    def get_attribute_name(self, rep:str):
        cdef char *string
        pyrep = rep.encode('ascii')
        string = <char*>PMIx_Get_attribute_name(pyrep)
        if NULL == string:
            return None
        pystr = string
        return pystr.decode('ascii')

    def link_state_string(self, pystat:int):
        cdef char *string

        string = <char*>PMIx_Link_state_string(pystat)
        if NULL == string:
            return None
        pystr = string
        return pystr.decode('ascii')

    def device_type_string(self, pystat:int):
        cdef char *string

        string = <char*>PMIx_Device_type_string(pystat)
        if NULL == string:
            return None
        pystr = string
        return pystr.decode('ascii')

    def value_comparison_string(self, pystat:int):
        cdef char *string

        string = <char*>PMIx_Value_comparison_string(pystat)
        if NULL == string:
            return None
        pystr = string
        return pystr.decode('ascii')

    def group_operation_string(self, pystat:int):
        cdef char *string

        string = <char*>PMIx_Group_operation_string(pystat)
        if NULL == string:
            return None
        pystr = string
        return pystr.decode('ascii')

    def resource_block_directive_string(self, pystat:int):
        cdef char *string

        string = <char*>PMIx_Resource_block_directive_string(pystat)
        if NULL == string:
            return None
        pystr = string
        return pystr.decode('ascii')

    def alloc_inheritance_string(self, pystat:int):
        cdef char *string

        string = <char*>PMIx_Alloc_inheritance_string(pystat)
        if NULL == string:
            return None
        pystr = string
        return pystr.decode('ascii')

    # The inverse of error_string: map the name of a PMIx status back to
    # its numeric value. Returns INT32_MIN for an unknown name - every
    # real status is itself negative, so only that sentinel means "not
    # recognized" - and PMIX_ERR_BAD_PARAM if handed None
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
            # zero first: a load that fails partway is destructed below,
            # and the destructor frees whatever pointers it finds in the
            # struct - which on an unzeroed stack value is whatever the
            # previous frame left there
            memset(&value, 0, sizeof(pmix_value_t))
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
            rc = pmix_load_info(&info, 1, [pysrc])
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
        rc = pmix_load_info(&info, 1, [pyinfo])
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
        # zero first - see data_print
        memset(&value, 0, sizeof(pmix_value_t))
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
        rc = pmix_load_procs(&proc, 1, [pyproc])
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
        rc = pmix_load_apps(&app, 1, [pyapp])
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
        rc = pmix_load_units(&unit, 1, [pyunit])
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
        cdef pmix_data_type_t ctype
        cdef pmix_status_t _gilrc
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
            rc = pmix_load_procs(&proc, 1, [target])
            if PMIX_SUCCESS != rc:
                return (rc, pybuf)
            tgt = &proc

        rc = pmix_load_dbuf(&buf, pybuf)
        if PMIX_SUCCESS != rc:
            pmix_destruct_dbuf(&buf)
            return (rc, pybuf)

        if PMIX_VALUE == data_type:
            # zero first - see data_print
            memset(&value, 0, sizeof(pmix_value_t))
            rc = pmix_load_value(&value, pysrc)
            if PMIX_SUCCESS == rc:
                ctype = PMIX_VALUE
                with nogil:
                    _gilrc = PMIx_Data_pack(tgt, &buf, <void*>&value, 1, ctype)
                rc = _gilrc
            pmix_destruct_value(&value)
        else:
            if 'key' not in pysrc:
                pmix_destruct_dbuf(&buf)
                return (PMIX_ERR_BAD_PARAM, pybuf)
            memset(&info, 0, sizeof(pmix_info_t))
            rc = pmix_load_info(&info, 1, [pysrc])
            if PMIX_SUCCESS == rc:
                ctype = PMIX_INFO
                with nogil:
                    _gilrc = PMIx_Data_pack(tgt, &buf, <void*>&info, 1, ctype)
                rc = _gilrc
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
        cdef pmix_data_type_t ctype
        cdef pmix_status_t _gilrc
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
            rc = pmix_load_procs(&proc, 1, [target])
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
            ctype = PMIX_VALUE
            with nogil:
                _gilrc = PMIx_Data_unpack(tgt, &buf, <void*>&value, &cnt, ctype)
            rc = _gilrc
            if PMIX_SUCCESS == rc:
                pyout = pmix_unload_value(&value)
                if pyout is None:
                    rc = PMIX_ERR_NOT_SUPPORTED
            pmix_destruct_value(&value)
        else:
            memset(&info, 0, sizeof(pmix_info_t))
            ctype = PMIX_INFO
            with nogil:
                _gilrc = PMIx_Data_unpack(tgt, &buf, <void*>&info, &cnt, ctype)
            rc = _gilrc
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
            # zero first - see data_print
            memset(&value, 0, sizeof(pmix_value_t))
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
            if pyout is None:
                rc = PMIX_ERR_NOT_SUPPORTED
            # the copy came from the library, so it goes back to it
            PMIx_Value_free(valdest, 1)
            return (rc, pyout)
        elif PMIX_INFO == data_type:
            if 'key' not in pysrc:
                return (PMIX_ERR_BAD_PARAM, None)
            memset(&info, 0, sizeof(pmix_info_t))
            rc = pmix_load_info(&info, 1, [pysrc])
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
        cdef pmix_status_t _gilrc
        cdef pmix_info_t *info
        cdef pmix_info_t **info_ptr
        cdef size_t sz
        fabricinfo = []
        if 1 == self.fabric_set or 1 == self.fabric_busy:
            return (PMIX_ERR_RESOURCE_BUSY, None)
        # claimed at once - see fabric_register_nb
        self.fabric_busy = 1

        # allocate and load pmix info structs from python list of dictionaries
        info_ptr = &info
        rc = pmix_alloc_info(info_ptr, &sz, dicts)
        if PMIX_SUCCESS != rc:
            self.fabric_busy = 0
            return (rc, None)

        with nogil:
            _gilrc = PMIx_Fabric_register(&self.myfabric, info, sz)
        self.fabric_busy = 0
        rc = _gilrc
        if 0 < sz:
            pmix_free_info(info, sz)
        if PMIX_SUCCESS == rc:
            self.fabric_set = 1
            # convert the fabric info array for return
            fabricinfo = self._fabric_info()
            if fabricinfo is None:
                return (PMIX_ERR_NOT_SUPPORTED, [])
        return (rc, fabricinfo)

    def fabric_update(self):
        cdef pmix_status_t _gilrc
        fabricinfo = []
        if 0 == self.fabric_set:
            return (PMIX_ERR_INIT, None)
        if 1 == self.fabric_busy:
            return (PMIX_ERR_RESOURCE_BUSY, None)
        self.fabric_busy = 1
        with nogil:
            _gilrc = PMIx_Fabric_update(&self.myfabric)
        self.fabric_busy = 0
        rc = _gilrc
        if PMIX_SUCCESS != rc:
            return (rc, fabricinfo)
        # convert the fabric info array for return
        fabricinfo = self._fabric_info()
        if fabricinfo is None:
            return (PMIX_ERR_NOT_SUPPORTED, [])
        return (rc, fabricinfo)

    def fabric_deregister(self):
        cdef pmix_status_t _gilrc
        if 0 == self.fabric_set:
            return PMIX_ERR_INIT
        if 1 == self.fabric_busy:
            return PMIX_ERR_RESOURCE_BUSY
        self.fabric_busy = 1
        with nogil:
            _gilrc = PMIx_Fabric_deregister(&self.myfabric)
        self.fabric_busy = 0
        rc = _gilrc
        # a deregistration that failed left the fabric registered
        if PMIX_SUCCESS == rc:
            self.fabric_set = 0
        return rc

    def load_topology(self):
        cdef pmix_status_t _gilrc
        with nogil:
            _gilrc = PMIx_Load_topology(&self.topo)
        rc = _gilrc
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
        cdef pmix_status_t _gilrc
        cdef pmix_cpuset_t cpuset

        pycpus = {}
        PMIx_Cpuset_construct(&cpuset)
        rc = PMIx_Get_cpuset(&cpuset, ref)
        if PMIX_SUCCESS == rc:
            rc = pmix_unload_cpuset(&cpuset, pycpus)
        pmix_destruct_cpuset(&cpuset)
        return (rc, pycpus)

    def compute_distances(self, pycpus, dicts):
        cdef pmix_status_t _gilrc
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
        with nogil:
            _gilrc = PMIx_Compute_distances(&self.topo, &cpuset, info, sz, &distances, &ndist)
        rc = _gilrc

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
        with nogil:
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
                 'toolconnected2', 'log2', 'sessioncontrol', 'resourceblock',
                 'abort2']
    if k not in permitted:
        return PMIX_ERR_BAD_PARAM
    # the listener slot is the host's own socket listener thread, which a
    # Python server cannot provide. It is in the list so the name is
    # recognized, and refused here rather than accepted and never wired
    # in - a handler registered for it would simply never run
    if 'listener' == k:
        print("SERVER MODULE FUNCTION 'listener' CANNOT BE PROVIDED BY A PYTHON SERVER")
        return PMIX_ERR_NOT_SUPPORTED
    if not callable(f):
        return PMIX_ERR_BAD_PARAM
    # last registration wins: a server that re-inits with a different
    # handler for a key used to keep silently calling the first one
    pmixservermodule[k] = f
    return PMIX_SUCCESS

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
        cdef pmix_status_t _gilrc
        cdef pmix_info_t *info
        cdef pmix_info_t **info_ptr
        cdef size_t sz

        # setup server module
        if map is None or 0 == len(map):
            print("SERVER REQUIRES AT LEAST ONE MODULE FUNCTION TO OPERATE")
            return PMIX_ERR_INIT
        kvkeys = list(map.keys())
        for key in kvkeys:
            mrc = setmodulefn(key, map[key])
            if PMIX_SUCCESS != mrc:
                if PMIX_ERR_BAD_PARAM == mrc:
                    print("SERVER MODULE FUNCTION ", key, " IS NOT RECOGNIZED")
                return PMIX_ERR_INIT

        # setup server module functions
        self.server_module_init(kvkeys)

        # allocate and load pmix info structs from python list of dictionaries
        info_ptr = &info
        rc = pmix_alloc_info(info_ptr, &sz, dicts)
        if PMIX_SUCCESS != rc:
            return rc
        if sz > 0:
            with nogil:
                _gilrc = PMIx_server_init(&self.myserver, info, sz)
            rc = _gilrc
            pmix_free_info(info, sz)
        else:
            with nogil:
                _gilrc = PMIx_server_init(&self.myserver, NULL, 0)
            rc = _gilrc
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
        # v7.x interfaces
        if 'abort2' in kvkeys:
            self.myserver.abort2 = <pmix_server_abort2_fn_t>clientaborted2

    def finalize(self):
        cdef pmix_status_t crc
        # an upcall still being delivered needs the GIL to finish, and
        # finalize waits for it
        with nogil:
            crc = PMIx_server_finalize()
        return crc

    def generate_regex(self, hosts:list):
        cdef pmix_status_t _gilrc
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
        # the library allocated it, so hand it back
        free(regex)
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
        cdef pmix_status_t _gilrc
        cdef char *ppn = NULL
        mysemi = ";"
        myprocs = mysemi.join(procs)
        pyprocs = myprocs.encode('ascii')
        rc = PMIx_generate_ppn(pyprocs, &ppn)
        # the library leaves the output pointer untouched when it fails
        if PMIX_SUCCESS != rc or NULL == ppn:
            return (rc, bytearray(0))
        # the generator answers a NUL-terminated string in every form the
        # library produces, so the whole string is the value
        ba = pmix_convert_regex(ppn)
        # the library allocated it, so hand it back
        free(ppn)
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
        cdef pmix_status_t _gilrc
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
    def register_nspace(self, ns:str, nlocalprocs:int, dicts,
                        cbfunc=None, cbdata=None):
        cdef pmix_status_t _gilrc
        cdef int cnlocal = nlocalprocs
        cdef pmix_nspace_t nspace
        cdef pmix_info_t *info
        cdef pmix_info_t **info_ptr
        cdef size_t sz
        cdef pypmix_nb_cbdata_t *cd
        cdef pmix_status_t crc
        cdef int prc
        global active
        # convert the args into the necessary C-arguments
        pmix_copy_nspace(nspace, ns)
        cnlocal = nlocalprocs

        # Non-blocking form. The C API expresses it with an optional
        # callback on the same entry point rather than with a separate
        # _nb function, but the machinery is the one every _nb binding
        # uses - see section 4c of the bindings AGENTS.md.
        #
        # This is the only form that works from inside a server module
        # upcall: those run on the progress thread, and the blocking form
        # would there be waiting on the event loop it is standing in.
        if cbfunc is not None:
            if not callable(cbfunc):
                return PMIX_ERR_BAD_PARAM
            # the library holds the info array until it completes, so it
            # goes in the caddy rather than being freed at return
            cd = pypmix_nb_setup(dicts, &prc)
            if NULL == cd:
                return prc
            cd.idx = pypmix_nb_register(cbfunc, cbdata)
            nbidx = cd.idx
            with nogil:
                crc = PMIx_server_register_nspace(nspace, cnlocal,
                                                  cd.info, cd.ninfo,
                                                  pypmix_client_op_cbfunc,
                                                  <void *> cd)
            if PMIX_SUCCESS != crc:
                # no callback will be executed, so reclaim it here
                if pypmix_nb_take(nbidx) is not None:
                    pypmix_nb_cbdata_free(cd)
            return crc

        # allocate and load pmix info structs from python list of dictionaries
        info_ptr = &info
        rc = pmix_alloc_info(info_ptr, &sz, dicts)
        if PMIX_SUCCESS != rc:
            return rc

        # the blocking form copies whatever it needs before it returns, so
        # the array is ours to release - it used to be leaked on every
        # namespace registration
        if sz > 0:
            with nogil:
                _gilrc = PMIx_server_register_nspace(nspace, cnlocal, info, sz, NULL, NULL)
            rc = _gilrc
            pmix_free_info(info, sz)
        else:
            with nogil:
                _gilrc = PMIx_server_register_nspace(nspace, cnlocal, NULL, 0, NULL, NULL)
            rc = _gilrc
        return rc

    # Deregister a namespace
    #
    # @ns [INPUT]
    #     - Namespace of job (string)
    #
    # Deleting a namespace from inside a server module upcall - when the
    # last client finalizes, say - is the natural thing to write, and the
    # upcall runs on the progress thread. Pass a callback for that case:
    # the blocking form there would be waiting on the event loop it is
    # standing in, which the library now reports rather than hanging on.
    #
    # cbfunc(status:int, cbdata)
    def deregister_nspace(self, ns:str, cbfunc=None, cbdata=None):
        cdef pmix_nspace_t nspace
        cdef pypmix_nb_cbdata_t *cd
        cdef int prc
        global active
        # convert the args into the necessary C-arguments
        pmix_copy_nspace(nspace, ns)

        if cbfunc is not None:
            if not callable(cbfunc):
                return PMIX_ERR_BAD_PARAM
            cd = pypmix_nb_setup(None, &prc)
            if NULL == cd:
                return prc
            cd.idx = pypmix_nb_register(cbfunc, cbdata)
            with nogil:
                PMIx_server_deregister_nspace(nspace, pypmix_client_op_cbfunc,
                                              <void *> cd)
            return PMIX_SUCCESS

        with nogil:
            PMIx_server_deregister_nspace(nspace, NULL, NULL)
        return PMIX_SUCCESS

    # Register a session
    #
    # @sessionID [INPUT]
    #     - the session's numerical identifier (int). UINT32_MAX is the
    #       library's own spelling of "no session" and is refused
    #
    # @dicts [INPUT]
    #     - a list of info dicts describing the session. Registering a
    #       session that is already known updates it
    #
    # A session can be registered before any job is launched into it,
    # and outlives its jobs until it is deregistered. With a callback
    # this is the non-blocking form, and the library holds the info array
    # until it completes, so it lives in the caddy.
    #
    # cbfunc(status:int, cbdata)
    def register_session(self, sessionID:int, dicts, cbfunc=None, cbdata=None):
        cdef pmix_status_t crc
        cdef uint32_t csid
        cdef pmix_info_t *info
        cdef pmix_info_t **info_ptr
        cdef size_t sz
        cdef pypmix_nb_cbdata_t *cd
        cdef int prc

        prc = pmix_check_int(sessionID, 0, 4294967295, "session ID")
        if PMIX_SUCCESS != prc:
            return prc
        csid = sessionID

        if cbfunc is not None:
            if not callable(cbfunc):
                return PMIX_ERR_BAD_PARAM
            cd = pypmix_nb_setup(dicts, &prc)
            if NULL == cd:
                return prc
            cd.idx = pypmix_nb_register(cbfunc, cbdata)
            nbidx = cd.idx
            with nogil:
                crc = PMIx_server_register_session(csid, cd.info, cd.ninfo,
                                                   pypmix_client_op_cbfunc,
                                                   <void *> cd)
            if PMIX_SUCCESS != crc:
                # no callback will be executed, so reclaim it here
                if pypmix_nb_take(nbidx) is not None:
                    pypmix_nb_cbdata_free(cd)
            return crc

        info_ptr = &info
        prc = pmix_alloc_info(info_ptr, &sz, dicts)
        if PMIX_SUCCESS != prc:
            return prc
        with nogil:
            crc = PMIx_server_register_session(csid, info, sz, NULL, NULL)
        if 0 < sz:
            pmix_free_info(info, sz)
        return crc

    # Deregister a session, releasing what the library holds for it. The
    # jobs in it are not deregistered by this; a host deregisters them
    # itself. The C API reports no status, so the blocking form returns
    # PMIX_SUCCESS, and the callback form reports through the callback
    #
    # cbfunc(status:int, cbdata)
    def deregister_session(self, sessionID:int, cbfunc=None, cbdata=None):
        cdef uint32_t csid
        cdef pypmix_nb_cbdata_t *cd
        cdef int prc

        prc = pmix_check_int(sessionID, 0, 4294967295, "session ID")
        if PMIX_SUCCESS != prc:
            return prc
        csid = sessionID

        if cbfunc is not None:
            if not callable(cbfunc):
                return PMIX_ERR_BAD_PARAM
            cd = pypmix_nb_setup(None, &prc)
            if NULL == cd:
                return prc
            cd.idx = pypmix_nb_register(cbfunc, cbdata)
            # every path through the C call completes through the
            # callback, including its early refusals
            with nogil:
                PMIx_server_deregister_session(csid, pypmix_client_op_cbfunc,
                                               <void *> cd)
            return PMIX_SUCCESS

        with nogil:
            PMIx_server_deregister_session(csid, NULL, NULL)
        return PMIX_SUCCESS

    # Register resources
    # cbfunc(status:int, cbdata) - the non-blocking form, which is the
    # only one usable from a server module upcall (see deregister_nspace)
    def register_resources(self, directives, cbfunc=None, cbdata=None):
        cdef pmix_status_t _gilrc
        cdef pmix_info_t *info
        cdef pmix_info_t **info_ptr
        cdef size_t sz
        cdef pypmix_nb_cbdata_t *cd
        cdef pmix_status_t crc
        cdef int prc

        if cbfunc is not None:
            if not callable(cbfunc):
                return PMIX_ERR_BAD_PARAM
            # the library holds the info array until it completes
            cd = pypmix_nb_setup(directives, &prc)
            if NULL == cd:
                return prc
            cd.idx = pypmix_nb_register(cbfunc, cbdata)
            nbidx = cd.idx
            with nogil:
                crc = PMIx_server_register_resources(cd.info, cd.ninfo,
                         pypmix_client_op_cbfunc, <void *> cd)
            if PMIX_SUCCESS != crc:
                if pypmix_nb_take(nbidx) is not None:
                    pypmix_nb_cbdata_free(cd)
            return crc

        # allocate and load pmix info structs from python list of dictionaries
        info_ptr = &info
        rc = pmix_alloc_info(info_ptr, &sz, directives)
        if PMIX_SUCCESS != rc:
            return rc

        with nogil:
            _gilrc = PMIx_server_register_resources(info, sz, NULL, NULL)
        rc = _gilrc
        if 0 < sz:
            pmix_free_info(info, sz)
        return rc

    # Deregister resources
    # cbfunc(status:int, cbdata) - the non-blocking form, which is the
    # only one usable from a server module upcall (see deregister_nspace)
    # Hand a collective still collecting its local participants to the host
    # now, as though it had timed out - see PMIx_server_collective_expire(3).
    # 'group' names a group construct or destruct; 'peers' the procs of a
    # fence, connect or disconnect
    def collective_expire(self, op:int, group, peers, directives,
                          cbfunc=None, cbdata=None):
        cdef pypmix_nb_cbdata_t *cd
        cdef pmix_status_t rc
        cdef int prc
        cdef pmix_collective_op_t cop = <pmix_collective_op_t> op

        if cbfunc is not None and not callable(cbfunc):
            return PMIX_ERR_BAD_PARAM
        # the group id, converted before the caddy exists so an id that
        # cannot be has nothing to leak
        pygrp = None
        if group is not None:
            pygrp = pmix_name_bytes(group, PMIX_MAX_NSLEN)
            if pygrp is None:
                return PMIX_ERR_BAD_PARAM
        # the library holds the id, procs and info until it completes
        cd = pypmix_nb_setup(directives, &prc)
        if NULL == cd:
            return prc
        if pygrp is not None:
            cd.grp = strdup(pygrp)
            if NULL == cd.grp:
                pypmix_nb_cbdata_free(cd)
                return PMIX_ERR_NOMEM
        # no procs means "not by membership" here, not "my job"
        if peers is not None:
            peers = pmix_listcopy(peers)
            if peers is None:
                pypmix_nb_cbdata_free(cd)
                return PMIX_ERR_BAD_PARAM
        if peers is not None and 0 < len(peers):
            prc = pypmix_nb_add_procs(cd, peers, self.myproc.nspace)
            if PMIX_SUCCESS != prc:
                pypmix_nb_cbdata_free(cd)
                return prc

        if cbfunc is not None:
            cd.idx = pypmix_nb_register(cbfunc, cbdata)
            nbidx = cd.idx
            with nogil:
                rc = PMIx_server_collective_expire(cop, cd.grp,
                         cd.procs, cd.nprocs, cd.info, cd.ninfo,
                         pypmix_client_op_cbfunc, <void *> cd)
            if PMIX_SUCCESS != rc:
                if pypmix_nb_take(nbidx) is not None:
                    pypmix_nb_cbdata_free(cd)
            return rc

        with nogil:
            rc = PMIx_server_collective_expire(cop, cd.grp,
                     cd.procs, cd.nprocs, cd.info, cd.ninfo, NULL, NULL)
        pypmix_nb_cbdata_free(cd)
        return rc

    def deregister_resources(self, directives, cbfunc=None, cbdata=None):
        cdef pmix_status_t _gilrc
        cdef pmix_info_t *info
        cdef pmix_info_t **info_ptr
        cdef size_t sz
        cdef pypmix_nb_cbdata_t *cd
        cdef pmix_status_t crc
        cdef int prc

        if cbfunc is not None:
            if not callable(cbfunc):
                return PMIX_ERR_BAD_PARAM
            # the library holds the info array until it completes
            cd = pypmix_nb_setup(directives, &prc)
            if NULL == cd:
                return prc
            cd.idx = pypmix_nb_register(cbfunc, cbdata)
            nbidx = cd.idx
            with nogil:
                crc = PMIx_server_deregister_resources(cd.info, cd.ninfo,
                         pypmix_client_op_cbfunc, <void *> cd)
            if PMIX_SUCCESS != crc:
                if pypmix_nb_take(nbidx) is not None:
                    pypmix_nb_cbdata_free(cd)
            return crc

        # allocate and load pmix info structs from python list of dictionaries
        info_ptr = &info
        rc = pmix_alloc_info(info_ptr, &sz, directives)
        if PMIX_SUCCESS != rc:
            return rc

        with nogil:
            _gilrc = PMIx_server_deregister_resources(info, sz, NULL, NULL)
        rc = _gilrc
        if 0 < sz:
            pmix_free_info(info, sz)
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
    # cbfunc(status:int, cbdata) - see deregister_nspace for why the
    # non-blocking form matters. Registering a namespace and its clients
    # from inside a 'spawn' upcall is the ordinary way a host services a
    # spawn request, and that upcall runs on the progress thread.
    def register_client(self, proc:dict, uid:int, gid:int,
                        cbfunc=None, cbdata=None):
        cdef pmix_status_t _gilrc
        cdef uid_t cuid = uid
        cdef gid_t cgid = gid
        global active
        cdef pmix_proc_t p;
        cdef pypmix_nb_cbdata_t *cd
        cdef pmix_status_t crc
        cdef int prc
        pmix_copy_nspace(p.nspace, proc['nspace'])
        p.rank = proc['rank']
        cuid = uid
        cgid = gid

        if cbfunc is not None:
            if not callable(cbfunc):
                return PMIX_ERR_BAD_PARAM
            # the library copies the proc before it returns, so the caddy
            # carries nothing but the registry key
            cd = pypmix_nb_setup(None, &prc)
            if NULL == cd:
                return prc
            cd.idx = pypmix_nb_register(cbfunc, cbdata)
            nbidx = cd.idx
            with nogil:
                crc = PMIx_server_register_client(&p, cuid, cgid, NULL,
                                                  pypmix_client_op_cbfunc,
                                                  <void *> cd)
            if PMIX_SUCCESS != crc:
                if pypmix_nb_take(nbidx) is not None:
                    pypmix_nb_cbdata_free(cd)
            return crc

        with nogil:
            _gilrc = PMIx_server_register_client(&p, cuid, cgid, NULL, NULL, NULL)
        rc = _gilrc
        return rc

    # Deregister a client process
    #
    # @proc [INPUT]
    #       - namespace and rank of the client (dict)
    #
    # cbfunc(status:int, cbdata) - see deregister_nspace
    def deregister_client(self, proc:dict, cbfunc=None, cbdata=None):
        global active
        cdef pmix_proc_t p;
        cdef pypmix_nb_cbdata_t *cd
        cdef int prc
        pmix_copy_nspace(p.nspace, proc['nspace'])
        p.rank = proc['rank']

        if cbfunc is not None:
            if not callable(cbfunc):
                return PMIX_ERR_BAD_PARAM
            cd = pypmix_nb_setup(None, &prc)
            if NULL == cd:
                return prc
            cd.idx = pypmix_nb_register(cbfunc, cbdata)
            with nogil:
                PMIx_server_deregister_client(&p, pypmix_client_op_cbfunc,
                                              <void *> cd)
            return PMIX_SUCCESS

        with nogil:
            PMIx_server_deregister_client(&p, NULL, NULL)
        return PMIX_SUCCESS

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
        cdef pmix_status_t _gilrc
        cdef pmix_proc_t p;
        cdef char **penv = NULL;
        cdef unicode pstring
        pmix_copy_nspace(p.nspace, proc['nspace'])
        p.rank = proc['rank']
        # convert the incoming dictionary to an array
        # of strings
        with nogil:
            _gilrc = PMIx_server_setup_fork(&p, &penv)
        rc = _gilrc
        if PMIX_SUCCESS == rc:
            # update the incoming dictionary
            n = 0
            while NULL != penv[n]:
                ln = strlen(penv[n])
                pstring = penv[n].decode('ascii')
                # split once: PATH-like values routinely contain '=', and
                # splitting on every one of them dropped everything past
                # the second
                kv = pstring.split('=', 1)
                if 2 == len(kv):
                    envin[kv[0]] = kv[1]
                free(penv[n])
                n += 1
            free(penv)
        return rc

    def dmodex_request(self, proc, dataout:dict):
        cdef pmix_status_t _gilrc
        global active
        cdef pmix_proc_t p;
        # this waits on 'active' for a callback the progress thread runs:
        # see _on_library_thread
        if _on_library_thread():
            return PMIX_ERR_WOULD_BLOCK, (None, 0)
        pmix_copy_nspace(p.nspace, proc['nspace'])
        p.rank = proc['rank']
        active.clear()
        pybo = (None, 0)
        with nogil:
            _gilrc = PMIx_server_dmodex_request(&p, dmodx_cbfunc, NULL)
        rc = _gilrc
        if PMIX_SUCCESS == rc:
            active.wait()
            # the call only accepted the request; its outcome - not
            # found, say - is what the callback reported
            rc = active.get_status()
            # transfer the data to the dictionary
            (data, sz) = active.fetch_data()
            pybo = (data, sz)
        return rc, pybo

    def dmodex_request2(self, proc, dicts:list):
        cdef pmix_status_t _gilrc
        global active
        cdef pmix_proc_t p;
        cdef pmix_info_t *info
        cdef pmix_info_t **info_ptr
        cdef size_t sz
        # this waits on 'active' for a callback the progress thread runs:
        # see _on_library_thread
        if _on_library_thread():
            return PMIX_ERR_WOULD_BLOCK, (None, 0)
        pmix_copy_nspace(p.nspace, proc['nspace'])
        p.rank = proc['rank']
        pybo = (None, 0)
        # the directives - the requester's identity - stay valid until the
        # callback, which we wait for
        info_ptr = &info
        rc = pmix_alloc_info(info_ptr, &sz, dicts)
        if PMIX_SUCCESS != rc:
            return rc, pybo
        active.clear()
        with nogil:
            _gilrc = PMIx_server_dmodex_request2(&p, info, sz, dmodx_cbfunc, NULL)
        rc = _gilrc
        if PMIX_SUCCESS == rc:
            active.wait()
            rc = active.get_status()
            # transfer the data to the dictionary
            (data, sz2) = active.fetch_data()
            pybo = (data, sz2)
        if 0 < sz:
            pmix_free_info(info, sz)
        return rc, pybo

    def setup_application(self, ns:str, dicts):
        cdef pmix_status_t _gilrc
        global active
        cdef pmix_nspace_t nspace;
        cdef pmix_info_t *info
        cdef pmix_info_t **info_ptr
        cdef size_t sz
        dataout = []
        # this waits on 'active' for a callback the progress thread runs:
        # see _on_library_thread
        if _on_library_thread():
            return (PMIX_ERR_WOULD_BLOCK, dataout)
        pmix_copy_nspace(nspace, ns)

        # allocate and load pmix info structs from python list of dictionaries
        info_ptr = &info
        rc = pmix_alloc_info(info_ptr, &sz, dicts)
        if PMIX_SUCCESS != rc:
            return (rc, dataout)

        active.clear()
        with nogil:
            _gilrc = PMIx_server_setup_application(nspace, info, sz, setupapp_cbfunc, NULL)
        rc = _gilrc
        if PMIX_SUCCESS == rc:
            active.wait()
            # transfer the data to the dictionary
            active.fetch_info(dataout)
            rc = active.get_status()
        if 0 < sz:
            pmix_free_info(info, sz)
        return (rc, dataout)

    def register_attributes(self, function, attrs):
        cdef pmix_status_t _gilrc
        cdef size_t nattrs
        cdef char *func
        cdef char **attarray
        nattrs    = 0

        if function is None:
            return PMIX_ERR_BAD_PARAM
        if attrs is None or 0 == len(attrs):
            return PMIX_SUCCESS
        # strdup takes bytes; handing it the str raised TypeError inside a
        # cdef function, where the exception was swallowed
        if isinstance(function, str):
            pyfunc = function.encode('ascii')
        else:
            pyfunc = function
        func = strdup(pyfunc)
        if NULL == func:
            return PMIX_ERR_NOMEM

        nattrs = len(attrs)
        # the entries are strdup'd, so the array must come from the C
        # allocator too - the old code freed a strdup'd string and a
        # PyMem array through the wrong allocators in both directions
        attarray = <char **> malloc((nattrs+1) * sizeof(char*))
        if not attarray:
            free(func)
            return PMIX_ERR_NOMEM
        memset(attarray, 0, (nattrs+1) * sizeof(char*))
        rc = pmix_load_argv(attarray, nattrs, attrs)
        if PMIX_SUCCESS != rc:
            pmix_free_argv(attarray)
            free(func)
            return rc

        # call Server API
        with nogil:
            _gilrc = PMIx_Register_attributes(func, attarray)
        rc = _gilrc

        pmix_free_argv(attarray)
        free(func)
        # report what the library said, rather than claiming success
        return rc

    def collect_inventory(self, pydirs):
        cdef pmix_status_t _gilrc
        cdef pmix_info_t *directives
        cdef pmix_info_t **directives_ptr
        cdef size_t ndirs
        ndirs   = 0
        dataout = []
        # this waits on 'active' for a callback the progress thread runs:
        # see _on_library_thread
        if _on_library_thread():
            return (PMIX_ERR_WOULD_BLOCK, dataout)

        # allocate and load pmix info structs from python list of dictionaries
        directives_ptr = &directives
        rc = pmix_alloc_info(directives_ptr, &ndirs, pydirs)
        if PMIX_SUCCESS != rc:
            return (rc, dataout)

        # call the API
        active.clear()
        with nogil:
            _gilrc = PMIx_server_collect_inventory(directives, ndirs,
                                               collectinventory_cbfunc, NULL)
        rc = _gilrc
        if PMIX_SUCCESS == rc:
            active.wait()
            # transfer the data to the dictionary
            active.fetch_info(dataout)
            rc = active.get_status()
        if 0 < ndirs:
            pmix_free_info(directives, ndirs)
        return (rc, dataout)

    # cbfunc(status:int, cbdata) - see deregister_nspace
    def deliver_inventory(self, pyinfo, pydirs, cbfunc=None, cbdata=None):
        cdef pmix_status_t _gilrc
        cdef pmix_info_t *directives
        cdef pmix_info_t **directives_ptr
        cdef pmix_info_t *info
        cdef pmix_info_t **info_ptr
        cdef size_t ndirs
        cdef size_t ninfo
        cdef pypmix_nb_cbdata_t *cd
        cdef pmix_status_t crc
        cdef int prc
        ndirs   = 0
        ninfo   = 0

        if cbfunc is not None:
            if not callable(cbfunc):
                return PMIX_ERR_BAD_PARAM
            # both arrays are held by the library until it completes
            cd = pypmix_nb_setup(pyinfo, &prc)
            if NULL == cd:
                return prc
            prc = pypmix_nb_add_dirs(cd, pydirs)
            if PMIX_SUCCESS != prc:
                pypmix_nb_cbdata_free(cd)
                return prc
            cd.idx = pypmix_nb_register(cbfunc, cbdata)
            nbidx = cd.idx
            with nogil:
                crc = PMIx_server_deliver_inventory(cd.info, cd.ninfo,
                                                    cd.dirs, cd.ndirs,
                                                    pypmix_client_op_cbfunc,
                                                    <void *> cd)
            if PMIX_SUCCESS != crc:
                if pypmix_nb_take(nbidx) is not None:
                    pypmix_nb_cbdata_free(cd)
            return crc

        # allocate and load pmix info structs from python list of dictionaries
        directives_ptr = &directives
        rc = pmix_alloc_info(directives_ptr, &ndirs, pydirs)
        if PMIX_SUCCESS != rc:
            return rc
        info_ptr = &info
        rc = pmix_alloc_info(info_ptr, &ninfo, pyinfo)
        if PMIX_SUCCESS != rc:
            if 0 < ndirs:
                pmix_free_info(directives, ndirs)
            return rc

        # call the API
        with nogil:
            _gilrc = PMIx_server_deliver_inventory(info, ninfo, directives, ndirs,
                                               NULL, NULL)
        rc = _gilrc
        if 0 < ninfo:
            pmix_free_info(info, ninfo)
        if 0 < ndirs:
            pmix_free_info(directives, ndirs)
        return rc

    # cbfunc(status:int, cbdata) - see deregister_nspace
    def setup_local_support(self, ns:str, ilist, cbfunc=None, cbdata=None):
        cdef pmix_status_t _gilrc
        global active
        cdef pmix_nspace_t nspace;
        cdef pmix_info_t *info
        cdef pmix_info_t **info_ptr
        cdef size_t sz
        cdef pypmix_nb_cbdata_t *cd
        cdef pmix_status_t crc
        cdef int prc
        pmix_copy_nspace(nspace, ns)

        if cbfunc is not None:
            if not callable(cbfunc):
                return PMIX_ERR_BAD_PARAM
            # the library copies the namespace but holds the info array
            cd = pypmix_nb_setup(ilist, &prc)
            if NULL == cd:
                return prc
            cd.idx = pypmix_nb_register(cbfunc, cbdata)
            nbidx = cd.idx
            with nogil:
                crc = PMIx_server_setup_local_support(nspace, cd.info, cd.ninfo,
                                                      pypmix_client_op_cbfunc,
                                                      <void *> cd)
            if PMIX_SUCCESS != crc:
                if pypmix_nb_take(nbidx) is not None:
                    pypmix_nb_cbdata_free(cd)
            return crc
        # convert the info list
        info_ptr = &info
        rc = pmix_alloc_info(info_ptr, &sz, ilist)
        if PMIX_SUCCESS != rc:
            return rc
        # the blocking form completes before it returns, so there is no
        # callback to wait on - the wait that used to be here was on the
        # module-global lock nothing on this path ever sets
        if sz > 0:
            with nogil:
                _gilrc = PMIx_server_setup_local_support(nspace, info, sz, NULL, NULL)
            rc = _gilrc
            pmix_free_info(info, sz)
        else:
            with nogil:
                _gilrc = PMIx_server_setup_local_support(nspace, NULL, 0, NULL, NULL)
            rc = _gilrc
        return rc

    # cbfunc(status:int, cbdata) - see deregister_nspace. Delivering
    # output from inside an IOF or server upcall needs this form
    def iof_deliver(self, pysrc:dict, pychannel:int, pydata:dict, pydirs,
                    cbfunc=None, cbdata=None):
        cdef pmix_status_t _gilrc
        cdef pmix_proc_t source
        cdef pmix_iof_channel_t channel
        cdef pypmix_nb_cbdata_t *cd
        cdef pmix_status_t crc
        cdef pmix_iof_channel_t cchannel
        cdef int prc

        if cbfunc is not None:
            if not callable(cbfunc):
                return PMIX_ERR_BAD_PARAM
            cchannel = pychannel
            # the library holds the source, the payload and the
            # directives until it completes, so all three go in the caddy
            cd = pypmix_nb_setup(None, &prc)
            if NULL == cd:
                return prc
            prc = pypmix_nb_add_procs(cd, [pysrc], self.myproc.nspace)
            if PMIX_SUCCESS != prc:
                pypmix_nb_cbdata_free(cd)
                return prc
            prc = pypmix_nb_add_bo(cd, pydata)
            if PMIX_SUCCESS != prc:
                pypmix_nb_cbdata_free(cd)
                return prc
            prc = pypmix_nb_add_dirs(cd, pydirs)
            if PMIX_SUCCESS != prc:
                pypmix_nb_cbdata_free(cd)
                return prc
            cd.idx = pypmix_nb_register(cbfunc, cbdata)
            nbidx = cd.idx
            with nogil:
                crc = PMIx_server_IOF_deliver(&cd.procs[0], cchannel, cd.bo,
                                              cd.dirs, cd.ndirs,
                                              pypmix_client_op_cbfunc,
                                              <void *> cd)
            if PMIX_SUCCESS != crc:
                if pypmix_nb_take(nbidx) is not None:
                    pypmix_nb_cbdata_free(cd)
            return crc
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
        rc = pmix_load_bo(&bo, pydata)
        if PMIX_SUCCESS != rc:
            return rc

        # allocate and load pmix info structs from python list of dictionaries
        directives_ptr = &directives
        rc = pmix_alloc_info(directives_ptr, &ndirs, pydirs)
        if PMIX_SUCCESS != rc:
            if NULL != bo.bytes:
                free(bo.bytes)
            return rc

        # call API
        with nogil:
            _gilrc = PMIx_server_IOF_deliver(&source, channel, &bo, directives, ndirs,
                                         NULL, NULL)
        rc = _gilrc
        if NULL != bo.bytes:
            free(bo.bytes)
        if 0 < ndirs:
            pmix_free_info(directives, ndirs)
        return rc

    # cbfunc(status:int, cbdata) - see deregister_nspace
    def iof_flow_control(self, pysrc, pychannel:int, pyxoff:bool, pydirs,
                         cbfunc=None, cbdata=None):
        cdef pmix_status_t _gilrc
        # Suspend (pyxoff True) or resume (pyxoff False) the processes
        # feeding stdin to us. A pysrc of None means every one of them,
        # so it is deliberately not annotated "dict" - Cython would
        # reject the None. Only PMIX_FWD_STDIN_CHANNEL can be
        # flow-controlled; anything else returns PMIX_ERR_NOT_SUPPORTED.
        cdef pmix_proc_t source
        cdef pmix_proc_t *srcptr
        cdef pmix_iof_channel_t channel
        cdef bint xoff
        cdef pmix_info_t *directives
        cdef pmix_info_t **directives_ptr
        cdef size_t ndirs
        cdef pypmix_nb_cbdata_t *cd
        cdef pmix_status_t crc
        cdef pmix_proc_t *cbsrc
        cdef int prc
        ndirs   = 0
        channel = pychannel
        xoff    = pyxoff
        srcptr  = NULL

        if cbfunc is not None:
            if not callable(cbfunc):
                return PMIX_ERR_BAD_PARAM
            cd = pypmix_nb_setup(None, &prc)
            if NULL == cd:
                return prc
            cbsrc = NULL
            if pysrc is not None:
                # a None source means "every producer", which the C API
                # spells as a NULL pointer
                prc = pypmix_nb_add_procs(cd, [pysrc], self.myproc.nspace)
                if PMIX_SUCCESS != prc:
                    pypmix_nb_cbdata_free(cd)
                    return prc
                cbsrc = &cd.procs[0]
            prc = pypmix_nb_add_dirs(cd, pydirs)
            if PMIX_SUCCESS != prc:
                pypmix_nb_cbdata_free(cd)
                return prc
            cd.idx = pypmix_nb_register(cbfunc, cbdata)
            nbidx = cd.idx
            with nogil:
                crc = PMIx_server_IOF_flow_control(cbsrc, channel, xoff,
                                                   cd.dirs, cd.ndirs,
                                                   pypmix_client_op_cbfunc,
                                                   <void *> cd)
            if PMIX_SUCCESS != crc:
                if pypmix_nb_take(nbidx) is not None:
                    pypmix_nb_cbdata_free(cd)
            return crc

        # convert pysrc to pmix_proc_t, if one was given
        if pysrc is not None:
            pmix_copy_nspace(source.nspace, pysrc['nspace'])
            source.rank = pysrc['rank']
            srcptr = &source

        # allocate and load pmix info structs from python list of dictionaries
        directives_ptr = &directives
        rc = pmix_alloc_info(directives_ptr, &ndirs, pydirs)
        if PMIX_SUCCESS != rc:
            return rc

        # call API - blocking form, so no callback
        with nogil:
            _gilrc = PMIx_server_IOF_flow_control(srcptr, channel, xoff,
                                              directives, ndirs, NULL, NULL)
        rc = _gilrc
        if 0 < ndirs:
            pmix_free_info(directives, ndirs)
        return rc

    def define_process_set(self, members, name:str):
        cdef pmix_status_t _gilrc
        cdef const char *cset
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
        rc = pmix_load_procs(procs, nprocs, members)
        if PMIX_SUCCESS != rc:
            pmix_free_procs(procs, nprocs)
            return rc
        # define the set
        cset = pyset
        with nogil:
            _gilrc = PMIx_server_define_process_set(procs, nprocs, cset)
        rc = _gilrc
        pmix_free_procs(procs, nprocs)
        return rc

    def delete_process_set(self, name:str):
        cdef pmix_status_t _gilrc
        cdef char *cset
        # convert set name
        pyset = name.encode('ascii')
        # delete the set
        cset = pyset
        with nogil:
            _gilrc = PMIx_server_delete_process_set(cset)
        rc = _gilrc
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
        rc = pmix_load_procs(procs, nprocs, peers)
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

    # cbfunc(status:int, results:list, cbdata) - note that this one
    # reports results, so it takes the info-style callback rather than
    # the plain op-style one the other server calls use
    def session_control(self, sessionID:int, ilist, cbfunc=None, cbdata=None):
        cdef pmix_status_t _gilrc
        cdef uint32_t csid = sessionID
        cdef pmix_info_t *info
        cdef pmix_info_t **info_ptr
        cdef size_t sz
        cdef pypmix_nb_cbdata_t *cd
        cdef pmix_status_t crc
        cdef uint32_t csession
        cdef int prc

        if cbfunc is not None:
            if not callable(cbfunc):
                return PMIX_ERR_BAD_PARAM
            csession = sessionID
            cd = pypmix_nb_setup(ilist, &prc)
            if NULL == cd:
                return prc
            cd.idx = pypmix_nb_register(cbfunc, cbdata)
            nbidx = cd.idx
            with nogil:
                crc = PMIx_Session_control(csession, cd.info, cd.ninfo,
                                           pypmix_client_info_cbfunc,
                                           <void *> cd)
            if PMIX_SUCCESS != crc:
                if pypmix_nb_take(nbidx) is not None:
                    pypmix_nb_cbdata_free(cd)
            return crc

        # allocate and load pmix info structs from python list of dictionaries
        info_ptr = &info
        rc = pmix_alloc_info(info_ptr, &sz, ilist)
        if PMIX_SUCCESS != rc:
            return rc

         # call the API
        if 0 < sz:
            with nogil:
                _gilrc = PMIx_Session_control(csid, info, sz, NULL, NULL)
            rc = _gilrc
            pmix_free_info(info, sz)
        else:
            with nogil:
                _gilrc = PMIx_Session_control(csid, NULL, 0, NULL, NULL)
            rc = _gilrc
        return rc

cdef int clientconnected(pmix_proc_t *proc, void *server_object,
                         pmix_op_cbfunc_t cbfunc, void *cbdata) noexcept with gil:
    # libpmix calls this across a C frame, so an exception
    # raised by the handler has nowhere to propagate to.
    # Without this, Cython returned -1 and left the error
    # indicator set for whatever Python code ran next in
    # this process to trip over.
    cbdata_dict = None
    try:
        keys = pmixservermodule.keys()
        if 'clientconnected' in keys:
            if not proc:
                return PMIX_ERR_BAD_PARAM
            myproc = []
            rc = pmix_unload_procs(proc, 1, myproc)
            if PMIX_SUCCESS != rc:
                return rc
            cbdata_dict = {'cbdata' : <uintptr_t> cbdata, 'cbfunc' : <uintptr_t> cbfunc}
            return pypmix_settle(pmixservermodule['clientconnected'](myproc[0], pypmix_op_cbfunc, cbdata_dict), cbdata_dict)

        return PMIX_ERR_NOT_FOUND
    except:
        return pypmix_settle_raise(cbdata_dict)

cdef int clientconnected2(pmix_proc_t *proc, void *server_object,
                          pmix_info_t info[], size_t ninfo,
                          pmix_op_cbfunc_t cbfunc, void *cbdata) noexcept with gil:
    # libpmix calls this across a C frame, so an exception
    # raised by the handler has nowhere to propagate to.
    # Without this, Cython returned -1 and left the error
    # indicator set for whatever Python code ran next in
    # this process to trip over.
    cbdata_dict = None
    try:
        keys = pmixservermodule.keys()
        if 'clientconnected2' in keys:
            if not proc:
                return PMIX_ERR_BAD_PARAM
            args = {}
            myproc = []
            rc = pmix_unload_procs(proc, 1, myproc)
            if PMIX_SUCCESS != rc:
                return rc
            args['proc'] = myproc[0]
            ilist = []
            if NULL != info:
                rc = pmix_unload_info(info, ninfo, ilist)
                if PMIX_SUCCESS != rc:
                    return rc
                args['directives'] = ilist
            cbdata_dict = {'cbdata' : <uintptr_t> cbdata, 'cbfunc' : <uintptr_t> cbfunc}
            return pypmix_settle(pmixservermodule['clientconnected2'](args, pypmix_op_cbfunc, cbdata_dict), cbdata_dict)

        return PMIX_ERR_NOT_FOUND
    except:
        return pypmix_settle_raise(cbdata_dict)

cdef int clientfinalized(pmix_proc_t *proc, void *server_object,
                         pmix_op_cbfunc_t cbfunc, void *cbdata) noexcept with gil:
    # libpmix calls this across a C frame, so an exception
    # raised by the handler has nowhere to propagate to.
    # Without this, Cython returned -1 and left the error
    # indicator set for whatever Python code ran next in
    # this process to trip over.
    cbdata_dict = None
    try:
        keys = pmixservermodule.keys()
        if 'clientfinalized' in keys:
            if not proc:
                return PMIX_ERR_BAD_PARAM
            myproc = []
            rc = pmix_unload_procs(proc, 1, myproc)
            if PMIX_SUCCESS != rc:
                return rc
            cbdata_dict = {'cbdata' : <uintptr_t> cbdata, 'cbfunc' : <uintptr_t> cbfunc}
            return pypmix_settle(pmixservermodule['clientfinalized'](myproc[0], pypmix_op_cbfunc, cbdata_dict), cbdata_dict)

        return PMIX_ERR_NOT_SUPPORTED
    except:
        return pypmix_settle_raise(cbdata_dict)

cdef int clientaborted(const pmix_proc_t *proc, void *server_object,
                       int status, const char msg[],
                       pmix_proc_t procs[], size_t nprocs,
                       pmix_op_cbfunc_t cbfunc, void *cbdata) noexcept with gil:
    # libpmix calls this across a C frame, so an exception
    # raised by the handler has nowhere to propagate to.
    # Without this, Cython returned -1 and left the error
    # indicator set for whatever Python code ran next in
    # this process to trip over.
    cbdata_dict = None
    try:
        keys = pmixservermodule.keys()
        if 'abort' in keys:
            args = {}
            myproc = []
            myprocs = []
            # convert the caller's name
            rc = pmix_unload_procs(proc, 1, myproc)
            if PMIX_SUCCESS != rc:
                return rc
            args['caller'] = myproc[0]
            # record the status
            args['status'] = status
            # record the msg, if given
            if NULL != msg:
                args['msg'] = pmix_decode_str(msg)
            # convert any provided array of procs to be aborted
            if NULL != procs:
                rc = pmix_unload_procs(procs, nprocs, myprocs)
                if PMIX_SUCCESS != rc:
                    return rc
                args['targets'] = myprocs
            cbdata_dict = {'cbdata' : <uintptr_t> cbdata, 'cbfunc' : <uintptr_t> cbfunc}
            # upcall it
            return pypmix_settle(pmixservermodule['abort'](args, pypmix_op_cbfunc, cbdata_dict), cbdata_dict)

        return PMIX_ERR_NOT_SUPPORTED
    except:
        return pypmix_settle_raise(cbdata_dict)

cdef int clientaborted2(const pmix_proc_t *proc, void *server_object,
                        int status, const char msg[],
                        pmix_proc_t procs[], size_t nprocs,
                        const pmix_info_t info[], size_t ninfo,
                        pmix_op_cbfunc_t cbfunc, void *cbdata) noexcept with gil:
    # libpmix calls this across a C frame, so an exception
    # raised by the handler has nowhere to propagate to.
    # Without this, Cython returned -1 and left the error
    # indicator set for whatever Python code ran next in
    # this process to trip over.
    cbdata_dict = None
    try:
        keys = pmixservermodule.keys()
        if 'abort2' in keys:
            args = {}
            myproc = []
            myprocs = []
            ilist = []
            # convert the caller's name
            rc = pmix_unload_procs(proc, 1, myproc)
            if PMIX_SUCCESS != rc:
                return rc
            args['caller'] = myproc[0]
            # record the status
            args['status'] = status
            # record the msg, if given
            if NULL != msg:
                args['msg'] = pmix_decode_str(msg)
            # convert any provided array of procs to be aborted
            if NULL != procs:
                rc = pmix_unload_procs(procs, nprocs, myprocs)
                if PMIX_SUCCESS != rc:
                    return rc
                args['targets'] = myprocs
            # the requester's identity, among anything else provided
            if NULL != info:
                rc = pmix_unload_info(info, ninfo, ilist)
                if PMIX_SUCCESS != rc:
                    return rc
                args['info'] = ilist
            cbdata_dict = {'cbdata' : <uintptr_t> cbdata, 'cbfunc' : <uintptr_t> cbfunc}
            # upcall it
            return pypmix_settle(pmixservermodule['abort2'](args, pypmix_op_cbfunc, cbdata_dict), cbdata_dict)

        return PMIX_ERR_NOT_SUPPORTED
    except:
        return pypmix_settle_raise(cbdata_dict)

cdef int fencenb(const pmix_proc_t procs[], size_t nprocs,
                 const pmix_info_t info[], size_t ninfo,
                 char *data, size_t ndata,
                 pmix_modex_cbfunc_t cbfunc, void *cbdata) noexcept with gil:
    # libpmix calls this across a C frame, so an exception
    # raised by the handler has nowhere to propagate to.
    # Without this, Cython returned -1 and left the error
    # indicator set for whatever Python code ran next in
    # this process to trip over.
    cbdata_dict = None
    try:
        keys = pmixservermodule.keys()
        if 'fencenb' in keys:
            args = {}
            myprocs = []
            blist = []
            ilist = []
            barray = None

            if NULL == procs:
                myprocs.append({'nspace': myname.get('nspace', ''), 'rank': PMIX_RANK_WILDCARD})
            else:
                rc = pmix_unload_procs(procs, nprocs, myprocs)
                if PMIX_SUCCESS != rc:
                    return rc
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
            return pypmix_settle(pmixservermodule['fencenb'](args, pypmix_modex_cbfunc, cbdata_dict), cbdata_dict)

        return PMIX_ERR_NOT_SUPPORTED
    except:
        return pypmix_settle_raise(cbdata_dict)



cdef int directmodex(const pmix_proc_t *proc,
                     const pmix_info_t info[], size_t ninfo,
                     pmix_modex_cbfunc_t cbfunc, void *cbdata) noexcept with gil:
    # libpmix calls this across a C frame, so an exception
    # raised by the handler has nowhere to propagate to.
    # Without this, Cython returned -1 and left the error
    # indicator set for whatever Python code ran next in
    # this process to trip over.
    cbdata_dict = None
    try:
        keys = pmixservermodule.keys()
        if 'directmodex' in keys:
            args = {}
            myprocs = []
            ilist = []
            rc = pmix_unload_procs(proc, 1, myprocs)
            if PMIX_SUCCESS != rc:
                return rc
            args['proc'] = myprocs[0]
            if NULL != info:
                rc = pmix_unload_info(info, ninfo, ilist)
                if PMIX_SUCCESS != rc:
                    return rc
                args['directives'] = ilist
            cbdata_dict = {'cbdata' : <uintptr_t> cbdata, 'cbfunc' : <uintptr_t> cbfunc}
            return pypmix_settle(pmixservermodule['directmodex'](args, pypmix_modex_cbfunc, cbdata_dict), cbdata_dict)

        return PMIX_ERR_NOT_SUPPORTED
    except:
        return pypmix_settle_raise(cbdata_dict)

cdef int publish(const pmix_proc_t *proc,
                 const pmix_info_t info[], size_t ninfo,
                 pmix_op_cbfunc_t cbfunc, void *cbdata) noexcept with gil:
    # libpmix calls this across a C frame, so an exception
    # raised by the handler has nowhere to propagate to.
    # Without this, Cython returned -1 and left the error
    # indicator set for whatever Python code ran next in
    # this process to trip over.
    cbdata_dict = None
    try:
        keys = pmixservermodule.keys()
        if 'publish' in keys:
            args = {}
            myprocs = []
            ilist = []
            rc = pmix_unload_procs(proc, 1, myprocs)
            if PMIX_SUCCESS != rc:
                return rc
            args['proc'] = myprocs[0]
            if NULL != info:
                rc = pmix_unload_info(info, ninfo, ilist)
                if PMIX_SUCCESS != rc:
                    return rc
            args['directives'] = ilist
            cbdata_dict = {'cbdata' : <uintptr_t> cbdata, 'cbfunc' : <uintptr_t> cbfunc}
            return pypmix_settle(pmixservermodule['publish'](args, pypmix_op_cbfunc, cbdata_dict), cbdata_dict)

        return PMIX_ERR_NOT_SUPPORTED
    except:
        return pypmix_settle_raise(cbdata_dict)

cdef int lookup(const pmix_proc_t *proc, char **keys,
                const pmix_info_t info[], size_t ninfo,
                pmix_lookup_cbfunc_t cbfunc, void *cbdata) noexcept with gil:
    # libpmix calls this across a C frame, so an exception
    # raised by the handler has nowhere to propagate to.
    # Without this, Cython returned -1 and left the error
    # indicator set for whatever Python code ran next in
    # this process to trip over.
    cbdata_dict = None
    try:
        srvkeys = pmixservermodule.keys()
        if 'lookup' in srvkeys:
            args = {}
            pdata   = []
            myprocs = []
            ilist = []
            pykeys = []
            rc = pmix_unload_procs(proc, 1, myprocs)
            if PMIX_SUCCESS != rc:
                return rc
            args['proc'] = myprocs[0]
            # decode, as the unpublish upcall and the query builder do - a
            # handler must not have to guess whether its keys are str or bytes
            if NULL != keys:
                pmix_unload_argv(keys, pykeys)
            args['keys'] = pykeys
            if NULL != info:
                rc = pmix_unload_info(info, ninfo, ilist)
                if PMIX_SUCCESS != rc:
                    return rc
                args['directives'] = ilist
            cbdata_dict = {'cbdata' : <uintptr_t> cbdata, 'cbfunc' : <uintptr_t> cbfunc}
            return pypmix_settle(pmixservermodule['lookup'](args, pypmix_lookup_cbfunc, cbdata_dict), cbdata_dict)

        return PMIX_ERR_NOT_SUPPORTED
    except:
        return pypmix_settle_raise(cbdata_dict)

cdef int unpublish(const pmix_proc_t *proc, char **keys,
                   const pmix_info_t info[], size_t ninfo,
                   pmix_op_cbfunc_t cbfunc, void *cbdata) noexcept with gil:
    # libpmix calls this across a C frame, so an exception
    # raised by the handler has nowhere to propagate to.
    # Without this, Cython returned -1 and left the error
    # indicator set for whatever Python code ran next in
    # this process to trip over.
    cbdata_dict = None
    try:
        srvkeys = pmixservermodule.keys()
        if 'unpublish' in srvkeys:
            args = {}
            myprocs = []
            ilist = []
            pykeys = []
            rc = pmix_unload_procs(proc, 1, myprocs)
            if PMIX_SUCCESS != rc:
                return rc
            args['proc'] = myprocs[0]
            if NULL != keys:
                pmix_unload_argv(keys, pykeys)
                args['keys'] = pykeys
            if NULL != info:
                rc = pmix_unload_info(info, ninfo, ilist)
                if PMIX_SUCCESS != rc:
                    return rc
                args['directives'] = ilist
            cbdata_dict = {'cbdata' : <uintptr_t> cbdata, 'cbfunc' : <uintptr_t> cbfunc}
            return pypmix_settle(pmixservermodule['unpublish'](args, pypmix_op_cbfunc, cbdata_dict), cbdata_dict)

        return PMIX_ERR_NOT_SUPPORTED
    except:
        return pypmix_settle_raise(cbdata_dict)

cdef int spawn(const pmix_proc_t *proc,
               const pmix_info_t job_info[], size_t ninfo,
               const pmix_app_t apps[], size_t napps,
               pmix_spawn_cbfunc_t cbfunc, void *cbdata) noexcept with gil:
    # libpmix calls this across a C frame, so an exception
    # raised by the handler has nowhere to propagate to.
    # Without this, Cython returned -1 and left the error
    # indicator set for whatever Python code ran next in
    # this process to trip over.
    cbdata_dict = None
    try:
        keys = pmixservermodule.keys()
        if 'spawn' in keys:
            args = {}
            myprocs = []
            ilist = []
            pyapps = []
            rc = pmix_unload_procs(proc, 1, myprocs)
            if PMIX_SUCCESS != rc:
                return rc
            args['proc'] = myprocs[0]
            if NULL != job_info:
                rc = pmix_unload_info(job_info, ninfo, ilist)
                if PMIX_SUCCESS != rc:
                    return rc
                args['jobinfo'] = ilist
            pmix_unload_apps(apps, napps, pyapps)
            args['apps'] = pyapps
            cbdata_dict = {'cbdata' : <uintptr_t> cbdata, 'cbfunc' : <uintptr_t> cbfunc}
            return pypmix_settle(pmixservermodule['spawn'](args, pypmix_spawn_cbfunc, cbdata_dict), cbdata_dict)

        return PMIX_ERR_NOT_SUPPORTED
    except:
        return pypmix_settle_raise(cbdata_dict)


cdef int connect(const pmix_proc_t procs[], size_t nprocs,
                 const pmix_info_t info[], size_t ninfo,
                 pmix_op_cbfunc_t cbfunc, void *cbdata) noexcept with gil:
    # libpmix calls this across a C frame, so an exception
    # raised by the handler has nowhere to propagate to.
    # Without this, Cython returned -1 and left the error
    # indicator set for whatever Python code ran next in
    # this process to trip over.
    cbdata_dict = None
    try:
        keys = pmixservermodule.keys()
        if 'connect' in keys:
            args = {}
            myprocs = []
            ilist = []
            if NULL != procs:
                rc = pmix_unload_procs(procs, nprocs, myprocs)
                if PMIX_SUCCESS != rc:
                    return rc
                args['procs'] = myprocs
            if NULL != info:
                rc = pmix_unload_info(info, ninfo, ilist)
                if PMIX_SUCCESS != rc:
                    return rc
                args['directives'] = ilist
            cbdata_dict = {'cbdata' : <uintptr_t> cbdata, 'cbfunc' : <uintptr_t> cbfunc}
            return pypmix_settle(pmixservermodule['connect'](args, pypmix_op_cbfunc, cbdata_dict), cbdata_dict)

        return PMIX_ERR_NOT_SUPPORTED
    except:
        return pypmix_settle_raise(cbdata_dict)

cdef int disconnect(const pmix_proc_t procs[], size_t nprocs,
                    const pmix_info_t info[], size_t ninfo,
                    pmix_op_cbfunc_t cbfunc, void *cbdata) noexcept with gil:
    # libpmix calls this across a C frame, so an exception
    # raised by the handler has nowhere to propagate to.
    # Without this, Cython returned -1 and left the error
    # indicator set for whatever Python code ran next in
    # this process to trip over.
    cbdata_dict = None
    try:
        keys = pmixservermodule.keys()
        if 'disconnect' in keys:
            args = {}
            myprocs = []
            ilist = []
            if NULL != procs:
                rc = pmix_unload_procs(procs, nprocs, myprocs)
                if PMIX_SUCCESS != rc:
                    return rc
                args['procs'] = myprocs
            if NULL != info:
                rc = pmix_unload_info(info, ninfo, ilist)
                if PMIX_SUCCESS != rc:
                    return rc
                args['directives'] = ilist
            cbdata_dict = {'cbdata' : <uintptr_t> cbdata, 'cbfunc' : <uintptr_t> cbfunc}
            return pypmix_settle(pmixservermodule['disconnect'](args, pypmix_op_cbfunc, cbdata_dict), cbdata_dict)

        return PMIX_ERR_NOT_SUPPORTED
    except:
        return pypmix_settle_raise(cbdata_dict)

cdef int registerevents(pmix_status_t *codes, size_t ncodes,
                        const pmix_info_t info[], size_t ninfo,
                        pmix_op_cbfunc_t cbfunc, void *cbdata) noexcept with gil:
    # libpmix calls this across a C frame, so an exception
    # raised by the handler has nowhere to propagate to.
    # Without this, Cython returned -1 and left the error
    # indicator set for whatever Python code ran next in
    # this process to trip over.
    cbdata_dict = None
    try:
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
                rc = pmix_unload_info(info, ninfo, ilist)
                if PMIX_SUCCESS != rc:
                    return rc
                args['directives'] = ilist
            cbdata_dict = {'cbdata' : <uintptr_t> cbdata, 'cbfunc' : <uintptr_t> cbfunc}
            return pypmix_settle(pmixservermodule['registerevents'](args, pypmix_op_cbfunc, cbdata_dict), cbdata_dict)

        return PMIX_ERR_NOT_SUPPORTED
    except:
        return pypmix_settle_raise(cbdata_dict)

cdef int deregisterevents(pmix_status_t *codes, size_t ncodes,
                          pmix_op_cbfunc_t cbfunc, void *cbdata) noexcept with gil:
    # libpmix calls this across a C frame, so an exception
    # raised by the handler has nowhere to propagate to.
    # Without this, Cython returned -1 and left the error
    # indicator set for whatever Python code ran next in
    # this process to trip over.
    cbdata_dict = None
    try:
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
            return pypmix_settle(pmixservermodule['deregisterevents'](args, pypmix_op_cbfunc, cbdata_dict), cbdata_dict)

        return PMIX_ERR_NOT_SUPPORTED
    except:
        return pypmix_settle_raise(cbdata_dict)


cdef int notifyevent(pmix_status_t code,
                     const pmix_proc_t *source,
                     pmix_data_range_t drange,
                     pmix_info_t info[], size_t ninfo,
                     pmix_op_cbfunc_t cbfunc, void *cbdata) noexcept with gil:
    # libpmix calls this across a C frame, so an exception
    # raised by the handler has nowhere to propagate to.
    # Without this, Cython returned -1 and left the error
    # indicator set for whatever Python code ran next in
    # this process to trip over.
    cbdata_dict = None
    try:
        keys = pmixservermodule.keys()
        if 'notifyevent' in keys:
            args = {}
            ilist = []
            myproc = []
            args['code'] = code
            rc = pmix_unload_procs(source, 1, myproc)
            if PMIX_SUCCESS != rc:
                return rc
            args['source'] = myproc[0]
            args['range'] = drange
            if NULL != info:
                rc = pmix_unload_info(info, ninfo, ilist)
                if PMIX_SUCCESS != rc:
                    return rc
                args['directives'] = ilist
            cbdata_dict = {'cbdata' : <uintptr_t> cbdata, 'cbfunc' : <uintptr_t> cbfunc}
            return pypmix_settle(pmixservermodule['notifyevent'](args, pypmix_op_cbfunc, cbdata_dict), cbdata_dict)

        return PMIX_ERR_NOT_SUPPORTED
    except:
        return pypmix_settle_raise(cbdata_dict)


cdef int query(pmix_proc_t *source,
               pmix_query_t *queries, size_t nqueries,
               pmix_info_cbfunc_t cbfunc,
               void *cbdata) noexcept with gil:
    # libpmix calls this across a C frame, so an exception
    # raised by the handler has nowhere to propagate to.
    # Without this, Cython returned -1 and left the error
    # indicator set for whatever Python code ran next in
    # this process to trip over.
    cbdata_dict = None
    try:
        pyqueries = []
        keys = pmixservermodule.keys()
        if 'query' in keys:
            args = {}
            myproc = []
            if NULL == queries or NULL == source:
                return PMIX_ERR_BAD_PARAM
            rc = pmix_unload_queries(queries, nqueries, pyqueries)
            if PMIX_SUCCESS != rc:
                return rc
            args['queries'] = pyqueries
            rc = pmix_unload_procs(source, 1, myproc)
            if PMIX_SUCCESS != rc:
                return rc
            args['source'] = myproc[0]
            cbdata_dict = {'cbdata' : <uintptr_t> cbdata, 'cbfunc' : <uintptr_t> cbfunc}
            return pypmix_settle(pmixservermodule['query'](args, pypmix_info_cbfunc, cbdata_dict), cbdata_dict)

        return PMIX_ERR_NOT_SUPPORTED
    except:
        return pypmix_settle_raise(cbdata_dict)

# The two up-calls below return nothing, so the library has no status to
# act on: the callback is the only way the request is ever answered. So
# every path that does not reach the handler - a missing handler, a
# conversion that fails, a handler that raises before completing -
# completes it with an error, through the same one-shot token the
# helpers use.
cdef void toolconnected(pmix_info_t *info, size_t ninfo,
                        pmix_tool_connection_cbfunc_t cbfunc,
                        void *cbdata) noexcept with gil:
    cbdata_dict = {'cbdata' : <uintptr_t> cbdata, 'cbfunc' : <uintptr_t> cbfunc}
    try:
        keys = pmixservermodule.keys()
        if 'toolconnected' in keys:
            args = {}
            ilist = []
            if NULL != info:
                rc = pmix_unload_info(info, ninfo, ilist)
                if PMIX_SUCCESS != rc:
                    pypmix_tool_connection_cbfunc(rc, None, cbdata_dict)
                    return
                args['directives'] = ilist
            pmixservermodule['toolconnected'](args, pypmix_tool_connection_cbfunc, cbdata_dict)
            return
        pypmix_tool_connection_cbfunc(PMIX_ERR_NOT_SUPPORTED, None, cbdata_dict)
    except:
        traceback.print_exc()
        try:
            pypmix_tool_connection_cbfunc(PMIX_ERROR, None, cbdata_dict)
        except:
            traceback.print_exc()

cdef void log(const pmix_proc_t *client,
              const pmix_info_t data[], size_t ndata,
              const pmix_info_t directives[], size_t ndirs,
              pmix_op_cbfunc_t cbfunc, void *cbdata) noexcept with gil:
    # see toolconnected above
    cbdata_dict = {'cbdata' : <uintptr_t> cbdata, 'cbfunc' : <uintptr_t> cbfunc}
    try:
        keys = pmixservermodule.keys()
        if 'log' in keys:
            args = {}
            ilist = []
            myproc = []
            mydirs = []
            if NULL == client:
                pypmix_op_cbfunc(PMIX_ERR_BAD_PARAM, cbdata_dict)
                return
            rc = pmix_unload_procs(client, 1, myproc)
            if PMIX_SUCCESS == rc and NULL != data:
                rc = pmix_unload_info(data, ndata, ilist)
                args['data'] = ilist
            if PMIX_SUCCESS == rc and NULL != directives:
                rc = pmix_unload_info(directives, ndirs, mydirs)
                args['directives'] = mydirs
            if PMIX_SUCCESS != rc:
                pypmix_op_cbfunc(rc, cbdata_dict)
                return
            args['source'] = myproc[0]
            pmixservermodule['log'](args, pypmix_op_cbfunc, cbdata_dict)
            return
        pypmix_op_cbfunc(PMIX_ERR_NOT_SUPPORTED, cbdata_dict)
    except:
        traceback.print_exc()
        try:
            pypmix_op_cbfunc(PMIX_ERROR, cbdata_dict)
        except:
            traceback.print_exc()

cdef int allocate(const pmix_proc_t *client,
                  pmix_alloc_directive_t action,
                  const pmix_info_t directives[], size_t ndirs,
                  pmix_info_cbfunc_t cbfunc, void *cbdata) noexcept with gil:
    # libpmix calls this across a C frame, so an exception
    # raised by the handler has nowhere to propagate to.
    # Without this, Cython returned -1 and left the error
    # indicator set for whatever Python code ran next in
    # this process to trip over.
    cbdata_dict = None
    try:
        keys = pmixservermodule.keys()
        if 'allocate' in keys:
            args = {}
            myproc = []
            keyvals = []
            if NULL == client:
                return PMIX_ERR_BAD_PARAM
            rc = pmix_unload_procs(client, 1, myproc)
            if PMIX_SUCCESS != rc:
                return rc
            args['source'] = myproc[0]
            args['action'] = action
            if NULL != directives:
                rc = pmix_unload_info(directives, ndirs, keyvals)
                if PMIX_SUCCESS != rc:
                    return rc
                args['directives'] = keyvals
            cbdata_dict = {'cbdata' : <uintptr_t> cbdata, 'cbfunc' : <uintptr_t> cbfunc}
            return pypmix_settle(pmixservermodule['allocate'](args, pypmix_info_cbfunc, cbdata_dict), cbdata_dict)

        return PMIX_ERR_NOT_SUPPORTED
    except:
        return pypmix_settle_raise(cbdata_dict)


cdef int jobcontrol(const pmix_proc_t *requestor,
                    const pmix_proc_t targets[], size_t ntargets,
                    const pmix_info_t directives[], size_t ndirs,
                    pmix_info_cbfunc_t cbfunc, void *cbdata) noexcept with gil:
    # libpmix calls this across a C frame, so an exception
    # raised by the handler has nowhere to propagate to.
    # Without this, Cython returned -1 and left the error
    # indicator set for whatever Python code ran next in
    # this process to trip over.
    cbdata_dict = None
    try:
        keys = pmixservermodule.keys()
        if 'jobcontrol' in keys:
            args = {}
            myproc = []
            mytargets = []
            mydirs = []
            if NULL != requestor:
                rc = pmix_unload_procs(requestor, 1, myproc)
                if PMIX_SUCCESS != rc:
                    return rc
                args['source'] = myproc[0]
            if NULL != targets:
                rc = pmix_unload_procs(targets, ntargets, mytargets)
                if PMIX_SUCCESS != rc:
                    return rc
                args['targets'] = mytargets
            if NULL != directives:
                rc = pmix_unload_info(directives, ndirs, mydirs)
                if PMIX_SUCCESS != rc:
                    return rc
                args['directives'] = mydirs
            cbdata_dict = {'cbdata' : <uintptr_t> cbdata, 'cbfunc' : <uintptr_t> cbfunc}
            return pypmix_settle(pmixservermodule['jobcontrol'](args, pypmix_info_cbfunc, cbdata_dict), cbdata_dict)

        return PMIX_ERR_NOT_SUPPORTED
    except:
        return pypmix_settle_raise(cbdata_dict)

cdef int monitor(const pmix_proc_t *requestor,
                 const pmix_info_t *monitor, pmix_status_t error,
                 const pmix_info_t directives[], size_t ndirs,
                 pmix_info_cbfunc_t cbfunc, void *cbdata) noexcept with gil:
    # libpmix calls this across a C frame, so an exception
    # raised by the handler has nowhere to propagate to.
    # Without this, Cython returned -1 and left the error
    # indicator set for whatever Python code ran next in
    # this process to trip over.
    cbdata_dict = None
    try:
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
                rc = pmix_unload_procs(requestor, 1, myproc)
                if PMIX_SUCCESS != rc:
                    return rc
                args['source'] = myproc[0]
            rc = pmix_unload_info(monitor, 1, mymon)
            if PMIX_SUCCESS != rc:
                return rc
            args['monitor'] = mymon[0]
            args['error'] = error
            if NULL != directives:
                rc = pmix_unload_info(directives, ndirs, mydirs)
                if PMIX_SUCCESS != rc:
                    return rc
                args['directives'] = mydirs
            cbdata_dict = {'cbdata' : <uintptr_t> cbdata, 'cbfunc' : <uintptr_t> cbfunc}
            return pypmix_settle(pmixservermodule['monitor'](args, pypmix_info_cbfunc, cbdata_dict), cbdata_dict)

        return PMIX_ERR_NOT_SUPPORTED
    except:
        return pypmix_settle_raise(cbdata_dict)

cdef int getcredential(const pmix_proc_t *proc,
                       const pmix_info_t directives[], size_t ndirs,
                       pmix_credential_cbfunc_t cbfunc, void *cbdata) noexcept with gil:
    # libpmix calls this across a C frame, so an exception
    # raised by the handler has nowhere to propagate to.
    # Without this, Cython returned -1 and left the error
    # indicator set for whatever Python code ran next in
    # this process to trip over.
    cbdata_dict = None
    try:
        keys = pmixservermodule.keys()
        if 'getcredential' in keys:
            args = {}
            myproc = []
            mydirs = []
            if NULL != proc:
                rc = pmix_unload_procs(proc, 1, myproc)
                if PMIX_SUCCESS != rc:
                    return rc
                args['source'] = myproc[0]
            if NULL != directives:
                rc = pmix_unload_info(directives, ndirs, mydirs)
                if PMIX_SUCCESS != rc:
                    return rc
                args['directives'] = mydirs
            cbdata_dict = {'cbdata' : <uintptr_t> cbdata, 'cbfunc' : <uintptr_t> cbfunc}
            return pypmix_settle(pmixservermodule['getcredential'](args, pypmix_credential_cbfunc, cbdata_dict), cbdata_dict)

        return PMIX_ERR_NOT_SUPPORTED
    except:
        return pypmix_settle_raise(cbdata_dict)

cdef int validatecredential(const pmix_proc_t *proc,
                            const pmix_byte_object_t *cred,
                            const pmix_info_t directives[], size_t ndirs,
                            pmix_validation_cbfunc_t cbfunc, void *cbdata) noexcept with gil:
    # libpmix calls this across a C frame, so an exception
    # raised by the handler has nowhere to propagate to.
    # Without this, Cython returned -1 and left the error
    # indicator set for whatever Python code ran next in
    # this process to trip over.
    cbdata_dict = None
    try:
        keys = pmixservermodule.keys()
        if 'validatecredential' in keys:
            args = {}
            keyvals = {}
            myproc = []
            mydirs = []
            blist = []
            pycred = {}
            if NULL != proc:
                rc = pmix_unload_procs(proc, 1, myproc)
                if PMIX_SUCCESS != rc:
                    return rc
                args['source'] = myproc[0]
            if NULL != cred:
                pmix_unload_bytes(cred[0].bytes, cred[0].size, blist)
                barray = bytearray(blist)
                pycred['bytes'] = barray
                pycred['size'] = cred[0].size
                args['credential'] = pycred
            if NULL != directives:
                rc = pmix_unload_info(directives, ndirs, mydirs)
                if PMIX_SUCCESS != rc:
                    return rc
                args['directives'] = mydirs
            cbdata_dict = {'cbdata' : <uintptr_t> cbdata, 'cbfunc' : <uintptr_t> cbfunc}
            return pypmix_settle(pmixservermodule['validatecredential'](args, pypmix_validation_cbfunc, cbdata_dict), cbdata_dict)

        return PMIX_ERR_NOT_SUPPORTED
    except:
        return pypmix_settle_raise(cbdata_dict)

cdef int iofpull(const pmix_proc_t procs[], size_t nprocs,
                 const pmix_info_t directives[], size_t ndirs,
                 pmix_iof_channel_t channels,
                 pmix_op_cbfunc_t cbfunc, void *cbdata) noexcept with gil:
    # libpmix calls this across a C frame, so an exception
    # raised by the handler has nowhere to propagate to.
    # Without this, Cython returned -1 and left the error
    # indicator set for whatever Python code ran next in
    # this process to trip over.
    cbdata_dict = None
    try:
        keys = pmixservermodule.keys()
        if 'iofpull' in keys:
            args = {}
            keyvals = {}
            myprocs = []
            mydirs = []
            pychannels = int(channels)
            args['channels'] = channels
            if NULL != procs:
                rc = pmix_unload_procs(procs, nprocs, myprocs)
                if PMIX_SUCCESS != rc:
                    return rc
                args['sources'] = myprocs
            if NULL != directives:
                rc = pmix_unload_info(directives, ndirs, mydirs)
                if PMIX_SUCCESS != rc:
                    return rc
                args['directives'] = mydirs
            cbdata_dict = {'cbdata' : <uintptr_t> cbdata, 'cbfunc' : <uintptr_t> cbfunc}
            return pypmix_settle(pmixservermodule['iofpull'](args, pypmix_op_cbfunc, cbdata_dict), cbdata_dict)

        return PMIX_ERR_NOT_SUPPORTED
    except:
        return pypmix_settle_raise(cbdata_dict)

cdef int pushstdin(const pmix_proc_t *source,
                   const pmix_proc_t targets[], size_t ntargets,
                   const pmix_info_t directives[], size_t ndirs,
                   const pmix_byte_object_t *bo,
                   pmix_op_cbfunc_t cbfunc, void *cbdata) noexcept with gil:
    # libpmix calls this across a C frame, so an exception
    # raised by the handler has nowhere to propagate to.
    # Without this, Cython returned -1 and left the error
    # indicator set for whatever Python code ran next in
    # this process to trip over.
    cbdata_dict = None
    try:
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
                rc = pmix_unload_procs(source, 1, myproc)
                if PMIX_SUCCESS != rc:
                    return rc
                args['source'] = myproc[0]
            if NULL != targets:
                rc = pmix_unload_procs(targets, ntargets, mytargets)
                if PMIX_SUCCESS != rc:
                    return rc
                args['targets'] = mytargets
            if NULL != directives:
                rc = pmix_unload_info(directives, ndirs, mydirs)
                if PMIX_SUCCESS != rc:
                    return rc
                args['directives'] = mydirs
            if NULL != bo:
                pmix_unload_bytes(bo[0].bytes, bo[0].size, blist)
                barray = bytearray(blist)
                pyload['bytes'] = barray
                pyload['size'] = bo[0].size
                args['payload'] = pyload
            cbdata_dict = {'cbdata' : <uintptr_t> cbdata, 'cbfunc' : <uintptr_t> cbfunc}
            return pypmix_settle(pmixservermodule['pushstdin'](args, pypmix_op_cbfunc, cbdata_dict), cbdata_dict)

        return PMIX_ERR_NOT_SUPPORTED
    except:
        return pypmix_settle_raise(cbdata_dict)


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
               pmix_info_cbfunc_t cbfunc, void *cbdata) noexcept with gil:
    # libpmix calls this across a C frame, so an exception
    # raised by the handler has nowhere to propagate to.
    # Without this, Cython returned -1 and left the error
    # indicator set for whatever Python code ran next in
    # this process to trip over.
    cbdata_dict = None
    try:
        keys = pmixservermodule.keys()
        if 'group' in keys:
            args = {}
            myprocs = []
            mydirs = []
            args['op'] = op
            if NULL == grp:
                return PMIX_ERR_BAD_PARAM
            args['group'] = pmix_decode_str(grp)
            if NULL != procs:
                rc = pmix_unload_procs(procs, nprocs, myprocs)
                if PMIX_SUCCESS != rc:
                    return rc
            args['procs'] = myprocs
            if NULL != directives:
                rc = pmix_unload_info(directives, ndirs, mydirs)
                if PMIX_SUCCESS != rc:
                    return rc
                args['directives'] = mydirs
            cbdata_dict = {'cbdata' : <uintptr_t> cbdata, 'cbfunc' : <uintptr_t> cbfunc}
            return pypmix_settle(pmixservermodule['group'](args, pypmix_info_cbfunc, cbdata_dict), cbdata_dict)

        return PMIX_ERR_NOT_SUPPORTED
    except:
        return pypmix_settle_raise(cbdata_dict)

cdef int fabric(const pmix_proc_t *requestor,
                pmix_fabric_operation_t op,
                const pmix_info_t directives[],
                size_t ndirs,
                pmix_info_cbfunc_t cbfunc,
                void *cbdata) noexcept with gil:
    # libpmix calls this across a C frame, so an exception
    # raised by the handler has nowhere to propagate to.
    # Without this, Cython returned -1 and left the error
    # indicator set for whatever Python code ran next in
    # this process to trip over.
    cbdata_dict = None
    try:
        keys = pmixservermodule.keys()
        if 'fabric' in keys:
            args = {}
            keyvals = {}
            myprocs = []
            mydirs = []
            args['op'] = op
            if NULL != directives:
                rc = pmix_unload_info(directives, ndirs, mydirs)
                if PMIX_SUCCESS != rc:
                    return rc
                args['directives'] = mydirs
            cbdata_dict = {'cbdata' : <uintptr_t> cbdata, 'cbfunc' : <uintptr_t> cbfunc}
            return pypmix_settle(pmixservermodule['fabric'](args, pypmix_info_cbfunc, cbdata_dict), cbdata_dict)

        return PMIX_ERR_NOT_SUPPORTED
    except:
        return pypmix_settle_raise(cbdata_dict)

cdef int toolconnected2(pmix_info_t *info, size_t ninfo,
                        pmix_tool_connection_cbfunc_t cbfunc,
                        void *cbdata) noexcept with gil:
    # libpmix calls this across a C frame, so an exception
    # raised by the handler has nowhere to propagate to.
    # Without this, Cython returned -1 and left the error
    # indicator set for whatever Python code ran next in
    # this process to trip over.
    cbdata_dict = None
    try:
        keys = pmixservermodule.keys()
        ret_proc = {'nspace': "UNDEF", 'rank': PMIX_RANK_UNDEF}
        if 'toolconnected2' in keys:
            args = {}
            ilist = []
            if NULL != info:
                rc = pmix_unload_info(info, ninfo, ilist)
                if PMIX_SUCCESS != rc:
                    return rc
                args['directives'] = ilist
            cbdata_dict = {'cbdata' : <uintptr_t> cbdata, 'cbfunc' : <uintptr_t> cbfunc}
            return pypmix_settle(pmixservermodule['toolconnected2'](args, pypmix_tool_connection_cbfunc, cbdata_dict), cbdata_dict)

        return PMIX_ERR_NOT_SUPPORTED
    except:
        return pypmix_settle_raise(cbdata_dict)

cdef int log2(const pmix_proc_t *client,
              const pmix_info_t data[], size_t ndata,
              const pmix_info_t directives[], size_t ndirs,
              pmix_op_cbfunc_t cbfunc, void *cbdata) noexcept with gil:
    # libpmix calls this across a C frame, so an exception
    # raised by the handler has nowhere to propagate to.
    # Without this, Cython returned -1 and left the error
    # indicator set for whatever Python code ran next in
    # this process to trip over.
    cbdata_dict = None
    try:
        keys = pmixservermodule.keys()
        if 'log2' in keys:
            args = {}
            ilist = []
            myproc = []
            mydirs = []
            if NULL == client:
                return PMIX_ERR_BAD_PARAM
            rc = pmix_unload_procs(client, 1, myproc)
            if PMIX_SUCCESS != rc:
                return rc
            args['source'] = myproc[0]
            if NULL != data:
                rc = pmix_unload_info(data, ndata, ilist)
                if PMIX_SUCCESS != rc:
                    return rc
                args['data'] = ilist
            if NULL != directives:
                rc = pmix_unload_info(directives, ndirs, mydirs)
                if PMIX_SUCCESS != rc:
                    return rc
                args['directives'] = mydirs
            cbdata_dict = {'cbdata' : <uintptr_t> cbdata, 'cbfunc' : <uintptr_t> cbfunc}
            return pypmix_settle(pmixservermodule['log2'](args, pypmix_op_cbfunc, cbdata_dict), cbdata_dict)

        return PMIX_ERR_NOT_SUPPORTED
    except:
        return pypmix_settle_raise(cbdata_dict)

cdef int sessioncontrol(const pmix_proc_t *requestor,
                        uint32_t sessionID,
                        const pmix_info_t directives[], size_t ndirs,
                        pmix_info_cbfunc_t cbfunc, void *cbdata) noexcept with gil:
    # libpmix calls this across a C frame, so an exception
    # raised by the handler has nowhere to propagate to.
    # Without this, Cython returned -1 and left the error
    # indicator set for whatever Python code ran next in
    # this process to trip over.
    cbdata_dict = None
    try:
        keys = pmixservermodule.keys()
        if 'sessioncontrol' in keys:
            args = {}
            myproc = []
            blist = []
            ilist = []
            barray = None

            if NULL == requestor:
                return PMIX_ERR_BAD_PARAM
            rc = pmix_unload_procs(requestor, 1, myproc)
            if PMIX_SUCCESS != rc:
                return rc
            args['requestor'] = myproc[0]
            args['sessionID'] = sessionID
            if NULL != directives:
                rc = pmix_unload_info(directives, ndirs, ilist)
                if PMIX_SUCCESS != rc:
                    return rc
                args['directives'] = ilist
            cbdata_dict = {'cbdata' : <uintptr_t> cbdata, 'cbfunc' : <uintptr_t> cbfunc}
            return pypmix_settle(pmixservermodule['sessioncontrol'](args, pypmix_info_cbfunc, cbdata_dict), cbdata_dict)

        return PMIX_ERR_NOT_SUPPORTED
    except:
        return pypmix_settle_raise(cbdata_dict)

cdef int resourceblock(const pmix_proc_t *requestor,
                       pmix_resource_block_directive_t directive,
                       const char *block,
                       const pmix_resource_unit_t *units, size_t nunits,
                       const pmix_info_t *info, size_t ninfo,
                       pmix_op_cbfunc_t cbfunc, void *cbdata) noexcept with gil:
    # libpmix calls this across a C frame, so an exception
    # raised by the handler has nowhere to propagate to.
    # Without this, Cython returned -1 and left the error
    # indicator set for whatever Python code ran next in
    # this process to trip over.
    cbdata_dict = None
    try:
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
            # a block delete carries no units, and the server passes NULL
            # for an empty list
            rc = pmix_unload_procs(requestor, 1, myproc)
            if PMIX_SUCCESS != rc:
                return rc
            args['requestor'] = myproc[0]
            args['directive'] = directive
            args['block'] = pmix_decode_str(block)
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
            return pypmix_settle(pmixservermodule['resourceblock'](args, pypmix_op_cbfunc, cbdata_dict), cbdata_dict)

        return PMIX_ERR_NOT_SUPPORTED
    except:
        return pypmix_settle_raise(cbdata_dict)


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
        cdef pmix_status_t _gilrc
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
            with nogil:
                _gilrc = PMIx_tool_init(&self.myproc, info, sz)
            rc = _gilrc
            pmix_free_info(info, sz)
        else:
            with nogil:
                _gilrc = PMIx_tool_init(&self.myproc, NULL, 0)
            rc = _gilrc
        _pmix_sync_environ()
        if PMIX_SUCCESS == rc:
            # convert the returned name
            myname = {'nspace': (<bytes>self.myproc.nspace).decode('UTF-8'), 'rank': self.myproc.rank}
        return rc, myname

    # Finalize the tool library
    def finalize(self):
        cdef pmix_status_t _gilrc
        # finalize
        with nogil:
            _gilrc = PMIx_tool_finalize()
        rc = _gilrc
        return rc

    # see if the tool is connected
    def is_connected(self):
        return PMIx_tool_is_connected()

    # Disconnect from a server
    def disconnect(self, server:dict):
        cdef pmix_status_t _gilrc
        cdef pmix_proc_t srvr

        # convert the server name
        pmix_copy_nspace(srvr.nspace, server['nspace'])
        srvr.rank = server['rank']

        # perform disconnect
        with nogil:
            _gilrc = PMIx_tool_disconnect(&srvr)
        rc = _gilrc
        return rc

    # Connect to a server
    #
    # @dicts [INPUT]
    #          - a list of dictionaries, where each
    #            dictionary has a key, value, and val_type
    #            defined as such:
    #            [{key:y, value:val, val_type:ty}, … ]
    def attach_to_server(self, dicts):
        cdef pmix_status_t _gilrc
        cdef pmix_info_t *info
        cdef pmix_info_t **info_ptr
        cdef size_t sz
        cdef pmix_proc_t srvr
        global myname

        # allocate and load pmix info structs from python list of dictionaries
        info_ptr = &info
        rc = pmix_alloc_info(info_ptr, &sz, dicts)
        if PMIX_SUCCESS != rc:
            return rc, None, None

        if sz > 0:
            with nogil:
                _gilrc = PMIx_tool_attach_to_server(&self.myproc, &srvr, info, sz)
            rc = _gilrc
            pmix_free_info(info, sz)
        else:
            with nogil:
                _gilrc = PMIx_tool_attach_to_server(&self.myproc, &srvr, NULL, 0)
            rc = _gilrc
        if PMIX_SUCCESS == rc:
            # convert the returned name
            myname = {'nspace': (<bytes>self.myproc.nspace).decode('UTF-8'), 'rank': self.myproc.rank}
            mysrvr = {'nspace': (<bytes>srvr.nspace).decode('UTF-8'), 'rank': srvr.rank}
            return PMIX_SUCCESS, myname, mysrvr
        else:
            return rc, None, None

    def get_servers(self):
        cdef pmix_status_t _gilrc
        cdef pmix_proc_t *servers
        cdef size_t nservers

        pysrvrs = []
        servers = NULL
        nservers = 0
        with nogil:
            _gilrc = PMIx_tool_get_servers(&servers, &nservers)
        rc = _gilrc
        if PMIX_SUCCESS != rc or NULL == servers:
            return rc, pysrvrs
        rc = pmix_unload_procs(servers, nservers, pysrvrs)
        # the array came from the library's allocator (PMIX_PROC_CREATE),
        # so handing it to Python's allocator was a mismatch that can abort
        # the process
        PMIx_Proc_free(servers, nservers)
        return rc, pysrvrs

    def set_server(self, server:dict, pyinfo):
        cdef pmix_status_t _gilrc
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
        with nogil:
            _gilrc = PMIx_tool_set_server(&srvr, info, ninfo)
        rc = _gilrc
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
        cdef pmix_status_t _gilrc
        # setup server module
        if map is None or 0 == len(map):
            print("SERVER REQUIRES AT LEAST ONE MODULE FUNCTION TO OPERATE")
            return PMIX_ERR_INIT
        kvkeys = list(map.keys())
        for key in kvkeys:
            mrc = setmodulefn(key, map[key])
            if PMIX_SUCCESS != mrc:
                if PMIX_ERR_BAD_PARAM == mrc:
                    print("SERVER MODULE FUNCTION ", key, " IS NOT RECOGNIZED")
                return PMIX_ERR_INIT
        self.server_module_init(kvkeys)
        # wiring the trampolines into our own struct only prepares the
        # module - the library has to be given it, or none of the
        # handlers registered above is ever called
        with nogil:
            _gilrc = PMIx_tool_set_server_module(&self.myserver)
        rc = _gilrc
        return rc


    def iof_pull(self, pyprocs, iof_channel:int, pydirs, hdlr):
        cdef pmix_proc_t *procs
        cdef pmix_info_t *directives
        cdef pmix_info_t **directives_ptr
        cdef pmix_iof_channel_t channel
        cdef size_t ndirs
        cdef size_t nprocs

        # see _on_library_thread: refuse before taking _reglock
        if _on_library_thread():
            return PMIX_ERR_WOULD_BLOCK, -1
        nprocs      = 0
        ndirs       = 0
        channel     = iof_channel
        cdef pmix_status_t pmix_rc = PMIX_ERR_EVENT_REGISTRATION

        # convert list of procs to array of pmix_proc_t's
        if pyprocs is not None:
            nprocs = len(pyprocs)
            procs = <pmix_proc_t*> PyMem_Malloc(nprocs * sizeof(pmix_proc_t))
            if not procs:
                return PMIX_ERR_NOMEM, -1
            rc = pmix_load_procs(procs, nprocs, pyprocs)
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
        if PMIX_SUCCESS != rc:
            pmix_free_procs(procs, nprocs)
            return rc, -1

        # Call the library. It can deliver output it already holds
        # before this returns - see _hdlr_find
        global _pending_iofreg
        with _reglock:
            _pending_iofreg = {'hdlr': hdlr, 'refid': None, 'codes': None}
            try:
                with nogil:
                     pmix_rc = PMIx_IOF_pull(procs, nprocs, directives, ndirs, channel,
                                             pyiofhandler,
                                             NULL, NULL)
                _hdlr_settle(myiofhdlrs, _pending_iofreg, pmix_rc)
            finally:
                _pending_iofreg = None
        rc = pmix_rc
        if 0 < nprocs:
            pmix_free_procs(procs, nprocs)
        if 0 < ndirs:
            pmix_free_info(directives, ndirs)

        # if rc < 0, then there was an error
        if 0 > rc:
            return rc, -1

        # otherwise, this is our ref ID for this hdlr
        refid = rc
        rc = PMIX_SUCCESS
        return rc, refid

    # cbfunc(status:int, cbdata) - dropping an IOF registration from
    # inside the IOF handler itself is natural, and that handler runs on
    # the progress thread
    def iof_deregister(self, regid:int, pydirs, cbfunc=None, cbdata=None):
        cdef pmix_info_t *directives
        cdef pmix_info_t **directives_ptr
        cdef size_t ndirs
        cdef size_t iofhdlr
        cdef pypmix_nb_cbdata_t *cd
        cdef pmix_status_t crc
        cdef int prc
        ndirs       = 0
        iofhdlr     = regid

        if cbfunc is not None:
            if not callable(cbfunc):
                return PMIX_ERR_BAD_PARAM
            cd = pypmix_nb_setup(pydirs, &prc)
            if NULL == cd:
                return prc
            cd.idx = pypmix_nb_register(cbfunc, cbdata)
            nbidx = cd.idx
            with nogil:
                crc = PMIx_IOF_deregister(iofhdlr, cd.info, cd.ninfo,
                                          pypmix_client_op_cbfunc, <void *> cd)
            if PMIX_SUCCESS != crc:
                if pypmix_nb_take(nbidx) is not None:
                    pypmix_nb_cbdata_free(cd)
                return crc
            # the local record goes with it, exactly as in the blocking
            # form - the library will not call our handler again
            _hdlr_drop(myiofhdlrs, regid)
            return crc

        # allocate and load pmix info structs from python list of dictionaries
        directives_ptr = &directives
        rc = pmix_alloc_info(directives_ptr, &ndirs, pydirs)
        if PMIX_SUCCESS != rc:
            return rc

        # call the library - without the GIL, as in deregister_event_handler
        with nogil:
            crc = PMIx_IOF_deregister(iofhdlr, directives, ndirs, NULL, NULL)
        rc = crc
        if 0 < ndirs:
            pmix_free_info(directives, ndirs)
        # remove our local hdlr
        _hdlr_drop(myiofhdlrs, regid)
        return rc

    # cbfunc(status:int, cbdata) - see deregister_nspace
    def iof_push(self, pytargets, data:dict, pydirs, cbfunc=None, cbdata=None):
        cdef pmix_status_t _gilrc
        cdef size_t cntargets
        cdef pmix_info_t *directives
        cdef pmix_info_t **directives_ptr
        cdef pmix_byte_object_t *bo
        cdef pmix_byte_object_t pushbo
        cdef size_t ndirs
        cdef pmix_proc_t *targets
        cdef pypmix_nb_cbdata_t *cd
        cdef pmix_status_t crc
        cdef int prc
        ntargets    = 0
        ndirs       = 0

        if cbfunc is not None:
            if not callable(cbfunc):
                return PMIX_ERR_BAD_PARAM
            # targets, payload and directives are all held by the library
            # until the push completes
            cd = pypmix_nb_setup(None, &prc)
            if NULL == cd:
                return prc
            prc = pypmix_nb_add_procs(cd, pytargets, self.myproc.nspace)
            if PMIX_SUCCESS != prc:
                pypmix_nb_cbdata_free(cd)
                return prc
            prc = pypmix_nb_add_bo(cd, data)
            if PMIX_SUCCESS != prc:
                pypmix_nb_cbdata_free(cd)
                return prc
            prc = pypmix_nb_add_dirs(cd, pydirs)
            if PMIX_SUCCESS != prc:
                pypmix_nb_cbdata_free(cd)
                return prc
            cd.idx = pypmix_nb_register(cbfunc, cbdata)
            nbidx = cd.idx
            with nogil:
                crc = PMIx_IOF_push(cd.procs, cd.nprocs, cd.bo,
                                    cd.dirs, cd.ndirs,
                                    pypmix_client_op_cbfunc, <void *> cd)
            if PMIX_SUCCESS != crc:
                if pypmix_nb_take(nbidx) is not None:
                    pypmix_nb_cbdata_free(cd)
            return crc

        # convert data to pmix_byte_object_t. The struct lives on the
        # stack - the heap copy the old code made was never released, and
        # bytes(x, 'ascii') raised on a payload that already was bytes
        bo = NULL
        if data:
            rc = pmix_load_bo(&pushbo, data)
            if PMIX_SUCCESS != rc:
                return rc
            bo = &pushbo

        # convert list of proc targets to array of pmix_proc_t's. Every
        # failure from here on has the payload to give back
        if pytargets is not None and 0 < len(pytargets):
            ntargets = len(pytargets)
            targets = <pmix_proc_t*>PyMem_Malloc(ntargets * sizeof(pmix_proc_t))
            if not targets:
                if NULL != bo:
                    free(bo.bytes)
                return PMIX_ERR_NOMEM
            rc = pmix_load_procs(targets, ntargets, pytargets)
            if PMIX_SUCCESS != rc:
                pmix_free_procs(targets, ntargets)
                if NULL != bo:
                    free(bo.bytes)
                return rc
        else:
            ntargets = 1
            targets = <pmix_proc_t*>PyMem_Malloc(ntargets * sizeof(pmix_proc_t))
            if not targets:
                if NULL != bo:
                    free(bo.bytes)
                return PMIX_ERR_NOMEM
            pmix_copy_nspace(targets[0].nspace, self.myproc.nspace)
            targets[0].rank = PMIX_RANK_WILDCARD

        # allocate and load pmix info structs from python list of dictionaries
        directives_ptr = &directives
        rc = pmix_alloc_info(directives_ptr, &ndirs, pydirs)
        if PMIX_SUCCESS != rc:
            pmix_free_procs(targets, ntargets)
            if NULL != bo:
                free(bo.bytes)
            return rc

        # Call the library
        cntargets = ntargets
        with nogil:
            _gilrc = PMIx_IOF_push(targets, cntargets, bo, directives, ndirs, NULL, NULL)
        rc = _gilrc
        if NULL != bo and NULL != bo.bytes:
            free(bo.bytes)
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
#     module key, while the scheduler's own directives to a session -
#     including assigning a session to the RTE - go out through the
#     inherited session_control method (PMIx_Session_control). There is
#     no separate session-assignment method, and none should be added.
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
        cdef pmix_status_t _gilrc
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
            with nogil:
                _gilrc = PMIx_tool_init(&self.myproc, info, sz)
            rc = _gilrc
            pmix_free_info(info, sz)
        else:
            with nogil:
                _gilrc = PMIx_tool_init(&self.myproc, NULL, 0)
            rc = _gilrc
        _pmix_sync_environ()
        if PMIX_SUCCESS == rc:
            # convert the returned name
            myname = {'nspace': (<bytes>self.myproc.nspace).decode('UTF-8'), 'rank': self.myproc.rank}
        return rc, myname

    # Finalize the tool library
    def finalize(self):
        cdef pmix_status_t _gilrc
        # finalize
        with nogil:
            _gilrc = PMIx_tool_finalize()
        rc = _gilrc
        return rc



# The documented way to use these bindings is "from pmix import *", which
# without an __all__ hands the caller every name at module scope - and
# that includes the modules this file imports for its own use.  A program
# that did
#
#     import time
#     from pmix import *
#
# silently got the extension's "time" instead of its own, and the same for
# os, sys, array, queue, signal, threading, ctypes and traceback.  Rebinding
# a caller's imports is not something a library may do.
#
# Build the export list at import time rather than maintaining it by hand:
# the whole point of the module is the several thousand PMIX_* constants
# construct.py generates, and enumerating those would go stale on the first
# new attribute.  So export everything public except the modules and the
# handful of symbols pulled in by "from X import Y".
def _pmix_build_all():
    import types as _types
    borrowed = ('Callable', 'Timer', 'addressof', 'c_int', 'address')
    names = []
    for name, obj in globals().items():
        if name.startswith('_'):
            continue
        if name in borrowed:
            continue
        if isinstance(obj, _types.ModuleType):
            continue
        names.append(name)
    return sorted(names)

__all__ = _pmix_build_all()
