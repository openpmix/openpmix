.. _man3-PMIx_Check_rank:

PMIx_Check_rank
===============

.. include_body

``PMIx_Check_rank`` |mdash| Compare two
:ref:`pmix_rank_t(5) <man5-pmix_rank_t>` values for equality


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   bool PMIx_Check_rank(pmix_rank_t a,
                        pmix_rank_t b);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``a``: The first :ref:`pmix_rank_t(5) <man5-pmix_rank_t>` to be compared.
* ``b``: The second :ref:`pmix_rank_t(5) <man5-pmix_rank_t>` to be compared.


DESCRIPTION
-----------

Compare two ranks for equality, honoring wildcard semantics. The two ranks
are considered equal if their values are identical, or if *either* value is
``PMIX_RANK_WILDCARD``, in which case the wildcard matches any rank.


RETURN VALUE
------------

Returns ``true`` if the ranks are equal, or if either rank is
``PMIX_RANK_WILDCARD``; returns ``false`` otherwise.


NOTES
-----

``PMIx_Check_rank`` is an OpenPMIx convenience routine and is the backing
implementation of the ``PMIX_CHECK_RANK`` macro.


.. seealso::
   :ref:`PMIx_Rank_valid(3) <man3-PMIx_Rank_valid>`,
   :ref:`PMIx_Check_procid(3) <man3-PMIx_Check_procid>`,
   :ref:`pmix_rank_t(5) <man5-pmix_rank_t>`
