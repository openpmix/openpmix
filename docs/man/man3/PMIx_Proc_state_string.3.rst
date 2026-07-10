.. _man3-PMIx_Proc_state_string:

PMIx_Proc_state_string
======================

.. include_body

``PMIx_Proc_state_string`` |mdash| Return the string representation of a
``pmix_proc_state_t`` value.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   const char* PMIx_Proc_state_string(pmix_proc_state_t state);


INPUT PARAMETERS
----------------

* ``state`` : A ``pmix_proc_state_t`` process-state value.

Recognized values map to a human-readable string, for example:

* ``PMIX_PROC_STATE_UNDEF`` |mdash| ``"UNDEFINED"``
* ``PMIX_PROC_STATE_RUNNING`` |mdash| ``"PROC EXECUTING"``
* ``PMIX_PROC_STATE_CONNECTED`` |mdash| ``"PROC HAS CONNECTED TO LOCAL PMIX SERVER"``
* ``PMIX_PROC_STATE_TERMINATED`` |mdash| ``"PROC HAS TERMINATED"``
* ``PMIX_PROC_STATE_ABORTED`` |mdash| ``"PROC ABNORMALLY ABORTED"``
* ``PMIX_PROC_STATE_CALLED_ABORT`` |mdash| ``"PROC CALLED PMIx_Abort"``


DESCRIPTION
-----------

Return a human-readable, statically-allocated string naming the given
``pmix_proc_state_t`` value. The returned string is owned by the library and
must not be modified or freed by the caller. An unrecognized value yields the
fallback string ``"UNKNOWN STATE"``.


RETURN VALUE
------------

Returns a pointer to a constant, statically-allocated, NUL-terminated string.
The pointer remains valid for the lifetime of the process and must not be
freed.


.. seealso::
   :ref:`PMIx_Register_event_handler(3) <man3-PMIx_Register_event_handler>`,
   :ref:`PMIx_Notify_event(3) <man3-PMIx_Notify_event>`,
   :ref:`pmix_proc_state_t(5) <man5-pmix_proc_state_t>`
