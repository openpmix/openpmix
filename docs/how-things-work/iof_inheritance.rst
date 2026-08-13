Output Forwarding for Spawned Jobs
==================================

This page describes how the output of a *dynamically spawned* job is routed, what the current implementation does and does not do, and what is left. It is a working design document rather than a description of finished behavior. The PMIx side is implemented; the items still marked **Planned - PRRTE** are not, and the "What is missing" section below is kept as the description of the defect each fix closed rather than as a statement of current behavior.

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

This is now demonstrated rather than argued: ``contrib/dockerswarm/run-spawn-iof.sh`` runs the same one-rank job twice against the same six-node DVM and the same tool, moving only the rank. With the rank on the tool's own daemon the child's output comes back on both channels; with the rank one node over, none of it does. The run also sharpens the diagnosis in a way worth keeping. In the failing case the child job happened to be placed on the *tool's own node*, so its output reached the very server that holds the tool's subscription - and was still dropped, because no subscription for the child's namespace was ever created anywhere. The two halves of the gap are therefore independent: this one is a PMIx bug about *where* the inheritance is performed, and it would still bite after PRRTE learns to route a child's output correctly.

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

**PMIx - done.** Both items below are implemented.

5. A **delivery-time ancestry fallback**. When output for a namespace matches no subscription - the point at which the server used to fall through to caching it - the server walks that namespace's ``PMIX_PARENT_ID`` chain and tests the subscriptions against each ancestor. A daemon that receives a child's output and holds a subscription for one of its ancestors delivers it, with no new wire protocol and no propagation of subscriptions between servers. See ``inherit_from_ancestry()`` in ``src/server/pmix_server_iof.c``.

   Two details of the implementation answered open questions that were on this page:

   * A match **clones** the ancestor's subscriptions onto the child's namespace rather than delivering that one chunk, so every later chunk takes the ordinary path and the datastore is consulted once per namespace instead of once per line. The clones are the same ones the spawn-time path creates, made by the same function, and have the same lifetime.

   * The walk is **transitive**, bounded by ``PMIX_IOF_MAX_ANCESTRY``. "Treated the way its parent is treated" says nothing about depth, and the chain is host-supplied data, so the bound is what keeps a malformed or circular one from spinning the progress thread. It stops at the first ancestor anybody here is watching: once the clones exist, that generation's watchers are this namespace's watchers.

   This turned out to close more than it was expected to. ``PMIX_PARENT_ID`` is per-process job data, so it is available on any server the namespace was registered with, and PRRTE funnels every daemon's output through the HNP, which hands everything it receives to its own PMIx server - which is where an ordinary ``prun`` or ``prterun`` tool's subscription lives. So for a tool attached to the HNP, the fallback alone is sufficient: ``run-spawn-iof.sh`` now passes with the tool, the spawning rank and the child job on three different daemons, against unmodified PRRTE.

6. A channel for the host's inheritance decision: ``PMIX_IOF_INHERIT``, a boolean the host places in the job-level information of the spawned namespace. Absent means "inherit", so a host says nothing unless it is turning inheritance off. Both decision points consult it through ``iof_inherit_allowed()``.

   Job information is the channel rather than anything carried with the spawn request, and that follows from the two decision points being on *different daemons*: the flag has to be readable by the server the output arrives at, which may have had nothing to do with the spawn. Job info is exactly the thing PRRTE already ships to every daemon that registers the namespace - it is how ``PMIX_PARENT_ID`` gets there. Nothing about it is read by the spawned processes; it is server-side state that happens to be keyed by their namespace. A server-wide default at ``PMIx_server_init`` was rejected because it cannot express a per-job answer, and PRRTE's inherit setting is per-job.

   The spawn-time site needs one more rule, because it is the only place where "the host said nothing" and "we cannot ask yet" are distinguishable. It runs from the spawn *completion*, and whether the child's namespace has been registered with that server by then is the host's business - a daemon hosting none of the child's processes may never register it at all. A fetch that found nothing there would read as "inherit", which is the wrong way to be wrong: it would clone against the host's wishes and nothing would take the clone back. So an unregistered namespace defers to the delivery-time half, which cannot run before the namespace exists and therefore always has a real answer. Deferring is never *behaviorally* wrong - it only postpones a decision that the other half will make correctly.

The spawn-time inheritance is kept rather than replaced. It is what implements the precedence rule itself - the spawn request's own attributes against the parent's settings - and it answers the co-located case without waiting for output to arrive. The two are independent: neither needs the other to have happened, and where both apply the second finds the subscription the first created and does nothing.

