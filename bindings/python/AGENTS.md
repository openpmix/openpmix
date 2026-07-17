# AGENTS.md: PMIx Python Bindings (`bindings/python/`)

Orientation for AI and human contributors working on the PMIx Python
bindings. This is a *map*, not the rulebook — the top-level
[`CLAUDE.md`/`AGENTS.md`](../../CLAUDE.md) at the repo root and the docs under
[`docs/`](../../docs/) are authoritative. When this file and those disagree,
they win — and please fix this file.

The bindings are a **Cython** extension named `pmix` that wraps `libpmix`. A
Python program does `from pmix import *` and drives PMIx through four classes
(`PMIxClient`, `PMIxServer`, `PMIxTool`, `PMIxScheduler`).

---

## 1. File map — what is source, what is generated

**Never hand-edit a generated file.** Editing the wrong file means your
change is silently discarded on the next build.

| File | Role | Edit? |
|------|------|-------|
| `pmix.pyx` | **Source.** The four role classes and all module-level callback trampolines. The heart of the bindings. | ✅ |
| `pmix.pxi` | **Source.** Cython `include`d by `pmix.pyx`. All the Python↔C conversion helpers (`pmix_load_*`, `pmix_unload_*`, `pmix_free_*`), the `myLock` class, and the `pmix_pyshift_t` caddy. | ✅ |
| `construct.py` | **Source.** A code generator: harvests `#define`/enum/typedef constants and API/type names from the installed C headers and emits `pmix_constants.pxi` and `pmix_constants.pxd`. | ✅ |
| `setup.py` | **Source.** setuptools/Cython build driver. Reads `PMIX_BINDINGS_TOP_{BUILDDIR,SRCDIR}` env vars to find `pmix.pyx` and the include dirs; derives the version from `include/pmix_version.h`. | ✅ |
| `Makefile.am` | **Source.** Automake wiring: runs `construct.py`, then `setup.py`, and installs the egg. | ✅ |
| `requirements.txt` | **Source.** `cython`, `setuptools`. | ✅ |
| `pmix_constants.pxi` | **Generated** by `construct.py`. Runtime constant values (`PMIX_SUCCESS = 0`, …). Its first line, `from pmix_constants cimport *`, pulls in the `.pxd`. | ❌ |
| `pmix_constants.pxd` | **Generated** by `construct.py`. `cdef extern` declarations of every C struct/type/function the bindings call. | ❌ |
| `pmix.c` | **Generated** by Cython from `pmix.pyx`. ~7 MB. Never read or edit it to understand behavior — read the `.pyx`/`.pxi`. | ❌ |
| `Makefile`, `Makefile.in` | **Generated** by Autotools. | ❌ |
| `build/`, `pypmix.egg-info/` | **Generated** build products. | ❌ |

`tests/` under this directory holds a small Cython round-trip smoke test
(`tests/cython/`) and legacy copies of the connected test scripts
(`tests/python/`). The *maintained* test scripts live in the top-level
[`test/python/`](../../test/python/) directory (see §7).

---

## 2. The class hierarchy

```
PMIxClient
    └── PMIxServer
            └── PMIxTool
                    └── PMIxScheduler
```

Each subclass *is a* superset of the one above: a `PMIxServer` can call every
`PMIxClient` method, a `PMIxTool` adds tool-attach/IOF, and `PMIxScheduler`
adds session/allocation direction. The classes are Cython `cdef class`es;
their C-level state (`pmix_proc_t myproc`, `pmix_server_module_t myserver`,
…) is declared with `cdef` at class scope and is not visible from Python.

Method → C-API coverage (and the gaps) is tracked in
[`MISSING_BINDINGS.md`](MISSING_BINDINGS.md). Consult it before adding a new
method so you match the existing naming and don't duplicate work.

---

## 3. The data convention: dicts and lists, not structs

The bindings never expose C structs to Python. Everything crosses the
boundary as plain `dict`/`list` objects, converted by the helpers in
`pmix.pxi`. Learn these shapes — every method uses them.

