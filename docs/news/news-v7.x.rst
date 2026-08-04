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
