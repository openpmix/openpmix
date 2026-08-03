PMIx v6.x series
================

This file contains all the NEWS updates for the PMIx v6.x
series, in reverse chronological order.

6.1.1 -- xx May 2026
--------------------
Detailed changes since v6.1.0:
 - PMIx no longer reads past the end of a message when a peer claims
   more values than it sent. The flexible integer decoder bounded its
   loop with an expression that underflowed once nothing was left to
   read, so a three-byte buffer could drive an unbounded over-read and
   crash the process. It now refuses an empty region and reports a
   truncated encoding rather than assembling a value out of whatever
   bytes followed
 - A data array carrying no element type no longer desynchronizes the
   message it is packed into. The packer wrote a PMIX_UNDEF type tag and
   a size while the unpacker read the tag as a terminator and never
   consumed the size, so everything after it was misread. Such an array
   is trivially produced - copying a value whose data array pointer is
   NULL yields one
 - Copying a data array of PMIX_PROC_CPUSET whose elements are not valid
   hwloc objects no longer releases the element block twice. Any
   PMIx_Value_xfer or PMIX_INFO_XFER of an array from
   PMIx_Data_array_create reached it
 - An unpacked PMIX_DATA_BUFFER can now be read from. Unpacking restored
   the payload but left the cursors and the allocation size unset, so
   the caller received a buffer it could not extract a single value out
   of
 - Eighteen PMIx data types that pack and unpack correctly on their own
   can now also be used as data array element types. PMIX_DATA_ARRAY_CONSTRUCT
   silently produced an empty descriptor for them, which made unpacking
   an array of any of them fail with PMIX_ERR_NOMEM. The affected types
   include PMIX_NODE_PID, PMIX_REGEX2, PMIX_TOPO, PMIX_DATA_BUFFER,
   PMIX_KVAL, PMIX_BUFFER, PMIX_DEVTYPE, PMIX_JOB_STATE, PMIX_LOCTYPE,
   PMIX_COMMAND and the PMIX_STOR_* family
 - Copying a data array of PMIX_POINTER no longer reads past the end of
   the array. The elements were allocated one byte apiece and copied a
   pointer apiece
 - Strings unpacked from a message are now guaranteed to be terminated
   rather than trusting the sender to have included the terminator
 - PMIx_Value_get_number no longer accepts a negative PMIX_INT16,
   PMIX_INT32 or PMIX_INT64 into an unsigned destination type. All
   three sign checks tested the wrong member of the value union, so
   the check never fired and the value wrapped silently
 - PMIx_Value_get_number no longer reports failure for a conversion it
   performed correctly. Every conversion whose destination was
   PMIX_PROC_RANK wrote the right answer and then returned
   PMIX_ERR_BAD_PARAM
 - PMIx_Value_get_number now accepts PMIX_PID as a source type. It had
   no handler at all, so every conversion out of a pid returned
   PMIX_ERR_BAD_PARAM
 - PMIx_Value_get_number range checks corrected: a PMIX_INT32 converted
   to PMIX_INT8 or PMIX_UINT8 was range-checked as though it were an
   int16 and could truncate silently; PMIX_INT64 to PMIX_PID or
   PMIX_STATUS was bounded by UINT32_MAX rather than by the signed
   32-bit range and left the negative side unchecked; a float to
   PMIX_UINT was bounded by 42949670295 rather than 4294967295; a float
   or double of exactly 256 converted to PMIX_UINT8 as 0; and a double
   in the top 255 values of the uint32 range was refused although it is
   exactly representable
 - PMIx_Data_type_string now names PMIX_NODE_PID when called before the
   library is initialized
 - A pmix_value_t that names a type whose payload lives behind a pointer
   but carries no object no longer crashes the library. Such a value is
   malformed but takes nothing more than setting the type before the
   data, and it faulted in PMIx_Value_string, PMIx_Info_string,
   PMIx_Value_compare, PMIx_Value_get_size, PMIx_Info_get_size,
   PMIx_Value_xfer, PMIx_Info_xfer, PMIx_Info_list_xfer and
   PMIx_Value_unload, for every pointer-backed type. Each of those now
   treats it as what it is - an absent object
 - PMIx_Value_unload no longer writes through a NULL destination for a
   PMIX_JOB_STATE value. That type is copied into caller-supplied
   storage like its neighbours, but was missing from the list the
   function checks before it does so, so it crashed where the others
   returned PMIX_ERR_BAD_PARAM
 - A process-realm info array (PMIX_PROC_INFO_ARRAY, also known by its
   deprecated name PMIX_PROC_DATA) may now identify the process it
   describes with any of the forms its definition allows. The datastore
   accepted only PMIX_RANK, and only as the very first element,
   rejecting anything else with PMIX_ERR_TYPE_MISMATCH - so a host that
   identified the array with PMIX_PROCID, as the attribute expressly
   permits, or that simply ordered the array differently, could not
   register its job. PMIX_RANK and PMIX_PROCID are now both accepted, in
   any position; PMIX_RANK is preferred when both are present
 - Documented the self-referential reserved keys on the PMIx_Get man
   page. PMIX_RANK and PMIX_PROCID name the very thing the proc
   parameter has to supply, so they are retrieved by leaving that part
   of proc unspecified - a rank of PMIX_RANK_INVALID for PMIX_RANK, a
   NULL proc for PMIX_PROCID - rather than for the process identified in
   proc as the surrounding text implied
 - A PMIx_Query_info that is outstanding when the connection to the
   server is lost now returns PMIX_ERR_COMM_FAILURE instead of hanging.
   The reply handler treated a zero-byte buffer with a bare return, so
   the blocking form waited forever on a wakeup nobody would send
 - PMIx_Process_monitor with PMIX_SEND_HEARTBEAT no longer hangs. The
   non-blocking form sent the beat and returned success without ever
   invoking the callback the blocking form was waiting on
 - PMIx_Resource_block no longer completes twice when the host accepts
   the request. The callback was invoked whether or not the host had
   taken ownership, so a caller saw two completions - and a blocking
   caller had its lock woken a second time after it had been destructed
 - PMIx_Job_control on a server no longer returns the caller's own
   directives as the operation's results. The caddy carried the
   directives in the same fields used to stage the host's answer, so a
   host that returned no info echoed the request back as the reply
 - PMIx_Data_pack and PMIx_Data_unpack no longer release the peer they
   looked up before using it. The peer was stored in a caddy whose
   destructor releases that field, so packing for a process in another
   namespace freed the peer out from under the pack - and out of the
   server's client array
 - A malformed directive value no longer crashes the library. PMIx_Log
   read PMIX_LOG_SOURCE through the proc member of the value union,
   PMIx_Query_info read its PMIX_PROCID/PMIX_NSPACE/PMIX_RANK qualifiers
   the same way, PMIx_Process_monitor read its four target directives as
   data arrays, and the IOF layer read PMIX_IOF_OUTPUT_TO_FILE and
   PMIX_IOF_OUTPUT_TO_DIRECTORY as strings - none of them checking the
   type the caller had actually set. All now return PMIX_ERR_BAD_PARAM
   or ignore the directive
 - PMIx_Query_info with a query carrying no keys, and PMIx_IOF_pull with
   a source count and no source array, are rejected rather than
   dereferenced
 - XML-tagged output no longer emits invalid character references. Bytes
   above 0x7f were read as signed and escaped as "&#-128;" style
   references
 - Listing the attributes of a registered function no longer overflows
   its output buffer. The function name was copied into a fixed-width
   line with no bound, so a host that registered a long name smashed the
   stack of anything that listed that level - pmix_info included
 - The pfexec kill sequence now escalates to SIGKILL as documented. The
   escalation was conditional on the SIGTERM having failed to be
   delivered, so a child that received SIGTERM and ignored it was left
   running - and, having already been removed from the children list,
   never reaped
 - PMIx_Register_attributes, PMIx_Get_attribute_string and
   PMIx_Get_attribute_name now reject a NULL name rather than passing it
   to strdup or strcasecmp
 - A reply whose status cannot be unpacked is reported as a failure.
   Several handlers left the status at success, telling the caller the
   operation had worked and simply returned no data
 - PMIx_Load_topology(NULL) and PMIx_Get_relative_locality with a NULL
   output pointer no longer segfault - both now return
   PMIX_ERR_BAD_PARAM. The two neighbouring hwloc entry points had
   already been screened for this; these were missed
 - A client can once again use PMIX_SETUP_APP_ENVARS on PMIx_Spawn. The
   pmdl framework that performs the envar harvest is opened only by the
   server and tool roles, and the "no programming-model support in this
   role" answer its stub returns to a client - PMIX_ERR_INIT - was
   treated as fatal, so the whole spawn failed and reported that the
   library was not initialized. The directive is carried in the
   job-level info regardless, and whoever launches for us performs the
   harvest with its own pmdl
 - PMIx_Spawn no longer modifies the caller's app array. The apps
   parameter is declared const, and the library copies it precisely
   because it modifies what it spawns, but the PMIX_SETUP_APP_ENVARS
   harvest ran before that copy was made and so called PMIx_Setenv()
   against the caller's own env arrays. A caller reusing the same
   pmix_app_t for a second spawn accumulated the harvested envars a
   second time. Only the roles that have a pmdl open - tool, launcher,
   server - were affected
 - The PMIX_READY_FOR_DEBUG announcement issued by PMIx_Init now reaches
   the local server. Its custom-range directive was built from the
   server's pmix_peer_t rather than the server's process ID, and a
   pmix_proc_t copied out of that object is the object header
   reinterpreted as a namespace and a rank, so the notification named a
   process that does not exist and no debugger ever saw it
 - A malformed pmix_value_t no longer crashes the library. A value
   tagged PMIX_DATA_ARRAY whose array pointer is NULL is now treated as
   an absent array by the copy path, rather than being dereferenced for
   its type and size - which every PMIX_INFO_XFER and every pack that
   copies first was exposed to. Several client entry points that read a
   union member on the strength of the attribute key alone, without
   checking the type tag, now check it: PMIX_GROUP_INFO and
   PMIX_PARENT_ID from the caller, and the affected proc, group id,
   group membership and group-info blobs that arrive over the wire
 - Bounded how deeply data arrays may be nested inside one another when
   packed or unpacked. Each level of nesting costs the sender only a
   type tag and a size on the wire but costs the receiver a frame of
   recursion in the unpacker, so a small message could describe a nest
   deep enough to exhaust the stack. The limit is the new
   bfrops_base_max_array_depth MCA parameter, 100 by default and
   unlimited if set to zero, and it counts every form of nesting - an
   array whose element type is PMIX_DATA_ARRAY, and the ordinary array
   / info / value chain alike. Both ends enforce it: the sender refuses
   to build such a message rather than emitting one the far side is
   bound to reject
 - A pmix_data_array_t whose element type is PMIX_DATA_ARRAY is now
   supported end-to-end. It has always packed - and printed - as a
   contiguous block of pmix_data_array_t descriptors, exactly as
   pmix_data_array_t.5.rst describes any array of its declared type,
   but nothing else agreed: construct had no arm for the type and so
   quietly produced a NULL array, which made unpack of a nested array
   fail with PMIX_ERR_NOMEM on a machine that was not out of memory;
   copy refused it with PMIX_ERR_NOT_SUPPORTED, so PMIx_Value_xfer
   could not carry one; and destruct treated the block as a single
   descriptor, ignoring size and never freeing the block, so it leaked
   for size > 1 and dereferenced NULL for size 0. All four now use the
   documented layout. The Python bindings had built a third layout
   again - each element pointing at one further descriptor, with the
   element's size set to the inner count - and now load and unload the
   elements in place; a nested element is described with the same
   {'type', 'array'} dict as the enclosing array, not the {'val_type',
   'value'} form of a value (see openpmix#4054)
 - Implemented PMIx_Group_invite_nb, which was previously inert: it
   resolved the invitation, invoked the caller's callback, and stopped
   without announcing anything at all - no PMIX_GROUP_INVITE_FAILED, no
   PMIX_GROUP_CONSTRUCT_COMPLETE, no PMIX_GROUP_CONSTRUCT_ABORT - so no
   group was ever formed, and every call leaked its tracker and its
   event registration. The announcement was built out of blocking
   notifications and so could only run on the blocking form's own
   thread; it is now a chain of non-blocking notifications driven from
   the progress thread, which both forms share. A NULL callback is now
   rejected rather than accepted and then never invoked
 - PMIx_Group_join and PMIx_Group_join_nb now complete when the group
   construct resolves, as PMIx_Group_join.3.rst has always specified,
   and return the group id and membership the leader announced. They
   previously completed as soon as the accept/decline notification had
   been handed to the local event system - much earlier, and carrying no
   group data - so results/nresults always came back empty. Losing the
   leader before the construct resolves now completes the call with
   PMIX_GROUP_LEADER_FAILED rather than leaving it pending. A declined
   join, or one that names no leader, has no construct outcome to wait
   for and still completes as soon as its notification is away. This is
   a behavior change for anything written against the old timing:
   detect it with the new PMIX_CAP_GROUP_JOIN_COMPLETES capability flag
 - Fixed an unbounded hang in PMIx_Group_invite, and a leader-failure
   safety net that never fired, both caused by the library having no way
   to observe an event that the application cannot suppress. PMIx used
   ordinary event handlers for events it needs for its own correctness,
   and an application is entitled to end the event chain by returning
   PMIX_EVENT_ACTION_COMPLETE - the normal way to say "I handled this" -
   which silently skipped every library handler positioned after it. An
   application that registered a handler for PMIX_GROUP_INVITE_ACCEPTED
   (to log who joined, say) therefore blinded the leader to the answers
   it was waiting for, and PMIx_Group_invite blocked forever unless the
   caller supplied a PMIX_TIMEOUT; an application handling the process
   termination or group construct codes likewise disabled the
   invitee-side watch that turns a lost leader into
   PMIX_GROUP_LEADER_FAILED. The library now watches these events
   through an internal observer registry that runs ahead of the
   application's event chain and cannot be pre-empted (see
   openpmix#4059)
 - Fixed the same defect pointed the other way: the library's own group
   invitation handler ended the event chain itself, so whenever it ran
   first it swallowed the application's handlers for
   PMIX_GROUP_INVITE_ACCEPTED, PMIX_GROUP_INVITE_DECLINED and
   PMIX_PROC_TERMINATED. Library observers have no return value, so they
   can no longer suppress an application handler
 - Repaired a second round of defects in the client library, found on a
   follow-up sweep of src/client. Several were crashes on inputs the
   APIs document as legal or that a careless caller can easily produce:
   PMIx_Put dereferenced the value's type and printed the key from a
   pmix_output_verbose() call placed above the validation meant to catch
   them, and never checked the value at all; PMIx_Compute_distances,
   PMIx_Resolve_peers and PMIx_Resolve_nodes each stored a default
   through their OUT parameters without checking them; PMIx_Group_construct
   and PMIx_Group_invite wrote results/nresults unchecked although the
   man pages document both as optional; PMIx_Group_join did not validate
   the group ID before strdup'ing it; PMIx_Spawn walked the apps array
   without checking it was there; and PMIx_Fabric_deregister was the one
   fabric entry point with no "library initialized" gate in front of a
   dereference of state that init creates
 - Fixed PMIx_Compute_distances_nb, which could never fall back to
   asking the server. The fallback passes a NULL topology (and sometimes
   a NULL cpuset) to mean "use your own", which the server supports, but
   the generic pack entry point rejects a NULL source before the packer
   that understands it is reached - so every request that could not be
   answered locally returned PMIX_ERR_BAD_PARAM instead of being sent
 - Fixed PMIX_GROUP_NOTIFY_TERMINATION, which was never recorded against
   a group. PMIx_Group_construct captured the directive onto a tracker
   the completion path does not consult, so the group was always
   registered with the flag clear and PMIx_Group_destruct never
   re-applied the policy for anyone
 - Fixed a group invitation resolving one answer early whenever the
   leader was not itself among the invitees. The leader's own answer was
   credited unconditionally, so a leader inviting only the others started
   the count one ahead of the membership: the last accept arrived after
   the decision, was recorded as a non-responder, and - the default
   construct being all-or-nothing - aborted the whole thing
 - PMIx_Abort now reports the status the server returned rather than
   always reporting success, so a host that refuses the abort, or does
   not support one, reaches the caller. This is the same defect fixed
   earlier in PMIx_Commit
 - Plugged three leaks in the client: PMIx_Fabric_update orphaned the
   fabric's previous info array on every refresh, PMIx_Get under
   PMIX_GET_STATIC_VALUES abandoned the library's copy of the value after
   copying it into the caller's storage, and PMIx_Resolve_peers leaked
   its working list when the aggregate walk collected entries but
   resolved no peers. Also stopped the fabric and device-distance reply
   handlers from reporting a tolerated short read to the application as
   the result of an operation the server had said succeeded
 - Fixed PMIx_Connect_nb's handling of the two optional trailing info
   blobs it appends - the caller's endpoint data and the job-level data
   its namespace contributes. Both are a data array of info structs,
   which a pre-v4 peer's bfrops cannot pack at all, and both were packed
   straight into the message: the failure is partial, so the wire got a
   half-encoded field that the receiver had to trip over and skip. They
   are now packed through a scratch buffer and appended only if that
   succeeds, so a peer that cannot represent one simply does not receive
   it - which is a state the receiver already handles, since it reads
   each with a trailing unpack. A genuine failure to append data that
   did pack is now reported rather than passing silently
 - Added regression coverage for the above: the new cases in
   test/unit/client_api.c (which segfaults against the unfixed library),
   and a new test/unit/run_grpinviteothers.pl plus
   examples/group_invite_others exercising an invitation whose leader is
   not one of the invitees. Both are in "make check", and the swarm
   suite gained the multi-node form of the invite case
 - Stopped "make -j check" from failing test/unit. Most of that
   directory's tests drive a real PMIx server through simptest, which
   comes up in the scheduler role - a node-wide singleton that claims a
   fixed rendezvous file, so the second server to want it is refused by
   design. Automake's parallel harness starts several at once, and all
   but one lost the race. The directory is now marked .NOTPARALLEL,
   which costs nothing measurable: the whole suite still runs under -j
   in well under a minute. This was not a new defect, only an
   unreachable one - the CI job that builds with "make -j check"
   configures with default visibility, and so never descended into
   test/unit until the SUBDIRS gate above was removed
 - Made "make check" actually run the test suite. test/Makefile.am
   wrapped its whole SUBDIRS line in "if !WANT_HIDDEN", so unless the
   tree was configured --disable-visibility a top-level make check ran
   the 15 programs in test/ itself, printed "# FAIL: 0", and silently
   skipped test/unit, test/unit/class, test/unit/util, test/simple and
   test/topologies. The optimized, default-visibility configuration -
   the one those tests most need to cover, and the one users get - was
   therefore never exercised by make check, and nothing said so. The
   condition is no longer true now that the class descriptors declared
   in installed headers are exported: all five subdirectories build and
   pass with symbol visibility on, verified in five configurations
   across both primary platforms - Linux/gcc and macOS/clang, debug and
   optimized - with 80 tests passing and no warnings in every one. Two guards against a repeat: test/Makefile.am now fails
   check-local if SUBDIRS ever comes out empty rather than recursing
   into nothing and reporting success, and the top-level Makefile.am
   prints a "no tests were run" notice when configured
   --without-tests-examples, which produces the same silent success one
   level up
 - Closed five more argument-checking holes in the src/class container
   classes, all of them invisible in the configuration developers build
   in. The hotel's three room-number accessors - pmix_hotel_checkout,
   pmix_hotel_checkout_and_return_occupant and pmix_hotel_knock - tested
   the lower bound of room_num with an if and the upper bound with an
   assert, and configure adds -DNDEBUG whenever --enable-debug is off, so
   no build that ships checked it: checkout then wrote through rooms[]
   past the end of the array and pushed a second out-of-bounds write into
   unoccupied_rooms[]. Callers pass a cached cd->room, and a destructed
   hotel reached the same place through room 0. pmix_ring_buffer_push had
   no guard at all where pop and poke both have one, so pushing to a ring
   that was never initialized, or one already destructed, dereferenced a
   NULL addr. pmix_value_array_reserve was missing the zero-item-size
   check its two siblings carry, and since glibc answers realloc(NULL, 0)
   with a non-NULL zero-byte block it reported success and committed a
   capacity over storage that did not exist. The hash table's "one key
   type per table" check sat inside PMIX_ENABLE_DEBUG, so mixing key
   types was silent in a shipping build - leaking every copied ptr key
   and rehashing live entries through the low bytes of a key pointer; it
   is unconditional now, with only its diagnostic still behind the switch,
   and its TMA exemption (needed because a shared segment's method
   pointers mean nothing to a peer) is now covered by a cross-process
   test. The ptr key family also rejects a NULL or zero-length key rather
   than dereferencing it. pmix_bitmap_set_max_size did not validate its
   argument, and because the cap is stored as a word count a negative bit
   count wrapped round to a stored cap of zero, failing every later init
   and set_bit far from the call that was actually wrong
 - Fixed the dockerswarm class-test harness, whose ten-node pass had
   never actually run anything: it staged the libtool wrapper script
   rather than the executable under .libs, so every node reported that
   the binary did not exist. contrib/dockerswarm/build.sh now also
   detects a build directory whose component set has changed underneath
   it - the gds/shmem2 to gds/shmem3 rename left every pre-existing tree
   permanently unbuildable, because a configure.m4 recorded as a
   prerequisite of aclocal.m4 no longer existed and make tried to run an
   aclocal the image does not carry
 - Repaired a set of init, teardown and argument-checking defects in the
   src/class container classes. pmix_hash_table_construct() left ht_label
   uninitialized, and that field is read and printed by src/util/pmix_hash.c
   behind only a NULL test, so every table that does not set its own label
   carried garbage there. The base class descriptor carried NULL
   constructor and destructor arrays, so PMIX_DESTRUCT on a statically
   initialized object that had not yet been constructed dereferenced NULL.
   The ring buffer's destructor left head and tail behind a freed array, so
   a pop after destruct followed them; its init never set them, so
   re-initializing a used ring returned one still claiming the old
   occupancy. Two failed-realloc paths (pmix_bitmap_set_bit,
   pmix_value_array_init) leaked the old block and left a NULL pointer
   behind a non-zero size. Two guards - the NULL check in
   pmix_bitmap_num_set_bits, and a negative-index assert in
   pmix_pointer_array_test_and_set_item - were absent from every build
   that ships, the first because it sat inside PMIX_ENABLE_DEBUG and the
   second because configure adds -DNDEBUG whenever --enable-debug is off.
   pmix_list_insert accepted a negative index and inserted anyway,
   pmix_hash_table_init2 used its two ratios without validating them, and
   pmix_next_poweroftwo shifted a signed int by 31 and by 32
 - Made the double-checked lock behind lazy class initialization correct.
   Every PMIX_NEW and PMIX_CONSTRUCT tests whether a class is initialized
   without taking the class mutex, and both that flag and the epoch were
   plain int, so a thread could observe the new epoch with nothing
   ordering it against the constructor array the initializing thread had
   just built. Both are C11 atomics now, published with a release store
   after the arrays are written and read with an acquire. This does not
   change the size of pmix_class_t, so consumers that declare their own
   classes are unaffected. ThreadSanitizer reports seven data races in
   pmix_class_initialize against the old code and none against the new
 - Removed two dead members of the object model: pmix_tma_t's tma_memmove
   and pmix_list_item_t's item_free. Both were written and never read, and
   there was no helper through which tma_memmove could even be called.
   Both live in structures gds/shmem3 builds inside the segment it shares
   with its local clients, so dropping them moves every field below - a
   cross-version change, made now while the gds/shmem2 to gds/shmem3
   rename earlier in this cycle still covers it, since that rename has not
   yet appeared in a release. pmix_object_t shrinks from 104 to 96 bytes
   and pmix_list_t from 248 to 232 (measured --enable-debug)
 - Gave a default-visibility descriptor to every class declared in an
   installed header. Without it a consumer can include the header but
   cannot link PMIX_NEW or PMIX_CONSTRUCT on the class, and the failure is
   invisible in a tree configured --disable-visibility - which is how
   these are usually built. PRRTE has no object system of its own and uses
   this one directly, and one of the descriptors it needs
   (pmix_mca_base_var_file_value_t) was hidden; test/unit could not link
   at all against a default-visibility build. Classes declared only in
   headers that are not installed remain hidden
 - Renamed the gds/shmem2 component to gds/shmem3. The component shares
   memory layout rather than a wire format - the server builds the job's
   data structures inside a segment its local clients map and read in
   place - so any change to the size or layout of those structures
   silently repoints every field an older client reads. The version
   number in the name is what prevents that: a client selects its gds
   module by name, so a client from an older release does not recognize
   the new one and falls back to hash. This rename accompanies a change
   to the size of pmix_object_t (see below); without it, an older peer
   mapping the segment segfaults. The component was renamed once before,
   from gds/shmem to gds/shmem2, for exactly this reason
 - gds/shmem3 segments now carry a layout stamp, and a client whose
   in-memory layout differs from the server's declines the segment and
   falls back to hash instead of reading every field at the wrong
   offset. The component name guards this across releases, but two
   builds of the same source can still disagree: --enable-debug adds a
   magic ID to the front of pmix_object_t, so a debug server and a
   default-build client differ by 24 bytes in the base class and 56 in
   pmix_list_t. The stamp is computed from the sizes of the types that
   go in the segment rather than hand-maintained
 - Replaced the per-object pthread_mutex_t used to guard the object
   reference count with a C11 atomic. The mutex was initialized and
   never destroyed, so it leaked on any platform where init allocates;
   and objects allocated from a touchable-memory allocator live in a
   segment shared between processes, where the mutex was never created
   PTHREAD_PROCESS_SHARED - so cross-process retain/release was
   undefined. An atomic int32 in shared memory is well defined. Every
   object in the tree also shrinks by the size of a pthread_mutex_t
 - Completed the server-to-host handshake for system (environmental)
   event registration. The server now reference-counts each system
   event code across all of its local clients and its own
   registrations: it calls the host's register_events only for the
   codes acquiring their first registrant - rather than on every
   registration for a code it had already asked for - and calls the
   host's deregister_events, which previously had no callers at all,
   when a code loses its last registrant. A code registered again
   after being released is re-activated with the host. Only system
   codes are handed to the host, which was always the contract for
   these entry points. Hosts that implement register_events should
   also implement deregister_events; leaving it NULL simply means the
   host keeps forwarding codes nothing is listening for
 - Fixed a server taking the scheduler, system-controller or system-tool
   role being unable to restart after it was killed or crashed. Those
   roles publish a rendezvous file that is removed only by a clean
   finalize, and the listener treated any pre-existing file as fatal -
   so an orphaned file left by the dead server blocked the next one
   until someone deleted it by hand, and the failure was reported as
   "the listener thread failed to start", which points at sockets rather
   than at a leftover file. The file records the pid that wrote it, so
   the listener now reclaims a file whose owner is gone (or that carries
   no usable pid, as happens when its creator died partway thru writing
   it). A file whose owner is still running is left untouched, and the
   init fails with a message naming the role, the file and the owning
   pid
 - Fixed PMIxTool.set_server_module in the Python bindings, which built
   its server-function module and returned PMIX_SUCCESS without ever
   handing it to the library - so a Python tool acting as a server had
   the handlers it registered silently never invoked. It now calls
   PMIx_tool_set_server_module and reports what the library says
 - PMIx_tool_set_server_module no longer segfaults when called before
   PMIx_tool_init. It marks the caller's peer as a server, and there is
   no peer until the library has been initialized; it now returns
   PMIX_ERR_INIT, and rejects a NULL module
 - Fixed a segmentation fault in PMIx_tool_finalize for any tool that
   became a server after initializing. PMIx_tool_init opens the
   fork/exec framework only for a launcher or a scheduler, but the
   finalize path tore it down for a launcher or a *server* - and a plain
   tool becomes a server precisely by calling
   PMIx_tool_set_server_module. Finalize then walked a children list
   that was never constructed. The framework now records whether it was
   opened, its close is a no-op if it was not, and finalize checks that
   rather than the peer type
 - Added Python bindings for the five struct pretty-printers -
   info_string, value_string, proc_string, app_string and
   resource_unit_string. These render a whole struct the way the library
   itself does, which a Python program cannot reproduce by printing a
   dict, and each has its own man page; they had been swept out of scope
   by a prefix rule aimed at the construct/free helper families
 - Added Python bindings for the serialization APIs - data_pack,
   data_unpack, data_copy, data_copy_payload, data_load, data_unload,
   data_embed, data_compress and data_decompress. A data buffer crosses
   as the dict {'bytes': ..., 'bytes_used': n, 'bytes_unpacked': m},
   mirroring the fields of a pmix_data_buffer_t that mean anything on
   the Python side; the three raw pointers the struct also carries are
   rebuilt from those offsets on each call. There is no buffer object to
   create or release, and because the whole state is a payload plus an
   offset, a buffer can be stored or sent somewhere as ordinary bytes
   and picked up again
 - Fixed the Python conversion layer returning signed bytes. Payloads
   were read through a char pointer, which is signed on most platforms,
   so any byte at or above 0x80 arrived as a negative number and
   bytearray() rejected the whole buffer with "byte must be in
   range(0, 256)". Every non-ASCII payload hit this - credentials,
   forwarded I/O, a packed buffer
 - Completed the Python bindings for the non-blocking APIs. The twenty
   remaining _nb entry points - fence_nb, get_nb, publish_nb, lookup_nb,
   unpublish_nb, spawn_nb, connect_nb, disconnect_nb, query_nb, log_nb,
   allocation_request_nb, job_control_nb, monitor_nb,
   get_credential_nb, validate_credential_nb, fabric_register_nb,
   fabric_update_nb, fabric_deregister_nb, compute_distances_nb and
   resource_block_nb - join the group operations bound earlier. Each
   returns only the status of the request and delivers its result to a
   Python callback executed on the progress thread, which runs if and
   only if the call returned PMIX_SUCCESS. Every operational PMIx API is
   now reachable from Python in both forms
 - Fixed four latent defects in the Python conversion layer, found by
   putting new argument shapes through it. Key arrays built by
   pmix_load_argv were allocated with Python's allocator but released
   with free() (and vice versa), which aborts the process;
   pmix_free_apps released neither the argv/env arrays nor the app
   array; an application carrying an empty info list produced a
   zero-length but non-NULL array, which the library reads as
   "terminated by an end marker" and walks past the allocation looking
   for one, so such a spawn packed garbage; and an attribute list
   containing an unconvertible value type crashed the caller, because
   the partially-filled array was released in full and the entries never
   written were destructed as if they held real values
 - Corrected every Python example in the man pages. All of them wrote an
   attribute with its value nested in a second dictionary, a shape the
   bindings do not accept - the examples raised KeyError rather than
   running
 - Added Python bindings for the remaining unique blocking APIs. The
   client gains heartbeat, progress_thread_stop and resource_block; the
   server gains generate_regex2 and parse_regex2 - the current regex
   interface, of which only the deprecated generate_regex had been bound
   - plus generate_cpuset and collect_job_info; and the pretty-print
   family gains error_code (the inverse of error_string), data_print,
   value_comparison_string, group_operation_string,
   resource_block_directive_string and alloc_inheritance_string. A
   regular expression crosses to Python as
   {'type': str, 'bytes': bytes, 'len': int} and a resource unit as
   {'type': PMIX_DEVTYPE_x, 'count': n}. What remains unbound is the
   non-blocking family and one deprecated tool API
 - PMIx_Heartbeat is now a no-op for a process that has no server above
   it. A heartbeat travels *up* to the server watching the caller, but a
   PMIx server - and a tool that has not attached to one - is its own
   server, so the message looped back into the process's own matching
   code, found no posted receive for the heartbeat tag, and was reported
   as an unexpected message. Each call therefore raised an error event
   and grew the process. A heartbeat is likewise skipped once the
   connection to the server has been lost
 - Fixed three library entry points that crashed or hung when called
   before PMIx_Init - reachable from C, but found while adding their
   Python bindings, where a script naturally calls a method on a freshly
   constructed object. PMIx_Heartbeat sent through a peer pointer that
   is NULL until initialization, PMIx_Progress_thread_stop walked its
   tracking list before that list was constructed, and
   PMIx_server_collect_job_info thread-shifted its request and then
   waited forever for a progress thread that did not exist
 - PMIx_server_collect_job_info now rejects a request that names no
   procs. There is no job to collect in that case, and the collection
   loop walked the resulting NULL namespace list. A NULL proc array or
   output buffer is likewise rejected rather than dereferenced
 - PMIx_Get_relative_locality now accepts the strings that
   PMIx_server_generate_locality_string actually produces. The generator
   emits a bare "SK0:L20:CR0:HT0" and a host environment stores exactly
   that as PMIX_LOCALITY_STRING, but the comparison routine required an
   "hwloc:" prefix and otherwise returned PMIX_ERR_TAKE_NEXT_OPTION - so
   the usage the header documents, passing it the generator's output,
   reported no locality at all and every consumer of PMIX_LOCALITY_STRING
   silently saw unrelated processes. Both spellings are now accepted, and
   a string carrying some other provider's prefix is still passed along
   rather than misparsed. A NULL locality is also now reported instead of
   being dereferenced
 - Fixed a segmentation fault in PMIx_server_generate_cpuset_string and
   PMIx_server_generate_locality_string. Both compared the cpuset's
   source string with strncasecmp before checking it for NULL, so a host
   environment passing a cpuset that carried a bitmap but no source -
   or simply a zeroed struct, in the locality case, which did not check
   the cpuset pointer either - crashed the library. A NULL source is
   legal on an input cpuset the caller wants filled in, but not on one
   being serialized, so both now report PMIX_ERR_BAD_PARAM. They also
   set the returned string to NULL on every failure path; the
   "not my cpuset" path previously left the caller's pointer untouched
 - Implemented the Python cpuset bindings, which had been stubs that
   returned PMIX_ERR_NOT_SUPPORTED no matter what they were passed:
   parse_cpuset_string, generate_cpuset_string and
   generate_locality_string now do real work. They rest on a new
   conversion layer that represents a cpuset as the dict
   {'source': str, 'cpus': [int, ...]} and translates it to and from the
   library's "<source>:<range-list>" string form. get_cpuset, which had
   returned the provider name as undecoded bytes and left the source
   prefix embedded in the first CPU entry, now returns that same dict,
   and compute_distances - which expected a cpuset shape no other method
   produced - accepts it. Both also stop leaking the cpuset and the info
   array, and the device-distance array is now released by the library
   that allocated it rather than by Python's allocator
 - Added Python bindings for the non-blocking group operations -
   group_construct_nb, group_invite_nb, group_join_nb, group_leave_nb
   and group_destruct_nb. Each takes a Python callback that is executed
   on the progress thread when the operation completes, which is what
   lets a Python program accept a group invitation from inside an event
   handler - the blocking form must not be called there. These are the
   first non-blocking client bindings, and the machinery they introduce
   is documented in docs/how-things-work/python_nonblocking.rst for the
   remaining non-blocking APIs to reuse
 - Fixed a segmentation fault in PMIx_server_init when the PMIX_SINGLETON
   directive carries a malformed value. The parser assumed the value
   always contained the "nspace.rank" separator and dereferenced the
   result of its search for it, so a value with no '.' faulted the
   server; values that did contain a '.' but were otherwise invalid (an
   empty nspace, a non-numeric or out-of-range rank) were silently
   accepted and registered a bogus singleton. The directive is now
   validated, and a bad value is reported through show_help and rejected
   with PMIX_ERR_BAD_PARAM
 - Fixed the server aborting when a host environment rejects an incoming
   client connection. The error path in the connection handler released
   a rank_info object owned by the namespace's rank list, driving its
   refcount to zero while it was still on that list, and released the
   pending-connection object twice. A host returning an error from its
   client_connected callback now simply gets the connection refused
 - Fixed a group bug affecting every caller of PMIx_Group_construct_nb,
   in C as well as Python: the client recorded the constructed group
   only on the completion path used by the blocking wrapper, so a
   non-blocking caller's subsequent leave or destruct failed with
   PMIX_ERR_NOT_FOUND against a group it had just built
 - Fixed several defects in the Python bindings found while adding the
   above: values handed to the library were allocated with Python's
   allocator and freed with the C one, which aborted the process when a
   Python server returned a data array; parameter annotations rejected
   None on optional arguments, making documented defaults unreachable;
   the server-module upcalls could not pass directives to a Python
   handler; and the regex/ppn generators read their output pointer
   without checking for failure. The Python test scripts also never ran,
   as the search path they were given was left unexpanded
 - Added the --enable-test-build configure option. It force-builds the
   test and environment-specific components - the GPU vendor components,
   the NVIDIA/simptest/TCP transports, the pgpu test component, and the
   optional-library wrappers (zlib, zlib-ng, libesmtp, MUNGE) - so they
   can be compile-checked on a machine that lacks the supporting hardware
   or libraries. Components that need a third-party header supply a
   non-functional shim header used under the resulting PMIX_TESTBUILD
   macro. The option is intended for developers and CI: the shimmed
   components are not functional, so a test-build must not be installed
   for real use
 - Revived the pnet/simptest test fabric component. It had grown stale
   (its module signatures and internal node struct no longer matched the
   pnet interface) and no longer compiled; it has been ported to the
   current interface and once again drives the static endpoint/coordinate
   assignment path end-to-end. Fabric endpoints are delivered as per-proc
   data and fabric coordinates as per-node info, matching how PMIx_Get
   resolves each
 - PMIx_Load_topology no longer leaks the source string. When the caller
   does not request a specific source, the returned topology's source
   field now points to a read-only, statically allocated string instead
   of a heap copy the caller had no safe way to release (the returned
   topology is shared, library-managed state that must not be destructed
   by the caller). A source the caller did supply remains theirs to free.
   The PMIx_Load_topology man page documents this ownership split
 - Group construct fault handling. When a member is lost before
   contributing to a PMIx_Group_construct, the server now consults the
   PMIX_GROUP_FT_COLLECTIVE directive (previously ignored): by default
   the construct aborts with PMIX_GROUP_CONSTRUCT_ABORT rather than
   silently forming a reduced group, while with the directive set it
   completes on the survivors and notifies each of them via a
   PMIX_GROUP_MEMBER_FAILED event naming the lost member. The server
   synthesizes PMIX_GROUP_MEMBER_FAILED itself, so this works with any
   host environment
 - Group destruct fault handling. When a member is lost before
   contributing to a PMIx_Group_destruct, the server now consults the
   PMIX_GROUP_NOTIFY_TERMINATION directive (previously ignored, so the
   documented behavior never occurred): by default the teardown completes
   on the survivors but is reported as an error, while with the directive
   set each survivor receives a PMIX_GROUP_MEMBER_FAILED event naming the
   lost member and the destruct returns PMIX_SUCCESS - the event serving
   in place of an error. The directive is supplied at construct time but
   governs the destruct; the library remembers it and re-applies it
   automatically (it may also be passed directly to PMIx_Group_destruct)
 - Group leader failure. A process that accepts an invitation with
   PMIx_Group_join_nb now watches the group leader; if the leader
   terminates before the construct completes, the library delivers a
   PMIX_GROUP_LEADER_FAILED event to the waiting process (naming the
   leader) so the application can drive reselection, rather than
   blocking indefinitely
 - Group construct timeout. A PMIX_TIMEOUT supplied to
   PMIx_Group_construct is now enforced by the server for the local
   phase: if a live participant never contributes, the construct
   completes with PMIX_ERR_TIMEOUT instead of hanging
 - Added the PMIX_GROUP_CANCEL group operation (a host callback used to
   abort an in-flight cross-server construct) and the PMIX_CAP_GROUP_FT
   capability flag advertising the group fault-tolerance feature set
 - PMIx_Group_leave / PMIx_Group_leave_nb are now implemented. Previously
   the client sent a command the server never handled, so the call
   returned PMIX_ERR_NOT_SUPPORTED and the documented PMIX_GROUP_LEFT
   event was never generated. A voluntary leave now generates a
   PMIX_GROUP_LEFT event notifying the remaining members of the caller's
   departure (returning success once the event is locally generated, per
   the API contract), updates group membership on the members and in the
   host environment, and completes any in-flight group construct/destruct
   collective on the survivors rather than hanging
 - Resolve a status-code value collision: PMIX_ERR_LOST_PRECISION and
   PMIX_ERR_CHANGE_SIGN shared the values -400 and -401 with the
   PMIX_ERR_PROC_KILLED_BY_CMD and PMIX_ERR_PROC_FAILED_TO_START event
   codes, making the two indistinguishable to event handlers, status
   comparisons, and PMIx_Error_string. The two conversion-error codes
   have been renumbered to -202 and -203
 - gds/shmem2: make fixed-address segment attach reliable, fixing an
   intermittent spawn-time PMIx_Init failure (PMIX_ERR_PACK_MISMATCH)
   seen under AddressSanitizer and during MPI_Comm_spawn
 - gds: when a client cannot attach a gds/shmem2 fixed-address segment
   at runtime, it now gracefully falls back to the next available GDS
   module (typically hash) and re-requests its job data, instead of
   failing PMIx_Init. GDS module selection is now tracked per-client
   rather than per-namespace, so one client falling back does not affect
   its peers
 - plog/stdfd: implement the output-formatting directives for the
   stdout/stderr log channels. PMIx_Log requests that carry
   PMIX_LOG_TAG_OUTPUT, PMIX_LOG_TIMESTAMP_OUTPUT, and/or
   PMIX_LOG_XML_OUTPUT now have their output tagged, timestamped, and/or
   wrapped in XML, matching the formatting used for forwarded stdio.
   These directives were previously accepted but silently ignored
 - iof: fix XML output escaping. An ampersand was emitted as the invalid
   entity "&ap;" instead of "&amp;", and each escaped character left a
   trailing zero byte in the output due to an allocation-size overcount.
   This affected all XML-formatted stdout/stderr output
 - plog/smtp: fix the "body_suffix" MCA parameter, which was mistakenly
   registered under the name "body_prefix" and so could not be set
 - configure now refuses to build PMIx with a compiler that does not
   provide C11 atomics, instead of quietly reconfiguring for C99 and
   carrying on. That fallback could never have worked: the library uses
   _Atomic, the C11 atomic convenience types and <stdatomic.h>
   unconditionally, so a C99 build failed on the first translation unit
   with unknown-type errors that named neither C11 nor the compiler. The
   required-atomics check was also widened from a sample of four
   primitives to the full set the code actually uses, and each failure
   now names the missing primitive
 - Fixed the device distances reported by PMIx_Compute_distances, which
   always came back with mindist equal to maxdist. The two fields exist
   to describe a process bound to more than one location where those
   locations sit at different distances from the device, but the
   distance was measured from the single lowest object covering the
   whole cpuset rather than from each location, so it did not vary and
   the documented case could not be expressed. A binding that spans
   locations at differing distances now reports a real range. A cpuset
   that matches no location in the topology reports UINT16_MAX - the
   "distance unknown" sentinel - in both fields, rather than a minimum
   of UINT16_MAX with a maximum of zero
 - Fixed PMIx_Value_get_size returning a wrapped, meaningless size for a
   PMIX_PROC_CPUSET value. The size was computed from the weight of a
   filled hwloc bitmap - which is infinitely set, so hwloc returns -1 -
   giving SIZE_MAX regardless of the cpuset. Callers sizing an array of
   cpusets overflowed their running total. The reported size now
   reflects the cpuset's serialized form
 - Fixed a stack buffer overflow when printing a topology. Each object's
   cpuset was rendered into a buffer half the size of the length passed
   to hwloc, so a machine with enough processing units to exceed the
   buffer's real size overwrote the stack
 - Fixed a crash in PMIx_Parse_cpuset_string when passed a NULL string,
   and added the same screening to PMIx_Get_cpuset and
   PMIx_Compute_distances. A failed parse no longer leaves a partially
   built cpuset on the caller's struct
 - Fixed the hwloc shared-memory topology segment not being reclaimed
   when a server finalized: the cleanup was gated on a condition only a
   client could satisfy, so the server that created the segment leaked
   its descriptor and never removed the backing file itself
 - Removed the internal header src/include/pmix_hash_string.h. Nothing
   in PMIx or PRRTE used either macro it defined

6.0.0 -- TBD
------------
.. important:: This is the first release in the v6 family. The intent
               for this series is to provide regular "reference tags",
               effectively serving as milestones for any development
               that might occur after the project achieved a stable
               landing zone at the conclusion of the v5 series. It
               is expected, therefore, that releases shall be infrequent
               and rare occurrences, primarily driven by the completion
               of some significant feature or some particularly
               critical bug fix.

               For this initial release, that feature is completion of
               the Group family of APIs. This includes support for all
               three of the group construction modes, including the new
               "bootstrap" method. A description of each mode can be
               found in the :ref:`Group Construction <group-construction-label>`
               section of the documentation.

               A few notes:

               (1) Proper execution of the various group construction
               modes requires that the host provide the necessary backend
               support. Please check with your host provider to ensure it
               is available, or feel free to use the PMIx Reference
               RunTime Environment (`PRRTE <https://github.com/openpmix/prrte/releases>`_) - you will require v4.0 or above.

               (2) Starting with this release, PMIx requires
               Python >= v3.7 to build a Git clone (ie., not a tarball).
               Certain elements of the code base are constructed at build
               time, with the construction performed by Python script. The
               constructed elements are included in release tarballs.

               (3) PRRTE < v4.0 is not compatible with PMIx >= v6.0 due
               to internal changes (e.g., show-help messages are now
               contained in memory instead of on-disk files).

A full list of individual changes will not be provided here,
but will commence with the v6.0.1 release.
