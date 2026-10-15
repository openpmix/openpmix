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

Every up-call made for a client or tool identifies the requester:

* **The requester's process ID** is the up-call's ``proc`` argument.
  Where an up-call has no such argument (``iof_pull``,
  ``register_events``), it is ``PMIX_REQUESTOR`` in the info array.
* **PMIX_USERID** is the uid PMIx established when the requester
  connected. Any ``PMIX_USERID`` the requester put in its request is
  removed. This is the identity to authorize.
* **PMIX_GRPID** is the group the requester asked to charge the work
  to, if it named one, and otherwise the gid established at connection.
  PMIx resolves a group name to a number but **does not check that the
  requester belongs to the group it names**. If the group matters to
  your decision, check membership yourself, and refuse the request
  (for example with ``PMIX_ERR_NO_PERMISSIONS``) if it fails.

Each appears exactly once. A few up-calls differ:

* ``fence_nb``, ``connect``, ``disconnect`` and ``group`` are made for
  all participants, not for one requester. They carry no identity.
  PMIx has already checked that each local caller is a participant.
* ``direct_modex`` carries ``PMIX_REQUESTOR``, ``PMIX_USERID`` and
  ``PMIX_GRPID`` for the requesters it stands for. Here ``PMIX_GRPID``
  is always the gid established at connection.
* ``notify_event`` carries no identity. Events are not governed by the
  access rule: the process raising an event chooses who receives it
  through the event's range.
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
or a launcher relays one to a scheduler. The operation should be judged
by who first asked for it, not by the process relaying it.

* **When you relay a request,** pass the original requester's
  ``PMIX_USERID`` and ``PMIX_GRPID`` with it, marked as relayed with
  :ref:`PMIx_Info_relayed(3) <man3-PMIx_Info_relayed>`.
* **The receiving server keeps a relayed identity** instead of
  replacing it with the relaying process's own. Only a PMIx server's
  library sends the mark; any other process's library clears it.
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
* Check group membership yourself before acting on a ``PMIX_GRPID``.
* Carry the requester's identity, marked as relayed, when you pass a
  request to another server, and with
  :ref:`PMIx_server_dmodex_request2(3) <man3-PMIx_server_dmodex_request2>`
  when you fetch data for it.
