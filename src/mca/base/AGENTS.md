<!--
  Copyright (c) 2026      Nanook Consulting  All rights reserved.

  $COPYRIGHT$

  Additional copyrights may follow

  $HEADER$
-->

# AGENTS.md: The MCA base

This document orients AI agents and human contributors working in
`src/mca/base` — the machinery that *is* the Modular Component
Architecture. Read the top-level [`AGENTS.md`](../../../AGENTS.md) first;
the golden rules, prefix conventions, and MCA vocabulary described there
apply here and are not repeated. [`../AGENTS.md`](../AGENTS.md) covers
the `src/mca` tree as a whole.

This directory is **not a framework**. There is no `src/mca/base/base/`,
no components, no `mca_base` module interface. It is the library that
every *other* framework's `base/` is written against: it registers MCA
variables, finds and opens components (statically linked or `dlopen`ed),
drives the register/open/select/close lifecycle, and hands frameworks
their verbosity streams. Nothing in `src/mca/base` knows what any
framework does.

## The two halves

Almost everything here belongs to one of two subsystems that only touch
at the edges:

| Subsystem | Files | What it owns |
|-----------|-------|--------------|
| **MCA variables** | `pmix_mca_base_var*.c/h`, `pmix_mca_base_parse_paramfile.c` | the `name -> value` registry behind `PMIX_MCA_*` env vars, `mca-params.conf`, and `pmix_info` |
| **Components** | `pmix_mca_base_component_*.c`, `pmix_mca_base_components_*.c`, `pmix_mca_base_framework.c`, `pmix_mca_base_alias.c` | finding, loading, registering, opening, selecting and closing plugins |

`pmix_mca_base_open.c` / `_close.c` bring both up and down and own the
handful of `mca_base_*` parameters the base registers for itself.

## Part 1: the MCA variable system

### Registration is the API

A variable is a **name plus a pointer to the caller's storage**. The
caller declares the storage, seeds it with the default, and registers it:

```c
static int my_param = 5;
(void) pmix_mca_base_var_register("pmix", "ptl", "tcp", "my_param",
                                  "what this does",
                                  PMIX_MCA_BASE_VAR_TYPE_INT, &my_param);
```

Registration **immediately resolves the value** and writes it through
that pointer, so `my_param` is correct the moment the call returns —
there is no separate "look it up" step. The storage must outlive the
registration; every caller in the tree uses a file-scope static.

`pmix_mca_base_var_get_value()` exists for the case where you have an
index but not the storage, and it does **not** copy: it hands back a
*pointer to* the registered storage, so the argument is the address of
a pointer and the value is read by dereferencing what comes back.

```c
int *val;
pmix_mca_base_var_get_value(idx, &val, NULL, NULL);   /* &val, not &myint */
printf("%d\n", *val);
```

Passing the address of an `int` writes a pointer through it. The header
described a copy-out interface with a `value_size` parameter for years;
no such parameter has ever existed.

Three conveniences wrap the same call:
`pmix_mca_base_component_var_register()` (takes the names out of a
component struct, so the variable is dropped when the component closes)
and `pmix_mca_base_framework_var_register()` (component name `"base"`).

**`pmix_mca_base_var_register()` returns the variable's index, not a
status.** Index 0 is a perfectly good index and happens to equal
`PMIX_SUCCESS`, which makes `if (PMIX_SUCCESS != ret)` look like it
works. Test the sign — `if (0 > ret)` — as the rest of this directory
does.

### Where a value comes from, and in what order

`var_set_initial()` resolves each variable through, in increasing
precedence:

1. the default already in the caller's storage,
2. the **override file** (`$sysconfdir/pmix-mca-params-override.conf`),
3. a **`PMIX_MCA_<name>`** environment variable,
4. an ordinary **parameter file** (`~/.pmix/mca-params.conf`,
   `$sysconfdir/pmix-mca-params.conf`, or whatever
   `mca_base_param_files` names).

