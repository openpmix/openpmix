.. _man3-PMIx_Resource_block_directive_string:

PMIx_Resource_block_directive_string
====================================

.. include_body

``PMIx_Resource_block_directive_string`` |mdash| Return the string
representation of a ``pmix_resource_block_directive_t`` value.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   const char* PMIx_Resource_block_directive_string(pmix_resource_block_directive_t directive);


INPUT PARAMETERS
----------------

* ``directive`` : A ``pmix_resource_block_directive_t`` resource-block
  directive.

Recognized values map to a human-readable string:

* ``PMIX_RESOURCE_BLOCK_DEFINE`` |mdash| ``"DEFINE"``
* ``PMIX_RESOURCE_BLOCK_EXTEND`` |mdash| ``"EXTEND"``
* ``PMIX_RESOURCE_BLOCK_REMOVE`` |mdash| ``"REMOVE"``
* ``PMIX_RESOURCE_BLOCK_DELETE`` |mdash| ``"DELETE"``


DESCRIPTION
-----------

Return a human-readable, statically-allocated string naming the given
``pmix_resource_block_directive_t`` value. The returned string is owned by the
library and must not be modified or freed by the caller. An unrecognized value
yields the fallback string ``"UNSPECIFIED"``.


RETURN VALUE
------------

Returns a pointer to a constant, statically-allocated, NUL-terminated string.
The pointer remains valid for the lifetime of the process and must not be
freed.


.. seealso::
   :ref:`PMIx_Resource_block(3) <man3-PMIx_Resource_block>`
