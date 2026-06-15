
PMIx_Log Support
================

This document describes how the ``PMIx_Log`` / ``PMIx_Log_nb`` API is
implemented inside the PMIx library, from the public API entry points
down through the MCA ``plog`` framework and its components, including the
special ``pmix_show_help`` duplicate-suppression use-case.


Public API
----------

Two functions are exported from ``include/pmix.h``::

    pmix_status_t PMIx_Log(const pmix_info_t data[], size_t ndata,
                           const pmix_info_t directives[], size_t ndirs);

    pmix_status_t PMIx_Log_nb(const pmix_info_t data[], size_t ndata,
                              const pmix_info_t directives[], size_t ndirs,
                              pmix_op_cbfunc_t cbfunc, void *cbdata);

``PMIx_Log`` is a thin blocking wrapper: it constructs a ``pmix_cb_t``,
calls ``PMIx_Log_nb``, then sleeps on the condition variable inside the
callback object until the non-blocking path completes.

The *data* array carries one entry per logging channel (what to log and
where), and the *directives* array carries behavioral control attributes
(timestamps, aggregation keys, ``PMIX_LOG_ONCE``, etc.).

Both functions are implemented in ``src/common/pmix_log.c``.


Channel Constants
-----------------

Log channel names are defined in ``include/pmix_common.h.in``.  The key
ones are:

.. list-table::
   :header-rows: 1
   :widths: 30 22 20

   * - Constant
     - Key string
     - Component that handles it
   * - ``PMIX_LOG_STDERR``
     - ``pmix.log.stderr``
     - ``plog/stdfd``
   * - ``PMIX_LOG_STDOUT``
     - ``pmix.log.stdout``
     - ``plog/stdfd``
   * - ``PMIX_LOG_SYSLOG``
     - ``pmix.log.syslog``
     - ``plog/syslog``
   * - ``PMIX_LOG_LOCAL_SYSLOG``
     - ``pmix.log.lsys``
     - ``plog/syslog``
   * - ``PMIX_LOG_GLOBAL_SYSLOG``
     - ``pmix.log.gsys``
     - ``plog/syslog`` (gateway only)
   * - ``PMIX_LOG_EMAIL``
     - ``pmix.log.email``
     - ``plog/smtp``

Directive constants (passed in the *directives* array) include:

* ``PMIX_LOG_SOURCE`` — ``(pmix_proc_t*)`` originator of the request
* ``PMIX_LOG_TIMESTAMP`` — ``(time_t)`` explicit timestamp
* ``PMIX_LOG_GENERATE_TIMESTAMP`` — ``(bool)`` auto-generate a timestamp
* ``PMIX_LOG_ONCE`` — ``(bool)`` stop after the first successful component
* ``PMIX_LOG_AGG`` — ``(bool)`` aggregate (suppress) duplicate messages
* ``PMIX_LOG_KEY`` / ``PMIX_LOG_VAL`` — ``(char*)`` key/value pair used for
  duplicate detection (see `pmix_show_help Aggregation`_ below)
* ``PMIX_LOG_SYSLOG_PRI`` — ``(int)`` syslog priority level
* ``PMIX_LOG_TAG_OUTPUT`` — ``(bool)`` prefix output with channel name
* ``PMIX_LOG_XML_OUTPUT`` — ``(bool)`` emit output in XML format


Call Flow
---------

The diagram below traces the full path for a log request.

