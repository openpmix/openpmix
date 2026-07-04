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

`init`/`finalize` are no-ops. `query` and the periodic `update` are copied
verbatim from `plinux` — they implement the framework contract (request
dispatch, target-peer resolution, the synchronous `op->cb` pass, the
periodic timer/event path, and `PMIX_MONITOR_CANCEL`) with no
platform-specific content. All the macOS specifics live in the four
readers, each of which appends one or more `PMIX_*_RESOURCE_USAGE` data
arrays tagged with a `*_SAMPLE_TIME`, exactly like `plinux`.

### The four macOS readers

- **`proc_stat`** — for one peer, `proc_pidinfo(PROC_PIDTASKINFO)` gives
  CPU time (user+system nanoseconds → `PMIX_TIMEVAL`), thread count,
  scheduling priority, and virtual/resident sizes;
  `proc_pidinfo(PROC_PIDTBSDINFO)` gives the process status, mapped to the
  same single-character state string `plinux` emits (`proc_state_string`);
  and `proc_name` gives the command name. Only fields whose `op->pstats`
  flag is set are emitted. Emits `PMIX_PROC_RESOURCE_USAGE`.
- **`node_stat`** — `getloadavg` for the three load averages,
  `sysctlbyname("hw.memsize")` for total RAM, `host_statistics64`
  (`HOST_VM_INFO64`) × page size for free (`free_count`) and cached
  (`external_page_count`) memory, and `sysctlbyname("vm.swapusage")` for
  swap total/free. Gated by `op->ndstats`; emits into the node sub-list.
- **`disk_stat`** — enumerates `IOBlockStorageDriver` services via IOKit,
  recovers each device's BSD name (`disk0`, `disk1`, …) from its child
  `IOMedia` (`disk_bsd_name`), and pulls the per-device `Statistics`
  dictionary. Byte counts are normalized to 512-byte sectors and
  nanosecond times to milliseconds so the units match `plinux`. Selection
  honors `op->disks` by exact BSD-name match. Emits
  `PMIX_DISK_RESOURCE_USAGE`.
- **`net_stat`** — walks `getifaddrs`, reads the per-interface `if_data`
  on each `AF_LINK` entry, and emits received/sent byte/packet/error
  counters selected by `op->netstats`, optionally filtered by `op->nets`.
  Emits `PMIX_NETWORK_RESOURCE_USAGE`.

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
- **`net_stat` uses the 32-bit `if_data` counters** from `getifaddrs`,
  which can wrap on long-running interfaces. The 64-bit counters would
  require parsing `sysctl(NET_RT_IFLIST2)` (`if_data64`); switching to
  that is a reasonable future improvement.
- **The IOKit main port is passed as `0`** (`PMIX_PSTAT_IOMAIN_PORT`)
  rather than `kIOMainPortDefault`/`kIOMasterPortDefault`, so the code
  builds cleanly across macOS SDK versions without version-gating or
  tripping `-Werror` on the deprecated name.
- **The registry-plane argument is copied into a full `io_name_t`
  (`char[128]`) buffer** in `disk_bsd_name` before being passed to the
  IOKit lookups; passing the short `kIOServicePlane` literal directly
  trips GCC's `-Wstringop-overread` under `--enable-devel-check`.
