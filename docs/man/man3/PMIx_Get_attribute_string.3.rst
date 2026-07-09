.. _man3-PMIx_Get_attribute_string:

PMIx_Get_attribute_string
=========================

.. include_body

``PMIx_Get_attribute_string`` |mdash| Return the string key corresponding to a
PMIx attribute name.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   const char* PMIx_Get_attribute_string(const char *attribute);


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

  from pmix import *

  foo = PMIxClient()
  key = foo.get_attribute_string("PMIX_UNIV_SIZE")


INPUT PARAMETERS
----------------

* ``attribute``: The name of a PMIx attribute as it appears in the header
  definitions |mdash| for example, ``"PMIX_UNIV_SIZE"``. The comparison is
  case-insensitive.


DESCRIPTION
-----------

Look up the given PMIx attribute name in the library's attribute dictionary and
return its corresponding string key |mdash| for example, ``"PMIX_UNIV_SIZE"`` maps
to ``"pmix.univ.size"``. This is the string representation transmitted on the wire
and used to label the attribute internally.

If the attribute name is not found in the dictionary, or if the dictionary has not
yet been initialized (for example, before :ref:`PMIx_Init(3) <man3-PMIx_Init>` has
been called), the function returns the input ``attribute`` pointer unchanged.

The returned pointer refers to constant storage owned by the PMIx library (or to
the caller-supplied ``attribute`` string in the not-found case). In either case the
caller must **not** modify or free it.

``PMIx_Get_attribute_name`` performs the inverse mapping, translating a string key
back to its attribute name.


RETURN VALUE
------------

Returns a pointer to a constant character string: the string key for the attribute
when known, or the unmodified input pointer when the attribute is unknown or the
dictionary is not yet initialized. The pointer must not be freed by the caller.


.. seealso::
   :ref:`PMIx_Get_attribute_name(3) <man3-PMIx_Get_attribute_name>`,
   :ref:`PMIx_Query_info(3) <man3-PMIx_Query_info>`
