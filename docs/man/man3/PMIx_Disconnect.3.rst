.. _man3-PMIx_Disconnect:

PMIx_Disconnect
===============

.. include_body

``PMIx_Disconnect``, ``PMIx_Disconnect_nb`` |mdash| Disconnect a previously
connected set of processes.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_status_t PMIx_Disconnect(const pmix_proc_t procs[], size_t nprocs,
                                 const pmix_info_t info[], size_t ninfo);

   pmix_status_t PMIx_Disconnect_nb(const pmix_proc_t procs[], size_t nprocs,
                                    const pmix_info_t info[], size_t ninfo,
                                    pmix_op_cbfunc_t cbfunc, void *cbdata);


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

  from pmix import *

  foo = PMIxClient()
  # ... after a successful foo.init() and a matching foo.connect() ...
  # the peers is a list of Python ``pmix_proc_t`` dictionaries naming the same
  # set of participants that were connected
  peers = [{'nspace': "testnspace", 'rank': PMIX_RANK_WILDCARD},
           {'nspace': "othernspace", 'rank': PMIX_RANK_WILDCARD}]
  # the directives is a list of Python ``pmix_info_t`` dictionaries
  pydirs = [{'key': PMIX_TIMEOUT,
             'value': {'value': 10, 'val_type': PMIX_INT}}]
  rc = foo.disconnect(peers, pydirs)


INPUT PARAMETERS
----------------

* ``procs``: Pointer to an array of ``pmix_proc_t`` structures naming **all** of
  the processes that are to be disconnected. This must identify a set that was
  previously connected via :ref:`PMIx_Connect(3) <man3-PMIx_Connect>`. A rank of
  ``PMIX_RANK_WILDCARD`` in any element indicates that all processes in that
  namespace are participating. ``procs`` must be non-``NULL`` and ``nprocs`` must
  be greater than zero; otherwise the call returns ``PMIX_ERR_BAD_PARAM``.
* ``nprocs``: Number of elements in the ``procs`` array.
* ``info``: Pointer to an array of :ref:`pmix_info_t(5) <man5-pmix_info_t>`
  structures conveying directives that qualify the operation (see `DIRECTIVES`_).
  A ``NULL`` value is supported when no directives are desired.
* ``ninfo``: Number of elements in the ``info`` array.

The non-blocking form takes two additional parameters:

* ``cbfunc``: Callback function of type ``pmix_op_cbfunc_t`` to be invoked when the
  disconnect operation completes.
* ``cbdata``: Opaque pointer that is passed, unmodified, to ``cbfunc``.


DESCRIPTION
-----------

Disconnect a set of processes that were previously connected via
:ref:`PMIx_Connect(3) <man3-PMIx_Connect>`. ``PMIx_Disconnect`` is the blocking
form: it does not return until all processes identified in ``procs`` have called
either ``PMIx_Disconnect`` or ``PMIx_Disconnect_nb`` **and** the host environment
has completed any required supporting operations (or the operation fails).
``PMIx_Disconnect_nb`` is the non-blocking form: it returns immediately, and the
provided ``cbfunc`` is invoked with the final status once the operation completes.

Disconnecting dissolves the *connected* relationship: the host environment ceases
to treat the assemblage as a single application for the purposes of fault handling
and event notification. As part of the operation, the local library releases the
cached job-level data it was holding for any namespace, other than the caller's
own, that was involved in the connect. After a successful disconnect the members
are free to reconnect.

Every element of ``procs`` must name a participant, and the calling process must
itself be among them; if it is not, the operation returns
``PMIX_ERR_NOT_A_MEMBER``. The ordering of the entries in ``procs`` has no
significance. However, all processes engaged in a given disconnect operation must
use the same method to identify the participants: callers that describe the target
set using ``PMIX_RANK_WILDCARD`` are not matched with callers that list the
individual processes of a namespace explicitly. A group identifier appearing in
``procs`` is first expanded into that group's member processes.

A process can only engage in one disconnect operation involving the identical
``procs`` array at a time. However, a process can be simultaneously engaged in
multiple disconnect operations, each involving a different ``procs`` array. A
process is not allowed to reconnect to a set of processes that has not fully
completed disconnect |mdash| the disconnect must complete before the same set of
processes can be reconnected.

``PMIx_Disconnect`` and ``PMIx_Disconnect_nb`` are *collective* operations. The
PMIx server library aggregates the participation of its local clients, passing a
single request to the host environment once all local participants have called the
API; the host then executes the collective across all participating nodes.

As with all non-blocking PMIx APIs, callers of ``PMIx_Disconnect_nb`` **must** keep
the ``procs`` and ``info`` arrays valid until ``cbfunc`` is invoked.


DIRECTIVES
----------

PMIx libraries are not required to directly support any attributes for this
operation, but any provided attributes are passed to the host environment for
processing. The following attributes are optional and depend on the
implementation and host environment:

* ``PMIX_ALL_CLONES_PARTICIPATE`` (bool) |mdash| all clones of the calling process
  must participate in the collective operation.
* ``PMIX_TIMEOUT`` (int) |mdash| maximum time, in seconds, for the disconnect to
  complete before the host declares an error. This helps avoid "hangs" caused by a
  programming error that prevents one or more processes from reaching the
  disconnect.


RETURN VALUE
------------

For the blocking form, ``PMIX_SUCCESS`` indicates that all participants have
disconnected. For the non-blocking form, a return of ``PMIX_SUCCESS`` indicates
only that the request was accepted for processing and the final status will be
delivered to ``cbfunc``.

* ``PMIX_SUCCESS`` |mdash| the disconnect operation completed successfully.
* ``PMIX_ERR_INVALID_OPERATION`` |mdash| the specified set of ``procs`` was not
  previously connected via :ref:`PMIx_Connect(3) <man3-PMIx_Connect>` or its
  non-blocking form.
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

Disconnecting is the required counterpart to :ref:`PMIx_Connect(3)
<man3-PMIx_Connect>`. If a connected process terminates or calls
:ref:`PMIx_Finalize(3) <man3-PMIx_Finalize>` without first disconnecting, the host
environment generates a ``PMIX_ERR_PROC_TERM_WO_SYNC`` event; if a job is
terminated as a result, the host also generates ``PMIX_ERR_JOB_TERM_WO_SYNC`` for
every job terminated as a result. Calling ``PMIx_Disconnect`` before finalizing
avoids these events.


.. seealso::
   :ref:`PMIx_Init(3) <man3-PMIx_Init>`,
   :ref:`PMIx_Finalize(3) <man3-PMIx_Finalize>`,
   :ref:`PMIx_Connect(3) <man3-PMIx_Connect>`,
   :ref:`PMIx_Fence(3) <man3-PMIx_Fence>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`,
   :ref:`pmix_status_t(5) <man5-pmix_status_t>`,
   :ref:`pmix_proc_t(5) <man5-pmix_proc_t>`
