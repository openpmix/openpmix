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

where ``bts`` is the payload and ``sz`` the number of bytes in it. The
payload may be ``bytes``, a ``bytearray``, a ``memoryview`` or a ``str``
(encoded as UTF-8); anything else, an integer included, is
``PMIX_ERR_TYPE_MISMATCH``. The length is taken from the payload actually
given, so ``size`` is ignored on input and may be left out. A missing or
``None`` payload is an empty byte object. A byte object handed back by the
library gives ``bytes`` as a ``bytes`` object and ``size`` as its length.


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

STATIC INITIALIZER
------------------

A statically declared ``pmix_byte_object_t`` may be initialized with the
``PMIX_BYTE_OBJECT_STATIC_INIT`` macro, which sets ``bytes`` to ``NULL`` and ``size`` to ``0``:

.. code-block:: c

   pmix_byte_object_t bo = PMIX_BYTE_OBJECT_STATIC_INIT;


.. seealso::
   :ref:`pmix_value_t(5) <man5-pmix_value_t>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`
