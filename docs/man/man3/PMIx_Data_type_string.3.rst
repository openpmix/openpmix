.. _man3-PMIx_Data_type_string:

PMIx_Data_type_string
=====================

.. include_body

``PMIx_Data_type_string`` |mdash| Return the string representation of a
``pmix_data_type_t`` value.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   const char* PMIx_Data_type_string(pmix_data_type_t type);


INPUT PARAMETERS
----------------

* ``type`` : A ``pmix_data_type_t`` data-type identifier (for example, the
  ``type`` field of a :ref:`pmix_value_t <man5-pmix_value_t>`).


DESCRIPTION
-----------

Return a human-readable, statically-allocated string naming the given
``pmix_data_type_t`` value. The lookup is delegated to the active ``bfrops``
serialization modules, each of which knows the data types defined for its
wire-format version; the first module that recognizes the type supplies the
name. The returned string is owned by the library and must not be modified or
freed by the caller. A value that no active module recognizes yields the
fallback string ``"UNKNOWN"``.


RETURN VALUE
------------

Returns a pointer to a constant, statically-allocated, NUL-terminated string.
The pointer remains valid for the lifetime of the process and must not be
freed.


.. seealso::
   :ref:`pmix_value_t(5) <man5-pmix_value_t>`,
   :ref:`pmix_data_type_t(5) <man5-pmix_data_type_t>`
