<!--
  Copyright (c) 2026      Nanook Consulting  All rights reserved.

  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
-->

# AGENTS.md: The PREG `raw` Component

`raw` is the `preg` component that performs **no compression at all**: it
wraps the input in a `raw:` tag on the way out and strips it on the way
back. Read the framework [`AGENTS.md`](../AGENTS.md) first; this file
covers only what is specific to `raw`. It is the simplest component and
the framework's guaranteed fallback encoder.

## Files

| File | Contents |
|------|----------|
| `preg_raw.h` | Declares the component and module symbols. |
| `preg_raw_component.c` | Component struct + `component_query` (priority **50**, always available). |
| `preg_raw.c` | The module: all legacy string functions **and** both regex2 functions. |

`raw` is the one component that implements the **complete** module — the
full legacy string API *and* the regex2 `generate_regex`/`parse_regex`
pair. That completeness is deliberate: `raw` is the safety net that
guarantees *some* module can always encode and decode a request.

## What it does

Every operation is a tag manipulation, no real work:

- **`generate_node_regex` / `generate_ppn`** — if the input already starts
  with `raw:`, `strdup` it; otherwise emit `raw:<input>`. They **always
  return `PMIX_SUCCESS`** — `raw` never declines to encode.
- **`parse_nodes`** — require the `raw:` prefix, then
  `PMIx_Argv_split(&regex[4], ',')`.
- **`parse_procs`** — require the `raw:` prefix, then
  `PMIx_Argv_split(&regex[4], ';')`.
- **`copy` / `pack` / `unpack` / `release`** — the standard tag-gated
  `strdup` / `memcpy`-into-buffer / pointer-advance / `free`, identical in
  shape to `native`'s (the `raw:` string has no embedded NULs, so the
  simple string routines are correct).

Every parse/copy/pack/unpack/release returns `PMIX_ERR_TAKE_NEXT_OPTION`
when the value is not tagged `raw:`, so the base moves on to another
scheme.

### regex2 functions

- **`generate_regex`** — sets `regex->type = "raw"`,
  `regex->bytes = strdup(input)`, `regex->len = strlen(input) + 1`. It
  always succeeds (barring `PMIX_ERR_NOMEM`), so in the framework's
  **smallest-wins** regex2 contest `raw` is the baseline every other
  component must beat. For inputs too small or too random for `compress`
  to shrink, `raw` legitimately wins.
- **`parse_regex`** — gates on `regex->type == "raw"` and returns
  `strdup(regex->bytes)`. Because `raw`'s bytes are just the original NUL-
  terminated node string, no expansion is needed.

Both currently ignore their `info`/`ninfo` attribute arrays
(`PMIX_HIDE_UNUSED_PARAMS`); the parameters exist only to satisfy the
"every API carries attributes" rule.

## Why `raw` matters despite doing nothing

1. **It is the encoder of last resort.** Because its generators always
   succeed and it sits at priority 50 (above `native`'s 30), the legacy
   `generate_node_regex` path is claimed by `raw` whenever `compress` is
   absent — the data still ships, just uncompressed. Removing `raw` would
   make `native`'s generator reachable again but would also remove the
   guaranteed-success encoder, so the base's `strdup` fallback would carry
   more traffic.
2. **It is the regex2 baseline.** `generate_regex`'s smallest-wins logic
   needs at least one always-succeeding candidate or it would return
   `PMIX_ERR_NOT_SUPPORTED` for incompressible input. `raw` is that
   candidate.
3. **It round-trips anything.** Since it does not interpret the payload at
   all, `raw` can carry a list that no range compressor understands.

## Gotchas

- Do not add "cleverness" to `raw`. Its whole value is that it is a
  transparent, always-available identity transform. If you want smarter
  encoding, add a new component with a new tag.
- The `raw:` prefix is exactly 4 bytes and all gates use
  `strncmp(x, "raw:", 4)`. Keep the tag and the offset (`&regex[4]`) in
  sync if you ever touch it — but you should not need to.
