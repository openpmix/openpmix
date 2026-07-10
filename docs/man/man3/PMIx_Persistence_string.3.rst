.. _man3-PMIx_Persistence_string:

PMIx_Persistence_string
=======================

.. include_body

``PMIx_Persistence_string`` |mdash| Return the string representation of a
``pmix_persistence_t`` value.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   const char* PMIx_Persistence_string(pmix_persistence_t persist);


INPUT PARAMETERS
----------------

* ``persist`` : A ``pmix_persistence_t`` data-persistence value.

Recognized values map to a human-readable string:

* ``PMIX_PERSIST_INDEF`` |mdash| ``"INDEFINITE"``
* ``PMIX_PERSIST_FIRST_READ`` |mdash| ``"DELETE ON FIRST ACCESS"``
* ``PMIX_PERSIST_PROC`` |mdash| ``"RETAIN UNTIL PUBLISHING PROCESS TERMINATES"``
* ``PMIX_PERSIST_APP`` |mdash| ``"RETAIN UNTIL APPLICATION OF PUBLISHING PROCESS TERMINATES"``
* ``PMIX_PERSIST_SESSION`` |mdash| ``"RETAIN UNTIL ALLOCATION OF PUBLISHING PROCESS TERMINATES"``
* ``PMIX_PERSIST_INVALID`` |mdash| ``"INVALID"``


DESCRIPTION
-----------

Return a human-readable, statically-allocated string naming the given
``pmix_persistence_t`` value. The returned string is owned by the library and
must not be modified or freed by the caller. An unrecognized value yields the
fallback string ``"UNKNOWN PERSISTENCE"``.


RETURN VALUE
------------

Returns a pointer to a constant, statically-allocated, NUL-terminated string.
The pointer remains valid for the lifetime of the process and must not be
freed.


.. seealso::
   :ref:`PMIx_Publish(3) <man3-PMIx_Publish>`,
   :ref:`pmix_persistence_t(5) <man5-pmix_persistence_t>`
