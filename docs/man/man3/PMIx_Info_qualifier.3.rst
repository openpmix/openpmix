.. _man3-PMIx_Info_qualifier:

PMIx_Info_qualifier
===================

.. include_body

``PMIx_Info_qualifier`` |mdash| Mark a :ref:`pmix_info_t(5) <man5-pmix_info_t>` as a qualifier


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_Info_qualifier(pmix_info_t *p);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``p``: Pointer to the :ref:`pmix_info_t(5) <man5-pmix_info_t>` structure to be
  marked as a qualifier.


DESCRIPTION
-----------

The ``PMIx_Info_qualifier`` function sets the ``PMIX_INFO_QUALIFIER`` directive
bit in the info structure's flags field. Marking an info as a qualifier
indicates that it does not stand on its own but instead refines or scopes a
primary value |mdash| for example, an info array passed to a query may contain a
primary key together with one or more qualifiers that narrow the request.

Whether an info is currently marked as a qualifier can be tested with
:ref:`PMIx_Info_is_qualifier(3) <man3-PMIx_Info_is_qualifier>`.


RETURN VALUE
------------

``PMIx_Info_qualifier`` returns no value (``void``).


NOTES
-----

``PMIx_Info_qualifier`` is the OpenPMIx routine underlying the historical
``PMIX_INFO_SET_QUALIFIER`` macro. That macro is retained in
``pmix_deprecated.h`` for backward compatibility and simply expands to a call to
this function.


.. seealso::
   :ref:`PMIx_Info_is_qualifier(3) <man3-PMIx_Info_is_qualifier>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`