**A value** is a dict with `value` and `val_type` (a `PMIX_*` type constant):

```python
{'value': 42, 'val_type': PMIX_INT32}
{'value': 'hello', 'val_type': PMIX_STRING}
{'value': {'nspace': 'myjob', 'rank': 0}, 'val_type': PMIX_PROC}
```

**An info/attribute** adds a `key` (and optional `flags`):

```python
{'key': PMIX_PROGRAMMING_MODEL, 'value': 'MPI', 'val_type': PMIX_STRING}
```

Methods that take attributes take a **list of info dicts** (`dicts:list`,
`pyinfo:list`, `pydirs:list`, …). An empty list or `None` means "no
attributes".

**A proc** is `{'nspace': str, 'rank': int}`. Lists of procs are passed as
`peers:list` / `pytargets:list`. When a proc list is empty/`None`, most
methods default to *this proc's whole job* (`self.myproc.nspace`,
`PMIX_RANK_WILDCARD`).

**Return convention:** most methods return the integer `rc` (a `PMIX_*`
status). Methods that also produce data return a **tuple**, e.g.
`get()` → `(rc, value_dict)`, `spawn()` → `(rc, nspace)`,
`init()` → `(rc, myname_dict)`. Check the first element against
`PMIX_SUCCESS` (== 0) before trusting the second.

### The conversion helpers (`pmix.pxi`)

- `pmix_load_value` / `pmix_unload_value` — one value dict ↔ `pmix_value_t`.
  A big `if/elif` over `val_type`, one arm per PMIx data type.
- `pmix_load_info` / `pmix_unload_info` / `pmix_alloc_info` / `pmix_free_info`
  — info dict lists ↔ `pmix_info_t[]`. `pmix_alloc_info` is the standard
  "malloc + load" entry point used by nearly every method.
- `pmix_load_darray` / `pmix_unload_darray` — nested `PMIX_DATA_ARRAY`.
- `pmix_load_procs` / `pmix_unload_procs`, `pmix_load_pdata`, `pmix_load_apps`,
  etc. — the remaining structured types.

**Cython auto-converts C structs to dicts.** When a helper reads a struct
value like `value[0].data.endpoint[0]`, Cython (having the struct's fields
from the `.pxd`) silently produces a **Python dict** with those fields. That
means `value[0].data.endpoint[0].size` is *attribute access on a dict*, not a
C field read — it raises `AttributeError` at runtime, not compile time. Access
converted-struct members with subscript (`['endpt']['bytes']`), and be aware
that struct-member typos in the unload paths are **runtime** failures the
compiler will not catch.

---

## 4. Threading and callbacks — the hardest part

`libpmix` is asynchronous and runs its own progress thread; the bindings must
bridge that to Python. Two distinct mechanisms are in play.

### 4a. Blocking bridge: `myLock` + `active`

Some server methods call an async C API and wait for its C callback before
returning. They use the module-global `active = myLock()` (a
`threading.Event` subclass) and the pattern:

```python
active.clear()
rc = PMIx_server_setup_application(nspace, info, sz, setupapp_cbfunc, NULL)
if PMIX_SUCCESS == rc:
    active.wait()             # blocks until the C callback fires active.set()
    active.fetch_info(dataout)
```

The C callback (`setupapp_cbfunc`, `dmodx_cbfunc`, `collectinventory_cbfunc`)
caches its results into `active` and calls `active.set(status)`. Because
`active` is a single global, these blocking calls are **not reentrant / not
thread-safe** across concurrent operations — a known limitation.

### 4b. Upcalls: C server-module and event/IOF handlers → Python

When the process acts as a server, `libpmix` calls back *into* Python for
every server operation (client connected, fence, publish, …). The chain:

1. `setmodulefn(key, fn)` validates `key` against a fixed `permitted` list and
   records the Python function in the module-global dict `pmixservermodule`.