That ordering is enforced by the `mbv_source` each stage records, and
`var_set_from_file()` cannot tell which of the two lists it was handed —
it always records `SOURCE_FILE`. `var_set_initial()` is what promotes an
override-file hit to `SOURCE_OVERRIDE`, and it has to do so on **both**
the name that matched and the variable that name stands for. Marking
only the former is invisible for an ordinary variable (they are the same
object) and wrong for a synonym: the environment check that runs next
tests the *original's* source, so an override file that used a
deprecated spelling was silently beaten by a user's environment.

Two names are accepted for every variable: the **full name**
(`framework_component_variable`) and the **long name**
(`project_framework_component_variable`), each with the project's
upper-cased prefix — `PMIX_MCA_ptl_tcp_if_include` and
`PMIX_MCA_pmix_ptl_tcp_if_include` both work. A companion
`PMIX_MCA_SOURCE_<name>` variable records where a value came from when
it is forwarded between processes.

**Parameter files are read once, at `pmix_mca_base_var_init()`**, into
`pmix_mca_base_var_file_values` / `..._override_values`; registration
then searches those lists. So a variable registered before the files are
read would silently miss a user's setting — which is why
`pmix_mca_base_var_cache_files()` registers the two
`suppress_*_warning` parameters *after* the read, with a comment saying
so. Keep that ordering.

**`PMIX_PARAM_FILE_PASSED` short-circuits the whole file stage.** The
`pmdl` framework sets it in every child's environment (see
`src/mca/pmdl/base/pmdl_base_stubs.c`) to say "your parent already
resolved the files and put the results in your environment." In such a
process `pmix_mca_base_var_cache_files()` returns early, so
`pmix_mca_base_var_override_file` and friends stay `NULL` while
`PMIX_MCA_SOURCE_*` variables are still present in the environment.
Anything comparing against those file names has to tolerate `NULL`.

### Synonyms

`pmix_mca_base_var_register_synonym()` creates a second name for an
existing variable, normally flagged `..._SYN_FLAG_DEPRECATED` so setting
it prints a warning. **A synonym has no storage of its own** — reads and
writes resolve to the original through `var_get(..., original=true)`.
That is the single most common source of NULL dereferences in this file:
code that walks `pmix_mca_base_vars` directly and touches
`var->mbv_storage` must skip synonyms (`PMIX_VAR_IS_SYNONYM`) and
deregistered variables (`!PMIX_VAR_IS_VALID`, storage dropped), because
neither has any. `pmix_mca_base_var_build_env()` carries exactly that
guard, and a comment saying why.

A synonym's name must not collide with its original's full name.
`register_variable()` catches that and raises the `var-name-conflict`
`show_help` topic; under `--enable-debug` it also `assert(0)`s.

### Deregistration is not deletion

`pmix_mca_base_var_deregister()` clears `PMIX_MCA_BASE_VAR_FLAG_VALID`,
frees a string value, drops the enumerator, and sets `mbv_storage` to
`NULL` — but keeps the entry and its index. Re-registering the same name
**revives the same index**. Nothing is actually freed until
`pmix_mca_base_var_finalize()`. Indices are therefore stable for the life
of the process, which is what lets `pmix_mca_base_var_group_t` hold
plain `int` lists of its members.

### Groups

[`pmix_mca_base_var_group.c`](pmix_mca_base_var_group.c) is a parallel
registry of `(project, framework, component)` tuples, each holding the
indices of its variables and its subgroups. It exists so that closing a
component can deregister everything that component registered in one
call (`pmix_mca_base_var_group_deregister()`), and so `pmix_info` can
present variables grouped by owner. Lookups go through a hash of the
generated full name, except when any name component is `"*"`, which
forces the linear scan in `group_find_linear()`.

**`pmix_mca_base_var_group_find()` returns the group's index, not a
status** — same trap as `pmix_mca_base_var_register()`, and the header
said `PMIX_SUCCESS` for years while every caller in the tree correctly
treated the result as an index. Test the sign.

