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

### Version floors

**A server accepts peers from v2.1 on; a client or tool connects only to
a server from v3.2 on.** Both are enforced here, by version, before
anything is read that depends on it.

- The connection handler refuses a peer whose version string says v1.x
  or v2.0 (`unsupported-client-version`), and sends it `PMIX_ERR_OUTDATED`
  in the status slot every client reads first. The old client cannot name
  the code, but it fails instead of carrying on. A v1.x peer cannot reach us
  anyway - it speaks only `usock` - but a v2.0 peer can, through a
  rendezvous file, and its handshake ends at the version string: no wire
  format, no buffer type, no datastore. The buffer type is the fatal
  one. It cannot be inferred, the server's default depends on whether
  *it* was built with debug, and a mismatch misreads the peer's very
  first message. That, plus a get reply the 2.0 client could not unpack
  even with the types forced to agree, is why 2.0 is refused rather than
  served. An unparseable version (major 0) is not refused.
- `pmix_ptl_base_make_connection()` refuses a server known to be older
  than v3.2 (`unsupported-server-version`) before opening a socket. Such
  a server accepts the connection and then leaves the client waiting on
  answers it never sends - `PMIx_Get` of a reserved key hangs. The
  refusal returns `PMIX_ERR_OUTDATED`, which a client's `PMIx_Init` treats
  as a failure rather than falling back to a singleton the way it does
  for a server it could not reach; a tool fails with it unless its
  connection was optional. It has to be its own code: the connect path
  already returns `PMIX_ERR_NOT_SUPPORTED` for a malformed URI, an
  unknown address scheme and a server's own rejection, all of which must
  keep falling back. "Known" matters:
  the version comes from `PMIX_VERSION` or the `PMIX_SERVER_URIxx` name
  for a client, and from the version line of a rendezvous file, whose
  absence means v2.0. A server reached through a URI handed over
  directly has no recorded version, and `PMIX_PEER_IS_EARLIER()` would
  call it "earlier", so the check reads the fields itself rather than
  asking that macro.

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
- The optional trailing `pmix_info_t` blob (field 10) carries its own
  element **count** as the first thing `PMIX_BFROPS_UNPACK` reads out of
  it, and that count is off the wire too. Bound it against the blob's own
  byte length before handing it to `PMIX_INFO_CREATE` — a packed
  `pmix_info_t` is never smaller than a byte, so a count larger than the
  bytes received is malformed, and passing it on either over-allocates or
  returns a NULL array the follow-on unpack then walks off of. **Both**
  the client path (`pmix_ptl_base_connection_handler`) and the tool path
  (`process_tool_request`) parse this blob and both must apply the guard;
  the client path silently lacked it for a time. Check the unpack return
  on each step here as well — a corrupt blob is the normal way these fail.
- **Except one failure, which a released version sends on every
  connection.** v4.1 counted its connector-flag byte twice when sizing
  the connect-ack, so the message ends with one zero byte after the gds
  name. A v4.1 peer that sends no info has that byte where the blob would
  be, and unpacking a count from it reads past the end. Both parsers
  treat `PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER` on the *count* as "no
  blob" (`PMIX_PTL_LEGACY_PAD` in `ptl_base_connection_hdlr.c`); every
  other failure is still refused. Making the count check strict once
  failed every v4.1 client's `PMIx_Init`. Only the `xversion (v4.1)` CI
  leg noticed; `test/unit/ptl_legacy_ack.c` now sends that exact message.

Similarly, the peer's version string is parsed by
`pmix_ptl_base_parse_version`, which tolerates any number of components
— a peer is under no obligation to send three.

## Inbound: accept to connected peer

Four functions, in this order, all on the progress thread:

1. **`connection_event_handler`** (`ptl_base_listener.c`) — `accept()`,
   make the socket non-blocking, wrap the fd in a
   `pmix_pending_connection_t` (`pnd`), put it on
   `pending_connections`, arm the connect-ack timer, and arm a one-shot
   **read** event on the fd. It does the minimum on purpose: a slow
   accept loop makes the OS start refusing connections. See *The inbound
   connect-ack never blocks the server's progress thread*.
