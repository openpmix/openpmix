.. _man3-PMIx_Finalize:

PMIx_Finalize
=============

.. include_body

``PMIx_Finalize`` |mdash| Finalize the PMIx client library.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_status_t PMIx_Finalize(const pmix_info_t info[], size_t ninfo);


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

  from pmix import *

  foo = PMIxClient()
  # ... after a successful foo.init() ...
  # the directives is a list of Python ``pmix_info_t`` dictionaries
  pydirs = [{'key': PMIX_EMBED_BARRIER,
             'value': {'value': True, 'val_type': PMIX_BOOL}}]
  rc = foo.finalize(pydirs)


INPUT PARAMETERS
----------------

* ``info``: Pointer to an array of :ref:`pmix_info_t(5) <man5-pmix_info_t>`
  structures conveying directives that qualify the operation (see `DIRECTIVES`_).
  A ``NULL`` value is supported when no directives are desired.
* ``ninfo``: Number of elements in the ``info`` array.


DESCRIPTION
-----------

Finalize the PMIx client, closing the connection with the local PMIx server.
An error code is returned if, for some reason, the connection cannot be closed.

Because the PMIx client library is reference counted (see
:ref:`PMIx_Init(3) <man3-PMIx_Init>`), ``PMIx_Finalize`` decrements the library
reference count. Each call to ``PMIx_Init`` must be balanced with a matching call
to ``PMIx_Finalize``. Only when the reference count reaches zero |mdash| i.e., on
the call that balances the first ``PMIx_Init`` |mdash| does the library actually
finalize the client, close the connection to the local server, and release all
internally allocated memory. Intermediate calls simply decrement the count and
return ``PMIX_SUCCESS``.

By default, ``PMIx_Finalize`` does **not** include an internal barrier operation:
it returns as soon as the local bookkeeping and (on the final call) the
server-side termination handshake are complete. Callers that require all
participating processes to synchronize before disconnecting may request an
embedded barrier via the ``PMIX_EMBED_BARRIER`` directive (see `DIRECTIVES`_).


DIRECTIVES
----------

The following attribute is relevant to this operation. Support for it is optional
and depends on how the PMIx implementation was built.

* ``PMIX_EMBED_BARRIER`` (bool) |mdash| execute a blocking fence operation across
  the caller's namespace as part of the finalize operation. ``PMIx_Finalize``
  performs no internal barrier by default; setting this attribute to ``true``
  directs it to run the equivalent of a :ref:`PMIx_Fence(3) <man3-PMIx_Fence>`
  over the caller's namespace before disconnecting from the local server.


RETURN VALUE
------------

Returns ``PMIX_SUCCESS`` on success, including on intermediate calls that only
decrement the reference count. On error, a negative value corresponding to a
PMIx error constant is returned, including:

* ``PMIX_ERR_INIT`` |mdash| the PMIx library has not been initialized, so there is
  nothing to finalize.

Any other negative value indicates an appropriate error condition. PMIx error
constants are defined in ``pmix_common.h``.


NOTES
-----

``PMIx_Finalize`` is the counterpart to :ref:`PMIx_Init(3) <man3-PMIx_Init>` and
is intended for use by *client* processes. Processes that initialized as *tools*
should instead call ``PMIx_tool_finalize``, and processes hosting a PMIx server
should call ``PMIx_server_finalize``.


.. seealso::
   :ref:`PMIx_Init(3) <man3-PMIx_Init>`,
   :ref:`PMIx_Abort(3) <man3-PMIx_Abort>`,
   :ref:`PMIx_Initialized(3) <man3-PMIx_Initialized>`,
   PMIx_tool_finalize(3),
   PMIx_server_finalize(3),
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`,
   :ref:`pmix_status_t(5) <man5-pmix_status_t>`
