.. _man3-PMIx_Scope_string:

PMIx_Scope_string
=================

.. include_body

``PMIx_Scope_string`` |mdash| Return the string representation of a
``pmix_scope_t`` value.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   const char* PMIx_Scope_string(pmix_scope_t scope);


INPUT PARAMETERS
----------------

* ``scope`` : A ``pmix_scope_t`` data-sharing scope value.

Recognized values map to a human-readable string:

* ``PMIX_SCOPE_UNDEF`` |mdash| ``"UNDEFINED"``
* ``PMIX_LOCAL`` |mdash| ``"SHARE ON LOCAL NODE ONLY"``
* ``PMIX_REMOTE`` |mdash| ``"SHARE ON REMOTE NODES ONLY"``
* ``PMIX_GLOBAL`` |mdash| ``"SHARE ACROSS ALL NODES"``
* ``PMIX_INTERNAL`` |mdash| ``"STORE INTERNALLY"``


DESCRIPTION
-----------

Return a human-readable, statically-allocated string naming the given
``pmix_scope_t`` value. The returned string is owned by the library and must
not be modified or freed by the caller. An unrecognized value yields the
fallback string ``"UNKNOWN SCOPE"``.


RETURN VALUE
------------

Returns a pointer to a constant, statically-allocated, NUL-terminated string.
The pointer remains valid for the lifetime of the process and must not be
freed.


.. seealso::
   :ref:`PMIx_Put(3) <man3-PMIx_Put>`
