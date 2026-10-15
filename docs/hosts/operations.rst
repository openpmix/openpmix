.. _label-host-env-operations:

Operation by Operation
======================

This page shows, for each operation, what PMIx does before it calls the
host, and what it then expects the host to do. Each up-call is
described in full in
:ref:`pmix_server_module_t(5) <man5-pmix_server_module_t>`.

Some things are true of every up-call made for a client or tool, and
are not repeated below:

* PMIx has already authenticated the requester's connection.
* PMIx has checked the request's arguments - counts, array types, the
  form of names.
* PMIx adds the requester's identity (see :doc:`authorization`).
* The host decides whether the requester may have the operation, and
  reports the result through the callback.

"If ``NULL``" says what happens when the host leaves the up-call out.


Collective operations
---------------------

A collective involves processes on many nodes. **PMIx assembles the
part on its own node. The host completes it across nodes.**

For ``fence_nb``, ``connect`` and ``disconnect``, PMIx:

* replaces any group name among the participants with the group's
  members;
* checks that each local caller is one of the participants, and fails
  it with ``PMIX_ERR_NOT_A_MEMBER`` if not;
* sorts the participants, and says so with ``PMIX_SORTED_PROC_ARRAY``;
* waits until every local participant has called, enforcing any
  ``PMIX_TIMEOUT`` meanwhile;
* calls the host **once** for the whole node, with
  ``PMIX_LOCAL_COLLECTIVE_STATUS`` reporting how the local part went.
  If that status is an error, the host should fail the collective
  rather than complete it.

The host must use the job information it registered to know how many
local participants to expect; that is why every job taking part in a
collective with a local process must be registered, even one with no
process on this node.

fence_nb
^^^^^^^^

Up-call: :ref:`fence_nb <man5-pmix_server_module_t-fence_nb>`.

Services :ref:`PMIx_Fence(3) <man3-PMIx_Fence>`.

**PMIx:** collects the data the local participants posted into one
blob and passes it to the host. If every participant is on this node,
PMIx completes the fence itself and does not call the host (unless
``pmix_server_fence_localonly_opt`` is set to false).

**The host:**

* runs the collective across every server hosting a participant;
* if data is being collected (``PMIX_COLLECT_DATA``), returns the
  combined blobs of all those servers through the callback;
* honors the directives, and fails the fence if it cannot honor one
  marked required;
* ``free()``\ s the blob it was given when it is done with it.

**If NULL:** a fence that spans nodes fails with
``PMIX_ERR_NOT_SUPPORTED``.

connect / disconnect
^^^^^^^^^^^^^^^^^^^^

Up-calls: :ref:`connect <man5-pmix_server_module_t-connect>`, :ref:`disconnect <man5-pmix_server_module_t-disconnect>`.

Service :ref:`PMIx_Connect(3) <man3-PMIx_Connect>` and
:ref:`PMIx_Disconnect(3) <man3-PMIx_Disconnect>`.

**PMIx:** for ``connect``, adds to the request, for each local
participant, the data it posted for remote access
(``PMIX_PROC_INFO_ARRAY``), and passes on the job information each
participant supplied (``PMIX_JOB_INFO_ARRAY``). When the host completes
the operation, PMIx gives each local participant the job-level
information of every participating job, from what it holds.

**The host:**

* runs the collective across every server hosting a participant. For
  ``connect`` this is an exchange of data, not just a barrier, even
  though the request carries no ``PMIX_COLLECT_DATA``: the
  ``PMIX_PROC_INFO_ARRAY`` and ``PMIX_JOB_INFO_ARRAY`` entries from every
  server must reach every other;
* before completing the operation on a server, gives that server those
  entries and the job-level information of every participating job.
  Two ways to do it:

  - register each entry as an update to its job with
    :ref:`PMIx_server_register_nspace(3) <man3-PMIx_server_register_nspace>`,
    passing a negative number of local processes. A
    ``PMIX_PROC_INFO_ARRAY`` updates the process it describes; a
    ``PMIX_JOB_INFO_ARRAY`` updates the job. This is what PRRTE does.
    The job must already be registered with that server - PRRTE
    registers every job with every server;
  - or deliver them with
    :ref:`PMIx_server_register_resources(3) <man3-PMIx_server_register_resources>`,
    as for ``group`` below;

