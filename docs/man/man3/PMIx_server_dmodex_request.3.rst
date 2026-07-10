.. _man3-PMIx_server_dmodex_request:

PMIx_server_dmodex_request
==========================

.. include_body

``PMIx_server_dmodex_request`` |mdash| Request locally posted modex data
for a specified process on behalf of a remote peer.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix_server.h>

   pmix_status_t PMIx_server_dmodex_request(const pmix_proc_t *proc,
                                            pmix_dmodex_response_fn_t cbfunc,
                                            void *cbdata);


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

  from pmix import *

  foo = PMIxServer()
  # ... after a successful foo.init() ...
  # proc identifies the process whose posted data is being requested
  proc = {'nspace': "myapp", 'rank': 3}
  rc, (data, sz) = foo.dmodex_request(proc, {})
  # data/sz carry the returned blob when rc is PMIX_SUCCESS


INPUT PARAMETERS
----------------

* ``proc``: Pointer to a :ref:`pmix_proc_t(5) <man5-pmix_proc_t>` identifying
  the process whose posted data is being requested. A rank of
  ``PMIX_RANK_WILDCARD`` requests the job-level information for the namespace
  rather than the data posted by a specific process. Must not be ``NULL``.
* ``cbfunc``: Callback function of type :ref:`pmix_dmodex_response_fn_t <man5-pmix_dmodex_response_fn_t>` that is
  invoked with the requested data once it becomes available. Must not be
  ``NULL``.
* ``cbdata``: Opaque pointer that is passed, unmodified, to ``cbfunc``.


DESCRIPTION
-----------

Request that the local PMIx server library return the modex data (endpoint
and other connection information posted via :ref:`PMIx_Put(3) <man3-PMIx_Put>`
and committed via :ref:`PMIx_Commit(3) <man3-PMIx_Commit>`) for a specified
process on behalf of a remote peer.

PMIx supports an alternative wireup method known as *Direct Modex* that
replaces the barrier-based global exchange of all process-posted information
(the :ref:`PMIx_Fence(3) <man3-PMIx_Fence>` operation) with an on-demand fetch
of a peer's data. Data posted by each process is cached on its local PMIx
server. When a process requests information posted by a peer that is not in its
local cache, the request is passed to the local PMIx server, which asks its
host resource manager (RM) to obtain the data from the RM daemon on the node
where the target process resides. Upon receiving such a request, that RM daemon
passes it into its own PMIx server library via ``PMIx_server_dmodex_request``.

``PMIx_server_dmodex_request`` is a **non-blocking** operation: it thread-shifts
the request into the library's progress thread and returns immediately. The
provided ``cbfunc`` is invoked once the target process has posted its
information |mdash| which may be some time after the call returns, since the
data may not yet be available when the request arrives. Requests that cannot be
satisfied immediately (because the namespace or rank is not yet known, or the
process has not yet committed its data) are deferred internally and completed
when the data arrives.


CALLBACK FUNCTION
-----------------

The ``cbfunc`` has the signature ``pmix_dmodex_response_fn_t``:

.. code-block:: c

   typedef void (*pmix_dmodex_response_fn_t)(pmix_status_t status,
                                             char *data, size_t sz,
                                             void *cbdata);

When the requested information becomes available, the library invokes
``cbfunc`` with:

* ``status`` |mdash| ``PMIX_SUCCESS`` if the data was successfully collected,
  or an error constant describing why it could not be returned.
* ``data`` |mdash| pointer to a data blob containing the requested information,
  or ``NULL`` if none is available.
* ``sz`` |mdash| number of bytes in the ``data`` blob.
* ``cbdata`` |mdash| the opaque pointer passed to
  ``PMIx_server_dmodex_request``.

The ``data`` blob is owned by the PMIx server library and is **freed upon
return** from ``cbfunc``. The host RM must therefore copy, transmit, or
otherwise consume the blob before the callback returns; it must not retain the
pointer afterward.


RETURN VALUE
------------

A return of ``PMIX_SUCCESS`` indicates only that the request was accepted for
processing; the final status and any data are delivered through ``cbfunc``.
Possible immediate return values include:

* ``PMIX_SUCCESS`` |mdash| the request was accepted for processing.
* ``PMIX_ERR_BAD_PARAM`` |mdash| ``proc`` or ``cbfunc`` was ``NULL``.
* ``PMIX_ERR_NOT_AVAILABLE`` |mdash| the operation cannot be serviced because
  the library's progress engine has been stopped.
* ``PMIX_ERR_INIT`` |mdash| the PMIx server library has not been initialized.

When an error is returned immediately, ``cbfunc`` is **not** called. Any other
negative value indicates an appropriate error condition. PMIx error constants
are defined in ``pmix_common.h``.


NOTES
-----

This function is available only after the PMIx server library has been
initialized with :ref:`PMIx_server_init(3) <man3-PMIx_server_init>`. It is
intended for use by a host RM daemon acting on behalf of a remote peer; the
data returned reflects the information posted by the library's own local
clients.


.. seealso::
   :ref:`PMIx_server_init(3) <man3-PMIx_server_init>`,
   :ref:`PMIx_Put(3) <man3-PMIx_Put>`,
   :ref:`PMIx_Commit(3) <man3-PMIx_Commit>`,
   :ref:`PMIx_Fence(3) <man3-PMIx_Fence>`,
   :ref:`PMIx_Get(3) <man3-PMIx_Get>`,
   :ref:`pmix_proc_t(5) <man5-pmix_proc_t>`,
   :ref:`pmix_status_t(5) <man5-pmix_status_t>`
