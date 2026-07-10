.. _man3-PMIx_Group_construct:

PMIx_Group_construct
====================

.. include_body

``PMIx_Group_construct``, ``PMIx_Group_construct_nb`` |mdash| Construct a new
PMIx group from a specified set of processes.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   pmix_status_t PMIx_Group_construct(const char grp[],
                                      const pmix_proc_t procs[], size_t nprocs,
                                      const pmix_info_t directives[], size_t ndirs,
                                      pmix_info_t **results, size_t *nresults);

   pmix_status_t PMIx_Group_construct_nb(const char grp[],
                                         const pmix_proc_t procs[], size_t nprocs,
                                         const pmix_info_t info[], size_t ninfo,
                                         pmix_info_cbfunc_t cbfunc, void *cbdata);


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

  from pmix import *

  foo = PMIxClient()
  # the peers is a list of Python ``pmix_proc_t`` dictionaries
  peers = [{'nspace': "testnspace", 'rank': 0},
           {'nspace': "testnspace", 'rank': 1}]
  # the directives is a list of Python ``pmix_info_t`` dictionaries
  pydirs = [{'key': PMIX_GROUP_ASSIGN_CONTEXT_ID,
             'value': {'value': True, 'val_type': PMIX_BOOL}}]
  rc, results = foo.group_construct("mygroup", peers, pydirs)


INPUT PARAMETERS
----------------

* ``grp``: A NULL-terminated character string identifying the group. The string
  must be of length less than or equal to ``PMIX_MAX_NSLEN`` and may contain
  only characters accepted by the standard string comparison functions (e.g.,
  ``strncmp``). Each simultaneously-active group construct operation must use a
  unique identifier.
* ``procs``: Pointer to an array of ``pmix_proc_t`` structures naming the
  processes that are to compose the group.
* ``nprocs``: Number of elements in the ``procs`` array.
* ``directives`` (``info`` for the non-blocking form): Pointer to an array of
  :ref:`pmix_info_t(5) <man5-pmix_info_t>` structs conveying user directives that
  qualify the operation (see `DIRECTIVES`_).
* ``ndirs`` (``ninfo`` for the non-blocking form): Number of elements in the
  ``directives`` array.

The blocking form returns results directly:

* ``results``: Address of a pointer that, upon successful return, is set to an
  array of :ref:`pmix_info_t(5) <man5-pmix_info_t>` structs containing any values
  returned by the operation (e.g., an assigned context ID). May be ``NULL`` if
  the caller does not want the returned values. The caller is responsible for
  releasing the array with ``PMIX_INFO_FREE``.
* ``nresults``: Address of a ``size_t`` that is set to the number of elements in
  the returned ``results`` array.

The non-blocking form takes two additional parameters in place of ``results``
and ``nresults``:

* ``cbfunc``: Callback function of type :ref:`pmix_info_cbfunc_t <man5-pmix_info_cbfunc_t>` to be executed once
  the group has been constructed.
* ``cbdata``: Opaque pointer that is passed, unmodified, to ``cbfunc``.


DESCRIPTION
-----------

Construct a new group composed of the specified processes and identified with
the provided group identifier. A *group* is a collection of processes desiring a
unified identifier for a purpose such as a group-wide event notification, or for
efficient collective operations across a subset of the processes in one or more
jobs. Upon completion of the construct procedure, each group member has access
to the job-level information of all namespaces represented in the group and the
contact information for every group member.

``PMIx_Group_construct`` is the blocking form: it does not return until all
specified processes have joined the group (or the operation fails). Returned
values are delivered in the ``results`` array. ``PMIx_Group_construct_nb`` is the
non-blocking form: it returns immediately, and the provided ``cbfunc`` is invoked
with the final status and any returned values once the operation completes.

As with all non-blocking PMIx APIs, callers of ``PMIx_Group_construct_nb`` **must**
keep the ``grp``, ``procs``, and ``info`` arrays valid until ``cbfunc`` is invoked.

Processes may engage in multiple simultaneous group construct operations, so long
as each is provided with a unique group ID. Processes in a group under
construction are not allowed to leave the group until construction is complete.

Construction methods
^^^^^^^^^^^^^^^^^^^^^

For purposes of construction, PMIx distinguishes two classes of group member.
*Leaders* have some global view of the group at construction time |mdash| for
example, the number of leaders or the process IDs of all leaders. *Members* know
only the group ID they are to join, with no other knowledge of the group's size
or membership. ``PMIx_Group_construct`` supports two collective construction
methods, selected by how the caller populates the ``procs`` array and the
directives; the fully asynchronous *invite* method is handled by the separate
:ref:`PMIx_Group_invite(3) <man3-PMIx_Group_invite>` API.

* **Collective method.** Every leader knows the process IDs of all other leaders
  and calls ``PMIx_Group_construct`` with the full array of leader process IDs.
  All leaders must call the API, but the ``procs`` array need not be ordered
  identically across leaders. This is the traditional form of the operation.

