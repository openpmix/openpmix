# AGENTS.md: Guidance for AI Contributors to PMIx

This document is an orientation guide for AI coding agents and their human operators working in the PMIx (OpenPMIx) source tree.

## Overview

PMIx (Process Management Interface - Exascale) is an open-source reference implementation of the PMIx Standard, a low-level API used by resource managers and launchers to communicate with processes in high-performance computing environments.  It provides a framework through which programming models, tools, and runtime systems can exchange information without depending on a specific resource manager or job launcher.

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

## Key Coding Rules

**Include order:** `pmix_config.h` must be the very first `#include` in every C source file, before any system headers.

**Prefix rule:** All internal symbols and filenames must carry the appropriate prefix to avoid namespace collisions:
- Public API symbols: `PMIx_` (functions) / `PMIX_` (macros/types)
- Internal library symbols: `pmix_`
- Framework-scoped symbols: `pmix_FRAMEWORK_` (e.g., `pmix_ptl_`)
- Component-scoped symbols: `pmix_FRAMEWORK_COMPONENT_` (e.g., `pmix_ptl_client_`)

**Symbol visibility:** Use `PMIX_EXPORT` on any symbol that must be visible outside `libpmix.so`.  Do not mark internal symbols with `PMIX_EXPORT`.

**Constant-on-left comparisons:** Always write `NULL == ptr` rather than `ptr == NULL`.  This turns typos (`=` instead of `==`) into compile errors.

**Always brace blocks:** Use `{ }` around every conditional or loop body, even single-line ones.

**Indentation:** 4 spaces, never tab characters.

**Language standard:** C99 is required.  C++-style `//` comments are allowed and preferred.

**No hand-editing of generated files:** Do not modify files produced by autotools (`configure`, `Makefile.in`, etc.), pre-rendered documentation, or third-party vendored code.

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

## Performance Considerations

PMIx is used in latency-critical paths during job launch and process management at scale.  Avoid adding heap allocations, lock acquisitions, or branches to hot paths without first measuring the impact.  Microseconds matter at the scale PMIx targets.

## Contributing Requirements

- Every commit **must** include a `Signed-off-by:` line (`git commit -s`).  Pull requests without it will not be accepted.
- Write descriptive commit messages explaining *why* the change is made, not just what changed.
- Update documentation under `docs/` for any user-visible behavior change.
- Target the `master` branch for new development; use appropriate cherry-pick procedures for backports to release branches.
- Open a pull request against [https://github.com/openpmix/openpmix](https://github.com/openpmix/openpmix).
