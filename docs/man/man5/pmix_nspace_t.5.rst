.. _man5-pmix_nspace_t:

pmix_nspace_t
=============

.. include_body

`pmix_nspace_t` |mdash| Character array holding a PMIx namespace

SYNTAX
------

C Syntax
^^^^^^^^

.. code-block:: c

   #include <pmix_common.h>

   typedef char pmix_nspace_t[PMIX_MAX_NSLEN+1];


DESCRIPTION
-----------

The `pmix_nspace_t` type is a statically defined character array of length
``PMIX_MAX_NSLEN+1``, thus supporting namespaces of maximum length
``PMIX_MAX_NSLEN`` (255) while preserving space for a mandatory ``NULL``
terminator.

A namespace is a character string assigned by the resource manager to a job;
all applications in that job share the same namespace. The string must be
unique within the scope of the governing resource manager, and is often the
string form of the numerical job ID.

References to namespace values in PMIx v1 were defined simply as an array of
characters of size ``PMIX_MAX_NSLEN+1``. The `pmix_nspace_t` type definition
was introduced in version 2 of the standard. The two definitions are
code-compatible and thus do not represent a break in backward compatibility.

Passing a `pmix_nspace_t` value to the standard ``sizeof`` utility can result
in compiler warnings of an incorrect returned value. Users are advised to avoid
using ``sizeof(pmix_nspace_t)`` and instead rely on the ``PMIX_MAX_NSLEN``
constant.


.. seealso::
   :ref:`pmix_proc_t(5) <man5-pmix_proc_t>`,
   :ref:`pmix_key_t(5) <man5-pmix_key_t>`
