.. _man3-PMIx_Data_buffer_load:

PMIx_Data_buffer_load
=====================

.. include_body

``PMIx_Data_buffer_load`` |mdash| Load a raw byte payload into a
``pmix_data_buffer_t``.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_Data_buffer_load(pmix_data_buffer_t *b,
                              char *bytes, size_t sz);


INPUT PARAMETERS
----------------

* ``b``: Pointer to the destination ``pmix_data_buffer_t`` into which the
  payload is to be loaded. The buffer must already have been created (e.g.,
  with :ref:`PMIx_Data_buffer_create(3) <man3-PMIx_Data_buffer_create>`) or
  constructed (e.g., with :ref:`PMIx_Data_buffer_construct(3)
  <man3-PMIx_Data_buffer_construct>`).
* ``bytes``: Pointer to the block of packed bytes to be transferred into the
  buffer.
* ``sz``: The number of bytes referenced by ``bytes``.


DESCRIPTION
-----------

Transfer a block of previously packed bytes into ``b`` so that the data can
subsequently be extracted with the unpack operations. The function wraps
``bytes`` and ``sz`` in a :ref:`pmix_byte_object_t(5) <man5-pmix_byte_object_t>`
and loads it into the buffer via :ref:`PMIx_Data_load(3)
<man3-PMIx_Data_load>`.

The transfer is a move, not a copy: the buffer takes direct ownership of the
memory referenced by ``bytes`` |mdash| no new allocation is made and no
byte-copy is performed. Because ownership has moved into the buffer, the caller
must **not** free ``bytes`` afterward; that memory will be released when the
buffer is later destructed or released (for example with
:ref:`PMIx_Data_buffer_destruct(3) <man3-PMIx_Data_buffer_destruct>` or
:ref:`PMIx_Data_buffer_release(3) <man3-PMIx_Data_buffer_release>`).

If the buffer already holds a payload, that existing payload is first freed
before the new one is loaded. The caller is responsible for supplying data that
is already in packed form |mdash| such as the payload previously extracted with
:ref:`PMIx_Data_buffer_unload(3) <man3-PMIx_Data_buffer_unload>`. The load
operation performs no serialization of its own.


RETURN VALUE
------------

``PMIx_Data_buffer_load`` returns no value (``void``).


NOTES
-----

``PMIx_Data_buffer_load`` is an OpenPMIx convenience routine. The
Standard-defined interface for this operation is the ``PMIX_DATA_BUFFER_LOAD``
macro, which is implemented as a direct call to this function.


.. seealso::
   :ref:`PMIx_Data_buffer_unload(3) <man3-PMIx_Data_buffer_unload>`,
   :ref:`PMIx_Data_buffer_create(3) <man3-PMIx_Data_buffer_create>`,
   :ref:`PMIx_Data_buffer_construct(3) <man3-PMIx_Data_buffer_construct>`,
   :ref:`PMIx_Data_load(3) <man3-PMIx_Data_load>`
