# AGENTS.md: `src/tools` — the user-facing PMIx command-line executables

This document orients AI agents and human contributors working in
`src/tools`. It assumes you have already read the top-level
[`AGENTS.md`](../../AGENTS.md) — the golden rules (prefix conventions,
`pmix_config.h`-first include order, constant-on-the-left comparisons,
brace-everything, warning-free under `--enable-devel-check`), the
thread-safety / progress-thread / caddy model, and the
copyright-header and contribution rules all apply here and are not
repeated. This file covers what is specific to `src/tools`.

## What this directory is

`src/tools` holds the **installed, user-facing executables** that ship
with PMIx. Each subdirectory builds exactly one `bin_PROGRAM`. They are
thin front-ends: nearly all real work is done by `libpmix` (the client,
tool, and common layers). A tool's job is to (1) bootstrap just enough of
the library to parse a command line, (2) translate CLI options into a
`pmix_info_t` array or a query/request, (3) call one public `PMIx_*` API,
and (4) print the result.

**Do not confuse `src/tools` with `src/tool` (singular).** `src/tool` is
the *tool-role library code* (`PMIx_tool_init` and friends) that lives
inside `libpmix`; `src/tools` (plural) is the collection of standalone
*programs* that link against that library. Most tools here call
`PMIx_tool_init`, but `pmix_info` and `pmixcc` do not connect to anything.

| Tool | One-liner | Connects to a server? |
|------|-----------|-----------------------|
| `pmix_info` | Report build config, version, MCA params, and component versions (the PMIx analogue of `ompi_info`) | No |
| `wrapper` (`pmixcc`) | Compiler wrapper — injects PMIx's CPPFLAGS/LDFLAGS/LIBS and execs the real compiler | No |
| `pattrs` | List the attributes/functions supported by the client, server, tool, or host | Only for `--host` (else `PMIX_TOOL_DO_NOT_CONNECT`) |
| `pquery` | Run `PMIx_Query_info_nb` for one or more keys (with optional qualifiers) | Yes |
| `pevent` | Generate/notify a PMIx event (`PMIx_Notify_event`) | Optional |
| `plookup` | `PMIx_Lookup` of published keys | Yes |
| `palloc` | Request/extend/shrink an allocation via `PMIx_Allocation_request_nb` | Yes (scheduler/controller) |
| `pctrl` | Job control (`PMIx_Job_control_nb`): pause/resume/kill/signal/checkpoint/… | Yes |
| `pps` | "PMIx ps" — intended to list namespaces/nodes/procs. **Skeleton only** (see below) | Yes |

## The shared skeleton every tool follows

The connecting tools (everything except `pmix_info` and `pmixcc`) are
copy-paste siblings. Reading one teaches you all of them; the flip side
is that **a bug fixed in one is usually present in the others** — grep the
siblings before assuming a fix is local. The common spine:

1. `signal(SIGPIPE, SIG_IGN)` — tolerate a consumer closing a pipe early.
2. Set `pmix_tool_basename` (used by `pmix_show_help` for the program name).
3. **Bootstrap the library enough to parse.** Two idioms exist:
   - The **long form** (`pmix_info`, `pps`, `pevent`, `plookup`, `pattrs`):
     `pmix_output_init()` → open+init `pinstalldirs` → `pmix_show_help_init()`
     → `pmix_util_keyval_parse_init()` → `pmix_mca_base_var_init()`.
   - The **short form** (`pquery`, `palloc`, `pctrl`, `pmixcc`):
     a single `pmix_init_util(NULL, 0, NULL)` that does the equivalent.
     Prefer `pmix_init_util` for new tools — it is the maintained wrapper.
4. `pmix_cmd_line_parse(argv, shorts, options, NULL, &results, "help-<tool>.txt")`.
5. Walk `results.instances` for `PMIX_CLI_PMIXMCA` and feed each value to
   `pmix_expose_param()` so `--pmixmca foo bar` works.
6. `pmix_register_params()` (long form) — the short form did it already.
7. Build a `pmix_info_t` array describing **how to connect** (see the
   connection ladder below), then `PMIx_tool_init(&myproc, info, ninfo)`.
8. Register a default event handler (all of them install a no-op
   `notification_fn` plus an `evhandler_reg_callbk` that wakes a lock).
9. Do the one operation, print, `PMIx_tool_finalize()`, return `rc`.

### The connection-selection ladder

