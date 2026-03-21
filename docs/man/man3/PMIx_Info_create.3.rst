.. _man3-PMIx_Info_create:

PMIx_Info_create
===================

.. include_body

`PMIx_Info_create` |mdash| Initialize the contents of a :ref:`pmix_info_t(5) <man5-pmix_info_t>`


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_info_t *info;
   size_t sz;

   sz = 5;
   info = PMIx_Info_create(sz);

Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``sz``: Number of :ref:`pmix_info_t(5) <man5-pmix_info_t>` structures to allocate


DESCRIPTION
-----------

The ``PMIx_Info_create`` function creates an array of :ref:`pmix_info_t(5) <man5-pmix_info_t>` structures and initializes all fields within them.


RETURN VALUE
------------

Pointer to the allocated array. A return of ``NULL`` indicates that the memory allocation failed.


EXAMPLES
--------

Allocate an array:

.. code-block:: c

    pmix_info_t *info;

    info = PMIx_Info_create(3);


.. seealso::
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`
