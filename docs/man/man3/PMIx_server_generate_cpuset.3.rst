.. _man3-PMIx_server_generate_cpuset:

PMIx_server_generate_cpuset
===========================

.. include_body

``PMIx_server_generate_cpuset`` |mdash| Generate a cpuset from a PMIx
string representation.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix_server.h>

   pmix_status_t PMIx_server_generate_cpuset(const char *cpuset_string,
                                             pmix_cpuset_t *cpuset);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``cpuset_string``: NULL-terminated PMIx string representation of a
  cpuset, as produced by :ref:`PMIx_server_generate_cpuset_string(3)
  <man3-PMIx_server_generate_cpuset_string>`. The string must begin with
  a colon-delimited ``source`` prefix (e.g., ``"hwloc:0-3"``) identifying
  the subsystem able to decode the remainder.


OUTPUT PARAMETERS
-----------------

* ``cpuset``: Pointer to caller-provided storage for a
  :ref:`pmix_cpuset_t(5) <man5-pmix_cpuset_t>` structure. On success, the
  ``source`` and ``bitmap`` fields are populated with a decoded copy of
  the bitmap. The caller must provide the storage for the structure
  itself; the library allocates the internal ``source`` string and
  ``bitmap`` object, which the caller is responsible for releasing (for
  example, via ``PMIx_Cpuset_destruct``).


DESCRIPTION
-----------

Provide a function by which the host environment can convert a PMIx
string representation of a cpuset into the corresponding cpuset bitmap.
This is the reciprocal of :ref:`PMIx_server_generate_cpuset_string(3)
<man3-PMIx_server_generate_cpuset_string>`: the ``source`` prefix embedded
in the string is used to route the request to the subsystem (e.g.,
``hwloc``) that can decode it, and the decoded bitmap is returned in the
caller-supplied structure.

The operation is performed synchronously in the caller's thread; it does
not thread-shift into the PMIx progress engine and does not invoke a
callback.

This API is a PMIx library convenience routine; it is not part of the
PMIx Standard.


RETURN VALUE
------------

Returns one of the following:

* ``PMIX_SUCCESS`` |mdash| the string was parsed and the bitmap returned
  in ``cpuset``.
* ``PMIX_ERR_INIT`` |mdash| the PMIx server library has not been
  initialized.
* ``PMIX_ERR_BAD_PARAM`` |mdash| the string was malformed |mdash| for
  example, it lacked a colon-delimited ``source`` prefix or the encoded
  bitmap could not be decoded.
* another non-success PMIx error constant |mdash| no configured provider
  recognized the ``source`` prefix of the supplied string, so no cpuset
  could be generated.

PMIx error constants are defined in ``pmix_common.h``.


NOTES
-----

This API is restricted to the server role and must be called only after a
successful :ref:`PMIx_server_init(3) <man3-PMIx_server_init>`. The caller
owns the contents populated into the ``cpuset`` structure and must release
them when no longer needed.


.. seealso::
   :ref:`PMIx_server_init(3) <man3-PMIx_server_init>`,
   :ref:`PMIx_server_generate_cpuset_string(3) <man3-PMIx_server_generate_cpuset_string>`,
   :ref:`PMIx_server_generate_locality_string(3) <man3-PMIx_server_generate_locality_string>`,
   :ref:`pmix_cpuset_t(5) <man5-pmix_cpuset_t>`,
   :ref:`pmix_status_t(5) <man5-pmix_status_t>`
