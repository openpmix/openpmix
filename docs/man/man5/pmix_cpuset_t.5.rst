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


.. seealso::
   :ref:`PMIx_Get_cpuset(3) <man3-PMIx_Get_cpuset>`,
   :ref:`PMIx_Parse_cpuset_string(3) <man3-PMIx_Parse_cpuset_string>`,
   :ref:`pmix_bind_envelope_t(5) <man5-pmix_bind_envelope_t>`,
   :ref:`pmix_topology_t(5) <man5-pmix_topology_t>`
