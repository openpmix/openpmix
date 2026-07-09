.. _man3-PMIx_Initialized:

PMIx_Initialized
================

.. include_body

``PMIx_Initialized`` |mdash| Determine whether the PMIx library has been
initialized.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   int PMIx_Initialized(void);


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

  from pmix import *

  foo = PMIxClient()
  state = foo.initialized()


DESCRIPTION
-----------

Check whether the PMIx library has been initialized by any of the initialization
functions |mdash| :ref:`PMIx_Init(3) <man3-PMIx_Init>`, ``PMIx_server_init``, or
``PMIx_tool_init``. This function may be called outside of the initialized and
finalized region, and is usable by servers and tools in addition to clients.

The function only reports the internal state of the PMIx library. It does **not**
verify that an active connection with the local server exists, nor that the
server is functional.


RETURN VALUE
------------

Returns ``1`` (true) if the PMIx library has been initialized, and ``0`` (false)
otherwise.

.. note::
   The return value is an ``int`` rather than a ``bool`` for historical reasons:
   this matches the signature of prior PMI libraries.


.. seealso::
   :ref:`PMIx_Init(3) <man3-PMIx_Init>`,
   :ref:`PMIx_Finalize(3) <man3-PMIx_Finalize>`,
   PMIx_tool_init(3),
   PMIx_server_init(3)
