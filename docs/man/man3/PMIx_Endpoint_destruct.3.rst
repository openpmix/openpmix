.. _man3-PMIx_Endpoint_destruct:

PMIx_Endpoint_destruct
======================

.. include_body

``PMIx_Endpoint_destruct`` |mdash| Release the contents of a
:ref:`pmix_endpoint_t(5) <man5-pmix_endpoint_t>` structure.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_Endpoint_destruct(pmix_endpoint_t *e);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``e``: Pointer to the
  :ref:`pmix_endpoint_t(5) <man5-pmix_endpoint_t>` structure whose contents
  are to be released.


DESCRIPTION
-----------

Release any heap memory referenced by the fields of a
:ref:`pmix_endpoint_t <man5-pmix_endpoint_t>` structure. Specifically, the
``uuid`` and ``osname`` strings and the ``bytes`` buffer of the embedded
``endpt`` byte object are freed if they are non-``NULL``. The storage for the
structure itself is **not** freed |mdash| that remains the responsibility of
the caller, since a constructed structure occupies caller-provided storage.

``PMIx_Endpoint_destruct`` is the counterpart to
:ref:`PMIx_Endpoint_construct(3) <man3-PMIx_Endpoint_construct>` and should be
used on any structure that was initialized with it. To release an array of
structures that was allocated by
:ref:`PMIx_Endpoint_create(3) <man3-PMIx_Endpoint_create>`, use
:ref:`PMIx_Endpoint_free(3) <man3-PMIx_Endpoint_free>` instead, which both
destructs the contents and frees the array storage.


RETURN VALUE
------------

``PMIx_Endpoint_destruct`` returns no value (``void``).


.. seealso::
   :ref:`PMIx_Endpoint_construct(3) <man3-PMIx_Endpoint_construct>`,
   :ref:`PMIx_Endpoint_create(3) <man3-PMIx_Endpoint_create>`,
   :ref:`PMIx_Endpoint_free(3) <man3-PMIx_Endpoint_free>`,
   :ref:`pmix_endpoint_t(5) <man5-pmix_endpoint_t>`
