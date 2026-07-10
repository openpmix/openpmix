.. _man3-PMIx_Multicluster_nspace_parse:

PMIx_Multicluster_nspace_parse
==============================

.. include_body

``PMIx_Multicluster_nspace_parse`` |mdash| Split a multicluster
:ref:`pmix_nspace_t(5) <man5-pmix_nspace_t>` into its cluster and namespace
parts


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_Multicluster_nspace_parse(pmix_nspace_t target,
                                       pmix_nspace_t cluster,
                                       pmix_nspace_t nspace);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT/OUTPUT PARAMETERS
-----------------------

* ``target``: The combined multicluster namespace (of the form
  ``cluster:nspace``) to be parsed.
* ``cluster``: The :ref:`pmix_nspace_t(5) <man5-pmix_nspace_t>` (supplied by
  the caller) into which the cluster portion is written.
* ``nspace``: The :ref:`pmix_nspace_t(5) <man5-pmix_nspace_t>` (supplied by
  the caller) into which the namespace portion is written.


DESCRIPTION
-----------

Parse a combined multicluster namespace |mdash| as produced by
:ref:`PMIx_Multicluster_nspace_construct(3) <man3-PMIx_Multicluster_nspace_construct>`
|mdash| back into its two components. The characters of ``target`` up to the
first ``':'`` separator are copied into ``cluster``; the characters following
the separator are copied into ``nspace``. The ``cluster`` output is cleared
before parsing begins.


RETURN VALUE
------------

``PMIx_Multicluster_nspace_parse`` returns no value (``void``).


NOTES
-----

``PMIx_Multicluster_nspace_parse`` is an OpenPMIx convenience routine and is
the backing implementation of the ``PMIX_MULTICLUSTER_NSPACE_PARSE`` macro.
The ``cluster`` and ``nspace`` output arrays must be supplied by the caller.


.. seealso::
   :ref:`PMIx_Multicluster_nspace_construct(3) <man3-PMIx_Multicluster_nspace_construct>`,
   :ref:`PMIx_Load_nspace(3) <man3-PMIx_Load_nspace>`,
   :ref:`pmix_nspace_t(5) <man5-pmix_nspace_t>`
