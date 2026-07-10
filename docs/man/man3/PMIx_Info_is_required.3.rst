.. _man3-PMIx_Info_is_required:

PMIx_Info_is_required
=====================

.. include_body

``PMIx_Info_is_required`` |mdash| Test whether a :ref:`pmix_info_t(5) <man5-pmix_info_t>`
is marked as required.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   bool PMIx_Info_is_required(const pmix_info_t *p);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``p``: Pointer to the :ref:`pmix_info_t(5) <man5-pmix_info_t>` structure to test.


DESCRIPTION
-----------

Test whether the ``PMIX_INFO_REQD`` flag is set in the ``flags`` field of the
referenced :ref:`pmix_info_t <man5-pmix_info_t>` structure. A required
directive is one that the receiving process is obligated to honor; if it cannot
be satisfied, the associated operation is expected to fail rather than proceed
while ignoring the directive.

The required flag is applied with
:ref:`PMIx_Info_required(3) <man3-PMIx_Info_required>` and cleared with
:ref:`PMIx_Info_optional(3) <man3-PMIx_Info_optional>`.


RETURN VALUE
------------

Returns ``true`` if the ``PMIX_INFO_REQD`` flag is set in the structure, and
``false`` otherwise.


.. seealso::
   :ref:`PMIx_Info_required(3) <man3-PMIx_Info_required>`,
   :ref:`PMIx_Info_optional(3) <man3-PMIx_Info_optional>`,
   :ref:`PMIx_Info_is_optional(3) <man3-PMIx_Info_is_optional>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`
