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
subtle indexing, so read it before touching.

**Writing is what a second thread may do; changing the table is not.**
Every line is rendered on the stack and delivered with one `write()`, so
the `ptl` listener thread and the progress thread can both write without
a lock — which is just as well, since there is no lock anywhere in the
file. Everything that *changes* the table (`init`, `open`, `reopen`,
`close`, `finalize`, `set_output_file_info`) is startup/shutdown work and
must be serialized by the caller; two concurrent `pmix_output_open()`
calls can settle on the same free slot. The header used to promise
serialization the code has never implemented; it now states this.

Four things about a stream's lifetime are easy to get wrong, and were.

- **A disabled stream is still an open stream.** `pmix_output_switch()`
  is documented as discarding output without closing anything, but
  `pmix_output_close()` and `free_descriptor()` both refused to act on a
  stream whose `ldi_enabled` was false — so a stream that had been
  switched off, or opened with `lds_is_debugging` in a build that is not
  `--enable-debug`, could never be closed: its `strdup`'ed prefix,
  suffix, syslog identity and file suffix were stranded, its file stayed
  open, and its slot stayed `ldi_used` forever. Sixty-four of those
  exhaust `pmix_output_open()`. The guards now test `ldi_used` alone;
  `test_output_close_disabled_reclaims_slot` in
  [`test/unit/util/util_output.c`](../../test/unit/util/util_output.c)
  pins it.
- **Two streams may name the same file, and each needs its own
  descriptor.** `open_file()` deliberately does not open a file a second
  time — the second open carries `O_TRUNC` and the two independent
  offsets would overwrite each other — so it looks for a stream already
  holding that file. It used to *alias* that stream's descriptor, which
  made the first close take the file away from everyone else still
  pointed at it: those streams then wrote into whatever descriptor the
  OS handed out next, and closed it a second time on their own way out.
  It now `dup()`s, so the file description (and its offset) is shared
  while each stream owns what it closes. The scan also `break`ed on the
  first stream whose file *differs* from ours instead of moving on to
  the next, so a matching stream further down the table was never found.
  `PMIX_OUTPUT_REDIRECT=file` is all it takes to have several file
  streams at once.
- **A file that will not open yet is not a failure.** The header
  promises that output to a stream whose session directory does not
  exist is counted as lost and that the open is retried on every later
  write. The failure path used to mark the slot *unused* instead, which
  silenced the stream permanently and handed its id to the next
  `pmix_output_open()` while the original holder was still using it.
- **`free_descriptor()` must reset `ldi_fd`.** The slot outlives the
  stream and gets reissued, and one arm of `do_open()` did not set the
  descriptor either, so a recycled slot could start life pointed at a
  descriptor that had been closed.

**The syslog support was dead in every build ever shipped.** All of it
was compiled behind `HAVE_SYSLOG`, and nothing in this tree defines that
macro — `configure` probes for the *header* (`HAVE_SYSLOG_H`, via the
`AC_CHECK_HEADERS` list in `config/pmix.m4`) and never for the function.
So `USE_SYSLOG` was always 0, a stream that asked for syslog was given no
destination at all, and `PMIX_OUTPUT_REDIRECT=syslog` — which also turns
stdout, stderr and file output off — silently discarded every line the
library produced. The guards now key off `HAVE_SYSLOG_H`, which is what
the `#include <syslog.h>` at the top of the file is guarded on and what
POSIX ties `openlog`/`syslog`/`closelog` to; `parse_verbose()` in
`src/mca/base/pmix_mca_base_open.c` carried the same guard and is fixed
with it, so `pmix mca base verbose = syslog` stops reporting that syslog
is unavailable on a system that has it. This is the recurring shape from
`pmix_getid`: **the macro guarding a declaration and the macro guarding
its use must be the same one, and a guard nothing defines gets no
compiler coverage** — that block had never been compiled by anyone.

Five environment variables, read once in `pmix_output_init()`, override
what every stream does: `PMIX_OUTPUT_REDIRECT` (`syslog` or `file`),
`PMIX_OUTPUT_SYSLOG_PRI`, `PMIX_OUTPUT_SYSLOG_IDENT`,
`PMIX_OUTPUT_SUFFIX` and `PMIX_OUTPUT_STDERR_FD`. They exist because this
subsystem comes up before the MCA parameter system does; they are now
documented in the header.

Two smaller contracts worth knowing: `pmix_output_open()` answers either
a handle or a *negative PMIx status*, so a caller storing the result has
to tell them apart before using it; and `pmix_output_reopen_all()` does
not reopen anything despite its name — it re-reads
`PMIX_OUTPUT_STDERR_FD` and rebuilds the `[hostname:pid]` prefix of the
default verbose stream, which is the part a restart invalidates. It used
to rebuild only the *template* and not the descriptor's own copy, so even
that did nothing. A stream someone else opened keeps the prefix its
opener gave it, and only that opener can rebuild it.

### `pmix_show_help` — help text is compiled *into the library*

Help content does **not** come from the `help-*.txt` files at runtime.
`convert-help.py` scans every `help-*.txt` in the tree and generates
`pmix_show_help_content.c` (a big static table) which is compiled into
`libpmix`. So after **any** add/delete/modify of `show_help` content you
must regenerate the pair, exactly as the top-level guide says:

```sh
rm src/util/pmix_show_help_content.* && make
```

A plain `make` will *not* pick up changed help text, and the omission
hides more than stale strings: under `--enable-devel-check` the generator
runs with `--purge`, which makes a help topic that **no code references**
a hard build error. So a change that removes the last caller of a topic
compiles and tests clean for as long as the stale generated file survives,
and breaks the build for whoever regenerates it next — which on a fresh
clone is CI. Regenerate whenever you add, remove, or rename a
`pmix_show_help*()` call, not only when you edit the text.

`pmix_show_help` also aggregates duplicate notices (fired from a libevent
timer) and can thread-shift delivery through the log path; the global
`abd_tuples` list is manipulated without locking and assumes
progress-thread-only callers.

**The header describes a system that no longer exists**, or rather it
did: it walked the reader through opening a file in `$pkgdatadir` and
told an MCA component to install its own helpfile there. Nothing opens
anything at run time, so following it produced a file PMIx never reads.
It now says what actually happens, including the regenerate step and the
`--purge` trap above, and `pmix_show_help_add_data()` — documented as
adding a *search directory* — now says what it takes, which is a
compiled-in table that it borrows rather than copies.

**The aggregation state is process-global and outlives a finalize**, the
same shape as `pmix_keyval_parse`'s `env_str` below. `pmix_show_help_init()`
can run again in the same process — `pmix_rte_init()` calls it, several
tools call it directly, and `pmix_show_help_finalize()` clears the
initialized flag — so a `show_help_timer_set` left standing meant the
aggregation timer was never armed again for the rest of the process.
Finalize now deletes the timer and resets the flag and the timestamp.

**Finalize is also the only place the promised termination flush can
happen.** `pmix_help_check_dups()` documents that accumulated duplicates
are displayed unconditionally at termination, but the summary only ever
ran off a five-second timer, and finalize destructed the list without
looking at it — so a job that ended sooner, which is most of them, was
never told about the other N processes at all. It now flushes, and it
clears `pmix_show_help_initialized` *first* so the notice takes
`local_delivery()`'s direct-to-stderr path: `pmix_rte_finalize()` has
already paused the progress thread by the time this runs, so anything
thread-shifted onto the event base here would neither run nor be
released. `test_duplicates_flushed_at_finalize` in
[`test/unit/util/util_show_help.c`](../../test/unit/util/util_show_help.c)
covers it, and has to fork before the test's own `PMIx_Init` — a child of
an already-initialized process only adjusts a reference count and never
reaches `pmix_rte_finalize()` at all.

**A message can pull in another one**, with a line of the form
`#include#FILE#TOPIC` or `#include#PROJECT#FILE#TOPIC`, parsed from the
right. No `help-*.txt` in the tree uses it yet, which is why both copies
of the parser — the local-array branch and the registered-array branch,
which were the same twenty lines twice — dereferenced whatever
`strrchr()` returned. A line with too few fields, `#include` on its own
being the easy one to write, is a NULL dereference, and a topic that
includes itself recursed until the stack ran out. The parse is now one
`parse_include()` used by both, which refuses a line it cannot read, and
the recursion carries a depth bound. Both cases are in `util_show_help.c`,
which drives them through `pmix_show_help_add_data()` — registering a
table of your own is the only way to reach this parser from a test, and
the malformed cases die on a signal rather than failing an assertion when
the guards are removed.

