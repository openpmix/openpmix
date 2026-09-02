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
component must honor. Each component subdirectory (`zlib/`, `zlibng/`, `zstd/`, `lz4/`)
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
| `src/mca/bfrops/base/bfrop_base_unpack.c` | `decompress_string` when expanding a `PMIX_COMPRESSED_STRING` value off the wire |
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
  `zlibng` = 75, `lz4` = 60, `zlib` = 50). So on a host where several libraries were
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
line (`External Packages: ZLIB` / `ZLIBNG` / `ZSTD` / `LZ4`), not by any MCA
parameter — though `--pmixmca pcompress <name>` can still force one of the
built components at run time.

## Module interface (`pmix_compress_base_module_t`)

Defined in [`pcompress.h`](pcompress.h) as
`pmix_compress_base_module_1_0_0_t`. A component fills in the subset it
implements; the rest stay `NULL`.

| Field | Signature | Purpose |
|-------|-----------|---------|
| `init` | `(void) -> int` | one-time setup; **unset by every module today**. A non-`PMIX_SUCCESS` return means "I cannot run" — the base defaults stay in place and the library carries on without compression |
| `finalize` | `(void) -> int` | teardown; called by base close only if non-`NULL` |
| `compress` | `(const uint8_t *in, size_t size, uint8_t **out, size_t *nbytes) -> bool` | compress a byte block; `true` if it did |
| `decompress` | `(uint8_t **out, size_t *outlen, const uint8_t *in, size_t len) -> bool` | inflate a block produced by `compress` |
| `get_decompressed_size` | `(const pmix_byte_object_t *) -> size_t` | inflated size of a `PMIX_COMPRESSED_BYTE_OBJECT`, read from the blob's 4-byte length prefix; implemented by every module. Returns **0** to mean "I cannot answer" |
| `compress_string` | `(char *in, uint8_t **out, size_t *nbytes) -> bool` | compress a NUL-terminated string |
| `decompress_string` | `(char **out, uint8_t *in, size_t len) -> bool` | inflate a string produced by `compress_string` |
| `get_decompressed_strlen` | `(const pmix_byte_object_t *) -> size_t` | inflated length (+1 for the NUL) of a `PMIX_COMPRESSED_STRING`, read from the blob's 4-byte length prefix; implemented by every module. Returns **0** to mean "I cannot answer" |

The return convention is a plain `bool`: `true` means "I compressed the
data and `*out`/`*nbytes` are set," `false` means "I did not" (input too
small, would not shrink, allocation failure, or — for the base default —
no library at all). A `false` from `compress*` is not an error; the
caller ships the data uncompressed.

### The two fields nobody implements (and two that everybody does)

`init` and `finalize` are declared in the struct but set by **neither**
the base default module **nor** any of the four components — they are always
`NULL` in practice. `finalize` is safely guarded (`pcompress_base_close`
calls it only when non-`NULL`), and a failing `init` is too: it degrades
to the base default rather than failing `PMIx_Init` (see `select` above).
Both behaviors are written for the first module that does implement them;
neither can be reached today.

`get_decompressed_size` and `get_decompressed_strlen`, by contrast, **are**
implemented by every module (the base default, `zlib`, `zlibng`, `zstd`
and `lz4`).
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
| `pcompress_base_limit` | size_t, **256** | value written into `compress_limit`; the byte threshold below which data is left uncompressed |
| `pcompress_base_silence_warning` | bool, **false** | value written into `silent`; suppresses the base default's "unavailable" warning |

### Choosing `compress_limit`

The floor is an economy measure, not a safety one: every component
already declines a result that is not strictly smaller than its input, so
lowering it cannot make a payload grow. What it buys or wastes is CPU.

It was **4096** until August 2026, and that turned out to be far above
where compression stops paying. Measured on modex-shaped payloads — the
per-node fence bucket, whose values are endpoint blobs and therefore
close to incompressible, so the compressor has only the repeated key
strings to work with. `zstd` is what actually runs wherever it was
built (priority 90); `zlib` at level 1 is shown beside it because it is
the fallback and because its cost is easy to attribute:

| bytes offered | `zstd` result | `zlib`-1 result | `zlib`-1 deflate |
|---|---|---|---|
| 64 – 192 | declined | declined | — |
| 256 | 255 | — | — |
| 384 | 318 | — | — |
| 512 | 374 | — | — |
| 1088 (4 procs) | 601 (0.55) | 0.58 | 13.2 us |
| 2176 (8 procs) | 1028 (0.47) | 0.52 | 17.3 us |
| 4352 (16 procs) | 1866 (0.43) | 0.48 | 24.1 us |
| 17408 (64 procs) | 6881 (0.40) | 0.44 | 60.9 us |

There is no crossover: the ratio improves monotonically with size, the
cost stays in the tens of microseconds, and it is paid once per node per
fence. Below roughly 256 bytes the compressor **declines on its own** —
the result is not smaller — so a floor there costs no opportunity at all
and only saves the pointless attempt, which is exactly what a floor is
for. That is why the value is **256** rather than something larger: every
byte of headroom above it was a payload we could have shrunk and did not.
The old 4096 meant a node running fewer than about sixteen processes
shipped its whole modex raw, which is most of the GPU-dense machines now
in service.

This parameter used to govern a second, unrelated decision — the length
above which `PMIx_Put` converted a string value into a
`PMIX_COMPRESSED_STRING` — which is what kept it from being lowered.
That coupling is gone; see the next section.

### Strings are not compressed individually, and never arrive compressed

`PMIx_Put` used to convert a string value longer than `compress_limit`
into a `PMIX_COMPRESSED_STRING` before storing it. That is no longer
done, for two independent reasons.

**It leaked an unreadable type to the application.** PMIx publishes no
way to expand a `PMIX_COMPRESSED_STRING` — there is no
`PMIx_Value_decompress` — so a caller who got one back from `PMIx_Get`
held bytes they could not read, and the datastore held a value nothing
could match against. The macro meant to undo it at the far end,
`PMIX_VALUE_COMPRESSED_STRING_UNPACK`, had its last two call sites
removed by `d5ec82c11` in the client get path and sat unused thereafter.
Both macros are gone now, and the expansion happens instead in
`pmix_bfrops_base_unpack_val()`, which is the one point every value
arriving from anywhere passes through exactly once. **A peer may still
send one and always will** — every released PMIx compresses large string
values on the way out — so that arm is live compatibility code, not
legacy cleanup.

**And it made the fence bucket bigger, not smaller.** Compressing a
string on its own destroys the cross-rank redundancy that the
bucket-level pass would otherwise exploit, and large string values are
precisely the ones every rank on a node emits a near-identical copy of
(a locality string, a topology rendering). Measured as a 64-rank bucket
carrying one large string per rank, shipped size after the fence's own
compression:

| string size | ranks emit similar strings | ranks emit unrelated strings |
|---|---|---|
| 1 KB | no change | no change |
| 2 KB | **+53%** worse | −1% |
| 4 KB | **+21%** worse | no change |
| 8 KB | −21% better | +1% |
| 16 KB | −49% better | +0.5% |

For unrelated strings it is a wash at every size. For similar ones —
the realistic case — pre-compression is a substantial *loss* below about
8 KB and only starts paying above it. If that upper range ever matters
enough to reclaim, give it a **separate** threshold rather than reviving
this one, compress at pack time rather than at `PMIx_Put` time so the
datastore still holds the natural string, and keep the unpack-side
expansion.

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
| `pcompress_lz4_level` | int, **0** | level passed to `LZ4F_compressFrame`; 0 is the plain LZ4 codec, any positive value switches to the slower, denser LZ4HC |

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

`base/base.h` used to export two convenience macros for callers —
`PMIX_STRING_SIZE_CHECK`, which asked whether a string value was long
enough to be worth compressing, and
`PMIX_VALUE_COMPRESSED_STRING_UNPACK`, which inflated one back into a
`PMIX_STRING`. Both are gone; see "Strings are not compressed
individually" above. Do not reintroduce either without reading it.