* from then on, treats the connected processes as one: when one of
  them terminates without disconnecting, it generates
  ``PMIX_ERR_PROC_TERM_WO_SYNC`` for the others;
* for ``disconnect``, fails the operation if the processes were not
  connected.

**If NULL:** an operation whose participants are all on this node
completes locally; any other fails with ``PMIX_ERR_NOT_SUPPORTED``.

group
^^^^^

Up-call: :ref:`group <man5-pmix_server_module_t-group>`.

Services :ref:`PMIx_Group_construct(3) <man3-PMIx_Group_construct>`,
:ref:`PMIx_Group_destruct(3) <man3-PMIx_Group_destruct>`, and their
related operations.

**PMIx:**

* replaces any group name among the participants with its members, and
  checks that each local caller is a participant (except for processes
  joining as added members or through bootstrap, which are not named in
  advance);
* builds, for each local participant, the data it posted for remote
  access, and attaches it to the request;
* combines the local participants into one request, except for
  bootstrap and invited members, which reach the host one call at a
  time;
* when the operation completes, stores what the host returned and
  releases the participants.

**The host:**

* runs the collective across every server hosting a participant, and
  declares it complete only when every member has taken part - for a
  bootstrap, the stated number of leaders plus each added member;
* returns through the callback the final membership
  (``PMIX_GROUP_MEMBERSHIP``), a context ID if one was requested
  (``PMIX_GROUP_CONTEXT_ID``), and the group information participants
  contributed (``PMIX_GROUP_INFO``);
* before completing the operation on a server, gives that server:

  - **the job-level information of every job with a participant.** A
    host that registers every job with every server, as PRRTE does,
    has done this already. Otherwise, collect it where it is held with
    :ref:`PMIx_server_collect_job_info(3) <man3-PMIx_server_collect_job_info>`
    and deliver it with
    :ref:`PMIx_server_register_resources(3) <man3-PMIx_server_register_resources>`
    as ``PMIX_GROUP_JOB_INFO``;
  - **the data every participant posted for remote access** - the
    contributions PMIx attached to the request - delivered with
    ``PMIx_server_register_resources`` as ``PMIX_GROUP_ENDPT_DATA``;

* stops the operation when PMIx sends ``PMIX_GROUP_CANCEL``.

Each participant then has the job-level information of every
participating job, and every participant's posted data.

**If NULL:** ``PMIX_ERR_NOT_SUPPORTED``.

For the construction methods and their data flow, see
:ref:`group-construction-label` and :ref:`group-implementation-label`.


Data
----

direct_modex
^^^^^^^^^^^^

Up-call: :ref:`direct_modex <man5-pmix_server_module_t-direct_modex>`.

Fetches data a process posted, when it is held on another node.

**PMIx:** applies the access rule for the requester, then asks the host
for the target process's data. Requesters with the same identity share
one request.

**The host:** asks the server holding the data with
:ref:`PMIx_server_dmodex_request2(3) <man3-PMIx_server_dmodex_request2>`,
passing the identity from the up-call. That server applies the rule
again before answering. The host returns the data through the callback;
it may not answer ``PMIX_OPERATION_SUCCEEDED``.

**If NULL:** the request fails with ``PMIX_ERR_NOT_FOUND``.

publish / lookup / unpublish
^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Up-calls: :ref:`publish <man5-pmix_server_module_t-publish>`, :ref:`lookup <man5-pmix_server_module_t-lookup>`, :ref:`unpublish <man5-pmix_server_module_t-unpublish>`.

Service :ref:`PMIx_Publish(3) <man3-PMIx_Publish>`,
:ref:`PMIx_Lookup(3) <man3-PMIx_Lookup>` and
:ref:`PMIx_Unpublish(3) <man3-PMIx_Unpublish>`.

**PMIx:** passes the request up with the requester's identity.

**The host:** keeps the published data, applies its range and
persistence, records who published each item, and decides who may see
or remove it - honoring any ``PMIX_ACCESS_PERMISSIONS`` the publisher
gave. ``lookup`` may not answer ``PMIX_OPERATION_SUCCEEDED``.

