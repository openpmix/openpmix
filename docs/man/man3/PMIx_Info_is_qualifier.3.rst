.. _man3-PMIx_Info_is_qualifier:

PMIx_Info_is_qualifier
======================

.. include_body

``PMIx_Info_is_qualifier`` |mdash| Test whether a :ref:`pmix_info_t(5) <man5-pmix_info_t>`
is marked as a qualifier.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   bool PMIx_Info_is_qualifier(const pmix_info_t *p);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``p``: Pointer to the :ref:`pmix_info_t(5) <man5-pmix_info_t>` structure to test.


DESCRIPTION
-----------

Test whether the ``PMIX_INFO_QUALIFIER`` flag is set in the ``flags`` field of
the referenced :ref:`pmix_info_t <man5-pmix_info_t>` structure. A qualifier is
an entry in a ``pmix_info_t`` array that further constrains or refines a
primary value rather than standing on its own |mdash| for example, narrowing
the scope of a query.

The qualifier flag is applied with
:ref:`PMIx_Info_qualifier(3) <man3-PMIx_Info_qualifier>`.


RETURN VALUE
------------

Returns ``true`` if the ``PMIX_INFO_QUALIFIER`` flag is set in the structure,
and ``false`` otherwise.


.. seealso::
   :ref:`PMIx_Info_qualifier(3) <man3-PMIx_Info_qualifier>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`
