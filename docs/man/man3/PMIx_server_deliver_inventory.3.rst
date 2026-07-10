.. _man3-PMIx_server_deliver_inventory:

PMIx_server_deliver_inventory
=============================

.. include_body

``PMIx_server_deliver_inventory`` |mdash| Deliver collected inventory to the
PMIx server library for storage.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix_server.h>

   pmix_status_t PMIx_server_deliver_inventory(pmix_info_t info[], size_t ninfo,
                                               pmix_info_t directives[], size_t ndirs,
                                               pmix_op_cbfunc_t cbfunc, void *cbdata);


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

  from pmix import *

  foo = PMIxServer()
  # ... after a successful foo.init() ...
  # info carries the inventory to store; directives scope the request
  pyinfo = [{'key': PMIX_HOSTNAME,
             'value': {'value': "node01", 'val_type': PMIX_STRING}}]
  pydirs = []
  rc = foo.deliver_inventory(pyinfo, pydirs)


INPUT PARAMETERS
----------------

* ``info``: Array of :ref:`pmix_info_t(5) <man5-pmix_info_t>` structures
  containing the inventory to be stored |mdash| typically the data previously
  returned by
  :ref:`PMIx_server_collect_inventory(3) <man3-PMIx_server_collect_inventory>`.
* ``ninfo``: Number of elements in the ``info`` array.
* ``directives``: Array of :ref:`pmix_info_t(5) <man5-pmix_info_t>` structures
  conveying directives that scope or refine the storage request (see
  `DIRECTIVES`_). A ``NULL`` value (with ``ndirs`` of zero) is supported when no
  directives are desired.
* ``ndirs``: Number of elements in the ``directives`` array.
* ``cbfunc``: Callback function of type ``pmix_op_cbfunc_t`` invoked once the
  library no longer requires access to the provided data. A ``NULL`` value makes
  the call blocking (see `DESCRIPTION`_).
* ``cbdata``: Opaque pointer that is passed, unmodified, to ``cbfunc``.


DESCRIPTION
-----------

Deliver inventory information |mdash| obtained from a node via a prior call to
:ref:`PMIx_server_collect_inventory(3) <man3-PMIx_server_collect_inventory>`
|mdash| to the PMIx server library for storage by the corresponding plugins.
The library forwards the data to its ``pnet`` and ``pgpu`` frameworks for
archiving.

This function is typically executed on a *gateway* server associated with the
system scheduler, so that the stored inventory can be used by the scheduling
algorithm and by the library when responding to
:ref:`PMIx_server_setup_application(3) <man3-PMIx_server_setup_application>`.
It may also be used on compute nodes to store a broader picture of the system
for access by applications (for example, via the
:ref:`PMIx_Get(3) <man3-PMIx_Get>` API, depending on the implementation).

``PMIx_server_deliver_inventory`` supports both blocking and non-blocking
operation. When ``cbfunc`` is non-``NULL`` the call is **non-blocking**: it
thread-shifts the request into the library's progress thread, returns
immediately, and invokes ``cbfunc`` once the library no longer needs the
provided data. When ``cbfunc`` is ``NULL`` the call is **blocking**: it does
not return until the operation has completed. In either case the caller must
keep the ``info`` and ``directives`` arrays valid until the library has
finished with them |mdash| until ``cbfunc`` fires in the non-blocking case, or
until the function returns in the blocking case.


DIRECTIVES
----------

The ``directives`` array scopes and refines the storage request. The specific
directives honored are determined by the active ``pnet`` and ``pgpu`` plugins
and are therefore implementation-defined; an unrecognized directive is ignored.
Directives that are commonly meaningful include:

* ``PMIX_HOSTNAME`` (char*) |mdash| associate the delivered inventory with a
  specific host.
* ``PMIX_NODEID`` (uint32_t) |mdash| associate the delivered inventory with a
  specific node identifier.


CALLBACK FUNCTION
-----------------

For the non-blocking form, the ``cbfunc`` has the signature
``pmix_op_cbfunc_t``:

.. code-block:: c

   typedef void (*pmix_op_cbfunc_t)(pmix_status_t status, void *cbdata);

The library invokes ``cbfunc`` with the final ``status`` of the storage
operation and the original ``cbdata`` once it no longer requires access to the
``info`` and ``directives`` arrays.


RETURN VALUE
------------

For the non-blocking form (``cbfunc`` is non-``NULL``), a return of
``PMIX_SUCCESS`` indicates only that the request was accepted for processing;
the final status is delivered through ``cbfunc``. For the blocking form
(``cbfunc`` is ``NULL``), a return of ``PMIX_OPERATION_SUCCEEDED`` indicates
that the request was immediately processed and completed successfully |mdash|
``cbfunc`` is not called. Possible return values include:

* ``PMIX_SUCCESS`` |mdash| (non-blocking form) the request was accepted for
  processing.
* ``PMIX_OPERATION_SUCCEEDED`` |mdash| (blocking form) the operation completed
  successfully.
* ``PMIX_ERR_NOMEM`` |mdash| the library was unable to allocate memory for the
  request.
* ``PMIX_ERR_NOT_AVAILABLE`` |mdash| the operation cannot be serviced because
  the library's progress engine has been stopped.
* ``PMIX_ERR_INIT`` |mdash| the PMIx server library has not been initialized.

Any other negative value indicates an appropriate error condition. PMIx error
constants are defined in ``pmix_common.h``.


.. seealso::
   :ref:`PMIx_server_init(3) <man3-PMIx_server_init>`,
   :ref:`PMIx_server_collect_inventory(3) <man3-PMIx_server_collect_inventory>`,
   :ref:`PMIx_server_setup_application(3) <man3-PMIx_server_setup_application>`,
   :ref:`PMIx_Get(3) <man3-PMIx_Get>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`,
   :ref:`pmix_status_t(5) <man5-pmix_status_t>`