2. **`pmix_ptl_base_connection_handler`** (`ptl_base_connection_hdlr.c`)
   — runs each time the socket is readable until the whole connect-ack
   has arrived, then takes the `pnd` off the pending list, flips the
   socket to **blocking** for the replies, parses the connect-ack with
   the `GET_*` macros, and splits:
   - a **simple client or singleton** must already be a registered
     nspace+rank; the handler builds the peer inline;
   - anything else is a tool/launcher/scheduler and goes to
     `process_tool_request`.
3. **`process_tool_request`** → the host's `tool_connected`/
   `tool_connected2` → **`cnct_cbfunc`** → thread-shift →
   **`process_cbfunc`**. This is the two-step path: the host may assign
   the tool a namespace, which is why the namespace object cannot be put
   on `pmix_globals.nspaces` until `process_cbfunc` runs — see
   *One namespace object per name* below, which is the rule that step
   exists to satisfy.
4. **`_cnct_complete`** (client path) or the tail of `process_cbfunc`
   (tool path) — reply with the status and the peer's array index, run
   the psec server handshake if the module asked for one, set the socket
   non-blocking, arm the recv/send events, and flush cached
   notifications.

### The inbound connect-ack never blocks the server's progress thread

`pmix_ptl_base_connection_handler` runs **on the progress thread, before
the credential is checked**, for a socket that anything able to reach the
listener can open. Every client of this server waits while it runs. It
is not "only startup": that is true of the peer connecting, not of the
server, which reaches this code whenever anyone connects. So the
connect-ack is never waited for:

- **The accepted socket is non-blocking**, set in
  `connection_event_handler` before anything reads it. An accepted
  socket inherits the listener's `O_NONBLOCK` on BSD-derived systems and
  never does on Linux, so do not rely on inheritance; a socket that
  cannot be made non-blocking is closed rather than used.
- **The pending connection is a one-shot read event**, not an
  immediately-active one. A peer that connects and sends nothing — a
  port probe, a misbehaving local process, or a tool suspended with ^Z
  between its `connect()` and its first send — costs the progress thread
  nothing, and a peer that closes without sending still wakes the event,
  so the handler's error path reclaims the socket.
- **The handler reads what has arrived and keeps its place.**
  `read_connect_ack` fills `pnd->hdr` and then `pnd->msg`, counting bytes
  in `hdr_recvd`/`msg_recvd`, and answers `PMIX_ERR_WOULD_BLOCK` when the
  socket has nothing more; the handler re-arms the read event and
  returns. The header is judged against `PMIX_MAX_CRED_SIZE` on every
  pass over a complete header - before any payload is allocated from the
  length it names, and again before each resumed read runs to it. Only once the whole connect-ack is in does the handler take the
  payload, put the socket into blocking mode and parse.

A blocking read bounded by a receive timeout is **not** a substitute:
that was the intermediate version, and it still let a peer that sent a
few bytes and stalled freeze the whole server for the length of the
timeout, as often as it cared to reconnect.

**`pmix_ptl_base.pending_connections`** holds every `pnd` whose
connect-ack is still arriving — `pmix_pending_connection_t` is a list
item for this. It is how such a connection is found again, and three
things take it off:

1. the handler, the moment the connect-ack is complete or has failed —
   before anything else can release the `pnd`;
2. `connect_ack_expired`, when `ptl_base_connect_ack_timeout` (seconds;
   default 5; 0 disables it) passes before the whole connect-ack has
   arrived. The listener arms that timer at accept. Without it an idle
   connection costs nothing but a descriptor — but it holds that
   descriptor for as long as it likes, and anything that can reach the
   listener can open more;
3. `pmix_ptl_base_stop_listening`, at finalize, which is what closes
   them when the timeout is disabled. Both finalize paths stop the
   progress thread first, so neither event can be running.

All three go through `pmix_ptl_base_drop_pending_connection` or remove
the item themselves before the `pnd` can be released: releasing a list
item that is still on a list aborts a debug build.

