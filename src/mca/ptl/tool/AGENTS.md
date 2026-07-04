<!--
  Copyright (c) 2026      Nanook Consulting  All rights reserved.

  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
-->

# AGENTS.md: The PTL `tool` Component

`tool` is the `ptl` component selected by **any tool** process — a
debugger, profiler, launcher, or client-tool that initializes via
`PMIx_tool_init`. Read the framework [`AGENTS.md`](../AGENTS.md) first;
this file covers only what is specific to `tool`. It is the broadest of
the three components (it claims launchers and client-tools as well as
plain tools) and the only one that adds a small wrapper of its own around
the base.

## Files

| File | Contents |
|------|----------|
| `ptl_tool.h` | Declares the component and module symbols. |
| `ptl_tool_component.c` | Component struct + `component_query` (priority **40**). |
| `ptl_tool.c` | The module + a local `setup_listener` wrapper. |

```c
pmix_ptl_module_t pmix_ptl_tool_module = {
    .name = "tool",
    .connect_to_peer = pmix_ptl_base_connect_to_peer,
    .setup_fork      = pmix_ptl_base_setup_fork,
    .setup_listener  = setup_listener   // local wrapper, see below
};
```

## When it is selected

`component_query` returns the module whenever

```c
PMIX_PEER_IS_TOOL(pmix_globals.mypeer)
```

That single condition is deliberately broad. Because `PMIX_PROC_LAUNCHER`
and `PMIX_PROC_CLIENT_TOOL` both include the `PMIX_PROC_TOOL` bit, this
component also serves:

- **Launchers** (`tool+server`) — which is why the module carries
  `setup_listener`: a launcher must *listen* for the children it spawns
  while also *connecting up* to its own server.
- **Client-tools** (`tool+client`) — a process that is both a registered
  client and a tool.

The pure-[`client`](../client/AGENTS.md) and pure-[`server`](../server/AGENTS.md)
gates explicitly exclude tools, so there is never a selection conflict.

## The `connect_to_peer` path

`tool` uses the base's full **discovery matrix**
(`pmix_ptl_base_connect_to_peer`), not the client's short path. A tool may
be trying to reach any of several servers and must search for the right
one. The base honors, roughly in this order of specificity:

- an explicit `PMIX_SERVER_URI` / `PMIX_TCP_URI`;
- a rendezvous / `PMIX_TOOL_ATTACHMENT_FILE`;
- a caller-supplied **connection order**
  (`PMIX_CONNECT_SYSTEM_FIRST`, `PMIX_CONNECT_TO_SYSTEM`,
  `PMIX_CONNECT_TO_SCHEDULER`, `PMIX_CONNECT_TO_SYS_CONTROLLER`), each of
  which maps to a well-known rendezvous filename in the system tmpdir;
- a specific server **PID** (`pmix.<host>.tool.<pid>`) or **nspace**
  (`pmix.<host>.tool.<nspace>`);
- otherwise a directory search of the session tmpdir for any
  `pmix.<host>.tool*` server, taking the first that connects.

`PMIX_TOOL_CONNECT_OPTIONAL` controls whether failure to find a server is
fatal or simply leaves the tool unconnected. All of this lives in
`base/ptl_base_connect.c`; the component only selects it.

## The local `setup_listener` wrapper

This is the one piece of genuinely component-specific code in `ptl`. It
calls `pmix_ptl_base_setup_listener` (identical to what the `server`
component uses) and then adds a cleanup step:

```c
rc = pmix_ptl_base_setup_listener(info, ninfo);
...
if (connected) {
    // register our nspace/session rendezvous files with the server
    // for cleanup via PMIx_Job_control_nb(PMIX_REGISTER_CLEANUP, ...)
}
```

If the tool is connected to a server, it hands that server the paths of
the rendezvous files it just created (`nspace_filename`,
`session_filename`) via `PMIx_Job_control_nb` with `PMIX_REGISTER_CLEANUP`.
That way, if the tool dies abruptly, the **server** removes the tool's
stale rendezvous files — a pure server has no equivalent need because it
removes its own files directly in `pmix_ptl_close`.

## Why a tool both listens and connects

A plain debugger only connects up to a server and would not strictly need
`setup_listener`. But a **launcher** — the dominant tool case — is
simultaneously a server to the processes it starts. Rather than split
tools into "listening" and "non-listening" variants, the single `tool`
component always wires `setup_listener`; a tool that never spawns children
simply never has anyone connect to its listener. Keep this in mind before
"optimizing away" the listener for tools.

## Gotchas

- **The cleanup registration is guarded by `pmix_globals.connected`.** A
  tool that set up its listener *before* connecting to a server will not
  register its files for remote cleanup. That ordering is intentional
  (you cannot ask a server to clean up for you before you have a server),
  but it means an unconnected tool relies on its own `pmix_ptl_close` to
  remove its files.
- Like the other components, `tool` contains no transport code. The
  handshake it performs when connecting (as a `PMIX_TOOL_*` flag value)
  and the server-side handling of a tool connection both live in the
  base — `ptl_base_fns.c` (`pmix_ptl_base_tool_handshake`,
  `pmix_ptl_base_set_flag`) and `ptl_base_connection_hdlr.c`
  (`process_tool_request` / `process_cbfunc`).
