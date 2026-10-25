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
what is specific to `ptl`: what the framework is for, how a process's
role picks the functions it uses, the wire protocol, how a connection is
established from both ends, and how steady-state messages flow.
[`base/AGENTS.md`](base/AGENTS.md) covers the base, which is where nearly
all of the code actually lives and the document to read before changing
any of it. For
an integration-level view of how this framework connects a PMIx process
to its server, see
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

**The base owns essentially all of the code** — framing, the handshake,
the listener, rendezvous files, send/recv, teardown. What a process does
with it depends on its *role*, and the roles are not components: they are
tables of base functions.

- **The role is the process's own type.** `pmix_ptl_base_select`
  (`base/ptl_base_select.c`) looks at `pmix_globals.mypeer` and fills the
  exported global **`pmix_ptl`** (a `pmix_ptl_api_t`, see [`ptl.h`](ptl.h))
  with that role's functions. Back-end code reaches the role through that
  global and through the `PMIX_PTL_*` macros in [`ptl_types.h`](ptl_types.h).
- **The roles, in the order they are tested:**

  | Role | The local peer is… | `connect_to_peer` | `setup_listener` | `setup_fork` | `connect_to_peer_nb` |
  |------|--------------------|-------------------|------------------|--------------|----------------------|
  | client | a client **and not** a tool | `pmix_ptl_base_client_connect_to_peer` | NULL | NULL | NULL |
  | tool | any tool — a debugger, a launcher (tool+server), a client-tool | `pmix_ptl_base_connect_to_peer` | `pmix_ptl_base_tool_setup_listener` | `pmix_ptl_base_setup_fork` | `pmix_ptl_base_connect_to_peer_nb` |
  | server | a server (and not a tool) | `pmix_ptl_base_connect_to_peer` | `pmix_ptl_base_setup_listener` | `pmix_ptl_base_setup_fork` | `pmix_ptl_base_connect_to_peer_nb` |

  A process with none of those bits is refused at select with the
  `no-plugins` `show_help` topic and `PMIX_ERR_SILENT`.
- **The NULLs are part of the contract.** Callers test for them: a
  client's `PMIx_tool_attach_to_server` takes the blocking path because
  `connect_to_peer_nb` is NULL, and a client forks no PMIx children.
- **The role is fixed at select.** A process's type changes after it -
  a client becomes a singleton, a tool becomes a client-tool - but none of
  those changes moves it from one row to another.
- **A tool listens because a launcher must.** A launcher is a tool and a
  server at once: it listens for the children it starts while it connects
  up to its own server. A plain debugger's listener is simply never used.
- **Handshake versioning is done inside the base** by inspecting the
  peer's version string, not by swapping code.

## Transports are the components

`ptl` is a **multi-select** framework, and its components are
transports. `pmix_ptl_base_select` keeps every one whose query succeeds on
`pmix_ptl_base.actives`, highest priority first. Today there is one,
[`tcp/`](tcp/AGENTS.md).

A transport (`pmix_ptl_module_t` in [`ptl.h`](ptl.h)) does only what
depends on the kind of socket:

| Field | What it does |
|-------|--------------|
| `setup_listener` | open the listening socket for a `pmix_listener_t` - bound, listening, non-blocking - and make its URI, acting on any directive meant for it |
| `uri_to_addr` | the address a URI of this transport's scheme names; `PMIX_ERR_NOT_SUPPORTED` for any other scheme, so the next transport is asked |
| `close` | release what it holds, at framework close |

Everything done on a socket once it exists is the base's. The base reaches
the transports through two functions: `pmix_ptl_base_listen` (the
highest-priority transport that listens opens the primary listener) and
`pmix_ptl_base_setup_connection` (the transport whose scheme a URI has
turns it into an address; its callers do not know which).

