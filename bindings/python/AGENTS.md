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
| `pmix.pxi` | **Source.** Cython `include`d by `pmix.pyx`. All the Python↔C conversion helpers (`pmix_load_*`, `pmix_unload_*`, `pmix_free_*`) and the `myLock` class. | ✅ |
| `construct.py` | **Source.** A code generator: harvests `#define`/enum/typedef constants and API/type names from the installed C headers and emits `pmix_constants.pxi` and `pmix_constants.pxd`. | ✅ |
| `setup.py` | **Source.** setuptools/Cython build driver. Reads `PMIX_BINDINGS_TOP_{BUILDDIR,SRCDIR}` env vars to find `pmix.pyx` and the include dirs; derives the version from `include/pmix_version.h`. | ✅ |
| `Makefile.am` | **Source.** Automake wiring: runs `construct.py`, then `setup.py`, and installs the egg. | ✅ |
| `requirements.txt` | **Source.** `cython`, `setuptools`. | ✅ |
| `pmix_constants.pxi` | **Generated** by `construct.py`. Runtime constant values (`PMIX_SUCCESS = 0`, …). Its first line, `from pmix_constants cimport *`, pulls in the `.pxd`. | ❌ |
| `pmix_constants.pxd` | **Generated** by `construct.py`. `cdef extern` declarations of every C struct/type/function the bindings call. | ❌ |
| `pmix.c` | **Generated** by Cython from `pmix.pyx`. ~7 MB. Never read or edit it to understand behavior — read the `.pyx`/`.pxi`. | ❌ |
| `Makefile`, `Makefile.in` | **Generated** by Autotools. | ❌ |
| `build/`, `pypmix.egg-info/` | **Generated** build products. | ❌ |

**All of the in-tree tests live in the top-level
[`test/python/`](../../test/python/) directory** (see §7, and
[`test/python/AGENTS.md`](../../test/python/AGENTS.md)) — that is the
one place to add or fix one. There is deliberately nothing under
`bindings/python/tests/`: it held duplicate copies of the connected
scripts that drifted out of sync with the bindings (only CI ran them, so
a change could pass `make check` and still break CI), plus a Cython
round-trip smoke test that nothing compiled or ran. The conversion cases
that smoke test carried now live in `test/python/test_bindings.py`,
reachable from Python through the `pmix_value_roundtrip` hook (§7).

Multi-node coverage lives in
[`contrib/dockerswarm/python/`](../../contrib/dockerswarm/python/) (§7).

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

**Every operational C API is bound.** Before concluding something is
missing, check `pmix.pyx` — and read the two exclusions below, because
they are choices, not gaps.

**PMIx spells "non-blocking" two different ways, and both are bound —
but not identically.**

- A **separate `*_nb` function**, which is the client family: 25 of
  them, from `fence_nb` through `resource_block_nb`. Each is its own
  Python method (§4c).
- An **optional trailing `cbfunc`/`cbdata` on the same entry point**,
  which is mostly the server family. These are bound as optional
  arguments on the existing method — `deregister_nspace(ns)` blocks,
  `deregister_nspace(ns, cbfunc, cbdata)` does not — mirroring the C
  rather than inventing an `_nb` name the library does not have.

The second kind is not a nicety. Every server-module upcall and event
handler runs **on the progress thread**, and the blocking form called
from there waits on the event loop it is standing in; the library now
reports that rather than deadlocking (see §4b). So a handler that wants
to register or delete a namespace *must* pass a callback.

All fifteen of that family now take one: `register_nspace`,
`deregister_nspace`, `register_client`, `deregister_client`,
`notify_event`, `register_resources`, `deregister_resources`,
`setup_local_support`, `deliver_inventory`, `iof_deliver`,
`iof_flow_control`, `session_control`, `deregister_event_handler`,
`iof_deregister` and `iof_push`.

Fourteen take the op-style callback, `cbfunc(status, cbdata)`.
`session_control` is the exception: it reports results, so it takes the
info-style `cbfunc(status, results, cbdata)` and routes through
`pypmix_client_info_cbfunc`.

Adding a callback to a further one follows the same pattern: build the
caddy with `pypmix_nb_setup`, register the callback, pass the right
trampoline and the caddy, and reclaim on a failed return. **Anything the
library holds until completion has to live in the caddy** rather than be
freed at return — an info array (`register_nspace` and `notify_event`
both retain theirs), a proc array, or a byte object. `pypmix_nb_add_bo`
exists for the last of those: the IOF payloads cannot be the stack copy
the blocking forms use.

Which of those a given API retains is not guessable — read it. The
server calls are split roughly evenly: `register_client` and
`deregister_client` copy the proc and keep nothing, `setup_local_support`
strdup's the namespace but holds the info array, and `IOF_deliver` holds
the source, the payload *and* the directives.

