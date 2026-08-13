Output Forwarding for Spawned Jobs
==================================

This page describes how the output of a *dynamically spawned* job is routed, what the current implementation does and does not do, and the plan for closing the gap. It is a working design document rather than a description of finished behavior: the sections marked **Planned** are not implemented yet.

The behavior we want
--------------------

A job spawned through ``PMIx_Spawn`` should have its output forwarded according to the following precedence, highest first:

1. **The output attributes the spawn request carried.** If the request named any of ``PMIX_FWD_STDOUT``, ``PMIX_FWD_STDERR`` or ``PMIX_FWD_STDDIAG``, that request is honored exactly as written - including a channel explicitly set to ``false``, which is how a spawn asks for silence from a job whose parent is being watched. A partial request is not merged into anything else; naming one channel settles the matter for all of them.

2. **Otherwise, the child inherits its parent's output forwarding** - unless the PRRTE ``inherit`` MCA parameter was explicitly *set* to false. "Inherits" means the child is treated the way the parent is being treated: whoever is receiving the parent job's output also receives the child's, on the same channels and with the same formatting.

Two properties of that second rule matter as much as the rule itself, and they are what make this more than a local bookkeeping change:

* It must hold **regardless of which daemon the originating tool attached to** when it launched the parent job, and it must hold under ``prterun`` as well as under a persistent DVM.

* It must also hold for a tool that connects to a DVM daemon *after* a job is running and asks (through ``PMIx_IOF_pull``) for that job's output to be forwarded to it. If that job subsequently spawns a child, the tool's request carries over to the child, so the tool receives a copy of the child's output too.

Finally, one thing that must **not** happen: output is never forwarded to an application process. Doing so is a loopback rather than a fallback - the output would simply be emitted again on the receiving process's own ``stdout``, for the runtime to collect and forward a second time. So where there is nothing to inherit, no forwarding is set up at all.

How output forwarding works today
---------------------------------

Responsibility is split cleanly between the two projects, and keeping the split straight is the key to reading any of this code:

* **PRRTE gets the bytes to the right daemon.** A ``prted`` captures its local processes' output and forwards it to the HNP; the HNP fans it back out. There is no daemon-to-daemon traffic - everything funnels through the HNP. PRRTE's own write machinery (sinks, write handlers) is used almost entirely for the ``stdin`` direction.

* **PMIx decides who gets a copy, and emits it.** Every daemon hands the output it has to ``PMIx_server_IOF_deliver()``. The PMIx server writes it locally if it is meant to, then walks ``pmix_globals.iof_requests`` - the list of ``pmix_iof_req_t`` subscriptions - and sends a copy to every registered requestor whose channel and source filters match. Output that matches nothing is cached in ``pmix_server_globals.iof`` until it ages out of that bounded cache.

A tool's subscription is therefore held by **the PMIx server of the daemon that tool attached to**, and only that server can deliver to it. For output produced elsewhere to reach that tool, PRRTE must relay a copy to that daemon. That is what ``prte_iof_hnp_relay_to_tool()`` does, and it selects the destination from ``jdata->originator``.

Two details of the current implementation are easy to miss and change the analysis:

* **The HNP hands everything it receives to its own PMIx server**, not merely what it relays onward. So a tool attached to the HNP - the ordinary ``prun --dvm-uri`` case - is reachable for any job's output, whether or not the relay would have selected the HNP.

* **PRRTE already inherits the parent's output** *formatting* **directives** onto a spawned child, gated on the same ``inherit`` flag as mapping, ranking and binding: tagging, timestamping and stderr/stdout merging are copied from parent to child. What is not inherited is anything about *who receives* the output.

What is missing
---------------

**PMIx did not give a spawned job any subscription at all.** The spawn parser defaulted the output channels on only when the spawn came from a *tool*, so a spawn issued by an ordinary application process - an ``MPI_Comm_spawn`` - subscribed to nothing, and the child's output matched no request and aged out of the cache unseen. Asking explicitly did not help either: ``PMIX_FWD_STDOUT`` is documented as forwarding the output "to this process", and the request was registered and the bytes shipped, but every place that would arm local writing for the spawned namespace is gated on the peer being a tool. There was no combination of directives a client could pass that worked.

This part is fixed. A spawn that names no output channel now clones the subscriptions covering the spawning process's own job onto the child's namespace, keeping the requestor, channels, formatting and handler id, so the child's output goes where the parent's goes. See ``inherit_parent_iof()`` in ``src/server/pmix_server_iof.c``. **It is only sufficient when the spawn is processed on the same server that holds the parent's subscription** - a single-node DVM, ``prterun`` on one node, or any layout where the spawner's daemon is also the tool's daemon.

**The inheritance is performed on the wrong daemon for a multi-node DVM.** ``pmix_server_process_iof()`` runs on whichever PMIx server received the spawn command, which is the daemon hosting the *spawning process*. The tool's subscription for the parent job lives on the daemon the *tool* attached to. When those differ, the clone is built where the output does not arrive, and the server that does have the output has nothing to match it against.

