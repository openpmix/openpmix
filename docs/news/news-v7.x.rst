PMIx v7.x series
================

This file contains all the NEWS updates for the PMIx v7.x
series, in reverse chronological order.

7.0.0 -- TBD
------------
Detailed changes since v6.1.0:
 - A deletion now reaches the data that has already been handed out.
   Removing a key from a PMIx server's own store was only half of it: a
   client caches what it reads about other processes and holds the
   job-level data it was given at initialization, so every local client
   that had looked the key up still had it. The server now tells its
   local clients, on a PTL tag of its own so that a peer too old to know
   about deletion never receives one. It goes to every local client
   except the one that asked, and is not restricted to the affected
   namespace, since a process may have cached data belonging to any
   namespace it asked about.
   This is also what answers PMIx_server_deregister_resources. The global
   cache is copied into a namespace's datastore once, when that namespace
   is first registered, and nothing re-reads it - so a deregistration
   governed only the namespaces registered afterwards and left every
   running job with its copy. It now takes the key back from the
   namespaces that already hold it, and from their clients. A qualified
   deregistration that prunes elements out of an entry rather than
   removing it is deliberately not propagated: the host asked for part of
   a value to go, so a deletion would take more than was asked. A
   namespace using gds/shmem3 also still keeps its copy, in its shared
   segment. See openpmix#4087.
 - PMIx_Put gained four scope values - PMIX_DEL_LOCAL, PMIX_DEL_REMOTE,
   PMIX_DEL_GLOBAL and PMIX_DEL_INTERNAL - naming the same audiences as
   their storing counterparts but directing that the key be removed
   rather than stored. The value is ignored and may be NULL, and removing
   a key that was never stored is not an error. The removal takes effect
   on the calling process at once and reaches the local server through
   the usual commit, so a PMIX_DEL_INTERNAL - which was never shared - is
   complete on return. This is a permitted extension: PMIx_Put(3) already
   states that an implementation may support additional scope values and
   must answer PMIX_ERR_NOT_SUPPORTED for one it does not, which is what
   a server predating these returns. That check is made up front, in
   PMIx_Put, because a server that does not recognize a scope would
   otherwise drop the block and let the delete appear to succeed.
   Companion projects can detect support through PMIX_CAP_DATA_DELETE.
   Propagating a removal to the other clients of a namespace, and to
   gds/shmem3's shared segments, is not yet implemented. See
   openpmix#4087.
 - A PMIx server now rejects a commit whose scope it does not recognize
   rather than silently discarding the data that scope labelled. It also
   rejects PMIX_INTERNAL, which names data that never leaves the process
   and so has no business on the wire.
 - A PMIx server can now contribute only what its processes published
   since they last took part in a collecting fence, rather than
   everything they have published, controlled by the new
   pmix_server_fence_delta_modex MCA parameter. It defaults to false: a
   server from a release that predates the delta marker rejects the whole
   collective rather than storing a contribution it cannot interpret -
   the right failure, but it means a job running mixed releases works
   today and would stop working if this defaulted on. Enable it once
   every node understands the marker. A delta is only sent when every
   local participant has already contributed to a fence over the same
   participant set; anything else falls back to the full set, which
   keeps two sub-communicators fencing independently from withholding
   data from each other. The watermark advances only once the host has
   taken the bucket, since the request has three arms that discard it.
 - gds/shmem3 keeps modex generations rather than always dropping the
   previous one. A cumulative contribution repeats everything, so it
   still supersedes the generation before it and every one behind that.
   A delta repeats nothing, so the previous generation is retired instead
   and a read walks the generations newest-first: a keyed lookup stops at
   the newest one holding the key, and a whole-process lookup consults
   all of them while dropping a copy of a key a newer generation already
   supplied. The chain is empty unless a delta has been stored, so the
   ordinary case is the single lookup it has always been, and any
   cumulative fence collapses it again. Segment blobs gained a field
   telling the client which kind it is looking at, so it makes the same
   decision its server made.
 - PMIx_Commit now transmits only what the process has posted since the
   previous commit. It used to fetch and send that process's entire local
   and remote store every time, so a single new PMIx_Put caused everything
   published so far to be sent again and n put/commit cycles moved O(n^2)
   bytes. PMIx_Put records which keys each scope owes the server and the
   commit fetches just those; a key posted repeatedly between two commits
   is still sent once, carrying the value it had when PMIx_Commit was
   called. The cumulative fetch remains as the fallback for the cases a
   per-key record cannot express - a PMIX_QUALIFIED_VALUE, which has no
   key a later fetch could ask for it back by; a tool that has repointed
   at another server through PMIx_tool_set_server or _attach_to_server,
   which has seen none of what we sent the previous one; and the first
   commit after PMIx_Init. Nothing changes for the caller, and nothing
   changes on the wire, so a client and server of different releases
   interoperate exactly as before. See openpmix#4087.
 - The per-server flag byte carried in the modex envelope is now screened
   before it is used. That byte says what kind of contribution a server
   made, and the only check on it was that the contributing servers agreed
   with each other - so a value they all agreed on, and that no datastore
   knew how to act on, passed straight through and its data was stored as
   though it were an ordinary full contribution. It is now rejected with
   PMIX_ERR_BAD_PARAM. A new value, PMIX_MODEX_DELTA, marks a contribution
   carrying only what the sending processes published since they last took
   part in a collecting fence, and the value is now handed to the
   datastore, which decides what it means for its own storage. Since the
   agreement check is what an older release already performs, a job mixing
   a release that sends delta data with one that cannot store it fails
   loudly on both sides instead of silently losing data. See openpmix#4087
   and the delta-exchange section of docs/how-things-work/modex.rst.
 - An MCA component whose framework interface version does not match the
   framework it is being loaded into is now refused instead of being
   opened. Nothing checked this before: the only version test in the
   loader compares the *MCA* major.minor, which is one number for the
   whole project and says nothing about whether a given framework's
   module struct has changed underneath the component, and its "TODO --
   add checks for project version (from framework)" has been there since
   the file was inherited from Open MPI (whose copy still has it, worked
   around by eight of its frameworks repeating the test themselves).
   Because components are run-time-loadable, an installed plugin older
   than the library loading it is what any partial upgrade produces, and
   it presents whatever module struct its header had at the time - which
   the library then calls through. pcompress grew two entry points in
   July 2026 without a version bump, and a plugin from before that left
   them NULL and turned the first size query into a jump to address
   zero. pcompress is accordingly bumped to 3.0.0, so such a plugin is
   now declined and the framework falls back to its own defaults - no
   compression rather than a crash. A refused component is reported
   through the usual mca_base_component_show_load_errors gate, which
   defaults to "none"; set it to the framework name (or "all") to see
   which plugins were dropped and why. Anyone carrying an out-of-tree
   pcompress component must rebuild it. A framework states its interface
   version as three macros in its own header, which both its components
   and its declaration read, so there is one place to edit when it
   changes; PMIX_MCA_BASE_FRAMEWORK_DECLARE is unchanged, and a framework
   declared with it - as every framework outside this project is - states
   no version and has its components checked as before
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
