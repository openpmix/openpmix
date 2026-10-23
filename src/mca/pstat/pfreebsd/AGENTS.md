<!--
  Copyright (c) 2026      Nanook Consulting  All rights reserved.

  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
-->

# AGENTS.md: The PSTAT `pfreebsd` Component

`pfreebsd` is the `pstat` component for FreeBSD: it samples
resource-utilization statistics from the kernel's `sysctl(3)` records and
`getifaddrs(3)` - there is no `/proc` to read (FreeBSD's `procfs` is
optional, not mounted by default, and has no node-wide files). Read the
framework [`AGENTS.md`](../AGENTS.md) first; this file only covers what is
specific to `pfreebsd`. It is the FreeBSD analog of
[`plinux`](../plinux/AGENTS.md) and [`pmacos`](../pmacos/AGENTS.md); a
given host builds exactly one of the three.

## Files

| File | Contents |
|------|----------|
| `pstat_pfreebsd.h` | Declares the component and module symbols. |
| `pstat_pfreebsd_component.c` | Component struct + `pstat_pfreebsd_component_query`. |
| `pstat_pfreebsd_module.c` | The module: `query`, the periodic `update`, and the four FreeBSD `*_stat` readers. |
| `configure.m4` | Gates the build to FreeBSD 12 and later. |

## Component and build gating

The bare `pmix_pstat_base_component_t`, whose query is unconditional at
run time and returns the module at **priority 80** - the same as `plinux`
and `pmacos`, which never build here. `configure.m4` sets the component
happy only when `oac_found_freebsd` is `yes` and a compile check finds the
records the readers use (`kinfo_proc.ki_pctcpu`, `devstat.busy_time`,
`struct xswdev`) under `__FreeBSD_version >= 1200000`. Everything comes
from libc, so there is nothing to link.

## Module (`pstat_pfreebsd_module.c`)

`init`/`finalize` are no-ops. `query`, `monitor_fields` and `update` are
the same code as in `plinux` and `pmacos`, copied verbatim; see the
`pmacos` guide for why that matters and how to check it. Only the four
readers are FreeBSD's.

- **`proc_stat`** - one `KERN_PROC_PID` sysctl returns the process's
  `kinfo_proc`: owner (`ki_uid`, the effective uid), command name
  (`ki_comm`), state (`ki_stat`, mapped to the letter `plinux` emits;
  `SWAIT` and `SLOCK` take ps(1)'s `W` and `L`), CPU time (`ki_runtime`,
  microseconds), %CPU (`ki_pctcpu` / `FSCALE`, as ps(1) computes it),
  priority (`ki_pri.pri_level - PUSER`, as ps(1) shows it),
  thread count, last processor (`ki_lastcpu`), and virtual and resident
  sizes. The kernel fills the record from the process in one pass, so -
  unlike `pmacos`, which reads twice through libproc and must compare the
  start time again - the owner checked against `tgt->owner` is the owner
  of the numbers reported, and there is no second read to race a reused
  pid. A pid with no process answers `ESRCH`, and so does another user's
  process to a server that is not root while `security.bsd.see_other_uids`
  is `0`; both are `PMIX_ERR_NOT_FOUND`, which `update()` skips. `pss` and
  `pkvsize` have no FreeBSD analog and are not emitted.
- **`node_stat`** - `getloadavg`, `hw.physmem` for the total,
  `vm.stats.vm.v_free_count` for free, `vfs.bufspace` for buffers, and
  `vm.stats.vm.v_inactive_count` for cached: FreeBSD has no separate page
  cache count, and the inactive queue - which holds most file pages and is
  reclaimed first - is the closest analog, as external pages are on macOS.
  Swap is summed over the `vm.swap_info.<n>` records (`struct xswdev`, in
  pages), stopping at the `ENOENT` past the last device; a node with no
  swap reports totals of zero. `mswapcached` and `mmap` are not emitted.
- **`disk_stat`** - `kern.devstat.all`, the table `iostat(8)` reads: a
  `long` generation count and then one `struct devstat` per device, after
  checking `kern.devstat.version` is the `DEVSTAT_VERSION` compiled
  against. The kernel updates a record without a lock between
  `sequence0` and `sequence1`, so a record whose two differ was read
  mid-update; the table is read again, up to five times, until none is.
  The name is `device_name` + `unit_number` (`ada0`, `vtbd0`); `pass(4)`
  devices are skipped, as `iostat` skips them, since each is the CAM
  pass-through to a disk listed under its own name. Bytes are normalized
  to 512-byte sectors and `struct bintime` durations to milliseconds, to
  match `plinux`. `ioprog` is `start_count - end_count` and `ioms` is
  `busy_time`; the merged counts and `ioweight` have no analog. Records
  are `memcpy`d out of the buffer: nothing promises their alignment.
- **`net_stat`** - `getifaddrs`, taking the `AF_LINK` entry each interface
  has, whose `ifa_data` is a `struct if_data`. Unlike macOS, FreeBSD's
  counters there are `uint64_t` (since FreeBSD 11), so the routing-socket
  walk `pmacos` needs to avoid 32-bit wrap is not needed here.

Memory and size values are reported in MB (float), as `plinux` reports
them; disk and network counters are raw `uint64` totals.

The base's `pmix_pstat_base_pid_owner` and `pmix_pstat_base_pids_of` have
FreeBSD branches too (`KERN_PROC_PID` and `KERN_PROC_UID`), which
`PMIX_MONITOR_TARGET_PIDS` needs whatever component is selected.
