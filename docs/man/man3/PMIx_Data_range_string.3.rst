.. _man3-PMIx_Data_range_string:

PMIx_Data_range_string
======================

.. include_body

``PMIx_Data_range_string`` |mdash| Return the string representation of a
``pmix_data_range_t`` value.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   const char* PMIx_Data_range_string(pmix_data_range_t range);


INPUT PARAMETERS
----------------

* ``range`` : A ``pmix_data_range_t`` data-range value.

Recognized values map to a human-readable string:

* ``PMIX_RANGE_UNDEF`` |mdash| ``"UNDEFINED"``
* ``PMIX_RANGE_RM`` |mdash| ``"INTENDED FOR HOST RESOURCE MANAGER ONLY"``
* ``PMIX_RANGE_LOCAL`` |mdash| ``"AVAIL ON LOCAL NODE ONLY"``
* ``PMIX_RANGE_NAMESPACE`` |mdash| ``"AVAIL TO PROCESSES IN SAME JOB ONLY"``
* ``PMIX_RANGE_SESSION`` |mdash| ``"AVAIL TO PROCESSES IN SAME ALLOCATION ONLY"``
* ``PMIX_RANGE_GLOBAL`` |mdash| ``"AVAIL TO ANYONE WITH AUTHORIZATION"``
* ``PMIX_RANGE_CUSTOM`` |mdash| ``"AVAIL AS SPECIFIED IN DIRECTIVES"``
* ``PMIX_RANGE_PROC_LOCAL`` |mdash| ``"AVAIL ON LOCAL PROC ONLY"``
* ``PMIX_RANGE_INVALID`` |mdash| ``"INVALID"``


DESCRIPTION
-----------

Return a human-readable, statically-allocated string naming the given
``pmix_data_range_t`` value. The returned string is owned by the library and
must not be modified or freed by the caller. An unrecognized value yields the
fallback string ``"UNKNOWN"``.


RETURN VALUE
------------

Returns a pointer to a constant, statically-allocated, NUL-terminated string.
The pointer remains valid for the lifetime of the process and must not be
freed.


.. seealso::
   :ref:`PMIx_Notify_event(3) <man3-PMIx_Notify_event>`,
   :ref:`PMIx_Publish(3) <man3-PMIx_Publish>`
