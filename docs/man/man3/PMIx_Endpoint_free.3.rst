.. _man3-PMIx_Endpoint_free:

PMIx_Endpoint_free
==================

.. include_body

``PMIx_Endpoint_free`` |mdash| Release an array of
:ref:`pmix_endpoint_t(5) <man5-pmix_endpoint_t>` structures allocated by
:ref:`PMIx_Endpoint_create(3) <man3-PMIx_Endpoint_create>`.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_Endpoint_free(pmix_endpoint_t *e, size_t n);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``e``: Pointer to the array of
  :ref:`pmix_endpoint_t(5) <man5-pmix_endpoint_t>` structures to be released.
  May be ``NULL``, in which case the call is a no-op.
* ``n``: Number of structures in the array.


DESCRIPTION
-----------

Release an array of :ref:`pmix_endpoint_t <man5-pmix_endpoint_t>` structures
that was previously allocated by
:ref:`PMIx_Endpoint_create(3) <man3-PMIx_Endpoint_create>`. The function
destructs each of the ``n`` elements |mdash| freeing the ``uuid`` and
``osname`` strings and the ``endpt`` byte-object payload of every member
|mdash| and then frees the array storage itself. If ``e`` is ``NULL`` the
function returns immediately.

Use this routine only on array storage that the library allocated. A single
structure that was initialized with
:ref:`PMIx_Endpoint_construct(3) <man3-PMIx_Endpoint_construct>` must be
released with
:ref:`PMIx_Endpoint_destruct(3) <man3-PMIx_Endpoint_destruct>` instead, since
its storage was provided by the caller.


RETURN VALUE
------------

``PMIx_Endpoint_free`` returns no value (``void``).


NOTES
-----

Unlike :ref:`PMIx_Endpoint_destruct(3) <man3-PMIx_Endpoint_destruct>`, which
releases only the contents of a structure, ``PMIx_Endpoint_free`` also frees
the memory occupied by the array itself.


.. seealso::
   :ref:`PMIx_Endpoint_create(3) <man3-PMIx_Endpoint_create>`,
   :ref:`PMIx_Endpoint_construct(3) <man3-PMIx_Endpoint_construct>`,
   :ref:`PMIx_Endpoint_destruct(3) <man3-PMIx_Endpoint_destruct>`,
   :ref:`pmix_endpoint_t(5) <man5-pmix_endpoint_t>`
