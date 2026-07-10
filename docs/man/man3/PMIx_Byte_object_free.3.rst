.. _man3-PMIx_Byte_object_free:

PMIx_Byte_object_free
=====================

.. include_body

``PMIx_Byte_object_free`` |mdash| Release an array of
:ref:`pmix_byte_object_t(5) <man5-pmix_byte_object_t>` structures and its
storage.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_Byte_object_free(pmix_byte_object_t *g, size_t n);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``g``: Pointer to the array of :ref:`pmix_byte_object_t(5)
  <man5-pmix_byte_object_t>` structures to be released, as returned by
  :ref:`PMIx_Byte_object_create(3) <man3-PMIx_Byte_object_create>`.
* ``n``: Number of structures in the array.


DESCRIPTION
-----------

Release an array of :ref:`pmix_byte_object_t <man5-pmix_byte_object_t>`
structures that was allocated by :ref:`PMIx_Byte_object_create(3)
<man3-PMIx_Byte_object_create>`. Each of the ``n`` elements is destructed
(freeing any ``bytes`` payload it holds), and then the storage for the array
itself is freed.

If ``g`` is ``NULL`` the function does nothing.


RETURN VALUE
------------

``PMIx_Byte_object_free`` returns no value (``void``).


NOTES
-----

``PMIx_Byte_object_free`` frees the array storage itself, so it must only be
used on arrays obtained from :ref:`PMIx_Byte_object_create(3)
<man3-PMIx_Byte_object_create>`. To release only the payload of a
caller-provided (stack or embedded) object without freeing its storage, use
:ref:`PMIx_Byte_object_destruct(3) <man3-PMIx_Byte_object_destruct>` instead.

The convenience macro ``PMIX_BYTE_OBJECT_FREE`` wraps this function and
additionally sets the caller's pointer to ``NULL``.


.. seealso::
   :ref:`PMIx_Byte_object_create(3) <man3-PMIx_Byte_object_create>`,
   :ref:`PMIx_Byte_object_construct(3) <man3-PMIx_Byte_object_construct>`,
   :ref:`PMIx_Byte_object_destruct(3) <man3-PMIx_Byte_object_destruct>`,
   :ref:`PMIx_Byte_object_load(3) <man3-PMIx_Byte_object_load>`,
   :ref:`pmix_byte_object_t(5) <man5-pmix_byte_object_t>`
