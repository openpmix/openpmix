<!--
  Copyright (c) 2026      Nanook Consulting  All rights reserved.

  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
-->

# AGENTS.md: The PNET `tcp` Component

`tcp` is the `pnet` component that illustrates assigning **static TCP/UDP
ports** to processes for the "instant on" case — a gateway/scheduler hands
each job a slice of a pre-configured port pool, and each daemon caches the
assignment as job-level info. Read the framework [`AGENTS.md`](../AGENTS.md)
first; this file covers only what is specific to `tcp`.

**Status: experimental.** `tcp` compiles against the current
`pmix_pnet_module_t`, but it is **opt-in twice over**, and both gates
matter:

1. Its `configure.m4` is guarded by `--with-tcp`, so it is **not built by
   default** (like `simptest`).
2. Having built it, `component_open` still declines unless the `pnet` MCA
   parameter **names it** — `PMIX_MCA_pnet=tcp`, or `pnet = tcp` in a
   parameter file. Unlike `nvd`/`opa` it has no hardware gate, so without
   this it would join `actives` on every server the library comes up in
   and answer the inventory fan-out there, purely because somebody
   configured the tree with `--with-tcp`. Building a component is not the
   same as asking for it. `simptest` applies the same rule.

The match is against an *entry* of that list, not a substring of it, so a
component whose name merely contains "tcp" does not select this one and an
exclusion (`^tcp`) is not read as a request. Declining is reported as
`PMIX_ERR_NOT_AVAILABLE`, the MCA's "silently ignore me" cue; any other
status would be shown to the user as a component that failed to open.

Being selected is still not enough to allocate anything: the port pool
comes from `static_ports`, and without it `tcp_init` has nothing to parse.

Once active, its entry points self-gate: `allocate`/`tcp_init`/
`deregister_nspace` act only for the gateway role, and
`setup_local_network`/`deliver_inventory` act only on their own blob key.
`collect_inventory`, however, runs on every server it is active on and
reports the local TCP interfaces.

## Files

| File | Contents |
|------|----------|
| `pnet_tcp.h` | Component struct type (`static_ports`, `default_request`, the envar globs). |
| `pnet_tcp_component.c` | Component struct, `component_register` (MCA params), `component_open` (selection gate), `component_query` (priority **5**). |
| `pnet_tcp.c` | The module: init/finalize, allocate, setup_local_network, finalize hooks, collect/deliver inventory, and the `process_request` port assigner. |
| `configure.m4` | Guarded by `--with-tcp` (off by default); adds the `TCP` summary line. |

## Interface notes for maintainers

The module was ported to the current framework interface; when editing,
keep it aligned with `pmix_pnet_module_t`:

- There is **no `.setup_fork` slot** — fork-time envar injection is
  base-only, so a component that needs an envar in the child appends a
  `pmix_envar_list_item_t` to the namespace's `ns->envars` cache during
  `setup_local_network`.
- `collect_inventory` takes a plain `pmix_list_t *inventory` (append
  `pmix_kval_t`s to it) and `deliver_inventory` takes
  `(info, ninfo, directives, ndirs)` — neither uses a callback.
- `setup_local_network` takes a `pmix_nspace_env_cache_t *` (use
  `ns->ns` for the underlying `pmix_namespace_t *`).
- The framework's old global inventory store (`pmix_pnet_globals.nodes`
  and the `pmix_pnet_node_t` / `pmix_pnet_resource_t` types) was removed.
  `deliver_inventory` therefore archives into a **component-local** node
  tree (`tcp_node_t` → `tcp_resource_t` → `tcp_available_ports_t`), held
  on the static `nodes` list and dumped at verbosity >5. Nothing else
  queries that tree today; it exists to keep the archiving example intact.

## The intended design (what the code shows)

