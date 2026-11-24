Non-Blocking Operations in the Python Bindings
==============================================

This document describes how the PMIx Python bindings drive a
non-blocking (``_nb``) PMIx API: what has to stay alive while the
request is in flight, how the caller's Python callback is reached from
the library's progress thread, and which rules a new ``_nb`` binding has
to follow. The machinery lives in
``bindings/python/pmix.pyx``; the conversion helpers it leans on are in
``bindings/python/pmix.pxi``.

The non-blocking group operations - ``group_construct_nb``,
``group_invite_nb``, ``group_join_nb``, ``group_leave_nb`` and
``group_destruct_nb`` - were the first bindings built on it, and are
used as the worked example throughout. Every other ``_nb`` entry point
is now bound on the same machinery; they are tabulated with their
callback signatures under `The bound non-blocking APIs`_ below.


The problem
-----------

A blocking binding has an easy life. It converts its Python arguments
into C, calls the library, and by the time the call returns the library
is finished with everything it was handed, so the method frees it all
before returning a status::

    rc = PMIx_Group_construct(pygrp, procs, nprocs, info, ninfo,
                              &results, &nresults)
    if 0 < nprocs:
        pmix_free_procs(procs, nprocs)
    if 0 < ninfo:
        pmix_free_info(info, ninfo)

None of that holds for a non-blocking call. It returns as soon as the
request has been handed to the library, and reports the outcome later by
executing a callback on the library's internal progress thread. Two
distinct problems follow.

**PMIx does not copy its input.** The rule stated in the top-level
developer guide - callers keep their input valid until the callback
fires - applies to the bindings exactly as it does to a C program.
Freeing the ``pmix_info_t`` array when the method returns would be a
use-after-free. The group identifier is worse: the blocking methods
write ::

    pygrp = group.encode('ascii')

which produces a Python ``bytes`` object whose lifetime is the local
variable. Pass its buffer to a non-blocking API and the pointer dangles
the moment the method returns.

**Nothing holds the caller's callback.** The Python function and the
``cbdata`` object the caller wants handed back exist only as arguments
to the method. Once it returns, the interpreter is free to collect
them, and there is no C-visible reference keeping them alive - the
bindings never take a reference on a Python object, by long-standing
convention.


The caddy
---------

Everything of the first kind goes into a small C struct - a *caddy*, in
the same sense the term is used elsewhere in PMIx - that outlives the
method::

    cdef struct nbcbd:
        char *grp             # strdup'd group identifier
        char *ky              # strdup'd key (get_nb)
        char *blk             # strdup'd resource block name
        char **keys           # NULL-terminated argv (lookup_nb, unpublish_nb)
        pmix_proc_t *procs    # PyMem_Malloc'd by pmix_load_procs
        size_t nprocs
        pmix_info_t *info     # malloc'd by pmix_alloc_info
        size_t ninfo
        pmix_info_t *dirs     # the second info array, where an operation
        size_t ndirs          #   carries one (log, job control, monitor)
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

One struct covers every operation rather than one per API, because the
trampolines reach the registry through ``cd.idx`` and must be able to
cast any caddy to the same type. ``pypmix_nb_cbdata_new`` zeroes it, so
an operation simply leaves unused fields alone and
``pypmix_nb_cbdata_free`` skips them.

The caddy is passed to the library as the operation's ``cbdata``, so it
comes back to the trampoline unchanged and can be released there.

Note the comments on the allocators. The buffers come from several
different places and each must be returned to its own:
``pypmix_nb_cbdata_free`` calls ``free`` on the strings and the info
arrays, ``pmix_free_argv`` on the key arrays (whose entries are
``strdup``'d), and ``pmix_free_procs`` / ``pmix_free_units`` (which are
``PyMem_Free``) on the arrays the bindings both build and release.
Crossing them is not a stylistic matter - Python's allocator serves small
blocks from its own arenas, and handing one of those to ``free()`` aborts
the process.

``group_join_nb`` takes a single ``const pmix_proc_t *leader`` rather
than an array, and ``get_nb`` a single ``const pmix_proc_t *proc``. Both
store it as a one-element ``procs`` array and pass ``&cd.procs[0]``, so
one set of fields covers every operation.

