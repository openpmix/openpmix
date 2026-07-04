<!--
  Copyright (c) 2026      Nanook Consulting  All rights reserved.

  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
-->

# AGENTS.md: The PTL Framework

This document orients AI agents and human contributors working in the
`ptl` (**P**MIx **T**ransport **L**ayer) framework. It assumes you have
already read the top-level [`AGENTS.md`](../../../AGENTS.md) — the golden
rules, prefix conventions, thread-safety/caddy model, and MCA concepts
described there all apply here and are not repeated. This file covers
what is specific to `ptl`: what the framework is for, the (unusual) way
its components are selected, the wire protocol, how a connection is
established from both ends, and how steady-state messages flow. Each
component subdirectory (`client/`, `server/`, `tool/`) carries its own
`AGENTS.md` with role-specific detail. For an integration-level view of
how this framework connects a PMIx process to its server, see
[`docs/how-things-work/ptl.rst`](../../../docs/how-things-work/ptl.rst).

## What PTL does

`ptl` is the socket layer that carries every PMIx message between a
process and the PMIx server it talks to. It owns four things:

1. **Connection establishment** — discovering *where* the server is (an
   env var, an explicit URI, or a rendezvous file dropped in a tmpdir),
   opening the TCP socket, and running the identity/security **handshake**
   that both sides must agree on before any real traffic flows.
2. **The listener** — on the server/tool side, selecting a network
   interface, binding a listen socket, publishing its URI, and dropping
   the rendezvous files that let clients and tools find it.
3. **Message framing and transport** — a fixed header plus payload, sent
   and received on the libevent progress thread, with a posted-recv/tag
   matching scheme for delivering replies.
4. **Connection teardown** — detecting a dropped peer and unwinding all
   the state (events, collectives, notifications) that depended on it.

Everything above the socket — packing/unpacking, security credentials,
datastore selection — belongs to other frameworks (`bfrops`, `psec`,
`gds`). `ptl` is purely the pipe and the rules for setting it up.

## The single most important structural fact

`ptl` is a **single-select** framework (only one component is active at a
time), but it does **not** work the way most single-select frameworks do.

In a typical framework the component *is* the implementation and the
`base/` directory is just plumbing. In `ptl` it is the reverse: **the
base owns essentially all of the code** — TCP sockets, framing, the
handshake, the listener, interface discovery, rendezvous files,
send/recv, teardown — and the three components (`client`, `server`,
`tool`) are extremely thin *role selectors*. A component is little more
than a `pmix_ptl_module_t` whose function pointers mostly point straight
back at `pmix_ptl_base_*` functions in `base/`.

Consequently:

- **Components partition by process role, not by transport.** Only one
  wins because `component_query` gates on `pmix_globals.mypeer`'s type
  (see the selection table below), and the gates are mutually exclusive.
  The `client` component takes a *pure client*; the `tool` component
  takes *any tool* (including a launcher, which is tool+server); the
  `server` component takes a *pure server*.
- **The component priorities are effectively vestigial.** Because the
  gates never overlap, at most one component ever returns a module, so
  the "highest priority wins" logic in `pmix_ptl_base_select` never
  actually arbitrates between two candidates.
- **The header comment in `ptl.h` describes an aspiration, not today's
  reality.** It says components exist to support new handshake versions
  and new transports. In practice there is one transport (TCP, protocol
  `PMIX_PROTOCOL_V2`); the old Unix-domain-socket transport (`usock`,
  `PMIX_PROTOCOL_V1`) is gone except for the vestigial `base/usock.h`.
  Handshake versioning is handled *inside* the base by inspecting the
  peer's version string, not by swapping components.

If you are looking for "the TCP code," it is in `base/`, not in a
component. Read `base/` first.

## Module interface (`pmix_ptl_module_t`)

Defined in [`ptl.h`](ptl.h). A component fills in the subset of these it
needs; the rest stay `NULL`.

