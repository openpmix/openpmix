.. _man3-PMIx_Info_string:

PMIx_Info_string
================

.. include_body

``PMIx_Info_string`` |mdash| Return an allocated string representation of a :ref:`pmix_info_t(5) <man5-pmix_info_t>`.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   char* PMIx_Info_string(const pmix_info_t *info);


INPUT PARAMETERS
----------------

* ``info``: Pointer to the :ref:`pmix_info_t(5) <man5-pmix_info_t>` structure to be
  rendered.


DESCRIPTION
-----------

Build a human-readable rendering of the supplied :ref:`pmix_info_t(5) <man5-pmix_info_t>`,
including its key, the type and contents of its embedded
:ref:`pmix_value_t(5) <man5-pmix_value_t>`, and its directive flags. The rendering is
produced by the same internal print routine used elsewhere in the library, followed
by a trailing newline.

Unlike the ``const char*`` converter routines (such as
:ref:`PMIx_Error_string(3) <man3-PMIx_Error_string>`), which return a pointer to
storage owned by the library, ``PMIx_Info_string`` returns a freshly
**heap-allocated** buffer. The buffer is allocated with the library's ``asprintf``
wrapper, so the **caller is responsible for releasing it** with the standard C
library ``free()`` when it is no longer needed. Failing to do so leaks the buffer.


RETURN VALUE
------------

Returns a pointer to an allocated, NULL-terminated string that the caller must
release with ``free()``.

Returns ``NULL`` if the information could not be rendered |mdash| for example, if
``info`` is ``NULL`` or references an unknown data type.


NOTES
-----

The returned buffer must be freed with ``free()``; do not pass it to
``PMIX_RELEASE`` or any other PMIx destructor macro. This routine is a library
convenience function and is not part of the PMIx Standard.


.. seealso::
   :ref:`PMIx_Info_construct(3) <man3-PMIx_Info_construct>`,
   :ref:`PMIx_Value_string(3) <man3-PMIx_Value_string>`
