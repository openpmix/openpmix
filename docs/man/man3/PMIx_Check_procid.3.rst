.. _man3-PMIx_Check_procid:

PMIx_Check_procid
=================

.. include_body

``PMIx_Check_procid`` |mdash| Compare two
:ref:`pmix_proc_t(5) <man5-pmix_proc_t>` process identifiers for equality


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   bool PMIx_Check_procid(const pmix_proc_t *a,
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

Compare two process identifiers for equality. The two are considered equal
when *both* their namespaces match (as determined by
:ref:`PMIx_Check_nspace(3) <man3-PMIx_Check_nspace>`) *and* their ranks match
(as determined by :ref:`PMIx_Check_rank(3) <man3-PMIx_Check_rank>`).

Because the underlying namespace and rank comparisons both apply wildcard
semantics, this is a matching test rather than a strict byte-for-byte
comparison: an invalid namespace matches any namespace, and
``PMIX_RANK_WILDCARD`` matches any rank.


RETURN VALUE
------------

Returns ``true`` if both the namespace and rank of ``a`` and ``b`` are
considered equal, and ``false`` otherwise.


NOTES
-----

``PMIx_Check_procid`` is an OpenPMIx convenience routine and is the backing
implementation of the ``PMIX_CHECK_PROCID`` macro. Both arguments must point
to valid structures; the routine performs no ``NULL`` checking.


.. seealso::
   :ref:`PMIx_Load_procid(3) <man3-PMIx_Load_procid>`,
   :ref:`PMIx_Check_nspace(3) <man3-PMIx_Check_nspace>`,
   :ref:`PMIx_Check_rank(3) <man3-PMIx_Check_rank>`,
   :ref:`PMIx_Procid_invalid(3) <man3-PMIx_Procid_invalid>`,
   :ref:`pmix_proc_t(5) <man5-pmix_proc_t>`
