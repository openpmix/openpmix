.. _man3-PMIx_Fabric_deregister:

PMIx_Fabric_deregister
======================

.. include_body

``PMIx_Fabric_deregister``, ``PMIx_Fabric_deregister_nb`` |mdash| Deregister a
fabric object and release any information associated with it.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_status_t PMIx_Fabric_deregister(pmix_fabric_t *fabric);

   pmix_status_t PMIx_Fabric_deregister_nb(pmix_fabric_t *fabric,
                                           pmix_op_cbfunc_t cbfunc, void *cbdata);


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

  from pmix import *

  foo = PMIxClient()
  # ... after a successful foo.init() and foo.fabric_register() ...
  rc = foo.fabric_deregister()


INPUT PARAMETERS
----------------

* ``fabric``: Address of the ``pmix_fabric_t`` structure that was provided to
  :ref:`PMIx_Fabric_register(3) <man3-PMIx_Fabric_register>`. The contents of the
  structure are invalidated upon return.

The non-blocking form takes two additional parameters:

* ``cbfunc``: Callback function of type ``pmix_op_cbfunc_t`` to be invoked when
  the deregistration completes.
* ``cbdata``: Opaque pointer that is passed, unmodified, to ``cbfunc``.


DESCRIPTION
-----------

Deregister a fabric object, providing an opportunity for the PMIx library to
clean up any information (e.g., the cost matrix) associated with it. The contents
of the provided ``pmix_fabric_t`` structure are invalidated upon return.
``PMIx_Fabric_deregister`` is the blocking form; ``PMIx_Fabric_deregister_nb`` is
the non-blocking form.

Deregistration is a local operation that completes immediately in the reference
implementation: both forms return ``PMIX_SUCCESS`` (blocking) or
``PMIX_OPERATION_SUCCEEDED`` (non-blocking) without invoking ``cbfunc``. Callers
of the non-blocking form should nonetheless be prepared for ``cbfunc`` to be
invoked, as permitted by the interface contract.


RETURN VALUE
------------

* ``PMIX_SUCCESS`` |mdash| (blocking form) the fabric object was deregistered.
* ``PMIX_OPERATION_SUCCEEDED`` |mdash| (non-blocking form) the request was
  satisfied immediately and ``cbfunc`` will **not** be called.

Any other negative value indicates an appropriate error condition. PMIx error
constants are defined in ``pmix_common.h``.


NOTES
-----

The fabric object must first have been registered with
:ref:`PMIx_Fabric_register(3) <man3-PMIx_Fabric_register>`. After
deregistration, the ``fabric`` structure must not be used again until it is
re-registered.


.. seealso::
   :ref:`PMIx_Fabric_register(3) <man3-PMIx_Fabric_register>`,
   :ref:`PMIx_Fabric_update(3) <man3-PMIx_Fabric_update>`,
   :ref:`pmix_status_t(5) <man5-pmix_status_t>`,
   :ref:`pmix_fabric_t(5) <man5-pmix_fabric_t>`
