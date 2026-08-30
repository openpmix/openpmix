<!--
  Copyright (c) 2026      Nanook Consulting  All rights reserved.

  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
-->

# AGENTS.md: The GDS Framework

This document orients AI agents and human contributors working in the
`gds` (**G**eneralized **D**ata **S**tore) framework. It assumes you have
already read the top-level [`AGENTS.md`](../../../AGENTS.md) — the golden
rules, prefix conventions, thread-safety/caddy model, and MCA concepts
described there all apply here and are not repeated. This file covers what
is specific to `gds`: what the framework stores, the unusual
"assign-on-demand per peer" way its modules are selected, the large module
interface every component fills in, how the base routes calls through the
`PMIX_GDS_*` macros, and the two shipped components (`hash` and `shmem3`).
Each component subdirectory carries its own `AGENTS.md` with
component-specific detail.

## What GDS does

`gds` is the **datastore** underneath PMIx. Everything a process "puts"
(`PMIx_Put`), every job-level value a launcher hands a server at
`PMIx_server_register_nspace`, and every remote value received through a
fence/modex exchange is stored in, and later fetched from, a `gds` module.
It answers one question — *"where and how do I keep this key/value, and how
do I get it back for the process that is asking?"* — and it answers it
differently depending on which module a given peer negotiated.

Concretely, a `gds` module is responsible for:

- **Job-level data** — the node map, proc map, app info, session info, and
  every job-scoped key the host RM provides for an nspace. The server
  caches this (`cache_job_info`), packs a per-client copy on demand
  (`register_job_info`), and the client stores what it receives
  (`store_job_info`).
- **Peer "put" data** — key/value pairs a process publishes, tagged with a
  `pmix_scope_t` (`PMIX_LOCAL`, `PMIX_REMOTE`, `PMIX_GLOBAL`,
  `PMIX_INTERNAL`) that controls who may later see them (`store`).
- **Modex data** — the aggregated blob of remote-proc data produced by a
  collective fence with data collection (`store_modex`,
  `mark_modex_complete`, `recv_modex_complete`).
- **Retrieval** — resolving a `(proc, key, scope)` request into a list of
  `pmix_kval_t` for the *requesting* peer (`fetch`), formatting the answer
  to match that peer's version and module.

Everything about the *transport* of this data (sockets, framing,
handshake) belongs to `ptl`; the *serialization* belongs to `bfrops`; the
*node/proc-map regex* belongs to `preg`. `gds` is purely the store and the
retrieval logic layered on top of `src/util/pmix_hash.c` and (for
`shmem3`) `src/util/pmix_shmem.c`.

## The single most important structural fact: assign-on-demand per peer

`gds` is a **multi-select** framework — several modules are active at once
— but it does **not** dispatch the way `preg` or `bfrops` do (offer a
request to each active module until one claims it). Instead, **each peer is
bound to exactly one `gds` module for its whole lifetime**, and almost
every operation resolves the module *from the peer*, not from a global
active list.

The binding works like this:

1. At startup every runnable component's module is placed on
   `pmix_gds_globals.actives` in descending priority order
   (`gds_base_select.c`). Nothing is chosen yet — the list is the menu.
2. A server advertises its menu to connecting peers: the comma-delimited
   list `pmix_gds_globals.all_mods`, returned by
   `pmix_gds_base_get_available_modules()`, is sent as part of the `ptl`
   connection handshake.
3. When two peers connect, one side proposes a module (the client dictates,
   via the `PMIX_GDS_MODULE` attribute, which module the server should use
   for it). `pmix_gds_base_assign_module(info, ninfo)` walks the actives,
   asks each module's `assign_module` whether it will serve *these
   directives* and at what priority, and returns the **highest-priority
   responder**. The result is cached in
   `peer->nptr->compat.gds` — the module for that peer's whole namespace.
   (See `src/server/pmix_server.c`, `src/client/pmix_client.c`,
   `src/tool/pmix_tool.c`, and
   `src/mca/ptl/base/ptl_base_connection_hdlr.c` for the call sites.)
4. Thereafter, back-end code reaches "the right module for this peer"
   through the `PMIX_GDS_*` macros, which read `peer->nptr->compat.gds`
   (or the per-peer override `peer->gds`; see below).