**If NULL:** ``PMIX_ERR_NOT_SUPPORTED``.

query
^^^^^

Up-call: :ref:`query <man5-pmix_server_module_t-query>`.

Services :ref:`PMIx_Query_info(3) <man3-PMIx_Query_info>`, and the
queries behind :ref:`PMIx_Resolve_peers(3) <man3-PMIx_Resolve_peers>`
and :ref:`PMIx_Resolve_nodes(3) <man3-PMIx_Resolve_nodes>`.

**PMIx:** answers every query it can from what it holds, applying the
access rule. Only the rest go to the host.

**The host:** answers the remaining queries, deciding what the
requester may learn.

**If NULL:** the requester receives what PMIx could answer, as
``PMIX_ERR_PARTIAL_SUCCESS``. If PMIx could answer none of it, the
request fails.


Jobs and resources
------------------

spawn
^^^^^

Up-call: :ref:`spawn <man5-pmix_server_module_t-spawn>`.

Services :ref:`PMIx_Spawn(3) <man3-PMIx_Spawn>`.

**PMIx:** resolves user and group names in the job's directives, and
sets up output forwarding and inheritance for the new job.

**The host:** decides whether the requester may launch the job, then
launches all of it or none of it. It records any
``PMIX_ACCESS_PERMISSIONS`` given, to register with the job. The new
job's namespace is returned through the callback;
``PMIX_OPERATION_SUCCEEDED`` is not allowed.

**If NULL:** ``PMIX_ERR_NOT_SUPPORTED`` - except in a launcher that
runs its own processes, which starts the job itself for its own user.

abort2 / abort
^^^^^^^^^^^^^^

Up-calls: :ref:`abort2 <man5-pmix_server_module_t-abort2>`, :ref:`abort <man5-pmix_server_module_t-abort>`.

Services :ref:`PMIx_Abort(3) <man3-PMIx_Abort>`.

**PMIx:** checks the request's form only. It does not check who the
targets are.

