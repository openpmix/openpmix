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

Same shape as the others: `PMIX_PIF_BASE_VERSION_2_0_0`, name
`"linux_ipv6"`, single `.pmix_mca_open_component = if_linux_ipv6_open`.

## What `if_linux_ipv6_open` does

1. Tries to `fopen("/proc/net/if_inet6", "r")`. **If the file is absent
   the function returns `PMIX_SUCCESS` and adds nothing** — a kernel
   without that proc entry simply yields no IPv6 interfaces, not an error.
2. `fscanf`s each line: sixteen 2-hex-digit address bytes, then the
   interface's kernel index, prefix length, scope, DAD status, and name.
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

### Flags are borrowed from `posix_ipv4`

`/proc/net/if_inet6` does not carry interface flags, so the component
looks them up from the *already-discovered* IPv4 interface of the same
name:

```c
if (PMIX_SUCCESS == pmix_ifindextoflags(pmix_ifnametoindex(ifname), &flag)) {
    intf->if_flags = flag;
} else {
    intf->if_flags = IFF_UP;   // fallback
}
```

This makes `linux_ipv6` **depend on `posix_ipv4` having run first** (which
it does, per the static-components order on Linux). If the same-named
interface was not discovered by `posix_ipv4`, the IPv6 entry gets a bare
`IFF_UP` and, in particular, will *not* be recognized as loopback by
`pmix_ifisloopback`. Keep the component order intact.

## Gotchas

- **Missing `/proc/net/if_inet6` is not an error** — it just means no
  IPv6. Don't change the early return into a failure.
- **Flag borrowing is load-bearing** (see above): the ordering dependency
  on `posix_ipv4` is real. A hypothetical IPv6-only Linux build would give
  every IPv6 interface `IFF_UP` and lose loopback detection.
- **Scope, not address-prefix, is the filter here** (0x00/0x10 accepted),
  which is a different mechanism than the BSD component's
  `IN6_IS_ADDR_LINKLOCAL` check even though both aim to drop link-local
  addresses. If you adjust which scopes are accepted, mirror the intent of
  the BSD side.
- The `fscanf` format string and the 16-byte address loop are brittle;
  changes need testing against a real `/proc/net/if_inet6` with multiple
  scopes present.
