.. _man5-pmix_regattr_t:

pmix_regattr_t
==============

.. include_body

`pmix_regattr_t` |mdash| Registers attribute support for a PMIx function

SYNTAX
------

C Syntax
^^^^^^^^

.. code-block:: c

   #include <pmix_common.h>

   typedef struct pmix_regattr_t {
       char *name;
       pmix_key_t string;
       pmix_data_type_t type;
       char **description;
   } pmix_regattr_t;


DESCRIPTION
-----------

The `pmix_regattr_t` structure is used to register attribute support for a PMIx
function.

The fields are:

``name``
   The actual name of the attribute |mdash| e.g., ``"PMIX_MAX_PROCS"``. Must be a
   ``NULL``-terminated string composed of standard alphanumeric values supported
   by common utilities such as ``strcmp``.

``string``
   The literal string value of the attribute |mdash| e.g., ``"pmix.max.size"``
   for the ``PMIX_MAX_PROCS`` attribute. Must be a ``NULL``-terminated string
   composed of standard alphanumeric values.

``type``
   A PMIx data type (:ref:`pmix_data_type_t <man5-pmix_data_type_t>`) identifying
   the type of data associated with this attribute.

``description``
   A ``NULL``-terminated array of strings describing the attribute, optionally
   including a human-readable description of the range of accepted values |mdash|
   e.g., ``"ALL POSITIVE INTEGERS"``, or a comma-delimited list of enum value
   names.

Although not strictly required, both PMIx library implementers and host
environments are strongly encouraged to provide both human-readable and
machine-parsable descriptions of supported attributes when registering them.

STATIC INITIALIZER
------------------

A statically declared ``pmix_regattr_t`` may be initialized with the
``PMIX_REGATTR_STATIC_INIT`` macro, which sets ``name`` and ``description`` to ``NULL``, clears the ``string`` key, and sets ``type`` to ``PMIX_UNDEF``:

.. code-block:: c

   pmix_regattr_t attr = PMIX_REGATTR_STATIC_INIT;


.. seealso::
   :ref:`PMIx_Query_info(3) <man3-PMIx_Query_info>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`,
   :ref:`pmix_data_type_t(5) <man5-pmix_data_type_t>`
