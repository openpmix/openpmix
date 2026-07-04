Process & Resource Statistics (Monitoring)
==========================================

This document describes how PMIx answers a request for
resource-utilization statistics — per-process CPU/memory usage, node
load and memory, disk counters, and network counters — from the
``PMIx_Process_monitor`` family of APIs down through the MCA ``pstat``
(Process Statistics) framework and its components. It also covers
the *periodic* monitoring mode, in which the server keeps sampling on a
timer and streams each sample back to the requestor as a PMIx event.

For a code-oriented orientation aimed at contributors working *inside*
the framework, each ``pstat`` directory also carries an ``AGENTS.md``
(with a ``CLAUDE.md`` symlink): ``src/mca/pstat/AGENTS.md`` for the
framework as a whole, plus one each in ``plinux/`` and ``test/``. This
document explains how the pieces fit into the wider library; those explain
each piece's internals in detail.


The Problem
-----------

A resource manager, a tool such as an activity monitor, or an application
runtime often needs to know how much CPU, memory, disk, or network a set
of processes — or a whole node — is consuming. In an HPC system those
processes are spread across many nodes, and only the PMIx *server* on each
node can actually read that node's operating system. A client or tool
cannot read ``/proc`` for a process on some other node.

PMIx therefore exposes a single generic **monitoring** API and routes each
request to wherever the data lives:

* a **local** portion — processes or a node the receiving server owns — is
  satisfied by reading the local OS; and
* a **remote** portion — anything on other nodes — is handed up to the
  host resource manager, which relays it to the relevant servers.

The ``pstat`` framework owns the *local* half: reading the local operating
system and packaging the numbers as PMIx attributes. The routing decision
itself lives just above it, in ``src/common/pmix_monitor.c``.


Public API
----------

Three functions are exported from ``include/pmix.h``::

    pmix_status_t PMIx_Process_monitor(const pmix_info_t *monitor,
                                       pmix_status_t error,
                                       const pmix_info_t directives[],
                                       size_t ndirs,
                                       pmix_info_t **results,
                                       size_t *nresults);

    pmix_status_t PMIx_Process_monitor_nb(const pmix_info_t *monitor,
                                          pmix_status_t error,
                                          const pmix_info_t directives[],
                                          size_t ndirs,
                                          pmix_info_cbfunc_t cbfunc,
                                          void *cbdata);

    void PMIx_Heartbeat(void);

``PMIx_Process_monitor`` is a thin blocking wrapper: it constructs a
``pmix_cb_t``, calls ``PMIx_Process_monitor_nb``, and sleeps on the
condition variable inside the callback object until the non-blocking path
completes. Both are implemented in ``src/common/pmix_monitor.c``.

The single ``monitor`` argument is the request. Its **key** names *what
kind* of statistics are wanted; its **value** is a ``PMIX_DATA_ARRAY`` of
``pmix_info_t`` naming the *specific fields*. The ``directives`` array
qualifies the request — which processes/nodes to target, whether to sample
once or periodically, an ID handle, and so on. As with every PMIx API, new
capability is added by defining new attributes rather than new entry
points.

``PMIx_Heartbeat`` and the ``PMIX_SEND_HEARTBEAT`` monitor key are a
distinct, lighter-weight use of the same API used for liveness detection;
they are handled entirely in ``pmix_monitor.c`` and never reach the
``pstat`` framework, so they are not discussed further here.


Request Attributes
-------------------

The monitor **keys** (the ``monitor->key``) that ``pstat`` understands:

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Monitor key
     - Meaning
   * - ``PMIX_MONITOR_PROC_RESOURCE_USAGE``
     - per-process statistics
   * - ``PMIX_MONITOR_NODE_RESOURCE_USAGE``
     - node load / memory (may nest network + disk)
   * - ``PMIX_MONITOR_DISK_RESOURCE_USAGE``
     - disk read/write/io counters
   * - ``PMIX_MONITOR_NET_RESOURCE_USAGE``
     - network byte/packet/error counters
   * - ``PMIX_MONITOR_CANCEL``
     - cancel a previously-started periodic monitor (value = its ID)

