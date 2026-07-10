.. _man3-PMIx_Resource_unit_destruct:

PMIx_Resource_unit_destruct
===========================

.. include_body

``PMIx_Resource_unit_destruct`` |mdash| Release the contents of a
:ref:`pmix_resource_unit_t(5) <man5-pmix_resource_unit_t>` structure.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_Resource_unit_destruct(pmix_resource_unit_t *d);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``d``: Pointer to the :ref:`pmix_resource_unit_t(5)
  <man5-pmix_resource_unit_t>` structure whose contents are to be released.


DESCRIPTION
-----------

Release any dynamically allocated memory held by a
:ref:`pmix_resource_unit_t <man5-pmix_resource_unit_t>` structure. The storage
for the structure itself is **not** freed, since that storage was provided by
the caller.

A :ref:`pmix_resource_unit_t <man5-pmix_resource_unit_t>` contains only scalar
fields (a ``type`` and a ``count``) and owns no heap-allocated payload, so this
routine currently performs no work. It is provided for symmetry with
:ref:`PMIx_Resource_unit_construct(3) <man3-PMIx_Resource_unit_construct>` and
so that correct calling code remains valid should the structure gain
owned resources in a future release. Well-behaved code should continue to call
it.

To release an array of structures allocated by
:ref:`PMIx_Resource_unit_create(3) <man3-PMIx_Resource_unit_create>`, use
:ref:`PMIx_Resource_unit_free(3) <man3-PMIx_Resource_unit_free>` instead, which
destructs each element **and** frees the array storage.


RETURN VALUE
------------

``PMIx_Resource_unit_destruct`` returns no value (``void``).


.. seealso::
   :ref:`PMIx_Resource_unit_construct(3) <man3-PMIx_Resource_unit_construct>`,
   :ref:`PMIx_Resource_unit_create(3) <man3-PMIx_Resource_unit_create>`,
   :ref:`PMIx_Resource_unit_free(3) <man3-PMIx_Resource_unit_free>`,
   :ref:`pmix_resource_unit_t(5) <man5-pmix_resource_unit_t>`
