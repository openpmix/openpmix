.. _man3-PMIx_Job_state_string:

PMIx_Job_state_string
=====================

.. include_body

``PMIx_Job_state_string`` |mdash| Return the string representation of a
``pmix_job_state_t`` value.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   const char* PMIx_Job_state_string(pmix_job_state_t state);


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

  from pmix import *

  foo = PMIxClient()
  name = foo.job_state_string(PMIX_JOB_STATE_RUNNING)


INPUT PARAMETERS
----------------

* ``state``: A job state value of type ``pmix_job_state_t``. Recognized values
  include ``PMIX_JOB_STATE_UNDEF``, ``PMIX_JOB_STATE_PREPPED``,
  ``PMIX_JOB_STATE_LAUNCH_UNDERWAY``, ``PMIX_JOB_STATE_RUNNING``,
  ``PMIX_JOB_STATE_SUSPENDED``, ``PMIX_JOB_STATE_CONNECTED``,
  ``PMIX_JOB_STATE_UNTERMINATED``, ``PMIX_JOB_STATE_TERMINATED``, and
  ``PMIX_JOB_STATE_TERMINATED_WITH_ERROR``.


DESCRIPTION
-----------

Return a human-readable, statically-allocated string describing the given job
state |mdash| for example, ``"RUNNING"`` or ``"TERMINATED WITH ERROR"``. If the
value does not correspond to a recognized job state, the fallback string
``"UNKNOWN"`` is returned.

The returned pointer refers to constant storage owned by the PMIx library. The
caller must **not** modify or free it.


RETURN VALUE
------------

Returns a pointer to a constant, statically-allocated character string. The pointer
is always non-``NULL`` and must not be freed by the caller.


.. seealso::
   ``PMIx_IOF_channel_string(3)``,
   ``PMIx_Link_state_string(3)``
