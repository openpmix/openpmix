.. _man3-PMIx_Coord_construct:

PMIx_Coord_construct
====================

.. include_body

``PMIx_Coord_construct`` |mdash| Initialize a caller-provided
:ref:`pmix_coord_t(5) <man5-pmix_coord_t>` structure.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_Coord_construct(pmix_coord_t *m);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``m``: Pointer to the :ref:`pmix_coord_t(5) <man5-pmix_coord_t>` structure to
  be initialized. The storage for the structure itself must already exist
  |mdash| it is supplied by the caller (typically declared on the stack or
  embedded within another object).


DESCRIPTION
-----------

Initialize the fields of a :ref:`pmix_coord_t <man5-pmix_coord_t>` that the
caller has already allocated. The function sets the ``view`` field to
``PMIX_COORD_VIEW_UNDEF``, the ``coord`` pointer to ``NULL``, and the ``dims``
field to zero. No heap memory is allocated and no coordinate array is attached.


RETURN VALUE
------------

``PMIx_Coord_construct`` returns no value (``void``).


NOTES
-----

Because ``PMIx_Coord_construct`` operates on caller-provided storage, it must
be paired with :ref:`PMIx_Coord_destruct(3) <man3-PMIx_Coord_destruct>` to
release any coordinate array the structure subsequently acquires. Do **not**
pass a constructed (as opposed to created) structure to
:ref:`PMIx_Coord_free(3) <man3-PMIx_Coord_free>`, as that would attempt to free
storage the library did not allocate.

The convenience macro ``PMIX_COORD_CONSTRUCT`` is provided as a direct wrapper
around this function.


EXAMPLES
--------

Construct a structure:

.. code-block:: c

    pmix_coord_t coord;

    PMIx_Coord_construct(&coord);


.. seealso::
   :ref:`PMIx_Coord_destruct(3) <man3-PMIx_Coord_destruct>`,
   :ref:`PMIx_Coord_create(3) <man3-PMIx_Coord_create>`,
   :ref:`PMIx_Coord_free(3) <man3-PMIx_Coord_free>`,
   :ref:`pmix_coord_t(5) <man5-pmix_coord_t>`
