.. _man3-PMIx_Connect:

PMIx_Connect
============

.. include_body

``PMIx_Connect``, ``PMIx_Connect_nb`` |mdash| Record a set of processes as
"connected" so that the host environment treats them as a single assemblage for
fault and event-notification purposes.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_status_t PMIx_Connect(const pmix_proc_t procs[], size_t nprocs,
                              const pmix_info_t info[], size_t ninfo);

   pmix_status_t PMIx_Connect_nb(const pmix_proc_t procs[], size_t nprocs,
                                 const pmix_info_t info[], size_t ninfo,
                                 pmix_op_cbfunc_t cbfunc, void *cbdata);


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

  from pmix import *

  foo = PMIxClient()
  # ... after a successful foo.init() ...
  # the peers is a list of Python ``pmix_proc_t`` dictionaries naming all
  # participants (including the caller)
  peers = [{'nspace': "testnspace", 'rank': PMIX_RANK_WILDCARD},
           {'nspace': "othernspace", 'rank': PMIX_RANK_WILDCARD}]
  # the directives is a list of Python ``pmix_info_t`` dictionaries
  pydirs = [{'key': PMIX_TIMEOUT,
             'value': {'value': 10, 'val_type': PMIX_INT}}]
  rc = foo.connect(peers, pydirs)


INPUT PARAMETERS
----------------

* ``procs``: Pointer to an array of ``pmix_proc_t`` structures naming **all** of
  the processes that are to be connected. A rank of ``PMIX_RANK_WILDCARD`` in any
  element indicates that all processes in that namespace are participating. Unlike
  the collective APIs that default to the caller's own namespace, ``procs`` must
  be non-``NULL`` and ``nprocs`` must be greater than zero; otherwise the call
  returns ``PMIX_ERR_BAD_PARAM``.
* ``nprocs``: Number of elements in the ``procs`` array.
* ``info``: Pointer to an array of :ref:`pmix_info_t(5) <man5-pmix_info_t>`
  structures conveying directives that qualify the operation (see `DIRECTIVES`_).
  A ``NULL`` value is supported when no directives are desired.
* ``ninfo``: Number of elements in the ``info`` array.

The non-blocking form takes two additional parameters:

* ``cbfunc``: Callback function of type ``pmix_op_cbfunc_t`` to be invoked when the
  connect operation completes.
* ``cbdata``: Opaque pointer that is passed, unmodified, to ``cbfunc``.


DESCRIPTION
-----------

Record the processes specified by the ``procs`` array as *connected*.
``PMIx_Connect`` is the blocking form: it does not return until all processes
identified in ``procs`` have called either ``PMIx_Connect`` or ``PMIx_Connect_nb``
**and** the host environment has completed any supporting operations required to
meet the terms of the PMIx definition of *connected* processes (or the operation
fails). ``PMIx_Connect_nb`` is the non-blocking form: it returns immediately, and
the provided ``cbfunc`` is invoked with the final status once the operation
completes.

The PMIx definition of *connected* solely implies that the host environment should
treat the failure of any process in the assemblage as a reportable event, taking
action on the assemblage as if it were a single application. For example, if the
environment defaults (in the absence of any application directives) to terminating
an application upon failure of any process in that application, then it should
terminate all processes in the connected assemblage upon failure of any member.
The host environment may choose to assign a new namespace to the connected
assemblage and/or assign new ranks to its members for its own internal tracking
purposes; whether such namespaces are exposed to clients is implementation
defined.

Every element of ``procs`` must name a participant, and the calling process must
itself be among them; if it is not, the operation returns
``PMIX_ERR_NOT_A_MEMBER``. The ordering of the entries in ``procs`` has no
significance. However, all processes engaged in a given connect operation must use
the same method to identify the participants: callers that describe the target set
using ``PMIX_RANK_WILDCARD`` are not matched with callers that list the individual
processes of a namespace explicitly. A group identifier appearing in ``procs`` is
first expanded into that group's member processes.