**What is still blocking, and bounded by the same timeout.** Once the
connect-ack is in, the replies to it and the psec server handshake
(`PMIX_PSEC_SERVER_HANDSHAKE_IFNEED`) are blocking exchanges on the same
socket, still before the connection is trusted. The handler sets
`SO_RCVTIMEO` from `connect_ack_timeout` when it puts the socket into
blocking mode, and the option stays until the socket goes non-blocking
for steady-state traffic. That bound only works because
**`pmix_ptl_base_recv_blocking` reports it**: on a socket in blocking
mode, `EAGAIN`/`EWOULDBLOCK` is how `recv()` says an `SO_RCVTIMEO`
expired, not "no data yet", so the function returns `PMIX_ERR_TIMEOUT`
there and cycles only for a socket that really is non-blocking. It used
to cycle unconditionally, which turned every receive timeout into an
unbounded wait — the outbound `handshake_wait_time` never expired
either. The psec exchange itself stays blocking, and deliberately: its
interface is `server_handshake(int sd)`, and the only module that
implements one is the test module `psec/dummy_handshake`, built only
under `--enable-dummy-handshake`. No production build reaches it, so it
is not worth redesigning that interface for; revisit if a real
handshake-model module ever appears.

`test/unit/ptl_stalled_peer.c` runs every stall case with the timeout
**disabled**, so only the non-blocking read can pass them, and gives the
timeout's own job a separate case. `test/unit/ptl_recv_timeout.c` pins
the two readings of `EAGAIN` directly, over a socketpair.

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

### One namespace object per name

The server resolves a namespace by scanning `pmix_globals.nspaces`, so
that list must hold exactly one `pmix_namespace_t` per name and every
peer must be using the object that is on it. Get this wrong and the
failure is quiet: half the library reaches a tool through one object and
half through the other, and whichever half loses sees an empty rank
list.

The tool path is the one place this is hard, because a tool that asks to
be given an identity has no name until the host's `tool_connected`
upcall returns — and that upcall is also the window in which the host
may register a namespace of its own under the very name it just handed
us. So the append is deferred to `process_cbfunc`, and when it happens
it has to reconcile against whatever appeared meanwhile. That is
`consolidate_nspace()`: it appends our object if no other carries the
name, and otherwise moves the peer (and any rank info we built) onto the
one already there and disposes of ours.

Run it for **every** tool we created a namespace for. It used to run
only for a tool the server had already registered as a client, so a
self-started tool's object was never appended at all — reachable only
through its peer, with the reference held for the list stranded, while
`gds/hash`'s find-or-create built a second object under the same name
and put *that* on the list. `test/unit/tool_nspace.c` pins the property
by driving a real tool connection; note that the entry count alone does
not catch it, which the test says in a comment.

**`_check_cached_events()` in this file is one of three replays of the
notification hotel, and it is not the one you are likely to be looking
at.** A newly-connected peer gets whatever it is a target of; so does a
peer every time it registers another handler
(`src/server/pmix_server_events.c`); and the live fan-out
(`_notify_client_event` in `src/event/`) delivers to peers already
registered. One peer can reach all three for the same event, so every
one of them must call `pmix_notify_mark_notified()` and neither send nor
decrement `nleft` when it answers true — otherwise the peer's handler
fires more than once and the event is evicted before its other targets
have seen it. The three must also agree on the wire form of a
`PMIX_NOTIFY_CMD`: command, status, source, info count, info array, and
**the range last** — a tool recipient defaults a missing range to
`PMIX_RANGE_LOCAL`, so omitting it silently downgrades the event. See
the event-registration-store section of
[`src/server/AGENTS.md`](../../../server/AGENTS.md).

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
the session tmpdir. Things to keep straight:

- **Every branch must produce a URI before jumping to `complete:`.**
  Falling through with a NULL `suri` reaches `setup_connection`, which
  dereferences it. "Optional" governs whether a failure is worth
  complaining about, not whether we have an address.
- **The info array the caller passed is theirs.** Copy before you carve
  a string up; a caller may legitimately hand us a string literal.
