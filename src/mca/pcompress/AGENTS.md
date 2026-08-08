<!--
  Copyright (c) 2026      Nanook Consulting  All rights reserved.

  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
-->

# AGENTS.md: The PCOMPRESS Framework

This document orients AI agents and human contributors working in the
`pcompress` (**P**MIx **Compress**ion) framework. It assumes you have
already read the top-level [`AGENTS.md`](../../../AGENTS.md) — the golden
rules, prefix conventions, thread-safety model, and MCA concepts
described there all apply here and are not repeated. This file covers
what is specific to `pcompress`: what the framework is for, the way its
one active component is chosen (entirely at build time), the shared
compressed-blob format its components must agree on, and the contract a
component must honor. Each component subdirectory (`zlib/`, `zlibng/`, `zstd/`)
carries its own `AGENTS.md` with component-specific detail. There is no
`docs/how-things-work/` page for this framework.

## What PCOMPRESS does

`pcompress` is a thin abstraction over whatever general-purpose
compression library is available on the build host. Its job is to shrink
**large data objects** before they are shipped over the wire or stored,
so that a big blob (a job map, a modex value, a set of network endpoints)
costs less network bandwidth and memory at scale. It offers two closely
related pairs of operations plus their string variants:

- **Byte-block** compression: `compress` / `decompress` — operate on a
  raw `uint8_t *` buffer of a given length.
- **String** compression: `compress_string` / `decompress_string` — a
  convenience wrapper that treats a NUL-terminated `char *` as the input
  and hands back a `char *` on the way out.

The framework does **not** decide *what* to compress or *when*; that
policy lives in the callers. `pcompress` only answers "compress these
bytes if it is worth it, and tell me whether you did." Every component
enforces the same two economy rules internally: it refuses to compress an
input shorter than the configured limit, and it refuses to emit a result
that is not actually smaller than the input (see the shared blob format
below).

### Who calls it

The selected module is reached through the exported global
**`pmix_compress`**. Representative callers across the tree:

| Caller | Uses |
|--------|------|
| `PMIx_Data_compress` / `PMIx_Data_decompress` ([`src/common/pmix_data.c`](../../common/pmix_data.c)) | the public block API |
| `src/client/pmix_client.c` | `compress_string` on large modex string values |
| `src/server/pmix_server_fence.c` | `compress` the collected fence blob |
| `src/mca/gds/base/gds_base_fns.c` | `decompress` a stored blob |
| `src/mca/preg/compress/` | `compress_string`/`decompress_string` behind the `blob:` regex encoding (see [`../preg/AGENTS.md`](../preg/AGENTS.md)) |
| `src/mca/pgpu/*`, `src/mca/pnet/*` | `compress`/`decompress` endpoint/inventory blobs |

Because these callers all go through `pmix_compress`, they get whichever
component was built — or the do-nothing base default if none was — with
no code changes of their own.

## Single-select, chosen at build time

`pcompress` is a **single-select** framework: exactly one module is
active, held in the global `pmix_compress`.
`base/pcompress_base_select.c` calls the stock `pmix_mca_base_select`,
which keeps the **highest-priority** component's module and copies it into
`pmix_compress`.

The unusual part is that the *real* selection happens at **configure
time**, not run time:

- No component's `component_query` gates on anything — each
  unconditionally returns its module and a fixed priority (`zstd` = 90,
  `zlibng` = 75, `zlib` = 50). So on a host where several libraries were
  found at configure time the highest wins; where only one was found, it
  wins. `zstd` is ranked first because it is both faster and smaller than
  the zlib components on the payloads this framework actually sees — see
  [`zstd/AGENTS.md`](zstd/AGENTS.md).
- A component is only compiled at all if its `configure.m4` located the
  underlying library (`OAC_CHECK_PACKAGE`). If the library is absent, the
  component is never built, so it is not in `static-components.h` and
  cannot be selected. This is why the runtime query needs no availability
  check — an unavailable component simply does not exist in the build.
- If **no** component was built (no compression library on the host),
  `pmix_mca_base_select` selects nothing and `pmix_compress` retains the
  **base default module** wired up in `pcompress_base_frame.c`, whose
  entry points do nothing but return `false`.

So "which compressor am I getting?" is answered by the configure summary
line (`External Packages: ZLIB` / `ZLIBNG` / `ZSTD`), not by any MCA
parameter — though `--pmixmca pcompress <name>` can still force one of the
built components at run time.

## Module interface (`pmix_compress_base_module_t`)

Defined in [`pcompress.h`](pcompress.h) as
`pmix_compress_base_module_1_0_0_t`. A component fills in the subset it
implements; the rest stay `NULL`.

