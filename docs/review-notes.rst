Deep review notes
=================

This page is the *record* of the deep review of the PMIx source tree:
entries that have been closed, the dated log of what each pass found and
what it retired, the directories the review has already covered, and the
things that look like bugs and are not.

**Nothing on this page is open work.**  What still needs to be addressed
is on :doc:`todo`, and only there.

The record is kept for one reason: it stops the same ground being
re-covered.  A closed entry that is deleted gets rediscovered, argued
again from the same plausible-but-wrong reading, and sometimes "fixed"
back into a defect.  Read a section here before writing a new entry
about the same code.

Closed entries
--------------

Entries that stood on :doc:`todo` and have since been answered.  Each is
kept with the reasoning that closed it, because in most cases the way it
closed contradicted what the entry predicted.

Who owns a credential the host hands up
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

``pmix_credential_cbfunc_t`` is documented in ``include/pmix_common.h.in``
as transferring ownership of the credential to the receiving function —
"responsibility for releasing the memory lies outside the PMIx library."
This entry used to read that sentence as also governing the server-side
up-call, where the receiving function is ``pmix_server_cred_cbfunc``, and
asked whether the host's ``pmix_byte_object_t`` was therefore ours to
free.  **That is closed**, and it closed on the shape of the call rather
than on the Standard's word.

The host hands its data down and is given no callback telling it when the
library is finished; the library, for its part, cannot tell whether what
it was handed was ever ``malloc``'d.  Ownership stays with the host, and
the only contract binding the library is the narrower one: be done with
the data by the time the up-call returns, so the host may dispose of it
on the next line.  That is what
:ref:`pmix_server_module_t(5) <man5-pmix_server_module_t>` already told
hosts — data returned through a completion callback is the host's to
release once the callback returns — read from the library's side.

Being *done with it* never required copying it.  Both
``pmix_server_cred_cbfunc`` and ``pmix_server_validate_cbfunc`` now pack
the whole reply where they stand, on whatever thread the host called
them from, and thread-shift only the finished buffer; packing writes into
a buffer of our own and reads the peer's ``bfrops`` module, neither of
which is progress-thread state.  ``_queue_sec_reply`` does the queueing,
which is the part that genuinely has to be on the progress thread, since
``PMIX_SERVER_QUEUE_REPLY`` writes ``peer->send_msg``, the peer's send
queue, and the send event.  The credential copy, the info-array copy, and
the ``infocopy`` bookkeeping that went with them are gone.  The
regression case in ``test/unit/server_control.c`` is unchanged and still
passes: its host stub destructs and overwrites both the credential and
the info array the instant the callback returns, which a reply packed
after the return cannot survive.

The client side is closed too, and it is where the sentence in the header
was aimed all along — but it says less than it appears to.  What
transfers is the *credential*: the payload the byte object points at, and
its size.  The byte object carrying it does not transfer; it stays the
library's, and may be a stack frame.  So the receiver takes the pointer
out of it and releases that when done, and the library's obligation is
the negative one — do not destruct the object once the callback has been
given it, because the payload is no longer ours.  ``getcbfunc()`` in
``src/common/pmix_security.c`` destructed exactly there, freeing the
payload out from under the application: a receiver that kept it read
freed memory, and one that released it, as the header requires, double
freed.  The two arms that answer from a local ``psec`` mechanism did the
same to the payload the mechanism had just allocated.  Each of them now
destructs only on the arm where there was no ``cbfunc`` to hand the
payload to.

One asymmetry had to be dealt with rather than documented away.  The same
signature runs in both directions, so a server whose host implements
``get_credential`` used to have the host's borrowed payload passed
straight through to the caller of ``PMIx_Get_credential_nb``, who is told
it owns what it receives.  ``cred_relay()`` now interposes on that path
and copies, so the caller answers to one contract rather than two.  With
the transfer uniform, ``mycdcb()`` takes the payload instead of copying
it and ``PMIx_Get_credential`` hands that same allocation to its caller,
retiring both of the copies the blocking form used to make.

Covered by ``test/unit/common_api.c``, which reads the credential after
the callback that delivered it has returned and then releases it — a
use-after-free and a double free, respectively, against the old code.

A pruned deregistration is not propagated
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

This entry used to say that
:ref:`PMIx_server_deregister_resources(3) <man3-PMIx_server_deregister_resources>`
could not take information back from a namespace that already held it —
the server's global cache is copied into a namespace's data store once,
when that namespace is first registered, and nothing re-reads it — so a
deregistration governed only the namespaces registered *after* it while a
running job kept its copy.  **That is closed.**  ``PMIx_Put`` gained
delete scopes, ``gds`` gained a ``del_key`` slot, a deregistration takes
the key back from every namespace that already holds it, and the server
tells its local clients so their cached copies go too.

Two things about how it closed are worth keeping, because both contradict
what this entry predicted.

``gds/shmem3`` did **not** need a new segment generation.  The obstacle
was always that a client reads the shared segment directly, so removing
data from one would mean putting a lock on a read path that is lock-free
by design — and the assumed answer was to publish a new generation
carrying a ``PMIX_UNDEF`` tombstone, which there is no way to advertise
outside a fence reply.  The record does not have to be *in* the segment.
Each process keeps its own list on ``job->tombstones``, built from the
notification its server sends, and every read consults it;
``pack_tombstones()`` adds the list to the cached job-info reply so a
client attaching later gets it too.

And the deletion had to be made to cross a node boundary, which is a
separate problem from the one this entry describes.  ``deregister_resources``
does not need it — the host calls that API on every daemon — but a
``PMIx_Put`` deletion happens in one process, and the modex is additive,
so a contribution that merely stops naming a key removes nothing at the
far end.  A removal is therefore *stated* in the next collecting fence, as
an entry whose value is ``PMIX_UNDEF``, and both datastores act on one
that arrives.  ``examples/delete_key.c`` is the case that proves it, and
it only proves anything with one rank per node.

A qualified deregistration is propagated too
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

The *qualified* form stood open after that, and closed on 2026-08-30.
A deregistration carrying a data-array qualifier can ask for **part** of
a value to go — this key, on that node, and only these elements of it.
The entry survives, pruned, and every namespace already holding it holds
the *unpruned* one.  Nothing correct could be done about that: pushing a
deletion would take from a namespace more than the host asked to remove.
So the arm did nothing, and the entry named two obstacles — "an update
push which does not exist", and "deciding what a partial update looks
like on the wire and in a shared segment neither datastore can rewrite
in place".

Both were gone by the time it was re-read, and neither was removed with
this case in mind.  The update push is ``add_job_data``, built so that a
resource registered *after* a job could reach it.  The shared-segment
half is the job-data chain: an addition is a new segment at the head, and
a read stops at the first segment carrying the key, so a pruned value
shadows the one it replaces without anything being rewritten.  And the
wire question turned out not to arise — the pruned value is an ordinary
value, so a "partial update" is just a store, which replaces by key.

