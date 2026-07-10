.. _man5-pmix_info_t:

pmix_info_t
===========

.. include_body

`pmix_info_t` |mdash| Defines a key-value pair

SYNTAX
------

C Syntax
^^^^^^^^

.. code-block:: c

   #include <pmix_common.h>

   typedef struct pmix_info {
       pmix_key_t key;
       pmix_info_directives_t flags;   // bit-mask of flags
       pmix_value_t value;
   } pmix_info_t;

where ``key`` is the string name of the attribute (e.g., ``PMIX_TIMEOUT``), ``flags`` is a bit-mask of
:ref:`pmix_info_directives_t <man5-pmix_info_directives_t>` values qualifying the entry, and ``value``
is the associated :ref:`pmix_value_t <man5-pmix_value_t>`.


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

   from pmix import *

   foo = {'key': "string name", 'flags': u32, 'value': {'value': val, 'val_type': type}}

where ``key`` is a string key (e.g., ``PMIX_TIMEOUT``), ``flags`` is a `uint32_t` value from the :ref:`pmix_info_directives_t <man5-pmix_info_directives_t>` table, and ``value`` is a Python version of a :ref:`pmix_value_t <man5-pmix_value_t>`.


DESCRIPTION
-----------

The `pmix_info_t` structure is a core building block of PMIx, used to pass
information and directives between applications, servers, and host environments.

The ``key`` field is a :ref:`pmix_key_t(5) <man5-pmix_key_t>` string naming the
attribute being conveyed. The ``value`` field is a
:ref:`pmix_value_t(5) <man5-pmix_value_t>` carrying the typed data associated
with that key. The ``flags`` field is a bit-mask of
:ref:`pmix_info_directives_t(5) <man5-pmix_info_directives_t>` values that
qualify how the entry is to be interpreted |mdash| for example, marking the
entry as required rather than optional.


SUPPORT FUNCTIONS
-----------------

PMIx provides a number of functions to support constructing, destructing, allocating, loading, and
setting flags on `pmix_info_t` structures. These include:

* :ref:`PMIx_Info_construct(3) <man3-PMIx_Info_construct>` |mdash| Initialize the fields of a single,
  statically declared `pmix_info_t` structure.
* :ref:`PMIx_Info_create(3) <man3-PMIx_Info_create>` |mdash| Allocate and initialize an array of
  `pmix_info_t` structures.
* ``PMIx_Info_destruct`` |mdash| Release the resources held by the fields of a `pmix_info_t` structure
  (the companion to ``PMIx_Info_construct``).
* ``PMIx_Info_free`` |mdash| Release an array of `pmix_info_t` structures allocated by
  ``PMIx_Info_create`` (the companion to ``PMIx_Info_create``).
* ``PMIx_Info_load`` |mdash| Load a key and a typed value into a `pmix_info_t` structure.
* ``PMIx_Info_xfer`` |mdash| Copy the contents of one `pmix_info_t` structure into another.
* ``PMIx_Info_required`` / ``PMIx_Info_optional`` |mdash| Mark a `pmix_info_t` as required or optional
  by setting or clearing the ``PMIX_INFO_REQD`` flag.
* ``PMIx_Info_true`` |mdash| Return the boolean interpretation of the value in a `pmix_info_t`.

The flag values that may appear in the ``flags`` field are enumerated in
:ref:`pmix_info_directives_t(5) <man5-pmix_info_directives_t>`.

.. seealso::
   :ref:`PMIx_Info_construct(3) <man3-PMIx_Info_construct>`,
   :ref:`PMIx_Info_create(3) <man3-PMIx_Info_create>`,
   :ref:`pmix_info_directives_t(5) <man5-pmix_info_directives_t>`,
   :ref:`pmix_value_t(5) <man5-pmix_value_t>`