**Every connection records its transport** in `transport`: a listener
gets it from `pmix_ptl_base_listen`, a pending connection and then the
peer from their listener on the accepting side, and the peer from
`pmix_ptl_base_uri_to_addr` - which reports the transport that took the
URI - on the connecting side, blocking or event-driven.
`test/unit/ptl_transport.c` checks the listener, the server's peer for a
tool, and a tool's peer for its server, by both connects; a pending
connection is covered only by way of the peer it becomes.

**A process's transports are not the user's to choose.** The ptl
selection parameter (`PMIX_MCA_ptl`, or `ptl` in a parameter file) is
cleared in `pmix_ptl_register`, before the components are found, so every
transport available is used whatever it says. Client, server and tool
init also remove `PMIX_MCA_ptl` from the environment, so their children
do not inherit it. `test/unit/ptl_select.c` covers both.

A transport is built into the library, never as a plugin - its
`configure.m4` sets the compile mode to static. Every process needs one,
and tests reach its helpers by name.

## The role interface (`pmix_ptl_api_t`)

Defined in [`ptl.h`](ptl.h).

| Field | Signature | Purpose |
|-------|-----------|---------|
| `name` | `char *` | the role, for verbose output |
| `connect_to_peer` | `(peer, info, ninfo, &suri)` | open + handshake a connection *up* to a server, blocking |
| `connect_to_peer_nb` | `(peer, info, ninfo, cbfunc, cbdata)` | the same driven by events; NULL for a client |
| `setup_listener` | `(info, ninfo)` | bind a listen socket and publish rendezvous |
| `setup_fork` | `(proc, &env)` | inject `PMIX_*_TMPDIR` env into forked children |

The only functions that differ between roles are the client's own
`connect_to_peer` (`base/ptl_base_client.c`, a short path: environment,
directive, else the system server, else a singleton) and the tool's
`setup_listener` wrapper (`base/ptl_base_tool.c`, which would register the
tool's rendezvous files for cleanup with its server — see the comment
there for why that registration is always declined today).

## Lifecycle

- **`base/ptl_base_frame.c`** declares the framework
  (`PMIX_MCA_BASE_FRAMEWORK_DECLARE`), instantiates the global
  `pmix_ptl_base` state struct and the `pmix_ptl` role table, registers all
  the MCA parameters (`pmix_ptl_register`), opens the framework
  (`pmix_ptl_open`), and defines the `PMIX_CLASS_INSTANCE`s for the many
  transport objects (`pmix_ptl_send_t`, `pmix_ptl_recv_t`,
  `pmix_ptl_posted_recv_t`, `pmix_ptl_sr_t`, `pmix_ptl_queue_t`,
  `pmix_pending_connection_t`, `pmix_listener_t`, `pmix_connection_t`).
  `pmix_ptl_close` is where all the rendezvous files and tmpdirs created
  by the listener are removed.
- **`base/ptl_base_select.c`** (`pmix_ptl_base_select`) fills `pmix_ptl`
  for the role, once per open; `pmix_ptl_close` lets the next init select
  again.

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

Every message is `hdr` followed by `hdr.nbytes` of payload. On the
**steady-state** path the header integers travel in **network byte
order** (`htonl`/`ntohl`) — this is the one place `ptl` must be
endian-correct on the wire.

The **connect-ack** is the exception, and it catches people out: the
first message on a new socket reuses the same struct but writes
`nbytes` in *host* order on both ends. That is consistent with every
released version, so it is a contract rather than a bug — see
[`base/AGENTS.md`](base/AGENTS.md) before touching either side.

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
(255 = wildcard). These roles are what `pmix_ptl_base_select` and the
connection handler switch on.

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