**PRRTE has no notion of a set of interested daemons.** ``jdata->originator`` is a single ``pmix_proc_t`` - "originator of a dynamic spawn" - and ``prte_iof_hnp_relay_to_tool()`` relays a job's output to exactly that one daemon. For a client-issued spawn the field is set to the requesting process and then overwritten at the HNP with the daemon that forwarded the launch request, so a child job's output is relayed toward the *spawner's* daemon rather than toward any tool.

**A tool that pulls an existing job's output is not recorded anywhere.** The ``iof_pull`` upcall handler defines sinks on the daemon's own ``stdout``/``stderr`` and returns; it does not record which tool asked, or on which daemon it sits. Consequently a tool attached to a daemon that hosts none of a job's processes receives nothing from the other nodes - and this is true of the *parent* job, not merely of a child. The last of the desired properties above therefore cannot be layered on top of the current code; the parent case has to be built first.

The plan
--------

The division follows the architectural split: PRRTE learns to route a job's output to every daemon that has an interested tool, and PMIx learns to recognize that a subscription covering a parent job also covers its children.

**Planned - PRRTE.**

1. Give a job a **set of interested daemons** for output-routing purposes, rather than deriving a single destination from ``jdata->originator``. Seed it with the daemon hosting the tool that launched the job, which is what ``originator`` supplies today.

2. Record the requesting tool's daemon into that set in the ``iof_pull`` upcall, for each job the request names. This is what makes a tool that attaches later and pulls an existing job's output work across nodes at all, and it is a prerequisite for the child case rather than part of it.

3. Fan out in ``prte_iof_hnp_relay_to_tool()`` to the whole set instead of to a single daemon.

4. Initialize a spawned child's set from its parent's, subject to the ``inherit`` rule. This is the routing half of the desired behavior, and it is what makes the property hold regardless of where the originating tool attached.

**Planned - PMIx.**

5. Add a **delivery-time ancestry fallback**. When output for a namespace matches no subscription - the point at which the server currently falls through to caching it - walk that namespace's ``PMIX_PARENT_ID`` chain and re-test the subscriptions against each ancestor. Any daemon that receives a child's output and holds a subscription for one of its ancestors then delivers it, with no new wire protocol and no propagation of subscriptions between servers. This is what makes the PRRTE routing change sufficient, and it makes the "tool attaches later, then the job spawns" case fall out without further work.

6. Provide a way for PRRTE's inheritance decision to reach PMIx. The timing is favorable: ``pmix_server_process_iof()`` runs from the spawn *completion*, after the host has processed the spawn, so the host has already decided by the time PMIx acts. The proposed mechanism is a new boolean attribute - absent meaning "inherit" - that PRRTE sets in the child namespace's job information; PMIx consults it when deciding whether to inherit. A server-wide default established at ``PMIx_server_init`` was considered and rejected because it cannot express a per-spawn override.

The existing spawn-time inheritance (item 5's predecessor) is kept rather than replaced. It is what implements the precedence rule itself - the spawn request's own attributes against the parent's settings - and it answers the common co-located case without waiting for output to arrive.

Open questions
--------------

* Does ``PMIX_PARENT_ID`` reach a child namespace's registration on daemons that did not process the spawn? The delivery-time fallback depends on it. If it does not, PRRTE must include it.

* Is a new attribute the right channel for the host's inheritance decision, and what should it be called? The alternative is for PRRTE to suppress inheritance by some other means and for PMIx to have no opinion.

* Should the ancestry walk be transitive - a grandchild inheriting through a child that itself inherited - or only one level? Transitive is the more obviously correct reading of "treated the way its parent is treated", and costs nothing extra on the miss path.

* Should a match found through the ancestry walk register a real subscription for the child, so subsequent chunks take the fast path, or be re-tested per delivery? Registering is faster but adds a lifetime question; re-testing is simpler and only runs on output that would otherwise have been cached.

Sequencing and testing
----------------------

The PMIx delivery-time fallback should land before the PRRTE routing change, because it is what makes the routing change pay off; until it is in place, relaying a child's output to a daemon holding only the parent's subscription still matches nothing.

None of this is reachable from ``make check``. The spawn-time inheritance is covered by ``test/unit/iof_inherit.c``, which drives the parser and the registration directly and reads the subscription list back - it establishes which subscriptions are created and for whom, which needs no spawn, and no spawn is available in any case because ``test/simple/simptest`` cannot host one. Everything described above as planned is about *which daemon* holds a subscription and *where* bytes are relayed, so it can only be demonstrated across several daemons. The ten-container swarm under ``contrib/dockerswarm`` is the vehicle for that, and the cases worth building there are:

* a client-issued spawn where the spawning rank is hosted by a daemon other than the one the launching tool attached to;
* the same under ``prterun`` rather than a persistent DVM;
* a tool that attaches to a non-master daemon, pulls a running job's output, and then observes the output of a job that job spawns.

The first of these is also the reproducer for the current gap, so it is worth building before any of the planned work, to hold the analysis above down.
