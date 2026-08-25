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
`pmix_cli_item_t` plus a `tail` of positionals). Each stored value also
carries the position at which it was given (`opt->seq[n]`, `-1` if the
value was added by the caller after the parse rather than by the parser),
because the grouped view cannot express the interleaving of two repeated
options — see `pmix_cmd_line_get_nth_seq` and `pmix_cmd_line_get_ordered`.
The stamping is done by the parser around the store call, not inside
`check_store`, so it keeps working when a caller supplies a store
function of its own.

The parser is intricate and
carries several special cases (`-v` repeat counting, `--` separator,
MCA-param option pairs, the `-np` shortcut, optional `-h` argument). One
quirk that bit `src/tools` is worth remembering: a bare invocation
(`argc == 1`) returns early with `tail == NULL` (it must *not* fall
through the `done:` copy, which would leave the program name in the
tail).

**Any arm that claims a token getopt did not consume must first check
that the token is there.** Several options are declared to getopt as
taking one argument and then take a second from `argv[optind]` on the
parser's own account — the MCA pair, `--prepend-env`/`--append-env`, the
`-np` shortcut, `--show-version`'s trailing tokens. `argv[optind]` at the
end of the array is the `NULL` that terminates it, and an arm that stored
that `NULL` as a value and stepped `optind` past it left `optind ==
argc + 1`, which the loop's end test — an equality against `argc` — did
not catch. The next iteration then read past the end of the copied array
and dereferenced what it found. The end test is now `>=`, and each of
those arms reports `not-enough-arguments` instead; `is_option_token()` is
what tells a real value from the next option. Regression cases are the
`two-token:` group in
[`test/unit/util/util_cmd_line.c`](../../test/unit/util/util_cmd_line.c),
and they die on a signal rather than failing an assertion.

**A lone `-` is not an option.** It conventionally names stdin, and getopt
agrees: it stops at a bare `-` without consuming it. The loop asked only
whether the token began with a dash, so it called getopt anyway, got the
"no more options" answer, and the arms below then reasoned about
`argv[optind - 1]` — the token *before* the dash — and reported an option
that had been given its argument as though it were missing one. A bare
`-` now ends the option list and goes to the tail, which is what
`is_option_token()` has always said it should. Regression cases are the
`bare dash:` group.

**An optional argument arrives two ways.** getopt reports the argument of
`-h` in `optarg` when it was written attached (`-htopic`,
`--help=topic`) and leaves it at `argv[optind]` when it was the next
token (`--help topic`). Only the second was ever looked at, so
`--help=version` was refused as an unrecognized option while
`--help version` worked. Both spellings are the same request; answer them
in one place.

**One parse at a time, on the tool's main thread.** `getopt_long` keeps
its state in the process-global `optind`/`optarg`/`opterr`/`optopt`, which
this parser resets on entry — so two concurrent parses corrupt each
other, no matter which `pmix_cli_result_t` each is filling. Every caller
today parses once, at startup, before any progress thread exists.

The inline helpers in the header are used by tools outside this tree
(the header is installed), and two of them have to remember that
`PMIx_Argv_split()` drops empty fields: a string that is nothing but
separators splits to **nothing at all**, not to a one-element array. So
`pmix_convert_string_to_time(":")` and `pmix_check_cli_option("-", ...)`
both had a `NULL` array to walk. Screen the count before indexing.

**The first token that is not an option ends the option list**, and the
short-option string is prefixed with `'+'` to make getopt agree with
that. Without it getopt permutes non-options to the end of argv as it
goes, so by the time the loop looked at `argv[optind]` to decide it had
reached one, they had already been moved past it: the tail began *after*
the token it should have begun with, and a lone positional was dropped
altogether while the option beyond it was parsed as though it were the
tool's. Two things depend on the `'+'`, so do not remove it — the tail
boundary, and the option-name recovery that reads `argv[optind-1]` to
see which spelling the user typed, which is only meaningful while argv
is still in the order they typed it in.

The cost is deliberate: an option placed *after* the first non-option is
no longer the tool's, it goes to the tail. That is what a launcher
needs, since the tokens after the executable are the *application's*
flags and must not be eaten. Regression cases are
`test_parse_positional_first` / `test_parse_positional_after_options` in
[`test/unit/util/util_cmd_line.c`](../../test/unit/util/util_cmd_line.c).

### `pmix_hash` — the TMA-aware datastore helper