Upon completion, the server returns any job-level information that participating
processes do not already possess. When the connect request spans processes from
different namespaces, each participant receives the job-level information for the
namespaces other than its own, so that subsequent :ref:`PMIx_Get(3)
<man3-PMIx_Get>` queries against the connected peers can be satisfied locally.

A process can only engage in one connect operation involving the identical
``procs`` array at a time. However, a process can be simultaneously engaged in
multiple connect operations, each involving a different ``procs`` array.

``PMIx_Connect`` and ``PMIx_Connect_nb`` are *collective* operations. The PMIx
server library aggregates the participation of its local clients, passing a single
request to the host environment once all local participants have called the API;
the host then executes the collective across all participating nodes.

As with all non-blocking PMIx APIs, callers of ``PMIx_Connect_nb`` **must** keep
the ``procs`` and ``info`` arrays valid until ``cbfunc`` is invoked.


DIRECTIVES
----------

PMIx libraries are not required to directly support any attributes for this
operation, but any provided attributes are passed to the host environment for
processing. The following attributes are optional and depend on the
implementation and host environment:

* ``PMIX_ALL_CLONES_PARTICIPATE`` (bool) |mdash| all clones of the calling process
  must participate in the collective operation.
* ``PMIX_TIMEOUT`` (int) |mdash| maximum time, in seconds, for the connect to
  complete before the host declares an error. This helps avoid "hangs" caused by a
  programming error that prevents one or more processes from reaching the connect.


RETURN VALUE
------------

For the blocking form, ``PMIX_SUCCESS`` indicates that all participants have
connected and any returned job-level data is available. For the non-blocking form,
a return of ``PMIX_SUCCESS`` indicates only that the request was accepted for
processing and the final status will be delivered to ``cbfunc``.

* ``PMIX_SUCCESS`` |mdash| the connect operation completed successfully.
* ``PMIX_ERR_NOT_A_MEMBER`` |mdash| the calling process is not among the named
  participants.
* ``PMIX_ERR_BAD_PARAM`` |mdash| an invalid argument was supplied (e.g., a ``NULL``
  ``procs`` array or a zero ``nprocs``).
* ``PMIX_ERR_UNREACH`` |mdash| the local PMIx server could not be reached.
* ``PMIX_ERR_NOT_AVAILABLE`` |mdash| the operation cannot be serviced because the
  library's progress engine has been stopped.
* ``PMIX_ERR_INIT`` |mdash| the PMIx library has not been initialized.

Any other negative value indicates an appropriate error condition. PMIx error
constants are defined in ``pmix_common.h``.


NOTES
-----

Attempting to *connect* processes solely within the same namespace is essentially
a no-op operation. While not explicitly prohibited, a PMIx implementation or host
environment may return an error in such cases.

Once processes are connected, the host environment is required to generate a
``PMIX_ERR_PROC_TERM_WO_SYNC`` event should any process in the assemblage
terminate or call :ref:`PMIx_Finalize(3) <man3-PMIx_Finalize>` without first
disconnecting from the assemblage via :ref:`PMIx_Disconnect(3)
<man3-PMIx_Disconnect>`. If a job is terminated as a result of that action, the
host environment is also required to generate ``PMIX_ERR_JOB_TERM_WO_SYNC`` for
all jobs that were terminated as a result.

By default, the processes created by :ref:`PMIx_Spawn(3) <man3-PMIx_Spawn>` are
connected to the parent process upon successful launch, following the same
semantics described here.


.. seealso::
   :ref:`PMIx_Init(3) <man3-PMIx_Init>`,
   :ref:`PMIx_Finalize(3) <man3-PMIx_Finalize>`,
   :ref:`PMIx_Disconnect(3) <man3-PMIx_Disconnect>`,
   :ref:`PMIx_Fence(3) <man3-PMIx_Fence>`,
   :ref:`PMIx_Get(3) <man3-PMIx_Get>`,
   :ref:`PMIx_Spawn(3) <man3-PMIx_Spawn>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`,
   :ref:`pmix_status_t(5) <man5-pmix_status_t>`
