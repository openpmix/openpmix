Overview
========

PMIx provides a portable, RM-agnostic mechanism by which tools,
applications, and runtime environments (RTEs) can interact with the
system scheduler (also referred to as the Workload Manager, or WLM).
The integration is built from the same primitives used throughout
PMIx - a small number of generic APIs whose behavior is modulated by
attributes, plus an event-notification channel for asynchronous
alerts.

General layout
--------------

Two communication paths are supported:

* **Direct.** A tool or application can, where the environment permits,
  open a connection directly to the scheduler and exchange requests and
  responses with it.

* **Relayed (more common).** A tool or application communicates its
  request to its local RTE, which already holds a connection to the
  scheduler. The RTE then acts as a relay between the requestor and the
  scheduler, forwarding the request and returning the scheduler's
  response. This is the typical arrangement because most processes are
  not granted - and do not want the burden of - a direct scheduler
  connection.

In addition, the RTE itself uses its connection to the scheduler to
coordinate their mutual interactions. For example, the scheduler may
use PMIx to inform the RTE of a new session it should instantiate;
likewise, the RTE can use PMIx to inform the scheduler that a session
has terminated and thus all of its resources are available for reuse.

While the scheduler-integration support can be used for the more
common operations (e.g., submitting allocation requests, RTE-scheduler
coordination), the primary intent of the support is to enable
**dynamic environments** - i.e., the ability for an application to
request on-the-fly modifications of its session.

Support is provided via a few APIs, attributes, and events.

APIs
----

The principal entry points used to interact with the scheduler are:

* ``PMIx_Allocation_request`` (and its non-blocking form
  ``PMIx_Allocation_request_nb``) - request a change to an allocation.
  The accompanying ``pmix_alloc_directive_t`` selects the operation:

  ============================  ==================================================
  Directive                     Meaning
  ============================  ==================================================
  ``PMIX_ALLOC_NEW``            Request a new, disjoint allocation.
  ``PMIX_ALLOC_EXTEND``         Extend an existing allocation in time and/or
                                resources.
  ``PMIX_ALLOC_RELEASE``        Release part or all of an existing allocation,
                                optionally "lending" the resources back for a
                                period of time.
  ``PMIX_ALLOC_REAQUIRE``       Reacquire resources previously lent to the
                                scheduler.
  ``PMIX_ALLOC_REQ_CANCEL``     Cancel a pending allocation request.
  ============================  ==================================================

* ``PMIx_Session_control`` - request a control action against a session
  (identified by its ``sessionID``; ``UINT32_MAX`` denotes all sessions
  under the caller's control).

As with every PMIx API, these calls accept an array of ``pmix_info_t``
attributes so that new behavior can be added over time without altering
the signatures.

Identifying an allocation
-------------------------

Two identifiers are associated with an allocation, and either or both
may appear in requests, queries, and event payloads:

* ``PMIX_ALLOC_REQ_ID`` (``char*``) - a **user-provided** string label
  for an allocation request, supplied by the requestor so that it can
  later reference or query the request.

* ``PMIX_ALLOC_ID`` (``char*``) - a **scheduler-provided** string
  identifier for the resulting allocation, used to reference the
  allocated resources in subsequent operations (e.g., a later
  ``PMIx_Spawn``).

Both forms are carried so that a process can correlate a scheduler
notification with the request it originally issued, regardless of which
identifier it happens to hold.

Allocation timeout warning
---------------------------

Allocations are frequently granted for a bounded period of time. When
the period expires, the scheduler reclaims the resources - typically
terminating any jobs still running within the allocation. Applications
that wish to checkpoint, drain work, or request an extension before
that happens need advance notice.

PMIx supports this through a request attribute and a corresponding
event.

Requesting a warning
^^^^^^^^^^^^^^^^^^^^^^

``PMIX_ALLOC_WARN_TIMEOUT`` (``uint32_t``) is passed in the
``pmix_info_t`` array of an allocation request (for example, alongside
``PMIX_ALLOC_NEW`` or ``PMIX_ALLOC_EXTEND``). Its value is the number of
**seconds** prior to expiration at which the requestor wishes to be
warned - matching the units of the ``PMIX_TIME_REMAINING`` value carried
in the warning event. By including it, the requestor asks the scheduler
to deliver an advance warning of the impending timeout of the allocation
that many seconds ahead of the actual expiration.

Honoring the request is up to the scheduler: like all attributes, it is
advisory, and a scheduler that does not implement the capability will
simply ignore it.

Receiving the warning
^^^^^^^^^^^^^^^^^^^^^^^

When the warning threshold is reached, the scheduler issues the
``PMIX_ALLOC_TIMEOUT_WARNING`` event. The event is **targeted at the
process that requested the allocation** - it is not a general
broadcast - so the requestor registers an event handler for
``PMIX_ALLOC_TIMEOUT_WARNING`` to receive it.

The accompanying ``pmix_info_t`` payload must include enough
information for the recipient to identify which allocation is about to
expire and how long it has left:

============================  ==========================  ==============================================
Attribute                     Type                        Meaning
============================  ==========================  ==============================================
``PMIX_ALLOC_ID``             ``char*``                   Scheduler-assigned ID of the affected
                                                          allocation.
``PMIX_ALLOC_REQ_ID``         ``char*``                   User-provided request ID, included whenever
                                                          one was given on the original request.
``PMIX_TIME_REMAINING``       ``uint32_t``                Number of seconds remaining before the
                                                          allocation expires.
============================  ==========================  ==============================================

The scheduler includes ``PMIX_ALLOC_ID`` in all cases. It also includes
``PMIX_ALLOC_REQ_ID`` whenever the requestor supplied one, so that the
recipient can match the warning against the request it issued using
either identifier. ``PMIX_TIME_REMAINING`` reports the time left at the
moment the warning is generated, expressed in seconds - the same units
used for the ``PMIX_ALLOC_WARN_TIMEOUT`` request threshold.

The targeting of the event at the original requestor is expressed in the
usual way for directed PMIx events - e.g., by restricting the
notification range to the requesting process. A recipient handler
returns ``PMIX_EVENT_ACTION_COMPLETE`` (or the appropriate status) once
it has reacted, for instance by issuing a ``PMIX_ALLOC_EXTEND`` request
to lengthen the allocation, or by beginning an orderly shutdown.