Backs the `gds` framework. Stores `pmix_dstor_t` values per rank in a
`pmix_hash_table_t`, keyed by an integer *keyindex* (`pmix_hash_lookup_key`
auto-registers unknown string keys). Values can carry **qualifiers**
(a `pmix_qual_t[]` in a side pointer-array). All allocation goes through
the table's **TMA** (`pmix_tma_*` off `pmix_obj_get_tma`) so the same code
serves both the normal-heap and shared-memory (`gds/shmem3`) cases —
never call raw libc `malloc`/`free` on data that lives in a hash table.
When constructing the qualifier array, remember the count of *actual*
qualifiers (`PMIX_INFO_IS_QUALIFIER`) can be smaller than `nquals`: index
the destination array by the compacted counter, not the loop variable
(this was the bug fixed in July 2026 — see below).

### `pmix_basename` — two implementations, one of them invisible

`pmix_dirname()` has two bodies behind a configure-time `#if`. Where
`dirname(3)` exists — which is everywhere PMIx is actually built — it
strdups the input, hands it to libgen, and strdups the answer back. The
`#else` is a hand-rolled walk that no ordinary build ever compiles.

Two consequences worth keeping in mind:

- **The branch you can read is not the branch you are running,** and a
  divergence between them is invisible to a build that only checks that
  the code compiles. They *did* diverge: the fallback answered `"."` for
  a path that is nothing but separators (`"/"`, `"//"`) where libgen
  answers the root. `util_basename.c` now pins the whole answer table for
  whichever branch is compiled, so a future edit to either one has to
  keep them agreeing. If you change one body, run the corpus against the
  other by hand before you believe it.
- **`dirname(3)` is not reentrant on every platform.** glibc rewrites the
  buffer you hand it, but macOS and the older BSDs return a pointer into
  internal static storage, so two threads inside `pmix_dirname()` at once
  can each strdup the other's answer. Nothing in the tree does that
  today, and the reason is worth preserving rather than rediscovering:
  the `ptl` caller (`write_rndz_file`) is reachable only from
  `pmix_ptl_base_setup_listener`, which runs once while the process is
  still initializing, and the remaining callers (`pmix_iof`, `pmix_path`)
  run on the progress thread. **Do not add a `pmix_dirname()` call on a
  thread that can run concurrently with the progress thread** without
  first replacing the libgen branch — the `#else` body is already a
  correct, reentrant implementation of the same function.

### `pmix_argv` — copies must not write through the original

Everything in this file that is named "copy" or "insert" takes its input
by value, and two of them used to cheat:

- `pmix_argv_copy_strip()` marked the end of a quoted element by punching
  a temporary NUL into the *caller's* string and putting the quote back
  afterwards. That faults outright on an argv whose elements are string
  literals — an entirely ordinary thing to hand a function that promises
  a copy — and it is not safe against a second thread reading the same
  array. It now measures the surviving span and copies that out.
  `test_copy_strip_readonly_source` in `util_argv.c` is the regression:
  it dies on a signal, not with a failed assertion, if this comes back.
- `pmix_argv_insert()` and `pmix_argv_insert_element()` grew the target
  and shifted its suffix *before* strdup'ing the source in. A failed
  strdup then left a NULL in the middle of the array — which terminates
  it, so every element past the hole is at once lost to the caller and
  leaked — and the functions reported `PMIX_SUCCESS` anyway. Both now
  copy first and only touch the target once the copies are in hand, so
  the failure is reported with the target untouched. Keep that order.

Note also that `pmix_argv_append_unique_idx()` is documented as the
public `PMIx_Argv_append_unique_nosize()` plus an index out-parameter,
and callers are entitled to that: it screens `NULL` inputs the same way
rather than handing a `NULL` arg to `strcmp`.

### `pmix_context_fns` — moving the process to where an app runs

Two helpers that `pmix_pfexec` calls to place a child: one `chdir()`s to
the app's working directory, the other resolves its executable to a full
path. Three things about them are easy to get wrong.

- **Both out-parameters are owned in and owned out.** Each takes a
  `char **`, frees what it finds there, and writes back a fresh
  allocation. A caller must own the string it passes and must free what
  comes back — and neither may be handed a string literal.
- **`chdir()` moves the process, not a thread, and this runs in the
  parent.** `pmix_pfexec`'s `setup_path()` calls these *before* the fork,
  so honoring an app's `cwd` changes the **server's** working directory
  for everything that follows. `pmix_util_check_context_app()` depends on
  exactly that: a relative executable is checked with `access()`, which
  resolves against the process's current directory and not against the
  `cwd` argument, which is only used as the base for the `PATH` search of
  a *naked* filename.
