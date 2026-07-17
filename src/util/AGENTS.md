# AGENTS.md: `src/util` — the PMIx low-level utility layer

This document orients AI agents and human contributors working in
`src/util`. It assumes you have already read the top-level
[`AGENTS.md`](../../AGENTS.md) — the golden rules (prefix conventions,
`pmix_config.h`-first include order, constant-on-the-left comparisons,
brace-everything, `#define` logical macros to 0/1, warning-free under
`--enable-devel-check`), the thread-safety / progress-thread / caddy
model, and the copyright-header and contribution rules all apply here and
are not repeated. This file covers what is specific to `src/util`.

## What this directory is

`src/util` is the **grab-bag of low-level, mostly self-contained helper
code** that the rest of PMIx builds on: argv/string/environment munging,
path and filesystem helpers, the output/verbose subsystem, the
`show_help` machinery, the command-line parser, network/interface
utilities, the TMA-aware hash datastore helper, a flex-based keyval
parser, shared-memory/virtual-memory helpers, an RNG, and profiling.

Unlike `src/mca/*`, **this is not an MCA framework** — there are no
components, no selection logic, no `configure.m4`. Everything here
compiles into the convenience library `libpmix_util.la` (plus the
`keyval/` sub-library `libpmixutilkeyval.la`), which is absorbed into
`libpmix`. A change here therefore takes effect with a plain top-level
`make` from an already-configured tree; you only need
`autogen.pl`/`configure` if you add or remove a source file (adding a
file only needs `make`, which regenerates the `Makefile`).

Most functions here are **pure-ish leaf utilities** that do not touch
shared library state and do not thread-shift, so they are safe to call
from anywhere (including inside the library). The notable stateful
exceptions — which keep process-global state and rely on the caller
serializing (in practice, the progress thread) — are `pmix_output`,
`pmix_show_help`, `pmix_keyval_parse`, and `pmix_if` (the discovered
interface list).

## File map

