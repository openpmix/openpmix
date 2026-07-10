.. _man3-PMIx_tool_init:

PMIx_tool_init
==============

.. include_body

``PMIx_tool_init`` |mdash| Initialize the PMIx tool library.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix_tool.h>

   pmix_status_t PMIx_tool_init(pmix_proc_t *proc,
                                pmix_info_t info[], size_t ninfo);


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

  from pmix import *

  foo = PMIxTool()
  # the directives is a list of Python ``pmix_info_t`` dictionaries
  pydirs = [{'key': PMIX_TOOL_NSPACE,
             'value': {'value': "mytool", 'val_type': PMIX_STRING}},
            {'key': PMIX_TOOL_RANK,
             'value': {'value': 0, 'val_type': PMIX_PROC_RANK}}]
  rc, myname = foo.init(pydirs)


INPUT PARAMETERS
----------------

* ``info``: Pointer to an array of :ref:`pmix_info_t(5) <man5-pmix_info_t>`
  structures conveying directives that qualify the initialization and any
  subsequent server connection (see `DIRECTIVES`_). A ``NULL`` value is
  supported if no directives are desired.
* ``ninfo``: Number of elements in the ``info`` array.


OUTPUT PARAMETERS
-----------------

* ``proc``: Pointer to a :ref:`pmix_proc_t(5) <man5-pmix_proc_t>` structure in
  which the tool's namespace and rank are to be returned. Unlike
  :ref:`PMIx_Init(3) <man3-PMIx_Init>`, the *initial* call to ``PMIx_tool_init``
  requires a non-``NULL`` value here so that the assigned identifier can be
  returned; passing ``NULL`` on the first call returns ``PMIX_ERR_BAD_PARAM``.


DESCRIPTION
-----------

Initialize the PMIx tool library, returning the process identifier assigned to
this tool in the provided ``pmix_proc_t`` struct.

When called, the PMIx tool library will check for the required connection
information of a local PMIx server and, unless directed otherwise, will attempt
to establish a connection to it. If the information is not found, or the server
connection fails, then an appropriate error constant will be returned |mdash|
unless the caller has requested that the connection be treated as optional (see
``PMIX_TOOL_CONNECT_OPTIONAL``) or has requested that no connection be attempted
at all (see ``PMIX_TOOL_DO_NOT_CONNECT``).

If successful, the function will return ``PMIX_SUCCESS`` and will fill the
provided structure with the server-assigned namespace and rank of the tool. If
the tool self-assigns its identity (because it is not connecting to a server, or
because connection was optional and failed), the returned namespace is
synthesized from the tool's hostname and process ID and the rank is set to zero.

A process may declare itself to be a *launcher* (via ``PMIX_LAUNCHER``) or the
system *scheduler* (via ``PMIX_SERVER_SCHEDULER``), in which case the tool
library additionally initializes the server-side infrastructure so the process
can host its own PMIx server and spawn or accept connections from clients.

Reference counting
^^^^^^^^^^^^^^^^^^

The PMIx tool library is reference counted, and so multiple calls to
``PMIx_tool_init`` are allowed. Thus, one way to obtain the namespace and rank of
the process is to simply call ``PMIx_tool_init`` with a non-``NULL`` ``proc``
parameter. Each call must be balanced by a matching call to
:ref:`PMIx_tool_finalize(3) <man3-PMIx_tool_finalize>`; only the last such
finalize actually tears the library down.


DIRECTIVES
----------

The following attributes are relevant to this operation. Support for any given
attribute is optional and depends on how the PMIx implementation was built and on
the capabilities of the host environment.

Tool identity attributes
^^^^^^^^^^^^^^^^^^^^^^^^^

* ``PMIX_TOOL_NSPACE`` (char\*) |mdash| namespace to use for this tool. Must be
  accompanied by ``PMIX_TOOL_RANK``; providing one without the other returns an
  error. If not given, the identity is taken from the ``PMIX_NAMESPACE``
  environment variable (set when the tool was launched by a PMIx-enabled daemon)
  or is self-assigned.
* ``PMIX_TOOL_RANK`` (uint32_t) |mdash| rank of this tool within its namespace.
* ``PMIX_LAUNCHER`` (bool) |mdash| the tool is a launcher and needs rendezvous
  files created; causes the server-side infrastructure to be initialized.
* ``PMIX_SERVER_SCHEDULER`` (bool) |mdash| the process is hosted by the system
  scheduler and intends to act as the system-level server.

Connection attributes
^^^^^^^^^^^^^^^^^^^^^^

These attributes govern whether and how the tool rendezvous with a PMIx server
during initialization.

* ``PMIX_TOOL_DO_NOT_CONNECT`` (bool) |mdash| the tool wants to use internal PMIx
  support but does not want to connect to a server. The tool self-assigns its
  identity.
