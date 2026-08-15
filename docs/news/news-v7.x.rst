PMIx v7.x series
================

This file contains all the NEWS updates for the PMIx v7.x
series, in reverse chronological order.

7.0.0 -- TBD
------------
Detailed changes since v6.1.0:
 - A group formed through PMIx_Group_invite / PMIx_Group_join now
   exchanges its members' endpoint data, which only the collective
   PMIx_Group_construct did before. An acceptance carries the accepting
   process's own contribution, the leader assembles them all and returns
   the set in the PMIX_GROUP_CONSTRUCT_COMPLETE event, and every member
   absorbs it into its local store - so a PMIx_Get against another
   member is answered without leaving the process, as it already was for
   a constructed group. Where the group was assigned a context ID the
   values are stored qualified by it, matching what the collective path
   stores; without one they are stored plain. What is exchanged is
   documented explicitly now, on PMIx_Connect, PMIx_Group_construct and
   PMIx_Group_join and at PMIX_GROUP_ENDPT_DATA: each participant
   contributes exactly its own PMIx_Put values at PMIX_REMOTE and
   PMIX_GLOBAL scope, less any reserved key. Every participant calls the
   operation, so each supplies its own and no server sweeps for it -
   which also means anything the runtime computed rather than the
   application posting it is job-level data and travels separately
 - PMIX_GROUP_ASSIGN_CONTEXT_ID is now honored by PMIx_Group_invite and
   PMIx_Group_join, which accepted the directive and silently did nothing
   with it. Only the host environment can mint an ID that is unique
   across its scope, and a group formed by invitation runs no server
   collective through which one could be asked - so the leader now
   requests it separately, through job control with no targets, once the
   membership has resolved and before the completion event goes out. The
   ID rides in that event and so reaches every member, and a pending
   PMIx_Group_join returns it in its results, matching what
   PMIx_Group_construct has always done. A host that does not support
   the request is not fatal: the group forms without an ID, which is
   what happened before in every case. The announcement now waits on one
   host round trip, which forming a group by invitation can afford
 - The PMIX_GROUP_CONTEXT_ID_ASSIGNED event code has been removed. It was
   defined but never generated - not by this library and not by any known
   host - and the PMIx_Group_construct man page promised delivery of the
   assigned context ID through it, which never happened. The context ID
   is returned in the results array, as that page also says and as the
   code has always done. The value -169 is retired and will not be
   reused; the Standard will be updated to match
 - A PMIx server no longer drops an event one of its clients wanted. The
   fan-out filter matched an event's affected processes against the
   affected-process list a client's event registration had carried - but
   that list belongs to a single registration message, and a handler
   registered afterwards for a code the server is already forwarding
   sends no message at all. So a process that registered one handler
   restricted to a particular process and a second handler with no
   restriction stopped receiving the code entirely for the second one.
   The filter now matches on the event code, which is the only thing a
   server can decide correctly; the affected-process and source-range
   restrictions a handler registered with are enforced by the receiving
   process, which is the only place that knows what each handler asked
   for
 - A client now caches an event forwarded to it that none of its
   handlers accepted, so a handler registering a moment later is still
   given it - the behavior a tool has always had, and the reason the
   notification cache exists. An event can legitimately arrive unmatched
   because a handler's source range is never sent to the server, so the
   server cannot filter on it
 - A collective no longer counts a participant's orderly PMIx_Finalize
   as the loss of that participant. Such a rank has not left - its local
   process count is deliberately retained and its peer object tombstoned
   rather than retired, so it may PMIx_Init again and contribute -
   and recording it as departed counted it twice, letting the collective
   complete without it and return PMIX_ERR_PARTIAL_SUCCESS or
   PMIX_ERR_LOST_CONNECTION to the ranks that did participate. A client
   rapidly cycling init/finalize, as an MPI Sessions application does,
   could thereby drift by whole fence cycles and intermittently hang.
   Loss accounting now applies to an abnormal termination only. One
   deliberate consequence: a rank that finalizes and does not return,
   while its peers wait in a collective that names it, now leaves them
   waiting rather than collapsing the collective - PMIX_TIMEOUT is the
   remedy there
 - PMIx_Disconnect now honors PMIX_TIMEOUT while the server is still
   collecting local contributions, as PMIx_Fence and PMIx_Connect
   already did. Until every local participant has called, the request
   has not been passed to the host environment, so nothing the host
   might do about the timeout can reach it - which is precisely the
   case the attribute is documented for, a participant that never
   arrives. Such a disconnect previously blocked its callers
   indefinitely and stranded the collective's tracker; it now completes
   every waiting participant with PMIX_ERR_TIMEOUT
 - PMIx_Spawn and PMIx_Spawn_nb no longer treat a non-NULL job_info
   pointer carrying a zero ninfo as an empty directive list. Such a call
   failed the whole spawn with PMIX_ERR_EMPTY, a status naming nothing
   the caller had done wrong; (info, ninfo) is a pair and a zero count
   means "no directives" whatever the pointer is
 - PMIx_Spawn no longer writes into the caller's const pmix_app_t apps[].
   An app may declare its directives by terminating them with an
   end-marked info rather than setting ninfo, and the count worked out
   from that scan was stored back into the caller's array - which
   segfaults for a caller whose apps sit in read-only storage
 - An app-level PMIX_SETUP_APP_ENVARS directive is now honored for every
   app that carries one. Only the first such app was served, because the
   per-app harvest set the flag meaning "the job-level directive already
   covered every app" - so an MPMD spawn whose second app asked for its
   own programming model's envars silently got none. This affects the
   roles that have a pmdl framework open: tool, launcher and server
 - PMIx_Compute_distances no longer reports device distances it does not
   have. The reply handler took the count the server said it was sending
   rather than the count that arrived, so an application could be handed
   entries that were never written - zeroed, so a caller reading uuid
   found a NULL. The distance array and its count now agree on every
   path, including the failure ones
 - PMIx_Resolve_peers and PMIx_Resolve_nodes no longer crash on a node
   that hosts none of a namespace's processes, and report the documented
   empty result - PMIX_SUCCESS with a NULL array and a zero count -
   rather than PMIX_ERR_NOMEM. Both the client and server halves of the
   computation had this
 - PMIx_Lookup no longer reports a result count larger than the number of
   entries it actually unpacked
 - PMIx_Fabric_update now reports a refresh the server never completed as
   a failure instead of returning PMIX_SUCCESS with the caller's
   pmix_fabric_t left stale
 - A group reference that expands to no members at all is now rejected
   with PMIX_ERR_NOT_FOUND rather than yielding a NULL participant array
   that the caller then indexes
 - PMIx_Group_invite and PMIx_Group_invite_nb no longer race their own
   observer registration. An invitation could resolve inside the
   registration call, after which the setup path was still writing to a
   tracker the announcement chain already owned - which lost a
   PMIX_GROUP_OPTIONAL directive, armed a timer on freed memory, and in
   the non-blocking form used the tracker after it had been released
 - A PMIx_Get answered on the caller's thread that then declines the
   short-circuit no longer leaves its fetched entries behind for the
   ordinary path to inherit, which turned a scalar get into a
   PMIX_DATA_ARRAY
 - A PMIx_Get for a reserved key that is not held locally is now allowed
   to reach the host environment. The client used to force PMIX_IMMEDIATE
   onto any such request, which confines the search to what the local
   PMIx server already holds - and the client only reaches that point
   after having asked that server's datastore and its own, so the flag
   could only ever return the answer already in hand. It also stopped the
   one party that may still know the value: a host frequently withholds
   reserved keys it could supply, handing each daemon only the job-level
   data its own local clients need rather than replicating every proc's
   location keys on every node. Such a key is now requested like any
   other, so the server can surface it to its host. A PMIX_IMMEDIATE the
   caller passed itself is unaffected and is still honored
 - Relatedly, a server asked for a job-level reserved key it does not hold
   no longer answers with an empty payload. That is indistinguishable from
   "not found" to the client and it foreclosed the host, which is the one
   party that may still have the value; the request is now passed up
   instead. A server with no direct_modex support behaves as before,
   reporting PMIX_ERR_NOT_FOUND. Only reserved keys are treated this way -
   a miss on a key some process put says nothing about what the host knows
 - A reserved key requested for a process local to the server now fails
   immediately with PMIX_ERR_NOT_FOUND rather than waiting out a timeout.
   Such a key is ours from the moment the namespace is registered; it does
   not arrive later in that client's commit, which carries only what the
   client itself put, so there was never anything to wait for
 - Every pcompress component now exposes its compression level as an MCA
   parameter - pcompress_zlib_level, pcompress_zlibng_level and
   pcompress_zstd_level - where the zlib components previously hard-coded
   level 9. That is the wrong end of the speed/size curve for what this
   framework compresses: a large collective payload, on a single progress
   thread, with a whole job waiting on the result. A broadcast pays the
   deflate once and the wire cost on every link of the tree, so the last
   couple of percent of ratio only repays itself on a very wide tree over
   a slow link. Measured through PMIx on a 25.6 MB aggregated modex,
   single-threaded, zlib level 1 gives a ratio of 0.649 at 103 MB/s
   against level 9's 0.638 at 54 MB/s. The defaults are now 1 for zlib
   and 2 for zlib-ng - they differ by one deliberately, because zlib-ng
   remaps its level 1 onto a quick-deflate strategy that is much faster
   and appreciably weaker, so zlib-ng level 2 is what zlib level 1
   produces. The two defaults match in behaviour rather than in digit
 - Added a zstd component to the pcompress framework, ranked above the
   zlib ones (zstd 90, zlibng 75, zlib 50), so a build that finds
   libzstd uses it for every compressed payload. The zlib component
   deflates at level 9 unconditionally, which is the wrong end of the
   speed/size curve for what this framework is used for - shrinking a
   large collective payload on a single progress thread while the whole
   job waits on it. Measured on a 25.6 MB aggregated modex,
   single-threaded, zstd's default level 3 reaches a ratio of 0.588 at
   427 MB/s where zlib level 9 takes 64 MB/s to reach only 0.600, and
   decompression - which every peer pays rather than just the
   originator - runs at 3.6 GB/s against 0.6. The compression level is
   an MCA parameter, pcompress_zstd_level. Note that a zstd blob is not
   DEFLATE, so unlike zlib and zlib-ng the components are NOT mutually
   readable: every node in a job must run the same one. The zstd
   component checks the frame magic and refuses a foreign blob rather
   than inflating garbage
 - Added PMIx_server_IOF_flow_control, by which a host environment can
   suspend and resume the processes that are feeding stdin to it. A
   host falling behind on stdin previously had no way to slow its
   producers: the library discarded the status of every push_stdin
   upcall and re-armed the read unconditionally, so a refusal anywhere
   would have dropped bytes rather than throttled anyone. The library
   now applies such a request both to any stdin it is reading itself
   and to every tool that has pushed stdin to it, and a tool that is
   itself a server relays it onward - so a chain of launchers carries
   it back to whoever holds the input stream. Nothing is buffered on
   behalf of a suspended stream and nothing is lost; the bytes stay in
   the producer's input stream, where the OS applies the back-pressure.
   A host can also suspend opportunistically by completing a push_stdin
   upcall with the new PMIX_ERR_IOF_XOFF status, which means "I have
   taken this data, now stop sending" and is deliberately not a
   failure. Detectable at build time as PMIX_CAP_IOF_FLOW_CONTROL
 - The library no longer re-arms its stdin read before the far end has
   acknowledged the chunk it just sent. Both the server-role path (the
   push_stdin upcall) and the tool-role path (the relay to its server)
   used to fire and forget, which is what made the stdin producer
   impossible to pace; the tool path additionally armed the read event
   twice per chunk. Stdin now flows at the rate the consumer accepts it
 - A tool whose stdin forwarding is suspended is no longer told its
   forwarding failed. The tool-side handler treated every non-success
   acknowledgement as terminal - deleting the read event and raising
   PMIX_ERR_IOF_FAILURE - so a "slow down" from the far end would have
   killed the stream instead of pausing it
 - Fixed a crash on SIGCONT in a tool or launcher that is collecting a
   terminal's stdin. The signal handler is registered with no callback
   data, and the handler it shares with the stdin restart path
   dereferenced that as a read event
 - A process now records the alias forms of its own node name no matter
   where that name came from. The library builds the alias list - which
   is what lets it recognize its own node under both the FQDN and the
   short form - only when it had to discover the hostname itself, so a
   host environment that supplied PMIX_HOSTNAME, which every resource
   manager does, left the list empty. Every other node's name was
   already being passed through the same normalization, so ours was the
   only one not getting it
 - The list of CPUs to which the internal progress thread is to be bound
   is now validated before it is used. An entry that is not a number, a
   negative one, one beyond the end of the CPU mask, or a backwards
   range is reported and skipped; if binding was requested as required
   and nothing usable remains, initialization fails. Previously the list
   went to strtoul unchecked, which reports zero for a token containing
   no digits - so a typo such as "cpu0" silently became "bind to CPU 0"
 - Added the missing help text for a malformed or unrecognized entry in
   the pmix_var_dump_color MCA parameter. Both messages were being
   requested by name and neither existed, so a user who mistyped that
   parameter got "I couldn't find that help reference" in place of the
   explanation
 - Under PMIX_EXTERNAL_PROGRESS, finalize now releases the event base
   and marks the library as no longer accepting work. Both were being
   skipped along with the progress thread that mode does not start, so
   the base outlived finalize while roughly a hundred API entry points
   went on believing a torn-down library was open for business
 - Several allocations are no longer leaked on each init/finalize cycle:
   the output channels opened for the client verbosity parameters, the
   two static IOF sinks in a server, the file and directory names given
   by PMIX_IOF_OUTPUT_TO_FILE / _TO_DIRECTORY, and the environment-
   variable harvest patterns held by the pnet, pgpu and pmdl components.
   Also, directive state such as the node ID and the external-progress
   and external-topology flags is now reset, so what one PMIx_Init was
   told no longer becomes the default for the next one
 - The datastore now fills in the per-proc location keys - hostname,
   nodeid, local rank and node rank - for every rank a host did not
   describe itself, and for every one of those keys a host left out. It
   works these out from the node and proc maps, and it rightly declines
   to overwrite anything the host stated in a PMIX_PROC_INFO_ARRAY,
   since what it computes is only an assumption. But that decision was
   being made once for the whole job: a single proc-info array anywhere
   in the registration suppressed the derivation of nodeid, local rank
   and node rank for every rank in it. So a host that described one
   process lost those three keys for all the others, and a host that
   described every process but named only some of the keys lost the
   rest for all of them - PMIx_Get answering PMIX_ERR_NOT_FOUND with
   nothing to fall back on. The choice is now made per rank and per
   key. Hostname, which was being derived outside the same test, now
   follows the rule in both directions: it is supplied when the host
   gave none, and left alone when the host gave one
 - A batch of server-side request-handling fixes from a review of
   src/server. Two of them are use-after-free or double-free rather
   than leaks: the IOF pull and deregister handlers released the
   caddy the host had just accepted ownership of, so the host's
   completion callback ran on freed memory; and the fabric-register
   handler released its query caddy twice on a malformed request.
   Related, the internal response path for a fabric request the
   pnet layer answered itself handed the completion function a
   server caddy where it expected a query caddy, and the fabric
   update path left that function unset entirely
 - A server no longer answers a PMIx_Get for a rank that is not one
   of its own by reading a peer id out of a list sentinel. The value
   is arbitrary, and when it happened to name a live client slot the
   request was treated as local and parked to wait for a commit that
   was never coming
 - A deferred direct-modex request that the server could not launch -
   no host support, or a host that rejected the up-call - is now
   fully discarded. Only the tracker's own reference was being
   dropped, and since every parked requester holds one of its own,
   the tracker survived unreachable: off the pending list where no
   later resolve could find it, and still holding a pointer to the
   caddy that was about to be freed
 - A registration for a default event handler (one made with no event
   codes) now checks the notification cache, so an event that arrived
   before the handler registered is still delivered to it
 - PMIX_TIMEOUT is now read into a variable of the width the accessor
   is told to write. The fence, connect and get handlers were passing
   the address of a time_t while asking for a 32-bit conversion, which
   fills the wrong half of the field on a big-endian host
 - Assorted leaks on server error paths: the group id and participant
   array of a malformed group request, the scratch lists of a job
   control request that was rejected part-way through the directive
   scan, the retained caddy behind a failed fabric request, the reply
   buffer of a collective or get whose status could not be packed, and
   the payload already assembled for a cross-namespace get that then
   found nothing. The fence and connect timeout handlers also no longer
   release a tracker that is still linked into the collectives list
 - The fence and connect handlers no longer drive their completion
   callback on the internal error path that has no tracker to give it -
   in connect's case it was being handed a caddy instead, and in both
   cases the caller was then answered and released a second time by
   the switchyard
 - A server asked to resolve the peers of every known namespace now
   reports that it found none, rather than reporting success with an
   empty list, and gives each namespace its own first-choice lookup
   rather than letting one namespace's fallback rank change the search
   for all the ones after it
 - The fence collective's duplicate-contributor check now works. The
   list it consults was never appended to, so it was always empty: a
   clone sharing a rank with its parent had its remote data packed into
   the modex bucket twice, and one tracking object was leaked per
   contributor per fence. The per-rank blob that same loop assembles is
   also released now, rather than being leaked once the pack has copied
   it
 - A server no longer answers a notification addressed to several
   processes by rebuilding its target list out of copies of the one
   process being removed from it. The purge that runs when a peer departs
   indexed the surviving array by the wrong loop variable
 - When a process others are waiting on departs, the direct-modex
   requests parked against it are now failed rather than dropped. The
   tracker was being released while each waiting request still held a
   reference on it, so it survived unreachable and the waiting clients
   were left expecting a reply nobody would send
 - A namespace update that carries a group context id now stores it
   against the namespace rather than against whatever process identity
   happened to be left on the stack, and reports a failure of that store
   instead of the result of the preceding value copy
 - PMIx_server_finalize now releases the system temporary directory it
   recorded at init. Besides the leak, a second PMIx_server_init found it
   still set and silently kept the previous cycle's value
 - A server told to make an optional connection to another server, and
   unable to make it, no longer composes a PMIX_SERVER_URI out of the
   connection that did not happen
 - Assorted server leaks on paths that "can do nothing": five host
   callbacks that returned without honoring the release function the host
   gave them, the scratch lists of PMIx_server_register_resources, the
   directive array a launcher-spawned server uses to attach to its parent,
   and the result caddies of PMIx_server_setup_application and
   PMIx_server_collect_inventory when the caller supplies no callback
 - PMIX_GROUP_ENDPT_DATA is now documented with the type it actually
   carries: a pmix_data_array_t* of pmix_info_t, led by the contributing
   process' PMIX_PROCID and the PMIX_DATA_SCOPE at which the remaining
   elements are to be stored. The header had described it as a
   pmix_byte_object_t, which was the shape of a group-construct data
   exchange the library no longer performs. That stale type reached the
   generated attribute dictionary as well, so it is what the
   attribute-support queries and pattrs reported for the attribute
 - Every blocking PMIx entry point now reports PMIX_ERR_WOULD_BLOCK when
   called from within the PMIx progress thread, rather than hanging
   there. Making such a call has always been disallowed - the work that
   would release the caller is what that thread was about to do - but
   only a minority of entry points said so, and the rest simply stopped:
   an event handler that called PMIx_Fence, PMIx_Connect or
   PMIx_Group_construct wedged the process with nothing reported. The
   screen now covers all of them, sixty-one call sites, and emits a
   diagnostic naming the call. Where the wait is conditional on a NULL
   cbfunc - the fence, notify and event-registration entry points - only
   that path is screened, since with a callback those are meant to be
   driven from a handler. The affected man pages carry a PROGRESS THREAD
   RESTRICTION section stating the rule and the return code
 - PMIx_server_deregister_resources now honors the qualifiers its man page
   describes. A request element carrying a data array narrows the removal:
   PMIX_NODEID/PMIX_HOSTNAME choose the entries it applies to, and any
   other member of the array chooses the elements to remove from within
   them - matching an element directly or matching one that contains it,
   which is how a PMIX_FABRIC_DEVICE is named by its PMIX_FABRIC_DEVICE_NAME
   or PMIX_DEVICE_ID. Removing the last element an entry described removes
   the entry as well. Previously every such request matched on the key
   alone, so the man page's own example - remove one node's fabric device -
   deleted every node entry the server held. Note that deregistration
   governs the namespaces registered after it: non-namespace information is
   copied into a namespace's data store when that namespace is registered,
   so a job already running retains it
 - Fixed a double completion of a collective tracker that corrupted the
   server's collectives list and could abort a later PMIx_server_finalize
   with an invalid free. A collective the host never sees - a strictly
   local fence is the ordinary case - is finished by driving the tracker's
   completion function, which thread-shifts: until its handler runs the
   tracker is still on the collectives list and still tests as complete,
   because only that handler drains the participant list. Anything that
   walked the list in that window - the lost-connection sweep, a queued
   collective timeout - saw a complete, unclaimed collective and drove its
   completion a second time, and two handlers then each unlinked and
   released the same tracker. The host handoff was already guarded by
   host_called; a local completion had no equivalent. A tracker whose
   completion has been driven is now marked, and every path that can reach
   one honors the mark - including the tracker lookup, which would
   otherwise join a new contributor to a tracker about to be freed,
   hanging that client
 - A host that answers a spawn or direct-modex up-call with
   PMIX_OPERATION_SUCCEEDED is now told that it cannot, and the request
   is failed to its requestor rather than reported as a success carrying
   nothing. Both operations report a result - the namespace of the job
   that was launched, or the data that was fetched - and the callback the
   library supplies is the only channel for it, so an atomic completion
   had nowhere to put the answer: a spawn came back to the application as
   PMIX_SUCCESS with an empty namespace, over both the local and the
   remote path, with nothing said. The library always supplies that
   callback, so a host completing the work immediately can simply invoke
   it - before returning, if it likes - and return PMIX_SUCCESS. The
   pmix_server_module_t man page now states this, and PMIx_Spawn_nb no
   longer returns PMIX_OPERATION_SUCCEEDED, which the Standard does not
   define for it
 - A mistyped PMIX_DATA_SCOPE qualifier on PMIx_Get is now rejected with
   PMIX_ERR_BAD_PARAM rather than taken at face value, on both the client
   and the server side. The scope selects which table the datastore
   searches, so reading it out of a union that holds something else
   answered the request confidently and wrongly - it could not crash,
   since a scope is only ever compared and never used as an index, which
   is why it outlived the qualifiers beside it. Every other typed
   qualifier in the same code already rejected a type mismatch. The
   server-side read is the more exposed of the two: that info array
   arrives off the wire, so its type tag is the requesting peer's word