- **Ask `pmix_home_directory()` by `geteuid()`, never by a sentinel.**
  When the app's directory is unreachable and the user did not choose it,
  these fall back to `$HOME`. That call used to pass `-1`, and both
  spellings do take the `getenv("HOME")` shortcut — but they part company
  the moment `$HOME` is unset, which is when the fallback matters:
  `pmix_home_directory()` then looks up the passwd entry, and there is no
  passwd entry for `(uid_t)-1`. The fallback therefore never fired in the
  one case it exists for, and the caller was handed back the name of a
  directory the process is not in and cannot reach. `getpwuid()` is also
  not reentrant, so like `pmix_dirname()` these must not be called on a
  thread that runs concurrently with the progress thread.
  `test_check_cwd_home_fallback_no_HOME` in
  [`test/unit/util/util_context_fns.c`](../../test/unit/util/util_context_fns.c)
  pins it by unsetting `$HOME`.

### `pmix_net` / `pmix_if` — network helpers

`pmix_net` is pure address math (CIDR→netmask in *network* order,
localhost/link-local/public classification, `isaddr`). `pmix_if` owns the
discovered-interface list (populated by the `src/mca/pif/*` components —
this file only *consumes* it) and the `a.b.c.d[/mask]` tuple parser
`pmix_iftupletoaddr`, which returns masks in **host** order. Mind that
byte-order difference if you ever mix the two.

### `pmix_shmem` — a created segment reads as zero

`pmix_shmem_segment_create()` opens its backing file `O_CREAT | O_TRUNC`
and `ftruncate`s it to the full size, so a freshly created segment is a
sparse file whose every page reads as zero. **That is a contract, not an
incidental property**: `gds/shmem3`'s bump allocator relies on it to hand
out zeroed storage without writing a byte (see `tma_carve()` and
[`src/mca/gds/shmem3/AGENTS.md`](../mca/gds/shmem3/AGENTS.md)), which is
what keeps building a segment from faulting in its whole — heavily
over-estimated — extent up front.

The `O_TRUNC` is the load-bearing half. Segments are unlinked when their
last holder lets go, so a path collides only with a file left behind by a
server that died; but the path carries a pid, and pids get reused, and
`ftruncate()` to the same or a smaller size would leave that file's bytes
in place. There is exactly one caller of this function and it always
populates the segment from scratch, so the truncate costs nothing.

### `pmix_vmem` — reserving an address before you need it

`pmix_vmem_find_hole()` answers "where could I map something?", which is
only useful if you map it immediately, and useless if the process that
has to map at the *same* address is a different one that will get there
later. `pmix_vmem_reserve()` / `_reserve_at()` / `_restore()` /
`_release()` exist for that second case: claim the range now, with an
inaccessible `PROT_NONE` anonymous mapping that commits nothing, and map
over it later with `MAP_FIXED` — which cannot fail for want of the
address, because you have been holding it.

Two rules for a caller:

- **Give the range back with `pmix_vmem_restore()`, not `munmap()`,** when
  you are done with something you mapped inside it. Unmapping punches a
  hole in the reservation and hands the address to whatever asks next.
  `pmix_shmem_segment_detach()` does this for you when the segment was
  attached with `PMIX_SHMEM_MAP_OVER_RESERVATION`.
- **Ask `pmix_shmem_utils_segment_footprint()` where a segment ends.** It
  is not the size you asked for — a segment carries a page-aligned header
  ahead of its data — so placing segments back to back by the requested
  size overlaps each with the next.

`pmix_gds_shmem3` is the caller, and
[its AGENTS.md](../mca/gds/shmem3/AGENTS.md) explains the whole design
under "The address-space arena". Note also `VMEM_HOLE_BIGGEST_OFFSET`,
which exists because the midpoint that `use_hole()` picks is the same
midpoint hwloc and Open MPI pick — see the comment on `use_hole_offset()`
for why the top of the hole is a worse answer than either.

The `_scattered` variants exist because determinism cuts both ways:
computing the same answer from the same map is what lets two processes
agree without talking, and is also what makes two *unrelated* callers
collide. A scatter value derived from something that differs between
them (for `shmem3`, the namespace) displaces the result within a window
around the chosen placement, without giving up the agreement. Tested in
[`test/unit/util/util_vmem.c`](../../test/unit/util/util_vmem.c) — which
is where it has to be tested, since ASLR separates real processes well
enough to hide whether the scatter works at all.

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

