<!--
  Copyright (c) 2026      Nanook Consulting  All rights reserved.

  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
-->

# AGENTS.md: The PIF `bsdx_ipv4` Component

`bsdx_ipv4` discovers the local node's **IPv4** interfaces on BSD-family
systems (including macOS) using `getifaddrs(3)`. Read the framework
[`AGENTS.md`](../AGENTS.md) first; this file covers only what is specific
to `bsdx_ipv4`. Like every `pif` component it has no module and no
priority — it is a bare component whose `open` function does all the work
by appending `pmix_pif_t` entries to the shared `pmix_if_list`.

## Files

| File | Contents |
|------|----------|
| `pif_bsdx.c` | Component struct + the `if_bsdx_open` discovery routine. |
| `configure.m4` | Platform gate (static-only) and BSD/Apple detection. |
| `Makefile.am` | Builds `libpmix_mca_pif_bsdx_ipv4.la`. |

## When it is compiled in

`configure.m4` enables the component only when `struct sockaddr` was found
**and** the OS is NetBSD, FreeBSD, OpenBSD, DragonFly, **or Apple**. The
compile mode is hard-set to `static`. There is no runtime gate: if it
compiled, its `open` runs at framework open on this platform. On a typical
macOS/BSD build this and `bsdx_ipv6` are the two `pif` components present;
`posix_ipv4` is explicitly *excluded* on these OSes.

## The component struct

```c
pmix_pif_base_component_t pmix_mca_pif_bsdx_ipv4_component = {
    PMIX_MCA_BASE_VERSION(pif),
    .pmix_mca_component_name = "bsdx_ipv4",
    PMIX_MCA_BASE_MAKE_VERSION(component, ...),
    .pmix_mca_open_component = if_bsdx_open
};
```

No module, no query, no close — just the open. `PMIX_MCA_BASE_COMPONENT_INIT`
publishes the component pointer into `static-components.h`.

## What `if_bsdx_open` does

1. Calls `getifaddrs(3)` to obtain the linked list of `struct ifaddrs`.
2. Walks the list and, for each entry, **skips** it if `ifa_addr` is NULL
   (see below), skips it unless it is `AF_INET` (IPv4 only — IPv6 is
   `bsdx_ipv6`'s job), skips interfaces that are not `IFF_UP`, and skips
   `IFF_POINTOPOINT` (p2p) interfaces.
3. For each surviving entry, `PMIX_NEW`s a `pmix_pif_t` and fills:
   `af_family = AF_INET`; the primary address (`sin_addr`, `sin_family`,
   and the BSD-specific `sin_len`); `if_name`; `if_index` from the running
   list size (`pmix_list_get_size(&pmix_if_list) + 1`); `if_mask` as a
   **CIDR prefix length** obtained by running the local `prefix()` helper
   over **`ifa_netmask`** (which converts a netmask by counting trailing
   zero bits); `if_flags`; and `if_kernel_index` from
   `if_nametoindex(name)`.
4. Appends the object to `pmix_if_list`.

It does **not** populate `if_mac` or `ifmtu` — `getifaddrs` does not hand
those back here, so MAC/MTU stay zero for BSD/macOS interfaces. Only
`posix_ipv4` fills those.

## Gotchas

- **`ifa_addr` and `ifa_netmask` are each documented as possibly NULL.**
  The `getifaddrs(3)` man page on these very platforms says `ifa_addr`
  references the interface's address "if one exists, otherwise it is
  NULL", and likewise `ifa_netmask` "if one is set". Discovery is the
  first thing that touches the OS in `pmix_rte_init()`, for *every* PMIx
  process, so an unchecked dereference here is a segfault before the
  library is up. Both are checked; a NULL netmask is recorded as a host
  route (`if_mask` 32), which is the safe direction — it makes
  `pmix_net_samenetwork()` demand an exact address match rather than
  claim every address is on our network.
- **`if_mask` comes from `ifa_netmask`, never from the address.** This is
  worth stating because the component got it wrong for years: it ran
  `prefix()` over `ifa_addr`, so a /24 interface reported whatever the
  trailing zero bits of its own address happened to say (32 for any
  address ending in an odd octet). Nothing crashes on that — it silently
  turns `pmix_ifaddrtokindex()` into an exact-address match and hands
  `pmix_ifindextomask()` callers a wrong prefix. `test/unit/pif_discovery`
  cross-checks every discovered prefix against `getifaddrs(3)` to keep it
  from coming back.
- **Clean up the `getifaddrs` results on every path.** `getifaddrs`
  allocates the list and hands it back through the pointer you give it;
  every exit must `freeifaddrs()` it. (Older revisions `malloc`ed an
  outer `struct ifaddrs **` first, with a comment claiming the call
  segfaulted without it. It does not — `getifaddrs(&local_ptr)` is the
  documented usage — and the extra allocation was itself unchecked.)
- **CIDR, not netmask.** `if_mask` holds a prefix length. The `prefix()`
  helper is duplicated verbatim in `posix_ipv4`; keep them consistent if
  you change one.
- The p2p skip is marked with a `TODO: do we really skip p2p?` — it is a
  long-standing question, not settled policy. Don't remove it without
  understanding who relies on the current interface set.
