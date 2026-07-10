.. _man3-PMIx_Device_free:

PMIx_Device_free
================

.. include_body

``PMIx_Device_free`` |mdash| Release an array of
:ref:`pmix_device_t(5) <man5-pmix_device_t>` structures allocated by
:ref:`PMIx_Device_create(3) <man3-PMIx_Device_create>`.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_Device_free(pmix_device_t *d, size_t n);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``d``: Pointer to the array of
  :ref:`pmix_device_t(5) <man5-pmix_device_t>` structures to be released. May
  be ``NULL``, in which case the call is a no-op.
* ``n``: Number of structures in the array.


DESCRIPTION
-----------

Release an array of :ref:`pmix_device_t <man5-pmix_device_t>` structures that
was previously allocated by
:ref:`PMIx_Device_create(3) <man3-PMIx_Device_create>`. The function destructs
each of the ``n`` elements |mdash| freeing the ``uuid`` and ``osname`` strings
of every member |mdash| and then frees the array storage itself. If ``d`` is
``NULL`` the function returns immediately.

Use this routine only on array storage that the library allocated. A single
structure that was initialized with
:ref:`PMIx_Device_construct(3) <man3-PMIx_Device_construct>` must be released
with :ref:`PMIx_Device_destruct(3) <man3-PMIx_Device_destruct>` instead, since
its storage was provided by the caller.


RETURN VALUE
------------

``PMIx_Device_free`` returns no value (``void``).


NOTES
-----

Unlike :ref:`PMIx_Device_destruct(3) <man3-PMIx_Device_destruct>`, which
releases only the contents of a structure, ``PMIx_Device_free`` also frees the
memory occupied by the array itself.


.. seealso::
   :ref:`PMIx_Device_create(3) <man3-PMIx_Device_create>`,
   :ref:`PMIx_Device_construct(3) <man3-PMIx_Device_construct>`,
   :ref:`PMIx_Device_destruct(3) <man3-PMIx_Device_destruct>`,
   :ref:`pmix_device_t(5) <man5-pmix_device_t>`
