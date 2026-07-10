.. _man3-PMIx_tool_is_connected:

PMIx_tool_is_connected
======================

.. include_body

``PMIx_tool_is_connected`` |mdash| Test whether the tool is connected to a PMIx
server.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix_tool.h>

   bool PMIx_tool_is_connected(void);


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

  from pmix import *

  foo = PMIxTool()
  rc, myname = foo.init(None)
  if foo.is_connected():
      # the tool currently has an active server connection
      ...


DESCRIPTION
-----------

Return ``true`` if the tool currently has an active connection to a PMIx server,
and ``false`` otherwise.

A tool is *connected* when it has an active primary-server connection |mdash| for
example, immediately after a successful
:ref:`PMIx_tool_init(3) <man3-PMIx_tool_init>` that contacted a server, or after a
successful :ref:`PMIx_tool_attach_to_server(3) <man3-PMIx_tool_attach_to_server>`
that designated the new server as primary. A tool is *not* connected when it was
initialized with ``PMIX_TOOL_DO_NOT_CONNECT``, when an optional connection
attempt failed, or after
:ref:`PMIx_tool_disconnect(3) <man3-PMIx_tool_disconnect>` has dropped the primary
server (which leaves the tool pointing its active server back at itself).

This test reflects only the *primary* server association. A tool may still retain
connections to other servers (retrievable via
:ref:`PMIx_tool_get_servers(3) <man3-PMIx_tool_get_servers>`) even when it reports
as not connected to a primary server.


RETURN VALUE
------------

Returns the boolean connection state as described above. This routine does not
return a ``pmix_status_t`` and never fails; if the tool library has not been
initialized the returned value is simply ``false``.


.. seealso::
   :ref:`PMIx_tool_init(3) <man3-PMIx_tool_init>`,
   :ref:`PMIx_tool_attach_to_server(3) <man3-PMIx_tool_attach_to_server>`,
   :ref:`PMIx_tool_disconnect(3) <man3-PMIx_tool_disconnect>`,
   :ref:`PMIx_tool_get_servers(3) <man3-PMIx_tool_get_servers>`
