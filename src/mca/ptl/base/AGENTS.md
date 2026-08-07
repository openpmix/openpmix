<!--
  Copyright (c) 2026      Nanook Consulting  All rights reserved.

  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
-->

# AGENTS.md: The PTL base

This document orients AI agents and human contributors working in
`src/mca/ptl/base` — the implementation of the PMIx Transport Layer.
Read the top-level [`AGENTS.md`](../../../../AGENTS.md) and then the
framework's [`AGENTS.md`](../AGENTS.md) first: the golden rules, the
caddy/thread-shift model, and the (unusual) fact that `ptl`'s components
are role selectors rather than implementations all come from there and
are not repeated. **This directory is where essentially all of `ptl`
lives** — sockets, framing, the handshake, the listener, rendezvous
files, teardown. The `client/`, `server/` and `tool/` component
directories together are under 250 lines; this one is nearly 6000.

## The files

| File | Owns |
|------|------|
| [`base.h`](base.h) | the internal API and the `pmix_ptl_base` global-state struct |
| [`ptl_base_handshake.h`](ptl_base_handshake.h) | the connector flags and the `PUT_*`/`GET_*` (de)serialization macro pair |
| [`ptl_base_frame.c`](ptl_base_frame.c) | framework open/close, MCA parameters, every `PMIX_CLASS_INSTANCE` in `ptl` |
| [`ptl_base_select.c`](ptl_base_select.c) | role-based component selection |
| [`ptl_base_listener.c`](ptl_base_listener.c) | interface choice, bind, URI publication, rendezvous files, the accept handler |
| [`ptl_base_connection_hdlr.c`](ptl_base_connection_hdlr.c) | the **inbound** half: parse a connect-ack, build a peer, tell the host |
| [`ptl_base_connect.c`](ptl_base_connect.c) | the **outbound** half: blocking socket helpers, `connect()`, the tool discovery matrix |
| [`ptl_base_fns.c`](ptl_base_fns.c) | string/URI parsing, connect-ack construction, the client and tool handshakes, server discovery |
| [`ptl_base_sendrecv.c`](ptl_base_sendrecv.c) | steady-state send/recv handlers, tag matching, `lost_connection` |
| [`ptl_base_stubs.c`](ptl_base_stubs.c) | version comparison and the notification recv registration |
| [`help-ptl-base.txt`](help-ptl-base.txt) | this framework's `show_help` topics |

The split between `ptl_base_connect.c` and `ptl_base_fns.c` is
historical rather than principled — the outbound connection path runs
through both, and `pmix_ptl_base_make_connection` (in `fns.c`) calls
`pmix_ptl_base_connect` (in `connect.c`). Follow the call graph, not the
file names.

## Two wire formats, and they are not the same

This is the single most common thing to get wrong here.

### The steady-state message header

`pmix_ptl_hdr_t` (in [`../ptl_types.h`](../ptl_types.h)) followed by
`nbytes` of payload. On this path — `pmix_ptl_base_send`,
`pmix_ptl_base_send_recv`, `PMIX_SERVER_QUEUE_REPLY` out;
`pmix_ptl_base_recv_handler` in — **every header integer is converted
with `htonl`/`ntohl`.** This is the one place `ptl` is endian-correct on
the wire.

### The connect-ack

The very first message on a new socket is *not* framed that way. The
same `pmix_ptl_hdr_t` struct is used, but `construct_message` writes
`hdr.nbytes` in **host** byte order and
`pmix_ptl_base_connection_handler` reads it back the same way. Do not
"fix" one side in isolation: the two are consistent with each other and
with every released version, and changing either breaks the
interoperability guarantee. (The fields *inside* the payload are
network-order — `PMIX_PTL_PUT_U32`/`GET_U32` do the conversion.)

The payload layout, built by `construct_message` in `ptl_base_fns.c` and
parsed by `pmix_ptl_base_connection_handler`:

