.. _man3-PMIx_generate_regex2:

PMIx_generate_regex2
====================

.. include_body

``PMIx_generate_regex2`` |mdash| Generate a compressed encoded
representation of a list of values.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix_server.h>

   pmix_status_t PMIx_generate_regex2(const char *input,
                                      pmix_info_t info[], size_t ninfo,
                                      pmix_regex2_t *regex);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``input``: A comma-separated list of values (for example, a list of node
  names) to be encoded.
* ``info``: Optional array of :ref:`pmix_info_t(5) <man5-pmix_info_t>`
  directives influencing the encoding (see `DIRECTIVES`_). May be ``NULL``.
* ``ninfo``: Number of elements in the ``info`` array; zero if none.


OUTPUT PARAMETERS
-----------------

* ``regex``: Pointer to caller-provided storage for a
  ``pmix_regex2_t`` structure into which the
  encoded representation is placed. The structure carries a colon-delimited
  ``type`` string identifying the encoding method, a ``bytes`` buffer
  holding the encoded representation (which may **not** be
  NULL-terminated), and a ``len`` giving the number of bytes. The caller is
  responsible for releasing the returned data (for example, via
  ``PMIx_Regex2_destruct``).


DESCRIPTION
-----------

Given a comma-separated list of ``input`` values, generate a reduced-size
representation of the input that can be passed to
:ref:`PMIx_server_register_nspace(3) <man3-PMIx_server_register_nspace>`
for processing, or expanded again with
:ref:`PMIx_parse_regex2(3) <man3-PMIx_parse_regex2>`. The order of the
individual values in the ``input`` string is preserved across the
generate/parse operation.

``PMIx_generate_regex2`` replaces the deprecated ``PMIx_generate_regex``.
Rather than returning a raw ``char*`` |mdash| which incorrectly implied a
NULL-terminated string |mdash| the new API returns a
``pmix_regex2_t``, which properly conveys that
the encoded output may be an arbitrary binary byte array. The precise
encoding is implementation specific; the leading colon-delimited ``type``
string (e.g., ``"pmix"``, ``"raw"``, or ``"blob"``) identifies the method
used so that a decoder can select the correct expansion algorithm.

The operation is performed synchronously in the caller's thread; it does
not thread-shift into the PMIx progress engine and does not invoke a
callback.


DIRECTIVES
----------

The ``info`` array is provided so that encoding behavior can be tuned or
extended without changing the API signature. No standardized attributes
are currently defined for this API; an implementation is free to define
its own, and unrecognized directives are ignored. Callers that require no
special handling may pass ``NULL`` / ``0``.


RETURN VALUE
------------

Returns one of the following:

* ``PMIX_SUCCESS`` |mdash| the encoded representation was generated and
  placed in ``regex``.
* ``PMIX_ERR_INIT`` |mdash| the PMIx library has not been initialized.
* ``PMIX_ERR_BAD_PARAM`` |mdash| ``regex`` was ``NULL``.

Any other negative value indicates an appropriate error condition. PMIx
error constants are defined in ``pmix_common.h``.


NOTES
-----

The returned ``pmix_regex2_t`` is owned by the caller and must be released
when no longer needed. To communicate the encoded representation to a
remote peer, pack it using ``PMIx_Data_pack`` with the ``PMIX_REGEX2``
type; the pack/unpack routines handle the encoding-specific framing based
on the ``type`` prefix.


.. seealso::
   :ref:`PMIx_parse_regex2(3) <man3-PMIx_parse_regex2>`,
   :ref:`PMIx_server_register_nspace(3) <man3-PMIx_server_register_nspace>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`,
   :ref:`pmix_status_t(5) <man5-pmix_status_t>`
