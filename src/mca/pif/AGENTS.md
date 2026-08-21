<!--
  Copyright (c) 2026      Nanook Consulting  All rights reserved.

  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
-->

# AGENTS.md: The PIF Framework

This document orients AI agents and human contributors working in the
`pif` (**P**MIx network **I**nter**F**ace discovery) framework. It
assumes you have already read the top-level [`AGENTS.md`](../../../AGENTS.md)
— the golden rules, prefix conventions, thread-safety model, and MCA
concepts described there all apply here and are not repeated. This file
covers what is specific to `pif`: what the framework is for, its unusual
"there is no module" structure, the shared interface object every
component populates, the large family of lookup helpers built on top of
it, and the platform gates that decide which components exist. Each
component subdirectory (`bsdx_ipv4/`, `bsdx_ipv6/`, `linux_ipv6/`,
`posix_ipv4/`) carries its own `AGENTS.md` with component-specific
detail. There is no `docs/how-things-work/` page for this framework.

## What PIF does

`pif` enumerates the **local node's network interfaces** exactly once, at
library startup, into a single process-wide list. For every interface it
records the name, primary address, netmask (as a CIDR prefix length),
kernel index, flags, and — where the platform exposes them — the MAC
address and MTU. That list is then queried through a family of
`pmix_if*` lookup helpers by the rest of the library: most importantly
the `ptl` listener, which walks the interfaces to choose one to bind and
publish, and `if_include`/`if_exclude` interface filtering. Code that
needs to answer "is this hostname local?", "what is the kernel index of
the NIC on the same subnet as address X?", or "give me all my aliases"
also comes here.

`pif` is a **read-only, discover-once** subsystem: nothing in it opens
sockets for traffic, sends, or mutates the interface list after startup.
It is pure local-node inventory.

## The single most important structural fact

`pif` does **not** look like the other MCA frameworks, and mis-reading it
as one is the most common way to get lost. Three things are missing that
you would normally expect:

1. **There is no module struct.** There is no `pmix_pif_module_t`, no
   table of function pointers, and no exported "active module" global.
   A `pif` component is a bare `pmix_mca_base_component_t` (typedef'd as
   `pmix_pif_base_component_t` in [`pif.h`](pif.h)) — nothing more.

2. **There is no selection.** There is no `pif_base_select.c` and no call
   to `pmix_mca_base_select` anywhere in the tree. Components are never
   arbitrated by priority; they carry no priority at all.

3. **Discovery happens in the component's `open` function.** Each
   component's entire job is done by its `pmix_mca_open_component`
   entry point: it probes the OS and **appends** what it finds to the
   shared global list `pmix_if_list`. When the framework is opened
   (`pmix_mca_base_framework_components_open`), every built-in component's
   open runs, so all of them contribute to the same list. In that sense
   `pif` is **multi-select** — several components are active at once — but
   the "selection" is simply "every component that compiled on this
   platform runs its discovery."

Consequently, **the public API of this framework is not in the framework
directory at all.** The interface object (`pmix_pif_t`), the shared list
(`pmix_if_list`), and every lookup helper live in
[`src/util/pmix_if.h`](../../util/pmix_if.h) and
[`src/util/pmix_if.c`](../../util/pmix_if.c). The `pif/base/` directory
holds only the framework declaration, the one MCA parameter, and the
`pmix_pif_t` class instance. If you are looking for "the interface
lookup code," it is in `src/util/`, not under `src/mca/pif/`.

## The shared interface object (`pmix_pif_t`)

Defined in [`src/util/pmix_if.h`](../../util/pmix_if.h) and constructed
in `pif_base_components.c`. Every component fills one of these per
discovered interface and appends it to `pmix_if_list`.

