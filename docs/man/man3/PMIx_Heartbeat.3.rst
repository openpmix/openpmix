.. _man3-PMIx_Heartbeat:

PMIx_Heartbeat
==============

.. include_body

``PMIx_Heartbeat`` |mdash| Send a heartbeat to the local PMIx server.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix.h>

   void PMIx_Heartbeat(void);


DESCRIPTION
-----------

Send a single heartbeat to the local PMIx server. ``PMIx_Heartbeat`` is a
convenience wrapper around :ref:`PMIx_Process_monitor(3) <man3-PMIx_Process_monitor>`
(equivalent to passing the ``PMIX_SEND_HEARTBEAT`` attribute as the monitor action):
it transmits a lightweight, one-way message to the server and returns immediately.

Heartbeats feed the server-side liveness monitoring that is configured through
:ref:`PMIx_Process_monitor(3) <man3-PMIx_Process_monitor>` with the
``PMIX_MONITOR_HEARTBEAT`` action. A process that has registered to be watched for
heartbeats is expected to call ``PMIx_Heartbeat`` often enough to satisfy the
window established by the ``PMIX_MONITOR_HEARTBEAT_TIME`` and
``PMIX_MONITOR_HEARTBEAT_DROPS`` directives. If the server does not receive the
expected heartbeats within that window, it generates the monitoring event
(e.g., ``PMIX_MONITOR_HEARTBEAT_ALERT``) that the requestor set up.

``PMIx_Heartbeat`` takes no arguments. If the library's progress engine has been
stopped, or the message cannot be built or sent, the call silently returns without
emitting a heartbeat.


RETURN VALUE
------------

``PMIx_Heartbeat`` returns no value (``void``). Any failure to send the heartbeat
|mdash| for example, because the progress engine has been stopped |mdash| is not
reported to the caller.


NOTES
-----

Because the heartbeat is delivered as a best-effort, one-way message and the
function returns no status, callers cannot detect a failed transmission from the
call itself. Detection of a missing heartbeat is instead surfaced through the
monitoring event registered via
:ref:`PMIx_Process_monitor(3) <man3-PMIx_Process_monitor>`.

Sending heartbeats is only meaningful for a process (client or tool) connected to a
local server; a server does not send heartbeats to itself.


.. seealso::
   :ref:`PMIx_Init(3) <man3-PMIx_Init>`,
   :ref:`PMIx_Process_monitor(3) <man3-PMIx_Process_monitor>`
