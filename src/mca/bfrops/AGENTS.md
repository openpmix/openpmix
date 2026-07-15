<!--
  Copyright (c) 2026      Nanook Consulting  All rights reserved.

  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
-->

# AGENTS.md: The BFROPS Framework

This document orients AI agents and human contributors working in the
`bfrops` (PMIx **B**uffer **Op**eration**s** — the framework declares
itself as "PMIx Buffer Operations") framework. It assumes you have
already read the
top-level [`AGENTS.md`](../../../AGENTS.md) — the golden rules, prefix
conventions, thread-safety model, MCA concepts, and especially the
**Version Interoperability / wire-format stability** rules described
there all apply here and are not repeated. This file covers what is
specific to `bfrops`: what the framework is for, why it is multi-select,
how a component maps to a wire-format version, the very large body of
shared code in `base/`, and the rules you must obey to avoid silently
breaking cross-version communication. Each component subdirectory
(`v12/`, `v20/`, `v21/`, `v3/`, `v4/`, `v41/`, `v51/`, `v61/`) carries
its own `AGENTS.md` describing that version's deltas.

`bfrops` is the most wire-sensitive framework in the tree. Read the
top-level interoperability section before touching anything here.

## What BFROPS does

`bfrops` is the serialization engine of PMIx. Every piece of data that
travels between a PMIx process and its server — and everything stored in
the `gds` datastore — passes through this framework. It owns the
`pmix_buffer_t` and provides, for every PMIx data type, the four core
operations:

- **pack** — serialize one or more values of a given `pmix_data_type_t`
  into a `pmix_buffer_t`.
- **unpack** — deserialize the next value(s) of a given type back out.
- **copy** — deep-copy a value of a given type from one location to
  another (registered types can be complex structures).
- **print** — render a value as a human-readable string (debug/`peek`).

