.. _man3-PMIx_Endpoint_construct:

PMIx_Endpoint_construct
=======================

.. include_body

``PMIx_Endpoint_construct`` |mdash| Initialize a caller-provided
:ref:`pmix_endpoint_t(5) <man5-pmix_endpoint_t>` structure.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_Endpoint_construct(pmix_endpoint_t *e);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``e``: Pointer to the
  :ref:`pmix_endpoint_t(5) <man5-pmix_endpoint_t>` structure to be
  initialized. The storage for the structure itself must already exist |mdash|
  it is supplied by the caller (typically declared on the stack or embedded
  within another object).


DESCRIPTION
-----------

Initialize the memory of a :ref:`pmix_endpoint_t <man5-pmix_endpoint_t>` that
the caller has already allocated. The function zeroes the entire structure,
clearing the ``uuid`` and ``osname`` string pointers and the embedded
``endpt`` byte object (its ``bytes`` pointer and ``size``). No heap memory is
allocated.

A structure must be constructed (or created with
:ref:`PMIx_Endpoint_create(3) <man3-PMIx_Endpoint_create>`) before any of its
fields are used; operating on an uninitialized structure produces undefined
behavior.

Because ``PMIx_Endpoint_construct`` operates on caller-provided storage, it
must be paired with
:ref:`PMIx_Endpoint_destruct(3) <man3-PMIx_Endpoint_destruct>` to release any
``uuid``, ``osname``, or ``endpt`` payload the structure subsequently
acquires. Do **not** pass a constructed (as opposed to created) structure to
:ref:`PMIx_Endpoint_free(3) <man3-PMIx_Endpoint_free>`, as that would attempt
to free storage the library did not allocate.


RETURN VALUE
------------

``PMIx_Endpoint_construct`` returns no value (``void``).


EXAMPLES
--------

Construct a structure:

.. code-block:: c

    pmix_endpoint_t ep;

    PMIx_Endpoint_construct(&ep);


.. seealso::
   :ref:`PMIx_Endpoint_destruct(3) <man3-PMIx_Endpoint_destruct>`,
   :ref:`PMIx_Endpoint_create(3) <man3-PMIx_Endpoint_create>`,
   :ref:`PMIx_Endpoint_free(3) <man3-PMIx_Endpoint_free>`,
   :ref:`pmix_endpoint_t(5) <man5-pmix_endpoint_t>`
