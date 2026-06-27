Overview
========

When a process (or tool) asks a scheduler/WLM for resources via
``PMIx_Allocation_request``, the resulting allocation is *reserved* to
the requesting namespace for the life of that namespace. What happens
to the allocation when the owning namespace *terminates* is governed by
its inheritance rule.

Historically there was only one behavior: the allocation was released
back to the scheduler when the owning namespace exited. That is the
correct behavior for the common case of a single job that allocates
resources, runs to completion, and exits, and it remains available as
``PMIX_ALLOC_INHERIT_NONE``. It is, however, too restrictive for a
growing number of workflows:

* A *session leader* (e.g., a workflow orchestrator or a long-running
  tool) may acquire an allocation and then spawn a series of child
  jobs that come and go within that allocation. The allocation must
  outlive any individual child.

* A parent job may spawn child jobs that, in turn, spawn further
  ("derived") children. The allocation should survive until the entire
  tree of descendants has drained.

* A job may wish to hand its resources back into the general pool of
  its session when it exits - making them available to other members
  of the session - rather than returning them all the way to the
  scheduler.

*Allocation inheritance* is the mechanism by which the requestor
declares, at allocation time, what is to happen to the allocation when
the owning namespace terminates. It answers two coupled questions:

#. **Lifetime**: when is the allocation actually torn down - at
   termination of the owning namespace, or only after some set of
   descendant namespaces has also terminated?

#. **Disposition**: when the allocation *is* torn down, do the
   resources return to the scheduler, or do they merely become
   *unreserved* and remain available within the owning session?

This document specifies the data type, attribute, and semantics that
implement allocation inheritance, and describes the behavior a
conforming host environment is expected to provide.


Terminology
-----------

The terms *session*, *job*, *namespace*, *application*, and
*scheduler/WLM* are used here exactly as defined in Chapter 2 of the
PMIx Standard and in the project ``CLAUDE.md`` terminology table. In
addition, this document uses:

owning namespace
    The namespace to which an allocation is reserved. By default this
    is the namespace of the process that issued the
    ``PMIx_Allocation_request``, but it may be redirected with
    ``PMIX_ALLOC_TARGET``.

child namespace
    A namespace spawned (directly or transitively) by a process within
    the owning namespace. A *derived child* is any namespace reachable
    by following the spawn relationship to arbitrary depth - i.e., a
    descendant at any level, not just an immediate child.

reserved vs. unreserved
    A *reserved* resource may be used only by members of the namespace
    to which it is reserved. An *unreserved* resource remains part of
    the owning **session** (it has not been returned to the scheduler)
    but is generally available to any member of that session. See
    ``PMIX_ALLOC_SHARE`` below.


The data type
-------------

Inheritance is expressed with the ``pmix_alloc_inheritance_t`` data
type (an 8-bit unsigned integer), introduced as wire-format data type
``PMIX_ALLOC_INHERIT`` (numeric value 75). Four values are defined:

.. list-table::
   :header-rows: 1
   :widths: 28 12 60

   * - Value
     - Lifetime
     - Meaning
   * - ``PMIX_ALLOC_INHERIT_NONE``
     - owning nspace
     - No one inherits the allocation. When the owning namespace
       terminates, the allocation is released back to the scheduler.
       This is the legacy behavior.
   * - ``PMIX_ALLOC_INHERIT_CHILD``
     - last derived child
     - The allocation remains alive until **all** child namespaces -
       including derived (transitive) children - have terminated. When
       the last descendant exits, the allocation is released back to
       the scheduler.
   * - ``PMIX_ALLOC_INHERIT_DEFAULT``
     - owning nspace
     - When the owning namespace terminates, the allocation is **not**
       released to the scheduler; instead it becomes *unreserved* and
       remains available within the owning session.
   * - ``PMIX_ALLOC_INHERIT_CHILD_DEFAULT``
     - last derived child
     - The allocation remains alive until the last derived child
       namespace terminates, at which point it becomes *unreserved*
       (as in ``DEFAULT``) rather than being released to the scheduler.

The two axes are orthogonal and combine as follows:

.. list-table::
   :header-rows: 1
   :widths: 34 33 33

   * -
     - **Released to scheduler**
     - **Becomes unreserved in session**
   * - **At owning-nspace termination**
     - ``PMIX_ALLOC_INHERIT_NONE``
     - ``PMIX_ALLOC_INHERIT_DEFAULT``
   * - **At last-derived-child termination**
     - ``PMIX_ALLOC_INHERIT_CHILD``
     - ``PMIX_ALLOC_INHERIT_CHILD_DEFAULT``