The connecting tools share a near-identical if/else chain that inspects
the CLI and loads **one** connection directive into `info[0]`, in this
priority order: `--pid <int|file:PATH>` → `--namespace`/`--nspace` →
`--uri` → `--system-server-first` → `--system-server[-only]` →
(`palloc` only) `--connection-order`, `--system-controller`,
`--scheduler` → a tool-specific default. The `--pid file:PATH` branch
`fscanf`s a single `%lu` out of a rendezvous file. When you add a
connection option to one tool, decide deliberately whether the siblings
need it too.

### Command-line option tables

Options are declared as a `static struct option[]` built from
`PMIX_OPTION_DEFINE` / `PMIX_OPTION_SHORT_DEFINE` macros, terminated by
`PMIX_OPTION_END`, paired with a short-option string (e.g. `"h::vV"`).
Reusable option *names* come from `src/util/pmix_cmd_line.h`
(`PMIX_CLI_HELP`, `PMIX_CLI_PID`, `PMIX_CLI_URI`, …). Tool-specific
options are `#define`d locally at the top of the `.c` (e.g. `pevent`'s
`PMIX_CLI_RANGE`, `pattrs`'s `PMIX_CLI_CLIENT`). Keep the short-option
string and the table in sync: a short letter in the string with no
matching `PMIX_OPTION_SHORT_DEFINE` (or vice-versa) is a latent parse bug.

> **Positional arguments come back in `results.tail`.** After a parse,
> `results.tail` is the `NULL`-terminated vector of arguments the parser
> did not consume as options — the tool's positional operands (keys,
> event names, …), or `NULL` if there were none. Two parser quirks are
> worth knowing:
> - A **bare invocation** (just the program name) used to leave the
>   program name itself as `tail[0]`, defeating every tool's
>   "you-gave-me-no-arguments" guard. That was fixed in
>   `pmix_cmd_line_parse` (the `argc == 1` early-out now returns with
>   `tail == NULL` instead of falling through the `done:` copy); a bare
>   `pevent`/`plookup`/`pquery` now correctly prints its usage/guard.
> - A **positional placed before an option** (`pquery key --uri x`) is
>   still dropped by the parser's option/positional reordering — put
>   options first (`pquery --uri x key`). This is a `pmix_cmd_line`
>   limitation, not a per-tool bug.
>
> `pmix_info` additionally compares `join(results.tail)` against `argv[0]`
> as belt-and-suspenders; new tools need only null-check `results.tail`.

## `pmix_info` — the odd one out (no connection)

`pmix_info` never calls `PMIx_tool_init`. It drives the
`src/runtime/pmix_info_support.*` helpers directly: it builds an
`mca_types` pointer-array and a `pmix_component_map`, registers all
framework params, then dispatches on the flags (`--all`, `--config`,
`--param`, `--path`, `--arch`, `--hostname`, `--type`, `--version`).
With no arguments it prints a default digest. Its cleanup is meticulous
(it destructs the arrays and releases every `pmix_info_component_map_t`)
because it is the reference for "a tool that shuts the library down
cleanly." If you touch the info-dumping logic, the substance lives in
`src/runtime/pmix_info_support.c`, not here.

## `pmixcc` (the wrapper) — how it actually works

`pmixcc` is a self-contained compiler wrapper descended from Open MPI's
`opal_wrapper`. It does **not** use `PMIx_tool_init`. Flow:

- `data_init()` reads the installed **`pmixcc-wrapper-data.txt`** (built
  from [`wrapper/pmixcc-wrapper-data.txt.in`](wrapper/pmixcc-wrapper-data.txt.in)
  by `configure`, which substitutes `@WRAPPER_CC@`,
  `@PMIX_WRAPPER_CPPFLAGS@`, `@PMIX_WRAPPER_LIBS@`, …). The file is a
  keyval file parsed by `pmix_util_keyval_parse` into one or more
  `options_data_t` blocks. The wrapper flags themselves are decided in
  `config/pmix_setup_wrappers.m4` — **that** is where you change what
  `pmixcc` injects, not this `.c`.
- Environment overrides: `PMIX_CC`, `PMIX_CPPFLAGS`, `PMIX_CFLAGS`,
  `PMIX_LDFLAGS`, `PMIX_LIBS` (via `load_env_data*`).