The propagation is therefore three lines of intent and no new mechanism:
copy the pruned entry and hand it to every namespace.  Worth keeping for
the shape rather than the fix: **an entry can be closed by machinery
built for something else**, and the way to notice is to re-read what it
says it needs rather than what it concludes.  This one concluded
"deliberate rather than missing", which reads as settled; what it
actually recorded was two prerequisites, and both had quietly been met.

``test/unit/gds_datastore`` covers it — register a node-scoped entry with
two elements, launch a job into it, prune one element, and require the
job to see the other survive and the pruned one gone.  Against the
previous commit the job still reports all three.

Smaller items carried forward (closed 2026-08-13)
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

The items that stood under this heading were closed on 2026-08-13.  One
of them did not close all the way and is now an entry of its own:
:ref:`todo-resolve-peers-wildcard`.  The other left a behavior change
worth recording.

* **A malformed** ``PMIX_QUALIFIED_VALUE`` **or a NULL key arriving in a
  cache refresh now fails the enclosing** ``PMIx_Get``.  See the
  ``refcb()`` entry in ``src/client/AGENTS.md`` for why all-or-nothing is
  the only answer an application can act on.  Recorded here because it is
  the one behavior change in that group of fixes.

A shared session's segment has nothing that shares it
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Found in the ``src/mca/gds/shmem3`` re-review (2026-08-21), closed
2026-08-28.  The entry recorded that nothing ever appended to
``pmix_mca_gds_shmem3_component.sessions``, so the list was permanently
empty, both searches in ``pmix_gds_shmem3_get_session_tracker()`` were
unreachable, and a job's session object was created by
``job_construct()`` and died with it — two jobs in one session each built
their own copy of the session data.  What it deferred was "deciding when
a session tracker is registered on the component, and what its lifetime
is once more than one job holds it".

Both are now decided.  A tracker is registered the first time a job names
a real session id, on the server as it builds its segments and on a
client as it reads the session seg blob, and the component's list holds a
reference until the host says the session is over.

The lifetime took two goes, and the wrong one is worth recording.  It was
first made *weak* — the last job out reclaimed the segment — on the
reasoning that a persistent DVM would otherwise accumulate a session
segment per allocation forever.  That is inferring the end of a session
from the end of its last job, and a session with no jobs running in it is
an ordinary state: another job may be about to be launched into it.  It
also silently diverged from ``gds/hash``, whose ``nspace_del`` has always
dropped the job and left the session standing.  The answer was not to
pick a side but to add the signal that was missing —
``PMIx_server_deregister_session`` and a ``del_session`` slot on the gds
module — which is what landed.  Note that the sharing had already taken
most of the accumulation away by itself: the growth is one segment per
*session* rather than one per *job*.

Three things about how it closed are worth keeping.

**The obstacle named in the entry was not the one that mattered.**  The
entry framed the problem around the arena, since a session's segment
placed inside a job's arena would be unmapped by ``job_destruct()``.  But
``job_destruct()`` also released the session's segment *by hand* — SESSION
was one of the ids in its teardown loop — so the first job destructed
unmapped the segment and freed the handle whatever the placement was.  The
arena was a red herring in front of a plain ownership inversion, and the
fix for it (release the segment from ``session_destruct()``) is a no-op
until sharing exists, which is what let it land first and separately.

**A property the code merely happened to have was documented as a
guarantee.**  The session segment really was outside the arena, but not
because anything said so: ``arena_alloc_static()`` was asked for it like
any other segment and refused only because the static region is sized for
exactly one segment and the job's own fills it.  Any later change to the
reservation would have silently put the session inside the arena and
reintroduced the unmap the comment claimed was impossible.  Worth a habit:
when a comment asserts an invariant, check whether the code decides it or
merely produces it today.

**A regression test that passes against the unfixed code is testing the
wrong read.**  The first version of the new case had the surviving job
fetch a job-level key, which passed with the ownership move reverted —
that key is answered out of the job's own segment and never touches the
session's.  Only the whole-job (NULL-key) fetch walks the session list.
With that changed, reverting the ownership move segfaults the test at
exactly that assertion.  ``test/unit/gds_datastore``'s shared-session case
was verified by re-breaking each half separately: without the sharing the
"second job is given the first job's session segment" assertion fails,
and without the ownership move the surviving-job read crashes.

Review log
----------

What each pass found, what it retired, and — where it is worth carrying —
the shape of the mistake it corrected.  In date order.

2026-08-11 through 2026-08-16 — the standing re-check
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

**Every open entry was checked against the tree on 2026-08-11.**  That
pass retired three of them — one whose fix had landed a month before this
page first recorded it as open, one that had been fixed since, and one
that a fresh valgrind run against a launcher could no longer reproduce —
and corrected a claim that was never true.  Treat that as the standing
expectation rather than a one-off: an entry here is a lead, not a finding.
The two "never validated" coverage gaps are the only remaining entries
that cannot be checked by reading the tree, so they are where the next
stale one will be.

Three of the "smaller items carried forward" were closed on 2026-08-13 —
the ``PMIx_Value_get_number`` screen (now in the function itself, where
every caller in the tree gets it), the ``init_called`` clear (now
``pmix_atomic_clear``, the required partner of the
``__atomic_test_and_set`` that sets it), and ``refcb()``'s dropped store
status.

**Checked again on 2026-08-15**, which retired one more: the open decision
about a tool caching an event nobody handled while a client discarded it.
The fan-out fix for `openpmix#4101
<https://github.com/openpmix/openpmix/issues/4101>`_ answered it — a
client now parks such an event, the tool's unreachable copy of the parking
code is gone, and ``test/unit/event_forward`` covers both halves.  Two
entries were corrected rather than retired, both because the code had
moved: the tool's SIGCONT stdin handler now lives in ``src/common``, and
the ``cd->nondefault`` blocks are down to one that still assigns inside
its guard.  Everything else was re-verified against the tree and stands as
written.

A second entry went the same way on 2026-08-16: the open decision about
two concurrent spawns not being formattable differently.  It listed three
unattractive choices and missed a fourth — hold the output until the reply
that can identify it arrives, rather than formatting it on a guess at all.
The entry is retired; see ``test/unit/iof_pending``.  Worth noting *why*
it read as closed, since the shape recurs: it framed the problem as "which
flags does this output get", which really has no answer at that moment,
when the question that does have one is "does this output have to be
formatted yet".

2026-08-18 — the psensor progress thread
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

The ``psensor`` progress-thread entry that stood under
"Deferred work" is closed.  Both halves it called for were made together:
``pmix_psensor_base_open`` now starts the ``"PSENSOR"`` thread it creates,
and ``pmix_psensor_base_close`` pauses that thread before anything is torn
down and stops it only after the components -- which own the trackers, and
therefore the timers armed on the base -- have closed.  Both components'
samplers were moved to ``EV_PERSIST`` timers they no longer re-arm, for
the reason spelled out in ``src/mca/pstat/AGENTS.md``.
``test/unit/run_monitor.pl`` now runs its heartbeat *and* file scenario
twice, once with ``psensor_base_use_separate_thread`` set, so the
configuration that was silently dead is exercised by ``make check``.

