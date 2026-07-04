<!--
  Copyright (c) 2026      Nanook Consulting  All rights reserved.

  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
-->

# AGENTS.md: The PLOG `smtp` Component

`smtp` is the `plog` component that delivers a log request as an **email**,
using the third-party **libesmtp** library. Read the framework
[`AGENTS.md`](../AGENTS.md) first — this file only covers what is specific
to `smtp`. It is the most involved of the three components: it has an
external dependency, a rich configuration struct, a streaming message
callback, and it handles a *nested* attribute array rather than a flat key.

## Files

| File | Contents |
|------|----------|
| `plog_smtp.h` | `pmix_plog_smtp_component_t` (server/port, to/from/subject, body prefix+suffix, resolved `hostent`, priority) and the module symbol. Includes `<libesmtp.h>`. |
| `plog_smtp_component.c` | Component struct, MCA parameter registration, `component_query` (resolves the SMTP server), `smtp_close`. |
| `plog_smtp.c` | The module: `init`, `finalize`, `mylog`, `send_email`, and the libesmtp callback machinery. |
| `configure.m4` | Finds libesmtp; the component is built only if it is present. |

## Build gating (`configure.m4`)

`MCA_pmix_plog_smtp_CONFIG` uses `OAC_CHECK_PACKAGE` to locate
`libesmtp.h` and the `esmtp` library (honoring `--with-smtp[-libdir]`).
If the user explicitly requested SMTP and it is not found, configure
**errors out**; otherwise the component silently disables. The resolved
flags feed `PMIX_EMBEDDED_{LIBS,LDFLAGS,CPPFLAGS}`. Because this is
configure-time logic, changing it requires the full `./autogen.pl &&
./configure && make` cycle. libesmtp is the only `plog` component with a
real third-party link dependency — keep that in mind for portability:
code here must not be reachable when the component is not built.

## Component (`plog_smtp_component.c`)

### Configuration struct and parameters

`pmix_plog_smtp_component_t` (in `plog_smtp.h`) embeds the base component
as `super` and holds all the SMTP/email configuration. `smtp_register`
registers these MCA parameters:

| Param | Field | Default |
|-------|-------|---------|
| `plog_smtp_server` | `server` | `"localhost"` |
| `plog_smtp_port` | `port` | `25` |
| `plog_smtp_to` | `to` | (none) |
| `plog_smtp_from_addr` | `from_addr` | (none) |
| `plog_smtp_from_name` | `from_name` | `"PMIx Plog"` |
| `plog_smtp_subject` | `subject` | `"PMIx Plog"` |
| `plog_smtp_body_prefix` | `body_prefix` | boilerplate intro text |
| `plog_smtp_body_suffix` | `body_suffix` | `"…Sincerely,\nOscar the PMIx Owl"` |
| `plog_smtp_priority` | `priority` | `10` |

It also stashes the libesmtp version string via `smtp_version()`.
`smtp_close` frees the heap-allocated strings.

> **Historical note.** Through mid-2026 the body *suffix* parameter was
> mistakenly registered under the name `"body_prefix"` a second time,
> making two MCA variables share a name and leaving the suffix unsettable
> under its own name. This has been fixed to `"body_suffix"` (the name
> shown above). If you are chasing a report against an older release where
> `plog_smtp_body_suffix` "does nothing," that is the cause.

### `component_query` resolves the server

Unlike the other components, `smtp`'s query does real work: it calls
`gethostbyname(component.server)` and, if the name does not resolve,
**disables the component** (`*priority = 0; *module = NULL; return
PMIX_ERR_NOT_FOUND`). This front-loads the failure so the module never
tries to talk to an unresolvable server later. On success it returns
priority **10** and the module. (The resolved `hostent` is cached in
`server_hostent`.) This is the canonical example of a `plog` component
that opts out at query time based on the runtime environment.

## Module (`plog_smtp.c`)

### Channel

`init` claims the single channel `"email"`; `finalize` frees it. The one
attribute key it handles is `PMIX_LOG_EMAIL` (`pmix.log.email`).

