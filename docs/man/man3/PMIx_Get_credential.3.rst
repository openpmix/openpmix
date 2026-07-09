.. _man3-PMIx_Get_credential:

PMIx_Get_credential
===================

.. include_body

``PMIx_Get_credential``, ``PMIx_Get_credential_nb`` |mdash| Request a security
credential from the PMIx server library or the host environment.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_status_t PMIx_Get_credential(const pmix_info_t info[], size_t ninfo,
                                     pmix_byte_object_t *credential);

   pmix_status_t PMIx_Get_credential_nb(const pmix_info_t info[], size_t ninfo,
                                        pmix_credential_cbfunc_t cbfunc,
                                        void *cbdata);


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

  from pmix import *

  foo = PMIxClient()
  # ... after a successful foo.init() ...
  # the directives is a list of Python ``pmix_info_t`` dictionaries
  pydirs = [{'key': PMIX_TIMEOUT,
             'value': {'value': 10, 'val_type': PMIX_INT}}]
  rc, cred = foo.get_credential(pydirs)
  # on success, cred is a dictionary with 'bytes' and 'size' members


INPUT PARAMETERS
----------------

* ``info``: Pointer to an array of :ref:`pmix_info_t(5) <man5-pmix_info_t>`
  structures conveying directives that qualify the operation (see `DIRECTIVES`_).
  A ``NULL`` value is supported when no directives are desired.
* ``ninfo``: Number of elements in the ``info`` array.


OUTPUT PARAMETERS
-----------------

* ``credential`` (blocking form): Address of a ``pmix_byte_object_t`` within
  which the credential is returned. The credential is opaque to the caller |mdash|
  it is returned as a byte object to support arbitrary binary formats, and no
  information about its source is provided. On success the object's ``bytes``
  buffer is allocated by the library; the caller is responsible for releasing it
  with ``PMIX_BYTE_OBJECT_DESTRUCT`` when done.

The non-blocking form replaces ``credential`` with a callback:

* ``cbfunc``: Callback function of type ``pmix_credential_cbfunc_t`` invoked with
  the final status, the returned credential, and any accompanying information once
  the request completes.
* ``cbdata``: Opaque pointer that is passed, unmodified, to ``cbfunc``.


DESCRIPTION
-----------

Request a credential from the PMIx server library or the host environment. The
credential is returned as a :ref:`pmix_byte_object_t(5) <man5-pmix_byte_object_t>`
to support potential binary formats |mdash| it is therefore opaque to the caller,
and no information as to the source of the credential is provided.

``PMIx_Get_credential`` is the blocking form: it does not return until the
credential is available (or the operation fails or times out). On success the
credential is returned in the caller-provided ``credential`` object.

``PMIx_Get_credential_nb`` is the non-blocking form: it returns immediately, and
the provided ``cbfunc`` is invoked with the final status once the request
completes. This version is generally preferred in scenarios where the host
environment may have to contact a remote credential service, since it allows the
system to return additional information (for example, the identity of the issuing
agent) outside of the credential itself and visible to the application. That
information is delivered to ``cbfunc`` in its ``info`` array, which is owned by
the PMIx library and must not be released or altered by the receiving party.

As with all non-blocking PMIx APIs, callers of ``PMIx_Get_credential_nb`` **must**
keep the ``info`` array valid until ``cbfunc`` is invoked.

The request is serviced by the host environment if it supports the operation;
otherwise, the PMIx library attempts to generate the credential itself using an
internal security (``psec``) plugin. A client or tool that is not connected to a
local server, and a server whose host provides no credential support, are both
handled by the internal plugins.


DIRECTIVES
----------

The following attributes are relevant to this operation. There are no required
attributes; implementations may internally integrate with a security environment
(for example, contacting a *munge* server).

* ``PMIX_CRED_TYPE`` (char*) |mdash| a prioritized, comma-delimited list of
  desired credential types, for use in environments where multiple authentication
  mechanisms may be available. When returned in the callback, a string identifier
  of the credential type that was actually provided.
* ``PMIX_TIMEOUT`` (int) |mdash| maximum time, in seconds, to wait for a
  credential before timing out and returning an error. Optional for host
  environments that support the operation.

Implementations that support the operation but cannot directly process the
request pass the caller's attributes to the host environment; in that case the
``PMIX_USERID`` and ``PMIX_GRPID`` attributes are required to be included in the
array passed from the PMIx library to the host.


RETURN VALUE
------------

For the blocking form, ``PMIX_SUCCESS`` indicates that the credential was returned
in the provided ``credential`` object. For the non-blocking form, a return of
``PMIX_SUCCESS`` indicates only that the request was accepted for processing; the
final status, credential, and any information are delivered to ``cbfunc``. If the
non-blocking form returns any value other than ``PMIX_SUCCESS``, ``cbfunc`` will
**not** be called.

* ``PMIX_SUCCESS`` |mdash| the credential has been (or, for the non-blocking form,
  will be) returned.
* ``PMIX_ERR_NOT_AVAILABLE`` |mdash| the operation cannot be serviced because the
  library's progress engine has been stopped.
* ``PMIX_ERR_INIT`` |mdash| the PMIx library has not been initialized.

If the connection to the local server is lost while a non-blocking request is
outstanding, ``cbfunc`` is invoked with a status of ``PMIX_ERR_COMM_FAILURE``.

Any other negative value indicates an appropriate error condition. PMIx error
constants are defined in ``pmix_common.h``.


NOTES
-----

The credential is opaque and is understandable only by a service compatible with
its issuer. The information array delivered to the non-blocking callback is owned
by the PMIx library and must not be released or modified by the caller; it is
unrelated to the ``info`` array passed into the call.


.. seealso::
   :ref:`PMIx_Init(3) <man3-PMIx_Init>`,
   :ref:`PMIx_Validate_credential(3) <man3-PMIx_Validate_credential>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`,
   :ref:`pmix_byte_object_t(5) <man5-pmix_byte_object_t>`,
   :ref:`pmix_status_t(5) <man5-pmix_status_t>`