**The PRRTE side.** Items 1-4 are the answer for a tool that is *not* attached to the HNP, and for a tool that attaches to a running DVM and pulls a job's output. Neither is reachable from the PMIx side: the first needs the output relayed to a daemon PRRTE does not otherwise send it to, and the second needs the request recorded against the job at all. All four are implemented in prrte#2658, along with PRRTE's half of item 6 - setting ``PMIX_IOF_INHERIT`` when its own ``inherit`` rule refuses. Note the default there is the opposite of this one: PRRTE does not inherit *launch directives* unless asked, so the gate on output forwarding has to be the explicit refusal rather than the resolved value.

Answered
--------

* *Is a new attribute the right channel for the host's inheritance decision?* **Yes**, and it is ``PMIX_IOF_INHERIT`` in the spawned namespace's job information. The alternative considered - PMIx having no opinion, and a spawn controlling its own output through the ``PMIX_FWD_*`` directives it already has - would have left a launcher's ``inherit`` setting silently not covering output forwarding. See item 6.

* *Does* ``PMIX_PARENT_ID`` *reach a child namespace's registration on daemons that did not process the spawn?* **Yes.** PRRTE adds it to the per-process job data in ``prte_pmix_server_register_nspace()``, which every daemon registering the namespace receives; ``pmix_pfexec`` records it for the job as a whole. The lookup tries the specific rank and then the wildcard, so both shapes answer. Demonstrated by ``run-spawn-iof.sh`` case 3, where the decision is taken on a daemon that hosts neither the spawner nor the child.

* *Should the ancestry walk be transitive?* **Transitive**, bounded. See item 5.

* *Should a match register a real subscription for the child, or be re-tested per delivery?* **Register.** Re-testing looked simpler until the cost was written down: it puts a datastore fetch in front of every line a watched job writes, where registering pays once per namespace. The lifetime question it seemed to add is not a new one - the clone is identical to the one the spawn-time path already creates, and is made by the same function.

Sequencing and testing
----------------------

The PMIx delivery-time fallback landed first, and it turned out to be sufficient on its own for a tool attached to the HNP - which is every tool PRRTE's own launchers produce. The remaining PRRTE work is what a tool attached elsewhere needs.

None of this is reachable from ``make check``. The spawn-time inheritance is covered by ``test/unit/iof_inherit.c``, which drives the parser and the registration directly and reads the subscription list back - it establishes which subscriptions are created and for whom, which needs no spawn, and no spawn is available in any case because ``test/simple/simptest`` cannot host one. Everything described above as planned is about *which daemon* holds a subscription and *where* bytes are relayed, so it can only be demonstrated across several daemons. The ten-container swarm under ``contrib/dockerswarm`` is the vehicle for that, and the cases worth building there are:

* a client-issued spawn where the spawning rank is hosted by a daemon other than the one the launching tool attached to;
* the same under ``prterun`` rather than a persistent DVM;
* a tool that attaches to a non-master daemon, pulls a running job's output, and then observes the output of a job that job spawns.

The first two are built, in ``contrib/dockerswarm/run-spawn-iof.sh``, driving ``examples/spawn_iof.c``; README §21 describes the geometry. The runner reports:

* **spawner on the tool's daemon** - the child's output comes back on both channels. This is the control: it says the example, the launcher and the capture work, so that the next line means something.
* **spawner one node over** - nothing of the child's output comes back. This is the reproducer, and it fails today.
* **the same under prterun** - the output does reach the terminal. ``prterun`` also writes output locally, so its terminal is fed by a route that has nothing to do with the subscription being inherited; a pass here is not evidence that the inheritance worked, and the runner says so rather than counting it.

Reading a negative result needs one more thing, because "the forwarding dropped it" and "the child never ran" look identical from the tool and want opposite repairs. ``spawn_iof --markers <dir>`` therefore has every process drop a file of its identity on the node it is running on, by a channel with nothing to do with IOF; the runner gathers those from all ten nodes and prints the layout under every case. That is how the failing case above can say *where* the child ran.

The third case is built as well, as case 5, driving ``examples/iof_watcher.c`` - a tool that attaches to whatever server is local to the node it is started on, and so lands on a non-HNP daemon when started anywhere but node1. It is the only case that depends on a tool's ``iof_pull`` being recorded against a job at all, and it therefore fails against a PRRTE without item 2. Its ordering is load-bearing: the watcher has to be subscribed before the child exists, or a pass would only show cached output being replayed, so ``spawn_iof --delay`` holds the spawn open and both asserted lines are written after the subscription.
