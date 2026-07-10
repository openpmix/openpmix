.. _man3-PMIx_Cpuset_free:

PMIx_Cpuset_free
================

.. include_body

``PMIx_Cpuset_free`` |mdash| Release an array of :ref:`pmix_cpuset_t(5)
<man5-pmix_cpuset_t>` structures and its storage.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_Cpuset_free(pmix_cpuset_t *c, size_t n);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``c``: Pointer to the array of :ref:`pmix_cpuset_t(5) <man5-pmix_cpuset_t>`
  structures to be released, as returned by :ref:`PMIx_Cpuset_create(3)
  <man3-PMIx_Cpuset_create>`.
* ``n``: Number of structures in the array.


DESCRIPTION
-----------

Release an array of :ref:`pmix_cpuset_t <man5-pmix_cpuset_t>` structures that
was allocated by :ref:`PMIx_Cpuset_create(3) <man3-PMIx_Cpuset_create>`. Each
of the ``n`` elements is destructed (freeing its ``source`` string and
``bitmap``), and then the storage for the array itself is freed.

If ``c`` is ``NULL`` the function does nothing.


RETURN VALUE
------------

``PMIx_Cpuset_free`` returns no value (``void``).


NOTES
-----

``PMIx_Cpuset_free`` frees the array storage itself, so it must only be used on
arrays obtained from :ref:`PMIx_Cpuset_create(3) <man3-PMIx_Cpuset_create>`. To
release only the contents of a caller-provided (stack or embedded) structure
without freeing its storage, use :ref:`PMIx_Cpuset_destruct(3)
<man3-PMIx_Cpuset_destruct>` instead.

The convenience macro ``PMIX_CPUSET_FREE`` wraps this function and additionally
sets the caller's pointer to ``NULL``.


.. seealso::
   :ref:`PMIx_Cpuset_create(3) <man3-PMIx_Cpuset_create>`,
   :ref:`PMIx_Cpuset_construct(3) <man3-PMIx_Cpuset_construct>`,
   :ref:`PMIx_Cpuset_destruct(3) <man3-PMIx_Cpuset_destruct>`,
   :ref:`pmix_cpuset_t(5) <man5-pmix_cpuset_t>`