Three helpers fill the caddy for the shapes that recur:
``pypmix_nb_setup`` builds it and takes ownership of the primary info
array, ``pypmix_nb_add_procs`` parks the target procs (defaulting, as the
blocking forms do, to the caller's entire job), and
``pypmix_nb_add_keys`` parks a key array. Anything more specific -
apps, queries, a credential, a cpuset - is built by the method itself.

Two operations hand the library a pointer *into the Python object*:
``fabric_register_nb`` and friends pass ``&self.myfabric``, and
``compute_distances_nb`` passes ``&self.topo``. Those would dangle if the
interpreter collected the object while the request was in flight, so
those methods pass ``self`` to ``pypmix_nb_register`` as a third
argument. Nothing ever reads it; holding the reference is the whole
point.


The registry
------------

The caller's Python callback and ``cbdata`` cannot travel through the
caddy - a ``PyObject *`` in a C struct is invisible to the interpreter,
so nothing would stop it being collected. Instead they are held in a
module-global dictionary, which is how the bindings already keep event
handlers (``myhdlrs``) and server-module functions (``pmixservermodule``)
alive::

    pynbcbs  = {}                 # idx -> {'cbfunc': callable, 'cbdata': object}
    pynbidx  = 0
    pynblock = threading.Lock()

``pypmix_nb_register`` adds an entry and returns its integer key, which
is what the caddy carries. ``pypmix_nb_take`` removes and returns an
entry. The lock matters: entries are added by whichever thread called
the method and removed by the progress thread.

**The registry entry is the ownership token for the caddy.** Whoever
takes the entry is responsible for releasing the caddy, and a take that
comes up empty means someone else already has. Without that rule the
trampoline and the method's error path could both decide to free the
same caddy.


The trampolines
---------------

There is one C function per callback signature the bound APIs use. Two
of them cover the group operations::

    cdef void pypmix_client_op_cbfunc(pmix_status_t status,
                                      void *cbdata) noexcept with gil

    cdef void pypmix_client_info_cbfunc(pmix_status_t status,
                                        pmix_info_t *info, size_t ninfo,
                                        void *cbdata,
                                        pmix_release_cbfunc_t release_fn,
                                        void *release_cbdata) noexcept with gil

``with gil`` is mandatory and is the whole reason these cannot be
ordinary functions. They run on the library's progress thread, which
Python knows nothing about; the declaration makes Cython acquire the GIL
(and register the thread) around the body. ``noexcept`` pairs with the
``try``/``except`` inside: an exception raised by the user's callback is
printed and swallowed, because there is no sane way to propagate one
into libpmix.

The ``pmix_info_cbfunc_t`` form carries an extra ``release_fn`` /
``release_cbdata`` pair. Those results belong to the library, so the
trampoline converts them to Python first and then calls ``release_fn``
to give them back - the same sequence ``collectinventory_cbfunc`` uses.

The Python-facing signatures are::

    cbfunc(status, results, cbdata)   # construct, invite, join
    cbfunc(status, cbdata)            # leave, destruct

where ``results`` is a list of info dictionaries and ``cbdata`` is
whatever object the caller passed in, returned untouched.

The other bound operations need six more, one per remaining C callback
type:

.. list-table::
   :header-rows: 1

   * - C callback type
     - Trampoline
     - Python signature
   * - ``pmix_value_cbfunc_t``
     - ``pypmix_client_value_cbfunc``
     - ``cbfunc(status, value, cbdata)``
   * - ``pmix_lookup_cbfunc_t``
     - ``pypmix_client_lookup_cbfunc``
     - ``cbfunc(status, pdata, cbdata)``
   * - ``pmix_spawn_cbfunc_t``
     - ``pypmix_client_spawn_cbfunc``
     - ``cbfunc(status, nspace, cbdata)``
   * - ``pmix_credential_cbfunc_t``
     - ``pypmix_client_credential_cbfunc``
     - ``cbfunc(status, credential, results, cbdata)``
   * - ``pmix_validation_cbfunc_t``
     - ``pypmix_client_validation_cbfunc``
     - ``cbfunc(status, results, cbdata)``
   * - ``pmix_device_dist_cbfunc_t``
     - ``pypmix_client_devdist_cbfunc``
     - ``cbfunc(status, distances, cbdata)``

All six follow the same shape, and nearly all of what they are handed
follows one rule that is easy to get wrong: **what the library hands a
callback belongs to the library**, with the two exceptions below. Only ``pmix_info_cbfunc_t`` and ``pmix_device_dist_cbfunc_t``
carry an explicit ``release_fn``; for the rest the library reclaims the
data the moment the callback returns. So each trampoline converts to
Python *first* - a ``pmix_value_t`` through ``pmix_unload_value``, a
``pmix_nspace_t`` through ``decode``, a credential through
``pmix_unload_bytes`` - and never stores the C pointer it was given.

There are two exceptions, and each is a transfer the C API documents:

- **The value a** ``pmix_value_cbfunc_t`` **is handed.** ``PMIx_Get_nb``
  gives the ``pmix_value_t`` to its receiver, which must release it (see
  :ref:`PMIx_Get(3) <man3-PMIx_Get>`). So ``pypmix_client_value_cbfunc``
  releases it with ``PMIx_Value_free`` once it is converted; taking it for
  the library's, as the trampoline once did, leaked one value per
  ``get_nb``. The only value the library keeps is the one it answers a
  ``PMIX_GET_POINTER_VALUES`` request with, a pointer into its own store,
  and ``get_nb`` refuses that directive with ``PMIX_ERR_NOT_SUPPORTED``
  exactly as ``get`` does.
- **The credential's payload.** A ``pmix_credential_cbfunc_t`` transfers
  the bytes to whoever receives the callback, so
  ``pypmix_client_credential_cbfunc`` frees them once it has copied them;
  the byte object carrying them, and the info array, stay the library's.
  The blocking ``get_credential`` does the same.

Check the C man page of the callback type before writing a trampoline:
the ownership of what it is handed is stated there, and it is not the
same for every type.

The conversion can fail - a value of a type the bindings cannot convert,
a string that does not decode - and when it does it must not end the
trampoline: the caller's callback would never run, the caddy would leak,
and for the two forms with a ``release_fn`` the library's data would
never be given back. Every trampoline therefore catches its conversion,
turns a failure into an error status for the callback, and goes on to
release, reclaim and call as usual. ``pypmix_client_finish`` is the tail
they share.


Putting it together
-------------------

A binding then reads::

    def group_leave_nb(self, group, pyinfo, cbfunc, cbdata=None):
        if not callable(cbfunc):
            return PMIX_ERR_BAD_PARAM
        pygrp = group.encode('ascii')
        cd = pypmix_nb_group_setup(pygrp, pyinfo, &prc)
        if NULL == cd:
            return prc
        cd.idx = pypmix_nb_register(cbfunc, cbdata)
        nbidx = cd.idx
        with nogil:
            rc = PMIx_Group_leave_nb(cd.grp, cd.info, cd.ninfo,
                                     pypmix_client_op_cbfunc, <void *> cd)
        if PMIX_SUCCESS != rc:
            if pypmix_nb_take(nbidx) is not None:
                pypmix_nb_cbdata_free(cd)
        return rc

Five details in that are load-bearing.

*Every argument is converted before the caddy is built, or by a helper
that reports failure as a status.* An exception raised after
``pypmix_nb_setup`` leaves the method without freeing the caddy, or the
info array already copied into it.

*The callback is validated first.* A callback that cannot be executed
would leave the caddy and the registry entry stranded for the life of
the process, so a non-callable is rejected before anything is
allocated.

*The library call releases the GIL.* Inside ``with nogil`` the progress
thread is free to run - including running the trampoline for this very
operation, which is why the registration happens before the call and not
after.

*The error path cleans up.* When a PMIx ``_nb`` API returns anything
other than ``PMIX_SUCCESS`` it has not accepted the request and will
never execute the callback, so nothing else will ever free the caddy.
The method must do it, and it does so through the registry, honoring the
ownership rule above - by the id it copied before the call, since if the
callback did run, ``cd`` is already gone.

*The method returns a bare status.* Results arrive through the callback,
so unlike the blocking forms these return ``rc`` alone rather than a
tuple.


Rules for callers
-----------------

A callback runs on the progress thread. It must not call a blocking
PMIx operation - that is the same deadlock a C program would hit, and
the reason ``PMIx_Group_join_nb`` exists at all: an invitation is
delivered in an event handler, where the blocking ``PMIx_Group_join``
cannot be used. Record the result and wake another thread, as
``test/python/client.py`` does with a ``threading.Event``. A blocking
method called there answers ``PMIX_ERR_WOULD_BLOCK`` - including the
server methods that wait in Python rather than in the library
(``setup_application``, ``dmodex_request``, ``dmodex_request2``,
``collect_inventory``), which used to wait there for good.

A callback is executed if and only if the method returned
``PMIX_SUCCESS``. A method that answers ``PMIX_OPERATION_SUCCEEDED`` has
completed the operation on the spot - the fabric calls do, see below - and
runs no callback, exactly as the C API does.

The *blocking* forms release the GIL as well, for the same reason the
``_nb`` forms do: while one waits for the progress thread, that thread
may need to run Python - an event handler, an ``_nb`` callback, a
server-module upcall - and it cannot while the caller holds the GIL. A
blocking method that kept it deadlocked against anything of the kind
that became due during the wait. ``test/python/client.py`` checks this
with an event handler that takes the GIL and then gives it up while it
sleeps, during which the main thread makes a blocking ``put()``.


State that lives in the class
-----------------------------

The fabric calls have a wrinkle the others do not: the class tracks
whether a fabric is currently registered, and that flag must flip when
the *library* says the operation finished - not when the request was
accepted. The trampoline knows nothing about the class, so these methods
register a small closure in place of the caller's callback, which also
reads the fabric information the library filled into the class's fabric
object and hands it to the caller::

    def regdone(status, ud):
        finfo = []
        self.fabric_busy = 0
        if PMIX_SUCCESS == status:
            self.fabric_set = 1
            finfo = self._fabric_info()
            if finfo is None:
                # the fabric information could not be converted
                status = PMIX_ERR_NOT_SUPPORTED
                finfo = []
        cbfunc(status, finfo, ud)

The closure captures ``self`` and the caller's callback, runs on the
progress thread like any other, and invokes the caller's function.
Anything else that has to update class state from a completion should
do the same rather than teach the trampolines about it.

The library may also complete a fabric operation on the spot: a
scheduler registers a fabric itself, and a deregistration never leaves
the process. It then answers ``PMIX_OPERATION_SUCCEEDED`` and runs no
callback, so the method updates the flag itself before returning that -
otherwise the class would go on believing a deregistered fabric was still
registered, and refuse every later registration as busy. The fabric
information a registration completed that way produced is read with
``fabric_info()``.

Only one fabric request is outstanding at a time. The library writes the
class's fabric object from the progress thread when a request completes,
so another request - or ``fabric_info()`` - while one is in flight answers
``PMIX_ERR_RESOURCE_BUSY``. The method claims the fabric in the statement
after it tests it, before converting anything: a conversion runs Python
code, and another thread that ran then would pass the same test.


The bound non-blocking APIs
---------------------------

Every non-blocking PMIx entry point is bound. Each returns only the
integer status of the *request*; the result arrives later by executing
the caller's callback on the progress thread. The trailing ``cbdata``
argument is optional and is handed back to the callback unmodified.

.. list-table::
   :header-rows: 1

   * - C API
     - Python method
     - Callback signature
   * - ``PMIx_Fence_nb``
     - ``fence_nb(peers, dicts, cbfunc, cbdata=None)``
     - ``(status, cbdata)``
   * - ``PMIx_Get_nb``
     - ``get_nb(proc, ky, dicts, cbfunc, cbdata=None)``
     - ``(status, value, cbdata)``
   * - ``PMIx_Publish_nb``
     - ``publish_nb(dicts, cbfunc, cbdata=None)``
     - ``(status, cbdata)``
   * - ``PMIx_Lookup_nb``
     - ``lookup_nb(pykeys, dicts, cbfunc, cbdata=None)``
     - ``(status, pdata, cbdata)``
   * - ``PMIx_Unpublish_nb``
     - ``unpublish_nb(pykeys, dicts, cbfunc, cbdata=None)``
     - ``(status, cbdata)``
   * - ``PMIx_Spawn_nb``
     - ``spawn_nb(jobInfo, pyapps, cbfunc, cbdata=None)``
     - ``(status, nspace, cbdata)``
   * - ``PMIx_Connect_nb``
     - ``connect_nb(peers, pyinfo, cbfunc, cbdata=None)``
     - ``(status, cbdata)``
   * - ``PMIx_Disconnect_nb``
     - ``disconnect_nb(peers, pyinfo, cbfunc, cbdata=None)``
     - ``(status, cbdata)``
   * - ``PMIx_Query_info_nb``
     - ``query_nb(pyq, cbfunc, cbdata=None)``
     - ``(status, results, cbdata)``
   * - ``PMIx_Log_nb``
     - ``log_nb(pydata, pydirs, cbfunc, cbdata=None)``
     - ``(status, cbdata)``
   * - ``PMIx_Allocation_request_nb``
     - ``allocation_request_nb(directive, pyinfo, cbfunc, cbdata=None)``
     - ``(status, results, cbdata)``
   * - ``PMIx_Job_control_nb``
     - ``job_control_nb(pytargets, pydirs, cbfunc, cbdata=None)``
     - ``(status, results, cbdata)``
   * - ``PMIx_Process_monitor_nb``
     - ``monitor_nb(pymonitor_info, code, pydirs, cbfunc, cbdata=None)``
     - ``(status, results, cbdata)``
   * - ``PMIx_Get_credential_nb``
     - ``get_credential_nb(pyinfo, cbfunc, cbdata=None)``
     - ``(status, credential, results, cbdata)``
   * - ``PMIx_Validate_credential_nb``
     - ``validate_credential_nb(pycred, pyinfo, cbfunc, cbdata=None)``
     - ``(status, results, cbdata)``
   * - ``PMIx_Fabric_register_nb``
     - ``fabric_register_nb(dicts, cbfunc, cbdata=None)``
     - ``(status, fabricinfo, cbdata)``
   * - ``PMIx_Fabric_update_nb``
     - ``fabric_update_nb(cbfunc, cbdata=None)``
     - ``(status, fabricinfo, cbdata)``
   * - ``PMIx_Fabric_deregister_nb``
     - ``fabric_deregister_nb(cbfunc, cbdata=None)``
     - ``(status, cbdata)``
   * - ``PMIx_Compute_distances_nb``
     - ``compute_distances_nb(pycpus, dicts, cbfunc, cbdata=None)``
     - ``(status, distances, cbdata)``
   * - ``PMIx_Resource_block_nb``
     - ``resource_block_nb(directive, block, pyunits, pyinfo, cbfunc, cbdata=None)``
     - ``(status, cbdata)``
   * - ``PMIx_Group_construct_nb``
     - ``group_construct_nb(group, peers, pyinfo, cbfunc, cbdata=None)``
     - ``(status, results, cbdata)``
   * - ``PMIx_Group_invite_nb``
     - ``group_invite_nb(group, peers, pyinfo, cbfunc, cbdata=None)``
     - ``(status, results, cbdata)``
   * - ``PMIx_Group_join_nb``
     - ``group_join_nb(group, leader, opt, pyinfo, cbfunc, cbdata=None)``
     - ``(status, results, cbdata)``
   * - ``PMIx_Group_leave_nb``
     - ``group_leave_nb(group, pyinfo, cbfunc, cbdata=None)``
     - ``(status, cbdata)``
   * - ``PMIx_Group_destruct_nb``
     - ``group_destruct_nb(group, pyinfo, cbfunc, cbdata=None)``
     - ``(status, cbdata)``

One signature differs from its blocking counterpart on purpose:
``lookup_nb`` takes a list of key strings, not the ``pmix_pdata_t`` dict
list ``lookup`` takes, because that is what the C API accepts and there
is nothing for the caller to fill in on input.


Adding another non-blocking binding
-----------------------------------

The machinery is not specific to groups. To bind another ``_nb`` API:

#. Confirm the C prototype is already in the generated
   ``pmix_constants.pxd`` - it almost certainly is, since
   ``construct.py`` harvests every public API. Do not hand-edit that
   file.
#. Park every input the library will hold - and anything derived from a
   Python object, which dies with the method - in a caddy. Add fields to
   ``nbcbd`` if the operation carries arguments the group operations do
   not, and extend ``pypmix_nb_cbdata_free`` to match.
#. Reuse one of the eight existing trampolines. Between them they cover
   every callback type the public APIs use, so a new binding should not
   need a new one; if it does, write it to the same shape - convert what
   the library owns *first*, take the registry entry, free the caddy,
   then call into Python inside a ``try``/``except``.
#. Follow the error-path rule exactly. It is the easiest part to get
   wrong and the hardest to notice, because the leak only shows up when
   the library refuses a request.

``test/python/test_bindings.py`` shows how to cover the result without a
server: calling the method before ``init()`` returns ``PMIX_ERR_INIT``,
which drives the whole marshaling path and then the error-path cleanup,
and the test asserts that no callback fired and that the registry is
empty afterwards.


Related reading
---------------

* ``bindings/python/AGENTS.md`` - conventions for the bindings as a
  whole, including the data conversion model and the upcall
  trampolines that run in the opposite direction.
* The *Thread Safety and the Progress Thread* section of the top-level
  developer guide - the C-side contract these bindings are honoring.
