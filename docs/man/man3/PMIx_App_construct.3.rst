.. _man3-PMIx_App_construct:

PMIx_App_construct
==================

.. include_body

``PMIx_App_construct`` |mdash| Initialize a caller-provided
:ref:`pmix_app_t(5) <man5-pmix_app_t>` structure.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_App_construct(pmix_app_t *p);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``p``: Pointer to the :ref:`pmix_app_t(5) <man5-pmix_app_t>` structure to be
  initialized. The storage for the structure itself must already exist |mdash|
  it is supplied by the caller (typically declared on the stack or embedded
  within another object).


DESCRIPTION
-----------

Initialize the memory of a :ref:`pmix_app_t <man5-pmix_app_t>` that the caller
has already allocated. The function zeroes the entire structure, clearing the
``cmd``, ``argv``, ``env``, and ``cwd`` pointers, setting ``maxprocs`` to zero,
and setting the ``info`` pointer to ``NULL`` with ``ninfo`` equal to zero. No
heap memory is allocated and no payload is attached.


RETURN VALUE
------------

``PMIx_App_construct`` returns no value (``void``).


NOTES
-----

Because ``PMIx_App_construct`` operates on caller-provided storage, it must be
paired with :ref:`PMIx_App_destruct(3) <man3-PMIx_App_destruct>` to release any
payload the app subsequently acquires. Do **not** pass a constructed (as
opposed to created) app to :ref:`PMIx_App_release(3) <man3-PMIx_App_release>`
or :ref:`PMIx_App_free(3) <man3-PMIx_App_free>`, as those would attempt to free
storage the library did not allocate.


EXAMPLES
--------

Construct a structure:

.. code-block:: c

    pmix_app_t app;

    PMIx_App_construct(&app);


.. seealso::
   :ref:`PMIx_App_destruct(3) <man3-PMIx_App_destruct>`,
   :ref:`PMIx_App_create(3) <man3-PMIx_App_create>`,
   :ref:`PMIx_App_info_create(3) <man3-PMIx_App_info_create>`,
   :ref:`PMIx_App_free(3) <man3-PMIx_App_free>`,
   :ref:`PMIx_App_release(3) <man3-PMIx_App_release>`,
   :ref:`pmix_app_t(5) <man5-pmix_app_t>`
