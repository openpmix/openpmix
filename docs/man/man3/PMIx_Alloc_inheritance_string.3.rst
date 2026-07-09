.. _man3-PMIx_Alloc_inheritance_string:

PMIx_Alloc_inheritance_string
=============================

.. include_body

``PMIx_Alloc_inheritance_string`` |mdash| Return the string representation of a
``pmix_alloc_inheritance_t`` value.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   const char* PMIx_Alloc_inheritance_string(pmix_alloc_inheritance_t inheritance);


INPUT PARAMETERS
----------------

* ``inheritance`` : A ``pmix_alloc_inheritance_t`` allocation-inheritance value.

Recognized values map to a human-readable string:

* ``PMIX_ALLOC_INHERIT_NONE`` |mdash| ``"NONE"``
* ``PMIX_ALLOC_INHERIT_CHILD`` |mdash| ``"CHILD"``
* ``PMIX_ALLOC_INHERIT_DEFAULT`` |mdash| ``"DEFAULT"``
* ``PMIX_ALLOC_INHERIT_CHILD_DEFAULT`` |mdash| ``"CHILD_DEFAULT"``


DESCRIPTION
-----------

Return a human-readable, statically-allocated string naming the given
``pmix_alloc_inheritance_t`` value. The returned string is owned by the library
and must not be modified or freed by the caller. An unrecognized value yields
the fallback string ``"UNSPECIFIED"``.


RETURN VALUE
------------

Returns a pointer to a constant, statically-allocated, NUL-terminated string.
The pointer remains valid for the lifetime of the process and must not be
freed.


.. seealso::
   :ref:`PMIx_Allocation_request(3) <man3-PMIx_Allocation_request>`
