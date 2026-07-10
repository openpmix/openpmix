.. _man3-PMIx_Info_destruct:

PMIx_Info_destruct
==================

.. include_body

``PMIx_Info_destruct`` |mdash| Release the contents of a :ref:`pmix_info_t(5) <man5-pmix_info_t>`


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_Info_destruct(pmix_info_t *p);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``p``: Pointer to the :ref:`pmix_info_t(5) <man5-pmix_info_t>` structure whose
  contents are to be released. The storage for the structure itself is *not*
  freed |mdash| it is assumed to belong to the caller (typically declared on the
  stack or embedded within another object).


DESCRIPTION
-----------

The ``PMIx_Info_destruct`` function releases any memory held by the
:ref:`pmix_value_t(5) <man5-pmix_value_t>` payload contained within the info
structure. It does **not** free the ``pmix_info_t`` storage itself; that
remains the responsibility of the caller.

If the info structure has been marked persistent (see
:ref:`PMIx_Info_persistent(3) <man3-PMIx_Info_persistent>`), the contained
value is left untouched |mdash| a persistent info does not own its value, so its
memory is deliberately not released.

``PMIx_Info_destruct`` is the counterpart to
:ref:`PMIx_Info_construct(3) <man3-PMIx_Info_construct>`. A structure that was
allocated as part of an array by :ref:`PMIx_Info_create(3) <man3-PMIx_Info_create>`
should be released with :ref:`PMIx_Info_free(3) <man3-PMIx_Info_free>` instead,
which destructs each element and frees the array storage.


RETURN VALUE
------------

``PMIx_Info_destruct`` returns no value (``void``).


NOTES
-----

``PMIx_Info_destruct`` is the OpenPMIx routine underlying the historical
``PMIX_INFO_DESTRUCT`` macro. That macro is retained in ``pmix_deprecated.h``
for backward compatibility and simply expands to a call to this function; new
code may call either form.


.. seealso::
   :ref:`PMIx_Info_construct(3) <man3-PMIx_Info_construct>`,
   :ref:`PMIx_Info_create(3) <man3-PMIx_Info_create>`,
   :ref:`PMIx_Info_free(3) <man3-PMIx_Info_free>`,
   :ref:`PMIx_Info_persistent(3) <man3-PMIx_Info_persistent>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`,
   :ref:`pmix_value_t(5) <man5-pmix_value_t>`
