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

The canonical example (from `preg.h`):

```
Input:   odin009,odin010,odin011,odin012,odin017,odin018,thor176
Output:  pmix[3:9-12,17-18]... , thor176
```

Two facts about the encoded string are load-bearing for the whole design:

1. **The encoding is self-identifying.** Every encoded value begins with
   a *tag* naming the scheme that produced it — `pmix[` for the native
   range compressor, `raw:` for the pass-through, `blob:` for the zlib
   compressor. A component inspects that tag to decide whether a given
   blob is "its" to parse, and declines (returns
   `PMIX_ERR_TAKE_NEXT_OPTION`) if not. This is what lets a server built
   with one set of components correctly parse data produced by a peer
   with a different set — the tag, not the local configuration, selects
   the parser.
2. **Order is preserved.** The individual values must come back out of
   the parser in exactly the order they went into the generator. The
   native component goes to real trouble (the `skip` flag, see its
   `AGENTS.md`) to guarantee this.

`preg` is a **multi-select** framework: several components are active at
once, and a request is offered to each in priority order until one claims
it. It is one of the framework types the top-level guide calls out as
multi-select (alongside `bfrops` and `psec`).

## Two API generations

This is the single most important thing to understand before editing
`preg`, because the module struct carries **both** generations at once
and they behave differently.

### Legacy: the `char *` string API

The original API represents an encoded regex as a plain, NUL-terminated
`char *`:

| Module fn | Purpose |
|-----------|---------|
| `generate_node_regex(input, &regex)` | comma-list of node names → tagged string |
| `generate_ppn(input, &ppn)` | semicolon-list of per-node rank ranges → tagged string |
| `parse_nodes(regex, &names)` | tagged string → argv of node names |
| `parse_procs(regex, &procs)` | tagged string → argv of per-node rank lists |
| `copy(&dest, &len, input)` | deep-copy an encoded value (it may contain embedded NULs) |
| `pack(buffer, input)` | serialize an encoded value into a `pmix_buffer_t` |
| `unpack(buffer, &regex)` | deserialize one back out |
| `release(regex)` | free a value this scheme produced |

`copy`/`pack`/`unpack`/`release` exist because a `char *` is a leaky
abstraction for this data: the `compress` scheme's blob **contains
embedded NUL bytes**, so `strlen`/`strdup` are wrong for it and it needs
custom copy/pack routines that know its true byte length. This is exactly
the wart the second generation was created to remove.

The legacy API is still exported to the world through the **deprecated**
`PMIx_generate_regex()` and `PMIx_generate_ppn()` (see
[`include/pmix_deprecated.h`](../../../include/pmix_deprecated.h)) and the
deprecated `PMIX_REGEX` (value `49`) data type. It is still used
*internally* to carry the per-node process map and as the back-end of the
regex2 path (see below), so it is far from dead — but no new public
surface should be built on it.

### Current: the `pmix_regex2_t` API

The second generation makes the "bytes plus length plus type" nature of
an encoded regex explicit in a struct
([`include/pmix_common.h.in`](../../../include/pmix_common.h.in)):

```c
typedef struct pmix_regex2 {
    char *type;      // encoding tag, e.g. "pmix", "raw", "compress"
    uint8_t *bytes;  // encoded representation (may NOT be NUL-terminated)
    size_t len;      // number of bytes in `bytes`
} pmix_regex2_t;
```

Two module functions operate on it:

| Module fn | Purpose |
|-----------|---------|
| `generate_regex(input, info, ninfo, &regex)` | node-name list → filled-in `pmix_regex2_t` |
| `parse_regex(&regex, info, ninfo, &output)`  | `pmix_regex2_t` → comma-separated node string |

Note the `pmix_info_t info[], size_t ninfo` parameter pair on both: per
the top-level rule that *every* PMIx API must carry an attribute array,
these are present so behavior can be extended by attribute later. No
attributes are defined for them yet; components currently
`PMIX_HIDE_UNUSED_PARAMS(info, ninfo)`.

`generate_regex`/`parse_regex` back the current public
`PMIx_generate_regex2()` / `PMIx_parse_regex2()` server APIs and the
`PMIX_REGEX2` data type. `type` here is the same tag concept as the
legacy string prefix, just lifted into its own field instead of being
smuggled into the front of the byte stream.