| # | Field | Macro |
|---|-------|-------|
| 1 | psec module name | `PUT_STRING` / `GET_STRING` |
| 2 | credential length | `PUT_U32` / `GET_U32` |
| 3 | credential bytes | `PUT_BLOB` / `GET_BLOB` |
| 4 | connector flag (`pmix_rnd_flag_t`) | `PUT_U8` / `GET_U8` |
| 5 | flag-dependent identity: uid/gid and/or procid | see the switch in both files |
| 6 | version string | `PUT_STRING` / `GET_STRING` |
| 7 | bfrops module name | `PUT_STRING` / `GET_STRING` |
| 8 | buffer type | `PUT_U8` / `GET_U8` |
| 9 | gds module name | `PUT_STRING` / `GET_STRING` |
| 10 | optional packed `pmix_info_t` blob (the rest of the message) | `PUT_BLOB` / `GET_BLOB` |

**This order is frozen.** Per the top-level interoperability rules it is
append-only — never insert, never reorder — and the `PUT_*`/`GET_*` pairs
are deliberately written to be read side by side so a change to one that
is not mirrored in the other is visible. `test/unit/ptl_handshake.c`
drives the pair directly and will catch the drift.

A 2.0 peer is a special case in the parser: its handshake ends at field
6, so the handler synthesizes the remaining values and sets the
remaining count to zero.

### Everything in the connect-ack is untrusted

The connection handler runs **before** the credential is validated.
Anything that can open a TCP connection to the listener gets to drive
these macros. Two consequences:

- The message size is bounded by `PMIX_MAX_CRED_SIZE` before it is
  allocated.
- Every `GET_*` macro bounds itself against `cnt`, the bytes remaining.
  `GET_BLOB` in particular must check the caller's length against `cnt`:
  the credential length is a `uint32_t` off the wire, and a peer that
  claims more than it sent would otherwise read past the buffer *and*
  underflow `cnt`, which then lets every later field read out of bounds
  too. If you add a field, bound it the same way, and add the truncated
  case to `test/unit/ptl_handshake.c`.

Similarly, the peer's version string is parsed by
`pmix_ptl_base_parse_version`, which tolerates any number of components
— a peer is under no obligation to send three.

## Inbound: accept to connected peer

Four functions, in this order, all on the progress thread:

1. **`connection_event_handler`** (`ptl_base_listener.c`) — `accept()`,
   wrap the fd in a `pmix_pending_connection_t` (`pnd`), post it. It
   does the minimum on purpose: a slow accept loop makes the OS start
   refusing connections.
2. **`pmix_ptl_base_connection_handler`** (`ptl_base_connection_hdlr.c`)
   — flips the socket to **blocking**, reads the connect-ack, parses it
   with the `GET_*` macros, and then splits:
   - a **simple client or singleton** must already be a registered
     nspace+rank; the handler builds the peer inline;
   - anything else is a tool/launcher/scheduler and goes to
     `process_tool_request`.
3. **`process_tool_request`** → the host's `tool_connected`/
   `tool_connected2` → **`cnct_cbfunc`** → thread-shift →
   **`process_cbfunc`**. This is the two-step path: the host may assign
   the tool a namespace, which is why the namespace object cannot be put
   on `pmix_globals.nspaces` until `process_cbfunc` runs.
4. **`_cnct_complete`** (client path) or the tail of `process_cbfunc`
   (tool path) — reply with the status and the peer's array index, run
   the psec server handshake if the module asked for one, set the socket
   non-blocking, arm the recv/send events, and flush cached
   notifications.

### Ownership along that path — read this before editing

Most of the historical bugs in this directory are refcount and
list-membership mistakes on the failure paths. The invariants:

- **`pnd` belongs to whoever is going to release it.** The connection
  handler releases it on its own `error:` path; on success it hands
  ownership to the `cnct_hdlr_t` caddy (whose destructor releases it) or
  to `process_cbfunc`. When you add an early exit after the caddy exists,
  null `ch->pnd` first if the error path below is also going to release
  it — the caddy destructor would otherwise release it a second time.
- **`pnd->info` is freed by `pnd`'s destructor.** Never free it by hand,
  and never `PMIX_RELEASE` it — it is a `pmix_info_t` array, not an
  object.