| Field | Type | Purpose |
|-------|------|---------|
| `super` | `pmix_list_item_t` | lets the object live on `pmix_if_list` |
| `if_name` | `char[PMIX_IF_NAMESIZE+1]` | OS interface name (e.g. `en0`, `eth0`) |
| `if_index` | `int` | **PMIx list index** — 1-based position, assigned `pmix_list_get_size(&pmix_if_list) + 1` as each entry is appended |
| `if_kernel_index` | `uint16_t` | **OS kernel index** (from `if_nametoindex`, `SIOCGIFINDEX`, or `/proc`) |
| `af_family` | `uint16_t` | `AF_INET` or `AF_INET6` |
| `if_flags` | `int` | interface flags (`IFF_UP`, `IFF_LOOPBACK`, …) |
| `if_speed` | `int` | link speed — constructed to 0; **not populated** by any current component |
| `if_addr` | `struct sockaddr_storage` | primary address (v4 or v6) |
| `if_mask` | `uint32_t` | netmask as a **CIDR prefix length** (e.g. 24), *not* the dotted netmask |
| `if_bandwidth` | `uint32_t` | constructed to 0; **not populated** by any current component |
| `if_mac` | `uint8_t[6]` | MAC address — only `posix_ipv4` fills this (Linux `SIOCGIFHWADDR`) |
| `ifmtu` | `int` | MTU — only `posix_ipv4` fills this; named `ifmtu` to dodge a `if_mtu` macro collision on some BSDs |

Two facts here bite repeatedly:

- **`if_index` and `if_kernel_index` are different numbers with different
  meanings.** `if_index` is just this list's slot; `if_kernel_index` is
  what the OS calls the NIC. The lookup helpers come in `*index*` and
  `*kindex*` flavors precisely to keep them apart — do not swap one for
  the other.
- **`if_mask` is a CIDR prefix, not a netmask.** The header comment on
  the lookup helper (`pmix_ifindextomask`) even flags that it returns
  CIDR "NOT the actual netmask itself!". Each component converts the
  OS-supplied netmask to a prefix length (the `prefix()` helper in the
  IPv4 components) before storing it. The IPv6 components do *not* agree
  on what to put here: `linux_ipv6` stores the true prefix from `/proc`,
  `bsdx_ipv6` hard-codes 64. That is not simply an oversight — see
  [`bsdx_ipv6/AGENTS.md`](bsdx_ipv6/AGENTS.md), and note that the sole
  consumer, `pmix_net_samenetwork()`, only compares IPv6 addresses at
  all when the prefix is 64.
- **`if_kernel_index` is a `uint16_t`, and `pmix_ifnametokindex()`
  returns an `int16_t`.** Linux kernel interface indices are 32-bit and
  monotonically increasing, so a long-lived host that churns veth/container
  interfaces can hand out an index this field cannot hold. Both types are
  in the installed [`src/util/pmix_if.h`](../../util/pmix_if.h) and are
  therefore ABI; it is recorded in
  [`docs/todo.rst`](../../../docs/todo.rst) rather than fixed here.

`PMIX_IF_NAMESIZE` is 256. `PMIX_PIF_DEFAULT_NUMBER_INTERFACES` (10) and
`PMIX_PIF_MAX_PIFCONF_SIZE` (10 MiB) in [`pif.h`](pif.h) size the
`posix_ipv4` ioctl buffer growth. `pif.h` is an *installed* header, so
anything added there needs the prefix.

## Selection and lifecycle (`base/pif_base_components.c`)

The base file is small and does four things:

- **Declares the framework** with
  `PMIX_MCA_BASE_VERSIONED_FRAMEWORK_DECLARE(pmix, pif, NULL, register,
  open, close, static_components,
  PMIX_MCA_BASE_FRAMEWORK_FLAG_DEFAULT)`. Note the `NULL` description and
  the *default* flag — there is no separate select phase. The *versioned*
  form is what lets the base refuse a component built against a different
  `PMIX_MCA_pif_*_VERSION` (stated in [`pif.h`](pif.h) and nowhere else).
