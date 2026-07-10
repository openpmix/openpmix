.. _man3-PMIx_Pdata_load:

PMIx_Pdata_load
===============

.. include_body

``PMIx_Pdata_load`` |mdash| Populate a
:ref:`pmix_pdata_t(5) <man5-pmix_pdata_t>` structure with a process
identifier, key, and value.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_Pdata_load(pmix_pdata_t *dest,
                        const pmix_proc_t *p,
                        const char *key,
                        const void *data,
                        pmix_data_type_t type);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT/OUTPUT PARAMETERS
-----------------------

* ``dest``: Pointer to the destination :ref:`pmix_pdata_t(5)
  <man5-pmix_pdata_t>` structure to be populated. Any prior contents are
  overwritten without being released.


INPUT PARAMETERS
----------------

* ``p``: Pointer to a :ref:`pmix_proc_t(5) <man5-pmix_proc_t>` whose namespace
  and rank are copied into ``dest->proc``.
* ``key``: String key to copy into ``dest->key``.
* ``data``: Pointer to the value payload to store, or ``NULL``.
* ``type``: The :ref:`pmix_data_type_t(5) <man5-pmix_data_type_t>` describing
  the object referenced by ``data``.


DESCRIPTION
-----------

Populate the ``dest`` structure in a single operation. The function first
zeroes ``dest``, then copies the process identifier (namespace and rank) from
``p``, copies the ``key`` string (truncated to the fixed
``pmix_key_t`` length), and loads the supplied ``data``
into ``dest->value`` according to ``type``.

The value is **copied** into ``dest`` |mdash| the library allocates its own
storage for any data that requires it, so the caller's ``data`` buffer need not
remain valid after the call. Because ``PMIx_Pdata_load`` overwrites ``dest``
without releasing any previous contents, only call it on a freshly constructed
or otherwise empty structure to avoid leaking a prior payload.


RETURN VALUE
------------

``PMIx_Pdata_load`` returns no value (``void``).


NOTES
-----

The memory populated by ``PMIx_Pdata_load`` should ultimately be released with
:ref:`PMIx_Pdata_destruct(3) <man3-PMIx_Pdata_destruct>` (for a caller-provided
structure) or :ref:`PMIx_Pdata_free(3) <man3-PMIx_Pdata_free>` (for an element
of an array created with :ref:`PMIx_Pdata_create(3)
<man3-PMIx_Pdata_create>`).


EXAMPLES
--------

Load a string value into a pdata structure:

.. code-block:: c

    pmix_pdata_t pdata;
    pmix_proc_t proc;
    char *val = "example";

    PMIx_Proc_load(&proc, "mynspace", 0);
    PMIx_Pdata_construct(&pdata);
    PMIx_Pdata_load(&pdata, &proc, "mykey", val, PMIX_STRING);
    /* ... */
    PMIx_Pdata_destruct(&pdata);


.. seealso::
   :ref:`PMIx_Pdata_construct(3) <man3-PMIx_Pdata_construct>`,
   :ref:`PMIx_Pdata_destruct(3) <man3-PMIx_Pdata_destruct>`,
   :ref:`PMIx_Pdata_create(3) <man3-PMIx_Pdata_create>`,
   :ref:`PMIx_Pdata_free(3) <man3-PMIx_Pdata_free>`,
   :ref:`PMIx_Pdata_xfer(3) <man3-PMIx_Pdata_xfer>`,
   :ref:`pmix_pdata_t(5) <man5-pmix_pdata_t>`