| Field | Signature | Who sets it | Purpose |
|-------|-----------|-------------|---------|
| `name` | `char *` | all | component name string |
| `init` / `finalize` | `(void)` | none today | per-module lifecycle hooks (unused) |
| `recv` | register a persistent recv | none today | vestigial — posted recvs are managed directly by the base |
| `cancel` | cancel a persistent recv | none today | vestigial |
| `connect_to_peer` | `(peer, info, ninfo, &suri)` | **all three** | open + handshake a connection *up* to a server |
| `query_servers` | `(dirname, list)` | none today | vestigial — server discovery runs through `pmix_ptl_base_query_servers` |
| `setup_listener` | `(info, ninfo)` | server, tool | bind a listen socket and publish rendezvous |
| `setup_fork` | `(proc, &env)` | server, tool | inject `PMIX_*_TMPDIR` env into forked children |

Note how much is `NULL`: `init`, `finalize`, `recv`, `cancel`, and
`query_servers` are never wired by any current component. Do not assume a
non-`NULL` value; the base guards these paths in other ways. The only
functionally distinct component implementation is the `client`
component's *own* `connect_to_peer` (a simpler client-only path) versus
the shared `pmix_ptl_base_connect_to_peer` used by `server` and `tool`.

The selected module is copied into the exported global
**`pmix_ptl`** (in `base/ptl_base_frame.c`) during selection; all
back-end code reaches the framework through that global and through the
`PMIX_PTL_*` macros in [`ptl_types.h`](ptl_types.h).

## Selection and lifecycle

- **`base/ptl_base_frame.c`** declares the framework
  (`PMIX_MCA_BASE_FRAMEWORK_DECLARE`), instantiates the global
  `pmix_ptl_base` state struct and the `pmix_ptl` module, registers all
  the MCA parameters (`pmix_ptl_register`), opens components
  (`pmix_ptl_open`), and defines the `PMIX_CLASS_INSTANCE`s for the many
  transport objects (`pmix_ptl_send_t`, `pmix_ptl_recv_t`,
  `pmix_ptl_posted_recv_t`, `pmix_ptl_sr_t`, `pmix_ptl_queue_t`,
  `pmix_pending_connection_t`, `pmix_listener_t`, `pmix_connection_t`).
  `pmix_ptl_close` is where all the rendezvous files and tmpdirs created
  by the listener are removed.
- **`base/ptl_base_select.c`** (`pmix_ptl_base_select`) queries each
  component, keeps the highest-priority module whose `init` (if any)
  succeeds, and copies it into `pmix_ptl`. If none is selected it emits
  the `no-plugins` `show_help` topic and returns `PMIX_ERR_SILENT` —
  `ptl` requires exactly one component.

