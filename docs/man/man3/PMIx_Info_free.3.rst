.. _man3-PMIx_Info_free:

PMIx_Info_free
==============

.. include_body

``PMIx_Info_free`` |mdash| Release an array of :ref:`pmix_info_t(5) <man5-pmix_info_t>` structures


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_Info_free(pmix_info_t *p, size_t n);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``p``: Pointer to the array of :ref:`pmix_info_t(5) <man5-pmix_info_t>`
  structures to be released. This must be storage that was allocated by the
  library (e.g., via :ref:`PMIx_Info_create(3) <man3-PMIx_Info_create>`).
* ``n``: Number of structures in the array.


DESCRIPTION
-----------

The ``PMIx_Info_free`` function releases the contents of each element in an
array of :ref:`pmix_info_t(5) <man5-pmix_info_t>` structures and then frees the
array storage itself. Each element is destructed exactly as by
:ref:`PMIx_Info_destruct(3) <man3-PMIx_Info_destruct>` |mdash| the contained
:ref:`pmix_value_t(5) <man5-pmix_value_t>` payload is released unless the
element has been marked persistent |mdash| after which the block of memory
holding the array is returned to the system.

If ``p`` is ``NULL`` or ``n`` is zero, the function does nothing.

``PMIx_Info_free`` is the counterpart to
:ref:`PMIx_Info_create(3) <man3-PMIx_Info_create>`. Use it only on
library-allocated array storage; to release the contents of a single caller-owned
(stack or embedded) structure without freeing it, use
:ref:`PMIx_Info_destruct(3) <man3-PMIx_Info_destruct>` instead.


RETURN VALUE
------------

``PMIx_Info_free`` returns no value (``void``).


NOTES
-----

``PMIx_Info_free`` is the OpenPMIx routine underlying the historical
``PMIX_INFO_FREE`` macro. That macro is retained in ``pmix_deprecated.h`` for
backward compatibility; it calls this function and then sets the caller's
pointer to ``NULL``.


EXAMPLES
--------

Allocate and later release an array:

.. code-block:: c

    pmix_info_t *info;

    info = PMIx_Info_create(3);
    /* ... populate and use the array ... */
    PMIx_Info_free(info, 3);


.. seealso::
   :ref:`PMIx_Info_create(3) <man3-PMIx_Info_create>`,
   :ref:`PMIx_Info_construct(3) <man3-PMIx_Info_construct>`,
   :ref:`PMIx_Info_destruct(3) <man3-PMIx_Info_destruct>`,
   :ref:`PMIx_Info_persistent(3) <man3-PMIx_Info_persistent>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`,
   :ref:`pmix_value_t(5) <man5-pmix_value_t>`
