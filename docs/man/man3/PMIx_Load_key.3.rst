.. _man3-PMIx_Load_key:

PMIx_Load_key
=============

.. include_body

``PMIx_Load_key`` |mdash| Load a string into a :ref:`pmix_key_t(5) <man5-pmix_key_t>`


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_Load_key(pmix_key_t key, const char *src);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``key``: The :ref:`pmix_key_t(5) <man5-pmix_key_t>` (a fixed-size
  character array) into which the string is to be loaded. The storage must
  already exist |mdash| it is supplied by the caller.
* ``src``: Pointer to a NULL-terminated string to be copied into ``key``.
  May be ``NULL``.


DESCRIPTION
-----------

Load the contents of a NULL-terminated string into a
:ref:`pmix_key_t <man5-pmix_key_t>`. The function first zeroes the entire
key array (all ``PMIX_MAX_KEYLEN + 1`` bytes) and then, if ``src`` is
non-``NULL``, copies up to ``PMIX_MAX_KEYLEN`` characters from ``src`` into
it. A string longer than ``PMIX_MAX_KEYLEN`` is silently truncated; the
result is always properly NULL-terminated because the trailing byte is left
as the zero written during the initial clear. If ``src`` is ``NULL`` the key
is simply left cleared (all zeroes).


RETURN VALUE
------------

``PMIx_Load_key`` returns no value (``void``).


NOTES
-----

``PMIx_Load_key`` is an OpenPMIx convenience routine and is the backing
implementation of the ``PMIX_LOAD_KEY`` macro.


.. seealso::
   :ref:`PMIx_Check_key(3) <man3-PMIx_Check_key>`,
   :ref:`PMIx_Check_reserved_key(3) <man3-PMIx_Check_reserved_key>`,
   :ref:`pmix_key_t(5) <man5-pmix_key_t>`
