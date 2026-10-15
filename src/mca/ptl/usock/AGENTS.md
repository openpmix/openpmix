<!--
  Copyright (c) 2026      Nanook Consulting  All rights reserved.

  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
-->

# AGENTS.md: The PTL `usock` Transport

`usock` is the `ptl` transport for processes on the server's own host: an
AF_UNIX socket whose far end the kernel identifies. Read the framework
[`AGENTS.md`](../AGENTS.md) and [`base/AGENTS.md`](../base/AGENTS.md)
first; the base owns everything done on a socket once it exists.

## Files

| File | Contents |
|------|----------|
| `ptl_usock.h` | The component struct (the directory and socket this process made) and the `usock://` scheme. |
| `ptl_usock_component.c` | Component struct + `component_query` (always available, priority 50). |
| `ptl_usock.c` | The module: `usock_setup_listener`, `uri_to_addr`, `peer_identity`, `usock_close`. |
| `configure.m4` | Makes the component static - always part of the library, never a plugin. |

## What it does

- **It is the local transport** (`.local = true`). The base opens its
  listener beside tcp's, as `pmix_ptl_base.local_listener`, and a server
  that cannot open it fails init: every process of this release on the
  host connects there.
- **`setup_listener`** makes a directory of its own with `mkdtemp`
  beneath the session tmpdir (0700, so nobody else can reach anything in
  it), binds `pmix-<pid>` inside it, and gives the socket its mode (0600
  unless `PMIX_SOCKET_MODE` says otherwise) and its owner and group
  (`PMIX_USERID`, `PMIX_GRPID` - a number or a name, resolved by
  `pmix_util_uid_from_value`/`gid_from_value` as everywhere else; the
  group defaults to our egid, not the directory's) while the directory is
  still closed. Only then is the directory opened to whom the mode lets
  connect - group or others get search permission, never read; a socket
  given to another user makes it searchable by others. The directory's
  owner never changes.
- **Every change in the directory goes through `dfd`**, opened
  `O_DIRECTORY|O_NOFOLLOW` right after `mkdtemp` and checked (ours, 0700).
  `bind` is the one call that takes a path, so the socket is then looked
  up through `dfd` and must be there. `close` unlinks through `dfd` too.
- The URI is `<nspace>.<rank>;usock://<path>`. A path too long for
  `sun_path`, or one holding a `;`, fails init with the `usock-listener`
  help topic.
- **`uri_to_addr`** takes only `usock://` URIs with an absolute path that
  fits; anything else is another transport's.
- **`peer_identity`** is what the kernel says of the far end: uid and gid
  (`pmix_util_getid`) and the pid where the platform gives one
  (`pmix_util_getpid`, else 0).
- **`close`** removes the socket and the directory this process made.

## What reads the kernel's answer

- psec/native takes uid and gid from `peer_identity` for any peer whose
  transport has one, in place of the TCP-table lookup.
- The connection handler gives the host the kernel's pid for a local peer,
  and no pid when the kernel gives none - never the pid the peer sent.

## Gotchas

- **Never widen the socket after the directory is opened.** The mode and
  owner are set while the directory is still 0700; that is what leaves no
  moment when the socket is reachable more widely than intended.
- **A server allowing foreign tools does not widen anything by itself.**
  Its host must pass the mode, owner or group that lets them in.
- `test/unit/ptl_usock.c` covers the listener; `test/unit/ptl_accept.c`
  covers the kernel's identity for a local client and tool.
