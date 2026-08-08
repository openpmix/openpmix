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
to `zlib`. It is the baseline compressor: broadly available, selected
when built unless the faster [`zlibng`](../zlibng/AGENTS.md) was also
built (which outranks it).

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

Because priority 50 is below `zlibng`'s 75, on a host where both
libraries were found `zlib` is built but **not selected** —
`pmix_mca_base_select` prefers `zlibng`. `zlib` therefore wins only when
it is the sole compression component in the build.

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
length prefix) because `bfrops` calls them unguarded — see the framework
doc.

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
- **`compress_string`** — `strlen`s the input and forwards to
  `zlib_compress`.
- **`zlib_decompress`** — reads the leading 4-byte length, then calls the
  local `doit` helper (`inflateInit` + a single `Z_FINISH` inflate into a
  buffer of exactly that length) on the bytes past the prefix.
- **`decompress_string`** — same, but treats a stored length of
  `UINT32_MAX` as an error sentinel, allocates one extra byte, and forces
  a NUL terminator on the inflated string.
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
  `htonl`. Fine on a single node and between the two components; a
  cross-endian wire path would need a defined byte order (see the
  framework doc).
- **Keep it a mirror of `zlibng`.** The two components are intentionally
  line-for-line parallel (only the `zng_`-prefixed API and header differ).
  Any fix here almost certainly belongs in `zlibng` too, or the "either
  build produces interoperable blobs" guarantee breaks.
- The heavy lifting is zlib's; this file only frames the result and
  enforces the decline rules. Bugs in the DEFLATE stream itself belong to
  the system zlib, not here.
