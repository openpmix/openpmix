.. _man3-PMIx_Rank_valid:

PMIx_Rank_valid
===============

.. include_body

``PMIx_Rank_valid`` |mdash| Test whether a
:ref:`pmix_rank_t(5) <man5-pmix_rank_t>` is a valid process rank


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   bool PMIx_Rank_valid(pmix_rank_t a);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``a``: The :ref:`pmix_rank_t(5) <man5-pmix_rank_t>` value to be tested.


DESCRIPTION
-----------

Test whether a rank is a valid process rank. The upper end of the
:ref:`pmix_rank_t <man5-pmix_rank_t>` value space is reserved for special
sentinel values such as ``PMIX_RANK_WILDCARD``, ``PMIX_RANK_UNDEF``, and
``PMIX_RANK_INVALID``. This routine returns ``true`` only for values that
fall below ``PMIX_RANK_VALID`` |mdash| that is, ranks that denote an actual
process position rather than one of the reserved markers.


RETURN VALUE
------------

Returns ``true`` if ``a`` is strictly less than ``PMIX_RANK_VALID`` (a valid
process rank), and ``false`` otherwise.


NOTES
-----

``PMIx_Rank_valid`` is an OpenPMIx convenience routine and is the backing
implementation of the ``PMIX_RANK_IS_VALID`` macro.


.. seealso::
   :ref:`PMIx_Check_rank(3) <man3-PMIx_Check_rank>`,
   :ref:`pmix_rank_t(5) <man5-pmix_rank_t>`
