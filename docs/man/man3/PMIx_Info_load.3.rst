.. _man3-PMIx_Info_load:

PMIx_Info_load
==============

.. include_body

``PMIx_Info_load`` |mdash| Load a key and typed value into a :ref:`pmix_info_t(5) <man5-pmix_info_t>`


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_status_t PMIx_Info_load(pmix_info_t *info,
                                const char *key,
                                const void *data,
                                pmix_data_type_t type);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT/OUTPUT PARAMETERS
-----------------------

* ``info``: Pointer to the :ref:`pmix_info_t(5) <man5-pmix_info_t>` structure to
  be populated. The function first constructs (initializes) the structure, so
  any prior contents are overwritten, not released |mdash| the caller must ensure
  the structure holds no live payload that would otherwise leak.


INPUT PARAMETERS
----------------

* ``key``: The string key to store. Must not be ``NULL``.
* ``data``: Pointer to the value to load, interpreted according to ``type``.
  May be ``NULL`` for types that carry no data (e.g., ``PMIX_UNDEF``).
* ``type``: The :ref:`pmix_data_type_t(5) <man5-pmix_data_type_t>` describing
  the datum referenced by ``data``.


DESCRIPTION
-----------

The ``PMIx_Info_load`` function populates a :ref:`pmix_info_t(5) <man5-pmix_info_t>`
structure with a key and a typed value. It is equivalent to constructing the
structure, copying ``key`` into the info's ``key`` field, and then performing a
value-load of ``data`` (of the given ``type``) into the info's contained
:ref:`pmix_value_t(5) <man5-pmix_value_t>`.

The load **copies** the supplied data: the ``key`` string and the value payload
are duplicated into memory owned by the info structure, so the caller's ``key``
and ``data`` buffers may be freed or reused after the call returns. The memory
acquired by the info must subsequently be released with
:ref:`PMIx_Info_destruct(3) <man3-PMIx_Info_destruct>` (for a caller-owned
structure) or :ref:`PMIx_Info_free(3) <man3-PMIx_Info_free>` (for a
library-allocated array).


RETURN VALUE
------------

Returns ``PMIX_SUCCESS`` on success. On error, one of the negative PMIx error
constants is returned, including:

* ``PMIX_ERR_BAD_PARAM``: ``key`` was ``NULL``.

Other errors (for example ``PMIX_ERR_NOMEM`` or an unsupported-type error) may
be propagated from the underlying value-load operation.


NOTES
-----

``PMIx_Info_load`` is the OpenPMIx routine underlying the historical
``PMIX_INFO_LOAD`` macro. That macro is retained in ``pmix_deprecated.h`` for
backward compatibility and expands to a call to this function, discarding the
return status.


EXAMPLES
--------

Load a string value under a key:

.. code-block:: c

    pmix_info_t info;
    pmix_status_t rc;

    rc = PMIx_Info_load(&info, "pmix.example.key", "hello", PMIX_STRING);
    if (PMIX_SUCCESS != rc) {
        /* handle error */
    }
    /* ... use info ... */
    PMIx_Info_destruct(&info);


.. seealso::
   :ref:`PMIx_Info_construct(3) <man3-PMIx_Info_construct>`,
   :ref:`PMIx_Info_destruct(3) <man3-PMIx_Info_destruct>`,
   :ref:`PMIx_Info_xfer(3) <man3-PMIx_Info_xfer>`,
   :ref:`PMIx_Info_free(3) <man3-PMIx_Info_free>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`,
   :ref:`pmix_value_t(5) <man5-pmix_value_t>`,
   :ref:`pmix_data_type_t(5) <man5-pmix_data_type_t>`