A value of ``PMIX_ALLOC_INHERIT_NONE`` is therefore exactly the legacy
behavior. Note, however, that it is *not* the default: when no
inheritance is specified, the host assumes
``PMIX_ALLOC_INHERIT_DEFAULT`` (see below).

The value is carried in the ``pmix_value_t`` union member named
``inheritance``:

.. code-block:: c

   typedef struct pmix_value {
       pmix_data_type_t type;
       union {
           ...
           pmix_alloc_inheritance_t inheritance;
           ...
       } data;
   } pmix_value_t;


The attribute
-------------

Inheritance is requested by passing the ``PMIX_ALLOC_INHERITANCE``
attribute in the ``pmix_info_t`` array of a ``PMIx_Allocation_request``
(or its non-blocking form ``PMIx_Allocation_request_nb``):

.. code-block:: c

   #define PMIX_ALLOC_INHERITANCE  "pmix.alloc.inhrt"
   // (pmix_alloc_inheritance_t) inheritance rules to be applied to
   // the allocated resources

* **Direction**: IN (from the requestor to the host environment).
* **Accepting APIs**: ``PMIx_Allocation_request`` and
  ``PMIx_Allocation_request_nb``, on ``PMIX_ALLOC_NEW`` and
  ``PMIX_ALLOC_EXTEND`` requests. It is ignored on
  ``PMIX_ALLOC_RELEASE``, ``PMIX_ALLOC_REAQUIRE``, and
  ``PMIX_ALLOC_REQ_CANCEL`` requests.
* **Default**: if the attribute is absent, the host shall behave as if
  ``PMIX_ALLOC_INHERIT_DEFAULT`` had been specified - i.e., on
  termination of the owning namespace the allocation becomes unreserved
  and remains available within the owning session rather than being
  released back to the scheduler. A requestor that wants the legacy
  "release to scheduler on termination" behavior must explicitly pass
  ``PMIX_ALLOC_INHERIT_NONE``.

Example:

.. code-block:: c

   pmix_info_t info[2];
   pmix_status_t rc;

   PMIX_INFO_LOAD(&info[0], PMIX_ALLOC_NUM_NODES,
                  &(uint64_t){16}, PMIX_UINT64);
   PMIX_INFO_LOAD(&info[1], PMIX_ALLOC_INHERITANCE,
                  &(pmix_alloc_inheritance_t){PMIX_ALLOC_INHERIT_CHILD_DEFAULT},
                  PMIX_ALLOC_INHERIT);

   rc = PMIx_Allocation_request(PMIX_ALLOC_NEW, info, 2);


Relationship to other allocation attributes
-------------------------------------------

Inheritance interacts closely with three other allocation attributes;
understanding the division of responsibility is important.

``PMIX_ALLOC_SHARE``
    ``(bool)`` Governs the *initial* reservation state of the
    allocation. ``false`` (the default) reserves the resources for use
    only by members of the requestor's namespace; ``true`` makes them
    generally available within the requestor's session from the
    outset. ``PMIX_ALLOC_SHARE`` describes the allocation *while the
    owning namespace lives*; inheritance describes what happens *when
    it dies*. An allocation may be reserved during its life and become
    unreserved on termination (e.g.,
    ``PMIX_ALLOC_INHERIT_DEFAULT``).

    (``PMIX_ALLOC_SHARE`` replaces the former ``PMIX_ALLOC_RESERVED``
    attribute with inverted sense; see "Backward compatibility"
    below.)

``PMIX_ALLOC_TARGET``
    ``(char*)`` Names the namespace to which the allocated resources
    are to be reserved. When given, that namespace - not the
    requestor's - is the *owning namespace* for inheritance purposes.

``PMIX_SPAWN_TARGET``
    ``(varies)`` Used on ``PMIx_Spawn`` to map applications onto one or
    more specific existing allocations, identified by their
    ``PMIX_ALLOC_ID`` string(s) (a single ``char*`` or a
    ``pmix_data_array_t`` of ``char*``). This is the mechanism by which
    a child job is launched *into* an inherited allocation rather than
    triggering a fresh allocation request. An invalid/empty nspace
    equates to the "default" allocation.

A typical inheritance workflow ties these together:

#. An orchestrator calls ``PMIx_Allocation_request(PMIX_ALLOC_NEW,
   ...)`` with ``PMIX_ALLOC_INHERIT_CHILD`` (or ``CHILD_DEFAULT``) and
   records the returned ``PMIX_ALLOC_ID``.
#. The orchestrator spawns child jobs with ``PMIX_SPAWN_TARGET`` set to
   that ``PMIX_ALLOC_ID``, so the children run within the inherited
   allocation.
