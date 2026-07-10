.. _man3-PMIx_Envar_free:

PMIx_Envar_free
===============

.. include_body

``PMIx_Envar_free`` |mdash| Release an array of
:ref:`pmix_envar_t(5) <man5-pmix_envar_t>` structures.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_Envar_free(pmix_envar_t *e, size_t n);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``e``: Pointer to the array of :ref:`pmix_envar_t(5) <man5-pmix_envar_t>`
  structures to be released.
* ``n``: Number of structures in the array.


DESCRIPTION
-----------

Release an array of :ref:`pmix_envar_t <man5-pmix_envar_t>` structures that was
previously obtained from :ref:`PMIx_Envar_create(3) <man3-PMIx_Envar_create>`.
Each of the ``n`` elements is destructed |mdash| its ``envar`` and ``value``
strings are freed |mdash| and then the array storage itself is freed.

If ``e`` is ``NULL``, the call is a no-op.


RETURN VALUE
------------

``PMIx_Envar_free`` returns no value (``void``).


NOTES
-----

``PMIx_Envar_free`` is the counterpart to :ref:`PMIx_Envar_create(3)
<man3-PMIx_Envar_create>`. It both destructs the contents of each element and
frees the block of memory holding the array. To release only the contents of a
single caller-owned structure without freeing its storage, use
:ref:`PMIx_Envar_destruct(3) <man3-PMIx_Envar_destruct>` instead.


.. seealso::
   :ref:`PMIx_Envar_create(3) <man3-PMIx_Envar_create>`,
   :ref:`PMIx_Envar_construct(3) <man3-PMIx_Envar_construct>`,
   :ref:`PMIx_Envar_destruct(3) <man3-PMIx_Envar_destruct>`,
   :ref:`PMIx_Envar_load(3) <man3-PMIx_Envar_load>`,
   :ref:`pmix_envar_t(5) <man5-pmix_envar_t>`