Note that `group_register()` builds a group's name by calling
`pmix_mca_base_var_generate_full_name4(NULL, project, framework,
component, ...)` — the arguments are shifted one slot to the left of
what the parameter names suggest. It works because that function only
joins its non-`NULL` arguments with `_` and attaches no meaning to the
slots, and it produces the same string `group_find()` looks up. Don't
"fix" one side without the other.

### Enumerators

An enumerator maps strings to integers, and is what `pmix_info` prints
as "Valid values".

**No variable in this tree actually has one.** `pmix_mca_base_var_t`
carries an `mbv_enumerator` field, `pmix_mca_base_var.c` has five
branches that use it, and the header used to document an `enumerator`
parameter — but `pmix_mca_base_var_register()` takes no such parameter
and nothing ever assigns that field. Those branches are dead code, and
the enumerators are reachable only by calling their vtable directly.
Know that before "fixing" the variable-side enumerator paths against
behavior you cannot reproduce, and before assuming a variable you
register can validate its own values.

Two enumerators are built in and statically
allocated — `pmix_mca_base_var_enum_bool` and
`pmix_mca_base_var_enum_verbose` — and **must never be `PMIX_RELEASE`d**;
`enum_is_static` guards that, and `PMIX_MCA_VAR_MBV_ENUMERATOR_FREE()`
in `pmix_mca_base_var.c` honours it. Dynamic ones come from
`pmix_mca_base_var_enum_create()` (value list) or
`pmix_mca_base_var_enum_create_flag()` (bitmask list, with conflict
declarations).

Two ownership rules in that vtable pull in opposite directions, and
getting them backwards leaks or double-frees:

- **`string_from_value()` returns a string the caller owns** and must
  `free()`.
- **`get_value()` returns a string the caller *borrows*** from the
  enumerator. It cannot be otherwise: the boolean enumerator's
  `get_value()` returns a string literal, so no caller could safely free
  what this slot hands back.

The flag enumerator's `value_from_string()` parses a **comma-delimited
list**, which means two unrelated index spaces in one nest of loops: the
outer index walks the caller's tokens, the inner walks the enumerator's
flag table. Confusing them matches the wrong flag *and* reads past the
end of the table as soon as the caller passes more tokens than the
enumerator has flags.

Whatever an enumerator's `dump()` advertises has to be accepted by its
`value_from_string()`. That text is what a user reads out of
`pmix_info`, so a value listed there and then rejected is a bug, not a
cosmetic mismatch. `test/unit/mca/mca_base_var_enum.c` feeds the bool
enumerator's own dump back into it for exactly this reason.

## Part 2: components and frameworks

### The framework lifecycle

A framework is a `pmix_mca_base_framework_t` declared by
`PMIX_MCA_BASE_FRAMEWORK_DECLARE()` and driven through three entry
points in [`pmix_mca_base_framework.c`](pmix_mca_base_framework.c):

```
pmix_mca_base_framework_register()   refcnt++, register MCA vars,
                                     find components, call each
                                     component's register function
pmix_mca_base_framework_open()       register() first, then call the
                                     framework's open (default:
                                     open every component)
pmix_mca_base_framework_close()      refcnt--, at zero deregister the
                                     variable group and close/unload
                                     every component
```

**`framework_refcnt` is the whole contract.** Register and open bump it;
close decrements and only does real work at zero. Every failure path in
`register()` and `open()` must **undo the registration it performed** —
not merely decrement:

- `register()` funnels its errors through a single `error:` label that
  puts the count back before the `REGISTERED` bit is ever set.
- `open()` registers on the way in, so its failure path calls
  `pmix_mca_base_framework_close()`. That is the only thing that gets
  both cases right: if someone else already held a reference it just
  gives ours back, and if this call was the first one in it deregisters
  the variable group and tears the component lists down.

Getting this wrong does not merely leak. A framework left `REGISTERED`
with a count of zero **wedges permanently**: the next `close()`
decrements 0 to −1, finds that non-zero, and returns success without
tearing anything down, so no later close can ever reach zero. The
`assert(framework->framework_refcnt)` at the top of `close()` catches
it — but only in a build with asserts enabled, which is not the one
users get. `test/unit/mca/mca_base_framework.c` covers both failure
paths and both holder cases.