A second pass then cleared the remaining latent/robustness items:

- **`pmix_show_help.c` `get_content` `#include` parser.** The
  `project == NULL` first branch mis-parsed an `#include#FILE#TOPIC` line
  (`strrchr` re-found the same `#`, never NUL-terminated, so `file == topic`).
  Now strdups the line and parses each `#`-delimited field like the second
  (correct) branch, defaulting the project to `pmix`. Unreachable today (no
  `help-*.txt` uses `#include`) but now correct if one does.
- **`pmix_path.c`.** `pmix_path_df` checked the sign of `f_bavail` after
  truncating it to `int` (could report 0 free on a large filesystem) — now
  uses an `int64_t` sign test. `pmix_path_nfs` set `*fstype` on false
  returns (a leak, against its "valid only if true" contract) and never
  NULL-checked the pointer — now sets it only on the true path, guarded.
  `pmix_path_findv` leaked the partial `dirv` on a `strdup` OOM — now frees it.
- **`pmix_output.c`.** `open_file` assembled the filename with an unbounded
  `strcat` chain into a `PMIX_PATH_MAX` buffer — now a single bounded
  `snprintf`. `pmix_output_reopen_all` called `gethostname` without
  guaranteeing NUL termination — now matches the init path.
- **`pmix_environ.c`.** The include/exclude matcher treated a trailing `*`
  as a wildcard but matched every entry as a prefix regardless, so `"PATH"`
  also matched `"PATHFINDER"`. Now a bare name is an exact match (bounded at
  the `=`/NUL) and only a trailing `*` prefix-matches — matching the
  documented `"OMPI_*,OPAL_*"` convention. The exclusion loop also now skips
  non-`PMIX_ENVAR` kvals before reading `data.envar`.
- **OOM robustness / doc drift.** `pmix_os_path`, `pmix_os_dirpath_create`,
  `pmix_path_findv`, and the `pmix_name_fns` TSD allocator now NULL-check
  their allocations (the last unwinds partially-allocated buffers).
  `pmix_getcwd` also NULL-checks `pmix_basename` before copying; its header
  referenced a nonexistent `PMIX_ERR_TEMP_OUT_OF_RESOURCE` — the doc was
  corrected to the real `PMIX_ERR_OUT_OF_RESOURCE`. `pmix_os_dirpath_destroy`
  now returns `PMIX_ERR_NOT_FOUND` (not `PMIX_ERROR`) for a missing
  directory, per its header.
- **`pmix_printf.c`.** The `!HAVE_VASPRINTF` fallback `guess_strlen` read
  `double`/`long` varargs via `va_arg(ap, int)` — now uses the correct
  `double`/`long` types. Dead on modern platforms, but no longer UB.
- **`pmix_tty.c`.** `pmix_settermios` verified a set via a full-struct
  `memcmp` of `struct termios` (padding / canonicalized fields → spurious
  failures) — now compares the individual POSIX fields (`c_iflag`,
  `c_oflag`, `c_cflag`, `c_lflag`, `c_cc`).

## Known issue left as-is (by design)

- **`pmix_alfg.c`.** `pmix_srand(buff, seed)` also copies the seeded state
  into the file-static `alfg_buffer` that `pmix_random()` reads. This looks
  like a footgun (two callers seeding their own buffers stomp the shared
  global), but it is the *only* way to seed `pmix_random`'s global — and
  `util_alfg.c` deliberately tests that `pmix_srand` + `pmix_random` is
  deterministic. It is left unchanged: `pmix_random` is unused in-tree and
  the two real `pmix_srand` callers (`pnet/tcp`, `pnet/opa`) use their own
  buffers and never call `pmix_random`, so the "last writer wins" hazard
  is not actually reachable. Do not "fix" it by dropping the global copy
  without also giving `pmix_random` another way to be seeded.

## When in doubt

- These are leaf utilities — match the surrounding function's style and
  keep them free of hidden global state and thread-shifts.
- Prefer a `make check`-able pure unit test over a manual check; the
  `test/unit/util` harness makes it cheap.
- Regenerate `pmix_show_help_content.*` after touching any help text.
- Don't hand-edit generated files (`pmix_show_help_content.c`,
  `keyval/keyval_lex.c`).
