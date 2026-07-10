.. _man3-PMIx_Check_nspace:

PMIx_Check_nspace
=================

.. include_body

``PMIx_Check_nspace`` |mdash| Compare two
:ref:`pmix_nspace_t(5) <man5-pmix_nspace_t>` values for equality


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   bool PMIx_Check_nspace(const char *key1, const char *key2);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``key1``: Pointer to the first namespace (typically a
  :ref:`pmix_nspace_t(5) <man5-pmix_nspace_t>`) to be compared.
* ``key2``: Pointer to the second namespace to be compared.


DESCRIPTION
-----------

Compare two namespaces for equality. The comparison examines at most
``PMIX_MAX_NSLEN`` characters, which is the maximum length a
:ref:`pmix_nspace_t <man5-pmix_nspace_t>` can hold.

A wildcard match is applied: if *either* namespace is invalid (``NULL`` or
zero-length, as determined by
:ref:`PMIx_Nspace_invalid(3) <man3-PMIx_Nspace_invalid>`), the two are
treated as matching and the routine returns ``true``. Otherwise the two
strings are compared directly.


RETURN VALUE
------------

Returns ``true`` if the namespaces are considered equal |mdash| that is, if
either namespace is invalid, or if their first ``PMIX_MAX_NSLEN`` characters
match. Returns ``false`` when both namespaces are valid and differ.


NOTES
-----

``PMIx_Check_nspace`` is an OpenPMIx convenience routine and is the backing
implementation of the ``PMIX_CHECK_NSPACE`` macro. Because an invalid
namespace is treated as a wildcard, this routine is intended for matching
semantics rather than a strict byte-for-byte equality test.


.. seealso::
   :ref:`PMIx_Load_nspace(3) <man3-PMIx_Load_nspace>`,
   :ref:`PMIx_Nspace_invalid(3) <man3-PMIx_Nspace_invalid>`,
   :ref:`pmix_nspace_t(5) <man5-pmix_nspace_t>`
