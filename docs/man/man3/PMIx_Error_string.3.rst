.. _man3-PMIx_Error_string:

PMIx_Error_string
=================

.. include_body

``PMIx_Error_string`` |mdash| Return a string representation of a PMIx status code.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   const char* PMIx_Error_string(pmix_status_t status);


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

  from pmix import *

  foo = PMIxClient()
  # status is an integer PMIx status code
  string = foo.error_string(PMIX_ERR_NOT_FOUND)


INPUT PARAMETERS
----------------

* ``status``: A ``pmix_status_t`` value (a PMIx status or event constant) for
  which a string representation is desired.


DESCRIPTION
-----------

Return a human-readable, NULL-terminated string that names the provided PMIx
status code. The function searches the library's table of known status and event
constants for one whose numeric value matches ``status`` and returns its symbolic
name (for example, ``"NOT-FOUND"`` for ``PMIX_ERR_NOT_FOUND``).

``PMIx_Error_string`` may be called at any time, including outside of the
initialized region, and is usable by servers and tools in addition to clients.

.. note::
   The returned string is statically allocated within the PMIx library and must
   **not** be modified or freed by the caller.


RETURN VALUE
------------

Returns a pointer to a statically allocated, NULL-terminated string naming the
provided status code. If ``status`` does not correspond to any status or event
constant known to the library, the fixed string ``"ERROR STRING NOT FOUND"`` is
returned. The function always returns a valid, non-``NULL`` pointer.


NOTES
-----

``PMIx_Error_string`` is the inverse of
:ref:`PMIx_Error_code(3) <man3-PMIx_Error_code>`, which maps a status name string
back to its numeric ``pmix_status_t`` value.

Because the returned string is owned by the library, callers must never pass it to
``free()``.


.. seealso::
   :ref:`PMIx_Error_code(3) <man3-PMIx_Error_code>`,
   :ref:`PMIx_Get_version(3) <man3-PMIx_Get_version>`,
   :ref:`pmix_status_t(5) <man5-pmix_status_t>`
