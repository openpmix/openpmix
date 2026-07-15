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
**preferred** compressor: when built, it outranks
[`zlib`](../zlib/AGENTS.md) and is selected.

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

Priority **75** is above `zlib`'s **50**, so on any host where zlib-ng was
found `zlibng` is the module `pmix_mca_base_select` copies into
`pmix_compress` — `zlib`, if also built, is present but unselected. This
is how "use the faster library when it's available" is expressed without
any runtime probing: it falls straight out of the priority ordering.

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
the blob's 4-byte length prefix) because `bfrops` calls them unguarded —
see the framework doc.

## What the functions do

The logic is **line-for-line identical** to the `zlib` component; only the
API names carry the `zng_` prefix (`zng_stream`, `zng_deflateInit`,
`zng_deflateBound`, `zng_deflate`, `zng_inflate`, …) and the header is
`<zlib-ng.h>`:

- **`zlibng_compress`** — declines if input `< compress_limit` or
  `>= UINT32_MAX`; otherwise `zng_deflateInit(&strm, 9)` (level 9),
  `zng_deflateBound` for the output bound, one `Z_FINISH` deflate,
  declines if the result is not strictly smaller than the input, then
  emits the shared framing: a leading host-order `uint32_t` uncompressed
  length followed by the DEFLATE stream.
- **`compress_string`** — `strlen` + forward to `zlibng_compress`.
- **`zlibng_decompress`** / **`decompress_string`** — read the 4-byte
  length prefix and inflate the remainder via the local `doit`
  (`zng_inflateInit` + one `Z_FINISH` `zng_inflate`); the string variant
  treats `UINT32_MAX` as an error sentinel and NUL-terminates.

Because zlib-ng emits the standard DEFLATE format and this component uses
the **same** 4-byte framing as `zlib`, blobs are interchangeable between
the two: a `zlibng`-compressed value decompresses correctly under a peer
built with `zlib`, and vice versa.

## Gotchas

- **It must stay a mirror of `zlib`.** The two files differ only by the
  `zng_` API prefix and the include. If you change framing, the level, or
  the decline rules in one, change the other too, or the interchange
  guarantee breaks.
- **Level 9 is hard-coded** (`zng_deflateInit(&strm, 9)`), same as `zlib`.
- **The size prefix is host byte order** (`memcpy` of the `uint32_t`), not
  network order — same caveat as `zlib`.
- The performance win over `zlib` is entirely inside zlib-ng; PMIx does
  nothing special to obtain it beyond linking the faster library and
  ranking this component higher.
