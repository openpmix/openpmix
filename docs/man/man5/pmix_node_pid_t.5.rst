.. _man5-pmix_node_pid_t:

pmix_node_pid_t
===============

.. include_body

`pmix_node_pid_t` |mdash| Identifies an operating system process by node
and PID

SYNTAX
------

C Syntax
^^^^^^^^

.. code-block:: c

   #include <pmix_common.h>

   typedef struct pmix_node_pid {
       char *hostname;
       uint32_t nodeid;
       pid_t pid;
   } pmix_node_pid_t;


DESCRIPTION
-----------

The `pmix_node_pid_t` structure identifies a specific operating system
process on a specific node by pairing a node identifier with the process's
local PID. It is used, for example, to specify the set of processes to be
watched via the ``PMIX_MONITOR_TARGET_PIDS`` attribute.

The fields of the structure are:

* ``hostname`` |mdash| the name of the node on which the process resides.
  May be ``NULL`` if the node is instead identified by ``nodeid``.
* ``nodeid`` |mdash| the numerical identifier of the node on which the
  process resides. A value of ``UINT32_MAX`` indicates the field is not
  set.
* ``pid`` |mdash| the local operating system process identifier (PID) of
  the target process. A value of ``-1`` indicates the field is not set.

Either ``hostname`` or ``nodeid`` may be used to identify the node; a
consumer must be prepared to accept whichever the producer has supplied.

STATIC INITIALIZER
------------------

A statically declared ``pmix_node_pid_t`` may be initialized with the
``PMIX_NODE_PID_STATIC_INIT`` macro, which sets ``hostname`` to ``NULL``, ``nodeid`` to ``UINT32_MAX``, and ``pid`` to ``-1``:

.. code-block:: c

   pmix_node_pid_t npid = PMIX_NODE_PID_STATIC_INIT;


.. seealso::
   :ref:`PMIx_Process_monitor(3) <man3-PMIx_Process_monitor>`,
   :ref:`pmix_proc_t(5) <man5-pmix_proc_t>`
