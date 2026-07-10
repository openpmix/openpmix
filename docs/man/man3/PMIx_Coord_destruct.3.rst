.. _man3-PMIx_Coord_destruct:

PMIx_Coord_destruct
===================

.. include_body

``PMIx_Coord_destruct`` |mdash| Release the contents of a
:ref:`pmix_coord_t(5) <man5-pmix_coord_t>` structure.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_Coord_destruct(pmix_coord_t *m);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``m``: Pointer to the :ref:`pmix_coord_t(5) <man5-pmix_coord_t>` structure
  whose contents are to be released.


DESCRIPTION
-----------

Release the contents of a :ref:`pmix_coord_t <man5-pmix_coord_t>`. The ``view``
field is reset to ``PMIX_COORD_VIEW_UNDEF``. If the ``coord`` pointer is
non-``NULL``, the coordinate array it references is freed, the ``coord``
pointer is reset to ``NULL``, and the ``dims`` field is set to zero.

Only the contents are released; the storage for the structure itself is not
freed. This routine is therefore the correct counterpart to
:ref:`PMIx_Coord_construct(3) <man3-PMIx_Coord_construct>` for caller-provided
(stack or embedded) structures.


RETURN VALUE
------------

``PMIx_Coord_destruct`` returns no value (``void``).


NOTES
-----

To release an array of coordinate structures obtained from
:ref:`PMIx_Coord_create(3) <man3-PMIx_Coord_create>`, including the array
storage itself, use :ref:`PMIx_Coord_free(3) <man3-PMIx_Coord_free>` instead.

The convenience macro ``PMIX_COORD_DESTRUCT`` is provided as a direct wrapper
around this function.


.. seealso::
   :ref:`PMIx_Coord_construct(3) <man3-PMIx_Coord_construct>`,
   :ref:`PMIx_Coord_create(3) <man3-PMIx_Coord_create>`,
   :ref:`PMIx_Coord_free(3) <man3-PMIx_Coord_free>`,
   :ref:`pmix_coord_t(5) <man5-pmix_coord_t>`
