<!--
  Copyright (c) 2026      Nanook Consulting  All rights reserved.

  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
-->

# AGENTS.md: The `lz4` PCOMPRESS Component

Orientation for the `pcompress/lz4` component. Read the framework's
[`../AGENTS.md`](../AGENTS.md) first — the module contract, the selection
model, and the shared blob format are described there and are not repeated
here. The top-level [`AGENTS.md`](../../../../AGENTS.md) governs everything
else.

## Why this component exists

The other three components sit close together on the speed/size curve:
`zlib` and `zlibng` emit DEFLATE, and `zstd` is both faster and denser
than either. None of them offers the *fast* end of the curve, where the
compressor costs so little that attempting it is nearly free even when it
does not pay off.

That end matters here for a specific reason. `pcompress` is called on the
progress thread of a daemon while a whole DVM waits on the collective, and
the decision to compress is made on **size alone** — no component looks at
the data. For a payload that does not compress (opaque endpoint blobs,
already-compressed application data) the whole cost is spent and nothing is
returned. The cheaper the codec, the less that mistake costs.

LZ4 is that codec. It gives up ratio for throughput, so it is not the right
default where bandwidth is the scarce resource — which is why it is **not**
ranked first. See "Priority" below.

## Priority: 60, deliberately below `zstd`

| component | priority |
|---|---|
| `zstd` | 90 |
| `zlibng` | 75 |
| **`lz4`** | **60** |
| `zlib` | 50 |

Above `zlib`, because it beats `zlib` on both terms. Below `zstd`, because
which of those two should win is a **bandwidth-against-CPU judgement that
depends on the link the payload will cross**, and nothing in the library
can see that link. Ranking `lz4` first would silently change the trade for
every existing build.

Select it explicitly to measure or to use it:

```sh
prterun --pmixmca pcompress lz4 ...
```

Re-rank it here if measurement on a real fabric says it should win by
default — that is a policy decision backed by numbers, not a default to
drift into.

## The level parameter means something different here

`pcompress_lz4_level` defaults to **0**, and 0 is not "the lowest setting of
one algorithm" the way it is for the zlib components. In the LZ4 library a
level of 0 selects the plain LZ4 codec, and any positive value switches the
same call to **LZ4HC** — a different, much slower, denser algorithm.

LZ4HC is not a useful place for this framework to be: it is slower than
`zstd` at a comparable ratio, so if density is what you want, use `zstd`.
The parameter exists because the framework's convention is that a level is
tunable rather than baked in, not because a positive value is expected to
be a good idea.

## It uses the FRAME api, not the block api

`LZ4F_compressFrame` / `LZ4F_decompress` from `<lz4frame.h>`, not
`LZ4_compress_default` / `LZ4_decompress_safe` from `<lz4.h>`. The block
API would be marginally faster and the framework's 4-byte length prefix
already supplies the uncompressed size the block decoder needs — so the
choice needs a reason, and the reason is the **mixed-build failure mode**.

A blob carries no indication of which component produced it (see the
framework's blob-format section). The LZ4 block format has no header at
all, so a DEFLATE blob handed to the block decoder is decoded as far as it
happens to parse. A **frame** starts with a magic number, so it can be
refused at the first byte — which is what `is_lz4_frame()` does, exactly as
`zstd` does with its own magic.

That does not make a mixed build work; nothing here can. Every node in a
job must run the same component. It converts the failure from a corrupted
modex into a clean refusal, which is the difference between a diagnosable
configuration error and a wrong answer.

The frame also carries `contentSize` in its header. This component does not
need it — it has the prefix — but it costs 8 bytes and makes a captured
blob readable by anything that speaks LZ4.

## Decompression is strict on purpose

`doit()` rejects the result unless **all** of these hold: the decoder
returned 0 (frame complete, no more input wanted), it produced exactly the
length the prefix promised, and it consumed exactly the bytes it was given.
A frame that decodes partially, or that leaves trailing bytes, is not one
this component wrote. Loosening any of the three turns a truncated or
foreign blob into a partial answer the caller cannot tell from a real one.

## Building

`configure.m4` probes for `lz4frame.h` and the **`LZ4F_compressFrame`**
symbol. Probe the frame symbol, not `LZ4_compress` — that name is the
long-deprecated block entry point and has been removed from current lz4
releases, so probing it would fail on exactly the systems where the library
is newest.

Like the other components, this one is compiled only if `configure` found
the library, so its `component_query` gates on nothing. The dev package
(`liblz4-dev` on Debian/Ubuntu) is what decides this — the runtime `.so`
alone is not enough, and an image carrying one without the other silently
drops the component. `contrib/dockerswarm/Dockerfile` installs the dev
packages for all of `zlib`/`zstd`/`lz4` for that reason.

`testbuild_lz4.h` is the non-functional shim `--enable-test-build` compiles
against where the real headers are absent. It is not a working compressor;
never run the functional suite against a test build.

## Testing

`test/unit/compress` and `test/unit/compress_block` exercise whichever
component is selected, so run them once per component:

```sh
cd test/unit
for c in lz4 zstd zlibng zlib; do
    (. ../pmix_test_env.sh; PMIX_MCA_pcompress=$c ./compress_block)
done
```

Sourcing `pmix_test_env.sh` is what points the loader at the components in
the **build tree**; without it the binary finds whatever is installed under
`$prefix/lib/pmix`, which is how a component that was never built appears
to "work" by silently being a different one.
