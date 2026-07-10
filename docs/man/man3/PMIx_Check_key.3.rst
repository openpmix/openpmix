.. _man3-PMIx_Check_key:

PMIx_Check_key
==============

.. include_body

``PMIx_Check_key`` |mdash| Compare a :ref:`pmix_key_t(5) <man5-pmix_key_t>`
against a string for equality


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   bool PMIx_Check_key(const char *key, const char *str);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``key``: Pointer to the key (typically a :ref:`pmix_key_t(5)
  <man5-pmix_key_t>`) to be tested.
* ``str``: Pointer to the string to compare against.


DESCRIPTION
-----------

Compare a key against a given string for equality. The comparison examines at
most ``PMIX_MAX_KEYLEN`` characters, which is the maximum length a
:ref:`pmix_key_t <man5-pmix_key_t>` can hold. This provides a safe,
bounded way to test whether a key matches an expected attribute string
without risking a read past the end of the fixed-size key array.


RETURN VALUE
------------

Returns ``true`` if the first ``PMIX_MAX_KEYLEN`` characters of ``key`` and
``str`` match, and ``false`` otherwise.


NOTES
-----

``PMIx_Check_key`` is an OpenPMIx convenience routine and is the backing
implementation of the ``PMIX_CHECK_KEY`` macro. Both arguments must point to
valid, NULL-terminated strings; the routine performs no ``NULL`` checking.


.. seealso::
   :ref:`PMIx_Load_key(3) <man3-PMIx_Load_key>`,
   :ref:`PMIx_Check_reserved_key(3) <man3-PMIx_Check_reserved_key>`,
   :ref:`pmix_key_t(5) <man5-pmix_key_t>`