The **directives** that shape a request:

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Directive
     - Effect
   * - ``PMIX_MONITOR_ID``
     - caller-supplied string handle; required to later cancel a periodic monitor
   * - ``PMIX_MONITOR_RESOURCE_RATE``
     - sample **periodically** every N seconds (uint32) instead of once
   * - ``PMIX_MONITOR_TARGET_PROCS``
     - array of ``pmix_proc_t`` restricting which processes to sample
   * - ``PMIX_MONITOR_TARGET_PIDS``
     - array of ``pmix_node_pid_t`` (node + pid) restricting the sample
   * - ``PMIX_MONITOR_TARGET_NODES`` / ``PMIX_MONITOR_TARGET_NODEIDS``
     - restrict to named nodes / node IDs (used for scope resolution)
   * - ``PMIX_MONITOR_LOCAL_ONLY``
     - do not forward any part of the request to the host RM

The **results** come back under one attribute per category, each a
``PMIX_DATA_ARRAY`` of the individual measurements:
``PMIX_PROC_RESOURCE_USAGE``, ``PMIX_NODE_RESOURCE_USAGE``,
``PMIX_DISK_RESOURCE_USAGE``, and ``PMIX_NETWORK_RESOURCE_USAGE``. Each
inner array is tagged with a sample time (``PMIX_PROC_SAMPLE_TIME``,
``PMIX_NODE_SAMPLE_TIME``, and so on). Because every component emits the
same attributes, a caller parses the answer identically regardless of
which component (or node) produced it.


From API to Framework
---------------------

The path from the public call to the framework is:

#. **Client / tool side.** A non-server process cannot read the OS, so
   ``PMIx_Process_monitor_nb`` packs the ``monitor``, error code, and
   directives into a ``PMIX_MONITOR_CMD`` message and sends it to its
   server. The reply is unpacked and handed to the caller's callback.

#. **Server side, entry.** A server calling the API locally, or receiving
   the command from a client (``pmix_server_monitor`` in
   ``src/server/pmix_server_ops.c``), builds a ``pmix_cb_t`` carrying the
   monitor, directives, and requestor identity, and thread-shifts it onto
   the progress thread via ``PMIX_THREADSHIFT(cb, pmix_monitor_processing)``.

#. **Scope resolution.** ``pmix_monitor_processing`` walks the target
   directives to classify the request as *local*, *remote*, or *both*, by
   comparing the requested procs/nodes/pids against
   ``pmix_server_globals.clients`` and the local hostname/nodeid:

   * purely local — call ``pmix_pstat.query(...)`` and return its results
     directly;
   * purely remote — call ``pmix_host_server.monitor(...)`` (the host RM);
     if the host provides no monitor entry point, return
     ``PMIX_ERR_NOT_SUPPORTED``;
   * both — call ``pmix_pstat.query`` for the local contribution, then
     pass the request up to the host and *combine* the local results with
     whatever the host returns (see ``hostcb`` / ``hostprocess``).

#. **Into ``pstat``.** ``pmix_pstat.query`` is the selected component's
   ``query`` function — the boundary into the framework. It is synchronous
   and already on the progress thread, so it may read shared server state
   directly.

The framework itself is opened and a component selected during **server**
startup, in ``pmix_server_init`` (``src/server/pmix_server.c``), right
after the ``pgpu`` framework and before the listener starts.


Inside the Framework
--------------------

``pstat`` is a **single-select** framework: exactly one component runs.

.. list-table::
   :header-rows: 1
   :widths: 20 15 65

   * - Component
     - Priority
     - Role
   * - ``plinux``
     - 80
     - Reads real statistics from the Linux ``/proc`` filesystem. Built
       only on Linux with a readable ``/proc`` and a defined ``HZ``.
   * - ``test``
     - 20
     - Returns fixed, canned values with no OS access. Always built; the
       fallback on non-Linux hosts and the vehicle for CI.

If no component can run, the base leaves an *unsupported* module in place
whose ``query`` returns ``PMIX_ERR_NOT_SUPPORTED``, so the monitor API
degrades cleanly and reports the capability as absent rather than
crashing.

Selection (``pstat_base_select.c``) is a textbook
``pmix_mca_base_select``: pick the highest-priority runnable component,
cache its module in the global ``pmix_pstat``, and call its ``init``.


The request object and its two collection modes
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Each request becomes a ``pmix_pstat_op_t`` (defined in
``src/mca/pstat/base/base.h``) that records the target peers, the
selected fields (four all-``bool`` "which fields?" structs —
``pmix_procstats_t``, ``pmix_ndstats_t``, ``pmix_netstats_t``,
``pmix_dkstats_t``), the optional device-ID filters, and — for a periodic
monitor — a libevent timer and interval.