| File | Purpose | Testable? |
|------|---------|-----------|
| `pmix_argv.{c,h}` | argv-array helpers *not* in the public `PMIx_Argv_*` set (append-unique, join-range, delete, insert, copy-strip). The core `PMIx_Argv_*` primitives live in `src/mca/bfrops/base`. | pure |
| `pmix_environ.{c,h}` | environ-array merge/get/unset, `$TMPDIR`/`$HOME` resolution, envar harvesting | pure (some touch `environ`/`getpwuid`) |
| `pmix_string_copy.{c,h}` / `pmix_strnlen.h` | always-NUL-terminating bounded copy; `pmix_getline`; `PMIX_STRNLEN` | pure |
| `pmix_basename.{c,h}` | OS-independent `basename`/`dirname` (fresh allocations) | pure |
| `pmix_parse_options.{c,h}` | numeric range-string expansion (`"1,3-5"` → argv) | pure |
| `pmix_keyval_parse.{c,h}` + `keyval/keyval_lex.l` | flex parser for `key = value` / `-mca` / `-x` config files | needs a temp file + callback |
| `pmix_cmd_line.{c,h}` | the `getopt_long`-based CLI parser used by `src/tools` | partial (see `util_cmd_line`) |
| `pmix_output.{c,h}` | the generalized output/verbose stream subsystem (≤64 streams) | partial |
| `pmix_show_help.{c,h}` + `pmix_show_help_content.c` + `convert-help.py` | help-message lookup/aggregation; content compiled from `help-*.txt` | partial |
| `pmix_printf.{c,h}` | portable `asprintf`/`snprintf` wrappers | pure |
| `pmix_error.{c,h}` | `PMIx_Error_string` / `PMIx_Error_code` over the event-strings table | pure |
| `pmix_name_fns.{c,h}` | `[nspace,rank]` printing (rotating TSD buffers); proc compare | pure |
| `pmix_net.{c,h}` | IP address parsing/classification, CIDR→netmask | pure |
| `pmix_if.{c,h}` | discovered-interface list + lookups; the `a.b.c.d/mask` tuple parser | tuple parser is pure; list lookups need fixtures |
| `pmix_hash.{c,h}` | TMA-aware key/value datastore used by `gds` (store/fetch/remove, keyindex, qualifiers) | needs `PMIx_server_init` |
| `pmix_path.{c,h}` / `pmix_os_path.{c,h}` / `pmix_os_dirpath.{c,h}` / `pmix_getcwd.{c,h}` | path search/resolution, path assembly, dir-tree create/destroy, cwd | `os_path` pure; rest need a tmpdir |
| `pmix_fd.{c,h}` | fd read/write, cloexec, type predicates, peer name, mass-close | needs pipes/sockets |
| `pmix_few.{c,h}` | fork/exec/waitpid a child | needs a child |
| `pmix_getid.{c,h}` | peer uid/gid over a socket (`SO_PEERCRED`/`getpeereid`) | needs a socketpair |
| `pmix_shmem.{c,h}` / `pmix_vmem.{c,h}` | mmap-backed shared-memory segment; `/proc/self/maps` hole finder (Linux) | `pad_to_page`/`parse_map_line` pure; rest need mmap |
| `pmix_pty.{c,h}` / `pmix_tty.{c,h}` | pty open/forkpty; termios/winsize helpers | need real pty/tty |
| `pmix_alfg.{c,h}` | additive lagged-Fibonacci RNG (from Open MPI's `opal_rand`) | pure/deterministic |
| `pmix_timings.{c,h}` | optional profiling (`--enable-timing`, off by default) | needs `--enable-timing` |
| `pmix_context_fns.{c,h}` | process-launch cwd/exe resolution helpers | needs a fixture |

## Subsystems that deserve a closer look

### `pmix_output` — the verbose/output engine

A fixed array of up to `PMIX_OUTPUT_MAX_STREAMS` (64) stream descriptors,
each able to fan a message out to stdout/stderr/syslog/a file. The
`pmix_output_verbose(level, id, ...)` macro is the hot entry point;
guard expensive verbose output behind it. `make_string` (static) is where
a formatted line gets its prefix/suffix and trailing newline assembled —
subtle indexing, so read it before touching. This subsystem is
process-global and assumes single-threaded (progress-thread) use.

### `pmix_show_help` — help text is compiled *into the library*

Help content does **not** come from the `help-*.txt` files at runtime.
`convert-help.py` scans every `help-*.txt` in the tree and generates
`pmix_show_help_content.c` (a big static table) which is compiled into
`libpmix`. So after **any** add/delete/modify of `show_help` content you
must regenerate the pair, exactly as the top-level guide says:

```sh
rm src/util/pmix_show_help_content.* && make
```

A plain `make` will *not* pick up changed help text. `pmix_show_help`
also aggregates duplicate notices (fired from a libevent timer) and can
thread-shift delivery through the log path; the global `abd_tuples` list
is manipulated without locking and assumes progress-thread-only callers.

### `pmix_cmd_line` — the CLI parser

A `getopt_long` wrapper that copies argv (getopt reorders in place),
walks options, and stashes results in a `pmix_cli_result_t` (a list of
`pmix_cli_item_t` plus a `tail` of positionals). It is intricate and
carries several special cases (`-v` repeat counting, `--` separator,
MCA-param option pairs, the `-np` shortcut, optional `-h` argument). Two
parser quirks that bit `src/tools` and are worth remembering: a bare
invocation (`argc == 1`) returns early with `tail == NULL` (it must *not*
fall through the `done:` copy, which would leave the program name in the
tail), and a positional placed *before* an option is dropped by getopt's
reordering — put options first.

### `pmix_hash` — the TMA-aware datastore helper

Backs the `gds` framework. Stores `pmix_dstor_t` values per rank in a
`pmix_hash_table_t`, keyed by an integer *keyindex* (`pmix_hash_lookup_key`
auto-registers unknown string keys). Values can carry **qualifiers**
(a `pmix_qual_t[]` in a side pointer-array). All allocation goes through
the table's **TMA** (`pmix_tma_*` off `pmix_obj_get_tma`) so the same code
serves both the normal-heap and shared-memory (`gds/shmem2`) cases —
never call raw libc `malloc`/`free` on data that lives in a hash table.
When constructing the qualifier array, remember the count of *actual*
qualifiers (`PMIX_INFO_IS_QUALIFIER`) can be smaller than `nquals`: index
the destination array by the compacted counter, not the loop variable
(this was the bug fixed in July 2026 — see below).

### `pmix_net` / `pmix_if` — network helpers

`pmix_net` is pure address math (CIDR→netmask in *network* order,
localhost/link-local/public classification, `isaddr`). `pmix_if` owns the
discovered-interface list (populated by the `src/mca/pif/*` components —
this file only *consumes* it) and the `a.b.c.d[/mask]` tuple parser
`pmix_iftupletoaddr`, which returns masks in **host** order. Mind that
byte-order difference if you ever mix the two.

### `keyval/` — the flex lexer

`keyval_lex.l` is the flex source; `keyval_lex.c` is a **generated build
product** — never hand-edit it, edit the `.l` and let the build
regenerate. `pmix_keyval_parse` drives it over a real `FILE*` under
`keyval_mutex` with process-global scratch buffers.

## Build wiring

- `Makefile.am` builds `noinst` `libpmix_util.la` from the `headers` +
  `sources` lists and pulls in `keyval/libpmixutilkeyval.la` via
  `LIBADD`. Adding a source means adding it to `sources` (and `headers`
  if it ships a public internal header, which are installed into
  `$(pmixincludedir)/src/util`).
- `pmix_show_help_content.c` is generated by the `convert-help.py` rule
  and is a `clean-local` / `distclean` / `MAINTAINERCLEAN` target — it is
  in `.gitignore`, never commit it.
- Editing only `Makefile.am` needs just `make`; adding/removing a source
  file also only needs `make` (it regenerates the `Makefile`). You only
  need the full `autogen.pl && configure` if you touch `configure.ac` /
  `config/*.m4`.

## Testing

Unit tests live in [`test/unit/util`](../../test/unit/util) and are wired
into `make check`. Each is a standalone `main()` that links
`libpmix.la`, runs a set of `report(name, passed)` assertions, prints a
pass/fail tally, and returns non-zero on any failure. To add one:

1. Write `util_<thing>.c` following the existing pattern (copyright
   header, `report()` helper, `PMIX_HIDE_UNUSED_PARAMS(argc, argv)`).
2. Add it to `check_PROGRAMS`, `TESTS`, a `*_SOURCES/_LDFLAGS/_LDADD`
   block, and the `clean-local` list in `test/unit/util/Makefile.am`.
3. Add the binary name to `test/unit/util/.gitignore`.
4. `make check` from `test/unit/util`.

**Prefer testing the pure leaf functions** — they need no server. A few
helpers (`pmix_hash`) need `PMIx_server_init` because they copy values
through the `bfrops` MCA framework (`util_hash.c` shows the pattern).
Things that need real OS resources (pty, tty, shmem, `getid`, `few`) are
integration-style; only the arithmetic/parsing parts (`pad_to_page`,
`parse_map_line`, `pmix_alfg` determinism) are unit-friendly.

Current coverage includes: `argv`, `alfg`, `basename`, `cmd_line`,
`context_fns`, `environ`, `error`, `hash` (incl. a mixed-qualifier
regression), `if` (the tuple parser), `name_fns` (incl. special-rank
compare), `net`, `os_dirpath`, `os_path`, `output`, `parse_options`
(incl. the bare-`-` crash regression), `path`, `printf`, `show_help`,
`string_copy`, `timings`.

## Fixed defects (July 2026 review)

A deep review of this directory fixed the following, each landed as its
own commit. Recorded so they are not re-introduced by a future edit.

- **`pmix_hash.c` qualifier array — heap overflow + wrong index.** When
  the info array mixes non-qualifier and qualifier entries, the store
  loop wrote `qarray[n].index` (loop var, indexing all infos) instead of
  `qarray[m].index` (compacted qualifier counter) — an out-of-bounds
  write past the `m`-element array, leaving the real slot's index
  uninitialized so qualified fetches silently failed to match. Also
  `calloc` the array so an error-path `erase_qualifiers()` never releases
  an uninitialized `value` pointer. Regression test: `util_hash.c`.
- **`pmix_if.c` tuple parsing.** `pmix_iftupletoaddr` swallowed a
  malformed dotted netmask (the error rc was overwritten by the network
  parse — now returns immediately); rejected valid `/0` and `/32`
  prefixes and did a shift-by-32 (UB) — now accepts `0..32` with a
  guarded shift; `parse_ipv4_dots` accepted octets that wrap a `uint32_t`
  — now range-checks before truncating; `isalpha()` was called on a
  possibly-negative `char` — now cast to `unsigned char`; `pmix_ifindextomask`
  did an unclamped `memcpy(length)` — now `MIN`-clamped like its siblings.
  Regression test: `util_if.c`.
- **`pmix_fd.c`.** `pmix_fd_get_peer_name` used a bare `struct sockaddr`
  (16 bytes) for an IPv6 peer → truncated address + out-of-bounds stack
  read; now uses `struct sockaddr_storage`. `pmix_close_open_file_descriptors`
  tested `__OSX__` (never defined) instead of `__APPLE__`, so macOS
  always took the slow O(fdmax) close path.
- **`pmix_path.c`.** `pmix_find_absolute_path` `free()`d the caller's
  `app_name` when `realpath` failed on an already-absolute input (the
  success path guards this; the failure path did not).
- **`pmix_name_fns.c`.** `pmix_util_compare_proc` returned
  `a->rank - b->rank` on `uint32_t` ranks — the mod-2³² result has the
  wrong sign when ranks differ by more than `INT_MAX`, mis-ordering sorts
  that mix normal ranks with the special top-of-range ranks
  (`PMIX_RANK_WILDCARD`, `…UNDEF`). Now an explicit three-way compare.
  Regression test: `util_name_fns.c`.
- **`pmix_environ.c`.** `pmix_home_directory` dereferenced a NULL
  `getpwuid()` result (reachable in a container whose passwd lacks the
  UID, with `HOME` unset); now NULL-checked. `pmix_util_harvest_envars`
  indexed `var[len-1]` without guarding an empty include/exclude var
  (`size_t` underflow → OOB read); both loops now guard `0 < len`.
- **`pmix_parse_options.c`.** `pmix_util_parse_range_options` dereferenced
  a NULL `r2[0]` on a bare `"-"` token (`PMIx_Argv_split("-", '-')`
  returns NULL); reachable from user port-range strings. Now guarded.
  Regression test: `util_parse_options.c`.
- **`pmix_show_help.c`.** `pmix_show_accumulated_duplicates` leaked the
  two `pmix_asprintf`'d `tmp` buffers on every duplicate-help flush
  (`local_delivery` copies the message, it does not take ownership).
- **`pmix_output.c`.** `make_string` indexed `[len-1]` on an empty
  formatted string (`SIZE_MAX` read); now guards `0 == len`.
  `pmix_output_init` returned `PMIX_ERR_NOMEM` (truthy) from a `bool`
  function on `asprintf` failure — reported success; now `false`.
- **`pmix_timings.c`.** Both output paths did
  `snprintf(buf, PMIX_TIMING_STR_LEN, "%s%s", buf, line)` — aliasing
  `buf` as both source and destination (UB) and capping the buffer at
  1024 rather than the size it was allocated for. Now appends at the
  current end with the real remaining capacity. (`--enable-timing` only.)
- **`pmix_pty.c`.** The `PMIX_ENABLE_PTY_SUPPORT == 0` stubs had a missing
  semicolon and signatures that disagreed with the header (`pmix_ptymopen`
  dropped `maxlen`; `pmix_forkpty` had extra/concrete params), and the
  hand-rolled `pmix_openpty` fallback called `pmix_ptymopen(line)` without
  `maxlen` and used `pmix_string_copy` without including its header —
  compile failures in those (CI-unexercised) build configs.
- **`pmix_keyval_parse.c`.** `isspace()` on a possibly-negative `char`
  (project portability rule) → cast to `unsigned char`; `trim_name`'s
  suffix back-scan could step before the buffer on an all-whitespace
  value → now bounded at `buffer`.
- **`pmix_shmem.c`.** `pmix_shmem_segment_create` sized the backing store
  as `pad_to_page(size + sizeof(header))`, but the data region starts a
  full page in (`data_addr_from_base` rounds the header up to a page), so
  for any non-page-multiple `size` the tail of the requested range fell
  past the end of the mapping. Now `pad_to_page(header) + pad_to_page(size)`.
  Masked today only because the sole caller pre-pads to a page multiple.

## Known / latent issues not yet fixed

These were surfaced by the same review but left as-is (latent,
config-gated, or by-design). Fix opportunistically, as separate commits.

- **`pmix_show_help.c` `get_content` `#include` parser (latent).** The
  first branch (the `project == NULL` path) mis-parses an
  `#include#FILE#TOPIC` content line — `strrchr` re-finds the same `#`,
  and it never NUL-terminates, so `file` == `topic`. It is unreachable
  today because no `help-*.txt` uses `#include`, but it will silently
  break the moment one does. The second branch is the correct reference.
- **`pmix_path.c`.** `pmix_path_df` casts a 64-bit `f_bavail` to `int`
  for its sign test → can report 0 bytes free on very large filesystems.
  `pmix_path_nfs` sets/leaks `*fstype` even when returning `false` and
  never NULL-checks `strdup`/`fstype`.
- **`pmix_output.c`.** `open_file` does an unbounded `strcat` chain into a
  `PMIX_PATH_MAX` buffer (pathological `TMPDIR`); `pmix_output_reopen_all`
  uses `gethostname` without guaranteeing NUL termination (the init path
  guards it).
- **`pmix_environ.c`.** A non-wildcard include like `"PATH"` prefix-matches
  `"PATHFINDER=…"` (`strncmp` over the key length); the exclusion loop
  reads `data.envar` without checking `type == PMIX_ENVAR`.
- **OOM robustness / doc drift.** Several helpers (`pmix_os_path`,
  `pmix_os_dirpath_create`, `pmix_path_findv`, `pmix_name_fns` TSD alloc)
  don't NULL-check `malloc`/`calloc`/`strdup`; `pmix_getcwd` and
  `pmix_os_dirpath_*` return codes disagree with their header docs.
- **`pmix_alfg.c`.** `pmix_srand` unconditionally clobbers the file-static
  global `alfg_buffer` even when seeding a caller-supplied buffer, and
  `pmix_random()` (unused today) is never auto-seeded.
- **`pmix_printf.c`.** The `!HAVE_VASPRINTF` fallback `guess_strlen` reads
  `double`/`long` args via `va_arg(ap, int)` (UB) — dead on any modern
  platform, but wrong.
- **`pmix_tty.c`.** `pmix_settermios` verifies via a full-struct `memcmp`
  of `struct termios`, which can spuriously fail across libc/kernel
  combos (padding / canonicalized fields).

## When in doubt

- These are leaf utilities — match the surrounding function's style and
  keep them free of hidden global state and thread-shifts.
- Prefer a `make check`-able pure unit test over a manual check; the
  `test/unit/util` harness makes it cheap.
- Regenerate `pmix_show_help_content.*` after touching any help text.
- Don't hand-edit generated files (`pmix_show_help_content.c`,
  `keyval/keyval_lex.c`).