Plus a handful of value-level helpers on the module: `copy_payload`
(append one buffer's payload to another), `value_xfer` / `value_load` /
`value_unload` (move data in and out of a `pmix_value_t`), `value_cmp`
(compare two `pmix_value_t`), and `data_type_string` (name a type).

The layer above (`ptl`, `gds`, the server/client op code) never calls the
module directly — it uses the `PMIX_BFROPS_*` macros in
[`bfrops.h`](bfrops.h) (`PMIX_BFROPS_PACK`, `PMIX_BFROPS_UNPACK`,
`PMIX_BFROPS_COPY`, …). Each macro dereferences the *peer's* assigned
module (`peer->nptr->compat.bfrops`) so that a message to a given peer is
always encoded with the format that peer understands.

## The single most important structural fact

`bfrops` is a **multi-select** framework: *all* available components are
opened, initialized, and kept "active" at once (see `bfrops.h` header
comment and `pmix_bfrop_base_select`). This is unlike a single-select
framework where exactly one component wins.

The reason is cross-version interoperability. **Each component is one
wire-format version.** A process may simultaneously talk to peers built
against several different PMIx releases; it therefore needs every
version's encoder available at the same time. When a peer is identified,
its version string is handed to `pmix_bfrops_base_assign_module`, which
returns the matching module; that module is cached on the peer
(`peer->nptr->compat.bfrops`) and used for all traffic to/from that peer.

Two independent axes of variation are handled:

1. **Data-type definitions / encodings differ between versions.** A newer
   PMIx defines new `pmix_data_type_t` values and occasionally changed
   how a family of types is encoded. Older peers cannot unpack a type
   they never knew, so we must not send it to them — assigning the peer's
   module guarantees we only ever pack types (and encodings) that peer
   supports.
2. **Buffer type differs** (described vs. non-described — see below).
   This is carried in the buffer metadata and negotiated at connection
   time, independent of the version component.

The community's intent (stated in `bfrops.h`) is that data-type
definitions change *rarely*, so new components should be rare. In
practice eight exist, spanning PMIx 1.2 through 6.1.

## Module interface (`pmix_bfrops_module_t`)

Defined in [`bfrops.h`](bfrops.h). Every component fills in all of these
(none is left `NULL` in current components):

| Field | Signature (typedef) | Purpose |
|-------|---------------------|---------|
| `version` | `char *` | the version string, identical to the component/directory name (`"v61"`, …); this is what the handshake matches on |
| `init` | `pmix_bfrop_init_fn_t` | register this version's data-type table (see registration below) |
| `finalize` | `pmix_bfrop_finalize_fn_t` | release the registered-type array |
| `pack` | `pmix_bfrop_pack_fn_t` | `(buffer, src, num_values, type)` |
| `unpack` | `pmix_bfrop_unpack_fn_t` | `(buffer, dest, *max_num_values, type)` |
| `copy` | `pmix_bfrop_copy_fn_t` | `(**dest, *src, type)` |
| `print` | `pmix_bfrop_print_fn_t` | `(**output, prefix, src, type)` |
| `copy_payload` | `pmix_bfrop_copy_payload_fn_t` | append `src` buffer payload to `dest` |
| `value_xfer` | `pmix_bfrop_value_xfer_fn_t` | copy one `pmix_value_t` to another |
| `value_load` | `pmix_bfrop_value_load_fn_t` | load raw data into a `pmix_value_t` |
| `value_unload` | `pmix_bfrop_value_unload_fn_t` | extract raw data from a `pmix_value_t` |
| `value_cmp` | `pmix_bfrop_value_cmp_fn_t` | compare two `pmix_value_t` |
| `data_type_string` | `pmix_bfrop_data_type_string_fn_t` | debug name of a type |

The header comment on the module notes the key design principle: **a
module only needs to provide the functions that *differ* from the base**
— it does not duplicate all the code. In the base-driven components `pack`,
`unpack`, `copy`, and `print` are thin trampolines into the base driver
(`pmix_bfrops_base_pack`, …), passing the component's own registered-type
array; and `copy_payload`/`value_*` point straight at the corresponding
`pmix_bfrops_base_*` functions. What actually distinguishes one modern
version from another is the *contents of its `init` type table*, not
bespoke pack code.

## Component structure (`pmix_bfrops_base_component_t`)

Also in [`bfrops.h`](bfrops.h). Beyond the standard
`pmix_mca_base_component_t base`, a bfrops component carries:

- `int priority` — higher = newer/preferred (see the table below).
- `pmix_pointer_array_t types` — **the registered-type table**, indexed
  by `pmix_data_type_t`. Constructed in `component_open`, torn down in
  `component_close`, and populated by the module's `init`.
- `assign_module` — a `component`-level function returning this
  component's module (used by the base `assign_module` dispatcher).

Every component's struct opens with `PMIX_BFROPS_BASE_VERSION_1_0_0`.

## Selection, assignment, and lifecycle

Unlike most frameworks, `bfrops` is opened and selected during **every**
role's startup (client, server, and tool), in
[`src/runtime/pmix_init.c`](../../../src/runtime/pmix_init.c) — the
framework is opened, then `pmix_bfrop_base_select()` runs. Serialization
is needed by everyone.

- **`base/bfrop_base_frame.c`** declares the framework
  (`PMIX_MCA_BASE_FRAMEWORK_DECLARE(pmix, bfrops, …)`), instantiates the
  global `pmix_bfrops_globals` state, registers the MCA parameters
  (`pmix_bfrop_register`), opens all components (`pmix_bfrop_open`), and
  defines the `PMIX_CLASS_INSTANCE`s for `pmix_buffer_t`,
  `pmix_bfrop_type_info_t`, `pmix_kval_t`, and the active-module wrapper.
- **`base/bfrop_base_select.c`** (`pmix_bfrop_base_select`) queries each
  component, calls the module's `init` (rejecting the module if `init`
  fails), and inserts the surviving modules into
  `pmix_bfrops_globals.actives` in **descending priority order**. If zero
  survive it emits the `no-plugins` `show_help` topic (from
  `help-pmix-runtime.txt`) and returns `PMIX_ERR_SILENT` — `bfrops`
  requires at least one component. At verbosity >4 it prints the resolved
  priority list.
