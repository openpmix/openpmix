.. _man3-PMIx_server_generate_locality_string:

PMIx_server_generate_locality_string
====================================

.. include_body

``PMIx_server_generate_locality_string`` |mdash| Generate a PMIx locality
string from a given cpuset.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix_server.h>

   pmix_status_t PMIx_server_generate_locality_string(const pmix_cpuset_t *cpuset,
                                                      char **locality);


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

  from pmix import *

  foo = PMIxServer()
  # ... after a successful foo.init() ...
  # cpuset is a Python dict describing the bitmap
  rc, locality = foo.generate_locality_string(cpuset)


INPUT PARAMETERS
----------------

* ``cpuset``: Pointer to a :ref:`pmix_cpuset_t(5) <man5-pmix_cpuset_t>`
  structure containing the bitmap of assigned processing units (PUs). The
  ``source`` field identifies the subsystem (e.g., ``"hwloc"``) that
  produced the bitmap.


OUTPUT PARAMETERS
-----------------

* ``locality``: Address where a pointer to a freshly allocated,
  NULL-terminated string representation of the PMIx locality corresponding
  to the input bitmap is returned. The caller is responsible for releasing
  the string with ``free()``. If the process is not bound (the bitmap is
  ``NULL`` or fully set), ``locality`` is set to ``NULL`` and
  ``PMIX_SUCCESS`` is returned.


DESCRIPTION
-----------

Provide a function by which the host environment can generate a PMIx
locality string for inclusion in the call to
:ref:`PMIx_server_register_nspace(3) <man3-PMIx_server_register_nspace>`.
This function shall only be called for local client processes, with the
returned locality included in the job-level information (via the
``PMIX_LOCALITY_STRING`` attribute) provided to local clients. Local
clients can use these strings as input to determine the relative locality
of their local peers via the ``PMIx_Get_relative_locality`` API.

The returned string is prefixed by the ``source`` field of the provided
``cpuset`` followed by a colon; the remainder of the string represents the
corresponding locality (the set of topology levels |mdash| package, NUMA
domain, cache levels, core, PU |mdash| the process shares) as expressed by
the underlying implementation.

The operation is performed synchronously in the caller's thread; it does
not thread-shift into the PMIx progress engine and does not invoke a
callback.


RETURN VALUE
------------

Returns one of the following:

* ``PMIX_SUCCESS`` |mdash| the locality string was generated and returned
  in ``locality`` (which may be ``NULL`` when the process is unbound).
* ``PMIX_ERR_INIT`` |mdash| the PMIx server library has not been
  initialized.
* another non-success PMIx error constant |mdash| no configured provider
  recognized the ``source`` of the supplied cpuset, so no locality string
  could be generated.

PMIx error constants are defined in ``pmix_common.h``.


NOTES
-----

This API is restricted to the server role and must be called only after a
successful :ref:`PMIx_server_init(3) <man3-PMIx_server_init>`. The
returned string is owned by the caller.


.. seealso::
   :ref:`PMIx_server_init(3) <man3-PMIx_server_init>`,
   :ref:`PMIx_server_register_nspace(3) <man3-PMIx_server_register_nspace>`,
   :ref:`PMIx_server_generate_cpuset_string(3) <man3-PMIx_server_generate_cpuset_string>`,
   :ref:`pmix_cpuset_t(5) <man5-pmix_cpuset_t>`,
   :ref:`pmix_status_t(5) <man5-pmix_status_t>`
