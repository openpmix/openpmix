.. _man3-PMIx_Coord_create:

PMIx_Coord_create
=================

.. include_body

``PMIx_Coord_create`` |mdash| Allocate and initialize an array of
:ref:`pmix_coord_t(5) <man5-pmix_coord_t>` structures.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_coord_t* PMIx_Coord_create(size_t dims,
                                   size_t number);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``dims``: Number of dimensions to allocate for the coordinate array of the
  created structure(s).
* ``number``: Number of :ref:`pmix_coord_t(5) <man5-pmix_coord_t>` structures
  to allocate.


DESCRIPTION
-----------

Allocate a contiguous array of ``number`` :ref:`pmix_coord_t
<man5-pmix_coord_t>` structures on the heap. The ``view`` field is set to
``PMIX_COORD_VIEW_UNDEF`` and the ``dims`` field to ``dims``. When ``dims`` is
non-zero, a ``dims``-element array of ``uint32_t`` coordinate values is
allocated and zero-initialized and attached via the ``coord`` pointer; when
``dims`` is zero, ``coord`` is left ``NULL``.

If ``number`` is zero, no allocation is performed and ``NULL`` is returned.


RETURN VALUE
------------

Returns a pointer to the newly allocated array, or ``NULL`` if ``number`` is
zero or the allocation fails.


NOTES
-----

An array obtained from ``PMIx_Coord_create`` must be released with
:ref:`PMIx_Coord_free(3) <man3-PMIx_Coord_free>`, which destructs each
element's coordinate array and frees the array storage itself. Pass the same
``number`` used at creation.

The convenience macro ``PMIX_COORD_CREATE`` is provided as a wrapper around
this function.


EXAMPLES
--------

Create a single three-dimensional coordinate and release it:

.. code-block:: c

    pmix_coord_t *coord;

    coord = PMIx_Coord_create(3, 1);
    coord->view = PMIX_COORD_LOGICAL_VIEW;
    coord->coord[0] = 0;
    coord->coord[1] = 1;
    coord->coord[2] = 2;
    /* ... use the coordinate ... */
    PMIx_Coord_free(coord, 1);


.. seealso::
   :ref:`PMIx_Coord_free(3) <man3-PMIx_Coord_free>`,
   :ref:`PMIx_Coord_construct(3) <man3-PMIx_Coord_construct>`,
   :ref:`PMIx_Coord_destruct(3) <man3-PMIx_Coord_destruct>`,
   :ref:`pmix_coord_t(5) <man5-pmix_coord_t>`
