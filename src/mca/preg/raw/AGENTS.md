<!--
  Copyright (c) 2026      Nanook Consulting  All rights reserved.

  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
-->

# AGENTS.md: The PREG `raw` Component

`raw` is the `preg` component that performs **no compression at all**: it
hands the input back as the encoded payload, tagged `raw`. Read the
framework [`AGENTS.md`](../AGENTS.md) first; this file covers only what is
specific to `raw`. It is the simplest component and the framework's
guaranteed fallback encoder.

## Files

| File | Contents |
|------|----------|
| `preg_raw.h` | Declares the component and module symbols. |
| `preg_raw_component.c` | Component struct + `component_query` (priority **50**, always available). |
| `preg_raw.c` | The module: `generate_regex` and `parse_regex`. |

## What it does

- **`generate_regex`** — sets `regex->type = "raw"`,
  `regex->bytes = strdup(input)`, `regex->len = strlen(input) + 1`. It
  always succeeds (barring `PMIX_ERR_NOMEM`), so in the framework's
  **smallest-wins** contest `raw` is the baseline every other component
  must beat. For inputs too small or too random for `compress` to shrink,
  `raw` legitimately wins.
- **`parse_regex`** — gates on `regex->type == "raw"`, returning
  `PMIX_ERR_TAKE_NEXT_OPTION` otherwise, and returns
  `strdup(regex->bytes)`. Because `raw`'s bytes are just the original
  NUL-terminated list, no expansion is needed.

Both ignore their `info`/`ninfo` attribute arrays
(`PMIX_HIDE_UNUSED_PARAMS`); the parameters exist only to satisfy the
"every API carries attributes" rule.

Note that `raw` knows nothing about node lists versus process maps, and
nothing about the deprecated `char *` API. The base serializes a `raw`
regex2 as `raw:<string>` when a caller comes in through
`PMIx_generate_regex`/`PMIx_generate_ppn` — see
[`preg_base_legacy.c`](../base/preg_base_legacy.c).

## Why `raw` matters despite doing nothing

1. **It is the regex2 baseline.** `generate_regex`'s smallest-wins logic
   needs at least one always-succeeding candidate or it would return
   `PMIX_ERR_NOT_SUPPORTED` for incompressible input. `raw` is that
   candidate, and it is the reason `PMIx_generate_regex2` can be relied on
   to work in a build with no compression library at all.
2. **It is the encoder of last resort.** When `compress` is absent the
   data still ships, just uncompressed.
3. **It round-trips anything.** Since it does not interpret the payload at
   all, `raw` can carry a list no other scheme understands.

## Gotchas

- Do not add "cleverness" to `raw`. Its whole value is that it is a
  transparent, always-available identity transform. If you want smarter
  encoding, add a new component with a new tag.
- The component name string `"raw"` is the on-the-wire type tag and is
  also what `preg_base_legacy.c` keys its `raw:` serialization on.
  Renaming the component would break both. Do not.
