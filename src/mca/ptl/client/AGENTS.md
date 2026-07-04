<!--
  Copyright (c) 2026      Nanook Consulting  All rights reserved.

  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
-->

# AGENTS.md: The PTL `client` Component

`client` is the `ptl` component selected by a **pure client** process — a
process that was registered with a PMIx server before it started and
connects via `PMIx_Init`. Read the framework [`AGENTS.md`](../AGENTS.md)
first; this file covers only what is specific to `client`. It is the
simplest of the three components and the only one that supplies its *own*
`connect_to_peer` rather than reusing the base's discovery matrix.

## Files

| File | Contents |
|------|----------|
| `ptl_client.h` | Declares the component and module symbols. |
| `ptl_client_component.c` | Component struct + `component_query` (priority **50**). |
| `ptl_client.c` | The module: a single `connect_to_peer` implementation. |

## When it is selected

`component_query` returns the module only when the local peer

```c
PMIX_PEER_IS_CLIENT(pmix_globals.mypeer) && !PMIX_PEER_IS_TOOL(pmix_globals.mypeer)
```

i.e. a client that is *not also* a tool. A client-tool or launcher is a
tool for selection purposes and takes the [`tool`](../tool/AGENTS.md)
component instead. Because this gate is mutually exclusive with the tool
and server gates, the priority value (50) never actually arbitrates a tie
— see the framework doc.

## The module

```c
pmix_ptl_module_t pmix_ptl_client_module = {
    .name = "client",
    .connect_to_peer = connect_to_peer
};
```

Everything else is `NULL`. A client never listens for connections, never
forks PMIx children of its own, and needs no `init`/`finalize` — it only
ever connects *up* to the one server that started it.

## What `connect_to_peer` does

This is the **short** connection path, in contrast to the full discovery
matrix in `pmix_ptl_base_connect_to_peer` used by tools and servers. A
client already knows (or can trivially find) its server, so the logic is:

1. **Look for an explicit `PMIX_SERVER_URI`** in the passed info array. If
   present, split the version prefix from the URI, and call
   `pmix_ptl_base_set_peer` on each `:`-separated candidate to select the
   matching `bfrops` module and record the server's version.
2. **Collect our identity** (`PMIX_PROC_PID`, `PMIX_CMD_LINE`) into an
   info array to hand the server during the handshake.
3. **If no URI was given, check the environment** via
   `pmix_ptl_base_set_peer(peer, &evar)` — which walks the active
   `bfrops` modules in priority order and, for each, looks for the
   corresponding `PMIX_SERVER_URIvNN` env var. Finding one both yields the
   URI *and* tells us the server's version.
4. **Singleton fallback.** If neither a URI nor an env var is found, this
   process was not launched under a PMIx server. Mark ourselves a
   `PMIX_PROC_SINGLETON`, assign default `bfrops` modules, and look for a
   *system* server via the `pmix.sys.<host>` rendezvous file in the system
   tmpdir. If that connects, great; otherwise return `PMIX_ERR_UNREACH`
   and the library proceeds as a standalone singleton.
5. **Connect.** With a URI in hand, `pmix_ptl_base_parse_uri` splits it
   into `nspace` / `rank` / `suri`, and `pmix_ptl_base_make_connection`
   opens the socket and runs the handshake. `pmix_ptl_base_complete_connection`
   then records the server and arms the steady-state events.

## Why the client path is separate

The base `connect_to_peer` exists to serve *tools*, which may connect to
any of several servers (system, scheduler, controller, a specific PID or
nspace, an arbitrary URI) and must search for them. A client has exactly
one server — the one that registered and launched it — so folding it into
the tool matrix would add branches that can never fire for a client and
would blur the singleton-detection logic. Keeping the client path small
and separate is deliberate; resist the urge to "unify" the two.

## Gotchas

- **The singleton decision lives here.** A client that finds no server is
  how PMIx recognizes a singleton. Do not change the fallback to return an
  error before the system-server probe, or singletons that *do* have a
  system server will fail to connect.
- **Version detection is a side effect of URI discovery.**
  `pmix_ptl_base_set_peer` couples "which env var is set" to "which
  `bfrops`/version the server speaks." If you refactor URI lookup, keep
  that coupling — the client otherwise has no other signal for the
  server's version at connect time.
- The heavy lifting (socket, handshake, framing) is all in the base; this
  file only decides *where* to connect. Bug reports about the wire
  protocol belong in `base/`, not here.
