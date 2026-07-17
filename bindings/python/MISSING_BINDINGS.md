# PMIx Python Bindings — Coverage Gaps and Defects

Tracking document for the PMIx Python bindings (`bindings/python/`). It has
two parts:

1. **Missing bindings** — real operational C APIs that have no Python method
   yet, to be implemented in a later pass.
2. **Defects** — bugs found in the *existing* bindings during the 2026-07 deep
   review, to be fixed in a later pass.

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

### 1.1 Non-blocking (`_nb`) variants — none are bound

Every operational client call is bound **only** in its blocking form. None of
the callback-style non-blocking variants exist in Python. Adding these is the
single largest coverage gap and would let Python programs drive PMIx
asynchronously (they would need the callback-trampoline machinery already used
for the server module — see AGENTS.md §4).

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
- `PMIx_Group_construct_nb`
- `PMIx_Group_destruct_nb`
- `PMIx_Group_invite_nb`
- `PMIx_Group_join_nb`
- `PMIx_Group_leave_nb`
- `PMIx_Fabric_register_nb`
- `PMIx_Fabric_update_nb`
- `PMIx_Fabric_deregister_nb`
- `PMIx_Compute_distances_nb`
- `PMIx_Resource_block_nb`

### 1.2 Client role (`PMIxClient`) — unique unbound APIs

| C API | Description |
|-------|-------------|
| `PMIx_Heartbeat` | Send a heartbeat to the server to feed process health/monitoring. No args. |
| `PMIx_Progress_thread_stop` | Stop the internal progress thread (accepts directives, e.g. `PMIX_PROGRESS_THREAD_FLUSH`). |
| `PMIx_Resource_block` | Request/manipulate a block of resources per a `pmix_resource_block_directive_t`. Scheduler-facing. (`_nb` form also unbound — see §1.1.) |

### 1.3 Server role (`PMIxServer`) — unbound APIs

| C API | Description |
|-------|-------------|
| `PMIx_generate_regex2` | Current regex generator producing a `pmix_regex2_t`. Only the **deprecated** `PMIx_generate_regex` is bound (as `generate_regex`). |
| `PMIx_parse_regex2` | Parse a `pmix_regex2_t` back into its node/host list. Counterpart to `generate_regex2`. |
| `PMIx_server_generate_cpuset` | Build a `pmix_cpuset_t` from a cpuset string. Only the `_string` form is bound (and it is a stub — see Defects). |
| `PMIx_server_collect_job_info` | Collect job-level info for a set of procs, for packaging/forwarding to a remote server. |

### 1.4 Tool role (`PMIxTool`)

| C API | Description |
|-------|-------------|
| `PMIx_tool_connect_to_server` | **Deprecated**; superseded by `PMIx_tool_attach_to_server` (bound). Safe to leave unbound. |

### 1.5 Scheduler role (`PMIxScheduler`)

The scheduler's core operation is `PMIx_Resource_block` /
`PMIx_Resource_block_nb` (see §1.2) — unbound. `PMIx_Session_control` is
bound, but on `PMIxServer`, not surfaced as a distinct scheduler method.

### 1.6 Low-priority unbound string/utility converters

Non-operational pretty-printers; add only if a caller needs them:
`PMIx_Error_code`, `PMIx_Group_operation_string`,
`PMIx_Value_comparison_string`, `PMIx_Resource_block_directive_string`,
`PMIx_Alloc_inheritance_string`, `PMIx_Data_print`.

### 1.7 Coverage summary

- Operational client/server/tool APIs bound: **~75** (blocking forms + tool IOF).
- Not bound: **25** `_nb` variants, **3** unique client APIs, **4** server
  APIs, **1** deprecated tool API.

---

## Part 2 — Defects in existing bindings

Found during the 2026-07 deep review. **Status: D1–D21 and D23 were fixed in
the follow-up pass** (the source cythonizes warning-free, the standalone unit
suite passes, and the connected server↔client round-trip below succeeds).
**D22 remains** — it is an unimplemented feature (honest stub), not a bug.
Line numbers are against the originally reviewed `pmix.pyx` / `pmix.pxi` and
drift as the files are edited; they are kept for historical traceability and
as a map of the fragile spots.

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
| D22 | `pmix.pyx` `PMIxServer.generate_cpuset_string` / `generate_locality_string` (~1962) | **NOT fixed (feature, not bug).** Honest stubs returning `PMIX_ERR_NOT_SUPPORTED`; the cpuset conversion layer they need is itself unimplemented. `PMIxScheduler.assign_session` is likewise now a well-formed stub (the truncation bug D2 is fixed) pending a C entry point — see Part 1. |

---

*Generated during the 2026-07 deep review. Keep this file in sync as gaps are
closed and defects fixed.*
