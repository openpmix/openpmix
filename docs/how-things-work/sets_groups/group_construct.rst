.. _group-construction-label:

Group Construction, Destruction, and Fault Tolerance
====================================================

This section describes, from the application's point of view, how a PMIx
group is built, torn down, and how the library behaves when a participant
is lost while a group operation is still in flight. The internal mechanics
that realize these behaviors are documented separately in
:ref:`Group Operations: Theory and Implementation <group-implementation-label>`.

For purposes of constructing PMIx groups, PMIx defines two
classes of group members:

*leaders* have some global view of the group at time of
construction. This might consist of knowing the number of
leaders in the group, or knowing the process IDs of all
group leaders. All leaders must, of course, know the group
ID they are attempting to construct.

*members* know only that they are to participate in a given
group ID, but have no other knowledge of the group. For example,
a member may not know how many processes will be in the group
or any of their process IDs. The only requirement for membership
is that the process know the group ID to which they are to belong.

Within that context,
PMIx supports three methods for constructing PMIx groups. The
*collective* method is considered the more traditional form
of the operation but requires all group leaders to know the process ID
of all other leaders prior to calling the API.

In contrast,
the *bootstrap* method is a somewhat more dynamic form of the operation
that assumes each leader only knows the number of group leaders,
but does not know their process IDs. This is commonly the case when
two or more collections of processes wish to join together (e.g., in an
MPI connect/accept operation), but only the "root" processes
know of each other. In such cases, each root process typically knows the
process ID of all processes in its collection, but only the root
process ID in the other collection(s).

In either of these two methods, additional group members can be
specified by any leader via the ``PMIX_GROUP_ADD_MEMBERS``
attribute. The PMIx server library and host are jointly responsible
for aggregating the
additional group members specified across leaders. Processes that are on the
"additional member" list must call ``PMIx_Group_construct``
with ``NULL`` in the  ``procs`` argument - this
indicates that the process is to
be added to the group when the group construct operation has completed.

Note that the group construct operation *cannot* complete until all
"leaders" and all add members" have
called ``PMIx_Group_construct``. This is required so that any group
and/or endpoint information
provided by the added members can be included in the returned
``pmix_info_t`` array.

.. note::

    **Participant array expansion and caller verification.** When a
    ``PMIx_Group_construct`` call names its participants explicitly (i.e.,
    it is *not* using the ``PMIX_GROUP_ADD_MEMBERS`` or
    ``PMIX_GROUP_BOOTSTRAP`` methods), the client library first expands any
    PMIx *group* identifier appearing in the ``procs`` array into that
    group's actual member processes, and then verifies that the calling
    process is itself among the resolved participants. A caller that is not
    a member of the operation it is invoking receives
    ``PMIX_ERR_NOT_A_MEMBER`` immediately, rather than issuing a construct
    that can never complete for it. This check is intentionally skipped for
    the add-members and bootstrap methods, since those deliberately omit
    participants from the ``procs`` array (a non-bootstrap "add member" may
    even pass ``NULL``), so local membership cannot be validated.


Finally, the *invite* method represents the most dynamic form
of group construction as it is executed in an ad hoc manner that
revolves around a single leader that asynchronously invites
processes to join a group.

Each of these methods is explained further below. The PMIx library currently
conducts Continuous Integration (CI) tests for all three methods on each
pull request (PR) submitted to the library.


Collective Method
-----------------

All leaders know the ID of all other leaders, and thus call
``PMIx_Group_construct`` with the array of all leader process IDs.
Note that in this method, *all* leaders *must* call the API
with the array of process IDs. However, the array does not need
to be ordered - i.e., the order of IDs in the array does not need to
be the same across leaders.

An example of the collective method can be seen in the
:ref:`group.c <group-example-label>` example taken from the PMIx library.

Library responsibilities
^^^^^^^^^^^^^^^^^^^^^^^^

The PMIx client library will collect all information posted by the client
on a ``PMIX_REMOTE`` scope and include it in the participation message
sent to its local server. Only "remote" data is included as all
local procs already have access to any "local" data that has been posted. The
message also includes any `pmix_info_t` values that were provided by
the caller, including any ``PMIX_GROUP_INFO`` contributions.

The local server will aggregate participation from clients operating
as leaders before
passing the request up to the host. If a timeout value was provided
by one or more clients, then the server must monitor the request for
timeout until it is passed up to the host. This is to protect against
the case where multiple local clients are participating, and one of
those clients fails to call the construct API (thus preventing the
server from passing the request to the host) within the specified
timeout. The server no longer has responsibility for detecting a
timeout condition once it determines the operation is locally complete
and has passed it to the host.

The local server will also immediately pass to the host a participation
request from a client operating as a "member" - i.e., a process that
is being included in the construct operation via an "add member"
specification. These participants are not counted against local completion
of the operation.

