# AGENTS.md: The PSTAT `pmacos` Component

`pmacos` ("**P**MIx **macOS**") is the real, production `pstat` component
for macOS (Darwin): it samples resource-utilization statistics from a Mac
using the native mach, libproc, sysctl, and IOKit interfaces — there is no
`/proc` to read. Read the framework [`AGENTS.md`](../AGENTS.md) first —
this file only covers what is specific to `pmacos`. The op object, the
`op->cb` synchronous-vs-timer duality, the four spec structs, and the
parse helpers are all framework concepts described there. `pmacos` is the
Darwin analog of [`plinux`](../plinux/AGENTS.md); the two never coexist (a
given host builds exactly one of them), and its `query`/`update`/cancel
control flow is deliberately identical to `plinux`'s so only the four
category readers differ.

## Files

| File | Contents |
|------|----------|
| `pstat_pmacos.h` | Declares the component and module symbols. |
| `pstat_pmacos_component.c` | Component struct + `pstat_pmacos_component_query`. |
| `pstat_pmacos_module.c` | The module: `query`, the periodic `update`, and the four macOS `*_stat` readers. |
| `configure.m4` | Gates the build to Apple/Darwin and links IOKit + CoreFoundation. |

## Component (`pstat_pmacos_component.c`)

Identical in shape to `plinux`: the bare `pmix_pstat_base_component_t`,
whose `pstat_pmacos_component_query` is unconditional at run time (all the
"can I run here?" work was done at **configure** time) and returns the
module with **priority 80**. 80 beats `test` (20); because `plinux` never
builds on macOS, the equal 80s never compete.

### Build gating (`configure.m4`)

`MCA_pmix_pstat_pmacos_CONFIG` sets the component "happy" only when
`oac_found_apple` is `yes` (from `OAC_CHECK_OS_FLAVORS`). When happy it
sets `pstat_pmacos_LIBS="-framework IOKit -framework CoreFoundation"`,
`AC_SUBST`s it (the `Makefile.am` feeds it to both the DSO and static
`LIBADD`), and appends it to `PMIX_EMBEDDED_LIBS` so a **static** build
links the frameworks into the final consumers. On a non-Apple host the
whole directory is omitted from the build, so nothing here needs a
run-time `#ifdef`.

## Module (`pstat_pmacos_module.c`)

`init`/`finalize` are no-ops. `query`, `monitor_fields` and the periodic
`update` are copied verbatim from `plinux` — they implement the framework
contract (request dispatch, target-peer resolution, the synchronous
`op->cb` pass, the periodic timer/event path, and `PMIX_MONITOR_CANCEL`)
with no platform-specific content. All the macOS specifics live in the
four readers, each of which appends one or more `PMIX_*_RESOURCE_USAGE`
data arrays tagged with a `*_SAMPLE_TIME`, exactly like `plinux`.

**Keep it that way.** These two functions are the same code in two files,
so a defect found in one is a defect in the other, and a fix applied to
one and not the other silently gives macOS and Linux different behavior
for the same request. The rules they encode — the untrusted monitor
value, who owns the answer list on the synchronous path, why a reader's
`PMIX_ERR_NOT_FOUND` is skipped rather than fatal, why every
`PMIx_Info_list_convert()` result is destructed after it is added, and
why a refused `PMIx_Notify_event` releases its own caddy — are written up
once, in the [framework guide](../AGENTS.md) and in
[`plinux/AGENTS.md`](../plinux/AGENTS.md). Read them there; only what is
genuinely Darwin-specific is repeated below. Diffing the two `query`
bodies (ignoring whitespace) is the cheap way to confirm they have not
drifted.

### The four macOS readers

- **`proc_stat`** — opens with a `proc_pidinfo(PROC_PIDTBSDINFO)` call
  that serves as the existence check: libproc stops answering for a pid
  the instant the process exits, and a periodic monitor samples processes
  that come and go, so a short read there returns `PMIX_ERR_NOT_FOUND`
  and `update()` skips that peer. This is the Darwin equivalent of
  `plinux` failing to open `/proc/<pid>/stat`, and it has to be done
  *before* anything is appended: without it a dead process still produced
  a record — a process ID, a pid and a sample time with no statistics
  behind them — that a caller could not tell apart from a live process
  for which no fields were requested. The same call supplies the process
  status, mapped to the single-character state string `plinux` emits
  (`proc_state_string`). `proc_pidinfo(PROC_PIDTASKINFO)` then gives CPU
  time (user+system nanoseconds → `PMIX_TIMEVAL`), thread count,
  scheduling priority, and virtual/resident sizes, and `proc_name` gives
  the command name. The nanosecond CPU total is split into the
  `struct timeval` with integer arithmetic rather than through a
  `double`, and its seconds are carried at the width of a `time_t`; the
  old cast through `int` capped the reported time at 68 CPU-years, which
  a long-lived many-threaded process on a large node does reach. Only
  fields whose `op->pstats` flag is set are emitted. Emits
  `PMIX_PROC_RESOURCE_USAGE`.