- **A namespace the handler created is not on any list yet.** Do not
  `pmix_list_remove_item` it from `pmix_globals.nspaces` on a failure
  path; just drop the reference. A namespace that already existed *is* on
  that list and the list holds its own reference — the peer's retain is
  the only one you may give back.
- **Take exactly one namespace/rank-info retain for the peer.** Releasing
  the peer gives it back. Nulling `peer->nptr` or `peer->info` to "protect"
  a shared object strands that retain instead.
- **The rank's `proc_cnt` and `peerid` are shared bookkeeping.** The
  connection handler raises `proc_cnt` and points `peerid` at the new
  clients-array slot. Any failure after that point must give both back —
  `proc_cnt == 0` is the gate on reclaiming a finalized tombstone peer
  when the rank reconnects, so a count that never drops leaks one peer
  per later reconnect.
- **`info` is a member of the namespace's rank list**, not something the
  handler owns. Releasing it directly drives its refcount to zero while
  it is still linked.

## Outbound: connect_to_peer to connected peer

`pmix_ptl_base_connect_to_peer` (`ptl_base_connect.c`) is the tool and
server path; the `client` component has its own, simpler one. Both
converge on `pmix_ptl_base_make_connection` (`ptl_base_fns.c`):

```
setup_connection()          parse "tcp4://host:port" into a sockaddr
pmix_ptl_base_connect()     socket() + connect(), retrying
send_connect_ack()          construct_message() builds the blob
recv_connect_ack()          client or tool handshake, per our own role
                            assign the LOWER half of the dynamic tag space
```

then `pmix_ptl_base_complete_connection` records the server and arms the
steady-state events.

The discovery matrix in `connect_to_peer` is the bulk of that file: a
caller-specified connection **order**, an explicit URI, a rendezvous or
attachment file, a server pid, a server nspace, or a directory search of
the session tmpdir. Two things to keep straight:

- **Every branch must produce a URI before jumping to `complete:`.**
  Falling through with a NULL `suri` reaches `setup_connection`, which
  dereferences it. "Optional" governs whether a failure is worth
  complaining about, not whether we have an address.
- **The info array the caller passed is theirs.** Copy before you carve
  a string up; a caller may legitimately hand us a string literal.

## Steady state

`ptl_base_sendrecv.c`. The framework doc describes the flow; the details
that bite:

- **Tag space is split in half per connection** so the two ends of a
  bidirectional socket never collide. The side that *accepted* takes the
  upper half (set in the connection handler), the side that *initiated*
  takes the lower half (set in `make_connection`). `UINT_MAX` is the
  wildcard the server posts to catch every client command.
- **A dynamic-tag recv is one-shot**; `process_msg` removes it after
  firing. The reserved tags below `PMIX_PTL_TAG_DYNAMIC` are persistent
  and are never removed.
- **`lost_connection` must only complete the dynamic-tag recvs.** It
  hands them an empty buffer so blocked `SEND_RECV` callers unwind. The
  persistent recvs (notification, IOF, IOF flow control) have nobody
  waiting on them; giving them that buffer only makes them fail to
  unpack a message that was never sent.
- **Loopback bypasses the socket entirely.** A send whose peer is
  `pmix_globals.mypeer` goes straight to `PMIX_ACTIVATE_POST_MSG`.
- **`send_msg` handles partial writes** by tracking `hdr_sent` plus the
  `sdptr`/`sdbytes` cursor; `EAGAIN` returns `PMIX_ERR_RESOURCE_BUSY` and
  the event refires. Do not simplify this into a single `write`.

## The listener

`pmix_ptl_base_setup_listener` picks an interface (loopback by default;
a public one only if remote or tool connections were asked for), binds,
publishes the URI into `gds`, and drops rendezvous files.

- **The port scan opens a socket per attempt.** Close it before moving to
  the next port, and detect the case where the whole range is taken —
  `listen()` on an unbound socket succeeds and silently gets a port of
  the kernel's choosing, which is not what the user asked for.
