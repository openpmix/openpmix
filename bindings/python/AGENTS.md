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

### What is bound, and what deliberately is not

**Every operational C API is bound**, in both its blocking and its
non-blocking form. Before concluding something is missing, check
`pmix.pyx` — and read the two exclusions below, because they are
choices, not gaps.

**Struct and utility helper families are out of scope.** They are *not*
missing bindings and should not be added: `PMIx_Argv_*`, `PMIx_Setenv`,
`PMIx_Load_*`, `PMIx_Xfer_*`, `PMIx_Coord_*`, `PMIx_Topology_*`,
`PMIx_Cpuset_*`, `PMIx_Device_*`, `PMIx_Geometry_*`, `PMIx_Endpoint_*`,
`PMIx_Envar_*`, `PMIx_Pdata_*`, `PMIx_Regattr_*`, `PMIx_Node_pid_*`,
`PMIx_Value_load/unload/xfer/compare`, the `PMIx_Info_list_*` builder,
the `PMIx_Check_*` / `PMIx_*_valid` / `PMIx_*_invalid` predicates, and
every `*_construct/_destruct/_create/_free/_release/_load/_xfer` macro
family. In Python these are either handled by the dict/list conversion
layer in `pmix.pxi` (§3) or are one line of ordinary Python. Two
families that *look* like helpers by prefix are **not** excluded and
are bound: the struct pretty-printers and the serialization calls.

**One deprecated API is skipped on purpose:**
`PMIx_tool_connect_to_server`, superseded by `PMIx_tool_attach_to_server`
(which is bound). Leave it unbound.

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

**A few structs have their own dict shape.** They follow the struct's own
field names, so the mapping is mechanical:

```python
{'source': 'hwloc', 'cpus': [0, 1, 2, 3, 8]}      # pmix_cpuset_t   (§8a)
{'bytes': b'...', 'size': 12}                     # pmix_byte_object_t
{'type': 'raw', 'bytes': b'n1,n2', 'len': 6}      # pmix_regex2_t
{'type': PMIX_DEVTYPE_GPU, 'count': 4}            # pmix_resource_unit_t
{'bytes': b'...', 'bytes_used': 43,               # pmix_data_buffer_t
 'bytes_unpacked': 22}                            #   (§8b)
```

The regex2 `bytes` are *not* necessarily NUL-terminated, which is why the
struct carries a length and the dict keeps it — treat the payload as
`bytes`, never as a string.

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
- `pmix_load_units` / `pmix_unload_units` / `pmix_alloc_units` /
  `pmix_free_units` — resource unit dict lists ↔ `pmix_resource_unit_t[]`.
  The struct has no allocated members and the APIs that take one do not
  retain it, so the array stays on `PyMem_*`.
- `pmix_load_regex2` / `pmix_unload_regex2` — regex dict ↔ `pmix_regex2_t`.
  The loader constructs the struct first, so the caller can (and must)
  `PMIx_Regex2_destruct` it on every path, including a partial failure.
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

### 4c. Downcalls: a client `_nb` API → a Python callback

The reverse of §4b, and the newest of the three. A non-blocking client
method hands a request to `libpmix` and returns immediately; the result
arrives later on the progress thread. Every `_nb` API is bound this way
— the group operations (`group_construct_nb` and friends) are the
worked example, and the RST linked at the end of this section tabulates
all of them with their callback signatures.

Three pieces, all in `pmix.pyx`:

- **`nbcbd`, a keepalive caddy.** PMIx does not copy its input, so the
  blocking methods' habit of freeing `procs`/`info` the moment the C call
  returns is a use-after-free here. Strings are worse:
  `group.encode('ascii')` is a Python local, so its buffer dangles at
  return. All of it goes in the caddy, which is passed as the operation's
  `cbdata` and released by the trampoline. One struct serves every
  operation — the trampolines cast any caddy to it to reach `idx` — so
  it is zeroed at allocation and each field goes back to its own
  allocator (`free` / `PyMem_Free` / `pmix_free_info` /
  `pmix_free_argv`) — see §8.
- **`pynbcbs`, a registry.** The caller's Python callback and `cbdata`
  live in a module-global dict keyed by an integer the caddy carries, so
  no `PyObject *` crosses into C. Same technique as `myhdlrs`. The entry
  can also carry an unread reference to an object that must outlive the
  operation — which is how the calls that hand `libpmix` a pointer into
  the class (`&self.myfabric`, `&self.topo`) keep it from dangling.
