<!--
  Copyright (c) 2026      Nanook Consulting  All rights reserved.

  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
-->

# AGENTS.md: The PIF `linux_ipv6` Component

`linux_ipv6` discovers the local node's **IPv6** interfaces on Linux by
reading the kernel's `/proc/net/if_inet6` table. Read the framework
[`AGENTS.md`](../AGENTS.md) first; this file covers only what is specific
to `linux_ipv6`. Like every `pif` component it has no module and no
priority — its `open` function appends `pmix_pif_t` entries to the shared
`pmix_if_list`.

## Files

| File | Contents |
|------|----------|
| `pif_linux_ipv6.c` | Component struct + the `if_linux_ipv6_open` discovery routine. |
| `configure.m4` | Platform gate (static-only): `struct sockaddr` present and Linux. |
| `Makefile.am` | Builds `libpmix_mca_pif_linux_ipv6.la`. |

## When it is compiled in

`configure.m4` enables the component when `struct sockaddr` was found
**and** `oac_found_linux = yes`. Compile mode is hard-set to `static`. On
a typical Linux build this is the IPv6 half, paired with `posix_ipv4` for
IPv4. It is not built on BSD/macOS (which use the `bsdx_*` components).

## The component struct

Same shape as the others: `PMIX_MCA_BASE_VERSION(pif)`, name
`"linux_ipv6"`, single `.pmix_mca_open_component = if_linux_ipv6_open`.

## What `if_linux_ipv6_open` does

1. Tries to `fopen("/proc/net/if_inet6", "r")`. **If the file is absent
   the function returns `PMIX_SUCCESS` and adds nothing** — a kernel
   without that proc entry simply yields no IPv6 interfaces, not an error.
2. `fscanf`s each line: sixteen 2-hex-digit address bytes, then the
   interface's kernel index, prefix length, scope, DAD status, and name.
   The loop condition is `21 == fscanf(...)`, i.e. **a complete line or
   stop** — not `!= EOF`. A partial match neither consumes the input that
   did not convert nor reports EOF, so the `!= EOF` form would spin
   forever appending interfaces built from whatever `idx`/`pfxlen`/`scope`
   happened to hold from the previous line (or, on the first line, from
   nothing at all).
3. **Scope filter:** accepts only global (`0x00`) and host/loopback
   (`0x10`) scope; skips everything else (link-local `0x20`, site-local
   `0x40`, …). This is the Linux analogue of `bsdx_ipv6`'s `fe80::` skip.
4. For each accepted line, `PMIX_NEW`s a `pmix_pif_t` and fills:
   `af_family = AF_INET6`; the address assembled byte-by-byte into
   `sin6_addr`; `sin6_family`; `sin6_scope_id` = the scope from `/proc`;
   `if_name`; `if_index` from the running list size; `if_kernel_index` =
   the index read from `/proc`; and `if_mask` = the **prefix length read
   from `/proc`** (a genuine per-interface prefix, unlike `bsdx_ipv6`'s
   hard-coded 64).
5. Appends the object to `pmix_if_list`.

### Flags come from the kernel, with `posix_ipv4` as a fallback

`/proc/net/if_inet6` has no flags column, so the component asks the
kernel directly and only falls back to the already-discovered IPv4
interface of the same name:

```c
if (if_linux_ipv6_flags(ifname, &flag)) {            /* SIOCGIFFLAGS */
    intf->if_flags = flag;
} else if (PMIX_SUCCESS == pmix_ifindextoflags(pmix_ifnametoindex(ifname), &flag)) {
    intf->if_flags = flag;                            /* what posix_ipv4 found */
} else {
    intf->if_flags = IFF_UP;                          /* last resort */
}
```

The `SIOCGIFFLAGS` probe was added because the two fallbacks are both
unreliable: `pmix_if_list` is the list being built, so a device whose
IPv4 address has not been discovered yet is simply absent from it, and a
bare `IFF_UP` loses `IFF_LOOPBACK` — which makes `::1` look like an
ordinary routable address to everything downstream, including the code
that picks which address to advertise to other hosts. The middle branch
still makes the component prefer to run *after* `posix_ipv4`, which is
the static-components order on Linux; keep it intact.

## Gotchas

- **Missing `/proc/net/if_inet6` is not an error** — it just means no
  IPv6. Don't change the early return into a failure.
- **Flags are not in `/proc`** (see above). The kernel probe is what
  makes loopback detection work; the `posix_ipv4` fallback below it is
  why the component still prefers to run second.
- **Scope, not address-prefix, is the filter here** (0x00/0x10 accepted),
  which is a different mechanism than the BSD component's
  `IN6_IS_ADDR_LINKLOCAL` check even though both aim to drop link-local
  addresses. If you adjust which scopes are accepted, mirror the intent of
  the BSD side.
- The `fscanf` format string and the 16-byte address loop are brittle;
  changes need testing against a real `/proc/net/if_inet6` with multiple
  scopes present. Whatever you change, keep the loop condition an
  exact-field-count test rather than an EOF test (see above).
- **This component is not compiled on macOS**, so nothing a developer
  builds at home covers it. `test/unit/pif_discovery` asserts on the
  discovered list rather than on the mechanism, so it runs unchanged on
  either platform — run it in a Linux container (see
  [`contrib/dockerswarm/AGENTS.md`](../../../../contrib/dockerswarm/AGENTS.md))
  after touching this file.