**Relationship between the two.** `parse_regex` does not re-implement node
parsing — it decodes the `pmix_regex2_t` back into a `char *` node list
and the caller (e.g. `gds/hash`) then runs the *legacy* `parse_nodes` on
that string. So the two generations interlock: regex2 is the transport,
the legacy string parser is still the thing that expands ranges. Keep
that in mind before "simplifying" either half.

## Core data structures

### `preg.h` — the module and version

`preg.h` is the framework's public header, the contract every component
compiles against. It defines **`pmix_preg_module_t`** (the `.name` field
plus the ten function pointers above; a component may leave any pointer
`NULL` and the base will skip it), the exported global
**`pmix_preg`** module through which all back-end code calls the
framework, and **`PMIX_PREG_BASE_VERSION_1_0_0`**, the version macro every
component struct must open with. It also defines `PMIX_MAX_NODE_PREFIX`
(8192) — the cap on a node-name prefix length used by the native
generator.

### `preg_types.h` — the range/value helper classes

The native range compressor builds its output from two PMIx classes,
defined here and instantiated in `preg_base_frame.c`:

- **`pmix_regex_range_t`** `{ int start; int cnt; }` — a contiguous run of
  integers, e.g. `9-12` is `start=9, cnt=4`.
- **`pmix_regex_value_t`** `{ char *prefix; char *suffix; int num_digits;
  pmix_list_t ranges; bool skip; }` — one distinct name shape (prefix +
  zero-padded number field + optional suffix) together with the list of
  integer ranges seen for it. `skip` is the ordering-preservation flag
  described in the native component's `AGENTS.md`.

These are used only by the `native` and (for `generate_ppn`) related code;
`raw` and `compress` do not touch them.

## Directory layout

```
src/mca/preg/
├── preg.h                  Framework public API: module & version macro
├── preg_types.h            pmix_regex_range_t / pmix_regex_value_t classes
├── base/                   Framework infrastructure (see below)
│   ├── base.h              Internal base API + globals + active-module struct
│   ├── preg_base_frame.c   open/close, framework decl, class instances
│   ├── preg_base_select.c  component query + priority ordering
│   └── preg_base_stubs.c   the pmix_preg_base_* routing functions
├── native/                 the "pmix[...]" range compressor + parser
├── raw/                    the "raw:" pass-through (no compression)
└── compress/               the "blob:" zlib compressor (needs pcompress)
```

## The base routing layer (`preg_base_stubs.c`)

Every entry in the global `pmix_preg` module points at a
`pmix_preg_base_*` stub here. The stubs are thin dispatchers over the
priority-ordered `pmix_preg_globals.actives` list, and they come in three
flavors — know which one you are touching:

1. **First-success-wins** (`generate_node_regex`, `generate_ppn`,
   `parse_nodes`, `parse_procs`, `copy`, `pack`, `unpack`). Walk the
   actives in priority order, call the module fn, and **return on the
   first `PMIX_SUCCESS`**. If no module claims the request, apply a
   built-in fallback:
   - `generate_*` → `strdup(input)` (ship the input uncompressed).
   - `parse_nodes` → `PMIx_Argv_split(regex, ',')`.
   - `parse_procs` → `PMIx_Argv_split(regex, ';')`.
   - `copy` → `strdup` + `strlen+1`.
   - `pack`/`unpack` → treat it as a `PMIX_STRING` via bfrops.

   The fallbacks are why these functions essentially never fail: an
   unrecognized value is simply treated as a literal string.

2. **`release`** is first-success-wins but has **no fallback** — it
   returns `PMIX_ERR_BAD_PARAM` if no scheme claims the pointer. Only free
   a regex through the same framework that produced it.

3. **Smallest-wins** (`generate_regex`, the regex2 generator). This is the
   exception: it does **not** stop at the first success. It calls *every*
   module's `generate_regex`, keeps the candidate with the smallest
   `len`, frees the losers, and returns the winner (or
   `PMIX_ERR_NOT_SUPPORTED` if none produced anything). This is how the
   framework automatically picks the most compact encoding — usually
   `compress`, but `raw` wins for inputs too small or too random to
   compress. `parse_regex` is back to plain first-success (tag match), and
   returns `PMIX_ERR_NOT_SUPPORTED` if no active scheme recognizes the
   `type`.

### A consequence worth internalizing