- **Eight trampolines**, one per C callback type
  (`pypmix_client_op_cbfunc`, `_info_`, `_value_`, `_lookup_`, `_spawn_`,
  `_credential_`, `_validation_`, `_devdist_`). All are `noexcept with
  gil`, per the rules below, with the user's callback wrapped in
  `try`/`except`. **Everything the library hands a callback belongs to
  the library**, and only the info and device-distance forms carry a
  `release_fn` — the rest reclaim their data as soon as the callback
  returns, so convert to Python before doing anything else.

Two invariants are easy to miss:

- **The registry entry is the ownership token for the caddy.** Whoever
  pops it frees the caddy; a pop that returns `None` means someone else
  already did. This is what keeps the trampoline and the method's error
  path from double-freeing.
- **A failed C call means no callback, ever.** When the `_nb` API returns
  anything but `PMIX_SUCCESS` the request was not accepted, so the method
  must reclaim the caddy itself before returning. Forgetting this leaks
  silently and only on the error path.

Validate that `cbfunc` is callable before allocating anything — a callback
that can never run strands the caddy for the life of the process.

To update class state from a completion, register a closure in place of
the caller's callback rather than teaching a trampoline about the class
— `fabric_register_nb` does this to flip `fabric_set` when the library
reports success, not when the request is accepted.

Full write-up, including how to add a further `_nb` binding:
[`docs/how-things-work/python_nonblocking.rst`](../../docs/how-things-work/python_nonblocking.rst).

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
code; all were fixed, most in one pass and the cpuset stubs in a follow-on
that had to build the conversion layer they depended on (§8a). The
per-defect catalogue lives in the git history of that work. The patterns
that caused them are worth internalizing so they are not reintroduced —
they map the fragile spots in this code:

- **A `cdef class` method needs an explicit `self`.** Several methods were
  declared `def foo(arg):` and raised `TypeError` when called. Cython does not
  add `self` for you — every instance method needs it.
- **Struct-to-dict auto-conversion hides field typos** (see §3). The unload
  paths had several `.size`/`.bytes`/`[index]` mistakes that compiled cleanly
  and only failed (or silently returned garbage) at runtime. When you touch an
  unload arm, exercise it on real data.
- **Anything `libpmix` will free must come from `malloc`, not
  `PyMem_Malloc`.** Python's allocator serves small blocks out of its own
  arenas, so passing one to `free()` is not a mismatch the system tolerates
  — it aborts the process. The load paths (`pmix_load_value`,
  `pmix_load_darray`) build values the library takes ownership of and
  therefore use `malloc`; buffers the bindings both allocate and release
  themselves (the proc array, the query array) stay on `PyMem_*`, where
  they are self-consistent. Getting this backwards is invisible until a
  Python program passes a `PMIX_DATA_ARRAY` into the library.
- **`memset`/`malloc` counts must include `sizeof(T)`**, and NUL-terminated
  `char**` arrays need one extra slot. Byte counts vs element counts were a
  recurring source of overflow.
- **Zero an array before you fill it entry by entry.** Every loader here
  can fail partway (an unconvertible value type is enough), and the
  caller then releases the *whole* array — destructing entries that were
  never written. `pmix_load_info`, `pmix_load_apps` and the query builder
  all zero first for that reason.
- **An empty list must produce a NULL array, not a zero-length one.** The
  C APIs read a non-NULL array with a count of zero as "terminated by an
  end marker" and walk off the allocation looking for one. `malloc(0)`
  returns a valid pointer, so this is easy to get wrong; `pmix_alloc_info`
  gets it right and `pmix_load_apps` had to be fixed to match.
- **Copy from the right key.** Loaders read from a value dict; getting the
  wrong key (`envar` vs `value`, `val_type` vs `value`) silently corrupts data.
- **`setmodulefn`'s `permitted` list and `server_module_init`'s wiring names
  must stay in lockstep** (§4b).
- **Binding an API exposes it to callers a C program never produced.** A
  Python script can call any method the moment it constructs the class,
  and the natural first thing a test does is call it before `init()`.
  Four C entry points had no `pmix_globals.initialized` guard and
  crashed or hung on that path the first time they were bound. When you
  bind a new API, call it
  before `init()` and with empty/None arguments *before* you call it for
  real — and fix the guard in the C library rather than papering over it
  in the binding.