**`pmix_show_help_string()` is not a pure lookup.** When the topic does
not exist it displays the "couldn't find that help reference" notice
itself and *then* returns `NULL`, so a caller that reports the miss again
reports it twice. Anything that stores a `strdup`ed filename or topic on
`abd_tuples` has to check it: `match()` hands both straight to `strcmp()`,
and the list outlives the call that built it, so an entry carrying a
`NULL` is a segfault on the next lookup rather than on this one.

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

### `pmix_parse_options` — the port-range expander

Two functions that turn `"1,3-5"` into an argv. Their only callers are
the `ptl` and `pnet/tcp` port-range parameters, which is what makes
them worth care: **the strings they read come straight from an MCA
parameter or a host-supplied info key**, so every malformed input is a
user typo rather than a library bug, and none of it may take the
process down or be silently reinterpreted.

- **`strtol()` cannot tell you it failed, and its answer here was
  narrowed to an `int`.** It answers 0 for a string with no digits in
  it, and saturates at `LONG_MAX` for one that is too big — and
  `LONG_MAX` narrowed to `int` is `-1`, which this parser reads as the
  *wildcard*. So `ptl_base_ipv4_ports=4294967295` discarded whatever the
  caller had already collected and replaced it with "any port", and
  `ptl_base_ipv4_ports=abc` quietly became port 0. Both are now reported
  and skipped, by `parse_whole_int()`, which insists on digits, on
  nothing but whitespace after them, and on a value an `int` can hold.
  Pinned by `util_parse_options.c`.
- **The expansion can produce nothing at all, and its callers index
  element 0.** `"-"` splits to no tokens (`PMIx_Argv_split` drops empty
  fields), and a malformed element is now skipped, so `*output` is
  legitimately `NULL` on return. Four sites in `src/mca/ptl/base` read
  `ports[0]` to see whether the user had asked for the wildcard, which
  meant `PMIX_MCA_ptl_base_ipv4_ports=-` **segfaulted every PMIx client
  inside `PMIx_Init`**. `pnet/tcp` got this right by asking
  `PMIx_Argv_count()` first. If you call this function, the answer may
  be empty — the regression is the `setenv` at the top of
  [`test/unit/ptl_uri.c`](../../test/unit/ptl_uri.c), which dies on a
  signal rather than failing a case.
- Both functions return `void`, so an allocation failure is invisible
  to the caller and a malformed element can only be *reported*, not
  returned. `pmix_util_get_ranges()` — installed API with no in-tree
  caller — builds two parallel arrays a pair at a time, so an
  allocation failure between the two would leave them different
  lengths, and there is no way to say so. Do not add a caller that
  needs to know.

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

Four invariants in here are easy to break and hard to see broken.

- **A wildcard removal with a `NULL` key must empty the table, not just
  release what is in it.** `pmix_hash_remove_data()` releases every
  `proc_data` it walks, and the table it walked them through still holds
  a pointer to each one; the per-rank path removes its entry first, and
  the wildcard path has to do the same. It cannot do it inside the loop
  without invalidating the iteration, so it sweeps afterwards with
  `pmix_hash_table_remove_all()`. Take that sweep away and the very next
  `pmix_hash_fetch()` on the table dereferences a freed `proc_data` —
  `test_remove_wildcard_empties_the_table` in
  [`test/unit/util/util_hash.c`](../../test/unit/util/util_hash.c) dies
  on a signal, not on an assertion. The reason nobody has hit it is that
  the only caller of that combination is `gds/hash`'s job destructor,
  which destructs the table on the next line.
- **Copy the new value before releasing the old one.** An update that
  released first and then failed to copy left the entry in the table
  under its key with a `NULL` value, and nothing downstream expects
  that: `make_copy()` hands the stored value straight to
  `PMIx_Value_xfer()`, which reads `src->type` without looking. The
  failed store returns an error the caller sees, and the process then
  dies on the *next* fetch of that key, somewhere else entirely.
- **`pmix_pointer_array_add()` answers a negative status, and
  `qualindex` is unsigned.** Assigning the failure straight through
  gives the entry an index the qualifier array can never be asked for —
  `pmix_pointer_array_get_item()` bounds-checks and answers `NULL` — so
  the value is stored, is never findable again, and the store reports
  success. Check the `int` before narrowing it.
- **Every allocation in here goes through the TMA, and the TMA can
  fail.** Under `gds/shmem3` that allocator is a fixed-size shared
  segment, so an exhausted one is an ordinary outcome rather than the
  end of the process. The registration branch of `lookup_key()` is worth
  a second look on this point: an entry whose key string did not copy is
  worse than no entry at all, because `add_to_lookup()` declines to index
  a `NULL` string, so the key ends up registered under an id that nothing
  can find by name — every later reference to it mints another one — and
  `make_copy()` loads that string as the key it reports to the
  application.

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

### `pmix_environ` — harvesting, and the array you must not realloc

- **An include/exclude entry is a NAME, unless it ends in `*`.** The
  matcher used to treat every entry as a prefix, so `"PATH"` also took
  `PATHFINDER`; it now matches exactly unless a trailing `*` says
  otherwise, which is what the documented `"OMPI_*,OPAL_*"` convention
  has always meant. The cost of that correctness is that a list written
  as a bare prefix silently stops matching *anything*, and there was one:
  `src/mca/pmdl/base` harvests the local MCA params with `"PMIX_MCA_"`,
  which as an exact name is the name of no variable at all, so no MCA
  param reached a spawned child. If you add an include list, decide which
  of the two you mean and spell it. Both directions are pinned by
  `test_harvest_exact_vs_wildcard` in
  [`test/unit/util/util_environ.c`](../../test/unit/util/util_environ.c).
- **`ilist` belongs to the caller, so nothing on it can be assumed.**
  `pmix_util_harvest_envars` walks a list it did not build: entries may
  be of any type, and a `PMIX_ENVAR` entry need not carry a value, since
  `PMIx_Envar_load()` leaves the value untouched when handed `NULL`.
  Screen the type *and* the pointers before comparing.
- **`pmix_environ_merge_inplace()` must never be given `environ`.** It
  extends the array with `PMIx_Argv_append_nosize()`, which reallocs, and
  `environ` may not be realloc'd — the process aborts. This was enforced
  by a bare `assert()`, which `-DNDEBUG` removes from precisely the build
  that needs it; it is now a real check returning `PMIX_ERR_BAD_PARAM`.
  `test_merge_inplace_refuses_environ` aborts the test binary if the
  check is removed, so the hazard is not theoretical.
- **`pmix_home_directory()` consults `$HOME` only for the calling user.**
  Any other uid goes straight to `getpwuid()`. Ask about yourself with
  `geteuid()`, `(uid_t)-1`, or `UINT_MAX` — all three are accepted, and
  the last is what the code historically tested, which coincides with
  `(uid_t)-1` only while `uid_t` is exactly as wide as an `int`. See the
  `pmix_context_fns` note below for what happened to the one caller that
  spelled it `-1`. `getpwuid()` is not reentrant.

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

### `pmix_getcwd` — $PWD is not authoritative

The name and the ticket reference in the header suggest a function that
prefers `$PWD` — historically it did, comparing the two by `stat()` so a
`$PWD` that named the same directory through a symlink could be handed
back. The `stat()` went away years ago and the comparison is now a
`strcmp`, which means `$PWD` is used only when it is character-for-
character what `getcwd()` already said: **the answer is always the
`getcwd()` value.** Do not "restore" the `$PWD` preference without
deciding deliberately that a caller should be told a directory the
process is not in.

The length test is `>=`, not `>`: `size` counts the terminating NUL, so a
buffer exactly as long as the path is a truncation. A truncation is not
an empty answer — the caller gets as much of the *basename* as fits and
`PMIX_ERR_OUT_OF_RESOURCE`, so a short buffer yields a plausible-looking
relative name rather than an obvious failure. Both boundaries, and the
`$PWD` behavior, are pinned by
[`test/unit/util/util_getcwd.c`](../../test/unit/util/util_getcwd.c).

One contract seam worth knowing: this function accepts any `size` up to
`INT_MAX`, but hands it to `pmix_string_copy()`, which `assert()`s at
`PMIX_MAX_SIZE_ALLOWED_BY_PMIX_STRING_COPY` (128K). A caller with a
buffer between those two bounds would abort rather than be served. No
caller is close — the only one in the tree uses `PMIX_PATH_MAX` — but
do not widen the documented range on the strength of the bozo check
alone.

### `pmix_getid` — installed API with no caller

