.. _man3-PMIx_Get_attribute_name:

PMIx_Get_attribute_name
=======================

.. include_body

``PMIx_Get_attribute_name`` |mdash| Return the PMIx attribute name corresponding
to a string key.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   const char* PMIx_Get_attribute_name(const char *attrstring);


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

  from pmix import *

  foo = PMIxClient()
  name = foo.get_attribute_name("pmix.univ.size")


INPUT PARAMETERS
----------------

* ``attrstring``: The string key of a PMIx attribute |mdash| for example,
  ``"pmix.univ.size"``. The comparison is case-insensitive.


DESCRIPTION
-----------

Look up the given attribute string key in the library's attribute dictionary and
return its corresponding attribute name |mdash| for example, ``"pmix.univ.size"``
maps to ``"PMIX_UNIV_SIZE"``. This is the human-oriented name by which the
attribute is defined in the PMIx headers.

If the string key is not found in the dictionary, or if the dictionary has not yet
been initialized (for example, before :ref:`PMIx_Init(3) <man3-PMIx_Init>` has been
called), the function returns the input ``attrstring`` pointer unchanged.

The returned pointer refers to constant storage owned by the PMIx library (or to
the caller-supplied ``attrstring`` in the not-found case). In either case the
caller must **not** modify or free it.

``PMIx_Get_attribute_string`` performs the inverse mapping, translating an
attribute name to its string key.


RETURN VALUE
------------

Returns a pointer to a constant character string: the attribute name for the key
when known, or the unmodified input pointer when the key is unknown or the
dictionary is not yet initialized. The pointer must not be freed by the caller.


.. seealso::
   :ref:`PMIx_Get_attribute_string(3) <man3-PMIx_Get_attribute_string>`,
   :ref:`PMIx_Query_info(3) <man3-PMIx_Query_info>`
