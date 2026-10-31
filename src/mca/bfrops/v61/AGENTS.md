<!--
  Copyright (c) 2026      Nanook Consulting  All rights reserved.

  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
-->

# AGENTS.md: The BFROPS `v61` Component

`v61` is the `bfrops` component for the **PMIx 6.1** wire format. Read the
framework [`AGENTS.md`](../AGENTS.md) first; this file covers only what is
specific to `v61`. Its priority (65) sits between `v51` (60) and `v70`
(70), so it is chosen for a peer that names `"v61"`, which a v6.1 peer
does. It is not this build's native format; `v70` is.

## Files

| File | Contents |
|------|----------|
| `bfrop_pmix61.h` | Declares the component and module symbols. |
| `bfrop_pmix61_component.c` | Component struct + `component_query` (priority **65**), `component_open`/`close` (build/tear down the `types` array), `assign_module`. |
| `bfrop_pmix61.c` | The module `pmix_bfrops_pmix61_module` and its `init` type-registration table. |

## When it is selected

`v61` is always available (no `configure.m4`, no runtime gate;
`component_query` unconditionally returns the module at priority 65).
It is the module chosen for any peer that advertises `"v61"`, and the one
a tool picks for a v6.1 server whose file names only its version.

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

`v61` registers exactly what v6.1.0 registered: every type `v51`
registers, **plus** one:

| Added type | Handlers |
|------------|----------|
| `PMIX_NODE_PID` | `pack_nodepid` / `unpack_nodepid` / `copy_nodepid` / `print_nodepid` |

(67 registered types total.) `PMIX_ALLOC_INHERIT` and `PMIX_REGEX2` were
added after v6.1.0 and belong to `v70`. Master once registered them here
too, so a v7 peer and a v6.1 peer both said `v61` while speaking different
sets; they were taken out again for that reason.
`test/unit/bfrops_versions.c` checks the set. All handlers are `pmix_bfrops_base_*`
functions; `finalize` frees the type array.

## Gotchas

- **Frozen.** v6.1.0 shipped this component, so its type set must not
  change. A new PMIx data type goes in the newest component (`v70`),
  never here.
- Because `v61` shares the base pack code with `v51`/`v41`/`v4`, the only
  wire difference from `v51` is the one added type above. Everything
  both versions register is byte-identical.
