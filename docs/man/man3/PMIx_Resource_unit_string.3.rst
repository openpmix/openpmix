.. _man3-PMIx_Resource_unit_string:

PMIx_Resource_unit_string
=========================

.. include_body

``PMIx_Resource_unit_string`` |mdash| Return an allocated string representation of a ``pmix_resource_unit_t``.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   char* PMIx_Resource_unit_string(const pmix_resource_unit_t *unit);


INPUT PARAMETERS
----------------

* ``unit``: Pointer to the ``pmix_resource_unit_t`` structure to be rendered.


DESCRIPTION
-----------

Build a human-readable rendering of the supplied ``pmix_resource_unit_t``, reporting
its device type (``TYPE``) and count (``COUNT``) |mdash| for example,
``TYPE: <type>  COUNT: <n>``.

Unlike the ``const char*`` converter routines (such as
:ref:`PMIx_Error_string(3) <man3-PMIx_Error_string>`), which return a pointer to
storage owned by the library, ``PMIx_Resource_unit_string`` returns a freshly
**heap-allocated** buffer (produced with the library's ``asprintf`` wrapper, which
uses ``malloc``). The **caller is responsible for releasing it** with the standard C
library ``free()`` when it is no longer needed.


RETURN VALUE
------------

Returns a pointer to an allocated, NULL-terminated string that the caller must
release with ``free()``.


NOTES
-----

The returned buffer must be freed with ``free()``; do not pass it to
``PMIX_RELEASE`` or any other PMIx destructor macro. This routine is a library
convenience function and is not part of the PMIx Standard.

The ``unit`` argument is dereferenced directly and is not checked for ``NULL``; the
caller must supply a valid pointer.


.. seealso::
   :ref:`PMIx_Resource_block(3) <man3-PMIx_Resource_block>`,
   :ref:`PMIx_Info_string(3) <man3-PMIx_Info_string>`,
   :ref:`pmix_resource_unit_t(5) <man5-pmix_resource_unit_t>`