| Field | Signature | Purpose |
|-------|-----------|---------|
| `init` | `(void) -> int` | one-time setup; **unset by every module today** |
| `finalize` | `(void) -> int` | teardown; called by base close only if non-`NULL` |
| `compress` | `(const uint8_t *in, size_t size, uint8_t **out, size_t *nbytes) -> bool` | compress a byte block; `true` if it did |
| `decompress` | `(uint8_t **out, size_t *outlen, const uint8_t *in, size_t len) -> bool` | inflate a block produced by `compress` |
| `get_decompressed_size` | `(const pmix_byte_object_t *) -> size_t` | inflated size of a `PMIX_COMPRESSED_BYTE_OBJECT`, read from the blob's 4-byte length prefix; implemented by all three modules |
| `compress_string` | `(char *in, uint8_t **out, size_t *nbytes) -> bool` | compress a NUL-terminated string |
| `decompress_string` | `(char **out, uint8_t *in, size_t len) -> bool` | inflate a string produced by `compress_string` |
| `get_decompressed_strlen` | `(const pmix_byte_object_t *) -> size_t` | inflated length (+1 for the NUL) of a `PMIX_COMPRESSED_STRING`, read from the blob's 4-byte length prefix; implemented by all three modules |

The return convention is a plain `bool`: `true` means "I compressed the
data and `*out`/`*nbytes` are set," `false` means "I did not" (input too
small, would not shrink, allocation failure, or — for the base default —
no library at all). A `false` from `compress*` is not an error; the
caller ships the data uncompressed.

### The two fields nobody implements (and two that everybody does)

`init` and `finalize` are declared in the struct but set by **neither**
the base default module **nor** `zlib` **nor** `zlibng` **nor** `zstd` — they are always
`NULL` in practice. `finalize` is safely guarded (`pcompress_base_close`
calls it only when non-`NULL`).

`get_decompressed_size` and `get_decompressed_strlen`, by contrast, **are**
implemented by every module (the base default, `zlib`, `zlibng`, and `zstd`).
Their only call sites are in
[`src/mca/bfrops/base/bfrop_base_fns.c`](../bfrops/base/bfrop_base_fns.c)
(the `PMIX_COMPRESSED_STRING` / `PMIX_COMPRESSED_BYTE_OBJECT` cases of the
`PMIx_Value_get_size` / data-array size computation), and every module
should keep them non-`NULL` — but those call sites no longer *assume* it.
They used to call the slot directly, which turned a module that left it
`NULL` into a jump to address zero. A component is a run-time-loadable
plugin, so an installed component older than the `libpmix` that loads it
is exactly what a partial upgrade produces, and one built before these
two entry points existed is such a module. The two static helpers
`decompressed_size()` / `decompressed_strlen()` in that file now answer
0 in that case, which is what the base default already returns for a
blob it cannot read. Each reads the **4-byte length
prefix** the compressor folded into the blob and returns the inflated
size without decompressing: `get_decompressed_size` returns the raw byte
count, `get_decompressed_strlen` returns that count **+ 1** for the NUL
terminator (mirroring how a plain `PMIX_STRING` is sized). The base
default implements them too — with no compression library present a
process cannot itself produce a compressed blob, but it can still be
handed one by a peer that had a compressor, and the size query must not
crash. (Historically these two pointers were `NULL`; that was a latent
NULL-deref, now fixed.)

## The base default module (`pcompress_base_frame.c`)

When no component is built, `pmix_compress` is this fallback. Its
`compress`/`compress_string` do nothing but return `false`, and — the one
piece of real behavior here — the **first** time a non-client process
asks to compress, they emit the one-shot `unavailable` `show_help` topic
warning that no compression library was found, then set
`pmix_compress_base.silent` so it is never repeated. Clients
(`PMIX_PEER_IS_CLIENT`) are deliberately silent; only servers and tools
warn. `decompress`/`decompress_string` return `false` unconditionally.

## Globals and MCA parameters

`base/base.h` defines the framework state struct `pmix_compress_base_t`,
instantiated as the exported global **`pmix_compress_base`**:

| Field | Purpose |
|-------|---------|
| `compress_limit` | size threshold (bytes); inputs smaller than this are never compressed |
| `selected` | guards `pmix_compress_base_select` against running twice |
| `silent` | suppresses the "no compression library" warning (also the MCA flag) |

Two MCA parameters are registered in `pcompress_base_frame.c`
(`pmix_compress_base_register`):

| Parameter | Type / default | Meaning |
|-----------|----------------|---------|
| `pcompress_base_limit` | size_t, **4096** | value written into `compress_limit`; the byte threshold below which data is left uncompressed |
| `pcompress_base_silence_warning` | bool, **false** | value written into `silent`; suppresses the base default's "unavailable" warning |