Selection table (from each component's `component_query`):

| Component | Priority | Selected when the local peer is… |
|-----------|----------|----------------------------------|
| `client`  | 50 | a client **and not** a tool (a "pure" client) |
| `tool`    | 40 | any tool (debugger, launcher, or client-tool) |
| `server`  | 30 | a server **and not** a tool (a "pure" server) |

Remember these conditions are exclusive, so priority order never breaks a
tie in practice. A **launcher** (`PMIX_PROC_LAUNCHER` = tool+server) is a
tool for selection purposes and therefore uses the `tool` component —
which is why the `tool` module carries `setup_listener` (a launcher must
*listen* for its own children while also *connecting up* to its server).

## Core data structures ([`ptl_types.h`](ptl_types.h))

### Message header and tags

```c
typedef struct {
    int32_t  pindex;   // sender's process index (peer array slot)
    pmix_ptl_tag_t tag;  // matches a send to its posted recv
    uint32_t nbytes;   // payload length following the header
#if SIZEOF_SIZE_T == 8
    uint32_t padding;  // keeps the header 8-byte aligned
#endif
} pmix_ptl_hdr_t;
```

Every message is `hdr` followed by `hdr.nbytes` of payload. Header
integer fields travel in **network byte order** (`htonl`/`ntohl`) — this
is the one place `ptl` must be endian-correct on the wire.

Tags (`pmix_ptl_tag_t`, a `uint32_t`) route a message to the right
handler:

- **Reserved/persistent** tags: `PMIX_PTL_TAG_NOTIFY` (0),
  `PMIX_PTL_TAG_HEARTBEAT` (1), `PMIX_PTL_TAG_IOF` (2). A persistent recv
  posted on one of these fires for every matching message and is never
  removed.
- **Dynamic** tags start at `PMIX_PTL_TAG_DYNAMIC` (100) and are handed
  out one per `SEND_RECV`. A dynamic-tag recv is **one-shot** — it is
  removed after the reply fires.

A subtle but load-bearing detail: the dynamic-tag space for a peer is
**split in half** so the two ends of a bidirectional socket never collide
on a tag. The side that *accepted* the connection uses the upper half
(`base/ptl_base_connection_hdlr.c` sets `dyn_tags_start` to the midpoint);
the side that *initiated* it uses the lower half
(`pmix_ptl_base_make_connection` sets it to `PMIX_PTL_TAG_DYNAMIC`).
`UINT_MAX` is reserved as a wildcard/"any tag" recv.

### Transport objects

- **`pmix_ptl_send_t`** — a queued outbound message (its own `hdr` +
  `pmix_buffer_t *data`, plus `sdptr`/`sdbytes` cursor for partial
  writes). Lives on a peer's `send_queue`; the head is the peer's
  `send_msg` "on-deck" slot.
- **`pmix_ptl_recv_t`** — an inbound message under assembly (`hdr` first,
  then a malloc'd `data` region), carried by the peer's `recv_msg`.
- **`pmix_ptl_posted_recv_t`** — a registered interest in `(peer, tag)`
  with a `cbfunc`. Held on the framework-global
  `pmix_ptl_base.posted_recvs` list; matched in `process_msg`.
- **`pmix_ptl_sr_t`** / **`pmix_ptl_queue_t`** — the thread-shift caddies
  behind the `PMIX_PTL_SEND_RECV` and `PMIX_PTL_SEND_ONEWAY` macros. Each
  carries its own `pmix_event_t ev` (required by `PMIX_THREADSHIFT`) and
  a retained `peer`.
- **`pmix_pending_connection_t`** — a freshly `accept`ed but not-yet-
  processed inbound connection (raw socket + everything parsed out of the
  handshake). Built by the listener, consumed by the connection handler.
- **`pmix_listener_t`** — the bound listen socket, its URI, its protocol,
  and the accept `cbfunc`.
- **`pmix_connection_t`** (declared in `base/base.h`) — one parsed
  rendezvous entry (`nspace`, `rank`, `uri`, `version`) produced when
  reading a connection file.

### Process type and version

`pmix_proc_type_t` packs a process's role bit-mask (`PMIX_PROC_CLIENT`,
`PMIX_PROC_SERVER`, `PMIX_PROC_TOOL`, `PMIX_PROC_LAUNCHER`,
`PMIX_PROC_SCHEDULER`, …) with its `major.minor.release` version. The
`PMIX_PEER_IS_*` / `PMIX_PROC_IS_*` macros test the role bits; the
`PMIX_PEER_IS_Vxx` / `PMIX_PEER_TRIPLET` macros test versions
(255 = wildcard). These roles are what the component `component_query`
functions and the connection handler switch on.

## The public transport macros

Back-end code never calls `send()`/`recv()`. It uses these macros from
`ptl_types.h`, each of which allocates a caddy, retains the peer, and
`PMIX_THREADSHIFT`s onto the progress thread:

- **`PMIX_PTL_SEND_RECV(rc, peer, buf, cbfunc, cbdata)`** — two-way. Posts
  a one-shot recv on the next dynamic tag, then sends `buf`. The reply is
  delivered to `cbfunc`. Runs `pmix_ptl_base_send_recv`.
- **`PMIX_PTL_SEND_ONEWAY(rc, peer, buf, tag)`** — fire-and-forget on a
  fixed `tag`. Runs `pmix_ptl_base_send`.
- **`PMIX_SERVER_QUEUE_REPLY(rc, peer, tag, buf)`** — the server's reply
  path; queues a `pmix_ptl_send_t` directly onto the peer's send queue and
  arms the send event. (This one runs inline in the progress thread — it
  is only ever called from there.)

All three short-circuit to `PMIX_ERR_UNREACH` if the peer is already
`finalized`. The buffer is consumed (freed) by the transport.

## How a message flows (steady state — `base/ptl_base_sendrecv.c`)

**Sending.** `pmix_ptl_base_send` / `_send_recv` build a
`pmix_ptl_send_t`, place it in the peer's `send_msg` on-deck slot (or
append to `send_queue`), and activate the peer's persistent `send_event`.
When the socket is writable, `pmix_ptl_base_send_handler` calls
`send_msg`, which `writev`s the header and payload as one iovec. Partial
writes / `EAGAIN` return `PMIX_ERR_RESOURCE_BUSY` and the event refires
later; a hard error triggers `lost_connection`.