`pmix_util_getid()` is called from nowhere in PMIx and nowhere in PRRTE,
but `pmix_getid.h` is in the installed `headers` list and the symbol is
`PMIX_EXPORT`ed, so it is API — the same situation as the three unused
`pmix_fd` entry points above, and it takes the same treatment: judge it
as API, do not retire it on your own authority.

**Every route through it is conditionally compiled, and this platform
selects exactly one.** There are four: `getsockopt(SO_PEERCRED)` filling
`struct sockpeercred` (OpenBSD) or `struct ucred` with either `uid`/`gid`
or `cr_uid`/`cr_gid` members, and `getpeereid()`. Which one you can read
is not which one your users compile. Pick the shape in a single `#if`
chain at the top of the file and let the body test one macro — when the
declarations sat behind a bare `defined(SO_PEERCRED)` and the code
reading them behind the full condition, a platform that defines
`SO_PEERCRED` but whose `AC_CHECK_MEMBERS` probe did not fire got no
fallback at all: it failed to build, on an incomplete `struct ucred` and
on two variables nothing used. Compile-check every arm before believing
it — forcing the guards by hand for one `make pmix_getid.lo` is enough,
and a stand-in `struct ucred` in the same prologue covers the shapes this
platform has no header for.

Two things a caller must know, neither of which this function can fix:

- **Only a connected AF_UNIX socket gives a meaningful answer.** A pipe
  or a closed descriptor is `ENOTSOCK` and comes back as
  `PMIX_ERR_INVALID_CRED`, which is what you want. A *TCP* socket is
  worse than that: Linux answers `SO_PEERCRED` on one with success and an
  overflow uid, so the function reports `PMIX_SUCCESS` over credentials
  that identify nobody. Do not use this to authenticate a peer you did
  not reach over a Unix socket.
- **The out-parameters are written only on `PMIX_SUCCESS`.** Every error
  return leaves them exactly as the caller had them.

[`test/unit/util/util_getid.c`](../../test/unit/util/util_getid.c) covers
it over a `socketpair()`, and skips (77) on a platform that answers
`PMIX_ERR_NOT_SUPPORTED`.

### `pmix_error` — two lookups over a table you do not write

`PMIx_Error_string()` and `PMIx_Error_code()` are a linear scan each over
`pmix_event_strings[]`, which is **generated** into the *build* tree
(`$(builddir)/src/include/pmix_event_strings.{c,h}`) by
[`contrib/construct_event_strings.py`](../../contrib/construct_event_strings.py).
You will not find that table in the source tree, and you must not edit it.
Four things about how it is built decide how these two functions behave.

- **The table is fed by three headers, in order**: `include/pmix_common.h.in`,
  then `include/pmix_deprecated.h`, then **`src/util/pmix_error.h`** — which
  is why this directory's own header carries a `/**** PMIX ERROR CONSTANTS ****/`
  banner. That banner is not decoration: the harvester skips everything before
  it and stops at the first line mentioning `PMIX_INTERNAL_ERR_DONE`,
  `PMIX_ERR_SYS_BASE`, or `PMIX_EXTERNAL_ERR_BASE`. **An internal status added
  outside that window gets no name**, and `PMIX_ERROR_LOG` then prints
  `"ERROR STRING NOT FOUND"` for it. `PMIX_INTERNAL_ERR_BASE` sits above the
  banner and `PMIX_INTERNAL_ERR_DONE` below it precisely so the two range
  markers stay out of the table.
- **Scan order is what makes a renamed status print under its current name.**
  A deprecation rename is the one case where two constants legitimately share
  a value (see the top-level `AGENTS.md`), and there are several: `-11`, `-3`,
  `-58`, `-145`, `-231`, `-232`. `PMIx_Error_string()` returns the *first*
  match, and the current spelling is first only because `pmix_common.h.in` is
  harvested before `pmix_deprecated.h`. Reorder the harvest and every error
  message naming one of those codes silently changes.
  `test_error_string_prefers_current_name` pins it.
- **`PMIX_EVENT_INDEX_BOUNDARY` is the count of real entries, and the array is
  one longer than that.** The generator appends a terminator
  (`.index = UINT32_MAX, .name = "", .code = -1`) that both loops must stop
  short of — a scan that ran one entry too far would answer `PMIX_ERROR` for
  the empty name and `""` for a code of `-1`.
  `test_error_code_empty_name` is the guard.
- **`INT32_MIN` is the only "not recognized" answer `PMIx_Error_code()` can
  give**, because every real status is itself negative; callers must test for
  that sentinel rather than for negativity. A `NULL` name is answered the same
  way rather than handed to `strcasecmp` — this is an installed public entry
  point, so it screens its argument.

Both functions are pure reads of a `const` table with static storage duration:
reentrant, safe on any thread, and safe to call from inside the library. The
returned string belongs to the library and must never be freed.

### `pmix_name_fns` — printing a name costs two buffers, not one

Every `PMIX_NAME_PRINT`/`PMIX_RANK_PRINT` answer comes out of a ring of
16 buffers held in thread-specific storage, so the result is never freed
and stays valid only until the ring wraps. What is easy to miss is that
`print_args()` renders the rank by calling `pmix_util_print_rank()`,
which takes a buffer of its own — so a *name* print spends two, and the
real budget for one statement is about eight, not sixteen. Nothing in
the tree uses more than two per statement, and overrunning it does not
crash; it silently makes an earlier answer read as a later one.

The TSD key is created once behind an atomic latch, and
`pmix_name_fns_finalize()` clears that latch because
`pmix_tsd_keys_destruct()` deletes the key outright. The two must stay
adjacent, in that order, at the point in `pmix_rte_finalize` where this
is the only thread left — see
[`src/runtime/AGENTS.md`](../runtime/AGENTS.md) for why they used to run
further up and what broke.

**The rank is unsigned, and 45 of its reserved values have no name.**
`PMIX_RANK_VALID` is `UINT32_MAX-50`, so everything from there up is
reserved, and `pmix_util_print_rank()` names only five of them
(`UNDEF`, `WILDCARD`, `LOCAL_NODE`, `LOCAL_PEERS`, `INVALID`). The rest
fall through to the numeric branch, which therefore has to render values
above `INT32_MAX` — it cast to `long` and used `%ld`, which is exact on
a 64-bit platform and implementation-defined where `long` is 32 bits, so
the same rank printed positive on one machine and negative on another.
`test_print_rank_above_int32` in
[`test/unit/util/util_name_fns.c`](../../test/unit/util/util_name_fns.c)
pins it, and note that it *cannot* fail on a 64-bit build — it is there
for the 32-bit ones.

### `pmix_net` / `pmix_if` — network helpers

`pmix_net` is *almost* pure address math (CIDR→netmask in *network*
order, localhost/link-local/public classification, `isaddr`) — see the
two pieces of state it does keep, below. `pmix_if` owns the
discovered-interface list (populated by the `src/mca/pif/*` components —
this file only *consumes* it) and the `a.b.c.d[/mask]` tuple parser
`pmix_iftupletoaddr`, which returns masks in **host** order. Mind that
byte-order difference if you ever mix the two.

Three different things in here are called a "mask" and only two of them
are the same kind of number. `pmix_iftupletoaddr()` hands back a real
32-bit **bit mask** in host order; `pmix_ifindextomask()` hands back the
interface's **CIDR prefix length**, which is also what
`pmix_net_samenetwork()` expects as its third argument. Do not feed one
to the other.

**The list is built once and read without a lock.** `pmix_pif_base_open()`
constructs `pmix_if_list`, the discovery components append to it, and
nothing mutates it again until `pmix_pif_base_close()` drains and
destructs it. Every lookup in `pmix_if.c` is an unlocked walk that is safe
only because of that; if a future component ever refreshes the list at
run time, all of them become races at once.

**An empty list answers with its sentinel, not with `NULL`.**
`pmix_list_get_first()` returns `&list->pmix_list_sentinel` when there is
nothing on the list, so a `NULL` test on its result never fires — the
caller then reads its own struct fields off a `pmix_list_item_t` embedded
in the `pmix_list_t`, which is a read past the end of that object.
`pmix_ifbegin()` did exactly this and handed the `ptl` listener a garbage
starting index on any node where discovery found nothing (a container in
its own network namespace, a platform no `pif` component can read).
Terminate on `pmix_list_get_end()`, the way every other lookup in the file
does. `test_ifbegin_empty` in
[`test/unit/util/util_if.c`](../../test/unit/util/util_if.c) pins it.