Upon notification of operation completion, the server will locally
store all information collected from the participants and then notify
each participating client (leaders as well as members). Notification
will include an array of the resulting membership IDs, plus any
assigned context ID and contributed group info.

Upon concluding the operation, each client shall have access to all
job-level information for namespaces that had a process participating
in the group, plus all information provided by the individual participants
(e.g., posted "remote" data).


Host responsibilities
^^^^^^^^^^^^^^^^^^^^^

The host is responsible for performing a collective allgather operation
across participants, returning all provided information to the involved
PMIx servers. This includes a complete membership list, including the
process ID of all leaders and added members. The host must ensure
that all "members" have participated prior to declaring the operation
to be complete.

Note that the group construct operation requires that each
participant have access to the job-level data of each namespace with
a participating process in the group. Some hosts (e.g., PRRTE) automatically
register each namespace with all PMIx servers, thus ensuring the job-level
information is always available. Other hosts may need to assemble and
register the participating namespaces separately.


Bootstrap Method
----------------
Bootstrap is used when the processes leading a group construct operation do
not know the identity of all other processes that will be participating, but at least
know how may leaders will be involved.
Leaders provide only their
own process ID in the ``procs`` parameter to the ``PMIx_Group_construct``
API, and are *required* to include the
``PMIX_GROUP_BOOTSTRAP`` attribute in their array of ``pmix_info_t``
directives, with the value in that attribute set to equal the number
of leaders in the group construct operation. They may also provide the
``PMIX_GROUP_ADD_MEMBERS`` attribute with an array of process IDs that are to
belong to the final group - each of those processes will also call the group
construct, but with a ``NULL`` process ID to indicate they are joining
as "add members" and not leaders. Construction will complete once all
leaders and "add members" have participated.

An example of the bootstrap method can be seen in the
:ref:`group_bootstrap.c <group-bootstrap-example-label>` example taken from the PMIx library.


Library responsibilities
^^^^^^^^^^^^^^^^^^^^^^^^

The PMIx client library will collect all information posted by the client
on a ``PMIX_REMOTE`` scope and include it in the participation message
sent to its local server. Only "remote" data is included as all
local procs have access to any "local" data that has been posted. The
message also includes any `pmix_info_t` values that were provided by
the caller, including any ``PMIX_GROUP_INFO`` contributions.

Upon receiving the participation from a local client (whether "leader" or
"member"), the PMIx server will immediately pass the request up to the
host. The server bears no responsibility for aggregating the local
participants or for monitoring timeout conditions.

Upon notification of operation completion, the server will locally
store all information collected from the participants and then notify
each participating client (leaders as well as members). Notification
will include an array of the resulting membership IDs, plus any
assigned context ID and contributed group info.

Upon concluding the operation, each client shall have access to all
job-level information for namespaces that had a process participating
in the group, plus all information provided by the individual participants
(e.g., posted "remote" data).


Host responsibilities
^^^^^^^^^^^^^^^^^^^^^

The host is responsible for performing a collective allgather operation
across participants, returning all provided information to the involved
PMIx servers. This includes a complete membership list, including the
process ID of all leaders and added members. The host in this scenario
must look for and count participation by the specified number of leaders,
plus each individual member, before declaring the operation to be complete.

Note that the group construct operation requires that each
participant have access to the job-level data of each namespace with
a participating process in the group. Some hosts (e.g., PRRTE) automatically
register each namespace with all PMIx servers, thus ensuring the job-level
information is always available. Other hosts may need to assemble and
register the participating namespaces separately.


Invite Method
-------------

In contrast to the prior methods, the "invite" method does not involve
any collective operation. Instead, it relies solely on the event notification
subsystem (e.g., ``PMIx_Notify``) for its underlying execution.

Applications wishing to utilize this method must first register two
event handlers to receive (see the :ref:`asyncgroup.c <group-async-example-label>`
example from the PMIx library for details):

* the ``PMIX_GROUP_INVITED`` event. This will be triggered in
  a process when someone calls ``PMIx_Group_invite`` and includes that
  process in the array of desired members. The handler must retrieve the
  specified ``PMIX_GROUP_ID`` from the provided array of attributes, and
  then call ``PMIx_Group_join_nb`` to indicate the desired response (e.g., a
  value of ``PMIX_GROUP_ACCEPT`` to join the group).

  .. note:: The handler *cannot* call the blocking form of the "group join"
            API as it is invoked from inside the PMIx library's progress
            thread. Doing so will cause a thread deadlock condition.

* the ``PMIX_GROUP_COMPLETE`` event, which will be triggered once the
  construct operation has completed. This can be used to receive the final
  group membership, along with any provided group info or other data.

