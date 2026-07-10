.. _man3-PMIx_Data_buffer_unload:

PMIx_Data_buffer_unload
=======================

.. include_body

``PMIx_Data_buffer_unload`` |mdash| Extract the raw byte payload from a
``pmix_data_buffer_t``.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_Data_buffer_unload(pmix_data_buffer_t *b,
                                char **bytes, size_t *sz);


INPUT PARAMETERS
----------------

* ``b``: Pointer to the source ``pmix_data_buffer_t`` whose payload is to be
  extracted.


OUTPUT PARAMETERS
-----------------

* ``bytes``: Address of a ``char *`` in which the pointer to the extracted
  payload is returned.
* ``sz``: Address of a ``size_t`` in which the number of extracted bytes is
  returned.


DESCRIPTION
-----------

Remove the packed byte payload from ``b`` and hand it to the caller. The
function extracts the buffer's contents via :ref:`PMIx_Data_unload(3)
<man3-PMIx_Data_unload>`; on success it stores the pointer to the payload in
``*bytes`` and its length in ``*sz``.

The transfer is a move, not a copy: ownership of the underlying memory passes
out of the buffer to the caller, and the buffer is left empty. No new
allocation is made and no byte-copy is performed. Because the caller now owns
the returned memory, it becomes responsible for eventually freeing ``*bytes``.
The emptied buffer may itself still be destructed or released as usual; doing so
will not free the payload that has been handed off.

If the extraction fails, ``*bytes`` is set to ``NULL`` and ``*sz`` is set to
zero.

The extracted payload is in packed (serialized) form. It may be transmitted,
stored, and later reintroduced into a buffer with
:ref:`PMIx_Data_buffer_load(3) <man3-PMIx_Data_buffer_load>` for subsequent
unpacking.


RETURN VALUE
------------

``PMIx_Data_buffer_unload`` returns no value (``void``). Failure is signalled
by ``*bytes`` being set to ``NULL`` and ``*sz`` to zero.


NOTES
-----

``PMIx_Data_buffer_unload`` is an OpenPMIx convenience routine. The
Standard-defined interface for this operation is the ``PMIX_DATA_BUFFER_UNLOAD``
macro, which is implemented in terms of this function.


.. seealso::
   :ref:`PMIx_Data_buffer_load(3) <man3-PMIx_Data_buffer_load>`,
   :ref:`PMIx_Data_buffer_create(3) <man3-PMIx_Data_buffer_create>`,
   :ref:`PMIx_Data_buffer_release(3) <man3-PMIx_Data_buffer_release>`,
   :ref:`PMIx_Data_unload(3) <man3-PMIx_Data_unload>`,
   :ref:`pmix_data_buffer_t(5) <man5-pmix_data_buffer_t>`