Each component additionally registers its own compression level, and every
one of them is a parameter rather than a constant for the same reason: the
level decides whether compressing a large payload is worth doing at all, and
that depends on the bandwidth of the link the result will cross, which no
library can see.

| Parameter | Type / default | Meaning |
|-----------|----------------|---------|
| `pcompress_zlib_level` | int, **1** | level passed to `deflateInit` |
| `pcompress_zlibng_level` | int, **2** | level passed to `zng_deflateInit` |
| `pcompress_zstd_level` | int, **3** | level passed to `ZSTD_compress` |

The zlib defaults were a hard-coded **9** until these parameters were added.
On a 25.6 MB aggregated modex, level 9 costs roughly twice the CPU of level 1
to shrink the result a further ~2%, and a broadcast pays the deflate once
against a wire cost on every link of the tree — so 9 only repays itself on a
very wide tree over a slow link. The two zlib defaults differ by one deliberately: **zlib-ng's level 1 is not
zlib's level 1**. zlib-ng remaps its lowest level onto a quick-deflate
strategy, so `zlibng` level 2 is what `zlib` level 1 produces (0.649 on that
corpus, in both cases) and that is what it defaults to. The defaults match in
behaviour, not in digit — see [`zlibng/AGENTS.md`](zlibng/AGENTS.md).

Per the top-level guidance, prefer a new MCA parameter over a hard-coded
constant if you introduce a tunable here.

`base/base.h` also exports two convenience macros used by callers (not by
the components):

- **`PMIX_STRING_SIZE_CHECK(s)`** — true when a `pmix_value_t` holds a
  `PMIX_STRING` longer than `pmix_compress_base.compress_limit`, i.e. "is
  this string worth compressing?"
- **`PMIX_VALUE_COMPRESSED_STRING_UNPACK(s)`** — if a value's type is
  `PMIX_COMPRESSED_STRING`, inflate it in place via
  `pmix_compress.decompress_string` and retype it back to `PMIX_STRING`.
  (Used in `src/client/pmix_client.c`.)

## The shared compressed-blob format

Both real components produce and consume the **same** on-blob layout, and
this is a contract, not an implementation detail:

```
+-------------------+---------------------------------+
| uint32 raw_length |   DEFLATE-compressed payload    |
+-------------------+---------------------------------+
   4 bytes (host       zlib deflate stream
   byte order)
```

- The first 4 bytes are the **uncompressed** length, written with a raw
  `memcpy` of a `uint32_t` — i.e. **host byte order**, not network order.
- The remainder is the compressed payload. For `zlib` and `zlib-ng` it is
  a standard DEFLATE stream; because both emit and read that format and
  both use this identical 4-byte framing, a blob produced by one is
  readable by the other, which is what makes it safe for the build to pick
  `zlibng` on one node and `zlib` on another.
  **`zstd` breaks that symmetry**: it keeps the 4-byte prefix (so every
  size query still works) but its payload is a zstd frame, which a
  zlib-only peer cannot read. Compressed blobs *do* cross nodes — the
  server compresses each daemon's fence bucket and the receivers inflate
  it — so **every node in a job must run the same component**. In practice
  that follows from a shared installation. `zstd`'s decompress checks the
  zstd frame magic and refuses anything else, which converts the mixed-build
  failure from a corrupted modex into a clean error; it does not make the
  mixed build work. If heterogeneous deployments ever have to be
  supported, the fix is to make the blob self-describing across the whole
  framework — a scheme byte, or that sniff generalized into the base — not
  a per-component workaround.
- `compress` returns `false` (declines) when the input is shorter than
  `compress_limit`, when it is `>= UINT32_MAX` (the length would not fit
  the 4-byte prefix), or when the compressed result is **not** smaller
  than the input. This last check is why a caller can always trust that a
  `true` return actually saved space.
- `decompress_string` treats a stored `raw_length` of `UINT32_MAX` as an
  error sentinel and NUL-terminates the inflated string.

Because the length prefix is host-endian, a compressed blob is **not**
guaranteed portable across a big-endian/little-endian boundary. Today
compression is applied to payloads that are either consumed on the same
node or re-expanded before crossing an endianness boundary; do not extend
its use to a new cross-endian wire path without fixing the prefix to a
defined byte order first (and remember the top-level interoperability
rule: a new format means a new scheme, not a silent change to this one).

## Selection and lifecycle

