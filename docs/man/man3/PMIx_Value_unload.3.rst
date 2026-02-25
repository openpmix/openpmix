.. _PMIx_Value_unload:

PMIx_Value_unload
=================

.. include_body

:ref:`PMIx_Value_unload` |mdash| Unload the contents of a ``pmix_value_t``

SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_status_t PMIx_Value_unload(pmix_value_t *val,
                                   void **data,
                                   size_t *sz);

Python Syntax
^^^^^^^^^^^^^

No Python equivalent - the ``pmix_value_t`` equivalent in Python is a
dictionary containing a ``value`` and ``type`` field. It is therefore
unnecessary to provide an equivalent "unload" function.


INPUT PARAMETERS
----------------

* ``val``: Pointer to a ``pmix_value_t`` struct containing the
  value to be unloaded. Note that the provided struct will not
  be altered in any way
* ``data``: Pointer to the location where the data value is to be
  returned
* ``sz``: Pointer to return the size of the unloaded value


DESCRIPTION
-----------

The `PMIx_Value_unload` function follows the format of the data in the
`pmix_value_t` structure. If the data type in the `pmix_value_t`:

   * is a simple data field (e.g., a `size_t`), then the function just copies
     the data across. The caller needs to provide the storage for the data.

   * is a pointer (e.g., the pmix_envar_t field), then the function malloc's
     the storage for the value, copies the fields into the new storage, and
     returns that storage to the caller. The caller needs to provide storage
     for the pointer to the returned value.

So in other words, the caller needs to provide storage (and a pointer to) the
place where the final data is to be put. If it's a simple data field, the value
will be placed in the provided storage. If it's a complex data field (e.g., a
struct), then the function will place the pointer to that malloc'd data in the
provided storage.

Note that this has an unusual side effect. For example, consider the case where
the ``pmix_value_t`` contains a ``pmix_byte_object_t``. In this case, the caller
would need to pass the address of a pointer to a ``pmix_byte_object_t`` to the
`PMIx_Value_unload` function since the byte object (and not its internal data) is what is
being unloaded. The returned size, therefore, is the size of a ``pmix_byte_object_t``,
*not* the number of bytes in the ``bytes`` field of the object.

RETURN VALUE
------------

Returns ``PMIX_SUCCESS`` on success. On error, a negative value
corresponding to a PMIx ``errno`` is returned.

ERRORS
------

PMIx ``errno`` values are defined in ``pmix_common.h``.

.. JMS COMMENT When more man pages are added, they can be :ref:'ed
   appropriately, so that HTML hyperlinks are created to link to the
   corresponding pages.

.. seealso::
   :ref:`PMIx_Init(3) <man3-PMIx_Init>`
