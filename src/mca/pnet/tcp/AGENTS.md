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

**Status:** `tcp` compiles against the current `pmix_pnet_module_t`, but
it is **opt-in** — its `configure.m4` is guarded by `--with-tcp`, so it is
**not built by default** (like `simptest`). When you *do* build it, note
that unlike `nvd`/`opa` it has **no hardware gate** — `component_open`
always succeeds and `component_query` always returns the module at
priority **5** — so it is then **selected into `actives` on every
server**. Its entry points self-gate: `allocate`/`tcp_init`/
`deregister_nspace` act only for the gateway role, and
`setup_local_network`/`deliver_inventory` act only on their own blob key.
`collect_inventory`, however, runs on every server and reports the local
TCP interfaces.

## Files

| File | Contents |
|------|----------|
| `pnet_tcp.h` | Component struct type (`static_ports`, `default_request`, `costmatrix`, …). |
| `pnet_tcp_component.c` | Component struct, `component_register` (MCA params), `component_query` (priority **5**). |
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

- **`component_open`/`component_query`** return `pmix_tcp_module` at
  priority **5** (unconditionally — the real gating is inside the module
  on `PMIX_PEER_IS_GATEWAY`).
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
  kvals, and caches them as job info via `PMIX_GDS_CACHE_JOB_INFO`.
- **`deregister_nspace`** (gateway only) finds and releases the job's
  `tcp_port_tracker_t`, which frees the allocation and recycles its ports.
- **`collect_inventory`** enumerates non-loopback, non-virtual IPv4/IPv6
  interfaces via the `pmix_if*` API and packs `(device, tcp[46]://addr)`
  pairs into an inventory blob keyed `PMIX_TCP_INVENTORY_KEY`;
  **`deliver_inventory`** unpacks such blobs into the component-local
  `nodes` tree (see the interface notes above).

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
  `MCA pnet tcp`). Without it, the component is absent entirely.
