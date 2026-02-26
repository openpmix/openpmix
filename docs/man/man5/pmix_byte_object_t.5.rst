.. _man5-pmix_byte_object_t:

pmix_byte_object_t
==================

.. include_body

`pmix_byte_object_t` |mdash| a structure containing a raw byte sequence

SYNTAX
------

C Syntax
^^^^^^^^

.. code-block:: c

   #include <pmix_common.h>

   typedef struct pmix_byte_object {
      char *bytes;
      size_t size;
   } pmix_byte_object_t;


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

   from pmix import *

   foo = {'bytes': bts, 'size': sz}

where ``bts`` is a Python byte array and ``sz`` is the integer
number of bytes in that array.


DESCRIPTION
-----------

The `pmix_byte_object_t` structure is used to contain and transfer a sequence of
bytes across APIs and between processes.

.. seealso::
   PMIx_Initialized(3)
