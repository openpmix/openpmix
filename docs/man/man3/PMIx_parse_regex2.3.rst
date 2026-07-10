.. _man3-PMIx_parse_regex2:

PMIx_parse_regex2
=================

.. include_body

``PMIx_parse_regex2`` |mdash| Expand an encoded representation produced by
``PMIx_generate_regex2`` back into its list of values.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix_server.h>

   pmix_status_t PMIx_parse_regex2(const pmix_regex2_t *regex,
                                   pmix_info_t info[], size_t ninfo,
                                   char **output);


Python Syntax
^^^^^^^^^^^^^

No Python equivalent


INPUT PARAMETERS
----------------

* ``regex``: Pointer to a :ref:`pmix_regex2_t(5) <man5-pmix_regex2_t>` structure as returned by
  :ref:`PMIx_generate_regex2(3) <man3-PMIx_generate_regex2>`. Its
  colon-delimited ``type`` prefix selects the decoding method.
* ``info``: Optional array of :ref:`pmix_info_t(5) <man5-pmix_info_t>`
  directives influencing the expansion (see `DIRECTIVES`_). May be
  ``NULL``.
* ``ninfo``: Number of elements in the ``info`` array; zero if none.


OUTPUT PARAMETERS
-----------------

* ``output``: Address where a pointer to a freshly allocated,
  NULL-terminated string containing the comma-separated list of the
  individual values is returned. The caller is responsible for releasing
  the returned string with ``free()``.


DESCRIPTION
-----------

Given a ``pmix_regex2_t`` as returned by
:ref:`PMIx_generate_regex2(3) <man3-PMIx_generate_regex2>`, expand the
encoded representation and return a NULL-terminated string of the
individual values in ``output``. The order of the values is identical to
the order of the values in the original ``input`` string passed to
``PMIx_generate_regex2``.

The ``type`` prefix carried by the ``regex`` structure identifies the
encoding method (e.g., ``"pmix"``, ``"raw"``, or ``"blob"``) so that the
library can select the corresponding decoder. This is the reciprocal
operation of :ref:`PMIx_generate_regex2(3) <man3-PMIx_generate_regex2>`.

The operation is performed synchronously in the caller's thread; it does
not thread-shift into the PMIx progress engine and does not invoke a
callback.


DIRECTIVES
----------

The ``info`` array is provided so that decoding behavior can be tuned or
extended without changing the API signature. No standardized attributes
are currently defined for this API; an implementation is free to define
its own, and unrecognized directives are ignored. Callers that require no
special handling may pass ``NULL`` / ``0``.


RETURN VALUE
------------

Returns one of the following:

* ``PMIX_SUCCESS`` |mdash| the encoded representation was expanded and the
  resulting string returned in ``output``.
* ``PMIX_ERR_INIT`` |mdash| the PMIx library has not been initialized.
* ``PMIX_ERR_BAD_PARAM`` |mdash| ``regex`` or ``output`` was ``NULL``.

Any other negative value indicates an appropriate error condition. PMIx
error constants are defined in ``pmix_common.h``.


NOTES
-----

The returned ``output`` string is owned by the caller and must be released
with ``free()`` when no longer needed.


.. seealso::
   :ref:`PMIx_generate_regex2(3) <man3-PMIx_generate_regex2>`,
   :ref:`PMIx_server_register_nspace(3) <man3-PMIx_server_register_nspace>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`,
   :ref:`pmix_status_t(5) <man5-pmix_status_t>`,
   :ref:`pmix_regex2_t(5) <man5-pmix_regex2_t>`