**The host:** decides whether the requester may terminate the
processes named (all of the requester's job, if none are named), then
terminates them.

**If NULL:** ``PMIX_ERR_NOT_SUPPORTED``.

job_control
^^^^^^^^^^^

Up-call: :ref:`job_control <man5-pmix_server_module_t-job_control>`.

Services :ref:`PMIx_Job_control(3) <man3-PMIx_Job_control>`.

**PMIx:** handles cleanup directives itself (``PMIX_REGISTER_CLEANUP``
and related), applying the access rule to each job they name. A request
containing nothing else is completed without calling the host.

**The host:** decides whether the requester may act on the processes
named, and performs every other action - signal, kill, pause, resume,
terminate.

**If NULL:** ``PMIX_ERR_NOT_SUPPORTED``, including for requests that
contain only cleanup directives.

monitor
^^^^^^^

Up-call: :ref:`monitor <man5-pmix_server_module_t-monitor>`.

Services :ref:`PMIx_Process_monitor(3) <man3-PMIx_Process_monitor>`.

**PMIx:** monitors processes on its own node itself, applying the
access rule. It calls the host only when the request names processes on
other nodes.

**The host:** decides whether the requester may monitor those
processes, and arranges it. A host that wants its processes monitored
must record their pids (``PMIX_PROC_PID``) when it registers them. See
:doc:`/how-things-work/pstat`.

**If NULL:** requests for other nodes fail with
``PMIX_ERR_NOT_SUPPORTED``.

allocate / session_control / resource_block
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Up-calls: :ref:`allocate <man5-pmix_server_module_t-allocate>`, :ref:`session_control <man5-pmix_server_module_t-session_control>`, :ref:`resource_block <man5-pmix_server_module_t-resource_block>`.

Service :ref:`PMIx_Allocation_request(3) <man3-PMIx_Allocation_request>`,
:ref:`PMIx_Session_control(3) <man3-PMIx_Session_control>` and
:ref:`PMIx_Resource_block(3) <man3-PMIx_Resource_block>`.

**PMIx:** passes the request up with the requester's identity.

**The host:** decides whether the requester may obtain, change, or
release the resources or session concerned, and does so - often by
relaying the request to the scheduler. See
:doc:`/how-things-work/schedulers/index`.

**If NULL:** ``PMIX_ERR_NOT_SUPPORTED``.


Input and output
----------------

iof_pull
^^^^^^^^

Up-call: :ref:`iof_pull <man5-pmix_server_module_t-iof_pull>`.

Services :ref:`PMIx_IOF_pull(3) <man3-PMIx_IOF_pull>`.

**PMIx:** records the request but forwards nothing until the host
approves it. Output that arrives meanwhile is held, and delivered once
approved.

**The host:** decides whether the requester may receive output from the
named processes, which may belong to any job. To refuse, report an
error through the callback.

**If NULL:** ``PMIX_ERR_NOT_SUPPORTED``.

push_stdin
^^^^^^^^^^

Up-call: :ref:`push_stdin <man5-pmix_server_module_t-push_stdin>`.

Services :ref:`PMIx_IOF_push(3) <man3-PMIx_IOF_push>`.

**PMIx:** passes the input up, naming its source. It does not check
the targets.

**The host:** decides whether the requester may send input to the
targets, and delivers it.

**If NULL:** ``PMIX_ERR_NOT_SUPPORTED``.


Events
------

register_events / deregister_events
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Up-calls: :ref:`register_events <man5-pmix_server_module_t-register_events>`, :ref:`deregister_events <man5-pmix_server_module_t-deregister_events>`.

**PMIx:** counts registrations itself. It calls ``register_events``
only for *system* event codes gaining their first registrant, and
``deregister_events`` when one loses its last.

**The host:** forwards those system events while they are registered.
It reports events concerning its jobs whether or not anything is
registered.

**If NULL:** registering for a system event fails with
``PMIX_ERR_NOT_SUPPORTED``. Other registrations are handled locally.

notify_event
^^^^^^^^^^^^

Up-call: :ref:`notify_event <man5-pmix_server_module_t-notify_event>`.

Services :ref:`PMIx_Notify_event(3) <man3-PMIx_Notify_event>`.

**PMIx:** sets the event's source to the process that raised it, checks
the few events a client may not raise, delivers the event on this node,
and calls the host only if the event's range extends beyond the node.

**The host:** delivers the event to every server where its targets run.

**If NULL:** the event is delivered on this node only.


Other operations
----------------

log2 / log
^^^^^^^^^^

Up-calls: :ref:`log2 <man5-pmix_server_module_t-log2>`, :ref:`log <man5-pmix_server_module_t-log>`.

Services :ref:`PMIx_Log(3) <man3-PMIx_Log>`.

**PMIx:** adds the source and, if requested, a timestamp. A server
acting as a logging gateway logs the data itself and does not call the
host.

**The host:** sends the data to the channels requested, or to a
gateway. It must not call ``PMIx_Log`` from this up-call.

**If NULL:** ``PMIX_ERR_NOT_AVAILABLE``.

fabric
^^^^^^

Up-call: :ref:`fabric <man5-pmix_server_module_t-fabric>`.

Services :ref:`PMIx_Fabric_register(3) <man3-PMIx_Fabric_register>`
and :ref:`PMIx_Fabric_update(3) <man3-PMIx_Fabric_update>`.

**PMIx:** answers from its own network support if it can.

**The host:** obtains the fabric information, normally from the server
that supports the system scheduler.

**If NULL:** ``PMIX_ERR_NOT_SUPPORTED``.

get_credential / validate_credential
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Up-calls: :ref:`get_credential <man5-pmix_server_module_t-get_credential>`, :ref:`validate_credential <man5-pmix_server_module_t-validate_credential>`.

See :ref:`label-host-env-connections`. The result travels only through
the callback, so ``PMIX_OPERATION_SUCCEEDED`` is not allowed.

client_connected2 / tool_connected2 / client_finalized
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Up-calls: :ref:`client_connected2 <man5-pmix_server_module_t-client_connected2>`, :ref:`tool_connected2 <man5-pmix_server_module_t-tool_connected2>`, :ref:`client_finalized <man5-pmix_server_module_t-client_finalized>`.

See :ref:`label-host-env-connections` and :doc:`partnership`.