2. `server_module_init(kvkeys)` wires a C trampoline (`clientconnected`,
   `fencenb`, `publish`, …) into the `pmix_server_module_t` struct for each
   registered key.
3. When `libpmix` invokes a trampoline, the `cdef` function runs `with gil`,
   converts the C args to dicts/lists, looks the Python handler up in
   `pmixservermodule`, and calls it.

Event and IOF handlers work similarly through the module-global list
`myhdlrs` (a list of `{'refid', 'hdlr'}` dicts). `pyeventhandler` /
`pyiofhandler` find the matching handler by `refid`. If the handler isn't
registered yet (a race with registration), they stash the event in a
`pmix_pyshift_t` caddy wrapped in a `PyCapsule` and retry via a
`threading.Timer(0.001, …)`.

**Server-module keys** accepted by `setmodulefn` are the strings in its
`permitted` list; the *wiring* names checked in `server_module_init` must
match exactly (adding a key to one list without the other silently drops the
callback — that class of mismatch bit the `notifyevent` key historically). A
Python server registers them via `PMIxServer.init(info, module_map)` where
`module_map` is `{'clientconnected': fn, 'fencenb': fn, ...}`.

### Rules when touching callback code

- Trampolines that call back into Python **must** be `cdef ... with gil`.
- Trampolines that only touch C state and release into `libpmix` use
  `with nogil` around the actual C callback invocation (see
  `pypmix_op_cbfunc`).
- Do not add work between allocating a `pmix_pyshift_t` caddy and wrapping it
  in its capsule; the capsule owns the lifetime.
- Match the existing free discipline: whoever allocs the `pmix_info_t[]` /
  procs / caddy is responsible for freeing it after the C call returns or in
  the release callback.

---

## 5. Constants: `construct.py`

`pmix_constants.pxi` and `pmix_constants.pxd` are **generated** by
`construct.py` from the *installed* C headers (`pmix_common.h`,
`pmix_deprecated.h`, `pmix.h`, `pmix_server.h`, `pmix_tool.h`, …). To add a
new constant, attribute, or C API to the bindings you generally do **not**
hand-edit the `.pxi`/`.pxd` — you add it to the C header and re-run the
generator (Automake does this during a normal build). If a constant is
missing from Python, first confirm it exists in the header and that the build
re-ran `construct.py`.

`construct.py` classifies each harvested line as a string constant, numeric
constant, error code, typedef, or API declaration, and emits the appropriate
Cython. Its parsing is textual and line-oriented, so unusual formatting in a
header (multi-line macros, unusual comments) can trip it — keep new header
entries in the conventional one-line style.

---

## 6. Building

The bindings build as part of the normal tree build when Python bindings are
enabled (`configure` reports `WANT_PYTHON_BINDINGS`; requires a Python 3 with
Cython and setuptools). From the repo root:

```sh
./autogen.pl && ./configure --enable-python-bindings ... && make -j
```

`make` in this directory runs `construct.py` then `setup.py build_ext`. The
generated `pmix.c` and the compiled `pmix*.so` land under `build/`.

**Quick standalone check that the current source cythonizes** (does not need a
full build):

```sh
cd bindings/python
PMIX_BINDINGS_TOP_BUILDDIR=<repo-root> PMIX_BINDINGS_TOP_SRCDIR=<repo-root> \
    cython -3 -I. pmix.pyx -o /tmp/pmix_check.c
```

Exit 0 means the source is syntactically valid Cython. **Caution:** Cython
type-checks C struct field access but, per §3, struct-to-dict auto-conversion
means many logic bugs in the unload paths compile cleanly and only fail at
runtime. A clean cythonize is necessary, not sufficient.

The `.so` embeds an absolute path to `libpmix` (via the linker), so once built
it imports without setting `DYLD_LIBRARY_PATH`/`LD_LIBRARY_PATH` — only
`PYTHONPATH` to the `build/lib.*` directory is needed.

---

## 7. Testing

Two flavors of test live in [`test/python/`](../../test/python/):