- **There is one parser for a server URI, and it splits at the last
  `.`.** A URI is `nspace.rank;tcp4://host:port`, and an nspace may
  contain dots of its own (Slurm's are `slurm.pmix.<jobid>.<stepid>`).
  `pmix_ptl_base_parse_uri` searches from the end for that reason. A
  second, inline parser in `connect_to_peer` searched from the front, so
  a tool given `PMIX_SERVER_URI` reached the right address and then
  recorded its server as `slurm`, rank 0 — and because
  `pmix_ptl_base_complete_connection` runs *after* the tool handshake, it
  overwrote the correct identity the handshake had just delivered. Call
  `pmix_ptl_base_parse_uri`; do not split one by hand.
  `test/unit/tool_nspace.c` gives its server a dotted namespace to hold
  this.
- **A list-valued directive may split to nothing.** `PMIx_Argv_split`
  returns NULL for `""` and `","`, so a `PMIX_CONNECTION_ORDER` of either
  used to fault indexing the result. An empty order means no preference.
- **Every directive `connect_to_peer` reads is vetted first, by
  `pmix_ptl_base_check_connect_directives()`, and nothing else in the
  loop validates.** They come straight from the caller of
  `PMIx_tool_init` or `PMIx_tool_attach_to_server`, and the string ones
  used to be read out of the union as pointers whatever they held — a
  `bool` in `PMIX_SERVER_URI`, `PMIX_TCP_URI`,
  `PMIX_TOOL_ATTACHMENT_FILE`, `PMIX_SERVER_NSPACE` or
  `PMIX_CONNECTION_ORDER` was a SIGSEGV. The check covers each
  directive's type, a URI that does not parse (address included — no
  name resolution, so it cannot block), two different servers named by
  nspace, and a connection order entry that is not a connection target.
  It acts on nothing, so a bad value is refused before any
  `pmix_ptl_base` global is overwritten. Add a new directive to it, not
  to the loop.

  **It is exported because "no connection was attempted" has to be
  decidable.** A malformed directive is `PMIX_ERR_BAD_PARAM` even when the
  caller asked for an optional connection, and `PMIx_tool_init` calls the
  check itself, before the attempt, to know that — `connect_to_peer`'s
  status cannot tell it, because a host refusing a tool may answer
  `PMIX_ERR_BAD_PARAM` too. See `src/tool/AGENTS.md`.

  After the check, the loop leaves through `badinput:` only for a failed
  allocation, and that must fail the call too: losing the copy of a URI
  or an attachment file does not stop anything, it silently falls through
  to discovery and can attach the tool to a different server than the one
  it named. `test/unit/tool_api.c` sends each directive a `bool`, both
  with and without `PMIX_TOOL_CONNECT_OPTIONAL`.
- **`pmix_attributes_lookup()` never returns NULL for a name.** It hands
  back the attribute's string value, or the input unchanged when it knows
  no such name — so "is it NULL?" tells you nothing. The connection-order
  check used to ask exactly that, which made its `unknown-attribute`
  diagnostic unreachable: a misspelled entry, an attribute that is not a
  connection target, or an entry with a space after its comma was
  skipped without a word, and the tool connected in some other order than
  the one it asked for. Test the result against the values you accept.
- **`connect_to_peer` hands back `*suriout` on every path**, failure
  included, so every caller frees it whether or not the connection was
  made.
- **`pmix_ptl_base_complete_connection` can fail, and does its failing
  first.** It copies the server's identity into the peer before it sets
  `pmix_globals.connected` or arms an event, and on failure closes the
  socket the handshake just finished on, since nothing will ever service
  it. Keep anything fallible above the `connected` flag.

### Searching a directory for a server

`pmix_ptl_base_df_search` (behind `trysearch` in `connect_to_peer`) and
`query_servers` (behind `PMIX_QUERY_AVAIL_SERVERS`) walk a directory tree
for `pmix.*` contact files. **That tree is the system tmpdir, which
defaults to `$TMPDIR` or `/tmp` — anyone on the node can write there**,
so everything found in it is untrusted, including what kind of file it
is. Three rules, each of which was once broken:

- **Only a real directory is descended into.** An entry is classified
  with `lstat()`; a symbolic link is never followed to a directory. A
  link back up the tree makes the walk revisit everything below it at
  every level until the path length runs out, and two of them make that
  exponential — the walk never came back. A link to a regular file is
  still read.
- **Only a regular file is read.** `fopen()` on a FIFO blocks until
  something opens the other end, so one `pmix.*` FIFO hung every tool
  searching that directory. The walk skips anything that is not a
  regular file, and `open_conn_file()` then opens the file
  `O_NONBLOCK` and checks the *descriptor* with `fstat()`, so the entry
  cannot be swapped for a FIFO between the check and the open. A file
  the caller named explicitly (`PMIX_TOOL_ATTACHMENT_FILE`, a rendezvous
  file) is still opened with plain `fopen()` — naming a FIFO there is
  the caller's own choice.
- **A candidate that cannot be read or parsed is skipped, not fatal.**
  The search used to return the first such failure, which hid any valid
  file `readdir()` happened to list after it — and a server killed while
  writing its file leaves exactly that behind. Only `PMIX_ERR_NOMEM`
  ends the walk.