**An entry answers only for its own address family, and the list mixes
families in every build.** A kernel index carries one `pmix_if_list` entry
per address, so an interface with both an IPv4 and an IPv6 address is two
entries. Reading a `sockaddr_in` out of an AF_INET6 entry yields that
entry's flow label, and reading a `sockaddr_in6` out of an AF_INET entry
yields the zero padding behind the address — so an all-zero query compares
equal to an unrelated entry of the other family. That is how
`pmix_ifaddrtoname()` answered `"lo"` for `0.0.0.0` and for `::`, and
`pmix_ifislocal()` is a thin wrapper over it, so the same confusion calls a
remote node local. Screen `sa_family` before comparing, as
`pmix_ifaddrtokindex()` and `pmix_ifmatches()` already do. Note this is
**not** avoided by an IPv6-disabled build: `PMIX_ENABLE_IPV6` is 0 by
default, but the `pif` IPv6 components are gated on the OS rather than on
that flag, so they populate the list regardless — all `PMIX_ENABLE_IPV6`
turns off is the IPv6 arm of `pmix_ifgetaliases()`.

**`strtol()` answering 0 is not the same as reading a 0.** `/0` is legal
CIDR meaning "match everything", so once `pmix_iftupletoaddr()` started
accepting it, a suffix with no digits in it at all (`"10.0.0.0/"`,
`"10.0.0.0/abc"`) was quietly accepted as that instead of being reported.
In `pmix_ifmatches()` the resulting mask matches no interface, so an
`if_include` entry with a typo silently selected nothing *and* suppressed
the `invalid-net-mask` warning that used to fire. Check the `endptr`.

**`pmix_net_init()` runs before the MCA parameters have values.** It is
called from `pmix_init_util()`; `pmix_register_params()` — which is what
gives `pmix_net_private_ipv4` anything but its `NULL` initializer — runs
later, from `pmix_rte_init()` or from each tool's own startup. So the
private-network table cannot be built in `pmix_net_init()`: parsing there
saw `NULL`, `PMIx_Argv_split(NULL, ...)` answered `NULL`, and the table
stayed empty, which `pmix_net_addr_isipv4public()` reads as *no network is
private* — every RFC1918 address came back public and the MCA parameter
was inert in every build. Worse, the malformed-entry `show_help` in that
parse had never once fired. The parse now lives in
`pmix_net_setup_private_ipv4()`, called from `pmix_register_params()` at
the point the value exists. **If you add state here that comes from an MCA
parameter, it cannot be initialized in `pmix_net_init()` either.**
`test_isipv4public` in
[`test/unit/util/util_net.c`](../../test/unit/util/util_net.c) supplies
the list through the environment, so it also pins that the *user's* value
is the one consulted.

**`PMIX_ENABLE_IPV6` is not a statement about what addresses you will be
handed.** It is 0 by default, but the `pif` IPv6 discovery components are
gated on the OS, not on that flag, so `AF_INET6` addresses are entirely
ordinary in a default build. `pmix_net_addr_isipv6linklocal()` had its
`case AF_INET6:` behind that flag and so fell through to the `default:`
arm for every IPv6 address — answering `false` *and* printing "unhandled
sa_family" on stream 0 — while `pmix_net_islocalhost()` and
`pmix_net_samenetwork()` next to it handle `AF_INET6` unconditionally.
Family tests in this file are unconditional; keep them that way.

**An IPv4-mapped address is an IPv4 address.** `IN6_IS_ADDR_LOOPBACK`
matches only `::1`, so `::ffff:127.0.0.1` — the shape a dual-stack socket
reports for an ordinary IPv4 loopback peer — read as *not* localhost.
`pmix_net_islocalhost()` now unwraps the low 32 bits of a v4-mapped
address and applies the same 127.0.0.0/8 test the `AF_INET` arm does.