- **`base/pcompress_base_frame.c`** declares the framework
  (`PMIX_MCA_BASE_FRAMEWORK_DECLARE(pmix, pcompress, "PCOMPRESS MCA", ...)`),
  instantiates the global `pmix_compress` (initialized to the base
  default stubs) and `pmix_compress_base`, registers the two MCA
  parameters, and opens all built components. `pmix_compress_base_close`
  clears `selected`, calls the selected module's `finalize` if set, and
  closes the components.
- **`base/pcompress_base_select.c`** (`pmix_compress_base_select`) runs
  once, short-circuits if already `selected`, calls `pmix_mca_base_select`
  to pick the highest-priority module, runs its `init` if non-`NULL`, and
  copies it into `pmix_compress`. Selecting nothing is **not** an error —
  the base default stubs simply remain in place. (Contrast `ptl`/`preg`,
  which treat "no component" as fatal; `pcompress` degrades to a no-op.)

The framework is opened and selected during library init in
[`src/runtime/pmix_init.c`](../../runtime/pmix_init.c) for **all** process
roles (client, server, tool), and closed in
[`src/runtime/pmix_finalize.c`](../../runtime/pmix_finalize.c).

## Threading

The `pcompress` functions are pure, synchronous transforms of their
arguments: they allocate, compute, and return. They do not thread-shift,
block, or touch shared library state beyond the read-only
`pmix_compress_base.compress_limit`/`silent`. They are therefore safe to
call directly from a progress-thread handler (which is where the server,
gds, and pnet callers invoke them) and, for the public
`PMIx_Data_compress`/`_decompress`, from the caller's thread. There is no
caddy pattern here.

## Directory layout

```
src/mca/pcompress/
├── pcompress.h                 Framework API: module struct, typedefs, version macro
├── base/
│   ├── base.h                  Internal base API, pmix_compress_base state, helper macros
│   ├── pcompress_base_frame.c  open/close/register, framework decl, base default module
│   ├── pcompress_base_select.c single-component selection
│   └── help-pcompress.txt      the "unavailable" show_help topic
├── zlib/                       zlib compressor (priority 50)
└── zlibng/                     zlib-ng compressor (priority 75, preferred)
```

## Building

The framework core (`base/`) is always built into `libpmix`. Each
component is a standard MCA component **conditionally compiled** by its
`configure.m4`: `zlib` builds only if `OAC_CHECK_PACKAGE` finds `zlib.h`
+ `libz` (`deflate`); `zlibng` builds only if it finds `zlib-ng.h` +
`libz-ng` (`zng_deflate`). Each honors `--with-zlib[-libdir]` /
`--with-zlibng[-libdir]`, errors out if support was explicitly requested
but not found, and adds its library to the `PMIX_EMBEDDED_*` flags and
the configure summary. A host with neither library builds no component,
and the framework runs on the base default no-op module.

Note the version macro every component opens with is
**`PMIX_COMPRESS_BASE_VERSION_2_0_0`** (component version 2.0.0, even
though the module struct type is named `..._1_0_0_t`).

Golden rules that bite here:

- Editing a `Makefile.am` only needs a plain `make`; **adding or removing
  a component directory, or touching a `configure.m4`, changes the build
  wiring resolved by `configure`**, so re-run
  `./autogen.pl && ./configure ... && make`.
- `pcompress` ships its own `show_help` file,
  `base/help-pcompress.txt`. Per the top-level golden rule, after any
  add/delete/modify of that text you must
  `rm src/util/pmix_show_help_content.* && make` to regenerate the
  compiled help content.

## When working in this framework

- **Do not change the compressed-blob format of an existing component.**
  The 4-byte length prefix + DEFLATE framing is shared across `zlib` and
  `zlib-ng` and may be read by a differently-built peer. A new format
  means a new component, exactly as with `bfrops` versions.
- **Keep the two components byte-for-byte compatible.** `zlibng` is a
  drop-in of `zlib`; any change to framing, the size prefix, or the
  decline rules must land in *both* or the "either build works" guarantee
  breaks.
- **Respect the decline contract.** `compress*` returning `false` is
  normal and callers depend on it (they ship uncompressed). Never make it
  return `true` with a result that is not strictly smaller than the input.
- **Guard every module entry point you call.** `init`/`finalize` are
  `NULL` in every module today, and `get_decompressed_size` /
  `get_decompressed_strlen` were `NULL` in every module built before
  July 2026 — which is a live configuration, because components are
  run-time-loadable and an installed plugin can be older than the
  `libpmix` that loads it. A new module should still implement both
  `get_decompressed_*`, but no caller may treat that as guaranteed;
  `bfrops` learned this the hard way (see the note under those two
  fields above).
- **Remember selection is a build-time fact.** Reproducing a "wrong
  compressor selected" report means checking what `configure` found, not
  an MCA parameter.