A second entry was opened, corrected and closed the same day, and the
shape of the mistake is worth keeping.  The unexpected-message
``show_help`` that ``run_monitor.pl`` printed on every run was first
written down as a heartbeat arriving at a server with no posted recv --
plausible, since ``psensor/heartbeat`` really does post that recv lazily,
and wrong.  Tracing the tag showed the opposite: the server *does* match
the beat, on the wildcard recv the command switchyard serves, and answers
it with the error the switchyard gets for trying to read a command out of
a zero-byte buffer -- and it is the *client* that then cannot place the
reply, because ``PMIx_Heartbeat`` is one-way and nobody posts for tag 1.
Both halves are now fixed: ``pmix_server_message_handler`` drops
``PMIX_PTL_TAG_HEARTBEAT`` instead of dispatching it, and an unmatched
message is a framework trace rather than a ``show_help`` asking the user
to report a bug.  The lesson is the one this page keeps relearning -- an
entry written from a plausible reading of the code, rather than from
watching it run, is a lead and not a finding.

2026-08-20 — a directive parsed and never read
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

The ``src/mca/psensor`` review found one shape repeated
five times, and it is worth naming because it passes every test anyone
would think to write: a directive parsed out of the request, stored in the
tracker, and then never read again.  ``PMIX_MONITOR_HEARTBEAT_DROPS`` set
``ndrops`` and nothing consulted it, so a request asking to tolerate
missed beats alerted on the very first empty window. ``PMIX_MONITOR_ID``
was never parsed at all, so the ``id`` argument to ``stop()`` — the whole
cancel handle — could never match anything.  ``PMIX_MONITOR_CANCEL`` was
recognized by neither component, so it fell through to the resource-usage
path, which answers ``PMIX_SUCCESS`` for an id it has never held: a
client's cancel reported success and the monitor kept firing forever.  The
``error`` argument, which ``PMIx_Process_monitor(3)`` documents as "the
code the monitor is to use", was discarded in favour of a hardcoded alert.
And the ``file`` sampler checked only the first of the three attributes a
request named.

Each of those looked like working code, and the framework's own end-to-end
test passed throughout — because a test that waits for an alert cannot
tell an honored directive from an ignored one.
``test/unit/run_monitor.pl`` now arms a capped monitor and a cancelled one
under a status code of their own and fails if *either* fires, which is the
only shape that distinguishes them.  Each of the three fixes was verified
by re-breaking it and watching the new check catch it.

Two lifetime defects came out of the same pass. ``PMIx_Notify_event``
returns ``PMIX_ERR_NOT_AVAILABLE`` without reaching its callback once the
progress thread has stopped — which ``PMIx_server_finalize`` does well
before it closes this framework — so both samplers stranded a tracker and
the peer it retained.  And ``psensor/heartbeat`` never un-posted the PTL
recv it posts lazily, nor cleared the flag saying it had: ``ptl`` closes
after ``psensor``, so a DSO build left the ptl list naming a function in
an unloaded plugin, and a second ``PMIx_server_init`` in one process would
have found the flag set, the recv gone, and declared every monitored
client dead on its first window.

2026-08-21 — the gds/shmem3 re-review
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

The ``src/mca/gds/shmem3`` re-review — the entry that
stood under "Reviewed, but changed materially since" — is done, and three
of its findings generalize past the component.

``is_tsafe`` **is a claim about the reader's thread, not about the data.**
``shmem3`` sets it, and ``try_local_fetch()`` in
``src/client/pmix_client_get.c`` consults it on every keyed ``PMIx_Get``
— so the module's ``fetch`` runs on the application's own thread.  The justification
recorded in the guide was that a fetch holds a reference on the job
tracker and reads only data that is never written again.  That is true of
what is *in* a segment, and of the job segment, and says nothing about the
process-local bookkeeping a read walks beside it: the modex generation
chain, which each completing fence releases and ``munmap``\ s, and the
tombstone list, which ``del_key()`` appends to.  A ``PMIx_Fence_nb``
concurrent with a ``PMIx_Get`` of a remote rank's key could leave an
application thread inside ``pmix_hash_fetch()`` on a table that had just
been unmapped; the single-threaded case was safe only by accident, because
a blocking fence parks the app thread. Fixed with a per-job mutex.  When
reviewing any ``is_tsafe`` module the question to ask is not "is the data
immutable" but "what process-local state does the read touch, and who else
writes it".

**A doc comment saying a field is "X-side only" has to be checked against
every** *reader*, **not just the writer.** ``job->modex_generation`` was
introduced to name the next backing file, which is a server-side job, and
was documented and treated as server-only.  It is also what dates a
tombstone, and ``del_key()`` runs on the client — where the counter never
advanced, so every tombstone was stamped generation zero and shadowed
every generation that client would ever map: a key deleted and
re-published in a later fence stayed invisible to it.

**A modex is stored through the module of the namespace that contributed
it, not through the server's own.**  A server assigns *itself* ``hash``,
while ``PMIX_GDS_STORE_MODEX`` resolves from a local peer of the
contributing namespace — so a ``shmem3`` job's fence data went into a
shared segment that the lookup at the top of ``pmix_server_get()`` never
searched. Every remote get for such a job missed and was pushed up to the
host as a direct modex for data the server already held; if the owning
process had finalized, nothing answered and the requester waited forever,
because a remote request carries no timeout by design.
``_satisfy_request()`` a few hundred lines below already had the right
idiom — grep for ``local_peer_of_nspace`` before writing a second one.
Both halves were closed: the server now asks the namespace's own module,
and ``_dmodex_req()`` answers ``PMIX_ERR_NOT_FOUND`` for a rank the host
has already reaped rather than deferring the request forever.

One method note, because it decided the diagnosis: **a healthy run's
duration is the yardstick**.  The case that exposed this takes about a
second against a twenty-second limit, so a timeout there is a wedge and
not a slow launch.  Measure the healthy case before calling a timeout
flaky.

2026-08-24 — the src/server per-file pass
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

The per-file pass over ``src/server`` finished — twenty
files, one at a time, after the directory-wide review of 2026-08-09/15 —
and most of what it could not close itself landed here rather than in a
commit: the entry-point initialization sweep below, the ownership of a
credential the host hands up, the cleanup request that is applied in part
before it fails, the server-wide envar hook nothing fills, and the
allocation-failure injection the switchyard's out-of-memory arms need
before any of them can be tested.  The cleanup entry is gone from the list
below as of 2026-08-28: what had been recorded as a question for the
Standard turned out to rest on a reading of the code that did not survive
being checked, and the repair was a stage-then-commit split of the one
handler.

The credential entry closed the same day, and for a related reason: what
had been put to the Standard is answered by the call's own shape — the
host gets no callback saying the library is done, and the library cannot
tell a stack frame from a ``malloc``, so the host keeps its data and the
library must be finished with it by the time it returns.  Being finished
never required copying it; the server now packs the reply before it
returns.  Reading the question that way also exposed the client-side half,
where the library *does* transfer the credential and was freeing it
anyway.

