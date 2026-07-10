.. _man3-PMIx_Fabric_update:

PMIx_Fabric_update
==================

.. include_body

``PMIx_Fabric_update``, ``PMIx_Fabric_update_nb`` |mdash| Update the
fabric-related information contained in a registered fabric object.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_status_t PMIx_Fabric_update(pmix_fabric_t *fabric);

   pmix_status_t PMIx_Fabric_update_nb(pmix_fabric_t *fabric,
                                       pmix_op_cbfunc_t cbfunc, void *cbdata);


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

  from pmix import *

  foo = PMIxClient()
  # ... after a successful foo.init() and foo.fabric_register() ...
  rc, fabricinfo = foo.fabric_update()


INPUT PARAMETERS
----------------

* ``fabric``: Address of the ``pmix_fabric_t`` structure that was provided to
  :ref:`PMIx_Fabric_register(3) <man3-PMIx_Fabric_register>`. Upon successful
  completion, the information fields of this structure will have been updated.

The non-blocking form takes two additional parameters:

* ``cbfunc``: Callback function of type :ref:`pmix_op_cbfunc_t <man5-pmix_op_cbfunc_t>` to be invoked when
  the update completes.
* ``cbdata``: Opaque pointer that is passed, unmodified, to ``cbfunc``.


DESCRIPTION
-----------

Update the fabric-related information contained in a previously registered
``pmix_fabric_t`` object. This call may be made at any time to request a refresh
of the object's information |mdash| for example, to poll for changes to the cost
matrix. ``PMIx_Fabric_update`` is the blocking form: it does not return until the
update is complete (or the operation fails), at which point the information fields
of the ``fabric`` structure have been updated. ``PMIx_Fabric_update_nb`` is the
non-blocking form: it returns immediately, and the provided ``cbfunc`` is invoked
once the update has been applied to the ``fabric`` structure.

Because the PMIx library does not provide thread protection for the contents of
the ``pmix_fabric_t`` structure, the caller is **not** allowed to access the
provided ``fabric`` structure while an update is in progress: for the blocking
form, until the call returns; for the non-blocking form, until ``cbfunc`` has
been invoked.


RETURN VALUE
------------

For the blocking form, ``PMIX_SUCCESS`` indicates that the update completed and
the fields of the ``fabric`` structure have been refreshed. For the non-blocking
form, a return of ``PMIX_SUCCESS`` indicates only that the request was accepted
for processing; the final status is delivered to ``cbfunc``.

* ``PMIX_SUCCESS`` |mdash| the update completed successfully.
* ``PMIX_OPERATION_SUCCEEDED`` |mdash| (non-blocking form) the request was
  satisfied immediately and ``cbfunc`` will **not** be called.
* ``PMIX_ERR_NOT_SUPPORTED`` |mdash| the operation cannot be serviced |mdash| for
  example, a server whose host environment does not provide fabric support.
* ``PMIX_ERR_UNREACH`` |mdash| the process is not a server or scheduler and its
  local PMIx server could not be reached.
* ``PMIX_ERR_NOT_AVAILABLE`` |mdash| the operation cannot be serviced because the
  library's progress engine has been stopped.
* ``PMIX_ERR_INIT`` |mdash| the PMIx library has not been initialized.

Any other negative value indicates an appropriate error condition. PMIx error
constants are defined in ``pmix_common.h``.


NOTES
-----

The fabric object must first have been registered with
:ref:`PMIx_Fabric_register(3) <man3-PMIx_Fabric_register>`. Polling with
``PMIx_Fabric_update`` is one of two coordinated methods for refreshing fabric
information; the other is to register for ``PMIX_FABRIC_UPDATE_PENDING`` and
``PMIX_FABRIC_UPDATED`` events. See
:ref:`PMIx_Fabric_register(3) <man3-PMIx_Fabric_register>` for details.


.. seealso::
   :ref:`PMIx_Fabric_register(3) <man3-PMIx_Fabric_register>`,
   :ref:`PMIx_Fabric_deregister(3) <man3-PMIx_Fabric_deregister>`,
   :ref:`pmix_status_t(5) <man5-pmix_status_t>`,
   :ref:`pmix_fabric_t(5) <man5-pmix_fabric_t>`
