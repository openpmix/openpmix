.. _man3-PMIx_Check_nspace_strict:

PMIx_Check_nspace_strict
========================

.. include_body

``PMIx_Check_nspace_strict`` |mdash| Compare two
:ref:`pmix_nspace_t(5) <man5-pmix_nspace_t>` values for equality, without
treating an invalid namespace as a wildcard


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   bool PMIx_Check_nspace_strict(const char *key1, const char *key2);


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

An invalid namespace (``NULL`` or zero-length, as determined by
:ref:`PMIx_Nspace_invalid(3) <man3-PMIx_Nspace_invalid>`) matches
**nothing** |mdash| not even another invalid namespace. "Unset" is not a
value two namespaces can agree on.

This is the difference between this routine and
:ref:`PMIx_Check_nspace(3) <man3-PMIx_Check_nspace>`, which treats an
invalid namespace on either side as a *wildcard* that matches anything.


RETURN VALUE
------------

Returns ``true`` only when both namespaces are valid and their first
``PMIX_MAX_NSLEN`` characters match. Returns ``false`` otherwise |mdash|
including when either namespace, or both, is invalid.


NOTES
-----

``PMIx_Check_nspace_strict`` is an OpenPMIx convenience routine and is the
backing implementation of the ``PMIX_CHECK_NSPACE_STRICT`` macro.

Choosing between the two routines is a question of what is being asked:

* Use :ref:`PMIx_Check_nspace(3) <man3-PMIx_Check_nspace>` to match a
  request against a **filter**, where an unset namespace means "any" |mdash|
  for example, deciding whether a query restricted to one namespace should
  see a given entry.

* Use ``PMIx_Check_nspace_strict`` to ask whether two things belong to the
  **same namespace**. Structures frequently carry namespace fields that are
  legitimately empty (a launcher or parent that was never recorded, a
  request nobody is waiting on), and comparing one of those with
  ``PMIx_Check_nspace`` silently answers ``true`` against every namespace in
  the system.


.. seealso::
   :ref:`PMIx_Check_nspace(3) <man3-PMIx_Check_nspace>`,
   :ref:`PMIx_Load_nspace(3) <man3-PMIx_Load_nspace>`,
   :ref:`PMIx_Nspace_invalid(3) <man3-PMIx_Nspace_invalid>`,
   :ref:`pmix_nspace_t(5) <man5-pmix_nspace_t>`
