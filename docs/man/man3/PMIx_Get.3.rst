.. _man3-PMIx_Get:

PMIx_Get
========

.. include_body

``PMIx_Get``, ``PMIx_Get_nb`` |mdash| Retrieve a value posted by a process against
a specified key.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_status_t PMIx_Get(const pmix_proc_t *proc, const char key[],
                          const pmix_info_t info[], size_t ninfo,
                          pmix_value_t **val);

   pmix_status_t PMIx_Get_nb(const pmix_proc_t *proc, const char key[],
                             const pmix_info_t info[], size_t ninfo,
                             pmix_value_cbfunc_t cbfunc, void *cbdata);


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

  from pmix import *

  foo = PMIxClient()
  # ... after a successful foo.init() ...
  # the proc is a Python ``pmix_proc_t`` dictionary
  proc = {'nspace': "testnspace", 'rank': 0}
  # the directives is a list of Python ``pmix_info_t`` dictionaries
  pydirs = [{'key': PMIX_TIMEOUT,
             'value': {'value': 10, 'val_type': PMIX_INT}}]
  rc, value = foo.get(proc, "mykey", pydirs)


INPUT PARAMETERS
----------------

* ``proc``: Pointer to a ``pmix_proc_t`` identifying the process whose posted data
  is to be retrieved. A ``NULL`` value is used in place of the caller's own
  identifier. A rank of ``PMIX_RANK_WILDCARD`` retrieves job-level information for
  the process's namespace.
* ``key``: A NULL-terminated string, no longer than ``PMIX_MAX_KEYLEN``
  characters, naming the value to retrieve. A ``NULL`` key requests **all** data
  associated with the identified process.
* ``info``: Pointer to an array of :ref:`pmix_info_t(5) <man5-pmix_info_t>`
  structures conveying directives that qualify the operation (see `DIRECTIVES`_).
  A ``NULL`` value is supported when no directives are desired.
* ``ninfo``: Number of elements in the ``info`` array.


OUTPUT PARAMETERS
-----------------

* ``val`` (blocking form): Address of a ``pmix_value_t`` pointer. On success it is
  set as described under `DESCRIPTION`_. When the ``PMIX_GET_STATIC_VALUES``
  directive is used, the caller must instead provide storage and pass a pointer to
  it here as an input.

The non-blocking form replaces ``val`` with a callback:

* ``cbfunc``: Callback function of type ``pmix_value_cbfunc_t`` invoked with the
  retrieved value once it is available.
* ``cbdata``: Opaque pointer that is passed, unmodified, to ``cbfunc``.


DESCRIPTION
-----------

Retrieve information for the specified ``key`` as posted by the process identified
in ``proc``. ``PMIx_Get`` is the blocking form: it does not return until the data
is available (or the operation fails or times out). ``PMIx_Get_nb`` is the
non-blocking form: it returns immediately, and the provided ``cbfunc`` is invoked
with the final status and value once the data has been retrieved by the local
server.

The precedence rules governing where the library looks for the data |mdash| the
local client cache, the local PMIx server, and the host resource manager |mdash|
differ for reserved keys (those provided by the host environment, beginning with
``"pmix"``) and non-reserved keys (those posted by the application via
:ref:`PMIx_Put(3) <man3-PMIx_Put>`). Those rules are governed by the directives
described below.

Return of the value (blocking form) is controlled by the directives:

* In the absence of any directive, the returned ``pmix_value_t`` is a freshly
  allocated memory object. The caller is responsible for releasing it with
  ``PMIX_VALUE_RELEASE`` when done.
* With ``PMIX_GET_POINTER_VALUES``, the function returns a pointer directly into
  the PMIx library's key-value store. The caller **must not** release the returned
  data.
* With ``PMIX_GET_STATIC_VALUES``, the value is returned in caller-provided
  storage: the caller must pass a pointer to a ``pmix_value_t`` it owns in ``val``
  and destruct it with ``PMIX_VALUE_DESTRUCT`` when done. If the implementation
  cannot return a static value, ``PMIx_Get`` returns ``PMIX_ERR_NOT_SUPPORTED``.

As with all non-blocking PMIx APIs, callers of ``PMIx_Get_nb`` **must** keep the
``proc`` and ``info`` arrays valid until ``cbfunc`` is invoked.


DIRECTIVES
----------