`test/unit/ptl_search.c` builds a directory holding all three next to
one valid file and drives both walks under a watchdog.

### What a URI parse accepts

- **The port is strict.** `setup_connection` accepts only a whole
  decimal field in 1..65535. It used `atoi()`, so `""` and `"x"` were
  port 0 and `70000` wrapped to 4464 — a mistyped URI connected to some
  other port instead of being refused. The IPv6 branch additionally
  never stepped past the `:` it split at, so every `tcp6://` URI came
  out as port 0; nothing noticed because IPv6 is disabled by default.
- **The rank is deliberately lenient — do not tighten it.**
  `pmix_ptl_base_parse_uri` reads the rank with `strtoull()` and no end
  check. v3.2 servers write the URI with `"%d"`, so a wildcard or
  invalid rank arrives as a negative number, and `strtoull()` wraps it
  back onto the same `pmix_rank_t`. A digits-only parse would refuse
  those servers, which are inside the interoperability floor.

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
- **`lost_connection` completes the lost peer's dynamic-tag recvs, and
  only that peer's.** It hands each an empty buffer so a blocked
  `SEND_RECV` caller unwinds, then takes it off the list. Three ways to
  get this wrong, all of which the code once did:
  - completing *every* peer's recvs. A tool can be attached to several
    servers and switch its primary with `PMIx_tool_set_server`, so the
    list holds recvs for more than one peer. Completing another server's
    recv answers a live request; its real reply then runs the callback a
    second time, on a caddy the first run freed.
  - completing nothing when the lost peer is not `myserver`. That is the
    tool losing a non-primary server, and every request outstanding on
    it waited forever.
  - leaving a completed recv posted. Nothing else removes it, and it
    points at a peer that may be freed and its address reused.

  The persistent recvs (notification, IOF, IOF flow control) have nobody
  waiting on them; giving them that buffer only makes them fail to
  unpack a message that was never sent.
- **A sendrecv that cannot be sent is still answered.**
  `pmix_ptl_base_send_recv` can find the peer's socket already closed —
  the connection dropped after the caller checked `connected`, before the
  thread-shift ran. It gives the callback the same empty buffer instead
  of dropping the request, which left a blocking caller waiting forever.
  The same goes for an allocation failure on that path.
- **A server's request to itself is a real round trip, and its two
  halves share a tag.** A server's `myserver` is its own peer, which has
  no socket, so `pmix_ptl_base_send_recv` posts the request through
  `pmix_ptl_base_post_loopback()` and the switchyard's answer comes back
  the same way — `PMIX_SERVER_QUEUE_REPLY` loops a reply to
  `pmix_globals.mypeer` back instead of queuing it. Each message records
  its kind in `pmix_ptl_recv_t.loopback`, and `process_msg` keeps them
  apart:
  - a `REQUEST` never matches a dynamic-tag recv. The reply recv for it
    is posted first, on the same tag, and prepended, so the request
    would otherwise be handed to the caller as its own answer.
  - a `REPLY` never matches the wildcard. One that nobody waits for
    would otherwise be read as a command, and the error that draws is
    itself a reply to ourselves — the two go round forever. An unmatched
    reply is dropped without an event.

  Three things broke this before: the closed-socket screen ran ahead of
  the loopback branch, so nothing was ever sent; the request matched its
  own reply recv; and the reply sat forever on a peer with no socket to
  send it on. A process with no server half answers a request to itself
  with the empty buffer instead — nobody would read it. Note that every
  API entry point gates on `connected`, which is false whenever
  `myserver` is `mypeer`, so no caller issues one today.
  `test/unit/ptl_loopback.c` pins each piece.
- **A header is read into the message, through its cursor.** The
  receive handler must resume a header that arrived in pieces. It once
  read into a local copy that each call restarted, so the first piece
  was lost and every later header was read from the wrong offset. A
  sender splits a header whenever its kernel buffer fills part-way
  through one — `send_msg` has a branch for exactly that.
- **`flush_sends` must not `FD_SET` a descriptor at or past
  `FD_SETSIZE`.** `FD_SET` does not check, and a server hosting a few
  hundred local procs has descriptors well past 1024. Such a socket just
  waits out the retry interval.
- **Loopback bypasses the socket entirely.** A send whose peer is
  `pmix_globals.mypeer` goes through `pmix_ptl_base_post_loopback()`.
  A one-way send (kind `NONE`) matches like anything off a socket; that
  is how a server receives its own IOF.