* **Bootstrap method.** Each leader knows only *how many* leaders will participate,
  not their identities |mdash| the common case when two or more collections of
  processes join together (e.g., an MPI connect/accept) and only the "root"
  process of each collection knows of the others. Each leader passes only its own
  process ID in ``procs`` and **must** provide the ``PMIX_GROUP_BOOTSTRAP``
  attribute with a value equal to the number of leaders in the operation.
  Construction completes once the specified number of leaders (plus any added
  members) have called the API.

In either method, a leader may name additional processes that are to end up in the
final group |mdash| but that are not among the leaders |mdash| by supplying the
``PMIX_GROUP_ADD_MEMBERS`` attribute with an array of their process IDs. The PMIx
server library and host jointly aggregate the added members contributed across
leaders. Every process on an "additional member" list **must** itself call
``PMIx_Group_construct`` with ``NULL`` in the ``procs`` argument; this marks the
caller as an added member (rather than a leader) and lets the library register the
necessary events on its behalf. The construct operation cannot complete until all
leaders **and** all added members have called ``PMIx_Group_construct``, so that any
group or endpoint information provided by the added members is included in the
returned ``results`` array.

When a call names its participants explicitly (i.e., it uses neither
``PMIX_GROUP_ADD_MEMBERS`` nor ``PMIX_GROUP_BOOTSTRAP``), the client library first
expands any group identifier appearing in the ``procs`` array into that group's
member processes and then verifies that the caller is itself among the resolved
participants, returning ``PMIX_ERR_NOT_A_MEMBER`` immediately if it is not. This
check is intentionally skipped for the add-members and bootstrap methods, since
those deliberately omit participants from the ``procs`` array.

These methods and the responsibilities of the client library, server, and host are
described in full in
:ref:`Group Construction, Destruction, and Fault Tolerance <group-construction-label>`.

Membership changes and failures
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

If ``PMIX_GROUP_LEADER`` is provided, the declared leader (and only the leader)
will receive a ``PMIX_GROUP_MEMBER_FAILED`` event whenever a process fails or
terminates prior to calling ``PMIx_Group_construct(_nb)``. In the absence of a
declared leader, all participants that registered for the event receive it. The
event contains the identifier of the failed process plus any other information
the resource manager provided, giving the leader an opportunity to react |mdash|
for example, to invite an alternative member or to proceed with a smaller group.
The decision to proceed with a smaller group is communicated back to the PMIx
library in the results array returned at the end of the event handler, allowing
PMIx to adjust its accounting for completion of the procedure.

When construct is complete, the participating PMIx servers are alerted to any
change in participants, and each group member that registered for the
``PMIX_GROUP_MEMBERSHIP_UPDATE`` event receives it with the final membership.

Failure of the leader at any time causes a ``PMIX_GROUP_LEADER_FAILED`` event to
be delivered to all participants so they can optionally declare a new leader. A
new leader is identified by providing the ``PMIX_GROUP_LEADER`` attribute in the
results array returned from the event handler; only one process is permitted to
do so. The outcome of leader selection is communicated to all participants via a
``PMIX_GROUP_LEADER_SELECTED`` event. If no leader was selected, the status code
in that event carries an error value so participants can take appropriate action.

Any participant that returns ``PMIX_GROUP_CONSTRUCT_ABORT`` from the leader-failed
event handler aborts the construct procedure. Processes engaged in the blocking
form return with the ``PMIX_GROUP_CONSTRUCT_ABORT`` status; non-blocking
participants have their callback function invoked with that status.


DIRECTIVES
----------

The following attributes are relevant to this operation:

* ``PMIX_GROUP_ID`` (char*) |mdash| the user-provided group identifier. This is
  normally passed as the ``grp`` argument; the attribute form is accepted as an
  equivalent means of conveying the identifier.
* ``PMIX_GROUP_LEADER`` (bool) |mdash| declare this process to be the leader of
  the construction procedure. If a process provides this attribute, failure
  notification for any participating process goes only to that process. In the
  absence of a declared leader, failure events go to all participants.
* ``PMIX_GROUP_OPTIONAL`` (bool) |mdash| participation is optional; do not return
  an error if any of the specified processes terminate without having joined
  (default: ``false``).
* ``PMIX_GROUP_NOTIFY_TERMINATION`` (bool) |mdash| establishes the group's failure
  policy for the eventual teardown: when a member is lost before it calls destruct,
  report the loss to the survivors via a ``PMIX_GROUP_MEMBER_FAILED`` event and
  complete the destruct with success, rather than reporting an error (default:
  ``false``). This directive is supplied here at construct time but governs
  :ref:`PMIx_Group_destruct(3) <man3-PMIx_Group_destruct>`; the library remembers it
  and re-applies it to the destruct automatically.
