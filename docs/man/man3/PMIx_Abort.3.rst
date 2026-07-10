.. _man3-PMIx_Abort:

PMIx_Abort
==========

.. include_body

``PMIx_Abort`` |mdash| Abort the specified processes.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_status_t PMIx_Abort(int status, const char msg[],
                            pmix_proc_t procs[], size_t nprocs);


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

  from pmix import *

  foo = PMIxClient()
  # ... after a successful foo.init() ...
  # the peers is a list of Python ``pmix_proc_t`` dictionaries
  peers = [{'nspace': "testnspace", 'rank': 0},
           {'nspace': "testnspace", 'rank': 1}]
  rc = foo.abort(-1, "abort message", peers)


INPUT PARAMETERS
----------------

* ``status``: Error code to be returned to the invoking environment. A value of
  zero is permitted by PMIx, but may be ignored by some resource managers unless
  they are specifically configured to honor it.
* ``msg``: A NULL-terminated string message to be displayed or logged in
  association with the operation. Passing ``NULL`` is allowed.
* ``procs``: Pointer to an array of ``pmix_proc_t`` structures naming the
  processes that are to be aborted. A ``NULL`` value indicates that all processes
  in the caller's namespace |mdash| including the caller itself |mdash| are to be
  aborted; this is equivalent to passing a single ``pmix_proc_t`` containing the
  caller's namespace and a rank of ``PMIX_RANK_WILDCARD``. A wildcard rank in any
  element indicates that all processes in that namespace are to be aborted.
* ``nprocs``: Number of elements in the ``procs`` array.


DESCRIPTION
-----------

Request that the host resource manager abort the provided array of processes,
returning the provided ``status`` and printing the provided ``msg``. If the
design of the host resource manager allows, the message should be associated with
any record it prints or logs of the operation. If the processes were launched by
an application that exists for the lifetime of the processes, that application
should terminate with the provided ``status`` if the system allows.

A ``NULL`` for the ``procs`` array indicates that all processes in the caller's
namespace are to be aborted, including the caller itself. While a caller is
permitted to request the abort of processes from namespaces other than its own,
not all environments will support such requests.

``PMIx_Abort`` is a blocking call: it does not return until the host environment
has carried out the operation on the specified processes. A successful return
therefore indicates that the requested processes are in a terminated state. If
the caller's own process is included in the array of targets, the function does
**not** return in the success case |mdash| it returns only if the host is unable
to execute the operation.

.. note::
   The response to this request is somewhat dependent on the specific resource
   manager and its configuration. For example, some resource managers will not
   abort the application if the provided ``status`` is zero unless specifically
   configured to do so; some cannot abort subsets of processes in an application;
   and some may not permit termination of processes outside the caller's own
   namespace. This behavior lies outside the control of PMIx itself. Regardless
   of the value of ``status``, the PMIx client library will inform the resource
   manager of the request that the specified processes be aborted.

   Race conditions caused by multiple processes calling ``PMIx_Abort`` are left to
   the server implementation to resolve with regard to which status is returned
   and what messages (if any) are printed.


RETURN VALUE
------------

Returns ``PMIX_SUCCESS`` when the requested processes are in a terminated state.
On error, a negative value corresponding to a PMIx error constant is returned,
including:

* ``PMIX_ERR_PARAM_VALUE_NOT_SUPPORTED`` |mdash| the PMIx implementation and host
  environment support this API, but the request names processes the host cannot
  abort (e.g., a subset of a namespace, or processes outside the caller's own
  namespace, where the host does not permit such operations). In this case none
  of the specified processes are terminated.
* ``PMIX_ERR_NOT_SUPPORTED`` |mdash| the host environment does not provide an
  abort operation.
* ``PMIX_ERR_UNREACH`` |mdash| the local PMIx server could not be reached.
* ``PMIX_ERR_NOT_AVAILABLE`` |mdash| the operation cannot be serviced because the
  library's progress engine has been stopped.
* ``PMIX_ERR_INIT`` |mdash| the PMIx library has not been initialized.

Any other negative value indicates an appropriate error condition. PMIx error
constants are defined in ``pmix_common.h``.


.. seealso::
   :ref:`PMIx_Init(3) <man3-PMIx_Init>`,
   :ref:`PMIx_Finalize(3) <man3-PMIx_Finalize>`,
   :ref:`PMIx_Initialized(3) <man3-PMIx_Initialized>`,
   pmix_proc_t(5),
   :ref:`pmix_status_t(5) <man5-pmix_status_t>`,
   :ref:`pmix_proc_t(5) <man5-pmix_proc_t>`
