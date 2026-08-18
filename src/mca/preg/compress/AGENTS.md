<!--
  Copyright (c) 2026      Nanook Consulting  All rights reserved.

  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
-->

# AGENTS.md: The PREG `compress` Component

`compress` is the `preg` component that runs the node/proc list through
the `pcompress` framework to produce a compressed binary encoding. Read
the framework [`AGENTS.md`](../AGENTS.md) first; this file covers only
what is specific to `compress`. It is the highest-priority component and
usually the one that actually encodes job maps in production.

## Files

| File | Contents |
|------|----------|
| `preg_compress.h` | Declares the component and module symbols. |
| `preg_compress_component.c` | Component struct + `component_query`. |
| `preg_compress.c` | The module: `generate_regex` and `parse_regex`. |

## Availability — this component disables itself

Unlike `raw`, `compress` is only useful when a compression back end
exists. Its `component_query`:

```c
if (NULL == pmix_compress.compress_string) {
    return PMIX_ERROR;   // no module -> not selected
}
*priority = 100;         // highest priority when we ARE available
```

So `compress` is in `pmix_preg_globals.actives` **only** when a
`pcompress` component is loaded. Never assume its symbols are in the
active list — everything downstream is written to degrade to `raw` when
`compress` is absent. It depends on
[`src/mca/pcompress/pcompress.h`](../../pcompress/pcompress.h) for
`pmix_compress.compress_string` / `decompress_string`. Note that
`pcompress` has several components (`zlib`, `zlibng`, `zstd`, `lz4`);
`compress` neither knows nor cares which one answers.

## What it does

- **`generate_regex`** — `compress_string(input, &compressed, &len)`, then
  `regex->type = "compress"`, `regex->bytes = compressed`, `regex->len =
  len`. Returns `PMIX_ERR_TAKE_NEXT_OPTION` if compression is unavailable
  or fails. Because it produces the smallest encoding for any non-trivial
  list, it almost always wins the framework's **smallest-wins** contest.
- **`parse_regex`** — gates on `regex->type == "compress"` (returning
  `PMIX_ERR_TAKE_NEXT_OPTION` otherwise), then
  `decompress_string(&tmp, regex->bytes, regex->len)` and returns the
  decompressed list. The caller splits it on whichever delimiter it
  supplied.

Both ignore their `info`/`ninfo` arrays for now
(`PMIX_HIDE_UNUSED_PARAMS`).

The payload is the **literal delimited list**, compressed — there is no
range compression and no interpretation of the values, so the same code
serves a node list and a process map.

## The deprecated `char *` form is not this component's problem

A caller coming in through `PMIx_generate_regex` / `PMIx_generate_ppn`
gets a serialized `pmix_regex2_t`:

```
"blob:\0" "component=zlib:\0" "size=<N>:\0" <N bytes of compressed data>
```

That framing, and the offset arithmetic that walks it, live entirely in
[`preg_base_legacy.c`](../base/preg_base_legacy.c). `compress` used to
carry eight more entry points to produce and consume it; it no longer
does, and nothing should put them back.

Two things about that layout are worth knowing anyway, because you will
see it in packet dumps:

- **`component=zlib:` is a fixed label, not a claim about the
  compressor.** It was emitted unconditionally when zlib was the only
  option, and by the time the other `pcompress` components arrived it was
  frozen on the wire. Nothing selects a decompressor from it.
- **The blob contains embedded NUL bytes.** `strlen`/`strdup` are wrong
  for it; that is exactly why `pmix_regex2_t` carries an explicit `len`
  and why the deprecated form has to reconstruct one.

## Gotchas

- `decompress_string` failure in `parse_regex` returns
  `PMIX_ERR_TAKE_NEXT_OPTION`; keep that "decline, don't hard-error"
  behavior so a corrupt or foreign blob can still be retried by another
  scheme.
- The module name `"compress"` is the on-the-wire type tag, and
  `preg_base_legacy.c` keys its `blob:` serialization on that exact
  string. Renaming the component breaks both.
- Because `compress` sits at priority 100, it is tried first on every
  generate — but it is also the component most likely to be *absent* (no
  compression library). Code and tests must exercise both the present and
  absent configurations.