**`pmix_net_get_hostname()` is per-thread, and never `NULL`.** Its answer
lives in a `NI_MAXHOST + 1` buffer held in thread-specific storage, so the
two callers that reach it from different threads (the `ptl` listener
thread's accept handler, and the connect path) do not collide; the answer
is valid until that thread's next call and must not be freed. Anything it
cannot render — an unsupported family, a failed conversion, no memory for
the buffer — comes back as the file-static `"UNKNOWN"`, because a caller
about to print the answer should not have to check it. The `#else` arm
below has to honor that too, and used to return `NULL`. The TSD key is
created fresh by every `pmix_net_init()` and deleted outright by
`pmix_tsd_keys_destruct()` at finalize, so unlike `pmix_name_fns` this
file needs no latch to clear.

Four properties that are contracts rather than defects, so that the next
reader does not re-litigate them:

- **Neither resolver call in `pmix_net` resolves anything.**
  `pmix_net_isaddr()` passes `AI_NUMERICHOST` to `getaddrinfo()` and
  `pmix_net_get_hostname()` passes `NI_NUMERICHOST` to `getnameinfo()`,
  so despite the names neither performs a DNS lookup and neither can
  block on one. That is *not* true of `pmix_if` (see above) — do not
  carry an assumption from one file to the other.
- **`private_ipv4` is written only at init and finalize.**
  `pmix_net_addr_isipv4public()` walks it with no lock, which is safe
  because the only writers are `pmix_net_setup_private_ipv4()` (from
  `pmix_register_params()`) and `pmix_net_finalize()`. It has no in-tree
  caller, but `pmix_net.h` is installed, so an external one must not
  classify addresses concurrently with a library init or finalize.
- **`getaddrinfo()` blocks in `pmix_if`, and only `pmix_ifaddrtoname()`
  can be told not to call it.** `pif_do_not_resolve` short-circuits it
  (and therefore `pmix_ifislocal()`); `pmix_ifaddrtokindex()` resolves
  regardless. Both are installed API with no in-tree caller — the
  in-tree consumers (`ptl_base_listener`, `pnet/tcp`) use only the
  index/kernel-index lookups, at init — so nothing here runs a DNS
  timeout on the progress thread today. Do not add such a call to a
  progress-thread path.
- **Neither file screens its arguments.** A `NULL` name reaches
  `strncmp`, a `NULL` tuple reaches `strchr`, and a non-positive `length`
  reaches `memset`/`pmix_strncpy`. That is a departure from the screening
  the rest of this directory does on installed entry points; it is left
  alone because no caller in PMIx or PRRTE passes any of those, and the
  header does not promise otherwise.

**Both files carry a `#if HAVE_STRUCT_SOCKADDR_IN` `#else` arm, and
neither of them compiled.** This is the usual trap: **no configuration
anyone builds selects those arms, so they get no compiler coverage at
all.** `pmix_if.c`'s has to define *every* one of the 21 entry points
`pmix_if.h` declares — seven were simply missing, which is a link failure
rather than the graceful degradation the fallback exists for.
`pmix_net.c`'s defined all eight but named their parameters without
reading them, which is nine `-Werror=unused-parameter` errors in a tree
that builds `-Wextra -Werror`; `PMIX_HIDE_UNUSED_PARAMS` is how the rest
of the tree says "deliberately unread". Force the guard false for one
`make pmix_if.lo` / `make pmix_net.lo` before believing an edit there is
good — and note that `-UHAVE_STRUCT_SOCKADDR_IN` on the command line does
**not** do it, because `pmix_config.h` defines the macro again from
inside the file. Edit the `#ifdef` to `#if 0` for the one compile.

### `pmix_path` — searching for an executable without scribbling on the search

Resolves a filename against a search path (`pmix_path_find`,
`pmix_path_findv`, `pmix_find_absolute_path`) and answers two questions
about the filesystem (`pmix_path_nfs`, `pmix_path_df`). The searching
half is reached from `pmix_pfexec` and `pmix_context_fns` when a spawn
request has to be turned into an executable, and from `src/mca/base`
when a parameter file has to be found.

**Nothing here may write into what it was given, and two things did.**
This is the same rule `pmix_argv` records, and it was broken in the two
places it matters most:

- `pmix_path_find()` marked the end of a `"$VAR/rest"` prefix by
  punching a temporary NUL into `pathv[i]` and putting it back. Its
  callers pass arrays of string literals as a matter of course, which
  faults outright, and it races anything else reading the same array.
  It now copies the name out. `test_path_find_does_not_write_through_pathv`
  in [`test/unit/util/util_path.c`](../../test/unit/util/util_path.c)
  dies on a signal, not on an assertion, if this returns.
- `path_env_load()` walked the `$PATH` string the same way — and the
  string it is handed is normally the one `getenv()` returned, which
  POSIX says a program must not modify. Any other thread reading `PATH`
  in that window, or a child forked in it, saw a truncated one. It now
  splits a copy. Note this half is **not** observable from a
  single-threaded test, since the value is put back before the function
  returns; the assertion in `util_path.c` only catches a variant that
  forgets to restore.

**The two filesystem questions are each answered by an arm this platform
does not compile**, which is the recurring trap in this directory:

- `pmix_path_nfs()` reads the mount table, so it exists only where
  `<mntent.h>` does. **On macOS and the BSDs it answers `false`
  unconditionally**, whatever the file is really on — which the header
  had never said. It has no in-tree caller; do not add one that needs
  the answer to be right off Linux.
- `pmix_path_df()`'s `HAVE_STATVFS` arm did not compile *at all*. The
  sign test on `f_bavail` was written as a cast inside the comparison,
  and `statvfs`'s `f_bavail` is unsigned, so it is
  `-Werror=type-limits` on every platform that selects that arm — i.e.
  the ones without a `struct statfs`, NetBSD among them.
  The test now goes through a signed variable, which is correct for the
  BSD `statfs` case it exists for and silent for the unsigned ones.
  That arm also multiplied `f_bavail` by the wrong number: `statfs`
  counts it in `f_bsize` blocks, but POSIX `statvfs` counts it in
  `f_frsize` ones, and `f_bsize` there is the *preferred I/O size* —
  many times larger on some filesystems, so the caller was told it had
  far more room than it does. Force both arms by hand before believing
  an edit to this function; a stub `<mntent.h>` in the same style as
  `pcompress`'s test-build shims is enough for the first.

### `pmix_os_path` — a NULL that callers kept forgetting to read

One variadic function that concatenates its `char *` arguments with the
path separator between them. It is pure: no globals, no syscalls, no
look at the filesystem. Whether the name it builds refers to anything is
the caller's question, asked afterwards.

**It fails, and the failure is a `NULL` return.** Two ways: the assembled
name would exceed `PMIX_PATH_MAX` (`PATH_MAX + 1`, so 1025 bytes on Linux
and macOS), or the allocation failed. Neither is exotic - the usual
caller is walking a directory tree, and every level of nesting lengthens
the name - and five call sites across `src/include`, `src/mca/ptl`,
`src/mca/pmdl` and `src/tools` used the answer without asking. One of
them handed it to `strcmp()`. **If you call this, screen the result**;
`__pmix_attribute_warn_unused_result__` makes the compiler insist you use
it, which is not the same thing.

**The length is tested against an estimate, not against the answer.** The
estimate allows one separator per element, which is an upper bound
because an element that already begins with a separator does not get
one. It used to count that separator twice - once while measuring and
again in the `num_elements` term - which pushed a name that fits over the
limit and had it refused. `test_length_boundary` in
[`test/unit/util/util_os_path.c`](../../test/unit/util/util_os_path.c)
pins both sides of that boundary; note that the accepted case asserts the
*length* of what came back, since asserting merely that it is non-NULL
would pass against a library with any bound at all above 1024.

Two smaller things the header now states and the code has always done:
the empty argument list answers `"."` **plus a separator** for a relative
path, not `"."`; and the argument list must end in `NULL`, which
`__pmix_attribute_sentinel__` makes a compiler diagnostic rather than the
segfault the header threatens.

### `pmix_os_dirpath` — the session directories, and who may swap them

Creates and recursively destroys directory trees. Its callers are the
`ptl` rendezvous/session directories and the `pmix_iof` per-rank output
directories, which means the paths that reach it have **fully predictable
names under a world-writable root** (`/tmp` by default). That is the
threat model the whole file is written against, and it is why the code
looks more roundabout than "stat then chmod" or "readdir then unlink".

- **Act through a descriptor, never through a re-resolved path.**
  `dirpath_ensure_mode()` opens the final component
  (`O_DIRECTORY | O_NOFOLLOW`) and does its `fstat`/`fchmod` on that
  descriptor, so the object inspected is the object modified.
  `dirpath_destroy_at()` walks with `fstatat`/`openat`/`unlinkat`
  relative to a descriptor it already holds, so an entry cannot be
  swapped between being classified and being removed. `O_NOFOLLOW` and
  `AT_SYMLINK_NOFOLLOW` keep a symlink an entry to be unlinked rather
  than a path to be followed. If you add an operation here, add it in
  that style.
- **Permission is not tested, deliberately.** Asking whether a directory
  is writable and then acting on the answer is itself a check/use race
  that no spelling of `access()`/`faccessat()` can close. Whether the
  caller can put a file there is settled by the caller creating one.
  `pmix_os_dirpath_access()` is the fossil of that decision: it is a
  no-op that always answers `PMIX_SUCCESS`, kept because it is installed
  API that PRRTE may still link against. Do not give it a body.
- **The plain single-path entry points protect only the *final*
  component against a planted symlink.** `mkdir(2)` does not follow a
  symlink at the last component but does follow one at every component
  before it, and the tree-building loop accepts `EEXIST` on an
  intermediate component without asking what it is - by design, since an
  intermediate need only be traversable. So a symlink pre-planted midway
  through a predictable session path still redirects the leaf that gets
  created. That is why `pmix_os_dirpath_create_under()` and
  `_open_file_under()` exist, and why they take the trusted prefix and
  the composed tail as *separate arguments* rather than one path: only
  the tail can be walked with `O_NOFOLLOW` at every step. Which of the
  three entry points a caller wants is decided in
  [Composing a path PMIx will then open](#composing-a-path-pmix-will-then-open)
  below - and **do not "fix" the plain ones halfway**: a partial walk
  that still ends in a path-based `mkdir` buys nothing, and one that
  refuses a symlink at every component refuses macOS outright.

Three contracts the callers depend on:

- **`PMIX_ERR_EXISTS` is a success.** `pmix_os_dirpath_create()` answers
  it when the final directory was already there carrying at least the
  requested mode. Every in-tree caller tests for it alongside
  `PMIX_SUCCESS`; what the distinction buys them is knowing whether
  *they* are the ones who must remove it afterwards. A caller that tests
  only `PMIX_SUCCESS != rc` treats an ordinary rerun as a failure.
  `PMIX_ERR_SILENT` means the user has already been shown a message and
  must not be shown another.
- **The `tmp` buffer in `pmix_os_dirpath_create()` is sized exactly, not
  generously.** It is `strlen(path) + 1`, and the `strcat` chain fits
  only because every separator it writes was a separator in the input:
  `PMIx_Argv_split()` drops empty fields, so repeated separators
  collapse and no component is empty. Those same two facts are what make
  the `tmp[strlen(tmp) - 1]` read safe from the second iteration on. An
  edit that stops splitting on `path_sep[0]`, or that starts keeping
  empty fields, overruns the buffer and reads before it.
- **An empty path is not a path.** `mkdir("")` answers `ENOENT`, which
  is the same errno that means "the parents are missing, go build the
  tree" - and an empty path splits to no components at all, so the
  build loop never runs and the function used to report that it had
  created a tree while doing nothing. The screen for it sits next to the
  `NULL` screen; a `NULL` from `PMIx_Argv_split()` now therefore means
  out of memory, and is reported as such rather than walked past.

**This file reaches into two other subsystems, and that is a wart, not a
pattern to copy.** `pmix_os_dirpath_destroy()` includes
`src/mca/ptl/base/base.h` and `src/server/pmix_server_ops.h` so it can
decline to remove the system tmpdir unless PMIx created it - a policy
decision living in a leaf utility. It used to go further and `free()`
`pmix_ptl_base.system_tmpdir` on its way out, which aliased its own
`path` argument (the one caller that reaches that arm passes exactly that
string) and survived only because nothing read `path` again afterwards.
`pmix_ptl_base_close()` frees it on the line after the call. Keep the
release with the owner.

### `pmix_fd` — descriptor helpers, three of them with no caller

`pmix_fd_read`/`pmix_fd_write`/`pmix_fd_set_cloexec`/`pmix_fd_is_*` are
used across the tree. `pmix_fd_get_peer_name()`,
`pmix_fd_dup2()` and `pmix_close_open_file_descriptors()` are **called
from nowhere in PMIx and nowhere in PRRTE** — but `pmix_fd.h` is an
installed header, so they are still API. Judge them as API, not as dead
code; the alternative is to retire them deliberately, which is a
separate decision.

- **`pmix_fd_read()` and `pmix_fd_write()` must be given a *blocking*
  fd.** They handle `EAGAIN` by retrying immediately, which on a
  non-blocking descriptor is a spin loop that burns a core until the
  peer moves. Every caller today hands them an ordinary pipe
  (`pmix_pfexec`'s keepalive pipe, the `ptl` listener's `--report-uri`
  pipe), none of which is `O_NONBLOCK`. Also note `pmix_fd_read()`
  reports a short read as `PMIX_ERR_TIMEOUT` — that is EOF, not a
  timeout, and `pmix_pfexec` depends on the distinction to tell "the
  child reached execve" from "the child sent us a failure record".
- **`pmix_fd_get_peer_name()` answers out of one file-static buffer.**
  The result is valid only until the next call and two threads calling
  at once will overwrite each other. It never returns `NULL`: a peer it
  cannot name — `getpeername()` failed, or the family is not one it
  renders — reads as `"Unknown"`, because a caller that is about to
  print the answer should not have to check it. `inet_ntop()` can fail
  too, and used to leak that `NULL` straight out.
- **`pmix_close_open_file_descriptors()` has two routes and only one of
  them is testable.** Where the OS lists the process's descriptors
  (`/dev/fd`, `/proc/self/fd`) it closes exactly those; everywhere else
  it falls back to closing every fd below a bound. Two things about that
  bound, because getting it wrong fails *silently* — the function
  reports nothing, and the descriptors simply travel into the exec'd
  image:
  - `sysconf(_SC_OPEN_MAX)` returns a `long`, and the comment in the
    code records that some systems answer with a number in the billions.
    Clamp it against `pmix_maxfd` **while it is still a long**. Narrowing
    first can land on a negative `int`, and then the closing loop does
    not execute at all.
  - `strtol()` sets `errno` on failure and never clears it on success,
    so the scan has to zero `errno` before each call. It did not, and a
    stale `EINVAL`/`ERANGE` from anything earlier in the process
    abandoned the listing on the first entry.

  [`test/unit/util/util_fd.c`](../../test/unit/util/util_fd.c) runs the
  mass close in a forked child — in-process it would take the harness's
  own descriptors down — and checks that the protected fd and
  stdin/stdout/stderr survive. It reaches only the directory-scan route,
  since both Linux and macOS have one; the fallback has to be verified
  by hand by pointing the `opendir()` at a path that does not exist.

### `pmix_few` — the only place in this tree that forks by hand

One function, one caller: `src/tools/wrapper/pmixcc.c` runs the real
compiler through it. Everything else that starts a process goes through
`pmix_pfexec`. Three things follow from that.

- **The child must leave through `_exit()`, never `exit()`.** It shares
  the parent's stdio buffers, so `exit()` flushes whatever the parent had
  written and not yet flushed — the output appears twice — and it runs
  atexit handlers the parent registered, which in a threaded process can
  deadlock on a lock some other thread held at fork time. This is the same
  post-fork discipline `pmix_pfexec`'s `do_child()` follows, and for the
  same reason. `test_child_does_not_flush_our_stdio` in
  [`test/unit/util/util_few.c`](../../test/unit/util/util_few.c) is the
  regression; it has to run inside a child of its own with stdout on a
  pipe, since a line-buffered terminal hides the whole effect.
- **`status` is written only on `PMIX_SUCCESS`.** On any other return
  there was no child to wait for, so the caller's variable is whatever it
  already held. `pmixcc` decoded it unconditionally, off an uninitialized
  stack slot, which means a compiler that never started could hand the
  build system any exit code at all — zero included. Initialize it, and
  check the return code before reaching for a `WIF*` macro.
- **`status` is a wait status, not an errno.** Why a launch *failed* is
  in `errno`; `pmixcc` was passing the wait status to `strerror()`. If
  the exec itself is what failed, that is a different thing again — the
  child ran, so the call succeeded, and errno comes back as the child's
  exit status (truncated to 8 bits, as every exit status is).

Two notes on the guard. The body is compiled behind
`HAVE_FORK && HAVE_EXECVE && HAVE_WAITPID` but calls `execvp`, which
configure does not probe for separately. POSIX gives you the whole
`exec*` family together, so the proxy holds; it is worth knowing it *is*
a proxy before adding a fourth call here.

And the `#else` arm — the one no machine anyone builds on has ever
compiled — did not compile. It returns `PMIX_ERR_NOT_SUPPORTED` without
reading either parameter, and this tree builds `-Wextra -Werror`, so
both came out as errors. That is the same shape as the `pmix_pty.c`
stubs recorded below: **a conditionally-compiled arm no configuration
here selects gets no compiler coverage at all.** If you touch one,
compile it by hand — forcing the guard false for one `make` of that
object is enough.

### Composing a path PMIx will then open

Several places here build a name out of PMIx's own naming scheme and
then create something at it — a segment backing file, an `hwloc.sm`, a
per-rank `stdout`, a session directory. Two rules keep those from landing
somewhere other than where the caller asked, and both are about the
difference between naming a thing and holding it.

**`O_NOFOLLOW` applies to the last component of a path and nothing
else.** Every component before it is resolved in the ordinary way,
symlinks included. So adding `O_NOFOLLOW` to a plain `open()` says
nothing at all about the directories the name was reached through. That
gives three entry points rather than one, and which you want depends on
who composed which part of the name:

| the name is | use |
|---|---|
| entirely from outside PMIx | `pmix_os_dirpath_create()` — the whole path is taken as given |
| a caller's directory + a leaf PMIx composes | `pmix_os_dirpath_open_file()` |
| a caller's directory + directories PMIx composes | `pmix_os_dirpath_create_under()` / `_open_file_under()` |

- **The prefix is taken as given.** `$TMPDIR`, `PMIX_NSDIR`, a
  user-named output directory — somebody chose those, and PMIx is in no
  position to second-guess them. They are resolved normally, which they
  have to be: on macOS `/tmp`, `/var` and `/etc` are *all* root-owned
  symlinks into `/private`, so the default PMIx temporary directory
  reaches its contents through one. A blanket "decline a symlink at any
  component" walk therefore declines the platform; it was tried, and
  that is what it did — while also declining the legitimate reclaim of a
  stale segment file, which is how the mistake surfaced.
- **What PMIx composes below the prefix gets walked.** Those names are
  the library's own, so nothing else should have put anything at them.
  Each is created with `mkdirat()` and opened with `O_NOFOLLOW` against
  the descriptor of its parent, so the directory each one lands in is the
  one just checked. The test is **how many components PMIx composes**,
  not whether it composes any: a single one is already covered, since
  `mkdir(2)` does not follow a link at the last component of the name it
  is given and `dirpath_ensure_mode()` then opens that component
  `O_NOFOLLOW`. It is the *second* composed level that nothing sees.
  - `write_rndz_file()` does **not** need the `_under` pair. Its
    directory is `pmix_server_globals.tmpdir`, which is
    `PMIX_SERVER_TMPDIR` or `$TMPDIR` or `/tmp` verbatim — PMIx composes
    only the basename beneath it.
  - `pmix_iof`'s `PMIX_IOF_OUTPUT_TO_DIRECTORY` sink does: the caller
    names a directory and PMIx builds `<nspace>/rank.N` under it.
  - So does its `PMIX_IOF_OUTPUT_TO_FILE` sink, which is easy to miss.
    `process_pattern()` copies everything that is not a conversion
    verbatim, **separators included**, so `file:%h/%n/rank-%R` has PMIx
    composing two directory levels out of the hostname and the
    namespace. `pattern_literal_head()` there is what finds the dividing
    line: the last separator before the first conversion, since nothing
    ahead of that is transformed and the offset is therefore the same in
    the pattern and in its expansion.
- **Where the name is created fresh, pass `O_EXCL` with `O_CREAT`.**
  Together they decline *anything* already at the name rather than only a
  symlink. `write_rndz_file()` in
  [`ptl_base_listener.c`](../mca/ptl/base/ptl_base_listener.c) is the
  model, **including its two-pass loop** — a name that carries a pid may
  hold a file left over from an earlier run, since pids get reused, and
  that has to be reclaimed or the operation cannot succeed at all.
  Reclaim it with `unlink()`, which removes the name it is given and
  never follows it onward. A change that only declines breaks startup;
  `test/unit/util/util_shmem.c` covers both halves for exactly that
  reason.

**Once you hold a descriptor, act on it rather than on the name.**
`chmod()` follows a symlink at the final component and `lchown()`
deliberately does not, so a pair of them applied to the same path act on
two different objects — which is what `pmix_shmem_segment_chmod()` and
its neighbour were doing. Open once and use `fchmod()`/`fstat()`, the way
`dirpath_ensure_mode()` does in
[`pmix_os_dirpath.c`](pmix_os_dirpath.c).

### `pmix_shmem` — a created segment reads as zero

`pmix_shmem_segment_create()` creates its backing file fresh and
`ftruncate`s it to the full size, so a freshly created segment is a
sparse file whose every page reads as zero. **That is a contract, not an
incidental property**: `gds/shmem3`'s bump allocator relies on it to hand
out zeroed storage without writing a byte (see `tma_carve()` and
[`src/mca/gds/shmem3/AGENTS.md`](../mca/gds/shmem3/AGENTS.md)), which is
what keeps building a segment from faulting in its whole — heavily
over-estimated — extent up front.

"Fresh" is the load-bearing half, and it is why the open is
`O_CREAT | O_EXCL` (through `pmix_os_dirpath_open_file()`) rather than
`O_CREAT | O_TRUNC`. Segments are unlinked when their last holder lets
go, so a path collides only with a file left behind by a server that
died; but the path carries a pid, and pids get reused, and `ftruncate()`
to the same or a smaller size would leave that file's bytes in place.
`O_EXCL` declines whatever is there — a symlink included, which
`O_CREAT | O_TRUNC` would have followed and truncated — and the two-pass
loop then reclaims a leftover with `unlink()` and retries, exactly as
`write_rndz_file()` does. A change that only declines breaks a server
whose pid was reused; both halves are covered by
[`test/unit/util/util_shmem.c`](../../test/unit/util/util_shmem.c).

**The reference count in the segment header is what decides when the
backing file goes away, and it is the ONLY thing that does.**
`pmix_shmem_segment_attach()` takes a reference and
`pmix_shmem_segment_detach()` gives it back; dropping the last one
unlinks the file. Three consequences are easy to get wrong and two of
them were:

- **A created segment holds no reference and is not attached.** The
  internal attach that stamps the header is undone by the same detach as
  everybody else's, so it deliberately does not count — otherwise the
  count would never reach zero. The creator has to attach like any other
  holder if it means to keep the segment alive.
- **`detach()` releases what `attach()` took.** It used to release
  nothing: only `shmem_destruct()` decremented, and only for a handle
  that was still attached, so a handle detached explicitly left its
  reference standing forever and the backing file was never unlinked by
  anyone. `gds/shmem3`'s forced-attach-failure test parameters reach
  exactly that path.
- **A create that fails takes its own file with it.** Nothing else can:
  the caller is being told the create failed, and the destructor only
  unlinks a segment it managed to attach. `gds/shmem3` unlinks by hand
  in the window *after* a successful create (see its `out_release`),
  which is a different case and still needed.

**Two fields of the handle are inputs to `pmix_shmem_segment_attach()`**,
and a process attaching to somebody else's segment fills them in itself:
`backing_path`, and `size` — where `size` is the mapped *footprint*, the
number the creator's handle carried afterwards, not the number it asked
to store. `gds/shmem3` puts the creator's `shmem->size` on the wire for
that reason. Passing the smaller number maps a page short of the end of
the data region, which nothing here can detect.

**The mode a segment's file carries has to allow write to every process
meant to read it.** A peer maps `MAP_SHARED` from a descriptor it opened
`O_RDWR`, because it writes the reference count even when it never
writes the data — so a read-only mode does not make the segment
read-only to a peer, it makes it unopenable, and the peer falls back to
another GDS module. `gds/shmem3` sets `0660`. A reader that wants the
data itself to be unwritable asks for
`pmix_shmem_segment_protect_data()`, which leaves the header page
writable for exactly this reason.

A handle whose attach failed reads `NULL` in both address fields. That
is worth relying on rather than re-deriving: the failure path used to
store `MAP_FAILED` and `MAP_FAILED` plus a page — the second of which
wraps to a small non-NULL address — so the `NULL == hdr_address` screen
that `note_slot_in_use()` in `gds/shmem3` performs would not have caught
either one.

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

### `pmix_pty` — a helper that must not take a terminal for itself

Four wrappers around pty setup, and one caller: `pmix_pfexec`'s
`setup_prefork()` calls `pmix_openpty()` so a spawned child's stdout is a
pty rather than a pipe (PRRTE's `iof_base_setup.c` does the same thing
with the same call). `pmix_ptymopen()`/`pmix_ptysopen()` exist to build
`pmix_openpty()` on a platform that has no `openpty(3)`; nothing in PMIx
or PRRTE calls them directly, but `pmix_pty.h` is installed, so judge
them as API.

**The pty here is a pipe with a terminal on the end of it, and nothing
more.** The child does not `setsid()` and does not want a controlling
terminal — `pmix_pfexec` turns echo off and `dup2`s the slave onto
stdout. What it wants is line-discipline behavior, not a session.

**That makes the controlling-terminal rule the one thing to get right in
this file.** Opening a terminal device from a session leader that has no
controlling terminal *makes it one*, unless the open carries `O_NOCTTY`.
A PMIx server started as a daemon is precisely a session leader with no
controlling terminal. So a helper that opens a pty slave without
`O_NOCTTY` — on behalf of a child, in the parent, before any fork — hands
the **server's** controlling terminal to a pty the child is about to be
given, and closing the last master descriptor then sends `SIGHUP` to the
server's foreground process group. This is not theoretical: with the
`O_NOCTTY` removed, the probe child in
[`test/unit/util/util_pty.c`](../../test/unit/util/util_pty.c) does not
merely report the terminal, it is killed by the hangup before it can
report anything, which is why that test distinguishes "died on a signal"
as an outcome of its own.

`openpty(3)` gets this right, so the live path on every platform built
here was never affected; the exposure is the fallback and the two
exported helpers. An `ioctl(TIOCSCTTY)` on top of the open was the same
mistake twice and is gone. **Do not add either back.** If a future caller
genuinely wants the pty to be a controlling terminal, that belongs in the
child after `setsid()`, which is what `forkpty()` already does — and
`pmix_forkpty()` is there for callers who want it.

**The master descriptor belongs to the caller.** `pmix_ptysopen()` takes
it only because some platforms' STREAMS setup needs it, and it used to
`close()` it on every failure path — while its one caller,
`pmix_openpty()`'s fallback, closed it again on the next line. A double
close is the descriptor equivalent of a double free: the second one lands
on whatever the OS handed out in between. The function now leaves it
alone, and `PMIX_HIDE_UNUSED_PARAMS` marks it deliberately unread since
the signature is frozen.

**`maxlen` means what it says.** Both `pmix_ptymopen()` arms wrote the
device name in with `strncpy`/`strcpy` and neither honored the size: a
short buffer came back unterminated and the `open()` on the next line
read past the end of it, and the BSD arm's test was written
`strlen(...) < maxlen - 1`, which wraps for a `maxlen` of zero and lets
an 11-byte `strcpy` into a zero-length buffer through. Both now refuse a
buffer that cannot hold the name, with the `-5`/`EOVERFLOW` its sibling
path already used.

**Every arm in this file is conditionally compiled and this platform
selects one path through it.** `PMIX_ENABLE_PTY_SUPPORT` (all-stubs),
`HAVE_PTSNAME` (the `/dev/ptmx` route vs. the BSD `/dev/ptyXY` scan),
`HAVE_OPENPTY` and `HAVE_FORKPTY` are four independent switches, and the
stub arms take `void *` where the real ones take `struct termios *`, so
forcing `PMIX_ENABLE_PTY_SUPPORT` means forcing it in the header too. A
July 2026 pass fixed compile errors in those stubs by adding
`PMIX_HIDE_UNUSED_PARAMS` calls — without adding the
`src/include/pmix_globals.h` that declares it, so all three stub arms
still failed to build, and the `!HAVE_PTSNAME` arm failed on an
`errsave` it does not use. **Compile every combination by hand before
believing an edit here**; `make src/util/pmix_pty.lo` with the guards
edited to `#if 0`/`#if 1` is enough, and all six do build warning-free
today — every arm in the file is now reachable that way, since the one
that was not (the Solaris `__SVR4 && __sun` STREAMS module push, which
needed a `<stropts.h>` no supported platform ships) has been removed
along with the rest of this library's Solaris support. The `fdm`
parameter of `pmix_ptysopen()` is what is left of it: nothing reads it
any more, and it stays only because the signature is frozen.

Two things deliberately left alone:

- The BSD arm's `lchown()`/`chmod()` pair acts on two different objects
  if the slave name is a symlink, which is the shape of CVE-2023-41915
  (the `chown`→`lchown` sweep that put the `// DO NOT FOLLOW LINKS`
  comment on that line). It is left as is because the name is a `/dev`
  device node and both calls are no-ops unless the process is already
  root, so planting the symlink requires the privilege the attack would
  gain.
- `ptsname(3)` is not reentrant. `pmix_ptymopen()` has no caller on a
  platform with `openpty(3)`, and the one caller it can have runs before
  a fork; do not add a second one on another thread.

### `pmix_printf` — two implementations, only one of which you compile

Four `PMIX_EXPORT` wrappers over `asprintf`/`snprintf`. On any platform
built this decade they are pass-throughs, and every real defect this file
has ever had lives in the fallbacks nobody compiles.

There are **three** arms, selected independently by `configure`:

| Guard | What is compiled |
|---|---|
| `HAVE_VASPRINTF` | `pmix_vasprintf` is one call to `vasprintf(3)` |
| `HAVE_VSNPRINTF` | `pmix_vsnprintf` is one call to `vsnprintf(3)`; and, if `vasprintf` is missing, `guess_strlen` sizes the result with a `vsnprintf(NULL, 0, …)` probe |
| neither | `guess_strlen` sizes the result by **walking the format itself** |

Both macros really are set by `configure` (unlike some `HAVE_*` spellings
in this tree — see `pmix_output`'s `HAVE_SYSLOG` and `pmix_getid`'s
`SO_PEERCRED`), so the guards are honest. They just never select the
interesting arm here.

**The invariant `guess_strlen` has to hold is not "estimate the length".
It is "bound the length *and* consume exactly the arguments the format
names".** Those are one obligation, not two, and getting either half
wrong is memory-unsafe:

- Under-count and `vsprintf` — which has no bound argument — runs off the
  end of the buffer the caller just allocated from that count. A field
  width alone does this: `%200d` needs 200 bytes and mentions its size
  nowhere in the argument list.
- Fail to consume an argument and every conversion *after* it reads the
  wrong `va_list` slot. The usual ending is a `%s` reading an integer as
  a `char *`.

So an unrecognized conversion cannot be skipped over: not knowing what it
emits and not knowing what it consumes are the same ignorance. The walk
returns **-1** for anything it cannot bound (an unrecognized conversion,
or `%ls` with no precision) and `pmix_vasprintf` fails with `EINVAL`
rather than allocating. Every conversion, flag, width, precision and
length modifier C99 defines is recognized; over-estimating is deliberate
and the bounds are worst-case (`%Lf` of `LDBL_MAX` is ~4932 digits).

Two traps specific to sizing a float by arithmetic, both of which cost
this file an infinite loop before: `while (0 != x) x /= 10.0` never
terminates for an infinity **or** a NaN, and narrowing the `double` a
`%f` argument actually is into a `float` first *manufactures* an infinity
out of any ordinary value above `FLT_MAX` — `pmix_asprintf("%f", 1.0e300)`
was enough to hang the process. Do not reintroduce a divide-down loop;
the bounds are constants for a reason.

`pmix_vsnprintf` delegates to the platform `vsnprintf` where there is
one, rather than routing through `pmix_vasprintf`. It used to do the
latter, which put a `malloc`/`free` pair and a full-length format on
every line the library prints, and made `pmix_snprintf` fail outright
under exactly the memory pressure its callers are usually reporting.
Several in-tree callers (`pmix_output`, `pmix_name_fns`) ignore the
return value and read the buffer regardless, so the fallback arm writes a
terminator even when it fails — the header promises the output is always
null-terminated, and that promise has to survive the error path.

**Compiler coverage.** Nothing in a normal build compiles the fallbacks,
and `-U HAVE_VASPRINTF` on the command line does not help — `pmix_config.h`
defines it again from inside the file. Edit the `#if` lines to `#if 1` /
`#if 0` for the one compile. All four combinations (live, `vsnprintf`
probe, hand walk, and the `memcpy`-instead-of-`va_copy` arm) do compile
warning-free; keep it that way when you touch them.

**Testing them is the same trick.** `test/unit/util/util_printf.c` cross-
checks `pmix_asprintf` against the platform's own `snprintf` for a matrix
of conversions and asserts the return value equals `strlen` of what came
back. That is deliberately implementation-agnostic: on a normal build it
confirms the pass-throughs pass through, and on a platform that selects
the hand walk `make check` exercises the walk with no changes. To
exercise it *here*, force the guards as above, rebuild `libpmix` (not
just the test — remove `<builddir>/src/util/pmix_printf.lo` first), and
run `util_printf`.

### `keyval/` — the flex lexer

`keyval_lex.l` is the flex source; `keyval_lex.c` is a **generated build
product** — never hand-edit it, edit the `.l` and let the build
regenerate. `pmix_keyval_parse` drives it over a real `FILE*` under
`keyval_mutex` with process-global scratch buffers.

### `pmix_keyval_parse` — process-global state that outlives a run

Everything here is file-static: the key buffer, the lock, and `env_str`,
which is where the `-x FOO=bar` directives of *every* file parsed so far
pile up. They are not delivered as they are read — they are accumulated
into one `;`-separated string and handed over as a single pair named
`mca_base_env_list_internal` by `pmix_util_keyval_save_internal_envars()`,
which also frees it.

**That hand-off is not guaranteed to happen, and this state survives a
finalize.** `pmix_mca_base_var_cache_files()` returns on the first file
that fails to parse, *before* it reaches the store; and
`pmix_finalize_util()` clears `pmix_globals.util_initialized`, so
`pmix_init_util()` — and therefore `pmix_util_keyval_parse_init()` — runs
again in the same process. An `env_str` left standing is therefore not
just a leak: the next run is handed the previous run's variables.
`pmix_util_keyval_parse_finalize()` has to release it alongside the key
buffer. `test_finalize_drops_pending_envars` in
[`test/unit/util/util_keyval.c`](../../test/unit/util/util_keyval.c) pins
it.

**A malformed line is a warning; running out of memory is a return
code.** The caller acts on what `pmix_util_keyval_parse()` answers —
`pmix_mca_base_var_cache_files()` treats anything but `PMIX_SUCCESS` or
`PMIX_ERR_NOT_FOUND` as fatal — and the parse loop used to discard the
per-line return codes entirely, so a file it could not read reported
success. Only `PMIX_ERR_OUT_OF_RESOURCE` is propagated, and it stops the
parse. Syntax errors are deliberately *not* propagated: `parse_error()`
already reports them on stderr, the rest of the file is still read, and
turning a typo in an optional parameter file into an init failure is a
policy change, not a bug fix. If you want that behavior, decide it
deliberately.

Two more things the shape of this file invites:

- **`PMIX_ERR_NOT_FOUND` is the answer for "could not open", not for
  "does not exist".** The caller reads it as the latter and carries on,
  which is right for the default parameter files, most of which are
  absent — but it means an unreadable file discards every parameter in it
  and looks identical to no file at all. The code is kept (failing
  startup over an unreadable optional dotfile would be worse) and a
  non-`ENOENT` `errno` is now reported instead.
- **The callback runs with `keyval_mutex` held.** It is not recursive, so
  a callback that re-enters anything in `pmix_keyval_parse.h` deadlocks
  against itself. `save_value()` in `src/mca/base` does not; keep it that
  way.

The `NULL != pmix_util_keyval_yytext` test in `parse_line_new` is dead —
flex never leaves `yytext` NULL after a match, and the two
`save_param_name`-style paths `strlen()` it unguarded — but it is
harmless and is left alone.

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
`context_fns`, `environ`, `error`, `fd`, `few`, `hash` (incl. a mixed-qualifier
regression), `if` (the tuple parser), `name_fns` (incl. special-rank
compare), `net`, `os_dirpath`, `os_path`, `output`, `parse_options`
(incl. the bare-`-` crash regression), `path`, `printf`, `show_help`,
`string_copy`, `timings`, `getcwd`, `getid`, `keyval`, `pty` (the
controlling-terminal and descriptor-ownership regressions), `shmem` (the
symlink/stale-file halves of the create, and the reference-count
lifetime), `show_help` (the `#include` parser and the termination
flush).

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
  compile failures in those (CI-unexercised) build configs. Those stubs
  did **not** build afterwards either — see
  [`pmix_pty`](#pmix_pty--a-helper-that-must-not-take-a-terminal-for-itself)
  above; compile-check every arm rather than trusting this entry.
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
  `double`/`long` varargs via `va_arg(ap, int)` — corrected to the real
  types. That function has since been replaced outright; see
  [`pmix_printf`](#pmix_printf--two-implementations-only-one-of-which-you-compile)
  above for what it now guarantees.
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
