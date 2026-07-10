.. _man3-PMIx_Info_is_optional:

PMIx_Info_is_optional
=====================

.. include_body

``PMIx_Info_is_optional`` |mdash| Test whether a :ref:`pmix_info_t(5) <man5-pmix_info_t>`
is marked as optional.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   bool PMIx_Info_is_optional(const pmix_info_t *p);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``p``: Pointer to the :ref:`pmix_info_t(5) <man5-pmix_info_t>` structure to test.


DESCRIPTION
-----------

Test whether the referenced :ref:`pmix_info_t <man5-pmix_info_t>` structure is
optional. An optional directive is one the receiving process may honor if it is
able, but is not obligated to satisfy; the associated operation is expected to
proceed even when the directive cannot be met.

Optional is the complement of required: this routine returns ``true`` precisely
when the ``PMIX_INFO_REQD`` flag is *not* set in the ``flags`` field. It is
therefore the logical negation of
:ref:`PMIx_Info_is_required(3) <man3-PMIx_Info_is_required>`. The optional state
is applied with :ref:`PMIx_Info_optional(3) <man3-PMIx_Info_optional>`.


RETURN VALUE
------------

Returns ``true`` if the ``PMIX_INFO_REQD`` flag is *not* set in the structure,
and ``false`` if it is set.


.. seealso::
   :ref:`PMIx_Info_optional(3) <man3-PMIx_Info_optional>`,
   :ref:`PMIx_Info_required(3) <man3-PMIx_Info_required>`,
   :ref:`PMIx_Info_is_required(3) <man3-PMIx_Info_is_required>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`
