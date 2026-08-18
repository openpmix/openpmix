.. _man3-PMIx_Fence:

PMIx_Fence
==========

.. include_body

``PMIx_Fence``, ``PMIx_Fence_nb`` |mdash| Execute a barrier synchronization across
a set of processes, optionally collecting data posted via ``PMIx_Put``.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_status_t PMIx_Fence(const pmix_proc_t procs[], size_t nprocs,
                            const pmix_info_t info[], size_t ninfo);

   pmix_status_t PMIx_Fence_nb(const pmix_proc_t procs[], size_t nprocs,
                               const pmix_info_t info[], size_t ninfo,
                               pmix_op_cbfunc_t cbfunc, void *cbdata);


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

  from pmix import *

  foo = PMIxClient()
  # ... after a successful foo.init() ...
  # the peers is a list of Python ``pmix_proc_t`` dictionaries
  peers = [{'nspace': "testnspace", 'rank': PMIX_RANK_WILDCARD}]
  # the directives is a list of Python ``pmix_info_t`` dictionaries
  pydirs = [{'key': PMIX_COLLECT_DATA,
             'value': True, 'val_type': PMIX_BOOL}]
  rc = foo.fence(peers, pydirs)

  # the non-blocking form returns as soon as the request has been accepted
  # and reports the result by executing a callback on the PMIx progress
  # thread. The callback is run if and only if the call returned
  # PMIX_SUCCESS, and must not itself make a blocking PMIx call.
  def donecb(status, cbdata):
      print("fence completed:", foo.error_string(status))
  rc = foo.fence_nb(peers, pydirs, donecb, "mycbdata")


INPUT PARAMETERS
----------------

* ``procs``: Pointer to an array of ``pmix_proc_t`` structures naming the
  processes that are to participate in the barrier. A ``NULL`` value indicates
  that the fence is to span all processes in the caller's own namespace. A rank of
  ``PMIX_RANK_WILDCARD`` in any element indicates that all processes in that
  namespace are participating.
* ``nprocs``: Number of elements in the ``procs`` array.
* ``info``: Pointer to an array of :ref:`pmix_info_t(5) <man5-pmix_info_t>`
  structures conveying directives that qualify the operation (see `DIRECTIVES`_).
  A ``NULL`` value is supported when no directives are desired.
* ``ninfo``: Number of elements in the ``info`` array.

The non-blocking form takes two additional parameters:

* ``cbfunc``: Callback function of type :ref:`pmix_op_cbfunc_t <man5-pmix_op_cbfunc_t>` to be invoked when the
  fence completes.
* ``cbdata``: Opaque pointer that is passed, unmodified, to ``cbfunc``.


DESCRIPTION
-----------

Execute a barrier across the specified processes. ``PMIx_Fence`` is the blocking
form: it does not return until all participating processes have entered the fence
(or the operation fails). ``PMIx_Fence_nb`` is the non-blocking form: it returns
immediately, and the provided ``cbfunc`` is invoked with the final status once the
operation completes.

Passing ``NULL`` for ``procs`` indicates that the fence is to span all processes in
the caller's namespace, equivalent to passing a single ``pmix_proc_t`` containing
the caller's namespace and a rank of ``PMIX_RANK_WILDCARD``. The ordering of
entries in ``procs`` has no significance. However, all processes engaged in a given
fence operation must use the same method to identify the participants: callers that
describe the target set using ``PMIX_RANK_WILDCARD`` are not matched with callers
that list the individual processes of a namespace explicitly. A group identifier
appearing in ``procs`` is first expanded into that group's member processes.

The calling process must itself be among the participants; if it is not, the
operation returns ``PMIX_ERR_NOT_A_MEMBER``.

By default, and for scalability reasons, ``PMIx_Fence`` performs only
synchronization and does **not** return the data posted by participants. Setting
the ``PMIX_COLLECT_DATA`` directive causes the barrier to additionally collect all
committed :ref:`PMIx_Put(3) <man3-PMIx_Put>` data from the participants, making it
locally available to each participant at the end of the operation. Collected data
is cached at the server to reduce memory footprint and is retrieved as needed via
:ref:`PMIx_Get(3) <man3-PMIx_Get>`.

