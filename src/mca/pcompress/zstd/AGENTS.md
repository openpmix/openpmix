<!--
  Copyright (c) 2026      Nanook Consulting  All rights reserved.

  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
-->

# AGENTS.md: The `zstd` PCOMPRESS Component

Orientation for the `pcompress/zstd` component. Read the framework's
[`../AGENTS.md`](../AGENTS.md) first — the module contract, the selection
model, and the shared blob format are described there and are not repeated
here. The top-level [`AGENTS.md`](../../../../AGENTS.md) governs everything
else.

## Why this component exists

`pcompress` had two components, `zlib` and `zlibng`, both emitting DEFLATE
and both deflating at level 9 unconditionally — the wrong end of the
speed/size curve for what this framework is actually used for, which is
compressing large collective payloads on a single progress thread while a
whole DVM waits. (Those two now take a level parameter of their own and
default to 1; the comparison below is against the level 9 that was.)

Measured on a 25.6 MB aggregated modex (repetitive PMIx keys wrapped around
opaque endpoint values), single-threaded:

| | ratio | throughput |
|---|---|---|
| `zlib` deflate level 9 | 0.600 | 64 MB/s |
| zstd level 1 | 0.600 | 640 MB/s |
| **zstd level 3 (this default)** | **0.588** | **427 MB/s** |

So the default is both **smaller and ~7x faster** than what it displaces.
That is why this component is safe to rank above the zlib ones without
asking anyone to retune anything. Decompression, which every peer pays
rather than just the originator, is faster still: measured through
`PMIx_Data_decompress`, ~3.6 GB/s against zlib's ~0.6 GB/s.

## Priority: 90

Above `zlibng` (75) and `zlib` (50). Like theirs, `compress_zstd_query()`
gates on nothing — a component is compiled only if its `configure.m4` found
the library, so an unavailable one is simply not in the build and cannot be
selected. To force one of the others without rebuilding:

```sh
--pmixmca pcompress zlib          # or ^zstd to exclude this one
```

## The format problem, and what is and is not done about it

**This is the one thing to understand before touching this component.**

The framework's blob format is `[uint32 raw_length][compressed payload]`.
The length prefix is unchanged here — `get_decompressed_size` and
`get_decompressed_strlen` read it exactly as the other components do, so
every caller that sizes a `PMIX_COMPRESSED_BYTE_OBJECT` without inflating
it keeps working.

The **payload** is not DEFLATE. `zlib` and `zlibng` were interchangeable
because both emit the standard DEFLATE stream, which is what let a build
pick one on one node and the other on another. A zstd frame breaks that
symmetry: a blob this component produces **cannot be read by a peer whose
PMIx selected `zlib` or `zlibng`**.

Compressed blobs do cross nodes — `pmix_server_fence.c` compresses each
daemon's bucket and the receiving daemons inflate it — so this matters in
practice. What is done about it:

- `doit()` checks the **zstd frame magic** (`28 B5 2F FD`, the
  little-endian serialization of `ZSTD_MAGICNUMBER`) before inflating and
  refuses anything else. That does not make a mixed cluster work; it turns
  the failure from "silently inflate garbage into the modex" into a clean
  `false` that the caller reports.
- Nothing else. **Every node in a job must run the same `pcompress`
  component.** In practice that follows from a shared PMIx installation,
  and PRRTE already forbids mixed-version DVMs outright — but if you are
  staging a rolling upgrade across a cluster with a per-node PMIx, do not
  let some nodes get zstd and others not.

If a genuinely heterogeneous deployment ever has to be supported, the fix
is a self-describing blob format for the whole framework (a scheme byte, or
the sniff generalized into the base), **not** a per-component workaround
here. See the framework AGENTS.md's warning about changing this format.

## MCA parameter

| Parameter | Type / default | Meaning |
|-----------|----------------|---------|
| `pcompress_zstd_level` | int, **3** | Level passed to `ZSTD_compress`. 1 is fastest, 19 smallest. |

A parameter rather than a constant, deliberately — the level is the term
that decides whether compressing a large payload is worth doing at all, and
the answer depends on the bandwidth of the link the result will cross, which
no library can know. Every component in the framework now exposes it the
same way (`pcompress_zlib_level`, `pcompress_zlibng_level`), which is why
this one does not need to explain the convention.

## Contract details that are easy to get wrong

- **`capacity` and `expected` in `doit()` are not the same thing.** The
  string path allocates one byte more than the stored length so it can
  append the NUL that length deliberately does not count, then requires
  `ZSTD_decompress` to produce exactly `expected`. Folding the two into one
  argument makes the strict length check reject every string the component
  ever compressed — that bug was written and then caught by
  `test/unit/compress_block`.
- **The economy rules are the framework's, not zstd's.** Decline below
  `pmix_compress_base.compress_limit`, decline at `>= UINT32_MAX` (the
  length would not fit the prefix), and decline when the result is not
  actually smaller. Callers rely on a `true` return meaning space was saved.
- **`ZSTD_compressBound` can exceed the input** for incompressible data.
  The buffer is `realloc`'d down to what was produced before it is handed
  back; a failed trim is not an error, only a missed optimization.

## Testing

[`test/unit/compress_block.c`](../../../../test/unit/compress_block.c)
drives `PMIx_Data_compress`/`_decompress` and the string pair against
whichever component the build selected, asserting the framework contract
rather than anything zstd-specific: declines below the limit, never claims
success without shrinking, the prefix round-trips through
`get_decompressed_size`/`get_decompressed_strlen`, and the payload survives
exactly. It passes for `zlib`, `zlibng` and `zstd`, and skips (77) under
`--enable-test-build`, where every component is a non-functional shim.

`testbuild_zstd.h` is that shim for this component: it stubs
`ZSTD_compressBound`/`ZSTD_compress`/`ZSTD_decompress`/`ZSTD_isError` so the
component compiles on a machine with no zstd headers. It performs no
compression; a component built against it is not functional.