- **`sockerror:` returns `rc`.** `rc` starts at zero, so every path that
  jumps there must set a real status first or the caller is told the
  listener came up.
- **`created_*` bookkeeping must stay honest.** Every file and directory
  the listener creates is recorded so `pmix_ptl_close` removes exactly
  what we made and nothing a peer owns.
- **A stale rendezvous file is reclaimed, not fatal.** `write_rndz_file`
  reads the pid the existing file records; if that process is gone the
  orphan is unlinked and replaced, and if it is alive we fail with the
  `rndz-file-in-use` topic and `PMIX_ERR_SILENT` so the generic "listener
  thread failed to start" message does not bury it.
  `test/unit/rndz_stale.c` pins both halves.

## Threading

Two regimes, described in the framework doc. What matters *here*:

- `pmix_ptl_base_send_blocking` / `_recv_blocking` and everything in the
  connect-ack exchange are genuinely blocking. That is acceptable only
  because they run at init, before the peer is in steady-state traffic —
  and because the inbound side sets a receive timeout
  (`pmix_ptl_base_set_timeout`) so a peer that connects and says nothing
  cannot wedge the progress thread forever.
- The file-wait loops (`pmix_ptl_base_parse_uri_file`, `check_server`)
  sleep on a local `pmix_lock_t` armed by an evtimer rather than
  spinning. Every one of those loops needs its own
  `PMIX_CONSTRUCT_LOCK` — the lock is a stack object and waiting on an
  unconstructed one is undefined.

## Tests

| Test | Covers |
|------|--------|
| `test/unit/ptl_uri.c` | URI/version parsing and version comparison, including every malformed input |
| `test/unit/ptl_handshake.c` | the `PUT_*`/`GET_*` pair as a round trip, plus truncated-field rejection |
| `test/unit/rndz_stale.c` | reclaiming (or refusing to reclaim) a rendezvous file |
| `test/unit/tool_cycle.c`, `client_cycle.c` | repeated connect/finalize cycles through this code |
| `contrib/dockerswarm/run-ptl-tests.sh` | the paths a single node cannot reach: tools connecting across nodes, discovery by pid/nspace, remote-connection interface selection |

### Two things left alone on purpose

Both are visible from this directory and both look like defects at first
glance. Neither is one to "fix" without agreement:

- **A self-started tool's namespace never joins `pmix_globals.nspaces`.**
  `process_tool_request` takes two references on a namespace it creates
  — one for the peer, one on behalf of that list — and `process_cbfunc`
  appends it only on the tool-that-is-already-a-client path. For a tool
  that asked for an identity, the second reference is simply held. That
  looks like a leak, but a dozen other places in the library (the `gds`
  hash utilities among them) will find-or-create a namespace on that
  list, at which point the reference becomes the one the list holds.
  Deciding which of those is the intended owner is a question for the
  team, not a local edit: the alternative reading is that
  `process_tool_request`'s comment is right and the append is missing,
  and `process_tool_request`'s "a prior instance of the tool already
  connected, so we would know the nspace" branch only makes sense under
  that reading.

- **`tool/ptl_tool.c` calls the public `PMIx_Job_control_nb`** to register
  its rendezvous files for cleanup, which the top-level rules say
  back-end code must not do. It is safe today — `setup_listener` runs on
  the caller's thread during `PMIx_tool_init`, so the thread-shift the
  API performs is a post rather than a re-entry — but it is a deviation,
  and if that code ever moves onto the progress thread it becomes a
  deadlock.

Anything in here that can be expressed as a pure function of its inputs
should get a unit test — the parsers and the handshake macros both
could, and did, hide out-of-bounds reads for years because nothing
exercised them with input the library had not written itself.

## Building

All of `base/` compiles into `libpmix` unconditionally; there is no
`configure.m4` here. A plain `make` from the repo root is enough for a
source change. `help-ptl-base.txt` is the exception the top-level golden
rule calls out: after **any** edit to it you must

```sh
rm src/util/pmix_show_help_content.*
make
```

or the library keeps emitting the old text.