**What a collecting fence carries.** Each PMIx server contributes, on behalf
of each of its local participants, the data that process staged with
:ref:`PMIx_Put(3) <man3-PMIx_Put>` and committed with
:ref:`PMIx_Commit(3) <man3-PMIx_Commit>`. That contribution is *cumulative* by
default: everything the process has published so far is sent on every
collecting fence, so a job that fences repeatedly re-sends the same data each
time. Setting the ``pmix_server_fence_delta_modex`` MCA parameter (see
`MCA PARAMETERS`_) instead has each server send only what its processes have
committed since they last took part in a collecting fence.

The delta changes only what is put on the wire, not what a participant can
retrieve afterwards: the servers keep what earlier fences delivered, and a
contribution reverts to the full published set whenever a delta could not
express it |mdash| in particular when the participants of this fence are not
exactly the set the process last contributed to (two sub-communicators fencing
independently, for example), when a local participant has not yet taken part in
a collecting fence, or when data was published on that process's behalf by some
path other than a commit, such as
:ref:`PMIx_server_register_resources(3) <man3-PMIx_server_register_resources>`
or a group collective.

A fence that does not collect data exchanges nothing, and therefore does not
move that boundary: whatever is owed is still carried by the next collecting
fence.

**Deletions travel with a collecting fence.** A key removed with one of the
``PMIX_DEL_*`` scopes of :ref:`PMIx_Put(3) <man3-PMIx_Put>` cannot be retracted
from a remote node by simply omitting it |mdash| the exchange is additive, so a
contribution that stops naming a key removes nothing at the far end. The
removal is therefore stated explicitly in the next collecting fence, and
processes on other nodes stop seeing the key once that fence completes.
Processes on the deleting process's own node are corrected when the deletion is
committed and do not wait for a fence.

``PMIx_Fence`` and ``PMIx_Fence_nb`` are *collective* operations. The PMIx server
library aggregates the participation of its local clients, passing a single request
to the host environment once all local participants have called the API; the host
then executes the collective across all participating nodes.

As with all non-blocking PMIx APIs, callers of ``PMIx_Fence_nb`` **must** keep the
``procs`` and ``info`` arrays valid until ``cbfunc`` is invoked.


DIRECTIVES
----------

The following attributes are relevant to this operation. The first two are
required to be supported by all PMIx libraries; the remainder are optional and
depend on the implementation and host environment.

* ``PMIX_COLLECT_DATA`` (bool) |mdash| collect all data posted by the participants
  via :ref:`PMIx_Put(3) <man3-PMIx_Put>` and committed via ``PMIx_Commit``, making
  the collection locally available to each participant at the end of the
  operation. By default this also includes job-level information locally generated
  by the PMIx servers, unless excluded via ``PMIX_COLLECT_GENERATED_JOB_INFO``.
  The default behavior of the fence is **not** to collect data.
* ``PMIX_COLLECT_GENERATED_JOB_INFO`` (bool) |mdash| collect all job-level
  information (reserved keys) that was locally generated by the PMIx servers.
* ``PMIX_ALL_CLONES_PARTICIPATE`` (bool) |mdash| all clones of the calling process
  must participate in the collective operation.
* ``PMIX_TIMEOUT`` (int) |mdash| maximum time, in seconds, for the fence to
  execute before the host declares an error. This helps avoid "hangs" caused by a
  programming error that prevents one or more processes from reaching the fence.

.. note::
   The ``PMIX_COLLECTIVE_ALGO`` and ``PMIX_COLLECTIVE_ALGO_REQD`` attributes, used
   in earlier releases to request specific collective algorithms, are deprecated
   and should not be used in new code.


MCA PARAMETERS
--------------