The construct procedure is initiated by a single "leader" that calls the
``PMIx_Group_Invite`` API, providing (among other optional things) an array
of process IDs that it wishes to have join the group. Prior to doing so,
the leader may choose to register an event handler for the ``PMIX_GROUP_INVITE_FAILED``
event. This will allow the library to notify the process should any of
the specified prospective members reject the invitation, or terminate
prior to responding to it. Upon receiving a "failed" event, the leader
can optionally replace the rejecting process with another, can terminate
the group construct operation, or can ignore the failure (thereby accepting
a reduced final group membership).

The leader will return from the ``PMIx_Group_invite`` function once all
specified members have responded to the invitation. In addition, the leader
will (since it is a member of the group) receive the ``PMIX_GROUP_COMPLETE``
event specifying the status return of the operation (``PMIX_SUCCESS`` to
indicate that the group successfully constructed, or else an appropriate
error value) and, if successful, containing the resulting information.


Library responsibilities
^^^^^^^^^^^^^^^^^^^^^^^^

The PMIx client library is solely responsible for executing the underlying
handshakes to support this method of group construction. The library will
collect all information posted by each participant
on a ``PMIX_REMOTE`` scope and include it in the invitation event (if
the process is the leader) or in the "join" event (if a participating
member). Only "remote" data is included as all
local procs have access to any "local" data that has been posted. The
event also includes any `pmix_info_t` values that were provided by
the caller, including any ``PMIX_GROUP_INFO`` contributions.

The client library tracks all contributions and assembles the final
"complete" event payload, which includes a complete membership array
plus all provided information.  In this case, the provided information
(e.g., that posted by each participant) will *not* be stored on the
server, but instead delivered to each individual participant's data
store for subsequent retrieval via `PMIx_Get`.

The PMIx server library's sole responsibility is transferring the
events generated by its local clients to/from the host.


Host responsibilities
^^^^^^^^^^^^^^^^^^^^^

The host is responsible solely for propagating event notifications across
participating processes.


Group Destruction
-----------------

A constructed group is torn down with ``PMIx_Group_destruct`` (or its
non-blocking form). Destruction is a collective over the *current*
membership: every member must call ``PMIx_Group_destruct`` with the group
identifier, and the operation does not complete until they have all done so
(or a fault-handling rule below resolves it). Because the group identifier
and its membership are held by the client libraries — not the host — the
destruct call carries the membership to its local server so the server knows
how many local participants to wait for. On completion the group identifier
and any assigned context ID are released, and the group is removed from each
member's local group list.

By default, ``PMIx_Group_destruct`` returns an error if any group member
fails or terminates before calling destruct. If the group was constructed
with ``PMIX_GROUP_NOTIFY_TERMINATION`` set to true, then instead of an error
each surviving member receives a ``PMIX_GROUP_MEMBER_FAILED`` event naming
the departed process, the destruct tracker is adjusted to no longer wait on
that process, and the operation returns ``PMIX_SUCCESS`` once the remaining
members have all called destruct. In other words, with notification enabled
the event takes the place of the error return, and the destruct completes on
the survivors rather than hanging. A ``PMIX_TIMEOUT`` directive bounds how
long the library will wait for a live member that never calls destruct.


Leaving a Group
---------------

An individual process may withdraw from a live group with
``PMIx_Group_leave`` (or its non-blocking form) without tearing the group
down for everyone else. This is deliberately *not* a collective: it is
intended for the asynchronous departure of a single process when the rest of
the group has a valid reason to continue. For all other scenarios —
especially coordinated teardown — ``PMIx_Group_destruct`` is the correct,
scalable choice.

A leave is realized entirely through the event subsystem. The call generates
a ``PMIX_GROUP_LEFT`` event, ranged to the current membership and naming the
departing process, and returns once that event has been *locally generated*
(the return is not indicative of remote receipt). The departing process
drops the group from its own local list immediately. Each remaining member,
on receiving ``PMIX_GROUP_LEFT``, updates its local record of the group
membership to remove the departed process.

A voluntary leave is treated by the library as the deliberate cousin of a
lost member (see below): if a group construct or destruct for that same group
is still in flight when the leave arrives, the leaving process is accounted
as "departed" so the in-flight collective can complete on the survivors
rather than block on a process that will never call in.


Fault Tolerance
---------------

Group construct and destruct are collective operations, and a collective
that waits on a participant which has died will hang forever unless the loss
is accounted for. PMIx detects three kinds of departure while a group
operation is still in flight — a dropped connection, an abnormal process
termination, and a voluntary ``PMIx_Group_leave`` — and resolves the
operation according to the directives supplied by the participants and the
events for which they have registered.

The events and attributes named in this section are all defined in
:ref:`Group Operations: Theory and Implementation <group-implementation-label>`,
which describes how the behavior is realized inside the library.


