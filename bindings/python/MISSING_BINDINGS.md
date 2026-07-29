# PMIx Python Bindings — Coverage Gaps and Defects

Tracking document for the PMIx Python bindings (`bindings/python/`). It has
two parts:

1. **Missing bindings** — real operational C APIs that have no Python method
   yet. Every operational API is now bound in both its blocking and its
   non-blocking form; the only thing deliberately left out is one
   deprecated tool API (§1.4).
2. **Defects** — bugs found in the *existing* bindings during the 2026-07 deep
   review. All are fixed.

Utility/struct-helper C APIs with no meaningful Python analogue are
intentionally **out of scope** and are *not* listed as missing:
`PMIx_Argv_*`, `PMIx_Setenv`, `PMIx_Load_*`, `PMIx_Coord_*`,
`PMIx_Topology_*`, `PMIx_Cpuset_*`, `PMIx_Device_*`, `PMIx_Geometry_*`,
`PMIx_Endpoint_*`, `PMIx_Envar_*`, `PMIx_Pdata_*`, `PMIx_Regattr_*`,
`PMIx_Node_pid_*`, the `PMIx_Info_list_*` builder, the
`PMIx_Check_*`/`PMIx_*_invalid`/`PMIx_*_valid` predicates, and all
`*_construct/_destruct/_create/_free/_release/_load/_xfer` macro
families. In Python these are handled by the dict/list conversion layer
in `pmix.pxi`, or are one line of ordinary Python.

Two families that *look* like struct helpers by prefix are **not** out of
scope and are bound: the struct pretty-printers (§1.10) and the
serialization calls (§1.11).

Headers surveyed: `include/pmix.h`, `include/pmix_server.h`,
`include/pmix_tool.h`, `include/pmix_deprecated.h`. Bindings surveyed:
`bindings/python/pmix.pyx`.

---

## Part 1 — Missing bindings

### 1.1 Non-blocking (`_nb`) variants — **CLOSED**

The five group operations were bound first and brought the client-side
async machinery with them — a keepalive caddy, a callback registry, and
two trampolines. The remaining twenty are built on exactly that
machinery. See AGENTS.md §4c and
[`docs/how-things-work/python_nonblocking.rst`](../../docs/how-things-work/python_nonblocking.rst).

Every one returns only the integer status of the *request*; the result
arrives later by executing the caller's callback on the progress thread,
and the callback runs **if and only if** the method returned
`PMIX_SUCCESS`. The final `cbdata` argument is optional and is handed
back to the callback unmodified.

| C API | Python method | Callback signature |
|-------|---------------|--------------------|
| `PMIx_Fence_nb` | `fence_nb(peers, dicts, cbfunc, cbdata=None)` | `(status, cbdata)` |
| `PMIx_Get_nb` | `get_nb(proc, ky, dicts, cbfunc, cbdata=None)` | `(status, value, cbdata)` |
| `PMIx_Publish_nb` | `publish_nb(dicts, cbfunc, cbdata=None)` | `(status, cbdata)` |
| `PMIx_Lookup_nb` | `lookup_nb(pykeys, dicts, cbfunc, cbdata=None)` | `(status, pdata, cbdata)` |
| `PMIx_Unpublish_nb` | `unpublish_nb(pykeys, dicts, cbfunc, cbdata=None)` | `(status, cbdata)` |
| `PMIx_Spawn_nb` | `spawn_nb(jobInfo, pyapps, cbfunc, cbdata=None)` | `(status, nspace, cbdata)` |
| `PMIx_Connect_nb` | `connect_nb(peers, pyinfo, cbfunc, cbdata=None)` | `(status, cbdata)` |
| `PMIx_Disconnect_nb` | `disconnect_nb(peers, pyinfo, cbfunc, cbdata=None)` | `(status, cbdata)` |
| `PMIx_Query_info_nb` | `query_nb(pyq, cbfunc, cbdata=None)` | `(status, results, cbdata)` |
| `PMIx_Log_nb` | `log_nb(pydata, pydirs, cbfunc, cbdata=None)` | `(status, cbdata)` |
| `PMIx_Allocation_request_nb` | `allocation_request_nb(directive, pyinfo, cbfunc, cbdata=None)` | `(status, results, cbdata)` |
| `PMIx_Job_control_nb` | `job_control_nb(pytargets, pydirs, cbfunc, cbdata=None)` | `(status, results, cbdata)` |
| `PMIx_Process_monitor_nb` | `monitor_nb(pymonitor_info, code, pydirs, cbfunc, cbdata=None)` | `(status, results, cbdata)` |
| `PMIx_Get_credential_nb` | `get_credential_nb(pyinfo, cbfunc, cbdata=None)` | `(status, credential, results, cbdata)` |
| `PMIx_Validate_credential_nb` | `validate_credential_nb(pycred, pyinfo, cbfunc, cbdata=None)` | `(status, results, cbdata)` |
| `PMIx_Fabric_register_nb` | `fabric_register_nb(dicts, cbfunc, cbdata=None)` | `(status, cbdata)` |
| `PMIx_Fabric_update_nb` | `fabric_update_nb(cbfunc, cbdata=None)` | `(status, cbdata)` |
| `PMIx_Fabric_deregister_nb` | `fabric_deregister_nb(cbfunc, cbdata=None)` | `(status, cbdata)` |
| `PMIx_Compute_distances_nb` | `compute_distances_nb(pycpus, dicts, cbfunc, cbdata=None)` | `(status, distances, cbdata)` |
| `PMIx_Resource_block_nb` | `resource_block_nb(directive, block, pyunits, pyinfo, cbfunc, cbdata=None)` | `(status, cbdata)` |

