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

## Terminology

The following terms are used throughout the PMIx codebase and are defined precisely in **Chapter 2 of the PMIx Standard v5.0** (<https://pmix.org/standard>).  When reading or writing PMIx code, use these definitions — not informal or colloquial meanings.

| Term | Definition |
|------|-----------|
| **session** | A set of resources assigned by the Workload Manager (WLM) and reserved for one or more users. Identified by a session ID unique within the scope of the governing WLM. A session may be static (a fixed block of resources) or dynamic (resources accumulated from multiple allocation requests and managed as a single unit). |
| **job** | A set of one or more applications executed as a single user invocation within a session. Assigned a unique job ID by the RM or launcher. A single `mpiexec` invocation with multiple application specs is one MPMD job. A user may run multiple jobs within a session, sequentially or concurrently. |
| **namespace** / **nspace** | A character string assigned by the RM to a job. All applications in that job share the same namespace. Must be unique within the scope of the governing RM; often the string form of the numerical job ID. *Namespace* and *job* are used interchangeably in the Standard. |
| **application** | A set of identical (but not necessarily unique) execution contexts within a job. One job may contain multiple applications (MPMD). |
| **process** | An operating system process (heavyweight process), possibly comprising multiple lightweight threads. The Standard does not restrict "process" to a particular OS concept. |
| **client** | A process that was registered with the PMIx server *before* being started and connects to that server via `PMIx_Init`, using its pre-assigned namespace and rank. The connection information is provided at start of execution. |
| **tool** | A process that may or may not have been registered with the PMIx server before being started; initializes via `PMIx_tool_init`. Tools include debuggers, profilers, and workflow managers. |
| **clone** | A process directly started by a PMIx client (e.g., `fork`/`exec`) that calls `PMIx_Init` and connects to the local PMIx server using the same namespace and rank as its parent. |
| **rank** | The zero-based numerical position of a process within a defined scope. *Job rank* is the rank within the job (synonymous with unqualified "rank"); *application rank* is the rank within the application. |
| **peer** | Another process within the same job. |
| **resource manager (RM)** | The subsystem that hosts the PMIx server library. May be a vendor-supplied RM or a third-party agent such as a programming-model runtime library. |
| **host environment** | Used interchangeably with *resource manager*: the process that hosts the PMIx server library. |
| **scheduler** / **WLM** | The component of the system management stack (SMS) responsible for scheduling resource allocations. Also called the system workflow manager; *WLM* is used interchangeably with *scheduler* in the Standard. |
| **workflow** | An orchestrated execution plan involving multiple jobs under the control of a workflow manager. |
| **node** | A single operating system instance, which may encompass one or more physical objects. |
| **package** | A single object soldered or socketed onto a printed circuit board. May contain multiple chips (processing units, memory, peripheral interfaces). |
| **processing unit (PU)** | The electronic circuitry that executes instructions. May be a single hardware thread or multiple hardware threads organized as a core, depending on architecture. |
| **fabric** | The networks within a system, regardless of speed or protocol. Interchangeable with *network* throughout the Standard. |
| **fabric device** | An OS fabric interface, physical or virtual. Interchangeable with *Network Interface Card (NIC)*. |
| **fabric plane** | A collection of fabric devices in a common logical or physical configuration (e.g., a separate overlay or physical network controlled by a dedicated fabric manager). |
| **attribute** | A key-value pair: a string key (`pmix_key_t`) paired with a typed value. Attributes serve as both directives (qualifiers to APIs, passed in `pmix_info_t` arrays) and information identifiers (labeling the contents of a `pmix_value_t`). |
| **key** | The string component of an attribute. Passed to APIs such as `PMIx_Get` or `PMIx_Query_info` to identify requested information. Not all attributes can be used as keys; some are solely API qualifiers. |
| **instant on** | A PMIx concept: all information required for setup and communication — including the address vector of endpoints for every process — is available to each process at start of execution. |

### Normative language

The Standard also uses precise modal verbs whose meaning agents must respect when reading Standard text:

- **shall / must / will** — the behavior is required of all conforming implementations.
- **should / may** — the behavior is recommended or permitted, but not required.

### Naming conventions (from the Standard)

- Constants and attributes: `PMIX_` prefix.
- Structures and type definitions: `pmix_` prefix.
- String representations of attributes: `pmix` prefix.
- Client API functions: `PMIx_` prefix with mixed case after the first word — e.g., `PMIx_Get_version`.
- Server and tool API functions: `PMIx_` prefix, fully lower-case after — e.g., `PMIx_server_register_nspace`.

User code must not use the `PMIX_`, `PMIx_`, or `pmix_` prefixes to avoid symbol conflicts.

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

**Use the `__pmix_attribute_*__` macros for compiler attributes.**
  [`src/include/pmix_config_bottom.h`](src/include/pmix_config_bottom.h),
  pulled in transitively by `pmix_config.h`, defines portable wrappers —
  `__pmix_attribute_unused__`, `__pmix_attribute_noreturn__`,
  `__pmix_attribute_format__`, `__pmix_attribute_deprecated__`, and many
  more — that expand to the appropriate `__attribute__((...))` on
  compilers that support it and to nothing elsewhere. Reach for these
  (for example, to mark an unused function parameter) rather than writing
  a bare `__attribute__` or leaving a warning unaddressed.

**PMIx back-end code must never call public `PMIx_*()` APIs.** The
  bindings are thin wrappers; call the internal `pmix_*` routines, not
  the user-facing entry points.

**New files need the standard copyright/license header.** Copy the
  multi-institution BSD header block — including the `$COPYRIGHT$` and
  `$HEADER$` tokens — from a neighboring file. If you substantially
  change an existing file, add your copyright line to its block.

**`#define` logical macros to `0` or `1`; never `#undef` them.** Test
  with `#if FOO`, not `#ifdef FOO`, so a misspelling is a compiler
  error, not a silent false.

**Never reuse a status/event code value.** Every `PMIX_*` status and
  event constant in [`include/pmix_common.h.in`](include/pmix_common.h.in)
  must have a numeric value that is unique across the *entire* code base,
  including the deprecated constants in
  [`include/pmix_deprecated.h`](include/pmix_deprecated.h). Two active
  constants sharing a value are indistinguishable to event handlers and
  status comparisons — a latent, hard-to-trace bug. Before adding or
  changing a code, sweep both headers for the value you intend to use,
  e.g.:

  ```sh
  grep -hoE '^#define[[:space:]]+[A-Z0-9_]+[[:space:]]+-[0-9]+' \
      include/pmix_common.h.in include/pmix_deprecated.h \
      | awk '{print $3}' | sort -n | uniq -d
  ```

  The one legitimate exception is a deprecation *rename*: a deprecated
  alias in `pmix_deprecated.h` intentionally keeps the same value as the
  active constant that replaced it (e.g., `PMIX_ERR_EXISTS` and the
  deprecated `PMIX_EXISTS` are both `-11`). That is a rename, not a new
  code. Remember too that values in `-230`..`-330` are reserved as system
  events by the `PMIX_SYSTEM_EVENT()` macro range.

**Constant-on-left comparisons:** Always write `NULL == ptr` rather than `ptr == NULL`.  This turns typos (`=` instead of `==`) into compile errors.

**Always brace blocks:** Use `{ }` around every conditional or loop body, even single-line ones.

**Indentation:** 4 spaces, never tab characters.

**Spacing on conditional statements:** Use a space to separate the condition from the surrounding
  words — e.g., "if (condition) {" instead of "if(condition){". Separate conditions on long lines
  by putting the combining operator at the end of the preceding line

  ```c
  if (condition1 ||
      condition2) {
  ```

**Language standard:** C11 is required.  C++-style `//` comments are allowed and preferred.

**Stay compiler-warning-free.** PMIx strives to build with zero
  compiler warnings. Do not attempt to introduce code that adds new warnings. Git checkouts
  default to configure with `--enable-devel-check` - this instructs the compiler
  to error out on all warnings, and to warn on everything (e.g., unused variables).
  The CI checkers all operate with this option, so you won't pass CI if your
  code is generating warnings.

**No hand-editing of generated files:** Do not modify files produced by autotools (`configure`, `Makefile.in`, etc.), pre-rendered documentation, or third-party vendored code. Edit the source code instead.

**Update `.gitignore` for build products you introduce.** If your
change adds a new source file, component, or generated artifact that the
build produces something new from — a new executable or test binary, a
newly generated source/header, an object or library in a directory that
did not have one before — add the resulting build product to the
appropriate `.gitignore` so it does not show up as an untracked file.
Never commit the build product itself; ignore it. Check `git status`
after a clean build to confirm no generated file you created is left
untracked, and match the nearest existing `.gitignore` pattern style
(many component directories carry their own `.gitignore`).

**Never push a branch to `origin`.** The `origin` remote is the shared
upstream repository (`openpmix/openpmix`); pushing topic branches there
is not the project workflow. Always push your branch to your personal
fork remote instead, and open the pull request from the fork against
the upstream `master` (or the appropriate release branch). If you are
unsure which remote is the fork, run `git remote -v` and ask rather than
guessing.

**Regenerate `show_help` content after touching any help text.** The
`show_help` help messages are compiled into the library from a generated
source pair, [`src/util/pmix_show_help_content.c`](src/util/pmix_show_help_content.c)
and its header, which are built from the `help-*.txt` files. These
generated files are not refreshed automatically when the underlying help
text changes. After **any** add, delete, or modify of `show_help`
content — including adding a brand-new `show_help` file — you must
delete the generated pair and rebuild so the change is picked up:

```sh
rm src/util/pmix_show_help_content.*
make
```

Skipping this leaves the library emitting stale help output that no
longer matches the source.

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

## Modifying the configure / build system

Editing the build system means regenerating it — `make` alone can't,
and trying will wedge the tree. If you change `configure.ac` or any
`config/*.m4` file (including the embedded oac/Autotools macros), the
change does not take effect until the build system is regenerated. Do
not rely on a plain `make`: PMIx builds in maintainer mode, so
`make` auto-triggers a partial in-tree Autotools regeneration that
frequently fails (e.g., unexpanded `OAC_*` macros, `config.status`
errors) and can leave the tree half-regenerated and
unbuildable. Instead, regenerate and reconfigure explicitly:

```sh
./autogen.pl
./configure <same options as the original configure>
make -j
```

Recover the original configure invocation options from the existing
tree with `./config.status --config` (or read the header of
`config.log`). This process is slow but mandatory after any
build-system source change — there is no safe shortcut.

Note that editing `Makefile.am` files do *not* require the full
`autogen.pl` + `./configure` process.  A simple `make` will regenerate
the relevant `Makefile[.in]` files and then complete the build
successfully.


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

**Test across environments when you can.** Portability across a wide
variety of environments is a core PMIx goal. The primary development
environments are common Linux distributions and macOS, but the code is
expected to run far more widely. When container-based tooling is
available, it can be a practical way to reproduce, diagnose, and test
user-space behavior specific to an environment you aren't running
natively — for example, using Docker on macOS to exercise Linux
user-space code paths. (Containers don't replace real
network/hardware/launcher testing, but they're useful for OS and
user-space differences.)

**Add tests for new code.** Whenever practical, add unit tests under
[`test/unit`](test/unit) that are wired into `make check` (and therefore run in
CI). Prefer a `make check`-able test over a manual one-off so the
coverage sticks and regressions are caught automatically.

**Never bend a test to accommodate a bug.** Do not weaken, skip, or
rewrite an existing test — and do not craft a new one — merely to make
buggy behavior pass. Tests encode intended behavior: when one fails, the
default assumption is that the code is wrong, not the test. If you find a
genuine bug in the code base, identify it, report it, and where
appropriate fix it — don't paper over it in the test suite.


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

## The Role of Attributes

Attributes are the primary mechanism by which PMIx APIs are extended over time.  Rather than introducing new API entry points for every new capability, PMIx defines a small set of generic APIs that accept an array of `pmix_info_t` key-value pairs.  New and expanded functionality is delivered by defining one or more new attributes and passing them to an existing API — the API signature stays the same, so callers that do not need the new behavior are completely unaffected.

**Every PMIx API is required to include an attribute array in its parameter list.**  A function that accepts no attributes today must still declare a `pmix_info_t info[], size_t ninfo` parameter pair so that attributes can be added in the future without changing the signature.  Any proposed API that omits these parameters will be rejected.

This design has two practical consequences for contributors:

1. **Prefer a new attribute over a new API.** Before adding a new `PMIx_*` function, ask whether the desired behavior can be expressed as an attribute on an existing call.  In most cases it can.  A new attribute is far less disruptive: it does not alter any existing call sites, does not require version-gating in callers, and does not increase the size of the public API surface that must be supported forever.

2. **Document every new attribute fully.**  Because attributes are the extension point, their definitions carry more weight than individual API parameters.  Each new attribute must have: a string key (e.g., `"pmix.example.attr"`), a data type, a clear prose description of its semantics, and a note on which APIs accept it and in which direction (`IN` or `OUT`).  Add the definition to [`include/pmix_common.h`](include/pmix_common.h) following the conventions of the surrounding entries.

## Backward Compatibility

PMIx is required to continue supporting deprecated APIs and attributes indefinitely.  Applications in production HPC environments may have been built years ago against an older PMIx release and cannot easily be recompiled.  Containers and pre-built binaries compound this: a deprecated symbol removed from the library silently breaks a workload with no actionable error message for the user.

The rules are strict:

- **An API signature cannot be altered.** Once a function appears in an official PMIx release, its name, return type, and parameter list are frozen.
- **A data type definition cannot be altered.** Struct layouts and typedef'd types are ABI; changing them breaks binary compatibility.
- **An attribute cannot be removed.** A string key published in a release must continue to be accepted (and handled gracefully) by all future releases.

### Deprecating a user-facing API

1. Move the existing function declaration from [`include/pmix.h`](include/pmix.h) to [`include/pmix_deprecated.h`](include/pmix_deprecated.h).  This preserves the symbol for existing callers while signalling that it should not be used in new code.
2. Define the replacement function in the same location in [`include/pmix.h`](include/pmix.h) where the original appeared.  The replacement name should be the original name with a `2` appended (e.g., `PMIx_Foo` → `PMIx_Foo2`), or a higher integer if `2` is already taken.  Give it the revised signature (new return type and/or parameter list).
3. Update the accompanying description in the header and in [`docs/`](docs/) to explain what changed and point users to the replacement.

### Deprecating a server module function

The `pmix_server_module_t` struct in [`include/pmix_server.h`](include/pmix_server.h) is consumed by host environments (RMs, launchers) that may initialize it with positional initializers rather than designated initializers.  **The order of fields in this struct must never change.**

1. Leave the deprecated field in place and its associated typedef unmodified.  Add a comment to the field marking it `// DEPRECATED` so readers know not to use it.
2. Add a new `pmix_server_*_fn_t` typedef that supports the new signature near its predecessor in the pmix_server.h header. Provide an updated description that explains the modified signature.
3. Add the replacement field using the new typedef at the **end** of the struct.  Name it by appending or incrementing a numeric suffix: `client_connected` → `client_connected2`; if `client_connected2` already exists, use `client_connected3`.

## Compatibility Flags in Configure

A particular version of PMIx can indicate its level of support for critical APIs and capabilities via inclusion of **capability flags** in its version header ([`include/pmix_version.h.in`](include/pmix_version.h.in)).

### When to define a new flag

Add a new `PMIX_CAP_*` flag whenever:
- A new public API is added.
- A new utility or behavior is introduced that a companion project (such as PRRTE) might need to detect and compensate for.
- Functionality within the library has fundamentally changed in a way that a consumer cannot reliably infer from the release version numbers alone.

Do **not** add a flag for internal refactors that have no externally visible effect.

### How to define a flag

**Edit the `.in` templates, not the generated headers.**
[`include/pmix_version.h`](include/pmix_version.h) and [`include/pmix_common.h`](include/pmix_common.h) are both generated by `./configure` from their respective `.in` templates ([`pmix_version.h.in`](include/pmix_version.h.in) and [`pmix_common.h.in`](include/pmix_common.h.in)) and are listed in `.gitignore`; hand-edits to either generated file are silently discarded on the next configure run and must never be committed.

Add a `#define` to [`include/pmix_version.h.in`](include/pmix_version.h.in) in the **Individual capability flags** block, immediately after the last existing flag.  The value is arbitrary (sequential integers are the convention); what matters is that the definition **exists**:

```c
#define PMIX_CAP_MY_NEW_FEATURE   N   /* next sequential integer after the last existing flag */
```

Only flags that are actually supported by the version being built should appear in this file.  The absence of a definition is the signal that the capability is not present — do not add a flag and then conditionalize it out.

The new flag will appear in the installed `pmix_version.h` only after `./configure` is re-run.  Until then, the generated header will not reflect the change.

### How consumers check flags

Companion projects use the `AC_PREPROC_IFELSE` idiom in their configure scripts to test for a flag at build time.  PRRTE's `config/prte_setup_pmix.m4` is the canonical example; it defines a helper macro `PRTE_CHECK_PMIX_CAP` that reduces the check to:

```m4
PRTE_CHECK_PMIX_CAP([MY_NEW_FEATURE],
    [action if supported],
    [action if not supported])
```

The macro succeeds when `PMIX_CAP_MY_NEW_FEATURE` is defined in the installed `pmix_version.h`; it fails (and triggers the third argument) when the definition is absent, meaning the running PMIx predates the feature.

### The deprecated `PMIX_CAPABILITIES` bitmask

Earlier releases exported a composite `PMIX_CAPABILITIES` bitmask formed by OR-ing a fixed set of flag values.  This approach proved unworkable as the flag space grew, and has been abandoned in favor of presence-or-absence of individual `#define`s.  The bitmask and its constituent flags remain in [`include/pmix_version.h.in`](include/pmix_version.h.in) solely for backward compatibility with older consumers that still check `PMIX_CAPABILITIES`; do not extend or rely on it for new work.

## Version Interoperability

PMIx is required to be interoperable across released versions.  A server built against one PMIx version must be able to communicate with a client built against a different version, whether older or newer.

This requirement is especially critical in **static deployment environments such as containers**.  A containerized application image bundles a specific PMIx client library at build time and cannot be updated independently of the image.  When that container is later scheduled on a cluster whose RM hosts a different PMIx server version — possibly newer, possibly older — the two must still interoperate correctly.  Breaks in wire-format or behavioral compatibility silently strand jobs or produce incorrect results with no easy path to remediation short of rebuilding the image.  The same concern applies to any environment where components are independently installed and upgraded: bare-metal HPC clusters with vendor-supplied software stacks, pre-built application binaries distributed through package managers, and long-lived batch jobs that span a system software upgrade.

**Wire-format stability rules:**
- The order and content of fields in any message must not change between versions.
- New information may only be appended to the end of an existing message; nothing may be inserted, removed, or reordered.
- Readers must tolerate trailing data they do not recognize (forward compatibility) and must not require fields that did not exist in older versions (backward compatibility).

**Adding new data types:**
New data types may be introduced in new PMIx versions.  The `bfrops` framework is the correct place to add serialization/deserialization support for them.  Each `bfrops` component corresponds to a specific wire-format version; older components remain in the tree to support communication with older peers.  Do not modify the pack/unpack logic of an existing `bfrops` component in a way that changes its wire representation — create a new component (version) instead.

## Working in a shared repository

Don't assume you're the only agent (or person) using this clone. In
particular, if you're working in a **git worktree**, other worktrees may
be active against the same underlying repository at the same time. Avoid
repo-wide git commands that reach outside your own working area and can
disrupt others — for example, `git worktree prune`, or `git stash`
(which writes to the repository-wide stash ref shared by all worktrees).
Keep your git operations scoped to your own branch and worktree.

As a narrow exception, creating a **new branch** when you need to park
work in progress (for example, instead of `git stash`) is fine. Just be
careful not to collide with branches that other agents or people may be
using in the same clone — pick a clearly-scoped, unlikely-to-clash name.

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
- **Keep incidental fixes as their own commits.** Small "drive-by" bug
  fixes you notice while working on something else are welcome, but it is
  usually best to land them as standalone commits, separate from your
  main change, so each can be evaluated and reviewed on its own. One
  logical change per commit keeps history reviewable and easy to bisect.
- **Branch flow:** **Never commit directly to `master` or to a release
  branch.** All work — including the very first commit in a sequence —
  goes on a dedicated topic branch that you create before making any
  changes.

  - **Topic branches for `master`** are named `topic/<short-description>`,
    where the description is a few words that identify the work
    (e.g., `topic/fix-ptl-uaf`, `topic/cap-flags`, `topic/log-doc`).
    A topic branch may carry as many commits as the change requires;
    the requirement is one branch per logical unit of work, not one
    commit.
  - **Topic branches for release branches** follow a different naming
    scheme: `cmrNN/xxx`, where `NN` is the two-digit release series
    number (drop the dot) and `xxx` is the short description.
    For example, changes destined for the `v6.1.x` release branch are
    developed on a branch named `cmr61/<short-description>`.

  Once the topic branch is ready, open a pull request targeting
  `master` (or the appropriate release branch). After merge,
  cherry-pick to the relevant release branch(es) `vMAJOR.MINOR.x`
  with a `(cherry picked from commit ...)` line at the end of the
  commit message; use `git cherry-pick -x` to add it automatically.
  Never commit features directly to a release branch. See
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
