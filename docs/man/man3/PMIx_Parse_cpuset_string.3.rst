.. _man3-PMIx_Parse_cpuset_string:

PMIx_Parse_cpuset_string
========================

.. include_body

``PMIx_Parse_cpuset_string`` |mdash| Parse the processing-unit (PU) binding
bitmap from its string representation.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_status_t PMIx_Parse_cpuset_string(const char *cpuset_string,
                                          pmix_cpuset_t *cpuset);


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

  from pmix import *

  foo = PMIxClient()
  # ... after a successful foo.init() ...
  rc, cpuset = foo.parse_cpuset_string("hwloc:0-3")


INPUT PARAMETERS
----------------

* ``cpuset_string``: NULL-terminated string representation of the binding
  bitmap, as returned by :ref:`PMIx_Get(3) <man3-PMIx_Get>` using the
  ``PMIX_CPUSET`` key. The string is expected to carry a source prefix
  (e.g., ``"hwloc:"``) followed by the source-specific bitmap encoding.


OUTPUT PARAMETERS
-----------------

* ``cpuset``: Pointer to a ``pmix_cpuset_t`` object where the parsed bitmap is
  to be stored. On success, the object's ``source`` and ``bitmap`` fields are
  populated; the caller is responsible for releasing the associated storage
  with ``PMIx_Cpuset_destruct`` when done.


DESCRIPTION
-----------

Parse the string representation of the binding bitmap (as returned by
:ref:`PMIx_Get(3) <man3-PMIx_Get>` using the ``PMIX_CPUSET`` key) and set the
corresponding PU binding location information in the provided
``pmix_cpuset_t`` object.

The library selects the parser that matches the source prefix of
``cpuset_string``. If the ``source`` field of ``cpuset`` was pre-set by the
caller and does not match the underlying source that provided the binding
bitmap, an error is returned.


RETURN VALUE
------------

Returns ``PMIX_SUCCESS`` on success, with the bitmap loaded into ``cpuset``.
On error, a negative value corresponding to a PMIx error constant is
returned, including:

* ``PMIX_ERR_INIT`` |mdash| the PMIx library has not been initialized.
* ``PMIX_ERR_BAD_PARAM`` |mdash| the string is malformed |mdash| for example,
  it lacks the ``source:`` delimiter, or the source-specific bitmap encoding
  could not be parsed.

Any other negative value indicates an appropriate error condition. PMIx error
constants are defined in ``pmix_common.h``.


NOTES
-----

Unlike most PMIx APIs, ``PMIx_Parse_cpuset_string`` does not accept an array
of :ref:`pmix_info_t(5) <man5-pmix_info_t>` directives. This is an
intentional exception: the operation is a purely local parse of a string into
a bitmap and takes no attributes.


.. seealso::
   :ref:`PMIx_Get(3) <man3-PMIx_Get>`,
   :ref:`PMIx_Get_cpuset(3) <man3-PMIx_Get_cpuset>`,
   :ref:`PMIx_Get_relative_locality(3) <man3-PMIx_Get_relative_locality>`,
   :ref:`PMIx_Compute_distances(3) <man3-PMIx_Compute_distances>`,
   :ref:`PMIx_Load_topology(3) <man3-PMIx_Load_topology>`,
   :ref:`pmix_status_t(5) <man5-pmix_status_t>`,
   :ref:`pmix_cpuset_t(5) <man5-pmix_cpuset_t>`