**Every one of them can also answer `PMIX_ERR_NOMEM`, and each of them
allocates before it can.** `PMIX_SERVER_QUEUE_REPLY` allocates the
`pmix_ptl_send_t` it queues; the other three allocate the caddy they
thread-shift (`pmix_ptl_sr_t` or `pmix_ptl_queue_t`). All four used to
write through the result without checking it, so one failed allocation
took the process down — in a server, every client it was hosting with
it — for a single message that could not be queued. Callers already
release the buffer on a non-success return, since the finalized peer
above is answered the same way, so reporting the failure costs nothing
at any call site. Note the two are not equivalent to the *caller*,
though: a finalized peer has stopped waiting for the reply, and a peer
whose send could not be allocated has not. See the switchyard section of
[`../../server/AGENTS.md`](../../server/AGENTS.md).

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
dynamic-tag recv is then removed and released. An unmatched message is
**discarded with a framework trace** (verbosity 2) and a `PMIX_ERROR`
event, not a `show_help`: it is unusual, but it is not necessarily a
defect and it is never something a user can act on.

Do not read the matching loop as "we only ever receive what we asked
for". A server posts a **wildcard** (`UINT_MAX`) recv for the command
switchyard, so on a server almost nothing is unmatched — a message on a
tag nobody posted for is handed to `server_switchyard` as if it were a
command. That is why `pmix_server_message_handler` screens the **whole
reserved range** (`PMIX_PTL_TAG_DYNAMIC > hdr->tag`) before dispatching.
No reserved tag is a command, and a reserved-tag message whose own recv
was not posted lands on the wildcard rather than being discarded, so
without the screen it is read as a command, fails, and draws an error
reply on the reserved tag — where nobody is posted, because every
reserved tag is one-way.

**An unanswerable error reply is not a message dropped at the far end.**
If the sender also serves clients, its own wildcard recv catches the
error, fails to read a command out of it, and answers on the same tag;
the two processes then trade unreadable replies at full CPU for as long
as they both live. One key-deletion notice sent to an attached launcher
produced ~2M log lines of exactly that before the screen was
generalized — see `test/unit/tool_delete.c`.

Two tags can reach the screen today. `PMIX_PTL_TAG_HEARTBEAT`:
`psensor/heartbeat` posts its recv only when a monitor is first armed,
and nothing is lost by dropping a beat with no monitor to credit it to.
And `PMIX_PTL_TAG_DATA_DELETE` / `PMIX_PTL_TAG_GDS_UPDATE` if they ever
reach a role that did not post for them — a current client or tool posts
for both, through `pmix_client_post_data_recvs()`, so the screen is the
backstop rather than the mechanism. `PMIX_PTL_TAG_IOF` and
`PMIX_PTL_TAG_IOF_CONTROL` are posted at server init and at
`PMIx_tool_init`, ahead of the wildcard, so they never arrive here;
`PMIX_PTL_TAG_NOTIFY` is unused.

**A new reserved tag needs a posted recv on every role that can be sent
one.** The screen keeps the cost of getting that wrong at "the message
is ignored" instead of "both processes spin forever", but it does not
make the message arrive.

**Loopback.** A send whose peer is `pmix_globals.mypeer` skips the socket
entirely: the buffer is posted straight to the matching code. This is how
a server delivers to itself — one-way messages such as its own IOF, and
a sendrecv round trip, whose reply `PMIX_SERVER_QUEUE_REPLY` loops back
the same way. A request and its reply carry the same tag, so the matching
code tells them apart; see [`base/AGENTS.md`](base/AGENTS.md).

**Teardown.** `lost_connection` stops the peer's events, closes the
socket, and — if we are a server — accounts for the departed client in
every collective tracker it was part of (adjusting counts, possibly
completing or forwarding the collective), purges its cached
notifications, and reports `PMIX_ERR_LOST_CONNECTION`. For any lost peer
it completes the `SEND_RECV`s still waiting on *that peer* with an empty
buffer, so blocked callers do not hang; if the peer was our primary
server it also reports the event. A tool may be attached to several
servers, which is why the completion is per peer — see
[`base/AGENTS.md`](base/AGENTS.md).

## How a connection is established

### Outbound (a process connecting up to its server)

Entry point is `pmix_ptl.connect_to_peer`, invoked (blocking) during
init. Two implementations:

