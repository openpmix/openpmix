.. _man3-PMIx_Info_construct:

PMIx_Info_construct
===================

.. include_body

`PMIx_Info_construct` |mdash| Initialize the contents of a :ref:`pmix_info_t(5) <man5-pmix_info_t>`


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_Info_construct(pmix_info_t *ptr);

Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``ptr``: Pointer to a :ref:`pmix_info_t(5) <man5-pmix_info_t>` struct


DESCRIPTION
-----------

The ``PMIx_Info_construct`` function initializes all fields of a
:ref:`pmix_info_t <man5-pmix_info_t>` structure that has been previously instantiated in memory.


RETURN VALUE
------------

None


EXAMPLES
--------

Construct a structure:

.. code-block:: c

    pmix_info_t info;

    PMIx_Info_construct(&info);


.. seealso::
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`
