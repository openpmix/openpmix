.. _label-host-env-authorization:

Deciding What a Requester May Do
================================

Once a process is connected, every request it makes is either handled
by the PMIx library or passed up to the host. **Whoever performs the
operation decides whether the requester may have it.** The library
decides for what it does itself. The host decides for everything passed
up to it - and the library gives the host what it needs to decide.

The host itself is not restricted. These rules govern requests made by
clients and tools.


Who is asking
-------------

Every up-call made for a client or tool identifies the requester - the
process connected to this server - as its connection established it:

* **The requester's process ID** is the up-call's ``proc`` argument.
  Where an up-call has no such argument (``iof_pull``,
  ``register_events``), it is ``PMIX_REQUESTOR`` in the info array.
* **PMIX_USERID** and **PMIX_GRPID** are the uid and gid PMIx
  established when the requester connected. Any the requester put in
  its request are removed. This is the identity to authorize.
* **PMIX_REQUESTOR_PID** is the pid the kernel reported for the
  requester's connection. It is present only where the kernel reports
  one: on the local transport, on platforms that do. It is never
  present for a peer connected over TCP, where the only pid on record
  is the one the peer's library sent. Any ``PMIX_REQUESTOR_PID`` the
  requester put in its request is removed.
* **PMIX_REQUESTED_GRPID** is the group the requester asked to have the
  work charged to, if it named one - as ``PMIX_REQUESTED_GRPID``, or as
  ``PMIX_GRPID``, which earlier releases passed up for it. PMIx
  resolves a group name to a number but **does not check that the
  requester belongs to the group it names**. If the group matters to
  your decision, check membership yourself, and refuse the request
  (for example with ``PMIX_ERR_NO_PERMISSIONS``) if it fails. A request
  that names more than one, or a name that cannot be resolved, is
  refused before it reaches you.

Each appears at most once. A few up-calls differ:

* ``fence_nb``, ``connect``, ``disconnect`` and ``group`` are made for
  all participants, not for one requester. They carry no identity.
  PMIx has already checked that each local caller is a participant.
* ``publish`` carries only ``PMIX_USERID`` and ``PMIX_GRPID``: its info array
  is the data being published, so nothing more is added that a host might
  store as a published key. Skip those, and any ``PMIX_ORIGIN_*``, when you
  store the data.
* ``direct_modex`` carries ``PMIX_REQUESTOR``, ``PMIX_USERID``,
  ``PMIX_GRPID`` and, where there is one, ``PMIX_REQUESTOR_PID`` for
  the requesters it stands for. It carries no requested group.
* ``notify_event``, for an event a client raised, carries that
  client's ``PMIX_USERID``, ``PMIX_GRPID`` and, where there is one,
  ``PMIX_REQUESTOR_PID``; the ``source`` argument is the client. A
  host passing an event on decides with that identity whether it may.
  **For** ``PMIX_DEBUGGER_RELEASE``, **check, before relaying or
  delivering it, that the user may access every job the release can
  reach** - the jobs named in a custom range (a wildcard namespace
  meaning every job); the source's own job for
  ``PMIX_RANGE_NAMESPACE``; and for any wider range, every job you
  would deliver it to. PMIx applies that rule itself to the jobs
  registered with this server; for any other, the decision is yours.
* The deprecated ``abort`` has no info array; use ``abort2``, which
  carries the identity.

When the host itself calls a PMIx API that is serviced by its own
module, the up-call names the server as the requester and adds nothing.

The full list, per up-call, is under "The requester's identity" in
:ref:`pmix_server_module_t(5) <man5-pmix_server_module_t>`.


Requests that pass through more than one server
-----------------------------------------------

A request can travel through several servers. For example, a workflow
manager relays a request from its own client to the resource manager,
or a launcher relays one to a scheduler. Whether to pass a request on,
and whether to believe what a relay says about who first asked for it,
is policy - and it belongs to the host at each level. PMIx does not
make that decision, and does not relay a peer's request on its own: a
request it cannot serve goes to your up-call, and is refused if you
have none.

* **What you are told is always the relay.** ``PMIX_USERID``,
  ``PMIX_GRPID`` and ``PMIX_REQUESTOR_PID`` in an up-call describe the
  process connected to you, as its connection established it. Nothing
  a relay sends replaces them. A host that knows nothing of relays
  therefore judges the request as the relay's own - it fails safe.
* **What the relay claims comes beside it.** A relay names the process
  that first made the request with ``PMIX_ORIGIN_USERID``,
  ``PMIX_ORIGIN_GRPID``, ``PMIX_ORIGIN_PID``, ``PMIX_ORIGIN_IS_TOOL``
  and ``PMIX_ORIGIN_IS_CLIENT``, and with ``PMIX_REQUESTOR`` for its
  process ID. PMIx passes these to you untouched - only you can judge
  them - and refuses a request that carries any of them more than once.
* **When you accept relays,** decide from the verified identity (and
  pid) whether this peer may speak for others. If it may, attribute the
  work to the ``PMIX_ORIGIN_*`` claim, and check any
  ``PMIX_REQUESTED_GRPID`` against whoever you attribute the work to. If
  it may not, judge the request as the relay's own, or refuse it.
* **When you relay a request,** copy the identity you attributed the
  work to into the ``PMIX_ORIGIN_*`` attributes. At the first hop that
  is the up-call's own ``PMIX_USERID``, ``PMIX_GRPID`` and
  ``PMIX_REQUESTOR_PID``. Carry a requested group on as
  ``PMIX_REQUESTED_GRPID``.
