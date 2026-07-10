.. _man3-PMIx_Resource_unit_construct:

PMIx_Resource_unit_construct
============================

.. include_body

``PMIx_Resource_unit_construct`` |mdash| Initialize a caller-provided
:ref:`pmix_resource_unit_t(5) <man5-pmix_resource_unit_t>` structure.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_Resource_unit_construct(pmix_resource_unit_t *d);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``d``: Pointer to the :ref:`pmix_resource_unit_t(5)
  <man5-pmix_resource_unit_t>` structure to be initialized. The storage for the
  structure itself must already exist |mdash| it is supplied by the caller
  (typically declared on the stack or embedded within another object).


DESCRIPTION
-----------

Initialize the memory of a :ref:`pmix_resource_unit_t
<man5-pmix_resource_unit_t>` that the caller has already allocated. The
function zeroes the structure, setting the ``count`` field to zero and the
``type`` field to ``PMIX_DEVTYPE_UNKNOWN``. No heap memory is allocated.

Because ``PMIx_Resource_unit_construct`` operates on caller-provided storage,
it should be paired with :ref:`PMIx_Resource_unit_destruct(3)
<man3-PMIx_Resource_unit_destruct>`. Do **not** pass a constructed (as opposed
to created) structure to :ref:`PMIx_Resource_unit_free(3)
<man3-PMIx_Resource_unit_free>`, as that would attempt to free storage the
library did not allocate.


RETURN VALUE
------------

``PMIx_Resource_unit_construct`` returns no value (``void``).


EXAMPLES
--------

Construct a structure:

.. code-block:: c

    pmix_resource_unit_t unit;

    PMIx_Resource_unit_construct(&unit);


.. seealso::
   :ref:`PMIx_Resource_unit_destruct(3) <man3-PMIx_Resource_unit_destruct>`,
   :ref:`PMIx_Resource_unit_create(3) <man3-PMIx_Resource_unit_create>`,
   :ref:`PMIx_Resource_unit_free(3) <man3-PMIx_Resource_unit_free>`,
   :ref:`pmix_resource_unit_t(5) <man5-pmix_resource_unit_t>`