- `-showme`, `-showme:compile`, `-showme:link`, `-showme:libs`,
  `-showme:incdirs`, `-showme:version`, `-showme:help`, … either print a
  slice of the assembled command line and exit, or (the first few) set a
  `COMP_WANT_*` bitmask and still build the full line.
- The final command line is assembled in a fixed order (compiler →
  prefix flags → user args → preproc → compile → link → libs) and run
  via `pmix_few()`. `-showme` alone is a dry run (`COMP_DRY_RUN`).

`pmixcc` is the most self-testable tool: every `-showme:*` path runs
without a server and exits 0. Use it in smoke tests (see below).

## `pps` — process/node status

[`pps/pps.c`](pps/pps.c) connects to a server (honoring the standard
connection ladder: `--pid`, `--namespace`, `--uri`, `--system-server`,
else system-first), queries `PMIX_QUERY_NAMESPACES` for the active
namespaces, and then, for each namespace, runs a `PMIX_QUERY_PROC_TABLE`
query (qualified by `PMIX_NSPACE`) and prints the returned
`pmix_proc_info_t` array. Two presentations:

- default — a per-process table (rank, pid, node, state, executable);
- `--nodes` — a per-node roll-up (how many of the job's processes run on
  each node).

The proc-table result is a `pmix_data_array_t*` of `pmix_proc_info_t`;
`get_proc_array()` defends against a server that returns something else
(e.g. a stub `query_fn`) by type-checking and degrading to "no process
information available" rather than dereferencing. The data-array
consumption mirrors [`examples/debugger.c`](../../examples/debugger.c).

Historical note: `pps` used to be a skeleton — everything below `main()`
was fenced out under `#if 0` and referenced Open-MPI/ORTE types
(`pmix_ps_mpirun_info_t`, `pmix_job_t`, `pmix_node_t`, …) that no longer
exist. That dead block has been removed; do not resurrect it. A real
proc-table needs a proc-table-capable server (e.g. PRRTE's `prted`); the
`test/simple/simptest` stub `query_fn` returns a string for every key, so
`pps` against it exercises only the graceful-degradation path.

## Help text and `show_help`

Each tool ships a `help-<tool>.txt`. These are **not** installed as data
files by the tool `Makefile.am` (the `EXTRA_DIST` line only puts them in
the tarball). Their content is compiled *into `libpmix`* through the
generated `src/util/pmix_show_help_content.c` — so a runtime
`pmix_show_help("help-foo.txt", "topic", …)` call resolves against that
compiled table regardless of which tool emits it. Consequences:

- **After editing any `help-*.txt`, regenerate the content pair** exactly
  as the top-level guide says:
  `rm src/util/pmix_show_help_content.* && make`. A plain `make` will
  *not* pick up the new text otherwise.
- Because all help content lives in one table, `pevent` and `pctrl`
  currently source their `--pid` error messages from **`help-pquery.txt`**
  (topics `bad-option-input`, `file-open-error`, `bad-file`) rather than
  their own files. It works, but it is a coupling smell: those topics must
  never be deleted from `help-pquery.txt`. Prefer adding the topics to a
  tool's own help file when you touch this.

## Build wiring

- Every tool is registered in [`Makefile.include`](Makefile.include)
  (`SUBDIRS` + `DIST_SUBDIRS`), which is `include`d by `src/Makefile.am`.
  **Adding a new tool means adding it in both lists** and creating a
  `Makefile.am` modeled on an existing one.
- Each tool `Makefile.am` guards its `bin_PROGRAMS` with
  `if PMIX_INSTALL_BINARIES` so a library-only install builds nothing
  here. `*_LDADD = $(top_builddir)/src/libpmix.la`.
- `pmix_info` and `pmixcc` carry the extra `AM_CFLAGS` block of
  `-DPMIX_BUILD_*` / `-DPMIX_CONFIGURE_*` defines (they print build
  provenance); the others do not need it.
- Editing only a `Makefile.am` needs just `make`. Adding a tool
  directory, or anything touched by `configure` (like the wrapper data
  file), requires the full `./autogen.pl && ./configure` regeneration —
  see the top-level guide.
- `.gitignore`: each tool dir ignores its built binary. A new tool needs
  the same (see `wrapper/.gitignore`).

## Testing

These are `main()` programs whose interesting helpers (`convert_signal`,
`convert_procs` in `pctrl`; the flag assembly in `pmixcc`) are `static`,
so classic unit tests can't link them without refactoring. The
regression coverage that *is* possible drives the **no-connect** paths of
the built binaries and asserts they parse, execute, and finalize without
crashing:

- [`test/unit/run_tools.pl`](../../test/unit/run_tools.pl) (wired into
  `make check`) exercises `pmix_info --version`, several `pmixcc
  -showme:*` variants, and `pattrs --client-fns/--server-fns/--tool-fns`.
  It deliberately avoids every server-connecting path because a
  developer's machine may have live PMIx servers (stale `pmix.*`
  rendezvous files in `$TMPDIR`) that make those paths non-deterministic.

