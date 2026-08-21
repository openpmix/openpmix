<!--
  Copyright (c) 2026      Nanook Consulting  All rights reserved.

  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
-->

# AGENTS.md: The PIF `bsdx_ipv6` Component

`bsdx_ipv6` discovers the local node's **IPv6** interfaces on BSD-family
systems (including macOS) using `getifaddrs(3)`. Read the framework
[`AGENTS.md`](../AGENTS.md) first; this file covers only what is specific
to `bsdx_ipv6`. It is the IPv6 sibling of [`bsdx_ipv4`](../bsdx_ipv4/AGENTS.md)
and shares its no-module, discover-at-open shape.

## Files

| File | Contents |
|------|----------|
| `pif_bsdx_ipv6.c` | Component struct + the `if_bsdx_ipv6_open` discovery routine. |
| `configure.m4` | Platform gate (static-only) and BSD/Apple detection. |
| `Makefile.am` | Builds `libpmix_mca_pif_bsdx_ipv6.la`. |

## When it is compiled in

`configure.m4` enables the component when `struct sockaddr` was found
**and** the OS is NetBSD, OpenBSD, FreeBSD, 386BSD, bsdi, **or Apple**.
(Note the BSD list differs slightly from `bsdx_ipv4`'s — it adds 386BSD
and bsdi and drops DragonFly.) Compile mode is hard-set to `static`. It
runs unconditionally at framework open on a matching platform; there is no
runtime `--enable-ipv6` gate on *discovery* itself (that flag affects
consumers such as `pmix_ifgetaliases`, not this component).

## The component struct

Identical in shape to `bsdx_ipv4`: `PMIX_MCA_BASE_VERSION(pif)`, name
`"bsdx_ipv6"`, and a single `.pmix_mca_open_component = if_bsdx_ipv6_open`.
No module, no priority.

## What `if_bsdx_ipv6_open` does

1. Calls `getifaddrs(3)`.
2. For each entry, **skips** it if `ifa_addr` is NULL, skips it unless it
   is `AF_INET6`, skips non-`IFF_UP`
   interfaces, skips `IFF_POINTOPOINT`, and — importantly — **skips
   link-local addresses** (`IN6_IS_ADDR_LINKLOCAL`, i.e. the `fe80::`
   range). The code comment explains that `sin6_scope_id` from
   `getifaddrs` is unreliable (0 on macOS even when `ifconfig` shows a
   nonzero scope), so it filters on the address prefix instead of scope.
3. For each surviving entry, `PMIX_NEW`s a `pmix_pif_t` and fills:
   `af_family = AF_INET6`; the primary address (`sin6_addr`,
   `sin6_family`, and `sin6_scope_id` **forced to 0** since every nonzero
   scope was skipped); `if_name`; `if_index` from the running list size;
   `if_flags`; and `if_kernel_index` from `if_nametoindex(name)`.
4. Appends the object to `pmix_if_list`.

### The netmask

`if_mask` holds the real prefix length, converted from `ifa_netmask` by
the local `prefix6()` helper (leading one bits, walked a byte at a time
so endianness cannot enter into it). A NULL `ifa_netmask` is recorded as
a host route, `/128` — the safe direction, since it makes
`pmix_net_samenetwork()` demand an exact match rather than claim a whole
`/64` we cannot actually see.

It used to be hard-coded to **64** for every interface, on the grounds
that SLAAC hands out `/64` and "adrian says that's ok". That could not be
corrected on its own, which is worth knowing if you are reading old
history here: the sole consumer of an IPv6 `if_mask` is
`pmix_net_samenetwork()`, and *that* function compared IPv6 addresses
only when the prefix was 64 and returned false for every other value. So
the hard-coded 64 was compensating for a broken comparison, and the two
bugs concealed each other — `linux_ipv6` has always reported the true
prefix from `/proc`, which meant a loopback entry carrying `/128` was
never on the same network as anything, including itself. Both were fixed
together; `test/unit/util/util_net.c` pins the general comparison and
`test/unit/pif_discovery` pins the discovered prefixes against
`getifaddrs(3)`.

## Gotchas

- **The prefix is measured, not assumed** (see above) — and note that it
  and `pmix_net_samenetwork()` are a matched pair: neither the hard-coded
  /64 nor the /64-only comparison could have been changed alone.
- **Verbose discovery logging.** Unlike `bsdx_ipv4`, this component logs
  extensively via `pmix_output_verbose` on
  `pmix_pif_base_framework.framework_output` (each found/skipped
  interface, with the address printed). Raise the framework verbosity to
  see IPv6 discovery decisions.
- **`ifa_addr` may be NULL.** `getifaddrs(3)` on these platforms
  documents that it references the interface's address only "if one
  exists, otherwise it is NULL". Discovery runs in `pmix_rte_init()` for
  every PMIx process, so dereferencing it unchecked is a segfault before
  the library is up. See the same note in
  [`bsdx_ipv4`](../bsdx_ipv4/AGENTS.md).
- **Free the `getifaddrs` list on every exit path.** `getifaddrs`
  allocates the list and hands it back through the pointer you give it;
  every exit must `freeifaddrs()` it. (Older revisions `malloc`ed an
  outer `struct ifaddrs **` first, with a comment claiming the call
  segfaulted without it. It does not, and that allocation was itself
  unchecked — a failed `malloc` handed `getifaddrs` a NULL to write
  through.)
- `if_kernel_index` derivation is flagged `FIXME` in the source — the
  author was unsure `if_nametoindex` is the right source; it is what is
  used today.