Three more are blocking by construction and cannot simply grow a
callback: `dmodex_request`, `setup_application` and `collect_inventory`
drive an internal C callback and wait on the module-global `active`
lock, so making them asynchronous means first fixing the
non-reentrancy that lock imposes (§4a).

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
{'view': 1, 'coord': b'\x01\x00\x00\x00',        # pmix_coord_t
 'dims': 1}                                       #   (§8c)
```

A coordinate's `coord` is the raw `uint32` vector as **bytes**, and
`dims` counts coordinates, not bytes — so the payload is always four
times as long. A `pmix_geometry_t` carries a list of them under `coords`
plus their count under `ncoords`.

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
- `pmix_load_bo` / `pmix_unload_bo` — byte-object dict ↔
  `pmix_byte_object_t`. **This is the only correct way to move a payload
  across**: it takes its length from the bytes it was actually handed, not
  from the dict's `size`. Every arm that copies a blob goes through it.
- `pmix_load_coord` / `pmix_unload_coord` — coordinate dict ↔
  `pmix_coord_t`, likewise refusing a `dims` larger than the vector.
- `pmix_decode_str` — a `char *` the library is allowed to leave NULL,
  answered as `str` or `None`. Casting a NULL `char *` to `<bytes>` walks
  address zero; several unload arms used to.
- `pmix_darray_alloc` / `pmix_darray_nomem` — the block behind a data
  array, zeroed, with an empty array answered as NULL rather than as a
  `malloc(0)` whose return value is not portable.
- `pmix_value_alloc` — one zeroed struct for a `pmix_value_t` to point at
  (§8, "Zero before you fill").
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
registered yet (a race with registration), they retry via a
`threading.Timer(0.001, …)`.

**Only Python objects may cross that delay.** Everything the library
hands an upcall — the `pmix_info_t` array, the results array, the
payload — is released the instant the upcall returns, so the retry
carries the *converted* dicts and lists, never the pointers. This used to
stash the library's own arrays in a C caddy and free them from the timer:
a use-after-free followed by a double free, plus a leak of the caddy. And
the library's completion callback cannot be deferred with the rest — it
must fire before the upcall returns, or the event chain stalls — so
`pyeventhandler` completes the event with `PMIX_EVENT_NO_ACTION_TAKEN`
and lets the retry deliver it to the handler for its own sake.

**Server-module keys** accepted by `setmodulefn` are the strings in its
`permitted` list; the *wiring* names checked in `server_module_init` must
match exactly (adding a key to one list without the other silently drops the
callback — that class of mismatch bit the `notifyevent` key historically). A
Python server registers them via `PMIxServer.init(info, module_map)` where
`module_map` is `{'clientconnected': fn, 'fencenb': fn, ...}`.

`setmodulefn` **returns a status** — `PMIX_ERR_BAD_PARAM` for a key it
does not recognize or a handler that is not callable — and both callers
check it. They used to guard the call with `except KeyError`, which it
cannot raise, so an unrecognized key was accepted silently, never wired
in, and the caller's handler was simply never invoked.

### The upcall trampolines must not let an exception escape

Every `cdef int` trampoline is stored in `pmix_server_module_t` and called
by `libpmix` **across a C frame**, so an exception raised by the Python
handler has nowhere to propagate. Each one is therefore declared
`noexcept with gil` **and** wraps its whole body in `try`/`except`,
reporting the traceback and answering `PMIX_ERROR`.

Both halves are needed. Without `noexcept`, Cython returns −1 and leaves
the error indicator set, and the generated code then releases the GIL with
it still set — so the next Python code to run anywhere in the process
trips over an exception raised inside an unrelated upcall. With `noexcept`
alone, Cython prints the exception and returns **0**, which is
`PMIX_SUCCESS` — telling the library a completion callback is coming that
never will, and hanging the request. A new trampoline needs both.

The same reasoning is why a `cdef int` upcall must not fall off the end of
its body: that returns 0, i.e. success. `resourceblock` did.

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
- An upcall that defers work to a timer may carry only Python objects
  across the delay — see §4b.
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
header (multi-line macros) can trip it — keep new header entries in the
conventional one-line style.

Two things it now handles that it used to get silently wrong, and which
are worth knowing because the failure mode is a constant with the *wrong
value* rather than a build error:

- **Enum bodies.** Comments and blank lines inside a `typedef enum` are
  skipped rather than taken for enumerators, and an explicit `= <value>`
  is honored, with counting continuing from it. An unparsable value stops
  the build rather than emitting a wrong one.
- **A header that fails to parse stops the build.** The return of every
  `harvest_constants` call is checked; it used to be ignored for every
  file but the first, quietly producing bindings with that header's
  constants missing.

`construct.py` has its own unit suite,
[`test/python/test_construct.py`](../../test/python/test_construct.py),
which needs neither the extension nor the library. Add to it when you
touch the generator — its output is what every Python constant *is*, so a
parsing mistake is not visible until a comparison silently never matches
at run time.

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

**The module defines `__all__`, and it is computed at import time** at the
bottom of `pmix.pyx`: everything public except the modules `pmix.pyx`
imports for its own use and the few names it pulls in with `from X import
Y`. The documented usage is `from pmix import *`, so without it a caller
who wrote `import time` before that line silently got the extension's
`time` instead of their own — and likewise `os`, `sys`, `array`, `queue`,
`signal`, `threading`, `ctypes` and `traceback`. It is computed rather
than listed because the bulk of the module is the thousand-odd generated
`PMIX_*` constants, and a hand-written list would go stale on the first
new attribute. If you add a module-level import, nothing needs doing; if
you add a `from X import Y`, add `Y` to the `borrowed` tuple.

---

## 7. Testing

Full detail is in [`test/python/AGENTS.md`](../../test/python/AGENTS.md);
this is the summary.

Three flavors, at three levels of what they can reach:

- **Standalone unit tests** in [`test/python/`](../../test/python/):
  `test_bindings.py` covers everything that needs **no** server — import,
  the class hierarchy, constants, the stateless `*_string` converters, the
  struct pretty-printers, and the whole conversion layer through the
  `pmix_value_roundtrip` hook. `test_construct.py` covers the constants
  generator and needs not even the extension. Both self-locate what they
  need and exit 77 (automake SKIP) if it is missing, so they are safe to
  run anywhere.
- **Connected/integration** in the same directory: `server.py` launches
  `client.py` under a real PMIx server and is the only script that drives
  the *server* bindings and the upcall path; `sched.py` exercises the
  scheduler role. Wired into `make check` as `run_server.sh` /
  `run_sched.sh`.
- **Multi-node** in
  [`contrib/dockerswarm/python/`](../../contrib/dockerswarm/python/),
  driven by `run-python.sh` — `swarm_client.py`, `swarm_group.py`,
  `swarm_cpuset.py`, `swarm_datatypes.py`, `swarm_nonblocking.py`.

All four in-tree tests run against the **build** tree, not the installed
egg. Keep it that way if you add a wrapper script; §3 of
[`test/python/AGENTS.md`](../../test/python/AGENTS.md) explains why.

Run the unit tests directly:

```sh
cd test/python
PYTHONPATH=<repo>/bindings/python/build/lib.<platform> ./test_bindings.py
# or just ./test_bindings.py from an in-tree build (it finds the .so)
```

**Where unit tests can and can't reach:** the `*_string` converters and
constant/version accessors are pure and testable without a server. The
conversion helpers (`pmix_load_value`, …) are `cdef` and **not importable
from Python** — so `pmix.pyx` exposes one module-level hook,
**`pmix_value_roundtrip(value_dict)`**, which loads a value dict into a
`pmix_value_t`, converts it back, releases the value, and returns
`(rc, dict)`. That is what makes the load/unload arms — above all the
`PMIX_DATA_ARRAY` ones, where a struct-member typo compiles cleanly (§3) —
coverable with no server at all; see `TestValueConversion` and
`TestDataArrayConversion` in `test_bindings.py`. The hook needs no
initialized library, with one exception: a `PMIX_REGEX` value's storage
belongs to the `preg` framework, so it is refused rather than released
against an unbuilt module list.

A round trip through the hook is *necessary but not sufficient*: it
exercises the loader and the unloader against each other, so a mistake
both of them make — the element **size** of an array, say, or the byte
order of a coordinate vector — cancels out and looks correct. The
authority for a layout is the C side that will read it
(`pmix_bfrops_base_pack_*`), not the other half of the binding.
Anything the library itself must interpret still wants a connected
round trip (put→get, publish→lookup) in the integration scripts, and
`swarm_datatypes.py` sends every supported type to a peer for exactly
that reason.

**Two things only a multi-node run can reach**, which is why the swarm
tests exist rather than being duplicated in-tree:

- A same-host `PMIx_Get` is answered out of the local datastore, so it
  never travels through the bindings' remote-fetch path.
- A non-blocking request answered locally completes before the method has
  returned, so nothing the keepalive caddy holds actually has to survive.
  A lifetime bug there is invisible until the request is genuinely
  outstanding — which needs a peer behind a different PMIx server.

When adding a new stateless helper, prefer exposing enough of it to add a
`test_bindings.py` case. Prefer boundaries to typical values: the defects
this code has produced cluster at the largest representable value, the
empty list, a declared size larger than the payload, an optional string
the C side left NULL, and a payload full of NUL and high bytes.
`TestConversionBoundaries` is that set.

---

## 8. Defects found and fixed, and gotchas

Two deep reviews have been run over this code: 2026-07 (~22 defects) and
2026-08 (~40 more, across five passes). The per-defect catalogue lives in
the git history of each. What is worth keeping here is the *patterns* —
they map the fragile spots in this code, and every one of them has now
produced a defect more than once:

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

- **A bounds check written as arithmetic is a bounds check nobody read.**
  Several were `(65536*65536)` or `(2147483647*2147483647)` and simply had
  the wrong value: a uint16 rejected 65535 but accepted 65536 and let the
  C store truncate it to zero, while int64 and uint64 refused three
  quarters of their range. Write the literal — `4294967295` — and check
  both ends; the unsigned arms had no lower bound at all, so a negative
  reached the field as an enormous positive.
- **Trust the payload, not the declared size.** A byte object, an
  endpoint, a coordinate all carry both bytes and a count, and a caller
  can hand you a count larger than the bytes. Copying `size` bytes out of
  the Python object then reads off the end of it. `pmix_load_bo` and
  `pmix_load_coord` exist so no arm has to get this right on its own.
- **Zero before you fill, on the heap and on the stack.** A loader that
  fails partway leaves the caller holding a value it will destruct, and
  the destructor frees every pointer it finds. That means the struct a
  `pmix_value_t` points at (use `pmix_value_alloc`), the block behind a
  data array (`pmix_darray_alloc`), *and* the stack `pmix_value_t` a
  method loads into — four methods loaded into an unzeroed stack value
  and destructed it on failure.
- **A count the destructor walks must be set as you go.** The geometry
  loader filled in its coordinates and set `ncoords` at the end, then
  returned an error before reaching it; the destructor walked whatever the
  allocation happened to hold.
- **`malloc(0)` may portably answer NULL**, so "did the allocation fail?"
  cannot be `if not ptr`. An empty array allocates nothing and stays NULL.
- **A `cdef dict` function must return a dict.** Every arm of
  `pmix_unload_darray` answered `PMIX_ERR_NOMEM` — an int — when the block
  was NULL, which raises TypeError. Report failure as `None` and let the
  caller check.
- **Binary is not a string, in either direction.** `strdup` on a modex
  blob truncates it at the first NUL; letting Cython convert a bare
  `char *` runs `strlen` over an IOF stream; `sizeof(pyobj)` is the size
  of a pointer, not of the credential; and `<void*>pyobj` is the PyObject
  header, not its payload. All four shipped.
- **Everything the library hands an upcall dies when the upcall returns.**
  Convert it before you defer, before you release, before anything.
- **An exception has nowhere to go across a C frame** — see the
  trampoline rules in §4b.
- **Check what a conversion returned.** `pmix_alloc_info` left its caller
  holding a freed pointer and a non-zero count on failure, and a dozen
  methods passed that straight to the library. It now clears both, and
  the callers check — a sweep for an unchecked `pmix_alloc_info` is a
  cheap way to find the next one.
- **Return the same shape on every path.** A method that answers
  `(rc, results)` on success and a bare `rc` on one error path breaks the
  caller's tuple unpacking at exactly the moment things are already going
  wrong. Several did.
- **Report what the library said.** `register_attributes` called
  `PMIx_Register_attributes` and then returned `PMIX_SUCCESS` regardless.
- **A library must not touch the caller's namespace** — see the `__all__`
  note in §6.

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

### 8c. Coordinate and geometry conversion

A `pmix_coord_t` is a view, a vector of `uint32` coordinates, and the
number of them. Python sees the vector as **raw bytes** — there is no
list form — so a coord dict is

```python
{'view': PMIX_COORD_VIEW_LOGICAL, 'coord': b'\x01\x00\x00\x00', 'dims': 1}
```

and `dims` counts coordinates, so `len(coord)` is always `4 * dims`.
`pmix_load_coord` / `pmix_unload_coord` in `pmix.pxi` are the conversion;
what comes out of the unloader can be passed straight back in.

A `pmix_geometry_t` is a fabric index, a uuid, an OS name, and a list of
coords under `coords` with their count under `ncoords`. Two things to
respect:

- **`ncoords` is what the library's destructor walks.** Set it as each
  coordinate is built, not after the loop — a loader that fails partway
  and returns leaves the destructor reading past the end of the block
  otherwise. That is exactly what this arm used to do.
- **`dims` is not a byte count.** Believing a `dims` larger than the
  vector actually provided reads off the end of the Python object;
  `pmix_load_coord` refuses it.

Note that `PMIX_TOPO` and `PMIX_PROC_CPUSET` values remain unconvertible
(`PMIX_ERR_NOT_SUPPORTED` on load, `None` on unload) — both are opaque
provider objects. The cpuset has a string form the bindings use instead
(§8a); the topology does not.

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
