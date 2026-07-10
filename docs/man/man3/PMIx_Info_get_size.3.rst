.. _man3-PMIx_Info_get_size:

PMIx_Info_get_size
==================

.. include_body

``PMIx_Info_get_size`` |mdash| Compute the size (in bytes) of the data payload
in a :ref:`pmix_info_t(5) <man5-pmix_info_t>` structure.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_status_t PMIx_Info_get_size(const pmix_info_t *val,
                                    size_t *size);


INPUT PARAMETERS
----------------

* ``val``: Pointer to the :ref:`pmix_info_t(5) <man5-pmix_info_t>` whose payload
  size is to be computed.


OUTPUT PARAMETERS
-----------------

* ``size``: Pointer to a ``size_t`` variable where the computed size, in bytes,
  will be returned.


DESCRIPTION
-----------

Compute and return the size, in bytes, of the data payload contained in a
:ref:`pmix_info_t(5) <man5-pmix_info_t>` structure. A
:ref:`pmix_info_t(5) <man5-pmix_info_t>` associates a ``key`` string with a
:ref:`pmix_value_t(5) <man5-pmix_value_t>`, so the returned size accounts for
both components:

* The size of the contained :ref:`pmix_value_t(5) <man5-pmix_value_t>` payload,
  computed as described for
  :ref:`PMIx_Value_get_size(3) <man3-PMIx_Value_get_size>`.

* The length of the ``key`` string, up to ``PMIX_MAX_KEYLEN`` characters,
  including the terminating ``NULL`` where present.

The size of the :ref:`pmix_info_t(5) <man5-pmix_info_t>` structure itself is also
added, so ``size`` reflects the total storage associated with the info, not just
its payload.

The source :ref:`pmix_info_t(5) <man5-pmix_info_t>` is not altered by this
function.


RETURN VALUE
------------

Returns ``PMIX_SUCCESS`` on success. On error, a negative value corresponding to
a PMIx error constant is returned, including:

* ``PMIX_ERR_UNKNOWN_DATA_TYPE`` |mdash| the ``type`` field of the contained
  value is ``PMIX_UNDEF`` or otherwise not a recognized PMIx data type, so the
  size could not be computed.

Any other negative value indicates an appropriate error condition. PMIx error
constants are defined in ``pmix_common.h``.


NOTES
-----

The returned size includes the size of the
:ref:`pmix_info_t(5) <man5-pmix_info_t>` structure itself, the length of its
``key``, and the size of its contained
:ref:`pmix_value_t(5) <man5-pmix_value_t>` payload. The value payload is sized by
delegating to :ref:`PMIx_Value_get_size(3) <man3-PMIx_Value_get_size>`.


.. seealso::
   :ref:`PMIx_Value_get_size(3) <man3-PMIx_Value_get_size>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`,
   :ref:`pmix_value_t(5) <man5-pmix_value_t>`
