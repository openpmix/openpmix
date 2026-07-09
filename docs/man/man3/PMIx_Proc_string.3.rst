.. _man3-PMIx_Proc_string:

PMIx_Proc_string
================

.. include_body

``PMIx_Proc_string`` |mdash| Return an allocated string representation of a ``pmix_proc_t``.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   char* PMIx_Proc_string(const pmix_proc_t *proc);


INPUT PARAMETERS
----------------

* ``proc``: Pointer to the ``pmix_proc_t`` structure to be rendered.


DESCRIPTION
-----------

Build a human-readable rendering of the supplied ``pmix_proc_t``, expressing the
process identifier as its namespace and rank (for example, ``nspace:rank``).

Unlike the ``const char*`` converter routines (such as
:ref:`PMIx_Error_string(3) <man3-PMIx_Error_string>`), which return a pointer to
storage owned by the library, ``PMIx_Proc_string`` returns a freshly
**heap-allocated** buffer (produced with ``strdup``, which uses ``malloc``). The
**caller is responsible for releasing it** with the standard C library ``free()``
when it is no longer needed.


RETURN VALUE
------------

Returns a pointer to an allocated, NULL-terminated string that the caller must
release with ``free()``.


NOTES
-----

The returned buffer must be freed with ``free()``; do not pass it to
``PMIX_RELEASE`` or any other PMIx destructor macro. This routine is a library
convenience function and is not part of the PMIx Standard.

The ``proc`` argument is dereferenced directly and is not checked for ``NULL``; the
caller must supply a valid pointer.


.. seealso::
   :ref:`PMIx_Resolve_peers(3) <man3-PMIx_Resolve_peers>`,
   :ref:`PMIx_Info_string(3) <man3-PMIx_Info_string>`