- **`base/bfrop_base_stubs.c`** holds the exported dispatchers:
  - `pmix_bfrops_base_assign_module(version)` — **the version→component
    mapping.** If `version` is `NULL` it returns the *first* active
    module, i.e. the highest-priority (newest) one — this is how a
    process picks its own native format. If `version` is non-NULL it is a
    comma-separated list of acceptable component names; the function walks
    the priority-ordered actives and returns the first whose component
    name matches one of them. Matching is by **component name string**
    (`"v61"`, `"v4"`, …), never by numeric comparison.
  - `pmix_bfrops_base_get_available_modules()` — comma-joined list of
    active component names.
  - `PMIx_Data_type_string()` — the public type-name API, which walks the
    actives calling each `data_type_string` (falling back to a built-in
    static table before the framework is initialized).

The peer's version string that feeds `assign_module` is exchanged in the
`ptl` connection handshake (a process advertises its own
`compat.bfrops->version`); see the `ptl` framework doc. Call sites that
assign a module live in `ptl_base_connection_hdlr.c`, `ptl_client.c`,
`pmix_server.c`, `pmix_tool.c`, and `common/pmix_data.c`.

### Priority / version table

| Component | Priority | PMIx era | Encoding family | Notes |
|-----------|----------|----------|-----------------|-------|
| `v61` | 70 | 6.1 | modern (base, flex ints) | newest; native for this build |
| `v51` | 60 | 5.1 | modern | |
| `v41` | 58 | 4.1 | modern | |
| `v4`  | 50 | 4.0 | modern | **flex-int break**; dropped deprecated `MODEX`/`INFO_ARRAY` |
| `v3`  | 40 | 3.x | converged, fixed-width ints | own integer packers |
| `v21` | 30 | 2.1 | converged, fixed-width ints | first version built on the base driver |
| `v20` | 20 | 2.0 | legacy (self-contained) | own pack/unpack/copy/print files |
| `v12` | 5  | 1.2 | legacy (self-contained) | oldest supported |

Because `assign_module(NULL)` returns the highest priority, a fresh
build's local peer always uses `v61`. Two peers negotiate down to a
version both possess.

## The three families of components (read before editing any component)

The eight components fall into three tiers. Knowing which tier a
component is in tells you where its wire format actually lives.

1. **Legacy, self-contained (`v12`, `v20`).** These predate the shared
   base driver. Each has its *own* `pack.c`, `unpack.c`, `copy.c`,
   `print.c`, and `internal.h`, and its module wires `pack`/`unpack`/
   `copy`/`print`/`copy_payload`/`value_*` to `pmix20_bfrop_*` /
   `pmix12_bfrop_*` functions. Their integers are fixed-width with
   explicit byte-order handling; the buffer is the described-buffer era.
   They still carry the deprecated `PMIX_MODEX` and `PMIX_INFO_ARRAY`
   types. **Do not touch their bytes** — they exist solely to talk to
   ancient peers.
2. **Converged-but-fixed-width (`v21`, `v3`).** These use the shared base
   driver for the framing and for composite types, but register their
   *own* static integer packers (`pack_int`, `pack_int16/32/64`,
   `pack_sizet`) — so their integers are still fixed-width, not squashed.
   They keep `PMIX_MODEX`/`PMIX_INFO_ARRAY` (with per-component
   `pmixNN_bfrop_pack_modex` / `pack_array` handlers). `v21` introduced
   the base-driver framing; `v3` merely adds `PMIX_IOF_CHANNEL` and
   `PMIX_ENVAR`.
