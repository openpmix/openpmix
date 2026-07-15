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

**Status warning:** `tcp` is **not built** (hardwired off in
`configure.m4`) *and* is **stale** — its module functions no longer match
the current `pmix_pnet_module_t`, and it references base symbols that no
longer exist. It would not compile against today's tree. Read it as a
historical example of the intended port-allocation design, not as working
code.

## Files

| File | Contents |
|------|----------|
| `pnet_tcp.h` | Component struct type (`static_ports`, `default_request`, `costmatrix`, …). |
| `pnet_tcp_component.c` | Component struct, `component_register` (MCA params), `component_query` (priority **5**). |
| `pnet_tcp.c` | The module: init/finalize, allocate, setup_local_network, setup_fork, finalize hooks, collect/deliver inventory, and the `process_request` port assigner. |
| `configure.m4` | Hardwired **off** (`test "yes" = "no"`); adds the `TCP` summary line. |

## Why it does not compile against HEAD

The module struct it fills in uses an **older** interface:

- It sets a `.setup_fork` slot — but `pmix_pnet_module_t` has **no
  `setup_fork` field** any more (fork-time envar injection is base-only).
- `collect_inventory` is declared `(directives, ndirs,
  pmix_inventory_cbfunc_t cbfunc, void *cbdata)` and `deliver_inventory`
  `(info, ninfo, directives, ndirs, pmix_op_cbfunc_t cbfunc, void *cbdata)`
  — the current typedefs take a plain `pmix_list_t *inventory` and no
  callbacks, respectively.
- `setup_local_network` takes `pmix_namespace_t *` rather than the
  current `pmix_nspace_env_cache_t *`.
- `deliver_inventory` references `pmix_pnet_globals.nodes`,
  `pmix_pnet_node_t`, and `pmix_pnet_resource_t`, none of which exist in
  the current `base/base.h`.

Any resurrection must port all of the above to the current framework
header — do not change the header to match this file.

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
  **`deliver_inventory`** unpacks such blobs into the (now-removed)
  per-node resource lists.

## Gotchas

- **Do not treat this as a template.** Prefer [`opa`](../opa/AGENTS.md) or
  [`nvd`](../nvd/AGENTS.md), which match the current interface. This file
  is an archaeology reference for the port-allocation idea only.
- **The port-recycling lives in the tracker destructor.** If you ever port
  this forward, keep the invariant that releasing a `tcp_port_tracker_t`
  returns its ports to its `src` pool — that is what makes ports reusable
  across successive jobs.
- **MCA params still register.** `component_register` is on the built
  code path only if the component is built; today it is not, so
  `static_ports` / `default_network_allocation` / `include_envars` /
  `exclude_envars` are not actually available at runtime.