When adding a tool, add its no-connect invocation to that script. Testing
the connecting tools (`pquery`, `palloc`, `pctrl`, `plookup`, `pevent`)
end-to-end needs a server; model that on `test/simple/simptest` +
`test/unit/run_*.pl` and keep it out of the default `make check` unless it
is fully self-contained.

## Fixed defects (July 2026 review)

A deep review of this directory surfaced the following defects, all now
**fixed** on `topic/tool-review`. Recorded here so the same problems are
not re-introduced by a future copy-paste.

- **`palloc` / `pctrl` double-free on the synchronous-completion path.**
  When `PMIx_Allocation_request_nb` / `PMIx_Job_control_nb` returned
  `PMIX_OPERATION_SUCCEEDED`, the code did `PMIX_RELEASE(req)` and then
  `goto done`, where `if (NULL != req) PMIX_RELEASE(req)` released the
  same caddy again. Fixed by nulling `req` after the inline release.
- **`palloc --signal` targeted the wrong attribute** (`PMIX_ALLOC_NODE_LIST`,
  copy-paste from `--nodelist`). Now parses `<signal>[@seconds]`, converts
  the signal name/number via a local `convert_signal()`, and loads
  `PMIX_SESSION_SIGNAL` (int).
- **`palloc` printed `req->key`, which was never set.** Now prints the
  returned `req->sessionid` (captured by `cbfunc`) when available.
- **`pctrl` leaked `req->key`, and neither tool freed the converted info
  array.** `scdes` (in `src/include/pmix_globals.c`) now frees
  `pmix_shift_caddy_t.key` — verified safe: the only setters of that field
  are `pctrl` (owned `strdup`) and `palloc` (never set); the `key`
  assignments in `src/server/pmix_server_get.c` are on *different* caddy
  types. Both tools now set `req->infocopy = true` so the destructor frees
  the `PMIx_Info_list_convert` array.
- **`pctrl convert_procs` NULL-derefed a target with no `:`** — now
  reports a "malformed target" error (and frees the split argv, which it
  also used to leak).
- **`pattrs` read an uninitialized `mq`.** Now static-initialized like
  `pquery`, plus a `ninfo == 0` guard before dereferencing `mq.info[0]`.
- **`pquery` freed only `info[0]` of a 3-element array** (`PMIX_INFO_FREE(info, 1)`
  → `info, n`) and **called `pmix_init_util()` twice** (the second call
  removed).
- **`pmixcc` filter typo** `"-L/usr/lb"` → `"-L/usr/lib"`.
- **`plookup` did not include `pmix_config.h`** (now the first include,
  replacing the manual `#define _GNU_SOURCE`) and **ignored `--pid`**
  (now honored via the standard int/`file:PATH` branch).
- **Bare-invocation guards were defeated** by the parser leaving the
  program name in `results.tail`; fixed at the `pmix_cmd_line_parse`
  source (see the callout above).
- **`pps` was a `#if 0` skeleton** — now a working tool (see the `pps`
  section).

**Note the false alarm we ruled out:** `pmix_attributes_print_headers()`
returns immediately for a `*_FUNCTIONS` level, so `pattrs`'s
`--host-fns` branch was *not* leaking headers — that call is a harmless
no-op, left as-is.

None of these were hot-path; they are correctness/robustness and hygiene
issues. When you touch one tool, check the copy-paste siblings for the
same shape, and land each logical fix as its own commit.

## When in doubt

- Match the surrounding tool's structure — they are intentionally uniform.
- The real behavior lives in `libpmix`; a tool should stay a thin
  translator of CLI → `pmix_info_t[]` → one `PMIx_*` call → print.
- Never add a bare `PMIx_*` public call that thread-shifts from inside a
  callback; the tools only call public APIs from `main()`, which is the
  safe place to do so.
