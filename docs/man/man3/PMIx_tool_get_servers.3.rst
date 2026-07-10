.. _man3-PMIx_tool_get_servers:

PMIx_tool_get_servers
=====================

.. include_body

``PMIx_tool_get_servers`` |mdash| Return the process identifiers of all servers
to which a tool is currently connected.


SYNOPSIS
--------

.. code-block:: c

   #include <pmix_tool.h>

   pmix_status_t PMIx_tool_get_servers(pmix_proc_t *servers[],
                                       size_t *nservers);


Python Syntax
^^^^^^^^^^^^^

.. code-block:: python3

  from pmix import *

  foo = PMIxTool()
  rc, myname = foo.init(None)
  # ... attach to one or more servers ...
  rc, servers = foo.get_servers()
  # servers is a list of {'nspace':..., 'rank':...} dictionaries


OUTPUT PARAMETERS
-----------------

* ``servers``: Address at which the pointer to a newly allocated array of
  :ref:`pmix_proc_t(5) <man5-pmix_proc_t>` structures is returned, one entry per
  connected server. The tool's current *primary* (active) server, if any, is
  placed at the front of the array.
* ``nservers``: Address at which the number of elements in the returned
  ``servers`` array is stored.


DESCRIPTION
-----------

Obtain the set of PMIx servers to which the tool currently holds a connection.
On success the routine allocates an array of
:ref:`pmix_proc_t(5) <man5-pmix_proc_t>` structures and returns its address and
element count through the two output parameters. If the tool has an active
primary server, that server's identifier is returned as the first element of the
array.

This is a **blocking** call: internally the request is thread-shifted onto the
progress thread, which walks the tool's list of server connections and builds the
returned array.


RETURN VALUE
------------

Returns ``PMIX_SUCCESS`` and a non-empty array when the tool is connected to at
least one server. On error, a negative value corresponding to a PMIx error
constant is returned, including:

* ``PMIX_ERR_INIT`` |mdash| the tool library has not been initialized.
* ``PMIX_ERR_NOT_AVAILABLE`` |mdash| the internal progress thread has been
  stopped, so the operation cannot be serviced.
* ``PMIX_ERR_UNREACH`` |mdash| the tool is not currently connected to any server;
  in this case no array is returned and ``nservers`` is set to zero.

Other negative values indicate an appropriate error condition. PMIx error
constants are defined in ``pmix_common.h``.


NOTES
-----

The caller assumes ownership of the returned ``servers`` array and is responsible
for releasing it. In C, the array is allocated with the PMIx allocation macros
and should be freed with ``PMIX_PROC_FREE(servers, nservers)``; the Python
binding returns an ordinary list and frees the underlying array automatically.


.. seealso::
   :ref:`PMIx_tool_init(3) <man3-PMIx_tool_init>`,
   :ref:`PMIx_tool_attach_to_server(3) <man3-PMIx_tool_attach_to_server>`,
   :ref:`PMIx_tool_disconnect(3) <man3-PMIx_tool_disconnect>`,
   :ref:`PMIx_tool_set_server(3) <man3-PMIx_tool_set_server>`,
   :ref:`PMIx_tool_is_connected(3) <man3-PMIx_tool_is_connected>`,
   :ref:`pmix_proc_t(5) <man5-pmix_proc_t>`,
   :ref:`pmix_status_t(5) <man5-pmix_status_t>`
