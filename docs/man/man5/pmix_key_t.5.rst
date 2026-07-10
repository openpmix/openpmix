.. _man5-pmix_key_t:

pmix_key_t
==========

.. include_body

`pmix_key_t` |mdash| Character array holding an attribute key

SYNTAX
------

C Syntax
^^^^^^^^

.. code-block:: c

   #include <pmix_common.h>

   typedef char pmix_key_t[PMIX_MAX_KEYLEN+1];


DESCRIPTION
-----------

The `pmix_key_t` type is a statically defined character array of length
``PMIX_MAX_KEYLEN+1``, thus supporting keys of maximum length
``PMIX_MAX_KEYLEN`` (511) while preserving space for a mandatory ``NULL``
terminator.

A key is the string component of an attribute |mdash| a key-value pair passed
to APIs such as :ref:`PMIx_Get(3) <man3-PMIx_Get>` or
:ref:`PMIx_Query_info(3) <man3-PMIx_Query_info>` to identify requested
information.

References to keys in PMIx v1 were defined simply as an array of characters of
size ``PMIX_MAX_KEYLEN+1``. The `pmix_key_t` type definition was introduced in
version 2 of the standard. The two definitions are code-compatible and thus do
not represent a break in backward compatibility.

Passing a `pmix_key_t` value to the standard ``sizeof`` utility can result in
compiler warnings of an incorrect returned value. Users are advised to avoid
using ``sizeof(pmix_key_t)`` and instead rely on the ``PMIX_MAX_KEYLEN``
constant.


.. seealso::
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`,
   :ref:`pmix_nspace_t(5) <man5-pmix_nspace_t>`
