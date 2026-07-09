.. _man3-PMIx_Get_version:

PMIx_Get_version
================

.. include_body

``PMIx_Get_version`` |mdash| Return the PMIx version string.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   const char* PMIx_Get_version(void);


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

  from pmix import *

  foo = PMIxClient()
  version = foo.get_version()


DESCRIPTION
-----------

Return the PMIx version string identifying the PMIx library in use.

``PMIx_Get_version`` may be called outside of the initialized and finalized
region |mdash| it does not require a prior call to
:ref:`PMIx_Init(3) <man3-PMIx_Init>` |mdash| and is usable by servers and tools in
addition to clients.

.. note::
   The returned string is statically defined within the PMIx library and must
   **not** be modified or freed by the caller.


RETURN VALUE
------------

Returns a pointer to a statically allocated, NULL-terminated string containing the
PMIx version information. The function takes no arguments and always returns a
valid, non-``NULL`` pointer.


NOTES
-----

Because the returned string is owned by the library, callers must never pass it to
``free()``.


.. seealso::
   :ref:`PMIx_Init(3) <man3-PMIx_Init>`,
   :ref:`PMIx_Error_string(3) <man3-PMIx_Error_string>`,
   :ref:`PMIx_Error_code(3) <man3-PMIx_Error_code>`
