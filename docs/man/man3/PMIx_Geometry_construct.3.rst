.. _man3-PMIx_Geometry_construct:

PMIx_Geometry_construct
=======================

.. include_body

``PMIx_Geometry_construct`` |mdash| Initialize a caller-provided
:ref:`pmix_geometry_t(5) <man5-pmix_geometry_t>` structure.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_Geometry_construct(pmix_geometry_t *g);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``g``: Pointer to the :ref:`pmix_geometry_t(5) <man5-pmix_geometry_t>`
  structure to be initialized. The storage for the structure itself must
  already exist |mdash| it is supplied by the caller (typically declared on
  the stack or embedded within another object).


DESCRIPTION
-----------

Initialize the memory of a :ref:`pmix_geometry_t <man5-pmix_geometry_t>` that
the caller has already allocated. The function zeroes the entire structure,
clearing the ``fabric`` index, the ``uuid`` and ``osname`` string pointers, the
``coordinates`` array pointer, and the ``ncoords`` count.

No heap memory is allocated and no fabric coordinates are attached; the caller
is responsible for populating the fields after construction.


RETURN VALUE
------------

``PMIx_Geometry_construct`` returns no value (``void``).


NOTES
-----

Because ``PMIx_Geometry_construct`` operates on caller-provided storage, it
must be paired with :ref:`PMIx_Geometry_destruct(3)
<man3-PMIx_Geometry_destruct>` to release any payload (the ``uuid``,
``osname``, and ``coordinates`` fields) the structure subsequently acquires. Do
**not** pass a constructed (as opposed to created) structure to
:ref:`PMIx_Geometry_free(3) <man3-PMIx_Geometry_free>`, as that would attempt
to free storage the library did not allocate.

An equivalent static initializer, ``PMIX_GEOMETRY_STATIC_INIT``, is available
for initializing a structure at the point of declaration.


EXAMPLES
--------

Construct a structure:

.. code-block:: c

    pmix_geometry_t geo;

    PMIx_Geometry_construct(&geo);


.. seealso::
   :ref:`PMIx_Geometry_destruct(3) <man3-PMIx_Geometry_destruct>`,
   :ref:`PMIx_Geometry_create(3) <man3-PMIx_Geometry_create>`,
   :ref:`PMIx_Geometry_free(3) <man3-PMIx_Geometry_free>`,
   :ref:`pmix_geometry_t(5) <man5-pmix_geometry_t>`
