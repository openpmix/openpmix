# AGENTS.md: PMIx Language Bindings (`bindings/`)

Orientation for AI and human contributors working on the PMIx language
bindings. This is a *map*, not the rulebook — the top-level
[`CLAUDE.md`/`AGENTS.md`](../CLAUDE.md) at the repo root and the docs under
[`docs/`](../docs/) are authoritative. When this file and those disagree,
they win — and please fix this file.

---

## What lives here

This directory holds bindings of the PMIx C API to other programming
languages. Today there is exactly one:

| Directory | Binding | Guide |
|-----------|---------|-------|
| [`python/`](python/) | A Cython extension module named `pmix` | [`python/AGENTS.md`](python/AGENTS.md) |

`Makefile.am` here does nothing but list `SUBDIRS`. Adding a binding for
another language means adding a directory, listing it there, and wiring
its `configure` support in the usual way (see the top-level guidance on
[modifying the configure / build system](../CLAUDE.md)).

**Work on the Python bindings belongs in
[`python/AGENTS.md`](python/AGENTS.md)** — it is far more detailed than
this file and covers the conversion layer, the threading model, and the
defects that recur there.

---

## The one rule that governs every binding

From [`README`](README):

> You can wrap a framework, but you cannot wrap a specific plugin within
> that framework.

Plugins are reached only through their framework's interface, and which
plugin is active is a runtime selection the caller does not control.
Binding `pmix_preg_raw_generate_regex` rather than `PMIx_generate_regex2`
would produce something that silently does the wrong thing the moment a
different `preg` component wins selection. There is no restriction on how
many bindings exist, or on what kind of function they wrap, beyond that.

Two consequences worth stating plainly, because they are what the rule is
protecting:

- **Bind the public API, not the internals.** The `PMIx_*` entry points in
  [`include/`](../include/) are the surface that is frozen across releases
  (see the top-level *Backward Compatibility* section). Anything under
  `src/` is not, and a binding that reaches into it will break without
  warning.
- **A binding must actually reach a `PMIx_*` entry point.** A method that
  builds up arguments and returns success without calling the library is
  worse than a missing one: the caller believes it worked. This has
  happened here before; see §8 of the Python guide.

---

## Where the tests are

Deliberately **not** in this directory. The tests for a binding live with
the rest of the project's tests:

| What | Where |
|------|-------|
| Python unit tests + connected round trips | [`test/python/`](../test/python/) |
| Python multi-node tests | [`contrib/dockerswarm/python/`](../contrib/dockerswarm/python/) |
| Runnable Python examples | [`examples/python/`](../examples/python/) |

Keeping them out of `bindings/` is a deliberate correction: a copy of the
test scripts used to live under `bindings/python/tests/`, only CI ran it,
and it drifted out of sync with the bindings it was supposed to be
testing. One home per test.

## Building

The bindings are **not** built by default. `configure` must be told to
want them — for Python, `--enable-python-bindings`, which also requires a
Python 3 with Cython and setuptools. Everything under this directory is
inside a `WANT_<LANGUAGE>_BINDINGS` Automake conditional, so on a tree
configured without them, a change here compiles nothing at all and a
clean `make` proves nothing.

**Check that your configured tree is actually building what you changed**
before believing a green build. For Python:

```sh
grep WANT_PYTHON_BINDINGS_TRUE config.status   # "" means yes, "#" means no
```
