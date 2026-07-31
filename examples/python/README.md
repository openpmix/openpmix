# PMIx Python examples

These are Python ports of the C examples in the parent directory. Each
`foo.py` here corresponds one-for-one to `../foo.c`: same structure, same
sequence of PMIx calls, same messages. If you know one, you know the
other — and a side-by-side `diff` of what they print is a useful way to
see how a piece of PMIx behaves.

They exist for two reasons: to show what idiomatic PMIx looks like from
Python, and to exercise the [Python bindings](../../bindings/python/)
against the same ground the C examples cover.

## Running them

The examples need the `pmix` extension module. Configure the tree with

```sh
./configure --enable-python-bindings ...
make
```

and then just run one — `examples.py` locates the freshly built extension
under `bindings/python/build/lib.*` on its own, so no `PYTHONPATH` is
needed from an in-tree build. Against an installed PMIx, set
`PYTHONPATH` to the directory holding `pmix*.so` instead.

Most of these are *clients*: they expect to be launched by something that
provides a PMIx server. Any of the following works:

```sh
# under the C example server in the parent directory
cd .. && ./server -n 2 -e ./python/client.py

# under the Python server in this directory
./server.py -n 2 -e ./client.py

# under a real launcher
prterun -n 4 ./group.py
```

A few are not clients and are run directly: `server.py` and
`simple_server.py` are servers, and `tool.py`, `toolqry.py`,
`launcher.py`, `debugger.py` and `debuggerd.py` are tools that attach to a
server that is already running.

Some examples have prerequisites the C versions also have — a minimum
process count, more than one node, or a launcher that keeps the job alive
when a process exits (`prterun --rtos recoverable ...`). Each file's
header comment says so.

## Reading them alongside the C

The ports follow the C closely, including its output text, so that
differences in behavior are easy to spot. Where Python's model differs
from C's, the port takes the Python route and says so in a comment. The
recurring cases:

- **Event-handler registration is synchronous.** Every C example carries
  an `evhandler_reg_callbk` plus a lock to wait on it.
  `register_event_handler()` already blocks and returns `(rc, refid)`, so
  the ports drop that machinery.

- **`PMIX_EVENT_RETURN_OBJECT` has no Python form.** The C examples pass
  a pointer to a stack object as `PMIX_POINTER` and fish it back out of
  the handler's info array. The ports use a module-global or a closure —
  which is what a Python programmer would reach for anyway.

- **Locks are `threading.Event`.** `examples.py` provides `MyLock`,
  `MyQuery` and `MyRel`, the counterparts of `examples.h`'s `mylock_t`,
  `myquery_data_t` and `myrel_t`.

- **Nothing is allocated or freed.** Info arrays, proc arrays, data
  arrays and query structures are all ordinary lists and dicts. A data
  array is `{'value': {'type': <type>, 'array': [...]}, 'val_type':
  PMIX_DATA_ARRAY}`.

- **The struct helpers are not bound, on purpose.** `PMIx_Argv_*`,
  `PMIx_Value_compare`, the `PMIx_Info_list_*` builder and the rest are
  one line of ordinary Python. Where a C example uses one, the port just
  does the Python thing.

- **Attribute constants are `bytes`; returned keys are `str`.** So
  `PMIX_ALLOC_STATUS == info[n]['key']` is never true. Use `key_is()` and
  `find_key()` from `examples.py`, which normalize both sides.

- **`error_string()` and friends return a plain string, but the struct
  pretty-printers return `(rc, string)`** — so `info_string`,
  `value_string`, `proc_string`, `app_string` and `data_print` are
  unpacked as a tuple.

## Two deviations worth calling out

- **`simple_server.py`** registers one trivial module function. The C
  version declares a server module whose every entry is NULL, which the C
  API accepts; the Python binding deliberately refuses an empty module
  map. What the example exercises — repeated init/finalize of a server —
  is unchanged.

- **`pmi1client.c` has no Python counterpart.** It is written against the
  legacy PMI-1 API (`pmi.h`), not against PMIx. The bindings wrap PMIx
  only, so there is nothing to port it to.