## The shared compressed-blob format

All four components produce and consume the **same** on-blob layout, and
this is a contract, not an implementation detail:

```
+-------------------+---------------------------------+
| uint32 raw_length |       compressed payload        |
+-------------------+---------------------------------+
   4 bytes (host       DEFLATE stream, zstd frame or
   byte order)         LZ4 frame - see below
```

- The first 4 bytes are the **uncompressed** length, written with a raw
  `memcpy` of a `uint32_t` — i.e. **host byte order**, not network order.
- The remainder is the compressed payload. For `zlib` and `zlib-ng` it is
  a standard DEFLATE stream; because both emit and read that format and
  both use this identical 4-byte framing, a blob produced by one is
  readable by the other, which is what makes it safe for the build to pick
  `zlibng` on one node and `zlib` on another.
  **`zstd` and `lz4` break that symmetry**: each keeps the 4-byte prefix
  (so every size query still works) but its payload is a zstd or an LZ4
  frame, which a peer running any of the other components cannot read. Compressed blobs *do* cross nodes — the
  server compresses each daemon's fence bucket and the receivers inflate
  it — so **every node in a job must run the same component**. In practice
  that follows from a shared installation. Both check their own frame magic on the
  way in and refuse anything else, which converts the mixed-build failure
  from a corrupted modex into a clean error; it does not make the mixed
  build work. If heterogeneous deployments ever have to be
  supported, the fix is to make the blob self-describing across the whole
  framework — a scheme byte, or that sniff generalized into the base — not
  a per-component workaround.
- **Every entry point that inflates must screen the blob before trusting
  it.** `decompress` and `decompress_string` both read the 4-byte prefix
  and then size the payload as `len - 4`, and the length is a claim that
  generally came off a peer's wire — a byte object out of a modex, a
  `pmix_regex2_t` carrying whatever length the peer declared, or whatever
  a caller handed the public `PMIx_Data_decompress`, which screens for a
  NULL pointer and nothing else. Below four bytes the prefix read runs off
  the end and the subtraction underflows into roughly four billion bytes
  of "input" for the inflater to walk. So each of the two entry points
  begins with `if (NULL == in || len < sizeof(uint32_t)) return false;`
  and `get_decompressed_size` applies the same test against `bo->size`.
  This is a contract on the *slot*, not a property of any one component:
  a caller cannot know which component will answer, so all of them must
  screen or none of the screening counts. `test/unit/compress_block`
  drives every entry point with blobs of length 0..3 and with NULL for
  exactly this reason.
- `compress` returns `false` (declines) when the input is shorter than
  `compress_limit`, when it is `>= UINT32_MAX` (the length would not fit
  the 4-byte prefix), or when the compressed result is **not** smaller
  than the input. This last check is why a caller can always trust that a
  `true` return actually saved space.
- `decompress_string` treats a stored `raw_length` of `UINT32_MAX` as an
  error sentinel and NUL-terminates the inflated string.
- **`get_decompressed_size` / `get_decompressed_strlen` return 0 to mean
  "I cannot answer."** That is what they return for a NULL or too-short
  blob, and it is the value `bfrops`' `decompressed_size()` /
  `decompressed_strlen()` substitute for a module that leaves the slot
  `NULL` altogether. A stored length of zero is therefore also answered
  as 0 by *both* — `get_decompressed_strlen` must not turn it into
  `0 + 1` and report a one-byte string, because a zero prefix is not a
  real blob (`compress` declines everything below `compress_limit`) and
  the caller has no way to tell an invented 1 from a real one.

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
  clears `selected`, calls the selected module's `finalize` if set,
  **restores `pmix_compress` to the base default stubs**, and closes the
  components. That restore matters: `components_close` `dlclose`s the
  winning component — under `--enable-mca-dso` that is every component —
  while `pmix_compress` is still holding six function pointers into it.
  Clearing `selected` alone only guarantees that a later `select()` will
  *run*; it does not guarantee it will *find* anything, and a select that
  picks nothing leaves the stale pointers in place rather than replacing
  them.
