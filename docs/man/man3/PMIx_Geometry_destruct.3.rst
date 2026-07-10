.. _man3-PMIx_Geometry_destruct:

PMIx_Geometry_destruct
======================

.. include_body

``PMIx_Geometry_destruct`` |mdash| Release the contents of a
:ref:`pmix_geometry_t(5) <man5-pmix_geometry_t>` structure.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_Geometry_destruct(pmix_geometry_t *g);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``g``: Pointer to the :ref:`pmix_geometry_t(5) <man5-pmix_geometry_t>`
  structure whose contents are to be released.


DESCRIPTION
-----------

Release the dynamically allocated payload held within a
:ref:`pmix_geometry_t <man5-pmix_geometry_t>` structure. The function frees the
``uuid`` and ``osname`` strings (setting each pointer to ``NULL``) and releases
the ``coordinates`` array via the coordinate-free path.

``PMIx_Geometry_destruct`` releases only the *contents* of the structure; it
does **not** free the storage of the ``pmix_geometry_t`` structure itself. When
the structure was allocated on the stack or embedded in another object, that
storage is reclaimed by its owner.


RETURN VALUE
------------

``PMIx_Geometry_destruct`` returns no value (``void``).


NOTES
-----

``PMIx_Geometry_destruct`` is the counterpart to :ref:`PMIx_Geometry_construct(3)
<man3-PMIx_Geometry_construct>`. To release an array of structures that was
allocated by :ref:`PMIx_Geometry_create(3) <man3-PMIx_Geometry_create>`,
including the array storage itself, use :ref:`PMIx_Geometry_free(3)
<man3-PMIx_Geometry_free>` instead.


.. seealso::
   :ref:`PMIx_Geometry_construct(3) <man3-PMIx_Geometry_construct>`,
   :ref:`PMIx_Geometry_create(3) <man3-PMIx_Geometry_create>`,
   :ref:`PMIx_Geometry_free(3) <man3-PMIx_Geometry_free>`,
   :ref:`pmix_geometry_t(5) <man5-pmix_geometry_t>`
