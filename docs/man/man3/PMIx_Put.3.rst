.. _man3-PMIx_Put:

PMIx_Put
========

.. include_body

``PMIx_Put`` |mdash| Stage a key/value pair for distribution to other processes.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_status_t PMIx_Put(pmix_scope_t scope,
                          const char key[],
                          pmix_value_t *val);


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

  from pmix import *

  foo = PMIxClient()
  # ... after a successful foo.init() ...
  # the value is a Python ``pmix_value_t`` dictionary
  rc = foo.put(PMIX_GLOBAL, "mykey",
               {'value': 42, 'val_type': PMIX_UINT32})


INPUT PARAMETERS
----------------

* ``scope``: A ``pmix_scope_t`` value describing the distribution scope of the
  posted data |mdash| i.e., which processes are to be able to access it (see
  `SCOPE`_).
* ``key``: A NULL-terminated string identifying the value. The string must be no
  longer than ``PMIX_MAX_KEYLEN`` characters and must not begin with the reserved
  prefix ``"pmix"``.
* ``val``: Pointer to a ``pmix_value_t`` structure containing the value to be
  posted.


DESCRIPTION
-----------

Post a key-value pair for distribution. The provided value is copied into
internal memory before ``PMIx_Put`` returns, so the caller may modify or free
``val`` immediately afterward. The client PMIx library caches the posted value
locally until :ref:`PMIx_Commit(3) <man3-PMIx_Commit>` is called, at which point
the committed values are pushed to the local PMIx server, which distributes the
data as directed by each value's scope.

The ``pmix_value_t`` structure supports both string and binary values. PMIx
implementations support heterogeneous environments by properly converting binary
values between host architectures.

.. note::
   Keys beginning with the string ``"pmix"`` are reserved for use by the PMIx
   Standard and the library. Applications must never use a defined ``PMIX_``
   attribute |mdash| or any other ``"pmix"``-prefixed string |mdash| as the
   ``key`` in a call to ``PMIx_Put``; doing so returns ``PMIX_ERR_BAD_PARAM``.


SCOPE
-----

The ``pmix_scope_t`` value passed as ``scope`` determines which processes are
able to access the posted data. It is a ``uint8_t`` type taking one of the
following values:

* ``PMIX_LOCAL`` |mdash| the data is intended only for other application
  processes on the same node. It is not included in data packages sent to remote
  requesters.
* ``PMIX_REMOTE`` |mdash| the data is intended solely for application processes on
  remote nodes. It is not shared with other processes on the same node.
* ``PMIX_GLOBAL`` |mdash| the data is to be shared with all other requesting
  processes, regardless of location.
* ``PMIX_INTERNAL`` |mdash| the data is intended solely for this process and is
  not shared with any other process. Typically used to cache data the process
  obtained by means outside of PMIx.

A specific implementation may support additional scope values, but all
implementations support at least ``PMIX_GLOBAL``. If a specified scope value is
not supported, ``PMIx_Put`` returns ``PMIX_ERR_NOT_SUPPORTED``.


RETURN VALUE
------------

Returns ``PMIX_SUCCESS`` on success. On error, a negative value corresponding to
a PMIx error constant is returned, including:

* ``PMIX_ERR_BAD_PARAM`` |mdash| the ``key`` is ``NULL``, exceeds
  ``PMIX_MAX_KEYLEN``, or uses the reserved ``"pmix"`` prefix.
* ``PMIX_ERR_NOT_SUPPORTED`` |mdash| the requested ``scope`` is not supported by
  the implementation.
* ``PMIX_ERR_NOT_AVAILABLE`` |mdash| the operation cannot be serviced because the
  library's progress engine has been stopped.
* ``PMIX_ERR_INIT`` |mdash| the PMIx library has not been initialized.

Any other negative value indicates an appropriate error condition. PMIx error
constants are defined in ``pmix_common.h``.


NOTES
-----

``PMIx_Put`` only stages data locally; the values are not made available to other
processes until they are committed with :ref:`PMIx_Commit(3) <man3-PMIx_Commit>`
and, typically, a subsequent synchronization such as
:ref:`PMIx_Fence(3) <man3-PMIx_Fence>` has completed.


.. seealso::
   :ref:`PMIx_Init(3) <man3-PMIx_Init>`,
   :ref:`PMIx_Commit(3) <man3-PMIx_Commit>`,
   :ref:`PMIx_Get(3) <man3-PMIx_Get>`,
   :ref:`PMIx_Fence(3) <man3-PMIx_Fence>`,
   :ref:`pmix_status_t(5) <man5-pmix_status_t>`,
   :ref:`pmix_value_t(5) <man5-pmix_value_t>`
