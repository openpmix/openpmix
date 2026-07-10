.. _man3-PMIx_Info_processed:

PMIx_Info_processed
===================

.. include_body

``PMIx_Info_processed`` |mdash| Mark a :ref:`pmix_info_t(5) <man5-pmix_info_t>` as processed


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_Info_processed(pmix_info_t *p);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``p``: Pointer to the :ref:`pmix_info_t(5) <man5-pmix_info_t>` structure to be
  marked as processed.


DESCRIPTION
-----------

The ``PMIx_Info_processed`` function sets the ``PMIX_INFO_REQD_PROCESSED``
directive bit in the info structure's flags field. This bit records that a
required directive carried by the info has already been acted upon, allowing
code that handles required attributes to note which ones have been satisfied and
avoid processing them a second time.

Whether an info has been marked processed can be tested with
:ref:`PMIx_Info_was_processed(3) <man3-PMIx_Info_was_processed>`.


RETURN VALUE
------------

``PMIx_Info_processed`` returns no value (``void``).


NOTES
-----

``PMIx_Info_processed`` is the OpenPMIx routine underlying the historical
``PMIX_INFO_PROCESSED`` macro. That macro is retained in ``pmix_deprecated.h``
for backward compatibility and simply expands to a call to this function.

This flag is closely associated with the required/optional directives; see
:ref:`PMIx_Info_required(3) <man3-PMIx_Info_required>`.


.. seealso::
   :ref:`PMIx_Info_was_processed(3) <man3-PMIx_Info_was_processed>`,
   :ref:`PMIx_Info_required(3) <man3-PMIx_Info_required>`,
   :ref:`PMIx_Info_optional(3) <man3-PMIx_Info_optional>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`
