.. _man3-PMIx_Fabric_register:

PMIx_Fabric_register
====================

.. include_body

``PMIx_Fabric_register``, ``PMIx_Fabric_register_nb`` |mdash| Register for
access to fabric-related information, including the communication cost matrix.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_status_t PMIx_Fabric_register(pmix_fabric_t *fabric,
                                      const pmix_info_t directives[],
                                      size_t ndirs);

   pmix_status_t PMIx_Fabric_register_nb(pmix_fabric_t *fabric,
                                         const pmix_info_t directives[],
                                         size_t ndirs,
                                         pmix_op_cbfunc_t cbfunc, void *cbdata);


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

  from pmix import *

  foo = PMIxClient()
  # ... after a successful foo.init() ...
  # the directives is a list of Python ``pmix_info_t`` dictionaries
  pydirs = [{'key': PMIX_FABRIC_VENDOR,
             'value': {'value': "ACME", 'val_type': PMIX_STRING}}]
  rc, fabricinfo = foo.fabric_register(pydirs)


INPUT PARAMETERS
----------------

* ``fabric``: Address of a ``pmix_fabric_t`` structure (backed by storage). The
  caller may populate the ``name`` field at will |mdash| PMIx does not use that
  field. All remaining fields are filled in by the PMIx library and must not be
  modified by the caller.
* ``directives``: Pointer to an optional array of
  :ref:`pmix_info_t(5) <man5-pmix_info_t>` structures indicating desired
  behaviors and/or the fabric to be accessed (see `DIRECTIVES`_). A ``NULL``
  value directs that the highest-priority available fabric be used.
* ``ndirs``: Number of elements in the ``directives`` array.

The non-blocking form takes two additional parameters:

* ``cbfunc``: Callback function of type ``pmix_op_cbfunc_t`` to be invoked when
  the registration completes.
* ``cbdata``: Opaque pointer that is passed, unmodified, to ``cbfunc``.


DESCRIPTION
-----------

Register for access to fabric-related information, including the communication
cost matrix. This call must be made prior to requesting information from a
fabric. ``PMIx_Fabric_register`` is the blocking form: it does not return until
the registration is complete (or the operation fails), at which point the fields
of the provided ``fabric`` structure have been filled in.
``PMIx_Fabric_register_nb`` is the non-blocking form: it returns immediately, and
the provided ``cbfunc`` is invoked once the fabric information has been loaded
into the ``fabric`` structure.

The caller may request access to a particular fabric using the vendor, type, or
identifier, or to a specific fabric plane via the ``PMIX_FABRIC_PLANE`` attribute
|mdash| otherwise, information for the default fabric is returned.

For performance reasons, the PMIx library does **not** provide thread protection
for accessing the information in the ``pmix_fabric_t`` structure. Instead, two
methods are provided for coordinating updates to the fabric information: the
caller may periodically poll for updates using
:ref:`PMIx_Fabric_update(3) <man3-PMIx_Fabric_update>`, and/or register for
``PMIX_FABRIC_UPDATE_PENDING`` events. When a ``PMIX_FABRIC_UPDATE_PENDING``
event is received, the caller must pause or terminate any actions that access the
cost matrix before returning from the event handler; completion of the handler
signals the PMIx library that the object's entries may be updated. When the
update is complete, the library generates a ``PMIX_FABRIC_UPDATED`` event
indicating that it is safe to resume using the updated fabric object. There is no
requirement that a caller use only one of these methods.

As with all non-blocking PMIx APIs, callers of ``PMIx_Fabric_register_nb``
**must** keep the ``directives`` array valid until ``cbfunc`` is invoked, and
must not access the provided ``fabric`` structure until that time.


DIRECTIVES
----------

The following directives are required to be supported by all PMIx libraries to
aid callers in identifying the fabric whose data is being sought:

* ``PMIX_FABRIC_PLANE`` (char*) |mdash| ID string of the fabric plane whose
  information is to be accessed (e.g., CIDR for Ethernet).
* ``PMIX_FABRIC_IDENTIFIER`` (char*) |mdash| identifier of the fabric to be
  accessed (e.g., MgmtEthernet, Slingshot-11, OmniPath-1).
* ``PMIX_FABRIC_VENDOR`` (char*) |mdash| name of the fabric vendor (e.g., Amazon,
  Mellanox, HPE, Intel) whose fabric is to be accessed.


RETURN VALUE
------------

For the blocking form, ``PMIX_SUCCESS`` indicates that the registration completed
and the fields of the ``fabric`` structure have been populated. For the
non-blocking form, a return of ``PMIX_SUCCESS`` indicates only that the request
was accepted for processing; the final status is delivered to ``cbfunc``.

* ``PMIX_SUCCESS`` |mdash| the registration completed successfully.
* ``PMIX_OPERATION_SUCCEEDED`` |mdash| (non-blocking form) the request was
  satisfied immediately and ``cbfunc`` will **not** be called.
* ``PMIX_ERR_UNREACH`` |mdash| the process is not a server or scheduler and its
  local PMIx server could not be reached.
* ``PMIX_ERR_NOT_AVAILABLE`` |mdash| the operation cannot be serviced because the
  library's progress engine has been stopped.
* ``PMIX_ERR_INIT`` |mdash| the PMIx library has not been initialized.

Any other negative value indicates an appropriate error condition. PMIx error
constants are defined in ``pmix_common.h``.


NOTES
-----

Due to their high cost in terms of execution time, memory consumption, and
interaction with other system management software (e.g., a fabric manager), the
fabric support functions are intended primarily for use by the PMIx server
library hosting the system scheduler (see ``PMIX_SERVER_SCHEDULER``). Clients,
tools, and other servers that call these functions will have their requests
forwarded to the server supporting the scheduler.


.. seealso::
   :ref:`PMIx_Fabric_update(3) <man3-PMIx_Fabric_update>`,
   :ref:`PMIx_Fabric_deregister(3) <man3-PMIx_Fabric_deregister>`,
   :ref:`PMIx_Get(3) <man3-PMIx_Get>`,
   :ref:`PMIx_Query_info(3) <man3-PMIx_Query_info>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`,
   :ref:`pmix_status_t(5) <man5-pmix_status_t>`