One entry was opened and closed within the same day — the ordering of a
registration's acknowledgement against the cached events replayed behind
it — because the push-back that had deferred it ("libevent ordering is too
strong an assumption") was checkable and wrong: a single progress thread
draining activations in order is a design invariant, not an assumption.

That is the lesson worth carrying, and it is this page's own rule pointed
at a different artifact.  **A recorded push-back is a lead, not a
finding**, exactly as an entry here is.  The switchyard's unchecked
``PMIX_NEW`` in ``PMIX_GDS_CADDY`` and ``PMIX_SERVER_QUEUE_REPLY`` had
been logged in ``AGENTS.md`` as deliberate, on the reasoning that a
NULL-safe macro "only moves the crash to the handler on the next line".
The dispatch arm returns before the handler is called, so the reasoning
was simply false, and one failed allocation was killing the server and
every client it hosted.  When a push-back's stated reason can be checked,
check it before inheriting it.

2026-08-27 — where to fix a class of crash
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

The entry about ``PMIx_server_*`` entry points screening
the wrong initialization flag is retired, and the way it went is worth
keeping.  It had grown into a per-API audit: which roles may call what, a
lookup table, a completeness test to keep the table honest.  All of that
was infrastructure to protect against a program that calls a server API
from a client, which is a user error that has not occurred in production
in the life of the library.

The whole crash surface was one defect in one macro.
``PMIX_LIST_STATIC_INIT`` left the sentinel's ``next`` and ``prev`` NULL,
so a statically initialized list was not an empty list, it was an
unwalkable one — and every ``PMIX_LIST_FOREACH`` and ``pmix_list_append``
in the tree assumes an empty list can be walked.  The macro now takes the
object it initializes and points the sentinel at itself.  With that one
change, calling all fourteen server entry points from a client, a plain
tool and a launcher produced ten, four and zero crashes before, and zero,
zero and zero after.  The second initialization flag those entry points
had been given is gone; there is one ``pmix_globals.initialized`` again,
set at the end of whichever init you called.

**The lesson is about where to fix a class of crash.**  Nine entry points
crashing the same way is one bug in what they all walk, not nine bugs in
nine doorways — and a screen at the doorway costs a maintenance burden
forever while the fix underneath costs nothing after the day it lands.

2026-08-30 — writing the shmem3 document
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

This was not a review pass.  The task was to write
:doc:`how-things-work/shmem3` — the architecture of the shared-memory
datastore, how its data is written and updated, why none of it takes a
lock, and a step-by-step account of ``PMIx_Get``.  It turned up four
defects, which is the first thing worth carrying: **having to state
plainly what code does is a review technique.**  Prose has no ``else``
branch to hide in.  Three of the four were found not by reading for
faults but by trying to write a true sentence and discovering there
wasn't one.

**A test that cannot fail.**  The strongest lesson came from the
verification rather than the code.  ``job_data_update`` was run against
a deliberately broken build to confirm it *could* fail.  It printed
exactly the right diagnosis — "never saw the updated value in its own
store (last read 11, wanted 22)" — and then reported "2 passed, 0
failed" and exited **0**.  The client had exited at its first failed
check, which made the parent's next ``read()`` fail, which jumped to the
cleanup label before the final ``report()``.  With ``npass`` and
``nfail`` both zero, a run that asserted nothing was indistinguishable
from a run that passed.  ``test/unit/session_update`` had carried the
same hole since it was written, so it had been able to pass while
asserting nothing for as long as it had been in ``make check``.

Two rules come out of that, and neither is the obvious one.  The first
is that **the A/B check is the exit status, not the output**: a harness
that prints a complaint and exits zero has told you nothing, and reading
the printed complaint is exactly how it fools you.  The second is that
the fork-and-exec test shape has a structural hazard — the child's
failure arrives as an *I/O error in the parent*, on the path that skips
the reporting — so any test of that shape needs a sentinel set after the
last assertion and checked at the cleanup label.  All three were fixed
that way.

**A decision made by a proxy mis-decides the first case that does not
fit it.**  ``shmem3_attach()`` had to choose what a failed fixed-address
map costs.  During ``PMIx_Init`` there is a move to make — fall back to
``gds/hash`` and re-request the job data in that format — and dropping
the job tracker is part of taking it.  Afterwards there is no such move.
The code decided which case it was in by asking **which segment** had
failed, sparing the modex alone: a true proxy when the modex was the
only post-init delivery anyone had hit, and wrong the moment job and
session updates started arriving the same way.  A session update that
failed to map took down the tracker owning the segments the client had
been reading since init.  The three entry points already knew which case
they were; they now say so.  ``openpmix#4156`` reached by a different
delivery.

**An "else"-less dispatch silently drops the general case.**
``PMIx_server_register_nspace()`` with a negative ``nlocalprocs`` is the
documented way to revise a running job.  The branch handled three keys
and had no arm for anything else, so a plain job-level key was read and
discarded; a ``PMIX_JOB_INFO_ARRAY`` was skipped outright once the
namespace had job info, which for a registered namespace is always; and
the two arms that did store went through ``pmix_globals.mypeer``'s
module — the server's own ``hash`` — reaching neither the segments a
``shmem3`` client reads nor any client.  The API accepted the call,
returned success, and did nothing observable.  It is now routed through
``PMIX_GDS_ADD_JOB_DATA``, which is a fan-out precisely because more
than one module holds a namespace's job data at once.

**Fix a divergence in both components or in neither.**  A first pass at
the job-data notification fixed ``shmem3``'s ``pack_update()`` alone.
That was correct in isolation and was backed out: ``gds/hash`` had the
same gap, and a client that reads late job data out of its own store
under one module and not the other is a worse state than one that reads
it from the server under both.  It landed once the rule an update obeys
had been settled — an update carries either only the changed values or
the whole description with something changed in it, and both cost the
same, so each component drops every entry matching what it already
answers.  For ``shmem3`` that is not an optimization: nothing is ever
removed from its segment chain, so a segment published for an unchanged
value is address space the job never gets back.

**A knob that only makes things slower is not a user's to turn.**  The
short circuit that answers a keyed ``PMIx_Get`` on the caller's thread
was gated by an MCA parameter, ``pmix_client_fast_get``.  An MCA
parameter is an offer to a user and appears in ``pmix_info``; no user
has a reason to ask for a slower answer to the same question, and the
name invites someone to turn it off in production on the theory that
anything called "fast" is cutting a corner.  The mechanism was worth
keeping — it is how a datastore suspected of answering differently
depending on which thread asked gets tested both ways — so it became the
``PMIX_GET_ON_PROGRESS_THREAD`` environment variable, documented where a
developer looks rather than where a user does.  Worth recording as a
question to ask of any new parameter: *what would a user be choosing
between?*

**Two claims of this pass's own were wrong, and both the same way.**
The first declared the fence half of ``openpmix#4156`` still open, from
reading ``shmem3_attach()``'s return value without following it one
level up to the unpacker that already swallowed it.  The second said a
failed update attach let ``PMIX_ERR_INVALID_NAMESPACE`` reach the
application; that is what the *fetch* returns internally, and the
application gets the right answer by way of a server round trip — the
real cost is a round trip per lookup for the rest of the run, and seeing
it at all takes ``PMIX_OPTIONAL``.  Both were a conclusion drawn from
one frame of the stack.  **Read the caller before declaring a path
unhandled, and follow the value out to where a user would see it before
describing a symptom.**

One documentation defect is recorded here because it had propagated:
:doc:`how-things-work/modex` described a per-process key index that every
caller passed ``NULL`` to, and a job-segment mechanism
(``pack_server_keyindex_info()`` and its partners) that had been removed.
``src/mca/gds/shmem3/AGENTS.md`` carried its own retired references.  A
guide that outlives the code it describes is worse than no guide, because
it is read as evidence.

2026-09-02 — the IOF write event's libevent record
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

The deferred-work entry about ``pmix_iof_write_event_t.ev`` being
``malloc``\ ed by a constructor that cannot report a failure is closed.
The record is now held by value, as ``pmix_iof_read_event_t`` has always
held its own, so there is no allocation to fail and no ``free`` to
match it.

What the entry did not say, and what doing it turned up, is that the
``NULL`` it asked the two sink macros to screen was carrying **two**
meanings, not one.  Out-of-memory was the one it named.  The other is
ordinary and happens on every run: a sink is constructed before it has a
descriptor, and ``PMIX_IOF_SINK_DEFINE`` arms the record only when it is
given a non-negative one — while ``PMIX_IOF_SINK_STATIC_INIT`` produces a
sink that has run no constructor at all.  Deleting the pointer would have
deleted the answer to the second question along with the first, and
handed libevent a zeroed record.  The screens stay; they now read an
explicit ``evset`` flag that says what they were really asking.

**Worth carrying: a sentinel that has been standing in for a state
usually loses one of its meanings when the thing it lives in changes
shape.**  A pointer that is NULL when the allocation failed is also NULL
before anything has been done with it, and only one of those survives
embedding the object.  Ask what a screen is *for* before removing what it
screens.

The change is confined to the write event because a libevent record held
by value cannot be copied or moved once it is set — the base records its
address — which is a constraint the pointer did not impose and is now
stated on the member, in ``src/common/AGENTS.md``, and nowhere else it
could be missed.

2026-09-02 — a raw errno in a pmix_status_t
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

``sigproc()`` in ``src/common/pmix_pfexec.c`` is declared to answer a
``pmix_status_t`` and answered the ``errno`` from a failed ``kill(2)``,
which ``kill_stage2``/``kill_stage3`` and
``pmix_pfexec_base_signal_proc`` stored straight into
``scd->lock->status``.  PMIx statuses are negative, so an ``EPERM``
reached a caller as the positive value ``1`` — neither ``PMIX_SUCCESS``
nor any defined error.  That entry is closed: the function now answers
``PMIX_SUCCESS`` or ``PMIX_ERROR``, and the errno stays where it is
useful, in the verbose line beside the signal and the pid that produced
it.

The entry proposed converting the errno to a PMIx code and deferred that
until a consumer existed, so that the conversion and a test for it could
land together.  The deferral was right about the mapping and wrong about
what it was holding up.  **Nothing had to be converted for the type
violation to stop** — the function had only ever been asked whether the
signal was delivered, and answering that needs no table.  Deciding
EPERM's PMIx spelling is a separate question, and it is still open,
still with no consumer, and no longer keeping a wrong value in a status
field while it waits.

Worth carrying: *a defect and the design question next to it are not the
same work item, and pairing them lets the open question hold the closed
one hostage.*  Ask what the smallest correct answer costs before
recording the whole thing as deferred.

2026-09-02 — a debug facility nothing could turn on
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

``pmix_debug_threads`` gates the acquire/release tracing in the four lock
macros, and nothing in the tree ever assigned it — no MCA parameter, no
environment variable, no write outside its definition in
``src/threads/thread.c``.  It was reachable only by setting the variable
from a debugger, which meant a quiet run was no evidence the handshake
had not run.  ``pmix_register_params()`` now sets it from
``PMIX_DEBUG_THREADS``.

The entry proposed an MCA parameter and that is not what landed, for the
reason recorded under the 2026-08-30 pass: *what would a user be
choosing between?*  The tracing is compiled in only under
``PMIX_ENABLE_DEBUG``, so on an optimized build an MCA parameter would
put a row in ``pmix_info`` for a knob that does nothing at all — worse
than the facility being hard to reach.  It is a development switch, so
it takes the shape the other one took: an environment variable,
documented where a developer looks rather than where a user does.
``PMIX_GET_ON_PROGRESS_THREAD`` is its sibling and the two should stay
the same shape.

Also worth noting is what the entry gave as its reason for deferring —
that ``src/threads`` is semantics-frozen.  True, and it never applied:
the variable is *read* there and had to be *written* somewhere else, and
``src/runtime`` is where every other piece of that startup state is set.
**A frozen directory is a reason not to change that directory, not a
reason not to fix the thing.**

2026-09-02 — giving back mypeer's second reference
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

The server and the tool point ``pmix_client_globals.myserver`` at
``pmix_globals.mypeer`` and ``PMIX_RETAIN`` it, and both used to give
that reference back by releasing ``pmix_globals.mypeer`` a second time
after ``pmix_rte_finalize()`` rather than by releasing ``myserver``, the
pointer that took it.  It worked, and it cost the tool a
``myserver_is_mypeer`` flag captured before the teardown purely to
suppress a release that would otherwise have been a third one.  Both now
release ``myserver`` *before* ``pmix_rte_finalize()``, which is the order
the client already used, and the flag is gone.

The entry deferred this on one question — whether anything in the
``rte_finalize`` chain reads ``pmix_client_globals.myserver`` — and said
answering it wanted more than a grep.  It does, and the answer is **yes,
one thing does**: ``pmix_ptl_close()``, reached through the framework
close, dereferences ``myserver->sd`` to close the socket.  Every other
reader in the tree is on a request path rather than a teardown one;
``pmix_hwloc_finalize`` and ``pmix_iof_finalize``, the two that looked
most likely, touch it nowhere.

That reader is safe under the new order, and the reason is a property of
``PMIX_RELEASE`` worth naming: **it NULLs its argument only when the
count reaches zero.**  So the release before the teardown does not mean
the same thing in the two shapes this pointer has, and does not need to:

* aliased — count 2 to 1.  The object stands on ``mypeer``'s reference
  for the whole chain and ``myserver`` still names it, so
  ``pmix_ptl_close()`` closes a live socket.  ``rte_finalize`` then
  frees, exactly as it does for a client.
* a real server the tool connected to — count 1 to 0.  The object is
  freed here, the pointer is NULLed, and ``pmix_ptl_close()``'s guard
  skips.  Nothing is left open: the peer destructor closes ``sd``
  itself.

One release covers both, which is what removed the flag.  Verified by
1200 tool and 1200 client init/finalize cycles under ``MallocScribble``
and ``MallocPreScribble`` — the check that would catch touching the peer
after an early free — and by an A/B ``leaks`` run over the tool cycles,
identical before and after at 3657 allocations.

The reordering also made a comment in ``src/runtime/pmix_finalize.c``
wrong in the most expensive way a comment can be: it told the reader not
to NULL ``pmix_globals.mypeer`` there, *because* the roles holding a
second reference release it after returning and guard that release on
the pointer.  No role does that any more.  A comment describing a
contract that has moved is worse than none, since the next reader has no
way to tell it is out of date.

Review coverage as it stands
----------------------------

The directories the review has already read.  What it has *not* read is
on :doc:`todo` under "Review coverage".

Assessed on **2026-08-15** from the commit history and refreshed as each
review lands: on **2026-08-20** for the ``src/mca/pnet``, ``src/mca/preg``,
``src/mca/pgpu``, ``src/mca/pmdl``, ``src/mca/pcompress``,
``src/mca/plog``, ``src/mca/psensor``, ``src/mca/psec`` and
``src/mca/pif`` reviews, on **2026-08-21** when ``src/mca/gds/shmem3``
was re-reviewed against its redesign, and on **2026-08-24**, when the
per-file pass over ``src/server`` finished — all twenty ``.c`` files —
and the last of ``src/client`` came inside a review, and on
**2026-08-26**, when the per-file re-review of ``src/util`` finished —
all twenty-nine of its ``.c`` files.

Reviewed and current
^^^^^^^^^^^^^^^^^^^^

``src/class``, ``src/client``, ``src/event``, ``src/include``,
``src/runtime``, ``src/server``, ``src/tool``, ``src/tools``,
``src/mca/base``, ``src/mca/bfrops``, ``src/mca/gds/base``,
``src/mca/gds/hash``, ``src/mca/gds/shmem3``, ``src/mca/pcompress``,
``src/mca/pgpu``, ``src/mca/pif``, ``src/mca/plog``, ``src/mca/pmdl``,
``src/mca/pnet``, ``src/mca/preg``, ``src/mca/psec``, ``src/mca/psensor``,
``src/mca/pstat``, ``src/mca/ptl``, ``src/threads``, ``src/util``,
``src/hwloc``, and ``bindings/python``.
``src/common`` was reviewed too, but has moved since; it is listed
under "Review coverage" on :doc:`todo`.

``src/client``, ``src/server``, ``src/util`` **and** ``src/hwloc`` **are
the only four directories the review has taken file by file**, a
dedicated pass per file rather than a directory-wide one, and the
difference is worth stating rather than flattening: a directory-wide
sweep is real coverage, and it is not the same thing.  The dedicated
pass found something in *every* file it read, including files that had
already been through four or five directory-wide sweeps.  ``src/client``
reached that state on 2026-08-23, ``src/server`` on 2026-08-24,
``src/util`` on 2026-08-26 and ``src/hwloc`` on 2026-08-27 — eleven
files, twenty, twenty-nine and two, five lenses each, one file to a
pass.  The per-file record is in the 2026-08-23 and later rows of
``.git/deep-review/ledger.tsv``; the reasoning is in each directory's
``AGENTS.md``.

``src/util`` is the sharpest evidence for that difference, because it is
the only one of the three that had already had a full directory review —
the July 2026 pass — and the per-file re-review still came to
+5260/-1062 across 58 files in 57 commits, against 4672 lines of new
test (eight new programs under ``test/unit/util``).  Two of the defects
it found were invisible from macOS and needed a Linux container to see
at all — one of them an installed header that failed to build on Linux
at all; one whole
subsystem, ``pmix_timings`` under ``--enable-pmix-timing``, turned out
never to have been built by anybody.  The lesson to carry is in
``src/util/AGENTS.md``: **a directory that has been reviewed once is
not thereby covered file by file.**

Two files fall short of that and are named here rather than rounded up,
both in ``src/client``:

* ``pmix_client_fence.c`` has had **no dedicated pass**.  It was signed
  off by the *directory-wide* five-lens seventh sweep — recorded there as
  coming through all five lenses with nothing to fix — and has not moved
  since.
* ``pmix_client_get.c``'s dedicated pass ran in two parts, and only the
  second one finished it.  The first (2026-08-22) covered lens 1
  (memory) over ``process_request``, ``PMIx_Get``, ``PMIx_Get_nb``,
  ``gcbfn`` and ``try_local_fetch`` and stopped there; ``get_data``
  (~500 lines), ``_getnb_cbfunc``, ``process_values`` and
  ``refresh_cache``/``refcb`` went unread, and lenses 2–5 were never
  applied to any of it.  The work that followed the same day — the realm
  and job-level-data fixes, the NULL-key answer, the ownership contract
  now stated in ``PMIx_Get(3)`` — chased specific findings out of that
  partial pass rather than completing it.  **All five lenses have since
  been applied to the whole file (2026-08-25)**, which is what found the
  entry below.

  What that second part found is worth stating here, because it is a
  property of the *list* rather than of either function that walks it.
  ``pmix_client_globals.pending_requests`` is two tables in one — the
  coalescing table ``get_data()`` consults before it sends, and the
  delivery table ``_getnb_cbfunc()`` walks when a reply arrives — and
  they matched on different predicates: ``PMIX_CHECK_NAMES``, which
  treats ``PMIX_RANK_WILDCARD`` on either side as a match, against exact
  rank equality.  A get for a specific rank issued while a get at
  ``WILDCARD`` for the same namespace was outstanding was therefore
  folded onto it, never sent, and never matched by the reply — and
  nothing else drains that list, so a blocking ``PMIx_Get`` waited
  forever and a ``PMIx_Get_nb`` was never called back at all.  Both
  sites now use one exact predicate; ``test/unit/get_api.c`` covers it.
  The general shape to look for: **one list serving two questions is
  one question, and a request the first accepts and the second rejects
  is not refused, it is lost.**

Will not be done
----------------

Real defects, correctly described, that are **not going to be fixed** —
because the fix would invest in something the project has already
replaced.  They are recorded so nobody re-derives them and opens the
question again; an entry here is a decision, not an oversight.

* **The deprecated regex API cannot carry a length, and will not learn
  to.**  From the ``src/mca/preg`` review (2026-08-19), which narrowed
  it rather than closing it.  ``pmix_preg_base_legacy_decode`` bounds
  every read against an ``avail`` argument, but only ``unpack`` can
  supply a real one — it knows how many bytes are left in the buffer.
  Every other caller holds a bare ``char *`` and passes ``SIZE_MAX``,
  because ``pmix_preg.parse_nodes(regexp, &names)`` has nowhere to put a
  length without changing a signature that predates the current API.
  ``gds/hash`` has the length in ``val->data.bo.size`` and still cannot
  hand it over.

  What remains reachable is narrow: it needs a caller-owned string that
  really does carry the ``"blob:"`` tag and is truncated behind it, which
  is not something a peer can produce — those arrive through the bounded
  ``unpack``.  The review had already made the tag test exact, so a plain
  node list whose first node begins with ``blob`` is no longer taken for
  a blob and walked past its end, which was the case a host could trigger
  with ordinary data.

  Closing it properly means plumbing a length through the deprecated
  signatures, and **that is the direction** ``pmix_regex2_t`` **was
  created to avoid**.  The regex2 API exists precisely because the old
  one cannot express a bounded buffer; a caller that is in a position to
  supply a length is in a position to use regex2, and widening the
  deprecated signatures would spend ABI-visible work making the
  superseded interface almost safe.  Deprecated APIs are supported
  indefinitely here, which is a promise to keep them *working*, not a
  promise to keep developing them.

Not defects — by design
-----------------------

These look like bugs and are not.  They are recorded so that they are
not "fixed" by a later reader.

* ``PMIx_Compute_distances`` **searches every device type when the info
  array names none.**  It applies its curated default — network,
  OpenFabrics, GPU, coprocessor — only when the caller passed no
  directives at all.  A caller who passed only ``PMIX_DEVICE_ID`` gets
  the full set, block and DMA devices included, which looks like the
  same inconsistency as the ``(ptr, 0)`` case fixed on 2026-08-27 and is
  not.  ``PMIx_Compute_distances(3)`` documents ``PMIX_DEVICE_ID`` as an
  independent selector, so narrowing the type set behind a caller who
  named a block device by name would make their device vanish.  Leave
  it; see ``src/hwloc/AGENTS.md``.

* **A** ``pmix_lock_t`` **whose** ``status`` **reads** ``PMIX_ERR_INIT``
  **is reporting a missing assignment, not an init failure.**  Both
  ``PMIX_CONSTRUCT_LOCK`` and ``PMIX_LOCK_STATIC_INIT`` seed ``status``
  with ``PMIX_ERR_INIT``.  A waiter reads it only after a handler has
  woken it, so every handler on a path whose waiter reads ``status``
  must assign it — and nothing enforces that, which makes the default
  the thing that decides how a violation presents.  ``PMIX_SUCCESS``
  would turn a forgotten assignment into a confident wrong answer;
  ``PMIX_ERR_INIT`` makes it loud.  ``PMIX_CONSTRUCT_LOCK`` previously
  assigned nothing at all, and since the lock usually lives in a
  ``PMIX_NEW``'d caddy — which at the time allocated without zeroing —
  the alternative was heap garbage.  If one of these turns up in a bug report, look at
  the handler that woke the lock and check its error paths before
  believing the status.

  The sweep behind this is worth not redoing.  Poisoning ``status`` and
  warning on it in ``PMIX_WAIT_THREAD`` fires ~86,000 times across
  ``make check``, which looks damning and is not: those callers read
  ``cb->status``, the *caddy's* own field, and never touch the lock's.
  Grepping ``lock.status`` and ``lock->status`` specifically narrows it
  to about nine sites in the library, every one woken through an
  ``opcbfunc`` that assigns ``status`` first.  The two spellings are a
  character apart and only instrumentation separates them.

* ``PMIX_ACQUIRE_THREAD`` / ``PMIX_RELEASE_THREAD`` **having no callers
  in** ``libpmix`` **does not make them dead code.**  An in-tree grep
  finds only ``test/unit/threads_primitives.c``, and the live handshake
  everywhere in the library is ``WAIT``/``WAKEUP`` at about 120 sites
  each — which reads as a retirement candidate and is not one.  These
  headers are installed under ``$(pmixincludedir)/src/threads``, and
  PRRTE uses the pair: ``src/runtime/prte_locks.h`` includes
  ``src/threads/pmix_threads.h`` to declare ``prte_init_lock`` as a
  ``pmix_lock_t``, and ``prte_init()``, ``prte_finalize()`` and
  ``src/prted/pmix/pmix_server_notify.c`` guard ``prte_initialized``
  with it across eleven call sites (checked against openpmix/prrte
  master, 2026-08-20).  Removing them breaks PRRTE's build, and changing
  their semantics changes PRRTE's behavior.

  This is worth keeping precisely because the in-tree evidence points the
  wrong way, and because unused-looking API is where documentation rots:
  ``src/threads/AGENTS.md`` had described ``ACQUIRE_THREAD`` as *not*
  implying that it returns holding the mutex, when that pair is exactly a
  held-mutex critical section, so a reader who believed the guide and
  mixed the pairs would unlock a mutex they do not hold.  That is
  corrected, and the unit test now covers the pair.

* **The function-pointer cast in** ``pmix_thread_start`` **is
  deliberate.** It hands a ``void *(*)(pmix_object_t *)`` to
  ``pthread_create``, which wants a ``void *(*)(void *)`` — formally a
  call through an incompatible function pointer type.  GCC's
  ``-Wcast-function-type``, which is on through ``-Wextra -Werror``,
  treats any pointer parameter as matching any other and does not
  diagnose it, and every ABI PMIx supports passes a ``void *`` and a
  struct pointer identically.  Only clang's opt-in
  ``-Wcast-function-type-strict`` and UBSan's ``-fsanitize=function``
  object, and neither is used by the build or by CI — the sanitizer job
  in ``.github/workflows/builds.yaml`` is ASan only.  If one of those is
  ever turned on, the fix is a small trampoline that takes ``void *``
  and calls ``t_run``, not a change to ``pmix_thread_fn_t``.

* **A statically initialized mutex is not** ``ERRORCHECK`` **even in a
  debug build.**  ``pmix_mutex_construct`` sets
  ``PTHREAD_MUTEX_ERRORCHECK`` under ``PMIX_ENABLE_DEBUG`` so
  self-deadlocks and double-unlocks abort loudly, but
  ``PMIX_MUTEX_STATIC_INIT`` expands to ``PTHREAD_MUTEX_INITIALIZER`` —
  an ordinary mutex — because there is no static spelling of a mutex
  attribute.  This is a property of pthreads, not an oversight, and it
  cannot be fixed without giving up static initialization.  The
  consequence to remember is diagnostic: a clean debug run over a
  file-scope lock says nothing about whether it can deadlock.

* ``pmix_mutex_construct`` **ignoring** ``pthread_mutex_init``'s
  **return is not an unchecked error.**  The constructor returns
  ``void``, so there is nothing it could do but abort, and on both glibc
  and macOS ``pthread_mutex_init`` with a valid attribute allocates
  nothing and cannot fail.  POSIX permits ``EAGAIN``/``ENOMEM``, so if a
  platform that can really fail it ever appears, add a debug-only
  ``perror`` and ``abort`` matching the ``EDEADLK``/``EPERM`` checks in
  the same file rather than trying to report it upward.

* **A** ``pcompress`` ``compress_string`` **that does not screen its
  argument for NULL is not a missing guard.**  All five implementations
  hand the pointer straight to ``strlen``, which reads as an asymmetry
  now that both *decompress* entry points screen for NULL — but the two
  directions take different data.  A decompressor is handed a length and
  a buffer a peer declared; a compressor is handed a string the caller
  just built and owns.  No caller in the tree can produce a NULL there,
  and a component answering ``false`` for one would report "I declined
  to compress" for what is really a caller bug, hiding it.  Add the
  screen only alongside a caller that can actually pass NULL.

* **The fence modex bucket reported as a libpmix leak belongs to the
  host.**  ``pmix_server_fence`` unloads the assembled bucket with
  ``PMIX_UNLOAD_BUFFER`` and hands the bare pointer to
  ``pmix_host_server.fence_nb``; ownership transfers on the call, and the
  request direction carries no ``(release_fn, release_cbdata)`` pair the
  way the reply direction does.  We cannot free it on return — the host
  parks it and reads it from another thread — so a host that does not
  free it leaks about 128 bytes per collecting fence.  In a valgrind run
  against a launcher this looks exactly like a libpmix leak and nothing
  else does: the allocation is ``pmix_server_collect_data``, the stack
  bottoms out in ``progress_engine``, and there is no ``prte_`` frame
  anywhere in it.  Both in-tree hosts (PRRTE and ``test/simple/simptest``)
  currently leak it.  See ``src/server/AGENTS.md``.

* **An event-caching block that assigns** ``cd->nondefault`` **inside
  its** ``0 < ninfo`` **guard is not dropping a flag.**  It reads as
  though a non-default event carrying no info would be parked as a
  default one and then delivered to default handlers that should not see
  it.  It cannot happen: every path that sets ``chain->nondefault``
  reads it out of a ``PMIX_EVENT_NON_DEFAULT`` directive in the info
  array, so the flag is only ever true when there is info to have
  carried it.  Moving the assignment out of the guard is equivalent, not
  a fix — two of the three caching blocks now assign unconditionally
  only because the guarded form kept being re-reported.  The third, in
  ``pmix_notify_server_of_event``, still assigns inside the guard and is
  equally correct.
* ``pmix_srand()`` **copies the seeded state into a file-static buffer**
  (``src/util/pmix_alfg.c``).  It looks like a footgun, but it is the
  only way to seed the global that ``pmix_random()`` reads, and the unit
  test deliberately holds down that behavior.  ``pmix_random`` is unused
  in-tree and the real ``pmix_srand`` callers use their own buffers, so
  the "last writer wins" hazard is not reachable.  Do not drop the copy
  without giving ``pmix_random`` another way to be seeded.
* ``wait_signal_callback()`` **reads the child-list size without a
  lock** (``src/common/pmix_pfexec.c``).  This is a formal data race,
  kept on purpose: the child is appended to the list before the
  ``fork()`` that can generate its ``SIGCHLD``, so the count cannot be
  stale for a child that matters, and removing the guard would make
  pfexec call ``waitpid(-1)`` on every ``SIGCHLD`` in a process with no
  children of its own.  If it is restructured, replace the read with a
  counter maintained across all six add/remove sites.
* **A failed** ``pmix_rte_init`` **returns without unwinding.**  It
  emits a diagnostic and returns, deliberately leaving the frameworks,
  globals and event base it had already brought up, because a failed
  ``PMIx_Init`` aborts the process.  New failure points should follow
  the same pattern.
* ``get_job_data`` **returning success with an empty buffer is safe.**
  The requesting client initializes its reported status to
  ``PMIX_ERR_NOT_FOUND`` and overwrites it only if a value turns up, so
  an empty success degrades to not-found at the requester.
* ``pmix_server_job_ctrl`` **creating a namespace for an unknown target
  is intentional.**  A job-control request may name a job this server
  has not been told about yet, and the epilog directives need somewhere
  to hang; the entry is reused when registration arrives.
* **The plog router builds its per-request channel list out of the
  global module wrappers.**  ``pmix_plog_base_log`` appends the
  ``pmix_plog_base_active_module_t`` objects that live in
  ``pmix_plog_globals.actives`` onto a local ``pmix_list_t``, so the
  list links and the ``added`` flag are written into shared state.  It
  reads as a re-entrancy bug waiting to happen, and it would be one — a
  module whose ``log`` reached ``pmix_plog_base_log`` synchronously
  would relink the objects out from under the loop walking them.  No
  module does: ``stdfd`` hands off to ``PMIx_server_IOF_deliver``, which
  posts an event and returns, and ``pmix_show_help`` thread-shifts
  rather than calling down inline.  Copying the wrappers per request
  would cost an allocation on every log call to defend against a caller
  that does not exist; the invariant is documented in
  ``src/mca/plog/AGENTS.md`` instead.  Enforce it there, not here.
* ``pnet/opa`` **and** ``pnet/nvd`` **ship no** ``configure.m4``, **on
  purpose.** A component with no ``configure.m4`` is configured by the
  MCA machinery itself and therefore builds unconditionally, which is
  what these two want: neither links anything nor needs an SDK — they
  read info attributes hwloc already recorded and set environment
  variables — and whether there is work to do is a property of the
  machine the *daemon* runs on, which only ``component_open`` is in a
  position to know.  The files they used to have asked that question at
  build time and got it wrong in both directions (``nvd``'s was
  hardwired off, ``opa``'s always succeeded).  The one visible
  consequence is that ``configure``'s summary no longer prints
  ``Transports / NVIDIA|OmniPath`` lines.  Do not restore them.
* ``transports_print`` **in** ``pnet/opa`` **type-puns a** ``uint64_t``
  **through** ``unsigned int *``.  That is undefined behavior in general
  and is not a live defect here: the whole tree is built
  ``-fno-strict-aliasing`` (``config/pmix_setup_cc.m4`` adds it wherever
  the compiler takes it). The surrounding arithmetic is deliberately
  width-independent, and the byte-order dependence of the result does
  not matter because the key is generated once on the lead server and
  shipped as a string.  Do not copy the idiom into new code, and do not
  "fix" it as a bug.
* **A node in** ``pnet/simptest``'s **topology file that the job's node
  map does not name is silently skipped.**  ``allocate`` matches each
  config line against the ``PMIX_NODE_MAP`` by exact name, which is
  intrinsic to a file that describes the real nodes the RM placed the
  job on.  The converse is *not* silent: a node the job was placed on
  that the file fails to describe draws a ``node-not-found``
  ``show_help`` naming it and the request is then declined — declined,
  rather than failed, because a hard error out of ``allocate`` aborts
  the base's fan-out for every other component too.
* ``psec/dummy_handshake`` **sends its length and status words as raw
  host-format** ``size_t`` / ``pmix_status_t``.  That means it only
  interoperates between peers of identical width and endianness, which
  would be a wire-format defect in a real mechanism.  It is not one
  here: the component exists solely to exercise the ``ptl``/``psec``
  handshake plumbing, is built only under ``--enable-dummy-handshake``,
  and is documented as not a pattern to copy.  Do not "fix" it by
  inventing a wire encoding for a test harness.
* ``pmix_psec_base_select`` **sets** ``pmix_psec_globals.selected``
  **before it can fail.**  A select that ends with an empty actives list
  returns ``PMIX_ERR_SILENT`` with the flag already true, so a second
  call would return ``PMIX_SUCCESS`` over an unusable framework.  There
  is no second call: ``pmix_init.c`` invokes it once and treats the
  failure as fatal to library init.  Left as-is rather than adding a
  rollback for a path that cannot be re-entered.
* **The bare** ``atomic_bool`` **fields in** ``pmix_globals_t`` are
  correct, merely inconsistent with the typedefs used elsewhere, and the
  ``PMIX_C_HAVE_*`` defines in the installed ``pmix_config.h`` are now
  always ``1`` but are retained in case an out-of-tree consumer tests
  them.
