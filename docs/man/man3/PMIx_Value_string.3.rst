.. _man3-PMIx_Value_string:

PMIx_Value_string
=================

.. include_body

``PMIx_Value_string`` |mdash| Return an allocated string representation of a :ref:`pmix_value_t(5) <man5-pmix_value_t>`.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   char* PMIx_Value_string(const pmix_value_t *value);


INPUT PARAMETERS
----------------

* ``value``: Pointer to the :ref:`pmix_value_t(5) <man5-pmix_value_t>` structure to be
  rendered.


DESCRIPTION
-----------

Build a human-readable rendering of the supplied :ref:`pmix_value_t(5) <man5-pmix_value_t>`,
reporting both its data type and its contents. The rendering is produced by the
library's internal print routine for the value's declared type.

Unlike the ``const char*`` converter routines (such as
:ref:`PMIx_Error_string(3) <man3-PMIx_Error_string>`), which return a pointer to
storage owned by the library, ``PMIx_Value_string`` returns a freshly
**heap-allocated** buffer. The buffer is allocated internally with ``malloc``/the
library's ``asprintf`` wrapper, so the **caller is responsible for releasing it**
with the standard C library ``free()`` when it is no longer needed.


RETURN VALUE
------------

Returns a pointer to an allocated, NULL-terminated string that the caller must
release with ``free()``.

Returns ``NULL`` if the value could not be rendered |mdash| for example, if
``value`` is ``NULL`` or references an unknown data type.


NOTES
-----

The returned buffer must be freed with ``free()``; do not pass it to
``PMIX_RELEASE`` or any other PMIx destructor macro. This routine is a library
convenience function and is not part of the PMIx Standard.


.. seealso::
   :ref:`PMIx_Value_unload(3) <man3-PMIx_Value_unload>`,
   :ref:`PMIx_Info_string(3) <man3-PMIx_Info_string>`