- **`node_stat`** — `getloadavg` for the three load averages,
  `sysctlbyname("hw.memsize")` for total RAM, `host_statistics64`
  (`HOST_VM_INFO64`) × page size for free (`free_count`) and cached
  (`external_page_count`) memory, and `sysctlbyname("vm.swapusage")` for
  swap total/free. Gated by `op->ndstats`; emits into the node sub-list.
  **`mach_host_self()` returns a send right and adds a user reference to
  the host port on every call**, so the reference has to be handed back
  with `mach_port_deallocate()` — a periodic node monitor calls this on
  every tick. The port is taken once, both `host_*` calls are made
  through it, and it is released before any path that can return early;
  the `bool` that carries "we got the numbers" past the release exists
  for exactly that reason.
- **`disk_stat`** — enumerates `IOBlockStorageDriver` services via IOKit,
  recovers each device's BSD name (`disk0`, `disk1`, …) from its child
  `IOMedia` (`disk_bsd_name`), and pulls the per-device `Statistics`
  dictionary. Byte counts are normalized to 512-byte sectors and
  nanosecond times to milliseconds so the units match `plinux`. Selection
  honors `op->disks` by exact BSD-name match. Emits
  `PMIX_DISK_RESOURCE_USAGE`.
- **`net_stat`** — walks the routing-socket interface list
  (`sysctl(CTL_NET, PF_ROUTE, 0, 0, NET_RT_IFLIST2, 0)`), taking the
  `RTM_IFINFO2` messages, and emits received/sent byte/packet/error
  counters selected by `op->netstats`, optionally filtered by `op->nets`.
  Emits `PMIX_NETWORK_RESOURCE_USAGE`. It looks like the harder way to do
  what `getifaddrs()` does, and it is deliberate: `getifaddrs()` hands
  back a `struct if_data`, whose byte and packet counts are `u_int32_t`.
  A busy interface passes 4 GiB in minutes, the counter wraps, and
  successive samples of a monitor go *backwards* — the same defect the
  Linux reader had when it parsed its `/proc/net/dev` columns with
  `strtoul`. `NET_RT_IFLIST2` returns the identical per-interface data as
  a `struct if_data64`, which is the width these numbers need. Enumerating
  a live host both ways produces the same interfaces in the same order
  with the same values, so this is a widening and not a change of
  meaning.

  Two things about the walk. Each routing message opens with its own
  length, and the buffer holds more than one message type — `RTM_NEWADDR`
  records use a shorter struct — so the loop steps by `ifm_msglen` and
  reads nothing out of a message until it has checked that the message is
  long enough to hold it: a zero length would spin, and an overlong one
  would step past the end of the buffer. And `sdl_data` is **not**
  NUL-terminated; it is `sdl_nlen` name bytes followed immediately by the
  link-layer address, so the name is bounded by `sdl_nlen` and copied
  out.

Memory/size values reported by the OS in bytes are normalized to MB (float)
to match the Linux reader; disk/net counters stay as raw `uint64` totals.

## Caveats and known rough edges

If you touch this file, be aware of these (and fixing them is welcome as a
standalone commit — do not paper over them):

- **Some Linux fields have no macOS analog and are simply not emitted**
  even when requested: `proc_stat` omits per-process `cpu` (last CPU),
  `pctcpu`, `pss`, and `pkvsize`; `node_stat` omits `mbuf`,
  `mswapcached`, and `mmap`; `disk_stat` omits the merged-request counts
  (`rdmerged`/`wrtmerged`) and the `ioprog`/`ioms`/`ioweight` fields that
  `/proc/diskstats` exposes but IOKit does not.
- **The IOKit main port is passed as `0`** (`PMIX_PSTAT_IOMAIN_PORT`)
  rather than `kIOMainPortDefault`/`kIOMasterPortDefault`, so the code
  builds cleanly across macOS SDK versions without version-gating or
  tripping `-Werror` on the deprecated name.
- **The registry-plane argument is copied into a full `io_name_t`
  (`char[128]`) buffer** in `disk_bsd_name` before being passed to the
  IOKit lookups; passing the short `kIOServicePlane` literal directly
  trips GCC's `-Wstringop-overread` under `--enable-devel-check`.
