.. _man3-PMIx_tool_disconnect:

PMIx_tool_disconnect
====================

.. include_body

``PMIx_tool_disconnect`` |mdash| Disconnect a tool from a specified PMIx server.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix_tool.h>

   pmix_status_t PMIx_tool_disconnect(const pmix_proc_t *server);


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

  from pmix import *

  foo = PMIxTool()
  rc, myname = foo.init(None)
  # ... attach to one or more servers ...
  srvr = {'nspace': "myserver", 'rank': 0}
  rc = foo.disconnect(srvr)


INPUT PARAMETERS
----------------

* ``server``: Pointer to a :ref:`pmix_proc_t(5) <man5-pmix_proc_t>` structure
  giving the process identifier of the server from which the tool is to be
  disconnected.


DESCRIPTION
-----------

Disconnect the tool from the specified server connection while leaving the tool
library itself initialized. The tool remains free to continue operating and to
establish new connections via
:ref:`PMIx_tool_attach_to_server(3) <man3-PMIx_tool_attach_to_server>`.

If the identified server is the tool's current *primary* (active) server, the
tool transitions to a "disconnected" state: the active server is pointed back at
the tool itself |mdash| effectively the same condition as when a tool is
initialized without connecting to a server |mdash| and
:ref:`PMIx_tool_is_connected(3) <man3-PMIx_tool_is_connected>` will subsequently
report ``false``. Connections to any other servers are unaffected.

This is a **blocking** call: internally the request is thread-shifted onto the
progress thread, which locates the matching server, drops the connection, and
wakes the caller.


RETURN VALUE
------------

Returns ``PMIX_SUCCESS`` when the tool has been disconnected from the specified
server. On error, a negative value corresponding to a PMIx error constant is
returned, including:

* ``PMIX_ERR_INIT`` |mdash| the tool library has not been initialized.
* ``PMIX_ERR_NOT_AVAILABLE`` |mdash| the internal progress thread has been
  stopped, so the operation cannot be serviced.
* ``PMIX_ERR_NOT_FOUND`` |mdash| the tool has no connection to a server matching
  the supplied identifier.

Other negative values indicate an appropriate error condition. PMIx error
constants are defined in ``pmix_common.h``.


NOTES
-----

To close *all* connections and release the library, call
:ref:`PMIx_tool_finalize(3) <man3-PMIx_tool_finalize>` instead. Use
:ref:`PMIx_tool_get_servers(3) <man3-PMIx_tool_get_servers>` to obtain the set of
currently connected servers before selecting one to disconnect.


.. seealso::
   :ref:`PMIx_tool_init(3) <man3-PMIx_tool_init>`,
   :ref:`PMIx_tool_finalize(3) <man3-PMIx_tool_finalize>`,
   :ref:`PMIx_tool_attach_to_server(3) <man3-PMIx_tool_attach_to_server>`,
   :ref:`PMIx_tool_get_servers(3) <man3-PMIx_tool_get_servers>`,
   :ref:`PMIx_tool_is_connected(3) <man3-PMIx_tool_is_connected>`,
   :ref:`pmix_proc_t(5) <man5-pmix_proc_t>`,
   :ref:`pmix_status_t(5) <man5-pmix_status_t>`