- **Client** (`base/ptl_base_client.c`): the simple path. Reads
  `PMIX_SERVER_URI` from the info array or environment (via
  `pmix_ptl_base_set_peer`, which also detects the server's version by
  which `PMIX_SERVER_URIvNN` env var is set); if none is found, tries the
  system rendezvous file and otherwise declares itself a **singleton**.
- **Tool/server** (`pmix_ptl_base_connect_to_peer`, `base/ptl_base_connect.c`): the full discovery
  matrix. Honors a caller-specified connection **order**
  (`PMIX_CONNECT_SYSTEM_FIRST`, `PMIX_CONNECT_TO_SYSTEM`,
  `PMIX_CONNECT_TO_SCHEDULER`, `PMIX_CONNECT_TO_SYS_CONTROLLER`), an
  explicit URI, a rendezvous/attachment file, a server PID, a server
  nspace, or a directory search of the session tmpdir — collecting our
  pid/uid/gid/cmd-line into an info array to hand the server along the way.

Both converge on **`pmix_ptl_base_make_connection`**:
`setup_connection` (parse the `tcp4://host:port` URI into a
`sockaddr_storage`) → `pmix_ptl_base_server_on_host` (a TCP address that
is not this host's is not dialled: `PMIX_ERR_UNREACH`) →
`pmix_ptl_base_connect` (open the socket, retrying) →
`pmix_ptl_base_check_server` (the far end must be a server we may talk
to, by what the kernel says of it; with no TCP table, one something
vouches for - see `base/AGENTS.md`) → `send_connect_ack` (`construct_message` builds the handshake blob) →
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
   mode), guarded by the `PMIX_MAX_CRED_SIZE` size limit.
2. Parse it with the `PMIX_PTL_GET_*` macros
   ([`base/ptl_base_handshake.h`](base/ptl_base_handshake.h)): psec name,
   credential, a **flag** (`pmix_rnd_flag_t`, 0–10) identifying the kind
   of connector, the peer's procid/uid/gid, version, bfrops module, buffer
   type, gds module, and an optional info blob. A v1.x or v2.0 peer is
   refused at the version string - see "Version floors" in
   [`base/AGENTS.md`](base/AGENTS.md).
3. Branch on the flag: a **simple client / singleton** (`accept_client`)
   must already be a registered nspace+rank; a **tool/launcher**
   (`accept_tool`) may be given an identity by the host's
   `tool_connected` up-call.
4. **Settle the identity, check, ask the host, accept - in that order.**
   The peer's credential is checked (`PMIX_PSEC_VALIDATE_CONNECTION_WITH`,
   naming the psec module the peer asked for) on a scratch peer attached
   to nothing shared, and a credential that needs a live exchange gets
   it - from events - before anything else. Every check that can refuse
   runs next; then the host is asked; only once it accepts is anything
   shared changed: the namespace's compat modules and version, a
   clients-array slot, the rank's pid, a namespace or rank entry built for
   a tool. Every refusal is sent to the peer as a status. See "Inbound"
   in [`base/AGENTS.md`](base/AGENTS.md) for the steps and the
   ownership rule that makes a refusal at any of them a single release.

**A namespace's compat modules are set by the first peer to connect.**
They are shared by every peer of the namespace - what the server packs
for one it packs for all - so a later peer never replaces them. One whose
wire format (bfrops module, buffer type) differs is refused; one whose
psec or gds differs is served with its own (`peer->gds` holds a peer's
own gds). `test/unit/ptl_validate_first.c` covers the ordering and this
rule on both paths.

**`PMIX_PSEC_VALIDATE_CONNECTION` decides the matter.** Its answer is
`PMIX_SUCCESS` or the reason the peer is refused; no psec module runs a
live exchange over the socket, and the server never answers a
connect-ack with `PMIX_ERR_READY_FOR_HANDSHAKE`. A client or tool asked
for one by a server fails its connection with `PMIX_ERR_NOT_SUPPORTED`.
See "No handshake" in [`psec/AGENTS.md`](../psec/AGENTS.md).

