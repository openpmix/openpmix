# PMIx Python Bindings — Coverage Gaps and Defects

Tracking document for the PMIx Python bindings (`bindings/python/`). It has
two parts:

1. **Missing bindings** — real operational C APIs that have no Python method
   yet. The unique blocking APIs (§§1.2–1.6) have all been bound; what
   remains is the `_nb` family in §1.1 and one deprecated tool API.
2. **Defects** — bugs found in the *existing* bindings during the 2026-07 deep
   review. All are fixed.

Utility/struct-helper C APIs with no meaningful Python analogue are
intentionally **out of scope** and are *not* listed as missing:
`PMIx_Argv_*`, `PMIx_Setenv`, `PMIx_Info_*`, `PMIx_Value_*`, `PMIx_Data_*`
(struct helpers), `PMIx_Load_*`, `PMIx_Coord_*`, `PMIx_Topology_*`,
`PMIx_Cpuset_*`, `PMIx_Device_*`, `PMIx_Geometry_*`, `PMIx_Endpoint_*`,
`PMIx_Envar_*`, `PMIx_Pdata_*`, `PMIx_Proc_*`, `PMIx_App_*`,
`PMIx_Byte_object_*`, `PMIx_Regattr_*`, `PMIx_Resource_unit_*`,
`PMIx_Node_pid_*`, and all `*_construct/_destruct/_create/_free/_release/`
`_load/_xfer` macro families. In Python these are handled by the
dict/list conversion layer in `pmix.pxi`.

Headers surveyed: `include/pmix.h`, `include/pmix_server.h`,
`include/pmix_tool.h`, `include/pmix_deprecated.h`. Bindings surveyed:
`bindings/python/pmix.pyx`.

---

## Part 1 — Missing bindings

### 1.1 Non-blocking (`_nb`) variants — the group family is bound

The five group operations now have non-blocking bindings
(`group_construct_nb`, `group_invite_nb`, `group_join_nb`,
`group_leave_nb`, `group_destruct_nb`). They brought the client-side
async machinery with them — a keepalive caddy, a callback registry, and
the two trampolines — so the remaining entries below are a mechanical
follow-on rather than new infrastructure. See AGENTS.md §4c and
[`docs/how-things-work/python_nonblocking.rst`](../../docs/how-things-work/python_nonblocking.rst).

Every other operational client call is still bound **only** in its
blocking form:

- `PMIx_Fence_nb`
- `PMIx_Get_nb`
- `PMIx_Publish_nb`
- `PMIx_Lookup_nb`
- `PMIx_Unpublish_nb`
- `PMIx_Spawn_nb`
- `PMIx_Connect_nb`
- `PMIx_Disconnect_nb`
- `PMIx_Query_info_nb`
- `PMIx_Log_nb`
- `PMIx_Allocation_request_nb`
- `PMIx_Job_control_nb`
- `PMIx_Process_monitor_nb`
- `PMIx_Get_credential_nb`
- `PMIx_Validate_credential_nb`
- `PMIx_Fabric_register_nb`
- `PMIx_Fabric_update_nb`
- `PMIx_Fabric_deregister_nb`
- `PMIx_Compute_distances_nb`
- `PMIx_Resource_block_nb`

### 1.2 Client role (`PMIxClient`) — **CLOSED**

| C API | Python method | Notes |
|-------|---------------|-------|
| `PMIx_Heartbeat` | `heartbeat(dicts=None)` | Fire-and-forget; returns `PMIX_SUCCESS` once handed to the library (the C API returns `void`). Takes an ignored attribute list so it can grow one. |
| `PMIx_Progress_thread_stop` | `progress_thread_stop(dicts=None)` | Accepts `PMIX_PROGRESS_THREAD_FLUSH` / `PMIX_PROGRESS_THREAD_NAME`. Releases the GIL across the join. |
| `PMIx_Resource_block` | `resource_block(directive, block, pyunits, pyinfo)` | `pyunits` is a list of resource-unit dicts, `{'type': PMIX_DEVTYPE_x, 'count': n}` — converted by `pmix_load_units`/`pmix_alloc_units` in `pmix.pxi`. Blocking, so the GIL is released across the call. The `_nb` form remains part of §1.1. |

### 1.3 Server role (`PMIxServer`) — **CLOSED**

