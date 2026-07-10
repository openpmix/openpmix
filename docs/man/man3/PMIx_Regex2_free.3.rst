.. _man3-PMIx_Regex2_free:

PMIx_Regex2_free
================

.. include_body

``PMIx_Regex2_free`` |mdash| Release an array of :ref:`pmix_regex2_t(5) <man5-pmix_regex2_t>` structures.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_Regex2_free(pmix_regex2_t *d, size_t n);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``d``: Pointer to the array of ``pmix_regex2_t`` structures to be released.
* ``n``: Number of structures in the array.


DESCRIPTION
-----------

Release an array of ``pmix_regex2_t`` structures that was previously obtained
from :ref:`PMIx_Regex2_create(3) <man3-PMIx_Regex2_create>`. Each of the ``n``
elements is destructed |mdash| its ``type`` and ``bytes`` allocations are freed
|mdash| and then the array storage itself is freed.

If ``d`` is ``NULL``, the call is a no-op.


RETURN VALUE
------------

``PMIx_Regex2_free`` returns no value (``void``).


NOTES
-----

``PMIx_Regex2_free`` is the counterpart to :ref:`PMIx_Regex2_create(3)
<man3-PMIx_Regex2_create>`. It both destructs the contents of each element and
frees the block of memory holding the array. To release only the contents of a
single caller-owned structure without freeing its storage, use
:ref:`PMIx_Regex2_destruct(3) <man3-PMIx_Regex2_destruct>` instead.


.. seealso::
   :ref:`PMIx_Regex2_create(3) <man3-PMIx_Regex2_create>`,
   :ref:`PMIx_Regex2_construct(3) <man3-PMIx_Regex2_construct>`,
   :ref:`PMIx_Regex2_destruct(3) <man3-PMIx_Regex2_destruct>`,
   :ref:`pmix_regex2_t(5) <man5-pmix_regex2_t>`
