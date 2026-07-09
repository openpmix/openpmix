.. _man3-PMIx_Validate_credential:

PMIx_Validate_credential
========================

.. include_body

``PMIx_Validate_credential``, ``PMIx_Validate_credential_nb`` |mdash| Request
validation of a security credential by the PMIx server library or the host
environment.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_status_t PMIx_Validate_credential(const pmix_byte_object_t *cred,
                                          const pmix_info_t info[], size_t ninfo,
                                          pmix_info_t **results, size_t *nresults);

   pmix_status_t PMIx_Validate_credential_nb(const pmix_byte_object_t *cred,
                                             const pmix_info_t info[], size_t ninfo,
                                             pmix_validation_cbfunc_t cbfunc,
                                             void *cbdata);


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

  from pmix import *

  foo = PMIxClient()
  # ... after a successful foo.init() ...
  # the credential is a Python ``pmix_byte_object_t`` dictionary
  pycred = {'bytes': "mycredential", 'size': 12}
  # the directives is a list of Python ``pmix_info_t`` dictionaries
  pydirs = [{'key': PMIX_TIMEOUT,
             'value': {'value': 10, 'val_type': PMIX_INT}}]
  rc, results = foo.validate_credential(pycred, pydirs)
  # on success, results is a list of Python ``pmix_info_t`` dictionaries


INPUT PARAMETERS
----------------

* ``cred``: Pointer to a :ref:`pmix_byte_object_t(5) <man5-pmix_byte_object_t>`
  containing the credential to be validated.
* ``info``: Pointer to an array of :ref:`pmix_info_t(5) <man5-pmix_info_t>`
  structures conveying directives that qualify the operation (see `DIRECTIVES`_).
  A ``NULL`` value is supported when no directives are desired.
* ``ninfo``: Number of elements in the ``info`` array.


OUTPUT PARAMETERS
-----------------

* ``results`` (blocking form): Address where a pointer to an array of
  :ref:`pmix_info_t(5) <man5-pmix_info_t>` structures containing any information
  conveyed within the credential (for example, authorizations) is returned. The
  array is allocated by the library and must be released by the caller with
  ``PMIX_INFO_FREE`` when done.
* ``nresults`` (blocking form): Address where the number of elements in
  ``results`` is returned.

The non-blocking form replaces ``results`` and ``nresults`` with a callback:

* ``cbfunc``: Callback function of type ``pmix_validation_cbfunc_t`` invoked with
  the final status and any accompanying information once the request completes.
* ``cbdata``: Opaque pointer that is passed, unmodified, to ``cbfunc``.


DESCRIPTION
-----------

Request validation of a credential by the PMIx server library or the host
environment, returning any additional information (for example, authorizations)
conveyed within the credential.

``PMIx_Validate_credential`` is the blocking form: it does not return until the
credential has been validated (or the operation fails or times out). A successful
return indicates that the credential was valid and any information it contained
was successfully processed; the details are returned in the ``results`` array.

``PMIx_Validate_credential_nb`` is the non-blocking form: it returns immediately,
and the provided ``cbfunc`` is invoked with the final status once the request
completes. This version is generally preferred in scenarios where the host
environment may have to contact a remote credential service, and makes provision
for the system to return additional information regarding possible authorization
limitations beyond simple authentication. The information delivered to ``cbfunc``
is owned by the PMIx library and must not be released or altered by the receiving
party; it is unrelated to the ``info`` array passed into the call.

As with all non-blocking PMIx APIs, callers of ``PMIx_Validate_credential_nb``
**must** keep the ``cred`` and ``info`` arrays valid until ``cbfunc`` is invoked.

The request is serviced by the host environment if it supports the operation;
otherwise, the PMIx library attempts to validate the credential itself using an
internal security (``psec``) plugin. A client or tool that is not connected to a
local server, and a server whose host provides no credential support, are both
handled by the internal plugins.


DIRECTIVES
----------

The following attributes are relevant to this operation. There are no required
attributes; implementations may internally integrate with a security environment
(for example, contacting a *munge* server).

* ``PMIX_USERID`` (uint32_t) |mdash| the expected effective user id of the
  credential to be validated.
* ``PMIX_GRPID`` (uint32_t) |mdash| the expected effective group id of the
  credential to be validated.
* ``PMIX_TIMEOUT`` (int) |mdash| maximum time, in seconds, to wait for validation
  before timing out and returning an error. Optional for host environments that
  support the operation.

Implementations that support the operation but cannot directly process the
request pass the caller's attributes to the host environment; in that case the
``PMIX_USERID`` and ``PMIX_GRPID`` attributes are required to be included in the
array passed from the PMIx library to the host.


RETURN VALUE
------------

For the blocking form, ``PMIX_SUCCESS`` indicates that the credential was valid
and its contents were processed; any details are returned in ``results``. For the
non-blocking form, a return of ``PMIX_SUCCESS`` indicates only that the request
was accepted for processing; the final status and any information are delivered to
``cbfunc``. If the non-blocking form returns any value other than ``PMIX_SUCCESS``,
``cbfunc`` will **not** be called.

* ``PMIX_SUCCESS`` |mdash| the credential was valid (or, for the non-blocking
  form, the request was accepted; validity is reported to ``cbfunc``).
* ``PMIX_ERR_NOT_AVAILABLE`` |mdash| the operation cannot be serviced because the
  library's progress engine has been stopped.
* ``PMIX_ERR_INIT`` |mdash| the PMIx library has not been initialized.

If the connection to the local server is lost while a non-blocking request is
outstanding, ``cbfunc`` is invoked with a status of ``PMIX_ERR_COMM_FAILURE``.

Any other negative value indicates an appropriate error condition |mdash| in
particular, an error status reports that the credential was rejected. PMIx error
constants are defined in ``pmix_common.h``.


NOTES
-----

The credential is opaque and is understandable only by a service compatible with
its issuer. The information array returned by validation typically contains, at a
minimum, entries for the ``PMIX_USERID`` and ``PMIX_GRPID`` of the client
described in the credential, though the precise contents depend on the host
environment and its associated security system.


.. seealso::
   :ref:`PMIx_Init(3) <man3-PMIx_Init>`,
   :ref:`PMIx_Get_credential(3) <man3-PMIx_Get_credential>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`,
   :ref:`pmix_byte_object_t(5) <man5-pmix_byte_object_t>`,
   :ref:`pmix_status_t(5) <man5-pmix_status_t>`
