.. _man3-PMIx_Info_directives_string:

PMIx_Info_directives_string
===========================

.. include_body

``PMIx_Info_directives_string`` |mdash| Return an allocated string representation of a :ref:`pmix_info_directives_t(5) <man5-pmix_info_directives_t>` bit-mask.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   char* PMIx_Info_directives_string(pmix_info_directives_t directives);


Python Syntax
^^^^^^^^^^^^^^

.. code-block:: python3

  from pmix import *

  foo = PMIxClient()
  # directives is an integer bit-mask of PMIX_INFO_* flags
  string = foo.info_directives_string(PMIX_INFO_REQD)


INPUT PARAMETERS
----------------

* ``directives``: A :ref:`pmix_info_directives_t(5) <man5-pmix_info_directives_t>`
  bit-mask of ``PMIX_INFO_*`` flags to be rendered.


DESCRIPTION
-----------

Build a human-readable rendering of the supplied
:ref:`pmix_info_directives_t(5) <man5-pmix_info_directives_t>` bit-mask. The set
flags are rendered as colon-separated tokens, for example ``QUALIFIER`` for
``PMIX_INFO_QUALIFIER``, ``REQUIRED`` for ``PMIX_INFO_REQD`` (``OPTIONAL`` when the
required bit is clear), ``PROCESSED`` for ``PMIX_INFO_REQD_PROCESSED``, and ``END``
for ``PMIX_INFO_ARRAY_END``. When no recognized bit is set, the string
``UNSPECIFIED`` is returned.

Unlike the ``const char*`` converter routines (such as
:ref:`PMIx_Error_string(3) <man3-PMIx_Error_string>`), which return a pointer to
storage owned by the library, ``PMIx_Info_directives_string`` returns a freshly
**heap-allocated** buffer (produced with ``strdup`` or the library's argv-join
helper, both of which use ``malloc``). The **caller is responsible for releasing
it** with the standard C library ``free()`` when it is no longer needed.


RETURN VALUE
------------

Returns a pointer to an allocated, NULL-terminated string that the caller must
release with ``free()``. Because the argument is passed by value, this routine
always returns an allocated string (``UNSPECIFIED`` when no recognized flag is set)
rather than ``NULL``.


NOTES
-----

The returned buffer must be freed with ``free()``; do not pass it to
``PMIX_RELEASE`` or any other PMIx destructor macro.


.. seealso::
   :ref:`PMIx_Info_construct(3) <man3-PMIx_Info_construct>`,
   :ref:`PMIx_Info_string(3) <man3-PMIx_Info_string>`