`framework_flags` carries `NOREGISTER` (skip the MCA variable stage
entirely — used by frameworks that run before the variable system is
ready) and `NO_DSO` (this framework has no loadable components, so don't
scan). The `REGISTERED` and `OPEN` bits are internal state; do not set
them from outside this file.

### Finding components

[`pmix_mca_base_component_find.c`](pmix_mca_base_component_find.c) builds
`framework->framework_components` from two sources:

- **Static components** — the `framework_static_components` array
  generated into each framework's `base/static-components.h` at build
  time.
- **Dynamic components** — entries in the repository (below), opened
  with `dlopen` unless `mca_base_component_disable_dlopen` is set or the
  framework declared `NO_DSO`.

Both are filtered by `use_component()` against the framework's selection
parameter (`--mca <framework> a,b` or `--mca <framework> ^c`), parsed by
`pmix_mca_base_component_parse_requested()`. The negate character is only
legal at the very front of the value; anywhere else raises
`framework-param:too-many-negates`. In include mode,
`component_find_check()` then warns about every requested name that never
turned up, and aborts if `mca_base_abort_on_load_error` is set.

**Those last two are independent parameters, and the abort must not be
nested inside the report.** `mca_base_component_show_load_errors`
defaults to `"none"`, so gating the abort on "are we printing this?"
makes `mca_base_abort_on_load_error` a silent no-op in the default
configuration. The two used to be nested and got away with it only
because the old test was on the raw parameter *string*, which is
non-NULL even for `"none"` — fixing the spurious message without
splitting the abort out reintroduces the no-op.

### The component repository

[`pmix_mca_base_component_repository.c`](pmix_mca_base_component_repository.c)
is the catalogue of loadable plugins, built once at
`pmix_mca_base_open()` and keyed by framework name. It only exists when
`PMIX_HAVE_PDL_SUPPORT` is set — the entire file is `#if`'d, and the
`#else` arms return `PMIX_ERR_NOT_SUPPORTED` or do nothing.

Two things about it repay attention:

- **`pmix_mca_base_component_path` is a `;`-delimited list of
  `project@path` entries**, not a plain path. It is assembled in
  `pmix_mca_base_open()` from `$pmixlibdir`, `~/.pmix/components`, the
  `mca_base_component_path` MCA parameter, and whatever the caller passed
  as `add_path` — so **its content is partly user-supplied** and the
  parser may not assume the `@` is present or that the project name fits
  any particular buffer.
- **A repository item's `project` string belongs to the caller.**
  `process_repository_item()` receives it as the `void *data` cookie of
  `pmix_pdl_foreachfile()`, and `pmix_mca_base_component_repository_init()`
  passes a **stack buffer**. It must be copied, never freed.

`ri_constructor()` has to initialize every field of the item:
`PMIX_NEW` malloc's without zeroing, and the destructor `free()`s
`ri_project` and `ri_base`.

**Frameworks can be opened before the repository exists.**
`pmix_init_util()` opens `pinstalldirs` *before* it calls
`pmix_mca_base_open()`, and the repository hash table is constructed by
the latter. The only thing that keeps that first framework from reaching
`pmix_mca_base_component_repository_get_components()` with an
unconstructed table is the `NO_DSO` flag on its declaration — remove
that flag, or add another framework opened that early without it, and
the ordering matters again. `get_components()` therefore checks its own
`initialized` flag rather than relying on how `pmix_hash_table_t`
happens to treat a zero capacity.

`pmix_mca_base_component_repository_retain_component()` is exported and
has no callers anywhere in the tree.

Reference counting here is per-*file*: `ri_refcnt` goes to 1 when the
component is `dlopen`ed and back to 0 through
`pmix_mca_base_component_repository_release()`, at which point the
variable group is deregistered and the handle closed. Note the warning in
`ri_destructor()` — after `dlclose`, the `pmix_mca_base_component_t`
pointer is dangling.

### Opening, selecting, closing