Four things this pass established that the group operations had not:

- **`lookup_nb` takes a list of key strings**, not the `pmix_pdata_t`
  dict list its blocking counterpart takes. That is what the C API
  accepts, and there is nothing for the caller to fill in on input.
- **Six new callback shapes needed six new trampolines** —
  `pmix_value_cbfunc_t`, `pmix_lookup_cbfunc_t`, `pmix_spawn_cbfunc_t`,
  `pmix_credential_cbfunc_t`, `pmix_validation_cbfunc_t` and
  `pmix_device_dist_cbfunc_t`. Only the last carries a release function;
  everything the other five hand back is released by the library as soon
  as the callback returns, so each converts to Python before doing
  anything else.
- **The fabric and distance calls hand the library a pointer into the
  class** (`myfabric`, `topo`), which would dangle if the Python object
  were collected while the request was in flight. The registry entry now
  carries an unread reference to the object for exactly that reason.
- **`fabric_register_nb`/`fabric_deregister_nb` wrap the caller's
  callback in a closure** so the class's "registered" flag flips when the
  library reports the outcome, not when the request is accepted.

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

`PMIx_tool_set_server_module` was listed here as bound because
`PMIxTool.set_server_module` exists — but the method never called it. It
validated the map, recorded the handlers in `pmixservermodule`, wired the
trampolines into its own `pmix_server_module_t`, and returned
`PMIX_SUCCESS` **without ever handing the module to the library**, so a
Python tool acting as a server had its handlers silently never invoked.
It now calls the API and reports what it says. Two library defects came
out with it — see §1.12.

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

- Operational client/server/tool APIs bound: **~120** — every blocking
  form, tool IOF, all 25 non-blocking variants, the five struct
  pretty-printers (§1.10) and the nine serialization calls (§1.11).
- Not bound: **1** deprecated tool API (§1.4), deliberately.

Part 1 is closed. What remains open is not a *binding* gap:
`PMIxScheduler.assign_session` is a well-formed stub because the C
library exposes no entry point for it (§1.5), and `pmix_load_value` has
no arm for `PMIX_POINTER` — a data type a Python caller cannot
meaningfully produce, which surfaces as `PMIX_ERR_NOT_SUPPORTED` rather
than a crash.

A full sweep of the public headers (307 exported `PMIx_*` functions
across `pmix.h`, `pmix_common.h.in`, `pmix_server.h`, `pmix_tool.h` and
`pmix_deprecated.h`, each checked for a call site in `pmix.pyx`/`pmix.pxi`)
found nothing else outstanding: every `PMIx_server_*` entry point is
bound, and everything else unbound belongs to the helper families listed
at the top of this file.

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

### 1.9 Conversion-layer defects the `_nb` pass surfaced (now fixed)

Binding twenty more APIs put argument shapes through the conversion
helpers that no bound call had produced before, and four latent defects
came out. None were in the new code; all are fixed:

- **`pmix_load_argv` strdup's its entries**, but `query()` and
  `unpublish()` allocated the array with `PyMem_Malloc` and
  `pmix_free_queries` released the entries with `PyMem_Free`. Handing a
  Python-arena block to `free()` aborts the process. All argv arrays now
  come from the C allocator, released through a new `pmix_free_argv`.
  `unpublish()` also leaked its key array on the success path.
- **`pmix_free_apps` released neither the argv/env arrays nor the app
  array** — an acknowledged in-code TODO.
- **An empty per-app `info` list produced a non-NULL array with
  `ninfo == 0`**, which the library reads as "terminated by an end
  marker" and walks off the allocation looking for one. A `spawn` whose
  app carried `'info': []` packed garbage and failed — this was found by
  `spawn_nb` in the connected test, not by inspection.
- **A partially-loaded array was released in full.** `pmix_load_info`,
  `pmix_load_apps` and `query()` filled arrays entry by entry and freed
  the whole thing on failure, destructing uninitialized memory. An
  attribute carrying an unconvertible value type was enough to crash the
  caller. Every such array is now zeroed before loading.

### 1.10 Struct pretty-printers — **CLOSED**

| C API | Python method |
|-------|---------------|
| `PMIx_Info_string` | `info_string(pyinfo)` |
| `PMIx_Value_string` | `value_string(pyval)` |
| `PMIx_Proc_string` | `proc_string(pyproc)` |
| `PMIx_App_string` | `app_string(pyapp)` |
| `PMIx_Resource_unit_string` | `resource_unit_string(pyunit)` |

