# AGENTS.md: PMIx Python Binding Tests (`test/python/`)

Orientation for AI and human contributors working on the tests for the
PMIx Python bindings. This is a *map*, not the rulebook — the top-level
[`CLAUDE.md`/`AGENTS.md`](../../CLAUDE.md) at the repo root is
authoritative, and the bindings themselves are documented in
[`bindings/python/AGENTS.md`](../../bindings/python/AGENTS.md). When this
file and those disagree, they win — and please fix this file.

**This is the one place in-tree tests for the Python bindings live.**
There is deliberately nothing under `bindings/python/tests/`: it held
duplicate copies of these scripts that drifted out of sync, and only CI
ran them, so a change could pass `make check` and still break CI.

---

## 1. What is here

| File | Role | In `make check`? |
|------|------|------------------|
| `test_bindings.py` | `unittest` suite for everything that needs **no** server: import, the class hierarchy, constants, the stateless `*_string` converters, and the conversion layer via the `pmix_value_roundtrip` hook | ✅ |
| `test_construct.py` | `unittest` suite for `bindings/python/construct.py`, the constants generator. Needs neither the extension nor the library | ✅ |
| `server.py` | Stands up a PMIx server with Python handlers, forks `client.py` under it, and drives the server-role API. The only script that exercises the **upcall** path | ✅ via `run_server.sh` |
| `client.py` | The client `server.py` launches. Blocking and non-blocking client operations against a live server | (run by `server.py`) |
| `sched.py` | The scheduler role — session control, resource blocks | ✅ via `run_sched.sh` |
| `tool.py` | The tool role: attach, spawn. Needs a launcher, so it is **not** in `make check` — run it by hand against a live RM | ❌ |
| `run_server.sh.in`, `run_sched.sh.in` | Automake-substituted wrappers that put the built extension on `PYTHONPATH` (§3) | — |

Multi-node coverage lives elsewhere, in
[`contrib/dockerswarm/python/`](../../contrib/dockerswarm/python/) — see §4.

---

## 2. Running them

```sh
cd test/python
make check                 # all four, the way CI runs them
./test_bindings.py         # directly; it finds the built .so itself
./test_bindings.py -v      # per-test names
./test_construct.py        # needs only Python
```

`test_bindings.py` self-locates the extension: it tries `import pmix`,
then falls back to globbing `bindings/python/build/lib.*` relative to the
repo root. If it still cannot import, it exits **77** — Automake's SKIP —
so it is safe to run in a tree built without the bindings. Keep that
property when you edit it.

