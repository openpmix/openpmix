.. _man3-PMIx_Device_type_string:

PMIx_Device_type_string
=======================

.. include_body

``PMIx_Device_type_string`` |mdash| Return the string representation of a
``pmix_device_type_t`` value.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   const char* PMIx_Device_type_string(pmix_device_type_t type);


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

  from pmix import *

  foo = PMIxClient()
  name = foo.device_type_string(PMIX_DEVTYPE_GPU)


INPUT PARAMETERS
----------------

* ``type``: A device type value of type ``pmix_device_type_t``. Recognized values
  include ``PMIX_DEVTYPE_UNKNOWN``, ``PMIX_DEVTYPE_BLOCK``, ``PMIX_DEVTYPE_GPU``,
  ``PMIX_DEVTYPE_NETWORK``, ``PMIX_DEVTYPE_OPENFABRICS``, ``PMIX_DEVTYPE_DMA``, and
  ``PMIX_DEVTYPE_COPROC``.


DESCRIPTION
-----------

Return a human-readable, statically-allocated string describing the given device
type |mdash| for example, ``"GPU"``, ``"NETWORK"``, or ``"COPROCESSOR"``. If the
value does not correspond to a recognized device type, the fallback string
``"UNKNOWN"`` is returned.

The returned pointer refers to constant storage owned by the PMIx library. The
caller must **not** modify or free it.


RETURN VALUE
------------

Returns a pointer to a constant, statically-allocated character string. The pointer
is always non-``NULL`` and must not be freed by the caller.


.. seealso::
   ``PMIx_Link_state_string(3)``,
   ``PMIx_Job_state_string(3)``,
   :ref:`pmix_device_type_t(5) <man5-pmix_device_type_t>`