- [`pmix_mca_base_components_register.c`](pmix_mca_base_components_register.c)
  calls each component's `pmix_mca_register_component_params`, dropping
  any that fail. `PMIX_ERR_NOT_AVAILABLE` means "silently ignore me" and
  is not reported.
- [`pmix_mca_base_components_open.c`](pmix_mca_base_components_open.c)
  does the same for `pmix_mca_open_component`, with the same
  `NOT_AVAILABLE` convention, and also owns the
  `mca_base_component_show_load_errors` parser (below).
- [`pmix_mca_base_components_select.c`](pmix_mca_base_components_select.c)
  is the generic single-select helper: query every component, keep the
  highest priority, close the rest. A component returning
  `PMIX_ERR_FATAL` stops selection dead — that is how a component says
  "the user asked for something I cannot provide, do not paper over it
  by picking someone else."

  **It consumes the list.** On success it closes, unloads and releases
  every component except the winner; on `PMIX_ERR_NOT_FOUND` it empties
  the list entirely; on `PMIX_ERR_FATAL` it closes nothing at all. That
  last asymmetry is safe only because every caller passes its own
  `framework_components`, so the framework's close mops up. A caller
  that passed a list it owned separately would be left holding it.
  `pmix_base.h` now spells this out.
- [`pmix_mca_base_components_close.c`](pmix_mca_base_components_close.c)
  closes and unloads, with an optional `skip` for the component that was
  just selected.

### `show_load_errors`

`mca_base_component_show_load_errors` accepts `"all"`, `"none"`, any
boolean spelling, or a comma-delimited list of `framework` /
`framework/component` items optionally prefixed with `^` to negate the
list. `pmix_mca_base_show_load_errors_init()` parses it once into two
lists and `pmix_mca_base_show_load_errors(framework, component)` answers
per call site.

**A list entry naming a bare framework leaves its `component_name`
`NULL`**, and a caller may pass a `NULL` component to ask about the
framework as a whole. Both are ordinary, documented inputs, so the
matcher has to handle a `NULL` on either side rather than handing it to
`strcmp`.

Use the **function**, never the raw
`pmix_mca_base_component_show_load_errors` string: the string is
non-`NULL` whenever the parameter has any value at all, including
`"none"` and lists naming some other framework.

### Aliases

[`pmix_mca_base_alias.c`](pmix_mca_base_alias.c) maps an old component
name to a new one so that `--mca btl vader` and `--mca btl sm` select the
same thing and variables registered under one name get synonyms under
the other. It is a hash of `project_framework_component` to a list of
alias strings, torn down by `pmix_mca_base_alias_cleanup()`. Nothing in
PMIx registers an alias today; the machinery is here because the MCA
base is shared lineage with Open MPI and PRRTE.

## Directory layout

```
src/mca/base/
├── pmix_base.h                        the public face of this directory
├── pmix_mca_base_open.c / _close.c    bring-up/tear-down + the base's own MCA params
│
│   -- variables --
├── pmix_mca_base_var.h / .c           registration, resolution, dump, build_env
├── pmix_mca_base_vari.h               internal declarations (flags, file values)
├── pmix_mca_base_var_group.h / .c     (project, framework, component) groups
├── pmix_mca_base_var_enum.h / .c      value and flag enumerators
├── pmix_mca_base_parse_paramfile.c    thin wrapper over the keyval parser
│
│   -- components --
├── pmix_mca_base_framework.h / .c     framework register/open/close + refcount
├── pmix_mca_base_component_find.c     static + dynamic discovery, selection filter
├── pmix_mca_base_component_repository.h / .c   the dlopen catalogue (PDL-gated)
├── pmix_mca_base_components_register.c
├── pmix_mca_base_components_open.c    + the show_load_errors parser
├── pmix_mca_base_components_select.c  generic single-select
├── pmix_mca_base_components_close.c
├── pmix_mca_base_component_compare.c  ordering + version compatibility
├── pmix_mca_base_alias.h / .c         component name aliases
├── pmix_mca_base_list.c               the two list-item classes
│
└── help-pmix-mca-base.txt, help-pmix-mca-var.txt
```

## Threading