* **Carry the identity between your own daemons** when you forward a
  request from one node to another.


The access rule
---------------

PMIx has one rule for access to a job: a requester may access a job if
any of these holds:

#. its uid is 0 (root);
#. its uid is the server's own effective uid;
#. its uid is the job owner's;
#. its uid is in the job's access list;
#. it belongs to a group in the job's access list.

The job's owner and access list come from the host, when it registers
the job (``PMIX_USERID``, ``PMIX_GRPID``,
``PMIX_ACCESS_PERMISSIONS``). If the host gives no owner, the owner is
the user the job's clients were registered as, and failing that the
server's own user. A host running as a service account should
therefore always register each job's owner.

The rule, what it covers, and how it handles data held on other nodes
are described in full in :doc:`/hosts/access-control`.


What PMIx decides
-----------------

PMIx applies the rule to requests it answers or performs itself:

* reading another job's data (``PMIx_Get``), whether held locally or
  fetched from another node;
* queries and resolve requests it can answer itself;
* monitoring of processes on its own node;
* cleanup directives on ``PMIx_Job_control``;
* output that follows a job into the jobs it spawns;
* the ownership and mode of output files and shared-memory job data.

PMIx also checks participation: a process calling ``PMIx_Fence``,
``PMIx_Connect``, ``PMIx_Disconnect``, ``PMIx_Group_construct`` or
``PMIx_Group_destruct`` must be one of the participants it names, or
the call fails with ``PMIX_ERR_NOT_A_MEMBER``.


What the host decides
---------------------

PMIx does not decide whether a requester may have an operation it
passes up. The host does. In particular, **PMIx makes no access check
on the targets of these up-calls**:

.. list-table::
   :header-rows: 1
   :widths: 26 74

   * - Up-call
     - The host decides
   * - :ref:`abort2 <man5-pmix_server_module_t-abort2>` / :ref:`abort <man5-pmix_server_module_t-abort>`
     - whether the requester may terminate the processes it names.
   * - :ref:`job_control <man5-pmix_server_module_t-job_control>`
     - whether the requester may signal, kill, pause, resume, or
       otherwise act on the processes it names.
   * - :ref:`push_stdin <man5-pmix_server_module_t-push_stdin>`
     - whether the requester may send input to the processes it names.
   * - :ref:`spawn <man5-pmix_server_module_t-spawn>`
     - whether the requester may launch the job, with the resources and
       options it asks for.
   * - :ref:`allocate <man5-pmix_server_module_t-allocate>`, :ref:`session_control <man5-pmix_server_module_t-session_control>`, :ref:`resource_block <man5-pmix_server_module_t-resource_block>`
     - whether the requester may obtain, change, or release the
       resources or session concerned.
   * - :ref:`iof_pull <man5-pmix_server_module_t-iof_pull>`
     - whether the requester may receive output from the named
       processes. PMIx forwards nothing until the host approves.
   * - :ref:`monitor <man5-pmix_server_module_t-monitor>`
     - whether the requester may monitor processes on other nodes.
       (PMIx handles processes on its own node.)
   * - :ref:`query <man5-pmix_server_module_t-query>`
     - what the requester may learn from the queries PMIx could not
       answer itself.
   * - :ref:`publish <man5-pmix_server_module_t-publish>`, :ref:`lookup <man5-pmix_server_module_t-lookup>`, :ref:`unpublish <man5-pmix_server_module_t-unpublish>`
     - who may see and remove each published item, honoring any
       ``PMIX_ACCESS_PERMISSIONS`` the publisher gave.
   * - :ref:`log2 <man5-pmix_server_module_t-log2>` / :ref:`log <man5-pmix_server_module_t-log>`
     - whether the requester may log to the channels it names.
   * - :ref:`fabric <man5-pmix_server_module_t-fabric>`
     - what fabric information the requester may receive.
   * - :ref:`get_credential <man5-pmix_server_module_t-get_credential>`, :ref:`validate_credential <man5-pmix_server_module_t-validate_credential>`
     - whether to issue or check a credential for the requester.

A host that is linked with the library can apply exactly the library's
rule by calling ``pmix_server_access_check()`` with its own copies of a
job's access list and the requester's groups. See "What a host needs to
do" in :doc:`/hosts/access-control`.


Summary for host developers
---------------------------

* Register each job's owner and access list on every node that hosts
  part of the job.
* Accept ``PMIX_ACCESS_PERMISSIONS`` in spawn requests and carry it
  with the job.
* Authorize every operation PMIx passes up, using ``PMIX_USERID`` from
  the up-call - never an identity the request itself claims.
* Check group membership yourself before acting on a
  ``PMIX_REQUESTED_GRPID``.
* Before relaying or delivering a ``PMIX_DEBUGGER_RELEASE``, check that
  its raiser may access every job it can reach.
* Accept a ``PMIX_ORIGIN_*`` claim only from a relay you trust to make
  it. Carry the identity you attributed the work to in
  ``PMIX_ORIGIN_*`` when you pass a request to another server.
* Carry the requester's ``PMIX_USERID``, ``PMIX_GRPID`` and
  ``PMIX_REQUESTOR`` with
  :ref:`PMIx_server_dmodex_request2(3) <man3-PMIx_server_dmodex_request2>`
  when you fetch data for it.
