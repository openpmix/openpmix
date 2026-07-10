.. _man3-PMIx_Info_required:

PMIx_Info_required
==================

.. include_body

``PMIx_Info_required`` |mdash| Mark a :ref:`pmix_info_t(5) <man5-pmix_info_t>` as required


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_Info_required(pmix_info_t *p);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``p``: Pointer to the :ref:`pmix_info_t(5) <man5-pmix_info_t>` structure to be
  marked required.


DESCRIPTION
-----------

The ``PMIx_Info_required`` function sets the ``PMIX_INFO_REQD`` directive bit in
the info structure's flags field. Marking an info required tells the PMIx
implementation (and the host environment) that the directive it carries must be
honored: a request that cannot satisfy a required directive should fail rather
than silently ignore it.

This is the inverse of :ref:`PMIx_Info_optional(3) <man3-PMIx_Info_optional>`,
which clears the same bit. Whether an info is currently marked required can be
tested with :ref:`PMIx_Info_is_required(3) <man3-PMIx_Info_is_required>`.


RETURN VALUE
------------

``PMIx_Info_required`` returns no value (``void``).


NOTES
-----

``PMIx_Info_required`` is the OpenPMIx routine underlying the historical
``PMIX_INFO_REQUIRED`` macro. That macro is retained in ``pmix_deprecated.h``
for backward compatibility and simply expands to a call to this function.


.. seealso::
   :ref:`PMIx_Info_optional(3) <man3-PMIx_Info_optional>`,
   :ref:`PMIx_Info_is_required(3) <man3-PMIx_Info_is_required>`,
   :ref:`PMIx_Info_is_optional(3) <man3-PMIx_Info_is_optional>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`
