.. _man3-PMIx_Envar_destruct:

PMIx_Envar_destruct
===================

.. include_body

``PMIx_Envar_destruct`` |mdash| Release the contents of a
:ref:`pmix_envar_t(5) <man5-pmix_envar_t>` structure.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_Envar_destruct(pmix_envar_t *e);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``e``: Pointer to the :ref:`pmix_envar_t(5) <man5-pmix_envar_t>` structure
  whose contents are to be released.


DESCRIPTION
-----------

Release the payload held by a :ref:`pmix_envar_t <man5-pmix_envar_t>`. If the
``envar`` string pointer is non-``NULL`` it is freed and reset to ``NULL``;
likewise for the ``value`` string pointer. The storage for the structure
itself is **not** freed |mdash| that remains the responsibility of the caller.

``PMIx_Envar_destruct`` is the counterpart to :ref:`PMIx_Envar_construct(3)
<man3-PMIx_Envar_construct>` and should be used to clean up any structure that
was constructed on caller-provided storage.


RETURN VALUE
------------

``PMIx_Envar_destruct`` returns no value (``void``).


NOTES
-----

To release an array of structures that was obtained from
:ref:`PMIx_Envar_create(3) <man3-PMIx_Envar_create>` |mdash| which also frees
the array storage itself |mdash| use :ref:`PMIx_Envar_free(3)
<man3-PMIx_Envar_free>` instead.


.. seealso::
   :ref:`PMIx_Envar_construct(3) <man3-PMIx_Envar_construct>`,
   :ref:`PMIx_Envar_create(3) <man3-PMIx_Envar_create>`,
   :ref:`PMIx_Envar_free(3) <man3-PMIx_Envar_free>`,
   :ref:`PMIx_Envar_load(3) <man3-PMIx_Envar_load>`,
   :ref:`pmix_envar_t(5) <man5-pmix_envar_t>`