3. **Modern (`v4`, `v41`, `v51`, `v61`).** These delegate integers to
   `pmix_bfrops_base_pack_general_int` — the **flexible ("squashed")
   base-7 varint** encoding — and drop the deprecated types. Adjacent
   modern versions differ *only* by which additional data types their
   `init` registers; the pack/unpack/copy/print code is 100% shared base
   code. See each component's `AGENTS.md` for the exact per-version type
   delta.

A crucial consequence: because all modern components route `pack` through
the same base functions, **two modern components produce byte-identical
output for any type both of them register.** The version boundary is
about *which types exist*, not about re-encoding shared ones. That is
exactly why you must never edit a base pack/unpack function's byte layout
(it would change the wire format of *every* version at once).

## The base: where the bulk of the code lives

`base/` is enormous (the pack/unpack/print/cmp/get_number files alone run
to thousands of lines). It contains the actual serialization logic for
every type; components are mostly registration tables over it.

### The registered-type mechanism

The heart of the driver is the per-component `types` pointer array of
`pmix_bfrop_type_info_t` (defined in [`base/base.h`](base/base.h)):

```c
typedef struct {
    pmix_object_t super;
    pmix_data_type_t odti_type;               // the type id (array index)
    char *odti_name;                          // debug name
    pmix_bfrop_internal_pack_fn_t odti_pack_fn;
    pmix_bfrop_internal_unpack_fn_t odti_unpack_fn;
    pmix_bfrop_copy_fn_t odti_copy_fn;
    pmix_bfrop_print_fn_t odti_print_fn;
} pmix_bfrop_type_info_t;
```

