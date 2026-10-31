<!--
  Copyright (c) 2026      Nanook Consulting  All rights reserved.

  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
-->

# AGENTS.md: The BFROPS `v70` Component

`v70` is the `bfrops` component for the **PMIx 7.0** wire format. Read the
framework [`AGENTS.md`](../AGENTS.md) first; this file covers only what is
specific to `v70`. It is the **highest-priority (70)** component, so it is
the module `pmix_bfrops_base_assign_module(NULL)` hands back — i.e. the
native format a freshly-built process uses for itself and offers to peers.

## Files

| File | Contents |
|------|----------|
| `bfrop_pmix70.h` | Declares the component and module symbols. |
| `bfrop_pmix70_component.c` | Component struct + `component_query` (priority **70**), `component_open`/`close` (build/tear down the `types` array), `assign_module`. |
| `bfrop_pmix70.c` | The module `pmix_bfrops_pmix70_module` and its `init` type-registration table. |

## When it is selected

`v70` is always available (no `configure.m4`, no runtime gate;
`component_query` unconditionally returns the module at priority 70).
Being highest priority, it wins `assign_module(NULL)` and is the module
chosen for any peer that advertises `"v70"`.

## The module

A **modern** component: it delegates all real work to the base.

```c
pmix_bfrops_module_t pmix_bfrops_pmix70_module = {
    .version = "v70",
    .init = init, .finalize = finalize,
    .pack = pmix70_pack, .unpack = pmix70_unpack,
    .copy = pmix70_copy, .print = pmix70_print,
    .copy_payload = pmix_bfrops_base_copy_payload,
    .value_xfer   = pmix_bfrops_base_value_xfer,
    .value_load   = pmix_bfrops_base_value_load,
    .value_unload = pmix_bfrops_base_value_unload,
    .value_cmp    = pmix_bfrops_base_value_cmp,
    .data_type_string = data_type_string
};
```

`pmix70_pack`/`unpack`/`copy`/`print` are one-line trampolines into
`pmix_bfrops_base_pack`/… passing `&pmix_mca_bfrops_v70_component.types`.
Integers use the **flexible (squashed) base-7 varint** encoding via the
base `pack_general_int`. There is no `v70`-specific serialization code at
all — the version is defined entirely by its `init` table.

## What its `init` registers

`v70` registers the full modern type set: every type `v61` registers,
**plus** the two added after v6.1.0:

| Added type | Handlers |
|------------|----------|
| `PMIX_ALLOC_INHERIT` | `pack_alloc_inheritance` / `unpack_alloc_inheritance` / `std_copy` / `print_alloc_inheritance` |
| `PMIX_REGEX2` | `pack_regex2` / `unpack_regex2` / `copy_regex2` / `print_regex2` |

(69 registered types total.) `test/unit/bfrops_versions.c` checks that
the set is exactly `v61`'s plus these two. All handlers are `pmix_bfrops_base_*`
functions; `finalize` frees the type array.

## Gotchas

- **This is the version everyone else negotiates *down* from.** When you
  add a new PMIx data type, register it **here** (and add its base
  handlers) — but do **not** add it to `v51` or any older component, or
  you will offer a type to peers that cannot unpack it.
- **A new wire format is a new component, not an edit to `v70`.** Once
  v7.0.0 has shipped, `v70`'s type set is frozen. A type added for 7.1
  means a `v71` component with a higher priority; `v70` stays to talk to
  7.0 peers. Until v7.0.0 ships, new types still go here.
- **Why this component exists:** `v61` shipped in v6.1.0, and master then
  went on adding types to it, so the name meant two formats. See "Why
  `v70` exists" in the framework [`AGENTS.md`](../AGENTS.md).
- Because `v70` shares the base pack code with `v61`/`v51`/`v41`/`v4`, the
  only wire difference from `v61` is the two added types above. Everything
  both versions register is byte-identical.
