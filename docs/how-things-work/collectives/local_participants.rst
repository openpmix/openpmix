.. _collective-participants-label:

Telling the Host Who Took Part
==============================

A collective reaches the host as one upcall per server: ``fence_nb``,
``connect``, ``disconnect`` or ``group``. The upcall carries the processes
the callers *named* |mdash| the collective's membership |mdash| and,
until v7.0, nothing about which of them actually called. A host could
count the upcalls it received from each server, but it could not say
which processes it was still waiting for when an operation failed to
complete, nor check that the processes that took part were the ones the
operation names.

From v7.0 the server tells it. This page describes what the server
supplies, how a host can use it, and the attribute that carries the
other half of the answer |mdash| the members that never took part |mdash|
back to the participants.

The participant list
--------------------

Every ``fence_nb``, ``connect``, ``disconnect`` and ``group`` upcall
carries ``PMIX_LOCAL_PARTICIPANTS`` in its ``info`` array: a
``pmix_data_array_t`` of ``pmix_proc_t`` naming the local processes that
took part.

* **Who is on it.** A process is on the list if its call is on the
  tracker when the server makes the upcall |mdash| the
  ``local_cbs`` of the fence/connect/disconnect tracker, or of each of a
  group block's per-call trackers. That includes a process that called
  and has since lost its connection: it took part. A participant lost
  *before* it called is not on the list; it is reported through the
  departed accounting described in :ref:`the tracking specification
  <collectives-tracking-spec>` and the ``PMIX_LOCAL_COLLECTIVE_STATUS``
  the upcall also carries.

* **Its shape.** The list is sorted (``pmix_util_compare_proc`` order) and
  names each process once, however many calls it made.

* **Whole namespaces.** When every local process of a namespace took
  part, the namespace is given once, with rank ``PMIX_RANK_LOCAL_PEERS``,
  instead of rank by rank. "Local" means the node of the server making the
  upcall: a host that carries the list off the node must carry the node
  with it. A whole-job collective therefore costs one entry per job per
  node, however many ranks a node holds. A namespace some of whose local
  processes did not take part |mdash| a subset of a job, or a job that has
  lost a process |mdash| is listed by rank.

* **Every form of the operation.** The bootstrap, follower and
  add-members forms of a group construct, which the server hands up one
  call at a time, carry a list of their own call's process.

* **It is the server's.** A caller that puts ``PMIX_LOCAL_PARTICIPANTS`` in
  its own directives has it replaced, not passed up.

* **Where it sits.** The list goes ahead of the slots the participant
  handlers seed at the end of the array (``PMIX_SORTED_PROC_ARRAY`` and
  ``PMIX_LOCAL_COLLECTIVE_STATUS``), so those keep their places. A host
  should locate every entry by key regardless.

The list is built by ``pmix_server_set_participants()`` in
``src/server/pmix_server_fence.c``, which every upcall site calls just
before the upcall.

What a host can do with it
--------------------------

Nothing in PMIx requires a host to read the list, and a host that ignores
it behaves exactly as before. A host that reads it can:

* **Say who is missing.** The members of the collective, less the
  processes every server reported and those known to have failed, are the
  members that are alive and never called. A host can name them when an
  operation times out, rather than reporting only that it did.

* **Check the participants against the membership.** Each server already
  refuses a local caller that is not a member of what it names. A host
  that sees every server's list can check the same thing across the whole
  operation.

* **Count processes rather than calls.** A collective whose participants
  arrive one call at a time |mdash| a bootstrap group's leaders, say
  |mdash| can be counted by the processes reported, so that a repeated
  call does not stand in for a missing one.

PRRTE does all three. Each daemon packs the list it is handed into its
contribution as it arrives, tagged with its own rank since
``PMIX_RANK_LOCAL_PEERS`` means *its* node; relaying daemons append their
children's lists without opening them; and only the controller expands
the lists, through the job map, when it needs the non-participants or the
membership check.

Telling the participants who never took part
--------------------------------------------

``PMIX_COLLECTIVE_NONPARTICIPANTS`` is the host's answer: a
``pmix_data_array_t`` of ``pmix_proc_t`` naming the members that were
expected to take part and were still alive, but never did |mdash| for
example when an operation timed out, or a fault-tolerant
(``PMIX_GROUP_FT_COLLECTIVE``) group construct completed without them.

A host returns it in the ``info`` of its completion of a ``group``
construct. The server appends it to the reply it sends each participant
|mdash| after everything else the reply carries, which on a failed
construct is the status alone |mdash| and the client adds it to the
construct's results:

* ``PMIx_Group_construct_nb`` delivers it to the callback whatever the
  construct's status.
* ``PMIx_Group_construct`` returns its results on success, as always, and
  on failure *only* when they include this list. A caller of the blocking
  form that may see non-participants must therefore free ``results``
  whenever it is set, not only on success.

``PMIx_Fence``, ``PMIx_Connect`` and ``PMIx_Disconnect`` return no results
to their callers, so for those operations the host reports the
non-participants itself |mdash| in its own diagnostics, or in the error it
raises when the operation times out.

Compatibility
-------------

* A server before v7.0 sends no participant list. A host must treat a
  missing list as "not known" and fall back to counting upcalls.
* The non-participant list is appended to the construct reply, which a
  client stops reading once it has the fields it knows, so an older client
  ignores it. A newer client talking to an older server simply finds the
  end of the reply, and returns no list.
