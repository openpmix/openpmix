<!--
  Copyright (c) 2026      Nanook Consulting  All rights reserved.

  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
-->

# AGENTS.md: The PREG Framework

This document orients AI agents and human contributors working in the
`preg` (**P**MIx **Reg**ular-expression) framework. It assumes you have
already read the top-level [`AGENTS.md`](../../../AGENTS.md) — the golden
rules, prefix conventions, thread-safety model, and MCA concepts
described there all apply here and are not repeated. This file covers
what is specific to `preg`: what the framework is for, the two
generations of its API, how a request flows through it, and the contract
every component must honor. Each component subdirectory carries its own
`AGENTS.md` with component-specific detail. For an integration-level view
of how these functions connect to the public server APIs and the
`gds/hash` datastore, see
[`docs/how-things-work/regex.rst`](../../../docs/how-things-work/regex.rst).

## What PREG does

`preg` exists to solve one problem: a launcher or resource manager needs
to hand a PMIx server a **map of the job** — the list of node names in
the allocation (`PMIX_NODE_MAP`) and the list of process ranks resident
on each node (`PMIX_PROC_MAP`) — and that list must then be shipped, in a
compact form, down to every client. On a machine with 100,000 nodes named
`nodeXXXXXX`, sending the raw comma-separated list to every process is
wasteful. `preg` "generates" a small encoded representation on the server
side and "parses" it back into the full argv list on the receiving side.

Two facts about the encoded value are load-bearing for the whole design:

1. **The encoding is self-identifying.** Every encoded value carries a
   *type* naming the scheme that produced it — `raw` for the
   pass-through, `compress` for the compressed blob. A component inspects
   that type to decide whether a given value is "its" to parse, and
   declines (returns `PMIX_ERR_TAKE_NEXT_OPTION`) if not. This is what
   lets a peer built with one set of components correctly parse data
   produced by a peer with a different set — the type, not the local
   configuration, selects the parser.
2. **Order is preserved.** The individual values must come back out of
   the parser in exactly the order they went into the generator.
3. **The encoding is indifferent to what the values mean.** The same
   call encodes a comma-delimited node list and a semicolon-delimited
   process map; only the delimiter the *caller* splits on differs. There
   is no separate "ppn" encoder, and there should never be one again.

`preg` is a **multi-select** framework: several components are active at
once, and a request is offered to each in priority order until one claims
it. It is one of the framework types the top-level guide calls out as
multi-select (alongside `bfrops` and `psec`).

## One component interface, two public APIs

This is the single most important thing to understand before editing
`preg`. A component implements **two functions** and nothing else:

| Module fn | Purpose |
|-----------|---------|
| `generate_regex(input, info, ninfo, &regex)` | delimited list → filled-in `pmix_regex2_t` |
| `parse_regex(&regex, info, ninfo, &output)`  | `pmix_regex2_t` → the delimited list |

`pmix_regex2_t` ([`include/pmix_common.h.in`](../../../include/pmix_common.h.in))
makes the "bytes plus length plus type" nature of an encoded value
explicit:

```c
typedef struct pmix_regex2 {
    char *type;      // encoding tag, e.g. "raw", "compress"
    uint8_t *bytes;  // encoded representation (may NOT be NUL-terminated)
    size_t len;      // number of bytes in `bytes`
} pmix_regex2_t;
```

Note the `pmix_info_t info[], size_t ninfo` parameter pair on both: per
the top-level rule that *every* PMIx API must carry an attribute array,
these are present so behavior can be extended by attribute later. No
attributes are defined for them yet; components currently
`PMIX_HIDE_UNUSED_PARAMS(info, ninfo)`.

These back the public `PMIx_generate_regex2()` / `PMIx_parse_regex2()`
server APIs and the `PMIX_REGEX2` data type.

### The deprecated `char *` API is a serialization, not a second interface

The original API represented an encoded value as a plain `char *`, and it
survives in the **deprecated** `PMIx_generate_regex()` /
`PMIx_generate_ppn()` ([`include/pmix_deprecated.h`](../../../include/pmix_deprecated.h))
and the deprecated `PMIX_REGEX` (value `49`) data type, which PMIx must
keep accepting forever. It used to be a parallel set of eight module
entry points — `generate_node_regex`, `generate_ppn`, `parse_nodes`,
`parse_procs`, `copy`, `pack`, `unpack`, `release` — that every component
implemented alongside the regex2 pair.

It is not that any more. A `char *` regex is just a `pmix_regex2_t` with
its three fields flattened into one byte run, so the whole deprecated
interface now lives in **`base/preg_base_legacy.c`** as an encode/decode
pair over the frozen layouts:

```
compress:  "blob:" NUL "component=zlib:" NUL "size=" <decimal> ":" NUL <payload>
raw:       "raw:" <NUL-terminated string>
```

and the base stubs are built from it: `generate_node_regex`/`generate_ppn`
generate a regex2 and encode it; `parse_nodes`/`parse_procs` decode,
`parse_regex`, then split on `,` or `;`; `copy`/`pack`/`unpack` only need
the decoder to tell them how long the serialized form is.