**There is no thread-shifting here and no caddies.** The MCA base runs
during library start-up and shutdown, before the progress thread matters
and after it is gone, and its state — the variable pointer array, the
group array, the repository hash, the `show_load_errors` lists — is
guarded by nothing at all. That is safe only because everything that
touches it runs on the initializing thread inside `pmix_init_util()` /
`pmix_finalize_util()`, or from `pmix_info`.

Do not call `pmix_mca_base_var_register()` (or any framework
open/close) from the progress thread or from a user thread after
init. A component's `register`/`open`/`close` functions are the
sanctioned places to touch this machinery, and they are already called
on the right thread.

## Building

`src/mca/base` builds into `libpmix_mca_base.la`, which is linked into
`libpmix`. Its headers are **installed** (`pmix_HEADERS`) because
`pmix_info` and companion projects build against them. It has no
`configure.m4` and no components, so an edit here needs only a plain
`make` from the top of the tree — the `autogen.pl` + `configure` cycle
is not required unless you touch `Makefile.am`'s file lists in a way
that adds a new source, and even then `make` regenerates the `Makefile`.

The one build-system dependency worth knowing is `PMIX_HAVE_PDL_SUPPORT`,
which gates all of `pmix_mca_base_component_repository.c` and the
dynamic half of `pmix_mca_base_component_find.c`. A build without it
still works — every component is statically linked — so a change in
those files has to compile and behave in both configurations.

**Both `help-*.txt` files in this directory are `show_help` content.**
Per the top-level golden rule, after any edit to them:

```sh
rm src/util/pmix_show_help_content.*
make
```

## Testing

The unit suite for this directory is
[`test/unit/mca`](../../../test/unit/mca), wired into `make check`:

| Program | Covers |
|---------|--------|
| `mca_base_var` | name generation, register/find/get (including the aliasing contract of `get_value`), env-sourced values, synonyms, all three dump formats, `build_env`, deregister, `check_exclusive`, and the group registry |
| `mca_base_var_enum` | the bool and verbose enumerators, `_enum_create`, `_enum_create_flag`, and the dump-vs-accept agreement |
| `mca_base_component` | `component_compare` / `_compatible` / `_to_string`, `parse_requested`, and aliases |
| `mca_base_show_load_errors` | every accepted form of the parameter, including the bare-framework and `^` list forms |
| `mca_base_select` | `pmix_mca_base_select()` over fabricated components — priority order, the three ways a component is skipped, `PMIX_ERR_FATAL`, the empty list — and every include/exclude form of `pmix_mca_base_components_filter()` |
| `mca_base_paramfile` | the parameter-file path end to end: values read from a file, the file-list ordering, `~/` expansion, the `K`/`M`/`G` suffixes, and every precedence pair between file, override file and environment — including when the override names a synonym |
| `mca_base_framework` | register/open/close, the reference count, the `REGISTERED`/`OPEN` bits, the variables a registration creates, repeated cycling, and the `NOREGISTER`/`NO_DSO` flags. Its subject is a framework declared in the test itself with no components, so nothing a real framework's components do can colour the result |

`mca_base_var` comes up through `pmix_init_util()` — the lightest init
that establishes the install dirs and the MCA; a full `PMIx_Init` would
need a server. It sets `PMIX_MCA_mca_base_param_files=none` before that
call so the results do not depend on whatever is in the developer's
`~/.pmix`. **Keep that**: without it the suite passes or fails according
to the machine it runs on.

For the configurations a developer's own `make check` cannot produce —
notably a Linux tree built `--enable-mca-dso`, where every component is
a real `dlopen`ed plugin and the repository code path in this directory
is actually exercised — use
[`contrib/dockerswarm/run-mca-tests.sh`](../../../contrib/dockerswarm/run-mca-tests.sh).
On macOS with the default static build, the whole repository/`dlopen`
half of this directory never runs.

## Issues found in the August 2026 review

Recorded so a future change does not reintroduce them, and so the tests
above are read as regression tests rather than decoration:

- **`pmix_mca_base_show_load_errors()` dereferenced a `NULL` component
  name.** A list entry naming a bare framework (`^ptl`) plus any load
  error in one of its components was a segfault.
- **`pmix_mca_base_var_build_env()` dereferenced storage that synonyms
  and deregistered variables do not have.** Any deprecated STRING
  synonym with a non-default source — `PMIX_MCA_mca_base_param_files` is
  one — crashed the walk. Found by the new unit test.
- **The flag enumerator's `value_from_string()` indexed its flag table
  with the caller's token index**, matching the wrong flag and reading
  out of bounds for a list longer than the table.
- **`pmix_mca_base_component_repository_init()` parsed `project@path`
  with an unbounded copy loop** and no check that the `@` existed —
  a stack buffer overflow driven by a user-settable MCA parameter.
- **`process_repository_item()` `free()`d its caller's project string**
  on the out-of-memory path; the caller passes a stack buffer.
- **`ri_constructor()` left `ri_project`, `ri_base`, `ri_name` and
  `ri_refcnt` uninitialized** over a non-zeroing `PMIX_NEW`.
- **`parse_verbose()` pointed `lds_syslog_ident` into a buffer it then
  freed**, so `mca_base_verbose=syslogid:foo` used freed memory.
- **`var_set_from_env()` compared against `pmix_mca_base_var_override_file`
  without a `NULL` check**, which a process launched with
  `PMIX_PARAM_FILE_PASSED` can reach.
- **`pmix_mca_base_framework_register()` leaked its refcount** on every
  error path, leaving the framework permanently un-closeable.
- **`pmix_mca_base_show_load_errors_init()` leaked the `argv` vector of
  every list entry** and both vectors on each early return, and returned
  error codes through a `pmix_boolean_t`.
- **The enumerator `get_value()` slots `strdup`'d into a `const char **`
  out-parameter** whose other implementation returns a literal — an
  unfreeable allocation, i.e. a guaranteed leak in
  `pmix_mca_base_var_dump()`.
- **The bool enumerator's `dump()` advertised `y` and `n`**, which its
  `value_from_string()` rejected, and matched its other names
  case-sensitively while the non-enumerated bool path did not.
- **`component_find_check()` tested the raw
  `pmix_mca_base_component_show_load_errors` pointer**, so it reported
  even under `"none"`.
- Smaller items: `resolve_relative_paths()` freeing an indeterminate
  `asprintf` output, a leaked `getcwd` buffer, unchecked allocations in
  `register_variable()`, `err_msg` leaks in
  `pmix_mca_base_component_repository_open()`, the enumerator
  constructors leaking on a failed name `strdup`, a missing
  `<ctype.h>`, and `set_defaults()` still identifying itself to syslog
  as `"ompi"`.

A fifth sweep covered component selection and filtering and found **no
defect** — `pmix_mca_base_select()` and `pmix_mca_base_components_filter()`
behave as documented in all 32 cases now in `mca_base_select`. The only
change it produced was documentation: `pmix_mca_base_select()` is
exported and had a one-line comment saying "A generic select function",
which said nothing about the list ownership described above.

A fourth sweep, over the parameter-file and value-parsing path, found:

- **The override file could be defeated by the environment.** When the
  override file named a variable by a **deprecated synonym**, the
  promotion to `SOURCE_OVERRIDE` landed only on the synonym, so the
  environment check that follows saw an ordinary file value and let a
  user's `PMIX_MCA_*` setting win. A site administrator's "this is not
  negotiable" file was therefore enforced or not depending on which
  spelling of the name they happened to write — and the reported source
  said `FILE` either way. Covered now by
  `test/unit/mca/mca_base_paramfile.c`, which drives real files through
  a private temporary directory.

A third sweep, aimed at the lifecycle and re-entrancy paths, found:

