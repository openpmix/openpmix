.. _man3-PMIx_Regattr_create:

PMIx_Regattr_create
===================

.. include_body

``PMIx_Regattr_create`` |mdash| Allocate and initialize an array of
:ref:`pmix_regattr_t(5) <man5-pmix_regattr_t>` structures.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_regattr_t* PMIx_Regattr_create(size_t n);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``n``: Number of :ref:`pmix_regattr_t(5) <man5-pmix_regattr_t>` structures to
  allocate in the array.


DESCRIPTION
-----------

Allocate a contiguous array of ``n`` :ref:`pmix_regattr_t
<man5-pmix_regattr_t>` structures on the heap and initialize each element by
calling :ref:`PMIx_Regattr_construct(3) <man3-PMIx_Regattr_construct>` on it.
The returned array is owned by the caller and must eventually be released with
:ref:`PMIx_Regattr_free(3) <man3-PMIx_Regattr_free>` (passing the same count
``n``).

If ``n`` is zero, no allocation is performed and ``NULL`` is returned.


RETURN VALUE
------------

Returns a pointer to the newly allocated and initialized array of
``pmix_regattr_t`` structures, or ``NULL`` if ``n`` is zero or the allocation
fails.


NOTES
-----

The ``PMIX_REGATTR_CREATE`` convenience macro is a direct wrapper for this
function; it assigns the returned pointer to its first argument.


EXAMPLES
--------

Create, load, and free an array of registration-attribute structures:

.. code-block:: c

    pmix_regattr_t *attrs;

    attrs = PMIx_Regattr_create(1);
    PMIx_Regattr_load(&attrs[0], "PMIX_TIMEOUT",
                      PMIX_TIMEOUT, PMIX_INT,
                      "Time in seconds before the operation times out");
    /* ... use attrs ... */
    PMIx_Regattr_free(attrs, 1);


.. seealso::
   :ref:`PMIx_Regattr_construct(3) <man3-PMIx_Regattr_construct>`,
   :ref:`PMIx_Regattr_destruct(3) <man3-PMIx_Regattr_destruct>`,
   :ref:`PMIx_Regattr_free(3) <man3-PMIx_Regattr_free>`,
   :ref:`PMIx_Regattr_load(3) <man3-PMIx_Regattr_load>`,
   :ref:`PMIx_Regattr_xfer(3) <man3-PMIx_Regattr_xfer>`,
   :ref:`pmix_regattr_t(5) <man5-pmix_regattr_t>`
