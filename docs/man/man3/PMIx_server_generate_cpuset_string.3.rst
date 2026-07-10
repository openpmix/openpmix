.. _man3-PMIx_server_generate_cpuset_string:

PMIx_server_generate_cpuset_string
==================================

.. include_body

``PMIx_server_generate_cpuset_string`` |mdash| Generate a PMIx string
representation of a provided cpuset.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix_server.h>

   pmix_status_t PMIx_server_generate_cpuset_string(const pmix_cpuset_t *cpuset,
                                                    char **cpuset_string);


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

  from pmix import *

  foo = PMIxServer()
  # ... after a successful foo.init() ...
  # cpuset is a Python dict describing the bitmap (e.g. as
  # returned by foo.get_cpuset())
  rc, cpuset_string = foo.generate_cpuset_string(cpuset)


INPUT PARAMETERS
----------------

* ``cpuset``: Pointer to a :ref:`pmix_cpuset_t(5) <man5-pmix_cpuset_t>`
  structure containing the bitmap of assigned processing units (PUs). The
  ``source`` field identifies the subsystem (e.g., ``"hwloc"``) that
  produced the bitmap, and the ``bitmap`` field points to the
  corresponding implementation-specific object.


OUTPUT PARAMETERS
-----------------

* ``cpuset_string``: Address where a pointer to a freshly allocated,
  NULL-terminated string representation of the input bitmap is returned.
  The caller is responsible for releasing the string with ``free()``.


DESCRIPTION
-----------

Provide a function by which the host environment can generate a string
representation of a cpuset bitmap for inclusion in the call to
:ref:`PMIx_server_register_nspace(3) <man3-PMIx_server_register_nspace>`.
This function shall only be called for local client processes, with the
returned string included in the job-level information (via the
``PMIX_CPUSET`` attribute) provided to local clients. Local clients can
use these strings to obtain their PU bindings via the
``PMIx_Parse_cpuset_string`` API.

The returned string is prefixed by the ``source`` field of the provided
``cpuset`` followed by a colon (e.g., ``"hwloc:0-3"``); the remainder of
the string represents the PUs to which the process is bound as expressed
by the underlying implementation. This prefix allows the reciprocal
operation, :ref:`PMIx_server_generate_cpuset(3)
<man3-PMIx_server_generate_cpuset>`, to route the string back to the
subsystem that can decode it.

The operation is performed synchronously in the caller's thread; it does
not thread-shift into the PMIx progress engine and does not invoke a
callback.


RETURN VALUE
------------

Returns one of the following:

* ``PMIX_SUCCESS`` |mdash| the string was generated and returned in
  ``cpuset_string``.
* ``PMIX_ERR_INIT`` |mdash| the PMIx server library has not been
  initialized.
* ``PMIX_ERR_BAD_PARAM`` |mdash| ``cpuset`` was ``NULL`` or its ``bitmap``
  field was ``NULL``. In this case ``cpuset_string`` is set to ``NULL``.
* another non-success PMIx error constant |mdash| no configured provider
  recognized the ``source`` of the supplied cpuset, so no string could be
  generated.

PMIx error constants are defined in ``pmix_common.h``.


NOTES
-----

This API is restricted to the server role and must be called only after a
successful :ref:`PMIx_server_init(3) <man3-PMIx_server_init>`. The
returned string is owned by the caller.


.. seealso::
   :ref:`PMIx_server_init(3) <man3-PMIx_server_init>`,
   :ref:`PMIx_server_register_nspace(3) <man3-PMIx_server_register_nspace>`,
   :ref:`PMIx_server_generate_cpuset(3) <man3-PMIx_server_generate_cpuset>`,
   :ref:`PMIx_server_generate_locality_string(3) <man3-PMIx_server_generate_locality_string>`,
   :ref:`pmix_cpuset_t(5) <man5-pmix_cpuset_t>`,
   :ref:`pmix_status_t(5) <man5-pmix_status_t>`
