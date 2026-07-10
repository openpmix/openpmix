.. _man3-PMIx_tool_attach_to_server:

PMIx_tool_attach_to_server
==========================

.. include_body

``PMIx_tool_attach_to_server`` |mdash| Establish a connection from a tool to a
PMIx server.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix_tool.h>

   pmix_status_t PMIx_tool_attach_to_server(pmix_proc_t *myproc,
                                            pmix_proc_t *server,
                                            pmix_info_t info[], size_t ninfo);


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

  from pmix import *

  foo = PMIxTool()
  rc, myname = foo.init(None)
  # the directives is a list of Python ``pmix_info_t`` dictionaries
  pydirs = [{'key': PMIX_CONNECT_TO_SYSTEM,
             'value': {'value': True, 'val_type': PMIX_BOOL}}]
  rc, myname, mysrvr = foo.attach_to_server(pydirs)


INPUT PARAMETERS
----------------

* ``info``: Pointer to an array of :ref:`pmix_info_t(5) <man5-pmix_info_t>`
  structures identifying the target server and qualifying the connection (see
  `DIRECTIVES`_). Passing a ``NULL`` value for the ``info`` array pointer, or a
  zero ``ninfo``, is **not** allowed and results in return of
  ``PMIX_ERR_BAD_PARAM``.
* ``ninfo``: Number of elements in the ``info`` array.


OUTPUT PARAMETERS
-----------------

* ``myproc``: Pointer to a :ref:`pmix_proc_t(5) <man5-pmix_proc_t>` structure in
  which the tool's own namespace and rank are returned. PMIx does not currently
  support on-the-fly changes to the tool's identifier, so this is filled with the
  tool's existing identity; ``NULL`` may be passed if it is not required.
* ``server``: Pointer to a :ref:`pmix_proc_t(5) <man5-pmix_proc_t>` structure in
  which the identifier of the server that was attached is returned. ``NULL`` may
  be passed if the server's identity is not required.


DESCRIPTION
-----------

Establish a connection from an already-initialized tool to a PMIx server. The
target server is identified by one of the attributes in the ``info`` array (see
`DIRECTIVES`_).

This routine replaces the older ``PMIx_tool_connect_to_server`` entry point,
adding the ability to return the identifier of the server to which the tool
attached via the ``server`` output parameter.

By default, attaching to a server registers the connection but does not change
the tool's *primary* server |mdash| the one used for subsequent operations. To
make the newly attached server the primary (and mark the tool as connected),
include the ``PMIX_PRIMARY_SERVER`` attribute in the ``info`` array.

.. note::

   PMIx does not currently support on-the-fly changes to the tool's identifier.
   The new server must therefore be under the same namespace manager (e.g., the
   same host resource manager) as any prior server, so that the tool's original
   namespace remains a unique assignment. The ``myproc`` parameter is provided
   for obsolescence protection in case this constraint is ever removed; for now
   it is simply filled with the tool's existing namespace and rank.

This is a **blocking** call: internally the request is thread-shifted onto the
progress thread, and the caller waits for the connection attempt to complete
before the routine returns.


DIRECTIVES
----------

The following attributes are relevant to this operation. At least one server-
identifying attribute must be supplied.

Target server attributes
^^^^^^^^^^^^^^^^^^^^^^^^^

* ``PMIX_CONNECT_TO_SYSTEM`` (bool) |mdash| connect solely to the system-level
  PMIx server.
* ``PMIX_CONNECT_SYSTEM_FIRST`` (bool) |mdash| preferentially look for a
  system-level PMIx server first, and then fall back to a server identified by
  another attribute.
* ``PMIX_SERVER_URI`` (char\*) |mdash| connect to the server at the given URI.
* ``PMIX_SERVER_NSPACE`` (char\*) |mdash| connect to the server of the given
  namespace.
* ``PMIX_SERVER_PIDINFO`` (pid_t) |mdash| connect to the server embedded in the
  process with the given PID.
* ``PMIX_TCP_URI`` (char\*) |mdash| URI of the server to connect to, or a file
  name containing it in the form ``file:<name of file>``.
* ``PMIX_TOOL_ATTACHMENT_FILE`` (char\*) |mdash| file containing the connection
  information to be used for attaching to the server.

Connection-control attributes
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

* ``PMIX_PRIMARY_SERVER`` (bool) |mdash| designate the server being attached as
  the tool's primary server. When set, the tool's active server is switched to
  the new connection and the tool is marked as connected.
* ``PMIX_TIMEOUT`` (int) |mdash| time, in seconds, to wait for the connection to
  be established before returning an error (0 implies no timeout).


RETURN VALUE
------------

Returns ``PMIX_SUCCESS`` when the connection has been established. On error, a
negative value corresponding to a PMIx error constant is returned, including:

* ``PMIX_ERR_INIT`` |mdash| the tool library has not been initialized.
* ``PMIX_ERR_NOT_AVAILABLE`` |mdash| the internal progress thread has been
  stopped, so the operation cannot be serviced.
* ``PMIX_ERR_BAD_PARAM`` |mdash| the ``info`` array was ``NULL`` or ``ninfo`` was
  zero, so no target server was specified.
* ``PMIX_ERR_UNREACH`` |mdash| the specified server could not be reached.

Other negative values indicate an appropriate error condition. PMIx error
constants are defined in ``pmix_common.h``.


NOTES
-----

The tool must already have been initialized via
:ref:`PMIx_tool_init(3) <man3-PMIx_tool_init>` before calling this routine. A tool
may hold connections to multiple servers simultaneously; use
:ref:`PMIx_tool_get_servers(3) <man3-PMIx_tool_get_servers>` to enumerate them,
:ref:`PMIx_tool_set_server(3) <man3-PMIx_tool_set_server>` to select the active
server among them, and
:ref:`PMIx_tool_disconnect(3) <man3-PMIx_tool_disconnect>` to drop a connection.


.. seealso::
   :ref:`PMIx_tool_init(3) <man3-PMIx_tool_init>`,
   :ref:`PMIx_tool_disconnect(3) <man3-PMIx_tool_disconnect>`,
   :ref:`PMIx_tool_get_servers(3) <man3-PMIx_tool_get_servers>`,
   :ref:`PMIx_tool_set_server(3) <man3-PMIx_tool_set_server>`,
   :ref:`PMIx_tool_is_connected(3) <man3-PMIx_tool_is_connected>`,
   :ref:`pmix_info_t(5) <man5-pmix_info_t>`,
   :ref:`pmix_proc_t(5) <man5-pmix_proc_t>`,
   :ref:`pmix_status_t(5) <man5-pmix_status_t>`
