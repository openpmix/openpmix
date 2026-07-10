.. _man3-PMIx_Coord_free:

PMIx_Coord_free
===============

.. include_body

``PMIx_Coord_free`` |mdash| Release an array of :ref:`pmix_coord_t(5)
<man5-pmix_coord_t>` structures and its storage.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_Coord_free(pmix_coord_t *m, size_t number);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``m``: Pointer to the array of :ref:`pmix_coord_t(5) <man5-pmix_coord_t>`
  structures to be released, as returned by :ref:`PMIx_Coord_create(3)
  <man3-PMIx_Coord_create>`.
* ``number``: Number of structures in the array.


DESCRIPTION
-----------

Release an array of :ref:`pmix_coord_t <man5-pmix_coord_t>` structures that was
allocated by :ref:`PMIx_Coord_create(3) <man3-PMIx_Coord_create>`. Each of the
``number`` elements is destructed (freeing any ``coord`` array it holds), and
then the storage for the array itself is freed.

If ``m`` is ``NULL`` the function does nothing.


RETURN VALUE
------------

``PMIx_Coord_free`` returns no value (``void``).


NOTES
-----

``PMIx_Coord_free`` frees the array storage itself, so it must only be used on
arrays obtained from :ref:`PMIx_Coord_create(3) <man3-PMIx_Coord_create>`. To
release only the contents of a caller-provided (stack or embedded) structure
without freeing its storage, use :ref:`PMIx_Coord_destruct(3)
<man3-PMIx_Coord_destruct>` instead.

The convenience macro ``PMIX_COORD_FREE`` wraps this function and additionally
sets the caller's pointer to ``NULL``.


.. seealso::
   :ref:`PMIx_Coord_create(3) <man3-PMIx_Coord_create>`,
   :ref:`PMIx_Coord_construct(3) <man3-PMIx_Coord_construct>`,
   :ref:`PMIx_Coord_destruct(3) <man3-PMIx_Coord_destruct>`,
   :ref:`pmix_coord_t(5) <man5-pmix_coord_t>`
