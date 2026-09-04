.. _man3-PMIx_Check_procid_strict:

PMIx_Check_procid_strict
========================

.. include_body

``PMIx_Check_procid_strict`` |mdash| Compare two
:ref:`pmix_proc_t(5) <man5-pmix_proc_t>` values for equality, without
wildcard semantics on either the namespace or the rank


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   bool PMIx_Check_procid_strict(const pmix_proc_t *a,
                                 const pmix_proc_t *b);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``a``: Pointer to the first :ref:`pmix_proc_t(5) <man5-pmix_proc_t>` to be
  compared.
* ``b``: Pointer to the second :ref:`pmix_proc_t(5) <man5-pmix_proc_t>` to be
  compared.


DESCRIPTION
-----------

Compare two process identifiers for equality, treating neither an invalid
namespace nor ``PMIX_RANK_WILDCARD`` as matching anything other than itself.
The two are equal when their namespaces match as determined by
:ref:`PMIx_Check_nspace_strict(3) <man3-PMIx_Check_nspace_strict>`, *and*
their ranks are literally equal.

The two halves are strict in different ways, because the two fields differ:

* An invalid namespace (``NULL`` or zero-length) matches **nothing**, not
  even another invalid namespace, exactly as in
  :ref:`PMIx_Check_nspace_strict(3) <man3-PMIx_Check_nspace_strict>`.

* The rank is compared **literally**, and is not tested for validity.
  ``PMIX_RANK_WILDCARD`` is a value a caller may legitimately be holding
  |mdash| "every rank of this namespace" is how most connect requests name
  their participants |mdash| so it names the same thing as another
  ``PMIX_RANK_WILDCARD`` and a different thing from rank 0. This routine
  declines to treat one rank as standing for the others; it does not refuse
  any particular rank.

This is the difference between this routine and
:ref:`PMIx_Check_procid(3) <man3-PMIx_Check_procid>`, which wildcards twice
over: an invalid namespace there matches any namespace, and
``PMIX_RANK_WILDCARD`` matches any rank.

A ``NULL`` argument names no process, and so matches none |mdash| including
another ``NULL``. Note that this too is the opposite of
:ref:`PMIx_Check_procid(3) <man3-PMIx_Check_procid>`, which reports two
absent procIDs as equal.


RETURN VALUE
------------

Returns ``true`` only when both arguments are non-``NULL``, both namespaces
are valid and equal, and both ranks are equal. Returns ``false`` otherwise.


NOTES
-----

``PMIx_Check_procid_strict`` is an OpenPMIx convenience routine and is the
backing implementation of the ``PMIX_CHECK_PROCID_STRICT`` macro.

Choosing between the two routines is a question of what is being asked. Use
:ref:`PMIx_Check_procid(3) <man3-PMIx_Check_procid>` to match a procID
against a **specification**, where an unset namespace means "any namespace"
and ``PMIX_RANK_WILDCARD`` means "any rank". Use
``PMIx_Check_procid_strict`` to ask whether two procIDs **name the same
thing** |mdash| deciding whether a participant list matches one recorded
earlier, for instance, where ``ns/0`` and ``ns/WILDCARD`` are different
participant lists and must not be confused for one another.


.. seealso::
   :ref:`PMIx_Check_procid(3) <man3-PMIx_Check_procid>`,
   :ref:`PMIx_Check_nspace_strict(3) <man3-PMIx_Check_nspace_strict>`,
   :ref:`PMIx_Load_procid(3) <man3-PMIx_Load_procid>`,
   :ref:`PMIx_Check_rank(3) <man3-PMIx_Check_rank>`,
   :ref:`pmix_proc_t(5) <man5-pmix_proc_t>`