* ``PMIX_TOOL_CONNECT_OPTIONAL`` (bool) |mdash| the tool shall connect to a
  server if one is available, but otherwise shall continue (self-assigning its
  identity) rather than returning an error.
* ``PMIX_CONNECT_TO_SYSTEM`` (bool) |mdash| connect solely to the system-level
  PMIx server.
* ``PMIX_CONNECT_SYSTEM_FIRST`` (bool) |mdash| preferentially look for a
  system-level PMIx server first, then fall back to a server identified by
  another attribute.
* ``PMIX_SERVER_URI`` (char\*) |mdash| connect to the server at the given URI.
* ``PMIX_SERVER_NSPACE`` (char\*) |mdash| connect to the server of the given
  namespace.
* ``PMIX_SERVER_PIDINFO`` (pid_t) |mdash| connect to the server embedded in the
  process with the given PID.
* ``PMIX_TCP_URI`` (char\*) |mdash| URI of the server to connect to, or a file
  name containing it in the form ``file:<name of file>``.
* ``PMIX_TOOL_ATTACHMENT_FILE`` (char\*) |mdash| file containing the connection
  information to be used for attaching to a server.

Behavioral attributes
^^^^^^^^^^^^^^^^^^^^^^

* ``PMIX_FWD_STDIN`` (bool) |mdash| forward this process's ``stdin`` to the
  target processes.
* ``PMIX_IOF_LOCAL_OUTPUT`` (bool) |mdash| write forwarded output streams to the
  local ``stdout``/``stderr`` (the default for tools).
* ``PMIX_SERVER_TMPDIR`` (char\*) |mdash| temporary directory where the PMIx
  server places its rendezvous connection files. Defaults to the
  ``PMIX_SERVER_TMPDIR`` environment variable, if set.
* ``PMIX_SYSTEM_TMPDIR`` (char\*) |mdash| temporary directory used for the
  system-level server rendezvous point. Defaults to the ``PMIX_SYSTEM_TMPDIR``
  environment variable, if set.


RETURN VALUE
------------

Returns ``PMIX_SUCCESS`` on success. On error, a negative value corresponding to
a PMIx error constant is returned, including:

* ``PMIX_ERR_INIT`` |mdash| the PMIx library could not be initialized (for
  example, a usock-only transport was requested, or a tight race between
  concurrent init calls left the library in an unusable state).
* ``PMIX_ERR_BAD_PARAM`` |mdash| an invalid combination of directives was
  supplied (e.g., a namespace without a rank), or ``proc`` was ``NULL`` on the
  initial call.
* ``PMIX_ERR_UNREACH`` |mdash| the requested PMIx server could not be reached and
  the connection was not optional.
* ``PMIX_ERR_NOMEM`` |mdash| the library could not allocate required internal
  storage.

Any other negative value indicates an appropriate error condition. PMIx error
constants are defined in ``pmix_common.h``.


NOTES
-----

``PMIx_tool_init`` is intended for use by *tool* processes |mdash| debuggers,
profilers, and workflow managers |mdash| that may or may not have been registered
with a PMIx server before being started. Processes that were registered as
*clients* before being started should instead call
:ref:`PMIx_Init(3) <man3-PMIx_Init>`, and processes hosting a PMIx server on
behalf of a resource manager should call
:ref:`PMIx_server_init(3) <man3-PMIx_server_init>`.

A tool is restricted to the ``hash`` component of the GDS framework for
interacting with a server's data store.

After initialization, a tool may establish additional server connections with
:ref:`PMIx_tool_attach_to_server(3) <man3-PMIx_tool_attach_to_server>` and select
among them with :ref:`PMIx_tool_set_server(3) <man3-PMIx_tool_set_server>`.


.. seealso::
   :ref:`PMIx_tool_finalize(3) <man3-PMIx_tool_finalize>`,
   :ref:`PMIx_tool_attach_to_server(3) <man3-PMIx_tool_attach_to_server>`,
   :ref:`PMIx_tool_disconnect(3) <man3-PMIx_tool_disconnect>`,
   :ref:`PMIx_tool_get_servers(3) <man3-PMIx_tool_get_servers>`,
   :ref:`PMIx_tool_is_connected(3) <man3-PMIx_tool_is_connected>`,
   :ref:`PMIx_tool_set_server(3) <man3-PMIx_tool_set_server>`,
   :ref:`PMIx_Init(3) <man3-PMIx_Init>`,
   :ref:`PMIx_server_init(3) <man3-PMIx_server_init>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`,
   :ref:`pmix_proc_t(5) <man5-pmix_proc_t>`,
   :ref:`pmix_status_t(5) <man5-pmix_status_t>`