**Receiving.** When the socket is readable,
`pmix_ptl_base_recv_handler` reads the fixed-size header first, bounds-
checks `nbytes` against `pmix_ptl_base.max_msg_size`, allocates the data
region, reads the payload (resuming across `EAGAIN`), then
`PMIX_ACTIVATE_POST_MSG` posts the completed `pmix_ptl_recv_t` to
`pmix_ptl_base_process_msg`.

**Matching.** `process_msg` walks `pmix_ptl_base.posted_recvs` looking for
a recv whose `(peer, tag)` matches (with `UINT_MAX` as wildcard), loads
the payload into a `pmix_buffer_t`, and fires the recv's `cbfunc`. A
dynamic-tag recv is then removed and released. An unmatched message is an
error (`unexpected-message` `show_help`) — by design this subsystem never
receives anything it did not previously ask for.

**Loopback.** A send whose peer is `pmix_globals.mypeer` skips the socket
entirely: the buffer is handed straight to `PMIX_ACTIVATE_POST_MSG` and
matched locally. This is how a server delivers to itself.

**Teardown.** `lost_connection` stops the peer's events, closes the
socket, and — if we are a server — accounts for the departed client in
every collective tracker it was part of (adjusting counts, possibly
completing or forwarding the collective), purges its cached
notifications, and reports `PMIX_ERR_LOST_CONNECTION`. If instead our
*server* died, it completes any in-flight `SEND_RECV`s with an empty
buffer so blocked callers do not hang, then reports the event.

## How a connection is established

### Outbound (a process connecting up to its server)

Entry point is `pmix_ptl.connect_to_peer`, invoked (blocking) during
init. Two implementations:

- **Client** (`client/ptl_client.c`): the simple path. Reads
  `PMIX_SERVER_URI` from the info array or environment (via
  `pmix_ptl_base_set_peer`, which also detects the server's version by
  which `PMIX_SERVER_URIvNN` env var is set); if none is found, tries the
  system rendezvous file and otherwise declares itself a **singleton**.
- **Tool/server** (`base/ptl_base_connect_to_peer`): the full discovery
  matrix. Honors a caller-specified connection **order**
  (`PMIX_CONNECT_SYSTEM_FIRST`, `PMIX_CONNECT_TO_SYSTEM`,
  `PMIX_CONNECT_TO_SCHEDULER`, `PMIX_CONNECT_TO_SYS_CONTROLLER`), an
  explicit URI, a rendezvous/attachment file, a server PID, a server
  nspace, or a directory search of the session tmpdir — collecting our
  pid/uid/gid/cmd-line into an info array to hand the server along the way.

Both converge on **`pmix_ptl_base_make_connection`**:
`setup_connection` (parse the `tcp4://host:port` URI into a
`sockaddr_storage`) → `pmix_ptl_base_connect` (open the socket, retrying)
→ `send_connect_ack` (`construct_message` builds the handshake blob) →
`recv_connect_ack` (run the client or tool handshake) → assign the lower
half of the dynamic-tag space. Then `pmix_ptl_base_complete_connection`
sets the `connected` flag, records the server's nspace/rank, and arms the
recv/send events for steady-state traffic.

### Inbound (a server accepting a client or tool)

The listener's accept handler
(`base/ptl_base_listener.c::connection_event_handler`) does the minimum —
`accept`, wrap the fd in a `pmix_pending_connection_t`, and post it — then
`base/ptl_base_connection_hdlr.c::pmix_ptl_base_connection_handler` does
the real work on the progress thread:

1. Read the handshake header + payload (socket temporarily in **blocking**
   mode), guarded by `PMIX_MAX_CRED_SIZE` / taint limits.