Member failure during construction
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

When a required member is lost before it contributes to a
``PMIx_Group_construct``, the outcome depends on the
``PMIX_GROUP_FT_COLLECTIVE`` directive (a boolean, default ``false``):

* **Default — abort.** Without the directive, the construct is *aborted*
  rather than silently forming a reduced group. Every surviving
  participant's ``PMIx_Group_construct`` returns
  ``PMIX_GROUP_CONSTRUCT_ABORT`` (non-blocking callers have their callback
  invoked with that status). This is the safe default: an application that
  expected an exact membership is told the membership could not be formed,
  instead of unknowingly proceeding with fewer processes.

* **Survive on the survivors.** With ``PMIX_GROUP_FT_COLLECTIVE`` set to
  true, the construct instead *completes on the surviving members*. Each
  survivor receives a ``PMIX_GROUP_MEMBER_FAILED`` event naming the lost
  member, so the application knows exactly who is missing from the group it
  just formed. The library synthesizes this event itself, so the behavior is
  available with any host environment, not only those that generate their
  own member-failure notifications.

This same accounting handles a member lost mid-*destruct*: a destruct always
completes on the survivors (subject to ``PMIX_GROUP_NOTIFY_TERMINATION`` for
whether the departed member is reported), because tearing a group down does
not require the departed process.


Leader failure
^^^^^^^^^^^^^^^

The *leader* of a group construction (the process that declared
``PMIX_GROUP_LEADER``, or the inviter in the invite method) occupies a
distinguished role, so its loss is reported separately from an ordinary
member's. A process that has accepted an invitation with
``PMIx_Group_join_nb`` watches the leader for the duration of the
construction. If the leader terminates before the construct completes, that
process receives a ``PMIX_GROUP_LEADER_FAILED`` event naming the lost leader,
rather than blocking indefinitely.

On receiving ``PMIX_GROUP_LEADER_FAILED`` the application may drive
reselection: a single process nominates itself by returning the
``PMIX_GROUP_LEADER`` attribute in the results array of its event handler,
and the outcome is communicated to all participants via a
``PMIX_GROUP_LEADER_SELECTED`` event identifying the new leader. If no leader
was selected, the status carried by that event lets the participants take
appropriate action. Any participant may instead force the whole procedure to
end by returning ``PMIX_GROUP_CONSTRUCT_ABORT`` from the handler, which
causes all participants to be notified of the abort.


Invitation failures
^^^^^^^^^^^^^^^^^^^^^

In the invite method, an invited process that declines
(``PMIx_Group_join`` with ``PMIX_GROUP_DECLINE``), terminates before
responding, or never responds within the leader's ``PMIX_TIMEOUT`` is a
failure of that invitation. The leader is notified of each such process via a
``PMIX_GROUP_INVITE_FAILED`` event. What happens next depends on
``PMIX_GROUP_OPTIONAL``:

* **All-or-nothing (default).** Without ``PMIX_GROUP_OPTIONAL``, any invitee
  that fails to accept causes the whole construct to abort: every
  participant receives ``PMIX_GROUP_CONSTRUCT_ABORT`` and the leader's
  ``PMIx_Group_invite`` returns that status. No group is formed.

* **Optional.** With ``PMIX_GROUP_OPTIONAL`` set to true, the failed invitees
  are simply excluded and the group forms on those that accepted; each
  member then receives ``PMIX_GROUP_CONSTRUCT_COMPLETE`` carrying the reduced
  membership.


Timeouts
^^^^^^^^

A ``PMIX_TIMEOUT`` directive (in seconds) may be supplied to any of the group
construction calls. It bounds how long the operation will wait for a *live*
participant that never calls in — the classic "a process fails to call the
API due to hanging" scenario. For the collective and bootstrap methods the
server enforces the timeout over the local phase: if the timer expires before
every expected local participant has contributed, the construct completes
with ``PMIX_ERR_TIMEOUT`` instead of hanging. For the invite method the
leader's library arms the timeout over the invitation responses. The host
environment remains responsible for any cross-node (global) timeout.


Detecting fault-tolerance support
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

A companion project (such as PRRTE) can detect at build time whether the PMIx
it is compiling against provides this group fault-tolerance feature set by
testing for the ``PMIX_CAP_GROUP_FT`` capability flag in the installed
``pmix_version.h``. The flag's *presence* is the signal that the behaviors
described in this section — the ``PMIX_GROUP_FT_COLLECTIVE`` gate, the
synthesized ``PMIX_GROUP_MEMBER_FAILED`` and ``PMIX_GROUP_LEADER_FAILED``
events, the group construct timeout, and the ``PMIX_GROUP_CANCEL`` operation
— are available.