- **`component_open`** declines unless the `pnet` parameter names this
  component (see [Status](#agentsmd-the-pnet-tcp-component) above);
  **`component_query`** then returns `pmix_pnet_tcp_module` at priority **5**.
  Beyond that the gating is inside the module, on `PMIX_PEER_IS_GATEWAY`.
- **`tcp_init`** (gateway only) parses the `static_ports` MCA parameter —
  a `;`-delimited list of `type:plane:port-ranges` groups (e.g.
  `"tcp:10.10.10.0/24:32000-32100,33000;udp:40000,40005"`) — into
  `tcp_available_ports_t` entries on an `available` list, expanding the
  ranges with `pmix_util_parse_range_options`.
- **`allocate`** (gateway only) reads a `PMIX_ALLOC_FABRIC` array for the
  requested `type` / `plane` / endpoint count / id-key / seckey, finds a
  matching `tcp_available_ports_t`, and calls `process_request` to carve
  out `ports_per_node` ports into a `tcp_port_tracker_t`. Results (the
  id-key, the allocated port list, the type, and any plane) are packed
  into a blob keyed `PMIX_TCP_SETUP_APP_KEY` and appended to `ilist`. It
  can also harvest envars and generate a random security key.
- **`process_request`** pulls free ports out of the source pool (nulling
  each slot it takes) and joins them into a comma string; the tracker's
  destructor `ttdes` **returns those ports to the pool** when the job is
  deregistered — the core of the reuse scheme.
- **`setup_local_network`** unpacks `PMIX_TCP_SETUP_APP_KEY`, rebuilds the
  kvals, and caches them as job info via `PMIX_GDS_CACHE_JOB_INFO`, keyed
  by the fabric ID the allocation was made under.
- **`deregister_nspace`** (gateway only) finds and releases **every**
  `tcp_port_tracker_t` the job holds, which frees the allocation and
  recycles its ports.
- **`collect_inventory`** enumerates non-loopback, non-virtual IPv4/IPv6
  interfaces via the `pmix_if*` API and packs `(device, tcp[46]://addr)`
  pairs into an inventory blob keyed `PMIX_TCP_INVENTORY_KEY`;
  **`deliver_inventory`** unpacks such blobs into the component-local
  `nodes` tree (see the interface notes above).

## Invariants worth knowing before you edit

- **An `info` array handed to a module is on loan, and the framework
  hands the *same* array to every other active module after you.** Both
  `setup_local_network` and `deliver_inventory` receive a blob inside
  that array, and both must read it with `PMIX_LOAD_BUFFER_NON_DESTRUCT`.
  The plain `PMIX_LOAD_BUFFER` NULLs and zeroes the source byte object,
  and the buffer is deliberately never destructed (we do not own the
  bytes) — so a destructive load orphans the payload *and* leaves every
  module behind you in `pmix_pnet_globals.actives`, plus `pgpu` after the
  whole framework, looking at an empty blob. `opa` and `nvd` use the
  non-destructive form for the same reason.

- **`pmix_list_get_first()` returns the list's own sentinel, not `NULL`,
  when the list is empty.** The default-allocation paths reach for the
  first entry of `available`, which is empty on any server that has the
  component selected but no `static_ports` configured — the common case.
  Test `pmix_list_is_empty()`; a `NULL` check does nothing.

- **One `allocate` call can build more than one tracker.** The
  `default_network_allocation` path creates a `tcp_port_tracker_t` per
  `;`-separated group. `deregister_nspace` therefore has to walk the
  whole `allocations` list rather than stopping at the first match, or
  the pool drains a little further with every job.

- **A request may name a fabric type and ask for zero endpoints.** The
  header comment above `allocate` says callers "are allowed to simply
  request a network security key without asking for endpts", so that
  case must not reach `process_request` — it declines a zero count with
  `PMIX_ERR_NOT_SUPPORTED`, and the base's `allocate` fan-out aborts on
  any status other than `PMIX_SUCCESS` / `PMIX_ERR_NOT_AVAILABLE` /
  `PMIX_ERR_TAKE_NEXT_OPTION`, so one bad decline stops every other
  component from running.

- **A module whose `init` fails is never added to `actives`, so its
  `finalize` never runs.** `pmix_pnet_base_select` skips it (that is the
  documented way for a module to veto its own selection). `tcp_init`
  must therefore give back the lists and trackers it built before
  returning an error; nothing else will.

- **Every entry point runs on the progress thread**, so the static
  `allocations` / `available` / `nodes` lists need no locking. That
  includes the two inventory hooks: `PMIx_server_collect_inventory` and
  `PMIx_server_deliver_inventory` both `PMIX_THREADSHIFT` before calling
  into `pmix_pnet`. `tcp` has no `setup_fork` slot and no fabric entry
  points, which are the two places the framework `AGENTS.md` warns
  arrive on someone else's thread.

- **The gateway test is stable across the component's lifetime.** The
  role is fixed from `PMIX_SERVER_GATEWAY` in `PMIx_server_init` before
  the framework opens, and the framework closes in
  `PMIx_server_finalize` before `pmix_globals.mypeer` is released — so
  the `PMIX_PEER_IS_GATEWAY` guards in `tcp_init` and `tcp_finalize`
  cannot disagree about which lists exist.

- **`generate_key` seeds once and keeps drawing from that stream.** It
  used to re-seed from `time(NULL)` on every call, which handed two jobs
  allocated in the same second the identical "unique" key.

## Gotchas

- **It is an example, not production code.** `tcp` illustrates the
  static-port-allocation design; `opa` and `nvd` are the other current
  references. The port-allocation path only does real work on the gateway
  with `static_ports` configured.
- **The port-recycling lives in the tracker destructor.** Keep the
  invariant that releasing a `tcp_port_tracker_t` returns its ports to its
  `src` pool — that is what makes ports reusable across successive jobs.
- **MCA params register at runtime when built.** With `--with-tcp`,
  `static_ports` / `default_network_allocation` / `include_envars` /
  `exclude_envars` are available (visible via `pmix_info --all` under
  `MCA pnet tcp`) whether or not the selection gate lets the component
  open — registration happens before open, which is what keeps the
  parameters discoverable. Without `--with-tcp`, the component is absent
  entirely.

## Tests

[`test/unit/pnet_tcp_ports.c`](../../../../test/unit/pnet_tcp_ports.c)
drives the allocator through `PMIx_server_setup_application` /
`PMIx_server_deregister_nspace` with a deliberately tiny pool, so an
exhaustion boundary proves that a deregistered job's ports came back. It
also unpacks the `PMIX_TCP_SETUP_APP_KEY` blob and checks it against what
`setup_local_network` expects to find there.

Because the component is opt-in at configure time, the test asks
`pmix_mca_base_var_find("pmix", "pnet", "tcp", "static_ports")` whether it
was built and exits **77** (automake's "skip") when it was not. That probe
answers "built", not "selected" — parameter registration happens for any
built component whether or not the selection gate lets it open.
