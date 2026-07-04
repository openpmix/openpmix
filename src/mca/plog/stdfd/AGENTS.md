<!--
  Copyright (c) 2026      Nanook Consulting  All rights reserved.

  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
-->

# AGENTS.md: The PLOG `stdfd` Component

`stdfd` ("standard **f**ile **d**escriptors") is the `plog` component that
delivers log data to **stdout** and **stderr**. Read the framework
[`AGENTS.md`](../AGENTS.md) first — this file only covers what is specific
to `stdfd`. It is the simplest of the three components and the best
starting point for understanding the module contract.

## Files

| File | Contents |
|------|----------|
| `plog_stdfd.h` | Declares the component (`pmix_mca_plog_stdfd_component`) and module (`pmix_plog_stdfd_module`) symbols. |
| `plog_stdfd_component.c` | Component struct + `component_query`. |
| `plog_stdfd.c` | The module: `init`, `finalize`, and `mylog`. |

There is no `configure.m4` and no optional dependency — `stdfd` is always
built and always available.

## Component (`plog_stdfd_component.c`)

The component uses the bare `pmix_plog_base_component_t` (no extra
configuration state, so no wrapping struct and no
`register_component_params`). `component_query` is unconditional:

```c
*priority = 5;
*module = (pmix_mca_base_module_t *) &pmix_plog_stdfd_module;
return PMIX_SUCCESS;
```

Priority **5** is deliberately low — below `syslog` (10) and `smtp` (10) —
so that in unordered selection the more "notable" channels sort ahead of
plain stdout/stderr. `stdfd` never disables itself.

## Module (`plog_stdfd.c`)

### Channels

`init` claims two channels by splitting `"stdout,stderr"` into
`module->channels`; `finalize` frees that argv. So the router will list
this module for any data item whose key contains `stdout` or `stderr`.
The keys it actually handles are the attributes `PMIX_LOG_STDOUT`
(`pmix.log.stdout`) and `PMIX_LOG_STDERR` (`pmix.log.stderr`), each
carrying a `char*` string payload.

### `mylog` behavior

For each not-yet-complete data item, `mylog` matches `PMIX_LOG_STDERR` or
`PMIX_LOG_STDOUT` and then splits on the caller's role:

- **Client or tool** (`PMIX_PEER_IS_CLIENT` / `PMIX_PEER_IS_TOOL`): write
  the string straight to the local `stderr` / `stdout` with `fprintf`.
  There is nowhere to forward to — this process *is* the endpoint.
- **Server / daemon** (everything else): the output belongs to a process
  whose stdio the server is managing, so it is handed to the IOF
  (I/O forwarding) subsystem via
  `PMIx_server_IOF_deliver(&source, PMIX_FWD_STDOUT_CHANNEL |
  PMIX_FWD_STDERR_CHANNEL, ...)` so it reaches the correct output stream.

### The IOF hand-off and its scratch object

For the server path, `mylog` allocates a small
`pmix_iof_deliver_t` object (defined locally) that carries a copy of the
source `pmix_proc_t` and a `pmix_byte_object_t` holding the message bytes
(NUL terminator included). This is passed to the **non-blocking**
`PMIx_server_IOF_deliver`; the completion callback `lkcbfunc` simply
`PMIX_RELEASE`s the object. This is deliberate:

- The module must not block the progress thread waiting on IOF, so it
  uses the async form and lets the callback clean up.
- This one *does* copy the payload into the scratch object rather than
  aliasing the caller's `data`. That is a legitimate exception to the
  framework's no-copy rule: `mylog` returns immediately while IOF
  delivery completes later, so the caller's `data` array cannot be
  assumed live by then. (Contrast the caddy no-copy rule in the top-level
  guide, which relies on the caller keeping inputs valid until *their*
  callback fires — a guarantee that does not extend across this internal
  async boundary.)

### Return code

`mylog` starts `rc = PMIX_ERR_TAKE_NEXT_OPTION` and only sets success via
the IOF path. If none of the data keys are stdout/stderr it returns
`PMIX_ERR_TAKE_NEXT_OPTION`, correctly telling the router "not mine, try
the next module." If there is no data at all it returns
`PMIX_ERR_NOT_AVAILABLE`. Note it does **not** call
`PMIX_INFO_OP_COMPLETED` on the client/tool `fprintf` path — worth keeping
in mind if you extend it to cooperate with other modules on the same
array.

### Output-formatting directives

`mylog` honors three directives that decorate the emitted text:

- `PMIX_LOG_TAG_OUTPUT` — prefix each line with its channel tag
  (`[nspace,rank]<stdout>: `).
- `PMIX_LOG_TIMESTAMP_OUTPUT` — prefix with a timestamp.
- `PMIX_LOG_XML_OUTPUT` — wrap the line in an XML element
  (`<stdout .../>`), with the payload XML-escaped.

Rather than reimplement this formatting, `stdfd` maps those directives
onto a `pmix_iof_flags_t` and calls the shared IOF formatter
`pmix_iof_prep_output()` (in `src/common/pmix_iof.c`), so log output looks
identical to forwarded stdio. The formatter is only invoked when at least
one flag is set (`flags.set`); with no formatting directives the fast path
writes the raw string with a single `fprintf`, so the common case pays
nothing. Because `pmix_iof_prep_output` tags each line with the source
identity, `mylog` falls back to `pmix_globals.myid` when the caller
passes a `NULL` source.

Note a deliberate limitation: `pmix_iof_prep_output` generates the
timestamp at emission time (matching IOF), so the explicit
`PMIX_LOG_TIMESTAMP` *value* on the request is not used for the printed
stamp — unlike the `syslog`/`smtp` components, which do print the supplied
value. Reconciling that would mean teaching the shared formatter to accept
a caller-supplied timestamp; it is intentionally left as a central,
IOF-wide change rather than a `stdfd`-local divergence.

The formatted bytes are written directly on the client/tool path
(`fwrite`) and handed to IOF on the server path (the `pmix_iof_deliver_t`
takes ownership of the formatter's buffer). `pmix_iof_prep_output`
returns a plain `malloc`'d `pmix_byte_object_t`, so `stdfd` frees it with
`free()` (or steals its `bytes` pointer) rather than a public
byte-object API.

## Gotchas

- Because its priority is low and it claims only two specific channels,
  `stdfd` is easy to reason about — but it is often the *last* channel a
  `PMIX_LOG_ONCE` request reaches. If you are debugging "my stdout log
  never appears," check whether a higher-priority module claimed the
  request first.
- The role split (client/tool vs. server) is the subtle part. A change
  that makes a server `fprintf` directly, or a client call
  `PMIx_server_IOF_deliver` (which it has no server to service), will
  misbehave. Preserve the `PMIX_PEER_IS_CLIENT || PMIX_PEER_IS_TOOL`
  branch.