#. Children may themselves spawn derived children the same way.
#. The orchestrator exits. Because the inheritance rule is
   ``CHILD``-flavored, the allocation persists.
#. When the last derived child terminates, the host releases the
   allocation (``CHILD``) or marks it unreserved within the session
   (``CHILD_DEFAULT``).


Host environment responsibilities
---------------------------------

A conforming host environment (RM/scheduler hosting the PMIx server)
that advertises support for allocation inheritance shall:

#. **Record** the inheritance value associated with each allocation at
   the time the allocation is granted, keyed to the owning namespace
   (as possibly redirected by ``PMIX_ALLOC_TARGET``).

#. **Track descendants** for the ``CHILD`` and ``CHILD_DEFAULT`` cases.
   The host must maintain the spawn relationship deeply enough to know
   when the *last* derived child has terminated, not merely the
   immediate children. Children created via ``PMIX_SPAWN_TARGET`` into
   the allocation count as descendants for this purpose.

#. **Defer teardown** of the allocation past termination of the owning
   namespace whenever a ``CHILD``-flavored rule is in force and live
   descendants remain.

#. **Choose disposition** correctly at teardown time:

   * ``NONE`` / ``CHILD``: return the resources to the scheduler's
     general pool.
   * ``DEFAULT`` / ``CHILD_DEFAULT``: retain the resources within the
     owning **session** but clear their reservation, so any member of
     the session may use them. The resources are *not* returned to the
     scheduler until the session itself terminates (or they are
     explicitly released).

#. **Default correctly**: in the absence of
   ``PMIX_ALLOC_INHERITANCE``, behave as
   ``PMIX_ALLOC_INHERIT_DEFAULT`` - the allocation becomes unreserved
   within the owning session on termination of the owning namespace.

A host that does not support inheritance should reject a request
carrying a non-default ``PMIX_ALLOC_INHERITANCE`` value with an
appropriate error rather than silently ignoring it, so the requestor is
not misled about the lifetime of its allocation.

.. note:: Inheritance values do not, by themselves, grant resources to
   a session. ``DEFAULT``/``CHILD_DEFAULT`` make resources *unreserved*
   within the owning session - they remain charged to / part of that
   session. Returning resources to the scheduler is a distinct action
   that occurs for ``NONE``/``CHILD`` at teardown, or when the session
   ends.


Library support
---------------

The PMIx library provides full ``bfrops`` serialization support for the
new type - pack, unpack, copy (including the standard-copy sizing and
the TMA allocator copy path), compare, and print - in the base
functions, with the type registered in the most recent wire-format
component (``v61``). Older ``bfrops`` components are intentionally left
unchanged so that the wire format of prior versions is preserved for
interoperability; a v61 peer is required to exchange the
``PMIX_ALLOC_INHERIT`` type.

A string converter is provided for diagnostics and logging:

.. code-block:: c

   PMIX_EXPORT const char*
   PMIx_Alloc_inheritance_string(pmix_alloc_inheritance_t inheritance);

It returns the trailing portion of each value name following
``INHERIT_`` - i.e., ``"NONE"``, ``"CHILD"``, ``"DEFAULT"``, or
``"CHILD_DEFAULT"`` - and ``"UNSPECIFIED"`` for any unrecognized value.

The dictionary generator (``contrib/construct_dictionary.py``) maps the
``(pmix_alloc_inheritance_t)`` annotation on ``PMIX_ALLOC_INHERITANCE``
to the ``PMIX_ALLOC_INHERIT`` data type so the attribute harvests
cleanly into the generated attribute dictionary.

Unit coverage lives in ``test/unit/bfrops_alloc_inherit.c`` (wired into
``make check``), exercising pack/unpack of single and multiple values,
round-tripping through a ``pmix_value_t``, value transfer, compare,
print, and the string converter.


Backward compatibility
-----------------------

Allocation inheritance is purely additive at the API level: it
introduces a new data type, a new attribute, and a new string
converter. No existing API signature, struct layout, or wire format of
a prior ``bfrops`` version is altered, so binaries built against older
PMIx releases continue to interoperate. Older peers simply never send
or request the ``PMIX_ALLOC_INHERIT`` type, and a host that predates
the feature treats the unknown attribute per the usual unrecognized-
attribute rules.

One related change rides on the same branch and is *not* additive in
the same sense: the former ``PMIX_ALLOC_RESERVED`` boolean was replaced
by ``PMIX_ALLOC_SHARE`` with inverted polarity (``reserved == true`` is
the default; ``share == true`` opts *out* of reservation). Consumers
that used ``PMIX_ALLOC_RESERVED`` must migrate to ``PMIX_ALLOC_SHARE``
and invert their sense accordingly.
