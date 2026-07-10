.. _man5-pmix_rank_t:

pmix_rank_t
===========

.. include_body

`pmix_rank_t` |mdash| Type for process rank values

SYNTAX
------

C Syntax
^^^^^^^^

.. code-block:: c

   #include <pmix_common.h>

   typedef uint32_t pmix_rank_t;


DESCRIPTION
-----------

The `pmix_rank_t` type is a `uint32_t` used to hold rank values. A rank is the
zero-based numerical position of a process within a defined scope. Valid rank
values start at zero.

The following reserved constants may be used to set a variable of type
`pmix_rank_t`:

* ``PMIX_RANK_UNDEF`` (``UINT32_MAX``) |mdash| Requests job-level data where the
  information itself is not associated with any specific rank, or is used when
  passing a :ref:`pmix_proc_t(5) <man5-pmix_proc_t>` identifier to an operation
  that only references the ``nspace`` field of that structure.
* ``PMIX_RANK_WILDCARD`` (``UINT32_MAX-1``) |mdash| Indicates that the user
  wants the data for the given key from every rank that posted that key.
* ``PMIX_RANK_LOCAL_NODE`` (``UINT32_MAX-2``) |mdash| A special rank value used
  to define groups of ranks; identifies all ranks on the local node.
* ``PMIX_RANK_INVALID`` (``UINT32_MAX-3``) |mdash| An invalid rank value.
* ``PMIX_RANK_LOCAL_PEERS`` (``UINT32_MAX-4``) |mdash| A special rank value used
  to define groups of ranks; identifies all peers (i.e., all processes within
  the same namespace) on the local node.

In addition, ``PMIX_RANK_VALID`` (``UINT32_MAX-50``) defines the upper boundary
for valid rank values, and the ``PMIX_RANK_IS_VALID(r)`` macro tests whether a
given rank ``r`` is valid (i.e., less than ``PMIX_RANK_VALID``).


.. seealso::
   :ref:`pmix_proc_t(5) <man5-pmix_proc_t>`,
   :ref:`pmix_nspace_t(5) <man5-pmix_nspace_t>`
