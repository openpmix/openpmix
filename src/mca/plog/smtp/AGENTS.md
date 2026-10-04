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
| `plog_smtp_to` | `to` | (none — required) |
| `plog_smtp_from_addr` | `from_addr` | (none — required) |
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

### `component_query` checks the configuration and resolves the server

Unlike the other components, `smtp`'s query does real work. It first
requires both `plog_smtp_to` and `plog_smtp_from_addr` to be set and
non-empty — every email goes from the one and to addresses picked from
the other, so without both the component has nothing it can send and
declines selection. It then resolves
`component.server` with `getaddrinfo` and, if the name does not resolve,
**disables the component** (`*priority = 0; *module = NULL; return
PMIX_ERR_NOT_FOUND`). This front-loads the failure so the module never
tries to talk to an unresolvable server later. On success it returns
the `plog_smtp_priority` value (default **10**) and the module. This is the canonical example of a `plog`
component that opts out at query time based on the runtime environment.

The addresses themselves are not kept — libesmtp resolves the name again
when it connects, so this is purely a reachability probe. It used to be
`gethostbyname`, whose result was cached in a `server_hostent` field
nothing ever read; that call is obsolescent, is not required to be
reentrant, and answers only for IPv4, so an IPv6-only mail server looked
unreachable and silently disabled the component.

## Module (`plog_smtp.c`)

### Channel

`init` claims the single channel `"email"`; `finalize` frees it. The one
attribute key it handles is `PMIX_LOG_EMAIL` (`pmix.log.email`).

### `mylog` — a nested attribute array

`PMIX_LOG_EMAIL`'s value is **not** a string; it is a
`pmix_data_array_t` of further `pmix_info_t` entries. `mylog`:

1. Scans the top-level `data` for `PMIX_LOG_EMAIL` (skipping entries
   already marked complete, rejecting more than one per call with
   `PMIX_ERR_BAD_PARAM`), and picks up the nested `input[]` array — after
   confirming the value really is a `PMIX_DATA_ARRAY` of `PMIX_INFO`.
   That check is not paranoia: the type is whatever the client that sent
   the `PMIx_Log` packed (see the framework `AGENTS.md`), and this one is
   dereferenced twice before it is walked.
2. Reads `PMIX_LOG_TIMESTAMP` from the directives.
3. If no email item is present, returns `PMIX_ERR_TAKE_NEXT_OPTION` —
   correctly deferring to other modules.
4. Walks the nested array for:
   - `PMIX_LOG_EMAIL_ADDR` → recipient list (comma-delimited).
   - `PMIX_LOG_EMAIL_SUBJECT` → subject.
   - `PMIX_LOG_EMAIL_MSG` (the key `PMIx_Log(3)` documents) or
     `PMIX_LOG_MSG` → the body, accepted as either a `PMIX_STRING` or a
     `PMIX_BYTE_OBJECT` (more than one message is rejected with
     `PMIX_ERR_NOT_SUPPORTED`). A byte object carries a length, not a
     terminator, and everything downstream of here is `strlen`-based, so
     it is copied into a NUL-terminated scratch buffer first.
5. If no message body was found, returns `PMIX_ERR_TAKE_NEXT_OPTION`.
6. Requires `PMIX_LOG_EMAIL_ADDR` (`PMIX_ERR_BAD_PARAM` without it) and
   hands it to `select_recipients`, then calls `send_email(...)` and
   returns its status.

### Who the email goes to and comes from

The site's MCA parameters decide both; the request only chooses among
what they allow.

- **Recipients.** `plog_smtp_to` is the list of addresses a request may
  name. `select_recipients` splits both lists, trims white space, and
  requires every requested address to match a configured one
  (`strcasecmp`). One miss fails the whole request with
  `PMIX_ERR_NO_PERMISSIONS` and nothing is sent. What it returns is the
  *configured* spelling of each match, de-duplicated — so the strings that
  reach `smtp_add_recipient` (and the generated `To:` header) are always
  the site's own, never the request's.
- **Sender.** The envelope sender and the `From:` address are both
  `plog_smtp_from_addr`. `PMIX_LOG_EMAIL_SENDER_ADDR` is not read. The
  `From:` phrase is `plog_smtp_from_name` followed by the requester's
  `nspace:rank`, taken from `mylog`'s `source` — on a gateway server that
  is the name bound to the client's connection (`pmix_server_log`), and on
  a host relay it is the `PMIX_LOG_SOURCE` the relaying server appended.
- **Server.** `plog_smtp_server` / `plog_smtp_port`.
  `PMIX_LOG_EMAIL_SERVER` / `PMIX_LOG_EMAIL_SRVR_PORT` are not read.

libesmtp copies every string it is given into the SMTP dialogue or the
header block as-is — it does no CR/LF screening of its own. Hence:

- the subject and the `From:` phrase go through `header_text`, which turns
  every control character into a space (and, for the quoted phrase, a
  `"` or `\` into `'`);
- `crnl` turns *every* line break in the body — lone CR, lone LF, CRLF —
  into CRLF. libesmtp splits the body into lines on CRLF only and
  dot-stuffs each line that starts with `.`; a lone CR left in place would
  be a line break that libesmtp does not see.

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

  Two things about this callback are easy to get wrong, and both were:

  - **`send_email` must initialize its `message_status_t` before
    registering it.** The very first call reads `sent_flag`, `free()`s
    `prev_string`, and renders `msg` — all of which were whatever was on
    the stack. It is a local, so nothing else will do it.
  - **Returning `NULL` ends the message**, as far as libesmtp is
    concerned. A state that has nothing to contribute must therefore fall
    through to the next one rather than return `NULL`; the prefix state
    used to stop the message dead when `body_prefix` was unset, sending
    an empty email.

  The callback returns pointers to its own storage and never uses the
  `**buf` scratch parameter libesmtp offers, so it does not allocate one.

### Return code

`mylog` returns `PMIX_ERR_TAKE_NEXT_OPTION` when there is no email request
or no body (proper deference), `PMIX_SUCCESS` on a sent message, or an
error from `send_email` / a bad request. This follows the framework's
return-code contract correctly — a good template to copy.

## Gotchas

- **One email per call, one body per email.** Both are hard limits
  enforced with `PMIX_ERROR_LOG` + error return; don't relax them without
  reworking `send_email`, which assumes a single message.
- **`send_email` owns `str`, `phrase` and `subj`**, and the single
  `error` cleanup path frees all three. `str` holds `pmix_asprintf`
  results and is set back to `NULL` once freed, so a later failure does
  not free it twice. Keep that discipline if you add another header.
- **The recipient argv belongs to `mylog`**, which gets it from
  `select_recipients` and frees it after `send_email` returns.
- The component is entirely absent from the build when libesmtp is not
  installed — never assume its symbols exist elsewhere in `libpmix`.