The `PMIX_PTL_PUT_*` (build) and `PMIX_PTL_GET_*` (parse) macro pairs are
deliberately symmetric so the two ends can be read side by side. **The
field order in the handshake is a wire-format contract** — per the
top-level interoperability rules it is append-only; never insert or
reorder. `test/unit/ptl_handshake.c` drives the pair directly and will
catch a change made to one half and not the other.

Note that **every field here arrives before the credential has been
validated**. Each `GET_*` macro therefore bounds itself against the
number of bytes actually received, and a new field must do the same.

## The listener (`base/ptl_base_listener.c`, `tcp/ptl_tcp.c`)

`pmix_ptl_base_setup_listener` is where a server/tool chooses how it can
be reached. It takes the directives that are not a transport's, has the
transport open the socket (`pmix_ptl_base_listen`), and publishes what
the transport made. The local transport's listener is always made; the
tcp transport's only when the server takes legacy peers
(`pmix_ptl_base.legacy_peers`, from `ptl_base_legacy_peers` or
`PMIX_SERVER_LEGACY_PEERS`). The tcp transport's part is the first two
steps:

- **Interface selection.** Enumerate interfaces, filter through
  `if_include`/`if_exclude`, and take a **loopback** device. It never
  falls back to a public interface: directives that leave no loopback
  device fail the init (`no-loopback-interfaces`), and a host that asks
  for connections from other hosts (`PMIX_SERVER_REMOTE_CONNECTIONS`
  true) fails it too (`remote-connections-not-supported`).
- **Bind.** Try each port from `ipv4_ports` / `ipv6_ports` (default `0` =
  let the kernel choose), set `SO_REUSEADDR` and close-on-exec, `listen`
  with `SOMAXCONN`, and make the socket non-blocking.
- **Publish.** Build the URI `nspace.rank;tcp4://host:port`. The base
  stores the server's URI - the TCP one when there is a TCP listener,
  else the local socket's - in `gds` under both `PMIX_MYSERVER_URI` and
  (for older tools) `PMIX_SERVER_URI`, optionally emits it via
  `report_uri` (`-`=stdout, `+`=stderr, an integer pipe fd, or a file,
  created mode 0644), and puts it first in the rendezvous files.
- **Rendezvous files.** Depending on role/flags, drop contact files a
  peer can discover: `pmix.sched.<host>` (scheduler),
  `pmix.sysctrlr.<host>` (system controller), `pmix.sys.<host>` (system
  tool), `pmix.<host>.tool` (session tool), `pmix.<host>.tool.<pid>`, and
  `pmix.<host>.tool.<nspace>`. Each holds `uri / version / pid / uid:gid /
  timestamp`. The `created_*` bookkeeping in `pmix_ptl_base` records which
  of these we made so `pmix_ptl_close` can remove exactly those.

**Binding and accepting are two calls, on purpose.**
`pmix_ptl_base_create_listener` does everything above — bind, publish the
URI, write the rendezvous files — and `pmix_ptl_base_start_listening`
registers the accept event on the progress thread. They used to be one
call, and that armed the accept event — so connection handling could
start on the progress thread — in the *middle* of the caller's init.

The reason it matters is that `pmix_rte_init` ends by starting the
progress thread, so everything `PMIx_server_init` and `PMIx_tool_init` do
after it runs on the **caller's** thread — while
`pmix_ptl_base_connection_handler` on the progress thread extends
`pmix_globals.nspaces`, `pmix_server_globals.clients`,
`pmix_globals.iof_requests` and (through `PMIX_GDS_CACHE_JOB_INFO`) the
`gds` tracker lists. `gds/hash` holds no lock at all; it is correct only
because everything that touches it is meant to be on one thread. Neither
the accept nor the handshake path screens `pmix_globals.initialized`.