So a module's `assign_module` return value — not the component's static
priority — is what actually decides who serves a peer, and it can depend on
the directives. With the two shipped components, when the caller does not
name a module, `shmem3` (priority 20) is assigned over `hash` (priority 10)
wherever `shmem3` is available; when the caller passes
`PMIX_GDS_MODULE="hash"`, `hash` bids 100 and wins.

### The per-peer override and the fallback path

Normally a peer uses its namespace's module (`nptr->compat.gds`). But a
client can end up on a *different* module than the rest of its nspace — this
is what [`PMIX_GDS_PEER_MODULE(p)`](gds.h) exists for:

```c
#define PMIX_GDS_PEER_MODULE(p) \
    (NULL != (p)->gds ? (p)->gds : (p)->nptr->compat.gds)
```

If `peer->gds` is set it wins; otherwise the nspace default is used. The
one case that sets `peer->gds` today is the **GDS fallback**: if a client
was assigned `shmem3` but cannot attach the shared segment at the required
fixed address, `fallback_to_next_gds()` (in `src/client/pmix_client.c`)
calls `pmix_gds_base_get_fallback_module()` — the highest-priority active
module *other than* the failing one — points `myserver->gds` (and, on the
client, `nptr->compat.gds`) at it, and re-requests the job data from the
server via `PMIX_GDS_FALLBACK_CMD`. This is why no operation should assume
`nptr->compat.gds` and `peer->gds` are the same; always resolve through
`PMIX_GDS_PEER_MODULE` (or the macros, which already do).

## Module interface (`pmix_gds_base_module_t`)

Defined in [`gds.h`](gds.h). This is a **large** interface — far larger
than most PMIx frameworks — because a datastore has many distinct
server-side and client-side responsibilities. A component fills in the
subset it implements and leaves the rest `NULL`; several macros have
explicit fallbacks for the `NULL` case (see below).

