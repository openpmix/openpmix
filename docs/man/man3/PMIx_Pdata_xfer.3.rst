.. _man3-PMIx_Pdata_xfer:

PMIx_Pdata_xfer
===============

.. include_body

``PMIx_Pdata_xfer`` |mdash| Copy the contents of one
:ref:`pmix_pdata_t(5) <man5-pmix_pdata_t>` structure into another.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_Pdata_xfer(pmix_pdata_t *dest,
                        pmix_pdata_t *src);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT/OUTPUT PARAMETERS
-----------------------

* ``dest``: Pointer to the destination :ref:`pmix_pdata_t(5)
  <man5-pmix_pdata_t>` structure. Any prior contents are overwritten without
  being released.


INPUT PARAMETERS
----------------

* ``src``: Pointer to the source :ref:`pmix_pdata_t(5) <man5-pmix_pdata_t>`
  structure whose contents are copied into ``dest``.


DESCRIPTION
-----------

Copy the process identifier, key, and value from ``src`` into ``dest``. The
function first zeroes ``dest``, then copies the namespace and rank of
``src->proc``, copies the ``src->key`` string, and performs a deep copy of
``src->value`` into ``dest->value``.

The transfer is a **copy**, not a move: ``src`` is left unmodified, and
``dest`` receives its own independently allocated storage for any value payload
that requires it. As a result ``dest`` and ``src`` may afterward be destructed
independently.

If ``dest`` is ``NULL``, the function does nothing.


RETURN VALUE
------------

``PMIx_Pdata_xfer`` returns no value (``void``).


NOTES
-----

Because ``PMIx_Pdata_xfer`` overwrites ``dest`` without releasing any previous
contents, only call it on a freshly constructed or otherwise empty structure to
avoid leaking a prior payload. Both ``dest`` and ``src`` should ultimately be
released with :ref:`PMIx_Pdata_destruct(3) <man3-PMIx_Pdata_destruct>` or
:ref:`PMIx_Pdata_free(3) <man3-PMIx_Pdata_free>`.


EXAMPLES
--------

Copy one structure into another:

.. code-block:: c

    pmix_pdata_t dest;

    PMIx_Pdata_construct(&dest);
    PMIx_Pdata_xfer(&dest, &src);
    /* dest and src may now be destructed independently */
    PMIx_Pdata_destruct(&dest);


.. seealso::
   :ref:`PMIx_Pdata_construct(3) <man3-PMIx_Pdata_construct>`,
   :ref:`PMIx_Pdata_destruct(3) <man3-PMIx_Pdata_destruct>`,
   :ref:`PMIx_Pdata_create(3) <man3-PMIx_Pdata_create>`,
   :ref:`PMIx_Pdata_free(3) <man3-PMIx_Pdata_free>`,
   :ref:`PMIx_Pdata_load(3) <man3-PMIx_Pdata_load>`,
   :ref:`pmix_pdata_t(5) <man5-pmix_pdata_t>`