Both roles therefore create the listener where they always did — a
launcher that attaches to a parent partway through its own init has to
have a URI of its own to offer it, and that URI comes from the bind — and
call `pmix_ptl_base_start_listening()` at the foot of init, once the
caller's thread is done with shared state. A peer that connects in
between is **not refused**: the socket is bound and listening as far as
the kernel is concerned, so the connection waits in the `SOMAXCONN`
backlog until we start accepting. That is the whole reason to split the
call rather than move it — moving it would have taken the URI with it.

`pmix_ptl_base_start_listening` is a no-op when no listener was created
(a tool that is not acting as a server never creates one), so the call
site needs no condition of its own. `test/unit/runtime_init.c` asserts
the event really is armed once init returns, on both of its cycles: the
arming is a step an early return added above it would silently skip, and
a single-process test notices nothing when it is missed.

**Stale rendezvous files are reclaimed, not fatal.** A file is removed
only by a clean `pmix_ptl_close`, so a server that is killed or that
crashes leaves its file behind. `write_rndz_file` therefore treats an
existing file as a question rather than an answer: it reads the pid the
file records and, if that process is gone (or the file names no usable
pid at all, as happens when its creator died partway thru writing it),
unlinks the orphan and creates its own. If the recorded pid *is* alive,
the file belongs to a running server, so we leave it strictly alone and
fail — but with the `rndz-file-in-use` `show_help` topic naming the role,
the file, and the owning pid, and returning `PMIX_ERR_SILENT` so the
server's generic "listener thread failed to start" message does not bury
it. `test/unit/rndz_stale.c` pins both halves of that boundary.

Note the asymmetry: a **tool** given a rendezvous file treats an existing
file as its *server's* contact info and never writes one, so it never
reaches this path.

**Related environmental issue — a stale `$TMPDIR`.** Leftover session
directories and tool connection files still accumulate under `$TMPDIR`
when runs are killed, and can confuse *discovery* (see the
`too-many-conns` topic). Reproduce under `TMPDIR=$(mktemp -d)` before
chasing a wholesale failure of the server-based tests. See the *Building
and Testing* section of the top-level
[`AGENTS.md`](../../../AGENTS.md).

## MCA parameters (registered in `ptl_base_frame.c`)

All under the `pmix_ptl_base_` prefix, most with deprecated
`pmix_ptl_tcp_` synonyms retained for compatibility. The interface, port
and address-family ones are the tcp transport's, which reads them from
`pmix_ptl_base`: it cannot register them itself, as its `pmix_ptl_tcp_*`
names are already those synonyms.

| Parameter | Meaning |
|-----------|---------|
| `max_msg_size` | cap (MB) on an inbound message; 0 → no practical limit |
| `if_include` / `if_exclude` | interface selection (mutually exclusive) |
| `ipv4_ports` / `ipv6_ports` | ports to try when binding the listener |
| `disable_ipv4_family` / `disable_ipv6_family` | skip a whole address family (IPv6 disabled by default) |
| `connection_wait_time` / `max_retries` | how long/often to wait for a server's connection file to appear |
| `handshake_wait_time` / `handshake_max_retries` | how long a client or tool waits on a server while connecting - each `connect()` attempt and each handshake reply, or the whole event-driven attach (default 60s, 0 = unbounded) - and how often it retries a handshake the server asks it to retry |
| `connect_ack_timeout` | seconds a server gives an inbound connection to deliver its whole connect-ack, and to answer each step of the blocking handshake after it, before dropping it (default 5; 0 = no limit) |
| `max_pending_connections` | most inbound connections held while their connect-ack is still arriving, and separately most tools held while the host decides; at the first limit the oldest still arriving is dropped, at the second a further tool is refused (default 0 = an eighth of the descriptor limit, for each; -1 = no limit) |
| `report_uri` | where to print the listener URI |
| `server_access` | servers a client or tool will talk to on the local socket besides its own user's and root's, when no server it was started by named itself: `users=<list>,groups=<list>`, names or numbers separated by `:` |

