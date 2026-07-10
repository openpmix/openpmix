.. _man3-PMIx_Check_reserved_key:

PMIx_Check_reserved_key
=======================

.. include_body

``PMIx_Check_reserved_key`` |mdash| Test whether a
:ref:`pmix_key_t(5) <man5-pmix_key_t>` lies in the PMIx-reserved namespace


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   bool PMIx_Check_reserved_key(const char *key);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``key``: Pointer to the key (typically a :ref:`pmix_key_t(5)
  <man5-pmix_key_t>`) to be tested.


DESCRIPTION
-----------

Determine whether a key belongs to the reserved key space defined by the
PMIx Standard. All attribute keys defined by PMIx itself begin with the
``"pmix"`` prefix; user code is prohibited from using that prefix so as to
avoid collisions with library-defined attributes. This routine tests for
that reserved prefix by comparing the first four characters of ``key``
against the string ``"pmix"``.


RETURN VALUE
------------

Returns ``true`` if ``key`` begins with the reserved ``"pmix"`` prefix,
indicating a PMIx-reserved key, and ``false`` otherwise.


NOTES
-----

``PMIx_Check_reserved_key`` is an OpenPMIx convenience routine and is the
backing implementation of the ``PMIX_CHECK_RESERVED_KEY`` macro. The argument
must point to a valid string of at least four characters; the routine
performs no ``NULL`` checking.


.. seealso::
   :ref:`PMIx_Check_key(3) <man3-PMIx_Check_key>`,
   :ref:`PMIx_Load_key(3) <man3-PMIx_Load_key>`,
   :ref:`pmix_key_t(5) <man5-pmix_key_t>`