Because the first-success stubs take priorities in descending order and
`raw`'s generators **always succeed** (they only prepend `raw:`), the
legacy `generate_node_regex`/`generate_ppn` path is claimed by `compress`
(priority 100) when zlib is present, otherwise by `raw` (50) — the
`native` generator (30) is effectively shadowed on that path in a default
build. `native` still earns its keep as the **parser** of the `pmix[...]`
format (which external launchers such as PRRTE emit) and via the
smallest-wins regex2 path where it is *not* even registered
(`native.generate_regex == NULL`). Do not "fix" this by reordering
priorities without understanding that the `pmix[...]` producer and its
consumer live in different deployments.

## Selection and lifecycle

- **`preg_base_frame.c`** declares the framework
  (`PMIX_MCA_BASE_FRAMEWORK_DECLARE`), instantiates the global `pmix_preg`
  module (pointing every slot at the base stubs), and defines the
  `PMIX_CLASS_INSTANCE`s for `pmix_regex_range_t`, `pmix_regex_value_t`,
  and the active-module wrapper. `pmix_preg_open` constructs the `actives`
  list and opens all components; `pmix_preg_close` tears it down.
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
| `compress` | 100 | a `pcompress` (zlib) component provides `compress_string` |
| `raw`      | 50  | always |
| `native`   | 30  | always |

## Wire format (`pmix_regex2_t` on the buffer)

When a `pmix_regex2_t` travels between peers it is packed by
`pmix_bfrops_base_pack_regex2` (in `src/mca/bfrops/base/`) as: the `type`
string, then `len` as a `PMIX_SIZE`, then `len` raw `PMIX_BYTE`s (only if
`len > 0`). Unpacking mirrors this. Per the top-level interoperability
rules, this order is frozen — append only, never reorder. The legacy
string form is packed via each component's own `pack`/`unpack` (which
`memcpy` the exact byte length into the buffer, embedded NULs and all) or,
in the fallback, as a plain `PMIX_STRING`.

## Threading

The `preg` functions are pure, synchronous transforms of their arguments —
they allocate and return, they do not thread-shift, block, or touch shared
library state beyond the read-only `actives` list built once at init. They
are therefore safe to call directly from within a progress-thread handler
(which is where the server APIs and `gds/hash` invoke them). They must
**not** be called concurrently with framework open/close, but that only
happens at startup/shutdown. There is no caddy pattern here.

## Building

All three components are statically built into `libpmix` and wired through
the generated `base/static-components.h`; none ships a `configure.m4`, so
none is conditionally compiled out — but `compress` disables *itself* at
runtime (its `component_query` returns an error) if no `pcompress` module
is available, so its symbols must never be assumed to be in the active
list. Adding a component means creating `src/mca/preg/<name>/` with the
usual `Makefile.am`, a component struct opened with
`PMIX_PREG_BASE_VERSION_1_0_0`, and a module; the framework picks it up
through `static-components.h`. Editing a `Makefile.am` only needs a plain
`make`; adding or removing a *component directory* changes the build
wiring resolved by `configure`, so re-run
`./autogen.pl && ./configure ... && make`.

`preg` ships no `show_help` text of its own (the only `show_help` it uses,
`no-plugins`, lives in `help-pmix-runtime.txt`), so the regenerate-the-
help-content golden rule does not usually bite here.

## When adding or modifying a component

- Open the component struct with `PMIX_PREG_BASE_VERSION_1_0_0` and set
  `pmix_mca_component_name` to your directory name.
- Choose a **unique tag** for your encoding and make every generator emit
  it and every parser gate on it with `strncmp`/`strcmp`, returning
  `PMIX_ERR_TAKE_NEXT_OPTION` when the tag is not yours. The tag is the
  interoperability contract — a peer must be able to identify your
  encoding without knowing your component exists.
- Preserve value ordering across generate→parse. Test round-trips with
  interleaved, out-of-order, and mixed-prefix inputs.
- If your encoding can contain embedded NUL bytes, you **must** implement
  `copy`/`pack`/`unpack`/`release` (do not rely on the `strdup`/`PMIX_STRING`
  fallbacks) — see `compress` for the pattern.
- Prefer implementing the regex2 `generate_regex`/`parse_regex` for any
  new work; wire the legacy string functions too only if the encoding must
  also flow through the deprecated `PMIx_generate_regex` path or the
  internal node/proc map.
- Never change an existing component's on-the-wire encoding; add a new
  component (new tag) instead, exactly as with `bfrops` versions.