Three things follow, and they are the reason to keep it this way:

- **Components never see the deprecated form.** Do not add a legacy entry
  point back to `pmix_preg_module_t`. If you are tempted, you are about
  to reintroduce eight functions per component that all do the same two
  things.
- **`component=zlib:` is a fixed label, not a claim.** It was emitted
  unconditionally back when zlib was the only compressor PMIx had; by the
  time `zlibng`/`zstd`/`lz4` arrived it was on the wire and could not be
  changed. Nothing selects a decompressor from it — `pcompress`
  identifies its own payloads. It means only "a preg component wrote
  this". Do not read it as an abstraction boundary, and do not "fix" it
  to name the real component: that would break every peer.
- **The layouts are frozen.** They are what a PMIx of any vintage puts on
  the wire for a `PMIX_REGEX` value. A new component's encoding simply
  has no deprecated form; `generate_legacy` falls back to shipping the
  list uncompressed, exactly as it did when no component claimed the
  request.

## Core data structures

### `preg.h` — the module, the API, and the version

`preg.h` is the framework's public header, the contract every component
compiles against. It defines:

- **`pmix_preg_module_t`** — the component interface: `.name` plus the
  two regex2 function pointers. Nothing else.
- **`pmix_preg_api_t`** — the framework-level API, instantiated once as
  the exported global **`pmix_preg`** through which all back-end code
  calls `preg`. Every entry points at a `pmix_preg_base_*` function;
  there is no component dispatch table here. It is a wider struct than
  `pmix_preg_module_t` because it also exposes the deprecated `char *`
  operations the base synthesizes.
- the three **`PMIX_MCA_preg_*_VERSION`** macros stating the framework's
  interface version — the numbers `PMIX_MCA_BASE_VERSION(preg)` stamps
  into every component struct. Bump the major on any change a component
  built against the previous interface would not survive; dropping the
  eight legacy entry points was such a change, which is why it now reads
  `2`.

## Directory layout

```
src/mca/preg/
├── preg.h                  Framework public API: module, API struct, version macros
├── base/                   Framework infrastructure (see below)
│   ├── base.h              Internal base API + globals + active-module struct
│   ├── preg_base_frame.c   open/close, framework decl, class instance
│   ├── preg_base_legacy.c  serialization of a pmix_regex2_t to/from the char* form
│   ├── preg_base_select.c  component query + priority ordering
│   └── preg_base_stubs.c   the pmix_preg_base_* routing functions
├── raw/                    the "raw" pass-through (no compression)
└── compress/               the "compress" blob compressor (needs pcompress)
```

## The base routing layer (`preg_base_stubs.c`)

Every entry in the global `pmix_preg` API points at a `pmix_preg_base_*`
function here. Only two of them actually dispatch to components:

1. **Smallest-wins** — `generate_regex`. It does **not** stop at the
   first success: it calls *every* module's `generate_regex`, keeps the
   candidate with the smallest `len`, frees the losers, and returns the
   winner (or `PMIX_ERR_NOT_SUPPORTED` if none produced anything). This
   is how the framework automatically picks the most compact encoding —
   usually `compress`, but `raw` wins for inputs too small or too random
   to compress. Note there is **no fallback**: if no component implements
   `generate_regex`, `PMIx_generate_regex2` fails.

2. **First-success** — `parse_regex`, matching on the `type` tag.
   Returns `PMIX_ERR_NOT_SUPPORTED` if no active scheme recognizes it.

The rest are the deprecated `char *` operations, which are not dispatchers
at all — they are the serialization layer described above, built on those
two. Their failure behavior is worth knowing:

- `generate_node_regex` / `generate_ppn` fall back to `strdup(input)`
  (ship the list uncompressed) if no component can encode it or the
  winning encoding has no deprecated form, so they essentially never fail.
- `parse_nodes` / `parse_procs` split the input directly when it carries
  no recognizable framing — that is how a plain comma-separated list from
  a host still works. But when the framing *is* recognized and no active
  component owns the encoding, they return `PMIX_ERR_NOT_SUPPORTED`
  rather than splitting the payload. Do not "restore" a fallback there:
  splitting an encoded blob on commas yields plausible-looking garbage
  node names instead of an error, which is far worse than failing.
- `copy` / `pack` / `unpack` treat an unrecognized value as a plain
  string (`strdup`, or bfrops `PMIX_STRING`).
- `release` has no fallback — it returns `PMIX_ERR_BAD_PARAM` if the
  pointer carries no recognizable framing. Nothing in the tree currently
  calls it.

### The one wart the deprecated form cannot shed

`pmix_preg_base_legacy_decode` takes an `avail` bound and honors it, but
only `unpack` can supply a real one — it knows how many bytes remain in
the buffer. Every other caller holds a bare `char *` and passes
`SIZE_MAX`, because the deprecated interface has no length to pass:
`pmix_preg.parse_nodes(regexp, &names)` cannot carry one without changing
a signature that predates the current API. `gds/hash` has the length
right there in `val->data.bo.size` and still cannot hand it over.