### `mylog` — a nested attribute array

`PMIX_LOG_EMAIL`'s value is **not** a string; it is a
`pmix_data_array_t` of further `pmix_info_t` entries. `mylog`:

1. Scans the top-level `data` for `PMIX_LOG_EMAIL` (rejecting more than
   one per call with `PMIX_ERR_BAD_PARAM`), and picks up the nested
   `input[]` array.
2. Reads `PMIX_LOG_TIMESTAMP` from the directives.
3. If no email item is present, returns `PMIX_ERR_TAKE_NEXT_OPTION` —
   correctly deferring to other modules.
4. Walks the nested array for:
   - `PMIX_LOG_EMAIL_ADDR` → recipient list (comma-delimited).
   - `PMIX_LOG_EMAIL_SENDER_ADDR` → sender ("from").
   - `PMIX_LOG_EMAIL_SUBJECT` → subject.
   - `PMIX_LOG_MSG` → the body, accepted as either a `PMIX_STRING` or a
     `PMIX_BYTE_OBJECT` (more than one message is rejected with
     `PMIX_ERR_NOT_SUPPORTED`).
5. If no message body was found, returns `PMIX_ERR_TAKE_NEXT_OPTION`;
   otherwise calls `send_email(...)` and returns its status.

Recipient/sender fall back to the component defaults (`to`, `from_addr`)
when the request omits them; `send_email` returns `PMIX_ERR_BAD_PARAM` if
even the defaults are absent.

### `send_email` and the libesmtp flow

`send_email` drives libesmtp: create session, add message, set server
(`"server:port"`), reverse path, `To`/`Subject`/`X-Mailer`/`From`/optional
`Timestamp` headers, register the message-body callback, then
`smtp_start_session`. Points worth knowing:

- **SIGPIPE is temporarily ignored** around the network I/O (saved and
  restored via `sigaction`) so a remote server hangup cannot kill the
  whole process. Preserve this if you refactor.
- **Error handling is a single `goto error` cleanup path** that destroys
  the session, restores SIGPIPE, and — on failure — emits the
  `smtp:send_email failed` `show_help` message (text in
  [`../base/help-pmix-plog.txt`](../base/help-pmix-plog.txt); regenerate
  the `show_help` content after editing it).
- **The message body is streamed via `message_cb`**, a state machine
  (`SENT_NONE → SENT_HEADER → SENT_BODY_PREFIX → SENT_BODY →
  SENT_BODY_SUFFIX → SENT_ALL`) that libesmtp pumps for successive chunks.
  `crnl()` converts lone `\n` to `\r\n` as SMTP requires. The
  configurable `body_prefix` / `body_suffix` bracket the caller's message.

### Return code

`mylog` returns `PMIX_ERR_TAKE_NEXT_OPTION` when there is no email request
or no body (proper deference), `PMIX_SUCCESS` on a sent message, or an
error from `send_email` / a bad request. This follows the framework's
return-code contract correctly — a good template to copy.

## Gotchas

- **`from_addr` vs. `from_name` vs. request sender.** The SMTP reverse
  path uses `c->from_addr`; the `From` header uses `from_name` (display)
  with the resolved `myfrom` address. A request's
  `PMIX_LOG_EMAIL_SENDER_ADDR` overrides the address but not the display
  name. If mail shows the wrong sender, this three-way interaction is
  where to look.
- **One email per call, one body per email.** Both are hard limits
  enforced with `PMIX_ERROR_LOG` + error return; don't relax them without
  reworking `send_email`, which assumes a single message.
- **`asprintf` vs. `pmix_asprintf`.** `send_email` uses the bare
  `asprintf` in one spot (the `X-Mailer` header) and `pmix_asprintf`
  elsewhere. Prefer the `pmix_`-prefixed portable wrappers for any new
  code here.
- The component is entirely absent from the build when libesmtp is not
  installed — never assume its symbols exist elsewhere in `libpmix`.
