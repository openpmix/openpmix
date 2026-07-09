.. _man3-PMIx_App_string:

PMIx_App_string
===============

.. include_body

``PMIx_App_string`` |mdash| Return an allocated string representation of a ``pmix_app_t``.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   char* PMIx_App_string(const pmix_app_t *app);


INPUT PARAMETERS
----------------

* ``app``: Pointer to the ``pmix_app_t`` structure to be rendered.


DESCRIPTION
-----------

Build a multi-line, human-readable rendering of the supplied ``pmix_app_t``,
including its command (``CMD``), argument vector (``ARGV``), environment (``ENV``),
working directory (``CWD``), maximum process count (``MAXPROCS``), and each of its
attached :ref:`pmix_info_t(5) <man5-pmix_info_t>` entries (``INFO``). The individual
lines are joined with embedded newlines into a single string.

Unlike the ``const char*`` converter routines (such as
:ref:`PMIx_Error_string(3) <man3-PMIx_Error_string>`), which return a pointer to
storage owned by the library, ``PMIx_App_string`` returns a freshly
**heap-allocated** buffer (produced by the library's argv-join helper, which uses
``malloc``). The **caller is responsible for releasing it** with the standard C
library ``free()`` when it is no longer needed.


RETURN VALUE
------------

Returns a pointer to an allocated, NULL-terminated string that the caller must
release with ``free()``.

Returns ``NULL`` if one of the application's :ref:`pmix_info_t(5) <man5-pmix_info_t>`
entries could not be rendered.


NOTES
-----

The returned buffer must be freed with ``free()``; do not pass it to
``PMIX_RELEASE`` or any other PMIx destructor macro. This routine is a library
convenience function and is not part of the PMIx Standard.

The ``app`` argument is dereferenced directly and is not checked for ``NULL``; the
caller must supply a valid pointer.


.. seealso::
   :ref:`PMIx_Spawn(3) <man3-PMIx_Spawn>`,
   :ref:`PMIx_Info_string(3) <man3-PMIx_Info_string>`
