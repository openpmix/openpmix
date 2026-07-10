.. _man3-PMIx_Alloc_directive_string:

PMIx_Alloc_directive_string
===========================

.. include_body

``PMIx_Alloc_directive_string`` |mdash| Return the string representation of a
``pmix_alloc_directive_t`` value.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   const char* PMIx_Alloc_directive_string(pmix_alloc_directive_t directive);


INPUT PARAMETERS
----------------

* ``directive`` : A ``pmix_alloc_directive_t`` allocation directive.

Recognized values map to a human-readable string:

* ``PMIX_ALLOC_NEW`` |mdash| ``"NEW"``
* ``PMIX_ALLOC_EXTEND`` |mdash| ``"EXTEND"``
* ``PMIX_ALLOC_RELEASE`` |mdash| ``"RELEASE"``
* ``PMIX_ALLOC_REAQUIRE`` |mdash| ``"REACQUIRE"``


DESCRIPTION
-----------

Return a human-readable, statically-allocated string naming the given
``pmix_alloc_directive_t`` value. The returned string is owned by the library
and must not be modified or freed by the caller. An unrecognized value yields
the fallback string ``"UNSPECIFIED"``.


RETURN VALUE
------------

Returns a pointer to a constant, statically-allocated, NUL-terminated string.
The pointer remains valid for the lifetime of the process and must not be
freed.


.. seealso::
   :ref:`PMIx_Allocation_request(3) <man3-PMIx_Allocation_request>`,
   :ref:`pmix_alloc_directive_t(5) <man5-pmix_alloc_directive_t>`
