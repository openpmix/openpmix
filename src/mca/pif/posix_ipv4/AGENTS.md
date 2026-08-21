<!--
  Copyright (c) 2026      Nanook Consulting  All rights reserved.

  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
-->

# AGENTS.md: The PIF `posix_ipv4` Component

`posix_ipv4` discovers the local node's **IPv4** interfaces on POSIX
systems that are **not** the BSDs/macOS, using the classic
`socket()` + `ioctl(SIOCGIFCONF)` interface-listing technique plus
per-interface `SIOCGIF*` ioctls. Read the framework
[`AGENTS.md`](../AGENTS.md) first; this file covers only what is specific
to `posix_ipv4`. Like every `pif` component it has no module and no
priority — its `open` function appends `pmix_pif_t` entries to the shared
`pmix_if_list`. It is the **richest** discoverer: the only component that
fills in MAC address and MTU.

## Files

| File | Contents |
|------|----------|
| `pif_posix.c` | Component struct + the `if_posix_open` discovery routine. |
| `configure.m4` | Platform gate (static-only) + `struct ifreq` member checks. |
| `Makefile.am` | Builds `libpmix_mca_pif_posix_ipv4.la`. |

## When it is compiled in

`configure.m4` enables the component when `struct sockaddr` was found
**and** the OS is **not** NetBSD, FreeBSD, OpenBSD, DragonFly, or Apple —
i.e. the complement of the `bsdx_ipv4` gate, so on Linux (and other
generic POSIX systems) this is the IPv4 discoverer. It additionally runs
`AC_CHECK_MEMBERS` for `struct ifreq.ifr_hwaddr` and `struct ifreq.ifr_mtu`,
which gate the MAC and MTU ioctls below. Compile mode is hard-set to
`static`. On Linux it is paired with `linux_ipv6`.

## The component struct

Same shape as the other `pif` components: `PMIX_MCA_BASE_VERSION(pif)`,
name `"posix_ipv4"`, single `.pmix_mca_open_component = if_posix_open`.

## What `if_posix_open` does

1. Opens a scratch `socket(AF_INET, SOCK_DGRAM, 0)` to issue ioctls
   against. `AF_INET` is required — the comment notes `AF_UNSPEC`/`AF_INET6`
   make everything fail.
2. **Enumerates interfaces with `ioctl(SIOCGIFCONF)` in a growing-buffer
   loop.** The loop exists because different kernels report
   "buffer too small" incompatibly — the long comment enumerates Solaris
   (returns -1/EINVAL), macOS (fills only what fit), Linux (reports the
   full required length), and FreeBSD (sets len to 0). The buffer doubles
   up to `MAX_PIFCONF_SIZE` (10 MiB) until the returned length stops
   changing, starting from `DEFAULT_NUMBER_INTERFACES` (10) entries.
3. Walks the returned `struct ifreq` array (advancing by a
   `sa_len`-aware stride where `HAVE_STRUCT_SOCKADDR_SA_LEN` is defined),
   and for each interface:
   - skips non-`AF_INET` entries;
   - `SIOCGIFFLAGS` → skips interfaces that are not `IFF_UP`, and (where
     `IFF_SLAVE` is defined) skips bonded/load-balancer slaves so the
     master is picked up instead;
   - `PMIX_NEW`s a `pmix_pif_t`, sets `af_family = AF_INET`, copies the
     name, and assigns `if_index` from the running list size;
   - `SIOCGIFINDEX` → `if_kernel_index` (falling back to `if_index` where
     `SIOCGIFINDEX` is undefined, and reading `ifr_ifindex`/`ifr_index`
     per what the platform defines);
   - `SIOCGIFADDR` → primary IPv4 address (copied as `struct sockaddr_in`);
   - `SIOCGIFNETMASK` → `if_mask` as a **CIDR prefix length** via the local
     `prefix()` helper;
   - `SIOCGIFHWADDR` → `if_mac[6]` (only where `SIOCGIFHWADDR` and
     `ifr_hwaddr` exist — effectively Linux);
   - `SIOCGIFMTU` → `ifmtu` (only where `SIOCGIFMTU` and `ifr_mtu` exist);
   - appends the object to `pmix_if_list`.

   The last two are **informational and optional**: an interface that
   cannot report a MAC or an MTU is still perfectly usable, so those two
   failures log verbosely and keep the interface with the zeroed `if_mac`
   / `ifmtu` the constructor gave it (which is exactly what the `bsdx_*`
   components leave behind on every interface anyway). Every *other*
   per-interface ioctl failure abandons that one interface and moves on.

4. Frees the ioctl buffers and closes the scratch socket.

## Gotchas

- **This is the only component that fills `if_mac` and `ifmtu`.** The
  `bsdx_*` components leave them zero. Code that needs MAC/MTU therefore
  only gets them on `posix_ipv4` platforms — don't assume they are
  populated elsewhere.
- **Never `break` out of the interface loop on a per-interface error.**
  A failed `SIOCGIFADDR`, `SIOCGIFHWADDR` or `SIOCGIFMTU` used to do
  exactly that, so one unexpected errno on one interface silently
  truncated discovery of every interface after it — and the symptom is
  "PMIx cannot find a usable interface", nowhere near the ioctl that
  failed. Per-interface failures belong to that interface: `continue`
  (or, for the optional MAC/MTU, keep the interface as-is).
- **The `SIOCGIFCONF` growing-buffer logic is deliberately defensive.**
  The portability comment documents four incompatible kernel behaviors;
  do not "simplify" the loop without accounting for all of them. Two
  details in it are load-bearing:
  - The retry guard is `errno != EINVAL || lastlen != 0`. Only EINVAL on
    a call that has *not yet* succeeded means "your buffer is too small";
    with `&&` there instead, a genuine `ENOTTY`/`EPERM` on the first
    attempt was swallowed and retried all the way up to the 10 MiB cap
    before reporting a generic "unable to find network interfaces".
  - The walk clamps `rem` to the buffer that was actually allocated. The
    comment itself claims a platform (Linux) that reports the space the
    entries *would* have needed rather than the space they used; take
    that at face value and `ifc_len` bytes of `memcpy` become a heap
    overflow in both directions.
- **CIDR, not netmask** — `if_mask` is a prefix length via `prefix()`,
  the same helper duplicated in `bsdx_ipv4`. Keep them consistent.
- The per-address `if_index` is assigned per *address*, so an interface
  with multiple addresses can appear multiple times with distinct
  `if_index` values (but a shared `if_kernel_index`).