The following attributes are relevant to this operation. All but ``PMIX_TIMEOUT``
are required to be supported by all PMIx libraries; ``PMIX_TIMEOUT`` is optional
for host environments.

* ``PMIX_OPTIONAL`` (bool) |mdash| look only in the client's local data store; do
  not request the data from the PMIx server if it is not found locally.
* ``PMIX_IMMEDIATE`` (bool) |mdash| direct the PMIx server to immediately return an
  error if the requested data is not in its store, rather than requesting it from
  the host resource manager.
* ``PMIX_GET_REFRESH_CACHE`` (bool) |mdash| refresh the local cache for the target
  process from the server before returning, picking up values put and committed
  since the last refresh. A ``NULL`` key refreshes all values for the process; a
  rank of ``PMIX_RANK_WILDCARD`` refreshes job-related information.
* ``PMIX_DATA_SCOPE`` (pmix_scope_t) |mdash| restrict the scope of data to be
  searched (see :ref:`PMIx_Put(3) <man3-PMIx_Put>`).
* ``PMIX_GET_POINTER_VALUES`` (bool) |mdash| return a pointer to the value in the
  library's key-value store instead of a copy; the caller must not free it.
* ``PMIX_GET_STATIC_VALUES`` (bool) |mdash| return the value in storage provided by
  the caller via the ``val`` parameter.
* ``PMIX_SESSION_INFO`` / ``PMIX_JOB_INFO`` / ``PMIX_APP_INFO`` /
  ``PMIX_NODE_INFO`` (bool) |mdash| direct that the requested key be retrieved from
  the session-, job-, application-, or node-level information, respectively.
* ``PMIX_TIMEOUT`` (int) |mdash| maximum time, in seconds, for the get to complete
  before returning an error. Helps avoid "hangs" when the target process never
  exposes the requested data.


RETURN VALUE
------------

For the blocking form, ``PMIX_SUCCESS`` indicates that the requested data was
returned in the manner requested. For the non-blocking form, a return of
``PMIX_SUCCESS`` indicates only that the request was accepted for processing; the
final status and value are delivered to ``cbfunc``.

* ``PMIX_SUCCESS`` |mdash| the requested data has been returned.
* ``PMIX_ERR_NOT_FOUND`` |mdash| the requested data was not available.
* ``PMIX_ERR_EXISTS_OUTSIDE_SCOPE`` |mdash| the requested key exists, but was
  posted in a scope that does not include the requester.
* ``PMIX_ERR_BAD_PARAM`` |mdash| an invalid argument was supplied |mdash| for
  example, an over-length key, or the ``PMIX_GET_STATIC_VALUES`` directive with a
  ``NULL`` storage location.
* ``PMIX_ERR_NOT_SUPPORTED`` |mdash| the implementation cannot satisfy a requested
  directive (e.g., ``PMIX_GET_STATIC_VALUES``).
* ``PMIX_ERR_NOT_AVAILABLE`` |mdash| the operation cannot be serviced because the
  library's progress engine has been stopped.
* ``PMIX_ERR_INIT`` |mdash| the PMIx library has not been initialized.

Any other negative value indicates an appropriate error condition. PMIx error
constants are defined in ``pmix_common.h``.


NOTES
-----

For non-reserved keys, retrieval blocks until the target process has posted the
key with :ref:`PMIx_Put(3) <man3-PMIx_Put>`, committed it with
:ref:`PMIx_Commit(3) <man3-PMIx_Commit>`, and the data has been circulated |mdash|
typically via a :ref:`PMIx_Fence(3) <man3-PMIx_Fence>` |mdash| unless the
``PMIX_OPTIONAL`` or ``PMIX_IMMEDIATE`` directives are used to bound the search.


.. seealso::
   :ref:`PMIx_Init(3) <man3-PMIx_Init>`,
   :ref:`PMIx_Put(3) <man3-PMIx_Put>`,
   :ref:`PMIx_Commit(3) <man3-PMIx_Commit>`,
   :ref:`PMIx_Fence(3) <man3-PMIx_Fence>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`,
   :ref:`pmix_value_t(5) <man5-pmix_value_t>`,
   :ref:`pmix_status_t(5) <man5-pmix_status_t>`,
   :ref:`pmix_proc_t(5) <man5-pmix_proc_t>`
