.. _man3-PMIx_tool_set_server_module:

PMIx_tool_set_server_module
===========================

.. include_body

``PMIx_tool_set_server_module`` |mdash| Install a server function module so
that a tool can also act as a PMIx server.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix_tool.h>

   pmix_status_t PMIx_tool_set_server_module(pmix_server_module_t *module);


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

  from pmix import *

  foo = PMIxTool()
  # ... after a successful foo.init() ...
  # map server module entry-point names to Python callback functions
  modmap = {'clientconnected': myclientconnected,
            'iofpull': myiofpull}
  rc = foo.set_server_module(modmap)


INPUT PARAMETERS
----------------

* ``module``: Pointer to a ``pmix_server_module_t`` structure whose fields
  point at the host-provided functions the PMIx library is to call when it
  needs the tool (acting as a server) to perform a corresponding operation.
  Any field left ``NULL`` indicates that the tool does not provide that
  particular function.


DESCRIPTION
-----------

Provide an entry point by which a tool can supply a server function-pointer
module. Some tools (for example, launchers and workflow orchestrators) need
to act as PMIx servers to their own child processes while simultaneously
operating as tools with respect to a higher-level server. Calling
``PMIx_tool_set_server_module`` installs the host callbacks that the PMIx
library will invoke to satisfy client requests, and marks the process as
being of *server* type in addition to its tool role.

This is a purely local, *non-blocking* operation: the provided ``module``
structure is copied into the library's internal server state and the
function returns immediately. Because the library retains its own copy of
the structure contents, the caller is not required to keep the ``module``
storage alive after the call returns; however, any objects referenced by
the module's function pointers must remain valid for as long as the tool
acts as a server.

The module may be set only once. A second call, after a module has already
been installed, is rejected.


CALLBACK FUNCTION
-----------------

The ``pmix_server_module_t`` structure is a collection of function pointers,
each corresponding to a server operation that the host environment may be
asked to perform |mdash| for example, ``client_connected2``, ``fence_nb``,
``direct_modex``, ``spawn``, ``notify_event``, and the I/O forwarding
entry points. The full set of typedefs and their calling conventions is
defined in ``pmix_server.h``. The order of fields in this structure is part
of the PMIx ABI and never changes; new operations are only ever appended.


RETURN VALUE
------------

Returns ``PMIX_SUCCESS`` when the module has been installed successfully. On
error, a negative value corresponding to a PMIx error constant is returned,
including:

* ``PMIX_ERR_INIT`` |mdash| a server module has already been set for this
  process; the module cannot be replaced.

Any other negative value indicates an appropriate error condition. PMIx
error constants are defined in ``pmix_common.h``.


NOTES
-----

This function is available only to processes that initialized as *tools*
via :ref:`PMIx_tool_init(3) <man3-PMIx_tool_init>`. A process whose sole
role is that of a PMIx server should instead supply its module through
``PMIx_server_init``.


.. seealso::
   :ref:`PMIx_tool_init(3) <man3-PMIx_tool_init>`,
   :ref:`PMIx_tool_set_server(3) <man3-PMIx_tool_set_server>`,
   PMIx_server_init(3),
   :ref:`pmix_status_t(5) <man5-pmix_status_t>`
