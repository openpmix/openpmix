.. _man3-PMIx_Info_is_persistent:

PMIx_Info_is_persistent
=======================

.. include_body

``PMIx_Info_is_persistent`` |mdash| Test whether a :ref:`pmix_info_t(5) <man5-pmix_info_t>`
is marked as persistent.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   bool PMIx_Info_is_persistent(const pmix_info_t *p);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``p``: Pointer to the :ref:`pmix_info_t(5) <man5-pmix_info_t>` structure to test.


DESCRIPTION
-----------

Test whether the ``PMIX_INFO_PERSISTENT`` flag is set in the ``flags`` field of
the referenced :ref:`pmix_info_t <man5-pmix_info_t>` structure. A persistent
structure is one whose contained value must *not* be released when the
structure is destructed; the value is owned elsewhere and its lifetime is
managed independently of the ``pmix_info_t``.

The persistent flag is applied with
:ref:`PMIx_Info_persistent(3) <man3-PMIx_Info_persistent>`.


RETURN VALUE
------------

Returns ``true`` if the ``PMIX_INFO_PERSISTENT`` flag is set in the structure,
and ``false`` otherwise.


.. seealso::
   :ref:`PMIx_Info_persistent(3) <man3-PMIx_Info_persistent>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`
