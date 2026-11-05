.. _man3-PMIx_tool_finalize:

PMIx_tool_finalize
==================

.. include_body

``PMIx_tool_finalize`` |mdash| Finalize the PMIx tool library.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix_tool.h>

   pmix_status_t PMIx_tool_finalize(void);


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

  from pmix import *

  foo = PMIxTool()
  rc, myname = foo.init(None)
  # ... use the tool ...
  rc = foo.finalize()


DESCRIPTION
-----------

Finalize the PMIx tool library, closing any open connection to a PMIx server and
releasing the resources allocated by
:ref:`PMIx_tool_init(3) <man3-PMIx_tool_init>`. An error code is returned if, for
some reason, the connection cannot be cleanly closed.

If the tool is currently connected to a server, ``PMIx_tool_finalize`` sends a
finalize synchronization to that server and waits for the acknowledgment. This
wait is bounded by the ``pmix_tool_finalize_timeout`` parameter (5 seconds by
default), so the call cannot block indefinitely should the server become
unresponsive. Any output still buffered for the tool's
local ``stdout``/``stderr`` sinks is flushed before those sinks are torn down.

If the tool was acting as a launcher or server, any children it started are
cleanly terminated before the progress thread is stopped and the server-side
infrastructure is released.

Reference counting
^^^^^^^^^^^^^^^^^^

Because :ref:`PMIx_tool_init(3) <man3-PMIx_tool_init>` is reference counted, a
tool that initialized the library more than once must call ``PMIx_tool_finalize``
a matching number of times. Only the final call |mdash| the one that drops the
reference count to zero |mdash| actually tears the library down; earlier calls
simply decrement the count and return ``PMIX_SUCCESS``. The one-time-init latch
is reset by the final finalize, so a subsequent ``PMIx_tool_init`` in the same
process starts a fresh library instance.

This is a **blocking** call.


RETURN VALUE
------------

Returns ``PMIX_SUCCESS`` on success. On error, a negative value corresponding to
a PMIx error constant is returned, including:

* ``PMIX_ERR_INIT`` |mdash| the PMIx tool library was not initialized, so there is
  nothing to finalize.
* ``PMIX_ERR_WOULD_BLOCK`` |mdash| ``PMIx_tool_finalize`` was called from within a
  PMIx event handler or callback. The teardown waits on the PMIx progress thread
  and then stops it, which from that thread would be the thread waiting on
  itself. Nothing was finalized, and the call may be repeated from another
  thread.
* ``PMIX_ERR_TIMEOUT`` |mdash| the server did not answer the finalize handshake
  in time.

Other negative values indicate a failure to cleanly complete the finalize
handshake with the server: the host's answer to it, or a failure to send it. A
host that does not implement the ``client_finalized`` up-call is not a failure,
nor is a server that closes the connection in answer instead of replying.
In every case but ``PMIX_ERR_INIT`` and ``PMIX_ERR_WOULD_BLOCK`` the library is
finalized. PMIx error constants are defined in ``pmix_common.h``.


NOTES
-----

``PMIx_tool_finalize`` must only be called by processes that initialized via
:ref:`PMIx_tool_init(3) <man3-PMIx_tool_init>`. Client processes must instead call
:ref:`PMIx_Finalize(3) <man3-PMIx_Finalize>`, and server processes must call
:ref:`PMIx_server_finalize(3) <man3-PMIx_server_finalize>`.


.. seealso::
   :ref:`PMIx_tool_init(3) <man3-PMIx_tool_init>`,
   :ref:`PMIx_tool_disconnect(3) <man3-PMIx_tool_disconnect>`,
   :ref:`PMIx_Finalize(3) <man3-PMIx_Finalize>`,
   :ref:`PMIx_server_finalize(3) <man3-PMIx_server_finalize>`,
   :ref:`pmix_status_t(5) <man5-pmix_status_t>`