Each returns `(rc, str)`. These were being swept out of scope by the
`PMIx_Info_*` / `PMIx_Value_*` / `PMIx_App_*` / `PMIx_Proc_*` /
`PMIx_Resource_unit_*` prefix rule, which was aimed at the
`_construct`/`_free` families — but they are converters, peers of
`data_print` (bound in §1.6) rather than struct helpers, and each has its
own man page. They render a whole struct the way the library itself
would, which is not something a Python program can reproduce by printing
a dict.

They take no library state, so they work before `init()`. The library
allocates the string it returns, so each decodes it and hands the storage
back.

### 1.11 Serialization — **CLOSED**

| C API | Python method | Returns |
|-------|---------------|---------|
| `PMIx_Data_pack` | `data_pack(pybuf, pysrc, data_type=None, target=None)` | `(rc, buffer)` |
| `PMIx_Data_unpack` | `data_unpack(pybuf, data_type=None, target=None)` | `(rc, value or info)` |
| `PMIx_Data_copy` | `data_copy(pysrc, data_type=None)` | `(rc, dict)` |
| `PMIx_Data_copy_payload` | `data_copy_payload(pydest, pysrc)` | `(rc, buffer)` |
| `PMIx_Data_unload` | `data_unload(pybuf)` | `(rc, byte object)` |
| `PMIx_Data_load` | `data_load(pybuf, payload)` | `(rc, buffer)` |
| `PMIx_Data_embed` | `data_embed(pybuf, payload)` | `(rc, buffer)` |
| `PMIx_Data_compress` | `data_compress(pybytes)` | `(rc, bytes)` |
| `PMIx_Data_decompress` | `data_decompress(pybytes)` | `(rc, bytes)` |

**The buffer.** A `pmix_data_buffer_t` holds three raw pointers into one
allocation plus two sizes. Only two of those five fields mean anything on
the Python side — the payload, and how far the unpack cursor has advanced
through it — so a Python data buffer is the dict

```python
{'bytes': b'...', 'bytes_used': n, 'bytes_unpacked': m}
```

and the pointers are rebuilt from those offsets on every crossing.
`bytes_used` is redundant with `len(bytes)` and is carried only because
the struct has it; the loader trusts the payload, not the count.

Three consequences worth knowing:

- **There is nothing to create or release.** The dict *is* the buffer, and
  the C storage lives only for the duration of a call. That is why
  `PMIx_Data_buffer_create`/`_release`/`_construct`/`_destruct` are not
  bound — there is no object for them to act on.
- **A buffer is ordinary bytes.** Because the whole state is the payload
  plus an offset, a buffer can be stored, sent somewhere, and picked up
  again by building the dict from the bytes. `test/python/server.py`
  round-trips one that way.
- **The library's own `PMIx_Data_buffer_load` cannot be used** by the
  conversion layer: it routes through `PMIx_Data_load`, which refuses to
  run before `PMIx_Init` and discards its status, so a pre-init caller
  would silently get an empty buffer. `pmix_load_dbuf` sets the fields
  directly.

As with `data_print`, the payload a Python caller can build is a value
dict or an info dict, deduced from the presence of a `'key'`; every other
data type is reachable as the value of one of those, and an explicit
`data_type` outside the pair returns `PMIX_ERR_NOT_SUPPORTED`.

`data_compress`/`data_decompress` answer a C `bool` — whether the library
compressed the block at all, which it declines when no compression
component is available or the block is below the threshold where
compression would pay. That is reported as `PMIX_SUCCESS` or
`PMIX_ERR_NOT_AVAILABLE` so the return keeps the `(rc, data)` shape every
other method uses.

### 1.12 Defects this pass surfaced (now fixed)

- **`pmix_unload_bytes` returned signed bytes.** It read the payload
  through a `char *`, which is signed on most platforms, so any byte at or
  above `0x80` arrived as a negative number and `bytearray()` rejected the
  whole list with `byte must be in range(0, 256)`. Every non-ASCII payload
  hit this — credentials, IOF data, a packed buffer — and it is why
  `data_compress` failed on its first real input. Now read through an
  `unsigned char *`.
- **`PMIx_tool_set_server_module` had no pre-init guard.** It marks the
  caller's peer as a server, and `pmix_globals.mypeer` is NULL until
  `PMIx_tool_init` — so the first Python call to the newly fixed
  `set_server_module` segfaulted. It now returns `PMIX_ERR_INIT`, and
  rejects a NULL module.
- **`PMIx_tool_finalize` tore down a framework it never opened.** Init
  opens the `pfexec` framework only for a launcher or a scheduler, but
  finalize's teardown branch fires for a launcher *or a server* — and a
  plain tool becomes a server exactly by calling
  `PMIx_tool_set_server_module`. Finalize then walked an unconstructed
  children list and crashed. `pmix_pfexec_globals` now records whether it
  was opened, `pmix_pfexec_base_close` is a no-op if it was not, and the
  finalize branch checks that rather than the peer type. This is
  reachable from C, not just from Python: any tool that inits, sets a
  server module, and finalizes hits it.

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