| Field | Signature (typedef) | Role | Purpose |
|-------|---------------------|------|---------|
| `name` | `const char *` | both | module name string (`"hash"`, `"shmem3"`) — the identity used in `assign_module` and the handshake |
| `is_tsafe` | `const bool` | both | whether `fetch` may run outside the progress thread; both shipped modules set `false` |
| `init` / `finalize` | `init_fn_t` / `fini_fn_t` | both | per-module setup/teardown; `init` may reject (module can't run) |
| `assign_module` | `assign_module_fn_t` | both | bid to serve a peer given its directives; returns a priority (see selection) |
| `cache_job_info` | `cache_job_info_fn_t` | server | stash an nspace's job-level `pmix_info_t` until a local client's module is known |
| `register_job_info` | `register_job_info_fn_t` | server | produce the per-client reply buffer of job data for one connecting peer |
| `store_job_info` | `store_job_info_fn_t` | client | store the reply buffer produced above |
| `store` | `store_fn_t` | both | store one `pmix_kval_t` for a proc at a scope |
| `store_modex` | `store_modex_fn_t` | server | unpack + store an aggregated fence/modex blob |
| `fetch` | `fetch_fn_t` | both | retrieve `pmix_kval_t`s for a `(peer, proc, key, scope)` request |
| `setup_fork` | `setup_fork_fn_t` | server | add any env vars a child needs to reach this store |
| `add_nspace` | `add_nspace_fn_t` | server | note a new nspace (and per-nspace directives) |
| `del_nspace` | `del_nspace_fn_t` | both | drop an nspace and its data |
| `assemb_kvs_req` | `assemb_kvs_req_fn_t` | server | pack a server's answer to a client `get` |
| `accept_kvs_resp` | `accept_kvs_resp_fn_t` | client | unpack + store that answer |
| `fetch_arrays` | `fetch_array_fn_t` | server | pack info arrays for a peer |
| `mark_modex_complete` | `mark_modex_complete_fn_t` | server | finalize a modex (e.g. publish shmem contact info) |
| `recv_modex_complete` | `recv_modex_complete_fn_t` | client | receive that modex-complete payload |

There is **no** exported `pmix_gds` global module and **no** public
interface struct — `gds.h` says so explicitly. Unlike single-select
frameworks (`ptl`, `pstat`) there is nothing to "call through"; you always
call an *assigned* module, reached via the macros below.

## The `PMIX_GDS_*` macros — how the base routes a call

Because every call must resolve a per-peer module first, `gds.h` provides a
macro for essentially every module function. **Use the macro, not a direct
`module->fn(...)` call** — the macros encode the resolution rules and the
`NULL`-function fallbacks. There are four routing patterns; know which one
you are touching:

1. **Peer-resolved, direct.** Resolve `PMIX_GDS_PEER_MODULE(p)` and call
   the function. Examples: `PMIX_GDS_REGISTER_JOB_INFO`,
   `PMIX_GDS_STORE_JOB_INFO`, `PMIX_GDS_CACHE_JOB_INFO`,
   `PMIX_GDS_FETCH_KV`, `PMIX_GDS_FETCH_IS_TSAFE`. Of these,
   `PMIX_GDS_CACHE_JOB_INFO` also returns `PMIX_ERR_NOT_SUPPORTED` for a
   module that leaves the slot `NULL`; the rest are called by every
   shipped component and are not guarded.

   Every peer-resolved macro goes through `PMIX_GDS_PEER_MODULE(p)`, not
   through `p->nptr->compat.gds` directly. Four of them used to do the
   latter, which silently ignored the `peer->gds` override — so a client
   that had just fallen back to another module would have those
   operations handed back to the module it abandoned. If you add a
   macro, resolve it the same way.

2. **Peer-resolved with a `NULL`-function fallback.** `PMIX_GDS_STORE_KV`,
   `PMIX_GDS_ASSEMB_KVS_REQ`, and `PMIX_GDS_ACCEPT_KVS_RESP` first resolve
   the peer's module, then: if that module leaves the slot `NULL`, they
   return `PMIX_ERR_NOT_SUPPORTED` *if the module is `"hash"`*, otherwise
   they retry through `pmix_globals.mypeer->nptr->compat.gds` (the local
   server's own module). This is not cosmetic — `shmem3` deliberately sets
   `store`, `assemb_kvs_req`, and `accept_kvs_resp` to `NULL`, so these
   operations fall through to the local module. Do not "clean up" the
   fallback without understanding that a live component relies on it.

3. **Always-local.** `PMIX_GDS_STORE_MODEX`, `PMIX_GDS_MARK_MODEX_COMPLETE`,
   `PMIX_GDS_RECV_MODEX_COMPLETE`, and `PMIX_GDS_FETCH_INFO_ARRAYS` ignore
   the passed peer for resolution and always use
   `pmix_globals.mypeer->nptr->compat.gds` — modex assembly is the local
   server's job regardless of which peer contributed the bytes.
   `PMIX_GDS_FETCH_INFO_ARRAYS` returns `PMIX_ERR_NOT_SUPPORTED` for a
   `NULL` slot, because `shmem3` really does leave that one empty.

   **"Always-local" is doing real work here, and it is what makes the
   unguarded macros safe.** A server assigns *itself* `"hash"` at init
   (see `pmix_server.c`), so the local module is the one component that
   fills every slot. Anything that resolves a module some other way — or
   calls a slot by hand rather than through a macro — loses that
   guarantee.

4. **Fan-out across all actives.** `PMIX_GDS_ADD_NSPACE` and
   `PMIX_GDS_DEL_NSPACE` are the only macros that iterate the whole
   `pmix_gds_globals.actives` list, invoking every module that implements
   the slot. Because a peer's eventual module isn't known when an nspace is
   first added, *every* active module is told about it. This is the one
   place the "multi-select" nature is visible at call time.

`PMIX_GDS_CHECK_COMPONENT(p, s)` / `PMIX_GDS_CHECK_PEER_COMPONENT(p1, p2)`
are convenience string comparisons against the resolved module name — used,
e.g., to special-case `"hash"` behavior.

## Selection and lifecycle

The `base/` directory has its own [`AGENTS.md`](base/AGENTS.md), which is
the authoritative document for the framework infrastructure — the
selection routine, the exported helpers, and in particular the callback
contract of the modex envelope walker. The summary below is orientation.

- **`base/gds_base_frame.c`** declares the framework
  (`PMIX_MCA_BASE_FRAMEWORK_DECLARE(pmix, gds, "PMIx Generalized Data
  Store", NULL, pmix_gds_open, pmix_gds_close, ...)`), instantiates the
  global `pmix_gds_globals` state, and defines the `PMIX_CLASS_INSTANCE`
  for `pmix_gds_base_active_module_t`. Note the fourth argument is `NULL` —
  **the framework registers no MCA parameters of its own** (the only `gds`
  MCA parameters live in the `shmem3` component). `pmix_gds_open` opens all
  components and constructs the `actives` list; `pmix_gds_close` finalizes
  each active module, tears the list down, and frees `all_mods`.
- **`base/gds_base_select.c`** (`pmix_gds_base_select`, guarded by
  `pmix_gds_globals.selected` so it runs once) queries each component,
  calls each returned module's `init` (skipping any that fail), wraps the
  survivors in `pmix_gds_base_active_module_t { pri, module, component }`,
  and inserts them into `pmix_gds_globals.actives` in **descending priority
  order**. If zero modules survive it emits the `no-plugins` `show_help`
  topic (from `help-pmix-runtime.txt`) and returns `PMIX_ERR_SILENT` —
  `gds` requires at least one component. At verbosity >4 it prints the
  resolved priority list.

Component priorities (from each component's `component_query`, before any
`assign_module` bid):

| Component | `component_query` priority | Runnable when |
|-----------|----------------------------|---------------|
| `shmem3`  | 20 | `/proc/self/maps` exists (Linux) *and* it was built (64-bit, non-Apple) |
| `hash`    | 10 | always |

Remember these are just the *default* priorities that seed the actives
list. The number that actually decides a peer's module is what
`assign_module` returns for that peer's directives — see the top section.

## The base routing/helper layer (`base/gds_base_fns.c`)

Four exported helpers live here; the first three are the machinery behind
the selection story above:

- **`pmix_gds_base_get_available_modules()`** — returns a `strdup` of
  `all_mods` (the priority-ordered comma list), used to build the server's
  handshake advertisement.
- **`pmix_gds_base_assign_module(info, ninfo)`** — the per-peer chooser.
  Walks the actives, calls each `assign_module(info, ninfo, &pri)`, and
  keeps the highest returned priority (a module returning `pri < 0` falls
  back to its component priority). Returns the winning module or `NULL`.
- **`pmix_gds_base_get_fallback_module(failing)`** — the highest-priority
  active module whose `name` differs from `failing->name`, chosen purely by
  the recorded component priority (it does **not** consult `assign_module`).
  No module names are hard-coded; with today's two modules this returns
  `hash` when `shmem3` fails. Used by the client fallback path.
- **`pmix_gds_base_setup_fork(proc, &env)`** — rotates across *all* active
  modules, giving each its `setup_fork` turn (a fan-out, like
  add/del_nspace), tolerating `PMIX_ERR_NOT_AVAILABLE`.

The heaviest routine here is **`pmix_gds_base_store_modex(buff, cb_fn,
cbdata)`**. Both components' `store_modex` delegate to it. It walks the
nested envelope structure of an aggregated fence result — an outer
`PMIX_BYTE_OBJECT` per contributing server, each holding a compression flag
plus a byte object of rank-level blobs, each of those holding a
collect-flag byte and the per-proc blobs — decompressing with
`pmix_compress` where flagged, checking that the collect flag is consistent
across servers (else the `collection-mismatch` `show_help` in
`help-pmix-server.txt` fires), and calling the component-supplied `cb_fn`
once per proc blob to store it, then once per involved nspace with a `NULL`
buffer to signal "done." Keeping this envelope-walking in the base means
neither component reimplements the wire structure; they only supply the
"store this one proc's bytes" callback.

## Core data structures

### `gds.h` — the module, component, and version

`gds.h` defines `pmix_gds_base_module_t` (above), the trivial component
typedef `pmix_gds_base_component_t` (just
`pmix_mca_base_component_t` — `gds` components that need per-component
state, like both shipped ones, wrap it in a larger struct themselves), the
`PMIX_GDS_*` macros, and the three `PMIX_MCA_gds_*_VERSION` macros that
state the framework's interface version — the numbers
`PMIX_MCA_BASE_VERSION(gds)` stamps into every component struct. It also carries the load-bearing
comment that **the GDS is not guaranteed to be thread-safe** — see
Threading.

### `base/base.h` — globals and the active-module wrapper

- **`pmix_gds_globals`** (`pmix_gds_globals_t`) — the framework state:
  `actives` (the priority-ordered module list), `initialized`, `selected`,
  and `all_mods` (the advertised comma list).
- **`pmix_gds_base_active_module_t`** `{ super, pri, module, component }` —
  one entry on `actives`, pairing a module with the priority it was
  inserted at and its component.
`pmix_gds_modex_key_fmt_t` and the `PMIX_GDS_COLLECT_BIT` /
`PMIX_GDS_KEYMAP_BIT` blob-info flags used to be declared here too. They
were deleted in August 2026, having been dead since the modex keymap was
removed in 2024. See "The modex keymap, and why it is not coming back"
in [`base/AGENTS.md`](base/AGENTS.md) before reviving the idea.

## Directory layout

```
src/mca/gds/
├── gds.h                   Framework API: module struct, PMIX_GDS_* macros, version
├── base/                   Framework infrastructure (see above)
│   ├── base.h              Internal base API: globals, active-module wrapper
│   ├── gds_base_frame.c    open/close, framework decl (no MCA params), class instance
│   ├── gds_base_select.c   query components → build priority-ordered actives list
│   └── gds_base_fns.c      available-modules, assign/fallback module, setup_fork, store_modex
├── hash/                   in-process hash-table store (always available)
└── shmem3/                 shared-memory store (server writes, clients read-only)
```

## Threading

`gds.h` states it plainly: **the GDS is not guaranteed to be thread-safe**,
and GDS functions "should always be called in a thread-safe condition —
e.g., from within an event." In practice that means the progress thread:
the server ops, client ops, and `ptl` connection handler that drive these
modules are already on `pmix_globals.evbase`. There is **no caddy pattern
inside `gds`** — the modules are synchronous transforms of their arguments;
the thread-shift happens *above* them, in the caller. The one hook for
relaxing this is the module's `is_tsafe` flag, tested via
`PMIX_GDS_FETCH_IS_TSAFE`: a module that set it `true` would permit its
`fetch` to be called off-thread. Both shipped modules set it `false`, so do
not call any `gds` entry point from an arbitrary user thread without
thread-shifting first.

## Building

The framework core (`base/`) is always built into `libpmix`. Each component
builds as a standard MCA component and is wired through the generated
`base/static-components.h`. **Both** components ship a `configure.m4`:
`hash` is "always available" (its `configure.m4` unconditionally enables
it), while `shmem3` only builds on a **64-bit, non-Apple** host (it relies
on a large virtual address space and the `/proc/self/maps`-based VM-hole
finder). On macOS, therefore, only `hash` exists; on 64-bit Linux both
build and `shmem3` is preferred at runtime when `/proc/self/maps` is
present. Adding a component means creating `src/mca/gds/<name>/` with the
usual `Makefile.am` and (if it has build prerequisites) a `configure.m4`, a
component struct opened with `PMIX_MCA_BASE_VERSION(gds)`, and a module;
the framework picks it up through `static-components.h`.

Per the top-level golden rules: editing a `Makefile.am` only needs a plain
`make`; adding or removing a *component directory* or touching a
`configure.m4` changes the build wiring resolved by `configure`, so re-run
`./autogen.pl && ./configure ... && make`. `gds` ships **no** `show_help`
text of its own — the two topics it raises (`no-plugins`,
`collection-mismatch`) live in `help-pmix-runtime.txt` and
`help-pmix-server.txt` respectively — so the regenerate-the-help-content
golden rule does not usually bite here.

## When working in this framework

- **Resolve modules through the macros / `PMIX_GDS_PEER_MODULE`, never by
  hand.** A peer's module can be its nspace default *or* a per-peer
  override; the macros already encode that, plus the `NULL`-function
  fallbacks. Reaching into `nptr->compat.gds` directly will miss the
  `peer->gds` fallback case.
- **`assign_module`, not component priority, chooses a peer's module.**
  When adding a component, implement `assign_module` to bid a high priority
  only when the caller explicitly names you (via `PMIX_GDS_MODULE`) or when
  you can genuinely serve the directives, and to disqualify yourself
  (priority 0) when the caller named a *different* module — exactly as
  `shmem3` does.
- **A `NULL` module slot is a real design choice with a defined fallback.**
  If you leave `store` / `assemb_kvs_req` / `accept_kvs_resp` `NULL`,
  understand you are opting into the local-module fallback in the macros.
- **Preserve wire/format compatibility.** Job-info, modex, and get-response
  buffers are packed and unpacked by these modules and travel between peers
  that may be built from different PMIx releases; treat their layouts as
  append-only per the top-level interoperability rules, and remember a
  client and server can even be on *different* `gds` modules — the
  `NULL`-slot fallbacks and the fetch-format-for-the-requesting-peer logic
  exist precisely to keep that working.
- **Everything runs on the progress thread.** No new path may call a `gds`
  entry point without first thread-shifting, unless the module's `is_tsafe`
  says otherwise. `shmem3` does say otherwise, and that is not theoretical:
  `try_local_fetch()` in `src/client/pmix_client_get.c` consults
  `PMIX_GDS_FETCH_IS_TSAFE` on every keyed `PMIx_Get` and
  `pmix_client_globals.fast_get` is on by default, so on a client that
  module's `fetch` runs on the application's own thread. A module that
  sets `is_tsafe` owns the synchronization for any process-local state its
  `fetch` walks — see the `is_tsafe` section in
  [`shmem3/AGENTS.md`](shmem3/AGENTS.md).
- **Treat every job-level value as untrusted.** The `pmix_info_t` array a
  module is handed comes from a host environment (which is not this
  project) or off the wire from a peer (which may be a different release).
  A key does not guarantee its documented type, an array does not
  guarantee a non-zero size, and a `char *` in a value union does not
  guarantee a string. This is where the August 2026 review of this
  framework found most of what it found — a `PMIX_GDS_MODULE` that was
  not a usable string, a `PMIX_HOSTNAME` that was not a string, an empty
  `PMIX_QUALIFIED_VALUE` whose `size - 1` became `SIZE_MAX`, and a
  server-supplied key-index table indexed past its own announced length.
  Check the shape before indexing it.
- **A module's `store_modex` callback must report `PMIX_SUCCESS` for a
  blob it consumed.** See [`base/AGENTS.md`](base/AGENTS.md) — returning
  the unpack end-of-buffer code instead silently drops every proc after
  the first in each server's contribution, and still reports success to
  the caller. Both shipped components serve this path: the three modex
  macros resolve from the peer they are given, so a fence spanning local
  clients on `shmem3` stores through `shmem3`. (Before `9d9842e5` they
  ignored that peer and resolved `pmix_globals.mypeer`, which both the
  server and the client pin to `"hash"` — which is why `shmem3`'s modex
  half, though written, had never executed. If you are reading older
  notes that say modex storage is excluded from `shmem3`, that is what
  they are describing.)

## Testing

- **`test/unit/gds_datastore`** — module selection (including malformed
  `PMIX_GDS_MODULE` directives), the base modex envelope walker and its
  callback contract, node/proc map decoding in all accepted forms at both
  the top level and nested in a `PMIX_JOB_INFO_ARRAY`, malformed
  job-level input, store/fetch scope routing including
  `PMIX_ERR_EXISTS_OUTSIDE_SCOPE`, and the rule that a per-proc value
  the datastore derives from the maps is only a default: the host's own
  `PMIX_PROC_INFO_ARRAY` wins, but per rank and per key, so a host that
  described some procs or some keys still gets the rest filled in.
- **`test/unit/gds_fallback`** — `pmix_gds_base_get_fallback_module()` and
  `PMIX_GDS_PEER_MODULE()`.
- **`test/unit/proc_array_id`** — `pmix_gds_base_proc_array_id()`.
- **`contrib/dockerswarm/run-gds-tests.sh`** — everything a single
  process cannot reach: `shmem3` itself (it does not build on macOS, so a
  developer there gives it no compiler coverage at all, let alone
  runtime), a client fetching a peer's data from a *different* server,
  and the fixed-address attach failure that drives the GDS fallback.

### A note on what is *not* cleaned up

A session legitimately outlives the jobs in it — that is what a session
is — so `del_nspace` drops the *job* and its reference to the session,
and never the session itself. **The signal that a session has ended is
`del_session`,** driven by `PMIx_server_deregister_session`; see
"Sessions are said, not inferred" below.

`pmix_mca_gds_shmem3_component.sessions` works the same way, with one
addition of its own: jobs in a session share a single session object
*and a single shared-memory segment*, so the segment is given back when
the session's last holder — the list's reference, plus one per job —
lets go. See [`shmem3/AGENTS.md`](shmem3/AGENTS.md).

## Sessions are said, not inferred

Jobs have always been established and torn down explicitly. Sessions
were not: one existed only as a side effect of a
`PMIX_SESSION_INFO_ARRAY` riding inside some job's registration, which
left a session with no way to be described before its first job and no
way to survive its last.

**The library must not infer that a session has ended from its last job
being deregistered.** A session with no jobs running in it is an
ordinary state, not a finished one — a persistent DVM holds an
allocation across the gaps between the jobs run in it — so only the host
knows, and it says so through `PMIx_server_deregister_session`.

Two slots carry that, and both are **fan-outs** across
`pmix_gds_globals.actives` rather than peer-resolved, for the same
reason `add_nspace`/`del_nspace` are: which module will serve the jobs
in a session is not known when the session is established, and need not
be the same for all of them.

| slot | macro | driven by |
|------|-------|-----------|
| `add_session` | `PMIX_GDS_ADD_SESSION` | `PMIx_server_register_session` |
| `del_session` | `PMIX_GDS_DEL_SESSION` | `PMIx_server_deregister_session` |

Both are optional; a module that keeps no session data leaves them NULL
and the macro treats that as success.

The inference path still works — a job naming a session the library has
not been told about establishes it — because hosts that predate the API
depend on it. What the explicit call adds is the two ends that
inference cannot express. Consumers detect it through
`PMIX_CAP_SESSION_REGISTRATION` in `pmix_version.h`.

## Telling clients that data has changed

A client maps what a session said when it attached. When the description
changes, a client already running has to be told, or a job launched
after the change sees the new value and one already running does not.

Two slots carry that, and they are **peer-resolved** rather than
fanned out: what a peer needs depends on the module it is bound to.

| slot | side | what it does |
|------|------|--------------|
| `pack_update` | server | pack what THIS peer needs to catch up |
| `accept_update` | client | take delivery of it |

`pmix_server_notify_gds_update()` walks the local clients, asks each
peer's own module to pack, and sends whatever comes back on
`PMIX_PTL_TAG_GDS_UPDATE`. A module with nothing to say packs nothing
and the peer is not written to at all; both slots are optional.

**Both modules implement them, and that is not optional in practice.**
`hash` is the module every non-Linux developer runs, so leaving it
unimplemented would mean session updates silently never reach a client
there — the same divergence between the two components that the
first-writer-wins rule was corrected for. `test/unit/session_update`
is what catches it: it runs against whichever module is assigned, so it
covers `shmem3` on Linux and `hash` everywhere.

**What is packed is the whole description, not the delta.** Applying it
twice, or to a peer that is already current, has to land on the same
state - `shmem3` skips every segment it already holds, `hash` replaces
by key - so a notice that arrives twice costs a walk and nothing else.
That is what makes it safe to notify more peers than strictly need it.

**A session's description is written once.** Both components take the
first description offered — the host's registration, or the first job
whose registration carried a session array — and ignore later ones
rather than merging them. `hash` could merge; `shmem3` cannot, because
its session data lives in a segment local clients have mapped and a
segment a client can see is never written again. Two components
answering a host differently is worse than either answer, so both take
the behavior available to both.

Component-internal symbols (`pmix_gds_hash_*`, and `pmix_gds_shmem3_*`
other than the few marked `PMIX_EXPORT`) are hidden in a
default-visibility build, so a unit test cannot call them. Drive a
component through the `PMIX_GDS_*` macros — which call its
function-pointer table — and through the public server API, as
`gds_datastore` does.
