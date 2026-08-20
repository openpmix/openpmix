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
`openlog("PMIx Log Report:", LOG_PID [| LOG_CONS], component.facility)`.
Both configured values reach `openlog` here — `LOG_CONS` only when the
`console` parameter asks for it, and the facility rather than a
hard-coded `LOG_USER`. `finalize` calls `closelog()` and frees the
channels argv.

`finalize` is not only called at framework close: a module the user's
`plog_base_order` did not ask for is finalized as it is discarded during
selection, so this pair has to be safe to run on a module that never
logged anything. Those channel tokens map
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
  so only the gateway actually emits it. There is currently no separate
  forwarding transport here — the gateway simply writes locally.

Entries already marked `PMIX_INFO_OP_IS_COMPLETE` are skipped, and each
entry this module delivers is marked with `PMIX_INFO_OP_COMPLETED`. An
entry whose value is not a `PMIX_STRING` is rejected with a
`PMIX_ERROR_LOG` and marked complete rather than handed to `syslog()`'s
`%s` — see the framework `AGENTS.md` on why that type is not trustworthy.
`PMIX_LOG_SYSLOG_PRI` is read with `PMIx_Value_get_number` for the same
reason, and `PMIX_LOG_TIMESTAMP` is only taken when it really is a
`PMIX_TIME`.

### `write_local`

Formats and emits one record:

```c
syslog(severity, "%s %s:%s PROC %s REPORTS: %s",
       tod, my-id, sev2str(severity), source-id, msg);
```

`tod` is the human-readable timestamp (`ctime_r`, trailing newline
trimmed) when a timestamp was supplied, or `"N/A"`. `ctime_r` is allowed
to decline a time it cannot represent, and it leaves the buffer
untouched when it does, so its return is checked before `tod` is indexed
into. `sev2str` maps the numeric severity to a label for the message
text. `write_local` always returns `PMIX_SUCCESS`.

`PMIX_NAME_PRINT(source)` is safe with a `NULL` source —
`pmix_util_print_name_args` answers `[NO-NAME]` for one — so the router
handing down a `NULL` source is not a hazard here.

### Return code

`mylog` counts the entries it actually wrote: `PMIX_SUCCESS` when that
count is non-zero, `PMIX_ERR_TAKE_NEXT_OPTION` when it is zero, and the
error from a failed `write_local` immediately (aborting the router loop,
per the framework contract). If there is no data it returns
`PMIX_ERR_NOT_AVAILABLE`.

This module used to return `PMIX_SUCCESS` unconditionally at the bottom
of the scan, which is the exact defect the framework `AGENTS.md` calls
the most common one here. The case that made it visible: a
`pmix.log.gsys` request on a **non-gateway** peer writes nothing by
design, but reported success — so the router told the caller the message
had been logged, the client never fell back to anyone who could log it,
and the message simply vanished.

## Gotchas

- **Severity vs. facility are different axes.** `level` is per-message
  severity (`LOG_ERR`…`LOG_DEBUG`); `facility` categorizes the source
  program (`LOG_USER`…). Both are applied — the facility at `openlog`
  time, the severity per message — but only the severity can be
  overridden per call (`PMIX_LOG_SYSLOG_PRI`). Changing the facility
  after `init` would require a fresh `openlog`.
- **`console` is a `bool`, not an `int`.** The MCA variable system
  writes a `PMIX_MCA_BASE_VAR_TYPE_BOOL` through a `bool *`. It was
  declared `int` here, which left the remaining bytes untouched and made
  which byte got written an endianness question.
- The global-syslog path depends entirely on the gateway check; on a
  non-gateway peer a `pmix.log.gsys` request produces no output by
  design. Don't "fix" that into an unconditional local write.
- `console` maps to `LOG_CONS`, and `init` passes it only when the
  parameter is set. It used to be passed unconditionally, so every
  message the system logger could not take was written to the system
  console of every node — on a large machine, whether the operator
  asked for that or not.
