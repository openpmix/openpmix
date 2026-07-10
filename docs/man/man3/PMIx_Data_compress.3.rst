.. _man3-PMIx_Data_compress:

PMIx_Data_compress
==================

.. include_body

``PMIx_Data_compress`` |mdash| Perform a lossless compression on the provided
data block.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   bool PMIx_Data_compress(const uint8_t *inbytes,
                           size_t size,
                           uint8_t **outbytes,
                           size_t *nbytes);


INPUT PARAMETERS
----------------

* ``inbytes``: Pointer to the source data to be compressed. Must not be
  ``NULL``.
* ``size``: Number of bytes in the source data region.


OUTPUT PARAMETERS
-----------------

* ``outbytes``: Address at which a pointer to the newly allocated compressed
  data region is returned. On a ``true`` return this points to a buffer
  allocated by the library with ``malloc``; the caller is responsible for
  releasing it with ``free``. On a ``false`` return no buffer is produced.
* ``nbytes``: Address at which the number of bytes in the compressed data
  region is returned.


DESCRIPTION
-----------

Compress the provided data block using a lossless algorithm. Destination memory
is allocated only if the operation is successfully concluded, and the caller is
responsible for releasing that region. The input data block remains unaltered.
The method of compressing and decompressing data is implementation dependent.

In this implementation the compression is performed by the active ``pcompress``
component (e.g., a zlib-based component). Compression is attempted only when the
input is larger than a configurable threshold: inputs smaller than the
``pmix_pcompress_base_limit`` value (default 4096 bytes) are not compressed.
Even for larger inputs, if the resulting data would not be smaller than the
original, the function declines to compress it. In all such cases the function
returns ``false`` and no output buffer is allocated.

Data produced by ``PMIx_Data_compress`` is intended to be restored with
:ref:`PMIx_Data_decompress(3) <man3-PMIx_Data_decompress>`; only data compressed
by this function may be passed to that routine.


RETURN VALUE
------------

Returns a ``bool``, not a ``pmix_status_t``:

* ``true`` |mdash| the input data was compressed. ``*outbytes`` points to a
  freshly allocated buffer holding the compressed data and ``*nbytes`` holds its
  length. The caller must ``free`` ``*outbytes`` when done.
* ``false`` |mdash| the input data was not compressed. This is the normal result
  when the input is smaller than the compression threshold, or when compressing
  it would not yield a smaller data block. No output buffer is allocated in this
  case.

A ``false`` return is therefore an ordinary, non-error outcome, not an
indication of failure.


NOTES
-----

The compression threshold is controlled by the ``pmix_pcompress_base_limit``
MCA parameter (default 4096 bytes). Adjusting it changes the minimum input size
for which compression is attempted.

``PMIx_Data_compress`` must be called only after the PMIx library has been
initialized and with a non-``NULL`` ``inbytes`` pointer; no output is produced
otherwise.

The compressed representation is implementation dependent and is not portable
across differing PMIx implementations. It should be treated as an opaque blob to
be handed back to :ref:`PMIx_Data_decompress(3) <man3-PMIx_Data_decompress>`.


.. seealso::
   :ref:`PMIx_Data_decompress(3) <man3-PMIx_Data_decompress>`