- **`send_msg` handles partial writes** by tracking `hdr_sent` plus the
  `sdptr`/`sdbytes` cursor; `EAGAIN` returns `PMIX_ERR_RESOURCE_BUSY` and
  the event refires. Do not simplify this into a single `write`.
- **Two size limits apply before and during a send.**
  - A buffer larger than `UINT32_MAX` is refused before it is queued
    (`PMIX_PTL_MSG_TOO_BIG`, on all three send paths). The header holds
    the payload length in 32 bits; framing one anyway truncated the
    length, and the peer read the rest of the payload as the next
    message. A refused sendrecv is answered with the empty buffer.
  - No single `writev` carries more than `pmix_ptl_base.max_write`
    (`INT_MAX`). macOS refuses a larger one with `EINVAL` instead of
    writing part of it, which dropped the connection on any message over
    2 GB. `send_msg` loops over capped writes, and waits on the socket
    only after a genuinely short one. `read_bytes` caps each `read()`
    the same way. `max_write` exists for `test/unit/ptl_sendrecv.c`,
    which lowers it to drive the chunking with a small message; it is
    not a tuning knob.

## The listener

`pmix_ptl_base_setup_listener` picks an interface (loopback by default;
a public one only if remote or tool connections were asked for), binds,
publishes the URI into `gds`, and drops rendezvous files.

- **With remote connections, every public interface is listened on - but
  the URI still names one.** The scan keeps going past the first public
  interface and `open_alternates()` binds a `pmix_listener_t` on each of
  the others (`pmix_ptl_base.alt_listeners`), trying the primary's port
  first. Where they are is `pmix_ptl_base.alt_uris`, a comma-delimited
  list of address-only `tcpX://host:port` strings, stored as
  `PMIX_MYSERVER_ALT_URIS` and written after everything a released reader
  takes from a rendezvous or report file, on a line tagged
  `PMIX_PTL_ALT_URIS_TAG`. **Never put a second address in the URI itself,
  in `PMIX_SERVER_URI`/`PMIX_MYSERVER_URI`, or in the `PMIX_SERVER_URI*`
  environment variables**: every released parser (`parse_uri` wants exactly
  two `;` fields, `setup_connection` one `host:port`) refuses it, and a
  client refused that way silently runs as a singleton. Likewise never
  write an extra *file* per address - tool discovery compares the URIs of
  every matching file and refuses (`too-many-conns`) when they differ. An
  alternate that cannot be bound is skipped; it costs a remote tool one
  route, not the server its init. The accept handler takes its listener
  from `cbdata`, so the primary and the alternates share it, and
  `pmix_ptl_base_stop_listening` releases the alternates whether or not
  they were ever armed.
- **A tool moves to an alternate only when it cannot connect.** The file
  reader finds the tagged line by its tag, not its position;
  `PMIX_SERVER_ALT_URIS` supplies the same list with an explicit URI.
  `connect_any()` (blocking) and `cnct_begin()` (event-driven) try the
  next address only after `connect()` fails; once a socket is up, the
  handshake's verdict is the server's answer. A primary that does not
  parse is still an error, as it always was; an alternate that does not
  parse is passed over. `test/unit/tool_alt_uris.c` covers both paths,
  a garbled file entry, no alternates, and a bad directive; the listener
  half is in `test/unit/ptl_listener.c` and needs a host with two public
  addresses (it skips otherwise - an IPv6-enabled build on a host with
  global IPv6 addresses exercises it).

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
  `test/unit/rndz_stale.c` pins both halves. The recorded pid must fit
  a `pid_t` before it is handed to `kill()`: `4294967295` narrows to -1,
  and `kill(-1, 0)` asks about every process we may signal, so a garbled
  line always looked like a live owner.
- **An `accept()` failure that stops the listener must stop all of it.**
  Out of descriptors (or an error we do not recognize), the accept
  handler gives up for good — the connection stays in the backlog, so
  the event would otherwise fire again at once. `abandon_listener()`
  deletes the event, clears `active` and closes `lt->socket` through
  `CLOSE_THE_SOCKET`, which sets it to -1. Closing only the descriptor
  the handler was passed left `lt->socket` naming a number the kernel
  reuses, and the stale event and finalize then acted on whatever got
  it. A connection that was reset or hit a network error in the backlog
  is retried like `EAGAIN`; it says nothing about the listening socket.