A component's `init` calls the `PMIX_REGISTER_TYPE(name, type, pack,
unpack, copy, print, &component.types)` macro once per supported type;
that allocates a `pmix_bfrop_type_info_t` and stores it at index `type`
in the array. The base driver (`PMIX_BFROPS_PACK_TYPE` /
`PMIX_BFROPS_UNPACK_TYPE` macros) looks up the handler by type id and
calls it, returning `PMIX_ERR_UNKNOWN_DATA_TYPE` if the slot is empty.
That "empty slot ⇒ unknown type" behavior is precisely how an older
component safely rejects a newer type.

### Base source files

| File | Contents |
|------|----------|
| `base.h` | internal API: the type-info struct, `PMIX_REGISTER_TYPE`, the pack/unpack driver macros, size/`squash` helper macros, and prototypes for every `pmix_bfrops_base_pack_*` / `_unpack_*` / `_copy_*` / `_print_*` |
| `bfrop_base_frame.c` | framework decl, MCA params, globals, class instances (`pmix_buffer_t`, `pmix_kval_t`, `pmix_bfrop_type_info_t`) |
| `bfrop_base_select.c` | build the priority-ordered `actives` list |
| `bfrop_base_stubs.c` | `assign_module`, `get_available_modules`, `PMIx_Data_type_string`, and a static fallback type-name table |
| `bfrop_base_pack.c` | the pack driver (`pmix_bfrops_base_pack` → `_pack_buffer`) plus one `pmix_bfrops_base_pack_<type>` per data type |
| `bfrop_base_unpack.c` | the mirror-image unpack driver and per-type unpackers, incl. the size-mismatch (`UNPACK_SIZE_MISMATCH`) heterogeneity handling |
| `bfrop_base_copy.c` | per-type deep-copy functions and `pmix_bfrops_base_std_copy` (the trivial fixed-size copy) |
| `bfrop_base_print.c` | per-type `print` functions rendering values to strings |
| `bfrop_base_cmp.c` | per-type comparison helpers behind `pmix_bfrops_base_value_cmp` |
| `bfrop_base_squash.c` | the **flexible integer** codec: `encode_int`/`decode_int`/`get_max_size` and the base-7 varint (`flex_pack_integer`) used by modern components |
| `bfrop_base_get_number.c` | `PMIx_Value_get_number` and its per-numeric-type range/precision `check_*` helpers |
| `bfrop_base_fns.c` | buffer helpers (`buffer_extend`, `too_small`, `store`/`get_data_type`), `value_load`/`value_unload`/`value_xfer`/`value_destruct`, and a large block of **public** `PMIx_Info_list_*` / `PMIx_Value_get_size` / `PMIx_Info_get_size` utility APIs |
| `bfrop_base_macro_backers.c` | the out-of-line function bodies **backing the public inline PMIx utility macros** — `PMIx_Load_key`, `PMIx_Check_key`, the `PMIx_Argv_*` family, `PMIx_Value_*` / `PMIx_Info_*` construct/xfer/etc. |
| `bfrop_base_tma.h` | inline "TMA" (custom memory-allocator) variants of the buffer/value helpers; the non-TMA public entry points pass `tma == NULL` to use the default heap |

Note the last two entries: a surprising amount of the **public PMIx C
API surface** (argv utilities, info-list builders, value construct/load/
compare, `PMIx_Value_get_number`) is actually implemented here in
`bfrops/base`, because it is all fundamentally data-manipulation code.
Do not assume `bfrops/base` is purely internal wire code.

## The buffer (`bfrops_types.h`)

The `pmix_buffer_t` is a growable byte buffer with separate `pack_ptr`
and `unpack_ptr` cursors over one `base_ptr` allocation. Its `type` field
is a `pmix_bfrop_buffer_type_t`:

- `PMIX_BFROP_BUFFER_UNDEF` (0) — freshly constructed; the first pack sets
  the real type from the owning peer.
- `PMIX_BFROP_BUFFER_NON_DESC` (1) — **non-described**: only the values
  are packed. The unpacker must know the expected type. Default in
  optimized builds.
- `PMIX_BFROP_BUFFER_FULLY_DESC` (2) — **fully described**: each item is
  preceded on the wire by its `pmix_data_type_t` (stored as a `UINT16` by
  `pmix_bfrop_store_data_type`) and the count is likewise tagged. This
  lets a buffer be unpacked without prior knowledge of its contents, at a
  size cost. Default in `PMIX_ENABLE_DEBUG` builds.

The default is chosen in `bfrop_base_frame.c` (and exposed via the
`default_type` MCA param). **Both peers must agree on the buffer type**;
mismatches yield `PMIX_ERR_PACK_MISMATCH` / `PMIX_ERR_UNPACK_FAILURE`
(see the `PMIX_BFROPS_PACK`/`UNPACK` macros). Buffer type is negotiated in
the `ptl` handshake, not carried per message. The pack/unpack drivers
branch on `buffer->type` to decide whether to emit the type tag — this is
the one place the *framing* differs at runtime within a single component.

Convenience macros here — `PMIX_LOAD_BUFFER` / `PMIX_UNLOAD_BUFFER`
(non-copying blob↔buffer transfer), `PMIX_BUFFER_IS_EMPTY`,
`PMIX_KVAL_NEW` — plus the `pmix_kval_t` key/value list item, live in
this header.

## MCA parameters (registered in `bfrop_base_frame.c`)

All under the `pmix_bfrops_base_` prefix:

| Parameter | Meaning |
|-----------|---------|
| `initial_size` | starting allocation of a new buffer (default 128 bytes) |
| `threshold_size` | size at which `buffer_extend` switches from doubling to additive growth (default 1024) |
| `default_type` | default `pmix_bfrop_buffer_type_t` for new buffers (described vs. non-described) |

## Threading

`bfrops` functions are pure, synchronous transforms of their arguments —
they allocate and return; they do not thread-shift, block, or touch
shared library state beyond the read-only `actives` list (built once at
init) and the per-component `types` array (built once in `init`). They
are safe to call from any thread that owns the buffer, and are invoked
directly from progress-thread handlers throughout the library. There is
no caddy pattern here. The one concurrency rule: do not run
pack/unpack against a buffer while another thread mutates it, and do not
call framework open/close concurrently with packing (that only happens at
startup/shutdown).

## Directory layout

```
src/mca/bfrops/
├── bfrops.h                 Framework API: module & component structs, PMIX_BFROPS_* macros, version
├── bfrops_types.h           pmix_buffer_t, buffer-type constants, pmix_kval_t, load/unload macros
├── base/
│   ├── base.h               Internal API: type-info struct, REGISTER_TYPE, driver macros, all base prototypes
│   ├── bfrop_base_tma.h     Inline custom-allocator (TMA) variants of the helpers
│   ├── bfrop_base_frame.c   framework decl, MCA params, globals, class instances
│   ├── bfrop_base_select.c  build the priority-ordered actives list
│   ├── bfrop_base_stubs.c   assign_module (version→component), available-modules, type-string
│   ├── bfrop_base_pack.c    pack driver + per-type packers
│   ├── bfrop_base_unpack.c  unpack driver + per-type unpackers
│   ├── bfrop_base_copy.c    per-type deep-copy + std_copy
│   ├── bfrop_base_print.c   per-type print-to-string
│   ├── bfrop_base_cmp.c     per-type value comparison
│   ├── bfrop_base_squash.c  flexible (base-7 varint) integer codec
│   ├── bfrop_base_get_number.c  PMIx_Value_get_number + numeric range checks
│   ├── bfrop_base_fns.c     buffer helpers, value load/unload/xfer, public Info_list APIs
│   └── bfrop_base_macro_backers.c  out-of-line bodies for public PMIx_* inline macros
├── v12/  v20/               legacy self-contained encoders (own pack/unpack/copy/print)
├── v21/  v3/                base-driver framing, own fixed-width integer packers
└── v4/  v41/  v51/  v61/    modern (flex ints), differ only by registered type set
```

## Building

All eight components are statically built into `libpmix` and wired
through the generated `base/static-components.h`; **none ships a
`configure.m4`**, so none is conditionally compiled out — every version
is always present in every build. `bfrops` ships **no `show_help` text of
its own** (the only topic it uses, `no-plugins`, lives in
`help-pmix-runtime.txt`), so the regenerate-the-help-content golden rule
does not bite here. Editing a `Makefile.am` only needs a plain `make`;
adding or removing a *component directory* changes the build wiring
resolved by `configure`, so re-run `./autogen.pl && ./configure … &&
make`.

## When working in this framework — the wire-format rules

These are the top-level interoperability rules applied to `bfrops`.
Violating them silently strands jobs across mixed-version deployments.

- **Never change an existing component's on-the-wire representation.** A
  new PMIx version that changes any encoding gets a **new component**
  (new version string, new priority), exactly as `v4` did when it
  switched to flexible integers. The old component stays to talk to old
  peers.
- **Never change the byte layout of a base pack/unpack function.** Modern
  components all share it; a change there rewrites the wire format of
  every version simultaneously. If you need a different encoding for a
  new type, add a *new* base pack function and register it only in the
  new component.
- **Add new data types append-only.** Give the new `pmix_data_type_t` the
  next id (never reuse one), add its `pack`/`unpack`/`copy`/`print` to the
  base, and register it **only in the newest component**. Older
  components must not learn it — their "empty slot ⇒ unknown type"
  behavior is what makes them refuse to send something the old peer
  cannot read.
- **Keep the four handlers symmetric.** A type's `unpack` must exactly
  mirror its `pack` field order; `copy`/`print` must handle the same
  structure. The size-mismatch machinery in `unpack` tolerates a peer
  that packed a differently-sized integer, but only within the flexible
  integer family.
- **The buffer type (described/non-described) is negotiated, not
  guessed.** Do not assume one; honor `buffer->type` and the peer's
  `compat.type`.
- **`version` must equal the component name.** `assign_module` matches on
  that string; a mismatch makes your component unreachable across a
  handshake.
