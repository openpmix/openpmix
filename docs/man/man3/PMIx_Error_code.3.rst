.. _man3-PMIx_Error_code:

PMIx_Error_code
===============

.. include_body

``PMIx_Error_code`` |mdash| Return the PMIx status code corresponding to a status
name string.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_status_t PMIx_Error_code(const char *errname);


INPUT PARAMETERS
----------------

* ``errname``: A NULL-terminated string naming a PMIx status or event constant,
  as would be returned by :ref:`PMIx_Error_string(3) <man3-PMIx_Error_string>`.


DESCRIPTION
-----------

Return the numeric ``pmix_status_t`` value whose symbolic name matches the
provided string. The function searches the library's table of known status and
event constants for an entry whose name matches ``errname`` and returns that
entry's code. The comparison is case-insensitive.

``PMIx_Error_code`` is the inverse of
:ref:`PMIx_Error_string(3) <man3-PMIx_Error_string>`: given a name produced by
that function, it recovers the corresponding numeric status value.

.. note::
   ``PMIx_Error_code`` is a library extension provided by OpenPMIx; it is not
   defined by the PMIx Standard.


RETURN VALUE
------------

Returns the ``pmix_status_t`` value corresponding to ``errname`` on success. If no
known status or event constant has a matching name, the function returns
``INT32_MIN`` to indicate that the name was not recognized.


NOTES
-----

Because a successful lookup can legitimately return a large negative value (PMIx
error codes are negative), callers that need to detect failure should test
explicitly for the ``INT32_MIN`` sentinel rather than assuming any negative return
denotes an unrecognized name.


.. seealso::
   :ref:`PMIx_Error_string(3) <man3-PMIx_Error_string>`,
   :ref:`PMIx_Get_version(3) <man3-PMIx_Get_version>`,
   :ref:`pmix_status_t(5) <man5-pmix_status_t>`
