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
`PMIX_GDS_*` macros, and the two shipped components (`hash` and `shmem2`).
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
`shmem2`) `src/util/pmix_shmem.c`.

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
name a module, `shmem2` (priority 20) is assigned over `hash` (priority 10)
wherever `shmem2` is available; when the caller passes
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
was assigned `shmem2` but cannot attach the shared segment at the required
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
| `name` | `const char *` | both | module name string (`"hash"`, `"shmem2"`) — the identity used in `assign_module` and the handshake |
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

1. **Peer-resolved, direct.** Resolve `PMIX_GDS_PEER_MODULE(p)` (or
   `p->nptr->compat.gds`) and call the function. Examples:
   `PMIX_GDS_REGISTER_JOB_INFO`, `PMIX_GDS_STORE_JOB_INFO`,
   `PMIX_GDS_CACHE_JOB_INFO`, `PMIX_GDS_FETCH_KV`, `PMIX_GDS_FETCH_IS_TSAFE`.

2. **Peer-resolved with a `NULL`-function fallback.** `PMIX_GDS_STORE_KV`,
   `PMIX_GDS_ASSEMB_KVS_REQ`, and `PMIX_GDS_ACCEPT_KVS_RESP` first resolve
   the peer's module, then: if that module leaves the slot `NULL`, they
   return `PMIX_ERR_NOT_SUPPORTED` *if the module is `"hash"`*, otherwise
   they retry through `pmix_globals.mypeer->nptr->compat.gds` (the local
   server's own module). This is not cosmetic — `shmem2` deliberately sets
   `store`, `assemb_kvs_req`, and `accept_kvs_resp` to `NULL`, so these
   operations fall through to the local module. Do not "clean up" the
   fallback without understanding that a live component relies on it.

3. **Always-local.** `PMIX_GDS_STORE_MODEX`, `PMIX_GDS_MARK_MODEX_COMPLETE`,
   `PMIX_GDS_RECV_MODEX_COMPLETE`, and `PMIX_GDS_FETCH_INFO_ARRAYS` ignore
   the passed peer for resolution and always use
   `pmix_globals.mypeer->nptr->compat.gds` — modex assembly is the local
   server's job regardless of which peer contributed the bytes.

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

- **`base/gds_base_frame.c`** declares the framework
  (`PMIX_MCA_BASE_FRAMEWORK_DECLARE(pmix, gds, "PMIx Generalized Data
  Store", NULL, pmix_gds_open, pmix_gds_close, ...)`), instantiates the
  global `pmix_gds_globals` state, and defines the `PMIX_CLASS_INSTANCE`
  for `pmix_gds_base_active_module_t`. Note the fourth argument is `NULL` —
  **the framework registers no MCA parameters of its own** (the only `gds`
  MCA parameters live in the `shmem2` component). `pmix_gds_open` opens all
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
| `shmem2`  | 20 | `/proc/self/maps` exists (Linux) *and* it was built (64-bit, non-Apple) |
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
  `hash` when `shmem2` fails. Used by the client fallback path.
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
`PMIX_GDS_*` macros, and `PMIX_GDS_BASE_VERSION_1_0_0`, the version macro
every component struct must open with. It also carries the load-bearing
comment that **the GDS is not guaranteed to be thread-safe** — see
Threading.

### `base/base.h` — globals, active-module wrapper, modex key formats

- **`pmix_gds_globals`** (`pmix_gds_globals_t`) — the framework state:
  `actives` (the priority-ordered module list), `initialized`, `selected`,
  and `all_mods` (the advertised comma list).
- **`pmix_gds_base_active_module_t`** `{ super, pri, module, component }` —
  one entry on `actives`, pairing a module with the priority it was
  inserted at and its component.
- **`pmix_gds_modex_key_fmt_t`** (`NATIVE_FMT` / `KEYMAP_FMT`) and the
  `PMIX_GDS_COLLECT_BIT` / `PMIX_GDS_KEYMAP_BIT` blob-info flags — used when
  (de)serializing modex blobs so a reader knows the key encoding and
  whether data was collected.

## Directory layout

```
src/mca/gds/
├── gds.h                   Framework API: module struct, PMIX_GDS_* macros, version
├── base/                   Framework infrastructure (see above)
│   ├── base.h              Internal base API: globals, active-module wrapper, modex flags
│   ├── gds_base_frame.c    open/close, framework decl (no MCA params), class instance
│   ├── gds_base_select.c   query components → build priority-ordered actives list
│   └── gds_base_fns.c      available-modules, assign/fallback module, setup_fork, store_modex
├── hash/                   in-process hash-table store (always available)
└── shmem2/                 shared-memory store (server writes, clients read-only)
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
it), while `shmem2` only builds on a **64-bit, non-Apple** host (it relies
on a large virtual address space and the `/proc/self/maps`-based VM-hole
finder). On macOS, therefore, only `hash` exists; on 64-bit Linux both
build and `shmem2` is preferred at runtime when `/proc/self/maps` is
present. Adding a component means creating `src/mca/gds/<name>/` with the
usual `Makefile.am` and (if it has build prerequisites) a `configure.m4`, a
component struct opened with `PMIX_GDS_BASE_VERSION_1_0_0`, and a module;
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
  `shmem2` does.
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
  says otherwise.
