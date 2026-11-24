.. _man5-pmix_cpuset_t:

pmix_cpuset_t
=============

.. include_body

`pmix_cpuset_t` |mdash| Contains the binding bitmap of a process

SYNTAX
------

C Syntax
^^^^^^^^

.. code-block:: c

   #include <pmix_common.h>

   typedef struct {
       char *source;
       void *bitmap;
   } pmix_cpuset_t;


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

   from pmix import *

   cpus = {'source': "hwloc", 'cpus': [0, 1, 2, 3, 8]}

where ``source`` names the provider that produced the bitmap (the library
supports ``"hwloc"``, which is also the default when ``source`` is left
out) and ``cpus`` lists the CPU indices in the set. On input ``cpus`` may
also be a list of range strings (``['0-3', '8']``) or one range-list string
(``"0-3,8"``). The bitmap itself has no Python form: the set crosses as the
library's string form, ``"<source>:<range-list>"``, which
:ref:`PMIx_Parse_cpuset_string(3) <man3-PMIx_Parse_cpuset_string>` and
:ref:`PMIx_server_generate_cpuset_string(3)
<man3-PMIx_server_generate_cpuset_string>` convert. A cpuset handed back
by the library always lists individual indices, and an unbound process
comes back as ``{'source': '', 'cpus': []}``.

This dict is what the methods that take or return a cpuset use
(``get_cpuset``, ``compute_distances``, ``parse_cpuset_string``). A
*value* of type ``PMIX_PROC_CPUSET`` is different: it can only be ``None``
(see :ref:`pmix_value_t(5) <man5-pmix_value_t>`).


DESCRIPTION
-----------

The `pmix_cpuset_t` structure contains a character string identifying the source
of the bitmap and a pointer to the corresponding implementation-specific
structure describing a process's CPU binding.

The ``source`` field is a string naming the implementation that generated the
bitmap (e.g., ``"hwloc"``). Operations that consume a `pmix_cpuset_t` will return
an error if the ``source`` does not match the underlying source that provided the
binding bitmap.

The ``bitmap`` field is an opaque pointer to the implementation-specific
structure that holds the actual binding (e.g., an ``hwloc_cpuset_t``). Its
interpretation depends on the value of ``source``.

The ``PMIX_CPUSET_STATIC_INIT`` macro is provided to statically initialize a
`pmix_cpuset_t` structure.

A `pmix_cpuset_t` is loaded by :ref:`PMIx_Get_cpuset(3) <man3-PMIx_Get_cpuset>`,
which retrieves the binding of the calling process, and by
:ref:`PMIx_Parse_cpuset_string(3) <man3-PMIx_Parse_cpuset_string>`, which parses
a ``PMIX_CPUSET`` string into the structure. The
:ref:`pmix_bind_envelope_t(5) <man5-pmix_bind_envelope_t>` value passed to
``PMIx_Get_cpuset`` controls which threads of a possibly multi-threaded process
are considered.

STATIC INITIALIZER
------------------

A statically declared ``pmix_cpuset_t`` may be initialized with the
``PMIX_CPUSET_STATIC_INIT`` macro, which sets both ``source`` and ``bitmap`` to ``NULL``:

.. code-block:: c

   pmix_cpuset_t cpuset = PMIX_CPUSET_STATIC_INIT;


.. seealso::
   :ref:`PMIx_Get_cpuset(3) <man3-PMIx_Get_cpuset>`,
   :ref:`PMIx_Parse_cpuset_string(3) <man3-PMIx_Parse_cpuset_string>`,
   :ref:`pmix_bind_envelope_t(5) <man5-pmix_bind_envelope_t>`,
   :ref:`pmix_topology_t(5) <man5-pmix_topology_t>`
