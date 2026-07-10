.. _man5-pmix_bind_envelope_t:

pmix_bind_envelope_t
====================

.. include_body

`pmix_bind_envelope_t` |mdash| Selects which threads are considered when getting a process cpuset

SYNTAX
------

C Syntax
^^^^^^^^

.. code-block:: c

   #include <pmix_common.h>

   typedef uint8_t pmix_bind_envelope_t;
   #define PMIX_CPUBIND_PROCESS    0
   #define PMIX_CPUBIND_THREAD     1


DESCRIPTION
-----------

The `pmix_bind_envelope_t` datatype is a ``uint8_t`` that defines the envelope of
threads within a possibly multi-threaded process that are to be considered when
getting the cpuset associated with the process. Valid values include:

``PMIX_CPUBIND_PROCESS`` (``0``)
   Use the location of all threads in the possibly multi-threaded process.

``PMIX_CPUBIND_THREAD`` (``1``)
   Use only the location of the thread calling the API.

A process whose threads are not all bound to the same location may return
inconsistent results from calls made by different threads when the
``PMIX_CPUBIND_THREAD`` envelope was used to generate the cpuset.

The `pmix_bind_envelope_t` value is passed to
:ref:`PMIx_Get_cpuset(3) <man3-PMIx_Get_cpuset>` to control which threads are
considered when populating a :ref:`pmix_cpuset_t(5) <man5-pmix_cpuset_t>`.


.. seealso::
   :ref:`PMIx_Get_cpuset(3) <man3-PMIx_Get_cpuset>`,
   :ref:`pmix_cpuset_t(5) <man5-pmix_cpuset_t>`