Per the top-level guidance, prefer adding an MCA parameter (or, better, an
attribute honored by `connect_to_peer`/`setup_listener`) over hard-coding
a new constant.

## Threading model

Two distinct regimes coexist, and confusing them is the most common way
to break this framework:

1. **Init-time connection is synchronous and blocking.** `connect_to_peer`
   runs in the caller's thread during `PMIx_Init`/`PMIx_tool_init`, does
   *blocking* socket I/O, and returns only once the handshake completes.
   Do not borrow the same excuse for the listener's side. "It is only
   startup" is true of the *connecting* peer and false of the server: the
   inbound handshake runs on the server's progress thread, at any moment,
   while that thread is serving every other client. So the server reads
   the connect-ack as its bytes arrive and
   never waits for them; only the replies after it are blocking writes,
   bounded by `ptl_base_connect_ack_timeout` — see
   [`base/AGENTS.md`](base/AGENTS.md).
2. **Steady-state I/O is entirely on the progress thread.** Every
   `PMIX_PTL_SEND_*` macro thread-shifts; the send/recv handlers,
   `process_msg`, the accept handler, and the connection handler all run
   on `pmix_globals.evbase`. Shared state
   (`pmix_ptl_base.posted_recvs`, a peer's send/recv queues) is touched
   only there.

The caddies (`pmix_ptl_sr_t`, `pmix_ptl_queue_t`, `pmix_pending_connection_t`)
all carry the mandatory `pmix_event_t ev` and retain the
peer so it outlives the async hop, exactly as the top-level thread-safety
rules require. The blocking file-wait paths (`pmix_ptl_base_parse_uri_file`,
`check_server`) pause through `retry_wait()`: an evtimer plus a local
`pmix_lock_t` when the caller is some other thread, and a plain sleep
when it is the progress thread itself — which it is for
`PMIx_tool_attach_to_server`, and where a timer on its own loop could
never fire. See [`base/AGENTS.md`](base/AGENTS.md).

## Directory layout

```
src/mca/ptl/
├── ptl.h                       Framework API: role table, macros, component struct, version
├── ptl_types.h                 Header, tags, transport objects, proc-type & version macros
├── base/
│   ├── base.h                  Internal base API + the pmix_ptl_base global-state struct
│   ├── ptl_base_handshake.h    PUT_*/GET_* handshake (de)serialization macros + rnd flags
│   ├── ptl_base_frame.c        open/close, MCA params, framework decl, class instances
│   ├── ptl_base_select.c       fills pmix_ptl with the process's role
│   ├── ptl_base_client.c       a client's own connect_to_peer
│   ├── ptl_base_tool.c         a tool's setup_listener wrapper
│   ├── ptl_base_connect.c      outbound connection: discovery matrix + socket connect
│   ├── ptl_base_connection_hdlr.c  inbound connection: accept → parse handshake → validate
│   ├── ptl_base_listener.c     interface selection, bind, publish URI, rendezvous files
│   ├── ptl_base_sendrecv.c     send/recv handlers, tag matching, lost-connection teardown
│   ├── ptl_base_fns.c          handshake build/parse, URI parsing, file discovery, helpers
│   ├── ptl_base_stubs.c        version comparison + notification-recv registration
│   └── AGENTS.md               the authoritative guide to everything above
└── tcp/                        the TCP transport: listening sockets and tcp4:// / tcp6:// URIs
```

## Building

The base and every transport are built into `libpmix`. Editing a
`Makefile.am` only needs a plain `make`; adding or removing a *component directory* changes the build
wiring resolved by `configure`, so re-run
`./autogen.pl && ./configure … && make`. `ptl` **does** ship its own
`show_help` file, `base/help-ptl-base.txt`, whose content is compiled
into the library; in a git clone a plain `make` regenerates it (see the
top-level guide for a tarball).

## When working in this framework

- **A role is not a component.** Role differences are rows in the table
  in `base/ptl_base_select.c`; do not reintroduce role components.
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