- **`base/pcompress_base_select.c`** (`pmix_compress_base_select`) runs
  once, short-circuits if already `selected`, calls `pmix_mca_base_select`
  to pick the highest-priority module, runs its `init` if non-`NULL`, and
  copies it into `pmix_compress`. Selecting nothing is **not** an error —
  the base default stubs simply remain in place. (Contrast `ptl`/`preg`,
  which treat "no component" as fatal; `pcompress` degrades to a no-op.)
  **It returns `PMIX_SUCCESS` in every case**, including a winning
  module whose `init` fails: `pmix_rte_init()` treats any other status as
  fatal to `PMIx_Init`, and a compression library that loads but cannot
  start is the same situation as one that was never installed — the
  stubs are what the library will call either way. A failed `init`
  leaves `pmix_compress` untouched, so the module never becomes the
  active one and `pcompress_base_close` never calls its `finalize`
  either. The degradation is not silent: the first attempt to compress
  anything emits the "unavailable" help message.

The framework is opened and selected during library init in
[`src/runtime/pmix_init.c`](../../runtime/pmix_init.c) for **all** process
roles (client, server, tool), and closed in
[`src/runtime/pmix_finalize.c`](../../runtime/pmix_finalize.c).

**Both writes to `pmix_compress` happen with no progress thread running**,
which is what makes a plain struct assignment to a global that the progress
thread later reads safe without any lock. `pmix_init.c` selects at line ~529
and does not call `pmix_progress_thread_start()` until ~607; `pmix_finalize.c`
calls `pmix_progress_thread_pause()` before it closes any framework. Do not
add a path that re-selects or re-closes this framework while the progress
thread is live without revisiting that.

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
├── zstd/                       zstd compressor (priority 90, preferred)
├── zlibng/                     zlib-ng compressor (priority 75)
├── lz4/                        LZ4 compressor (priority 60)
└── zlib/                       zlib compressor (priority 50)
```

## Building

The framework core (`base/`) is always built into `libpmix`. Each
component is a standard MCA component **conditionally compiled** by its
`configure.m4`, each probing for one header plus one symbol:

| Component | header | symbol |
|---|---|---|
| `zlib` | `zlib.h` | `deflate` in `libz` |
| `zlibng` | `zlib-ng.h` | `zng_deflate` in `libz-ng` |
| `zstd` | `zstd.h` | `ZSTD_compress` in `libzstd` |
| `lz4` | `lz4frame.h` | `LZ4F_compressFrame` in `liblz4` |

Each honors `--with-<name>[-libdir]`, errors out if support was
explicitly requested but not found, and adds its library to the
`PMIX_EMBEDDED_*` flags and the configure summary. A host with none of
the four libraries builds no component, and the framework runs on the
base default no-op module.

It is the **dev** package that decides this, not the runtime shared
library — an image carrying `liblz4` without `liblz4-dev` silently drops
the `lz4` component. `contrib/dockerswarm/Dockerfile` installs the dev
packages for `zlib`, `zstd` and `lz4` for exactly that reason.

Note the version macro every component opens with is
**`PMIX_MCA_BASE_VERSION(pcompress)`**, which stamps in the framework's
interface version — 3.0.0, from the three `PMIX_MCA_pcompress_*_VERSION`
macros in `pcompress.h`, even though the module struct type is still
named `..._1_0_0_t`.

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
- **Keep `zlib` and `zlibng` byte-for-byte compatible.** `zlibng` is a
  drop-in of `zlib` and the two files are line-for-line parallel; any
  change to framing, the size prefix, the decline rules, or the input
  screening must land in *both* or the "either build works" guarantee
  breaks. In practice a fix found in one of them is a fix in the other,
  and it is worth checking whether `zstd` and `lz4` need it too — the
  four are independent transcriptions of one contract, which is exactly
  how a guard ends up in three of them and not the fourth.
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
