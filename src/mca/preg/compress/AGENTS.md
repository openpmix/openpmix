<!--
  Copyright (c) 2026      Nanook Consulting  All rights reserved.

  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
-->

# AGENTS.md: The PREG `compress` Component

`compress` is the `preg` component that runs the node/proc list through
the `pcompress` framework (zlib) to produce a binary `blob:` encoding.
Read the framework [`AGENTS.md`](../AGENTS.md) first; this file covers only
what is specific to `compress`. It is the highest-priority component and
usually the one that actually encodes job maps in production.

## Files

| File | Contents |
|------|----------|
| `preg_compress.h` | Declares the component and module symbols. |
| `preg_compress_component.c` | Component struct + `component_query`. |
| `preg_compress.c` | The module: all legacy string functions + both regex2 functions + the `blob:` framing helpers. |

## Availability — this component disables itself

Unlike `raw` and `native`, `compress` is only useful when a compression
back end exists. Its `component_query`:

```c
if (NULL == pmix_compress.compress_string) {
    return PMIX_ERROR;   // no module -> not selected
}
*priority = 100;         // highest priority when we ARE available
```

So `compress` is in `pmix_preg_globals.actives` **only** when a
`pcompress` component (zlib) is loaded. Never assume its symbols are in
the active list — everything downstream is written to degrade to `raw`
when `compress` is absent. It depends on
[`src/mca/pcompress/pcompress.h`](../../pcompress/pcompress.h) for
`pmix_compress.compress_string` / `decompress_string`.

## The `blob:` framing

The legacy string form is **not** a printable string — it is a header
followed by raw compressed bytes, and it contains **embedded NUL bytes**.
`pack_blob` lays it out as a sequence of NUL-terminated tokens followed by
the payload:

```
"blob:\0" "component=zlib:\0" "size=<N>:\0" <N bytes of zlib data>
```

The parsers walk it by stepping over each token's NUL terminator
(`idx += strlen(&buf[idx]) + 1`), read `<N>` with `strtoul`, then jump two
bytes (the `:` and its NUL) to reach the payload. This is why `compress`
**must** implement its own `copy`/`pack`/`unpack`/`release`: `strlen` and
`strdup` would stop at the first embedded NUL and truncate the blob. The
true byte length is reconstructed as
`N + strlen(PREG_COMPRESS_PREFIX) + strlen("<N>") + 1`.

Every parse/copy/pack/unpack/release first checks the value begins with
`blob` **and** that the second token is `component=zlib:` — both gates must
pass, otherwise it returns `PMIX_ERR_TAKE_NEXT_OPTION`. The second check
ensures `compress` only claims blobs it (a zlib producer) generated, not
some future `blob:` variant.

## Legacy string operations

- **`generate_node_regex` / `generate_ppn`** — call
  `pmix_compress.compress_string(input, &tmp, &len)`; if compression fails
  (or is unavailable) return `PMIX_ERR_TAKE_NEXT_OPTION`, else frame the
  bytes with `pack_blob`. Note: these compress the **raw input list**, not
  a `native` regex — the payload is the literal comma/semicolon string.
- **`parse_nodes` / `parse_procs`** — validate the framing, `strtoul` the
  size, `decompress_string` back to the original list, then
  `PMIx_Argv_split` on `,` (nodes) or `;` (procs). Because the compressed
  payload is just the original delimited list, splitting is all that
  remains.
- **`copy` / `pack` / `unpack`** — reconstruct the true byte length as
  above and `memcpy`/`calloc` the exact bytes; never treat the blob as a
  C string.
- **`release`** — validates the framing then `free`s.

## regex2 operations

The regex2 path is cleaner because `pmix_regex2_t` already carries an
explicit `len`, so no `blob:` framing is needed:

- **`generate_regex`** — `compress_string(input, &compressed, &len)`, then
  `regex->type = "compress"`, `regex->bytes = compressed`, `regex->len =
  len`. Returns `PMIX_ERR_TAKE_NEXT_OPTION` if compression is unavailable.
  Because it produces the smallest encoding for any non-trivial list, it
  almost always wins the framework's **smallest-wins** `generate_regex`
  contest.
- **`parse_regex`** — gates on `regex->type == "compress"`, then
  `decompress_string(&tmp, regex->bytes, regex->len)` and returns the
  decompressed node string. The caller (`gds/hash`) then runs the legacy
  `parse_nodes` on that string to expand it.

Both ignore their `info`/`ninfo` arrays for now
(`PMIX_HIDE_UNUSED_PARAMS`).

## Note: legacy `blob:` tag vs. regex2 `compress` type

The two API generations use **different tags** for the same component:
the legacy string form is tagged `blob:` (with a `component=zlib:` marker),
while the regex2 form sets `type = "compress"` (the module name). They are
not interchangeable — a `blob:` string will not be recognized by
`parse_regex`, and a regex2 `compress` blob is raw zlib output with no
`blob:` header. Keep them straight when tracing data across the two paths.

## Gotchas

- All the framing offset arithmetic in the legacy path is fragile by
  nature (manual `idx` stepping over embedded NULs). If you change
  `PREG_COMPRESS_PREFIX` or the token layout in `pack_blob`, every parser's
  offset math must change with it — they do not share a parsing helper.
- `decompress_string` failure paths in `parse_nodes`/`parse_procs`/
  `parse_regex` `free(tmp)` and return `PMIX_ERR_TAKE_NEXT_OPTION`; keep
  that "decline, don't hard-error" behavior so a corrupt or foreign blob
  can still be retried by another scheme or the base fallback.
- Because `compress` sits at priority 100, it is tried first on every
  legacy generate — but it is also the component most likely to be
  *absent* (no zlib). Code and tests must exercise both the present and
  absent configurations.
