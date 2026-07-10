.. _man3-PMIx_App_create:

PMIx_App_create
===============

.. include_body

``PMIx_App_create`` |mdash| Allocate and initialize an array of
:ref:`pmix_app_t(5) <man5-pmix_app_t>` structures.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_app_t* PMIx_App_create(size_t n);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``n``: Number of :ref:`pmix_app_t(5) <man5-pmix_app_t>` structures to
  allocate.


DESCRIPTION
-----------

Allocate a contiguous array of ``n`` :ref:`pmix_app_t <man5-pmix_app_t>`
structures from the heap and initialize (construct) each element so that all
of its fields are zeroed. The returned array is owned by the caller and must be
released with :ref:`PMIx_App_free(3) <man3-PMIx_App_free>` (passing the same
count ``n``).


RETURN VALUE
------------

Returns a pointer to the newly allocated array of ``pmix_app_t`` structures on
success. Returns ``NULL`` if ``n`` is zero or if the allocation fails.


NOTES
-----

``PMIx_App_create`` both allocates and constructs its elements, so it must be
paired with :ref:`PMIx_App_free(3) <man3-PMIx_App_free>` (or, for a
single-element allocation, :ref:`PMIx_App_release(3) <man3-PMIx_App_release>`),
which destructs the contents and frees the array storage. Do not pass a
created array to :ref:`PMIx_App_destruct(3) <man3-PMIx_App_destruct>` alone, as
that would release the payload without freeing the array itself.


EXAMPLES
--------

Create an array of three app structures:

.. code-block:: c

    pmix_app_t *apps;

    apps = PMIx_App_create(3);
    if (NULL == apps) {
        /* handle allocation failure */
    }
    /* ... populate apps[0..2] ... */
    PMIx_App_free(apps, 3);


.. seealso::
   :ref:`PMIx_App_construct(3) <man3-PMIx_App_construct>`,
   :ref:`PMIx_App_destruct(3) <man3-PMIx_App_destruct>`,
   :ref:`PMIx_App_info_create(3) <man3-PMIx_App_info_create>`,
   :ref:`PMIx_App_free(3) <man3-PMIx_App_free>`,
   :ref:`PMIx_App_release(3) <man3-PMIx_App_release>`,
   :ref:`pmix_app_t(5) <man5-pmix_app_t>`
