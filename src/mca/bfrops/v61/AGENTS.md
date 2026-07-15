<!--
  Copyright (c) 2026      Nanook Consulting  All rights reserved.

  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
-->

# AGENTS.md: The BFROPS `v61` Component

`v61` is the `bfrops` component for the **PMIx 6.1** wire format. Read the
framework [`AGENTS.md`](../AGENTS.md) first; this file covers only what is
specific to `v61`. It is the **highest-priority (70)** component, so it is
the module `pmix_bfrops_base_assign_module(NULL)` hands back — i.e. the
native format a freshly-built process uses for itself and offers to peers.

## Files

| File | Contents |
|------|----------|
| `bfrop_pmix61.h` | Declares the component and module symbols. |
| `bfrop_pmix61_component.c` | Component struct + `component_query` (priority **70**), `component_open`/`close` (build/tear down the `types` array), `assign_module`. |
| `bfrop_pmix61.c` | The module `pmix_bfrops_pmix61_module` and its `init` type-registration table. |

## When it is selected

`v61` is always available (no `configure.m4`, no runtime gate;
`component_query` unconditionally returns the module at priority 70).
Being highest priority, it wins `assign_module(NULL)` and is the module
chosen for any peer that advertises `"v61"`.

## The module

A **modern** component: it delegates all real work to the base.

```c
pmix_bfrops_module_t pmix_bfrops_pmix61_module = {
    .version = "v61",
    .init = init, .finalize = finalize,
    .pack = pmix61_pack, .unpack = pmix61_unpack,
    .copy = pmix61_copy, .print = pmix61_print,
    .copy_payload = pmix_bfrops_base_copy_payload,
    .value_xfer   = pmix_bfrops_base_value_xfer,
    .value_load   = pmix_bfrops_base_value_load,
    .value_unload = pmix_bfrops_base_value_unload,
    .value_cmp    = pmix_bfrops_base_value_cmp,
    .data_type_string = data_type_string
};
```

`pmix61_pack`/`unpack`/`copy`/`print` are one-line trampolines into
`pmix_bfrops_base_pack`/… passing `&pmix_mca_bfrops_v61_component.types`.
Integers use the **flexible (squashed) base-7 varint** encoding via the
base `pack_general_int`. There is no `v61`-specific serialization code at
all — the version is defined entirely by its `init` table.

## What its `init` registers

`v61` registers the full modern type set: every type `v51` registers,
**plus** three types new in 6.1:

| Added type | Handlers |
|------------|----------|
| `PMIX_ALLOC_INHERIT` | `pack_alloc_inheritance` / `unpack_alloc_inheritance` / `std_copy` / `print_alloc_inheritance` |
| `PMIX_NODE_PID` | `pack_nodepid` / `unpack_nodepid` / `copy_nodepid` / `print_nodepid` |
| `PMIX_REGEX2` | `pack_regex2` / `unpack_regex2` / `copy_regex2` / `print_regex2` |

(69 registered types total.) All handlers are `pmix_bfrops_base_*`
functions; `finalize` frees the type array.

## Gotchas

- **This is the version everyone else negotiates *down* from.** When you
  add a new PMIx data type, register it **here** (and add its base
  handlers) — but do **not** add it to `v51` or any older component, or
  you will offer a type to peers that cannot unpack it.
- **A new wire format is a new component, not an edit to `v61`.** Once
  6.1 has shipped, `v61`'s bytes are frozen. A 6.2 encoding change means
  a `v62` component with a higher priority; `v61` stays to talk to 6.1
  peers.
- Because `v61` shares the base pack code with `v51`/`v41`/`v4`, the only
  wire difference from `v51` is the three added types above. Everything
  both versions register is byte-identical.
