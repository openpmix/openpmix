.. _man3-PMIx_Nspace_invalid:

PMIx_Nspace_invalid
===================

.. include_body

``PMIx_Nspace_invalid`` |mdash| Test whether a
:ref:`pmix_nspace_t(5) <man5-pmix_nspace_t>` is invalid


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   bool PMIx_Nspace_invalid(const char *nspace);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``nspace``: Pointer to the namespace (typically a :ref:`pmix_nspace_t(5)
  <man5-pmix_nspace_t>`) to be tested. May be ``NULL``.


DESCRIPTION
-----------

Test whether a namespace is invalid. A namespace is considered invalid when
the pointer is ``NULL`` or when the string is zero-length (empty). Any
non-empty namespace string is considered valid.


RETURN VALUE
------------

Returns ``true`` if the namespace is ``NULL`` or empty (invalid), and
``false`` if it contains at least one character (valid).


NOTES
-----

``PMIx_Nspace_invalid`` is an OpenPMIx convenience routine and is the backing
implementation of the ``PMIX_NSPACE_INVALID`` macro. It is safe to call with
a ``NULL`` argument.


.. seealso::
   :ref:`PMIx_Load_nspace(3) <man3-PMIx_Load_nspace>`,
   :ref:`PMIx_Check_nspace(3) <man3-PMIx_Check_nspace>`,
   :ref:`PMIx_Procid_invalid(3) <man3-PMIx_Procid_invalid>`,
   :ref:`pmix_nspace_t(5) <man5-pmix_nspace_t>`
