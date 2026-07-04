<!--
  Copyright (c) 2026      Nanook Consulting  All rights reserved.

  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
-->

# AGENTS.md: The PREG `native` Component

`native` is the `preg` component that implements PMIx's own
range-compressing regular-expression format — the `pmix[...]` encoding.
Read the framework [`AGENTS.md`](../AGENTS.md) first; this file covers
only what is specific to `native`. It is the component that gives the
`pmix` tag its meaning, and it is the parser of record for the human-
readable range format that external launchers (e.g. PRRTE) also produce.

## Files

| File | Contents |
|------|----------|
| `preg_native.h` | Declares the component and module symbols. |
| `preg_native_component.c` | Component struct + `component_query` (priority **30**, always available). |
| `preg_native.c` | The module: generate/parse/copy/pack/unpack/release + the range-parsing helpers. |

`native` implements the entire **legacy string API** but leaves the
regex2 slots `NULL` (`.generate_regex = NULL`, no `parse_regex`). It never
participates in the smallest-wins regex2 generation path; it works purely
in the `char *` world.

## The `pmix[...]` format

`native` compresses a comma-separated list of names into ranges, grouping
names that share a *prefix*, a *number-field width*, and a *suffix*:

```
Input:   odin009,odin010,odin011,odin012,odin017,odin018,thor176
Output:  pmix[odin[3:9-12,17-18],thor176]
```

Reading the encoding:

- The whole thing is wrapped `pmix[ ... ]`; the leading `pmix` is the tag
  that `parse_nodes`/`parse_procs` gate on.
- Inside, each element is either a bare name (`thor176`, uncompressible)
  or `prefix[W:ranges]suffix`, where **`W` is the number of digits** in
  the zero-padded numeric field. That width is what lets the parser
  regenerate `009` rather than `9` — leading zeros are preserved.
- `ranges` is a comma list of `start-end` runs (or bare singletons).

The `generate_ppn` variant emits a semicolon-delimited `pmix[...]` where
each node's rank ranges are separated by `;` instead of `,`.

## Generation: `generate_node_regex`

This is the algorithmic heart of the component. It builds a list of
`pmix_regex_value_t` (one per distinct prefix/width/suffix shape), each
holding a list of `pmix_regex_range_t` runs, then serializes them.

The loop walks the input names left to right and, for each, isolates the
prefix (leading alphabetic run), the numeric field (its digit count is the
width `W`), and any trailing suffix. Names that contain non-digit,
non-alpha characters, or that have no numeric field, are stored verbatim
and never compressed.

### The `skip` flag — ordering preservation (read this before editing)

The subtle part is that **the parser must reproduce the input order**, and
ranges only merge with the *last* range on a matching value. Consider:

```
a28n01, a99n02, a28n02
```

Naively, `a28n02` would merge with the `a28n01` value, "jumping over"
`a99n02` and corrupting the order when expanded. To prevent this, when a
candidate name fails to match an existing `pmix_regex_value_t` on prefix,
suffix, or width, that value is marked `skip = true` and excluded from all
*future* matches. So once `a99n02` (a different prefix) is seen, the
`a28...` value is frozen and `a28n02` starts a fresh value. The comment
block in `generate_node_regex` documents this with the same example —
**do not remove the `skip` logic** thinking it's redundant; it is the
ordering guarantee.

A name is appended to a range only if `vnum == range->start + range->cnt`
(strictly the next integer); otherwise a new range is started. This is
what turns `9,10,11,12` into `9-12` but leaves `9,11` as two runs.

### Serialization and the return code

After the list is built, each value is emitted as `prefix[W:ranges]suffix`
(or just the bare name if it had no ranges), the pieces are joined with
commas, and the whole is wrapped in `pmix[...]`. If **nothing** was
compressible (`regexargs` came out empty), it returns
`PMIX_ERR_TAKE_NEXT_OPTION` so the base moves on. Memory-allocation
failures return `PMIX_ERR_NOMEM`.

`generate_ppn` is a simpler cousin: it splits the input by `;` (nodes)
then `,` (ranges per node), coalesces consecutive ranks into ranges, and
emits `pmix[...]` with `;` node separators. It has a **size guard**: if
the encoded form is longer than the raw input, it returns
`PMIX_ERR_TAKE_NEXT_OPTION` rather than bloating the data.

## Parsing: `parse_nodes` / `parse_procs`

Both strip the trailing `]`, split off the leading tag at the first `[`,
and check it equals `"pmix"`. If it does not, they return
`PMIX_ERR_TAKE_NEXT_OPTION` — this is *not* an error, it just means the
blob belongs to another scheme. A malformed `pmix` blob (no `[`) returns
`PMIX_ERR_BAD_PARAM`.

`parse_nodes` delegates to **`pmix_regex_extract_nodes`**, a hand-written
scanner that walks the range string, and for each `prefix[W:ranges]suffix`
group calls `regex_parse_value_ranges` → `regex_parse_value_range`. The
inner routine reconstructs each name by:

1. copying the prefix,
2. writing `W` zero characters, then overwriting the low-order digits with
   the decimal of the current integer (this is the zero-padding logic),
3. appending the suffix if present,

for every integer from `start` to `end` inclusive. Bare singleton values
(no `[`) are appended as-is.

`parse_procs` delegates to **`pmix_regex_extract_ppn`**, which splits on
`;` (nodes), then `,` (ranks), expands each `start-end` run into
individual rank strings, and rejoins each node's ranks with commas so the
result is one argv entry per node.

### Editing the parser

These scanners are pointer-arithmetic-heavy and index into a mutable copy
of the string (they poke `\0` terminators in place). The classic hazards
are off-by-one on the width padding, mishandling a suffix that itself
contains no comma, and losing the "more to come" state across a range/
singleton boundary. Any change here needs round-trip tests against
`generate_node_regex` output including: leading zeros, suffixes,
multi-range values, and bare uncompressible names interleaved with ranges.

## copy / pack / unpack / release

Because the `pmix[...]` form is a genuine NUL-terminated string with no
embedded NULs, these are straightforward:

- `copy` — `strdup` + `strlen+1`, gated on the input starting with `pmix`.
- `pack` — `pmix_bfrop_buffer_extend` then `memcpy` the string (incl.
  terminator) directly into the buffer, gated on the `pmix` prefix.
- `unpack` — read from `buffer->unpack_ptr`, `strdup`, advance the pointer
  by `strlen+1`, gated on the `pmix` prefix.
- `release` — `free`, gated on the `pmix` prefix.

Every one returns `PMIX_ERR_TAKE_NEXT_OPTION` when the value is not tagged
`pmix`, so the base tries the next scheme. The prefix check is only 4
bytes (`strncmp(input, "pmix", 4)`) — note it does **not** verify the
`[` follows, so a hypothetical scheme tagged `pmixفoo` would false-match;
in practice no other tag begins with `pmix`.

## Gotchas

- `native` never sets `.generate_regex`, so it contributes nothing to
  `PMIx_generate_regex2`. If you want the `pmix[...]` encoding available
  through the regex2 API, you would add a `generate_regex`/`parse_regex`
  pair here — but weigh that against `compress`, which almost always wins
  the smallest-wins contest anyway.
- Its generators are shadowed by `raw`/`compress` on the legacy path (see
  the framework `AGENTS.md`), so in a default build `native` is exercised
  mainly as a **parser** of data produced elsewhere. Test it as a parser,
  not just as a round-tripper against its own generator.
- `PMIX_MAX_NODE_PREFIX` (8192) caps the prefix buffer; names with longer
  prefixes are truncated into the fixed `prefix[]` array. This is a real,
  if generous, limit.
