.. _man5-pmix_proc_t:

pmix_proc_t
===========

.. include_body

`pmix_proc_t` |mdash| Identifies a single process in the PMIx universe

SYNTAX
------

C Syntax
^^^^^^^^

.. code-block:: c

   #include <pmix_common.h>

   typedef struct pmix_proc {
       pmix_nspace_t nspace;
       pmix_rank_t rank;
   } pmix_proc_t;


DESCRIPTION
-----------

The `pmix_proc_t` structure is used to identify a single process in the PMIx
universe. It contains a reference to the namespace and the rank within that
namespace.

The fields are:

* ``nspace`` |mdash| The :ref:`pmix_nspace_t(5) <man5-pmix_nspace_t>` (a
  NULL-terminated character array) identifying the job to which the process
  belongs.
* ``rank`` |mdash| The :ref:`pmix_rank_t(5) <man5-pmix_rank_t>` of the process
  within that namespace. A number of reserved rank values (e.g.,
  ``PMIX_RANK_WILDCARD``) may be placed in this field to reference groups of
  processes or to indicate that only the ``nspace`` field is significant.

The macro ``PMIX_PROC_STATIC_INIT`` is provided to statically initialize the
fields of a `pmix_proc_t` structure, setting ``nspace`` to all zeroes and
``rank`` to ``PMIX_RANK_UNDEF``.


.. seealso::
   :ref:`pmix_nspace_t(5) <man5-pmix_nspace_t>`,
   :ref:`pmix_rank_t(5) <man5-pmix_rank_t>`,
   :ref:`pmix_proc_info_t(5) <man5-pmix_proc_info_t>`