The practical consequence is that a caller-owned string claiming to be a
`blob` but truncated before its framing completes can be read a few bytes
past its end. This is not new — it is what the per-component parsers did
before — and it is bounded in the direction that matters, since values
arriving from a peer come through `unpack`, which *is* bounded. Do not
"fix" it by removing the bound from `unpack`; if you want it fixed
properly, the length has to be plumbed through the deprecated signatures,
and at that point you are better off moving the caller to `pmix_regex2_t`.

## Selection and lifecycle

- **`preg_base_frame.c`** declares the framework
  (`PMIX_MCA_BASE_FRAMEWORK_DECLARE`), instantiates the global `pmix_preg`
  API (pointing every slot at the base functions), and defines the
  `PMIX_CLASS_INSTANCE` for the active-module wrapper. `pmix_preg_open`
  constructs the `actives` list and opens all components;
  `pmix_preg_close` tears it down.
- **`preg_base_select.c`** (`pmix_preg_base_select`, called once from
  library init) queries each component, wraps the returned modules in
  `pmix_preg_base_active_module_t { pri, module, component }`, and inserts
  them into `pmix_preg_globals.actives` in **descending priority order**.
  If zero modules are selected it emits the `no-plugins` `show_help` topic
  and returns `PMIX_ERR_SILENT` — `preg` requires at least one component.
  At verbosity >4 it prints the resolved priority list.

Default priorities (from each component's `component_query`):

| Component | Priority | Active when |
|-----------|----------|-------------|
| `compress` | 100 | a `pcompress` component provides `compress_string` |
| `raw`      | 50  | always |

`raw` is the floor: its `generate_regex` always succeeds, so there is
always something for smallest-wins to return. Excluding it
(`PMIX_MCA_preg=^raw`) with no `pcompress` available leaves `preg` with
no generator and `PMIx_generate_regex2` returns `PMIX_ERR_NOT_SUPPORTED`.

## Wire format (`pmix_regex2_t` on the buffer)

When a `pmix_regex2_t` travels between peers it is packed by
`pmix_bfrops_base_pack_regex2` (in `src/mca/bfrops/base/`) as: the `type`
string, then `len` as a `PMIX_SIZE`, then `len` raw `PMIX_BYTE`s (only if
`len > 0`). Unpacking mirrors this. Per the top-level interoperability
rules, this order is frozen — append only, never reorder. The deprecated
`char *` form is packed by `pmix_preg_base_pack`/`_unpack`, which
`memcpy` the serialized form's exact byte length into the buffer,
embedded NULs and all — it carries its own length, so it gets no bfrops
framing — or, when the value has no recognizable framing, as a plain
`PMIX_STRING`. The unpack side is reading peer-supplied bytes, so it
bounds every read against what remains in the buffer; keep it that way.

## Threading

The `preg` functions are pure, synchronous transforms of their arguments —
they allocate and return, they do not thread-shift, block, or touch shared
library state beyond the read-only `actives` list built once at init. They
are therefore safe to call directly from within a progress-thread handler
(which is where the server APIs and `gds/hash` invoke them). They must
**not** be called concurrently with framework open/close, but that only
happens at startup/shutdown. There is no caddy pattern here.

## Building

Both components are statically built into `libpmix` and wired through
the generated `base/static-components.h`; none ships a `configure.m4`, so
none is conditionally compiled out — but `compress` disables *itself* at
runtime (its `component_query` returns an error) if no `pcompress` module
is available, so its symbols must never be assumed to be in the active
list. Adding a component means creating `src/mca/preg/<name>/` with the
usual `Makefile.am`, a component struct opened with
`PMIX_MCA_BASE_VERSION(preg)`, and a module; the framework picks it up
through `static-components.h`. Editing a `Makefile.am` only needs a plain
`make`; adding or removing a *component directory* changes the build
wiring resolved by `configure`, so re-run
`./autogen.pl && ./configure ... && make`.

`preg` ships no `show_help` text of its own (the only `show_help` it uses,
`no-plugins`, lives in `help-pmix-runtime.txt`), so the regenerate-the-
help-content golden rule does not usually bite here.

## When adding or modifying a component

- Open the component struct with `PMIX_MCA_BASE_VERSION(preg)` and set
  `pmix_mca_component_name` to your directory name.
- Implement exactly `generate_regex` and `parse_regex`. Set `regex->type`
  to your component's name on generate, and gate `parse_regex` on it with
  `strcmp`, returning `PMIX_ERR_TAKE_NEXT_OPTION` when the type is not
  yours. The type is the interoperability contract — a peer must be able
  to identify your encoding without knowing your component exists.
- Do not add anything to `pmix_preg_module_t`. The deprecated `char *`
  interface is a base-level serialization (`preg_base_legacy.c`), not a
  component responsibility, and a new encoding simply has no deprecated
  form.
- Your encoding may contain embedded NUL bytes; that is what `len` is
  for. Never `strlen` a `regex->bytes`.
- Preserve value ordering across generate→parse, and remember the
  encoding must be indifferent to the delimiter: the same code path
  encodes a node list and a process map. Test round-trips with both.
- Never change an existing component's on-the-wire encoding; add a new
  component (new type) instead, exactly as with `bfrops` versions.
