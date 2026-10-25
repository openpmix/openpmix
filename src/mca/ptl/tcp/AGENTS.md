<!--
  Copyright (c) 2026      Nanook Consulting  All rights reserved.

  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
-->

# AGENTS.md: The PTL `tcp` Transport

`tcp` is a `ptl` transport: the code that depends on the socket being a
TCP one. Read the framework [`AGENTS.md`](../AGENTS.md) and
[`base/AGENTS.md`](../base/AGENTS.md) first - the base owns everything
done on a socket once it exists, and this component only makes sockets
and turns its URIs into addresses.

## Files

| File | Contents |
|------|----------|
| `ptl_tcp.h` | The component struct (with the primary listener's address) and `pmix_ptl_tcp_split_and_resolve`. |
| `ptl_tcp_component.c` | Component struct + `component_query` (always available, priority 30). |
| `ptl_tcp.c` | The module: `tcp_setup_listener`, `uri_to_addr`, and the interface helpers. |
| `configure.m4` | Makes the component static - always part of the library, never a plugin. |

## What it does

- **`setup_listener`** acts on the directives that are TCP's -
  `PMIX_TCP_IF_INCLUDE`/`EXCLUDE`, `PMIX_TCP_IPV4_PORT`/`IPV6_PORT`,
  `PMIX_TCP_DISABLE_IPV4`/`IPV6` - picks the interface (the first
  loopback device the directives leave, or none: then it reports
  `no-loopback-interfaces` and fails, and never takes a public one),
  tries each port, binds, listens, and makes the URI
  `<nspace>.<rank>;tcp4://host:port` (or `tcp6://[addr]:port`). The base
  stores the URI, reports it, and writes the rendezvous files. The
  transport serves peers of earlier releases on this host, and nothing
  else.
- **`uri_to_addr`** turns `tcp4://` and `tcp6://` URIs into a
  `sockaddr_storage`, refusing a malformed address or a port that is not
  1-65535. Any other scheme gets `PMIX_ERR_NOT_SUPPORTED`, so the base asks
  the next transport. Callers use `pmix_ptl_base_setup_connection`, never
  this directly.

## An interface list is used as given, or not at all

`pmix_ptl_tcp_split_and_resolve` turns an include or exclude list into
interface names. An entry that is neither a name nor a subnet refuses the
list, and an include list none of whose entries is on this host is
refused too: either used to be dropped, and an include list with nothing
left read as no list, so the listener opened on a public interface it was
told to avoid. A subnet no interface here is on is passed over, since one
list can name the subnets of several hosts.

## Its parameters are the base's

The interface, port and address-family MCA parameters are registered by
the base (`ptl_base_frame.c`) under their `pmix_ptl_base_*` names, and this
component reads them from `pmix_ptl_base`. It cannot register them: their
`pmix_ptl_tcp_*` names already exist, as deprecated synonyms of the base
ones. The same goes for `pmix_ptl_base_set_ports`, which parses the port
parameters.

## Gotchas

- **Never put a second address in the primary URI.** Released parsers
  take exactly one `host:port` after the `;`.
- `test/unit/ptl_uri.c` and `test/unit/ptl_listener.c` reach into this
  component by name (`pmix_ptl_tcp_split_and_resolve`,
  `pmix_mca_ptl_tcp_component.connection`). That is why it must stay
  static.
