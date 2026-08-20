<!--
  Copyright (c) 2026      Nanook Consulting  All rights reserved.

  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
-->

# AGENTS.md: The PCOMPRESS `zlibng` Component

`zlibng` is the `pcompress` component that compresses through
[zlib-ng](https://github.com/zlib-ng/zlib-ng) (`libz-ng`), a performance
fork of zlib that is a drop-in replacement offering the same DEFLATE
format at higher speed. Read the framework [`AGENTS.md`](../AGENTS.md)
first; this file covers only what is specific to `zlibng`. It is the
**preferred DEFLATE** compressor: when built, it outranks
[`zlib`](../zlib/AGENTS.md) and is selected in its place — but
[`zstd`](../zstd/AGENTS.md), at priority 90, outranks them both.

## Files

| File | Contents |
|------|----------|
| `compress_zlibng.h` | Declares the component and module symbols. |
| `compress_zlibng_component.c` | Component struct + `compress_zlibng_query` (priority **75**). |
| `compress_zlibng.c` | The module: `compress`/`decompress` and their string variants. |
| `configure.m4` | Locates zlib-ng; gates whether this component is built at all. |

## When it is selected

`compress_zlibng_query` unconditionally hands back the module and priority
**75** — no runtime gate. Availability is decided at **configure** time by
`configure.m4`, which runs
`OAC_CHECK_PACKAGE([zlibng], ..., [zlib-ng.h], [z-ng], [zng_deflate], ...)`.
If zlib-ng is not found the component is not compiled and never enters
`static-components.h`. `--with-zlibng=DIR` / `--with-zlibng-libdir=DIR`
steer the search; requesting zlib-ng explicitly and not finding it is a
hard configure error.

Priority **75** sits above `lz4`'s **60** and `zlib`'s **50** and below
`zstd`'s **90**. So on a host where zlib-ng was found but zstd was not,
`zlibng` is the module `pmix_mca_base_select` copies into `pmix_compress`
and `zlib`, if also built, is present but unselected; where zstd was also
found, `zstd` wins and this component is built but idle. This is how "use
the faster library when it's available" is expressed without any runtime
probing: it falls straight out of the priority ordering.

## The module

```c
pmix_compress_base_module_t pmix_pcompress_zlibng_module = {
    .compress = zlibng_compress,
    .decompress = zlibng_decompress,
    .get_decompressed_size = get_decompressed_size,
    .compress_string = compress_string,
    .decompress_string = decompress_string,
    .get_decompressed_strlen = get_decompressed_strlen,
};
```

`init` and `finalize` are left `NULL`, exactly as in `zlib`.
`get_decompressed_size` / `get_decompressed_strlen` are provided (reading
the blob's 4-byte length prefix). `bfrops` no longer calls them unguarded
— it substitutes 0 for a module that leaves the slot `NULL`, because a
run-time-loadable component can be older than the `libpmix` that loads it
— but a new module should still implement both. See the framework doc.

## What the functions do

The logic is **line-for-line identical** to the `zlib` component; only the
API names carry the `zng_` prefix (`zng_stream`, `zng_deflateInit`,
`zng_deflateBound`, `zng_deflate`, `zng_inflate`, …) and the header is
`<zlib-ng.h>`:

- **`zlibng_compress`** — declines if input `< compress_limit` or
  `>= UINT32_MAX`; otherwise `zng_deflateInit(&strm,
  pmix_pcompress_zlibng_level)` (**level 2** by default),
  `zng_deflateBound` for the output bound, one `Z_FINISH` deflate,
  declines if the result is not strictly smaller than the input, then
  emits the shared framing: a leading host-order `uint32_t` uncompressed
  length followed by the DEFLATE stream.
- **`compress_string`** — `strlen` into a `size_t` + forward to
  `zlibng_compress`. Narrowing to `uint32_t` here would compress a prefix
  of a very long string and label the blob as the whole thing; above
  `UINT32_MAX` the economy rule inside declines, which is where such an
  input belongs.
- **`zlibng_decompress`** / **`decompress_string`** — both screen for a
  NULL buffer or a length below `sizeof(uint32_t)` (see the framework
  doc's blob-screening rule), then read the 4-byte length prefix and
  inflate the remainder via the local `doit` (`zng_inflateInit` + one
  `Z_FINISH` `zng_inflate`); the string variant treats `UINT32_MAX` as an
  error sentinel and NUL-terminates.

Because zlib-ng emits the standard DEFLATE format and this component uses
the **same** 4-byte framing as `zlib`, blobs are interchangeable between
the two: a `zlibng`-compressed value decompresses correctly under a peer
built with `zlib`, and vice versa.

## Gotchas

- **It must stay a mirror of `zlib`.** The two files differ only by the
  `zng_` API prefix and the include. If you change framing, the level, the
  decline rules, or the input screening in one, change the other too, or
  the interchange guarantee breaks — and ask whether `zstd` and `lz4` need
  it as well. All four are independent transcriptions of one contract,
  which is how a guard ends up in three of them and not the fourth.
- **`zng_stream`'s `avail_in`/`avail_out` are 32-bit, but `zng_deflateBound`
  returns a `size_t`-width value and the framework's lengths are `size_t`.**
  `zlibng_compress` refuses a bound above `UINT_MAX` rather than assigning
  it: a truncated `avail_out` would leave the stream believing it had less
  room than was allocated while the local `len` still recorded the full
  amount, and the produced length computed from the two would overrun
  `tmp`. `doit` screens both of its lengths for the same reason.
- **The level is `pcompress_zlibng_level`, and it defaults to 2** — where
  `zlib` defaults to 1. Both used to be a hard-coded 9; see
  [`../zlib/AGENTS.md`](../zlib/AGENTS.md) for why that was the wrong end of
  the curve.
- **The reason the two defaults differ is that zlib-ng's level 1 is not
  zlib's level 1.** zlib-ng remaps its lowest level onto a quick-deflate
  strategy that is much faster and appreciably weaker. Measured through PMIx
  on a 25.6 MB aggregated modex: level 1 gives ratio 0.678 at 242 MB/s,
  level 2 gives **0.649** at 152 MB/s — and 0.649 is exactly what `zlib`
  level 1 produces. So 2 here is the setting that matches the sibling
  component's default *behaviour* rather than merely its number. Match the
  behaviour, not the digit, if you ever retune either one.
- **The size prefix is host byte order** (`memcpy` of the `uint32_t`), not
  network order — same caveat as `zlib`.
- The performance win over `zlib` is entirely inside zlib-ng; PMIx does
  nothing special to obtain it beyond linking the faster library and
  ranking this component higher.
