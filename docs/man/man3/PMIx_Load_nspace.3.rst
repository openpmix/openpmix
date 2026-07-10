.. _man3-PMIx_Load_nspace:

PMIx_Load_nspace
================

.. include_body

``PMIx_Load_nspace`` |mdash| Load a string into a
:ref:`pmix_nspace_t(5) <man5-pmix_nspace_t>`


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_Load_nspace(pmix_nspace_t nspace, const char *str);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``nspace``: The :ref:`pmix_nspace_t(5) <man5-pmix_nspace_t>` (a fixed-size
  character array) into which the string is to be loaded. The storage must
  already exist |mdash| it is supplied by the caller.
* ``str``: Pointer to a NULL-terminated string to be copied into ``nspace``.
  May be ``NULL``.


DESCRIPTION
-----------

Load the contents of a NULL-terminated string into a
:ref:`pmix_nspace_t <man5-pmix_nspace_t>`. The function first zeroes the
entire namespace array (all ``PMIX_MAX_NSLEN + 1`` bytes) and then, if
``str`` is non-``NULL``, copies up to ``PMIX_MAX_NSLEN`` characters from
``str`` into it. A string longer than ``PMIX_MAX_NSLEN`` is silently
truncated; the result is always properly NULL-terminated. If ``str`` is
``NULL`` the namespace is simply left cleared (all zeroes), which is the
invalid/empty namespace value.


RETURN VALUE
------------

``PMIx_Load_nspace`` returns no value (``void``).


NOTES
-----

``PMIx_Load_nspace`` is an OpenPMIx convenience routine and is the backing
implementation of the ``PMIX_LOAD_NSPACE`` macro.


.. seealso::
   :ref:`PMIx_Check_nspace(3) <man3-PMIx_Check_nspace>`,
   :ref:`PMIx_Nspace_invalid(3) <man3-PMIx_Nspace_invalid>`,
   :ref:`PMIx_Multicluster_nspace_construct(3) <man3-PMIx_Multicluster_nspace_construct>`,
   :ref:`pmix_nspace_t(5) <man5-pmix_nspace_t>`
