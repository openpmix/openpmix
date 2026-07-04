
Regular Expression (Node/Proc Map) Support
===========================================

This document describes how PMIx generates and parses the compact
"regular expressions" it uses to convey a job's **node map** and
**process map**, from the public server APIs down through the MCA
``preg`` framework and its components, and how the result is consumed by
the ``gds/hash`` datastore when a namespace is registered.

For a code-oriented orientation aimed at contributors working *inside*
the framework, each ``preg`` directory also carries an ``AGENTS.md`` (with
a ``CLAUDE.md`` symlink): ``src/mca/preg/AGENTS.md`` for the framework as a
whole, plus one each in ``native/``, ``raw/``, and ``compress/``. This
document explains how regex support is integrated into the wider library;
those explain each component's internals in detail.


The Problem
-----------

When a launcher or resource manager starts a job, it must tell the PMIx
server two things about the layout, and that information must ultimately
reach every client process:

* the **node map** — the list of node names in the allocation, and
* the **process map** — the list of process ranks resident on each node.

On a large machine these lists are enormous and highly regular
(``node000001,node000002,...``). Shipping the raw list to every one of
hundreds of thousands of processes is wasteful, so PMIx encodes the list
into a compact, self-describing representation on the producing side and
expands it again on the consuming side. The ``preg`` framework owns both
halves of that transform.

Two properties are guaranteed across the encode/decode round trip:

#. **Order preservation** — values come out in the same order they went
   in.
#. **Self-identification** — every encoded value is tagged with the name
   of the scheme that produced it, so a peer can select the correct parser
   without knowing which components the producer had configured. This is
   what makes the encoding interoperable across PMIx versions and
   deployments.


Public APIs
-----------

Two generations of API coexist.

Current API (``pmix_regex2_t``)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Declared in ``include/pmix_server.h``::

    pmix_status_t PMIx_generate_regex2(const char *input,
                                       pmix_info_t info[], size_t ninfo,
                                       pmix_regex2_t *regex);

    pmix_status_t PMIx_parse_regex2(const pmix_regex2_t *regex,
                                    pmix_info_t info[], size_t ninfo,
                                    char **output);

``PMIx_generate_regex2`` takes a comma-separated ``input`` list and fills
in a ``pmix_regex2_t`` (defined in ``include/pmix_common.h.in``)::

    typedef struct pmix_regex2 {
        char    *type;    /* encoding tag: "pmix", "raw", "compress"     */
        uint8_t *bytes;   /* encoded representation (may NOT be a string) */
        size_t   len;     /* number of bytes in `bytes`                  */
    } pmix_regex2_t;

``PMIx_parse_regex2`` reverses it, returning a comma-separated string.
The ``info``/``ninfo`` attribute arrays are required by the PMIx API
convention but carry no defined attributes yet.

The struct exists because an encoded regex is fundamentally *bytes + a
length + a type*, not a C string — the ``compress`` encoding contains
embedded NUL bytes. The matching data type is ``PMIX_REGEX2``; helper
routines ``PMIx_Regex2_construct`` / ``_destruct`` / ``_create`` /
``_free`` manage the struct (implemented in
``src/mca/bfrops/base/bfrop_base_macro_backers.c``).

Deprecated API (``char *``)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Declared in ``include/pmix_deprecated.h``::

    pmix_status_t PMIx_generate_regex(const char *input, char **regex);
    pmix_status_t PMIx_generate_ppn(const char *input, char **ppn);

These return the encoded form as a plain ``char *`` and use the
deprecated ``PMIX_REGEX`` data type (value ``49``). They remain because
production launchers built against older releases still call them, and
because the per-node process map is still carried internally in the string
form. New code should prefer ``PMIx_generate_regex2``.

All four entry points are thin wrappers in ``src/server/pmix_server.c``
that check ``pmix_globals.initialized`` and forward to the corresponding
``pmix_preg`` module function::

    PMIx_generate_regex   -> pmix_preg.generate_node_regex
    PMIx_generate_ppn     -> pmix_preg.generate_ppn
    PMIx_generate_regex2  -> pmix_preg.generate_regex
    PMIx_parse_regex2     -> pmix_preg.parse_regex


Producer / Consumer Flow
------------------------

The typical lifecycle of a node map::

  [ Host RM / launcher, e.g. PRRTE ]
      PMIx_generate_regex2("node01,node02,...", NULL, 0, &regex)
          └─► pmix_preg.generate_regex()  [smallest encoding wins]
                                                    │
      Load regex into a pmix_info_t keyed PMIX_NODE_MAP (type PMIX_REGEX2)
      (and a PMIX_PROC_MAP for the process layout)
          │
          ▼
      PMIx_server_register_nspace(nspace, ..., info, ninfo, ...)
          └─► stored via the gds framework
                    │
                    ▼
  [ gds/hash, when unpacking the job-level info ]
      store_map()  [src/mca/gds/hash/gds_hash.c]
          PMIX_NODE_MAP:
              PMIX_REGEX2 -> pmix_preg.parse_regex(regex2, &decoded)
                             pmix_preg.parse_nodes(decoded, &nodes)
              PMIX_REGEX  -> pmix_preg.parse_nodes(bo.bytes, &nodes)
              PMIX_STRING -> pmix_preg.parse_nodes(string, &nodes)
          PMIX_PROC_MAP: symmetric, using parse_procs
                    │
                    ▼
      The expanded node/rank argv arrays populate the datastore, and the
      per-process job-level keys (PMIX_NODEID, local peers, etc.) are
      derived from them and made available to clients via PMIx_Get.