| C API | Python method | Notes |
|-------|---------------|-------|
| `PMIx_generate_regex2` | `generate_regex2(hosts, dicts=None)` | Returns `(rc, {'type': str, 'bytes': bytes, 'len': int})`. `hosts` may be a list or an already comma-delimited string. |
| `PMIx_parse_regex2` | `parse_regex2(pyregex, dicts=None)` | Returns `(rc, [name, ...])`, the inverse of the above and order-preserving. |
| `PMIx_server_generate_cpuset` | `generate_cpuset(csetstr)` | Returns the same cpuset dict as `PMIxClient.parse_cpuset_string`, through the server-side entry point. |
| `PMIx_server_collect_job_info` | `collect_job_info(peers)` | Returns `(rc, {'bytes': bytes, 'size': int})`. An empty proc list is rejected with `PMIX_ERR_BAD_PARAM` — there is no job to collect. |

### 1.4 Tool role (`PMIxTool`)

| C API | Description |
|-------|-------------|
| `PMIx_tool_connect_to_server` | **Deprecated**; superseded by `PMIx_tool_attach_to_server` (bound). Deliberately left unbound. |

### 1.5 Scheduler role (`PMIxScheduler`) — **CLOSED**

Both scheduler-role operations are reachable, through what the class
inherits rather than through methods of its own:

- **Resource blocks.** `resource_block` is bound on `PMIxClient` (§1.2),
  which is where it belongs: a process that *is* the scheduler gets
  `PMIX_ERR_NOT_SUPPORTED` from the library, because there is no one
  above it to ask. A scheduler **services** these requests by registering
  a `'resourceblock'` handler in its server-module map.
- **Session control.** Likewise serviced through the `'sessioncontrol'`
  module key; the scheduler's own directives to a session go out through
  the inherited `session_control` method.

The class header comment in `pmix.pyx` records this so the next reader
does not go looking for methods that should not exist.
`PMIxScheduler.assign_session` remains a well-formed stub returning
`PMIX_ERR_NOT_SUPPORTED` — the C library exposes no entry point for it.

### 1.6 String/utility converters — **CLOSED**

| C API | Python method |
|-------|---------------|
| `PMIx_Error_code` | `error_code(errname)` — the inverse of `error_string` |
| `PMIx_Group_operation_string` | `group_operation_string(op)` |
| `PMIx_Value_comparison_string` | `value_comparison_string(cmp)` |
| `PMIx_Resource_block_directive_string` | `resource_block_directive_string(directive)` |
| `PMIx_Alloc_inheritance_string` | `alloc_inheritance_string(inheritance)` |
| `PMIx_Data_print` | `data_print(prefix, pysrc, data_type=None)` — returns `(rc, str)` |

`data_print` covers the two shapes a Python caller can hand across the
boundary: a value dict (`PMIX_VALUE`) and an info dict (`PMIX_INFO`),
auto-detected from the presence of a `'key'`. The C API accepts any
registered data type, but every other type is reachable as the *value* of
one of those two; an explicit `data_type` outside the pair returns
`PMIX_ERR_NOT_SUPPORTED`.

### 1.7 Coverage summary

- Operational client/server/tool APIs bound: **~86** (blocking forms, tool
  IOF, and the five `_nb` group operations).
- Not bound: **25** `_nb` variants (§1.1), **1** deprecated tool API
  (§1.4). Every unique blocking API in §§1.2–1.6 is now bound.

### 1.8 C-side hazards this pass surfaced (now fixed)

Binding an API means calling it from a program that has not necessarily
done everything a C caller would have. Three entry points had no guard
against that, and each one crashed or hung the moment a Python script
touched it before `init()`:

- `PMIx_Heartbeat` sent through `PMIX_PTL_SEND_ONEWAY`, which
  dereferences the peer, without checking that the library was
  initialized — so `pmix_client_globals.myserver` was still NULL.
- `PMIx_Progress_thread_stop` walked its static tracking list before that
  list had been constructed.
- `PMIx_server_collect_job_info` had no `initialized` check at all, so it
  thread-shifted a request and then blocked forever waiting for a
  progress thread that did not exist; separately, a request naming no
  procs produced a NULL namespace list that the collection loop then
  walked.

