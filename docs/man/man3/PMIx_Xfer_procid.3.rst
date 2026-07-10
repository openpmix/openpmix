.. _man3-PMIx_Xfer_procid:

PMIx_Xfer_procid
================

.. include_body

``PMIx_Xfer_procid`` |mdash| Copy one
:ref:`pmix_proc_t(5) <man5-pmix_proc_t>` into another


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_Xfer_procid(pmix_proc_t *dst,
                         const pmix_proc_t *src);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT/OUTPUT PARAMETERS
-----------------------

* ``dst``: Pointer to the destination :ref:`pmix_proc_t(5)
  <man5-pmix_proc_t>` structure. The storage must already exist |mdash| it is
  supplied by the caller. Its contents are overwritten.
* ``src``: Pointer to the source :ref:`pmix_proc_t(5) <man5-pmix_proc_t>`
  structure to be copied.


DESCRIPTION
-----------

Perform a non-destructive transfer of a process identifier by copying the
entire :ref:`pmix_proc_t <man5-pmix_proc_t>` structure |mdash| both the
``nspace`` and ``rank`` fields |mdash| from ``src`` to ``dst``. The copy is a
simple byte-for-byte ``memcpy`` of ``sizeof(pmix_proc_t)`` bytes.

Because a :ref:`pmix_proc_t <man5-pmix_proc_t>` contains no heap-allocated
members (the namespace is a fixed-size embedded array), no memory is
allocated or freed and ``src`` is left unchanged. Callers that already hold
the namespace and rank as separate values can instead use
:ref:`PMIx_Load_procid(3) <man3-PMIx_Load_procid>`.


RETURN VALUE
------------

``PMIx_Xfer_procid`` returns no value (``void``).


NOTES
-----

``PMIx_Xfer_procid`` is an OpenPMIx convenience routine and is the backing
implementation of the ``PMIX_XFER_PROCID`` macro. Both arguments must point
to valid structures; the routine performs no ``NULL`` checking.


.. seealso::
   :ref:`PMIx_Load_procid(3) <man3-PMIx_Load_procid>`,
   :ref:`PMIx_Check_procid(3) <man3-PMIx_Check_procid>`,
   :ref:`pmix_proc_t(5) <man5-pmix_proc_t>`
