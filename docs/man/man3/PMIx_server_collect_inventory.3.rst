.. _man3-PMIx_server_collect_inventory:

PMIx_server_collect_inventory
=============================

.. include_body

``PMIx_server_collect_inventory`` |mdash| Collect an inventory of local
resources through the PMIx server library.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix_server.h>

   pmix_status_t PMIx_server_collect_inventory(pmix_info_t directives[], size_t ndirs,
                                               pmix_info_cbfunc_t cbfunc, void *cbdata);


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

  from pmix import *

  foo = PMIxServer()
  # ... after a successful foo.init() ...
  # directives is a list of Python ``pmix_info_t`` dictionaries (may be empty)
  pydirs = []
  rc, results = foo.collect_inventory(pydirs)
  # results is a list of Python ``pmix_info_t`` dictionaries describing the inventory


INPUT PARAMETERS
----------------

* ``directives``: Array of :ref:`pmix_info_t(5) <man5-pmix_info_t>` structures
  conveying directives that scope or refine the collection request (see
  `DIRECTIVES`_). A ``NULL`` value (with ``ndirs`` of zero) is supported when no
  directives are desired.
* ``ndirs``: Number of elements in the ``directives`` array.
* ``cbfunc``: Callback function of type ``pmix_info_cbfunc_t`` invoked with the
  collected inventory once the operation completes (see `CALLBACK FUNCTION`_).
* ``cbdata``: Opaque pointer that is passed, unmodified, to ``cbfunc``.


DESCRIPTION
-----------

Request that the local PMIx server library collect an inventory of the
resources available on its node. The set of resources reported depends on the
PMIx implementation and the plugins built into the library, but typically
includes network fabric interfaces and GPU devices; the underlying ``pnet`` and
``pgpu`` frameworks are queried in turn and their contributions aggregated into
a single result array.

Servers designated as *gateways* (via the ``PMIX_SERVER_GATEWAY`` attribute at
server initialization) whose plugins support collection of infrastructure
information |mdash| for example, switch and fabric topology or connectivity
maps |mdash| shall return that broader information. Plugins on non-gateway
servers return only the node-local inventory.

``PMIx_server_collect_inventory`` is a **non-blocking** operation: it
thread-shifts the request into the library's progress thread and returns
immediately, because gathering inventory may involve lengthy operations. The
provided ``cbfunc`` is invoked with the assembled inventory once collection is
complete. Inventory collection is expected to be a rare event |mdash| performed
at system startup or on command from a system administrator |mdash| with later
changes handled through smaller, incremental inventory updates.


DIRECTIVES
----------

The ``directives`` array scopes and refines the collection. The specific
directives honored are determined by the active ``pnet`` and ``pgpu`` plugins
and are therefore implementation-defined; an unrecognized directive is ignored.
Directives that are commonly meaningful include:

* ``PMIX_HOSTNAME`` (char*) |mdash| restrict or label the request with respect
  to a specific host.
* ``PMIX_NODEID`` (uint32_t) |mdash| restrict or label the request with respect
  to a specific node identifier.

Whether a server returns node-local inventory only, or also infrastructure
inventory, is governed by whether it was initialized as a gateway rather than
by a directive to this call (see `DESCRIPTION`_).


CALLBACK FUNCTION
-----------------

The ``cbfunc`` has the signature ``pmix_info_cbfunc_t``:

.. code-block:: c

   typedef void (*pmix_info_cbfunc_t)(pmix_status_t status,
                                      pmix_info_t *info, size_t ninfo,
                                      void *cbdata,
                                      pmix_release_cbfunc_t release_fn,
                                      void *release_cbdata);

When collection completes, the library invokes ``cbfunc`` with the final
``status``, an ``info`` array of ``ninfo`` elements describing the collected
inventory, and the original ``cbdata``. If the node contributed no inventory,
``info`` is ``NULL`` and ``ninfo`` is zero with a ``status`` of
``PMIX_SUCCESS``.

The ``info`` array is owned by the PMIx server library. When the host has
finished consuming it, it **must** invoke the provided ``release_fn`` (passing
``release_cbdata``) so the library can free the array; the host must not free
the array itself.


RETURN VALUE
------------

A return of ``PMIX_SUCCESS`` indicates only that the request was accepted for
processing; the final status and the collected inventory are delivered through
``cbfunc``. Possible immediate return values include:

* ``PMIX_SUCCESS`` |mdash| the request was accepted for processing.
* ``PMIX_ERR_NOMEM`` |mdash| the library was unable to allocate memory for the
  request.
* ``PMIX_ERR_NOT_AVAILABLE`` |mdash| the operation cannot be serviced because
  the library's progress engine has been stopped.
* ``PMIX_ERR_INIT`` |mdash| the PMIx server library has not been initialized.

When an error is returned immediately, ``cbfunc`` is **not** called. Any other
negative value indicates an appropriate error condition. PMIx error constants
are defined in ``pmix_common.h``.


NOTES
-----

Collected inventory is intended to be delivered to the library (typically on a
gateway associated with the system scheduler) via
:ref:`PMIx_server_deliver_inventory(3) <man3-PMIx_server_deliver_inventory>`
for storage and subsequent use during allocation and application setup.


.. seealso::
   :ref:`PMIx_server_init(3) <man3-PMIx_server_init>`,
   :ref:`PMIx_server_deliver_inventory(3) <man3-PMIx_server_deliver_inventory>`,
   :ref:`PMIx_server_setup_application(3) <man3-PMIx_server_setup_application>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`,
   :ref:`pmix_status_t(5) <man5-pmix_status_t>`