* ``PMIX_GROUP_ASSIGN_CONTEXT_ID`` (bool) |mdash| request that the resource
  manager assign a unique numerical (``size_t``) context ID to the group. The
  value is returned in the ``results`` array (or in the callback) and is also
  delivered in the ``PMIX_GROUP_CONTEXT_ID_ASSIGNED`` event.
* ``PMIX_GROUP_BOOTSTRAP`` (size_t) |mdash| select the *bootstrap* construction
  method (see `DESCRIPTION`_). The value is the number of leaders that will start
  the construction. Each leader passes only its own process ID in ``procs``.
  Required of every leader participating in a bootstrap operation.
* ``PMIX_GROUP_ADD_MEMBERS`` (pmix_data_array_t\*) |mdash| an array of
  ``pmix_proc_t`` naming processes that are not included in the ``procs`` array but
  are to be part of the final group. May be supplied by any leader in either the
  collective or bootstrap method. Each named process must itself call
  ``PMIx_Group_construct`` with ``NULL`` for ``procs``.
* ``PMIX_GROUP_FT_COLLECTIVE`` (bool) |mdash| if true, complete the construct on
  the surviving members when a required member is lost, delivering a
  ``PMIX_GROUP_MEMBER_FAILED`` event for each loss, instead of aborting the
  operation (default: ``false``).
* ``PMIX_GROUP_INFO`` (pmix_data_array_t\*) |mdash| an array of
  :ref:`pmix_info_t(5) <man5-pmix_info_t>` containing data that is to be shared
  across all members of the group during construction. When the array is passed
  to the host, its first element must be the process ID (marked by
  ``PMIX_PROCID``) of the process that provided it.
* ``PMIX_GROUP_LOCAL_CID`` (size_t) |mdash| the local context ID for the
  providing process as a member of this group. It is supplied as an entry within
  the process's ``PMIX_GROUP_INFO`` array so that it is shared with the other
  group members during construction.
* ``PMIX_GROUP_LOCAL_ONLY`` (bool) |mdash| the group operation involves only
  processes that are local to this node, allowing the library to complete it
  without engaging the host environment.
* ``PMIX_GROUP_FINAL_MEMBERSHIP_ORDER`` (pmix_data_array_t\*) |mdash| an array of
  ``pmix_proc_t`` specifying the desired order of the processes in the final
  group membership. The order may be given by individual process or by namespace
  (with a wildcard rank). If more than one participant supplies this attribute,
  the provided orderings must be identical.
* ``PMIX_TIMEOUT`` (int) |mdash| return an error if the group does not assemble
  within the specified number of seconds. This targets the scenario where a
  process fails to participate due to hanging.

The following values may be returned in the ``results`` array (or the
non-blocking callback):

* ``PMIX_GROUP_CONTEXT_ID`` (size_t) |mdash| the numerical context ID assigned to
  the group by the resource manager, returned when ``PMIX_GROUP_ASSIGN_CONTEXT_ID``
  was requested.
* ``PMIX_GROUP_MEMBERSHIP`` (pmix_data_array_t\*) |mdash| an array of
  ``pmix_proc_t`` giving the final membership of the constructed group. This is
  also carried by the ``PMIX_GROUP_MEMBERSHIP_UPDATE`` event.


RETURN VALUE
------------

For the blocking form, ``PMIX_SUCCESS`` indicates that the group was successfully
constructed and any returned values are available in ``results``. For the
non-blocking form, a return of ``PMIX_SUCCESS`` indicates only that the request
was successfully accepted for processing; the final status is delivered to
``cbfunc``, and the callback is not invoked if any other value is returned.

* ``PMIX_SUCCESS`` |mdash| the group was successfully constructed.
* ``PMIX_GROUP_CONSTRUCT_ABORT`` |mdash| a participant aborted the construct
  procedure in response to a member or leader failure.
* ``PMIX_ERR_NOT_A_MEMBER`` |mdash| the caller named its participants explicitly
  but is not itself among them. Returned only for the collective method, since the
  add-members and bootstrap methods skip this check.
* ``PMIX_ERR_BAD_PARAM`` |mdash| a required argument was invalid (e.g., a ``NULL``
  or over-length group identifier).
* ``PMIX_ERR_NOT_SUPPORTED`` |mdash| the host environment does not support group
  operations.
* ``PMIX_ERR_INIT`` |mdash| the PMIx library has not been initialized.

Any other value indicates an appropriate error condition.


.. seealso::
   :ref:`PMIx_Group_invite(3) <man3-PMIx_Group_invite>`,
   :ref:`PMIx_Group_join(3) <man3-PMIx_Group_join>`,
   :ref:`PMIx_Group_leave(3) <man3-PMIx_Group_leave>`,
   :ref:`PMIx_Group_destruct(3) <man3-PMIx_Group_destruct>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`,
   :ref:`pmix_status_t(5) <man5-pmix_status_t>`,
   :ref:`pmix_proc_t(5) <man5-pmix_proc_t>`
