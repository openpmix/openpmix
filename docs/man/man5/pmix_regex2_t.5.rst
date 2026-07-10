.. _man5-pmix_regex2_t:

pmix_regex2_t
=============

.. include_body

``pmix_regex2_t`` |mdash| A compact, self-describing encoding of an ordered
list of values (for example, node or process names).


SYNTAX
------

C Syntax
^^^^^^^^

.. code-block:: c

   #include <pmix_common.h>

   typedef struct pmix_regex2 {
       char *type;         // colon-delimited identifier of the encoding method (e.g., "pmix" or "blob")
       uint8_t *bytes;     // encoded representation (may not be NULL-terminated)
       size_t len;         // number of bytes in the encoded representation
   } pmix_regex2_t;


DESCRIPTION
-----------

The ``pmix_regex2_t`` structure holds a reduced-size, encoded representation of
an ordered list of values |mdash| most commonly the set of node names, or the
set of process names, that participate in a job. Encoding a long, regular list
(such as ``node000,node001,...,node511``) into this compact form greatly
reduces the volume of data that must be exchanged when a namespace is
registered or distributed.

The fields are:

* ``type`` |mdash| A colon-delimited string naming the encoding method used to
  produce the representation (for example, ``"pmix"`` for the built-in regular
  expression generator, or ``"blob"`` for an opaque payload). The parser uses
  this identifier to select the matching decoder.
* ``bytes`` |mdash| The encoded representation itself. This is a raw byte buffer
  and is **not** guaranteed to be ``NULL``-terminated; its length is given by
  ``len``.
* ``len`` |mdash| The number of bytes in the ``bytes`` buffer.

A ``pmix_regex2_t`` is produced by :ref:`PMIx_generate_regex2(3)
<man3-PMIx_generate_regex2>` from a comma-separated input string, and is
expanded back into the original list by :ref:`PMIx_parse_regex2(3)
<man3-PMIx_parse_regex2>`. The order of the individual values is preserved
across the generate/parse round trip. The encoded form may also be passed to
:ref:`PMIx_server_register_nspace(3) <man3-PMIx_server_register_nspace>` for
processing. In all cases the caller is responsible for releasing the data the
structure references.

The ``pmix_regex2_t`` type supersedes the older raw ``char*`` regular-expression
form. It also appears as a member of the :ref:`pmix_value_t(5)
<man5-pmix_value_t>` union so that an encoded list can be carried as a typed
value.


STATIC INITIALIZER
------------------

A statically declared ``pmix_regex2_t`` may be initialized with the
``PMIX_REGEX2_STATIC_INIT`` macro, which sets ``type`` and ``bytes`` to ``NULL``
and ``len`` to ``0``:

.. code-block:: c

   pmix_regex2_t regex = PMIX_REGEX2_STATIC_INIT;


.. seealso::
   :ref:`PMIx_generate_regex2(3) <man3-PMIx_generate_regex2>`,
   :ref:`PMIx_parse_regex2(3) <man3-PMIx_parse_regex2>`,
   :ref:`PMIx_server_register_nspace(3) <man3-PMIx_server_register_nspace>`,
   :ref:`pmix_value_t(5) <man5-pmix_value_t>`,
   :ref:`pmix_byte_object_t(5) <man5-pmix_byte_object_t>`