Note the two-step decode for ``PMIX_REGEX2`` in
``src/mca/gds/hash/gds_hash.c``: ``parse_regex`` turns the
``pmix_regex2_t`` back into a comma-separated ``char *``, and then the
*legacy* ``parse_nodes`` expands any ranges in that string into individual
names. The regex2 struct is the transport; the legacy string parser is
still the range expander. ``src/mca/gds/hash/process_arrays.c`` performs
the same ``parse_nodes`` / ``parse_procs`` expansion when node/proc maps
arrive inside a nested info array.


The MCA ``preg`` Framework
--------------------------

The framework lives under ``src/mca/preg/``. Its module interface
(``src/mca/preg/preg.h``) is a struct of ten function pointers spanning
both API generations::

    typedef struct {
        char *name;
        /* legacy char* API */
        pmix_preg_base_module_generate_node_regex_fn_t generate_node_regex;
        pmix_preg_base_module_generate_ppn_fn_t        generate_ppn;
        pmix_preg_base_module_parse_nodes_fn_t         parse_nodes;
        pmix_preg_base_module_parse_procs_fn_t         parse_procs;
        pmix_preg_base_module_copy_fn_t                copy;
        pmix_preg_base_module_pack_fn_t                pack;
        pmix_preg_base_module_unpack_fn_t              unpack;
        pmix_preg_base_module_release_fn_t             release;
        /* current pmix_regex2_t API */
        pmix_preg_base_module_generate_regex_fn_t      generate_regex;
        pmix_preg_base_module_parse_regex_fn_t         parse_regex;
    } pmix_preg_module_t;

A component may leave any pointer ``NULL``; the base skips it. ``preg`` is
a **multi-select** framework — several components are active at once and a
request is offered to each in priority order.

Component Selection
~~~~~~~~~~~~~~~~~~~~

``src/mca/preg/base/preg_base_select.c`` queries each component once at
init, wraps the returned modules, and inserts them into
``pmix_preg_globals.actives`` in **descending priority order**. If no
component is selected it emits the ``no-plugins`` ``show_help`` topic and
returns ``PMIX_ERR_SILENT`` — at least one component is required.

.. list-table::
   :header-rows: 1
   :widths: 20 15 45

   * - Component
     - Priority
     - Active when
   * - ``compress``
     - 100
     - a ``pcompress`` (zlib) module supplies ``compress_string``
   * - ``raw``
     - 50
     - always
   * - ``native``
     - 30
     - always

Dispatch (``preg_base_stubs.c``)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Every ``pmix_preg`` entry points at a ``pmix_preg_base_*`` stub in
``src/mca/preg/base/preg_base_stubs.c``. There are three dispatch
behaviors:

**First-success-wins** — ``generate_node_regex``, ``generate_ppn``,
``parse_nodes``, ``parse_procs``, ``copy``, ``pack``, ``unpack``. Walk the
actives in priority order and return on the first ``PMIX_SUCCESS``. If no
module claims the request, apply a built-in fallback so the call
essentially never fails:

* ``generate_*`` → ``strdup(input)`` (ship it uncompressed)
* ``parse_nodes`` → ``PMIx_Argv_split(regex, ',')``
* ``parse_procs`` → ``PMIx_Argv_split(regex, ';')``
* ``copy`` → ``strdup`` + ``strlen + 1``
* ``pack`` / ``unpack`` → serialize as a plain ``PMIX_STRING``

**First-success, no fallback** — ``release`` returns
``PMIX_ERR_BAD_PARAM`` if no scheme claims the pointer. Always free a
regex through the framework that produced it.

**Smallest-wins** — ``generate_regex`` is the exception: it calls *every*
module's ``generate_regex``, keeps the candidate with the smallest
``len``, frees the losers, and returns the winner (or
``PMIX_ERR_NOT_SUPPORTED``). This is how the framework automatically
selects the most compact encoding without the caller choosing a component.
``parse_regex`` returns to first-success, matching on the ``type`` tag,
and returns ``PMIX_ERR_NOT_SUPPORTED`` if no active scheme recognizes it.

A practical consequence: because ``raw``'s generators always succeed and
outrank ``native``, the *legacy* generate path is claimed by ``compress``
(if present) or ``raw``, and ``native``'s generator is effectively
shadowed in a default build. ``native`` still serves as the **parser** of
the ``pmix[...]`` format, which external launchers produce.


``preg`` Components
-------------------

