<!--
  Copyright (c) 2026      Nanook Consulting  All rights reserved.

  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
-->

# AGENTS.md: Guidance for AI and Human Contributors to PMIx

This document is an orientation guide for AI coding agents and their human operators working in the PMIx (OpenPMIx) source tree.
This file is an *orientation map*, not the
full rulebook: the authoritative, human-maintained documentation lives
under [`docs/developers/`](docs/developers/) and
[`docs/contributing.rst`](docs/contributing.rst) (rendered at
<https://docs.openpmix.org/>). When this file and those docs disagree,
**the docs win** — and please fix this file.

AI-assisted contributions are welcome. But PMIx runs on the largest
supercomputers in the world and across a huge range of operating
systems and hardware. We want careful, portable code — not
plausible-looking code that solves one problem in one environment at the
expense of others. Hold yourself to the same bar as a thoughtful human
contributor.

## Overview

PMIx (Process Management Interface - Exascale) is an open-source reference implementation of the PMIx Standard (see <https://pmix.org/standard>), a low-level API used by resource managers and launchers to communicate with processes in high-performance computing environments.  It provides a framework through which programming models, tools, and runtime systems can exchange information without depending on a specific resource manager or job launcher.

Note that you will see **OpenPMIx** frequently referred to
as just **PMIx**. While there is a separate PMIx Standard, there
are (as of this writing) no alternative implementations of that
Standard. In fact, the Standard post-dates the library by several
years, and often lags behind the library in terms of new definitions.
Thus, it is customary to refer to the library as just **PMIx** and
drop the longer name - at least, until some other implementation
arises (which many consider unlikely).


## Single-Layer Architecture

Unlike multi-layer projects, PMIx has a single abstraction layer that generates one top-level shared library: **`libpmix`**.  All source lives under `src/` and is organized into functional subsystems:

| Directory | Purpose |
|-----------|---------|
| `src/class/` | C++-like "classes" built on the PMIx class system |
| `src/client/` | Client-side PMIx API implementation |
| `src/common/` | Code shared between client, server, and tool roles |
| `src/event/` | Event-loop integration (libevent wrapper) |
| `src/hwloc/` | Hardware locality (hwloc) integration |
| `src/include/` | Top-level internal headers (e.g., `pmix_config.h`) |
| `src/mca/` | MCA frameworks and components |
| `src/runtime/` | Library startup and shutdown |
| `src/server/` | Server-side PMIx API implementation |
| `src/threads/` | Threading utilities and locks |
| `src/tool/` | Tool-role support |
| `src/tools/` | User-facing executables (`pmix_info`, etc.) |
| `src/util/` | Miscellaneous utility code |

The public C headers live under `include/` at the top level of the repository.

## MCA: Modular Component Architecture

PMIx is built almost entirely out of MCA plugins.
Read [`docs/developers/terminology.rst`](docs/developers/terminology.rst)
and the MCA section it points to before doing real work. The hierarchy:

- **Project** → **Framework** → **Component** → **Module** (runtime
  instance, like a C++ object). Each level is isolated from its
  siblings; a framework exposes a top-level header for its public API.
- Example: `src/mca/preg/` is the PREG framework;
  `src/mca/preg/raw/` is one component within it.
- **MCA parameters** let users change behavior at run time (env var,
  file, CLI). **Prefer adding an MCA parameter over hard-coding a
  constant** — this is idiomatic and expected here.

PMIx uses plugins organized as **Framework → Component → Module**.  The MCA tree layout is strictly:

```
src/mca/FRAMEWORK/COMPONENT/
```

No directory under `src/mca/` may break this pattern (except `base/` subdirectories, which hold framework infrastructure, not components).

### Frameworks

| Framework | Description |
|-----------|-------------|
| `bfrops` | Buffer Operations — pack/unpack, copy, print, compare data types; multi-select (one component per wire format version) |
| `gds` | Generalized DataStore — stores job-level and peer data; assign-on-demand per peer |
| `pcompress` | Compression of large data objects |
| `pdl` | Dynamic library (dlopen) abstraction |
| `pgpu` | GPU resource discovery and support |
| `pif` | Network interface discovery |
| `pinstalldirs` | Installation directory resolution |
| `plog` | Logging of user-provided alerts |
| `pmdl` | Programming Model support — collects env-vars and parameters for MPI, OpenMP, etc. |
| `pnet` | Network support and endpoint computation for "instant on" launch |
| `preg` | Regular expression generator and parser; multi-select |
| `prm` | Resource Manager translation of generic PMIx directives |
| `psec` | Security operations (connection handshakes); multi-select |
| `psensor` | Process monitoring, resource utilization, and health checks |
| `psquash` | Internal integer squashing during transmission |
| `pstat` | System statistics at process, node, and disk levels |
| `ptl` | Transport Layer for client-server and tool-server communication; single-select |

**Single-select vs. multi-select:** A single-select framework activates exactly one component at a time (`ptl` is an example).  A multi-select framework can have several components active simultaneously (`bfrops`, `preg`, `psec`).  Check the framework header comment before assuming which kind you are modifying.

### Adding or Modifying Components

1. Place new component code in `src/mca/FRAMEWORK/COMPONENT/`.
2. Follow the existing naming convention: filenames use `framework_component_*` prefixes; public symbols use `pmix_framework_component_` prefixes.
3. Register an `open`, `close`, and `query` function in the component struct.
4. Wire the component into the build system via `Makefile.am` changes in the component directory.

## Golden rules (the things agents most often get wrong)

From [`docs/developers/source-code.rst`](docs/developers/source-code.rst)
and [`docs/contributing.rst`](docs/contributing.rst):

**Include order:** `pmix_config.h` must be the very first `#include` in every C source file, before any system headers.

**Prefix rule:** All internal symbols and filenames must carry the appropriate prefix to avoid namespace collisions:
- Public API symbols: `PMIx_` (functions) / `PMIX_` (macros/types)
- Internal library symbols: `pmix_`
- Framework-scoped symbols: `pmix_FRAMEWORK_` (e.g., `pmix_ptl_`)
- Component-scoped symbols: `pmix_FRAMEWORK_COMPONENT_` (e.g., `pmix_ptl_client_`)

**Symbol visibility:** Use `PMIX_EXPORT` on any symbol that must be visible outside `libpmix.so`.  Do not mark internal symbols with `PMIX_EXPORT`.

**PMIx back-end code must never call public `PMIx_*()` APIs.** The
  bindings are thin wrappers; call the internal `pmix_*` routines, not
  the user-facing entry points.

**New files need the standard copyright/license header.** Copy the
  multi-institution BSD header block — including the `Copyright (c) 2026      Nanook Consulting  All rights reserved.
  multi-institution BSD header block — including the `$COPYRIGHT$` and
  `$HEADER$` tokens — from a neighboring file. If you substantially
  change an existing file, add your copyright line to its block.

**`#define` logical macros to `0` or `1`; never `#undef` them.** Test
  with `#if FOO`, not `#ifdef FOO`, so a misspelling is a compiler
  error, not a silent false.

**Constant-on-left comparisons:** Always write `NULL == ptr` rather than `ptr == NULL`.  This turns typos (`=` instead of `==`) into compile errors.

**Always brace blocks:** Use `{ }` around every conditional or loop body, even single-line ones.

**Indentation:** 4 spaces, never tab characters.

**Language standard:** C11 is required.  C++-style `//` comments are allowed and preferred.

**Stay compiler-warning-free.** PMIx strives to build with zero
  compiler warnings. Do not attempt to introduce code that adds new warnings. Git checkouts
  default to configure with `--enable-devel-check` - this instructs the compiler
  to error out on all warnings, and to warn on everything (e.g., unused variables).
  The CI checkers all operate with this option, so you won't pass CI if your
  code is generating warnings.

**No hand-editing of generated files:** Do not modify files produced by autotools (`configure`, `Makefile.in`, etc.), pre-rendered documentation, or third-party vendored code. Edit the source code instead.

## Building and Testing

PMIx uses GNU Autotools.  From a clean clone:

```sh
./autogen.pl
./configure --prefix=/path/to/install
make -j$(nproc)
make install
```

Run the test suite with:

```sh
make check
```

Inspect available MCA parameters for any component with:

```sh
pmix_info --param FRAMEWORK COMPONENT
```

**"Did I break it?" — layered:**

1. **Build cleanly.** A clean `make` after your change is the baseline.
   PMIx is highly configurable at build time: many components,
   source files, directories, and generated artifacts are selected or
   omitted by `configure` and Automake based on the local environment.
   For any change to code or documentation that might be conditionally
   built, verify that your configured build is actually compiling or
   generating the thing you changed; do not assume this can be checked
   only by looking for `#if` blocks.
2. **Documentation-only changes can be narrower.** If the change is
   wholly under [`docs/`](docs/), it is often enough to configure with
   Sphinx support and run `make` in the `docs/` build directory instead
   of doing a full build, install, smoke test, or `make check`. Make
   sure Sphinx was really enabled by `configure`; one practical check is
   that `SPHINX_BUILD` in `config.status` names a valid executable, and
   that the summary at the end of configure includes the line
   "HTML docs and man pages: building and installing"
3. **Quick smoke test.** After `make install`, change to the `test` directory
   and run the test suite with:

	```sh
	make check
	```

	Then change to the `simple` directory below `test` and run:

	```sh
	./simptest
	```

**Add tests for new code.** Whenever practical, add unit tests under
[`test/unit`](test/unit) that are wired into `make check` (and therefore run in
CI). Prefer a `make check`-able test over a manual one-off so the
coverage sticks and regressions are caught automatically.


## Thread Safety and the Progress Thread

PMIx uses [Libevent](https://libevent.org/doc/) to drive an internal **progress thread**.  All internal library operations must execute inside this thread.  Code that runs outside it (e.g., the public API entry points, callbacks invoked by the caller's threads) must never touch shared library state directly — it must be **thread-shifted** first.

### Thread-shifting with caddies

A **caddy** is a short-lived heap object whose sole job is to carry a request's parameters across the thread boundary into the progress thread.  Every caddy struct must contain at minimum:

| Field | Type | Purpose |
|-------|------|---------|
| ev | `pmix_event_t` | Required by Libevent to queue the caddy; **must be named `ev`** |
| lock | `pmix_lock_t` | Thread synchronization (blocking APIs wait on this; handlers wake it) |
| cbdata | `void *` | Opaque pointer passed through to the callback |
| callback pointer(s) | function pointer(s) | Cache the caller-supplied callback function(s) |

The pattern:

1. Allocate a caddy with `PMIX_NEW(caddy_type_t)`.
2. Assign the caddy's fields to point at the caller's input parameters — **do not copy the data**.
3. Call `PMIX_THREADSHIFT(cd, handler_fn)` to post the caddy to the progress thread's event queue.
4. The progress thread fires `handler_fn(cd)`, which performs the actual work.

Never read or write shared library state before the `PMIX_THREADSHIFT`; do it only inside the handler that runs on the progress thread.

**Callers are required to keep all input parameters valid until their callback is invoked.**  This contract exists precisely so that caddies can hold pointers rather than copies.  Copying data into a caddy wastes allocation and free overhead and should not be done.  Exceptions exist but are rare; any departure from the no-copy rule requires explicit justification and team consensus before it is introduced.

### Blocking APIs

If the public API is **blocking** (no user callback, or `NULL` callback):

```c
pmix_lock_t mylock;
PMIX_CONSTRUCT_LOCK(&mylock);
cd->opcbfunc = internal_opcbfunc;   // sets mylock.status and wakes lock
cd->cbdata   = &mylock;
PMIX_THREADSHIFT(cd, _do_the_work);
PMIX_WAIT_THREAD(&mylock);          // caller blocks here
rc = mylock.status;
PMIX_DESTRUCT_LOCK(&mylock);
```

The handler running on the progress thread **must** call `PMIX_WAKEUP_THREAD(&cd->lock)` (or the equivalent through the internal callback) before it returns, or the caller will hang.

### Non-blocking APIs

If the public API is **non-blocking** (user supplies a callback):

```c
PMIX_THREADSHIFT(cd, _do_the_work);
return PMIX_SUCCESS;   // return immediately; result delivered via callback
```

The handler must:
1. Invoke the user's callback function with the result.
2. Release the caddy (e.g., `PMIX_RELEASE(cd)`) to free its memory.

It must **not** call `PMIX_WAKEUP_THREAD` in the non-blocking path — there is no thread waiting to wake.

### Reference implementation

`src/server/pmix_server.c` is the canonical reference.  The `PMIx_server_register_nspace` / `_register_nspace` pair shows both paths: when `cbfunc == NULL` the API constructs a local lock, substitutes an internal callback, and blocks; when a callback is provided it simply thread-shifts and returns.  Study that file before writing a new API or modifying an existing one.

### Rules to remember

- **Never touch shared state outside the progress thread** — always thread-shift first.
- **Do not copy input data into caddies.** Assign pointers instead; callers guarantee the data stays live until the callback fires.  Copying is the wrong default and requires explicit justification and team consensus.
- **Every blocking path needs a matching `PMIX_WAKEUP_THREAD`** in the handler.
- **Every non-blocking path must release the caddy** when the callback fires.
- Caddy structs must include a `pmix_event_t` field; without it `PMIX_THREADSHIFT` will corrupt memory.
- Do not allocate a caddy on the stack — it must outlive the function that creates it.

## Performance Considerations

PMIx is not generally used in latency-critical paths, but it is important that the code remain time-sensitive: PMIx can be used as a dependency by time-sensitive libraries, so unnecessary overhead is never acceptable. Concrete rules for hot paths include:

- **Don't add allocations, locks, or branches to the critical
  hot paths** without a clear, measured justification.
- **Guard debug output and expensive assertions behind
  `PMIX_ENABLE_DEBUG`** so release builds pay nothing for them.
- **Prefer an MCA parameter** to a hard-coded constant when a value
  might need tuning per environment.

## Version Interoperability

PMIx is required to be interoperable across released versions.  A server built against one PMIx version must be able to communicate with a client built against a different version, whether older or newer.

**Wire-format stability rules:**
- The order and content of fields in any message must not change between versions.
- New information may only be appended to the end of an existing message; nothing may be inserted, removed, or reordered.
- Readers must tolerate trailing data they do not recognize (forward compatibility) and must not require fields that did not exist in older versions (backward compatibility).

**Adding new data types:**
New data types may be introduced in new PMIx versions.  The `bfrops` framework is the correct place to add serialization/deserialization support for them.  Each `bfrops` component corresponds to a specific wire-format version; older components remain in the tree to support communication with older peers.  Do not modify the pack/unpack logic of an existing `bfrops` component in a way that changes its wire representation — create a new component (version) instead.

## Contributing

Authoritative process:
[`docs/contributing.rst`](docs/contributing.rst). Highlights agents must
honor:

- **Sign off every commit.** Each commit needs a `Signed-off-by:` line
  per the Contributor's Declaration — use `git commit -s`. Commits
  without it are not accepted. This applies to AI-assisted work too: the
  human submitter certifies the contribution.
- **Commit messages:** a short first line saying *what* changed, then a
  body explaining *why*. PMIx does **not** use Conventional Commits
  (`feat:`/`fix:` prefixes) — write prose. Don't add AI tooling
  attribution. Wrap commit-message lines at around 75 characters.
- **Branch flow:** land on `master` first via a GitHub pull request, then
  cherry-pick to the relevant release branch(es) `vMAJOR.MINOR.x` with a
  `(cherry picked from commit ...)` line at the end of the commit
  message; use `git cherry-pick -x` to add it. Never commit features
  directly to a release branch. See
  [`docs/developers/git-github.rst`](docs/developers/git-github.rst).
- **Update the docs and the news** when user-visible behavior
  changes: RST under [`docs/`](docs/), and a release-notes entry under
  [`docs/news/`](docs/news/)
  (`news-vMAJOR.x.rst`).


## When in doubt

- Match the surrounding code's style and conventions — this is an old,
  multi-author code base with established patterns.
- Read any relevant [`docs/developers/`](docs/developers/) or
  [`docs/how-things-work/`](docs/how-things-work/) page before
  inventing a new pattern.
- Ask on a GitHub issue for anything large before writing it.