The following MCA parameter influences the behavior of a collecting
``PMIx_Fence``. It is read by the PMIx **server** library, so it must be set in
the environment of the servers (e.g.,
``PMIX_MCA_pmix_server_fence_delta_modex=1``) and not in that of the
application processes. The complete, authoritative list of parameters (with
current values) can be displayed with ``pmix_info``.

* ``pmix_server_fence_delta_modex=<true|false>`` (default: ``false``). When
  ``true``, a server contributes only the data its processes have committed
  since they last took part in a collecting fence, rather than everything they
  have published. See the description above for the cases that fall back to the
  full set regardless.

.. caution::
   Every node in the job must be running a PMIx release that understands a
   delta contribution before this is enabled. A server that does not understand
   one rejects the entire collective rather than storing a contribution it
   cannot interpret, so a job whose nodes run mixed releases fails the fence
   with ``PMIX_ERR_BAD_PARAM`` |mdash| loudly, on both sides, rather than
   silently losing keys. That is why the parameter defaults to ``false``.


RETURN VALUE
------------

For the blocking form, ``PMIX_SUCCESS`` indicates that the barrier completed
successfully and any collected data is available for retrieval. For the
non-blocking form, a return of ``PMIX_SUCCESS`` indicates only that the request was
accepted for processing and the final status will be delivered to ``cbfunc``.

* ``PMIX_SUCCESS`` |mdash| the fence completed successfully.
* ``PMIX_OPERATION_SUCCEEDED`` |mdash| (non-blocking form) the request was
  satisfied immediately |mdash| for example, when the collective involved only
  processes on the local node |mdash| and ``cbfunc`` will **not** be called.
* ``PMIX_ERR_NOT_A_MEMBER`` |mdash| the calling process is not among the named
  participants.
* ``PMIX_ERR_BAD_PARAM`` |mdash| an invalid argument was supplied (e.g., a ``NULL``
  ``procs`` array with a non-zero ``nprocs``), or the servers contributing to a
  collecting fence did not agree on the kind of contribution they sent |mdash|
  see `MCA PARAMETERS`_.
* ``PMIX_ERR_UNREACH`` |mdash| the local PMIx server could not be reached.
* ``PMIX_ERR_NOT_AVAILABLE`` |mdash| the operation cannot be serviced because the
  library's progress engine has been stopped.
* ``PMIX_ERR_INIT`` |mdash| the PMIx library has not been initialized.
* ``PMIX_ERR_WOULD_BLOCK`` |mdash| the call would have blocked the PMIx progress
  thread from within that thread. See
  `PROGRESS THREAD RESTRICTION`_.

A ``NULL`` ``cbfunc`` passed to ``PMIx_Fence_nb`` makes that call **blocking**:
it does not return until the fence completes, and its return value is the
result of the fence rather than an indication that the request was accepted.
That is the general PMIx convention for a non-blocking entry point handed no
callback. It can only be honored where the operation's entire result is a
status, as it is here |mdash| an entry point that has no way to hand back what
the caller asked for, such as :ref:`PMIx_Get_nb(3) <man3-PMIx_Get>`, rejects a
``NULL`` ``cbfunc`` with ``PMIX_ERR_BAD_PARAM`` instead.

For a singleton process there are no peers to synchronize with, so the blocking
form returns ``PMIX_SUCCESS`` (and the non-blocking form
``PMIX_OPERATION_SUCCEEDED``) immediately without contacting a server.

Any other negative value indicates an appropriate error condition. PMIx error
constants are defined in ``pmix_common.h``.


.. include:: /man/no-blocking-in-progress-thread.rst


.. seealso::
   :ref:`PMIx_Init(3) <man3-PMIx_Init>`,
   :ref:`PMIx_Put(3) <man3-PMIx_Put>`,
   :ref:`PMIx_Commit(3) <man3-PMIx_Commit>`,
   :ref:`PMIx_Get(3) <man3-PMIx_Get>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`,
   :ref:`pmix_status_t(5) <man5-pmix_status_t>`,
   :ref:`pmix_proc_t(5) <man5-pmix_proc_t>`,
   :doc:`Modex: Exchanging Process Data </how-things-work/modex>`
