.. _man3-PMIx_Geometry_free:

PMIx_Geometry_free
==================

.. include_body

``PMIx_Geometry_free`` |mdash| Release an array of
:ref:`pmix_geometry_t(5) <man5-pmix_geometry_t>` structures.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_Geometry_free(pmix_geometry_t *g, size_t n);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``g``: Pointer to an array of :ref:`pmix_geometry_t(5) <man5-pmix_geometry_t>`
  structures previously returned by :ref:`PMIx_Geometry_create(3)
  <man3-PMIx_Geometry_create>`.
* ``n``: Number of structures in the array |mdash| must be the same count that
  was passed to :ref:`PMIx_Geometry_create(3) <man3-PMIx_Geometry_create>`.


DESCRIPTION
-----------

Release an array of :ref:`pmix_geometry_t <man5-pmix_geometry_t>` structures
that was allocated by :ref:`PMIx_Geometry_create(3)
<man3-PMIx_Geometry_create>`. The function destructs the contents of each of
the ``n`` elements |mdash| freeing the ``uuid``, ``osname``, and
``coordinates`` payloads as if by :ref:`PMIx_Geometry_destruct(3)
<man3-PMIx_Geometry_destruct>` |mdash| and then frees the array storage itself.

If ``g`` is ``NULL`` the function returns without action.


RETURN VALUE
------------

``PMIx_Geometry_free`` returns no value (``void``).


NOTES
-----

``PMIx_Geometry_free`` is the counterpart to :ref:`PMIx_Geometry_create(3)
<man3-PMIx_Geometry_create>`. Do not call it on a single structure that was
merely constructed with :ref:`PMIx_Geometry_construct(3)
<man3-PMIx_Geometry_construct>`; use :ref:`PMIx_Geometry_destruct(3)
<man3-PMIx_Geometry_destruct>` for that case, since the structure storage was
not allocated by the library.


.. seealso::
   :ref:`PMIx_Geometry_create(3) <man3-PMIx_Geometry_create>`,
   :ref:`PMIx_Geometry_construct(3) <man3-PMIx_Geometry_construct>`,
   :ref:`PMIx_Geometry_destruct(3) <man3-PMIx_Geometry_destruct>`,
   :ref:`pmix_geometry_t(5) <man5-pmix_geometry_t>`