- **Connected/integration:** `server.py` launches `client.py` under a real
  PMIx server; `sched.py` exercises the scheduler role. Wired into
  `make check` as `run_server.sh` / `run_sched.sh`. These need a working
  server environment.
- **Standalone unit tests:** `test_bindings.py` — a `unittest` suite covering
  everything that needs **no** server: import, the class hierarchy, constant
  definitions, and the stateless `*_string` converters. It self-locates the
  built extension and exits 77 (automake SKIP) if `pmix` can't be imported, so
  it is safe to run anywhere. Also wired into `make check`.

Run the unit tests directly:

```sh
cd test/python
PYTHONPATH=<repo>/bindings/python/build/lib.<platform> ./test_bindings.py
# or just ./test_bindings.py from an in-tree build (it finds the .so)
```

**Where unit tests can and can't reach:** the `*_string` converters and
constant/version accessors are pure and testable without a server. The
conversion helpers (`pmix_load_value`, …) are `cdef` and **not importable from
Python**, so they can't be unit-tested directly — the practical way to cover
them is a round-trip through a connected client/server (put→get,
publish→lookup), which belongs in the integration scripts. When adding a new
stateless helper, prefer exposing enough of it to add a `test_bindings.py`
case.

---

## 8. Defects found and fixed, and gotchas

A deep review (2026-07) found ~22 real bugs in the conversion and method
code; all but one (see below) were fixed in the same pass. The full catalogue
— with file/line, severity, and what was wrong — is retained in
[`MISSING_BINDINGS.md`](MISSING_BINDINGS.md) (§Defects) as history and as a
map of the fragile spots. The patterns that caused them are worth
internalizing so they are not reintroduced:

- **A `cdef class` method needs an explicit `self`.** Several methods were
  declared `def foo(arg):` and raised `TypeError` when called. Cython does not
  add `self` for you — every instance method needs it.
- **Struct-to-dict auto-conversion hides field typos** (see §3). The unload
  paths had several `.size`/`.bytes`/`[index]` mistakes that compiled cleanly
  and only failed (or silently returned garbage) at runtime. When you touch an
  unload arm, exercise it on real data.
- **`memset`/`malloc` counts must include `sizeof(T)`**, and NUL-terminated
  `char**` arrays need one extra slot. Byte counts vs element counts were a
  recurring source of overflow.
- **Copy from the right key.** Loaders read from a value dict; getting the
  wrong key (`envar` vs `value`, `val_type` vs `value`) silently corrupts data.
- **`setmodulefn`'s `permitted` list and `server_module_init`'s wiring names
  must stay in lockstep** (§4b).

The one **not** fixed is a genuine unimplemented feature, not a bug:
`PMIxScheduler.assign_session` and `PMIxServer.generate_cpuset_string` /
`generate_locality_string` are honest stubs that return
`PMIX_ERR_NOT_SUPPORTED` because the underlying capability isn't wired to a C
entry point yet. They are tracked in [`MISSING_BINDINGS.md`](MISSING_BINDINGS.md).

Do **not** work around a defect by weakening a test — fix the code (see the
top-level guidance on never bending a test to a bug).

### Style / conventions specific to these bindings

- Match the top-level PMIx rules: constant-on-left comparisons
  (`NULL == ptr`, `PMIX_SUCCESS != rc`), 4-space indent, brace every block.
- Every new public method takes the attribute list parameter
  (`dicts`/`pyinfo`) even if it ignores it today — attributes are how PMIx
  APIs grow. See the top-level *Role of Attributes* section.
- Strings cross to C as ASCII (`.encode('ascii')`); guard with
  `isinstance(x, str)` since callers may pass `bytes`. Use `pmix_copy_nspace`
  / `pmix_copy_key` for the fixed-size `nspace`/`key` arrays (they guarantee
  NUL-termination and length clamping).
- Accessors that return C strings decode to `str` (e.g. `get_version()` now
  returns `str`). A few round-trip payloads (byte objects, IOF data) are
  `bytes`; decode defensively when a value could be either.