Both suites print a certain amount of expected noise on stderr
("uint8 value is out of bounds", "SERVER REQUIRES AT LEAST ONE MODULE
FUNCTION TO OPERATE"). That is the conversion layer reporting refusals the
tests deliberately provoke; `OK` on the last line is the result.

---

## 3. `make check` runs against the BUILD tree

`run_server.sh` and `run_sched.sh` put
`<builddir>/bindings/python/build/lib.*` on `PYTHONPATH` first, and fall
back to the installed egg path only if that directory does not exist.

This is load-bearing and easy to undo by accident. They originally pointed
`PYTHONPATH` at the install prefix's `site-packages`, which meant `make
check` tested whatever version happened to be installed there — so a
newly bound method made `run_server.sh` fail against a months-old egg
while the code under test was correct, and a tree that had never been
installed failed outright. The rest of the PMIx test suite runs against
the build tree by design; these must too.

If you add another wrapper script here, copy that arrangement.

---

## 4. What can be tested where

This is the part worth internalizing before adding a test, because a case
put at the wrong level either cannot run or cannot fail.

**No server needed — put it in `test_bindings.py`.** The `*_string`
converters, the constants, the class hierarchy, the struct
pretty-printers, and — through the `pmix_value_roundtrip` hook that
`pmix.pyx` exposes for exactly this — every arm of `pmix_load_value` /
`pmix_unload_value`. That hook is what makes the `PMIX_DATA_ARRAY` arms
testable at all: Cython silently converts a C struct to a dict, so a
struct-member typo in an unload arm compiles cleanly and fails only at
run time.

**But a round trip through the hook is necessary, not sufficient.** It
runs the loader against the unloader, so a mistake *both* of them make —
the element size of an array, the byte order of a coordinate vector —
cancels out and looks correct. The authority for a layout is the C side
that has to read it (`pmix_bfrops_base_pack_*`), not the other half of the
binding. Anything the library itself must interpret needs a connected
test.

**Needs a live library but not a launcher — `server.py`.** Serialization
(`data_pack`/`data_unpack`), the regex generators, `collect_job_info`, the
cpuset string generators, and the whole server-module upcall path.
`server.py` forks `client.py`, so a put on one side and a get on the other
is a real round trip through the library.

**Needs more than one node — the dockerswarm harness.** A same-host
put/get is answered out of the local datastore, so it never touches the
bindings' remote-fetch path, and a non-blocking request completes before
the method has returned, so nothing the caddy holds has to survive. Both
of those only become real when the peer is behind a *different* PMIx
server. See
[`contrib/dockerswarm/AGENTS.md`](../../contrib/dockerswarm/AGENTS.md) and
`run-python.sh`.

**Two things macOS cannot test at all:** `get_cpuset` and
`compute_distances`. Darwin has no CPU-binding API behind
`hwloc_get_cpubind`, so they answer `PMIX_ERR_NOT_FOUND` /
`PMIX_ERR_UNREACH` no matter what the bindings do. Test those on Linux.

---

## 5. Conventions for a new test

- **Say what the case is protecting.** Every non-obvious case here carries
  a comment naming the defect it would catch. A test whose reason is not
  written down gets weakened the first time it fails.
- **Never bend a test to a bug.** If a case fails, the default assumption
  is that the code is wrong. See the top-level guidance.
- **Prefer `make check`-able over manual.** A test nothing runs is not
  coverage.
- **Exercise boundaries, not typical values.** The defects found in these
  bindings clustered there: the largest value a field can hold, an empty
  list, a declared size larger than the payload, an optional string the C
  side left NULL, a payload full of NUL and high bytes. `TestConversionBoundaries`
  in `test_bindings.py` is that set.
- **Repeat allocating operations.** A leak or a double free will not show
  up in one call. Several classes carry a
  `test_repeated_calls_are_stable` for this; add to one rather than
  inventing a new pattern.
- **A new script must go in `TESTS` and `noinst_SCRIPTS`** in
  `Makefile.am`, and be executable. Adding it to only one of those is the
  usual way a test ends up never running.
- **Adding a `.py` here needs no `autogen.pl`** — editing `Makefile.am`
  alone is enough; a plain `make` regenerates the `Makefile`. Changing
  `configure.ac` or a `run_*.sh.in` substitution does need the full
  `./autogen.pl && ./configure` cycle for the substitution, though
  `./config.status <file>` is enough to re-expand an existing one.

---

## 6. The upcall protocol, as these scripts use it

`server.py` and `sched.py` are the worked examples of writing a PMIx
server in Python, so their shape is the documentation for it.

A server-module handler is registered by name in the map passed to
`PMIxServer.init(info, map)`, and is called with three arguments:

```python
def clientconnected(proc, cbfunc, cbdata):
    cbfunc(PMIX_SUCCESS, cbdata)      # complete the operation
    return PMIX_SUCCESS               # ... and say a completion is coming
```

- The **first** argument is the converted request: a proc dict for the
  simple ones, an `args` dict for the rest.
- **`cbfunc`** is a Python wrapper around the C completion callback, and
  **`cbdata`** is an opaque dict holding only integers — so it may be
  saved and used after the handler returns, from another thread.
- The **return** value tells the library what to expect:
  `PMIX_SUCCESS` means "I will invoke `cbfunc` exactly once",
  `PMIX_OPERATION_SUCCEEDED` means "done already, do not wait for me", and
  an error means the request failed. Returning `PMIX_SUCCESS` without ever
  calling `cbfunc` hangs the request.

A handler that raises is caught by the trampoline, reported, and turned
into `PMIX_ERROR` — but do not rely on that to paper over a bug.

Event handlers are different: they return a `(status, results)` tuple, not
a bare status.
