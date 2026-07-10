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

* ``bytes`` |mdash| pointer to the raw byte sequence. The bytes are not
  interpreted by PMIx and need not be NULL-terminated.
* ``size`` |mdash| the number of bytes in the sequence pointed to by ``bytes``.

A `pmix_byte_object_t` may be carried as the value of a
:ref:`pmix_value_t(5) <man5-pmix_value_t>` (using the ``bo`` union member) or a
:ref:`pmix_info_t(5) <man5-pmix_info_t>`, allowing arbitrary binary payloads to
be exchanged through the standard PMIx information-passing mechanisms.

.. seealso::
   :ref:`pmix_value_t(5) <man5-pmix_value_t>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`
