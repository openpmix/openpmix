<!--
  Copyright (c) 2026      Nanook Consulting  All rights reserved.

  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
-->

# AGENTS.md: The PTL `server` Component

`server` is the `ptl` component selected by a **pure server** process —
one that hosts the PMIx server library for a resource manager or launcher
and is *not* itself a tool. Read the framework [`AGENTS.md`](../AGENTS.md)
first; this file covers only what is specific to `server`. It is a pure
pass-through to the base: it adds no code of its own, only wiring.

## Files

| File | Contents |
|------|----------|
| `ptl_server.h` | Declares the component and module symbols. |
| `ptl_server_component.c` | Component struct + `component_query` (priority **30**). |
| `ptl_server.c` | The module — three base function pointers, nothing else. |

Note `ptl_server.c` has no functions of its own at all:

```c
pmix_ptl_module_t pmix_ptl_server_module = {
    .name = "server",
    .connect_to_peer = pmix_ptl_base_connect_to_peer,
    .setup_fork      = pmix_ptl_base_setup_fork,
    .setup_listener  = pmix_ptl_base_setup_listener
};
```

## When it is selected

`component_query` returns the module only when the local peer

```c
PMIX_PEER_IS_SERVER(pmix_globals.mypeer) && !PMIX_PEER_IS_TOOL(pmix_globals.mypeer)
```

The `!PMIX_PEER_IS_TOOL` clause is what makes this the *pure*-server
component: a **launcher** is `tool+server` and therefore matches the
[`tool`](../tool/AGENTS.md) gate, not this one. So the `server` component
runs in exactly the classic case — an RM daemon (e.g. PRRTE's `prted`,
Slurm's plugin host) that accepts client and tool connections but never
connects *as a tool* to anything above it.

## What each wired function does

All three point straight at the base; see the framework doc and the base
sources for detail.

- **`setup_listener` → `pmix_ptl_base_setup_listener`**: the server's main
  job. Selects an interface (loopback unless remote/tool connections were
  requested), binds a listen socket, publishes its URI into `gds`, and
  drops the rendezvous files (`pmix.sys.<host>`, `pmix.<host>.tool*`, and
  — if this server is a scheduler or system controller — the
  correspondingly named files) that let clients and tools find it. The
  accept event is then armed by `pmix_ptl_base_start_listening`, and every
  inbound connection is handled by
  `base/ptl_base_connection_hdlr.c::pmix_ptl_base_connection_handler`.
- **`setup_fork` → `pmix_ptl_base_setup_fork`**: injects
  `PMIX_SERVER_TMPDIR` and `PMIX_SYSTEM_TMPDIR` into the environment of
  child processes the server launches, so those children's `ptl` layer can
  find the same rendezvous directories.
- **`connect_to_peer` → `pmix_ptl_base_connect_to_peer`**: present so a
  server can, when needed, connect *up* to another server (e.g. a system
  server or scheduler) using the full discovery matrix. In the common case
  a pure server only *accepts* connections and never uses this.

## Why there is nothing else here

The entire server-side protocol — accepting sockets, parsing the
handshake, validating credentials, assigning compat modules, calling the
host's `client_connected2` / `tool_connected` upcalls — lives in
`base/ptl_base_connection_hdlr.c`, not in this component. The `server`
component is just the role marker that says "this process should listen."
If you are debugging how the server treats an incoming connection, that
file is where to look; changes here would only ever be to the three
pointers above.

## Contrast with the `tool` component

The [`tool`](../tool/AGENTS.md) module wires the *same* three base
functions but wraps `setup_listener` in an extra step that registers the
tool's own rendezvous files for cleanup via `PMIx_Job_control_nb`. A pure
server does not need that wrapper (its files are removed directly in
`pmix_ptl_close`), which is the only functional difference between the two
modules' listener setup.
