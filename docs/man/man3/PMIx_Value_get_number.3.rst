.. _man3-PMIx_Value_get_number:

PMIx_Value_get_number
=====================

.. include_body

`PMIx_Value_get_number` |mdash| Extract a numerical value from a ``pmix_value_t``

SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_status_t PMIx_Value_get_number(const pmix_value_t *val,
                                       void *dest,
                                       pmix_data_type_t type;

Python Syntax
^^^^^^^^^^^^^

No Python equivalent - the ``pmix_value_t`` equivalent in Python is a
dictionary containing a ``value`` and ``type`` field. It is therefore
unnecessary to provide an equivalent function.


INPUT PARAMETERS
----------------

* ``val``: Pointer to a :ref:`pmix_value_t(5) <man5-pmix_value_t>` struct containing the
  numerical value to be extracted. Note that the provided struct will not
  be altered in any way
* ``data``: Pointer to the location where the data value is to be
  returned
* ``type``: Expected PMIx datatype of the extracted value


DESCRIPTION
-----------

The `PMIx_Value_get_number` function compares the datatype provided with
the datatype found in the given `val`, taking one of the following actions:

* If either the datatype in `val` or the datatype provided by the caller is
  not numeric, then a ``PMIX_ERR_BAD_PARAM`` error will be returned.

* If the two datatypes match, then the value stored in the ``pmix_value_t`` will be
  returned in the storage pointed to by `data`.

* If the data stored in `val` is numeric but of a different datatype than
  the one given, then:

    * if the data in `val` differs in sign from that provided by the
      caller (e.g., if `val` contains a negative number and the caller
      specified ``PMIX_UINT32``), then the ``PMIX_ERR_CHANGE_SIGN`` error
      will be returned
    * if the data in `val` can fit into the specified datatype, then the
      the data shall be returned. For example, if the datatype in `val` is
      ``PMIX_INT32`` but the datatype provided by the caller is ``PMIX_INT8``,
      the value will be returned if the numeric value is within the range
      of an 8-bit integer
    * if the data in `val` is a floating point (either `float` or `double`) value,
      but the caller specifies a non-floating point datatype,
      the value will be returned in the caller's  storage
      if it fits within the range of that integer type - the value
      will be cast to the matching type before being returned. Note that any fractional
      portion of the value is lost. If the value lies
      outside the range of the integer type, then the ``PMIX_ERR_LOST_PRECISION``
      error will be returned

.. note:: The caller must provide backing memory for the value being
          returned. This function will not allocate memory. In addition, this
          function is only usable when retrieving numerical values - the :ref:`PMIx_Value_unload(3) <man3-PMIx_Value_unload>`
          function should be used for all non-numerical values


RETURN VALUE
------------

Returns ``PMIX_SUCCESS`` on success. On error, a negative value
corresponding to a PMIx ``errno`` is returned.

EXAMPLES
--------

Unloading an int32_t:

.. code-block:: c

    pmix_value_t val;
    int32_t i32;
    pmix_status_t rc;

    val.type = PMIX_INT32;
    val.data.int32 = 1234;

    rc = PMIx_Value_get_number(&val, (void*)&i32, PMIX_INT32);

On return of ``PMIX_SUCCESS``, the variable `i32` will contain a
value of `1234`. Similarly, the following use-case:

.. code-block:: c

    pmix_value_t val;
    int16_t i16;
    pmix_status_t rc;

    val.type = PMIX_INT32;
    val.data.int32 = 1234;

    rc = PMIx_Value_get_number(&val, (void*)&i16, PMIX_INT16);

would also return ``PMIX_SUCCESS``, with the variable `i16` containing a
value of `1234` because that value lies within the range of an ``int16_t``.

Similarly:

.. code-block:: c

    pmix_value_t val;
    int32_t i32;
    pmix_status_t rc;

    val.type = PMIX_FLOAT;
    val.data.fval = -1234.567;

    rc = PMIx_Value_get_number(&val, (void*)&i32, PMIX_INT32);

would return ``PMIX_SUCCESS``, with the variable `i32` containing a
value of `-1234` which truncates the floating number,
but:

.. code-block:: c

    pmix_value_t val;
    uint32_t u32;
    pmix_status_t rc;

    val.type = PMIX_FLOAT;
    val.data.fval = -1234.567;

    rc = PMIx_Value_get_number(&val, (void*)&u32, PMIX_UINT32);

would return ``PMIX_ERR_CHANGE_SIGN`` as the sign requirements conflict.



.. seealso::
   :ref:`pmix_value_t(5) <man5-pmix_value_t>`