All three now return (or no-op) cleanly; the collect-job-info parameter
checks are covered by `test/unit/collect_job_info.c`, and the pre-init
behavior of all of them by `TestNewlyBoundAPIs` in
`test/python/test_bindings.py`.

---

## Part 2 — Defects in existing bindings

Found during the 2026-07 deep review. **Status: all of D1–D23 are now
fixed.** D1–D21 and D23 were fixed in the first follow-up pass; **D22 — the
cpuset stubs — was closed in a second pass** that implemented the missing
conversion layer (see below). The source cythonizes warning-free, the
standalone unit suite passes, and the connected server↔client round-trip
below succeeds. Line numbers are against the originally reviewed `pmix.pyx` /
`pmix.pxi` and drift as the files are edited; they are kept for historical
traceability and as a map of the fragile spots.

The one item still outstanding is **not** a defect: `PMIxScheduler.assign_session`
remains a well-formed stub returning `PMIX_ERR_NOT_SUPPORTED` because the C
library exposes no entry point for it (tracked in Part 1, not here).

**End-to-end verification.** After clearing a pre-existing environmental
blocker (a stale `pmix.sched.<host>` rendezvous file that made the PMIx
server's listener setup abort — unrelated to the bindings), the connected
round-trip in `test/python/` (`server.py` launching `client.py`) runs clean:
server `init`/`register_nspace`/`register_client`/`setup_fork` succeed, the
client connects, and a `put`(INT32) → `commit` → `fence` → `get` returns the
correct value (`{'value': 1, 'val_type': 9}`), with the event handler and the
`clientconnected`/`clientfinalized`/`fencenb` server-module upcalls all firing.
This exercises `pmix_load_value`/`pmix_unload_value` and the D18 `PMIx_Value_free`
fix live. (Fixing the round-trip also required updating the **stale
`test/python/server.py` harness**, whose server-module handlers used the old
1-argument signature instead of the current `(request, cbfunc, cbdata)`
convention — see `bindings/python/tests/python/server_upcalls.py` for the
reference pattern.)

### 2.1 High severity — method is unusable / crashes

| # | Location | Defect |
|---|----------|--------|
| D1 | `pmix.pyx` — `register_resources`, `deregister_resources`, `register_attributes`, `collect_inventory`, `deliver_inventory`, `iof_deliver`, `define_process_set`, `delete_process_set`, `session_control` (PMIxServer); `disconnect` (PMIxTool); `assign_session` (PMIxScheduler) | **Missing `self` parameter.** Declared `def foo(arg):` inside a `cdef class`. Called as bound methods they shift arguments and raise `TypeError`. Confirmed at runtime. |
| D2 | `pmix.pyx` `assign_session` (~3359) | **Truncated/incomplete.** The source file ends mid-method after `# convert the app list`; it never calls any C API and implicitly returns `None`. |
| D3 | `pmix.pyx` `PMIxScheduler.init` (~3334) | Calls `self.server_module_init()` with **no argument**, but the method requires `kvkeys:list`. Scheduler init raises `TypeError`. |
| D4 | `pmix.pyx` `PMIxServer.iof_deliver` (~2245-2251) | `source = NULL` then immediately `pmix_copy_nspace(source[0].nspace, …)` **dereferences NULL** → crash. Also `bo.size = sizeof(data)` uses `sizeof` (pointer size) instead of `len(data)`. |
| D5 | `pmix.pyx` `PMIxTool.iof_deregister` (~3247) | `for h in myhdlrs and not found:` iterates over the **boolean** `not found`, not the list → `TypeError`; the local handler is never removed. |

### 2.2 Medium severity — data corruption / wrong results

| # | Location | Defect |
|---|----------|--------|
| D6 | `pmix.pxi` `pmix_convert_locality` (~139) | `pyloc.len()` — lists have no `.len()` method → `AttributeError`. Should be `len(pyloc)`. |
| D7 | `pmix.pxi` `pmix_load_darray`, `PMIX_DOUBLE` arm (~322-324) | `n += 1` executed **twice** per element: writes every other slot and overruns the allocation. |
| D8 | `pmix.pxi` `pmix_load_darray`, `PMIX_ENVAR` arm (~485-497) | The `value` field is loaded from `item['envar']` (never reads `item['value']`); the envar name is duplicated as its value. |
| D9 | `pmix.pxi` `pmix_load_darray`, `PMIX_DATA_ARRAY` arm (~452-468) | Allocates only **one** element (`sizeof(pmix_data_array_t)`, not `mysize * …`) and `return`s inside the loop on the first item — only the first nested array is ever processed. |
| D10 | `pmix.pxi` `pmix_load_value`, `PMIX_TIME` arm (~1086) | Assigns `val['val_type']` instead of `val['value']` to `data.time`. |
| D11 | `pmix.pxi` `pmix_unload_value`, byte-object/coord/geometry/endpoint arms (~1588, 1643, 1670, 1694) | Reads a **single element at index `[size]`/`[dims]`** (out-of-bounds by one) instead of slicing `[:size]`. Returns one int, not the buffer. |
| D12 | `pmix.pxi` `pmix_unload_value`, `PMIX_ENDPOINT` arm (~1691-1695) | Accesses `.size`/`.bytes` on the Cython-converted endpoint dict; the struct field is `endpt` (a byte object) → `AttributeError` at runtime. Correct: `['endpt']['bytes']`. |
| D13 | `pmix.pxi` `pmix_load_value`, devdist osname (~1408-1411) and endpoint osname (~1440-1443) | Missing `else:` — the encoded ASCII value is unconditionally overwritten with the raw `str`, so a non-`bytes` object reaches `strdup`. (geometry/device arms do it correctly.) |
| D14 | `pmix.pxi` `pmix_unload_queries` (~1897-1910) | `query = {}`, `keylist`, `qualist` are created **once outside** the loop and reused/accumulated; every appended entry aliases the same dict. Broken for more than one query. |
| D15 | `pmix.pxi` `pmix_load_apps` (~2031, 2038, 2051) | `memset(argv, 0, m)` zeroes `m` **bytes**, but the array is `m * sizeof(char*)` bytes → tail pointers left uninitialized. |
| D16 | `pmix.pxi` `pmix_unload_darray`, `PMIX_DATA_ARRAY` arm (~786-801) | Never builds/returns the result dict (falls through returning the `PMIX_SUCCESS` int from a `dict`-typed function). Nested-array unload is effectively broken. |
| D17 | `pmix.pyx` server-module key mismatch | `setmodulefn`'s `permitted` list contains `'notify_event'` and `'listener'`, but `server_module_init` wires the key `'notifyevent'`. Registering `'notify_event'` passes validation but the callback is never installed. |
| D23 | `pmix.pyx` `register_event_handler` | The `pycodes:list` / `pyinfo:list` argument annotations make Cython reject `None` at the call boundary, even though the body explicitly handles `None` (default handler / no directives). The natural, documented usage `register_event_handler(None, None, hdlr)` raised `TypeError`. Fixed by dropping the `:list` annotations. Found while validating the connected round-trip. |

### 2.3 Low severity — leaks / API warts

| # | Location | Defect |
|---|----------|--------|
| D18 | `pmix.pyx` `PMIxClient.get` (~726) | The value from `PMIx_Get` is allocated by `libpmix` (libc `malloc`) but freed via `pmix_free_value` → `PyMem_Free` — allocator mismatch (UB on some builds). |
| D19 | `pmix.pyx` `pypmix_credential_cbfunc` (~205, 226) | `size` is initialized to 0 and never updated, so `if 0 < size: free(c_byteobject.bytes)` never runs → credential bytes leak. |
| D20 | `pmix.pyx` `PMIxClient.unpublish` (~777) | On alloc failure `PMIX_ERR_NOMEM` is evaluated but **not returned** (missing `return`); the code proceeds to use a possibly-NULL `keys`. |
| D21 | `pmix.pyx` `get_version` | Returns `bytes`, not `str`, unlike the other accessors — callers must decode. Minor API inconsistency. |
| D22 | `pmix.pyx` `PMIxServer.generate_cpuset_string` / `generate_locality_string` (~1962), `PMIxClient.parse_cpuset_string` | **FIXED.** Were stubs returning `PMIX_ERR_NOT_SUPPORTED` unconditionally, because the cpuset conversion layer they need was unimplemented. That layer now exists in `pmix.pxi` (`pmix_cpuset_from_string` / `pmix_cpuset_to_string` / `pmix_load_cpuset` / `pmix_unload_cpuset` / `pmix_destruct_cpuset`) and all three methods do real work. See §2.4. `PMIxScheduler.assign_session` is still a well-formed stub (the truncation bug D2 is fixed) pending a C entry point — see Part 1. |

### 2.4 The cpuset layer (closing D22)

D22 was the last open defect. Closing it required building the conversion
layer the three stubs depended on, which in turn exposed several adjacent
defects in the methods that were *already* supposed to consume that layer.

**The representation.** A `pmix_cpuset_t` carries an opaque provider bitmap
that Python cannot construct, so every crossing goes through the library's
string form, `"<source>:<range-list>"` (e.g. `"hwloc:0-3,8"`). The Python
side is a dict:

```python
{'source': 'hwloc', 'cpus': [0, 1, 2, 3, 8]}
```

`pmix_cpuset_from_string` / `pmix_cpuset_to_string` are deliberately plain
module-level `def`s, not `cdef` helpers, so the range expansion — the only
part with real logic — is unit-testable without a server
(`TestCpusetConverters` in `test/python/test_bindings.py`). `to_string`
accepts a list of indices, a list of range tokens (`'0-3'`), or an
already-formatted range list, since callers had used all three.

| # | Location | Defect |
|---|----------|--------|
| D24 | `pmix.pyx` `PMIxClient.get_cpuset` | Returned `strdup(cpuset.source)` — a `char*` that Cython converts to **bytes** while leaking the `strdup` copy — and set `cpus` to `txt.split(",")` on the *whole* string, leaving the `hwloc:` prefix glued to the first entry (`['hwloc:0-3', '8']`). The `pmix_cpuset_t` was never destructed, leaking the provider bitmap on every call. |
| D25 | `pmix.pyx` `PMIxClient.compute_distances` | Read `pycpus['cpus'].encode('ascii')`, i.e. expected `cpus` to be a **string** — a shape no other method produced, so a dict from `get_cpuset` raised `AttributeError`. It also returned early on failure without freeing the info array, and never destructed the cpuset. |
| D26 | `pmix.pyx` `PMIxClient.compute_distances` | Released the device-distance array with `PyMem_Free`, but the library allocates it with the C allocator — the same allocator mismatch as D18. Now released with `PMIx_Device_distance_free`. The per-result strings were stored as leaked `strdup` copies (which Cython stores as `bytes`, not `str`), and the `type` field was dropped entirely. |
| D27 | `pmix.pyx` `parse_cpuset_string` | The `csetstr:str` annotation made the body's own `None` check unreachable (Cython rejects `None` at the call boundary first) — the same defect class as D23. The annotations were dropped from the cpuset methods so their validation can report `PMIX_ERR_BAD_PARAM`. |

**A C-side hazard this surfaced (now fixed).**
`pmix_hwloc_generate_cpuset_string` rejected a NULL `bitmap` but then read
`cpuset->source` with `strncasecmp` without a NULL check, so a cpuset with a
bitmap but no source segfaulted; `pmix_hwloc_generate_locality_string` had
the same flaw and did not check the `cpuset` pointer at all. Both now return
`PMIX_ERR_BAD_PARAM` and NULL the output string on every failure path — see
`src/hwloc/AGENTS.md` item 7 and `test_cpuset_string_bad_source` in
[`test/unit/hwloc_datatype.c`](../../test/unit/hwloc_datatype.c). No binding
path produced that combination (both `PMIx_Parse_cpuset_string` and
`PMIx_Get_cpuset` set `source`), but `pmix_unload_cpuset` still guards
against it rather than relying on the layer below.

**What is verifiable on macOS.** `parse_cpuset_string`,
`generate_cpuset_string` and `generate_locality_string` all return correct
results live. `get_cpuset` returns `PMIX_ERR_NOT_FOUND` and
`compute_distances` returns `PMIX_ERR_UNREACH` — both **environmental**:
macOS exposes no CPU-binding API for `hwloc_get_cpubind`, and a standalone
server has no upstream to forward the distance request to. Neither reflects
the bindings; exercising those two paths needs Linux.

---

*Generated during the 2026-07 deep review. Keep this file in sync as gaps are
closed and defects fixed.*