native (src/mca/preg/native/)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Implements PMIx's own range-compressing format, tagged ``pmix``::

    Input:   odin009,odin010,odin011,odin012,odin017,odin018,thor176
    Output:  pmix[odin[3:9-12,17-18],thor176]

It groups names sharing a prefix, a numeric-field width, and a suffix, and
coalesces consecutive integers into ``start-end`` ranges. The width
(``3`` above) preserves leading zeros on expansion. A ``skip`` flag on
each candidate value enforces order preservation when names of different
shapes are interleaved. ``native`` implements only the legacy string API
(``generate_regex`` is ``NULL``); it is the parser of record for
``pmix[...]`` data whether produced locally or by an external launcher.

raw (src/mca/preg/raw/)
~~~~~~~~~~~~~~~~~~~~~~~~

The pass-through, tagged ``raw:``. Its generators simply prepend the tag
and *always succeed*; its parsers strip the tag and ``PMIx_Argv_split``.
``raw`` implements the **complete** module — every legacy function and
both regex2 functions — making it the framework's guaranteed encoder of
last resort and the baseline candidate in the smallest-wins ``generate_regex``
contest. For input too small or too random to compress, ``raw`` wins.

compress (src/mca/preg/compress/)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Runs the list through the ``pcompress`` (zlib) framework. It disables
itself (its ``component_query`` returns an error, so no module is
selected) when no compression back end is present, so its encoding must
never be assumed available. Two tags are involved, one per API generation:

* Legacy string form — framed as
  ``"blob:\0component=zlib:\0size=<N>:\0<N bytes>"``. This contains
  embedded NUL bytes, which is why ``compress`` implements its own
  ``copy``/``pack``/``unpack``/``release`` rather than relying on the
  string fallbacks.
* regex2 form — ``type = "compress"``, ``bytes`` = raw zlib output,
  ``len`` = its length (no framing needed, since the struct carries the
  length explicitly).

At priority 100 it is tried first on every legacy generate and almost
always wins the smallest-wins regex2 contest — but it is also the
component most likely to be absent, so both configurations must be tested.


Wire Format
-----------

When a ``pmix_regex2_t`` is transmitted between peers it is serialized by
``pmix_bfrops_base_pack_regex2`` (``src/mca/bfrops/base/bfrop_base_pack.c``)
as, in order: the ``type`` string, then ``len`` as a ``PMIX_SIZE``, then
``len`` ``PMIX_BYTE`` values (only when ``len > 0``). Unpacking mirrors
this. Per the PMIx interoperability rules this field order is frozen —
fields may only be appended, never reordered or removed — so that a server
and client built against different PMIx versions remain compatible.

The legacy string form is serialized either by a component's own
``pack``/``unpack`` (which ``memcpy`` the exact byte length into the
buffer, embedded NULs included) or, in the base fallback, as a plain
``PMIX_STRING``.


Thread Safety
-------------

The ``preg`` functions are pure, synchronous transforms of their
arguments. They allocate and return; they do not thread-shift, block, or
mutate shared library state beyond reading the ``actives`` list that was
built once at init. They are therefore safe to call directly from a
progress-thread handler — which is exactly where the server APIs and
``gds/hash`` invoke them — and there is no caddy/thread-shift dance in this
path. The only ordering constraint is that they must not run concurrently
with framework open/close, which happens only at startup and shutdown.


Key Source Files
----------------

.. list-table::
   :header-rows: 1
   :widths: 48 40

   * - File
     - Purpose
   * - ``include/pmix_server.h``
     - ``PMIx_generate_regex2`` / ``PMIx_parse_regex2`` declarations
   * - ``include/pmix_common.h.in``
     - ``pmix_regex2_t`` and ``PMIX_REGEX2`` definitions
   * - ``include/pmix_deprecated.h``
     - deprecated ``PMIx_generate_regex`` / ``PMIx_generate_ppn`` / ``PMIX_REGEX``
   * - ``src/server/pmix_server.c``
     - public API wrappers into ``pmix_preg``
   * - ``src/mca/preg/preg.h``
     - ``preg`` MCA module interface
   * - ``src/mca/preg/preg_types.h``
     - ``pmix_regex_range_t`` / ``pmix_regex_value_t`` helper classes
   * - ``src/mca/preg/base/preg_base_select.c``
     - component selection / priority ordering
   * - ``src/mca/preg/base/preg_base_stubs.c``
     - the ``pmix_preg_base_*`` dispatch functions
   * - ``src/mca/preg/native/preg_native.c``
     - the ``pmix[...]`` range compressor and parser
   * - ``src/mca/preg/raw/preg_raw.c``
     - the ``raw:`` pass-through
   * - ``src/mca/preg/compress/preg_compress.c``
     - the ``blob:`` / ``compress`` zlib encoder
   * - ``src/mca/gds/hash/gds_hash.c``
     - node/proc map consumer (``store_map``)
   * - ``src/mca/bfrops/base/bfrop_base_pack.c``
     - ``pmix_regex2_t`` wire serialization