- **The directives are the host's, and typed by nothing but their key.**
  The string ones (`PMIX_TCP_IF_INCLUDE`/`_EXCLUDE`, `PMIX_TCP_REPORT_URI`,
  `PMIX_SERVER_TMPDIR`, `PMIX_SYSTEM_TMPDIR`) go through
  `replace_string()`, which refuses any other type — a `bool` there was
  a SIGSEGV. A port goes through `port_directive()` and
  `pmix_ptl_base_set_ports()`, and the bind loop parses each entry
  strictly: a number past 65535 used to be narrowed to 16 bits and
  bound, so 70000 listened on 4464.
- **`report_uri` names the file `pmix_ptl_close` removes.** The open
  copies the MCA value into `urifile`, but a `PMIX_TCP_REPORT_URI`
  directive replaces `report_uri` later. The listener therefore records
  the name it actually wrote when it writes it. An empty value reports
  nowhere; it used to read as pipe descriptor 0, and stdin was written
  and closed.

## Framework open and close

`ptl_base_frame.c`. The framework can be opened, closed and opened again
in one process — a server init after a finalize does exactly that — so
three rules apply:

- **`pmix_ptl_close` puts every global back to its initializer value.**
  That covers more than the allocated strings. The listener's role flags
  (`remote_connections`, `tool_support`, `system_tool`, `session_tool`,
  `allow_foreign_tools`, `connections_specified`) are set only from the
  directives the caller passes, never cleared by the listener itself. A
  flag left over from the last cycle once made a second server listen on
  a public interface it never asked for. Add a new flag to the reset.
- **A failed `pmix_ptl_open` must clean up after itself.**
  `pmix_mca_base_framework_open()` answers a failed open by closing the
  framework, but a framework that never reached OPEN is closed *without*
  calling `pmix_ptl_close`. `open_cleanup()` is the unwind; keep it in
  step with what open allocates.
- **Register can run twice without a close in between.** The port
  arrays are built in `pmix_ptl_register`, and a framework that was
  registered but never opened is registered again by the next open.
  `pmix_ptl_base_set_ports()` frees the old array first.

`pmix_ptl_recv_t` owns its `data`. `pmix_ptl_base_process_msg` hands the
payload to a buffer and clears the pointer; every other ending — no recv
for the tag, a recv without a callback, a connection lost mid-message —
releases the message with the payload attached, and the destructor frees
it.

## Threading

Two regimes, described in the framework doc. What matters *here*:

- `pmix_ptl_base_send_blocking` / `_recv_blocking` and everything in the
  blocking connect-ack exchange are genuinely blocking. On the
  **outbound** side that is acceptable only on the caller's own thread -
  `PMIx_Init`, `PMIx_tool_init`, a server connecting upstream.
  `pmix_ptl_base_set_timeout` applies `handshake_wait_time` to each
  reply there, and `bounded_connect()` in `ptl_base_connect.c` applies
  it to each `connect()` attempt; it defaults to 60 seconds (0 means no
  bound). On the **inbound** side it is the server's progress thread:
  the connect-ack itself is read without blocking, and what blocks after
  it is bounded by `connect_ack_timeout` — see *The inbound connect-ack
  never blocks the server's progress thread*. `set_timeout` is not
  called inbound.
- `pmix_ptl_base_set_timeout` only ever *clears* its `sockopt`
  out-parameter, on failure. That is not a bug: the caller initializes it
  to `true`, and it means "restore the saved timeout afterwards".
- **A connect made from the progress thread must not wait on the
  server - use `connect_to_peer_nb`.** `PMIx_tool_attach_to_server`, a
  tool connecting to its parent at init, and a server attaching
  upstream all reach `pmix_tool_retry_attach`, a thread-shift handler.
  It used to run the blocking connect there, so the whole progress
  thread waited on the server - forever, while `handshake_wait_time`
  defaulted to 0. It now calls the module's `connect_to_peer_nb`
  (`pmix_ptl_base_connect_to_peer_nb` for the tool and server
  components; the client component has none and falls back to the
  blocking call). That locates the server exactly as the blocking form
  does - it shares `do_connect()` with it - and then hands the connection
  to the event-driven connect in `ptl_base_fns.c`: a non-blocking
  `connect()`, the same connect-ack sent as the socket accepts it, and
  each reply field read as it arrives, in the order
  `pmix_ptl_base_client_handshake` / `_tool_handshake` read them. One
  timer, `handshake_wait_time`, bounds the whole connect. **The wire
  does not change, so keep the two readers in step**: a field added to
  the blocking handshake has to be added to `cnct_field()` too.
  The callback never runs inside the starting call - the connect begins
  on the next pass of the event loop - and connects still under way at
  finalize are completed with `PMIX_ERR_NOT_AVAILABLE` by
  `pmix_ptl_base_abandon_connects()`. A psec client handshake is still
  run blocking, bounded by the same wait, for the reason given above.
  `test/unit/tool_attach_nb.c` holds this: an event registration made
  while an attach waits on a silent server has to complete promptly.