- **A failed `pmix_mca_base_framework_register()` orphaned whatever
  components it had already found.** Discovery runs inside `register()`,
  so `framework_components` is populated by the time
  `component_find_check()` rejects a selection naming a component that
  does not exist — and nothing ever drains it, because
  `pmix_mca_base_framework_close()` returns immediately when neither
  `REGISTERED` nor `OPEN` is set, which is exactly the state a failed
  register leaves. Reachable from ordinary user input
  (`mca_base_abort_on_load_error` plus a bad `--mca <framework>` value),
  not just from an allocation failure. The `error:` label now unwinds
  the way the not-open branch of `close()` does.
- **`mca_base_abort_on_load_error` was gated on
  `mca_base_component_show_load_errors`**, which defaults to `"none"` —
  so the abort silently did nothing unless the user had *also* asked to
  see load-error messages. See the note under "Finding components".
- **`pmix_mca_base_framework_open()`'s failure path wedged the
  framework.** It decremented the reference count but left `REGISTERED`
  set, so a framework whose open failed reported itself registered with
  nobody holding it, and the next `close()` underflowed to −1 and
  silently did nothing. Every later close was then a no-op too. Found by
  the new `mca_base_framework` test.
- The `--disable-dlopen` configuration (`PMIX_HAVE_PDL_SUPPORT` 0) was
  compile-checked and is clean; the `#else` arms behave, and the
  ordering coupling with `pinstalldirs` described above is now explicit
  in the code rather than implied by another subsystem's guard.

A second sweep found that the **installed headers documented interfaces
that do not exist**, in ways that would make a caller write wrong code
rather than merely be confused:

- **`pmix_mca_base_var_get_value()` was documented as copying the value
  out**, through a `value_size` parameter that has never been in the
  signature. It returns a pointer to the registered storage. A caller
  following the comment would pass `&myint` and get a pointer written
  through it. `test/unit/mca/mca_base_var.c` now pins the aliasing
  contract.
- **`pmix_mca_base_var_register()` was documented with five parameters
  it does not take** — `enumerator`, `bind`, `flags`, `info_lvl`,
  `scope` — plus several paragraphs about how they behave. The
  enumerator paragraphs in particular describe a capability the library
  does not have (see Enumerators above).
- **`pmix_mca_base_var_group_find()` was documented as returning
  `PMIX_SUCCESS`**; it returns the group index, which is what all four
  callers rely on.
- **`pmix_mca_base_var_group_t::group_index` was never assigned**, so
  every group reported index 0 to anyone reading the field out of the
  installed header. It is now set alongside the pointer-array add, the
  way `mbv_index` already was.
- **`pmix_mca_base_open()`'s `add_path` had no documentation at all**,
  and is not a directory: it is spliced into a `;`-delimited list of
  `project@path` entries. Every in-tree caller passes `NULL`, so the
  requirement was invisible, and an entry without an `@` is silently
  skipped (before the fix above, it was a buffer overrun).
- The colour strings in `pmix_var_dump_color[]` are only populated by
  `pmix_register_params()`, which `pmix_init_util()` does not call —
  so a `..._DUMP_READABLE_COLOR` dump reached before that fed `NULL` to
  a `"%s"`. The dump sites now fall back to the uncoloured default,
  which is what the surrounding code already intended.

## When working in this directory

- **Nothing here may assume the variable system is initialized.**
  `var_get()` and friends check `pmix_mca_base_var_initialized` and
  return `PMIX_ERROR`; keep new entry points doing the same.
- **Walking `pmix_mca_base_vars` directly means handling synonyms and
  deregistered entries.** Neither has storage. Prefer
  `pmix_mca_base_var_get_value()`, which resolves through.
- **Test the sign of a `_register()` return, never `!= PMIX_SUCCESS`.**
- **Every MCA parameter value is attacker-adjacent input.** They arrive
  from the environment, from files under the user's control, and from
  command lines. Parse them defensively — the two buffer bugs above were
  both in parameter parsing.
- **Preserve the "no thread-shifting" property.** If a new code path
  needs MCA state at run time, cache the value at registration instead of
  reaching into the variable system from the progress thread.
- **Add the regression test in [`test/unit/mca`](../../../test/unit/mca)
  in the same commit.** This directory has no server dependency and no
  hardware dependency, so there is essentially never a reason a fix here
  cannot be covered.
