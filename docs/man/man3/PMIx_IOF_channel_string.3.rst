.. _man3-PMIx_IOF_channel_string:

PMIx_IOF_channel_string
=======================

.. include_body

``PMIx_IOF_channel_string`` |mdash| Return the string representation of a
``pmix_iof_channel_t`` value.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   const char* PMIx_IOF_channel_string(pmix_iof_channel_t channel);


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

  from pmix import *

  foo = PMIxClient()
  # channel is an integer bitmask of PMIX_FWD_* channel flags
  name = foo.iof_channel_string(PMIX_FWD_STDOUT_CHANNEL)


INPUT PARAMETERS
----------------

* ``channel``: An I/O forwarding channel specifier of type ``pmix_iof_channel_t``.
  This is a bitmask, so more than one channel flag may be set at once. Recognized
  flags include ``PMIX_FWD_STDIN_CHANNEL``, ``PMIX_FWD_STDOUT_CHANNEL``,
  ``PMIX_FWD_STDERR_CHANNEL``, and ``PMIX_FWD_STDDIAG_CHANNEL``.


DESCRIPTION
-----------

Return a human-readable, statically-allocated string describing the given I/O
forwarding channel bitmask. Because ``channel`` is a bitmask, the returned string
lists each recognized channel that is set, separated by spaces |mdash| for example,
``"STDOUT STDERR "`` when both the standard-output and standard-error channels are
selected. If no recognized channel bit is set, the string ``"NONE"`` is returned.

The returned pointer refers to storage owned by the PMIx library. The caller must
**not** modify or free it. The buffer is reused on subsequent calls, so a returned
string should be copied if it must persist across another call to this function.


RETURN VALUE
------------

Returns a pointer to a constant, statically-allocated character string. The pointer
is always non-``NULL`` and must not be freed by the caller.


.. seealso::
   ``PMIx_Job_state_string(3)``,
   ``PMIx_Device_type_string(3)``