- **The file-wait loops pause through `retry_wait()`, and nothing else.**
  `pmix_ptl_base_parse_uri_file` pauses between looks at a connection
  file — once while it does not exist yet, and again while it exists but
  is still being written; `check_server` only for the second. A file a
  directory walk has just listed is never waited for as "not there yet":
  it is gone, not late, and `query_servers` runs on the progress thread,
  so `ptl_base_max_retries` × `connection_wait_time` of waiting per
  vanished file would stall everything. The pause is normally an
  evtimer on `pmix_globals.evbase` with the caller parked on a lock the
  timer releases. That cannot work on the progress thread: the timer
  fires only when that thread gets back to its loop, and it is the one
  waiting. All four loops used to open-code that pattern, so an attach
  to an attachment file that did not exist yet deadlocked the tool's
  progress thread, and the caller of the attach with it.
  `retry_wait()` sleeps for the interval instead when it finds itself on
  the progress thread; off it, nothing changed. Do not add a fifth loop
  that open-codes the timer. `test/unit/tool_api.c` holds the attach
  case.

## Tests

| Test | Covers |
|------|--------|
| `test/unit/ptl_uri.c` | URI/version parsing and version comparison, including every malformed input |
| `test/unit/ptl_handshake.c` | the `PUT_*`/`GET_*` pair as a round trip, plus truncated-field rejection |
| `test/unit/ptl_frame.c` | a released message frees its payload, an oversized `max_msg_size` means no limit, port-list fallback, and role flags reset across an init/finalize cycle |
| `test/unit/ptl_sendrecv.c` | lost-connection completion per peer, a split header, a sendrecv to a closed peer or to a tool itself, a message too large to frame, writes split by the cap, and `flush_sends` past `FD_SETSIZE` |
| `test/unit/ptl_legacy_ack.c` | a v4.1-shaped connect-ack, pad byte and all, still connects as a client and as a tool |
| `test/unit/ptl_loopback.c` | a server's request to itself reaches its switchyard and is answered once; a loopback reply nobody waits for is not read as a command |
| `test/unit/ptl_listener.c` | accept out of descriptors stops the listener cleanly; mistyped, out-of-range and empty directives; a directive-named report file is removed; with remote connections every public interface accepts connections and is advertised where older readers do not look |
| `test/unit/tool_alt_uris.c` | a tool whose URI's address is unreachable connects at an alternate - by directive, from a contact file, and through the event-driven attach - and still fails when there is none |
| `test/unit/rndz_stale.c` | reclaiming (or refusing to reclaim) a rendezvous file, including one whose pid does not fit a `pid_t` |
| `test/unit/ptl_search.c` | both tmpdir walks survive a FIFO, symlink loops and unreadable contact files, and still find the valid one |
| `test/unit/ptl_stalled_peer.c` | with no timeout, a server keeps servicing requests past an idle, one-byte or partial connection; a connect-ack in pieces is waited for and parsed, an oversized one refused on its header; finalize closes the unfinished ones; the timeout drops them |
| `test/unit/ptl_recv_timeout.c` | `pmix_ptl_base_recv_blocking` ends a blocking socket's receive at its timeout, and still waits out `EAGAIN` on a non-blocking one |
| `test/unit/tool_nspace.c` | a real tool connection leaves exactly one namespace object, and the peer resolves through the one on the list |
| `test/unit/tool_cycle.c`, `client_cycle.c` | repeated connect/finalize cycles through this code |
| `contrib/dockerswarm/run-ptl-tests.sh` | the paths a single node cannot reach: tools connecting across nodes, discovery by pid/nspace, remote-connection interface selection |

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
