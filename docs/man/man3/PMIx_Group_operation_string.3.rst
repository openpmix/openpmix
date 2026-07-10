.. _man3-PMIx_Group_operation_string:

PMIx_Group_operation_string
===========================

.. include_body

``PMIx_Group_operation_string`` |mdash| Return the string representation of a
``pmix_group_operation_t`` value.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   const char* PMIx_Group_operation_string(pmix_group_operation_t op);


INPUT PARAMETERS
----------------

* ``op``: A group operation value of type ``pmix_group_operation_t``. Recognized
  values are ``PMIX_GROUP_NONE``, ``PMIX_GROUP_CONSTRUCT``, ``PMIX_GROUP_DESTRUCT``,
  and ``PMIX_GROUP_CANCEL``.


DESCRIPTION
-----------

Return a human-readable, statically-allocated string describing the given group
operation. The recognized operations map to ``"CONSTRUCT"``, ``"DESTRUCT"``,
``"NONE"``, and ``"CANCEL"``. If the value does not correspond to a recognized
group operation, the fallback string ``"UNKNOWN VALUE"`` is returned.

The returned pointer refers to constant storage owned by the PMIx library. The
caller must **not** modify or free it.


RETURN VALUE
------------

Returns a pointer to a constant, statically-allocated character string. The pointer
is always non-``NULL`` and must not be freed by the caller.


.. seealso::
   :ref:`PMIx_Group_construct(3) <man3-PMIx_Group_construct>`,
   :ref:`PMIx_Group_destruct(3) <man3-PMIx_Group_destruct>`,
   :ref:`pmix_group_operation_t(5) <man5-pmix_group_operation_t>`