::

  PMIx_Log(data, directives)
      └─► PMIx_Log_nb(data, directives, opcbfunc, &cb)  [src/common/pmix_log.c]
               │
               ├─ [Client / tool, connected to server]
               │       Pack PMIX_LOG_CMD into wire message
               │       Optionally pack timestamp (v3+)
               │       Pack data[], directives[]
               │       PMIX_PTL_SEND_RECV → log_cbfunc()
               │           │
               │           │  Server receives PMIX_LOG_CMD
               │           │  pmix_server_log()  [src/server/pmix_server_ops.c]
               │           │      Unpack timestamp, data[], directives[]
               │           │      Append PMIX_LOG_SOURCE (requester's proc)
               │           │      Optionally append PMIX_LOG_TIMESTAMP
               │           │      ──────────────────────────────────────
               │           │      if (NOT gateway  OR  pmix_log_host_only)
               │           │          Upcall: pmix_host_server.log2()
               │           │                 or pmix_host_server.log()
               │           │      else (gateway and !pmix_log_host_only)
               │           │          pmix_plog.log()  ─► plog framework
               │           │      ──────────────────────────────────────
               │           │
               │           └─ If server returned ERR_NOT_AVAILABLE:
               │                  fall through to local plog processing
               │
               ├─ [Client / tool, NOT connected]  ──────────────────────────┐
               │                                                             │
               └─ [Server, not gateway or pmix_log_host_only set]           │
                       Upcall host via log2() / log()                       │
                       If neither available → fall through ──────────────────┤
                                                                             │
               [Server, gateway and !pmix_log_host_only] ───────────────────┤
                                                                             ▼
                                                             PMIX_THREADSHIFT → pmix_log_local_op()
                                                                 pmix_plog.log()
                                                                 └─► pmix_plog_base_log()
                                                                     [src/mca/plog/base/plog_base_stubs.c]


The MCA plog Framework
----------------------

The ``plog`` framework lives under ``src/mca/plog/``.  Its module
interface (``src/mca/plog/plog.h``) exposes::

    typedef struct {
        char  *name;
        char **channels;   /* NULL-terminated list of channel key substrings */
        pmix_plog_base_module_init_fn_t   init;
        pmix_plog_base_module_fini_fn_t   finalize;
        pmix_plog_base_module_log_fn_t    log;
    } pmix_plog_module_t;

    typedef pmix_status_t (*pmix_plog_base_module_log_fn_t)(
        const pmix_proc_t *source,
        const pmix_info_t data[],  size_t ndata,
        const pmix_info_t directives[], size_t ndirs);

Each component declares the channel key substrings it handles.  The base
framework matches an incoming data entry's key against those substrings
with a simple ``strstr`` call, so a key of ``"pmix.log.stderr"`` matches
a component whose ``channels[]`` contains ``"stderr"``.

Component Selection
~~~~~~~~~~~~~~~~~~~

At framework open time, ``src/mca/plog/base/plog_base_select.c`` queries
all available components, sorts them by priority, and builds
``pmix_plog_globals.actives`` — a ``pmix_pointer_array_t`` of
``pmix_plog_base_active_module_t`` objects.

The MCA parameter ``pmix_plog_base_order`` can force a specific ordering.
If a channel is suffixed with ``:req``, the framework will abort
initialization if no component can satisfy it (error via
``pmix_show_help("help-pmix-plog.txt", "reqd-not-found", ...)``.

Dispatch (pmix_plog_base_log)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``pmix_plog_base_log()`` in ``src/mca/plog/base/plog_base_stubs.c``:

1. Scans *directives* for ``PMIX_LOG_ONCE``, ``PMIX_LOG_AGG``,
   ``PMIX_LOG_KEY``, and ``PMIX_LOG_VAL``.
2. Runs the `pmix_show_help Aggregation`_ duplicate check (see below).
3. For each non-completed data entry, finds every active module whose
   ``channels[]`` list contains the entry's key as a substring, and
   appends that module to a temporary ordered list (preserving the order
   in which they appear in the data array — the first-listed channel gets
   first priority for ``PMIX_LOG_ONCE``).
4. Iterates through the assembled list, calling ``module->log()`` for each:

   * ``PMIX_SUCCESS`` or ``PMIX_OPERATION_SUCCEEDED`` — success; if
     ``PMIX_LOG_ONCE`` is set, stop.
   * ``PMIX_ERR_NOT_AVAILABLE`` or ``PMIX_ERR_TAKE_NEXT_OPTION`` — this
     module cannot handle the request; skip to next.
   * Any other error — abort processing.


plog Components
---------------

stdfd (src/mca/plog/stdfd/)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Handles ``PMIX_LOG_STDERR`` and ``PMIX_LOG_STDOUT``.

* When called on a *client*: writes directly to ``stderr`` / ``stdout``
  via ``fprintf``.
* When called on a *server*: delivers the output to the client's IOF
  stream via ``PMIx_server_IOF_deliver()`` so it appears on the
  originating process's stdio.

The component also honours ``PMIX_LOG_TAG_OUTPUT`` (prepend channel name)
and ``PMIX_LOG_XML_OUTPUT`` (wrap in XML tags).

syslog (src/mca/plog/syslog/)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Handles ``PMIX_LOG_SYSLOG``, ``PMIX_LOG_LOCAL_SYSLOG``, and (on gateway
servers only) ``PMIX_LOG_GLOBAL_SYSLOG``.

MCA parameters on the *component* (set at open time) configure the
default syslog facility and priority level.  The per-call
``PMIX_LOG_SYSLOG_PRI`` directive overrides the priority.  Unrecognised
facility or level strings cause a ``pmix_show_help`` warning and the
component falls back to sensible defaults.

smtp (src/mca/plog/smtp/)
~~~~~~~~~~~~~~~~~~~~~~~~~~

Handles ``PMIX_LOG_EMAIL``.  Requires libesmtp at build time.

Additional directives used only by this component:

* ``PMIX_LOG_EMAIL_ADDR`` — ``(char*)`` recipient address(es)
* ``PMIX_LOG_EMAIL_SENDER_ADDR`` — ``(char*)`` sender address
* ``PMIX_LOG_EMAIL_SUBJECT`` — ``(char*)`` subject line
* ``PMIX_LOG_EMAIL_SERVER`` — ``(char*)`` SMTP relay hostname

A failed ``libesmtp`` send emits a ``pmix_show_help`` warning from
``help-pmix-plog.txt`` topic ``smtp:send_email failed``.


pmix_show_help Aggregation
--------------------------

``pmix_show_help`` (``src/util/pmix_show_help.c``) is PMIx's general
facility for displaying parameterised, multi-line help and error messages
stored in ``*.txt`` files under ``$pkgdatadir``.  Callers pass a filename
and a topic name::

    pmix_show_help("help-pmix-plog.txt", "syslog:unrec-level", true, level);

The library locates the ``[syslog:unrec-level]`` block in the file,
performs ``printf``-style substitutions, and emits the result to stderr.

The plog framework uses ``pmix_show_help``'s duplicate-tracking
infrastructure to implement the ``PMIX_LOG_AGG`` directive.
Specifically, the function::

    pmix_status_t pmix_help_check_dups(const char *filename, const char *topic);

returns ``PMIX_SUCCESS`` if the ``(filename, topic)`` pair has been seen
before and records the pair on first call.

When ``pmix_plog_base_log()`` sees ``PMIX_LOG_AGG=true`` together with
both ``PMIX_LOG_KEY`` and ``PMIX_LOG_VAL`` in the directives, it calls
``pmix_help_check_dups(key, val)`` before dispatching to any component.
If the pair has already been logged, every data entry is marked
``PMIX_INFO_OP_COMPLETED`` and the dispatch loop skips them all — the
duplicate message is silently suppressed.

This is how callers (typically inside the PMIx library itself) prevent
the same error or status message from flooding the output when many
processes encounter the same condition.  The typical pattern is::

    pmix_info_t data[1], dirs[3];
    PMIX_INFO_LOAD(&data[0], PMIX_LOG_STDERR, message_string, PMIX_STRING);

    PMIX_INFO_LOAD(&dirs[0], PMIX_LOG_AGG,  &(bool){true}, PMIX_BOOL);
    PMIX_INFO_LOAD(&dirs[1], PMIX_LOG_KEY,  filename_key,  PMIX_STRING);
    PMIX_INFO_LOAD(&dirs[2], PMIX_LOG_VAL,  topic_key,     PMIX_STRING);

    PMIx_Log(data, 1, dirs, 3);

The first call goes through; subsequent calls with the same
``PMIX_LOG_KEY`` / ``PMIX_LOG_VAL`` pair are no-ops.


Gateway Servers vs. Non-Gateway Servers
---------------------------------------

The split between "upcall to host" and "process locally" appears in two
places: ``PMIx_Log_nb`` for server callers, and ``pmix_server_log`` for
messages arriving from a client.  Both use identical logic:

* **Non-gateway server** (ordinary daemon) — always upcall to the host
  RM via ``pmix_host_server.log2()`` (preferred) or
  ``pmix_host_server.log()`` (legacy).  If neither is implemented, the
  server falls through to local plog processing.

* **Gateway server** (the server that touches the outside world, e.g.,
  the head-node daemon in PRRTE) — processes the request locally via the
  plog framework so it can reach global syslog, send email, etc.

The ``pmix_log_host_only`` MCA parameter (``bool``, default ``false``)
overrides this: when set, *all* log requests are forwarded to the host
regardless of gateway status, and local plog processing is bypassed
entirely.  If the host provides no ``log`` or ``log2`` upcall and
``pmix_log_host_only`` is set, ``PMIx_Log`` returns
``PMIX_ERR_NOT_SUPPORTED``.


The pmix_log_host_only MCA Parameter
-------------------------------------

Defined and registered in ``src/runtime/pmix_params.c``::

    bool pmix_log_host_only = false;   /* exported via src/runtime/pmix_rte.h */

    pmix_mca_base_var_register("pmix", "pmix", NULL, "log_host_only",
        "Direct all PMIx_Log handling to the host environment;"
        " the library's plog framework will not be invoked [default: no]",
        PMIX_MCA_BASE_VAR_TYPE_BOOL,
        &pmix_log_host_only);

Set via the standard MCA parameter mechanism, e.g.::

    export PMIX_MCA_pmix_log_host_only=1


Thread Safety
-------------

``PMIx_Log_nb`` does not call the plog framework directly from the
calling thread.  When local processing is required, it packages the
request into a ``pmix_shift_caddy_t`` and uses ``PMIX_THREADSHIFT`` to
deliver it to ``pmix_log_local_op()`` on the PMIx progress thread.
Individual plog components may thread-shift further internally if they
need to perform blocking I/O.

When a client sends a log request to a server and the server cannot
satisfy it (returns ``PMIX_ERR_NOT_AVAILABLE``), the client-side
``log_cbfunc()`` receives the reply and falls through to local plog
processing on the client side — ensuring a best-effort result even when
the server is not a gateway.


Key Source Files
----------------

.. list-table::
   :header-rows: 1
   :widths: 45 35

   * - File
     - Purpose
   * - ``include/pmix.h``
     - Public API declarations
   * - ``include/pmix_common.h.in``
     - Channel and directive constants
   * - ``src/common/pmix_log.c``
     - ``PMIx_Log`` / ``PMIx_Log_nb`` implementation
   * - ``src/server/pmix_server_ops.c``
     - ``pmix_server_log()`` handler
   * - ``src/mca/plog/plog.h``
     - plog MCA module interface
   * - ``src/mca/plog/base/plog_base_select.c``
     - Component selection at init
   * - ``src/mca/plog/base/plog_base_stubs.c``
     - ``pmix_plog_base_log()`` dispatcher
   * - ``src/mca/plog/stdfd/plog_stdfd.c``
     - stdout / stderr component
   * - ``src/mca/plog/syslog/plog_syslog.c``
     - syslog component
   * - ``src/mca/plog/smtp/plog_smtp.c``
     - email (SMTP) component
   * - ``src/util/pmix_show_help.c``
     - show-help subsystem & dup tracking
   * - ``src/runtime/pmix_params.c``
     - ``pmix_log_host_only`` MCA parameter
