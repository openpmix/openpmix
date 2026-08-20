<!--
  Copyright (c) 2026      Nanook Consulting  All rights reserved.

  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
-->

# AGENTS.md: The PCOMPRESS `zlib` Component

`zlib` is the `pcompress` component that compresses through the classic
[zlib](https://zlib.net/) library (`libz`). Read the framework
[`AGENTS.md`](../AGENTS.md) first; this file covers only what is specific
to `zlib`. It is the baseline compressor: broadly available, and the lowest-ranked
of the four, so it is selected only when it is the sole compression
component in the build.

## Files

| File | Contents |
|------|----------|
| `compress_zlib.h` | Declares the component and module symbols. |
| `compress_zlib_component.c` | Component struct + `compress_zlib_query` (priority **50**). |
| `compress_zlib.c` | The module: `compress`/`decompress` and their string variants. |
| `configure.m4` | Locates zlib; gates whether this component is built at all. |

## When it is selected

`compress_zlib_query` unconditionally hands back the module and priority
**50** — there is no runtime availability gate. Whether `zlib` is even a
candidate is decided at **configure** time by `configure.m4`, which runs
`OAC_CHECK_PACKAGE([zlib], ..., [zlib.h], [z], [deflate], ...)`. If zlib
is not found the component is not compiled and never enters
`static-components.h`. `--with-zlib=DIR` / `--with-zlib-libdir=DIR` steer
the search; requesting zlib explicitly and not finding it is a hard
configure error.

Priority 50 is the lowest in the framework — below `zstd` (90),
`zlibng` (75) and `lz4` (60) — so on a host where any other compression
library was also found `zlib` is built but **not selected**. `zlib`
therefore wins only when it is the sole compression component in the
build.

## The module

```c
pmix_compress_base_module_t pmix_pcompress_zlib_module = {
    .compress = zlib_compress,
    .decompress = zlib_decompress,
    .get_decompressed_size = get_decompressed_size,
    .compress_string = compress_string,
    .decompress_string = decompress_string,
    .get_decompressed_strlen = get_decompressed_strlen,
};
```

`init` and `finalize` are left `NULL`. `get_decompressed_size` /
`get_decompressed_strlen` **are** provided (they read the blob's 4-byte
length prefix). `bfrops` no longer calls them unguarded — it substitutes
0 for a module that leaves the slot `NULL`, because a run-time-loadable
component can be older than the `libpmix` that loads it — but a new
module should still implement both. See the framework doc.

## What the functions do

- **`zlib_compress`** — the core. Declines (returns `false`) if the input
  is shorter than `pmix_compress_base.compress_limit` or `>= UINT32_MAX`.
  Otherwise it runs `deflateInit(&strm, pmix_pcompress_zlib_level)`
  (**level 1** by default; see the parameter below),
  sizes the output with `deflateBound`, deflates in a single
  `Z_FINISH` pass into that upper-bound buffer, and — critically — if the
  result is not strictly smaller than the input, frees it and declines.
  On success it allocates the final buffer as
  `sizeof(uint32_t) + compressed_len`, writes the **uncompressed** length
  into the leading 4 bytes (host-order `memcpy`), and copies the DEFLATE
  stream after it. This 4-byte-prefix framing is the shared format
  described in the framework doc.
- **`compress_string`** — `strlen`s the input into a `size_t` and
  forwards to `zlib_compress`. The width matters: narrowing to `uint32_t`
  here would hand `zlib_compress` a length modulo 2^32 for a very long
  string, and it would then compress a prefix and label the blob as the
  whole string. Above `UINT32_MAX` the economy rule inside declines, which
  is where such an input belongs.
- **`zlib_decompress`** — screens for a NULL buffer or a length below
  `sizeof(uint32_t)` (see the framework doc's blob-screening rule), reads
  the leading 4-byte length, then calls the local `doit` helper
  (`inflateInit` + a single `Z_FINISH` inflate into a buffer of exactly
  that length) on the bytes past the prefix.
- **`decompress_string`** — same screen, then the same inflate, but treats
  a stored length of `UINT32_MAX` as an error sentinel, allocates one
  extra byte, and forces a NUL terminator on the inflated string.
- **`get_decompressed_size`** / **`get_decompressed_strlen`** — read the
  leading 4-byte length prefix and return the inflated size *without*
  inflating: `_size` returns the raw byte count, `_strlen` returns it +1
  for the NUL (to match `PMIX_STRING` sizing). Both return 0 on a
  `NULL`/too-short blob. Used by `bfrops` to size a
  `PMIX_COMPRESSED_BYTE_OBJECT` / `PMIX_COMPRESSED_STRING` value.

## Gotchas

- **The level is `pcompress_zlib_level`, and it defaults to 1, not 9.**
  It used to be a hard-coded 9. That is the wrong end of the curve for what
  this framework compresses — a large collective payload, on one progress
  thread, with a whole job waiting. Measured through PMIx on a 25.6 MB
  aggregated modex: level 1 gives ratio 0.649 at 103 MB/s against level 9's
  0.638 at 54 MB/s, so 9 costs roughly twice the CPU to shrink the result a
  further 1.8%. A broadcast pays the deflate **once** and the wire cost on
  **every link of the tree**, so that 1.8% only repays itself where the tree
  is very wide and the link slow; raise the parameter there.
- **The size prefix is host byte order.** `memcpy` of the `uint32_t`, not
  `htonl`. Fine on a single node and between this component and `zlibng`;
  a cross-endian wire path would need a defined byte order (see the
  framework doc).
- **zlib's `avail_in`/`avail_out` are `uInt`, but `deflateBound` returns a
  `uLong` and the framework's lengths are `size_t`.** On LP64 those are
  not the same width. `zlib_compress` refuses a bound above `UINT_MAX`
  rather than assigning it: a truncated `avail_out` would leave the stream
  believing it had less room than was allocated while the local `len` still
  recorded the full amount, and the produced length computed from the two
  would overrun `tmp` in the memcpy that lifts the payload out. `doit`
  screens both of its lengths for the same reason. Nothing this framework
  compresses is anywhere near the ceiling; the screens are there so the
  narrowing can never happen silently.
- **A foreign blob is refused, but by accident rather than by a magic
  check.** Unlike `zstd` and `lz4`, this component does not sniff a frame
  header — it does not need to, because the DEFLATE header's own checksum
  rejects both of their magics (`28 B5 ...` fails the FCHECK mod-31 test,
  `04 22 ...` has an invalid compression method). The outcome is the same
  clean `false`, and `test/unit/compress_block` pins it down so it stays
  that way.
- **Keep it a mirror of `zlibng`.** The two components are intentionally
  line-for-line parallel (only the `zng_`-prefixed API and header differ).
  Any fix here almost certainly belongs in `zlibng` too, or the "either
  build produces interoperable blobs" guarantee breaks — and it is worth
  asking whether `zstd` and `lz4` need it as well. All four are
  independent transcriptions of one contract, which is how a guard ends up
  in three of them and not the fourth.
- The heavy lifting is zlib's; this file only frames the result and
  enforces the decline rules. Bugs in the DEFLATE stream itself belong to
  the system zlib, not here.