2. Parse it with the `PMIX_PTL_GET_*` macros
   ([`base/ptl_base_handshake.h`](base/ptl_base_handshake.h)): psec name,
   credential, a **flag** (`pmix_rnd_flag_t`, 0–10) identifying the kind
   of connector, the peer's procid/uid/gid, version, bfrops module, buffer
   type, gds module, and an optional info blob. There is a special case
   for a 2.0 peer, whose handshake ends at the version string.
3. Branch on the flag: a **simple client / singleton** must already be a
   registered nspace+rank; a **tool/launcher** goes through the two-step
   `process_tool_request` (which may call the host's `tool_connected` to
   get an nspace assigned, then `process_cbfunc` sends it back).
4. Assign the peer's compat modules (`psec`, `bfrops`, `gds`), validate
   the credential (`PMIX_PSEC_VALIDATE_CONNECTION`), tell the host via
   `client_connected2`, then `_cnct_complete` sends back the status +
   the peer's array index, runs the security handshake if the psec module
   asked for one, arms events, and flushes any cached notifications.

The `PMIX_PTL_PUT_*` (build) and `PMIX_PTL_GET_*` (parse) macro pairs are
deliberately symmetric so the two ends can be read side by side. **The
field order in the handshake is a wire-format contract** — per the
top-level interoperability rules it is append-only; never insert or
reorder.

## The listener (`base/ptl_base_listener.c`)

`pmix_ptl_base_setup_listener` is where a server/tool chooses how it can
be reached:

- **Interface selection.** Enumerate interfaces, filter through
  `if_include`/`if_exclude`, and default to a **loopback** device. Only
  if remote or tool connections were requested does it fall back to a
  public interface; the various "no interfaces" `show_help` topics fire
  when the request cannot be satisfied.
- **Bind.** Try each port from `ipv4_ports` / `ipv6_ports` (default `0` =
  let the kernel choose), set `SO_REUSEADDR` and close-on-exec, `listen`
  with `SOMAXCONN`, and make the socket non-blocking.
- **Publish.** Build the URI `nspace.rank;tcp4://host:port`, store it in
  `gds` under both `PMIX_MYSERVER_URI` and (for older tools)
  `PMIX_SERVER_URI`, optionally emit it via `report_uri`
  (`-`=stdout, `+`=stderr, an integer pipe fd, or a file).
- **Rendezvous files.** Depending on role/flags, drop contact files a
  peer can discover: `pmix.sched.<host>` (scheduler),
  `pmix.sysctrlr.<host>` (system controller), `pmix.sys.<host>` (system
  tool), `pmix.<host>.tool` (session tool), `pmix.<host>.tool.<pid>`, and
  `pmix.<host>.tool.<nspace>`. Each holds `uri / version / pid / uid:gid /
  timestamp`. The `created_*` bookkeeping in `pmix_ptl_base` records which
  of these we made so `pmix_ptl_close` can remove exactly those.

`pmix_ptl_base_start_listening` then registers the listen socket's accept
event on the progress thread.

## MCA parameters (registered in `ptl_base_frame.c`)

All under the `pmix_ptl_base_` prefix, most with deprecated
`pmix_ptl_tcp_` synonyms retained for compatibility:

| Parameter | Meaning |
|-----------|---------|
| `max_msg_size` | cap (MB) on an inbound message; 0 → taint limit |
| `if_include` / `if_exclude` | interface selection (mutually exclusive) |
| `ipv4_ports` / `ipv6_ports` | ports to try when binding the listener |
| `disable_ipv4_family` / `disable_ipv6_family` | skip a whole address family (IPv6 disabled by default) |
| `connection_wait_time` / `max_retries` | how long/often to wait for a server's connection file to appear |
| `handshake_wait_time` / `handshake_max_retries` | timeout/retries on the connect-ack exchange |
| `report_uri` | where to print the listener URI |

Per the top-level guidance, prefer adding an MCA parameter (or, better, an
attribute honored by `connect_to_peer`/`setup_listener`) over hard-coding
a new constant.

## Threading model

Two distinct regimes coexist, and confusing them is the most common way
to break this framework:

1. **Init-time connection is synchronous and blocking.** `connect_to_peer`
   runs in the caller's thread during `PMIx_Init`/`PMIx_tool_init`, does
   *blocking* socket I/O, and returns only once the handshake completes.
   The listener's per-connection handshake likewise flips the socket to
   blocking mode for the duration. This is acceptable precisely because it
   is startup, before the peer is participating in steady-state traffic.
2. **Steady-state I/O is entirely on the progress thread.** Every
   `PMIX_PTL_SEND_*` macro thread-shifts; the send/recv handlers,
   `process_msg`, the accept handler, and the connection handler all run
   on `pmix_globals.evbase`. Shared state
   (`pmix_ptl_base.posted_recvs`, a peer's send/recv queues) is touched
   only there.

The caddies (`pmix_ptl_sr_t`, `pmix_ptl_queue_t`, `pmix_pending_connection_t`,
`cnct_hdlr_t`) all carry the mandatory `pmix_event_t ev` and retain the
peer so it outlives the async hop, exactly as the top-level thread-safety
rules require. The blocking file-wait paths (`pmix_ptl_base_parse_uri_file`,
`check_server`) use a local `pmix_lock_t` + evtimer to sleep on the
progress thread without spinning.

## Directory layout

```
src/mca/ptl/
├── ptl.h                       Framework API: module struct, macros, component struct, version
├── ptl_types.h                 Header, tags, transport objects, proc-type & version macros
├── base/
│   ├── base.h                  Internal base API + the pmix_ptl_base global-state struct
│   ├── ptl_base_handshake.h    PUT_*/GET_* handshake (de)serialization macros + rnd flags
│   ├── ptl_base_frame.c        open/close, MCA params, framework decl, class instances
│   ├── ptl_base_select.c       role-based component selection
│   ├── ptl_base_connect.c      outbound connection: discovery matrix + socket connect
│   ├── ptl_base_connection_hdlr.c  inbound connection: accept → parse handshake → validate
│   ├── ptl_base_listener.c     interface selection, bind, publish URI, rendezvous files
│   ├── ptl_base_sendrecv.c     send/recv handlers, tag matching, lost-connection teardown
│   ├── ptl_base_fns.c          handshake build/parse, URI parsing, file discovery, helpers
│   ├── ptl_base_stubs.c        version comparison + notification-recv registration
│   └── usock.h                 vestige of the removed Unix-socket transport
├── client/                     pure-client role (its own simpler connect_to_peer)
├── server/                     pure-server role (base connect + listener + fork)
└── tool/                       any-tool role (base connect + listener w/ cleanup + fork)
```

## Building

All three components are statically built into `libpmix` and wired
through the generated `base/static-components.h`; none ships a
`configure.m4`, so none is conditionally compiled out. Editing a
`Makefile.am` only needs a plain `make`; adding or removing a *component
directory* changes the build wiring resolved by `configure`, so re-run
`./autogen.pl && ./configure … && make`. `ptl` **does** ship its own
`show_help` file, `base/help-ptl-base.txt` — per the top-level golden
rule, after any add/delete/modify of that text you must
`rm src/util/pmix_show_help_content.* && make` to regenerate the compiled
help content.

## When working in this framework

- **Do not add a component to implement a new transport or handshake
  without team consensus.** The current design puts everything in the
  base and selects components by role; a genuine second transport would be
  a significant departure. Discuss it on a GitHub issue first.
- **Treat the handshake and header layouts as frozen wire formats.**
  Append only, never reorder — a server and client built from different
  PMIx releases must interoperate (see the top-level interoperability
  rules). The `PMIX_PTL_PUT_*`/`GET_*` pairs must stay symmetric.
- **Never touch a peer's queues or the posted-recv list outside the
  progress thread.** Use the `PMIX_PTL_*` macros; they thread-shift for
  you. Only `connect_to_peer`/`setup_listener` run synchronously, and only
  at init.
- **Keep the `created_*` bookkeeping honest.** Every rendezvous file or
  tmpdir the listener creates must be recorded so `pmix_ptl_close` removes
  exactly what we made and nothing a peer owns.
- **Watch byte order.** Header integers and the handshake's `PUT_U32`/
  `GET_U32` fields are network-order on the wire; the payload buffer is
  handled by `bfrops`, not here.