- **Instantiates the globals**: `pmix_if_list` (the shared interface
  list, `PMIX_LIST_STATIC_INIT`) and `pmix_if_do_not_resolve` (a bool
  flag), plus the `PMIX_CLASS_INSTANCE(pmix_pif_t, ...)` whose
  constructor zeroes the object and sets `if_index = -1`,
  `if_kernel_index = (uint16_t)-1`, `af_family = PF_UNSPEC`.
- **`pmix_pif_base_open`** constructs `pmix_if_list` and calls
  `pmix_mca_base_framework_components_open`, which runs each component's
  open (i.e. its discovery). A `frameopen` guard makes re-open a no-op;
  it is only raised once the components have opened, because on the
  failure path the base does *not* call our close (see below).
- **`pmix_pif_base_close`** drains and `PMIX_RELEASE`s every entry on
  `pmix_if_list`, destructs the list, and closes the components.

The framework is opened once, early, by **`pmix_init_util()`** in
[`src/runtime/pmix_init.c`](../../runtime/pmix_init.c) — it is the last
thing the util-init phase does, right after `pmix_net_init()` — for
**every** PMIx process, client, tool and server alike, and closed from
[`src/runtime/pmix_finalize.c`](../../runtime/pmix_finalize.c). It is not
server-only, and it is **not** opened from `pmix_rte_init` itself: that
function reaches it only through `pmix_init_util`.

That matters as soon as you write a test. `pmix_mca_base_framework_open`
is reference-counted, so a program that calls `pmix_init_util()` and then
opens `pif` again holds *two* references, and its matching close merely
hands one back — `pmix_pif_base_close` never runs, the interface list is
never drained, and the symptom is a close that "succeeds" while leaving
everything in place. Take the framework as `pmix_init_util()` left it.

### A component's open cannot fail the framework

`open_components()` in the MCA base returns `PMIX_SUCCESS`
unconditionally: a component whose open returns an error is logged
(subject to `mca_base_component_show_load_errors`), dropped from the
list, and the walk continues. So a `pif` component's return value only
picks a log message — it cannot abort discovery, and, importantly,
**whatever that component already appended to `pmix_if_list` stays
there.** For `pif` that is the right outcome (partial inventory beats
none), but do not write code here that relies on an error return
unwinding anything.

The corollary is the one non-obvious thing in `pif_base_components.c`:
`pmix_pif_base_open` can still fail *before* any component runs (a
`component_find` or filter error), and on that path
`pmix_mca_base_framework_open` never sets the framework's OPEN flag —
so `pmix_mca_base_framework_close` takes its not-open branch and the
framework's own close function is never called. Nothing else would give
`pmix_if_list` back or lower `frameopen`, and a latch left standing
would make a later successful open skip discovery altogether. Hence the
explicit drain on that path.

### Which components exist on a given host

Because there is no runtime selection, "which components run" is decided
entirely at **configure time** by each component's `configure.m4`. Only
the components whose platform test passes are compiled in and wired into
the generated `base/static-components.h`; the rest do not exist in the
binary at all. The gates are mutually complementary by address family and
OS:

