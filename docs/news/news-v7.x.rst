PMIx v7.x series
================

This file contains all the NEWS updates for the PMIx v7.x
series, in reverse chronological order.

7.0.0 -- TBD
------------
Detailed changes since v6.1.0:
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