A component's ``query`` fills the op (using the base parse helpers
``pmix_pstat_parse_procstats`` / ``_ndstats`` / ``_netstats`` /
``_dkstats`` to translate the requested-field array into the bool
structs), selects the target peers with
``PMIX_PSTAT_APPEND_PEER_UNIQUE``, and then drives a single collection
function, ``update()``, which runs in one of two modes distinguished by
whether ``op->cb`` is set:

* **Synchronous (``op->cb != NULL``).** ``query`` points ``op->cb`` at a
  stack callback object holding an info-list builder, calls ``update()``
  directly, and converts the accumulated list into the ``*results`` array
  returned to the caller. This produces the immediate answer for a
  one-shot request — and the *first* sample of a periodic one.

* **Periodic (``op->cb == NULL``).** When a rate was given, ``query``
  appends the op to ``pmix_pstat_base.ops`` and arms the timer
  (``PMIX_PSTAT_OP_START``). Each time the timer fires, the *same*
  ``update()`` runs with ``op->cb == NULL``: it builds a fresh result,
  and instead of returning it, delivers it asynchronously with
  ``PMIx_Notify_event(op->eventcode, ...)`` targeted at the requestor,
  then re-arms the timer.

A one-shot op is released immediately after the synchronous pass; a
periodic op lives on ``pmix_pstat_base.ops`` until a
``PMIX_MONITOR_CANCEL`` naming its ``PMIX_MONITOR_ID`` removes and
releases it (which also deletes the timer).

By default the periodic timer runs on the library's main progress thread.
Setting the framework MCA parameter ``pstat_base_use_separate_thread``
moves sampling onto a dedicated ``"PSTAT"`` progress thread so it cannot
perturb the main thread.


Where the numbers come from
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The ``plinux`` component reads the kernel's ``/proc`` filesystem:

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - Category
     - Sources
   * - per-process
     - ``/proc/<pid>/stat`` (cmd, state, CPU time via ``HZ``, priority,
       thread count, processor), ``/proc/<pid>/status``
       (VmPeak/VmSize/VmRSS), ``/proc/<pid>/smaps`` (summed Pss)
   * - node
     - ``/proc/loadavg`` (load averages), ``/proc/meminfo`` (memory/swap)
   * - disk
     - ``/proc/diskstats`` (``sd*`` devices)
   * - network
     - ``/proc/net/dev`` (per-interface counters)

Only the fields the caller requested are read and emitted. Values that
``/proc`` reports in kB are normalized to MB. A ``PMIX_MONITOR_NODE_...``
request can nest network and disk sub-arrays inside the returned node data
array, so one call can retrieve a full node picture.

The ``test`` component mirrors ``plinux``'s control flow exactly but
substitutes fixed constants for the OS reads (two fabricated disks, three
fabricated interfaces, canned per-process and node figures), which makes
it both a portable fallback and a deterministic test double. To force it
during development::

    export PMIX_MCA_pstat=test


Threading
---------

Everything in ``pstat`` runs on a PMIx progress thread. ``query`` is
reached only through the thread-shift performed by the monitor API, so a
component may touch ``pmix_server_globals.clients`` and other shared
server state directly. Periodic ``update()`` calls fire from the same
event base (the main progress thread, or the dedicated ``"PSTAT"`` thread
if ``pstat_base_use_separate_thread`` is set). Result arrays are assembled
with the ``PMIx_Info_list_*`` builder helpers, and periodic samples are
delivered with the standard ``PMIx_Notify_event`` mechanism.


Summary
-------

* ``PMIx_Process_monitor`` / ``_nb`` are the public entry points;
  ``PMIx_Heartbeat`` is a related liveness call handled outside the
  framework.
* ``src/common/pmix_monitor.c`` splits each request into a local part
  (handled by ``pstat``) and a remote part (handed to the host RM).
* ``pstat`` is single-select: ``plinux`` reads ``/proc`` on Linux;
  ``test`` returns canned values everywhere else; an *unsupported* stub
  covers "no component."
* A ``pmix_pstat_op_t`` drives both one-shot collection (synchronous,
  results returned) and periodic monitoring (timer-driven, results pushed
  as events), distinguished by the ``op->cb`` field.
* Results are returned as ``PMIX_*_RESOURCE_USAGE`` data arrays, uniform
  across components and nodes.