| Component | Compiled when | Discovers | Mechanism |
|-----------|---------------|-----------|-----------|
| `posix_ipv4` | `struct sockaddr` present **and NOT** NetBSD/FreeBSD/OpenBSD/DragonFly/Apple | IPv4 | `socket()` + `ioctl(SIOCGIFCONF)` and per-IF `SIOCGIF*` ioctls |
| `linux_ipv6` | `struct sockaddr` present **and** Linux | IPv6 | reads and `fscanf`s `/proc/net/if_inet6` |
| `bsdx_ipv4` | `struct sockaddr` present **and** NetBSD/FreeBSD/OpenBSD/DragonFly/**Apple** | IPv4 | `getifaddrs(3)` |
| `bsdx_ipv6` | `struct sockaddr` present **and** NetBSD/OpenBSD/FreeBSD/386BSD/bsdi/**Apple** | IPv6 | `getifaddrs(3)` |

So a typical **Linux** build has `posix_ipv4` + `linux_ipv6`; a typical
**macOS/BSD** build has `bsdx_ipv4` + `bsdx_ipv6`. (This checkout, built
on macOS, has exactly `bsdx_ipv4` and `bsdx_ipv6` in
`static-components.h`.) Note Apple satisfies *both* the BSD IPv4 gate and
the BSD IPv6 gate but not the `posix_ipv4` gate (which explicitly
excludes Apple), so macOS never builds `posix_ipv4`.

### Ordering matters

Since every component appends to one shared list and `if_index` is
assigned from the running list size, the components' open order (the
order in `static-components.h`) determines the index numbering. It also
creates a soft cross-component dependency: `/proc/net/if_inet6` carries
no flags column, so `linux_ipv6` asks the kernel for them
(`SIOCGIFFLAGS`) and, if that fails, falls back to the flags
`posix_ipv4` already recorded for the same-named interface before
settling for a bare `IFF_UP`. Do not reorder the components.

### Testing a change here

`pif` is the one framework whose components are chosen at *configure*
time, so **a component you edit may not be compiled on the machine you
edit it on**: a macOS developer builds `bsdx_ipv4` + `bsdx_ipv6` and
gets no compiler coverage at all of `posix_ipv4` or `linux_ipv6`, and
vice versa on Linux. Building both halves means building on both, and a
Linux container is enough — see
[`contrib/dockerswarm/AGENTS.md`](../../../contrib/dockerswarm/AGENTS.md).

`test/unit/pif_discovery` (wired into `make check`) is written to make
that cheap: it asserts only on the *contents* of `pmix_if_list`, never on
the mechanism that filled it, so the same program is a meaningful test of
whichever pair of components the build selected. Its load-bearing check
cross-references every discovered IPv4 prefix against `getifaddrs(3)` —
an independent view of the same interfaces on both platforms, and the
only kind of check that catches a netmask that is wrong rather than
missing.

## The lookup helpers ([`src/util/pmix_if.c`](../../util/pmix_if.c))

This is the real public surface of the framework — the functions the rest
of the library calls after discovery has populated `pmix_if_list`. They
are all `PMIX_EXPORT`ed from [`src/util/pmix_if.h`](../../util/pmix_if.h)
and, apart from a couple of resolvers, are simple linear walks of
`pmix_if_list`.

### Iteration / counting

| Function | Returns |
|----------|---------|
| `pmix_ifcount()` | number of discovered interfaces (`pmix_list_get_size`) |
| `pmix_ifbegin()` | `if_index` of the first interface, or -1 |
| `pmix_ifnext(if_index)` | `if_index` of the entry after the given one, or -1 |

`pmix_ifbegin`/`pmix_ifnext` are the idiomatic way to walk interfaces by
index (as the `ptl` listener does).

### Name / index / kernel-index translation

| Function | Maps |
|----------|------|
| `pmix_ifnametoindex(name)` | name → PMIx list `if_index` |
| `pmix_ifnametokindex(name)` | name → `if_kernel_index` |
| `pmix_ifindextokindex(if_index)` | list index → kernel index |
| `pmix_ifindextoname(if_index, buf, len)` | list index → name |
| `pmix_ifkindextoname(if_kindex, buf, len)` | kernel index → name |

### Per-interface attribute lookups (by list index unless noted)

| Function | Yields |
|----------|--------|
| `pmix_ifindextoaddr` / `pmix_ifkindextoaddr` | primary `sockaddr` (by list / kernel index) |
| `pmix_ifindextomask` | CIDR prefix length |
| `pmix_ifindextomac` | 6-byte MAC |
| `pmix_ifindextomtu` | MTU |
| `pmix_ifindextoflags` | interface flags |
| `pmix_ifisloopback(if_index)` | true if `IFF_LOOPBACK` is set |

Most return `PMIX_SUCCESS`/`PMIX_ERROR` (not found).

**A kernel index does not identify one address.** An interface holding
both an IPv4 and an IPv6 address occupies *two* `pmix_if_list` entries
(one per address, appended by two different components) that share a
kernel index and differ in `if_index`. `pmix_ifkindextoaddr` returns
whichever of them comes first — on Linux routinely the IPv6 one, and
always so for loopback, since `linux_ipv6` reports `::1` ahead of
`posix_ipv4`'s `127.0.0.1`. Code that needs an address *of a given
family* must walk `pmix_if_list` and check each entry's `sa_family`
itself; reading a `sockaddr_in` out of the answer yields an IPv6 flow
label where it expected an address. This bit `pmix_ifmatches`, which
silently refused to match any dual-stack interface named by subnet.

### Address resolution and matching

These are the non-trivial ones:

- **`pmix_ifaddrtoname(if_addr, buf, len)`** — resolves `if_addr` (a
  hostname *or* a dotted/colon address) via `getaddrinfo`, then finds the
  local interface whose address exactly equals a resolved address and
  returns its name. **Honors `pmix_if_do_not_resolve`**: if that flag is
  set it returns `PMIX_ERR_NOT_FOUND` immediately without resolving.
- **`pmix_ifislocal(hostname)`** — thin wrapper: true iff
  `pmix_ifaddrtoname` finds a match. This is how PMIx decides a hostname
  refers to the local node (and why `do_not_resolve` forces "not local").
- **`pmix_ifaddrtokindex(if_addr)`** — resolves `if_addr`, then returns
  the **kernel index** of the local interface on the *same network*
  (`pmix_net_samenetwork` using the interface's `if_mask`), handling both
  AF_INET and AF_INET6. Unlike `pmix_ifaddrtoname` it does **not** check
  `do_not_resolve`.
- **`pmix_ifmatches(kidx, nets)`** — tests whether the interface with
  kernel index `kidx` matches any entry in a NULL-terminated `nets` argv,
  where each entry may be a named interface, an IP tuple, or a
  subnet+mask. Named entries are matched via `pmix_ifnametokindex`;
  tuple/CIDR entries via `pmix_iftupletoaddr` + mask compare against
  every IPv4 address the kernel index carries (see the caution above). Emits the
  `invalid-net-mask` `show_help` topic (from `help-pmix-util.txt`, in
  `src/util/` — not a `pif` help file) on an unparseable net. This backs
  `if_include`/`if_exclude` filtering.
- **`pmix_iftupletoaddr(inaddr, &net, &mask)`** — parses a dotted IPv4
  string, with optional `/N` or `/a.b.c.d` mask, into a network address
  and netmask; infers the mask from the number of dots when none is given.
  This is pure string parsing and does not touch `pmix_if_list`.
- **`pmix_ifgetaliases(&aliases)`** — appends the string form of every
  non-loopback interface address to an argv (IPv6 entries only when
  `PMIX_ENABLE_IPV6`). Used to enumerate all names/addresses for the
  local node.

### The `HAVE_STRUCT_SOCKADDR_IN` fallback

The entire file is wrapped in `#if HAVE_STRUCT_SOCKADDR_IN`. On an exotic
platform lacking `struct sockaddr_in`, a parallel set of stub
implementations returns `PMIX_ERR_NOT_SUPPORTED` (or `false`/no-op) for
most helpers, so callers still link and degrade cleanly.

## MCA parameters

`pif` registers exactly one, in `pmix_pif_base_register`:

| Parameter | Type | Meaning |
|-----------|------|---------|
| `pif_do_not_resolve` | bool (default false) | when true, `pmix_ifaddrtoname` (and therefore `pmix_ifislocal`) skip DNS/`getaddrinfo` resolution and report "not found" / "not local" |

It is registered against the framework (`"do_not_resolve"`) and stored in
the global `pmix_if_do_not_resolve`. Its purpose is to avoid potentially
slow or unwanted name resolution in environments where that is
undesirable.

## Threading

`pif` has no progress-thread interaction and no caddy pattern. Discovery
runs synchronously during `pmix_rte_init`, before the progress thread is
doing steady-state work, and builds `pmix_if_list` once. After that the
list is treated as **read-only** and its lookup helpers are pure reads,
so they are safe to call from any context (including progress-thread
handlers such as the `ptl` listener setup). Nothing here re-scans or
mutates the list at runtime; the only writer is component `open` at
startup and the only remover is framework `close` at shutdown.

## Directory layout

```
src/mca/pif/
├── pif.h                     Framework header: component typedef, version macro,
│                             DEFAULT_NUMBER_INTERFACES / MAX_PIFCONF_SIZE, includes
├── base/
│   ├── base.h                Framework declaration (extern pmix_pif_base_framework)
│   └── pif_base_components.c open/close/register, framework decl, pmix_pif_t class,
│                             pmix_if_list + pmix_if_do_not_resolve globals
├── bsdx_ipv4/                getifaddrs(3) IPv4 discovery (BSD/macOS)
├── bsdx_ipv6/                getifaddrs(3) IPv6 discovery (BSD/macOS)
├── linux_ipv6/               /proc/net/if_inet6 IPv6 discovery (Linux)
└── posix_ipv4/               SIOCGIFCONF ioctl IPv4 discovery (non-BSD POSIX)

src/util/pmix_if.h            pmix_pif_t definition + all lookup-helper prototypes
src/util/pmix_if.c            all lookup-helper implementations (the real API)
```

The split is deliberate but easy to trip over: the *framework* is in
`src/mca/pif/`, the *interface object and its query API* are in
`src/util/`.

## Building

Each component ships a `configure.m4` and builds as a static MCA
component (`libpmix_mca_pif_<name>.la`) linked into `libpmix`; there are
no DSO `pif` components (every `configure.m4` hard-sets the compile mode
to `static`). The gates in those `configure.m4` files (see the table
above) are what decide which components exist in a given build, so
**changing a platform gate, adding, or removing a component directory
requires re-running `./autogen.pl && ./configure … && make`** — a plain
`make` will not re-resolve `static-components.h`. Editing only a
`Makefile.am` needs just `make`.

`pif` ships **no `show_help` file of its own** — the only `show_help`
topic reachable through its code path (`invalid-net-mask`) lives in
`src/util/help-pmix-util.txt`, which belongs to `src/util`, not this
framework. So the "regenerate `pmix_show_help_content`" golden rule does
not normally bite here.

To exercise a real change, the interface list is easiest to observe
through the `ptl` listener (which logs its interface selection) or by
calling `pmix_ifcount`/`pmix_ifbegin`/`pmix_ifnext` from a small test —
`pif` is only meaningfully verified on the OS whose component you touched,
so test IPv4 changes on the matching platform (Linux for `posix_ipv4`,
BSD/macOS for `bsdx_ipv4`) and likewise for IPv6.

## When working in this framework

- **Do not go looking for a module or a select step — there is neither.**
  A component *is* its `open` (discovery) function. To add a discovery
  method, add a component whose open appends `pmix_pif_t` objects to
  `pmix_if_list`, and gate it in a `configure.m4`.
- **Populate every field you can, honestly.** Set `af_family`, and store
  the netmask as a **CIDR prefix length** (reuse a component's `prefix()`
  converter), not the raw netmask — the helpers assume CIDR. Leave
  `if_speed`/`if_bandwidth` at their constructed zero unless you actually
  measure them; today nothing does.
- **Respect the two index spaces.** Assign `if_index` from the current
  list size (append order) and `if_kernel_index` from the OS. Callers
  choose `*index*` vs `*kindex*` helpers deliberately; keep them
  meaningful.
- **Mind cross-component order.** Discovery order sets `if_index`
  numbering and, on Linux, `linux_ipv6` reads flags left behind by
  `posix_ipv4`. Don't assume a component runs in isolation.
- **The lookup helpers live in `src/util/pmix_if.c`.** Bug reports about
  `pmix_ifislocal`, resolution, or matching belong there; bug reports
  about *what got discovered* belong in the component `open` for the
  platform in question.
