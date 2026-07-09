.. _man3-PMIx_Store_internal:

PMIx_Store_internal
===================

.. include_body

``PMIx_Store_internal`` |mdash| Store a key/value pair locally for retrieval within
this process.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_status_t PMIx_Store_internal(const pmix_proc_t *proc,
                                     const char key[],
                                     pmix_value_t *val);


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

  from pmix import *

  foo = PMIxClient()
  # ... after a successful foo.init() ...
  # proc is a dict {'nspace': ..., 'rank': ...} or None for this process
  # the value is a Python ``pmix_value_t`` dictionary
  rc = foo.store_internal({'nspace': 'myapp', 'rank': 0}, "mykey",
                          {'value': 42, 'val_type': PMIX_UINT32})


INPUT PARAMETERS
----------------

* ``proc``: Pointer to a ``pmix_proc_t`` identifying the process (namespace and
  rank) under which the data is to be stored. If ``NULL``, the calling process's
  own identifier is used.
* ``key``: A NULL-terminated string identifying the value. The string must be no
  longer than ``PMIX_MAX_KEYLEN`` characters.
* ``val``: Pointer to a ``pmix_value_t`` structure containing the value to be
  stored.


DESCRIPTION
-----------

Store some data locally for retrieval by other areas of the process. This is data
that has only internal scope |mdash| it will never be "pushed" or otherwise posted
externally to other processes. It is typically used to cache data obtained by
means outside of PMIx so that it can subsequently be accessed by various parts of
the same process through :ref:`PMIx_Get(3) <man3-PMIx_Get>`.

The value is copied into internal memory before ``PMIx_Store_internal`` returns, so
the caller may modify or free ``val`` immediately afterward. Because the data is
associated with the ``proc`` identifier, a later :ref:`PMIx_Get(3) <man3-PMIx_Get>`
using that same identifier and ``key`` will retrieve the stored value.

Unlike :ref:`PMIx_Put(3) <man3-PMIx_Put>`, whose data is staged for distribution to
other processes, data stored with ``PMIx_Store_internal`` never leaves the process
and requires no subsequent commit or synchronization step.


RETURN VALUE
------------

Returns ``PMIX_SUCCESS`` on success. On error, a negative value corresponding to
a PMIx error constant is returned, including:

* ``PMIX_ERR_BAD_PARAM`` |mdash| the ``key`` is ``NULL`` or exceeds
  ``PMIX_MAX_KEYLEN`` characters.
* ``PMIX_ERR_NOT_AVAILABLE`` |mdash| the operation cannot be serviced because the
  library's progress engine has been stopped.
* ``PMIX_ERR_INIT`` |mdash| the PMIx library has not been initialized.
* ``PMIX_ERR_NOMEM`` |mdash| the library was unable to allocate memory to store the
  data.

Any other negative value indicates an appropriate error condition. PMIx error
constants are defined in ``pmix_common.h``.


NOTES
-----

Data stored with ``PMIx_Store_internal`` has purely internal scope; it is visible
only within the storing process and is never shared with peers. To make data
available to other processes, use :ref:`PMIx_Put(3) <man3-PMIx_Put>` followed by
:ref:`PMIx_Commit(3) <man3-PMIx_Commit>` instead.


.. seealso::
   :ref:`PMIx_Init(3) <man3-PMIx_Init>`,
   :ref:`PMIx_Put(3) <man3-PMIx_Put>`,
   :ref:`PMIx_Get(3) <man3-PMIx_Get>`,
   :ref:`pmix_value_t(5) <man5-pmix_value_t>`
