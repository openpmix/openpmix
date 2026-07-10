.. _man3-PMIx_Info_was_processed:

PMIx_Info_was_processed
=======================

.. include_body

``PMIx_Info_was_processed`` |mdash| Test whether a required
:ref:`pmix_info_t(5) <man5-pmix_info_t>` has been processed.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   bool PMIx_Info_was_processed(const pmix_info_t *p);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``p``: Pointer to the :ref:`pmix_info_t(5) <man5-pmix_info_t>` structure to test.


DESCRIPTION
-----------

Test whether the ``PMIX_INFO_REQD_PROCESSED`` flag is set in the ``flags``
field of the referenced :ref:`pmix_info_t <man5-pmix_info_t>` structure. This
flag records that a required directive has already been acted upon, allowing a
receiver to confirm that every required attribute in an array was in fact
honored.

The processed flag is applied with
:ref:`PMIx_Info_processed(3) <man3-PMIx_Info_processed>`.


RETURN VALUE
------------

Returns ``true`` if the ``PMIX_INFO_REQD_PROCESSED`` flag is set in the
structure, and ``false`` otherwise.


.. seealso::
   :ref:`PMIx_Info_processed(3) <man3-PMIx_Info_processed>`,
   :ref:`PMIx_Info_is_required(3) <man3-PMIx_Info_is_required>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`
