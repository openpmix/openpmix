.. _man3-PMIx_Multicluster_nspace_construct:

PMIx_Multicluster_nspace_construct
==================================

.. include_body

``PMIx_Multicluster_nspace_construct`` |mdash| Combine a cluster and a
namespace into a single multicluster
:ref:`pmix_nspace_t(5) <man5-pmix_nspace_t>`


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_Multicluster_nspace_construct(pmix_nspace_t target,
                                           pmix_nspace_t cluster,
                                           pmix_nspace_t nspace);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT/OUTPUT PARAMETERS
-----------------------

* ``target``: The :ref:`pmix_nspace_t(5) <man5-pmix_nspace_t>` (a fixed-size
  character array, supplied by the caller) into which the combined value is
  written.
* ``cluster``: The cluster identifier portion.
* ``nspace``: The namespace portion.


DESCRIPTION
-----------

Build a combined multicluster namespace of the form ``cluster:nspace`` in
``target``. The ``target`` array is first cleared, then the ``cluster``
string, a ``':'`` separator, and the ``nspace`` string are concatenated into
it.

The combined result must fit within ``PMIX_MAX_NSLEN`` characters. If the
sum of the cluster and namespace lengths would meet or exceed
``PMIX_MAX_NSLEN``, no concatenation is performed and ``target`` is left
cleared (empty). Callers that need to recover the two components can pass the
result to
:ref:`PMIx_Multicluster_nspace_parse(3) <man3-PMIx_Multicluster_nspace_parse>`.


RETURN VALUE
------------

``PMIx_Multicluster_nspace_construct`` returns no value (``void``).


NOTES
-----

``PMIx_Multicluster_nspace_construct`` is an OpenPMIx convenience routine and
is the backing implementation of the
``PMIX_MULTICLUSTER_NSPACE_CONSTRUCT`` macro. Because the combined value must
share the single ``PMIX_MAX_NSLEN`` budget of a
:ref:`pmix_nspace_t <man5-pmix_nspace_t>`, the cluster and namespace
components must together be short enough to leave room for the separator.


.. seealso::
   :ref:`PMIx_Multicluster_nspace_parse(3) <man3-PMIx_Multicluster_nspace_parse>`,
   :ref:`PMIx_Load_nspace(3) <man3-PMIx_Load_nspace>`,
   :ref:`pmix_nspace_t(5) <man5-pmix_nspace_t>`
