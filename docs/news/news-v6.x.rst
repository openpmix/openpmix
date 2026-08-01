PMIx v6.x series
================

This file contains all the NEWS updates for the PMIx v6.x
series, in reverse chronological order.

6.1.1 -- xx May 2026
--------------------
Detailed changes since v6.1.0:
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