- **A method that never calls the library is worse than a missing one.**
  `PMIxTool.set_server_module` built its module struct, returned
  `PMIX_SUCCESS`, and never handed it to `libpmix`, so a tool's handlers
  were silently never invoked. When adding or reviewing a method, check
  that it actually reaches a `PMIx_*` entry point; the only one that
  legitimately does not is `server_module_init` (internal wiring).
- **Bytes are unsigned.** Reading a payload through a `char *` yields
  negative values for anything at or above `0x80`, and `bytearray()`
  rejects the list. `pmix_unload_bytes` casts to `unsigned char *` for
  that reason — anything new that walks a raw buffer must too.
- **Don't invent a method for something that is an attribute.** PMIx
  extends its APIs through attributes, not new entry points, and the
  bindings must not paper over that with methods the library does not
  have. `PMIxScheduler` briefly carried an `assign_session` method; it
  was removed, because assigning a session to the RTE is a directive
  passed to the inherited `session_control` (`PMIx_Session_control`),
  not an API of its own. If a method you are about to add has no
  `PMIx_*` entry point behind it, that is the signal to look for the
  attribute that already covers it.

Do **not** work around a defect by weakening a test — fix the code (see the
top-level guidance on never bending a test to a bug).

### 8a. Cpuset conversion

A `pmix_cpuset_t` carries an opaque provider bitmap that Python cannot build,
so **every crossing goes through the library's string form**,
`"<source>:<range-list>"` — e.g. `"hwloc:0-3,8"`. The Python side is a dict:

```python
{'source': 'hwloc', 'cpus': [0, 1, 2, 3, 8]}
```

`pmix.pxi` provides the layer: `pmix_cpuset_from_string` /
`pmix_cpuset_to_string` (plain module-level `def`s, so the range expansion is
unit-testable with no server), plus `pmix_load_cpuset` /
`pmix_unload_cpuset` / `pmix_destruct_cpuset` for the C boundary. Use them
rather than hand-rolling a conversion — the methods that predated the layer
each invented a different, mutually incompatible shape.

Two things to respect when touching this code:

- **A cpuset the bindings build must be destructed.** `pmix_load_cpuset`
  produces a library-owned `source` string and provider bitmap.
  `PMIx_Parse_cpuset_string` can also allocate *before* it rejects a
  malformed range, so construct up front and destruct unconditionally on
  every path, not just the success path.
- **The library allocates the strings it hands back** (`pmix_asprintf`), so
  decode and then `free()` them. Do not `strdup` a `char *` into a dict: the
  copy leaks and Cython silently stores it as `bytes`, not `str`.

Note that `get_cpuset` and `compute_distances` cannot be verified on macOS —
it has no CPU-binding API for `hwloc_get_cpubind`, so they return
`PMIX_ERR_NOT_FOUND` / `PMIX_ERR_UNREACH` regardless of the bindings. Test
those two on Linux.

### 8b. Data-buffer conversion

A `pmix_data_buffer_t` holds three raw pointers into one allocation plus
two sizes. Only two of those five fields mean anything on the Python side —
the payload, and how far the unpack cursor has advanced through it — so a
Python data buffer is the dict

```python
{'bytes': b'...', 'bytes_used': 43, 'bytes_unpacked': 22}
```

and `pmix_load_dbuf` / `pmix_unload_dbuf` / `pmix_destruct_dbuf` in
`pmix.pxi` rebuild the pointers from those offsets on each crossing.
`bytes_used` is redundant with `len(bytes)`; the loader trusts the payload,
not the count.

Three things to respect:

- **There is no buffer object to create or release.** The dict *is* the
  buffer and the C storage lives only for the duration of a call, which is
  why `PMIx_Data_buffer_create`/`_release` are not bound. `data_pack` and
  friends take the dict, build a C buffer, call the library, write the
  state back, and destruct.
- **Do not reach for `PMIx_Data_buffer_load`.** It routes through
  `PMIx_Data_load`, which refuses to run before `PMIx_Init` *and discards
  its status*, so a pre-init caller would silently get an empty buffer.
  `pmix_load_dbuf` sets `base_ptr`/`pack_ptr`/`unpack_ptr` itself.
- **The payload must come from `malloc`** — the library's buffer destructor
  frees it with the C allocator.

`data_unload` is the one method whose semantics differ from a plain "give
me the bytes": the library hands back only the portion that has *not* been
unpacked, and consumes the buffer in the process.

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
