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

Identical in shape to `bsdx_ipv4`: `PMIX_PIF_BASE_VERSION_2_0_0`, name
`"bsdx_ipv6"`, and a single `.pmix_mca_open_component = if_bsdx_ipv6_open`.
No module, no priority.

## What `if_bsdx_ipv6_open` does

1. Calls `getifaddrs(3)`.
2. For each entry, **skips** it unless it is `AF_INET6`, skips non-`IFF_UP`
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

### The hard-coded /64 netmask

`if_mask` is set to a fixed **64** for every IPv6 interface — `getifaddrs`
does return a netmask, but the component ignores it and assumes the near-
universal `/64` prefix (the code comment attributes this to "adrian says
that's ok"). If you ever need the true IPv6 prefix length here, this is
the line to change — but be aware downstream code has only ever seen 64.

## Gotchas

- **/64 is assumed, not measured** (see above).
- **Verbose discovery logging.** Unlike `bsdx_ipv4`, this component logs
  extensively via `pmix_output_verbose` on
  `pmix_pif_base_framework.framework_output` (each found/skipped
  interface, with the address printed). Raise the framework verbosity to
  see IPv6 discovery decisions.
- **Free both the `getifaddrs` list and the outer pointer.** Each exit
  path calls `freeifaddrs(*ifadd_list)` (to release the list the kernel
  allocated) followed by `free(ifadd_list)` (the outer `malloc`ed
  pointer); the `getifaddrs`-failure path frees only the latter. The list
  content was historically leaked — keep the `freeifaddrs` in place.
- `if_kernel_index` derivation is flagged `FIXME` in the source — the
  author was unsure `if_nametoindex` is the right source; it is what is
  used today.
