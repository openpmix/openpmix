<!--
  Copyright (c) 2026      Nanook Consulting  All rights reserved.

  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
-->

# AGENTS.md: The PLOG `syslog` Component

`syslog` is the `plog` component that delivers log messages to the system
logger via the POSIX `syslog(3)` facility. Read the framework
[`AGENTS.md`](../AGENTS.md) first — this file only covers what is specific
to `syslog`. Compared to `stdfd`, this component adds configuration state
(a wrapping component struct with MCA parameters) and a
local-vs-global distinction.

## Files

| File | Contents |
|------|----------|
| `plog_syslog.h` | Declares `pmix_plog_syslog_component_t` (extends the base component with `console`, `level`, `facility`) and the module symbol. |
| `plog_syslog_component.c` | Component struct, MCA parameter registration, `component_query`. |
| `plog_syslog.c` | The module: `init`, `finalize`, `mylog`, and `write_local`. |
| `configure.m4` | Disables the component if `<syslog.h>` is not available. |

## Build gating (`configure.m4`)

`MCA_pmix_plog_syslog_CONFIG` probes for `<syslog.h>` with
`AC_CHECK_HEADER`; the component is built only if the header compiles.
On platforms lacking syslog it silently drops out. Because this is a
`configure`-time decision, **adding or changing this gating requires the
full `./autogen.pl && ./configure && make` cycle**, not a plain `make`.

## Component (`plog_syslog_component.c`)

### Extended component struct

`pmix_plog_syslog_component_t` embeds the base component as `super` and
adds three configured fields:

| Field | MCA param | Meaning |
|-------|-----------|---------|
| `console` | `plog_syslog_console` (bool) | Add `LOG_CONS` behavior — fall back to the console if the logger is unreachable. |
| `level` | `plog_syslog_level` (string) | Default syslog **severity** (`LOG_ERR`, `LOG_INFO`, …). |
| `facility` | `plog_syslog_facility` (string) | Syslog **facility** (`LOG_USER`, `LOG_DAEMON`, `LOG_AUTH`, `LOG_AUTHPRIV`). |

### Parameter parsing

`syslog_register` registers the three parameters and maps the
human-friendly strings onto the `LOG_*` constants from `<syslog.h>`:

- **level**: `err` → `LOG_ERR`, `alert`, `crit`, `emerg`, `warn` →
  `LOG_WARNING`, `not` → `LOG_NOTICE`, `info` (the registered default),
  `debug`/`dbg` → `LOG_DEBUG`. An unrecognized value emits the
  `syslog:unrec-level` `show_help` message and makes the registration
  return `PMIX_ERR_NOT_SUPPORTED`.
- **facility**: `auth` → `LOG_AUTH`, `priv` → `LOG_AUTHPRIV`, `daemon` →
  `LOG_DAEMON`, `user` → `LOG_USER` (default). Unrecognized values emit
  `syslog:unrec-facility` and return `PMIX_ERR_NOT_SUPPORTED`.

Note the struct's static initializer sets `level = LOG_ERR`, but the
registered string default is `"info"`, so after registration the
effective default level is `LOG_INFO`. Both `syslog:unrec-*` help texts
live in [`../base/help-pmix-plog.txt`](../base/help-pmix-plog.txt) — edits
there require regenerating the `show_help` content (`rm
src/util/pmix_show_help_content.*` then `make`).

### `component_query`

Unconditional: priority **10**, returns `pmix_plog_syslog_module`.
(The header-availability check already happened at configure time, so if
the component exists it can run.)

## Module (`plog_syslog.c`)

### Channels and lifecycle

`init` claims the channel set
`"lsys,gsys,syslog,local_syslog,global_syslog"` and opens the logger with
`openlog("PMIx Log Report:", LOG_CONS | LOG_PID, LOG_USER)`. `finalize`
calls `closelog()` and frees the channels argv. Those channel tokens map
to three attribute keys:

- `PMIX_LOG_SYSLOG` (`pmix.log.syslog`) — log to the local syslog
  (default behavior).
- `PMIX_LOG_LOCAL_SYSLOG` (`pmix.log.lsys`) — explicitly local syslog.
- `PMIX_LOG_GLOBAL_SYSLOG` (`pmix.log.gsys`) — the "global" / master
  syslog.

### `mylog` behavior

`mylog` reads two directives up front: `PMIX_LOG_SYSLOG_PRI` overrides the
severity for this call (default is the component's configured `level`),
and `PMIX_LOG_TIMESTAMP` supplies a `time_t` stamp. It then scans the
data items:

- `PMIX_LOG_SYSLOG` and `PMIX_LOG_LOCAL_SYSLOG` → `write_local(...)`.
- `PMIX_LOG_GLOBAL_SYSLOG` → **only** written if this process is a gateway
  server (`PMIX_PEER_IS_GATEWAY`); otherwise it is skipped (logged at
  verbosity, not delivered). The intent is that a "global" message is
  forwarded up to the gateway node and recorded in *that* node's syslog,
  so only the gateway actually emits it. Non-gateway peers silently
  decline. There is currently no separate forwarding transport here — the
  gateway simply writes locally.

### `write_local`

Formats and emits one record:

```c
syslog(severity, "%s %s:%s PROC %s REPORTS: %s",
       tod, my-id, sev2str(severity), source-id, msg);
```

`tod` is the human-readable timestamp (`ctime_r`, trailing newline
trimmed) when a timestamp was supplied, or `"N/A"`. `sev2str` maps the
numeric severity to a label for the message text. It always returns
`PMIX_SUCCESS`.

### Return code

On success `mylog` returns `PMIX_SUCCESS`. If a `write_local` fails it
returns that error immediately (aborting the router loop, per the
framework contract). If there is no data it returns
`PMIX_ERR_NOT_AVAILABLE`. Note that, unlike `stdfd`/`smtp`, this module
does *not* return `PMIX_ERR_TAKE_NEXT_OPTION` when its keys are simply
absent — it just falls through the scan and returns `PMIX_SUCCESS`
because `rc` from the last matched entry (or the initial state) is
returned. If you extend this module, prefer the explicit
"none-of-my-keys → `PMIX_ERR_TAKE_NEXT_OPTION`" convention so
`PMIX_LOG_ONCE` chaining stays correct.

## Gotchas

- **Severity vs. facility are different axes.** `level` is per-message
  severity (`LOG_ERR`…`LOG_DEBUG`); `facility` categorizes the source
  program (`LOG_USER`…). The current `openlog` hard-codes `LOG_USER`
  regardless of the configured `facility` field — the facility parameter
  is parsed and stored but not yet applied at `openlog` time. Keep that in
  mind before assuming the facility parameter changes routing.
- The global-syslog path depends entirely on the gateway check; on a
  non-gateway peer a `pmix.log.gsys` request produces no output by
  design. Don't "fix" that into an unconditional local write.
- `console` maps conceptually to `LOG_CONS`, which `init` already passes
  to `openlog`; the parameter exists mainly for callers/consumers that
  inspect it.
