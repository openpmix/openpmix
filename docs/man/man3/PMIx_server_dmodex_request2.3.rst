.. _man3-PMIx_server_dmodex_request2:

PMIx_server_dmodex_request2
===========================

.. include_body

``PMIx_server_dmodex_request2`` |mdash| Request locally posted modex data
for a specified process on behalf of a remote requester.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix_server.h>

   pmix_status_t PMIx_server_dmodex_request2(const pmix_proc_t *proc,
                                             const pmix_info_t info[], size_t ninfo,
                                             pmix_dmodex_response_fn_t cbfunc,
                                             void *cbdata);


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

  from pmix import *

  foo = PMIxServer()
  # ... after a successful foo.init() ...
  proc = {'nspace': "myapp", 'rank': 3}
  # the requester, as the requesting server's direct_modex up-call named it
  info = [{'key': PMIX_USERID, 'value': 1000, 'val_type': PMIX_UINT32},
          {'key': PMIX_GRPID, 'value': 1000, 'val_type': PMIX_UINT32},
          {'key': PMIX_REQUESTOR, 'value': {'nspace': "otherjob", 'rank': 0},
           'val_type': PMIX_PROC}]
  rc, (data, sz) = foo.dmodex_request2(proc, info)
  # rc is the status delivered to the callback


INPUT PARAMETERS
----------------

* ``proc``: Pointer to a :ref:`pmix_proc_t(5) <man5-pmix_proc_t>` identifying
  the process whose posted data is being requested. A rank of
  ``PMIX_RANK_WILDCARD`` requests the job-level information for the namespace
  rather than the data posted by a specific process. Must not be ``NULL``.
* ``info``: Array of :ref:`pmix_info_t(5) <man5-pmix_info_t>` directives, or
  ``NULL``. It must remain valid until ``cbfunc`` is called.
* ``ninfo``: Number of elements in ``info``.
* ``cbfunc``: Callback function of type :ref:`pmix_dmodex_response_fn_t <man5-pmix_dmodex_response_fn_t>` that is
  invoked with the requested data once it becomes available, or with the
  reason it will not be returned. Must not be ``NULL``.
* ``cbdata``: Opaque pointer that is passed, unmodified, to ``cbfunc``.


DIRECTIVES
----------

The host names the process the request is being made for, as the
requesting server named it in its ``direct_modex`` up-call (see
:ref:`pmix_server_module_t(5) <man5-pmix_server_module_t>`):

* ``PMIX_USERID`` (``uint32_t``) |mdash| the requester's user ID.
* ``PMIX_GRPID`` (``uint32_t``) |mdash| the requester's group ID.
* ``PMIX_REQUESTOR`` (:ref:`pmix_proc_t(5) <man5-pmix_proc_t>`) |mdash| the
  requesting process.


DESCRIPTION
-----------

``PMIx_server_dmodex_request2`` is
:ref:`PMIx_server_dmodex_request(3) <man3-PMIx_server_dmodex_request>` with
directives. When they name a requester, the library returns the data only if
that requester may access the target job - it is one of the job's own
processes, is root or runs as the server's own user, is the job's owner, or
is named by user or group in the job's access list - and otherwise completes
the request with ``PMIX_ERR_NO_PERMISSIONS``. A request that names no
requester is answered as ``PMIx_server_dmodex_request`` answers it.

The requesting server remembers which requesters the holding server
approved, answers only them from the copy it keeps, and asks again for any
other. See :doc:`/security-plan`.

A host can tell whether the library provides this function from the
``PMIX_CAP_DMODEX_REQUEST2`` capability flag in ``pmix_version.h``.

The request is **non-blocking**, is deferred when the data is not yet
available, and delivers its result through ``cbfunc`` exactly as
``PMIx_server_dmodex_request`` does; see that page for the callback and the
ownership of the returned blob.


RETURN VALUE
------------

A return of ``PMIX_SUCCESS`` indicates only that the request was accepted for
processing; the final status and any data are delivered through ``cbfunc``.
Possible immediate return values include:

* ``PMIX_SUCCESS`` |mdash| the request was accepted for processing.
* ``PMIX_ERR_BAD_PARAM`` |mdash| ``proc`` or ``cbfunc`` was ``NULL``, or
  ``info`` was ``NULL`` with a non-zero ``ninfo``.
* ``PMIX_ERR_NOT_AVAILABLE`` |mdash| the operation cannot be serviced because
  the library's progress engine has been stopped.
* ``PMIX_ERR_INIT`` |mdash| the PMIx server library has not been initialized.

When an error is returned immediately, ``cbfunc`` is **not** called. The
status delivered to ``cbfunc`` may additionally be:

* ``PMIX_ERR_NO_PERMISSIONS`` |mdash| the named requester may not access the
  target job's data.
* ``PMIX_ERR_BAD_PARAM`` |mdash| a directive naming the requester had the
  wrong type.


.. seealso::
   :ref:`PMIx_server_dmodex_request(3) <man3-PMIx_server_dmodex_request>`,
   :ref:`PMIx_server_init(3) <man3-PMIx_server_init>`,
   :ref:`pmix_server_module_t(5) <man5-pmix_server_module_t>`,
   :ref:`PMIx_Get(3) <man3-PMIx_Get>`,
   :ref:`pmix_proc_t(5) <man5-pmix_proc_t>`,
   :ref:`pmix_status_t(5) <man5-pmix_status_t>`
