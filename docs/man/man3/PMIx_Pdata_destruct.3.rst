.. _man3-PMIx_Pdata_destruct:

PMIx_Pdata_destruct
===================

.. include_body

``PMIx_Pdata_destruct`` |mdash| Release the contents of a
:ref:`pmix_pdata_t(5) <man5-pmix_pdata_t>` structure.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_Pdata_destruct(pmix_pdata_t *p);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``p``: Pointer to the :ref:`pmix_pdata_t(5) <man5-pmix_pdata_t>` structure
  whose contents are to be released.


DESCRIPTION
-----------

Release any dynamically allocated memory held by the ``value`` field of a
:ref:`pmix_pdata_t <man5-pmix_pdata_t>` structure. Only the payload contained
within the structure is freed; the storage for the ``pmix_pdata_t`` structure
itself is **not** freed, since that storage was provided by the caller.

``PMIx_Pdata_destruct`` is the counterpart to
:ref:`PMIx_Pdata_construct(3) <man3-PMIx_Pdata_construct>` and should be
applied to structures that were initialized with that routine (or otherwise
declared on the stack or embedded in another object). To release an array of
structures that was allocated by :ref:`PMIx_Pdata_create(3)
<man3-PMIx_Pdata_create>`, use :ref:`PMIx_Pdata_free(3)
<man3-PMIx_Pdata_free>` instead, which destructs each element **and** frees
the array storage.


RETURN VALUE
------------

``PMIx_Pdata_destruct`` returns no value (``void``).


EXAMPLES
--------

Construct, use, and destruct a structure:

.. code-block:: c

    pmix_pdata_t pdata;

    PMIx_Pdata_construct(&pdata);
    /* ... populate and use pdata ... */
    PMIx_Pdata_destruct(&pdata);


.. seealso::
   :ref:`PMIx_Pdata_construct(3) <man3-PMIx_Pdata_construct>`,
   :ref:`PMIx_Pdata_create(3) <man3-PMIx_Pdata_create>`,
   :ref:`PMIx_Pdata_free(3) <man3-PMIx_Pdata_free>`,
   :ref:`PMIx_Pdata_load(3) <man3-PMIx_Pdata_load>`,
   :ref:`PMIx_Pdata_xfer(3) <man3-PMIx_Pdata_xfer>`,
   :ref:`pmix_pdata_t(5) <man5-pmix_pdata_t>`
